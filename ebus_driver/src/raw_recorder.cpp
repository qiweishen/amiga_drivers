#include "ebus_driver/raw_recorder.h"
#include "ebus_driver/raw_format.h"
#include "time_util.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <limits>
#include <stdexcept>
#include <utility>
#include <unistd.h>


namespace ebus_driver {
    namespace {
        [[noreturn]] void IoError(const std::string &action) {
            throw std::runtime_error(action + ": " + std::strerror(errno));
        }

        void WriteAll(int fd, const void *data, std::size_t size) {
            const auto *p = static_cast<const std::uint8_t *>(data);
            while (size != 0) {
                const auto written = ::write(fd, p, size);
                if (written < 0 && errno == EINTR) continue;
                if (written < 0) IoError("raw write");
                if (written == 0) throw std::runtime_error("raw write made no progress");
                p += written;
                size -= static_cast<std::size_t>(written);
            }
        }

        void Sync(int fd, bool directory = false) {
            int result;
            do { result = directory ? ::fsync(fd) : ::fdatasync(fd); } while (result != 0 && errno == EINTR);
            if (result != 0) IoError(directory ? "raw directory sync" : "raw data sync");
        }

        void CloseFd(int &fd) {
            if (fd < 0) return;
            const int owned = std::exchange(fd, -1);
            // On Linux the descriptor is released even when close reports an error.
            if (::close(owned) != 0) IoError("raw close");
        }

        std::string SegmentName(std::uint64_t number) {
            char name[64];
            std::snprintf(name, sizeof(name), "seg_%05llu.raw", static_cast<unsigned long long>(number));
            return name;
        }
    } // namespace

    RawRecorder::RawRecorder(RecorderOptions options) : options_(std::move(options)) {
        if (options_.directory.empty() || options_.segment_bytes <= raw::kFileHeaderSize)
            throw std::invalid_argument("raw recorder needs a directory and positive segment capacity");
    }

    RawRecorder::~RawRecorder() {
        try { Close(false, "recorder destroyed before finalization"); } catch (...) {
        }
    }

    void RawRecorder::Fail() { failed_.store(true); }

