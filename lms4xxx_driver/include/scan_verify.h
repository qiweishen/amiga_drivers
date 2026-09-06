#pragma once

#include <string>

#include "app_config.h"
#include "scan_data.h"


namespace lms4xxx {
    // First-telegram proof that the configuration took effect: "" when the scan carries exactly
    // DIST1 + remission + ANGL1 + QLTY1 with the ScanFixed geometry and the expected optional blocks,
    // otherwise a comma-separated problem list
    std::string VerifyScanContent(const ScanData &scan, const ScanConfig &scan_config, bool ntp_enabled);
} // namespace lms4xxx
