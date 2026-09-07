#include "session.h"

#include <QMetaObject>

#include "logger.h"
#include "sbf_parsers.h"
#include "string_util.h"
#include "time_util.h"

#include <algorithm>


namespace asterx {
    namespace {
        constexpr std::string_view kModule = "AsteRx";
        common::DriverLog g_log{std::string(kModule)};

        constexpr int kConnectTimeoutMs = 5000; // TCP connect deadline (QTcpSocket's own timeout is ~30 s)
        constexpr int kRetryDelayMs = 5000; // fixed backoff: the peer is a LAN receiver
        constexpr int kRecoveryTimeoutMs = 60000; // total, including reconfiguration and warm-up
        constexpr int kWatchdogMs = 30000; // link up but no SBF -> reconnect
        constexpr int kCommandTimeoutMs = 15000; // no reply to a configure command
        constexpr int kWarmupLogMs = 30000; // warm-up progress line

        // Command replies can be multi-line; compact them for single-line logging.
        using common::StringUtil::EqualsCi;
        using common::StringUtil::OneLine;

        std::string FirstLine(const std::string &s) {
            const auto end = s.find_first_of("\r\n");
            return end == std::string::npos ? s : s.substr(0, end);
        }
    } // namespace

    Session::Session(AppConfig cfg, QObject *parent) : QObject(parent), cfg_(std::move(cfg)),
                                                       writer_(
                                                           SbfWriter::Config{
                                                               cfg_.output_dir,
                                                               cfg_.file_prefix,
                                                               cfg_.rotate_bytes,
                                                               std::chrono::seconds(cfg_.rotate_interval_seconds),
                                                           },
                                                           cfg_.write_queue_bytes,
                                                           [this] {
                                                               // Writer thread -> Qt thread; dropped if this
                                                               // object is gone before the event runs
                                                               QMetaObject::invokeMethod(
                                                                   this, [this] { OnWriteFailed(); },
                                                                   Qt::QueuedConnection);
                                                           }),
                                                       prewarm_writer_(SbfWriter::Config{
                                                           cfg_.output_dir / "prewarm", cfg_.file_prefix,
                                                           cfg_.rotate_bytes,
                                                           std::chrono::seconds(cfg_.rotate_interval_seconds)}),
                                                       live_(SbfLiveRecorder::Config{cfg_.output_dir, cfg_.live_csv}) {
        connect_timer_.setSingleShot(true);
        retry_timer_.setSingleShot(true);
        recovery_timer_.setSingleShot(true);
        connect(&recovery_timer_, &QTimer::timeout, this, [this]() {
            FailStartup("recovery deadline exceeded (60 s); GPS time association interrupted");
        });
        watchdog_timer_.setSingleShot(true);
        command_timer_.setSingleShot(true);

        connect(&retry_timer_, &QTimer::timeout, this, [this]() {
            if (state_ == State::kBackoff) {
                StartConnect();
            }
        });
        connect(&connect_timer_, &QTimer::timeout, this, [this]() {
            if (state_ == State::kConnecting) {
                // Fatal at startup (fail_startup_); a mid-run link loss keeps reconnecting
                HandleFailure("could not connect to " + cfg_.host + ":" + std::to_string(cfg_.ctrl_port) +
                                " within " + std::to_string(kConnectTimeoutMs / 1000) + " s");
            }
        });
        connect(&watchdog_timer_, &QTimer::timeout, this, [this]() { OnWatchdog(); });
        connect(&command_timer_, &QTimer::timeout, this, [this]() { OnCommandTimeout(); });
        connect(&stats_timer_, &QTimer::timeout, this, [this]() { OnStatsTimer(); });
        connect(&warmup_log_timer_, &QTimer::timeout, this, [this]() {
            if (state_ == State::kWarmingUp) {
                g_log.Info("Receiver warming up: {}", warmup_.Progress());
            }
        });
    }


