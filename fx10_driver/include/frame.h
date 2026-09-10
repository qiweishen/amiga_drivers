#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

// Transport <-> recorder contract. This header must stay free of eBUS types so the
// recorder side (fx10_core) never depends on the SDK.

namespace fx10 {
    struct SdkFrameMetadata {
        std::uint64_t acquired_size = 0;
        std::uint32_t payload_type = 0, operation_result = 0, chunk_count = 0;
        bool image_present = false;
        std::uint32_t pixel_type = 0, width = 0, height = 0, padding_x = 0, padding_y = 0;
        std::uint64_t image_size = 0, effective_image_size = 0;
    };
    // Non-owning view of one delivered GVSP frame. For the FX10 pushbroom camera one
    // frame is one spatial line: width = spatial samples, height = spectral bands
    // (rows are bands, row-major). Valid only for the duration of the sink call.
    // Timestamps are observations, not a claim about exposure time or clock sync.
    struct FrameView {
        const std::uint8_t *data = nullptr;
        std::size_t size = 0; // IFrameSink: canonical W*H*bpp; RecordingPipeline::Submit: SDK image bytes with padding
        std::uint32_t width = 0; // spatial samples
        std::uint32_t height = 0; // spectral bands (incl. status line if enabled)
        std::uint32_t bytes_per_pixel = 0; // Mono12/Mono10 -> 2, Mono8 -> 1
        std::uint64_t block_id = 0; // GVSP BlockID as reported by the SDK
        std::uint64_t device_timestamp_raw = 0; // SDK value; unit/epoch deliberately not assumed
        std::uint64_t host_receive_realtime_ns = 0;
        std::uint64_t host_receive_monotonic_ns = 0;
        bool block_id_anomaly = false; // retained, but must not be treated as a normal sequential line
        std::optional<SdkFrameMetadata> sdk; // before unpack/padding removal; absent for synthetic lines
    };

    // Frame consumer. Called serially on the recording worker, after unpacking.
    // Blocking consumes the bounded queue's stall budget. Implementations must
    // expose Failed() safely to the worker/monitor and may not retain the view.
    class IFrameSink {
    public:
        virtual ~IFrameSink() = default;

        virtual void OnFrame(const FrameView &frame) = 0;

        // Reported when BlockID accounting detects frames that never reached us.
        // first_missing_block_id is the first absent ID, missing_count the run length.
        virtual void OnGap(std::uint64_t first_missing_block_id, std::uint64_t missing_count) = 0;

        // A retrieved buffer rejected before pixel delivery. reason is a stable
        // token (operation-error/non-image/layout-error), not arbitrary SDK text.
        virtual void OnRejected(std::uint64_t, const char *, const FrameView * = nullptr) {}

        // Polled by the recording worker: true = the sink can no longer accept
        // frames (e.g. recorder latched a write error) and acquisition should stop.
        virtual bool Failed() const { return false; }
    };
} // namespace fx10
