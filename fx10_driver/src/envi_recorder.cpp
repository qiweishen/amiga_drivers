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
            {"description", init.description}, {"line_index_format", "fx10-line-index-v2"},
            {"recording_contract", RecordingContract(init.pixel_format)},
            {"trigger_association_verified", false}, {"leading_loss", "unknown"}
        };
        if (!WriteFileAtomic(session_dir_ / "capture.json.part", session_dir_ / "capture.json", capture.dump(2) + "\n") ||
            !FsyncDirectory(session_dir_)) {
            throw RecorderError("[Writer] Cannot publish capture geometry/identity metadata");
        }

        try {
            segments_.Open(session_dir_ / "segments.jsonl");
            OpenSegment();
        } catch (const MetadataError &e) {
            throw RecorderError(e.what());
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
        StartFlusher();
        g_log.Info("[Writer] Recording to {}", session_dir_.string());
    }


    // --- periodic durability helper, off the recording worker -------------------

    void EnviRecorder::StartFlusher() {
        if (config_.flush_interval_mb == 0 || flusher_.joinable()) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(flush_mu_);
            flusher_stop_ = false;
        }
        flusher_ = std::thread([this] { FlushLoop(); });
    }


    void EnviRecorder::StopFlusher() {
        if (!flusher_.joinable()) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(flush_mu_);
            flusher_stop_ = true;
        }
        flush_cv_.notify_all();
        flusher_.join(); // the loop drains what is queued before it exits
    }


    void EnviRecorder::DrainFlusher() {
        if (!flusher_.joinable()) {
            return;
        }
        std::unique_lock<std::mutex> lock(flush_mu_);
        flush_cv_.wait(lock, [this] { return !flush_pending_ && flush_in_progress_ == 0; });
    }


    void EnviRecorder::RequestFlush() {
        if (!flusher_.joinable()) {
            return; // flush_interval_mb == 0 never gets here; a missing thread means Start() did not run
        }
        std::lock_guard<std::mutex> lock(flush_mu_);
        // Rotation drains the helper before closing this segment. A request
        // still pending will sync this same file after all writes made so far;
        // coalesce it instead of allocating unbounded duplicate descriptors.
        if (flush_pending_) return;
        FlushRequest request{::dup(data_fd_), ::dup(index_fd_)};
        if (request.data_fd < 0 || request.index_fd < 0) {
            const int err = errno;
            if (request.data_fd >= 0) ::close(request.data_fd);
            if (request.index_fd >= 0) ::close(request.index_fd);
            flush_errors_.fetch_add(1, std::memory_order_relaxed);
            g_log.Error("[Writer] Cannot dup descriptors for the periodic flush: {}", std::strerror(err));
            return;
        }
        flush_pending_ = request;
        flush_cv_.notify_one();
    }


    void EnviRecorder::FoldFlushErrors() {
        const std::uint64_t total = flush_errors_.load(std::memory_order_acquire);
        if (total > flush_errors_folded_) {
            counters_.write_errors += total - flush_errors_folded_;
            flush_errors_folded_ = total;
            LatchFinalizeFailure("periodic data/index durability flush failed");
        }
    }


    void EnviRecorder::FlushLoop() {
        for (;;) {
            FlushRequest request{};
            {
                std::unique_lock<std::mutex> lock(flush_mu_);
                flush_cv_.wait(lock, [this] { return flusher_stop_ || flush_pending_.has_value(); });
                if (!flush_pending_) {
                    return; // stop requested and nothing left to sync
                }
                request = *flush_pending_;
                flush_pending_.reset();
                ++flush_in_progress_;
            }
            bool ok = true;
            int err = 0;
            if (Sync(request.data_fd) != 0) {
                ok = false;
                err = errno;
            }
            if (Sync(request.index_fd) != 0) {
                ok = false;
                err = errno;
            }
            ::close(request.data_fd);
            ::close(request.index_fd);
            if (!ok) {
                // Finalize still retries the barrier, but a failed durability
                // contract must stop acquisition and remain visible in the result.
                flush_errors_.fetch_add(1, std::memory_order_release);
                g_log.Error("[Writer] Periodic fdatasync failed: {}", std::strerror(err));
            }
            {
                std::lock_guard<std::mutex> lock(flush_mu_);
                --flush_in_progress_;
            }
            flush_cv_.notify_all(); // DrainFlusher waiters
        }
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
            "# fx10-line-index-v2; zero-based indices; device timestamp unit/epoch unverified; "
            "leading loss unknown; directory identifies connection epoch\n"
            "event,segment_line,global_line,byte_offset,block_id,count,device_timestamp_raw,"
            "host_receive_realtime_ns,host_receive_monotonic_ns,block_id_anomaly,"
            "sdk_acquired_size,sdk_payload_type,sdk_operation_result,sdk_chunk_count,sdk_image_present,"
            "sdk_pixel_type,sdk_width,sdk_height,sdk_padding_x,sdk_padding_y,sdk_image_size,sdk_effective_image_size\n";
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
        segment_global_first_ = global_line_index_;
        segment_frames_ = segment_padding_ = segment_missing_ = segment_rejected_ = segment_anomalies_ = 0;
        segment_first_frame_.reset();
        segment_last_frame_.reset();
    }


    void EnviRecorder::OnFrame(const FrameView &frame) {
        if (!started_ || stopped_ || Failed()) {
            return;
        }
        try {
            if (frame.data == nullptr || frame.size != line_bytes_ || frame.width != init_.samples || frame.height != init_.bands || frame.
                bytes_per_pixel != init_.bytes_per_pixel) {
                ++counters_.size_mismatch_drops;
                loss_seen_.store(true, std::memory_order_release);
                OnRejected(frame.block_id, "layout-error", &frame);
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
        if (!started_ || stopped_ || Failed() || missing_count == 0) {
            return;
        }
        try {
            ++counters_.blockid_gap_events;
            counters_.frames_missed_rx += missing_count;
            loss_seen_.store(true, std::memory_order_release);
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
            for (std::uint64_t i = 0; i < to_pad && !Failed(); ++i) {
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


    void EnviRecorder::OnRejected(std::uint64_t block_id, const char *reason, const FrameView *frame) {
        if (!started_ || stopped_ || Failed()) return;
        loss_seen_.store(true, std::memory_order_release);
        try {
            WriteIndex(reason, block_id, 1, frame);
        } catch (const std::exception &e) {
            LatchError(std::string("line index exception: ") + e.what(), ErrorKind::kOther);
        }
    }

    bool EnviRecorder::WriteIndex(const char *event, std::uint64_t block_id, std::uint64_t count,
                                  const FrameView *frame) {
        std::string row = fmt::format("{},{},{},{},{},{},{},{},{},{}", event, lines_this_segment_,
            global_line_index_, data_bytes_this_segment_, block_id, count,
            frame ? frame->device_timestamp_raw : 0,
            frame ? frame->host_receive_realtime_ns : 0,
            frame ? frame->host_receive_monotonic_ns : 0,
            frame && frame->block_id_anomaly ? 1 : 0);
        if (frame && frame->sdk) {
            const auto &sdk = *frame->sdk;
            row += fmt::format(",{},{},{},{},{}", sdk.acquired_size, sdk.payload_type,
                               sdk.operation_result, sdk.chunk_count, sdk.image_present ? 1 : 0);
            if (sdk.image_present) {
                row += fmt::format(",{},{},{},{},{},{},{}", sdk.pixel_type, sdk.width, sdk.height,
                    sdk.padding_x, sdk.padding_y, sdk.image_size, sdk.effective_image_size);
            } else row += ",,,,,,,"; // seven unknown image fields
        } else row += ",,,,,,,,,,,,"; // twelve unknown SDK fields (gap, padding, SDK-free inputs)
        row += '\n';
        if (!WriteAll(index_fd_, row.data(), row.size())) {
            const int err = errno;
            ++counters_.write_errors;
            LatchError("line index write failed: " + std::string(std::strerror(err)), ErrorKind::kIo);
            return false;
        }
        index_bytes_ += row.size();
        ++index_records_;
        const std::string_view kind(event);
        if (kind == "gap") segment_missing_ += count;
        else if (kind != "frame" && kind != "padding") segment_rejected_ += count;
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
        if (frame) {
            const FrameStamp stamp{global_line_index_, frame->block_id, frame->device_timestamp_raw,
                frame->host_receive_realtime_ns, frame->host_receive_monotonic_ns};
            if (!segment_first_frame_) segment_first_frame_ = stamp;
            segment_last_frame_ = stamp;
            ++segment_frames_;
            if (frame->block_id_anomaly) ++segment_anomalies_;
        } else ++segment_padding_;
        ++lines_this_segment_;
        ++global_line_index_;
        data_bytes_this_segment_ += line_bytes_;
        // Request durability of data and identity together. The helper can
        // coalesce pending requests; this byte cadence is not a time bound or
        // a guarantee of crash recovery for an unfinished segment.
        const std::uint64_t flush_bytes = static_cast<std::uint64_t>(config_.flush_interval_mb) * 1024ull * 1024ull;
        if (flush_bytes > 0 && data_bytes_this_segment_ - last_flush_bytes_ >= flush_bytes) {
            last_flush_bytes_ = data_bytes_this_segment_;
            if (data_fd_ >= 0) {
                RequestFlush();
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
        int result;
        do { result = sync_hook_ ? sync_hook_(fd) : ::fdatasync(fd); } while (result != 0 && errno == EINTR);
        return result;
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

        // Every queued periodic flush of this segment completes before the final
        // one, and its failures reach the ledger here (single-writer rule).
        DrainFlusher();
        FoldFlushErrors();

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
                "\nline index format: fx10-line-index-v2" +
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
        const bool directory_synced = FsyncDirectory(session_dir_);
        if (!directory_synced) {
            ++counters_.write_errors;
            g_log.Warn("[Writer] Cannot fsync session directory after publishing segment {}", segment_index_);
        }

        ++counters_.segments_finalized;
        try {
            const auto stamp_json = [](const std::optional<FrameStamp> &v) -> nlohmann::json {
                if (!v) return nullptr;
                return {{"global_line", v->global_line}, {"block_id", v->block_id},
                    {"device_timestamp_raw", v->device_timestamp_raw}, {"hrt", v->hrt}, {"hmn", v->hmn}};
            };
            segments_.Append({{"format", "fx10-segment-v1"}, {"segment", base},
                {"data", base + ".bil"}, {"index", base + ".lines.csv"}, {"header", base + ".hdr"},
                {"lines", lines_this_segment_}, {"frames", segment_frames_}, {"padding_lines", segment_padding_},
                {"bytes", data_bytes_this_segment_}, {"index_bytes", index_bytes_}, {"index_records", index_records_},
                {"global_line_first", segment_global_first_}, {"global_line_end_exclusive", global_line_index_},
                {"first_frame", stamp_json(segment_first_frame_)}, {"last_frame", stamp_json(segment_last_frame_)},
                {"gap_frames_reported", segment_missing_}, {"rejected_buffers", segment_rejected_},
                {"block_id_anomalies", segment_anomalies_},
                {"closed_clean", !failed_.load() && directory_synced},
                {"closed_clean_semantics", "segment finalization only; does not imply no acquisition loss"},
                {"header_published", true}, {"closed_host_realtime_ns", common::TimeUtil::RealtimeNowNs()}});
        } catch (const std::exception &e) {
            ++counters_.write_errors;
            LatchFinalizeFailure("segment summary failed: " + std::string(e.what()));
        }
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
        StopFlusher();
        try { segments_.Close(); }
        catch (const MetadataError &e) {
            ++counters_.write_errors;
            LatchFinalizeFailure(e.what());
        }
        g_log.Info("[Writer] Session stopped ({}): status={} frames_written={} bytes_written={} segments={}",
                   reason.empty() ? "unspecified" : reason,
                   failed_ ? "FAILED" : ToString(Classify(counters_)),
                   counters_.frames_written, counters_.bytes_written, counters_.segments_finalized);
        // Whole-ledger reads are safe here: receiver.Stop joined both workers.
        // After a worker failure the affected frame may have been partly or
        // fully written; unconfirmed is deliberately not an additive drop count.
        if (!failed_ && counters_.recording_worker_unconfirmed == 0 && counters_.retrieve_ok > 0 &&
            counters_.frames_written + counters_.size_mismatch_drops + counters_.recording_queue_drops != counters_.retrieve_ok) {
            g_log.Error("[Writer] Ledger inconsistency: retrieve_ok={} frames_written={} size_mismatch={} "
                        "queue_drops={}",
                        counters_.retrieve_ok,
                        counters_.frames_written,
                        counters_.size_mismatch_drops, counters_.recording_queue_drops
            );
        }
    }
} // namespace fx10
