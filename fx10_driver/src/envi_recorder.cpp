#include "../include/envi_recorder.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <system_error>


#include "logger.h"
#include "time_util.h"

namespace fx10 {
    namespace {
        Common::DriverLog g_log{"FX10"};

        // Ceiling on synthetic zero lines written for a single gap (pad_zero policy).
        // A cable glitch that loses 100k frames must not fabricate 40+ GB of zeros;
        // the remainder is only counted (frames_missed_rx) and warned in the log.
        constexpr std::uint64_t kMaxPadLinesPerGap = 10000;

        // An isolated wrong-size frame is a transient; a long run of them means the
        // camera configuration drifted from the recorder geometry — abort the recording
        // instead of silently dropping everything.
        constexpr std::uint32_t kMaxConsecutiveSizeMismatch = 25;


        std::string segmentBaseName(std::uint32_t index) {
            char buffer[32];
            std::snprintf(buffer, sizeof(buffer), "segment_%04u", index);
            return buffer;
        }


        bool fsyncDirectory(const std::filesystem::path &dir) {
            const int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            if (fd < 0) return false;
            const bool ok = ::fsync(fd) == 0;
            ::close(fd);
            return ok;
        }
    } // namespace


    std::filesystem::path createSessionDir(const std::filesystem::path &output_dir, const std::string &base_name,
                                           const std::string &utc_stamp) {
        std::error_code ec;
        std::filesystem::create_directories(output_dir, ec);
        if (ec) {
            throw RecorderError("cannot create output directory '" + output_dir.string() + "': " + ec.message());
        }
        const std::string stem = base_name + "_" + utc_stamp;
        for (int suffix = 0; suffix < 1000; ++suffix) {
            std::filesystem::path candidate = output_dir / (suffix == 0 ? stem : stem + "_" + std::to_string(suffix));
            ec.clear();
            if (std::filesystem::create_directory(candidate, ec)) {
                // Make the new directory entry durable in its parent.
                if (!fsyncDirectory(output_dir)) {
                    g_log.warn("[Writer] Cannot fsync output directory '{}'", output_dir.string());
                }
                return candidate;
            }
            if (ec && ec != std::errc::file_exists) {
                throw RecorderError("cannot create session directory '" + candidate.string() +
                                    "': " + ec.message());
            }
            // exists -> try the next suffix
        }
        throw RecorderError("cannot create session directory: 1000 name collisions under '" +
                            output_dir.string() + "'");
    }


    EnviRecorder::EnviRecorder(const RecordingConfig &config, Counters &counters)
        : config_(config), counters_(counters) {
    }


    EnviRecorder::~EnviRecorder() {
        try {
            stop();
        } catch (...) {
            // Destructor must not throw; stop() itself is designed not to.
        }
    }


    void EnviRecorder::start(const RecorderInit &init) {
        if (started_) {
            throw RecorderError("[Writer] Recorder already started");
        }
        if (init.samples == 0 || init.bands == 0) {
            throw RecorderError("[Writer] Recorder geometry must be non-zero (samples=" +
                                std::to_string(init.samples) + ", bands=" + std::to_string(init.bands) +
                                ")");
        }
        if (init.bytes_per_pixel != 1 && init.bytes_per_pixel != 2) {
            throw RecorderError("[Writer] Bytes_per_pixel must be 1 or 2");
        }
        if ((init.bytes_per_pixel == 1) != (init.data_type == EnviDataType::kUint8)) {
            throw RecorderError("[Writer] Bytes_per_pixel does not match ENVI data type");
        }
        if (!init.wavelengths.nm.empty() && init.wavelengths.nm.size() != init.bands) {
            throw RecorderError("[Writer] Wavelength count " + std::to_string(init.wavelengths.nm.size()) +
                                " does not match bands " + std::to_string(init.bands));
        }

        init_ = init;
        line_bytes_ = static_cast<std::size_t>(init.samples) * init.bands * init.bytes_per_pixel;
        if (config_.on_gap == GapPolicy::kPadZero) {
            zero_line_.assign(line_bytes_, 0);
        }

        session_dir_ = createSessionDir(config_.output_dir, config_.base_name,
                                        Common::TimeUtil::CompactUtc(Common::TimeUtil::RealtimeNowNs()));

        try {
            openSegment_();
        } catch (...) {
            // openSegment_ cleans up its own files, so the fresh session dir is empty.
            std::error_code ec;
            std::filesystem::remove(session_dir_, ec);
            throw;
        }
        // Only now is the recorder considered running;
        // a failed start leaves no half-initialized state
        started_ = true;
        stopped_ = false;
        g_log.info("[Writer] Recording to {}", session_dir_.string());
    }


