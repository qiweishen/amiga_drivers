#include "sbf_writer.h"

#include <stdexcept>
#include <system_error>
#include <utility>

#include "logger.h"
#include "time_util.h"


namespace asterx {
    namespace {
        constexpr std::string_view kModule = "AsteRx";
        common::DriverLog g_log{std::string(kModule)};

        common::RotatingFileWriter::Options MakeOptions(const SbfWriter::Config &cfg) {
            common::RotatingFileWriter::Options opts;
            // Fresh UTC stamp per file; seq is 0-based in the core, 1-based in
            // names. Capture by value: the lambda outlives this function
            opts.make_path = [dir = cfg.output_dir, prefix = cfg.file_prefix](std::uint32_t seq) {
                return (dir /
                        (prefix + "-" + common::TimeUtil::CompactUtcNow() + "-" +
                         std::to_string(seq + 1) + ".sbf"))
                        .string();
            };
            opts.max_file_bytes = cfg.rotate_bytes;
            opts.rotate_interval = cfg.rotate_interval;
            opts.precheck_size_cap = true; // rotate_bytes is a hard cap (RxTools < 2 GB)
            return opts;
        }
    } // namespace


    SbfWriter::SbfWriter(Config cfg) : cfg_(std::move(cfg)), writer_(MakeOptions(cfg_)) {
        std::error_code ec;
        std::filesystem::create_directories(cfg_.output_dir, ec);
        if (ec) {
            throw std::runtime_error("cannot create SBF output directory '" + cfg_.output_dir.string() +
                                     "': " + ec.message());
        }
    }


    SbfWriter::~SbfWriter() { close(); }


    bool SbfWriter::close() noexcept { return writer_.Close(); }


    bool SbfWriter::EndSegment() noexcept {
        if (writer_.IsOpen()) {
            g_log.Info("[Writer] Closing segment {} ({} bytes)", writer_.CurrentPath(), writer_.CurrentFileBytes());
        }
        return writer_.EndSegment();
    }


    bool SbfWriter::WriteBlock(const QByteArray &block) {
        if (block.isEmpty()) {
            return true;
        }

        const auto files_before = writer_.GetStats().files_opened;
        if (!writer_.Append(block.constData(), static_cast<std::size_t>(block.size()))) {
            if (!writer_.IsOpen()) {
                g_log.Error("[Writer] Cannot open SBF output file in '{}'", cfg_.output_dir.string());
            } else {
                g_log.Error("[Writer] Cannot write SBF block to '{}'", writer_.CurrentPath());
            }
            return false;
        }
        if (writer_.GetStats().files_opened != files_before) {
            g_log.Info("[Writer] Recording to {}", writer_.CurrentPath());
        }
        return true;
    }
} // namespace asterx
