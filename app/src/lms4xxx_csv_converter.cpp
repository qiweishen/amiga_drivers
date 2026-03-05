#include "lms4xxx_csv_converter.h"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <spdlog/spdlog.h>
#include <vector>

#include <iostream>


namespace {
	constexpr std::string_view kModule = "LMS4xxxConverter";

	constexpr std::size_t kMaxPointsPerScan = 841;

	namespace ChannelMask {
		constexpr std::uint16_t kDist1 = 0x01;
		constexpr std::uint16_t kRssi1 = 0x02;
		constexpr std::uint16_t kRefl1 = 0x04;
		constexpr std::uint16_t kAngl1 = 0x08;
		constexpr std::uint16_t kQlty1 = 0x10;
	}  // namespace ChannelMask

#pragma pack(push, 1)
	struct ScanFileHeader {
		std::uint32_t magic = 0x4C4D5334;
		std::uint16_t version = 1;
		std::uint16_t channel_mask = 0;
		std::uint16_t max_points = static_cast<std::uint16_t>(kMaxPointsPerScan);
		std::int32_t start_angle = 0;
		std::int32_t stop_angle = 0;
		std::uint16_t angle_step = 0;
		std::uint32_t scan_frequency = 0;
		std::uint32_t record_size = 0;
		std::uint8_t reserved[36] = {};
	};
#pragma pack(pop)

	static_assert(sizeof(ScanFileHeader) == 64, "ScanFileHeader must be 64 bytes");

#pragma pack(push, 1)
	struct ScanRecordHeader {
		std::uint32_t time_since_startup_us = 0;
		std::uint16_t telegram_counter = 0;
		std::uint16_t scan_counter = 0;
		std::uint16_t num_points = 0;
		std::int32_t start_angle = 0;
		std::uint16_t angle_step = 0;
	};
#pragma pack(pop)

	static_assert(sizeof(ScanRecordHeader) == 16, "ScanRecordHeader must be 16 bytes");
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

		std::ofstream csv_out(csv_path);
		if (!csv_out.is_open()) {
			spdlog::error("[{}] Failed to open CSV file: {}", kModule, csv_path);
			return 0;
		}

		constexpr std::size_t kCsvBufferSize = 256 * 1024;
		std::vector<char> csv_buffer(kCsvBufferSize);
		csv_out.rdbuf()->pubsetbuf(csv_buffer.data(), static_cast<std::streamsize>(csv_buffer.size()));

		// Write CSV header row.
		fmt::memory_buffer hdr;
		fmt::format_to(std::back_inserter(hdr), "time_us,telegram_counter,scan_counter,point_index,angle_deg");
		if (file_header.channel_mask & ChannelMask::kDist1) {
			fmt::format_to(std::back_inserter(hdr), ",distance_mm");
		}
		if (file_header.channel_mask & ChannelMask::kRssi1) {
			fmt::format_to(std::back_inserter(hdr), ",rssi");
		}
		if (file_header.channel_mask & ChannelMask::kRefl1) {
			fmt::format_to(std::back_inserter(hdr), ",reflectance_pct");
		}
		if (file_header.channel_mask & ChannelMask::kAngl1) {
			fmt::format_to(std::back_inserter(hdr), ",angle_correction_deg");
		}
		if (file_header.channel_mask & ChannelMask::kQlty1) {
			fmt::format_to(std::back_inserter(hdr), ",quality");
		}
		fmt::format_to(std::back_inserter(hdr), "\n");
		csv_out.write(hdr.data(), static_cast<std::streamsize>(hdr.size()));

		// Read records one by one.
		std::vector<std::uint8_t> record_buf(file_header.record_size);
		std::size_t record_count = 0;

		while (bin_in.read(reinterpret_cast<char *>(record_buf.data()), static_cast<std::streamsize>(file_header.record_size))) {
			const auto *ptr = record_buf.data();

			// Parse record header.
			ScanRecordHeader rec{};
			std::memcpy(&rec, ptr, sizeof(rec));
			ptr += sizeof(rec);

			// Locate channel data pointers (order matches serialization).
			const std::uint16_t *dist_data = nullptr;
			const std::uint16_t *rssi_data = nullptr;
			const std::uint16_t *refl_data = nullptr;
			const std::uint16_t *angl_data = nullptr;
			const std::uint8_t *qlty_data = nullptr;

			if (file_header.channel_mask & ChannelMask::kDist1) {
				dist_data = reinterpret_cast<const std::uint16_t *>(ptr);
				ptr += file_header.max_points * sizeof(std::uint16_t);
			}
			if (file_header.channel_mask & ChannelMask::kRssi1) {
				rssi_data = reinterpret_cast<const std::uint16_t *>(ptr);
				ptr += file_header.max_points * sizeof(std::uint16_t);
			}
			if (file_header.channel_mask & ChannelMask::kRefl1) {
				refl_data = reinterpret_cast<const std::uint16_t *>(ptr);
				ptr += file_header.max_points * sizeof(std::uint16_t);
			}
			if (file_header.channel_mask & ChannelMask::kAngl1) {
				angl_data = reinterpret_cast<const std::uint16_t *>(ptr);
				ptr += file_header.max_points * sizeof(std::uint16_t);
			}
			if (file_header.channel_mask & ChannelMask::kQlty1) {
				qlty_data = ptr;
			}

			// Write one CSV row per measurement point.
			for (std::uint16_t i = 0; i < rec.num_points; ++i) {
				const double angle_deg = (rec.start_angle + static_cast<int>(i) * rec.angle_step) / 10000.0;

				fmt::memory_buffer line;
				fmt::format_to(std::back_inserter(line), "{},{},{},{},{:.4f}", rec.time_since_startup_us, rec.telegram_counter,
							   rec.scan_counter, i, angle_deg);

				if (dist_data) {
					fmt::format_to(std::back_inserter(line), ",{:.1f}", dist_data[i] * 0.1);
				}
				if (rssi_data) {
					fmt::format_to(std::back_inserter(line), ",{}", rssi_data[i]);
				}
				if (refl_data) {
					fmt::format_to(std::back_inserter(line), ",{:.2f}", refl_data[i] * 0.01);
				}
				if (angl_data) {
					const double corr_deg = (static_cast<double>(angl_data[i]) - 32768.0) / 10000.0;
					fmt::format_to(std::back_inserter(line), ",{:.4f}", corr_deg);
				}
				if (qlty_data) {
					fmt::format_to(std::back_inserter(line), ",0x{:02X}", qlty_data[i]);
				}

				fmt::format_to(std::back_inserter(line), "\n");
				csv_out.write(line.data(), static_cast<std::streamsize>(line.size()));
			}

			++record_count;
		}

		spdlog::info("[{}] Converted {} scan frames: {} -> {}", kModule, record_count, bin_path, csv_path);
		return record_count;
	}

}  // namespace CsvConverter
