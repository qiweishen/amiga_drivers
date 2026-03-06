#include "lms4xxx_csv_converter.h"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <spdlog/spdlog.h>
#include <vector>

#include "lms4xxx_scan_record_writer.h"


namespace {
	constexpr std::string_view kModule = "LMS4xxxConverter";

	using namespace LMS4xxx;
}  // namespace


namespace CsvConverter {

	std::size_t ConvertLms4xxxBin(const std::string &bin_path, const std::string &csv_path) {
		std::ifstream bin_in(bin_path, std::ios::binary);
		if (!bin_in.is_open()) {
			spdlog::error("[{}] Failed to open binary file: {}", kModule, bin_path);
			return 0;
		}

		// Read and validate file header.
		ScanFileHeader file_header{};
		bin_in.read(reinterpret_cast<char *>(&file_header), sizeof(file_header));
		if (file_header.magic != 0x4C4D5334) {
			spdlog::error("[{}] Invalid file magic number in {}", kModule, bin_path);
			return 0;
		}

		const bool is_v2 = (file_header.version >= 2);
		const std::size_t record_header_size = is_v2 ? sizeof(ScanRecordHeader) : sizeof(ScanRecordHeaderV1);

		// For v1 files, record_size was computed with 16-byte header.
		// Validate that record_size is consistent.
		if (file_header.record_size == 0) {
			spdlog::error("[{}] Record size is 0 in {}", kModule, bin_path);
			return 0;
		}

		std::ofstream csv_out(csv_path);
		if (!csv_out.is_open()) {
			spdlog::error("[{}] Failed to open CSV file: {}", kModule, csv_path);
			return 0;
		}

		constexpr std::size_t kCsvBufferSize = 256 * 1024;
		std::vector<char> csv_buffer(kCsvBufferSize);
		csv_out.rdbuf()->pubsetbuf(csv_buffer.data(), static_cast<std::streamsize>(csv_buffer.size()));

		const auto mask = file_header.channel_mask;

		// Write CSV header row.
		fmt::memory_buffer hdr;

		// Per-frame metadata
		fmt::format_to(std::back_inserter(hdr),
			"time_us,transmission_time_us,"
			"telegram_counter,scan_counter,"
			"scan_freq_hz,meas_freq_hz,"
			"device_version,device_number,serial_number,"
			"device_status_1,device_status_2,"
			"digital_in_1,digital_in_2,digital_out_1,digital_out_2,"
			"encoder_position,encoder_speed,"
			"timestamp,"
			"device_name,y_rotation,"
			// Per-point
			"point_index,angle_deg");

		fmt::format_to(std::back_inserter(hdr),
			",distance_mm,rssi,reflectance_pct,angle_correction_deg,quality");

		fmt::format_to(std::back_inserter(hdr), "\n");
		csv_out.write(hdr.data(), static_cast<std::streamsize>(hdr.size()));

		// Read records.
		std::vector<std::uint8_t> record_buf(file_header.record_size);
		std::size_t record_count = 0;

		while (bin_in.read(reinterpret_cast<char *>(record_buf.data()),
						   static_cast<std::streamsize>(file_header.record_size))) {
			const auto *ptr = record_buf.data();

			// Parse record header — fill a v2 header struct from either format.
			ScanRecordHeader rec{};

			if (is_v2) {
				std::memcpy(&rec, ptr, sizeof(ScanRecordHeader));
				ptr += sizeof(ScanRecordHeader);
			} else {
				// V1: only first 16 bytes present.
				ScanRecordHeaderV1 v1{};
				std::memcpy(&v1, ptr, sizeof(v1));
				ptr += sizeof(v1);

				rec.time_since_startup_us = v1.time_since_startup_us;
				rec.telegram_counter = v1.telegram_counter;
				rec.scan_counter = v1.scan_counter;
				rec.num_points = v1.num_points;
				rec.start_angle = v1.start_angle;
				rec.angle_step = v1.angle_step;
			}

			// Locate channel data pointers (order matches serialization).
			const std::uint16_t *dist_data = nullptr;
			const std::uint16_t *rssi_data = nullptr;
			const std::uint16_t *refl_data = nullptr;
			const std::uint16_t *angl_data = nullptr;
			const std::uint8_t *qlty_data = nullptr;

			if (mask & ChannelMask::kDist1) {
				dist_data = reinterpret_cast<const std::uint16_t *>(ptr);
				ptr += file_header.max_points * sizeof(std::uint16_t);
			}
			if (mask & ChannelMask::kRssi1) {
				rssi_data = reinterpret_cast<const std::uint16_t *>(ptr);
				ptr += file_header.max_points * sizeof(std::uint16_t);
			}
			if (mask & ChannelMask::kRefl1) {
				refl_data = reinterpret_cast<const std::uint16_t *>(ptr);
				ptr += file_header.max_points * sizeof(std::uint16_t);
			}
			if (mask & ChannelMask::kAngl1) {
				angl_data = reinterpret_cast<const std::uint16_t *>(ptr);
				ptr += file_header.max_points * sizeof(std::uint16_t);
			}
			if (mask & ChannelMask::kQlty1) {
				qlty_data = ptr;
			}

			// Pre-format per-frame metadata (same for all points in this scan).
			fmt::memory_buffer frame_meta;

			// Timing
			fmt::format_to(std::back_inserter(frame_meta), "{},{},",
						   rec.time_since_startup_us, rec.transmission_time_us);

			// Counters
			fmt::format_to(std::back_inserter(frame_meta), "{},{},",
						   rec.telegram_counter, rec.scan_counter);

			// Frequencies
			fmt::format_to(std::back_inserter(frame_meta), "{:.2f},{},",
						   static_cast<double>(rec.scan_frequency) / 100.0,
						   rec.measurement_frequency);

			// Device info
			fmt::format_to(std::back_inserter(frame_meta), "{},{},{},",
						   rec.device_version, rec.device_number, rec.serial_number);
			fmt::format_to(std::back_inserter(frame_meta), "{},{},",
						   rec.device_status_1, rec.device_status_2);

			// Digital I/O
			fmt::format_to(std::back_inserter(frame_meta), "{},{},{},{},",
						   rec.digital_input_1, rec.digital_input_2,
						   rec.digital_output_1, rec.digital_output_2);

			// Encoder (empty if not present)
			if (rec.has_encoder) {
				fmt::format_to(std::back_inserter(frame_meta), "{},{},",
							   rec.encoder_position, rec.encoder_speed);
			} else {
				fmt::format_to(std::back_inserter(frame_meta), ",,");
			}

			// Timestamp (empty if not present)
			if (rec.has_timestamp) {
				fmt::format_to(std::back_inserter(frame_meta), "{:04d}-{:02d}-{:02d}T{:02d}:{:02d}:{:02d}.{:06d},",
							   rec.ts_year, rec.ts_month, rec.ts_day,
							   rec.ts_hour, rec.ts_minute, rec.ts_second,
							   rec.ts_microsecond);
			} else {
				fmt::format_to(std::back_inserter(frame_meta), ",");
			}

			// Device name (empty if not present)
			if (rec.has_device_name) {
				// Null-terminate safely
				char name_buf[17] = {};
				std::memcpy(name_buf, rec.device_name, 16);
				fmt::format_to(std::back_inserter(frame_meta), "{},", name_buf);
			} else {
				fmt::format_to(std::back_inserter(frame_meta), ",");
			}

			// Y rotation
			fmt::format_to(std::back_inserter(frame_meta), "{:.6f}", rec.y_rotation);

			const std::string_view meta_sv(frame_meta.data(), frame_meta.size());

			// Write one CSV row per measurement point.
			for (std::uint16_t i = 0; i < rec.num_points; ++i) {
				const double angle_deg = (rec.start_angle + static_cast<int>(i) * rec.angle_step) / 10000.0;

				fmt::memory_buffer line;
				// Per-frame metadata + per-point index and angle
				fmt::format_to(std::back_inserter(line), "{},{},{:.4f}", meta_sv, i, angle_deg);

				if (dist_data) {
					fmt::format_to(std::back_inserter(line), ",{:.1f}", dist_data[i] * 0.1);
				} else {
					fmt::format_to(std::back_inserter(line), ",");
				}
				if (rssi_data) {
					fmt::format_to(std::back_inserter(line), ",{}", rssi_data[i]);
				} else {
					fmt::format_to(std::back_inserter(line), ",");
				}
				if (refl_data) {
					fmt::format_to(std::back_inserter(line), ",{:.2f}", refl_data[i] * 0.01);
				} else {
					fmt::format_to(std::back_inserter(line), ",");
				}
				if (angl_data) {
					const double corr_deg = (static_cast<double>(angl_data[i]) - 32768.0) / 10000.0;
					fmt::format_to(std::back_inserter(line), ",{:.4f}", corr_deg);
				} else {
					fmt::format_to(std::back_inserter(line), ",");
				}
				if (qlty_data) {
					fmt::format_to(std::back_inserter(line), ",0x{:02X}", qlty_data[i]);
				} else {
					fmt::format_to(std::back_inserter(line), ",");
				}

				fmt::format_to(std::back_inserter(line), "\n");
				csv_out.write(line.data(), static_cast<std::streamsize>(line.size()));
			}

			++record_count;
		}

		spdlog::info("[{}] Converted {} scan frames (v{}): {} -> {}", kModule, record_count,
					 file_header.version, bin_path, csv_path);
		return record_count;
	}

}  // namespace CsvConverter
