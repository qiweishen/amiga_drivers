#include "ebus/ptp_manager.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <thread>

#include "logger.h"
#include "../../include/util.hpp"
#include "ebus/camera_controller.hpp"
#include "ebus/sdk_error.hpp"

namespace jai::ebus {
    namespace {
        Common::DriverLog g_log{"GoX"};

        constexpr int64_t kNsPerSecond = 1000000000ll;
        constexpr int64_t kExpectedTaiUtcOffsetS = 37; // leap seconds as of 2026

        std::string lower(std::string s) {
            std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            return s;
        }

        bool iequals(const std::string &a, const std::string &b) {
            return lower(a) == lower(b);
        }

        bool contains_ci(const std::string &haystack, const std::string &needle) {
            return lower(haystack).find(lower(needle)) != std::string::npos;
        }
    } // namespace

    PtpManager::PtpManager(std::string camera_id, PvGenParameterArray *params, PtpConfig cfg)
        : camera_id_(std::move(camera_id)), params_(params), cfg_(std::move(cfg)) {
    }

    bool PtpManager::detect_features() {
        if (cfg_.feature_set == "explicit") {
            report_.feature_set = "explicit";
            report_.enable_feature = cfg_.enable_feature;
            report_.status_feature = cfg_.status_feature;
            if (!feature_exists(params_, cfg_.enable_feature) || !feature_exists(params_, cfg_.status_feature)) {
                g_log.error("[{}] explicit PTP features not found on device: {} / {}", camera_id_, cfg_.enable_feature,
                            cfg_.status_feature);
                return false;
            }
            if (feature_exists(params_, "PtpDataSetLatch")) {
                report_.latch_command = "PtpDataSetLatch";
            }
            return true;
        }

        const bool allow_gev = cfg_.feature_set == "auto" || cfg_.feature_set == "gev_ieee1588";
        const bool allow_sfnc = cfg_.feature_set == "auto" || cfg_.feature_set == "sfnc_ptp";

        // 1. GEV 2.0 SFNC pair (most likely on the JAI Go-X series).
        if (allow_gev && feature_exists(params_, "GevIEEE1588") && feature_exists(params_, "GevIEEE1588Status")) {
            report_.feature_set = "gev_ieee1588";
            report_.enable_feature = "GevIEEE1588";
            report_.status_feature = "GevIEEE1588Status";
            return true;
        }

        // 2. SFNC 2.3+ pair (dataset reads need PtpDataSetLatch first).
        if (allow_sfnc && feature_exists(params_, "PtpEnable") && feature_exists(params_, "PtpStatus")) {
            report_.feature_set = "sfnc_ptp";
            report_.enable_feature = "PtpEnable";
            report_.status_feature = "PtpStatus";
            if (feature_exists(params_, "PtpDataSetLatch")) {
                report_.latch_command = "PtpDataSetLatch";
            }
            return true;
        }

        // 3. Fuzzy scan (auto only): a Boolean and an Enum whose names mention
        // IEEE1588/Ptp; the enum must look like a status node.
        if (cfg_.feature_set == "auto") {
            std::string enable_name, status_name;
            const uint32_t count = params_ != nullptr ? params_->GetCount() : 0;
            for (uint32_t i = 0; i < count; ++i) {
                PvGenParameter *p = params_->Get(i);
                if (p == nullptr) {
                    continue;
                }
                PvString pv_name;
                p->GetName(pv_name);
                const std::string name = to_std(pv_name);
                if (!contains_ci(name, "ieee1588") && !contains_ci(name, "ptp")) {
                    continue;
                }
                PvGenType type = PvGenTypeUndefined;
                if (!p->GetType(type).IsOK()) {
                    continue;
                }
                if (type == PvGenTypeBoolean && enable_name.empty() && !contains_ci(name, "status")) {
                    enable_name = name;
                } else if (type == PvGenTypeEnum && status_name.empty() && contains_ci(name, "status")) {
                    status_name = name;
                }
            }
            if (!enable_name.empty() && !status_name.empty()) {
                g_log.warn("[{}] PTP features found by fuzzy scan: enable={} status={}", camera_id_,
                           enable_name, status_name);
                report_.feature_set = "fuzzy";
                report_.enable_feature = enable_name;
                report_.status_feature = status_name;
                return true;
            }
        }
        return false;
    }

