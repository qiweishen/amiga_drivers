#include "../include/envi_recorder.h"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <limits>
#include <string_view>
#include <system_error>
#include <utility>
#include <nlohmann/json.hpp>


#include "logger.h"
#include "time_util.h"

namespace fx10 {
    namespace {
        common::DriverLog g_log{"FX10"};

        // Ceiling on synthetic zero lines written for a single gap (pad_zero policy).
        // A cable glitch that loses 100k frames must not fabricate 40+ GB of zeros;
        // the full gap is always retained in the line index, including the unpadded remainder.
        constexpr std::uint64_t kMaxPadLinesPerGap = 10000;

        // An isolated wrong-size frame is a transient; a long run of them means the
        // camera configuration drifted from the recorder geometry — abort the recording
        // instead of silently dropping everything.
        constexpr std::uint32_t kMaxConsecutiveSizeMismatch = 25;


        std::string SegmentBaseName(std::uint32_t index) {
            char buffer[32];
            std::snprintf(buffer, sizeof(buffer), "segment_%04u", index);
            return buffer;
        }


        bool FsyncDirectory(const std::filesystem::path &dir) {
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
                if (!FsyncDirectory(output_dir)) {
                    g_log.Warn("[Writer] Cannot fsync output directory '{}'", output_dir.string());
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
            Stop();
        } catch (...) {
            // Destructor must not throw; Stop() itself is designed not to.
        }
    }


    void EnviRecorder::Start(const RecorderInit &init) {
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
        if (static_cast<std::size_t>(init.samples) >
            std::numeric_limits<std::size_t>::max() / init.bands / init.bytes_per_pixel) {
            throw RecorderError("[Writer] Line byte count overflows size_t");
        }
        line_bytes_ = static_cast<std::size_t>(init.samples) * init.bands * init.bytes_per_pixel;
        if (config_.on_gap == GapPolicy::kPadZero) {
            zero_line_.assign(line_bytes_, 0);
        }

        session_dir_ = createSessionDir(config_.output_dir, "fx10",
                                        common::TimeUtil::CompactUtc(common::TimeUtil::RealtimeNowNs()));

        const int full_scale = init.pixel_format.find("12") != std::string::npos ? 4095 :
                               init.pixel_format.find("10") != std::string::npos ? 1023 :
                               init.pixel_format == "Mono8" ? 255 : 0;
        const nlohmann::json capture = {
            {"format", "fx10-capture-v1"}, {"samples", init.samples}, {"bands", init.bands},
            {"bytes_per_pixel", init.bytes_per_pixel}, {"byte_order", "little"},
            {"pixel_format", init.pixel_format}, {"full_scale", full_scale},
            {"expected_frame_rate_hz", init.expected_frame_rate_hz},
            {"wavelengths_nm", init.wavelengths.nm}, {"wavelength_source", init.wavelengths.source_tag},
            {"description", init.description}, {"line_index_format", "fx10-line-index-v1"},
            {"trigger_association_verified", false}, {"leading_loss", "unknown"}
        };
        if (!WriteFileAtomic(session_dir_ / "capture.json.part", session_dir_ / "capture.json", capture.dump(2) + "\n") ||
            !FsyncDirectory(session_dir_)) {
            throw RecorderError("[Writer] Cannot publish capture geometry/identity metadata");
        }

        try {
            OpenSegment();
        } catch (...) {
            // OpenSegment cleans up its descriptors; retain capture.json as startup provenance.
            std::error_code ec;
            std::filesystem::remove(session_dir_, ec);
            throw;
        }
        // Only now is the recorder considered running;
        // a failed start leaves no half-initialized state
        started_ = true;
        stopped_ = false;
        g_log.Info("[Writer] Recording to {}", session_dir_.string());
    }


