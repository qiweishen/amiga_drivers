#include "commands.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <unordered_set>

#include "string_util.h"


namespace asterx {
    namespace {
        constexpr std::size_t kMaxAsciiCommandLength = 2000;
        constexpr std::size_t kMaxUserLength = 16; // login UserName (p.87)
        constexpr std::size_t kMaxPasswordLength = 32; // login Password (p.87)

        using common::StringUtil::EqualsCi;
        using common::StringUtil::Join;
        using common::StringUtil::OneLine;
        using common::StringUtil::Split;
        using common::StringUtil::SplitTrim;
        using common::StringUtil::StartsWithCi;
        using common::StringUtil::ToLower;
        using common::StringUtil::Trim;

        // Receiver enumerations
        const std::vector<std::string> kConstellations{"GPS", "GLONASS", "GALILEO", "SBAS", "BEIDOU", "QZSS"};
        const std::vector<std::string> kSignals{
            "GPSL1CA", "GPSL2PY", "GPSL2C", "GPSL5", "GPSL1C",
            "GLOL1CA", "GLOL2P", "GLOL2CA",
            "GALE1BC", "GALE5a", "GALE5b", "GALE5",
            "GEOL1", "GEOL5",
            "BDSB1I", "BDSB2I", "BDSB3I",
            "QZSL1CA", "QZSL2C", "QZSL5", "QZSL1C", "QZSL1CB",
        };
        const std::vector<std::string> kPpsIntervals{
            "off", "msec10", "msec20", "msec50", "msec100", "msec200", "msec250", "msec500",
            "sec1", "sec2", "sec4", "sec5", "sec10", "sec30", "sec60",
        };
        const std::vector<std::string> kPpsPolarities{"Low2High", "High2Low"};
        const std::vector<std::string> kTimeScales{"GPS", "Galileo", "BeiDou", "GLONASS", "UTC", "RxClock"};
        const std::vector<std::string> kSendGga{"auto", "off", "sec1", "sec5", "sec10", "sec60"};
        const std::vector<std::string> kNtripVersions{"v1", "v2"};
        const std::vector<std::string> kPvtModes{"StandAlone", "SBAS", "DGNSS", "RTKFloat", "RTKFixed", "RTK"};
        const std::vector<std::string> kDynamics{"Max", "High", "Moderate", "Low"};
        const std::vector<std::string> kVehicleApplications{
            "Unknown", "RoadVehicle", "HaulTruck", "Tractor", "TerminalTractor", "ReachStacker", "LiftTruck",
            "Excavator", "Loader", "Grader", "Dozer", "RoadRobot", "OffroadRobot", "FixedWing", "Multirotor",
            "USV", "RailVehicle",
        };
        const std::vector<std::string> kClockSyncThresholds{
            "ClockSteering", "usec500", "msec1", "msec2", "msec3", "msec4", "msec5",
        };
        const std::vector<std::string> kInsOutputLocations{"MainAnt", "POI1"};
        const std::vector<std::string> kImuStartupModes{"Boot", "GnssTimeKnown"};
        const std::vector<std::string> kImuOrientationModes{"SensorDefault", "manual", "fixed"};

        // GPS PRN ceiling per command: setSatelliteUsage stops at G32 (p.128),
        // setSatelliteTracking / setSatelliteHealthOverride go to G39 (p.96/p.127)
        constexpr int kGpsMaxTracking = 39;
        constexpr int kGpsMaxUsage = 32;


        bool InListCi(const std::vector<std::string> &list, const std::string &value) {
            return std::any_of(list.begin(), list.end(), [&value](const std::string &v) { return EqualsCi(v, value); });
        }


        const char *OnOff(bool v) { return v ? "on" : "off"; }


        bool HasWhitespace(const std::string &s) {
            return std::any_of(s.begin(), s.end(), [](unsigned char c) { return std::isspace(c) != 0; });
        }


        std::string QuoteIfWhitespace(const std::string &s) {
            return HasWhitespace(s) ? "\"" + s + "\"" : s;
        }


        int ParseIntPrefix(const std::string &s, const std::string &field) {
            std::istringstream is(Trim(s));
            int v = 0;
            if (!(is >> v)) {
                throw ConfigError("could not parse integer field '" + field + "' from '" + s + "'");
            }
            return v;
        }


        // Payload of the first line whose first field is `key`
        std::string FindConfigLinePayload(const std::string &reply, const std::string &key) {
            std::istringstream lines(reply);
            std::string line;
            const std::string prefix = key + ",";
            while (std::getline(lines, line)) {
                line = Trim(line);
                if (StartsWithCi(line, prefix)) {
                    return Trim(line.substr(prefix.size()));
                }
            }
            throw ConfigError("receiver reply did not contain '" + key + "' line: " + OneLine(Trim(reply)));
        }


        // getReceiverCapabilities answers with one logical line that the receiver
        // may wrap; join everything after the key into a single field list.
        std::string CompactPayloadAfterKey(const std::string &reply, const std::string &key) {
            const std::string marker = key + ",";
            const auto pos = reply.find(marker);
            if (pos == std::string::npos) {
                throw ConfigError("receiver reply did not contain '" + key + "' payload: " + reply);
            }

            std::string payload = reply.substr(pos + marker.size());
            const auto prompt = payload.find('>');
            if (prompt != std::string::npos) {
                payload = payload.substr(0, prompt);
            }
            for (char &c: payload) {
                if (c == '\r' || c == '\n' || c == '\t') c = ' ';
            }
            return payload;
        }


        void EnforceCommandLength(const std::string &cmd) {
            if (cmd.size() > kMaxAsciiCommandLength) {
                throw ConfigError("ASCII command exceeds Septentrio 2000-character limit");
            }
        }