    Session::~Session() {
        // ~SsnRx emits connectionClosed while our members are being destroyed;
        // sever its connections to us first.
        if (rx_) {
            rx_->disconnect(this);
        }
    }


    void Session::ConnectRxSignals() {
        connect(rx_.get(), &SSN::SsnRx::connected, this, [this]() {
            connect_timer_.stop();
            g_log.Info("Connected");
            g_log.Trace("[TCP] TCP connected to {}:{}", cfg_.host, cfg_.ctrl_port);
            state_ = State::kWaitingDescriptor;
            // Covers waiting for the first prompt/descriptor as well
            command_timer_.start(kCommandTimeoutMs);
            rx_->sendPromptRequest();
        });

        connect(rx_.get(), &SSN::SsnRx::newConnectionDescriptor, this,
                [this](const QString &d) {
                    descriptor_ = d.toStdString();
                    g_log.Trace("[TCP] connection descriptor: {}", descriptor_);
                    if (state_ != State::kWaitingDescriptor) {
                        return;
                    }
                    try {
                        cmds_ = BuildCommandList(cfg_.receiver, descriptor_);
                    } catch (const ConfigError &e) {
                        FailStartup(std::string("cannot build command list: ") + e.what());
                        return;
                    }
                    whitelist_ = DriverCommandWhitelist(cmds_);
                    cmd_index_ = 0;
                    state_ = State::kConfiguring;
                    g_log.Info("Configuring receiver on {} ({} commands)", descriptor_, cmds_.size());
                    SendCurrentCommand();
                });

        connect(rx_.get(), &SSN::SsnRx::newCommandReply, this,
                [this](const QString &reply, bool error) {
                    HandleCommandReply(reply.toStdString(), error);
                });

        // lst replies open with a "$R;" header (newCommandReply), continue in
        // "$-- BLOCK i / n" frames and are complete at the following prompt.
        connect(rx_.get(), &SSN::SsnRx::newFormattedInformationBlock, this,
                [this](const QString &contents, int index, int count) {
                    if (state_ != State::kConfiguring || !lst_reply_seen_) {
                        g_log.Trace("[TCP] Stray formatted block {}/{} ignored", index, count);
                        return;
                    }
                    lst_text_ += contents.toStdString() + "\n";
                    command_timer_.start(kCommandTimeoutMs);
                });

        connect(rx_.get(), &SSN::SsnRx::newPrompt, this, [this](const QString &) {
            // Only a listing whose header has arrived is completed by a prompt;
            // SsnRx also emits the late prompt of the previous command and, after
            // 5 s of silence, the prompt answering its own "\r\n" keep-alive.
            if (state_ == State::kConfiguring && cmd_index_ < cmds_.size() && lst_reply_seen_) {
                CompleteListCommand();
            }
        });

        connect(rx_.get(), &SSN::SsnRx::newSBFBlock, this,
                [this](const QByteArray &block) { OnSbfBlock(block); });

        connect(rx_.get(), &SSN::SsnRx::sbfCRCError, this, [this]() {
            ++crc_errors_;
            OnStreamDamage("SBF CRC error", crc_errors_);
        });

        connect(rx_.get(), &SSN::SsnRx::discardedBytes, this, [this](int n) {
            discarded_bytes_ += static_cast<std::uint64_t>(n > 0 ? n : 0);
        });

        connect(rx_.get(), &SSN::SsnRx::communicationError, this,
                [this](const QString &msg) {
                    OnCommunicationError(msg.toStdString());
                });

        connect(rx_.get(), &SSN::SsnRx::connectionClosed, this, [this]() {
            HandleFailure("connection closed");
        });
    }


    void Session::Start() {
        writer_.Start();
        stats_timer_.start(cfg_.stats_period_ms);
        StartConnect();
    }


