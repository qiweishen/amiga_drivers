#include "ebus/ptp_manager.h"
#include "time_util.h"

#include <algorithm>
#include <chrono>
#include <thread>

#include "logger.h"
#include "string_util.h"
#include "util.h"
#include "apply_plan.h"
#include "ebus/camera_controller.h"

namespace gox::ebus {
    namespace {
        common::DriverLog g_log{"GoX"};

        constexpr uint32_t kStatusPollMs = 500;

        // GO-X feature Names (manual p.128). There is no alternative spelling to
        // fall back to: this camera family has no SFNC PtpEnable/PtpStatus pair.
        constexpr const char *kEnableFeature = "GevIEEE1588";
        constexpr const char *kStatusFeature = "GevIEEE1588Status";
        constexpr const char *kAccuracyFeature = "GevIEEE1588ClockAccuracy";

        // A bad health reading is re-checked this many times, one second apart,
        // before the session is failed (mirrors the lms4xxx NTP watchdog).
        constexpr int kHealthRetries = 3;
        constexpr int kHealthRetryDelayMs = 1000;
    } // namespace


    PtpManager::PtpManager(std::string camera_id, PvGenParameterArray *params, PtpConfig cfg)
        : camera_id_(std::move(camera_id)), params_(params), cfg_(std::move(cfg)) {
    }


    bool PtpManager::ReadStatus(std::string &out) {
        if (!ReadEnumFeature(params_, kStatusFeature, out) &&
            !ReadFeatureAsString(params_, kStatusFeature, out)) {
            return false;
        }
        last_status_ = out;
        return true;
    }


    bool PtpManager::ReadClockAccuracy(int64_t &out) {
        if (ReadIntFeature(params_, kAccuracyFeature, out)) {
            last_accuracy_ = out;
            return true;
        }
        // The node is an enumeration (p.128: 0..20), and GetIntegerValue above
        // fails on an enum node, so this is the path a real GO-X takes: read the
        // entry's own value, which is the number the manual prints.
        if (ReadEnumIntFeature(params_, kAccuracyFeature, out)) {
            last_accuracy_ = out;
            return true;
        }
        // Last resort: the node is exposed as text only. read_enum_feature
        // returns the entry NAME, so it has to be mapped back (a plain decimal
        // string is accepted too).
        std::string text;
        if (!ReadEnumFeature(params_, kAccuracyFeature, text) &&
            !ReadFeatureAsString(params_, kAccuracyFeature, text)) {
            return false;
        }
        if (!PtpClockAccuracyFromName(text, out)) {
            return false;
        }
        last_accuracy_ = out;
        return true;
    }


    bool PtpManager::Enable() {
        feature_found_ = false;
        enabled_ = false;
        synchronized_ = false;

        if (!cfg_.enabled) {
            // Host time is never recorded as a time source on this platform; PTP is the
            // camera's only absolute time
            g_log.Warn("[{}] [eBUS] ptp.enabled=false: device timestamps are free-running 1 GHz ticks with NO "
                       "absolute time; offline association with GNSS time is impossible for this run", camera_id_);
            return true;
        }

        const auto degrade = [&](const std::string &msg) {
            if (cfg_.on_timeout == PtpOnTimeout::kAbort) {
                g_log.Error("[{}] [eBUS] {}", camera_id_, msg);
                return false;
            }
            g_log.Warn("[{}] [eBUS] {}; Continuing unsynchronized (warn_continue)", camera_id_, msg);
            return true;
        };

        if (!FeatureExists(params_, kEnableFeature) || !FeatureExists(params_, kStatusFeature)) {
            return degrade(std::string("PTP features ") + kEnableFeature + " / " + kStatusFeature +
                           " not found on the device; this camera does not support IEEE 1588");
        }
        feature_found_ = true;

        // GevIEEE1588 is a boolean, "0: False / 1: True", factory FALSE (p.128).
        // The shared write path dispatches on the node's real type and verifies
        // the read-back; required=false keeps the on_timeout policy in charge of
        // what a failure means.
        FeatureWrite enable_write;
        enable_write.name = kEnableFeature;
        enable_write.value = "true";
        enable_write.value_is_string = false;
        enable_write.strict = true;
        if (!apply_genicam_feature(params_, enable_write, "[" + camera_id_ + "] [eBUS] PTP",
                                   /*required=*/false)) {
            return degrade(std::string("Enabling PTP via ") + kEnableFeature +
                           " failed (see the warning above); PTP is not running");
        }
        enabled_ = true;
        g_log.Info("[{}] [eBUS] PTP enabled via {}; Waiting for status \"slave\"", camera_id_, kEnableFeature);
        return true;
    }


