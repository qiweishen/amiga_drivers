#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "device_report.h"
#include "scan_record.h"


namespace lms4xxx {
    class ScanH5File {
    public:
        struct Options {
            std::string path; ///< file to create; an existing file is an ERROR, never overwritten
            std::string instance; ///< LiDAR id, stored as the instance_id attribute
            std::string session_timestamp; ///< session folder stamp (YYYYMMDD_HHMMSS)
            std::string config_yaml; ///< driver config text, embedded verbatim when non-empty
            std::string remission; ///< "rssi" | "refl"

            // Device provenance (root attributes)
            std::string device_firmware;
            std::string device_order_number;
            std::string device_type;
            std::string device_name;
            bool device_keeps_flagged_points = false;

            std::uint16_t channel_mask = ChannelMask::kDist1;
            std::uint32_t split_index = 0; ///< sequence number of this file within the run
            std::uint32_t chunk_frames = 64; ///< HDF5 chunk = this many scans on /channels/*
            int compression_level = 0; ///< 0 = none; 1..9 = gzip (shuffle + deflate) on /channels/*
            bool swmr = false; ///< true is rejected: the v3 variable-length strings are not SWMR-safe

            // Root attributes
            double start_angle_deg = 0.0;
            double stop_angle_deg = 0.0;
            double angle_step_deg = 0.0;
            std::uint16_t output_rate = 1;

            // What the device reported about itself at bring-up; every field is
            // optional and an absent one is simply not written as an attribute.
            DeviceAudit audit;
        };

        ScanH5File();

        ~ScanH5File();

        ScanH5File(const ScanH5File &) = delete;

        ScanH5File &operator=(const ScanH5File &) = delete;

        ScanH5File(ScanH5File &&) = delete;

        ScanH5File &operator=(ScanH5File &&) = delete;

        // Creates the file with the full layout; closed again on failure
        bool Open(const Options &options);

        // On failure the datasets may be unevenly extended: stop using the file
        bool Append(const ScanRecord *records, std::size_t count);

        // Appends one device telemetry row to /telemetry (extensible datasets,
        // written one row at a time — this happens every few seconds, not at
        // 600 Hz). No-op when the file is not open.
        bool AppendTelemetry(const TelemetrySample &sample);

        // H5Fflush + fdatasync of the underlying file. This is a durability
        // barrier, not a transaction or a guarantee of recovery after a crash:
        // dataset extents can diverge on a failed append, and an unclean HDF5
        // file may require recovery even after earlier successful flushes.
        bool Flush();

        // Writes the completeness marker (closed_cleanly / frames_total) and
        // closes. Returns false when HDF5 could not finish the file — H5Fclose
        // performs the final metadata flush, so a discarded return code here
        // reported a truncated file as a clean stop. Idempotent.
        bool Close();

        [[nodiscard]] bool IsOpen() const;

        [[nodiscard]] std::uint64_t FramesWritten() const;

        // False when gzip was requested but libhdf5 lacks the deflate filter
        [[nodiscard]] bool CompressionActive() const;

        [[nodiscard]] std::string LastError() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
} // namespace lms4xxx
