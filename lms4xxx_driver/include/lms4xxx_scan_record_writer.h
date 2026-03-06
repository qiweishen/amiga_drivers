#ifndef LMS4XXX_SCAN_RECORD_WRITER_H
#define LMS4XXX_SCAN_RECORD_WRITER_H

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include "lms4xxx_config.h"
#include "lms4xxx_scan_data.h"


namespace LMS4xxx {

	// Channel mask bits for binary recording.
	namespace ChannelMask {
		inline constexpr std::uint16_t kDist1 = 0x01;
		inline constexpr std::uint16_t kRssi1 = 0x02;
		inline constexpr std::uint16_t kRefl1 = 0x04;
		inline constexpr std::uint16_t kAngl1 = 0x08;
		inline constexpr std::uint16_t kQlty1 = 0x10;
	}  // namespace ChannelMask


	// Binary file header (64 bytes, written once at file start).
#pragma pack(push, 1)
	struct ScanFileHeader {
		std::uint32_t magic = 0x4C4D5334;	///< "LMS4"
		std::uint16_t version = 2;
		std::uint16_t channel_mask = 0;
		std::uint16_t max_points = kMaxPointsPerScan;
		std::int32_t start_angle = 0;	 ///< 1/10000 deg (from config)
		std::int32_t stop_angle = 0;	 ///< 1/10000 deg (from config)
		std::uint16_t angle_step = 0;	 ///< 1/10000 deg (from config)
		std::uint32_t scan_frequency = 0;  ///< 1/100 Hz
		std::uint32_t record_size = 0;	   ///< bytes per record (excluding this header)
		std::uint8_t reserved[36] = {};
	};
#pragma pack(pop)

	static_assert(sizeof(ScanFileHeader) == 64, "ScanFileHeader must be 64 bytes");


	// Per-frame record header for v1 format (16 bytes).
#pragma pack(push, 1)
	struct ScanRecordHeaderV1 {
		std::uint32_t time_since_startup_us = 0;
		std::uint16_t telegram_counter = 0;
		std::uint16_t scan_counter = 0;
		std::uint16_t num_points = 0;
		std::int32_t start_angle = 0;
		std::uint16_t angle_step = 0;
	};
#pragma pack(pop)

	static_assert(sizeof(ScanRecordHeaderV1) == 16, "ScanRecordHeaderV1 must be 16 bytes");


	// Per-frame record header for v2 format (96 bytes).
	// Contains all ScanData metadata fields.
#pragma pack(push, 1)
	struct ScanRecordHeader {
		// --- Core (same layout as v1 first 16 bytes) ---
		std::uint32_t time_since_startup_us = 0;
		std::uint16_t telegram_counter = 0;
		std::uint16_t scan_counter = 0;
		std::uint16_t num_points = 0;
		std::int32_t start_angle = 0;	///< 1/10000 deg (actual for this frame)
		std::uint16_t angle_step = 0;	///< 1/10000 deg (actual for this frame)
		// = 16 bytes

		// --- Timing ---
		std::uint32_t transmission_time_us = 0;
		std::uint32_t scan_frequency = 0;		  ///< 1/100 Hz
		std::uint32_t measurement_frequency = 0;  ///< Inverse of time between 2 measurement shots (100 Hz units)
		// = 28

		// --- Device info ---
		std::uint16_t device_version = 0;
		std::uint16_t device_number = 0;
		std::uint32_t serial_number = 0;
		std::uint8_t device_status_1 = 0;
		std::uint8_t device_status_2 = 0;
		// = 38

		// --- Digital I/O ---
		std::uint8_t digital_input_1 = 0;
		std::uint8_t digital_input_2 = 0;
		std::uint8_t digital_output_1 = 0;
		std::uint8_t digital_output_2 = 0;
		// = 42

		// --- Encoder ---
		std::uint8_t has_encoder = 0;
		std::uint8_t pad1 = 0;
		std::uint32_t encoder_position = 0;
		std::uint16_t encoder_speed = 0;
		// = 50

		// --- Timestamp ---
		std::uint8_t has_timestamp = 0;
		std::uint8_t pad2 = 0;
		std::uint16_t ts_year = 0;
		std::uint8_t ts_month = 0;
		std::uint8_t ts_day = 0;
		std::uint8_t ts_hour = 0;
		std::uint8_t ts_minute = 0;
		std::uint8_t ts_second = 0;
		std::uint8_t pad3 = 0;
		std::uint32_t ts_microsecond = 0;
		// = 64

		// --- Device name ---
		std::uint8_t has_device_name = 0;
		char device_name[16] = {};
		std::uint8_t pad4 = 0;
		// = 82

		// --- Position ---
		float y_rotation = 0.0f;
		// = 86

		std::uint8_t reserved[10] = {};
		// = 96
	};
#pragma pack(pop)

	static_assert(sizeof(ScanRecordHeader) == 96, "ScanRecordHeader must be 96 bytes");


	// Maximum serialized record size (all channels enabled, v2 header):
	// header(96) + dist(841*2) + rssi(841*2) + refl(841*2) + angl(841*2) + qlty(841*1)
	inline constexpr std::size_t kMaxRecordBytes = sizeof(ScanRecordHeader) + kMaxPointsPerScan * 9;


	// Asynchronous binary scan data writer.
	//
	// Architecture: parse thread -> OnScan() -> SPSC queue -> write thread -> ofstream
	//
	// The parse thread only does memcpy + queue push (sub-microsecond).
	// All file I/O is isolated in the write thread, ensuring the parse thread
	// is never blocked by disk operations.
	//
	// Usage:
	//   1. Create with output path and channel configuration
	//   2. Call Start() to open file and launch write thread
	//   3. Register OnScan() as the driver's ScanDataCallback
	//   4. Call Stop() to flush remaining records and close file
	//   5. Use DataConverter tool for post-processing binary to CSV
	class ScanRecordWriter {
	public:
		struct Config {
			std::string bin_path;
			std::uint16_t channel_mask = ChannelMask::kDist1;
			std::size_t queue_capacity = 512;
			std::size_t write_buffer_size = 256 * 1024;
			std::size_t max_file_bytes = 1ULL * 1024 * 1024 * 1024;  // 1 GB
		};

		explicit ScanRecordWriter(Config config);
		~ScanRecordWriter();

		ScanRecordWriter(const ScanRecordWriter &) = delete;
		ScanRecordWriter &operator=(const ScanRecordWriter &) = delete;
		ScanRecordWriter(ScanRecordWriter &&) = delete;
		ScanRecordWriter &operator=(ScanRecordWriter &&) = delete;

		// Open binary file, write file header, start write thread.
		[[nodiscard]] bool Start(const ScanConfig &scan_config);

		// Signal write thread to stop, flush remaining records, close file.
		void Stop();

		// Serialize scan data and push to write queue. Called from the parse thread.
		void OnScan(const ScanData &scan);

		struct Statistics {
			std::size_t frames_written = 0;
			std::size_t frames_dropped = 0;
			std::size_t bytes_written = 0;
			std::size_t files_created = 0;
		};

		[[nodiscard]] Statistics GetStatistics() const;

		void LogStatistics() const;

	private:
		struct Impl;
		std::unique_ptr<Impl> impl_;
	};

}  // namespace LMS4xxx

#endif	// LMS4XXX_SCAN_RECORD_WRITER_H