        void EnforceDescriptor(const std::string &descriptor) {
            if (descriptor.empty() ||
                descriptor.find(',') != std::string::npos ||
                descriptor.find(' ') != std::string::npos) {
                throw ConfigError("invalid connection descriptor '" + descriptor + "'");
            }
        }


        std::string SetVec3Command(const std::string &name, Vec3 v) {
            return name + ", " + FmtCommandDecimal(v.x) + ", " + FmtCommandDecimal(v.y) + ", " +
                   FmtCommandDecimal(v.z);
        }


        std::string Unquote(const std::string &s) {
            std::string out = Trim(s);
            if (out.size() >= 2 && out.front() == '"' && out.back() == '"') {
                out = out.substr(1, out.size() - 2);
            }
            return out;
        }


        // G01..G<gps_max> (36, 38, 39 for tracking; 32 for usage) | R01..R30 |
        // E01..E36 | S120..S158 | C01..C63 | J01..J10
        bool IsSatelliteId(const std::string &tok, int gps_max) {
            if (tok.size() < 3 || tok.size() > 4) {
                return false;
            }
            const char sys = static_cast<char>(std::toupper(static_cast<unsigned char>(tok[0])));
            const std::string digits = tok.substr(1);
            if (!std::all_of(digits.begin(), digits.end(), [](unsigned char c) { return std::isdigit(c) != 0; })) {
                return false;
            }
            const int n = std::stoi(digits);
            switch (sys) {
                case 'G':
                    if (digits.size() != 2) return false;
                    // The tracking list has a gap: G37 does not exist (p.96)
                    return gps_max >= 38 ? ((n >= 1 && n <= 36) || n == 38 || n == 39) : (n >= 1 && n <= gps_max);
                case 'R': return digits.size() == 2 && n >= 1 && n <= 30;
                case 'E': return digits.size() == 2 && n >= 1 && n <= 36;
                case 'S': return digits.size() == 3 && n >= 120 && n <= 158;
                case 'C': return digits.size() == 2 && n >= 1 && n <= 63;
                case 'J': return digits.size() == 2 && n >= 1 && n <= 10;
                default: return false;
            }
        }


        template<typename Pred>
        bool ValidPlusList(const std::string &value, bool allow_none, Pred token_ok) {
            const std::string v = Trim(value);
            if (v.empty()) {
                return false;
            }
            if (EqualsCi(v, "all")) {
                return true;
            }
            if (EqualsCi(v, "none")) {
                return allow_none;
            }
            for (const auto &raw: Split(v, '+')) {
                const std::string tok = Trim(raw);
                // Relative forms (+X / -X) are not used: the list is absolute after RxDefault
                if (tok.empty() || tok[0] == '+' || tok[0] == '-' || !token_ok(tok)) {
                    return false;
                }
            }
            return true;
        }


        // Message charset of p.64 (the five escapable characters are handled separately)
        bool ReceiverTextOk(const std::string &s, bool allow_escapable) {
            static const std::string extra = " !#%@()*+-./:;<=>?[\\]^_`{|}~";
            static const std::string escapable = "\"'$&,";
            for (const unsigned char c: s) {
                if (std::isalnum(c) || extra.find(static_cast<char>(c)) != std::string::npos) {
                    continue;
                }
                if (allow_escapable && escapable.find(static_cast<char>(c)) != std::string::npos) {
                    continue;
                }
                return false;
            }
            return true;
        }


        void RequireEnum(const std::string &value, const std::vector<std::string> &allowed, const char *key) {
            if (!InListCi(allowed, value)) {
                throw ConfigError(
                    std::string(key) + " must be one of " + Join(allowed, '|') + " (got '" + value + "')");
            }
        }


        void RequireRange(double v, double lo, double hi, const char *key) {
            if (!(v >= lo && v <= hi)) {
                std::ostringstream os;
                os << key << " must be in [" << lo << ", " << hi << "] (got " << v << ")";
                throw ConfigError(os.str());
            }
        }
    } // namespace


    std::string FmtCommandDecimal(double v, int digits) {
        if (std::fabs(v) < 0.5 * std::pow(10.0, -digits)) {
            v = 0.0;
        }
        std::ostringstream os;
        os << std::fixed << std::setprecision(digits) << v;
        return os.str();
    }


    bool IsValidSbfInterval(const std::string &interval) {
        // setSBFOutput / setNMEAOutput Interval column (p.193, p.202) without
        // "off": the driver only ever configures streams it wants data from.
        static const std::unordered_set<std::string> allowed{
            "onchange",
            "msec5", "msec10", "msec20", "msec40", "msec50",
            "msec100", "msec200", "msec500",
            "sec1", "sec2", "sec5", "sec10", "sec15", "sec30", "sec60",
            "min2", "min5", "min10", "min15", "min30", "min60",
        };
        return allowed.find(ToLower(interval)) != allowed.end();
    }


    bool IsValidNmeaMessage(const std::string &message) {
        // Messages column of the setNMEAOutput table (p.193);
        // note that common sentences absent there (e.g. GSV) are NOT supported.
        static const std::unordered_set<std::string> allowed{
            "gga", "gll", "gns", "gst", "hdt", "rmc",
            "vtg", "zda", "hrp", "ths", "pashr",
        };
        return allowed.find(ToLower(message)) != allowed.end();
    }


