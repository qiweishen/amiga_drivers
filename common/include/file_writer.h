#pragma once

#include <chrono>
#include <cstdint>
#include <fstream>
#include <functional>
#include <string>
#include <utility>
#include <vector>


namespace common {
    class BufferedFileWriter {
    public:
        BufferedFileWriter() = default;

        // Non-copyable (owns a stream and its buffer)
        BufferedFileWriter(const BufferedFileWriter &) = delete;

        BufferedFileWriter &operator=(const BufferedFileWriter &) = delete;

        // pubsetbuf must precede open; buffer_size = 0 keeps the default buffer
        bool Open(const std::string &path, std::size_t buffer_size,
                                std::ios::openmode mode = std::ios::out | std::ios::binary) {
            Close();
            if (buffer_size > 0) {
                buffer_.resize(buffer_size);
                file_.rdbuf()->pubsetbuf(buffer_.data(), static_cast<std::streamsize>(buffer_.size()));
            }
            file_.open(path, mode);
            if (!file_.is_open()) {
                return false;
            }
            path_ = path;
            return true;
        }

        bool IsOpen() const { return file_.is_open(); }

        // False once any write/flush failed (badbit/failbit); IsOpen() stays
        // true on a failed stream, so error detection must use this
        bool Good() const { return file_.good(); }

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

        const std::string &Path() const { return path_; }

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

            std::uint64_t max_file_bytes = 0; // 0 = no size rotation
            std::chrono::seconds rotate_interval{0}; // 0 = no time rotation
            std::size_t buffer_bytes = 0; // pubsetbuf size per file

            // Rotate BEFORE a write that would push the file past max_file_bytes,
            // making it a hard per-file cap (e.g. RxTools refuses >= 2 GB files)
            bool precheck_size_cap = false;

            std::function<bool(std::ofstream &)> on_new_file;
        };

        // NOT thread-safe across threads (single writer at a time);
        // read from the writing thread only
        struct Stats {
            std::uint64_t bytes_written = 0; // record payload bytes (headers excluded)
            std::uint64_t records_written = 0;
            std::uint64_t files_opened = 0;
        };

        explicit RotatingFileWriter(Options opts) : opts_(std::move(opts)) {
        }

        ~RotatingFileWriter() { Close(); }

        RotatingFileWriter(const RotatingFileWriter &) = delete;

        RotatingFileWriter &operator=(const RotatingFileWriter &) = delete;

        // Close the current file and open the next sequence file. Eager users
        // (writer threads that must fail fast) call this from their Start()
        bool OpenNext() {
            if (!CloseCurrent()) {
                return false;
            }
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
                failed_ = true;
                return false;
            }
            current_path_ = path;
            current_bytes_ = 0;
            ++seq_;
            ++stats_.files_opened;
            file_start_ = std::chrono::steady_clock::now();
            if (opts_.on_new_file) {
                if (!opts_.on_new_file(file_) || !file_) {
                    failed_ = true;
                    CloseCurrent();
                    return false;
                }
                const auto pos = file_.tellp();
                current_bytes_ = pos > 0 ? static_cast<std::uint64_t>(pos) : 0;
            }
            return true;
        }

        bool Append(const void *data, std::size_t len) {
            if (failed_) {
                return false;
            }
            if (!file_.is_open() && !OpenNext()) {
                return false;
            }
            if (opts_.precheck_size_cap && opts_.max_file_bytes > 0) {
                if (len > opts_.max_file_bytes) {
                    failed_ = true;
                    return false; // hard cap: one record can never fit
                }
                if (current_bytes_ > opts_.max_file_bytes - len && !OpenNext()) {
                    return false;
                }
            }
            file_.write(static_cast<const char *>(data), static_cast<std::streamsize>(len));
            if (!file_) {
                failed_ = true;
                return false;
            }
            current_bytes_ += len;
            stats_.bytes_written += len;
            ++stats_.records_written;
            return RotateIfNeeded();
        }

        // Close so the next Append starts a fresh sequence file (segment
        // boundaries then coincide with link gaps)
        bool EndSegment() { return CloseCurrent(); }

        bool Close() { return CloseCurrent(); }

        bool Failed() const { return failed_; }

        bool IsOpen() const { return file_.is_open(); }
        const Stats &GetStats() const { return stats_; }
        const std::string &CurrentPath() const { return current_path_; }
        std::uint64_t CurrentFileBytes() const { return current_bytes_; }

    private:
        bool CloseCurrent() {
            if (file_.is_open()) {
                file_.flush();
                failed_ = failed_ || !file_;
                file_.close();
                failed_ = failed_ || !file_;
            }
            return !failed_;
        }

        bool RotateIfNeeded() {
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
        bool failed_ = false;
    };
} // namespace common
