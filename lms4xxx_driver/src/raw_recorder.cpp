#include "raw_recorder.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <limits>
#include <stdexcept>
#include <thread>
#include <utility>
#include <unistd.h>

#include "bounded_queue.h"
#include "logger.h"


namespace lms4xxx {
    namespace {
        common::DriverLog g_log{"LMS4xxx"};
        constexpr char kIndexHeader[] =
            "offset,length,host_receive_monotonic_us,is_scan,checksum_valid,"
            "device_time_unix_us,clock_step_us,clock_quality_flags\n";

        [[noreturn]] void IoError(const char *action) {
            throw std::runtime_error(std::string(action) + ": " + std::strerror(errno));
        }

        void WriteAll(int fd, const void *data, std::size_t size) {
            const auto *bytes = static_cast<const std::uint8_t *>(data);
            while (size != 0) {
                const auto n = ::write(fd, bytes, size);
                if (n < 0 && errno == EINTR) continue;
                if (n < 0) IoError("raw write");
                if (n == 0) throw std::runtime_error("raw write made no progress");
                bytes += n;
                size -= static_cast<std::size_t>(n);
            }
        }

        void Sync(int fd, bool directory = false) {
            int result;
            do { result = directory ? ::fsync(fd) : ::fdatasync(fd); } while (result < 0 && errno == EINTR);
            if (result < 0) IoError(directory ? "raw directory sync" : "raw file sync");
        }

        void CloseFd(int &fd) {
            if (fd < 0) return;
            const int owned = std::exchange(fd, -1);
            // Linux releases the descriptor even if close reports an error.
            if (::close(owned) < 0) IoError("raw close");
        }

        int CreateExclusive(const std::filesystem::path &path) {
            const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
            if (fd < 0) IoError("create raw output (existing files are never overwritten)");
            return fd;
        }
    } // namespace

    struct RawRecorder::Impl {
        explicit Impl(Config cfg) : config(std::move(cfg)) {}

        Config config;
        std::unique_ptr<common::BoundedQueue<RawRecord>> queue;
        std::thread worker;
        std::atomic<bool> accepting{false};
        std::atomic<bool> failed{false};
        bool started = false; // Start/Stop belong to the owner thread
        int bin_fd = -1, csv_fd = -1, json_fd = -1, directory_fd = -1;
        std::uint64_t split_index = 0, split_bytes = 0, split_frames = 0, split_scans = 0;
        std::atomic<std::uint64_t> frames_queued{0}, frames_written{0}, frames_dropped{0};
        std::atomic<std::uint64_t> scans_queued{0}, scans_written{0}, bytes_written{0}, files_created{0};

        void Fail(const char *message) noexcept {
            failed.store(true, std::memory_order_release);
            try { g_log.Error("[{}] [RawRecorder] {}", config.instance, message); } catch (...) {}
        }

        std::filesystem::path SplitPath(const char *extension) const {
            const std::filesystem::path base(config.output_path);
            char suffix[32];
            std::snprintf(suffix, sizeof(suffix), "_%03llu", static_cast<unsigned long long>(split_index));
            return base.parent_path() / (base.stem().string() + suffix + extension);
        }

        void WriteMetadata(bool clean) {
            const nlohmann::ordered_json document = {
                {"format", "lms4xxx-cola-b"}, {"format_version", 1},
                {"instance_id", config.instance}, {"session_timestamp", config.session_timestamp},
                {"split_index", split_index}, {"metadata", config.metadata},
                {"frames_total", split_frames}, {"scans_total", split_scans},
                {"bytes_total", split_bytes}, {"closed_cleanly", clean}
            };
            const auto text = document.dump(2) + "\n";
            if (::lseek(json_fd, 0, SEEK_SET) < 0) IoError("seek raw metadata");
            WriteAll(json_fd, text.data(), text.size());
            if (::ftruncate(json_fd, static_cast<off_t>(text.size())) < 0) IoError("truncate raw metadata");
            Sync(json_fd);
        }

