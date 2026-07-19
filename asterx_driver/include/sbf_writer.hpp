// SPDX-License-Identifier: BSD-3-Clause
#pragma once

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>

#include <QByteArray>

namespace asterx {
    struct WriterStats {
        std::uint64_t bytes_written{0};
        std::uint64_t blocks_written{0};
        std::uint64_t files_opened{0};
    };

    // Streams CRC-validated SBF blocks (as delivered by SsnRx::newSBFBlock) to
    // disk, one file at a time, rotating when the current file passes a size or
    // wall-time threshold.
    //
    // Output filenames: <prefix>-YYYYMMDDTHHMMSSZ-<seq>.sbf  (seq starts at 1).
    //
    // Blocks arrive wire-exact (sync 0x24 0x40, CRC, ID, Length, body), so the
    // writer is a plain append; rotation boundaries always fall between blocks.
    class SbfWriter {
    public:
        struct Config {
            std::filesystem::path output_dir{"./recordings"};
            std::string file_prefix{"asterx"};
            std::uint64_t rotate_bytes{1ull << 30}; // 1 GiB
            std::chrono::seconds rotate_interval{std::chrono::hours(1)};
        };

        explicit SbfWriter(Config cfg);

        ~SbfWriter();

        SbfWriter(const SbfWriter &) = delete;

        SbfWriter &operator=(const SbfWriter &) = delete;

        // Append one validated SBF block. Opens a new file lazily on the first
        // block of a segment; checks rotation after the write.
        // Throws std::runtime_error on disk I/O failure.
        void write_block(const QByteArray &block);

        // Close the current file so the next block starts a fresh sequence file.
        // Called on disconnect: segment boundaries then coincide with link gaps.
        void end_segment() noexcept;

        // Flush + close the current file (final shutdown).
        void close() noexcept;

        [[nodiscard]] const WriterStats &stats() const noexcept { return stats_; }
        [[nodiscard]] std::filesystem::path current_file() const noexcept { return current_path_; }

    private:
        void open_new_file_();

        void rotate_if_needed_();

        Config cfg_;
        std::FILE *fp_{nullptr};
        std::filesystem::path current_path_;
        std::uint64_t current_size_{0};
        std::chrono::steady_clock::time_point file_start_{};
        int seq_{0};

        WriterStats stats_{};
    };
} // namespace asterx