    bool IsValidPinDescriptor(const std::string &descriptor) {
        // Cd column of the setNMEAOutput table (p.193), identical to the
        // setDataInOut (p.168) and setSBFOutput (p.202) columns. This receiver
        // has two COM ports (setCOMSettings); DSKx is a disk id, not a Cd.
        static const std::unordered_set<std::string> allowed{
            "com1", "com2",
            "usb1", "usb2",
            "ip10", "ip11", "ip12", "ip13", "ip14", "ip15", "ip16", "ip17",
            "ntr1", "ntr2", "ntr3",
            "ips1", "ips2", "ips3", "ips4", "ips5",
            "ipr1", "ipr2", "ipr3", "ipr4", "ipr5",
            "log1", "log2",
        };
        return allowed.find(ToLower(descriptor)) != allowed.end();
    }


    bool IsValidSatelliteList(const std::string &value, bool allow_none, int gps_max) {
        return ValidPlusList(value, allow_none, [gps_max](const std::string &tok) {
            return InListCi(kConstellations, tok) || IsSatelliteId(tok, gps_max);
        });
    }


    bool IsValidSignalList(const std::string &value, bool allow_constellation_alias) {
        return ValidPlusList(value, false, [allow_constellation_alias](const std::string &tok) {
            return InListCi(kSignals, tok) || (allow_constellation_alias && InListCi(kConstellations, tok));
        });
    }


    std::string BuildNmeaOutputCommand(const NmeaPinStream &stream) {
        if (stream.stream_id < 1 || stream.stream_id > 10) {
            throw ConfigError("NMEA stream id must be in range 1..10");
        }
        if (!IsValidPinDescriptor(stream.descriptor)) {
            throw ConfigError("Unsupported NMEA pin descriptor '" + stream.descriptor + "'");
        }
        if (stream.messages.empty()) {
            throw ConfigError("NMEA pin stream must contain at least one message");
        }
        for (const auto &m: stream.messages) {
            if (!IsValidNmeaMessage(m)) {
                throw ConfigError("Unsupported NMEA message '" + m + "'");
            }
        }
        if (!IsValidSbfInterval(stream.interval)) {
            throw ConfigError("Unsupported NMEA interval '" + stream.interval + "'");
        }

        const std::string cmd =
                "setNMEAOutput, Stream" + std::to_string(stream.stream_id) +
                ", " + stream.descriptor +
                ", " + Join(stream.messages, '+') +
                ", " + stream.interval;
        EnforceCommandLength(cmd);
        return cmd;
    }


    std::string BuildSbfOutputCommand(const SbfStream &stream, const std::string &descriptor) {
        if (stream.stream_id < 1 || stream.stream_id > 10) {
            throw ConfigError("SBF stream id must be in range 1..10");
        }
        EnforceDescriptor(descriptor);
        if (stream.blocks.empty()) {
            throw ConfigError("SBF stream must contain at least one block");
        }
        if (!IsValidSbfInterval(stream.interval)) {
            throw ConfigError("Unsupported SBF interval '" + stream.interval + "'");
        }

        const std::string cmd =
                "setSBFOutput, Stream" + std::to_string(stream.stream_id) +
                ", " + descriptor +
                ", " + Join(stream.blocks, '+') +
                ", " + stream.interval;
        EnforceCommandLength(cmd);
        return cmd;
    }


    std::string BuildInsAntLeverArmCommand(Vec3 lever_arm_m) {
        return SetVec3Command("setINSAntLeverArm", lever_arm_m);
    }


    std::string BuildImuOrientationCommand(const ImuSettings &imu) {
        if (EqualsCi(imu.orientation_mode, "SensorDefault")) {
            return "setIMUOrientation, SensorDefault";
        }
        return "setIMUOrientation, " + imu.orientation_mode + ", " +
               FmtCommandDecimal(imu.theta_x_deg) + ", " +
               FmtCommandDecimal(imu.theta_y_deg) + ", " +
               FmtCommandDecimal(imu.theta_z_deg);
    }


    std::string BuildAntennaTypeCommand(const std::string &which, const std::string &type) {
        return "setAntennaType, " + which + ", " + QuoteIfWhitespace(type);
    }


    std::string BuildPpsCommand(const PpsSettings &pps) {
        // Delay allows 2 decimals, PulseWidth 6 (p.156); more decimals are rejected (p.64)
        return "setPPSParameters, " + pps.interval +
               ", " + pps.polarity +
               ", " + FmtCommandDecimal(pps.delay_ns, 2) +
               ", " + pps.time_scale +
               ", " + std::to_string(pps.max_holdover_s) +
               ", " + FmtCommandDecimal(pps.pulse_width_ms, 6);
    }


    std::string EscapeNtripPassword(const std::string &raw) {
        std::string out;
        out.reserve(raw.size());
        for (const char c: raw) {
            switch (c) {
                case '"': out += "%%DQ";
                    break;
                case '\'': out += "%%SQ";
                    break;
                case '$': out += "%%DL";
                    break;
                case '&': out += "%%AM";
                    break;
                case ',': out += "%%CM";
                    break;
                default: out += c;
            }
        }
        return out;
    }


    std::vector<std::string> BuildNtripCommands(const NtripSettings &ntrip) {
        if (!ntrip.enabled) {
            return {"setNtripSettings, NTR1, off"};
        }
        const std::string settings =
                "setNtripSettings, NTR1, Client, " + ntrip.caster +
                ", " + std::to_string(ntrip.port) +
                ", " + QuoteIfWhitespace(ntrip.username) +
                ", " + QuoteIfWhitespace(EscapeNtripPassword(ntrip.password)) +
                ", " + ntrip.mount_point +
                ", " + ntrip.version +
                ", " + ntrip.send_gga;
        EnforceCommandLength(settings);
        return {settings, std::string("setNtripTlsSettings, NTR1, ") + OnOff(ntrip.tls) + ", \"\""};
    }


