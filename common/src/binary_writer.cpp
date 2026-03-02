#include "binary_writer.h"

#include <cerrno>
#include <cstring>
#include <utility>
#include <fcntl.h>
#include <spdlog/spdlog.h>
#include <unistd.h>


namespace Common {
    BinaryWriter::BinaryWriter(BinaryWriterConfig config)
        : config_(std::move(config)) {
    }


    BinaryWriter::~BinaryWriter() {
        if (fd_ >= 0) {
            close();
        }
    }


    bool BinaryWriter::open() {
        fd_ = ::open(config_.output_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd_ < 0) {
            spdlog::error("[BinaryWriter]: Failed to open '{}': {}", config_.output_path, std::strerror(errno));
            return false;
        }

        buffer_ = std::make_unique<uint8_t[]>(config_.buffer_size);
        buffer_pos_ = 0;
        bytes_written_ = 0;
        flush_count_ = 0;

        return true;
    }


    bool BinaryWriter::write(const void *data, size_t len) {
        const auto *src = static_cast<const uint8_t *>(data);
        size_t remaining = len;

        while (remaining > 0) {
            size_t space = config_.buffer_size - buffer_pos_;

            if (space == 0) {
                if (!flush()) {
                    return false;
                }
                space = config_.buffer_size;
            }

            size_t to_copy = std::min(remaining, space);
            std::memcpy(buffer_.get() + buffer_pos_, src, to_copy);
            buffer_pos_ += to_copy;
            src += to_copy;
            remaining -= to_copy;
        }

        return true;
    }


    bool BinaryWriter::flush() {
        if (buffer_pos_ == 0) {
            return true;
        }

        const uint8_t *ptr = buffer_.get();
        size_t remaining = buffer_pos_;

        while (remaining > 0) {
            ssize_t written = ::write(fd_, ptr, remaining);
            if (written < 0) {
                if (errno == EINTR) {
                    continue; // Interrupted by signal, retry.
                }
                spdlog::error("[BinaryWriter]: write() failed: {}", std::strerror(errno));
                return false;
            }
            ptr += written;
            remaining -= static_cast<size_t>(written);
            bytes_written_ += static_cast<uint64_t>(written);
        }

        buffer_pos_ = 0;
        flush_count_++;

        if (config_.sync_on_flush) {
            if (::fdatasync(fd_) < 0 && errno != EINVAL) {
                // EINVAL: fd doesn't support sync (e.g. pipe) — ignore silently.
                spdlog::warn("[BinaryWriter]: fdatasync() failed: {}", std::strerror(errno));
            }
        }

        return true;
    }


    void BinaryWriter::close() noexcept {
        if (fd_ < 0) {
            return;
        }

        // Defensive: catch any exception from flush() to guarantee fd cleanup.
        try {
            if (buffer_pos_ > 0) {
                flush();
            }
        } catch (const std::exception &e) {
            // Best effort: log directly via spdlog (bypasses Log::log_message to avoid
            // any risk of throwing in the shutdown path) and proceed to close the fd.
            spdlog::warn("[BinaryWriter]: flush() failed during close: {}", e.what());
        }

        ::close(fd_);
        fd_ = -1;
        buffer_.reset();
    }
} // namespace Common
