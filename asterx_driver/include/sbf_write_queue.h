#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include <QByteArray>

#include "sbf_writer.h"


namespace asterx {
    // Decouples SBF recording from the thread that reads the receiver socket.
    // SsnRx parses and emits blocks on the Qt event thread; if that thread waited
    // on a stalled disk it would stop reading the socket, the receiver's output
    // buffer would fill and blocks would be lost before they ever reached the
    // driver. So Session::OnSbfBlock only enqueues, and a dedicated thread feeds
    // the (synchronous) SbfWriter.
    //
    // The queue is bounded in bytes. A full queue means the disk fell behind for
    // longer than the budget; the block is REFUSED (never silently dropped) and
    // the queue latches Failed() — the caller ends the run (fail-fast: a dropped
    // block is data loss). A failed write on the writer thread latches Failed()
    // as well and invokes the failure callback once, ON THE WRITER THREAD (the
    // Session marshals it back to the Qt thread).
    class SbfWriteQueue {
    public:
        struct Stats {
            std::uint64_t records_written = 0; // blocks on disk
            std::uint64_t bytes_written = 0;
            std::uint64_t files_opened = 0;
            std::uint64_t pending_bytes = 0; // queued, not yet on disk
            std::uint64_t max_pending_bytes = 0; // high-water mark of pending_bytes
        };

        using FailureCallback = std::function<void()>;

        // Throws std::runtime_error when the output directory cannot be created (SbfWriter)
        SbfWriteQueue(SbfWriter::Config cfg, std::uint64_t max_pending_bytes, FailureCallback on_failure = {});

        ~SbfWriteQueue();

        SbfWriteQueue(const SbfWriteQueue &) = delete;

        SbfWriteQueue &operator=(const SbfWriteQueue &) = delete;

        // Spawn the writer thread (idempotent). Blocks enqueued earlier are written first, in order
        void Start();

        // False = refused: the budget would be exceeded (latches Failed()) or the queue has
        // already failed or closed. Never blocks; a refused block is not taken
        bool Enqueue(const QByteArray &block);

        // Drain everything still queued (synchronously when the thread never started), join
        // the thread and close the file. False when a write, an overflow or the close failed.
        // Safe to call more than once; later calls only repeat the verdict
        bool Close() noexcept;

        bool Failed() const noexcept { return failed_.load(std::memory_order_acquire); }

        // Why Failed() is set (empty otherwise); readable from any thread
        std::string LastError() const;

        Stats GetStats() const noexcept;

    private:
        void WriterLoop();

        bool WriteOne(const QByteArray &block); // writer thread, or Close() when it never started

        void Fail(const std::string &what, bool notify);

        SbfWriter writer_; // writer thread only after Start() (Close() once the thread is joined)
        const std::uint64_t max_pending_bytes_;
        FailureCallback on_failure_;

        std::thread thread_;
        mutable std::mutex mu_;
        std::condition_variable cv_;
        std::deque<QByteArray> queue_; // guarded by mu_
        std::uint64_t pending_bytes_ = 0; // guarded by mu_
        bool stop_ = false; // guarded by mu_
        bool closed_ = false; // guarded by mu_
        std::string error_; // guarded by mu_

        std::atomic<bool> failed_{false}; // any failure: overflow, write, close
        std::atomic<bool> write_failed_{false}; // the writer itself failed: later blocks are discarded
        std::atomic<std::uint64_t> records_written_{0};
        std::atomic<std::uint64_t> bytes_written_{0};
        std::atomic<std::uint64_t> files_opened_{0};
        std::atomic<std::uint64_t> pending_atomic_{0};
        std::atomic<std::uint64_t> max_pending_{0};
    };
} // namespace asterx
