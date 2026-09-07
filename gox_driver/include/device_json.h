#pragma once

// Session metadata sidecars (docs/DEVICE_CONFIG.md, "Dataset metadata"):
//   <cam>/device.json      once per session, before AcquisitionStart: read-backs, tolerances,
//                          the raw audit of features the driver never writes, identity, runtime shape
//   <cam>/telemetry.jsonl  one line per device-poll tick
// SDK-free: pure data + JSON rendering, unit-testable without a camera.

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace gox {
    // --- Sensor constants the registers do not report ---------------------------
    // Added to every commanded ExposureTime (manual p.38, p.171; p.164 prints "2us")
    constexpr double kExposureOffsetUs = 2.45;

    // Black level pedestal "8LSB@8bit" (manual p.172); the 12-bit figure is the driver's 8x16 rescaling
    constexpr int kBlackLevelPedestalLsb8 = 8;
    constexpr int kBlackLevelPedestalLsb12 = 128;
    constexpr const char *kDocumentedPedestal = "8LSB@8bit";
    constexpr const char *kPedestalAdjustmentResolution = "1LSB@12bit";

    // Manual p.173: internal temperature must not exceed 72 C
    constexpr double kThermalLimitC = 72.0;
    constexpr double kThermalRearmC = 67.0;

    // What device.json says about the PTP timescale. The manual (p.121) only states a 1970-01-01
    // origin; the grandmaster's timescale is a rig property the driver does not verify (no host
    // clock is consulted on this platform)
    constexpr const char *kPtpTimescaleNote =
            "not verified by the driver (rig: AsteRx RBi3 Pro+ PTP server, GPS timescale per configuration)";

    // --- Applied writes -------------------------------------------------------
    // One entry per FeatureWrite the apply plan carried out, in plan order.
    struct AppliedFeature {
        std::string name;
        std::string requested; // FeatureWrite::value verbatim ("" for commands)
        std::string readback; // camera text, or "<executed>" / "<absent>" / "<failed>"
        bool strict = true;
        double tolerance_rel = 0.0; // read-back tolerance (float nodes; 0 for exact types)
    };

    // True when the Counter0 binding took (decides whether trig= is a number or null)
    bool Counter0Bound(const std::vector<AppliedFeature> &applied);

    // --- Raw audit ------------------------------------------------------------
    enum class AuditOutcome {
        kMatch, // reads the factory value the manual documents
        kDeviates, // reads something else -> warned about at bring-up
        kRecorded, // read, but the manual documents no factory value to judge it against
        kUnreadable, // absent, not implemented, or not readable on this camera
    };

    const char *AuditOutcomeName(AuditOutcome outcome);

    struct AuditEntry {
        std::string name; // "BlackLevel[Red]" for selector-scoped reads
        std::string value; // ToString() text, or "<absent>"
        std::string expected; // "" for record-only entries
        AuditOutcome outcome = AuditOutcome::kUnreadable;
    };

    // A feature the driver never writes; expected "" = record only, "a|b" = either spelling
    struct RawAuditExpectation {
        const char *name;
        const char *expected;
    };

    // Selector-scoped features: write selector -> read -> restore (and verify the restore)
    struct SelectorAuditExpectation {
        const char *selector; // e.g. "GainSelector"
        const char *entry; // integer enum value to select, e.g. "1"
        const char *feature; // e.g. "Gain"
        const char *expected;
        const char *label; // reported as this name, e.g. "Gain[DigitalRed]"
    };

    const std::vector<RawAuditExpectation> &RawAuditExpectations();

    const std::vector<SelectorAuditExpectation> &SelectorAuditExpectations();

    // Selector -> the entry the factory load leaves it on
    const std::vector<std::pair<const char *, const char *> > &SelectorHomeEntries();

    const std::vector<const char *> &IdentityFeatures();

    const std::vector<const char *> &TransportFeatures();

    // Case-insensitive, "|" alternatives, numeric fallback ("0" matches "0.000000")
    bool RawAuditMatches(const std::string &value, const std::string &expected);

    AuditOutcome ClassifyAudit(const std::string &value, const std::string &expected, bool readable);

    // --- Blocks ---------------------------------------------------------------
    struct FactoryLoadResult {
        bool loaded = false;
        std::string selector_entry; // the "Default" entry as this camera spells it
        std::string selector_readback;
        uint64_t elapsed_ms = 0;
        bool is_done_supported = false; // false = UserSetLoad has no completion reporting
    };

    struct PtpSummary {
        bool enabled = false; // config asked for PTP
        bool feature_found = false;
        bool written = false; // GevIEEE1588 = true accepted
        bool synchronized = false; // status reached "slave"
        std::string status; // last GevIEEE1588Status text
        int64_t accuracy = -1; // last GevIEEE1588ClockAccuracy, -1 = never read
        uint64_t lock_wait_ms = 0;
    };

    // What the driver resolved at runtime ("auto" values in the config)
    struct RuntimeShape {
        uint32_t buffer_count = 0; // GVSP buffers actually allocated
        uint32_t queue_frames = 0; // frame queue capacity
        uint32_t socket_rx_requested_bytes = 0;
        uint32_t socket_rx_effective_bytes = 0; // raw SDK SO_RCVBUF readback, including Linux accounting
        std::string socket_rx_set_result = "NOT_QUERIED";
        std::string socket_rx_read_result = "NOT_QUERIED";
        uint32_t packet_size = 0; // GevSCPSPacketSize after negotiation
        uint64_t payload_size = 0;
        bool counter0_bound = false; // CounterEventSource=FrameTrigger accepted
        bool selector_restore_failed = false;
    };

    struct DeviceReport {
        std::string camera_id;
        std::string ip;
        std::string connection_path; // "mac_discovery" | "direct_ip"
        uint64_t created_realtime_ns = 0;
        FactoryLoadResult factory_load;
        std::vector<std::pair<std::string, std::string> > identity;
        std::vector<AppliedFeature> applied;
        std::vector<AuditEntry> raw_audit;
        std::vector<std::pair<std::string, std::string> > transport;
        std::optional<double> frame_rate_min_hz, frame_rate_max_hz;
        std::optional<double> exposure_time_us, exposure_time_min_us, exposure_time_max_us;
        std::optional<std::string> black_level_register;
        std::optional<int64_t> sensor_digitization_bits;
        RuntimeShape runtime;
        PtpSummary ptp;
    };

    // Every key always present; unavailable values are null
    nlohmann::ordered_json BuildDeviceJson(const DeviceReport &r);

    // --- Telemetry ------------------------------------------------------------
    struct TelemetrySample {
        uint64_t hrt = 0; // host CLOCK_REALTIME ns (same key as idx.jsonl)
        std::optional<double> temp_sensor, temp_mainboard, temp_fpga;
        std::optional<int64_t> trig; // CounterValue[Counter0]
        bool trig_overflow = false;
        std::optional<int64_t> pause_rx; // aPAUSEMACCtrlFramesReceived (p.130)
        std::optional<std::string> ptp_status;
        std::optional<int64_t> ptp_accuracy;
    };

    // One JSONL row, without the trailing newline.
    std::string BuildTelemetryLine(const TelemetrySample &s);
} // namespace gox
