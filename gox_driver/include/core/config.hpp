#pragma once

// config.json schema (see docs/CONFIG.md and config/config.example.json).
// Parsing is strict: unknown keys anywhere in the file are a fatal error
// (with a "did you mean ...?" suggestion), so a typo can never silently
// disable a setting during a capture session.

#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "core/logger.hpp"

namespace jai {

	class ConfigError : public std::runtime_error {
	public:
		using std::runtime_error::runtime_error;
	};

	struct LoggingConfig {
		LogLevel level = LogLevel::Info;
	};

	struct StatsConfig {
		bool enabled = true;
		double interval_s = 5.0;
	};

	struct AcquisitionLimits {
		uint64_t max_frames = 0;	// 0 = unlimited
		double max_duration_s = 0;	// 0 = unlimited
	};

	struct PreflightConfig {
		bool fail_on_error = true;
	};

	struct PtpConfig {
		bool enabled = true;
		std::string feature_set = "auto";  // auto | gev_ieee1588 | sfnc_ptp | explicit
		std::string enable_feature;		   // used when feature_set == "explicit"
		std::string status_feature;		   // used when feature_set == "explicit"
		std::string required_status = "Slave";
		double sync_timeout_s = 60.0;
		uint32_t poll_interval_ms = 500;
		std::string on_timeout = "abort";		 // abort | warn_continue
		double offset_report_interval_s = 60.0;	 // 0 = only at session start/end
		bool expect_tai_offset = true;
	};

	struct RecordingConfig {
		std::string output_dir = "/workspace/dataset/captures";
		std::string session_name = "auto";				 // auto = UTC timestamp + uuid prefix
		double segment_size_gib = 2.0;
		uint32_t record_align = 4096;					 // power of two; 1 = no alignment
		std::string payload_crc = "none";				 // none | crc32c
		uint32_t queue_max_frames = 32;					 // queue capacity; pool allocates +2 chunks
		std::string queue_on_full = "drop_newest";		 // drop_newest | block
		std::string on_buffer_error = "record_flagged";	 // record_flagged | drop
		uint32_t flush_interval_mb = 64;				 // sync_file_range cadence
		double min_free_gib = 10.0;						 // orderly stop below this
		uint32_t debug_slowdown_us = 0;					 // test hook: writer sleeps per frame
	};

	struct SelectorConfig {
		std::string by;	 // mac | ip | serial | user_defined_name
		std::string value;
	};

	struct ForceIpConfig {
		bool enabled = false;
		std::string ip;
		std::string subnet_mask;
		std::string gateway = "0.0.0.0";
	};

	struct DiscoveryConfig {
		uint32_t timeout_ms = 4000;
		uint32_t retries = 3;
		uint32_t retry_interval_ms = 1000;
		ForceIpConfig force_ip;
	};

	struct RoiConfig {
		uint32_t width = 0;	  // 0 = leave device value untouched
		uint32_t height = 0;  // 0 = leave device value untouched
		uint32_t offset_x = 0;
		uint32_t offset_y = 0;
	};

	struct TriggerConfig {
		bool enabled = false;
		std::string selector = "FrameStart";
		std::string source = "Line1";
		std::string activation = "RisingEdge";
	};

	struct ConvenienceConfig {
		std::optional<double> exposure_us;
		std::optional<double> gain;
		std::optional<double> frame_rate;
		std::optional<std::string> pixel_format;
		std::optional<RoiConfig> roi;
		std::optional<TriggerConfig> trigger;
	};

	struct GenicamFeature {
		std::string feature;
		std::string value;			   // scalar rendered as text; typed by GenICam node type
		bool value_is_string = false;  // true when the JSON value was a string
		std::string on_error;		   // fail | warn | skip; empty = apply.on_error_default
	};

	struct ApplyConfig {
		bool verify_readback = true;
		double float_verify_tolerance_rel = 1e-3;
		std::string on_error_default = "fail";	// fail | warn | skip
	};

	struct StreamConfig {
		uint32_t channel = 0;
		uint32_t buffer_count = 0;					  // 0 = auto (frame_rate x 0.5s, clamped [16, 256])
		uint32_t packet_size = 0;					  // 0 = NegotiatePacketSize, fallback 1476
		uint32_t socket_rx_buffer_mib = 16;
		std::string local_ip;						  // bind stream to a specific NIC; empty = auto
		uint32_t gev_scpd_ticks = 0;				  // inter-packet delay (bandwidth partitioning)
		std::vector<GenicamFeature> receiver_tuning;  // escape hatch on PvStream params
	};

	struct CameraConfig {
		std::string id;
		bool enabled = true;
		SelectorConfig selector;
		DiscoveryConfig discovery;
		ConvenienceConfig convenience;
		std::vector<GenicamFeature> genicam_features;
		ApplyConfig apply;
		StreamConfig stream;
		PtpConfig ptp;				// global ptp deep-merged with per-camera override
		RecordingConfig recording;	// global recording deep-merged with override
	};

	struct AppConfig {
		int version = 1;
		LoggingConfig logging;
		StatsConfig stats;
		AcquisitionLimits acquisition;
		PreflightConfig preflight;
		PtpConfig ptp;
		RecordingConfig recording;
		std::vector<CameraConfig> cameras;
		nlohmann::ordered_json raw;	 // verbatim config for session.json embedding
	};

	// Parses and validates a config file. Comments (// and /* */) are allowed.
	// Throws ConfigError with a JSON-path-qualified message on any problem.
	AppConfig load_config(const std::string &path);

	// Same, from an already-parsed JSON document (unit tests).
	AppConfig load_config_json(const nlohmann::ordered_json &doc);

}  // namespace jai
