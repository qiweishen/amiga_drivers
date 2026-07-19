#include "core/recorder.hpp"

#include <algorithm>
#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/uio.h>
#include <unistd.h>

#include "core/format.hpp"
#include "core/logger.hpp"
#include "core/util.hpp"

namespace jai {

	namespace {

		constexpr size_t kIdxBufFlushBytes = 64 * 1024;
		constexpr uint64_t kIdxFlushFrames = 100;
		constexpr uint64_t kIdxFlushIntervalNs = 1'000'000'000ull;	// 1 s

		// Shared page of zeros for alignment padding (record_align <= 4096 by far
		// the common case; larger pads loop).
		const uint8_t kZeros[4096] = {};

		[[noreturn]] void throw_errno(const std::string &what) {
			throw IoError(what + ": " + std::strerror(errno));
		}

		void write_all(int fd, const void *data, size_t size, const char *what) {
			const uint8_t *p = static_cast<const uint8_t *>(data);
			while (size > 0) {
				ssize_t n = ::write(fd, p, size);
				if (n < 0) {
					if (errno == EINTR) {
						continue;
					}
					throw_errno(what);
				}
				p += n;
				size -= static_cast<size_t>(n);
			}
		}

	}  // namespace

	Recorder::Recorder(RecorderOptions opts, CameraStats *stats) : opts_(std::move(opts)), stats_(stats) {}

	Recorder::~Recorder() {
		try {
			close();
		} catch (const std::exception &e) {
			LOG_ERROR("recorder(", opts_.camera_id, "): close failed in destructor: ", e.what());
		}
	}

