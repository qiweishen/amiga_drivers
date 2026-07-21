#include "session.hpp"

#include "asterx_log.hpp"
#include "string_util.h"


namespace asterx {
    namespace {
        constexpr int kRetryDelayMs = 5000; // fixed backoff: the peer is a LAN receiver
        constexpr int kWatchdogMs = 30000; // link up but no SBF -> reconnect
        constexpr int kCommandTimeoutMs = 15000; // no reply to a configure command
        constexpr int kStatsPeriodMs = 5000;

        // Command replies can be multi-line; compact them for single-line logging.
        using Common::StringUtil::OneLine;
    } // namespace

    Session::Session(AppConfig cfg, QObject *parent) : QObject(parent), cfg_(std::move(cfg)),
                                                       writer_(SbfWriter::Config{
                                                           cfg_.output_dir, cfg_.file_prefix, cfg_.rotate_bytes,
                                                           std::chrono::seconds(cfg_.rotate_interval_seconds),
                                                       }) {
        retry_timer_.setSingleShot(true);
        watchdog_timer_.setSingleShot(true);
        command_timer_.setSingleShot(true);

        connect(&retry_timer_, &QTimer::timeout, this, [this]() {
            if (state_ == State::Backoff) {
                start_connect_();
            }
        });
        connect(&watchdog_timer_, &QTimer::timeout, this, [this]() { on_watchdog_(); });
        connect(&command_timer_, &QTimer::timeout, this, [this]() { on_command_timeout_(); });
        connect(&stats_timer_, &QTimer::timeout, this, [this]() { on_stats_timer_(); });
    }


    Session::~Session() {
        // ~SsnRx emits connectionClosed while our members are being destroyed;
        // sever its connections to us first.
        if (rx_) {
            rx_->disconnect(this);
        }
    }


    void Session::connect_rx_signals_() {
        connect(rx_.get(), &SSN::SsnRx::connected, this, [this]() {
            log::info("[session] TCP connected to {}:{}", cfg_.host, cfg_.ctrl_port);
            state_ = State::WaitingDescriptor;
            // Covers waiting for the first prompt/descriptor as well
            command_timer_.start(kCommandTimeoutMs);
            rx_->sendPromptRequest();
        });

        connect(rx_.get(), &SSN::SsnRx::newConnectionDescriptor, this,
                [this](const QString &d) {
                    descriptor_ = d.toStdString();
                    log::info("[session] connection descriptor: {}", descriptor_);
                    if (state_ != State::WaitingDescriptor) {
                        return;
                    }
                    try {
                        cmds_ = build_command_list(cfg_.receiver, descriptor_);
                    } catch (const ConfigError &e) {
                        fail_startup_(std::string("cannot build command list: ") + e.what());
                        return;
                    }
                    cmd_index_ = 0;
                    state_ = State::Configuring;
                    send_current_command_();
                });

        connect(rx_.get(), &SSN::SsnRx::newCommandReply, this,
                [this](const QString &reply, bool error) {
                    handle_command_reply_(reply.toStdString(), error);
                });

        connect(rx_.get(), &SSN::SsnRx::newSBFBlock, this,
                [this](const QByteArray &block) { on_sbf_block_(block); });

        connect(rx_.get(), &SSN::SsnRx::sbfCRCError, this, [this]() {
            ++crc_errors_;
            log::warn("[session] SBF CRC error (total {})", crc_errors_);
        });

        connect(rx_.get(), &SSN::SsnRx::discardedBytes, this, [this](int n) {
            discarded_bytes_ += static_cast<std::uint64_t>(n > 0 ? n : 0);
        });

        connect(rx_.get(), &SSN::SsnRx::communicationError, this,
                [this](const QString &msg) {
                    on_communication_error_(msg.toStdString());
                });

        connect(rx_.get(), &SSN::SsnRx::connectionClosed, this, [this]() {
            handle_failure_("connection closed");
        });
    }


    void Session::start() {
        stats_timer_.start(kStatsPeriodMs);
        start_connect_();
    }