    void Session::StartConnect() {
        state_ = State::kConnecting;
        descriptor_.clear();
        cmds_.clear();
        cmd_index_ = 0;
        whitelist_.clear();
        lst_reply_seen_ = false;
        lst_text_.clear();

        // Fresh SsnRx per attempt (stale vendor parse buffer, see class comment);
        // disconnect the old instance first — its destructor emits connectionClosed.
        if (rx_) {
            rx_->disconnect(this);
        }
        rx_ = std::make_unique<SSN::SsnRx>();
        ConnectRxSignals();

        g_log.Info("Connecting to {}:{} ...", cfg_.host, cfg_.ctrl_port);
        connect_timer_.start(kConnectTimeoutMs);
        rx_->connectTcp(QString::fromStdString(cfg_.host), cfg_.ctrl_port);
    }


    void Session::SendCurrentCommand() {
        if (cmd_index_ >= cmds_.size()) {
            EnterWarmup();
            return;
        }
        const auto &cmd = cmds_[cmd_index_];
        lst_reply_seen_ = false;
        lst_text_.clear();
        g_log.Trace("[TCP] -> ({}/{}) {}", cmd_index_ + 1, cmds_.size(), RedactCmd(cmd.text));
        command_timer_.start(kCommandTimeoutMs);
        rx_->sendASCIICommand(QString::fromStdString(cmd.text));
    }


    void Session::HandleCommandReply(const std::string &reply, bool error) {
        if (state_ != State::kConfiguring || cmd_index_ >= cmds_.size()) {
            g_log.Trace("[TCP] Stray command reply ignored: {}", OneLine(RedactCmd(reply)));
            return;
        }
        const Command &cmd = cmds_[cmd_index_];
        const std::string shown_reply = OneLine(RedactCmd(reply));

        // p.62: "$R:" / "$R;" / "$R?" replies echo the command that produced them ("$R!" login
        // replies do not). Bind by name so a late or stray reply cannot shift the sequence.
        const bool echoes_command = reply.size() >= 3 && reply.compare(0, 2, "$R") == 0 &&
                                    (reply[2] == ':' || reply[2] == ';' || reply[2] == '?');
        if (echoes_command) {
            const std::string echoed = CommandNameOf(FirstLine(reply));
            const std::string expected = CommandNameOf(cmd.text);
            if (!echoed.empty() && !EqualsCi(echoed, expected)) {
                g_log.Warn("Reply echoes '{}' while '{}' is outstanding — ignored: {}", echoed, expected,
                           shown_reply);
                return; // keep waiting; the command timeout is still armed
            }
        }

        if (error) {
            command_timer_.stop();
            HandleFailure("receiver rejected '" + RedactCmd(cmd.text) + "': " + shown_reply);
            return;
        }

        command_timer_.stop();
        g_log.Trace("[TCP] <- {}", shown_reply);

        const bool listing_header = reply.size() >= 3 && reply.compare(0, 2, "$R") == 0 && reply[2] == ';';
        const bool list_kind = cmd.kind == CommandKind::kListConfig || cmd.kind == CommandKind::kListAntennaInfo;
        if (listing_header != list_kind) {
            HandleFailure(
                std::string(listing_header ? "unexpected listing reply to '" : "expected a listing reply to '")
                + RedactCmd(cmd.text) + "': " + shown_reply);
            return;
        }
        if (listing_header) {
            // The listing continues in formatted blocks; the prompt completes it
            lst_text_ = reply + "\n";
            lst_reply_seen_ = true;
            command_timer_.start(kCommandTimeoutMs);
            return;
        }

        try {
            switch (cmd.kind) {
                case CommandKind::kCheckCapabilities: {
                    const auto caps = ParseReceiverCapabilitiesReply(reply);
                    g_log.Info("AsteRx Capabilities: main={} aux1={}", caps.has_main, caps.has_aux1);
                    if (!caps.has_aux1) {
                        throw ConfigError(
                            "receiver capabilities do not include Aux1; dual-antenna collection is not available");
                    }
                    break;
                }
                case CommandKind::kVerifyEcho: {
                    const std::string payload = verify_first_fields(reply, cmd.verify_key, cmd.verify_fields);
                    // Log the readback line as the receiver reports it, so
                    // RedactCmd() recognises (and masks) an NtripSettings echo.
                    g_log.Info("Verified {}", OneLine(RedactCmd(cmd.verify_key + ", " + payload)));
                    break;
                }
                case CommandKind::kPlain:
                case CommandKind::kListAntennaInfo:
                case CommandKind::kListConfig:
                    break;
            }
        } catch (const ConfigError &e) {
            HandleFailure(e.what());
            return;
        }

        ++cmd_index_;
        SendCurrentCommand();
    }


