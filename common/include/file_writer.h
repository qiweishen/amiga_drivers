/// @file file_writer.h
/// @brief Shared binary-file writing infrastructure (header-only).
///
/// Two building blocks; the on-disk formats stay driver-private:
///  - BufferedFileWriter: ofstream + pubsetbuf boilerplate with an explicit
///    open-failure check (a silently unopened ofstream swallows all writes).
///  - RotatingFileWriter: size/interval rotation, lazy or eager open,
///    sequence-numbered paths via callback, optional per-file header hook and
///    an optional pre-write size cap (segment size as a HARD limit).
///
/// Neither class is thread-safe and neither throws; callers own the threading
/// model, the error reaction (throw / stop / degrade) and any statistics that
/// must be readable across threads.

#ifndef COMMON_FILE_WRITER_H
#define COMMON_FILE_WRITER_H

#include <chrono>
#include <cstdint>
#include <fstream>
#include <functional>
#include <string>
#include <utility>
#include <vector>


namespace Common {

	class BufferedFileWriter {
	public:
		BufferedFileWriter() = default;

		// Non-copyable (owns a stream and its buffer)
		BufferedFileWriter(const BufferedFileWriter &) = delete;
		BufferedFileWriter &operator=(const BufferedFileWriter &) = delete;

		// pubsetbuf must precede open; buffer_size = 0 keeps the default buffer
		[[nodiscard]] bool Open(const std::string &path, std::size_t buffer_size,
								std::ios::openmode mode = std::ios::out | std::ios::binary) {
			Close();
			if (buffer_size > 0) {
				buffer_.resize(buffer_size);
				file_.rdbuf()->pubsetbuf(buffer_.data(), static_cast<std::streamsize>(buffer_.size()));
			}
			file_.open(path, mode);
			path_ = path;
			return file_.is_open();
		}

		[[nodiscard]] bool IsOpen() const { return file_.is_open(); }

		void Write(const void *data, std::size_t len) {
			file_.write(static_cast<const char *>(data), static_cast<std::streamsize>(len));
		}

		void Flush() {
			if (file_.is_open()) {
				file_.flush();
			}
		}

		void Close() {
			if (file_.is_open()) {
				file_.flush();
				file_.close();
			}
		}

		[[nodiscard]] const std::string &Path() const { return path_; }

	private:
		std::ofstream file_;
		std::vector<char> buffer_;
		std::string path_;
	};


	class RotatingFileWriter {
	public:
		struct Options {
			// Maps a 0-based sequence number to the file path (required)
			std::function<std::string(std::uint32_t seq)> make_path;

			std::uint64_t max_file_bytes = 0;			 // 0 = no size rotation
			std::chrono::seconds rotate_interval{ 0 };	 // 0 = no time rotation
			std::size_t buffer_bytes = 0;				 // pubsetbuf size per file

			// Rotate BEFORE a write that would push the file past max_file_bytes,
			// making it a hard per-file cap (e.g. RxTools refuses >= 2 GB files)
			bool precheck_size_cap = false;

			// Called right after each file opens (e.g. write the format header).
			// Return false to fail the open. Header bytes count towards the
			// file-size rotation threshold but not towards Stats.bytes_written
			std::function<bool(std::ofstream &)> on_new_file;
		};

		// NOT thread-safe across threads (single writer at a time); read from
		// the writing thread only
		struct Stats {
			std::uint64_t bytes_written = 0;	// record payload bytes (headers excluded)
			std::uint64_t records_written = 0;
			std::uint64_t files_opened = 0;
		};

		explicit RotatingFileWriter(Options opts) : opts_(std::move(opts)) {}

		~RotatingFileWriter() { Close(); }

		RotatingFileWriter(const RotatingFileWriter &) = delete;
		RotatingFileWriter &operator=(const RotatingFileWriter &) = delete;

		// Close the current file and open the next sequence file. Eager users
		// (writer threads that must fail fast) call this from their Start()
		[[nodiscard]] bool OpenNext() {
			CloseCurrent();
			const std::string path = opts_.make_path(seq_);
			if (opts_.buffer_bytes > 0) {
				if (buffer_.size() != opts_.buffer_bytes) {
					buffer_.resize(opts_.buffer_bytes);
				}
				// pubsetbuf must precede open
				file_.rdbuf()->pubsetbuf(buffer_.data(), static_cast<std::streamsize>(buffer_.size()));
			}
			file_.open(path, std::ios::out | std::ios::binary);
			if (!file_.is_open()) {
				return false;
			}
			current_path_ = path;
			current_bytes_ = 0;
			++seq_;
			++stats_.files_opened;
			file_start_ = std::chrono::steady_clock::now();
			if (opts_.on_new_file) {
				if (!opts_.on_new_file(file_) || !file_) {
					CloseCurrent();
					return false;
				}
				const auto pos = file_.tellp();
				current_bytes_ = pos > 0 ? static_cast<std::uint64_t>(pos) : 0;
			}
			return true;
		}

		// Append one record: opens the first file lazily, applies the pre-write
		// cap and the post-write size/interval rotation. Returns false on any
		// open or stream failure (the stream state is left for inspection)
		[[nodiscard]] bool Append(const void *data, std::size_t len) {
			if (!file_.is_open() && !OpenNext()) {
				return false;
			}
			if (opts_.precheck_size_cap && opts_.max_file_bytes > 0 && current_bytes_ > 0 &&
				current_bytes_ + len > opts_.max_file_bytes) {
				if (!OpenNext()) {
					return false;
				}
			}
			file_.write(static_cast<const char *>(data), static_cast<std::streamsize>(len));
			if (!file_) {
				return false;
			}
			current_bytes_ += len;
			stats_.bytes_written += len;
			++stats_.records_written;
			return RotateIfNeeded();
		}

		// Close so the next Append starts a fresh sequence file (segment
		// boundaries then coincide with link gaps)
		void EndSegment() { CloseCurrent(); }

		void Close() { CloseCurrent(); }

		[[nodiscard]] bool IsOpen() const { return file_.is_open(); }
		[[nodiscard]] const Stats &GetStats() const { return stats_; }
		[[nodiscard]] const std::string &CurrentPath() const { return current_path_; }
		[[nodiscard]] std::uint64_t CurrentFileBytes() const { return current_bytes_; }

	private:
		void CloseCurrent() {
			if (file_.is_open()) {
				file_.flush();
				file_.close();
			}
		}

		[[nodiscard]] bool RotateIfNeeded() {
			const bool by_size = opts_.max_file_bytes > 0 && current_bytes_ >= opts_.max_file_bytes;
			const bool by_time = opts_.rotate_interval.count() > 0 &&
								 (std::chrono::steady_clock::now() - file_start_) >= opts_.rotate_interval;
			if (by_size || by_time) {
				return OpenNext();
			}
			return true;
		}

		Options opts_;
		std::ofstream file_;
		std::vector<char> buffer_;
		std::string current_path_;
		std::uint64_t current_bytes_ = 0;
		std::chrono::steady_clock::time_point file_start_{};
		std::uint32_t seq_ = 0;
		Stats stats_{};
	};

}  // namespace Common

#endif	// COMMON_FILE_WRITER_H
