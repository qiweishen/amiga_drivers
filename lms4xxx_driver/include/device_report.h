#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// What the device says about ITSELF, as opposed to what it measures. Kept in
// its own header so the recorder can store it without depending on the driver
// API: the driver fills these in, the HDF5 writer turns them into root
// attributes and /telemetry rows (docs/FORMAT_H5.md).
//
// Every field is optional. These are all "sRN <name>" reads whose variables the
// LMS4000 manual documents but does not guarantee on every firmware, so a
// device that does not answer must cost the metadata, never the recording.

namespace lms4xxx {
    // Read once at the end of Configure(), when the configuration is live.
    struct DeviceAudit {
        std::optional<double> temperature_c; ///< sRN OPcurtmpdev, manual p.130
        std::optional<std::uint32_t> operating_hours_deci; ///< sRN ODoprh, 1/10 h, p.128
        std::optional<std::uint32_t> power_on_count; ///< sRN ODpwrc, p.129
        std::optional<int> device_state; ///< sRN SCdevicestate: 0 busy, 1 ready, 2 error, p.124

        // sRN LMPscancfg (p.73): the scan configuration BEFORE any filter, i.e.
        // independent evidence of the geometry LMPoutputRange asked for.
        std::optional<std::uint32_t> scan_frequency_centi_hz;
        std::optional<std::uint32_t> angular_resolution_1e4;
        std::optional<std::int32_t> start_angle_1e4;
        std::optional<std::int32_t> stop_angle_1e4;

        // sRN IOlasc (p.77). Continuous output needs trigger source 0
        // (free-running, p.17), and a non-zero laser timeout would switch the
        // laser off part-way through a long survey.
        std::optional<int> laser_trigger_source;
        std::optional<int> laser_timeout_s;

        // sRN SYtype / SYphas (p.141-142). The driver never WRITES these: this
        // rig does not use motor synchronisation. It reads them because they are
        // Interfaces parameters, which mSCloadappdef explicitly does not reset
        // (p.79) — so a primary/secondary role left behind by a SOPAS session
        // survives every restart and every power cycle.
        std::optional<int> motor_sync_role; ///< 0 = no function, 1 = primary, 2 = secondary
        std::optional<double> motor_sync_phase_deg;
    };


    // One periodic device reading, collected while streaming.
    struct TelemetrySample {
        std::uint64_t unix_us = 0;
        std::optional<double> temperature_c;
        std::optional<int> device_state;
        std::vector<std::string> warnings; ///< sRN EMActiveCustomerInfo, clear text (p.125)
    };
} // namespace lms4xxx

