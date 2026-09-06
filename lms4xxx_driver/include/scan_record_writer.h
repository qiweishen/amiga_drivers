#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "app_config.h"
#include "device_report.h"
#include "scan_data.h"
#include "scan_record.h"


namespace lms4xxx {
    // Asynchronous HDF5 recorder: parse thread -> OnScan() -> SPSC queue ->
    // write thread -> ScanH5File. Batches of chunk_frames, H5Fflush every
    // flush_interval_ms, new split file every max_file_bytes of payload.
    class ScanRecordWriter {
    public:
        struct Config {
            // ".../scan_<id>_<ts>.h5" -> split files scan_<id>_<ts>_000.h5, _001.h5, ...
            std::string output_path;
            std::string instance; // log prefix and instance_id attribute
            std::string session_timestamp; // stored as an attribute
            std::string config_yaml_path; // embedded verbatim when readable
            std::string remission; // "rssi" | "refl" (config_remission attribute)

            // Device provenance (root attributes), from Lms4xxxDriver::GetDeviceIdentity()
            std::string device_firmware;
            std::string device_order_number;
            std::string device_type;
            std::string device_name;
            bool device_keeps_flagged_points = false;

            // Device self-report at bring-up; written as root attributes
            DeviceAudit audit;

            std::uint16_t channel_mask = ChannelMask::kDist1;
            std::size_t queue_capacity = 512;
            std::uint64_t max_file_bytes = 1ULL * 1024 * 1024 * 1024; // payload per split, 0 = never
            std::uint32_t chunk_frames = 64; // HDF5 chunk = write batch
            std::uint32_t flush_interval_ms = 1000;
            int compression_level = 0; // 0 = off, 1..9 = gzip on the channel datasets
            bool swmr = false; // single-writer/multiple-reader (local POSIX FS only)
        };

        explicit ScanRecordWriter(Config config);

        ~ScanRecordWriter();

        ScanRecordWriter(const ScanRecordWriter &) = delete;

        ScanRecordWriter &operator=(const ScanRecordWriter &) = delete;

        ScanRecordWriter(ScanRecordWriter &&) = delete;

        ScanRecordWriter &operator=(ScanRecordWriter &&) = delete;

        // Creates the first split file (a bad path fails here)
        bool Start();

        // Drains the queue, closes the file
        void Stop();

        // Parse thread; never blocks or throws
        void OnScan(const ScanData &scan);

        // Owner thread, every telemetry.interval_s. Writes one /telemetry row
        // directly (a few bytes every few seconds, not on the scan path), so it
        // does NOT go through the scan queue.
        void OnTelemetry(const TelemetrySample &sample);

        struct Statistics {
            std::size_t frames_written = 0; // on disk, advances per batch
            std::size_t frames_dropped = 0;
            std::size_t bytes_written = 0; // uncompressed payload
            std::size_t files_created = 0;
            std::size_t frames_queued = 0; // accepted into the queue, exact per scan (fps=)
        };

        Statistics GetStatistics() const;

        // Write thread stopped on an HDF5/IO failure; the owner ends the Run (like HasFault)
        bool HasFailed() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
} // namespace lms4xxx

