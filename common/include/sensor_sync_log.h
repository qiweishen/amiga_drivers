#pragma once

#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>


namespace common {
    class SensorSyncError : public std::runtime_error {
    public:
        using std::runtime_error::runtime_error;
    };

    class SensorSyncLog final {
    public:
        explicit SensorSyncLog(std::string port);

        ~SensorSyncLog(); // stops a running session (board -> idle)

        SensorSyncLog(const SensorSyncLog &) = delete;

        SensorSyncLog &operator=(const SensorSyncLog &) = delete;

        // Open + configure raw 115200 8N1, resynchronize command framing, send
        // STOP and confirm idle. Call before arming any camera, with exclusive
        // control of the board. Throws SensorSyncError; logs startup diagnostics.
        void Open();

        // Begin a board session: counters restart from 0 and the event stream is
        // captured into `log_path`. `channel_freqs_hz` retunes trigger channels
        // for this run. Protocol v2 pairs 0/1 = FX (>=20 Hz), 2/3 = JAI (1..10 Hz).
        // {ch, 0} disables its pair; omitted groups retain their previous rates.
        // Conflicting rates within a pair fail. #GROUP/#TRIG record effective
        // rates and #READY confirms hardware start. The rates ride inside the
        // START command. A failure ends the session; no automatic restart.
        // Throws SensorSyncError.
        void Start(const std::filesystem::path &log_path,
                   const std::vector<std::pair<int, double> > &channel_freqs_hz = {});

        // End the session (drains the tail, board -> idle). Idempotent, never throws.
        void Stop();

        // False = the log file write failed (e.g. disk full) or the protocol
        // reported loss/restart; the session data is incomplete and the run must
        // be treated as failed.
        bool Ok() const;

        // Seconds since the timing log last grew. A healthy session appends
        // continuously (the board emits a #H health line at least every 5 s and
        // the client flushes every read), so a large value means the USB link is
        // down and events are being lost while Ok() still reads true. -1 before
        // Start() and after Stop().
        double StalledSeconds() const;

        const std::filesystem::path &LogPath() const { return log_path_; }

        const std::string &Port() const { return port_; }

    private:
        struct Impl; // SensorSyncSession lives in the .cpp only

        std::unique_ptr<Impl> impl_;
        std::string port_;
        std::filesystem::path log_path_;
        bool started_ = false;
    };
} // namespace common
