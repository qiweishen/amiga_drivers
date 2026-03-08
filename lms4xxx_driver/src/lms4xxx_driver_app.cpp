#include "lms4xxx_driver_app.h"

#include <chrono>
#include <filesystem>
#include <thread>

#include "lms4xxx_tool.h"
#include "utility.h"


namespace {
	constexpr std::string_view kModule = "LMS4xxxApp";

	// Statistics logging interval during scanning
	constexpr auto kStatsInterval = std::chrono::seconds(30);

	// Main loop polling interval
	constexpr auto kPollInterval = std::chrono::milliseconds(200);

	// Build channel mask from scan config.
	std::uint16_t BuildChannelMask(const LMS4xxx::ScanConfig &scan) {
		std::uint16_t mask = 0;
		if (scan.enable_distance) {
			mask |= LMS4xxx::ChannelMask::kDist1;
		}
		if (scan.enable_rssi) {
			mask |= LMS4xxx::ChannelMask::kRssi1;
		}
		if (scan.enable_reflectance) {
			mask |= LMS4xxx::ChannelMask::kRefl1;
		}
		if (scan.enable_angle_correction) {
			mask |= LMS4xxx::ChannelMask::kAngl1;
		}
		if (scan.enable_quality) {
			mask |= LMS4xxx::ChannelMask::kQlty1;
		}
		return mask;
	}
}  // namespace


LidarDriverApp::LidarDriverApp(LiDARConfig config) : impl_(std::make_unique<Impl>()), config_(std::move(config)) {}


LidarDriverApp::~LidarDriverApp() {
	if (impl_->driver) {
		shutdown();
	}
}


bool LidarDriverApp::init() {
	// Determine instance name for logging
	impl_->instance_name = config_.position_name.empty() ? config_.hostname : config_.position_name;

	Common::Log::log_message(spdlog::level::trace, kModule,
							 fmt::format("Initializing LiDAR instance '{}' ({}:{})", impl_->instance_name,
										 config_.driver_config.device.ip, config_.driver_config.device.port));

	// Apply hostname override to driver config
	if (!config_.hostname.empty()) {
		config_.driver_config.device.ip = config_.hostname;
	}

	// Map LiDARConfig NTP fields to DriverConfig.ntp
	config_.driver_config.ntp.enable = config_.enable_ntp;
	if (!config_.ntp_server_ip.empty()) {
		config_.driver_config.ntp.server_ip = config_.ntp_server_ip;
	}
	if (config_.sync_time > 0.0) {
		config_.driver_config.ntp.update_interval_s = static_cast<std::uint32_t>(config_.sync_time);
	}

	// Validate config
	auto ec = config_.driver_config.Validate();
	if (ec) {
		Common::Log::log_and_throw(kModule, fmt::format("Invalid configuration for {}", impl_->instance_name), ec.message(), true);
		return false;
	}

	// Create driver
	impl_->driver = std::make_unique<LMS4xxx::LMS4xxxDriver>(config_.driver_config);

	// Register callbacks.
	impl_->driver->SetConnectionCallback([name = impl_->instance_name](LMS4xxx::ConnectionState state) {
		Common::Log::log_message(spdlog::level::trace, kModule, fmt::format("[{}] Connection: {}", name, LMS4xxx::ToString(state)));
	});

	impl_->driver->SetErrorCallback([name = impl_->instance_name](std::error_code err, const std::string &detail) {
		Common::Log::log_and_throw(
				kModule, fmt::format("[{}] Error: {}{}", name, err.message(), detail.empty() ? "" : " (" + detail + ")"), "", false);
	});

	// Create scan record writer
	if (!config_.data_folder_path.empty()) {
		LMS4xxx::ScanRecordWriter::Config writer_config;
		writer_config.bin_path = fmt::format("{}/bin/scan_{}_{}.bin",
											 config_.data_folder_path, impl_->instance_name, config_.timestamp);
		writer_config.channel_mask = BuildChannelMask(config_.driver_config.scan);
		writer_config.queue_capacity = config_.recording_queue_capacity;
		writer_config.write_buffer_size = config_.recording_write_buffer_size;
		writer_config.max_file_bytes = config_.recording_max_file_bytes;

		impl_->writer = std::make_unique<LMS4xxx::ScanRecordWriter>(writer_config);
		// Register scan callback to feed the writer.
		impl_->driver->SetScanCallback([writer = impl_->writer.get()](const LMS4xxx::ScanData &scan) {
			writer->OnScan(scan);
		});

		Common::Log::log_message(spdlog::level::trace, kModule,
								 fmt::format("[{}] Scan recording enabled (channels: 0x{:02X})",
											 impl_->instance_name, writer_config.channel_mask));
	} else {
		Common::Log::log_and_throw(kModule, fmt::format("Failed to set up writing module on [{}]", impl_->instance_name), "", false);
		return false;
	}

	// Connect
	ec = impl_->driver->Connect();
	if (ec) {
		Common::Log::log_and_throw(kModule, fmt::format("Failed to connect [{}]", impl_->instance_name), ec.message(), false);
		return false;
	}

	// Configure
	ec = impl_->driver->Configure();
	if (ec) {
		Common::Log::log_and_throw(kModule, fmt::format("Failed to configure [{}]", impl_->instance_name), ec.message(), false);
		impl_->driver->Disconnect();
		return false;
	}

	Common::Log::log_message(spdlog::level::info, kModule, fmt::format("LiDAR instance [{}] initialized successfully", impl_->instance_name));
	return true;
}