    std::vector<Command> BuildCommandList(const ReceiverSettings &settings, const std::string &descriptor) {
        EnforceDescriptor(descriptor);

        std::vector<Command> cmds;
        cmds.reserve(64);

        // Long form (p.18): creates User1 on a receiver without a full-control account, plain login otherwise
        const std::string login = "login, " + settings.user + ", " + settings.password + ", RxAdmin, S3pt3ntr10";

        const auto plain = [&cmds](std::string text) {
            cmds.push_back({std::move(text), CommandKind::kPlain, {}, {}});
        };
        // set + get with a first-fields readback ("" = don't care)
        const auto set_and_echo = [&cmds](std::string set_text, std::string get_text, std::string key,
                                          std::vector<std::string> fields) {
            cmds.push_back({std::move(set_text), CommandKind::kPlain, {}, {}});
            Command get;
            get.text = std::move(get_text);
            get.kind = CommandKind::kVerifyEcho;
            get.verify_key = std::move(key);
            get.verify_fields = std::move(fields);
            cmds.push_back(std::move(get));
        };

        // 1) Authenticate.
        plain(login);

        // 2) Reset the running configuration to the receiver defaults.
        plain("exeCopyConfigFile, RxDefault, Current");

        // 3) The reset wiped the accounts and this session's authorisation; the same login re-creates User1.
        plain(login);

        // 4) Check dual-antenna and multi-band capacities.
        cmds.push_back({"getReceiverCapabilities", CommandKind::kCheckCapabilities, {}, {}});

        // 5) Antenna names must exist on the receiver (read-only, before they are used).
        const bool check_antennas = !EqualsCi(settings.antenna.main_type, "Unknown") ||
                                    !EqualsCi(settings.antenna.aux_type, "Unknown");
        if (check_antennas) {
            cmds.push_back({"lstAntennaInfo, Overview", CommandKind::kListAntennaInfo, {}, {}});
        }

        // 6) Time: PPS output, NTP/PTP servers.
        const auto &ts = settings.time_system;
        set_and_echo(BuildPpsCommand(ts.pps), "getPPSParameters", "PPSParameters", {
                         ts.pps.interval, ts.pps.polarity, FmtCommandDecimal(ts.pps.delay_ns, 2),
                         ts.pps.time_scale, std::to_string(ts.pps.max_holdover_s),
                         FmtCommandDecimal(ts.pps.pulse_width_ms, 6)
                     });
        set_and_echo(std::string("setNTPServer, ") + OnOff(ts.ntp_server), "getNTPServer",
                     "NTPServer", {OnOff(ts.ntp_server)});
        set_and_echo(std::string("setPTPServer, ") + OnOff(ts.ptp_server), "getPTPServer",
                     "PTPServer", {OnOff(ts.ptp_server)});

        // 7) NMEA sentence output on fixed receiver ports (external hardware).
        std::vector<std::string> nmea_ports;
        for (const auto &s: settings.nmea_streams) {
            const bool port_enabled = std::any_of(
                nmea_ports.begin(), nmea_ports.end(),
                [&s](const std::string &p) { return EqualsCi(p, s.descriptor); });
            if (!port_enabled) {
                nmea_ports.push_back(s.descriptor);
                // Physical port modes are user-configurable, so only the port field is deterministic
                set_and_echo("setDataInOut, " + s.descriptor + ", , +NMEA", "getDataInOut, " + s.descriptor,
                             "DataInOut", {s.descriptor});
            }
            set_and_echo(BuildNmeaOutputCommand(s), "getNMEAOutput, Stream" + std::to_string(s.stream_id),
                         "NMEAOutput",
                         {"Stream" + std::to_string(s.stream_id), s.descriptor, "", s.interval});
        }

        // 8) Enable SBF output on our own connection.
        // Only the port field is deterministic (the output mode may already carry NMEA)
        set_and_echo("setDataInOut, " + descriptor + ", , +SBF", "getDataInOut, " + descriptor, "DataInOut",
                     {descriptor});

        // 9) Antennas: PCV model, attitude offset, IMU -> main antenna lever arm.
        const auto &a = settings.antenna;
        set_and_echo(BuildAntennaTypeCommand("Main", a.main_type), "getAntennaType, Main",
                     "AntennaType", {"Main", a.main_type});
        set_and_echo(BuildAntennaTypeCommand("Aux1", a.aux_type), "getAntennaType, Aux1",
                     "AntennaType", {"Aux1", a.aux_type});
        set_and_echo("setAttitudeOffset, " + FmtCommandDecimal(a.attitude_offset_deg.heading_deg) +
                     ", " + FmtCommandDecimal(a.attitude_offset_deg.pitch_deg),
                     "getAttitudeOffset", "AttitudeOffset",
                     {
                         FmtCommandDecimal(a.attitude_offset_deg.heading_deg),
                         FmtCommandDecimal(a.attitude_offset_deg.pitch_deg)
                     });
        set_and_echo(BuildInsAntLeverArmCommand(a.lever_arm_m), "getINSAntLeverArm", "INSAntLeverArm",
                     {
                         FmtCommandDecimal(a.lever_arm_m.x), FmtCommandDecimal(a.lever_arm_m.y),
                         FmtCommandDecimal(a.lever_arm_m.z)
                     });

        // 10) GNSS tracking and usage.
        const auto &g = settings.gnss;
        const auto none_or_blank = [](const std::string &v) {
            return EqualsCi(v, "none") ? std::string("none") : std::string();
        };
        set_and_echo("setCN0Mask, all, " + std::to_string(g.cn0_mask_dbhz), "getCN0Mask", "CN0Mask",
                     {"", std::to_string(g.cn0_mask_dbhz)});
        set_and_echo("setElevationMask, all, " + std::to_string(g.elevation_mask_deg), "getElevationMask",
                     "ElevationMask", {"", std::to_string(g.elevation_mask_deg)});
        set_and_echo("setSatelliteTracking, " + g.satellite_tracking, "getSatelliteTracking", "SatelliteTracking",
                     {none_or_blank(g.satellite_tracking)});
        set_and_echo("setSatelliteUsage, " + g.satellite_usage, "getSatelliteUsage", "SatelliteUsage",
                     {none_or_blank(g.satellite_usage)});
        set_and_echo("setSatelliteHealthOverride, " + g.satellite_health_override + ", " + g.satellite_health_override,
                     "getSatelliteHealthOverride", "SatelliteHealthOverride",
                     {none_or_blank(g.satellite_health_override), none_or_blank(g.satellite_health_override)});
        set_and_echo("setSignalTracking, " + g.signal_tracking, "getSignalTracking", "SignalTracking", {});
        set_and_echo("setSignalUsage, " + g.signal_usage + ", " + g.signal_usage, "getSignalUsage", "SignalUsage", {});

        // 11) IMU.
        const auto &imu = settings.imu;
        set_and_echo("setIMUStartupDataMode, " + imu.startup_data_mode, "getIMUStartupDataMode",
                     "IMUStartupDataMode", {imu.startup_data_mode});
        set_and_echo(BuildImuOrientationCommand(imu), "getIMUOrientation", "IMUOrientation",
                     EqualsCi(imu.orientation_mode, "SensorDefault")
                         ? std::vector<std::string>{"SensorDefault"}
                         : std::vector<std::string>{
                             imu.orientation_mode, FmtCommandDecimal(imu.theta_x_deg),
                             FmtCommandDecimal(imu.theta_y_deg), FmtCommandDecimal(imu.theta_z_deg)
                         });

        // 12) Dual-antenna attitude is always on.
        set_and_echo("setGNSSAttitude, MultiAntenna", "getGNSSAttitude", "GNSSAttitude", {"MultiAntenna"});

        // 13) Navigation: what the baseline solution is made of.
        const auto &nav = settings.navigation;
        set_and_echo("setPVTMode, Rover, " + nav.pvt_mode, "getPVTMode", "PVTMode", {"Rover"});
        set_and_echo("setReceiverDynamics, " + nav.receiver_dynamics, "getReceiverDynamics",
                     "ReceiverDynamics", {nav.receiver_dynamics});
        set_and_echo("setVehicleApplication, " + nav.vehicle_application, "getVehicleApplication",
                     "VehicleApplication", {nav.vehicle_application});
        set_and_echo("setClockSyncThreshold, " + nav.clock_sync_threshold, "getClockSyncThreshold",
                     "ClockSyncThreshold", {nav.clock_sync_threshold});
        set_and_echo(std::string("setMultipathMitigation, ") + OnOff(nav.multipath_mitigation) + ", " +
                     OnOff(nav.multipath_mitigation),
                     "getMultipathMitigation", "MultipathMitigation",
                     {OnOff(nav.multipath_mitigation), OnOff(nav.multipath_mitigation)});
        set_and_echo("setINSNavConfig, on, all, " + nav.ins_output_location,
                     "getINSNavConfig", "INSNavConfig", {"on", "", nav.ins_output_location});

        // 14) NTRIP client.
        {
            const auto &n = settings.ntrip;
            const auto ntrip_cmds = BuildNtripCommands(n);
            set_and_echo(ntrip_cmds[0], "getNtripSettings, NTR1", "NtripSettings",
                         n.enabled
                             ? std::vector<std::string>{
                                 "NTR1", "Client", n.caster, std::to_string(n.port), n.username, "",
                                 n.mount_point, n.version, n.send_gga
                             }
                             : std::vector<std::string>{"NTR1", "off"});
            if (ntrip_cmds.size() > 1) {
                set_and_echo(ntrip_cmds[1], "getNtripTlsSettings, NTR1", "NtripTlsSettings",
                             {"NTR1", OnOff(n.tls)});
            }
        }

        // 15) Proof: the running configuration now holds only our commands
        cmds.push_back({"lstConfigFile, Current", CommandKind::kListConfig, {}, {}});

        // 16) SBF stream membership + intervals, targeted at our own connection.
        for (const auto &s: settings.sbf_streams) {
            set_and_echo(BuildSbfOutputCommand(s, descriptor), "getSBFOutput, Stream" + std::to_string(s.stream_id),
                         "SBFOutput", {"Stream" + std::to_string(s.stream_id), descriptor, "", s.interval});
        }

        return cmds;
    }


