#include "sensor_trigger_log.h"

#include <sys/stat.h>
#include <cerrno>
#include <chrono>
#include <cstring>

#include "log_growth_tracker.h"
#include "logger.h"
#include "session_client.cpp"


namespace fx10 {
    namespace {
        common::DriverLog g_log{"FX10"};

        // No log growth for this long at Stop() time = the STOP command very likely never reached the board
        constexpr double kStopStallWarnS = 15.0;

        // errno of the failed port open -> field-actionable hint
        const char *OpenErrnoHint(int err) {
            switch (err) {
                case EACCES:
                    return " — no permission on the tty (is this user in the dialout group?)";
                case EBUSY:
                    return " — another process holds the port exclusively (check with lsof)";
                case ENOENT:
                case ENODEV:
                case ENXIO:
                    return " — board unplugged or not enumerated (check ls /dev/serial/by-id/)";
                default:
                    return "";
            }
        }
    } // namespace


    struct SensorTriggerLog::Impl {
        SensorSyncSession session;
        LogGrowthTracker growth; // see log_growth_tracker.hpp for why size, not mtime
    };


    SensorTriggerLog::SensorTriggerLog(std::string port)
        : impl_(std::make_unique<Impl>()), port_(std::move(port)) {
    }


    SensorTriggerLog::~SensorTriggerLog() {
        Stop();
    }


    void SensorTriggerLog::Open() {
        if (!impl_->session.open(port_.c_str())) {
            const int err = impl_->session.lastErrno();
            throw TriggerLogError(fmt::format("[TriggerLog] Cannot open sensor trigger port '{}': {} (errno {}){}",
                                              port_, std::strerror(err), err, OpenErrnoHint(err)));
        }
        g_log.Info("[TriggerLog] Sensor trigger port '{}' open", port_);
    }


    void SensorTriggerLog::Start(const std::filesystem::path &log_path,
                                 const std::vector<std::pair<int, double> > &channel_freqs_hz) {
        if (started_) {
            throw TriggerLogError("[TriggerLog] Trigger log session already running");
        }
        if (!impl_->session.start(log_path.string(), channel_freqs_hz)) {
            throw TriggerLogError("[TriggerLog] Cannot start the trigger log session (failed to create '" +
                                  log_path.string() + "' or to command the board)");
        }
        log_path_ = log_path;
        // Seed the stall baseline unconditionally, so a transient stat failure
        // cannot leave last_growth at the steady_clock epoch (an instant stall).
        struct stat st{};
        impl_->growth.Reset(::stat(log_path.c_str(), &st) == 0 ? static_cast<std::int64_t>(st.st_size) : -1,
                            std::chrono::steady_clock::now());
        started_ = true;
        std::string rates;
        for (const auto &[ch, hz]: channel_freqs_hz) {
            rates += fmt::format("{}trig[{}]={}", rates.empty() ? "" : ", ", ch,
                                 hz > 0.0 ? fmt::format("{:g} Hz", hz) : "off");
        }
        g_log.Info("[TriggerLog] Trigger pulses running ({}); timing log recording to {}",
                   rates.empty() ? "config.h default rates" : rates, log_path_.string());
    }


    void SensorTriggerLog::Stop() {
        if (!started_) {
            return;
        }
        const double stalled = StalledSeconds();
        started_ = false;
        impl_->session.stop(); // sends STOP, drains the tail (~300 ms), closes the file
        if (!impl_->session.ok()) {
            g_log.Warn("[TriggerLog] Timing session '{}' failed integrity checks (I/O, protocol, event loss or missing STOP acknowledgement)", log_path_.string());
        } else if (stalled > kStopStallWarnS) {
            g_log.Warn("[TriggerLog] Link was stalled for {:.0f} s at stop — the STOP command may not have "
                       "reached the board (it keeps pulsing until the next session start)", stalled);
        } else {
            g_log.Info("[TriggerLog] Trigger pulses stopped; timing log closed");
        }
    }


    bool SensorTriggerLog::Ok() const {
        return impl_->session.ok();
    }


    double SensorTriggerLog::StalledSeconds() const {
        if (!started_) {
            return -1.0;
        }
        struct stat st{};
        if (::stat(log_path_.c_str(), &st) != 0) {
            return -1.0; // Start() created the file; unreadable = fs trouble, not a stall
        }
        // impl_ is a pointer: mutating through it is fine in a const member
        return impl_->growth.Update(static_cast<std::int64_t>(st.st_size), std::chrono::steady_clock::now());
    }
} // namespace fx10
