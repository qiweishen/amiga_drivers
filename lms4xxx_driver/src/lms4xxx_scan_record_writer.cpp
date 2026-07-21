#include "lms4xxx_scan_record_writer.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>
#include <spdlog/spdlog.h>

#include "file_writer.h"
#include "logger.h"
#include "ring_buffer.h"
#include "utility.h"


namespace {
	constexpr std::string_view kModule = "LMS4xxxScanRecordWriter";
	Common::DriverLog g_log{ std::string(kModule) };
	constexpr auto kWriteBackoffSleep = std::chrono::microseconds(500);
	constexpr std::size_t kBatchSize = 32;
}  // namespace


namespace LMS4xxx {

	// Queue element: pre-serialized record bytes.
	struct WriteRecord {
		alignas(8) std::uint8_t data[kMaxRecordBytes];
		std::uint32_t size = 0;
	};


	struct ScanRecordWriter::Impl {
		Config config;
		std::uint32_t record_size = 0;

		// Write queue (SPSC: parse thread → write thread)
		std::unique_ptr<Common::RingBuffer<WriteRecord>> queue;

		// Write thread
		std::thread write_thread;
		std::atomic<bool> running{false};

		// Output file: shared rotating core (size-based splits, per-file header
		// via on_new_file); on-disk layout is unchanged
		std::unique_ptr<Common::RotatingFileWriter> file;
		ScanFileHeader file_header{};  // Cached copy, written to each split file

		// Statistics
		std::atomic<std::size_t> stat_frames_written{0};
		std::atomic<std::size_t> stat_frames_dropped{0};
		std::atomic<std::size_t> stat_bytes_written{0};
		std::atomic<std::size_t> stat_files_created{0};

		explicit Impl(Config cfg) : config(std::move(cfg)) {}

		// Compute the record size based on enabled channels.
		[[nodiscard]] std::uint32_t ComputeRecordSize() const {
			std::uint32_t size = sizeof(ScanRecordHeader);
			if (config.channel_mask & ChannelMask::kDist1) size += kMaxPointsPerScan * sizeof(std::uint16_t);
			if (config.channel_mask & ChannelMask::kRssi1) size += kMaxPointsPerScan * sizeof(std::uint16_t);
			if (config.channel_mask & ChannelMask::kRefl1) size += kMaxPointsPerScan * sizeof(std::uint16_t);
			if (config.channel_mask & ChannelMask::kAngl1) size += kMaxPointsPerScan * sizeof(std::uint16_t);
			if (config.channel_mask & ChannelMask::kQlty1) size += kMaxPointsPerScan * sizeof(std::uint8_t);
			return size;
		}