    void Session::CompleteListCommand() {
        command_timer_.stop();
        const Command &cmd = cmds_[cmd_index_];

        if (cmd.kind == CommandKind::kListAntennaInfo) {
            const auto ids = ParseAntennaOverview(lst_text_);
            try {
                VerifyAntennaTypes(ids, cfg_.receiver.antenna);
            } catch (const ConfigError &e) {
                HandleFailure(e.what());
                return;
            }
            g_log.Info("Antenna types verified against {} receiver-known names (main '{}', aux '{}')", ids.size(),
                       cfg_.receiver.antenna.main_type, cfg_.receiver.antenna.aux_type);
        } else {
            const auto lines = ParseConfigFileListing(lst_text_);
            const auto non_permanent = static_cast<std::size_t>(std::count_if(
                lines.begin(), lines.end(), [](const ConfigLine &l) { return !l.permanent; }));
            try {
                VerifyConfigListing(lines, whitelist_);
            } catch (const ConfigError &e) {
                HandleFailure(e.what());
                return;
            }
            g_log.Info("Verified running configuration: {} command(s), all ours", non_permanent);
            for (const auto &l: lines) {
                g_log.Trace("[TCP] running config: {}", RedactCmd(l.full_line));
            }
        }

        lst_reply_seen_ = false;
        lst_text_.clear();
        ++cmd_index_;
        SendCurrentCommand();
    }


    void Session::EnterWarmup() {
        state_ = State::kWarmingUp;
        // The configure sequence succeeded: a link loss from here on reconnects instead of failing startup
        ever_configured_ = true;
        command_timer_.stop();
        warmup_.min_uptime_s = cfg_.receiver.warmup.min_uptime_s;
        warmup_.require_finetime = cfg_.receiver.warmup.require_finetime;
        warmup_.Reset();
        watchdog_timer_.start(kWatchdogMs);
        // Main's no-data watchdog stays off through warm-up: the receiver is
        // legitimately not producing recordable data yet.
        PublishLiveness(0);
        if (warmup_.Ready()) {
            EnterRecording();
            return;
        }
        g_log.Info("Streams enabled — warming up (min uptime {}, FINETIME {})",
                   warmup_.min_uptime_s > 0 ? FormatHms(static_cast<std::uint32_t>(warmup_.min_uptime_s)) : "off",
                   warmup_.require_finetime ? "required" : "not required");
        warmup_log_timer_.start(kWarmupLogMs);
    }


    void Session::EnterRecording() {
        if (!prewarm_writer_.EndSegment()) {
            FailStartup("cannot finish prewarm SBF context segment");
            return;
        }
        recovery_timer_.stop();
        state_ = State::kRecording;
        command_timer_.stop();
        warmup_log_timer_.stop();
        watchdog_timer_.start(kWatchdogMs);
        // Arms Main's no-data watchdog: from here on silence means lost data.
        PublishLiveness(common::TimeUtil::SteadyNowUs());
        if (warmup_.up_time_s) {
            g_log.Info("Acquisition started (receiver up {}, FINETIME {})", FormatHms(*warmup_.up_time_s),
                       warmup_.finetime ? "set" : "not set");
        } else {
            g_log.Info("Acquisition started (warm-up gate off)");
        }
        emit Configured();
    }


