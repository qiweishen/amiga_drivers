#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>

#include <QByteArray>

#include "file_writer.h"

namespace asterx {
    struct WriterStats {
        std::uint64_t bytes_written{0};
        std::uint64_t blocks_written{0};
        std::uint64_t files_opened{0};
    };

    // Streams CRC-validated SBF blocks (as delivered by SsnRx::newSBFBlock) to
    // disk on the shared rotating-file core, one file at a time, rotating when
    // the current file passes a size or wall-time threshold. rotate_bytes is a
    // HARD cap (pre-write check): the RxTools converters refuse files >= 2 GB.
    //
    // Output filenames: <prefix>-YYYYMMDDTHHMMSSZ-<seq>.sbf  (seq starts at 1).
    //
    // Blocks arrive wire-exact (sync 0x24 0x40, CRC, ID, Length, body), so the
    // writer is a plain append; rotation boundaries always fall between blocks.
    // Runs synchronously on the Qt event loop thread: a disk failure surfaces
    // immediately as std::runtime_error (Session turns it into fatalError).
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

        [[nodiscard]] WriterStats stats() const noexcept;
        [[nodiscard]] std::filesystem::path current_file() const noexcept { return writer_.CurrentPath(); }

    private:
        Config cfg_;
        Common::RotatingFileWriter writer_;
    };
} // namespace asterx
