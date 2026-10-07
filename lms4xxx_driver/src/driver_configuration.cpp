#include "driver_internal.h"

#include <boost/asio/ip/address_v4.hpp>
#include "command_builder.h"
#include "error.h"
#include "logger.h"
#include "time_util.h"

namespace {
    common::DriverLog g_log{"LMS4xxx"};
    constexpr int kReadyTimeoutMs = 10000;
    constexpr int kReadyPollMs = 200;

    // "01 00 0D .." for readback-mismatch diagnostics
    std::string HexBytes(const std::vector<std::uint8_t> &bytes) {
        std::string out;
        out.reserve(bytes.size() * 3);
        for (const auto b: bytes) {
            out += fmt::format("{}{:02X}", out.empty() ? "" : " ", b);
        }
        return out;
    }
} // namespace

namespace lms4xxx {
    bool Lms4xxxDriver::Impl::IsTolerableSfa(const std::error_code &ec) {
        return ec == make_error_code(ErrorCode::kSopasError) ||
               ec == make_error_code(ErrorCode::kAccessDenied);
    }


    std::error_code Lms4xxxDriver::Impl::ApplyParameter(const ParamRow &row, int timeout_ms, int &changed) {
        const auto written = ColaBCodec::ParamsOf(row.frame, row.name.size());

        std::vector<std::uint8_t> before;
        const auto pre_ec = commands.ReadVariable(tcp_client.get(), row.name, before, timeout_ms, true);
        if (pre_ec && !IsTolerableSfa(pre_ec)) {
            g_log.Error("[{}] {} pre-read failed: {}", Tag(), row.name, pre_ec.message());
            return pre_ec;
        }
        const bool had_before = !pre_ec;

        ColaBMessage r;
        auto ec = commands.SendAndReceive(tcp_client.get(), row.frame, r, timeout_ms);
        if (ec) {
            g_log.Error("[{}] {} write failed: {}", Tag(), row.name, ec.message());
            return ec;
        }
        ec = commands.ValidateResponse(r, CommandType::kWriteAnswer, row.name);
        if (ec) {
            g_log.Error("[{}] {} not acknowledged", Tag(), row.name);
            return ec;
        }

        std::vector<std::uint8_t> after;
        ec = commands.ReadVariable(tcp_client.get(), row.name, after, timeout_ms, row.readback == Readback::kTolerateSfa);
        if (ec) {
            if (row.readback == Readback::kTolerateSfa && IsTolerableSfa(ec)) {
                g_log.Trace("[{}] {} has no readback (sFA): acknowledged only", Tag(), row.name);
            } else {
                g_log.Error("[{}] {} readback failed: {}", Tag(), row.name, ec.message());
                return ec;
            }
        } else if (after != written) {
            g_log.Error("[{}] {} readback mismatch: wrote [{}], device reports [{}]", Tag(), row.name,
                        HexBytes(written), HexBytes(after));
            return make_error_code(ErrorCode::kReadbackMismatch);
        }

        const bool differed = had_before && before != written;
        if (differed) {
            ++changed;
        }
        g_log.Trace("[{}] {} set: [{}]{} — {}", Tag(), row.name, HexBytes(written),
                    differed ? fmt::format(" (was [{}])", HexBytes(before)) : "", row.why);
        return {};
    }


    std::string Lms4xxxDriver::Impl::PrefixedString(const std::vector<std::uint8_t> &p) {
        if (p.size() >= 2) {
            const std::size_t n16 = ColaBCodec::DecodeUint16(p.data());
            if (n16 > 0 && n16 <= p.size() - 2) {
                return std::string(reinterpret_cast<const char *>(p.data() + 2), n16);
            }
        }
        if (!p.empty() && p[0] > 0 && p[0] <= p.size() - 1) {
            return std::string(reinterpret_cast<const char *>(p.data() + 1), p[0]);
        }
        return p.empty() ? std::string() : std::string(reinterpret_cast<const char *>(p.data()), p.size());
    }