    void Session::start_connect_() {
        state_ = State::Connecting;
        descriptor_.clear();
        cmds_.clear();
        cmd_index_ = 0;

        // Fresh SsnRx per attempt (stale vendor parse buffer, see class comment);
        // disconnect the old instance first — its destructor emits connectionClosed.
        if (rx_) {
            rx_->disconnect(this);
        }
        rx_ = std::make_unique<SSN::SsnRx>();
        connect_rx_signals_();

        log::info("[session] connecting to {}:{} ...", cfg_.host, cfg_.ctrl_port);
        rx_->connectTcp(QString::fromStdString(cfg_.host), cfg_.ctrl_port);
    }


    void Session::send_current_command_() {
        if (cmd_index_ >= cmds_.size()) {
            enter_recording_();
            return;
        }
        const auto &cmd = cmds_[cmd_index_];
        log::info("[session] -> ({}/{}) {}", cmd_index_ + 1, cmds_.size(),
                  redact_cmd(cmd.text));
        command_timer_.start(kCommandTimeoutMs);
        rx_->sendASCIICommand(QString::fromStdString(cmd.text));
    }


    void Session::handle_command_reply_(const std::string &reply, bool error) {
        if (state_ != State::Configuring || cmd_index_ >= cmds_.size()) {
            log::debug("[session] stray command reply ignored: {}", OneLine(reply));
            return;
        }
        command_timer_.stop();
        const Command &cmd = cmds_[cmd_index_];

        if (error) {
            if (cmd.kind == CommandKind::ToleratedError) {
                log::debug("[session] tolerated error for '{}': {}",
                           redact_cmd(cmd.text), OneLine(reply));
            } else {
                handle_failure_("receiver rejected '" + redact_cmd(cmd.text) +
                                "': " + OneLine(reply));
                return;
            }
        } else {
            log::debug("[session] <- {}", OneLine(reply));
            try {
                switch (cmd.kind) {
                    case CommandKind::CheckCapabilities: {
                        const auto caps = parse_receiver_capabilities_reply(reply);
                        log::info("[session] capabilities: main={} aux1={} meas={}ms pvt={}ms ins={}ms",
                                  caps.has_main, caps.has_aux1,
                                  caps.measurement_interval_ms, caps.pvt_interval_ms,
                                  caps.ins_interval_ms);
                        if (cfg_.receiver.require_aux1 && !caps.has_aux1) {
                            throw ConfigError("receiver capabilities do not include Aux1; "
                                "dual-antenna collection is not available");
                        }
                        break;
                    }
                    case CommandKind::VerifyImuOrientation:
                        verify_imu_orientation_reply(reply, cfg_.receiver);
                        log::info("[session] verified IMU orientation ({})",
                                  cfg_.receiver.imu_orientation_mode);
                        break;
                    case CommandKind::VerifyLeverArm:
                        verify_ins_ant_lever_arm_reply(reply, cfg_.receiver.ant_lever_arm_m);
                        log::info("[session] verified INS antenna lever arm");
                        break;
                    case CommandKind::VerifyGnssAttitude:
                        verify_gnss_attitude_reply(reply, cfg_.receiver.gnss_attitude_mode);
                        log::info("[session] verified GNSS attitude mode ({})",
                                  cfg_.receiver.gnss_attitude_mode);
                        break;
                    case CommandKind::VerifyAttitudeOffset:
                        verify_attitude_offset_reply(reply, cfg_.receiver.attitude_offset_deg);
                        log::info("[session] verified attitude offset");
                        break;
                    case CommandKind::Plain:
                    case CommandKind::ToleratedError:
                        break;
                }
            } catch (const ConfigError &e) {
                handle_failure_(e.what());
                return;
            }
        }

        ++cmd_index_;
        send_current_command_();
    }


    void Session::enter_recording_() {
        state_ = State::Recording;
        ever_configured_ = true;
        command_timer_.stop();
        watchdog_timer_.start(kWatchdogMs);
        log::info("[session] configured; recording SBF from {} ({} commands OK)",
                  descriptor_, cmds_.size());
        emit configured();
    }