    void Session::OnReceiverStatus(const QByteArray &block) {
        const auto *data = reinterpret_cast<const std::uint8_t *>(block.constData());
        const auto size = static_cast<std::size_t>(block.size());
        if (sbf::PeekId(data, size) != sbf::kIdReceiverStatus) {
            return;
        }
        sbf::ReceiverStatus status;
        if (!sbf::ParseReceiverStatus(data, size, status)) {
            return;
        }
        temperature_ = status.temperature == 0
                           ? std::numeric_limits<double>::quiet_NaN()
                           : static_cast<double>(status.temperature) - 100.0;
        const std::uint32_t previous = warmup_.up_time_s.value_or(0);
        const bool went_back = warmup_.Observe(status.up_time, status.rx_status);
        if (state_ == State::kWarmingUp) {
            if (warmup_.Ready()) {
                EnterRecording();
            }
            return;
        }
        if (state_ == State::kRecording && went_back) {
            // A receiver reset normally drops the TCP session; this catches the
            // in-session case. Either way the observations across the reset are
            // gone: fail-fast, no re-warm-up into an incomplete recording.
            ++recording_errors_;
            g_log.Critical("Receiver up-time went backwards ({} -> {}): receiver reset during recording — stopping "
                           "the rig (fail-fast: the SBF recording would be incomplete)",
                           FormatHms(previous), FormatHms(status.up_time));
            Shutdown();
            emit FatalError();
        }
    }


    void Session::OnSbfBlock(const QByteArray &block) {
        if (state_ == State::kBackoff || state_ == State::kStopping) {
            return;
        }
        const auto *bytes = reinterpret_cast<const std::uint8_t *>(block.constData());
        const auto size = static_cast<std::size_t>(block.size());
        bool opened_gate = false;
        if (state_ != State::kRecording) {
            // Preserve low-frequency metadata/ephemerides before the acceptance
            // gate as native SBF in prewarm/. Never replay old blocks as new samples.
            live_.OnBlock(bytes, size);
            if (state_ == State::kWarmingUp) {
                watchdog_timer_.start(kWatchdogMs);
                OnReceiverStatus(block);
            }
            if (state_ != State::kRecording) {
                if (state_ != State::kStopping && !prewarm_writer_.WriteBlock(block)) {
                    FailStartup("cannot write prewarm SBF context");
                }
                return;
            }
            opened_gate = true; // the ReceiverStatus that opened the gate is recorded too
        }
        if (!writer_.Enqueue(block)) {
            // Refused: the disk fell behind for longer than the queue budget, or a
            // write already failed. Neither is survivable for a recorder (a dropped
            // block is data loss; fail-fast)
            ++recording_errors_;
            g_log.Critical("SBF write queue refused a block ({}) — stopping acquisition", writer_.LastError());
            Shutdown();
            emit FatalError();
            return;
        }
        if (!opened_gate) {
            // Live CSV side channel: noexcept, degrades itself; the .sbf path stays authoritative
            live_.OnBlock(bytes, size);
            OnReceiverStatus(block);
        }
        if (state_ == State::kRecording) {
            watchdog_timer_.start(kWatchdogMs);
            PublishLiveness(common::TimeUtil::SteadyNowUs());
        }
    }


    void Session::OnCommunicationError(const std::string &message) {
        // SsnRx reports recoverable per-block parse diagnostics through this same
        // signal with the socket still open (ssnrx.cpp: parseSBF). They are
        // counters, not link failures.
        if (message.find("SBF CRC error") != std::string::npos) {
            return; // already counted via the sbfCRCError signal
        }
        if (message.find("Invalid SBF block length") != std::string::npos) {
            ++length_errors_;
            OnStreamDamage("invalid SBF block length", length_errors_);
            return;
        }
        HandleFailure("communication error: " + message);
    }