        void OpenSplit() {
            split_bytes = split_frames = split_scans = 0;
            // The initial manifest survives an interrupted acquisition as incomplete.
            json_fd = CreateExclusive(SplitPath(".json"));
            WriteMetadata(false);
            bin_fd = CreateExclusive(SplitPath(".bin"));
            files_created.fetch_add(1, std::memory_order_relaxed);
            csv_fd = CreateExclusive(SplitPath(".csv"));
            WriteAll(csv_fd, kIndexHeader, sizeof(kIndexHeader) - 1);
            Sync(bin_fd);
            Sync(csv_fd);
            Sync(directory_fd, true);
        }

        // Best effort close on errors; never abandon other owned descriptors.
        void CloseSplit() noexcept {
            const auto attempt = [this](auto action) {
                try { action(); }
                catch (const std::exception &e) { Fail(e.what()); }
                catch (...) { Fail("unknown error while closing a raw split"); }
            };
            if (bin_fd >= 0) attempt([&] { Sync(bin_fd); });
            if (csv_fd >= 0) attempt([&] { Sync(csv_fd); });
            attempt([&] { CloseFd(bin_fd); });
            attempt([&] { CloseFd(csv_fd); });
            if (json_fd >= 0) {
                // Persist the final counts while still incomplete. The clean
                // flag is the last field and only changes after this succeeds.
                attempt([&] { WriteMetadata(false); });
                if (!failed.load(std::memory_order_acquire)) attempt([&] { WriteMetadata(true); });
                // If finalization failed, retain an explicit incomplete marker
                // when the storage still accepts writes. It may instead be torn.
                if (failed.load(std::memory_order_acquire)) attempt([&] { WriteMetadata(false); });
                attempt([&] { CloseFd(json_fd); });
            }
        }

        void Append(const RawRecord &record) {
            const auto length = static_cast<std::uint64_t>(record.frame.data.size());
            const auto max_offset = static_cast<std::uint64_t>(std::numeric_limits<off_t>::max());
            if (length == 0 || length > max_offset) throw std::runtime_error("invalid raw telegram length");
            if (split_frames != 0 && config.max_file_bytes != 0 &&
                (split_bytes >= config.max_file_bytes || length > config.max_file_bytes - split_bytes)) {
                CloseSplit();
                if (failed.load(std::memory_order_acquire)) throw std::runtime_error("raw split could not be finalized");
                if (split_index == std::numeric_limits<std::uint64_t>::max())
                    throw std::overflow_error("raw split index exhausted");
                ++split_index;
                OpenSplit();
            }
            if (length > max_offset - split_bytes) throw std::overflow_error("raw file offset exhausted");
            const std::string row = std::to_string(split_bytes) + "," + std::to_string(length) + "," +
                std::to_string(record.frame.receive_timestamp_us) + "," + (record.is_scan ? "1," : "0,") +
                (record.frame.checksum_valid ? "1," : "0,") + std::to_string(record.device_time_unix_us) + "," +
                std::to_string(record.clock_step_us) + "," + std::to_string(record.clock_quality_flags) + "\n";
            WriteAll(bin_fd, record.frame.data.data(), record.frame.data.size());
            WriteAll(csv_fd, row.data(), row.size());
            split_bytes += length;
            ++split_frames;
            if (record.is_scan) ++split_scans;
            frames_written.fetch_add(1, std::memory_order_relaxed);
            if (record.is_scan) scans_written.fetch_add(1, std::memory_order_relaxed);
            bytes_written.fetch_add(length, std::memory_order_relaxed);
        }

