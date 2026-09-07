#pragma once

#include <cstdint>
#include <string>

#include "app_config.h"
#include "scan_data.h"


namespace lms4xxx {
    // Earliest device time the driver accepts as NTP-synchronised: 2026-01-01T00:00:00Z. The
    // LMS4xxx has no RTC (manual p.97) and streams a free-running clock (1970 epoch) until its
    // first NTP sync; anything before this floor is that unsynchronised clock. Host time is
    // deliberately not consulted (the platform does not trust it)
    constexpr std::int64_t kEarliestPlausibleDeviceTimeUs = 1767225600LL * 1000000LL;

    // True when the telegram time stamp is well-formed and at or after the floor above
    bool DeviceTimePlausible(const ScanTimestamp &ts);

    // First-telegram proof that the configuration took effect: "" when the scan carries exactly
    // DIST1 + remission + ANGL1 + QLTY1 with the ScanFixed geometry and the expected optional blocks,
    // otherwise a comma-separated problem list
    std::string VerifyScanContent(const ScanData &scan, const ScanConfig &scan_config, bool ntp_enabled);
} // namespace lms4xxx
