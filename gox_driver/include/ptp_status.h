#pragma once

// SDK-free helpers for the GO-X's single PTP feature pair, GevIEEE1588 /
// GevIEEE1588Status (manual p.121, p.128).

#include <cstdint>
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

    // GevIEEE1588ClockAccuracy (manual p.128) ranges 0..20 and reads 19
    // (Unknown) out of the factory. 0..9 is this driver's acceptance window,
    // not the register's range: 0 Within25ns .. 9 Within1ms are usable for
    // frame timestamping, 10..18 are 2.5 ms or worse, 19 is Unknown (a camera
    // that never locked reads it) and 20 is Reserved.
    bool PtpClockAccuracyOk(int64_t accuracy);

    // The register value behind a GevIEEE1588ClockAccuracy entry name, for the
    // last-resort path where the node is exposed as text only. Accepts the
    // manual's spellings (p.128, "Within25ns" … "Reserved", case-insensitive)
    // and a plain decimal string. False when the text is neither.
    bool PtpClockAccuracyFromName(const std::string &text, int64_t &out);
} // namespace gox
