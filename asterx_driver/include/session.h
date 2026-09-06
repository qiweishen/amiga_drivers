#pragma once

#include <atomic>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

#include <ssnrx.h>

#include "app_config.h"
#include "commands.h"
#include "warmup_gate.h"
#include "sbf_live_recorder.h"
#include "sbf_writer.h"


namespace asterx {
    // Single-threaded driver lifecycle on the Qt event loop:
    //     Connecting -> WaitingDescriptor -> Configuring -> WarmingUp -> Recording, with
    //     Backoff (retry timer) looping back to Connecting.
    class Session : public QObject {
        Q_OBJECT

    public:
        explicit Session(AppConfig cfg, QObject *parent = nullptr);

        ~Session() override;

        // Kick off the first connection attempt
        void Start();

        // Flush + close everything; no reconnect. Safe to call more than once
        void Shutdown();

        // Qt-thread-only; collect after Shutdown and publish after joining that thread.
        bool RecordingIncomplete() const {
            return crc_errors_ != 0 || length_errors_ != 0 || recovery_events_ != 0;
        }
        nlohmann::ordered_json FinalStatistics() const;

        // Slot for Main's no-data watchdog (owned by AsterxDriverApp, outlives the session): steady-clock
        // micros of the last recorded block or of the recording start; 0 = watchdog does not apply.
        // Call before Start()
        void SetLivenessSlot(std::atomic<std::uint64_t> *slot) { liveness_ = slot; }

    signals:
        // Unrecoverable startup failure or disk-write failure; Shutdown() has
        // already Run (files flushed). The wrapper terminates the whole rig.
        void FatalError();

        // Emitted on every entry into Recording; the first emission is the
        // wrapper's Init() gate.
        void Configured();

    private:
        enum class State {
            kIdle,
            kConnecting,
            kWaitingDescriptor,
            kConfiguring,
            kWarmingUp, // raw blocks preserved under prewarm/; main .sbf gate is ReceiverStatus
            kRecording,
            kBackoff,
            kStopping,
        };

        // 0 = the watchdog does not apply; otherwise a steady-clock micros stamp
        void PublishLiveness(std::uint64_t reference_us) {
            if (liveness_ != nullptr) {
                liveness_->store(reference_us, std::memory_order_release);
            }
        }

        void ConnectRxSignals(); // wired to the CURRENT rx_ instance
        void StartConnect();

        void SendCurrentCommand();

        void EnterWarmup();

        void EnterRecording();

        // ReceiverStatus blocks drive the warm-up gate (and detect a receiver reset)
        void OnReceiverStatus(const QByteArray &block);

        void HandleCommandReply(const std::string &reply, bool error);

        // lst replies are complete at the prompt that follows their "$R;" header
        void CompleteListCommand();

        void OnCommunicationError(const std::string &message);

        // Any post-startup failure: close the socket (it may still be open),
        // close the segment, back off, retry with a fresh SsnRx. Before the
        // first successful configure it delegates to fail_startup_ instead.
        void HandleFailure(const std::string &reason);

        // Unrecoverable startup failure -> shutdown + emit FatalError().
        void FailStartup(const std::string &reason);

        void OnSbfBlock(const QByteArray &block);

        void OnWatchdog();

        void OnCommandTimeout();

        void OnStatsTimer();

        AppConfig cfg_;
        std::unique_ptr<SSN::SsnRx> rx_; // recreated per connection attempt
        SbfWriter writer_;
        SbfWriter prewarm_writer_; // preserve configuration/warm-up context, separate from accepted recording
        SbfLiveRecorder live_; // real-time CSV side channel (never fatal)

        State state_{State::kIdle};
        bool ever_configured_{false};

        std::string descriptor_;
        std::vector<Command> cmds_;
        std::size_t cmd_index_{0};
        std::vector<std::string> whitelist_; // set* names the driver sends

        // lst command in flight: "$R;" header seen / text collected so far
        bool lst_reply_seen_{false};
        std::string lst_text_;

        QTimer connect_timer_; // single-shot, TCP connect deadline
        QTimer retry_timer_; // single-shot, Backoff -> Connecting
        QTimer recovery_timer_; // total recovery deadline, never restarted by retries
        QTimer watchdog_timer_; // single-shot, restarted per SBF block
        QTimer command_timer_; // single-shot, per configure command
        QTimer stats_timer_; // periodic status line
        QTimer warmup_log_timer_; // periodic warm-up progress line

        WarmupGate warmup_;
        double temperature_{std::numeric_limits<double>::quiet_NaN()};

        std::atomic<std::uint64_t> *liveness_{nullptr}; // owned by AsterxDriverApp

        std::uint64_t crc_errors_{0};
        std::uint64_t length_errors_{0};
        std::uint64_t discarded_bytes_{0};
        std::uint64_t recovery_events_{0};
    };
} // namespace asterx
