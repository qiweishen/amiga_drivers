#pragma once

#include <cstdint>

#include "scan_data.h"

namespace lms4xxx {
    // Calendar fields from the telegram expressed as Unix microseconds.
    // Returns 0 for malformed fields; does not certify NTP synchronization.
    std::int64_t DeviceTimeUnixUs(const ScanTimestamp &timestamp);
}
