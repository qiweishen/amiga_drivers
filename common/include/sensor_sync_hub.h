#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "sensor_sync_log.h"


namespace common {
    class SensorSyncBackend {
    public:
        virtual ~SensorSyncBackend() = default;

        virtual void Open(const std::string &port) = 0; // throws SensorSyncError

        virtual void Start(const std::filesystem::path &log_path,
                           const std::vector<std::pair<int, double> > &channel_freqs_hz) = 0; // throws

        virtual void Stop() = 0; // idempotent, never throws

        [[nodiscard]] virtual bool Ok() const = 0;

        [[nodiscard]] virtual double StalledSeconds() const = 0; // -1 outside a session
    };

    class SensorSyncHub {
    public:
        struct Participant {
            std::string owner;
            int channel = 0; // physical log ID 0..3
            double pulse_hz = 0.0; // 0 = no pulses on this participant's group, strobes still logged
            bool armed = false;
        };

        // `port`: the board's serial device (config-main.yaml "Sensor Trigger: Port"; a
        // /dev/serial/by-id/ path survives re-enumeration), "" = the rig has no board.
        // `backend` = nullptr uses the real board (SensorSyncLog); tests inject a fake
        SensorSyncHub(std::filesystem::path log_path, std::string port,
                      std::unique_ptr<SensorSyncBackend> backend = nullptr);

        ~SensorSyncHub(); // ends a running session

        SensorSyncHub(const SensorSyncHub &) = delete;

        SensorSyncHub &operator=(const SensorSyncHub &) = delete;

        // Throws SensorSyncError: no port configured, unknown/duplicate owner, channel
        // outside 0..3, a rate the group cannot run, a rate that conflicts with another
        // participant of the same pair, a registration after the session started, or
        // the board refusing to open.
        void Register(const std::string &owner, int channel, double pulse_hz);

        // True = the session is running (pulses on); false = still waiting for another
        // participant. Throws SensorSyncError when the START fails or the session has
        // already ended (a participant already tore down: nothing must restart pulses).
        bool Arm(const std::string &owner);

        // Ends the session on the first call (STOP -> board idle, log flushed and
        // closed); later calls are no-ops. Never throws. Also called by main at exit.
        void Disarm(const std::string &owner);

        bool Registered(const std::string &owner) const;

        bool HasParticipants() const;

        bool Started() const; // START succeeded (the session may have ended since)

        bool Ended() const;

        // False once anything failed: open, START, a protocol/loss report from the
        // board, the log write, or STOP. Every participant treats it as fatal
        bool Ok() const;

        // Seconds since the timing log last grew; -1 outside a running session
        double StalledSeconds() const;

        // Seconds this participant has been armed while the session is still not
        // started; 0 once started, when not armed, or when unknown
        double WaitingSeconds(const std::string &owner) const;

        const std::filesystem::path &LogPath() const { return log_path_; }

        const std::string &Port() const { return port_; }

        std::string LastError() const;

        // One line for the rig log: the port, each pair's rate and members, and
        // whether the pulses are running, waiting for an arm, or ended
        std::string Describe() const;

        // What START sends: one rate per group in channel order (0 = disabled). A group
        // without a participant is disabled explicitly rather than left at the board's
        // previous rate
        std::vector<std::pair<int, double> > PlannedRates() const;

        // For the log and the run manifest
        nlohmann::ordered_json Summary() const;

    private:
        std::vector<std::pair<int, double> > PlannedRatesLocked() const;

        void FailLocked(const std::string &what);

        std::filesystem::path log_path_;
        std::unique_ptr<SensorSyncBackend> backend_;

        mutable std::mutex mu_;
        std::string port_;
        bool opened_ = false;
        bool started_ = false;
        bool ended_ = false;
        bool failed_ = false;
        std::string last_error_;
        std::vector<Participant> participants_;
        std::vector<std::pair<int, double> > started_rates_;
        std::chrono::steady_clock::time_point armed_at_[4]{}; // by participant index (max 4 channels)
    };
} // namespace common
