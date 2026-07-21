#include "sbf_writer.hpp"

#include <stdexcept>
#include <system_error>
#include <utility>

#include "asterx_log.hpp"
#include "time_util.h"

namespace asterx {
    namespace {
        Common::RotatingFileWriter::Options make_options(const SbfWriter::Config &cfg) {
            Common::RotatingFileWriter::Options opts;
            // Fresh UTC stamp per file; seq is 0-based in the core, 1-based in
            // names. Capture by value: the lambda outlives this function
            opts.make_path = [dir = cfg.output_dir, prefix = cfg.file_prefix](std::uint32_t seq) {
                return (dir /
                        (prefix + "-" + Common::TimeUtil::CompactUtcNow() + "-" +
                         std::to_string(seq + 1) + ".sbf"))
                        .string();
            };
            opts.max_file_bytes = cfg.rotate_bytes;
            opts.rotate_interval = cfg.rotate_interval;
            opts.precheck_size_cap = true; // rotate_bytes is a hard cap (RxTools < 2 GB)
            return opts;
        }
    } // namespace

    SbfWriter::SbfWriter(Config cfg) : cfg_(std::move(cfg)), writer_(make_options(cfg_)) {
        std::error_code ec;
        std::filesystem::create_directories(cfg_.output_dir, ec);
        if (ec) {
            throw std::runtime_error("cannot create output directory '" +
                                     cfg_.output_dir.string() + "': " + ec.message());
        }
    }

    SbfWriter::~SbfWriter() { close(); }

    void SbfWriter::close() noexcept { writer_.Close(); }

    void SbfWriter::end_segment() noexcept {
        if (writer_.IsOpen()) {
            log::info("[writer] closing segment {} ({} bytes)",
                      writer_.CurrentPath(), writer_.CurrentFileBytes());
            writer_.EndSegment();
        }
    }

    void SbfWriter::write_block(const QByteArray &block) {
        if (block.isEmpty()) return;

        const auto files_before = writer_.GetStats().files_opened;
        if (!writer_.Append(block.constData(), static_cast<std::size_t>(block.size()))) {
            if (!writer_.IsOpen()) {
                throw std::runtime_error("cannot open SBF output file in '" +
                                         cfg_.output_dir.string() + "'");
            }
            throw std::runtime_error("cannot write SBF block to '" +
                                     writer_.CurrentPath() + "'");
        }
        if (writer_.GetStats().files_opened != files_before) {
            log::info("[writer] rotating to {}", writer_.CurrentPath());
        }
    }

    WriterStats SbfWriter::stats() const noexcept {
        const auto &s = writer_.GetStats();
        return WriterStats{ s.bytes_written, s.records_written, s.files_opened };
    }
} // namespace asterx
