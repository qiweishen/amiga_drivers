#pragma once

#include <cstddef>
#include <cstdint>

// Transport <-> recorder contract. This header must stay free of eBUS types so the
// recorder side (fx10_core) never depends on the SDK.

namespace fx10 {
    // Non-owning view of one delivered GVSP frame. For the FX10 pushbroom camera one
    // frame is one spatial line: width = spatial samples, height = spectral bands
    // (rows are bands, row-major). Valid only for the duration of the sink call.
    // Timestamps are observations, not a claim about exposure time or clock sync.
    struct FrameView {
        const std::uint8_t *data = nullptr;
        std::size_t size = 0; // payload bytes (== width * height * bytes_per_pixel)
        std::uint32_t width = 0; // spatial samples
        std::uint32_t height = 0; // spectral bands (incl. status line if enabled)
        std::uint32_t bytes_per_pixel = 0; // Mono12/Mono10 -> 2, Mono8 -> 1
        std::uint64_t block_id = 0; // GVSP BlockID as reported by the SDK
        std::uint64_t device_timestamp_raw = 0; // SDK value; unit/epoch deliberately not assumed
        std::uint64_t host_receive_realtime_ns = 0;
        std::uint64_t host_receive_monotonic_ns = 0;
        bool block_id_anomaly = false; // retained, but must not be treated as a normal sequential line
    };

    // Frame consumer. Called on the acquisition thread; implementations must return
    // promptly (the write path budget is ~3 ms at the camera's 327 fps maximum).
    class IFrameSink {
    public:
        virtual ~IFrameSink() = default;

        virtual void OnFrame(const FrameView &frame) = 0;

        // Reported when BlockID accounting detects frames that never reached us.
        // first_missing_block_id is the first absent ID, missing_count the run length.
        virtual void OnGap(std::uint64_t first_missing_block_id, std::uint64_t missing_count) = 0;

        // A retrieved buffer rejected before pixel delivery. reason is a stable
        // token (operation-error/non-image/layout-error), not arbitrary SDK text.
        virtual void OnRejected(std::uint64_t, const char *) {}

        // Polled by the transport each iteration: true = the sink can no longer accept
        // frames (e.g. recorder latched a write error) and acquisition should stop.
        virtual bool Failed() const { return false; }
    };
} // namespace fx10
