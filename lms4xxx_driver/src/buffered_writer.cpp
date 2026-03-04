#include "buffered_writer.h"
#include "utility.h"
#include <spdlog/spdlog.h>
#include "point_cloud_types.h"


BufferedBinaryWriter::BufferedBinaryWriter(const BufferedWriterConfig &config)
    : config_(config) , writer_(Common::BinaryWriterConfig{config.output_path, config.buffer_size}) {
}


bool BufferedBinaryWriter::open() {
    if (!writer_.open()) {
        return false;
    }

    stats_ = WriterStats{};

    // Write file header: magic + version.
    if (!writer_.write(FileFormat::kMagic.data(), FileFormat::kMagic.size())) {
        return false;
    }
    if (!writer_.write(&FileFormat::kVersion, sizeof(FileFormat::kVersion))) {
        return false;
    }

    Common::Log::log_message(spdlog::level::info, "Writer",
                             fmt::format("Opened {} (buffer: {} KB)", config_.output_path, config_.buffer_size / 1024));
    return true;
}


bool BufferedBinaryWriter::write_frame(const PointCloudFrame &frame) {
    // Frame header: timestamp_ns (8 bytes) + num_points (4 bytes)
    if (!writer_.write(&frame.timestamp_ns, sizeof(frame.timestamp_ns))) {
        return false;
    }

    uint32_t num_points = static_cast<uint32_t>(frame.points.size());
    if (!writer_.write(&num_points, sizeof(num_points))) {
        return false;
    }

    // Point data: N * 16 bytes
    if (num_points > 0) {
        if (!writer_.write(frame.points.data(), num_points * sizeof(PointXYZI))) {
            return false;
        }
    }

    stats_.frames_written++;
    stats_.bytes_written = writer_.bytes_written();
    stats_.flush_count = writer_.flush_count();

    // Periodic flush at reporting boundaries for data durability.
    if (config_.status_interval_frames > 0 &&
        stats_.frames_written % config_.status_interval_frames == 0) {
        if (!writer_.flush()) {
            return false;
        }
        stats_.flush_count = writer_.flush_count();
    }

    return true;
}


void BufferedBinaryWriter::close() {
    if (!writer_.is_open()) {
        return;
    }

    // Update stats before close (flush happens inside close()).
    stats_.bytes_written = writer_.bytes_written();
    stats_.flush_count = writer_.flush_count();

    writer_.close();

    Common::Log::log_message(spdlog::level::info, "Writer", fmt::format(
                                 "Closed {} (frames: {}, bytes: {}, flushes: {})",
                                 config_.output_path, stats_.frames_written,
                                 stats_.bytes_written, stats_.flush_count));
}


bool BufferedBinaryWriter::is_open() const {
    return writer_.is_open();
}


WriterStats BufferedBinaryWriter::stats() const {
    WriterStats s = stats_;
    s.bytes_written = writer_.bytes_written();
    s.flush_count = writer_.flush_count();
    return s;
}