    void EnviRecorder::openSegment_() {
        const std::uint32_t next_index = segment_index_ + 1;
        const std::string base = segmentBaseName(next_index);
        data_part_path_ = session_dir_ / (base + ".bil.part");

        data_fd_ = ::open(data_part_path_.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
        if (data_fd_ < 0) {
            const int err = errno;
            throw RecorderError("[Writer] Cannot create '" + data_part_path_.string() + "': " + std::strerror(err));
        }

        segment_index_ = next_index; // committed only after the open succeeded
        lines_this_segment_ = 0;
        data_bytes_this_segment_ = 0;
        segment_start_iso_ = Common::TimeUtil::Iso8601UtcSec(Common::TimeUtil::RealtimeNowNs());
    }


    void EnviRecorder::onFrame(const FrameView &frame) {
        if (!started_ || stopped_ || failed_) {
            return;
        }
        try {
            if (frame.size != line_bytes_ || frame.width != init_.samples || frame.height != init_.bands || frame.
                bytes_per_pixel != init_.bytes_per_pixel) {
                ++counters_.size_mismatch_drops;
                g_log.warn(
                    "[Writer] Dropping frame block_id={} with unexpected geometry {}x{}x{} ({} B, expected {} B)",
                    frame.block_id, frame.width, frame.height, frame.bytes_per_pixel, frame.size, line_bytes_);
                if (++consecutive_size_mismatch_ > kMaxConsecutiveSizeMismatch) {
                    latchError_("aborting: " + std::to_string(consecutive_size_mismatch_) +
                                " consecutive frames with wrong geometry (camera configuration drifted?)");
                }
                return;
            }
            consecutive_size_mismatch_ = 0;

            if (writeLine_(frame.data)) {
                ++counters_.frames_written;
                // Publish the ledger value rather than incrementing separately:
                // the two can never drift apart.
                frames_written_.store(counters_.frames_written, std::memory_order_relaxed);
                counters_.bytes_written += line_bytes_;
            }
        } catch (const std::exception &e) {
            latchError_(std::string("unexpected recorder exception: ") + e.what());
        } catch (...) {
            latchError_("unexpected recorder exception");
        }
    }


    void EnviRecorder::onGap(std::uint64_t first_missing_block_id, std::uint64_t missing_count) {
        if (!started_ || stopped_ || failed_ || missing_count == 0) {
            return;
        }
        try {
            ++counters_.blockid_gap_events;
            counters_.frames_missed_rx += missing_count;
            g_log.warn("[Writer] BlockID gap: {} frame(s) missing starting at block_id={}", missing_count,
                       first_missing_block_id);

            if (config_.on_gap != GapPolicy::kPadZero) {
                return; // gap recorded in the counters + log only
            }

            const std::uint64_t to_pad = std::min(missing_count, kMaxPadLinesPerGap);
            if (to_pad < missing_count) {
                // Beyond the cap the line index <-> trigger sequence alignment is lost
                // for this segment; the offline matcher must fall back to the log warns.
                g_log.warn("[Writer] Gap of {} exceeds pad cap {}; remainder NOT padded",
                           missing_count, kMaxPadLinesPerGap);
            }
            for (std::uint64_t i = 0; i < to_pad && !failed_; ++i) {
                if (writeLine_(zero_line_.data())) {
                    ++counters_.gap_lines_padded;
                    counters_.bytes_written += line_bytes_;
                }
            }
        } catch (const std::exception &e) {
            latchError_(std::string("unexpected recorder exception: ") + e.what());
        } catch (...) {
            latchError_("unexpected recorder exception");
        }
    }


    bool EnviRecorder::writeLine_(const std::uint8_t *data) {
        if (!writeAll_(data_fd_, data, line_bytes_)) {
            const int err = errno;
            ++counters_.write_errors;
            latchError_("write failed on '" + data_part_path_.string() + "': " + std::strerror(err));
            return false;
        }
        ++lines_this_segment_;
        ++global_line_index_;
        data_bytes_this_segment_ += line_bytes_;
        // The line itself is on disk; a failure inside rotation (reopening the next
        // segment) latches failed_ but must not un-count this line for the caller.
        rotateIfNeeded_();
        return true;
    }


    bool EnviRecorder::writeAll_(int fd, const void *data, std::size_t size) {
        const auto *p = static_cast<const std::uint8_t *>(data);
        std::size_t remaining = size;
        while (remaining > 0) {
            const ssize_t n = write_hook_ ? write_hook_(fd, p, remaining) : ::write(fd, p, remaining);
            if (n < 0) {
                if (errno == EINTR) continue;
                return false;
            }
            if (n == 0) {
                errno = EIO;
                return false;
            }
            p += n;
            remaining -= static_cast<std::size_t>(n);
        }
        return true;
    }


    bool EnviRecorder::writeFileAtomic_(const std::filesystem::path &part_path,
                                        const std::filesystem::path &final_path,
                                        const std::string &content) {
        const int fd = ::open(part_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
        if (fd < 0) {
            return false;
        }
        const bool written = writeAll_(fd, content.data(), content.size());
        const bool synced = written && ::fdatasync(fd) == 0;
        ::close(fd);
        if (!written || !synced) {
            ::unlink(part_path.c_str());
            return false;
        }
        std::error_code ec;
        std::filesystem::rename(part_path, final_path, ec);
        if (ec) {
            ::unlink(part_path.c_str());
            return false;
        }
        return true;
    }


    void EnviRecorder::rotateIfNeeded_() {
        const auto &rot = config_.rotation;
        const bool by_lines = rot.max_lines > 0 && lines_this_segment_ >= rot.max_lines;
        const bool by_bytes = rot.max_megabytes > 0 && data_bytes_this_segment_ >= rot.max_megabytes * 1024ull *
                              1024ull;
        if (!by_lines && !by_bytes) {
            return;
        }

        finalizeSegment_();
        if (failed_) {
            return; // finalize itself failed; do not open another segment
        }
        try {
            openSegment_();
        } catch (const RecorderError &e) {
            latchError_(std::string("rotation failed: ") + e.what());
        }
    }


    void EnviRecorder::removeEmptySegment_() {
        ::close(data_fd_);
        data_fd_ = -1;
        ::unlink(data_part_path_.c_str());
        --segment_index_; // the slot was never used
    }


    void EnviRecorder::finalizeSegment_() {
        if (data_fd_ < 0) return;

        if (lines_this_segment_ == 0) {
            removeEmptySegment_();
            return;
        }

        // Cut the file back to the last complete line on the error path, then make
        // the data durable
        bool io_ok = true;
        int io_err = 0;
        if (failed_) {
            if (::ftruncate(data_fd_, static_cast<off_t>(lines_this_segment_ * line_bytes_)) != 0) {
                io_err = errno;
                io_ok = false;
            }
        }
        if (io_ok && ::fdatasync(data_fd_) != 0) {
            io_err = errno;
            io_ok = false;
        }
        ::close(data_fd_);
        data_fd_ = -1;

        if (!io_ok) {
            ++counters_.write_errors;
            g_log.error("[Writer] Segment {} finalize sync/truncate failed ({}); leaving the .part file "
                        "(segment stays invalid)",
                        segment_index_, std::strerror(io_err));
            if (!failed_) {
                // set directly: latchError_ would recurse into finalizeSegment_
                failed_ = true;
                error_message_ = "finalize sync failed on segment " + std::to_string(segment_index_);
            }
            return;
        }

        const std::string base = segmentBaseName(segment_index_);
        const std::filesystem::path bil_path = session_dir_ / (base + ".bil");
        std::error_code ec;
        std::filesystem::rename(data_part_path_, bil_path, ec);
        if (ec) {
            ++counters_.write_errors;
            // Without a .hdr the segment is invalid regardless of file extensions.
            g_log.error("[Writer] Finalize rename failed for segment {}: {}", segment_index_, ec.message());
            return;
        }

        // Barrier: the data renames must be durable BEFORE the .hdr validity marker
        // can appear — otherwise a crash could persist the .hdr but not the renames.
        if (!fsyncDirectory(session_dir_)) {
            ++counters_.write_errors;
            g_log.error("[Writer] Cannot fsync session directory before publishing segment {} header", segment_index_);
            return; // segment stays without .hdr = invalid
        }

        // .hdr is written LAST (content fsynced before rename): presence marks validity.
        EnviHeaderInfo header;
        header.samples = init_.samples;
        header.lines = lines_this_segment_;
        header.bands = init_.bands;
        header.data_type = init_.data_type;
        // Producer tag only — the run's version/git SHA live in <data_folder>/drivers.json
        header.description =
                init_.description + "\nwavelength source: " +
                (init_.wavelengths.source_tag.empty() ? "none" : init_.wavelengths.source_tag) +
                "\ndriver: amiga_drivers" +
                "\nsegment status: " + (failed_ ? "partial-after-error" : "complete") +
                "\nrx frames missed so far: " + std::to_string(counters_.frames_missed_rx) + "\n";
        header.acquisition_time_iso = segment_start_iso_;
        header.wavelengths_nm = init_.wavelengths.nm;
        header.fwhm_nm = init_.wavelengths.fwhm;

        const std::filesystem::path hdr_path = session_dir_ / (base + ".hdr");
        const std::filesystem::path hdr_part = session_dir_ / (base + ".hdr.part");
        try {
            if (!writeFileAtomic_(hdr_part, hdr_path, generateEnviHeader(header))) {
                const int err = errno;
                ++counters_.write_errors;
                g_log.error("[Writer] Cannot write '{}': {}", hdr_path.string(), std::strerror(err));
                return;
            }
        } catch (const std::exception &e) {
            ++counters_.write_errors;
            g_log.error("[Writer] Cannot write '{}': {}", hdr_path.string(), e.what());
            return;
        }
        if (!fsyncDirectory(session_dir_)) {
            ++counters_.write_errors;
            g_log.warn("[Writer] Cannot fsync session directory after publishing segment {}", segment_index_);
        }

        ++counters_.segments_finalized;
    }


    void EnviRecorder::latchError_(const std::string &message) {
        if (failed_) {
            return;
        }
        failed_ = true;
        error_message_ = message;
        g_log.error("[Writer] Recorder failed: {} — finalizing partial data", message);
        finalizeSegment_();
    }


    void EnviRecorder::stop(const std::string &reason) {
        if (!started_ || stopped_) {
            return;
        }
        stopped_ = true;
        finalizeSegment_();
        g_log.info("[Writer] Session stopped ({}): status={} frames_written={} bytes_written={} segments={}",
                   reason.empty() ? "unspecified" : reason,
                   failed_ ? "FAILED" : toString(classify(counters_)),
                   counters_.frames_written, counters_.bytes_written, counters_.segments_finalized);
        // Ledger cross-check: every retrieved-OK frame is either written or dropped
        // for size mismatch. Only meaningful on non-failed runs — after a latch the
        // transport may deliver an unbounded number of frames that onFrame ignores
        // (until the sink-failed poll stops the loop), which is expected, not an
        // inconsistency; the run is already marked FAILED.
        if (!failed_ && counters_.retrieve_ok > 0 &&
            counters_.frames_written + counters_.size_mismatch_drops != counters_.retrieve_ok) {
            g_log.error("[Writer] Ledger inconsistency: retrieve_ok={} frames_written={} size_mismatch={}",
                        counters_.retrieve_ok,
                        counters_.frames_written,
                        counters_.size_mismatch_drops
            );
        }
    }
} // namespace fx10
