#include "sensor_sync_hub.h"

#include <algorithm>

#include "logger.h"
#include "trigger_groups.h"


namespace common {
    namespace {
        DriverLog g_log{"SensorSync"};

        constexpr int kChannelCount = 4;

        // The real board: SensorSyncLog owns the serial session and the timing log
        class LogBackend final : public SensorSyncBackend {
        public:
            void Open(const std::string &port) override {
                log_ = std::make_unique<SensorSyncLog>(port);
                log_->Open();
            }

            void Start(const std::filesystem::path &log_path,
                       const std::vector<std::pair<int, double> > &channel_freqs_hz) override {
                if (!log_) {
                    throw SensorSyncError("[TriggerLog] session START before the port was opened");
                }
                log_->Start(log_path, channel_freqs_hz);
            }

            void Stop() override {
                if (log_) {
                    log_->Stop();
                }
            }

            bool Ok() const override { return !log_ || log_->Ok(); }

            double StalledSeconds() const override { return log_ ? log_->StalledSeconds() : -1.0; }

        private:
            std::unique_ptr<SensorSyncLog> log_;
        };
    } // namespace


    SensorSyncHub::SensorSyncHub(std::filesystem::path log_path, std::string port,
                                 std::unique_ptr<SensorSyncBackend> backend)
        : log_path_(std::move(log_path)), backend_(backend ? std::move(backend) : std::make_unique<LogBackend>()),
          port_(std::move(port)) {
    }


    SensorSyncHub::~SensorSyncHub() {
        Disarm("~SensorSyncHub");
    }


    void SensorSyncHub::FailLocked(const std::string &what) {
        failed_ = true;
        if (last_error_.empty()) {
            last_error_ = what;
        }
    }


    void SensorSyncHub::Register(const std::string &owner, int channel, double pulse_hz) {
        std::lock_guard<std::mutex> lock(mu_);
        if (owner.empty()) {
            throw SensorSyncError("[TriggerLog] SensorSync participant needs a name");
        }
        if (port_.empty()) {
            throw SensorSyncError("[TriggerLog] '" + owner + "' needs the SensorSync board but no serial port is "
                                  "configured: set 'Sensor Trigger: Port' in config-main.yaml (a "
                                  "/dev/serial/by-id/... path)");
        }
        if (started_ || ended_) {
            throw SensorSyncError("[TriggerLog] '" + owner + "' registered after the SensorSync session " +
                                  (ended_ ? "ended" : "started") + "; participants register during bring-up");
        }
        if (channel < 0 || channel >= kChannelCount) {
            throw SensorSyncError("[TriggerLog] '" + owner + "': SensorSync channel " + std::to_string(channel) +
                                  " is outside 0..3 (0/1 = FX pair, 2/3 = JAI pair)");
        }
        const auto group = static_cast<trigger::Group>(channel / 2);
        if (!trigger::validRate(group, pulse_hz)) {
            throw SensorSyncError("[TriggerLog] '" + owner + "': pulse rate " + std::to_string(pulse_hz) +
                                  " Hz is not valid for the SensorSync " + trigger::name(group) + " group (" +
                                  (group == trigger::Group::FX ? "0 or >= 20 Hz" : "0 or 1..10 Hz") + ")");
        }
        if (participants_.size() >= static_cast<std::size_t>(kChannelCount)) {
            throw SensorSyncError("[TriggerLog] '" + owner + "': the board has only 4 channels");
        }
        for (const Participant &p: participants_) {
            if (p.owner == owner) {
                throw SensorSyncError("[TriggerLog] '" + owner + "' is already registered");
            }
            if (p.channel == channel) {
                throw SensorSyncError("[TriggerLog] '" + owner + "' and '" + p.owner +
                                      "' both claim SensorSync channel " + std::to_string(channel));
            }
            if (p.channel / 2 == channel / 2 && p.pulse_hz != pulse_hz) {
                // One hardware PWM rate per pair (session_rates.h rejects it on the wire too)
                throw SensorSyncError("[TriggerLog] '" + owner + "' asks the SensorSync " + trigger::name(group) +
                                      " pair for " + std::to_string(pulse_hz) + " Hz but '" + p.owner +
                                      "' already asked for " + std::to_string(p.pulse_hz) +
                                      " Hz; both outputs of a pair share one rate");
            }
        }
        if (!opened_) {
            // Opens, resynchronises and STOPs the board: before any camera is armed
            try {
                backend_->Open(port_);
            } catch (const SensorSyncError &e) {
                FailLocked(e.what());
                throw;
            }
            opened_ = true;
        }
        participants_.push_back({owner, channel, pulse_hz, false});
        g_log.Info("[TriggerLog] '{}' registered: channel {} ({} pair), {}", owner, channel, trigger::name(group),
                   pulse_hz > 0.0 ? fmt::format("{:g} Hz pulses", pulse_hz) : "strobe logging only (no pulses)");
    }