    void Lms4xxxDriver::Impl::ReadIdentity(int timeout_ms) {
        identity = DeviceIdentity{};
        std::vector<std::uint8_t> p;

        if (!commands.ReadVariable(tcp_client.get(), "DeviceIdent", p, timeout_ms)) {
            // <u16 len><designation><u16 len><version>
            std::size_t pos = 0;
            const auto next = [&p, &pos](std::string &out) {
                if (pos + 2 > p.size()) {
                    return false;
                }
                const std::size_t n = ColaBCodec::DecodeUint16(p.data() + pos);
                pos += 2;
                if (pos + n > p.size()) {
                    return false;
                }
                out.assign(reinterpret_cast<const char *>(p.data() + pos), n);
                pos += n;
                return true;
            };
            if (!next(identity.firmware_designation) || !next(identity.firmware_version)) {
                g_log.Warn("[{}] DeviceIdent answer not understood: [{}]", Tag(), HexBytes(p));
            }
        } else {
            g_log.Warn("[{}] DeviceIdent not readable", Tag());
        }
        if (!commands.ReadVariable(tcp_client.get(), "DIornr", p, timeout_ms)) {
            identity.order_number = PrefixedString(p);
        }
        if (!commands.ReadVariable(tcp_client.get(), "DItype", p, timeout_ms)) {
            identity.device_type = PrefixedString(p);
        }
        if (!commands.ReadVariable(tcp_client.get(), "LocationName", p, timeout_ms)) {
            identity.location_name = PrefixedString(p);
        }
        identity.is_s01_variant = identity.order_number == "1116198";
    }


    std::vector<std::string> Lms4xxxDriver::Impl::DecodeActiveMessages(const std::vector<std::uint8_t> &p,
                                                        bool *complete) {
        if (complete) *complete = false;
        std::vector<std::string> out;
        if (p.size() < 2) {
            return out;
        }
        const std::uint16_t count = ColaBCodec::DecodeUint16(p.data());
        std::size_t pos = 2;
        for (std::uint16_t i = 0; i < count && pos + 4 <= p.size(); ++i) {
            const auto id = ColaBCodec::DecodeUint16(p.data() + pos);
            const std::size_t len = ColaBCodec::DecodeUint16(p.data() + pos + 2);
            pos += 4;
            if (pos + len > p.size()) {
                break;
            }
            out.push_back(fmt::format("{}: {}", id,
                                      std::string(reinterpret_cast<const char *>(p.data() + pos), len)));
            pos += len;
        }
        if (complete) *complete = pos == p.size() && out.size() == count;
        return out;
    }


    void Lms4xxxDriver::Impl::LogActiveMessages(int timeout_ms) {
        std::vector<std::uint8_t> p;
        if (commands.ReadVariable(tcp_client.get(), "EMActiveCustomerInfo", p, timeout_ms)) {
            return;
        }
        for (const auto &message: DecodeActiveMessages(p)) {
            g_log.Warn("[{}] Device message {}", Tag(), message);
        }
    }