    std::string CommandNameOf(const std::string &line) {
        std::string s = Trim(line);
        // "$R: getPVTMode, ..." / "$R; lcf, Current": drop the reply marker
        if (s.size() >= 3 && s.compare(0, 2, "$R") == 0) {
            s = Trim(s.substr(3));
        }
        if (!s.empty() && s[0] == '#') {
            s = Trim(s.substr(1));
        }
        const auto end = s.find_first_of(", \t:");
        return end == std::string::npos ? s : s.substr(0, end);
    }


    std::vector<ConfigLine> ParseConfigFileListing(const std::string &text) {
        std::vector<ConfigLine> lines;
        std::istringstream in(text);
        std::string raw;
        while (std::getline(in, raw)) {
            const std::string line = Trim(raw);
            // "$R; lcf, Current" and "$-- BLOCK i / n" headers; SsnRx already
            // strips the prompts and the "---->" pseudo-prompts.
            if (line.empty() || line[0] == '$') {
                continue;
            }
            ConfigLine entry;
            entry.permanent = line[0] == '#';
            entry.command_name = CommandNameOf(line);
            entry.full_line = line;
            if (entry.command_name.empty()) {
                continue;
            }
            lines.push_back(std::move(entry));
        }
        return lines;
    }


    std::vector<std::string> DriverCommandWhitelist(const std::vector<Command> &cmds) {
        // The factory login re-creates the driver account, which the receiver
        // stores as a setUserAccessLevel line in the running configuration.
        std::vector<std::string> names{"setUserAccessLevel"};
        for (const auto &c: cmds) {
            const std::string name = CommandNameOf(c.text);
            if (!StartsWithCi(name, "set")) {
                continue;
            }
            const bool known = std::any_of(names.begin(), names.end(),
                                           [&name](const std::string &n) { return EqualsCi(n, name); });
            if (!known) {
                names.push_back(name);
            }
        }
        return names;
    }