void LidarDriverApp::run() {
	if (!impl_->driver) {
		Common::Log::log_and_throw(kModule, "run() called without init()", "", true);
		return;
	}

	// Start scan record writer before scanning so no frames are missed.
	if (impl_->writer) {
		if (!impl_->writer->Start(config_.driver_config.scan)) {
			Common::Log::log_message(spdlog::level::warn, kModule,
									 fmt::format("[{}] Failed to start scan writer, continuing without recording",
												 impl_->instance_name));
			impl_->writer.reset();
		}
	}

	// Start scanning.
	auto ec = impl_->driver->StartScanning();
	if (ec) {
		Common::Log::log_and_throw(kModule, fmt::format("Failed to start scanning '{}'", impl_->instance_name), ec.message(), true);
		if (impl_->writer) {
			impl_->writer->Stop();
		}
		terminate_.store(true, std::memory_order_release);
		return;
	}

	Common::Log::log_message(spdlog::level::info, kModule,
							 fmt::format("LiDAR instance [{}] start scanning", impl_->instance_name));

	// Main loop: wait for termination, periodically log statistics
	auto last_stats = std::chrono::steady_clock::now();

	while (!terminate_.load(std::memory_order_acquire)) {
		std::this_thread::sleep_for(kPollInterval);

		// Check if driver is still healthy
		if (!impl_->driver->IsScanning()) {
			Common::Log::log_message(spdlog::level::warn, kModule,
									 fmt::format("LiDAR instance [{}] stopped scanning unexpectedly", impl_->instance_name));
			terminate_.store(true, std::memory_order_release);
			break;
		}

		// Periodic statistics logging
		auto now = std::chrono::steady_clock::now();
		if (now - last_stats >= kStatsInterval) {
			impl_->driver->LogStatistics();
			last_stats = now;
		}
	}

	Common::Log::log_message(spdlog::level::trace, kModule, fmt::format("LiDAR instance [{}] Run() exiting", impl_->instance_name));
}


void LidarDriverApp::shutdown() {
	if (!impl_->driver) {
		return;
	}

	// Stop scanning if active
	if (impl_->driver->IsScanning()) {
		impl_->driver->StopScanning();
	}

	// Log final driver statistics
	impl_->driver->LogStatistics();

	// Stop recording: flush remaining frames and close binary file
	if (impl_->writer) {
		impl_->writer->Stop();
		impl_->writer->LogStatistics();
	}

	// Disconnect.
	impl_->driver->Disconnect();

	Common::Log::log_message(spdlog::level::info, kModule, fmt::format("LiDAR instance [{}] driver shutdown completely", impl_->instance_name));

	// Reset driver so destructor and repeated shutdown() calls are no-ops.
	impl_->driver.reset();
}
