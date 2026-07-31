#include "../include/timestamp_sidecar.hpp"


namespace fx10 {
    SidecarHeader makeSidecarHeader(std::uint64_t tick_frequency_hz,
                                    std::uint64_t session_start_realtime_ns,
                                    std::uint64_t session_start_monotonic_ns,
                                    std::uint32_t segment_index) {
        SidecarHeader header{};
        std::memcpy(header.magic, kSidecarMagic, sizeof(header.magic));
        header.version = kSidecarVersion;
        header.record_size = sizeof(SidecarRecord);
        header.tick_frequency_hz = tick_frequency_hz;
        header.session_start_realtime_ns = session_start_realtime_ns;
        header.session_start_monotonic_ns = session_start_monotonic_ns;
        header.segment_index = segment_index;
        return header;
    }

    bool sidecarHeaderValid(const SidecarHeader &header) {
        return std::memcmp(header.magic, kSidecarMagic, sizeof(header.magic)) == 0 &&
               header.version == kSidecarVersion && header.record_size == sizeof(SidecarRecord);
    }
} // namespace fx10
