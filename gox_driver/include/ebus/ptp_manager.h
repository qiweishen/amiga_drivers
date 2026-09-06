#pragma once

// PTP slave management: GevIEEE1588 / GevIEEE1588Status (manual p.128), 1 GHz ticks (p.121).
// A grandmaster is assumed on the camera's L2 domain; this turns the slave on, waits for the
// lock and keeps checking it stays locked and accurate.

#include <PvGenParameterArray.h>
#include <cstdint>
#include <string>

#include "app_config.h"
#include "device_json.h"
#include "ptp_status.h"
#include "signal_stop.h"

namespace gox::ebus {
    class PtpManager {
    public:
        // `params` must outlive the manager
        PtpManager(std::string camera_id, PvGenParameterArray *params, PtpConfig cfg);

        // GevIEEE1588 = true. False only under on_timeout=abort when the feature is missing or refused
        bool Enable();

        // Polls GevIEEE1588Status until "slave" (sync_timeout_s); "master"/"faulty" fail immediately.
        // False when the requirement was not met under on_timeout=abort, or the process is stopping
        bool WaitForSync(StopController *stop);

        // Re-reads the clock and repeats the host/device timestamp cross-check
        void RefreshOffset();

        // Periodic: status must stay "slave" and accuracy within 0..9; a bad reading is re-checked a
        // few times first. False = synchronization lost (caller stops); true when PTP is off
        bool CheckHealth(StopController *stop);

        // PTP state for device.json / telemetry.jsonl (no device access)
        PtpSummary Summary() const;

    private:
        bool ReadStatus(std::string &out);

        bool ReadClockAccuracy(int64_t &out);

        void CrossCheckTimestamp();

        std::string camera_id_;
        PvGenParameterArray *params_;
        PtpConfig cfg_;

        bool feature_found_ = false; // GevIEEE1588 + GevIEEE1588Status both present
        bool enabled_ = false; // GevIEEE1588 was written successfully
        bool synchronized_ = false; // WaitForSync() reached "slave"
        bool accuracy_readable_ = true; // GevIEEE1588ClockAccuracy could be read
        std::string last_status_;
        int64_t last_accuracy_ = -1; // -1 = never read
        uint64_t lock_wait_ms_ = 0;

        bool have_first_latch_offset_ = false;
        int64_t first_latch_offset_ns_ = 0;

        // Last CrossCheckTimestamp() result, mirrored into the sidecars.
        bool have_offset_ = false;
        int64_t raw_offset_ns_ = 0;
        int64_t adjusted_offset_ns_ = 0;
        int64_t drift_ns_ = 0;
        bool tai_detected_ = false;
    };
} // namespace gox::ebus