	void Recorder::open() {
		if (opened_) {
			return;
		}
		if (::mkdir(opts_.camera_dir.c_str(), 0755) != 0 && errno != EEXIST) {
			throw_errno("mkdir " + opts_.camera_dir);
		}
		dir_fd_ = ::open(opts_.camera_dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
		if (dir_fd_ < 0) {
			throw_errno("open dir " + opts_.camera_dir);
		}
		std::string segments_path = opts_.camera_dir + "/segments.jsonl";
		segments_fd_ = ::open(segments_path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
		if (segments_fd_ < 0) {
			throw_errno("open " + segments_path);
		}
		opened_ = true;
		open_segment();
	}

	void Recorder::open_segment() {
		++segment_index_;
		char name[32];
		snprintf(name, sizeof(name), "seg_%05u.raw", segment_index_);
		std::string seg_path = opts_.camera_dir + "/" + name;
		seg_fd_ = ::open(seg_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
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
			LOG_WARN("recorder(", opts_.camera_id, "): fallocate unsupported on this filesystem (", std::strerror(errno),
					 "); continuing without preallocation");
		}

		uint32_t seg_flags = opts_.payload_crc ? format::kSegFlagPayloadCrc : 0;
		format::FileHeader fh =
				format::make_file_header(segment_index_, now_realtime_ns(), opts_.session_uuid, opts_.camera_id.c_str(),
										 opts_.camera_serial.c_str(), opts_.record_align, seg_flags);
		write_all(seg_fd_, &fh, sizeof(fh), "write file header");
		if (::fdatasync(seg_fd_) != 0) {
			throw_errno("fdatasync file header");
		}
		seg_offset_ = sizeof(fh);

		// Pad so the first record starts on a record_align boundary — every
		// record offset in the file is then a multiple of record_align
		// (mmap/O_DIRECT friendly; format.hpp layout contract).
		const uint64_t first_record = format::align_up(seg_offset_, opts_.record_align);
		uint64_t pad_left = first_record - seg_offset_;
		while (pad_left > 0) {
			size_t n = static_cast<size_t>(std::min<uint64_t>(pad_left, sizeof(kZeros)));
			write_all(seg_fd_, kZeros, n, "write header padding");
			pad_left -= n;
		}
		seg_offset_ = first_record;
		last_synced_off_ = seg_offset_;

		char idx_name[40];
		snprintf(idx_name, sizeof(idx_name), "seg_%05u.idx.jsonl", segment_index_);
		std::string idx_path = opts_.camera_dir + "/" + idx_name;
		idx_fd_ = ::open(idx_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
		if (idx_fd_ < 0) {
			throw_errno("open " + idx_path);
		}
		idx_buf_.clear();
		idx_buf_.reserve(kIdxBufFlushBytes + 512);
		idx_frames_pending_ = 0;
		idx_last_flush_mono_ns_ = now_monotonic_ns();

		// Persist the directory entries of the new files.
		if (::fsync(dir_fd_) != 0) {
			throw_errno("fsync dir");
		}

		seg_frames_ = 0;
		if (stats_) {
			stats_->segments_created.fetch_add(1, std::memory_order_relaxed);
		}
		LOG_DEBUG("recorder(", opts_.camera_id, "): opened segment ", seg_path);
	}

	void Recorder::write_frame(const FrameMeta &meta, const uint8_t *data, size_t size) {
		if (!opened_ || closed_) {
			throw IoError("recorder not open");
		}

		const uint64_t record_bytes = format::align_up(format::kFrameHeaderSize + size, opts_.record_align);

		// Rotate when this record would overflow the segment (never split a
		// record). A single record larger than the segment size still goes into
		// its own fresh segment.
		if (seg_frames_ > 0 && seg_offset_ + record_bytes > opts_.segment_max_bytes) {
			close_segment(true);
			check_free_space();
			open_segment();
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
		h.payload_crc32c = opts_.payload_crc ? format::crc32c(data, size) : 0;
		format::seal_frame_header(h);

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
		write_iov_all(iov, iovcnt, total);
		if (pad > sizeof(kZeros)) {
			size_t left = pad;
			while (left > 0) {
				size_t n = std::min(left, sizeof(kZeros));
				write_all(seg_fd_, kZeros, n, "write padding");
				left -= n;
			}
		}
		seg_offset_ += record_bytes;

		// Keep dirty pages bounded: kick off async writeback periodically and
		// drop already-written-back pages from the cache. Prevents the kernel
		// from accumulating gigabytes of dirty data and then stalling write().
		if (seg_offset_ - last_synced_off_ >= opts_.flush_interval_bytes) {
#if defined(__linux__)
			(void) ::sync_file_range(seg_fd_, static_cast<off_t>(last_synced_off_), static_cast<off_t>(seg_offset_ - last_synced_off_),
									 SYNC_FILE_RANGE_WRITE);
			if (last_synced_off_ > 0) {
				(void) ::posix_fadvise(seg_fd_, 0, static_cast<off_t>(last_synced_off_), POSIX_FADV_DONTNEED);
			}
#endif
			last_synced_off_ = seg_offset_;
		}

		// Index line. Written after the payload so the index is always a subset
		// of the data. Short keys keep it ~150 bytes/frame.
		char line[320];
		int n = snprintf(line, sizeof(line),
						 "{\"seq\":%" PRIu64 ",\"bid\":%" PRIu64 ",\"dts\":%" PRIu64 ",\"hrt\":%" PRIu64 ",\"hmn\":%" PRIu64
						 ",\"off\":%" PRIu64
						 ",\"psz\":%zu,\"pf\":%u,\"w\":%u,\"h\":%u,"
						 "\"fl\":%u}\n",
						 frame_seq_, meta.block_id, meta.device_ts_ns, meta.host_realtime_ns, meta.host_monotonic_ns, record_offset,
						 size, meta.pixel_format, meta.width, meta.height, meta.status_flags);
		if (n > 0) {
			idx_buf_.append(line, static_cast<size_t>(n));
		}
		++idx_frames_pending_;
		flush_index(false);

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

		if (opts_.debug_slowdown_us > 0) {
			::usleep(opts_.debug_slowdown_us);
		}
	}

	void Recorder::write_iov_all(const struct iovec *iov, int iovcnt, size_t total) {
		struct iovec local[3];
		for (int i = 0; i < iovcnt; ++i) {
			local[i] = iov[i];
		}
		struct iovec *cur = local;
		while (total > 0) {
			ssize_t n = ::writev(seg_fd_, cur, iovcnt);
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

	void Recorder::flush_index(bool force) {
		bool due = force || idx_buf_.size() >= kIdxBufFlushBytes || idx_frames_pending_ >= kIdxFlushFrames;
		if (!due) {
			uint64_t now = now_monotonic_ns();
			due = idx_frames_pending_ > 0 && now - idx_last_flush_mono_ns_ >= kIdxFlushIntervalNs;
		}
		if (!due || idx_buf_.empty()) {
			if (due) {
				idx_last_flush_mono_ns_ = now_monotonic_ns();
			}
			return;
		}
		write_all(idx_fd_, idx_buf_.data(), idx_buf_.size(), "write index");
		idx_buf_.clear();
		idx_frames_pending_ = 0;
		idx_last_flush_mono_ns_ = now_monotonic_ns();
	}

	void Recorder::append_segment_summary(bool clean) {
		char name[32];
		snprintf(name, sizeof(name), "seg_%05u.raw", segment_index_);
		char line[512];
		int n = snprintf(line, sizeof(line),
						 "{\"seg\":\"%s\",\"frames\":%" PRIu64 ",\"bytes\":%" PRIu64 ",\"seq_first\":%" PRIu64 ",\"seq_last\":%" PRIu64
						 ",\"bid_first\":%" PRIu64 ",\"bid_last\":%" PRIu64 ",\"dts_first\":%" PRIu64 ",\"dts_last\":%" PRIu64
						 ",\"closed_clean\":%s}\n",
						 name, seg_frames_, seg_offset_, seg_seq_first_, seg_seq_last_, seg_bid_first_, seg_bid_last_, seg_dts_first_,
						 seg_dts_last_, clean ? "true" : "false");
		if (n > 0) {
			write_all(segments_fd_, line, static_cast<size_t>(n), "write segments.jsonl");
		}
	}

	void Recorder::close_segment(bool clean) {
		if (seg_fd_ < 0) {
			return;
		}
		flush_index(true);
		if (::fdatasync(idx_fd_) != 0) {
			throw_errno("fdatasync index");
		}
		::close(idx_fd_);
		idx_fd_ = -1;
		if (::fdatasync(seg_fd_) != 0) {
			throw_errno("fdatasync segment");
		}
		::close(seg_fd_);
		seg_fd_ = -1;
		append_segment_summary(clean);
		if (::fdatasync(segments_fd_) != 0) {
			throw_errno("fdatasync segments.jsonl");
		}
	}

	void Recorder::check_free_space() {
		if (opts_.min_free_bytes == 0) {
			return;
		}
		struct statvfs vfs{};
		if (::statvfs(opts_.camera_dir.c_str(), &vfs) != 0) {
			return;	 // best effort
		}
		uint64_t free_bytes = static_cast<uint64_t>(vfs.f_bavail) * vfs.f_frsize;
		if (free_bytes < opts_.min_free_bytes) {
			throw IoError("free disk space below threshold (" + human_bytes(free_bytes) + " < " + human_bytes(opts_.min_free_bytes) +
						  ")");
		}
	}

	void Recorder::close() {
		if (!opened_ || closed_) {
			return;
		}
		closed_ = true;
		close_segment(true);
		if (segments_fd_ >= 0) {
			::close(segments_fd_);
			segments_fd_ = -1;
		}
		if (dir_fd_ >= 0) {
			(void) ::fsync(dir_fd_);
			::close(dir_fd_);
			dir_fd_ = -1;
		}
	}

}  // namespace jai