    bool PtpManager::enable() {
        report_ = PtpStatusReport{};

        // Timestamp semantics: PvBuffer::GetTimestamp() is the raw 64-bit GVSP
        // tick; only at 1 GHz is it directly nanoseconds.
        if (read_int_feature(params_, "GevTimestampTickFrequency", report_.tick_frequency)) {
            if (report_.tick_frequency != kNsPerSecond) {
                g_log.warn(
                    "[{}] GevTimestampTickFrequency is {} (not 1e9): device_ts_ns in the segment files is raw ticks",
                    camera_id_, report_.tick_frequency);
            }
        } else {
            g_log.trace("[{}] GevTimestampTickFrequency not readable", camera_id_);
        }

        if (!cfg_.enabled) {
            g_log.info("[{}] PTP disabled by config; device timestamps are free-running", camera_id_);
            return true;
        }

        if (!detect_features()) {
            const std::string msg =
                    "no usable PTP feature pair found (feature_set=" + cfg_.feature_set +
                    "); the camera may not support IEEE 1588";
            if (cfg_.on_timeout == "abort") {
                g_log.error("[{}] {}", camera_id_, msg);
                return false;
            }
            g_log.warn("[{}] {}; continuing unsynchronized (warn_continue)", camera_id_, msg);
            return true;
        }
        report_.feature_found = true;

        // Enable: the node is a Boolean on most cameras but an On/Off enum on
        // some; dispatch on the actual type.
        PvGenParameter *p = params_->Get(PvString(report_.enable_feature.c_str()));
        PvGenType type = PvGenTypeUndefined;
        PvResult r(PvResult::Code::GENERIC_ERROR);
        if (p != nullptr && p->GetType(type).IsOK()) {
            if (type == PvGenTypeBoolean) {
                r = params_->SetBooleanValue(PvString(report_.enable_feature.c_str()), true);
            } else if (type == PvGenTypeEnum) {
                r = params_->SetEnumValue(PvString(report_.enable_feature.c_str()), PvString("On"));
            }
        }
        if (!r.IsOK()) {
            const std::string msg = "enabling PTP via " + report_.enable_feature + " failed: " + pv_result_to_string(r);
            if (cfg_.on_timeout == "abort") {
                g_log.error("[{}] {}", camera_id_, msg);
                return false;
            }
            g_log.warn("[{}] {}; continuing unsynchronized (warn_continue)", camera_id_, msg);
            return true;
        }
        report_.enabled = true;
        g_log.info("[{}] PTP enabled via {} (chain {}); waiting for status \"{}\"", camera_id_, report_.enable_feature,
                   report_.feature_set, cfg_.required_status);
        return true;
    }


    bool PtpManager::read_status(std::string &out) {
        if (!report_.latch_command.empty()) {
            execute_command_feature(params_, report_.latch_command);
        }
        if (read_enum_feature(params_, report_.status_feature, out)) {
            return true;
        }
        return read_feature_as_string(params_, report_.status_feature, out);
    }


    void PtpManager::read_dataset_extras() {
        int64_t off = 0;
        double off_f = 0;
        if (read_int_feature(params_, "PtpOffsetFromMaster", off)) {
            report_.offset_valid = true;
            report_.offset_from_master_ns = off;
        } else if (read_float_feature(params_, "PtpOffsetFromMaster", off_f)) {
            report_.offset_valid = true;
            report_.offset_from_master_ns = static_cast<int64_t>(off_f);
        }
    }


    bool PtpManager::wait_for_sync(StopController *stop) {
        if (!cfg_.enabled || !report_.enabled) {
            return true; // nothing to wait for (disabled, or already degraded via warn_continue)
        }
        const uint64_t start_mono = now_monotonic_ns();
        const uint64_t budget_ns = static_cast<uint64_t>(cfg_.sync_timeout_s * 1e9);
        const bool check_servo = feature_exists(params_, "PtpServoStatus");

        while (true) {
            if (stop != nullptr && stop->stop_requested()) {
                g_log.warn("[{}] PTP wait interrupted by stop request", camera_id_);
                return false;
            }

            std::string status;
            if (read_status(status)) {
                report_.status = status;
                if (iequals(status, "Master")) {
                    const std::string msg =
                            "camera became PTP *Master*: no grandmaster is winning the BMCA on this "
                            "network. Check that the grandmaster is up, shares the camera's L2 domain, "
                            "and that the switch does not filter PTP multicast (224.0.1.129 / "
                            "01-1B-19-00-00-00)";
                    if (cfg_.on_timeout == "abort") {
                        g_log.error("[{}] {}", camera_id_, msg);
                        return false;
                    }
                    g_log.warn("[{}] {}; continuing unsynchronized (warn_continue)", camera_id_, msg);
                    return true;
                }
                bool ok = iequals(status, cfg_.required_status);
                if (ok && check_servo) {
                    std::string servo;
                    if (read_enum_feature(params_, "PtpServoStatus", servo) ||
                        read_feature_as_string(params_, "PtpServoStatus", servo)) {
                        report_.servo_status = servo;
                        ok = iequals(servo, "Locked");
                    }
                }
                if (ok) {
                    report_.lock_wait_ms = (now_monotonic_ns() - start_mono) / 1000000ull;
                    read_dataset_extras();
                    std::string grandmaster;
                    read_feature_as_string(params_, "PtpGrandmasterClockID", grandmaster);
                    g_log.info("[{}] PTP synchronized: status={}{} lock_wait={}ms{}", camera_id_, report_.status,
                               check_servo ? (" servo=" + report_.servo_status) : std::string(), report_.lock_wait_ms,
                               grandmaster.empty() ? std::string() : " grandmaster=" + grandmaster);
                    return true;
                }
            }

            const uint64_t elapsed = now_monotonic_ns() - start_mono;
            if (elapsed >= budget_ns) {
                break;
            }
            const uint64_t remaining_ms = (budget_ns - elapsed) / 1000000ull;
            std::this_thread::sleep_for(
                std::chrono::milliseconds(
                    std::min<uint64_t>(cfg_.poll_interval_ms, std::max<uint64_t>(remaining_ms, 1))));
        }

        std::string msg = "PTP did not reach \"" + cfg_.required_status + "\" within " + std::to_string(
                              cfg_.sync_timeout_s) +
                          "s (last status \"" + report_.status + "\")";
        if (iequals(report_.status, "Listening")) {
            msg += "; stuck in Listening usually means the grandmaster is unreachable: verify it is "
                    "on the camera's L2 domain and the switch forwards PTP multicast";
        }
        if (cfg_.on_timeout == "abort") {
            g_log.error("[{}] {}", camera_id_, msg);
            return false;
        }
        g_log.warn("[{}] {}; continuing with ptp_synced=false — device_ts_ns degrades to a free-running tick counter",
                   camera_id_,
                   msg);
        return true;
    }

