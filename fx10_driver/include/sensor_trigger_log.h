#pragma once

#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// SensorSync-Logger (Teensy 4.1, 3rd_party/External/sensor_trigger) session wrapper.
// The board fires the camera's hardware trigger pulses and streams a raw
// timing Log (trigger/strobe/PPS/NMEA events) over USB serial; this class
// drives the session protocol (START/STOP) and captures the stream into
// <session_dir>/sensor_trigger.log. GNSS frame times are recovered offline
// with submodule/sensor_trigger/postprocess.py — the driver stores no
// per-line timestamps of its own.

namespace fx10 {
    class TriggerLogError : public std::runtime_error {
    public:
        using std::runtime_error::runtime_error;
    };

    class SensorTriggerLog final {
    public:
        explicit SensorTriggerLog(std::string port);

        ~SensorTriggerLog(); // stops a running session (board -> idle)

        SensorTriggerLog(const SensorTriggerLog &) = delete;

        SensorTriggerLog &operator=(const SensorTriggerLog &) = delete;

        // Open + configure the serial port (raw 115200 8N1). Throws TriggerLogError.
        void Open();

        // Begin a board session: counters restart from 0 and the event stream is
        // captured into `log_path`. `channel_freqs_hz` retunes trigger channels
        // for this Run ({ch, hz}; hz <= 0 switches the channel off, omitted
        // channels keep their config.h rates; the board requires hz >= 1 and
        // echoes the EFFECTIVE rates in the log's #TRIG header). The rates ride
        // inside the START command, so a watchdog re-START restores them.
        // Throws TriggerLogError.
        void Start(const std::filesystem::path &log_path,
                   const std::vector<std::pair<int, double> > &channel_freqs_hz = {});

        // End the session (drains the tail, board -> idle). Idempotent, never throws.
        void Stop();

        // False = the log file write Failed (e.g. disk full); the session data is
        // incomplete and the run must be treated as failed.
        bool Ok() const;

        // Seconds since the timing log last grew. A healthy session appends
        // continuously (the board emits a #H health line at least every 5 s and
        // the client flushes every read), so a large value means the USB link is
        // down and events are being lost while Ok() still reads true — the
        // client's port-reopen loop retries silently. -1 before Start().
        double StalledSeconds() const;

        const std::filesystem::path &LogPath() const { return log_path_; }

    private:
        struct Impl; // SensorSyncSession lives in the .cpp only

        std::unique_ptr<Impl> impl_;
        std::string port_;
        std::filesystem::path log_path_;
        bool started_ = false;
    };
} // namespace fx10