        void WriteLoop() noexcept {
            bool pending = false;
            try {
                const auto interval = std::chrono::milliseconds(config.flush_interval_ms);
                auto last_flush = std::chrono::steady_clock::now();
                RawRecord record;
                for (;;) {
                    if (queue->try_pop(record)) {
                        pending = true;
                        Append(record);
                        pending = false;
                    } else if (queue->closed() && queue->size() == 0) {
                        break;
                    } else {
                        std::this_thread::sleep_for(std::chrono::microseconds(500));
                    }
                    const auto now = std::chrono::steady_clock::now();
                    if (now - last_flush >= interval) {
                        Sync(bin_fd);
                        Sync(csv_fd);
                        last_flush = now;
                    }
                }
            } catch (const std::exception &e) {
                Fail(e.what());
            } catch (...) {
                Fail("unknown exception in raw writer");
            }
            accepting.store(false, std::memory_order_release);
            try {
                queue->close();
                if (pending) frames_dropped.fetch_add(1, std::memory_order_relaxed);
                RawRecord discarded;
                while (queue->try_pop(discarded)) frames_dropped.fetch_add(1, std::memory_order_relaxed);
            } catch (const std::exception &e) {
                Fail(e.what());
            } catch (...) {
                Fail("unknown exception draining failed raw writer");
            }
            CloseSplit();
        }
    };

    RawRecorder::RawRecorder(Config config) : impl_(std::make_unique<Impl>(std::move(config))) {}

    RawRecorder::~RawRecorder() { Stop(); }

    bool RawRecorder::Start() {
        if (impl_->started) return false;
        impl_->started = true;
        try {
            if (impl_->config.output_path.empty() || impl_->config.queue_capacity == 0 ||
                impl_->config.flush_interval_ms == 0)
                throw std::invalid_argument("raw output path, queue capacity and flush interval must be nonempty/positive");
            auto parent = std::filesystem::path(impl_->config.output_path).parent_path();
            if (parent.empty()) parent = ".";
            std::filesystem::create_directories(parent);
            impl_->directory_fd = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            if (impl_->directory_fd < 0) IoError("open raw output directory");
            impl_->queue = std::make_unique<common::BoundedQueue<RawRecord>>(impl_->config.queue_capacity);
            impl_->OpenSplit();
            impl_->accepting.store(true, std::memory_order_release);
            impl_->worker = std::thread([this] { impl_->WriteLoop(); });
            return true;
        } catch (const std::exception &e) {
            impl_->Fail(e.what());
        } catch (...) {
            impl_->Fail("unknown exception starting raw recorder");
        }
        impl_->accepting.store(false, std::memory_order_release);
        if (impl_->queue) impl_->queue->close();
        impl_->CloseSplit();
        try { CloseFd(impl_->directory_fd); } catch (const std::exception &e) { impl_->Fail(e.what()); }
        return false;
    }

    void RawRecorder::OnFrame(RawRecord &&record) {
        if (!impl_->accepting.load(std::memory_order_acquire)) {
            impl_->frames_dropped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        const bool is_scan = record.is_scan;
        try {
            if (impl_->queue->try_push(std::move(record))) {
                impl_->frames_queued.fetch_add(1, std::memory_order_relaxed);
                if (is_scan) impl_->scans_queued.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            impl_->Fail("raw queue is full or closed; recording is incomplete");
        } catch (const std::exception &e) {
            impl_->Fail(e.what());
        } catch (...) {
            impl_->Fail("unknown exception queuing raw telegram");
        }
        impl_->frames_dropped.fetch_add(1, std::memory_order_relaxed);
        impl_->accepting.store(false, std::memory_order_release);
        impl_->queue->close();
    }

    void RawRecorder::Stop() {
        impl_->accepting.store(false, std::memory_order_release);
        if (impl_->queue) impl_->queue->close();
        if (impl_->worker.joinable()) impl_->worker.join();
        try { CloseFd(impl_->directory_fd); } catch (const std::exception &e) { impl_->Fail(e.what()); }
    }

    bool RawRecorder::HasFailed() const { return impl_->failed.load(std::memory_order_acquire); }

    RawRecorder::Statistics RawRecorder::GetStatistics() const {
        return {
            impl_->frames_queued.load(std::memory_order_relaxed),
            impl_->frames_written.load(std::memory_order_relaxed),
            impl_->frames_dropped.load(std::memory_order_relaxed),
            impl_->scans_queued.load(std::memory_order_relaxed),
            impl_->scans_written.load(std::memory_order_relaxed),
            impl_->bytes_written.load(std::memory_order_relaxed),
            impl_->files_created.load(std::memory_order_relaxed)
        };
    }
} // namespace lms4xxx
