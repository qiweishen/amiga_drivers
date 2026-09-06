#pragma once

// SDK-free sizing rules for the two buffer pools. A GOX-12405C frame is
// 4128 x 3008 x 1.5 B = 18.6 MB, so both counts are real memory: the defaults
// buy a couple of seconds of jitter absorption, not ten.

#include <cstddef>
#include <cstdint>

namespace gox {
    // GVSP buffers handed to the SDK. Absorbs ~1 s at the configured rate plus
    // headroom, clamped to [8, 64]; 16 when no rate is configured. `queued_max`
    // is PvStream::GetQueuedBufferMaximum() (0 = unknown) and caps the result,
    // because queueing more than the stream accepts fails the bring-up.
    uint32_t AutoBufferCount(double frame_rate_hz, uint32_t queued_max);

    // Frame chunks between the acquisition and writer threads (the ChunkPool
    // allocates this many + 2). Absorbs ~2 s of writer stall, clamped to [4, 64].
    uint32_t AutoQueueFrames(double frame_rate_hz);
} // namespace gox