    void RawRecorder::Open(const nlohmann::json &device) {
        if (opened_ || closed_) throw std::logic_error("raw recorder is single-use");
        try {
            if (!device.is_object() || !device.contains("recording_format") ||
                !device.at("recording_format").is_string())
                throw std::invalid_argument("raw device metadata requires a recording_format string");
            recording_format_ = device.at("recording_format").get<std::string>();
            if (recording_format_.empty())
                throw std::invalid_argument("raw device metadata recording_format must not be empty");
            const auto parent = options_.directory.parent_path();
            if (!parent.empty()) std::filesystem::create_directories(parent);
            if (!std::filesystem::create_directory(options_.directory))
                throw std::runtime_error("refusing to reuse raw camera directory: " + options_.directory.string());
            directory_fd_ = ::open(options_.directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            if (directory_fd_ < 0) IoError("open raw camera directory");
            const auto part = options_.directory / "device.json.part";
            int metadata_fd = ::open(part.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
            if (metadata_fd < 0) IoError("create device metadata");
            try {
                const auto text = device.dump(2) + "\n";
                WriteAll(metadata_fd, text.data(), text.size());
                Sync(metadata_fd);
                CloseFd(metadata_fd);
                const auto path = options_.directory / "device.json";
                if (::link(part.c_str(), path.c_str()) != 0) IoError("publish device metadata");
                if (::unlink(part.c_str()) != 0) IoError("remove metadata temporary name");
            } catch (...) {
                if (metadata_fd >= 0) ::close(metadata_fd);
                throw;
            }
            const auto events = options_.directory / "events.jsonl";
            events_fd_ = ::open(events.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
            if (events_fd_ < 0) IoError("create raw events");
            opened_ = true;
            OpenSegment();
            Sync(directory_fd_, true);
            // Persist creation of this camera directory, too.
            const auto parent_path = parent.empty() ? std::filesystem::path(".") : parent;
            int parent_fd = ::open(parent_path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            if (parent_fd < 0) IoError("open raw parent directory");
            try {
                Sync(parent_fd, true);
                CloseFd(parent_fd);
            } catch (...) {
                if (parent_fd >= 0) ::close(parent_fd);
                throw;
            }
            Event({
                {"event", "session_start"}, {"format", recording_format_},
                {"host_realtime_ns", common::TimeUtil::RealtimeNowNs()}
            });
        } catch (...) {
            Fail();
            const auto error = std::current_exception();
            try { Close(false, "recorder initialization failed"); } catch (...) {
            }
            std::rethrow_exception(error);
        }
    }

    void RawRecorder::OpenSegment() {
        if (segment_ == std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error("raw segment index exhausted");
        const auto next_segment = segment_ + 1;
        const auto path = options_.directory / SegmentName(next_segment);
        data_fd_ = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
        if (data_fd_ < 0) IoError("create raw segment");
        // Count only files this recorder actually created. Reset the new file's
        // bookkeeping before any I/O can fail, never borrowing the previous segment.
        segment_ = next_segment;
        offset_ = synced_ = segment_records_ = 0;
        const auto header = raw::MakeFileHeader(common::TimeUtil::RealtimeNowNs(), segment_);
        WriteAll(data_fd_, header.data(), header.size());
        offset_ = header.size();
        Sync(data_fd_);
        synced_ = offset_;
        Sync(directory_fd_, true);
    }

    void RawRecorder::Append(const RawBuffer &buffer) {
        if (!opened_ || closed_ || Failed()) throw std::runtime_error("raw recorder is closed or failed");
        try {
            if (!buffer.metadata.is_object()) throw std::invalid_argument("raw buffer metadata must be an object");
            const auto metadata = buffer.metadata.dump();
            if (metadata.size() > raw::kMaxMetadataSize) throw std::invalid_argument("raw metadata exceeds 1 MiB");
            const bool valid_frame = buffer.metadata.value("valid_frame", false);
            const auto max_offset = static_cast<std::uint64_t>(std::numeric_limits<off_t>::max());
            const auto overhead = raw::kRecordHeaderSize + static_cast<std::uint64_t>(metadata.size());
            if (buffer.payload.size() > max_offset - raw::kFileHeaderSize - overhead)
                throw std::overflow_error("raw record exceeds file offset range");
            const auto size = overhead + static_cast<std::uint64_t>(buffer.payload.size());
            const auto segment_limit = std::min(options_.segment_bytes, max_offset);
            if (segment_records_ != 0 &&
                ((options_.segment_records != 0 && segment_records_ >= options_.segment_records) ||
                 offset_ >= segment_limit || size > segment_limit - offset_)) {
                CloseSegment();
                OpenSegment();
            }
            const auto header = raw::MakeRecordHeader(static_cast<std::uint32_t>(metadata.size()),
                                                      buffer.payload.size(), records_.load(),
                                                      raw::Crc32c(metadata.data(), metadata.size()),
                                                      raw::Crc32c(buffer.payload.data(), buffer.payload.size()));
            try {
                WriteAll(data_fd_, header.data(), header.size());
                WriteAll(data_fd_, metadata.data(), metadata.size());
                WriteAll(data_fd_, buffer.payload.data(), buffer.payload.size());
            } catch (...) {
                // Retain only complete records if the filesystem still permits rollback.
                // If rollback also fails, the prefix/length/CRCs delimit the recoverable prefix.
                if (::ftruncate(data_fd_, static_cast<off_t>(offset_)) != 0) {
                    Fail();
                    throw std::runtime_error("raw append failed and partial-record rollback also failed");
                }
                throw;
            }
            offset_ += size;
            ++segment_records_;
            records_.fetch_add(1);
            if (valid_frame) frames_.fetch_add(1);
            bytes_.fetch_add(buffer.payload.size());
            if (options_.flush_bytes != 0 && offset_ - synced_ >= options_.flush_bytes) {
                Sync(data_fd_);
                synced_ = offset_;
            }
        } catch (...) {
            Fail();
            throw;
        }
    }

    void RawRecorder::Event(const nlohmann::json &event) {
        std::lock_guard lock(events_mutex_);
        if (events_fd_ < 0) throw std::runtime_error("raw event stream is not open");
        try {
            const auto text = event.dump() + "\n";
            const auto before = ::lseek(events_fd_, 0, SEEK_CUR);
            if (before < 0) IoError("raw event offset");
            try {
                WriteAll(events_fd_, text.data(), text.size());
                Sync(events_fd_);
            } catch (...) {
                const auto error = std::current_exception();
                Fail();
                try {
                    if (::ftruncate(events_fd_, before) != 0 || ::lseek(events_fd_, before, SEEK_SET) < 0)
                        IoError("raw partial-row rollback");
                    Sync(events_fd_);
                } catch (...) {
                    // An unrepairable JSONL suffix must never receive more events,
                    // especially a misleading session_end after a torn row.
                    try { CloseFd(events_fd_); } catch (...) {
                    }
                    throw std::runtime_error("raw event write and partial-row rollback failed");
                }
                std::rethrow_exception(error);
            }
        } catch (...) {
            Fail();
            throw;
        }
    }

    void RawRecorder::CloseSegment() {
        if (data_fd_ < 0) return;
        std::exception_ptr error;
        try { Sync(data_fd_); } catch (...) {
            Fail();
            error = std::current_exception();
        }
        try { CloseFd(data_fd_); } catch (...) {
            Fail();
            if (!error) error = std::current_exception();
        }
        if (error) std::rethrow_exception(error);
        Event({
            {"event", "segment_closed"}, {"file", SegmentName(segment_)},
            {"bytes", offset_}, {"records", segment_records_}, {"closed_clean", !Failed()}
        });
    }

    void RawRecorder::Close(bool complete, const std::string &reason) {
        if (closed_) return;
        closed_ = true;
        std::exception_ptr error;
        const auto attempt = [&](auto action) {
            try { action(); } catch (...) {
                Fail();
                if (!error) error = std::current_exception();
            }
        };
        attempt([&] { CloseSegment(); });
        // All payload and directory finalization that can fail must precede the
        // completion marker, so it cannot claim success before those operations.
        if (directory_fd_ >= 0) attempt([&] { Sync(directory_fd_, true); });
        attempt([&] { CloseFd(directory_fd_); });
        if (events_fd_ >= 0)
            attempt([&] {
                auto end = Summary();
                end["event"] = "session_end";
                end["complete"] = complete && !Failed();
                end["reason"] = reason;
                end["host_realtime_ns"] = common::TimeUtil::RealtimeNowNs();
                Event(end);
            });
        attempt([&] { CloseFd(events_fd_); });
        if (error) std::rethrow_exception(error);
    }

    nlohmann::json RawRecorder::Summary() const {
        return {
            {"format", recording_format_}, {"directory", options_.directory.string()},
            {"records_written", records_.load()}, {"frames_written", frames_.load()},
            {"valid_frames_written", frames_.load()}, {"payload_bytes_written", bytes_.load()},
            {"segments_created", segment_}, {"failed", Failed()},
            {
                "count_semantics",
                "records include flagged SDK buffers; frames count valid_frame records only; written is not a per-record durability claim"
            }
        };
    }
} // namespace ebus_driver