    void Lms4xxxDriver::Impl::ReadAudit(int timeout_ms) {
        audit = DeviceAudit{};
        std::vector<std::uint8_t> p;

        if (!commands.ReadVariable(tcp_client.get(), "OPcurtmpdev", p, timeout_ms, true) && p.size() >= 4) {
            audit.temperature_c = ColaBCodec::DecodeFloat(p.data());
        }
        if (!commands.ReadVariable(tcp_client.get(), "ODoprh", p, timeout_ms, true) && p.size() >= 4) {
            audit.operating_hours_deci = ColaBCodec::DecodeUint32(p.data());
        }
        if (!commands.ReadVariable(tcp_client.get(), "ODpwrc", p, timeout_ms, true) && p.size() >= 4) {
            audit.power_on_count = ColaBCodec::DecodeUint32(p.data());
        }
        if (!commands.ReadVariable(tcp_client.get(), "SCdevicestate", p, timeout_ms, true) && !p.empty()) {
            audit.device_state = p[0];
        }
        // sRA LMPscancfg: freq(u32) reserved(i16) resolution(u32) start(i32) stop(i32)
        if (!commands.ReadVariable(tcp_client.get(), "LMPscancfg", p, timeout_ms, true) && p.size() >= 18) {
            audit.scan_frequency_centi_hz = ColaBCodec::DecodeUint32(p.data());
            audit.angular_resolution_1e4 = ColaBCodec::DecodeUint32(p.data() + 6);
            audit.start_angle_1e4 = ColaBCodec::DecodeInt32(p.data() + 10);
            audit.stop_angle_1e4 = ColaBCodec::DecodeInt32(p.data() + 14);
        }
        // sRA IOlasc: source(u8) delay_start(u16) delay_stop(u16) timeout(u16) reserved(u8)
        if (!commands.ReadVariable(tcp_client.get(), "IOlasc", p, timeout_ms, true) && p.size() >= 8) {
            audit.laser_trigger_source = p[0];
            audit.laser_timeout_s = ColaBCodec::DecodeUint16(p.data() + 5);
        }
        if (!commands.ReadVariable(tcp_client.get(), "SYtype", p, timeout_ms, true) && !p.empty()) {
            audit.motor_sync_role = p[0];
        }
        if (!commands.ReadVariable(tcp_client.get(), "SYphas", p, timeout_ms, true) && p.size() >= 4) {
            audit.motor_sync_phase_deg = ColaBCodec::DecodeUint32(p.data()) / 10000.0;
        }

        // Continuous output requires free-running laser control (p.17), and a
        // laser timeout would switch the laser off mid-survey (p.77).
        if (audit.laser_trigger_source && *audit.laser_trigger_source != 0) {
            g_log.Warn("[{}] Laser control is trigger source {} (0 = free-running); continuous measurement "
                       "output expects free-running (manual p.17/p.77)",
                       Tag(), *audit.laser_trigger_source);
        }
        if (audit.laser_timeout_s && *audit.laser_timeout_s != 0) {
            g_log.Warn("[{}] Laser timeout is {} s: the device will switch the laser off that long into the "
                       "Run (manual p.77)", Tag(), *audit.laser_timeout_s);
        }
        // Motor synchronisation is deliberately never written by this driver,
        // but it is an Interfaces parameter and mSCloadappdef does not reset
        // those (p.79), so a role left behind by SOPAS outlives everything.
        if (audit.motor_sync_role && *audit.motor_sync_role != 0) {
            g_log.Warn("[{}] Device has motor synchronisation configured (SYtype={}, phase={:.2f} deg). This "
                       "driver never sets it and mSCloadappdef does not reset Interfaces parameters (manual "
                       "p.79) — clear it in SOPAS if it is not intended",
                       Tag(), *audit.motor_sync_role, audit.motor_sync_phase_deg.value_or(0.0));
        }
        g_log.Info("[{}] Device audit: temperature={} C, operating_hours={} h, power_on_count={}", Tag(),
                   audit.temperature_c ? fmt::format("{:.1f}", *audit.temperature_c) : "n/a",
                   audit.operating_hours_deci ? fmt::format("{:.1f}", *audit.operating_hours_deci / 10.0) : "n/a",
                   audit.power_on_count ? fmt::format("{}", *audit.power_on_count) : "n/a");
    }