		// Serialize a ScanData into a WriteRecord. Only touches enabled channels.
		void Serialize(const ScanData &scan, WriteRecord &record) const {
			std::uint8_t *ptr = record.data;

			// Record header (v2: 96 bytes with all metadata)
			ScanRecordHeader header{};

			// Core fields
			header.time_since_startup_us = scan.time_since_startup_us;
			header.telegram_counter = scan.telegram_counter;
			header.scan_counter = scan.scan_counter;

			const auto *dist_ch = scan.DistanceChannel();
			if (dist_ch) {
				header.num_points = dist_ch->num_data;
				header.start_angle = dist_ch->start_angle;
				header.angle_step = dist_ch->angle_step;
			}

			// Timing
			header.transmission_time_us = scan.transmission_time_us;
			header.scan_frequency = scan.scan_frequency;
			header.measurement_frequency = scan.measurement_frequency;

			// Device info
			header.device_version = scan.device_info.version_number;
			header.device_number = scan.device_info.device_number;
			header.serial_number = scan.device_info.serial_number;
			header.device_status_1 = static_cast<std::uint8_t>(scan.device_info.device_status_1);
			header.device_status_2 = static_cast<std::uint8_t>(scan.device_info.device_status_2);

			// Digital I/O
			header.digital_input_1 = scan.digital_input_1;
			header.digital_input_2 = scan.digital_input_2;
			header.digital_output_1 = scan.digital_output_1;
			header.digital_output_2 = scan.digital_output_2;

			// Encoder
			header.has_encoder = scan.has_encoder ? 1 : 0;
			if (scan.has_encoder) {
				header.encoder_position = scan.encoder.position;
				header.encoder_speed = scan.encoder.speed;
			}

			// Timestamp
			header.has_timestamp = scan.has_timestamp ? 1 : 0;
			if (scan.has_timestamp) {
				header.ts_year = scan.timestamp.year;
				header.ts_month = scan.timestamp.month;
				header.ts_day = scan.timestamp.day;
				header.ts_hour = scan.timestamp.hour;
				header.ts_minute = scan.timestamp.minute;
				header.ts_second = scan.timestamp.second;
				header.ts_microsecond = scan.timestamp.microsecond;
			}

			// Device name
			header.has_device_name = scan.has_device_name ? 1 : 0;
			if (scan.has_device_name) {
				const auto copy_len = std::min(scan.device_name.size(), sizeof(header.device_name) - 1);
				std::memcpy(header.device_name, scan.device_name.data(), copy_len);
			}

			// Position
			header.y_rotation = scan.y_rotation;

			std::memcpy(ptr, &header, sizeof(header));
			ptr += sizeof(header);

			// DIST1
			if (config.channel_mask & ChannelMask::kDist1) {
				const std::size_t array_bytes = kMaxPointsPerScan * sizeof(std::uint16_t);
				std::memset(ptr, 0, array_bytes);
				if (dist_ch && dist_ch->num_data > 0) {
					std::memcpy(ptr, dist_ch->data.data(), dist_ch->num_data * sizeof(std::uint16_t));
				}
				ptr += array_bytes;
			}

			// RSSI1
			if (config.channel_mask & ChannelMask::kRssi1) {
				const std::size_t array_bytes = kMaxPointsPerScan * sizeof(std::uint16_t);
				std::memset(ptr, 0, array_bytes);
				const auto *ch = scan.RssiChannel();
				if (ch && ch->num_data > 0) {
					std::memcpy(ptr, ch->data.data(), ch->num_data * sizeof(std::uint16_t));
				}
				ptr += array_bytes;
			}

			// REFL1
			if (config.channel_mask & ChannelMask::kRefl1) {
				const std::size_t array_bytes = kMaxPointsPerScan * sizeof(std::uint16_t);
				std::memset(ptr, 0, array_bytes);
				const auto *ch = scan.ReflectanceChannel();
				if (ch && ch->num_data > 0) {
					std::memcpy(ptr, ch->data.data(), ch->num_data * sizeof(std::uint16_t));
				}
				ptr += array_bytes;
			}

			// ANGL1
			if (config.channel_mask & ChannelMask::kAngl1) {
				const std::size_t array_bytes = kMaxPointsPerScan * sizeof(std::uint16_t);
				std::memset(ptr, 0, array_bytes);
				const auto *ch = scan.AngleCorrectionChannel();
				if (ch && ch->num_data > 0) {
					std::memcpy(ptr, ch->data.data(), ch->num_data * sizeof(std::uint16_t));
				}
				ptr += array_bytes;
			}

			// QLTY1
			if (config.channel_mask & ChannelMask::kQlty1) {
				const std::size_t array_bytes = kMaxPointsPerScan * sizeof(std::uint8_t);
				std::memset(ptr, 0, array_bytes);
				const auto *ch = scan.QualityChannel();
				if (ch && ch->num_data > 0) {
					std::memcpy(ptr, ch->data.data(), ch->num_data * sizeof(std::uint8_t));
				}
				ptr += array_bytes;
			}

			record.size = static_cast<std::uint32_t>(ptr - record.data);
		}

		// Generate split file path: scan_xxx.bin → scan_xxx_000.bin, scan_xxx_001.bin, etc.
		[[nodiscard]] std::string GenerateFilePath(std::uint32_t index) const {
			std::filesystem::path p(config.bin_path);
			std::string stem = p.stem().string();
			std::string extension = p.extension().string();
			std::filesystem::path parent = p.parent_path();
			std::string filename = fmt::format("{}_{:03d}{}", stem, index, extension);
			return (parent / filename).string();
		}

		// Build the rotating-file core: split naming, size threshold and the
		// per-file format header, all unchanged from the previous in-place code.
		[[nodiscard]] std::unique_ptr<Common::RotatingFileWriter> MakeFile() {
			Common::RotatingFileWriter::Options opts;
			opts.make_path = [this](std::uint32_t seq) { return GenerateFilePath(seq); };
			opts.max_file_bytes = config.max_file_bytes;
			opts.buffer_bytes = config.write_buffer_size;
			opts.on_new_file = [this](std::ofstream &f) {
				f.write(reinterpret_cast<const char *>(&file_header), sizeof(file_header));
				if (!f) {
					return false;
				}
				stat_files_created.fetch_add(1, std::memory_order_relaxed);
				g_log.trace("Opened split file");
				return true;
			};
			return std::make_unique<Common::RotatingFileWriter>(std::move(opts));
		}