    void Session::OnStreamDamage(const char *what, std::uint64_t total) {
        if (state_ == State::kStopping) {
            return;
        }
        if (state_ != State::kRecording) {
            // Only the prewarm context is affected; the parser resyncs on the next block
            g_log.Warn("[TCP] {} before recording (total {}); prewarm context only", what, total);
            return;
        }
        // A damaged block is a lost block: the .sbf is incomplete from here on
        ++recording_errors_;
        g_log.Critical("{} during recording (total {}): a block is lost — stopping the rig (fail-fast: the SBF "
                       "recording would be incomplete)", what, total);
        Shutdown();
        emit FatalError();
    }


    void Session::OnWriteFailed() {
        if (state_ == State::kStopping) {
            return;
        }
        ++recording_errors_;
        g_log.Critical("SBF write failed ({}) — stopping acquisition (disk full or output directory lost?)",
                       writer_.LastError());
        Shutdown();
        emit FatalError();
    }


    void Session::HandleFailure(const std::string &reason) {
        if (state_ == State::kStopping || state_ == State::kBackoff) {
            return;
        }
        if (!ever_configured_) {
            FailStartup(reason);
            return;
        }
        if (state_ == State::kRecording) {
            // Fail-fast: the SBF stream has a gap from here on. Reconnecting would
            // resurrect a recording that is already incomplete, so end the rig now
            ++recording_errors_;
            g_log.Critical("Link failure during recording: {} — stopping the rig (fail-fast: the SBF recording "
                           "would be incomplete; no reconnect)", reason);
            Shutdown();
            emit FatalError();
            return;
        }
        ++recovery_events_; // reconnect may recover transport, never the missing observations
        g_log.Warn("Connect Failed: {} — reconnecting in {} s", reason, kRetryDelayMs / 1000);
        // Backoff before closing: closeConnection() may re-emit connectionClosed synchronously
        state_ = State::kBackoff;
        connect_timer_.stop();
        command_timer_.stop();
        watchdog_timer_.stop();
        warmup_log_timer_.stop();
        // A separate total deadline covers backoff, reconnect, configuration
        // and warm-up. A retry must never grant another full recovery window.
        if (!recovery_timer_.isActive()) {
            recovery_timer_.start(kRecoveryTimeoutMs);
        }
        PublishLiveness(0);
        if (rx_) {
            rx_->closeConnection(); // the socket may still be open (e.g. prompt failure)
        }
        // Only the prewarm context is open before Recording (the main writer has
        // not been fed yet), so that is the segment a reconnect closes
        if (!prewarm_writer_.EndSegment()) {
            FailStartup("cannot finish prewarm SBF segment during recovery");
            return;
        }
        live_.Flush(); // keep the GUI tail current across the link gap; files stay open
        retry_timer_.start(kRetryDelayMs);
    }


    void Session::FailStartup(const std::string &reason) {
        if (state_ == State::kStopping) {
            return;
        }
        g_log.Critical("Startup Failed: {} — check host, credentials and receiver state, then relaunch", reason);
        Shutdown();
        emit FatalError();
    }


    void Session::OnWatchdog() {
        if (state_ != State::kRecording && state_ != State::kWarmingUp) {
            return;
        }
        HandleFailure("no SBF data for " + std::to_string(kWatchdogMs / 1000) +
                        " s (receiver reconfigured or stalled?)");
    }


    void Session::OnCommandTimeout() {
        if (state_ != State::kConfiguring && state_ != State::kWaitingDescriptor) {
            return;
        }
        const std::string what =
                (state_ == State::kWaitingDescriptor)
                    ? std::string("no prompt/descriptor from receiver")
                    : std::string(lst_reply_seen_ ? "no prompt after the listing of '" : "no reply to '") +
                      RedactCmd(cmd_index_ < cmds_.size() ? cmds_[cmd_index_].text : "?") +
                      "'";
        HandleFailure(what + " within " + std::to_string(kCommandTimeoutMs / 1000) + " s");
    }


