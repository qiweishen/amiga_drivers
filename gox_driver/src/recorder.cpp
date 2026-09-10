#include "recorder.h"

#include <algorithm>
#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

#include "format.h"
#include "logger.h"
#include "time_util.h"
#include "util.h"

namespace gox {
    namespace {
        common::DriverLog g_log{"GoX"};

        constexpr size_t kIdxBufFlushBytes = 64 * 1024;
        constexpr uint64_t kIdxFlushFrames = 100;
        constexpr uint64_t kIdxFlushIntervalNs = 1'000'000'000ull; // 1 s

        // Shared page of zeros for alignment padding (record_align <= 4096 by far
        // the common case; larger pads loop).
        const uint8_t kZeros[4096] = {};


        [[noreturn]] void throw_errno(const std::string &what) {
            throw IoError(what + ": " + std::strerror(errno));
        }

        void SyncData(int fd, const char *what) {
            int result;
            do { result = ::fdatasync(fd); } while (result != 0 && errno == EINTR);
            if (result != 0) throw_errno(what);
        }

        void WriteAll(int fd, const void *data, size_t size, const char *what,
                      const Recorder::IndexWriteHook &hook = {}) {
            const auto *p = static_cast<const uint8_t *>(data);
            while (size > 0) {
                ssize_t n = hook ? hook(fd, p, size) : ::write(fd, p, size);
                if (n < 0) {
                    if (errno == EINTR) {
                        continue;
                    }
                    throw_errno(what);
                }
                if (n == 0) {
                    errno = EIO;
                    throw_errno(what);
                }
                p += n;
                size -= static_cast<size_t>(n);
            }
        }
    } // namespace


    Recorder::Recorder(RecorderOptions opts, CameraStats *stats) : opts_(std::move(opts)), stats_(stats) {
    }


    Recorder::~Recorder() {
        try {
            close();
        } catch (const std::exception &e) {
            g_log.Error("[{}] [Writer] close failed in destructor: {}", opts_.camera_id, e.what());
        }
    }