		// Write thread main loop. Append() handles the size-based split
		// (per-file header via on_new_file) internally.
		void WriteLoop() {
			g_log.trace("Write thread started");

			while (running.load(std::memory_order_acquire)) {
				std::size_t count = 0;
				WriteRecord record;

				while (count < kBatchSize && queue->try_pop(record)) {
					if (!file->Append(record.data, record.size)) {
						g_log.error("Failed to write split file, stopping write loop");
						return;
					}
					stat_bytes_written.fetch_add(record.size, std::memory_order_relaxed);
					stat_frames_written.fetch_add(1, std::memory_order_relaxed);
					++count;
				}

				if (count == 0) {
					std::this_thread::sleep_for(kWriteBackoffSleep);
				}
			}

			// Drain remaining records after stop signal.
			WriteRecord record;
			while (queue->try_pop(record)) {
				if (!file->Append(record.data, record.size)) {
					g_log.error("Failed to write split file during drain");
					break;
				}
				stat_bytes_written.fetch_add(record.size, std::memory_order_relaxed);
				stat_frames_written.fetch_add(1, std::memory_order_relaxed);
			}

			g_log.trace("Write thread stopped");
		}
	};


	ScanRecordWriter::ScanRecordWriter(Config config) : impl_(std::make_unique<Impl>(std::move(config))) {}


	ScanRecordWriter::~ScanRecordWriter() {
		if (impl_->running.load(std::memory_order_relaxed)) {
			Stop();
		}
	}


	bool ScanRecordWriter::Start(const ScanConfig &scan_config) {
		if (impl_->running.load(std::memory_order_relaxed)) {
			g_log.warn("Already running");
			return false;
		}

		impl_->record_size = impl_->ComputeRecordSize();

		std::filesystem::path parent = std::filesystem::path(impl_->config.bin_path).parent_path();
		if (!parent.empty()) {
			std::filesystem::create_directories(parent);
		}

		// Cache file header for reuse across split files.
		impl_->file_header = ScanFileHeader{};
		impl_->file_header.channel_mask = impl_->config.channel_mask;
		impl_->file_header.start_angle = scan_config.StartAngleDevice();
		impl_->file_header.stop_angle = scan_config.StopAngleDevice();
		impl_->file_header.angle_step = scan_config.AngularResolutionDevice();
		impl_->file_header.record_size = impl_->record_size;

		// Open first split file (index 0) eagerly so a bad path fails Start()
		impl_->file = impl_->MakeFile();
		if (!impl_->file->OpenNext()) {
			g_log.error("Failed to open split file: {}", impl_->GenerateFilePath(0));
			return false;
		}

		impl_->queue = std::make_unique<Common::RingBuffer<WriteRecord>>(impl_->config.queue_capacity);

		impl_->running.store(true, std::memory_order_release);
		impl_->write_thread = std::thread([this]() { impl_->WriteLoop(); });

		g_log.trace("Recording to {} (record: {} B, channels: 0x{:02X}, queue: {} frames, max file: {} MB)",
					impl_->config.bin_path, impl_->record_size,
					impl_->config.channel_mask, impl_->config.queue_capacity,
					impl_->config.max_file_bytes / (1024 * 1024));
		return true;
	}


	void ScanRecordWriter::Stop() {
		if (!impl_->running.load(std::memory_order_relaxed)) {
			return;
		}

		impl_->running.store(false, std::memory_order_release);

		if (impl_->write_thread.joinable()) {
			impl_->write_thread.join();
		}

		if (impl_->file) {
			impl_->file->Close();
			impl_->file.reset();
		}

		impl_->queue.reset();

		g_log.trace("Recording stopped");
	}


	void ScanRecordWriter::OnScan(const ScanData &scan) {
		WriteRecord record{};
		impl_->Serialize(scan, record);

		if (!impl_->queue->try_push(std::move(record))) {
			impl_->stat_frames_dropped.fetch_add(1, std::memory_order_relaxed);
		}
	}


	ScanRecordWriter::Statistics ScanRecordWriter::GetStatistics() const {
		return {
				impl_->stat_frames_written.load(std::memory_order_relaxed),
				impl_->stat_frames_dropped.load(std::memory_order_relaxed),
				impl_->stat_bytes_written.load(std::memory_order_relaxed),
				impl_->stat_files_created.load(std::memory_order_relaxed),
		};
	}


	void ScanRecordWriter::LogStatistics() const {
		const auto stats = GetStatistics();
		g_log.info("=== LMS4xxx RECORDING STATISTICS === : Total frames written: {}, dropped: {}, total bytes: {:.2f} MB, files created: {}",
				   stats.frames_written, stats.frames_dropped,
				   static_cast<double>(stats.bytes_written) / (1024.0 * 1024.0),
				   stats.files_created);
	}

}  // namespace LMS4xxx