    std::error_code Lms4xxxDriver::Impl::WaitDeviceReady(int timeout_ms) {
        const auto started = std::chrono::steady_clock::now();
        const auto deadline = started + std::chrono::milliseconds(kReadyTimeoutMs);
        while (true) {
            std::vector<std::uint8_t> p;
            auto ec = commands.ReadVariable(tcp_client.get(), "SCdevicestate", p, std::max(timeout_ms, commands.MethodTimeout()));
            if (ec) {
                g_log.Error("[{}] SCdevicestate read failed: {}", Tag(), ec.message());
                return ec;
            }
            const int state = p.empty() ? -1 : static_cast<int>(p[0]);
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started).count();
            if (state == 1) {
                g_log.Trace("[{}] Device ready after {} ms", Tag(), elapsed);
                return {};
            }
            if (state == 2) {
                g_log.Error("[{}] Device reports the error state", Tag());
                LogActiveMessages(timeout_ms);
                return make_error_code(ErrorCode::kDeviceNotReady);
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                g_log.Error("[{}] Device not ready after {} ms (SCdevicestate {})", Tag(), elapsed, state);
                LogActiveMessages(timeout_ms);
                return make_error_code(ErrorCode::kDeviceNotReady);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(kReadyPollMs));
        }
    }


    std::error_code Lms4xxxDriver::Configure() {
        if (!impl_->tcp_client || !impl_->tcp_client->IsConnected()) {
            return make_error_code(ErrorCode::kNotConnected);
        }

        impl_->SetState(ConnectionState::kConfiguring);
        const int timeout = impl_->config.network.config_timeout_ms;
        const auto &cfg = impl_->config;
        const auto fail = [this](std::error_code ec) {
            // LMCstartmeas may have taken effect even when its answer was lost.
            // Preserve the original configuration error; cleanup has its own status.
            impl_->ParkMeasurement(false, impl_->commands.Uncertain() ||
                                   !impl_->tcp_client || !impl_->tcp_client->IsConnected());
            impl_->SetState(ConnectionState::kError);
            return ec;
        };

        impl_->ReadIdentity(timeout);
        const auto &id = impl_->identity;
        g_log.Info("[{}] Device: {} fw {}, order {}{}, type {}, name '{}'", impl_->Tag(), id.firmware_designation,
                   id.firmware_version, id.order_number,
                   id.is_s01_variant ? " (LMS4124R-13000S01: quality-flagged points stay valid)" : "", id.device_type,
                   id.location_name);

        // 1. Login
        if (auto ec = impl_->commands.CallMethod(impl_->tcp_client.get(), CommandBuilder::BuildLogin(), "SetAccessMode", 0x01)) {
            return fail(ec);
        }
        g_log.Trace("[{}] Logged in as Authorized Client", impl_->Tag());

        // 2. Baseline: application defaults (p.79) — every "Application" parameter back to
        // factory, interface/IP settings untouched. Answered without a status byte. Whether
        // it logs the client out is undocumented, so log in again.
        if (auto ec = impl_->commands.CallMethod(impl_->tcp_client.get(), CommandBuilder::BuildLoadAppDefaults(), "mSCloadappdef", std::nullopt)) {
            return fail(ec);
        }
        g_log.Trace("[{}] Application defaults loaded", impl_->Tag());
        if (auto ec = impl_->commands.CallMethod(impl_->tcp_client.get(), CommandBuilder::BuildLogin(), "SetAccessMode", 0x01)) {
            return fail(ec);
        }

        // 3. What the recording depends on, written and read back regardless of the defaults
        using R = Impl::Readback;
        std::vector<Impl::ParamRow> rows = {
            {
                "LMDscandatacfg",
                CommandBuilder::BuildScanDataConfig(cfg.scan.remission, cfg.ntp.enabled),
                R::kTolerateSfa,
                cfg.ntp.enabled
                    ? "DIST + remission + ANGL + QLTY, time stamp, every scan"
                    : "DIST + remission + ANGL + QLTY, no time stamp (NTP off), every scan"
            },
            {
                "LMPoutputRange",
                CommandBuilder::BuildOutputRange(),
                R::kStrict,
                "55..125 deg @ 1/12 deg (841 points)"
            },
            // Filters
            {
                "LFPmeanfilter",
                CommandBuilder::BuildMeanFilter(false, 2),
                R::kStrict,
                "mean filter off"
            },
            {
                "LFPmedianfilter",
                CommandBuilder::BuildMedianFilter(false),
                R::kStrict,
                "median filter off"
            },
            {
                "LFPfrontendEdgefilter",
                CommandBuilder::BuildFrontendEdgeFilter(ScanFixed::kFrontendEdgeFilterOff),
                R::kStrict,
                "front-end edge filter off"
            },
            {
                "LFPedgefilter",
                CommandBuilder::BuildEdgeFilter(false),
                R::kStrict,
                "edge filter off"
            },
            {
                "LFPcubicareafilter",
                CommandBuilder::BuildCubicAreaFilter(ScanFixed::kCubicAreaFilterOff, 0, ScanFixed::kCubicMaxDist1e4mm,
                                                     -ScanFixed::kCubicExpansion1e4mm, ScanFixed::kCubicExpansion1e4mm),
                R::kStrict,
                "rectangular filter off"
            },
            {
                "LFPglossfilter",
                CommandBuilder::BuildGlossFilter(false),
                R::kStrict,
                "gloss compensation off"
            },
            // device time
            {
                "TSCTCtimezone",
                CommandBuilder::BuildSetNtpTimezone(ScanFixed::kTimezoneUtc),
                R::kTolerateSfa,
                "device timestamps in UTC"
            },
        };
        if (cfg.ntp.enabled) {
            boost::system::error_code bec;
            const auto addr = boost::asio::ip::make_address_v4(cfg.ntp.server, bec);
            if (bec) {
                g_log.Error("[{}] Invalid NTP server IP '{}'", impl_->Tag(), cfg.ntp.server);
                return fail(make_error_code(ErrorCode::kInvalidConfig));
            }
            rows.push_back({
                "TSCRole",
                CommandBuilder::BuildSetTimeSyncRole(static_cast<std::uint8_t>(TscRole::kClient)),
                R::kTolerateSfa,
                "NTP client"
            });
            rows.push_back({
                "TSCTCSrvAddr",
                CommandBuilder::BuildSetNtpServer(addr.to_bytes()),
                R::kTolerateSfa,
                "NTP server"
            });
            rows.push_back({
                "TSCTCupdatetime",
                CommandBuilder::BuildSetNtpUpdateTime(cfg.ntp.sync_interval_s),
                R::kTolerateSfa,
                "NTP update interval"
            });
        } else {
            rows.push_back({
                "TSCRole",
                CommandBuilder::BuildSetTimeSyncRole(static_cast<std::uint8_t>(TscRole::kOff)),
                R::kTolerateSfa,
                "NTP off"
            });
        }
        int changed = 0;
        for (const auto &row: rows) {
            if (auto ec = impl_->ApplyParameter(row, timeout, changed)) {
                return fail(ec);
            }
        }

        // 4. NTP effect: server liveness, not a device-clock comparison (see DriverStatistics)
        if (cfg.ntp.enabled) {
            std::string ntp_detail;
            if (!impl_->ProbeNtpServer(ntp_detail)) {
                g_log.Error("[{}] NTP server {} check failed: {}", impl_->Tag(), cfg.ntp.server, ntp_detail);
                return fail(make_error_code(ErrorCode::kInvalidConfig));
            }
            impl_->stats.ntp_server_reachable.store(true, std::memory_order_relaxed);
            impl_->stats.ntp_status.store(DriverStatistics::NtpStatus::kUnverified, std::memory_order_relaxed);
            impl_->stats.ntp_configured_at_us.store(common::TimeUtil::RealtimeNowUs(), std::memory_order_relaxed);
            g_log.Info("[{}] NTP configured: server={} ({}), interval={} s, timezone=UTC", impl_->Tag(),
                       cfg.ntp.server, ntp_detail, cfg.ntp.sync_interval_s);
        }

        // 5. Arm rollback BEFORE the request: losing its answer does not prove
        // that measurement stayed off. Run applies configuration and logs out.
        impl_->measurement_shutdown.Arm();
        if (auto ec = impl_->commands.CallMethod(impl_->tcp_client.get(), CommandBuilder::BuildStartMeasurement(), "LMCstartmeas", 0x00)) {
            return fail(ec);
        }
        if (auto ec = impl_->commands.CallMethod(impl_->tcp_client.get(), CommandBuilder::BuildRun(), "Run", 0x01)) {
            return fail(ec);
        }
        if (auto ec = impl_->WaitDeviceReady(timeout)) {
            return fail(ec);
        }

        // 6. What the device says about itself, once the configuration is live.
        // Queries need no user level (manual p.69), so this runs fine after Run
        // logged the session out — and it is the last command-phase traffic, so
        // a device that does not answer costs metadata and nothing else.
        impl_->ReadAudit(timeout);

        g_log.Info("[{}] Device configured", impl_->Tag());

        impl_->SetState(ConnectionState::kConnected);
        return {};
    }


} // namespace lms4xxx
