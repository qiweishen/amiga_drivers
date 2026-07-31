#pragma once

// PTP (IEEE 1588) slave-mode management per plan section 2: feature-name
// detection chain, enable, status polling until the required state, and the
// host-vs-device timestamp latch cross-check. The driver never runs a PTP
// stack itself — a grandmaster is assumed to exist on the camera's L2
// domain; this class only turns the camera's slave on and verifies it locks.

#include <PvGenParameterArray.h>
#include <cstdint>
#include <string>

#include "app_config.hpp"
#include "signal_stop.hpp"

namespace jai::ebus {
    // Internal PTP state shared across enable/wait/refresh (values surface in
    // the log lines, not in a persisted document).
    struct PtpStatusReport {
        bool feature_found = false; // a usable enable/status feature pair exists
        bool enabled = false; // enable feature was written successfully
        std::string feature_set; // resolved chain: gev_ieee1588 | sfnc_ptp | fuzzy | explicit
        std::string enable_feature;
        std::string status_feature;
        std::string latch_command; // PTP dataset latch (SFNC chain); empty otherwise
        std::string status; // last PTP status string read
        std::string servo_status;
        bool offset_valid = false;
        int64_t offset_from_master_ns = 0; // PtpOffsetFromMaster, when present
        uint64_t lock_wait_ms = 0; // time from wait start to sync
        int64_t tick_frequency = 0; // GevTimestampTickFrequency (0 = not readable)
        bool tai_offset_detected = false; // TAI-UTC ~37 s removed in the cross-check
    };

    class PtpManager {
    public:
        // `params` is the connected device's parameter array; it must outlive
        // this manager (CameraSession guarantees that).
        PtpManager(std::string camera_id, PvGenParameterArray *params, PtpConfig cfg);

        // Detects the feature pair (three-level chain when feature_set=="auto")
        // and writes the enable feature. Also reads GevTimestampTickFrequency
        // and warns when it is not 1 GHz. Returns false only when PTP is
        // required (cfg.enabled, on_timeout=="abort") and no usable feature was
        // found or the enable write failed — the caller then exits with code 4.
        bool enable();

        // Polls the status feature every poll_interval_ms within the
        // sync_timeout_s budget until it case-insensitively equals
        // required_status (and PtpServoStatus, when present, equals "Locked").
        // A "Master" reading is a hard failure (no grandmaster on the network).
        // Returns false when the sync requirement was not met and the policy is
        // "abort" (or the process is stopping); with "warn_continue" it returns
        // true after logging that the capture continues unsynchronized.
        bool wait_for_sync(StopController *stop);

        // Re-reads PtpOffsetFromMaster and repeats the latch cross-check;
        // called at session start and every offset_report_interval_s.
        void refresh_offset();

    private:
        bool detect_features();

        bool read_status(std::string &out);

        void read_dataset_extras(); // PtpOffsetFromMaster (optional feature)
        void cross_check_timestamp();

        std::string camera_id_;
        PvGenParameterArray *params_;
        PtpConfig cfg_;
        PtpStatusReport report_;
        bool have_first_latch_offset_ = false;
        int64_t first_latch_offset_ns_ = 0;
    };
} // namespace jai::ebus
