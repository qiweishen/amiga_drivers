#pragma once

#include <string>
#include <vector>

#include "config_util.h"


namespace asterx {
    using ConfigError = common::ConfigError;

    // One SBF stream definition pushed to the receiver
    struct SbfStream {
        int stream_id; // 1..10 — receiver-side Stream<id>
        std::vector<std::string> blocks; // block or group names, e.g. {"Measurements"}
        std::string interval; // e.g. "OnChange"
    };

    // One NMEA stream pushed to a fixed receiver port
    struct NmeaPinStream {
        int stream_id; // 1..10 — receiver-side NMEA Stream<id>
        std::string descriptor; // fixed connection descriptor, e.g. "COM2"
        std::vector<std::string> messages; // e.g. {"ZDA"}
        std::string interval; // e.g. "OnChange"
    };

    struct Vec3 {
        double x{0.0};
        double y{0.0};
        double z{0.0};
    };

    struct AttitudeOffset {
        double heading_deg{0.0};
        double pitch_deg{0.0};
    };

    struct ReceiverCapabilities {
        bool has_main{false};
        bool has_aux1{false};
    };

    // receiver.antenna
    struct AntennaSettings {
        // Exact names from "lstAntennaInfo, Overview" (p.66); Unknown = no PCV model
        std::string main_type{"Unknown"};
        std::string aux_type{"Unknown"};
        AttitudeOffset attitude_offset_deg{};
        Vec3 lever_arm_m{}; // IMU reference point -> main antenna ARP, vehicle frame
    };

    // receiver.gnss — values are the receiver's own list syntax
    struct GnssSettings {
        int cn0_mask_dbhz{0};
        int elevation_mask_deg{0};
        std::string satellite_tracking{"all"};
        std::string satellite_usage{"all"};
        std::string satellite_health_override{"none"};
        std::string signal_tracking{"all"};
        std::string signal_usage{"all"};
    };

    // receiver.imu
    struct ImuSettings {
        std::string startup_data_mode{"GnssTimeKnown"}; // Boot | GnssTimeKnown
        std::string orientation_mode{"SensorDefault"}; // SensorDefault | manual | fixed
        double theta_x_deg{0.0};
        double theta_y_deg{0.0};
        double theta_z_deg{0.0};
    };

    // receiver.time_system.pps
    struct PpsSettings {
        std::string interval{"sec1"};
        std::string polarity{"Low2High"}; // Low2High | High2Low
        double delay_ns{0.0};
        std::string time_scale{"GPS"}; // GPS | Galileo | BeiDou | GLONASS | UTC | RxClock
        int max_holdover_s{60};
        double pulse_width_ms{5.0};
    };

    // receiver.time_system
    struct TimeSystemSettings {
        bool ptp_server{false};
        bool ntp_server{false};
        PpsSettings pps{};
    };

    // receiver.ntrip
    struct NtripSettings {
        bool enabled{false};
        std::string caster;
        int port{2101};
        std::string username;
        std::string password;
        std::string mount_point;
        std::string version{"v2"}; // v1 | v2
        bool tls{false};
        std::string send_gga{"auto"}; // auto | off | sec1 | sec5 | sec10 | sec60
    };

    // receiver.navigation
    struct NavigationSettings {
        std::string pvt_mode{"all"}; // setPVTMode Rover: all | +list of StandAlone,SBAS,DGNSS,RTKFloat,RTKFixed,RTK
        std::string receiver_dynamics{"Moderate"}; // Max | High | Moderate | Low
        std::string vehicle_application{"Unknown"};
        std::string clock_sync_threshold{"usec500"}; // ClockSteering | usec500 | msec1..msec5
        bool multipath_mitigation{true};
        std::string ins_output_location{"POI1"}; // MainAnt | POI1
    };

    // receiver.warmup
    struct WarmupSettings {
        int min_uptime_s{1200}; // ReceiverStatus.UpTime; 0 = no uptime gate
        bool require_finetime{true}; // ReceiverStatus.RxState bit 6
    };

    struct ReceiverSettings {
        // Dedicated driver account (re-created by the factory login after every reset)
        std::string user{"admin"};
        std::string password{"septentrio"};

        // SBF streams on our own connection (required, from config-asterx.yaml)
        std::vector<SbfStream> sbf_streams{};

        // NMEA sentence output on fixed receiver ports (empty = feature off)
        std::vector<NmeaPinStream> nmea_streams{};

