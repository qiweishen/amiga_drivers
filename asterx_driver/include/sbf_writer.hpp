#pragma once

#include <chrono>
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

        void write_block(const QByteArray &block);

        // Close the current file so the next block starts a fresh sequence file
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
