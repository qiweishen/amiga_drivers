#pragma once

// PTP (IEEE 1588) slave-mode management per plan section 2: feature-name
// detection chain, enable, status polling until the required state, and the
// host-vs-device timestamp latch cross-check. The driver never runs a PTP
// stack itself — a grandmaster is assumed to exist on the camera's L2
// domain; this class only turns the camera's slave on and verifies it locks.

#include <PvGenParameterArray.h>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>

#include "core/config.hpp"
#include "core/session_log.hpp"
#include "core/signal_stop.hpp"

namespace jai::ebus {

	// Everything session.json needs to reproduce/trust the timestamps.
	struct PtpStatusReport {
		bool enabled_in_config = false;
		bool feature_found = false;	 // a usable enable/status feature pair exists
		bool enabled = false;		 // enable feature was written successfully
		bool synced = false;		 // reached required_status (+ servo Locked when present)
		std::string feature_set;	 // resolved chain: gev_ieee1588 | sfnc_ptp | fuzzy | explicit
		std::string enable_feature;
		std::string status_feature;
		std::string latch_command;	 // PTP dataset latch (SFNC chain); empty otherwise
		std::string servo_feature;	 // "PtpServoStatus" when the camera has it
		std::string status;			 // last PTP status string read
		std::string servo_status;
		std::string grandmaster_id;
		std::string parent_id;
		bool offset_valid = false;
		int64_t offset_from_master_ns = 0;	// PtpOffsetFromMaster, when present
		uint64_t lock_wait_ms = 0;			// time from wait start to sync
		int64_t tick_frequency = 0;			// GevTimestampTickFrequency (0 = not readable)
		// TimestampLatch cross-check against host CLOCK_REALTIME:
		bool latch_offset_valid = false;
		int64_t latch_offset_ns = 0;	   // host_midpoint - device_ns, TAI-UTC delta removed
		bool tai_offset_detected = false;
		int64_t tai_offset_applied_s = 0;  // seconds subtracted from the raw offset

		nlohmann::ordered_json to_json() const;
	};

	class PtpManager {
	public:
		// `params` is the connected device's parameter array; it must outlive
		// this manager (CameraSession guarantees that).
		PtpManager(std::string camera_id, PvGenParameterArray *params, PtpConfig cfg, EventLog *events);

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
		// true with report().synced == false.
		bool wait_for_sync(StopController *stop);

		// Re-reads PtpOffsetFromMaster and repeats the latch cross-check;
		// called at session start and every offset_report_interval_s.
		void refresh_offset();

		const PtpStatusReport &report() const { return report_; }

	private:
		bool detect_features();
		bool read_status(std::string &out);
		void read_dataset_extras();	 // offset / grandmaster / parent (all optional)
		void cross_check_timestamp();

		std::string camera_id_;
		PvGenParameterArray *params_;
		PtpConfig cfg_;
		EventLog *events_;
		PtpStatusReport report_;
		bool have_first_latch_offset_ = false;
		int64_t first_latch_offset_ns_ = 0;
	};

}  // namespace jai::ebus
