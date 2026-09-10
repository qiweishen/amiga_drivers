#include "recording_pipeline.h"

#include <doctest/doctest.h>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
    struct BlockingSink : fx10::IFrameSink {
        std::mutex mutex;
        std::condition_variable cv;
        bool entered = false, released = false;
        std::vector<std::string> events;
        std::vector<std::vector<std::uint16_t>> pixels;
        std::vector<fx10::FrameView> metadata;

        void OnFrame(const fx10::FrameView &frame) override {
            events.push_back("frame:" + std::to_string(frame.block_id));
            std::vector<std::uint16_t> values(frame.size / 2);
            std::memcpy(values.data(), frame.data, frame.size);
            pixels.push_back(std::move(values));
            auto copy = frame;
            copy.data = nullptr; // retain metadata only, never the worker's scratch pointer
            metadata.push_back(copy);
            std::unique_lock lock(mutex);
            entered = true;
            cv.notify_all();
            cv.wait(lock, [this] { return released; });
        }
        void OnGap(std::uint64_t first, std::uint64_t count) override {
            events.push_back("gap:" + std::to_string(first) + ":" + std::to_string(count));
        }
        void OnRejected(std::uint64_t id, const char *reason, const fx10::FrameView *frame) override {
            events.push_back(std::string(reason) + ":" + std::to_string(id));
            auto copy = *frame;
            copy.data = nullptr;
            metadata.push_back(copy);
        }
        bool WaitUntilEntered() {
            std::unique_lock lock(mutex);
            return cv.wait_for(lock, std::chrono::seconds(5), [this] { return entered; });
        }
        void Release() {
            std::lock_guard lock(mutex);
            released = true;
            cv.notify_all();
        }
    };
    struct ReleaseGuard {
        BlockingSink &sink;
        ~ReleaseGuard() { sink.Release(); }
    };

    fx10::RecordingPipeline::Options Options(std::uint32_t queue_frames) {
        fx10::RecordingPipeline::Options options;
        options.width = 2;
        options.height = 2;
        options.pixel_format = "Mono12Packed";
        options.payload_capacity = 10; // (3 pixel bytes + 1 padding byte) * 2 rows + 2 padding bytes
        options.queue_frames = queue_frames;
        return options;
    }

    fx10::FrameView Frame(const std::vector<std::uint8_t> &bytes, std::uint64_t id) {
        fx10::FrameView frame;
        frame.data = bytes.data();
        frame.size = bytes.size();
        frame.width = frame.height = 2;
        frame.bytes_per_pixel = 2;
        frame.block_id = id;
        frame.device_timestamp_raw = 1234567;
        frame.host_receive_realtime_ns = 987654321;
        frame.host_receive_monotonic_ns = 54321;
        frame.sdk.emplace();
        frame.sdk->image_present = true;
        frame.sdk->width = frame.sdk->height = 2;
        frame.sdk->padding_x = 1;
        frame.sdk->padding_y = 2;
        frame.sdk->acquired_size = frame.sdk->image_size = 10;
        frame.sdk->effective_image_size = 6;
        return frame;
    }
}

TEST_CASE("RecordingPipeline owns queued SDK bytes and preserves pixel values and event order") {
    BlockingSink sink;
    fx10::RecordingPipeline pipeline(sink, Options(4));
    ReleaseGuard release_on_failure{sink}; // release BEFORE pipeline destructor, even on REQUIRE failure
    std::vector<std::uint8_t> bytes{0x12, 0xC3, 0xAB, 0xEE, 0x00, 0xF1, 0xFF, 0xEE, 0xDD, 0xDD};
    REQUIRE(pipeline.Submit(Frame(bytes, 1)));
    REQUIRE(sink.WaitUntilEntered());
    REQUIRE(pipeline.Submit(Frame(bytes, 3), 2, 1));
    auto rejected = Frame(bytes, 4);
    rejected.sdk->operation_result = 42;
    REQUIRE(pipeline.Submit(rejected, 0, 0, fx10::RecordingPipeline::Rejection::kOperationError));
    // Simulates immediate SDK requeue/reuse while frame 3 is still queued.
    std::fill(bytes.begin(), bytes.end(), 0);
    sink.Release();
    pipeline.Finish();
    CHECK_FALSE(pipeline.Failed());
    CHECK(pipeline.FramesDelivered() == 2);
    CHECK(pipeline.FramesUnconfirmed() == 0);
    CHECK(sink.events == std::vector<std::string>{"frame:1", "gap:2:1", "frame:3", "operation-error:4"});
    REQUIRE(sink.pixels.size() == 2);
    const std::vector<std::uint16_t> expected{0x123, 0xABC, 0x001, 0xFFF};
    CHECK(sink.pixels[0] == expected);
    CHECK(sink.pixels[1] == expected);
    REQUIRE(sink.metadata.size() == 3);
    CHECK(sink.metadata[1].host_receive_realtime_ns == 987654321);
    CHECK(sink.metadata[1].host_receive_monotonic_ns == 54321);
    CHECK(sink.metadata[1].device_timestamp_raw == 1234567);
    REQUIRE(sink.metadata[1].sdk.has_value());
    CHECK(sink.metadata[1].sdk->acquired_size == 10);
    CHECK(sink.metadata[1].sdk->padding_y == 2);
    REQUIRE(sink.metadata[2].sdk.has_value());
    CHECK(sink.metadata[2].sdk->operation_result == 42);
}

TEST_CASE("RecordingPipeline queue overflow is fatal but accepted frames still drain") {
    BlockingSink sink;
    fx10::RecordingPipeline pipeline(sink, Options(1));
    ReleaseGuard release_on_failure{sink};
    const std::vector<std::uint8_t> bytes(10, 0);
    REQUIRE(pipeline.Submit(Frame(bytes, 1)));
    REQUIRE(sink.WaitUntilEntered());
    REQUIRE(pipeline.Submit(Frame(bytes, 2)));
    CHECK_FALSE(pipeline.Submit(Frame(bytes, 3)));
    CHECK(pipeline.Failed());
    CHECK(pipeline.ErrorMessage().find("BlockID 3") != std::string::npos);
    sink.Release();
    pipeline.Finish();
    pipeline.Finish(); // idempotent
    CHECK(pipeline.FramesDelivered() == 2);
    CHECK(pipeline.FramesUnconfirmed() == 0);
    CHECK(sink.events == std::vector<std::string>{"frame:1", "frame:2"});
}

TEST_CASE("RecordingPipeline captures sink exceptions and joins without terminating") {
    struct ThrowingSink : fx10::IFrameSink {
        void OnFrame(const fx10::FrameView &) override { throw std::runtime_error("injected sink failure"); }
        void OnGap(std::uint64_t, std::uint64_t) override {}
    } sink;
    fx10::RecordingPipeline pipeline(sink, Options(1));
    const std::vector<std::uint8_t> bytes(10, 0);
    REQUIRE(pipeline.Submit(Frame(bytes, 1)));
    pipeline.Finish();
    CHECK(pipeline.Failed());
    CHECK(pipeline.FramesDelivered() == 0);
    CHECK(pipeline.FramesUnconfirmed() == 1);
    CHECK(pipeline.ErrorMessage().find("injected sink failure") != std::string::npos);
}
