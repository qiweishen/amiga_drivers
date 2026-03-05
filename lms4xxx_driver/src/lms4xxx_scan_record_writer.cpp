#include "lms4xxx_scan_record_writer.h"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>
#include <spdlog/spdlog.h>

#include "ring_buffer.h"
#include "utility.h"


namespace {
	constexpr std::string_view kModule = "LMS4xxxScanRecordWriter";
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

		// Output file
		std::ofstream bin_file;
		std::vector<char> file_buffer;

		// File splitting state
		std::uint32_t file_index = 0;
		std::size_t current_file_bytes = 0;
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

			// Record header
			ScanRecordHeader header{};
			header.time_since_startup_us = scan.time_since_startup_us;
			header.telegram_counter = scan.telegram_counter;
			header.scan_counter = scan.scan_counter;

			const auto *dist_ch = scan.DistanceChannel();
			if (dist_ch) {
				header.num_points = dist_ch->num_data;
				header.start_angle = dist_ch->start_angle;
				header.angle_step = dist_ch->angle_step;
			}

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

		// Close current file and open next split file. Returns true on success.
		bool OpenNextFile() {
			if (bin_file.is_open()) {
				bin_file.flush();
				bin_file.close();
			}

			std::string path = GenerateFilePath(file_index);

			// pubsetbuf must be called before open; reuse the existing file_buffer
			bin_file.rdbuf()->pubsetbuf(file_buffer.data(),
										static_cast<std::streamsize>(file_buffer.size()));
			bin_file.open(path, std::ios::out | std::ios::binary);

			if (!bin_file.is_open()) {
				Common::Log::log_message(spdlog::level::err, kModule,
										 fmt::format("Failed to open split file: {}", path));
				return false;
			}

			// Write file header to new split file
			bin_file.write(reinterpret_cast<const char *>(&file_header), sizeof(file_header));
			current_file_bytes = sizeof(ScanFileHeader);
			++file_index;
			stat_files_created.fetch_add(1, std::memory_order_relaxed);

			Common::Log::log_message(spdlog::level::trace, kModule,
									 fmt::format("Opened split file: {}", path));
			return true;
		}

		// Write thread main loop.
		void WriteLoop() {
			Common::Log::log_message(spdlog::level::trace, kModule, "Write thread started");

			const bool splitting_enabled = config.max_file_bytes > 0;

			while (running.load(std::memory_order_acquire)) {
				std::size_t count = 0;
				WriteRecord record;

				while (count < kBatchSize && queue->try_pop(record)) {
					bin_file.write(reinterpret_cast<const char *>(record.data),
								   static_cast<std::streamsize>(record.size));
					stat_bytes_written.fetch_add(record.size, std::memory_order_relaxed);
					stat_frames_written.fetch_add(1, std::memory_order_relaxed);
					current_file_bytes += record.size;
					++count;

					if (splitting_enabled && current_file_bytes >= config.max_file_bytes) {
						if (!OpenNextFile()) {
							Common::Log::log_message(spdlog::level::err, kModule,
													 "Failed to open next split file, stopping write loop");
							return;
						}
					}
				}

				if (count == 0) {
					std::this_thread::sleep_for(kWriteBackoffSleep);
				}
			}

			// Drain remaining records after stop signal.
			WriteRecord record;
			while (queue->try_pop(record)) {
				bin_file.write(reinterpret_cast<const char *>(record.data),
							   static_cast<std::streamsize>(record.size));
				stat_bytes_written.fetch_add(record.size, std::memory_order_relaxed);
				stat_frames_written.fetch_add(1, std::memory_order_relaxed);
				current_file_bytes += record.size;

				if (splitting_enabled && current_file_bytes >= config.max_file_bytes) {
					if (!OpenNextFile()) {
						Common::Log::log_message(spdlog::level::err, kModule,
												 "Failed to open next split file during drain");
						break;
					}
				}
			}

			Common::Log::log_message(spdlog::level::trace, kModule, "Write thread stopped");
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
			Common::Log::log_message(spdlog::level::warn, kModule, "Already running");
			return false;
		}

		impl_->record_size = impl_->ComputeRecordSize();

		// Ensure parent directory exists.
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

		// Allocate file buffer once (reused across split files).
		impl_->file_buffer.resize(impl_->config.write_buffer_size);

		// Reset splitting state.
		impl_->file_index = 0;
		impl_->current_file_bytes = 0;

		// Open first split file (index 0).
		if (!impl_->OpenNextFile()) {
			return false;
		}

		// Create SPSC queue.
		impl_->queue = std::make_unique<Common::RingBuffer<WriteRecord>>(impl_->config.queue_capacity);

		// Start write thread.
		impl_->running.store(true, std::memory_order_release);
		impl_->write_thread = std::thread([this]() { impl_->WriteLoop(); });

		Common::Log::log_message(spdlog::level::trace, kModule,
								 fmt::format("Recording to {} (record: {} B, channels: 0x{:02X}, queue: {} frames, max file: {} MB)",
											 impl_->config.bin_path, impl_->record_size,
											 impl_->config.channel_mask, impl_->config.queue_capacity,
											 impl_->config.max_file_bytes / (1024 * 1024)));
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

		if (impl_->bin_file.is_open()) {
			impl_->bin_file.flush();
			impl_->bin_file.close();
		}

		impl_->queue.reset();

		Common::Log::log_message(spdlog::level::trace, kModule, "Recording stopped");
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
		Common::Log::log_message(spdlog::level::info, kModule,
								 fmt::format("=== LMS4xxx RECORDING STATISTICS === : Total frames written: {}, dropped: {}, total bytes: {:.2f} MB, files created: {}",
											 stats.frames_written, stats.frames_dropped,
											 static_cast<double>(stats.bytes_written) / (1024.0 * 1024.0),
											 stats.files_created));
	}

}  // namespace LMS4xxx