    void Session::on_sbf_block_(const QByteArray &block) {
        if (state_ == State::Backoff || state_ == State::Stopping) {
            // Never write while the session considers the link down: segment
            // boundaries must coincide with link gaps.
            return;
        }
        try {
            writer_.write_block(block);
        } catch (const std::exception &e) {
            // Disk failure is not survivable for a recorder.
            log::critical("[session] disk write failed: {}", e.what());
            shutdown();
            emit fatalError();
            return;
        }
        if (state_ == State::Recording) {
            watchdog_timer_.start(kWatchdogMs);
        }
    }


    void Session::on_communication_error_(const std::string &message) {
        // SsnRx reports recoverable per-block parse diagnostics through this same
        // signal with the socket still open (ssnrx.cpp: parseSBF). They are
        // counters, not link failures.
        if (message.find("SBF CRC error") != std::string::npos) {
            return; // already counted via the sbfCRCError signal
        }
        if (message.find("Invalid SBF block length") != std::string::npos) {
            ++length_errors_;
            log::warn("[session] invalid SBF block length (total {})", length_errors_);
            return;
        }
        handle_failure_("communication error: " + message);
    }


    void Session::handle_failure_(const std::string &reason) {
        if (state_ == State::Stopping || state_ == State::Backoff) {
            return;
        }
        if (!ever_configured_) {
            fail_startup_(reason);
            return;
        }
        log::warn("[session] {} — reconnecting in {} s", reason, kRetryDelayMs / 1000);
        // Set Backoff BEFORE closing: closeConnection() may synchronously re-emit
        // connectionClosed, which the guard above absorbs.
        state_ = State::Backoff;
        command_timer_.stop();
        watchdog_timer_.stop();
        if (rx_) {
            rx_->closeConnection(); // the socket may still be open (e.g. prompt failure)
        }
        writer_.end_segment();
        retry_timer_.start(kRetryDelayMs);
    }


    void Session::fail_startup_(const std::string &reason) {
        if (state_ == State::Stopping) {
            return;
        }
        log::critical("[session] startup failed: {} — check host, credentials and "
                      "receiver state, then relaunch", reason);
        shutdown();
        emit fatalError();
    }


    void Session::on_watchdog_() {
        if (state_ != State::Recording) {
            return;
        }
        handle_failure_("no SBF data for " + std::to_string(kWatchdogMs / 1000) +
                        " s (receiver reconfigured or stalled?)");
    }


    void Session::on_command_timeout_() {
        if (state_ != State::Configuring && state_ != State::WaitingDescriptor) {
            return;
        }
        const std::string what =
                (state_ == State::WaitingDescriptor)
                    ? std::string("no prompt/descriptor from receiver")
                    : "no reply to '" +
                      redact_cmd(cmd_index_ < cmds_.size() ? cmds_[cmd_index_].text : "?") +
                      "'";
        handle_failure_(what + " within " + std::to_string(kCommandTimeoutMs / 1000) + " s");
    }


    void Session::on_stats_timer_() {
        if (state_ != State::Recording) {
            return;
        }
        const auto &s = writer_.stats();
        log::info("[session] blocks={} bytes={} files={} crc_fail={} len_fail={} discarded={}",
                  s.blocks_written, s.bytes_written, s.files_opened,
                  crc_errors_, length_errors_, discarded_bytes_);
    }


    void Session::shutdown() {
        if (state_ == State::Stopping) {
            return;
        }
        state_ = State::Stopping;
        retry_timer_.stop();
        watchdog_timer_.stop();
        command_timer_.stop();
        stats_timer_.stop();
        if (rx_) {
            rx_->closeConnection(); // closing the socket also stops the IPxx streams
        }
        writer_.close();
        const auto &s = writer_.stats();
        log::info("[session] final stats: blocks={} bytes={} files={} crc_fail={} len_fail={} discarded={}",
                  s.blocks_written, s.bytes_written, s.files_opened,
                  crc_errors_, length_errors_, discarded_bytes_);
    }
} // namespace asterx