    void PtpManager::cross_check_timestamp() {
        std::string cmd;
        if (feature_exists(params_, "TimestampLatch")) {
            cmd = "TimestampLatch";
        } else if (feature_exists(params_, "GevTimestampControlLatch")) {
            cmd = "GevTimestampControlLatch";
        } else {
            g_log.debug("[{}] no timestamp latch command; cross-check skipped", camera_id_);
            return;
        }
        std::string value_name;
        if (feature_exists(params_, "TimestampLatchValue")) {
            value_name = "TimestampLatchValue";
        } else if (feature_exists(params_, "GevTimestampValue")) {
            value_name = "GevTimestampValue";
        } else {
            g_log.debug("[{}] no timestamp latch value feature; cross-check skipped", camera_id_);
            return;
        }

        // Bracket the latch with host CLOCK_REALTIME samples; the midpoint is
        // our best estimate of "host time when the camera latched".
        const uint64_t t0 = now_realtime_ns();
        if (!execute_command_feature(params_, cmd)) {
            g_log.warn("[{}] {} failed; timestamp cross-check skipped", camera_id_, cmd);
            return;
        }
        const uint64_t t1 = now_realtime_ns();
        int64_t ticks = 0;
        if (!read_int_feature(params_, value_name, ticks)) {
            g_log.warn("[{}] {} not readable; timestamp cross-check skipped", camera_id_, value_name);
            return;
        }
        int64_t device_ns = ticks;
        if (report_.tick_frequency > 0 && report_.tick_frequency != kNsPerSecond) {
            device_ns =
                    static_cast<int64_t>(static_cast<long double>(ticks) * 1e9L / static_cast<long double>(report_.
                                             tick_frequency));
        }
        const int64_t midpoint = static_cast<int64_t>(t0 / 2 + t1 / 2);
        const int64_t raw_offset = midpoint - device_ns; // host(UTC) - device(TAI when synced)

        // PTP time is TAI: expect host(UTC) - device(TAI) ~= -37 s. Detect 37+-1 s
        // and remove it before reporting so a healthy setup reads near zero.
        int64_t adjusted = raw_offset;
        report_.tai_offset_detected = false;
        if (cfg_.expect_tai_offset && std::llabs(raw_offset + kExpectedTaiUtcOffsetS * kNsPerSecond) <= kNsPerSecond) {
            adjusted = raw_offset + kExpectedTaiUtcOffsetS * kNsPerSecond;
            report_.tai_offset_detected = true;
        }

        std::string drift;
        if (have_first_latch_offset_) {
            drift = " drift_since_start=" + std::to_string(adjusted - first_latch_offset_ns_) + "ns";
        } else {
            have_first_latch_offset_ = true;
            first_latch_offset_ns_ = adjusted;
        }
        g_log.info(
            "[{}] timestamp cross-check: host-device offset {}ns (bracket {}ns{}){} — offset is only authoritative if "
            "the host clock is PTP/NTP disciplined",
            camera_id_, adjusted, t1 - t0, report_.tai_offset_detected ? ", TAI-UTC 37s removed" : "", drift);
    }

    void PtpManager::refresh_offset() {
        if (!cfg_.enabled || !report_.feature_found) {
            return;
        }
        if (!report_.latch_command.empty()) {
            execute_command_feature(params_, report_.latch_command);
        }
        read_dataset_extras();
        if (report_.offset_valid) {
            g_log.info("[{}] PtpOffsetFromMaster={}ns", camera_id_, report_.offset_from_master_ns);
        }
        cross_check_timestamp();
    }
} // namespace jai::ebus
