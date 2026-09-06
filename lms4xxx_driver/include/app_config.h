#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "config_util.h"


namespace lms4xxx {
    using ConfigError = common::ConfigError;

    // CoLa B over TCP (manual p.66)
    struct DeviceConfig {
        std::string ip;
        std::uint16_t port = 2111;
    };

    // LMDscandatacfg "Unit" byte (manual p.85): the device streams RSSI or REFL, never both
    enum class Remission : std::uint8_t {
        kRssi = 0, // signal strength in digits
        kRefl = 1, // calibrated reflectance in percent
    };

    inline const char *RemissionName(Remission r) { return r == Remission::kRefl ? "refl" : "rssi"; }

    inline const char *RemissionChannel(Remission r) { return r == Remission::kRefl ? "REFL1" : "RSSI1"; }

    // The only user-selectable measurement setting; everything else is ScanFixed
    struct ScanConfig {
        Remission remission = Remission::kRssi;
    };

    // Fixed measurement setup, written to the device on every start (docs/DEVICE_CONFIG.md)
    namespace ScanFixed {
        inline constexpr double kStartAngleDeg = 55.0;
        inline constexpr double kStopAngleDeg = 125.0;
        inline constexpr double kAngularResolutionDeg = 1.0 / 12.0;
        inline constexpr std::int32_t kStartAngle1e4 = 550000; // LMPoutputRange (manual p.86)
        inline constexpr std::int32_t kStopAngle1e4 = 1250000;
        inline constexpr std::uint32_t kAngularResolution1e4 = 833;
        inline constexpr std::uint16_t kPointsPerScan = 841;
        inline constexpr std::uint16_t kOutputRate = 1; // every scan
        inline constexpr std::uint32_t kScanFrequencyCentiHz = 60000;
        inline constexpr std::uint8_t kTimezoneUtc = 34; // TSCTCtimezone COORD_WORLD_TIME (manual p.100)
        inline constexpr std::uint32_t kCubicMaxDist1e4mm = 91776; // LFPcubicareafilter full range (manual p.110)
        inline constexpr std::int32_t kCubicExpansion1e4mm = 45000;
        // Status byte of LFPfrontendEdgefilter / LFPcubicareafilter: 01 = enable, 00 = disable,
        // confirmed by a SOPAS capture against the manual's inverted tables (docs/sopas_filter_polarity.md)
        inline constexpr std::uint8_t kFrontendEdgeFilterOff = 0x00;
        inline constexpr std::uint8_t kCubicAreaFilterOff = 0x00;
    } // namespace ScanFixed

    // TSCRole
    enum class TscRole : std::uint8_t {
        kOff = 0,
        kClient = 1,
        kServer = 2,
    };

    struct NtpConfig {
        bool enabled = false;
        std::string server; // IPv4; required when enabled
        std::uint32_t sync_interval_s = 1; // TSCTCupdatetime 1..3600
        std::uint32_t check_status_s = 5; // scanning-time server probe period
    };

    struct NetworkConfig {
        std::size_t recv_buffer_bytes = 4 * 1024 * 1024; // SO_RCVBUF
        std::size_t ring_buffer_frames = 1024;
        int receive_thread_priority = 99; // SCHED_FIFO 1..99
        int receive_thread_cpu = -1; // -1 = no pinning
        int connect_timeout_ms = 500;
        int response_timeout_ms = 200; // scanning-time variable access
        int config_timeout_ms = 2000; // per SOPAS round trip during Configure()
        bool tcp_keepalive = true;
        int keepalive_idle_s = 10;
        int keepalive_interval_s = 5;
        int keepalive_count = 3;
    };

    // What Lms4xxxDriver consumes for one device
    struct DriverConfig {
        std::string name; // log tag "[<name>] ..."
        DeviceConfig device;
        ScanConfig scan;
        NtpConfig ntp;
        NetworkConfig network;
    };

    // HDF5 recorder (docs/FORMAT_H5.md)
    struct OutputConfig {
        std::size_t queue_max_frames = 512; // parse thread -> write thread
        std::size_t max_file_bytes = 1ULL * 1024 * 1024 * 1024; // payload bytes per split (0 = no split)
        std::uint32_t chunk_frames = 64; // HDF5 chunk = write batch
        std::uint32_t flush_interval_ms = 1000;
        int compression_level = 0; // 0 = off, 1..9 = gzip
        bool swmr = false;
    };

    struct LidarConfig {
        std::string id; // log tag and file-name component
        bool enabled = true;
        DeviceConfig device;
    };

    struct AppConfig {
        std::vector<LidarConfig> lidars;
        ScanConfig scan;
        NtpConfig ntp;
        NetworkConfig network;
        OutputConfig output;
        double telemetry_interval_s = 10.0; // 0 = off
        double stats_interval_s = 2.5; // 0 = off

        // Enabled entries only
        std::vector<const LidarConfig *> EnabledLidars() const;

        // Per-device view handed to Lms4xxxDriver
        DriverConfig DriverConfigFor(const LidarConfig &lidar) const;
    };

    // Strict schema: unknown keys are errors, omitted keys keep the defaults. Throws ConfigError.
    AppConfig LoadAppConfig(std::string_view path);

    AppConfig LoadAppConfigText(const std::string &yaml_text);
} // namespace lms4xxx
