#include "lms4xxx_tool.h"

#include "string_util.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>

#include "logger.h"
#include "utility.h"


namespace {
	constexpr std::string_view kModule = "LMS4xxxTool";
	Common::DriverLog g_log{ std::string(kModule) };
}  // namespace


namespace LMS4xxxTool {

	std::vector<LiDARConfig> LoadConfigs(std::string_view config_path) {
		std::vector<LiDARConfig> configs;

		Common::ConfigLoader loader(config_path);
		const auto &root = loader.root();

		// Shared scan config
		LMS4xxx::ScanConfig shared_scan;
		const auto &scan = root["Scan Config"];
		if (scan) {
			shared_scan.start_angle_deg = scan["Start Angle Deg"].as<double>(55.0);
			shared_scan.stop_angle_deg = scan["Stop Angle Deg"].as<double>(125.0);
			shared_scan.angular_resolution_deg = scan["Angular Resolution Deg"].as<double>(0.0833);
			shared_scan.enable_distance = scan["Enable Distance"].as<bool>(true);
			shared_scan.enable_rssi = scan["Enable RSSI"].as<bool>(false);
			shared_scan.enable_reflectance = scan["Enable Reflectance"].as<bool>(false);
			shared_scan.enable_angle_correction = scan["Enable Angle Correction"].as<bool>(false);
			shared_scan.enable_quality = scan["Enable Quality"].as<bool>(false);
			shared_scan.output_rate = scan["Output Rate"].as<std::uint16_t>(1);
		}

		// Shared network config
		LMS4xxx::NetworkConfig shared_network;
		const auto &network = root["Network"];
		if (network) {
			shared_network.recv_buffer_bytes = network["Receive Buffer Bytes"].as<std::size_t>(4 * 1024 * 1024);
			shared_network.ring_buffer_frames = network["Ring Buffer Frames"].as<std::size_t>(1024);
			shared_network.receive_thread_priority = network["Receive Thread Priority"].as<int>(99);
			shared_network.receive_thread_cpu = network["Receive Thread CPU"].as<int>(-1);
			shared_network.connect_timeout_ms = network["Connect Timeout Ms"].as<int>(5000);
			shared_network.response_timeout_ms = network["Response Timeout Ms"].as<int>(2000);
			shared_network.tcp_keepalive = network["TCP Keepalive"].as<bool>(true);
			shared_network.keepalive_idle_s = network["Keepalive Idle S"].as<int>(10);
			shared_network.keepalive_interval_s = network["Keepalive Interval S"].as<int>(5);
			shared_network.keepalive_count = network["Keepalive Count"].as<int>(3);
		}

		// Time sync
		bool shared_enable_ntp = false;
		std::string shared_ntp_server;
		double sync_time = 1.0;
		const auto &ntp = root["Time Synchronization"];
		if (ntp) {
			shared_enable_ntp = ntp["Enable NTP"].as<bool>(false);
			shared_ntp_server = ntp["NTP Server"].as<std::string>("");
			sync_time = ntp["Sync Time"].as<double>(1.0);
		}

		// Recording
		std::size_t shared_queue_capacity = 512;
		std::size_t shared_write_buffer_size = 256 * 1024;
		std::size_t shared_max_file_bytes = 1ULL * 1024 * 1024 * 1024;
		const auto &recording = root["Recording"];
		if (recording) {
			shared_queue_capacity = recording["Queue Capacity"].as<std::size_t>(512);
			shared_write_buffer_size = recording["Write Buffer Size"].as<std::size_t>(256 * 1024);
			shared_max_file_bytes = recording["Max File Bytes"].as<std::size_t>(1ULL * 1024 * 1024 * 1024);
		}

		// Per-instance configuration
		const auto &instances = root["Instances"];
		if (!instances || !instances.IsMap()) {
			g_log.warn("No 'Instances' section found in {}", config_path);
			return configs;
		}

		for (auto it = instances.begin(); it != instances.end(); ++it) {
			LiDARConfig cfg;

			// Instance name (map key) → position_name
			const auto name = it->first.as<std::string>();
			cfg.position_name = ToSnakeCase(name);
			cfg.hostname = it->second["Hostname"].as<std::string>("192.168.0.1");

			// Apply shared config
			cfg.driver_config.device.ip = cfg.hostname;
			cfg.driver_config.device.port = it->second["Port"].as<std::uint16_t>(2111);
			cfg.driver_config.scan = shared_scan;
			cfg.driver_config.network = shared_network;
			cfg.enable_ntp = shared_enable_ntp;
			cfg.ntp_server_ip = shared_ntp_server;
			cfg.sync_time = sync_time;
			cfg.recording_queue_capacity = shared_queue_capacity;
			cfg.recording_write_buffer_size = shared_write_buffer_size;
			cfg.recording_max_file_bytes = shared_max_file_bytes;

			g_log.info("Loaded LiDAR instance [{}] ({}:{})", cfg.position_name, cfg.hostname, cfg.driver_config.device.port);

			configs.push_back(std::move(cfg));
		}

		g_log.trace("Loaded {} LiDAR instance(s) from {}", configs.size(), config_path);
		return configs;
	}


	std::string ToSnakeCase(std::string_view name) {
		return Common::StringUtil::ToSnakeCase(name);
	}


}  // namespace LMS4xxxTool
