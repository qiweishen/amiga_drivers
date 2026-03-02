/// @file common/binary_writer.h
/// @brief Unified buffered binary writer using POSIX write() with configurable buffer.
///
/// Accumulates data in a memory buffer and flushes to disk with POSIX write(),
/// handling partial-write loops and EINTR retry correctly.
///
/// NOT thread-safe by design: a ThreadSafeQueue is used to pass data to a
/// dedicated writer thread, which is the sole caller of this class.
///
/// The LMS driver uses POSIX write() with an 8 MB buffer; the INS driver uses
/// C++ streams with pubsetbuf (256 KB). This class adopts the POSIX approach
/// (more robust for crash safety when combined with periodic flushes) with a
/// configurable buffer size.

#ifndef COMMON_BINARY_WRITER_H
#define COMMON_BINARY_WRITER_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>


namespace Common {
    struct BinaryWriterConfig {
        std::string output_path;
        size_t buffer_size = 8 * 1024 * 1024; // 8 MB default (LMS driver value)
        bool sync_on_flush = false; // Call fdatasync() after each flush for durability.
    };


    /// Buffered binary writer. Writes arbitrary binary data in configurable chunks.
    /// Caller is responsible for any higher-level file format (headers, framing, etc.).
    class BinaryWriter {
    public:
        explicit BinaryWriter(BinaryWriterConfig config);

        ~BinaryWriter();

        BinaryWriter(const BinaryWriter &) = delete;

        BinaryWriter &operator=(const BinaryWriter &) = delete;

        /// Open the output file and allocate the internal buffer.
    /// Returns true on success.
        bool open();

        /// Append `len` bytes from `data` to the internal buffer, flushing as needed.
    /// Returns true on success.
        bool write(const void *data, size_t len);

        /// Flush all buffered data to the OS. Returns true on success.
        bool flush();

        /// Flush and close the file. Never throws (safe to call from destructors).
        void close() noexcept;

        [[nodiscard]] bool is_open() const { return fd_ >= 0; }
        [[nodiscard]] uint64_t bytes_written() const { return bytes_written_; }
        [[nodiscard]] uint64_t flush_count() const { return flush_count_; }

    private:
        BinaryWriterConfig config_;
        int fd_ = -1;
        std::unique_ptr<uint8_t[]> buffer_;
        size_t buffer_pos_ = 0;
        uint64_t bytes_written_ = 0;
        uint64_t flush_count_ = 0;
    };
} // namespace Common


#endif // COMMON_BINARY_WRITER_H
