#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <QObject>
#include <QTimer>

#include <ssnrx.h>

#include "app_config.hpp"
#include "commands.hpp"
#include "sbf_writer.hpp"

namespace asterx {
    // Single-threaded driver lifecycle on the Qt event loop:
    //   Connecting -> WaitingDescriptor -> Configuring -> Recording, with
    //   Backoff (retry timer) looping back to Connecting.
    // Commands and SBF share ONE TCP connection (SsnRx demultiplexes; the
    // streams die with the connection). A FRESH SsnRx is created per attempt:
    // the SDK keeps its parse buffer across closeConnection(), so a stale
    // partial block would poison the next connection.
    // Failure policy: any failure before the first successful configure emits
    // fatalError() after an orderly shutdown; afterwards every failure goes
    // through end_segment + reconnect + full reconfigure.
    class Session : public QObject {
        Q_OBJECT

    public:
        explicit Session(AppConfig cfg, QObject *parent = nullptr);

        ~Session() override;

        // Kick off the first connection attempt
        void start();

        // Flush + close everything; no reconnect. Safe to call more than once
        void shutdown();

    signals:
        // Unrecoverable startup failure or disk-write failure; shutdown() has
        // already run (files flushed). The wrapper terminates the whole rig.
        void fatalError();

        // Emitted on every entry into Recording; the first emission is the
        // wrapper's init() gate.
        void configured();

    private:
        enum class State {
            Idle,
            Connecting,
            WaitingDescriptor,
            Configuring,
            Recording,
            Backoff,
            Stopping,
        };

        void connect_rx_signals_(); // wired to the CURRENT rx_ instance
        void start_connect_();

        void send_current_command_();

        void enter_recording_();

        void handle_command_reply_(const std::string &reply, bool error);

        void on_communication_error_(const std::string &message);

        // Any post-startup failure: close the socket (it may still be open),
        // close the segment, back off, retry with a fresh SsnRx. Before the
        // first successful configure it delegates to fail_startup_ instead.
        void handle_failure_(const std::string &reason);

        // Unrecoverable startup failure -> shutdown + emit fatalError().
        void fail_startup_(const std::string &reason);

        void on_sbf_block_(const QByteArray &block);

        void on_watchdog_();

        void on_command_timeout_();

        void on_stats_timer_();

        AppConfig cfg_;
        std::unique_ptr<SSN::SsnRx> rx_; // recreated per connection attempt
        SbfWriter writer_;

        State state_{State::Idle};
        bool ever_configured_{false};

        std::string descriptor_;
        std::vector<Command> cmds_;
        std::size_t cmd_index_{0};

        QTimer retry_timer_; // single-shot, Backoff -> Connecting
        QTimer watchdog_timer_; // single-shot, restarted per SBF block
        QTimer command_timer_; // single-shot, per configure command
        QTimer stats_timer_; // periodic status line

        std::uint64_t crc_errors_{0};
        std::uint64_t length_errors_{0};
        std::uint64_t discarded_bytes_{0};
    };
} // namespace asterx
