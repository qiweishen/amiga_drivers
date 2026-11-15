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

        // Open + configure raw 115200 8N1, send STOP (ends a leftover session) and
        // confirm the board's #IDLE / #SESSION,STOP reply. Call before arming any
        // camera, with exclusive control of the board. Throws SensorSyncError;
        // logs startup diagnostics.
        void Open();

        // Begin a board session: sequence counters restart from 1 and the event
        // stream is captured into `log_path` (log format 3). `channel_freqs_hz`
        // retunes the trigger pairs for this run: 0/1 = FX, 2/3 = JAI, one rate
        // per pair, within sensor_sync_rates.h (the board answers other rates
        // with #ERR,bad_freq). {ch, 0} turns its pair off; an omitted pair keeps
        // its previous rate. Conflicting rates within a pair fail. #GROUP/#TRIG
        // record effective rates and #READY confirms hardware start. The rates
        // ride inside the START command. A failure ends the session; no
        // automatic restart. Throws SensorSyncError.
        void Start(const std::filesystem::path &log_path,
                   const std::vector<std::pair<int, double> > &channel_freqs_hz = {});

        // End the session (drains the tail, board -> idle). Idempotent, never throws.
        void Stop();

        // False = the port or the log file write failed (e.g. disk full), the
        // board sent #ERR, a #H health line reported drops != 0, or STOP was not
        // acknowledged; the session data is incomplete and the run must be
        // treated as failed.
        bool Ok() const;

        // Seconds since the timing log file last grew. The board emits a #H
        // health line at least every 5 s, but session_client.cpp writes through
        // an unflushed stdio buffer (typically 4 KiB), so the file grows in
        // buffer-sized steps: at low event rates (one Go-X at 1-2 Hz without the
        // FX10) a step can take longer than a stall threshold. A large value
        // otherwise means the USB link is down and events are being lost while
        // Ok() still reads true. -1 before Start() and after Stop().
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
