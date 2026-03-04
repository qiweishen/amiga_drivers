#ifndef LIDAR_DATA_TYPE_H
#define LIDAR_DATA_TYPE_H

#include <map>
#include <string>
#include <vector>


struct LiDARConfig {
    // SICK scanner configuration
    std::string launch_file;
	std::string lidar_position;
    std::string lidar_ip;
    std::map<std::string, std::string> launch_overrides;  // YAML key-value overrides for SICK CLI
    // NTP time synchronization: if non-empty, configures the scanner to use
    // this IP as NTP server. The SICK library sends SOPAS commands:
    //   sWN TSCTCInterface 0       (Ethernet)
    //   sWN TSCTCSrvAddr <ip>      (NTP server IP)
    //   sWN TSCTCupdatetime 5      (sync every 5 seconds)
    //   sWN TSCRole 1              (activate NTP client)
    std::string ntp_server_ip;
    bool quiet = true; // Suppress SICK console output

    // Output paths (set by the standalone or unified main)
    std::string output_file; // Binary output file path
    std::string data_folder_path;
    std::string timestamp;
    std::string log_file; // Log file path (standalone mode only)
    bool convert_to_csv = false; // Convert binary to CSV after recording

    // Queue and writer tuning
    size_t max_queue_size = 256;
    size_t write_buffer_size = 8 * 1024 * 1024; // 8 MB
    size_t status_interval_frames = 100;

    // SICK library loading
    std::vector<std::string> library_search_paths = {
        "3rd_party/FetchContent/sick_scan_xd/build/",
        "3rd_party/FetchContent/sick_scan_xd/Release/"
    };
    std::string library_name = "libsick_scan_xd_shared_lib.so";
};


#endif