    bool SensorSyncHub::Arm(const std::string &owner) {
        std::lock_guard<std::mutex> lock(mu_);
        const auto it = std::find_if(participants_.begin(), participants_.end(),
                                     [&](const Participant &p) { return p.owner == owner; });
        if (it == participants_.end()) {
            throw SensorSyncError("[TriggerLog] '" + owner + "' armed without registering");
        }
        if (ended_) {
            throw SensorSyncError("[TriggerLog] '" + owner + "' armed after the SensorSync session ended; no restart");
        }
        if (failed_) {
            throw SensorSyncError("[TriggerLog] '" + owner + "' armed after a SensorSync failure: " + last_error_);
        }
        if (!it->armed) {
            it->armed = true;
            armed_at_[static_cast<std::size_t>(it - participants_.begin())] = std::chrono::steady_clock::now();
        }
        if (started_) {
            return true;
        }
        const bool all_armed = std::all_of(participants_.begin(), participants_.end(),
                                           [](const Participant &p) { return p.armed; });
        if (!all_armed) {
            std::string waiting;
            for (const Participant &p: participants_) {
                if (!p.armed) {
                    waiting += (waiting.empty() ? "" : ", ") + p.owner;
                }
            }
            g_log.Info("[TriggerLog] '{}' armed; pulses start once {} arm(s) too", owner, waiting);
            return false;
        }
        started_rates_ = PlannedRatesLocked();
        try {
            backend_->Start(log_path_, started_rates_);
        } catch (const SensorSyncError &e) {
            FailLocked(e.what());
            throw;
        }
        started_ = true;
        g_log.Info("[TriggerLog] session started by '{}' with {} participant(s); timing log {}", owner,
                   participants_.size(), log_path_.string());
        return true;
    }


    void SensorSyncHub::Disarm(const std::string &owner) {
        std::lock_guard<std::mutex> lock(mu_);
        if (ended_) {
            return;
        }
        ended_ = true; // also blocks a START that would come after another participant's teardown
        if (!started_) {
            if (!participants_.empty()) {
                g_log.Info("[TriggerLog] '{}' disarmed before the session started; no pulses were requested", owner);
            }
            return;
        }
        backend_->Stop();
        if (!backend_->Ok()) {
            FailLocked("timing session failed integrity checks at stop (see the [TriggerLog] lines above)");
        }
        g_log.Info("[TriggerLog] session ended by '{}'", owner);
    }


    bool SensorSyncHub::Registered(const std::string &owner) const {
        std::lock_guard<std::mutex> lock(mu_);
        return std::any_of(participants_.begin(), participants_.end(),
                           [&](const Participant &p) { return p.owner == owner; });
    }


    bool SensorSyncHub::HasParticipants() const {
        std::lock_guard<std::mutex> lock(mu_);
        return !participants_.empty();
    }


    bool SensorSyncHub::Started() const {
        std::lock_guard<std::mutex> lock(mu_);
        return started_;
    }


    bool SensorSyncHub::Ended() const {
        std::lock_guard<std::mutex> lock(mu_);
        return ended_;
    }


    bool SensorSyncHub::Ok() const {
        std::lock_guard<std::mutex> lock(mu_);
        return !failed_ && (!started_ || backend_->Ok());
    }