    void VerifyConfigListing(const std::vector<ConfigLine> &lines, const std::vector<std::string> &whitelist) {
        std::vector<std::string> foreign;
        for (const auto &l: lines) {
            if (l.permanent) {
                continue;
            }
            const bool ours = std::any_of(whitelist.begin(), whitelist.end(),
                                          [&l](const std::string &n) { return EqualsCi(n, l.command_name); });
            if (!ours) {
                foreign.push_back(RedactCmd(l.full_line));
            }
        }
        if (!foreign.empty()) {
            throw ConfigError("running configuration contains " + std::to_string(foreign.size()) +
                              " foreign command(s): " + Join(foreign, ';'));
        }
    }


    std::vector<std::string> ParseAntennaOverview(const std::string &text) {
        // <Antenna ID="AERAT2775_159   SPKE"/> lines, possibly split over $-- BLOCK frames
        std::vector<std::string> ids;
        static constexpr std::string_view kMarker = "ID=\"";
        for (std::size_t pos = text.find(kMarker); pos != std::string::npos; pos = text.find(kMarker, pos)) {
            const std::size_t start = pos + kMarker.size();
            const std::size_t end = text.find('"', start);
            if (end == std::string::npos) {
                break;
            }
            ids.push_back(text.substr(start, end - start));
            pos = end + 1;
        }
        return ids;
    }


    std::vector<std::string> suggest_antenna_names(const std::vector<std::string> &ids, const std::string &name,
                                                   std::size_t max) {
        std::vector<std::string> out;
        const std::string prefix = ToLower(Trim(name)).substr(0, 4);
        if (prefix.empty()) {
            return out;
        }
        for (const auto &id: ids) {
            if (out.size() >= max) {
                break;
            }
            if (ToLower(id).compare(0, prefix.size(), prefix) == 0) {
                out.push_back(id);
            }
        }
        return out;
    }


    void VerifyAntennaTypes(const std::vector<std::string> &ids, const AntennaSettings &antenna) {
        const auto check = [&ids](const char *key, const std::string &type) {
            if (EqualsCi(type, "Unknown")) {
                return;
            }
            if (std::find(ids.begin(), ids.end(), type) != ids.end()) {
                return;
            }
            std::string msg = std::string(key) + " '" + type + "' is not a name known to the receiver (" +
                              std::to_string(ids.size()) + " names from lstAntennaInfo, Overview)";
            const auto close = suggest_antenna_names(ids, type);
            if (!close.empty()) {
                msg += "; closest: '" + Join(close, '|') + "'";
            }
            msg += " — copy the name exactly (case and spaces), or use Unknown";
            throw ConfigError(msg);
        };
        check("receiver.antenna.main_antenna_type", antenna.main_type);
        check("receiver.antenna.aux_antenna_type", antenna.aux_type);
    }


    ReceiverCapabilities ParseReceiverCapabilitiesReply(const std::string &reply) {
        const std::string payload = CompactPayloadAfterKey(reply, "ReceiverCapabilities");
        const auto fields = SplitTrim(payload, ',');
        if (fields.size() < 7) {
            throw ConfigError("ReceiverCapabilities reply had too few fields: " + OneLine(Trim(reply)));
        }

        ReceiverCapabilities caps;
        for (const auto &antenna: SplitTrim(fields[0], '+')) {
            if (EqualsCi(antenna, "Main")) {
                caps.has_main = true;
            }
            if (EqualsCi(antenna, "Aux1")) {
                caps.has_aux1 = true;
            }
        }

        return caps;
    }


    std::string verify_first_fields(const std::string &reply, const std::string &key,
                                    const std::vector<std::string> &expected) {
        const std::string payload = FindConfigLinePayload(reply, key);
        const auto fields = SplitTrim(payload, ',');
        if (fields.size() < expected.size()) {
            throw ConfigError(key + " reply had " + std::to_string(fields.size()) + " field(s), expected at least " +
                              std::to_string(expected.size()) + ": " + OneLine(Trim(reply)));
        }
        for (std::size_t i = 0; i < expected.size(); ++i) {
            if (expected[i].empty()) {
                continue;
            }
            // The receiver quotes strings containing spaces (e.g. antenna names)
            if (!EqualsCi(Unquote(fields[i]), expected[i])) {
                throw ConfigError(key + " field " + std::to_string(i + 1) + " reads back '" + fields[i] +
                                  "', expected '" + expected[i] + "'");
            }
        }
        return payload;
    }


