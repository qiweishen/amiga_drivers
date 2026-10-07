#pragma once

// SDK-free helpers for the GO-X's single PTP feature pair, GevIEEE1588 /
// GevIEEE1588Status (manual p.121, p.128).

#include <string>

namespace gox {
    enum class PtpState {
        kSlave, // 9: synchronized to a grandmaster - the only healthy state
        kMaster, // 6: no grandmaster is winning the BMCA on this network
        kFaulty, // 2: the camera's 1588 stack hit an internal error
        kOther // initializing / disabled / listening / preMaster / passive / uncalibrated
    };

    // Maps a GevIEEE1588Status reading to a state. Accepts both the symbolic
    // name and the integer value the enum is printed with (manual p.128), and is
    // case-insensitive.
    PtpState ClassifyPtpStatus(const std::string &status);
} // namespace gox
