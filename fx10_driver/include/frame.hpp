#pragma once

#include <cstddef>
#include <cstdint>

// Transport <-> recorder contract. This header must stay free of eBUS types so the
// recorder side (fx10_core) never depends on the SDK.

namespace fx10 {
    // Non-owning view of one delivered GVSP frame. For the FX10 pushbroom camera one
    // frame is one spatial line: width = spatial samples, height = spectral bands
    // (rows are bands, row-major). Valid only for the duration of the sink call.
    struct FrameView {
        const std::uint8_t *data = nullptr;
        std::size_t size = 0; // payload bytes (== width * height * bytes_per_pixel)
        std::uint32_t width = 0; // spatial samples
        std::uint32_t height = 0; // spectral bands (incl. status line if enabled)
        std::uint32_t bytes_per_pixel = 0; // Mono12/Mono10 -> 2, Mono8 -> 1
        std::uint64_t block_id = 0; // GVSP BlockID as reported by the SDK
        std::uint64_t device_timestamp_ticks = 0; // GVSP timestamp; 0 if unavailable
        std::int64_t host_realtime_ns = 0; // CLOCK_REALTIME at retrieve
        std::int64_t host_monotonic_ns = 0; // CLOCK_MONOTONIC, same instant
    };

    // Frame consumer. Called on the acquisition thread; implementations must return
    // promptly (the write path budget is ~3 ms at the camera's 327 fps maximum).
    class IFrameSink {
    public:
        virtual ~IFrameSink() = default;

        virtual void onFrame(const FrameView &frame) = 0;

        // Reported when BlockID accounting detects frames that never reached us.
        // first_missing_block_id is the first absent ID, missing_count the run length.
        virtual void onGap(std::uint64_t first_missing_block_id, std::uint64_t missing_count) = 0;

        // Polled by the transport each iteration: true = the sink can no longer accept
        // frames (e.g. recorder latched a write error) and acquisition should stop.
        virtual bool failed() const { return false; }
    };
} // namespace fx10