    void ValidateReceiverSettings(const ReceiverSettings &settings) {
        // An empty name/password would DELETE the account (setUserAccessLevel, p.90);
        // RxAdmin is the reserved factory name; the login command is sent without
        // escaping, so the credential charset is the plain message charset (p.64).
        const auto credential = [](const char *key, const std::string &v, std::size_t max_length) {
            if (v.empty() || v.size() > max_length) {
                throw ConfigError(std::string(key) + " must be 1.." + std::to_string(max_length) + " characters (p.87)");
            }
            if (!ReceiverTextOk(v, false)) {
                throw ConfigError(std::string(key) +
                                  " contains a character the receiver does not accept in a command line (p.64): "
                                  "letters, digits and !#%@()*+-./:;<=>?[\\]^_`{|}~");
            }
        };
        credential("device.user", settings.user, kMaxUserLength);
        credential("device.password", settings.password, kMaxPasswordLength);
        if (EqualsCi(settings.user, "RxAdmin")) {
            throw ConfigError("device.user must not be the reserved factory name RxAdmin (p.90)");
        }

        // antenna
        const auto &a = settings.antenna;
        const auto antenna_name = [](const char *key, const std::string &type) {
            if (type.empty() || type.size() > 20 || type.find('"') != std::string::npos ||
                type.find(',') != std::string::npos) {
                throw ConfigError(std::string(key) +
                                  " must be 1..20 characters without ',' or '\"' (an exact name from "
                                  "lstAntennaInfo, Overview, or Unknown)");
            }
        };
        antenna_name("receiver.antenna.main_antenna_type", a.main_type);
        antenna_name("receiver.antenna.aux_antenna_type", a.aux_type);
        RequireRange(a.lever_arm_m.x, -100.0, 100.0, "receiver.antenna.antenna_lever_arm_m.x");
        RequireRange(a.lever_arm_m.y, -100.0, 100.0, "receiver.antenna.antenna_lever_arm_m.y");
        RequireRange(a.lever_arm_m.z, -100.0, 100.0, "receiver.antenna.antenna_lever_arm_m.z");
        RequireRange(a.attitude_offset_deg.heading_deg, -360.0, 360.0,
                      "receiver.antenna.attitude_offset_deg.heading");
        RequireRange(a.attitude_offset_deg.pitch_deg, -90.0, 90.0, "receiver.antenna.attitude_offset_deg.pitch");

        // gnss
        const auto &g = settings.gnss;
        if (g.cn0_mask_dbhz < 0 || g.cn0_mask_dbhz > 60) {
            throw ConfigError("receiver.gnss.cn0_mask_dbhz must be in range 0..60 dB-Hz");
        }
        if (g.elevation_mask_deg < -90 || g.elevation_mask_deg > 90) {
            throw ConfigError("receiver.gnss.elevation_mask_deg must be in range -90..90 deg");
        }
        const auto sat_list = [](const char *key, const std::string &v, int gps_max) {
            if (!IsValidSatelliteList(v, true, gps_max)) {
                throw ConfigError(std::string(key) + " must be all, none or a '+'-joined list of constellations "
                                  "(GPS, GLONASS, GALILEO, SBAS, BEIDOU, QZSS) and satellite ids (G01..G" +
                                  std::to_string(gps_max) + ", R01..R30, E01..E36, S120..S158, C01..C63, "
                                  "J01..J10); got '" + v + "'");
            }
        };
        sat_list("receiver.gnss.satellite_tracking", g.satellite_tracking, kGpsMaxTracking);
        sat_list("receiver.gnss.satellite_usage", g.satellite_usage, kGpsMaxUsage);
        sat_list("receiver.gnss.satellite_health_override", g.satellite_health_override, kGpsMaxTracking);
        if (!IsValidSignalList(g.signal_tracking, true)) {
            throw ConfigError("receiver.gnss.signal_tracking must be all or a '+'-joined list of signal names / "
                              "constellations; got '" + g.signal_tracking + "'");
        }
        if (!IsValidSignalList(g.signal_usage, false)) {
            throw ConfigError("receiver.gnss.signal_usage must be all or a '+'-joined list of signal names "
                              "(no constellation aliases, p.131); got '" + g.signal_usage + "'");
        }

        // imu
        const auto &imu = settings.imu;
        RequireEnum(imu.startup_data_mode, kImuStartupModes, "receiver.imu.startup_data_mode");
        RequireEnum(imu.orientation_mode, kImuOrientationModes, "receiver.imu.imu_orientation.orientation_mode");
        // setIMUOrientation ranges (p.112)
        RequireRange(imu.theta_x_deg, -180.0, 180.0, "receiver.imu.imu_orientation.theta_x_deg");
        RequireRange(imu.theta_y_deg, -90.0, 90.0, "receiver.imu.imu_orientation.theta_y_deg");
        RequireRange(imu.theta_z_deg, -180.0, 180.0, "receiver.imu.imu_orientation.theta_z_deg");

        // navigation
        const auto &nav = settings.navigation;
        if (!ValidPlusList(nav.pvt_mode, false, [](const std::string &tok) { return InListCi(kPvtModes, tok); })) {
            throw ConfigError(
                "receiver.navigation.pvt_mode must be all or a '+'-joined list of " + Join(kPvtModes, '|') +
                "; got '" + nav.pvt_mode + "'");
        }
        RequireEnum(nav.receiver_dynamics, kDynamics, "receiver.navigation.receiver_dynamics");
        RequireEnum(nav.vehicle_application, kVehicleApplications, "receiver.navigation.vehicle_application");
        RequireEnum(nav.clock_sync_threshold, kClockSyncThresholds, "receiver.navigation.clock_sync_threshold");
        RequireEnum(nav.ins_output_location, kInsOutputLocations, "receiver.navigation.ins_output_location");

        // time_system.pps
        const auto &pps = settings.time_system.pps;
        RequireEnum(pps.interval, kPpsIntervals, "receiver.time_system.pps.interval");
        if (!InListCi(kPpsPolarities, pps.polarity)) {
            throw ConfigError(
                "receiver.time_system.pps.polarity must be Low2High or High2Low (receiver spelling, p.156); got '" +
                pps.polarity + "'");
        }
        RequireRange(pps.delay_ns, -1000000.0, 1000000.0, "receiver.time_system.pps.delay_ns");
        RequireEnum(pps.time_scale, kTimeScales, "receiver.time_system.pps.time_scale");
        if (pps.max_holdover_s < 0 || pps.max_holdover_s > 3600) {
            throw ConfigError("receiver.time_system.pps.max_holdover_s must be in range 0..3600 s");
        }
        RequireRange(pps.pulse_width_ms, 0.000001, 1000.0, "receiver.time_system.pps.pulse_width_ms");

        // ntrip (only checked when it will be sent)
        const auto &n = settings.ntrip;
        if (n.enabled) {
            const auto text = [](const char *key, const std::string &v, std::size_t max, bool is_password) {
                if (v.empty() || v == "<?>") {
                    throw ConfigError(std::string(key) + " is empty or the placeholder <?>: fill it in or set "
                                      "receiver.ntrip.enabled: false");
                }
                if (v.size() > max) {
                    throw ConfigError(std::string(key) + " exceeds " + std::to_string(max) + " characters");
                }
                if (!ReceiverTextOk(v, is_password)) {
                    throw ConfigError(std::string(key) + " contains a character the receiver does not accept (p.64)");
                }
            };
            text("receiver.ntrip.caster", n.caster, 40, false);
            if (HasWhitespace(n.caster) || HasWhitespace(n.mount_point)) {
                throw ConfigError("receiver.ntrip.caster and mount_point must not contain whitespace");
            }
            if (n.port < 1 || n.port > 65535) {
                throw ConfigError("receiver.ntrip.port must be in range 1..65535");
            }
            text("receiver.ntrip.username", n.username, 128, false);
            text("receiver.ntrip.password", n.password, 32, true);
            text("receiver.ntrip.mount_point", n.mount_point, 32, false);
            RequireEnum(n.version, kNtripVersions, "receiver.ntrip.version");
            RequireEnum(n.send_gga, kSendGga, "receiver.ntrip.send_gga_to_caster");
        }

        // warmup
        if (settings.warmup.min_uptime_s < 0) {
            throw ConfigError("receiver.warmup.min_uptime_s must be >= 0");
        }

        // streams
        if (settings.sbf_streams.empty()) {
            throw ConfigError("receiver.sbf_streams must contain at least one stream");
        }
        std::unordered_set<int> sbf_ids;
        bool has_receiver_status = false;
        for (const auto &s: settings.sbf_streams) {
            if (!sbf_ids.insert(s.stream_id).second) {
                throw ConfigError("receiver.sbf_streams contains duplicate stream id " + std::to_string(s.stream_id));
            }
            for (const auto &b: s.blocks) {
                if (EqualsCi(b, "ReceiverStatus") || EqualsCi(b, "Status") || EqualsCi(b, "Support")) {
                    has_receiver_status = true;
                }
            }
            // Validate with a placeholder descriptor; the real one is only known once connected.
            (void) BuildSbfOutputCommand(s, "IP10");
        }
        if (!has_receiver_status) {
            throw ConfigError("receiver.sbf_streams must include ReceiverStatus (or the Status group): the warm-up "
                "gate reads its UpTime and FINETIME bits");
        }
        std::unordered_set<int> nmea_ids;
        for (const auto &s: settings.nmea_streams) {
            // A duplicate id would silently overwrite the earlier stream on the receiver.
            if (!nmea_ids.insert(s.stream_id).second) {
                throw ConfigError("receiver.nmea_streams contains duplicate stream id " + std::to_string(s.stream_id));
            }
            (void) BuildNmeaOutputCommand(s);
        }
    }


