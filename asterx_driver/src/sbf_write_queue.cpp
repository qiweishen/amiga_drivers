#include "sbf_write_queue.h"

#include <utility>

#include "logger.h"


namespace asterx {
    namespace {
        common::DriverLog g_log{"AsteRx"};
    } // namespace


    SbfWriteQueue::SbfWriteQueue(SbfWriter::Config cfg, std::uint64_t max_pending_bytes, FailureCallback on_failure)
        : writer_(std::move(cfg)), max_pending_bytes_(max_pending_bytes), on_failure_(std::move(on_failure)) {
    }


    SbfWriteQueue::~SbfWriteQueue() { Close(); }


    void SbfWriteQueue::Start() {
        if (thread_.joinable()) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (closed_) {
                return;
            }
            stop_ = false;
        }
        thread_ = std::thread([this] { WriterLoop(); });
    }


    bool SbfWriteQueue::Enqueue(const QByteArray &block) {
        if (block.isEmpty()) {
            return true;
        }
        if (failed_.load(std::memory_order_acquire)) {
            return false;
        }
        const auto size = static_cast<std::uint64_t>(block.size());
        std::uint64_t pending = 0;
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (stop_ || closed_) {
                return false;
            }
            if (pending_bytes_ + size > max_pending_bytes_) {
                pending = pending_bytes_;
            } else {
                // A QByteArray copy shares the buffer (atomic refcount): no payload copy here
                queue_.push_back(block);
                pending_bytes_ += size;
                pending_atomic_.store(pending_bytes_, std::memory_order_relaxed);
                if (pending_bytes_ > max_pending_.load(std::memory_order_relaxed)) {
                    max_pending_.store(pending_bytes_, std::memory_order_relaxed);
                }
                cv_.notify_one();
                return true;
            }
        }
        // Overflow: the blocks already accepted still reach the disk; this one is refused
        // and the caller ends the run. No callback: the refusal itself is the signal.
        Fail("write queue budget exceeded (" + std::to_string(pending) + " B pending, " +
             std::to_string(max_pending_bytes_) + " B budget): the disk fell behind", /*notify=*/false);
        return false;
    }


    bool SbfWriteQueue::Close() noexcept {
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (closed_) {
                return !failed_.load(std::memory_order_acquire);
            }
            stop_ = true;
        }
        cv_.notify_all();
        if (thread_.joinable()) {
            thread_.join(); // the loop drains the queue before it exits
        } else {
            // Never started (e.g. a startup failure before Start()): write what was
            // queued, in order, on this thread
            std::deque<QByteArray> rest;
            {
                std::lock_guard<std::mutex> lock(mu_);
                rest.swap(queue_);
                pending_bytes_ = 0;
                pending_atomic_.store(0, std::memory_order_relaxed);
            }
            for (const auto &block: rest) {
                WriteOne(block);
            }
        }
        if (!writer_.close()) {
            Fail("SBF final flush/close failed", /*notify=*/false);
        }
        {
            std::lock_guard<std::mutex> lock(mu_);
            closed_ = true;
        }
        return !failed_.load(std::memory_order_acquire);
    }


    std::string SbfWriteQueue::LastError() const {
        std::lock_guard<std::mutex> lock(mu_);
        return error_;
    }


    SbfWriteQueue::Stats SbfWriteQueue::GetStats() const noexcept {
        Stats s;
        s.records_written = records_written_.load(std::memory_order_relaxed);
        s.bytes_written = bytes_written_.load(std::memory_order_relaxed);
        s.files_opened = files_opened_.load(std::memory_order_relaxed);
        s.pending_bytes = pending_atomic_.load(std::memory_order_relaxed);
        s.max_pending_bytes = max_pending_.load(std::memory_order_relaxed);
        return s;
    }


    void SbfWriteQueue::WriterLoop() {
        for (;;) {
            QByteArray block;
            {
                std::unique_lock<std::mutex> lock(mu_);
                cv_.wait(lock, [this] { return stop_ || !queue_.empty(); });
                if (queue_.empty()) {
                    return; // stop requested and nothing left to write
                }
                block = std::move(queue_.front());
                queue_.pop_front();
            }
            WriteOne(block);
            {
                std::lock_guard<std::mutex> lock(mu_);
                pending_bytes_ -= static_cast<std::uint64_t>(block.size());
                pending_atomic_.store(pending_bytes_, std::memory_order_relaxed);
            }
            cv_.notify_all();
        }
    }


    bool SbfWriteQueue::WriteOne(const QByteArray &block) {
        if (write_failed_.load(std::memory_order_acquire)) {
            return false; // the file is gone; the run is ending (fail-fast)
        }
        if (!writer_.WriteBlock(block)) {
            write_failed_.store(true, std::memory_order_release);
            Fail("SBF write failed (disk full or output directory lost?)", /*notify=*/true);
            return false;
        }
        const auto &s = writer_.Stats();
        records_written_.store(s.records_written, std::memory_order_relaxed);
        bytes_written_.store(s.bytes_written, std::memory_order_relaxed);
        files_opened_.store(s.files_opened, std::memory_order_relaxed);
        return true;
    }


    void SbfWriteQueue::Fail(const std::string &what, bool notify) {
        bool first;
        {
            std::lock_guard<std::mutex> lock(mu_);
            first = !failed_.exchange(true, std::memory_order_acq_rel);
            if (first) {
                error_ = what;
            }
        }
        if (first) {
            g_log.Error("[Writer] {}", what);
            if (notify && on_failure_) {
                on_failure_();
            }
        }
    }
} // namespace asterx
