#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "frame.h"
#include "pixel_format.h"

namespace fx10 {
    // One producer copies validated SDK image bytes into a preallocated pool;
    // one worker removes padding, unpacks pixels and calls the sink in order.
    // No SDK pointer escapes Submit(). Finish() must follow the producer join,
    // and must precede recorder.Stop(). Neither the sink nor its data may be
    // destroyed until Finish returns. Queue exhaustion is a visible fatal error.
    class RecordingPipeline {
    public:
        struct Options {
            std::uint32_t width = 0, height = 0;
            std::string pixel_format;
            bool status_line = false;
            std::size_t payload_capacity = 0;
            std::uint32_t queue_frames = 0;
        };
        enum class Rejection { kNone, kOperationError, kNonImage, kLayoutError };

        // Allocates all payload/scratch buffers before starting the worker.
        RecordingPipeline(IFrameSink &sink, Options options);
        ~RecordingPipeline();
        RecordingPipeline(const RecordingPipeline &) = delete;
        RecordingPipeline &operator=(const RecordingPipeline &) = delete;

        // frame.data/size are SDK image bytes INCLUDING declared padding here.
        // Rejected events carry metadata only; gap and frame occupy ONE ordered
        // queue entry, so a queue limit can never separate a gap from its frame.
        bool Submit(const FrameView &frame, std::uint64_t first_missing = 0,
                    std::uint64_t missing_count = 0, Rejection rejection = Rejection::kNone);
        void Finish(); // closes and drains, idempotent; never called by the worker
        bool Failed() const { return failed_.load(std::memory_order_acquire) || sink_.Failed(); }
        std::string ErrorMessage() const;
        std::uint64_t FramesDelivered() const { return delivered_.load(std::memory_order_relaxed); }
        // Includes a sink call that failed after writing some/all bytes. This
        // may overlap FramesDelivered and must not be added to a written count.
        std::uint64_t FramesUnconfirmed() const { return unconfirmed_.load(std::memory_order_relaxed); }

    private:
        struct Slot {
            std::vector<std::uint8_t> bytes;
            FrameView frame;
            std::uint64_t first_missing = 0, missing_count = 0;
            Rejection rejection = Rejection::kNone;
        };
        void Run();
        void Deliver(Slot &slot);
        void Fail(const std::string &error);
        void CheckStatusLine(const FrameView &frame);

        IFrameSink &sink_;
        const Options options_;
        const PixelFormatInfo *pixel_info_ = nullptr;
        std::vector<Slot> slots_;
        std::vector<std::size_t> free_, ready_; // fixed storage; no hot-path queue allocations
        std::size_t head_ = 0, tail_ = 0, ready_count_ = 0;
        std::vector<std::uint16_t> canonical_; // also supplies aligned byte storage for Mono8
        bool preamble_checked_ = false; // worker only
        std::mutex mutex_;
        std::condition_variable ready_cv_;
        bool closed_ = false;
        std::thread worker_;
        std::atomic<bool> failed_{false};
        std::atomic<std::uint64_t> delivered_{0}, unconfirmed_{0};
        mutable std::mutex error_mutex_;
        std::string error_;
    };
}