    std::string RedactCmd(const std::string &cmd) {
        // Only the NTRIP password is a remote credential; the receiver's own account
        // is local to the sensor and is logged in clear. Replies are multi-line and
        // echo the command: redact line by line.
        if (cmd.find('\n') != std::string::npos) {
            std::vector<std::string> out;
            for (const auto &line: Split(cmd, '\n')) {
                out.push_back(RedactCmd(line));
            }
            return Join(out, '\n');
        }

        std::string body = Trim(cmd);
        std::string prefix;
        if (body.size() >= 3 && body.compare(0, 2, "$R") == 0) {
            prefix = body.substr(0, 3) + " "; // "$R:", "$R?", "$R!", "$R;"
            body = Trim(body.substr(3));
        }
        if (!body.empty() && body[0] == '#') {
            prefix += "#";
            body = Trim(body.substr(1));
        }
        auto fields = SplitTrim(body, ',');
        if (fields.size() >= 7 &&
            (EqualsCi(fields[0], "setNtripSettings") || EqualsCi(fields[0], "NtripSettings") ||
             EqualsCi(fields[0], "snts"))) {
            // [set]NtripSettings / snts, NTR1, Client, caster, port, user, password, mount, version, sendgga
            fields[6] = "<REDACTED>";
            std::string out = prefix + fields[0];
            for (std::size_t i = 1; i < fields.size(); ++i) {
                out += ", " + fields[i];
            }
            return out;
        }
        return cmd;
    }
} // namespace asterx
