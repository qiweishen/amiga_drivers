#pragma once

#include <chrono>
#include <filesystem>
#include <string>

#include <QByteArray>

#include "file_writer.h"


namespace asterx {
    class SbfWriter {
    public:
        struct Config {
            std::filesystem::path output_dir{"./recordings"};
            std::string file_prefix{"asterx"};
            std::uint64_t rotate_bytes{1ull << 30}; // 1 GiB
            std::chrono::seconds rotate_interval{std::chrono::hours(1)};
        };

        // Throws std::runtime_error when the output directory cannot be created:
        // a recorder that cannot write must never reach the Recording state.
        explicit SbfWriter(Config cfg);

        ~SbfWriter();

        SbfWriter(const SbfWriter &) = delete;

        SbfWriter &operator=(const SbfWriter &) = delete;

        // False on any open/write failure (disk full, directory removed). The
        // caller must treat that as fatal — a silently dropped block is data loss.
        bool WriteBlock(const QByteArray &block);

        // Close the current file so the next block starts a fresh sequence file
        bool EndSegment() noexcept;

        // Flush + close the current file (final shutdown).
        bool close() noexcept;

        const common::RotatingFileWriter::Stats &Stats() const noexcept { return writer_.GetStats(); }

    private:
        Config cfg_;
        common::RotatingFileWriter writer_;
    };
} // namespace asterx
