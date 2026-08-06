#include "sensor_trigger_log.hpp"

#include <sys/stat.h>
#include <ctime>

#include "logger.h"
#include "session_client.cpp"


namespace fx10 {
    namespace {
        Common::DriverLog g_log{"FX10"};

        // No log growth for this long at stop() time = the STOP command very likely never reached the board
        constexpr double kStopStallWarnS = 15.0;
    } // namespace


    struct SensorTriggerLog::Impl {
        SensorSyncSession session;
    };


    SensorTriggerLog::SensorTriggerLog(std::string port)
        : impl_(std::make_unique<Impl>()), port_(std::move(port)) {
    }


    SensorTriggerLog::~SensorTriggerLog() {
        stop();
    }


    void SensorTriggerLog::open() {
        if (!impl_->session.open(port_.c_str())) {
            throw TriggerLogError("[TriggerLog] Cannot open sensor trigger port '" + port_ +
                                  "' (board unplugged, or another reader holds the port? check ls /dev/serial/by-id/)");
        }
        g_log.info("[TriggerLog] Sensor trigger port '{}' open", port_);
    }


    void SensorTriggerLog::start(const std::filesystem::path &log_path,
                                 const std::vector<std::pair<int, double> > &channel_freqs_hz) {
        if (started_) {
            throw TriggerLogError("[TriggerLog] Trigger log session already running");
        }
        if (!impl_->session.start(log_path.string(), channel_freqs_hz)) {
            throw TriggerLogError("[TriggerLog] Cannot start the trigger log session (failed to create '" +
                                  log_path.string() + "' or to command the board)");
        }
        log_path_ = log_path;
        started_ = true;
        std::string rates;
        for (const auto &[ch, hz]: channel_freqs_hz) {
            rates += fmt::format("{}trig[{}]={}", rates.empty() ? "" : ", ", ch,
                                 hz > 0.0 ? fmt::format("{:g} Hz", hz) : "off");
        }
        g_log.info("[TriggerLog] Trigger pulses running ({}); timing log recording to {}",
                   rates.empty() ? "config.h default rates" : rates, log_path_.string());
    }


    void SensorTriggerLog::stop() {
        if (!started_) {
            return;
        }
        const double stalled = stalledSeconds();
        started_ = false;
        impl_->session.stop(); // sends STOP, drains the tail (~300 ms), closes the file
        if (!impl_->session.ok()) {
            g_log.warn("[TriggerLog] Timing log '{}' had a write error — its tail is incomplete", log_path_.string());
        } else if (stalled > kStopStallWarnS) {
            g_log.warn("[TriggerLog] Link was stalled for {:.0f} s at stop — the STOP command may not have "
                       "reached the board (it keeps pulsing until the next session start)", stalled);
        } else {
            g_log.info("[TriggerLog] Trigger pulses stopped; timing log closed");
        }
    }


    bool SensorTriggerLog::ok() const {
        return impl_->session.ok();
    }


    double SensorTriggerLog::stalledSeconds() const {
        if (!started_) {
            return -1.0;
        }
        struct stat st{};
        if (::stat(log_path_.c_str(), &st) != 0) {
            return -1.0; // start() created the file; unreadable = fs trouble, not a stall
        }
        const auto now = ::time(nullptr);
        return now > st.st_mtime ? static_cast<double>(now - st.st_mtime) : 0.0;
    }
} // namespace fx10
