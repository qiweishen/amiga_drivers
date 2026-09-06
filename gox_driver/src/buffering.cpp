#include "buffering.h"

#include <algorithm>
#include <cmath>

namespace gox {
    namespace {
        constexpr uint32_t kMinGvspBuffers = 8;
        constexpr uint32_t kMaxGvspBuffers = 64;
        constexpr uint32_t kDefaultGvspBuffers = 16; // no rate configured
        constexpr uint32_t kMinQueueFrames = 4;
        constexpr uint32_t kMaxQueueFrames = 64;
        constexpr uint32_t kDefaultQueueFrames = 8; // no rate configured
    } // namespace

    uint32_t AutoBufferCount(double frame_rate_hz, uint32_t queued_max) {
        uint32_t count = kDefaultGvspBuffers;
        if (frame_rate_hz > 0.0) {
            const double n = std::ceil(frame_rate_hz * 1.0) + 8.0;
            count = static_cast<uint32_t>(std::clamp(n, static_cast<double>(kMinGvspBuffers),
                                                     static_cast<double>(kMaxGvspBuffers)));
        }
        if (queued_max != 0 && count > queued_max) {
            count = queued_max;
        }
        return std::max<uint32_t>(count, 1);
    }

    uint32_t AutoQueueFrames(double frame_rate_hz) {
        if (frame_rate_hz <= 0.0) {
            return kDefaultQueueFrames;
        }
        const double n = std::ceil(frame_rate_hz * 2.0);
        return static_cast<uint32_t>(std::clamp(n, static_cast<double>(kMinQueueFrames),
                                                static_cast<double>(kMaxQueueFrames)));
    }
} // namespace gox