    bool PtpManager::WaitForSync(StopController *stop) {
        if (!cfg_.enabled || !enabled_) {
            return true; // nothing to wait for (disabled, or already degraded via warn_continue)
        }
        const uint64_t start_mono = common::TimeUtil::MonotonicNowNs();
        const uint64_t budget_ns = static_cast<uint64_t>(cfg_.sync_timeout_s * 1e9);

        const auto degrade = [&](const std::string &msg) {
            if (cfg_.on_timeout == PtpOnTimeout::kAbort) {
                g_log.Error("[{}] [eBUS] {}", camera_id_, msg);
                return false;
            }
            g_log.Warn("[{}] [eBUS] {}; continuing with ptp_synced=false - device_ts_ns degrades to a "
                       "free-running tick counter", camera_id_, msg);
            return true;
        };

        while (true) {
            if (stop != nullptr && stop->StopRequested()) {
                g_log.Warn("[{}] [eBUS] PTP wait interrupted by stop request", camera_id_);
                return false;
            }

            std::string status;
            if (ReadStatus(status)) {
                switch (ClassifyPtpStatus(status)) {
                    case PtpState::kFaulty:
                        return degrade(
                            "camera PTP state is *Faulty*: the 1588 stack hit an internal error. This is "
                            "usually caused by incompatible master traffic (one-step Sync where the camera "
                            "expects Sync+FollowUp, or an invalid sourcePortIdentity such as portNumber 0) "
                            "- fix or remove the offending grandmaster, then toggle GevIEEE1588 off/on "
                            "(or power-cycle) to reset the state machine");
                    case PtpState::kMaster:
                        return degrade(
                            "camera became PTP *Master*: no grandmaster is winning the BMCA on this "
                            "network. Check that the grandmaster is up, shares the camera's L2 domain, "
                            "and that the switch does not filter PTP multicast (224.0.1.129 / "
                            "01-1B-19-00-00-00)");
                    case PtpState::kSlave: {
                        synchronized_ = true;
                        lock_wait_ms_ = (common::TimeUtil::MonotonicNowNs() - start_mono) / 1000000ull;
                        int64_t accuracy = 0;
                        std::string accuracy_text = " clock_accuracy=<not readable>";
                        if (ReadClockAccuracy(accuracy)) {
                            accuracy_text = " clock_accuracy=" + std::to_string(accuracy);
                            if (!PtpClockAccuracyOk(accuracy)) {
                                accuracy_text += " (outside the driver's acceptance window 0..9)";
                            }
                        } else {
                            accuracy_readable_ = false;
                        }
                        g_log.Info("[{}] [eBUS] PTP synchronized: status={} lock_wait={}ms{}", camera_id_,
                                   last_status_, lock_wait_ms_, accuracy_text);
                        return true;
                    }
                    case PtpState::kOther:
                        break; // still converging
                }
            }

            const uint64_t elapsed = common::TimeUtil::MonotonicNowNs() - start_mono;
            if (elapsed >= budget_ns) {
                break;
            }
            const uint64_t remaining_ms = (budget_ns - elapsed) / 1000000ull;
            std::this_thread::sleep_for(
                std::chrono::milliseconds(std::min<uint64_t>(kStatusPollMs, std::max<uint64_t>(remaining_ms, 1))));
        }

        std::string msg = "PTP did not reach \"slave\" within " + std::to_string(cfg_.sync_timeout_s) +
                          "s (last status \"" + last_status_ + "\")";
        if (common::StringUtil::EqualsCi(last_status_, "listening")) {
            msg += "; stuck in Listening usually means the grandmaster is unreachable: verify it is "
                    "on the camera's L2 domain and the switch forwards PTP multicast";
        }
        return degrade(msg);
    }


    bool PtpManager::CheckHealth(StopController *stop) {
        if (!cfg_.enabled || !synchronized_) {
            return true; // nothing to guard
        }

        std::string status;
        int64_t accuracy = 0;
        std::string reason;

        for (int attempt = 0; attempt <= kHealthRetries; ++attempt) {
            if (attempt > 0) {
                // A poll can land mid-BMCA; re-check quickly instead of waiting
                // for the next 5 s tick before ending a recording. The wait is
                // sliced so a shutdown request is not delayed by it.
                for (int slept = 0; slept < kHealthRetryDelayMs; slept += 100) {
                    if (stop != nullptr && stop->StopRequested()) {
                        return true; // the session is ending; do not fail it here
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
            }
            reason.clear();

            if (!ReadStatus(status)) {
                reason = "GevIEEE1588Status is no longer readable";
            } else if (ClassifyPtpStatus(status) != PtpState::kSlave) {
                reason = "PTP status left the \"slave\" state (now \"" + status + "\")";
            } else if (accuracy_readable_) {
                if (!ReadClockAccuracy(accuracy)) {
                    // Some firmware never populates the register; report it once
                    // and keep guarding the status only.
                    accuracy_readable_ = false;
                    g_log.Warn("[{}] [eBUS] {} is not readable; the PTP guard continues on the status alone",
                               camera_id_, kAccuracyFeature);
                } else if (!PtpClockAccuracyOk(accuracy)) {
                    reason = "PTP clock accuracy degraded to " + std::to_string(accuracy) +
                             " (the driver accepts 0..9, i.e. 1 ms or better; the register itself ranges "
                             "0..20 with 19 = Unknown, which is also the factory value)";
                }
            }

            if (reason.empty()) {
                if (attempt > 0) {
                    g_log.Warn("[{}] [eBUS] PTP recovered after {} re-check(s)", camera_id_, attempt);
                }
                return true;
            }
            g_log.Warn("[{}] [eBUS] PTP health check failed ({}), re-checking [{}/{}]", camera_id_, reason,
                       attempt + 1, kHealthRetries + 1);
        }

        g_log.Error("[{}] [eBUS] PTP synchronization lost: {}. Recorded device timestamps are no longer "
                    "traceable to the grandmaster; stopping to keep the dataset honest",
                    camera_id_, reason);
        return false;
    }


    PtpSummary PtpManager::Summary() const {
        PtpSummary s;
        s.enabled = cfg_.enabled;
        s.feature_found = feature_found_;
        s.written = enabled_;
        s.synchronized = synchronized_;
        s.status = last_status_;
        s.accuracy = last_accuracy_;
        s.lock_wait_ms = lock_wait_ms_;
        return s;
    }
} // namespace gox::ebus