    void EnviRecorder::OpenSegment() {
        const std::uint32_t next_index = segment_index_ + 1;
        const std::string base = SegmentBaseName(next_index);
        data_part_path_ = session_dir_ / (base + ".bil.part");
        index_part_path_ = session_dir_ / (base + ".lines.csv.part");
        auto segment_start = common::TimeUtil::Iso8601UtcSec(common::TimeUtil::RealtimeNowNs());

        data_fd_ = ::open(data_part_path_.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
        if (data_fd_ < 0) {
            const int err = errno;
            throw RecorderError("[Writer] Cannot create '" + data_part_path_.string() + "': " + std::strerror(err));
        }

        index_fd_ = ::open(index_part_path_.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
        constexpr std::string_view columns =
            "# fx10-line-index-v1; zero-based indices; device timestamp unit/epoch unverified; "
            "leading loss unknown; directory identifies connection epoch\n"
            "event,segment_line,global_line,byte_offset,block_id,count,device_timestamp_raw,"
            "host_receive_realtime_ns,host_receive_monotonic_ns,block_id_anomaly\n";
        if (index_fd_ < 0 || !WriteAll(index_fd_, columns.data(), columns.size())) {
            const int err = errno;
            if (index_fd_ >= 0) {
                ::close(index_fd_);
                ::unlink(index_part_path_.c_str());
            }
            index_fd_ = -1;
            ::close(data_fd_);
            data_fd_ = -1;
            ::unlink(data_part_path_.c_str());
            throw RecorderError("[Writer] Cannot create line index: " + std::string(std::strerror(err)));
        }
        index_bytes_ = columns.size();
        index_records_ = 0;

        segment_index_ = next_index; // committed only after the open succeeded
        lines_this_segment_ = 0;
        data_bytes_this_segment_ = 0;
        last_flush_bytes_ = 0;
        segment_start_iso_ = std::move(segment_start);
    }


    void EnviRecorder::OnFrame(const FrameView &frame) {
        if (!started_ || stopped_ || failed_) {
            return;
        }
        try {
            if (frame.data == nullptr || frame.size != line_bytes_ || frame.width != init_.samples || frame.height != init_.bands || frame.
                bytes_per_pixel != init_.bytes_per_pixel) {
                ++counters_.size_mismatch_drops;
                OnRejected(frame.block_id, "layout-error");
                g_log.Warn(
                    "[Writer] Dropping frame block_id={} with unexpected geometry {}x{}x{} ({} B, expected {} B)",
                    frame.block_id, frame.width, frame.height, frame.bytes_per_pixel, frame.size, line_bytes_);
                if (++consecutive_size_mismatch_ > kMaxConsecutiveSizeMismatch) {
                    LatchError("aborting: " + std::to_string(consecutive_size_mismatch_) +
                                " consecutive frames with wrong geometry (camera configuration drifted?)",
                                ErrorKind::kGeometry);
                }
                return;
            }
            consecutive_size_mismatch_ = 0;

            if (WriteLine(frame.data, &frame)) {
                ++counters_.frames_written;
                // Publish the ledger value rather than incrementing separately:
                // the two can never drift apart.
                frames_written_.store(counters_.frames_written, std::memory_order_relaxed);
                counters_.bytes_written += line_bytes_;
            }
        } catch (const std::exception &e) {
            LatchError(std::string("unexpected recorder exception: ") + e.what(), ErrorKind::kOther);
        } catch (...) {
            LatchError("unexpected recorder exception", ErrorKind::kOther);
        }
    }


    void EnviRecorder::OnGap(std::uint64_t first_missing_block_id, std::uint64_t missing_count) {
        if (!started_ || stopped_ || failed_ || missing_count == 0) {
            return;
        }
        try {
            ++counters_.blockid_gap_events;
            counters_.frames_missed_rx += missing_count;
            g_log.Warn("[Writer] BlockID gap: {} frame(s) missing starting at block_id={}", missing_count,
                       first_missing_block_id);
            if (!WriteIndex("gap", first_missing_block_id, missing_count)) {
                return;
            }

            if (config_.on_gap != GapPolicy::kPadZero) {
                return; // full gap remains in the index, without adding synthetic BIL lines
            }

            const std::uint64_t to_pad = std::min(missing_count, kMaxPadLinesPerGap);
            if (to_pad < missing_count) {
                // The index records the entire gap, independent of the padding cap.
                g_log.Warn("[Writer] Gap of {} exceeds pad cap {}; remainder NOT padded",
                           missing_count, kMaxPadLinesPerGap);
            }
            for (std::uint64_t i = 0; i < to_pad && !failed_; ++i) {
                if (WriteLine(zero_line_.data())) {
                    ++counters_.gap_lines_padded;
                    counters_.bytes_written += line_bytes_;
                }
            }
        } catch (const std::exception &e) {
            LatchError(std::string("unexpected recorder exception: ") + e.what(), ErrorKind::kOther);
        } catch (...) {
            LatchError("unexpected recorder exception", ErrorKind::kOther);
        }
    }


    void EnviRecorder::OnRejected(std::uint64_t block_id, const char *reason) {
        if (!started_ || stopped_ || failed_) return;
        try {
            WriteIndex(reason, block_id, 1);
        } catch (const std::exception &e) {
            LatchError(std::string("line index exception: ") + e.what(), ErrorKind::kOther);
        }
    }

    bool EnviRecorder::WriteIndex(const char *event, std::uint64_t block_id, std::uint64_t count,
                                  const FrameView *frame) {
        const std::string row = fmt::format("{},{},{},{},{},{},{},{},{},{}\n", event, lines_this_segment_,
            global_line_index_, data_bytes_this_segment_, block_id, count,
            frame ? frame->device_timestamp_raw : 0,
            frame ? frame->host_receive_realtime_ns : 0,
            frame ? frame->host_receive_monotonic_ns : 0,
            frame && frame->block_id_anomaly ? 1 : 0);
        if (!WriteAll(index_fd_, row.data(), row.size())) {
            const int err = errno;
            ++counters_.write_errors;
            LatchError("line index write failed: " + std::string(std::strerror(err)), ErrorKind::kIo);
            return false;
        }
        index_bytes_ += row.size();
        ++index_records_;
        return true;
    }

    bool EnviRecorder::WriteLine(const std::uint8_t *data, const FrameView *frame) {
        if (!WriteAll(data_fd_, data, line_bytes_)) {
            const int err = errno;
            ++counters_.write_errors;
            LatchError("write failed on '" + data_part_path_.string() + "': " + std::strerror(err),
                        ErrorKind::kIo);
            return false;
        }
        // Commit only when both pixels and their identity reached their files.
        // A short index write causes finalize to truncate BOTH to the previous pair.
        if (!WriteIndex(frame ? "frame" : "padding", frame ? frame->block_id : 0, 1, frame)) {
            return false;
        }
        ++lines_this_segment_;
        ++global_line_index_;
        data_bytes_this_segment_ += line_bytes_;
        // Bound the loss on a power cut (a segment without its .hdr is invalid as a whole)
        const std::uint64_t flush_bytes = static_cast<std::uint64_t>(config_.flush_interval_mb) * 1024ull * 1024ull;
        if (flush_bytes > 0 && data_bytes_this_segment_ - last_flush_bytes_ >= flush_bytes) {
            last_flush_bytes_ = data_bytes_this_segment_;
            if (data_fd_ >= 0 && (Sync(data_fd_) != 0 || Sync(index_fd_) != 0)) {
                // Not fatal: the data is in the page cache and the next flush or
                // the segment finalize will try again.
                ++counters_.write_errors;
                g_log.Warn("[Writer] Periodic fdatasync failed on '{}': {}", data_part_path_.string(),
                           std::strerror(errno));
            }
        }
        // The line itself is on disk; a failure inside rotation (reopening the next
        // segment) latches failed_ but must not un-count this line for the caller.
        RotateIfNeeded();
        return true;
    }


    void EnviRecorder::LatchFinalizeFailure(const std::string &message) {
        if (failed_) {
            return; // keep the first cause
        }
        error_message_ = message;
        error_kind_ = ErrorKind::kIo;
        failed_.store(true, std::memory_order_release);
    }


    int EnviRecorder::Sync(int fd) {
        return sync_hook_ ? sync_hook_(fd) : ::fdatasync(fd);
    }


    bool EnviRecorder::WriteAll(int fd, const void *data, std::size_t size) {
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


    bool EnviRecorder::WriteFileAtomic(const std::filesystem::path &part_path,
                                        const std::filesystem::path &final_path,
                                        const std::string &content) {
        const int fd = ::open(part_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
        if (fd < 0) {
            return false;
        }
        const bool written = WriteAll(fd, content.data(), content.size());
        const bool synced = written && Sync(fd) == 0;
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


    void EnviRecorder::RotateIfNeeded() {
        const auto &rot = config_.rotation;
        const bool by_lines = rot.max_lines > 0 && lines_this_segment_ >= rot.max_lines;
        const bool by_bytes = rot.max_mb > 0 && data_bytes_this_segment_ >= rot.max_mb * 1024ull *
                              1024ull;
        if (!by_lines && !by_bytes) {
            return;
        }

        FinalizeSegment();
        if (failed_) {
            return; // finalize itself failed; do not open another segment
        }
        try {
            OpenSegment();
        } catch (const RecorderError &e) {
            LatchError(std::string("rotation failed: ") + e.what(), ErrorKind::kIo);
        }
    }


    void EnviRecorder::RemoveEmptySegment() {
        ::close(data_fd_);
        data_fd_ = -1;
        ::unlink(data_part_path_.c_str());
        ::close(index_fd_);
        index_fd_ = -1;
        ::unlink(index_part_path_.c_str());
        --segment_index_; // the slot was never used
    }


    void EnviRecorder::FinalizeSegment() {
        if (data_fd_ < 0) return;

        if (lines_this_segment_ == 0 && index_records_ == 0) {
            RemoveEmptySegment();
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
            if (::ftruncate(index_fd_, static_cast<off_t>(index_bytes_)) != 0) {
                io_err = errno;
                io_ok = false;
            }
        }
        if (io_ok && (Sync(data_fd_) != 0 || Sync(index_fd_) != 0)) {
            io_err = errno;
            io_ok = false;
        }
        ::close(data_fd_);
        data_fd_ = -1;
        ::close(index_fd_);
        index_fd_ = -1;

        if (!io_ok) {
            ++counters_.write_errors;
            g_log.Error("[Writer] Segment {} finalize sync/truncate failed ({}); leaving the .part file "
                        "(segment stays invalid)",
                        segment_index_, std::strerror(io_err));
            LatchFinalizeFailure("finalize sync failed on segment " + std::to_string(segment_index_));
            return;
        }

        const std::string base = SegmentBaseName(segment_index_);
        const std::filesystem::path bil_path = session_dir_ / (base + ".bil");
        std::error_code ec;
        std::filesystem::rename(data_part_path_, bil_path, ec);
        if (!ec) {
            std::filesystem::rename(index_part_path_, session_dir_ / (base + ".lines.csv"), ec);
        }
        if (ec) {
            ++counters_.write_errors;
            // Without a .hdr the segment is invalid regardless of file extensions.
            g_log.Error("[Writer] Finalize rename failed for segment {}: {}", segment_index_, ec.message());
            LatchFinalizeFailure("finalize rename failed for segment " + std::to_string(segment_index_) +
                                  ": " + ec.message());
            return;
        }

        // Barrier: the data renames must be durable BEFORE the .hdr validity marker
        // can appear — otherwise a crash could persist the .hdr but not the renames.
        if (!FsyncDirectory(session_dir_)) {
            ++counters_.write_errors;
            g_log.Error("[Writer] Cannot fsync session directory before publishing segment {} header", segment_index_);
            LatchFinalizeFailure("cannot fsync the session directory before publishing segment " +
                                  std::to_string(segment_index_));
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
                "\nline index format: fx10-line-index-v1" +
                "\nline index file: " + base + ".lines.csv" +
                "\nline association: UNVERIFIED; file line numbers are not trigger sequence numbers" +
                "\nsegment start clock: host file-open time; not an exposure timestamp" +
                "\nsegment status: " + (failed_ ? "partial-after-error" : "complete") +
                "\nrx frames missed so far: " + std::to_string(counters_.frames_missed_rx) + "\n";
        header.acquisition_time_iso = segment_start_iso_;
        header.wavelengths_nm = init_.wavelengths.nm;
        header.fwhm_nm = init_.wavelengths.fwhm;

        const std::filesystem::path hdr_path = session_dir_ / (base + ".hdr");
        const std::filesystem::path hdr_part = session_dir_ / (base + ".hdr.part");
        try {
            if (!WriteFileAtomic(hdr_part, hdr_path, GenerateEnviHeader(header))) {
                const int err = errno;
                ++counters_.write_errors;
                g_log.Error("[Writer] Cannot write '{}': {}", hdr_path.string(), std::strerror(err));
                // A segment without its .hdr is INVALID data, not a warning:
                // the run must end with the recorder-failure exit code rather
                // than reporting CLEAN over a segment nothing can read.
                LatchFinalizeFailure("cannot write '" + hdr_path.string() + "': " + std::strerror(err));
                return;
            }
        } catch (const std::exception &e) {
            ++counters_.write_errors;
            g_log.Error("[Writer] Cannot write '{}': {}", hdr_path.string(), e.what());
            LatchFinalizeFailure("cannot write '" + hdr_path.string() + "': " + e.what());
            return;
        }
        if (!FsyncDirectory(session_dir_)) {
            ++counters_.write_errors;
            g_log.Warn("[Writer] Cannot fsync session directory after publishing segment {}", segment_index_);
        }

        ++counters_.segments_finalized;
    }


    void EnviRecorder::LatchError(const std::string &message, ErrorKind kind) {
        if (failed_.load(std::memory_order_relaxed)) {
            return; // single writer during the streaming window; no CAS needed
        }
        // Publish the payload BEFORE the flag, so a monitor thread that sees
        // Failed() == true is guaranteed to see the matching message and kind.
        error_message_ = message;
        error_kind_ = kind;
        failed_.store(true, std::memory_order_release);
        g_log.Error("[Writer] Recorder failed: {} — finalizing partial data", message);
        FinalizeSegment();
    }


    void EnviRecorder::Stop(const std::string &reason) {
        if (!started_ || stopped_) {
            return;
        }
        stopped_ = true;
        FinalizeSegment();
        g_log.Info("[Writer] Session stopped ({}): status={} frames_written={} bytes_written={} segments={}",
                   reason.empty() ? "unspecified" : reason,
                   failed_ ? "FAILED" : ToString(Classify(counters_)),
                   counters_.frames_written, counters_.bytes_written, counters_.segments_finalized);
        // Ledger cross-check: every retrieved-OK frame is either written or dropped
        // for size mismatch. Only meaningful on non-failed runs — after a latch the
        // transport may deliver an unbounded number of frames that onFrame ignores
        // (until the sink-failed poll stops the loop), which is expected, not an
        // inconsistency; the run is already marked FAILED.
        if (!failed_ && counters_.retrieve_ok > 0 &&
            counters_.frames_written + counters_.size_mismatch_drops != counters_.retrieve_ok) {
            g_log.Error("[Writer] Ledger inconsistency: retrieve_ok={} frames_written={} size_mismatch={}",
                        counters_.retrieve_ok,
                        counters_.frames_written,
                        counters_.size_mismatch_drops
            );
        }
    }
} // namespace fx10