    void Recorder::Open() {
        if (opened_) {
            return;
        }
        if (::mkdir(opts_.camera_dir.c_str(), 0755) != 0) {
            throw_errno("mkdir " + opts_.camera_dir);
        }
        dir_fd_ = ::open(opts_.camera_dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (dir_fd_ < 0) {
            throw_errno("open dir " + opts_.camera_dir);
        }
        std::string segments_path = opts_.camera_dir + "/segments.jsonl";
        segments_fd_ = ::open(segments_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
        if (segments_fd_ < 0) {
            const int saved = errno;
            ::close(dir_fd_);
            dir_fd_ = -1;
            errno = saved;
            throw_errno("open " + segments_path);
        }
        opened_ = true;
        OpenSegment();
    }


    void Recorder::OpenSegment() {
        ++segment_index_;
        char name[32];
        snprintf(name, sizeof(name), "seg_%05u.raw", segment_index_);
        std::string seg_path = opts_.camera_dir + "/" + name;
        seg_fd_ = ::open(seg_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
        if (seg_fd_ < 0) {
            throw_errno("open " + seg_path);
        }

        // Best-effort preallocation: one extent, no runtime block allocation
        // stalls, ENOSPC surfaces here rather than mid-frame. KEEP_SIZE keeps
        // st_size == bytes written, so a crashed file needs no truncation.
        if (::fallocate(seg_fd_, FALLOC_FL_KEEP_SIZE, 0, static_cast<off_t>(opts_.segment_max_bytes)) != 0) {
            if (errno == ENOSPC) {
                throw_errno("fallocate " + seg_path);
            }
            g_log.Warn("[{}] [Writer] fallocate unsupported on this filesystem ({}); continuing without preallocation",
                       opts_.camera_id, std::strerror(errno));
        }

        // Use the existing v1 flag/field: payload bytes and header layout stay
        // unchanged, while readers can detect damage inside a complete record.
        format::FileHeader fh =
                format::make_file_header(segment_index_, common::TimeUtil::RealtimeNowNs(), opts_.session_uuid, opts_.camera_id.c_str(),
                                         opts_.camera_serial.c_str(), opts_.record_align, format::kSegFlagPayloadCrc);
        WriteAll(seg_fd_, &fh, sizeof(fh), "write file header");
        if (::fdatasync(seg_fd_) != 0) {
            throw_errno("fdatasync file header");
        }
        seg_offset_ = sizeof(fh);

        // Pad so the first record starts on a record_align boundary — every
        // record offset in the file is then a multiple of record_align
        // (mmap/O_DIRECT friendly; format.hpp layout contract).
        const uint64_t first_record = format::AlignUp(seg_offset_, opts_.record_align);
        uint64_t pad_left = first_record - seg_offset_;
        while (pad_left > 0) {
            size_t n = static_cast<size_t>(std::min<uint64_t>(pad_left, sizeof(kZeros)));
            WriteAll(seg_fd_, kZeros, n, "write header padding");
            pad_left -= n;
        }
        seg_offset_ = first_record;
        last_synced_off_ = seg_offset_;

        char idx_name[40];
        snprintf(idx_name, sizeof(idx_name), "seg_%05u.idx.jsonl", segment_index_);
        std::string idx_path = opts_.camera_dir + "/" + idx_name;
        idx_fd_ = ::open(idx_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
        if (idx_fd_ < 0) {
            throw_errno("open " + idx_path);
        }
        idx_buf_.clear();
        idx_committed_bytes_ = 0;
        idx_write_failed_ = false;
        idx_buf_.reserve(kIdxBufFlushBytes + 512);
        idx_frames_pending_ = 0;
        idx_last_flush_mono_ns_ = common::TimeUtil::MonotonicNowNs();

        // Persist the directory entries of the new files.
        if (::fsync(dir_fd_) != 0) {
            throw_errno("fsync dir");
        }

        seg_frames_ = 0;
        if (stats_) {
            stats_->segments_created.fetch_add(1, std::memory_order_relaxed);
        }
        g_log.Trace("[{}] [Writer] opened segment {}", opts_.camera_id, seg_path);
    }


    void Recorder::WriteFrame(const FrameMeta &meta, const uint8_t *data, size_t size) {
        if (!opened_ || closed_) {
            throw IoError("recorder not open");
        }
        if (size != 0 && data == nullptr) {
            throw IoError("non-empty frame has no payload pointer");
        }

        const uint64_t record_bytes = format::AlignUp(format::kFrameHeaderSize + size, opts_.record_align);

        // Rotate when this record would overflow the segment (never split a
        // record). A single record larger than the segment size still goes into
        // its own fresh segment.
        if (seg_frames_ > 0 && seg_offset_ + record_bytes > opts_.segment_max_bytes) {
            CloseSegment(true);
            OpenSegment();
        }

        format::FrameHeader h{};
        h.frame_magic = format::kFrameMagic;
        h.header_size = format::kFrameHeaderSize;
        h.block_id = meta.block_id;
        h.device_ts_ns = meta.device_ts_ns;
        h.host_realtime_ns = meta.host_realtime_ns;
        h.host_monotonic_ns = meta.host_monotonic_ns;
        h.pixel_format = meta.pixel_format;
        h.width = meta.width;
        h.height = meta.height;
        h.offset_x = meta.offset_x;
        h.offset_y = meta.offset_y;
        h.status_flags = meta.status_flags;
        h.payload_size = size;
        h.frame_seq = frame_seq_;
        h.payload_crc32c = format::Crc32c(data, size);
        format::SealFrameHeader(h);

        const uint64_t record_offset = seg_offset_;
        const size_t pad = static_cast<size_t>(record_bytes - format::kFrameHeaderSize - size);

        struct iovec iov[3];
        iov[0].iov_base = &h;
        iov[0].iov_len = sizeof(h);
        iov[1].iov_base = const_cast<uint8_t *>(data);
        iov[1].iov_len = size;
        int iovcnt = 2;
        size_t total = sizeof(h) + size;
        if (pad > 0 && pad <= sizeof(kZeros)) {
            iov[2].iov_base = const_cast<uint8_t *>(kZeros);
            iov[2].iov_len = pad;
            iovcnt = 3;
            total += pad;
        }
        WriteIovAll(iov, iovcnt, total);
        if (pad > sizeof(kZeros)) {
            size_t left = pad;
            while (left > 0) {
                size_t n = std::min(left, sizeof(kZeros));
                WriteAll(seg_fd_, kZeros, n, "write padding");
                left -= n;
            }
        }
        seg_offset_ += record_bytes;

        // Index line. Written after the payload so the index is always a subset
        // of the data. SDK-only fields supplement the fixed version-1 raw header.
        char line[1024];
        int n = snprintf(line, sizeof(line),
                         "{\"seq\":%" PRIu64 ",\"bid\":%" PRIu64 ",\"dts\":%" PRIu64 ",\"hrt\":%" PRIu64 ",\"hmn\":%"
                         PRIu64
                         ",\"off\":%" PRIu64
                         ",\"psz\":%zu,\"pf\":%u,\"w\":%u,\"h\":%u,"
                         "\"fl\":%u,\"ox\":%u,\"oy\":%u,\"payload_type\":%u,"
                         "\"padding_x\":%u,\"padding_y\":%u,\"chunk_count\":%u,\"operation_result\":%u}\n",
                         frame_seq_, meta.block_id, meta.device_ts_ns, meta.host_realtime_ns, meta.host_monotonic_ns,
                         record_offset,
                         size, meta.pixel_format, meta.width, meta.height, meta.status_flags,
                         meta.offset_x, meta.offset_y, meta.payload_type, meta.padding_x, meta.padding_y,
                         meta.chunk_count, meta.operation_result);
        if (n > 0 && n < static_cast<int>(sizeof(line))) {
            idx_buf_.append(line, static_cast<size_t>(n));
        } else {
            throw IoError("frame index serialization failed");
        }
        ++idx_frames_pending_;
        FlushIndex(false);

        if (seg_frames_ == 0) {
            seg_seq_first_ = frame_seq_;
            seg_bid_first_ = meta.block_id;
            seg_dts_first_ = meta.device_ts_ns;
        }
        seg_seq_last_ = frame_seq_;
        seg_bid_last_ = meta.block_id;
        seg_dts_last_ = meta.device_ts_ns;
        ++seg_frames_;
        ++frame_seq_;

        if (stats_) {
            stats_->frames_written.fetch_add(1, std::memory_order_relaxed);
            stats_->bytes_written.fetch_add(record_bytes, std::memory_order_relaxed);
        }

        // A writeback hint is not a durability barrier. The dedicated writer
        // syncs data first, then the full index (including SDK-only metadata).
        // A stalled barrier consumes the bounded acquisition queue; I/O errors
        // propagate as IoError and stop the session rather than being ignored.
        if (opts_.flush_interval_bytes != 0 &&
            seg_offset_ - last_synced_off_ >= opts_.flush_interval_bytes) {
            SyncData(seg_fd_, "fdatasync periodic segment");
            FlushIndex(true);
            SyncData(idx_fd_, "fdatasync periodic index");
            last_synced_off_ = seg_offset_;
        }
    }


    void Recorder::WriteIovAll(const struct iovec *iov, int iovcnt, size_t total) const {
        struct iovec local[3];
        for (int i = 0; i < iovcnt; ++i) {
            local[i] = iov[i];
        }
        struct iovec *cur = local;
        while (total > 0) {
            ssize_t n = ::writev(seg_fd_, cur, iovcnt);
            if (n == 0) {
                errno = EIO;
                throw_errno("writev made no progress");
            }
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                throw_errno("writev frame record");
            }
            total -= static_cast<size_t>(n);
            // Advance the iovec array past the bytes written.
            size_t written = static_cast<size_t>(n);
            while (written > 0 && iovcnt > 0) {
                if (written >= cur->iov_len) {
                    written -= cur->iov_len;
                    ++cur;
                    --iovcnt;
                } else {
                    cur->iov_base = static_cast<uint8_t *>(cur->iov_base) + written;
                    cur->iov_len -= written;
                    written = 0;
                }
            }
        }
    }


    void Recorder::FlushIndex(const bool force) {
        bool due = force || idx_buf_.size() >= kIdxBufFlushBytes || idx_frames_pending_ >= kIdxFlushFrames;
        if (!due) {
            uint64_t now = common::TimeUtil::MonotonicNowNs();
            due = idx_frames_pending_ > 0 && now - idx_last_flush_mono_ns_ >= kIdxFlushIntervalNs;
        }
        if (!due || idx_buf_.empty()) {
            if (due) {
                idx_last_flush_mono_ns_ = common::TimeUtil::MonotonicNowNs();
            }
            return;
        }
        if (idx_write_failed_) throw IoError("index previously failed; refusing to append a duplicate partial batch");
        try {
            WriteAll(idx_fd_, idx_buf_.data(), idx_buf_.size(), "write index", index_write_hook_);
        } catch (...) {
            idx_write_failed_ = true;
            throw;
        }
        idx_committed_bytes_ += idx_buf_.size();
        idx_buf_.clear();
        idx_frames_pending_ = 0;
        idx_last_flush_mono_ns_ = common::TimeUtil::MonotonicNowNs();
    }


    void Recorder::AppendSegmentSummary(bool clean) const {
        char name[32];
        snprintf(name, sizeof(name), "seg_%05u.raw", segment_index_);
        char line[512];
        int n = snprintf(line, sizeof(line),
                         "{\"seg\":\"%s\",\"frames\":%" PRIu64 ",\"bytes\":%" PRIu64 ",\"seq_first\":%" PRIu64
                         ",\"seq_last\":%" PRIu64
                         ",\"bid_first\":%" PRIu64 ",\"bid_last\":%" PRIu64 ",\"dts_first\":%" PRIu64 ",\"dts_last\":%"
                         PRIu64
                         ",\"closed_clean\":%s}\n",
                         name, seg_frames_, seg_offset_, seg_seq_first_, seg_seq_last_, seg_bid_first_, seg_bid_last_,
                         seg_dts_first_,
                         seg_dts_last_, clean ? "true" : "false");
        if (n > 0 && n < static_cast<int>(sizeof(line))) {
            WriteAll(segments_fd_, line, static_cast<size_t>(n), "write segments.jsonl");
        } else {
            throw IoError("segment summary serialization failed");
        }
    }


    void Recorder::CloseSegment(bool clean) {
        if (seg_fd_ < 0) {
            return;
        }
        if (idx_write_failed_) {
            // The failed batch may contain a partial JSON line. Never append it
            // again at the partial offset during error cleanup; retain a valid prefix.
            if (::ftruncate(idx_fd_, static_cast<off_t>(idx_committed_bytes_)) != 0) {
                throw_errno("truncate failed index batch");
            }
            idx_buf_.clear();
            clean = false;
        } else {
            FlushIndex(true);
        }
        // A partial frame must be removed before the final durability barrier.
        if (::ftruncate(seg_fd_, static_cast<off_t>(seg_offset_)) != 0) {
            throw_errno("truncate segment");
        }
        SyncData(seg_fd_, "fdatasync segment");
        SyncData(idx_fd_, "fdatasync index");
        const int idx_closed = ::close(idx_fd_);
        idx_fd_ = -1;
        if (idx_closed != 0) throw_errno("close index");
        const int seg_closed = ::close(seg_fd_);
        seg_fd_ = -1;
        if (seg_closed != 0) throw_errno("close segment");
        AppendSegmentSummary(clean);
        SyncData(segments_fd_, "fdatasync segments.jsonl");
    }


    void Recorder::close(bool clean) {
        if (!opened_ || closed_) {
            return;
        }
        closed_ = true;
        const auto release_fds = [this] {
            for (int *fd: {&idx_fd_, &seg_fd_, &segments_fd_}) {
                if (*fd >= 0) {
                    ::close(*fd);
                    *fd = -1;
                }
            }
            if (dir_fd_ >= 0) {
                (void) ::fsync(dir_fd_);
                ::close(dir_fd_);
                dir_fd_ = -1;
            }
        };
        try {
            CloseSegment(clean);
        } catch (...) {
            release_fds();
            throw;
        }
        release_fds();
    }
} // namespace gox
