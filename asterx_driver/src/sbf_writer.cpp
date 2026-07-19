// SPDX-License-Identifier: BSD-3-Clause
#include "sbf_writer.hpp"

#include <cerrno>
#include <cstring>
#include <ctime>
#include <stdexcept>
#include <system_error>
#include <utility>

#include "asterx_log.hpp"

namespace asterx {
    namespace {
        std::string utc_timestamp_now() {
            const auto t = std::chrono::system_clock::now();
            const auto tt = std::chrono::system_clock::to_time_t(t);
            std::tm tm{};
            gmtime_r(&tt, &tm);
            char buf[32];
            std::strftime(buf, sizeof(buf), "%Y%m%dT%H%M%SZ", &tm);
            return std::string(buf);
        }
    } // namespace

    SbfWriter::SbfWriter(Config cfg) : cfg_(std::move(cfg)) {
        std::error_code ec;
        std::filesystem::create_directories(cfg_.output_dir, ec);
        if (ec) {
            throw std::runtime_error("cannot create output directory '" +
                                     cfg_.output_dir.string() + "': " + ec.message());
        }
    }

    SbfWriter::~SbfWriter() { close(); }

    void SbfWriter::close() noexcept {
        if (fp_) {
            std::fflush(fp_);
            std::fclose(fp_);
            fp_ = nullptr;
            current_size_ = 0;
        }
    }

    void SbfWriter::end_segment() noexcept {
        if (fp_) {
            log::info("[writer] closing segment {} ({} bytes)",
                         current_path_.string(), current_size_);
            close();
        }
    }

    void SbfWriter::open_new_file_() {
        if (fp_) {
            std::fflush(fp_);
            std::fclose(fp_);
            fp_ = nullptr;
        }
        ++seq_;
        ++stats_.files_opened;
        current_path_ = cfg_.output_dir /
                        (cfg_.file_prefix + "-" + utc_timestamp_now() + "-" +
                         std::to_string(seq_) + ".sbf");
        fp_ = std::fopen(current_path_.string().c_str(), "wb");
        if (!fp_) {
            throw std::runtime_error("cannot open output file '" +
                                     current_path_.string() +
                                     "': " + std::strerror(errno));
        }
        current_size_ = 0;
        file_start_ = std::chrono::steady_clock::now();
        log::info("[writer] opened {}", current_path_.string());
    }

    void SbfWriter::rotate_if_needed_() {
        if (!fp_) return;
        const auto now = std::chrono::steady_clock::now();
        const bool by_size = current_size_ >= cfg_.rotate_bytes;
        const bool by_time = (now - file_start_) >= cfg_.rotate_interval;
        if (by_size || by_time) {
            log::info("[writer] rotating ({} bytes, {} s)",
                         current_size_,
                         std::chrono::duration_cast<std::chrono::seconds>(now - file_start_).count());
            open_new_file_();
        }
    }

    void SbfWriter::write_block(const QByteArray &block) {
        if (block.isEmpty()) return;
        if (!fp_) open_new_file_();

        const auto n = static_cast<std::size_t>(block.size());

        // Rotate BEFORE a write that would push the file past rotate_bytes, so
        // rotate_bytes is a hard segment-size cap (the RxTools converters refuse
        // files of 2 GB or larger).
        if (current_size_ > 0 && current_size_ + n > cfg_.rotate_bytes) {
            log::info("[writer] rotating at size cap ({} bytes)", current_size_);
            open_new_file_();
        }

        if (std::fwrite(block.constData(), 1, n, fp_) != n) {
            throw std::runtime_error("fwrite(sbf block) failed");
        }
        current_size_ += n;
        stats_.bytes_written += n;
        ++stats_.blocks_written;

        rotate_if_needed_();
    }
} // namespace asterx
