#pragma once

#include <cstdint>
#include <optional>
#include <string>

// Rendering of the periodic and final [Statistics] lines.
//
// This is a GUI CONTRACT, not a log message: app/services/driver_stats.py
// routes the line by its "[Statistics] " prefix and reads the disk write rate
// out of the `fps=` field, and tools/check_contracts.py pins both the format
// literal (in stats_line.cpp) and a rendered sample. Field order and the
// DOUBLE-SPACE separator are therefore part of the contract — new fields are
// appended, never interleaved — and the rendering lives here, free of the eBUS
// SDK, so tests/test_stats_line.cpp can assert it.
//
// Telemetry the camera did not answer renders as "n/a" rather than as a NaN
// that reads like a real measurement.

namespace fx10 {
    struct StatsSample {
        std::uint64_t frames = 0; // frames delivered by the transport, session total
        double rate_hz = 0.0; // delivered frames/s over the last window
        double write_fps = 0.0; // frames that reached the .bil over the same window
        std::optional<std::int64_t> missed_triggers; // nullopt = counter unreadable / not baselined
        std::optional<double> temp_pcb_c; // processing board (cancels operation at 80 C, manual p.44)
        std::optional<double> temp_fpga_c; // FPGA (cancels operation at 90 C, manual p.44)
    };

    struct FinalStatsSample {
        std::uint64_t frames = 0;
        std::uint64_t frames_missed_rx = 0; // BlockID gaps: frames the camera sent that never arrived
        std::optional<std::int64_t> missed_triggers;
        std::uint64_t retrieve_timeouts = 0; // RetrieveBuffer timeouts (idle polls under external trigger)
    };

    std::string FormatStatsLine(const StatsSample &sample);

    std::string FormatFinalStatsLine(const FinalStatsSample &sample);
} // namespace fx10