        AntennaSettings antenna{};
        GnssSettings gnss{};
        ImuSettings imu{};
        TimeSystemSettings time_system{};
        NtripSettings ntrip{};
        NavigationSettings navigation{};
        WarmupSettings warmup{};
    };

    enum class CommandKind {
        kPlain, // error reply -> configuration failure
        kCheckCapabilities, // parse ReceiverCapabilities; Aux1 is mandatory
        kListAntennaInfo, // lstAntennaInfo, Overview: the configured antenna names must exist
        kListConfig, // lstConfigFile, Current: every non-permanent line must be ours
        kVerifyEcho, // reply line `verify_key` must start with `verify_fields` ("" = don't care)
    };

    struct Command {
        std::string text;
        CommandKind kind{CommandKind::kPlain};
        std::string verify_key; // VerifyEcho
        std::vector<std::string> verify_fields; // VerifyEcho
    };

    struct ConfigLine {
        bool permanent{false}; // "#" prefix: survives a reset to default configuration
        std::string command_name; // e.g. setSBFOutput
        std::string full_line;
    };

    // Fixed-point decimal as the receiver expects it; |v| below half a unit prints as 0
    std::string FmtCommandDecimal(double v, int digits = 3);

    // Output intervals the driver may request
    bool IsValidSbfInterval(const std::string &interval);

    bool IsValidNmeaMessage(const std::string &message);

    // Fixed receiver ports a pin stream may target
    bool IsValidPinDescriptor(const std::string &descriptor);

    // "all" | "none" (when allowed) | "+"-joined constellation aliases / satellite ids
    bool IsValidSatelliteList(const std::string &value, bool allow_none, int gps_max = 39);

    // "all" | "+"-joined signal names (constellation aliases only when allowed)
    bool IsValidSignalList(const std::string &value, bool allow_constellation_alias);

    std::string BuildSbfOutputCommand(const SbfStream &stream, const std::string &descriptor);

    std::string BuildNmeaOutputCommand(const NmeaPinStream &stream);

    std::string BuildInsAntLeverArmCommand(Vec3 lever_arm_m);

    std::string BuildImuOrientationCommand(const ImuSettings &imu);

    // setAntennaType, <Main|Aux1>, <name> — quoted when the name contains whitespace
    std::string BuildAntennaTypeCommand(const std::string &which, const std::string &type);

    std::string BuildPpsCommand(const PpsSettings &pps);

    // Password escaping of p.64: " ' $ & , -> %%DQ %%SQ %%DL %%AM %%CM
    std::string EscapeNtripPassword(const std::string &raw);

    // setNtripSettings (+ setNtripTlsSettings when enabled)
    std::vector<std::string> BuildNtripCommands(const NtripSettings &ntrip);

    // Full ordered configure sequence (docs/DEVICE_CONFIG.md). Re-run verbatim
    // after every reconnect.
    std::vector<Command> BuildCommandList(const ReceiverSettings &settings, const std::string &descriptor);

    ReceiverCapabilities ParseReceiverCapabilitiesReply(const std::string &reply);

    // lstConfigFile text in either shape ("$R;" header with or without "---->",
    // "$-- BLOCK i / n" frames); header lines are dropped
    std::vector<ConfigLine> ParseConfigFileListing(const std::string &text);

    // "#setIPSettings, DHCP, ..." -> "setIPSettings"; "$R: getPVTMode" -> "getPVTMode"
    std::string CommandNameOf(const std::string &line);

    // Names of every set* command the driver sends (de-duplicated)
    std::vector<std::string> DriverCommandWhitelist(const std::vector<Command> &cmds);

    // Throws ConfigError when a non-permanent line has a command name outside the whitelist
    void VerifyConfigListing(const std::vector<ConfigLine> &lines, const std::vector<std::string> &whitelist);

    // lstAntennaInfo, Overview text -> the ID="..." names, in order
    std::vector<std::string> ParseAntennaOverview(const std::string &text);

    // Names sharing the first four characters (case-insensitive), at most `max`
    std::vector<std::string> suggest_antenna_names(const std::vector<std::string> &ids, const std::string &name,
                                                   std::size_t max = 5);

    // Every configured type other than Unknown must appear in `ids` exactly
    void VerifyAntennaTypes(const std::vector<std::string> &ids, const AntennaSettings &antenna);

    void ValidateReceiverSettings(const ReceiverSettings &settings);

    std::string verify_first_fields(const std::string &reply, const std::string &key,
                                    const std::vector<std::string> &expected);

    // Redact the NTRIP password
    std::string RedactCmd(const std::string &cmd);
} // namespace asterx