    double SensorSyncHub::StalledSeconds() const {
        std::lock_guard<std::mutex> lock(mu_);
        if (!started_ || ended_) {
            return -1.0;
        }
        return backend_->StalledSeconds();
    }


    double SensorSyncHub::WaitingSeconds(const std::string &owner) const {
        std::lock_guard<std::mutex> lock(mu_);
        if (started_ || ended_) {
            return 0.0;
        }
        for (std::size_t i = 0; i < participants_.size(); ++i) {
            if (participants_[i].owner == owner) {
                if (!participants_[i].armed) {
                    return 0.0;
                }
                return std::chrono::duration<double>(std::chrono::steady_clock::now() - armed_at_[i]).count();
            }
        }
        return 0.0;
    }


    std::string SensorSyncHub::LastError() const {
        std::lock_guard<std::mutex> lock(mu_);
        return last_error_;
    }


    std::string SensorSyncHub::Describe() const {
        std::lock_guard<std::mutex> lock(mu_);
        if (participants_.empty()) {
            return "no participant; the board is not used";
        }
        std::string out = "port " + port_;
        for (unsigned g = 0; g < trigger::GROUP_COUNT; ++g) {
            std::string members;
            double hz = 0.0;
            for (const Participant &p: participants_) {
                if (static_cast<unsigned>(p.channel / 2) == g) {
                    members += (members.empty() ? "" : ", ") + p.owner + " (ch" + std::to_string(p.channel) + ")";
                    hz = p.pulse_hz;
                }
            }
            out += std::string("; ") + trigger::name(static_cast<trigger::Group>(g)) + " pair ";
            if (members.empty()) {
                out += "off (no participant)";
            } else {
                out += (hz > 0.0 ? fmt::format("{:g} Hz", hz) : std::string("strobes only, no pulses")) + ": " + members;
            }
        }
        if (failed_) {
            out += "; FAILED: " + last_error_;
        } else if (ended_) {
            out += "; session ended";
        } else if (started_) {
            out += "; pulses running";
        } else {
            out += "; pulses start once every participant has armed its camera";
        }
        return out;
    }


    std::vector<std::pair<int, double> > SensorSyncHub::PlannedRatesLocked() const {
        std::vector<std::pair<int, double> > rates;
        for (unsigned g = 0; g < trigger::GROUP_COUNT; ++g) {
            double hz = 0.0;
            for (const Participant &p: participants_) {
                if (static_cast<unsigned>(p.channel / 2) == g) {
                    hz = p.pulse_hz; // equal within a pair (enforced by Register)
                }
            }
            rates.emplace_back(static_cast<int>(2 * g), hz); // the pair's first channel names the group
        }
        return rates;
    }


    std::vector<std::pair<int, double> > SensorSyncHub::PlannedRates() const {
        std::lock_guard<std::mutex> lock(mu_);
        return PlannedRatesLocked();
    }


    nlohmann::ordered_json SensorSyncHub::Summary() const {
        std::lock_guard<std::mutex> lock(mu_);
        nlohmann::ordered_json doc;
        doc["port"] = port_;
        doc["log_path"] = log_path_.string();
        doc["started"] = started_;
        doc["ended"] = ended_;
        doc["ok"] = !failed_ && (!started_ || backend_->Ok());
        doc["last_error"] = last_error_;
        nlohmann::ordered_json parts = nlohmann::ordered_json::array();
        for (const Participant &p: participants_) {
            parts.push_back({{"owner", p.owner}, {"channel", p.channel}, {"pulse_hz", p.pulse_hz}, {"armed", p.armed}});
        }
        doc["participants"] = std::move(parts);
        nlohmann::ordered_json rates = nlohmann::ordered_json::array();
        for (const auto &[ch, hz]: started_ ? started_rates_ : PlannedRatesLocked()) {
            rates.push_back({{"group", ch < 2 ? "FX" : "JAI"}, {"channels", ch < 2 ? "0/1" : "2/3"}, {"pulse_hz", hz}});
        }
        doc["group_rates"] = std::move(rates);
        return doc;
    }
} // namespace common
