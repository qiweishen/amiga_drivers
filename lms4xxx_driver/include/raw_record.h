#pragma once

#include <cstdint>

#include "frame_receiver.h"

namespace lms4xxx {
    // Owns the complete received CoLa B telegram, including its checksum byte.
    // Parsed observations describe this telegram; they never replace its bytes.
    struct RawRecord {
        RawFrame frame;
        bool is_scan = false;
        std::int64_t device_time_unix_us = 0;
        std::int64_t clock_step_us = 0;
        std::uint16_t clock_quality_flags = 0;
    };
} // namespace lms4xxx