    void Session::OnStatsTimer() {
        if (state_ == State::kWarmingUp) {
            g_log.Info(
                "[Statistics] warmup={}  finetime={}  crc_fail={}  length_errors={}  discarded_bytes={}  temperature={}°C",
                warmup_.up_time_s
                    ? FormatHms(*warmup_.up_time_s) + "/" +
                      (warmup_.min_uptime_s > 0
                           ? FormatHms(static_cast<std::uint32_t>(warmup_.min_uptime_s))
                           : "off")
                    : std::string("waiting"),
                warmup_.finetime ? 1 : 0, crc_errors_, length_errors_, discarded_bytes_, temperature_);
            return;
        }
        if (state_ != State::kRecording) {
            return;
        }
        const auto s = writer_.GetStats();
        g_log.Info(
            "[Statistics] blocks={}  bytes={}  files={}  crc_fail={}  length_errors={}  discarded_bytes={}  "
            "temperature={}°C  queue_pending={}  queue_max={}",
            s.records_written, s.bytes_written, s.files_opened, crc_errors_, length_errors_, discarded_bytes_,
            temperature_, s.pending_bytes, s.max_pending_bytes);
    }


    nlohmann::ordered_json Session::FinalStatistics() const {
        const auto s = writer_.GetStats();
        const auto &pre = prewarm_writer_.Stats();
        return {{"blocks_written", s.records_written}, {"bytes_written", s.bytes_written},
                {"files_opened", s.files_opened}, {"write_queue_max_pending_bytes", s.max_pending_bytes},
                {"prewarm_blocks", pre.records_written},
                {"prewarm_bytes", pre.bytes_written}, {"crc_errors", crc_errors_},
                {"length_errors", length_errors_}, {"discarded_stream_bytes", discarded_bytes_},
                {"recovery_events", recovery_events_}, {"recording_errors", recording_errors_},
                {"known_acquisition_incomplete", RecordingIncomplete()}};
    }

    void Session::Shutdown() {
        if (state_ == State::kStopping) {
            return;
        }
        state_ = State::kStopping;
        PublishLiveness(0);
        connect_timer_.stop();
        retry_timer_.stop();
        recovery_timer_.stop();
        watchdog_timer_.stop();
        command_timer_.stop();
        stats_timer_.stop();
        warmup_log_timer_.stop();
        if (rx_) {
            rx_->closeConnection(); // closing the socket also stops the IPxx streams
        }
        // Drains the queue (every accepted block reaches the disk) and joins the writer thread
        const bool clean = writer_.Close();
        const bool prewarm_clean = prewarm_writer_.close();
        live_.close();
        if (!clean || !prewarm_clean) {
            ++recording_errors_;
            g_log.Error("SBF final flush/close failed ({}); recording is INCOMPLETE", writer_.LastError());
            emit FatalError();
        }
        const auto s = writer_.GetStats();
        const auto &lv = live_.GetStats();
        const auto &prewarm = prewarm_writer_.Stats();
        g_log.Info("[Statistics] Prewarm context: blocks={} bytes={} files={} (separate prewarm/ SBF, not accepted samples)",
                   prewarm.records_written, prewarm.bytes_written, prewarm.files_opened);
        g_log.Info("Acquisition stopped ({} blocks delivered)", s.records_written);
        g_log.Info(
            "[Statistics] Final: blocks={}  bytes={}  files={}  crc_fail={}  length_errors={}  discarded_bytes={}  "
            "temperature={}°C  live_rows={}  live_parse_errors={}  queue_max={}  recording_errors={}",
            s.records_written, s.bytes_written, s.files_opened, crc_errors_, length_errors_, discarded_bytes_,
            temperature_, lv.rows_written, lv.parse_errors, s.max_pending_bytes, recording_errors_);
    }
} // namespace asterx
