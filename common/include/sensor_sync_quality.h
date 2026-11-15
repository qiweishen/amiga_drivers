#pragma once

#include <array>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <istream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace common {
    // Session framing of a SensorSync-Logger log, format 3 (3rd_party/External/sensor_trigger:
    // README "Log format", SensorSync.ino). One session reads
    //   #SESSION,START,<path>  #LOG,SensorSync-logger,3,tick_hz=<f>  #GROUP/#TRIG/#STROBE ...
    //   #READY  {P | T | S | Z | "#H ... drops=<n>"}*  final "#H ..."  #SESSION,STOP,<path>
    // At STOP the board stops the PWM, keeps capturing exposures for STOP_TAIL_MS, drains its
    // queue and prints the final #H right before #SESSION,STOP. Data lines are all-or-nothing
    // (a line the USB link cannot take is counted in drops, never cut).
    struct SensorSyncProtocol {
        static constexpr unsigned kLogFormat = 3;
        static constexpr unsigned kChannels = 4;
        using Counts = std::array<std::uint64_t, kChannels>;

        unsigned starts = 0;
        bool format_ok = false;
        bool ready = false;
        bool stop_received = false;
        bool final_health_received = false; // a clean "#H" line right before #SESSION,STOP
        // #ERR, another log format, nonzero drops, a malformed #H, or any line the format
        // does not define (a board reboot banner, #IDLE, data after STOP)
        bool failed = false;
        // T= / S= of the final #H: events the ISRs saw per channel, dropped ones included
        Counts final_triggers{}, final_strobes{};

        static bool Number(std::string_view text, std::uint64_t &value) {
            if (text.empty()) return false;
            const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
            return ec == std::errc() && end == text.data() + text.size();
        }

        // "<key>a,b,c,d" inside a #H line
        static bool HealthCounts(std::string_view line, std::string_view key, Counts &counts) {
            const auto at = line.find(key);
            if (at == std::string_view::npos) return false;
            auto text = line.substr(at + key.size());
            text = text.substr(0, text.find(' '));
            for (unsigned i = 0; i < kChannels; ++i) {
                const auto comma = text.find(',');
                if ((i + 1 < kChannels) == (comma == std::string_view::npos)) return false;
                if (!Number(text.substr(0, comma), counts[i])) return false;
                if (i + 1 < kChannels) text.remove_prefix(comma + 1);
            }
            return true;
        }

        void Observe(std::string_view line) {
            const bool health = line.starts_with("#H ");
            const bool closes = line.starts_with("#SESSION,STOP,");
            if (stop_received && !line.empty()) {
                failed = true; // nothing follows the STOP acknowledgement
            } else if (line.starts_with("#SESSION,START,")) {
                ++starts;
            } else if (line.starts_with("#LOG,")) {
                constexpr std::string_view prefix = "#LOG,SensorSync-logger,";
                std::uint64_t version = 0;
                const auto field = line.starts_with(prefix) ? line.substr(prefix.size()) : std::string_view{};
                format_ok = Number(field.substr(0, field.find(',')), version) && version == kLogFormat;
                failed = failed || !format_ok;
            } else if (line == "#READY") {
                ready = true;
            } else if (health) {
                constexpr std::string_view drops_key = " drops=";
                const auto at = line.rfind(drops_key);
                std::uint64_t drops = 0;
                Counts t{}, s{};
                if (at == std::string_view::npos || !Number(line.substr(at + drops_key.size()), drops) || drops != 0 ||
                    !HealthCounts(line, " T=", t) || !HealthCounts(line, " S=", s)) {
                    failed = true;
                }
                final_triggers = t;
                final_strobes = s;
            } else if (closes) {
                stop_received = true;
                final_health_received = last_was_health_;
            } else if (!line.empty() && !line.starts_with("#GROUP,") && !line.starts_with("#TRIG,") &&
                       !line.starts_with("#STROBE,") && !line.starts_with("P,") && !line.starts_with("T,") &&
                       !line.starts_with("S,") && !line.starts_with("Z,")) {
                failed = true; // #ERR, #IDLE, a reboot banner, or something this format does not define
            }
            last_was_health_ = health;
        }

        bool Complete() const {
            return !failed && starts == 1 && format_ok && ready && final_health_received && stop_received;
        }

    private:
        bool last_was_health_ = false;
    };

    // Capture-time accounting only. Matching counts do NOT establish a frame
    // anchor, a PPS/UTC association, or calibrated exposure-edge timing.
    struct SensorSyncQuality {
        struct Edges {
            std::uint64_t rises = 0, falls = 0, last_seq = 0, last_tick = 0;
            unsigned last_level = 0;
            bool seen = false;
            bool valid = true;

            // seq counts from 1 per stream and ticks increase; `alternate`: levels must
            // alternate (exposure edges; trigger lines are rising edges only)
            void Observe(std::uint64_t seq, unsigned level, std::uint64_t tick, bool alternate) {
                if (seq != last_seq + 1 || (seen && (tick <= last_tick || (alternate && level == last_level))))
                    valid = false;
                if (level) ++rises; else ++falls;
                last_seq = seq;
                last_tick = tick;
                last_level = level;
                seen = true;
            }
        };
        Edges trigger; // rising edges of the trigger output
        Edges exposure; // rises = exposure starts, falls = exposure ends (line level mapped through ah)
        bool readable = true;
        bool protocol_complete = false;
        bool health_counts_match = false; // final #H T=/S= of this channel == the last seq logged
        bool polarity_supported = false; // the log declares this channel's exposure level (ah=0|1)
        std::optional<bool> exposure_active_high; // #STROBE ah=; a declaration, not physical verification
        std::optional<double> trigger_output_hz; // #TRIG: 0 = the firmware holds this output LOW
        std::uint64_t frames = 0, missing = 0;
        bool externally_triggered = false;

        bool CountsReconciled() const {
            if (missing > std::numeric_limits<std::uint64_t>::max() - frames) return false;
            return frames != 0 && exposure.rises == frames + missing &&
                   (!externally_triggered || trigger.rises == exposure.rises);
        }
        bool ExposureComplete() const {
            return exposure.seen && exposure.valid && exposure.rises == exposure.falls && exposure.last_level == 0;
        }
        bool Ok() const {
            const bool trigger_complete = !externally_triggered || (trigger.seen && trigger.valid);
            return readable && protocol_complete && health_counts_match && polarity_supported && trigger_complete &&
                   ExposureComplete() && CountsReconciled() && missing == 0;
        }
        nlohmann::json Json() const {
            return {{"schema_version", 2}, {"log_format", SensorSyncProtocol::kLogFormat}, {"ok", Ok()},
                {"log_readable", readable}, {"protocol_complete", protocol_complete},
                {"health_counts_match", health_counts_match}, {"exposure_polarity_supported", polarity_supported},
                {"exposure_active_high",
                 exposure_active_high ? nlohmann::json(*exposure_active_high) : nlohmann::json(nullptr)},
                {"externally_triggered", externally_triggered},
                {"trigger_output_hz", trigger_output_hz ? nlohmann::json(*trigger_output_hz) : nlohmann::json(nullptr)},
                {"trigger_rises", trigger.rises}, {"trigger_sequence_valid", trigger.valid},
                {"exposure_starts", exposure.rises}, {"exposure_ends", exposure.falls},
                {"exposure_sequence_valid", exposure.valid}, {"exposure_complete", ExposureComplete()},
                {"frames_written", frames}, {"frames_missing_rx", missing}, {"counts_reconciled", CountsReconciled()},
                {"association_verified", false},
                {"semantics", "session counts and edge completeness only; no verified frame/trigger anchor"}};
        }
    };

    inline SensorSyncQuality InspectSensorSync(std::istream &input, unsigned channel,
                                               bool external, std::uint64_t frames, std::uint64_t missing) {
        SensorSyncQuality out;
        out.frames = frames;
        out.missing = missing;
        out.externally_triggered = external;
        SensorSyncProtocol protocol;
        if (channel >= SensorSyncProtocol::kChannels || !input.good()) { out.readable = false; return out; }
        const std::string strobe = "#STROBE," + std::to_string(channel) + ",";
        const std::string trig = "#TRIG," + std::to_string(channel) + ",";
        std::string line;
        // Bound malformed lines without allocating in proportion to their size.
        std::array<char, 2049> buffer{};
        while (input.getline(buffer.data(), static_cast<std::streamsize>(buffer.size()))) {
            line = buffer.data();
            if (!line.empty() && line.back() == '\r') line.pop_back(); // header lines end in \r\n (println)
            const std::string_view text(line);
            protocol.Observe(text);
            // #STROBE,<ch>,<name>,<pin>,ah=<0|1>: the line level during exposure. Firmware
            // config.h declares it per input (FX10 high, Go-X opto output low); any other
            // value must not be guessed into exposure starts.
            if (text.starts_with(strobe)) {
                out.exposure_active_high.reset();
                if (text.ends_with(",ah=1")) out.exposure_active_high = true;
                else if (text.ends_with(",ah=0")) out.exposure_active_high = false;
                out.polarity_supported = out.exposure_active_high.has_value();
            }
            // #TRIG,<ch>,<name>,<pin>,<hz>: the output's effective rate, 0 = held LOW.
            // from_chars, not strtod: the process locale (Qt sets it from the environment)
            // must not change how the board's "%.8f" is read
            if (text.starts_with(trig)) {
                const auto hz = text.substr(text.rfind(',') + 1);
                double value = 0.0;
                const auto [end, ec] = std::from_chars(hz.data(), hz.data() + hz.size(), value);
                if (!hz.empty() && ec == std::errc() && end == hz.data() + hz.size()) out.trigger_output_hz = value;
                else out.readable = false;
            }
            if (!text.starts_with("T,") && !text.starts_with("S,")) continue;
            if (protocol.starts != 1 || !protocol.ready || protocol.stop_received)
                out.readable = false;
            // T,<seq>,<ch>,1,<tick> | S,<seq>,<ch>,<level>,<tick>
            auto fields = text.substr(2);
            std::uint64_t v[4]{};
            bool parsed = true;
            for (unsigned i = 0; i < 4; ++i) {
                const auto end = fields.find(',');
                if ((i < 3 && end == std::string_view::npos) || (i == 3 && end != std::string_view::npos) ||
                    !SensorSyncProtocol::Number(fields.substr(0, end), v[i])) { parsed = false; break; }
                if (i < 3) fields.remove_prefix(end + 1);
            }
            const bool is_trigger = text.front() == 'T';
            if (!parsed || v[1] >= SensorSyncProtocol::kChannels || v[2] > 1 || (is_trigger && v[2] != 1)) {
                out.readable = false;
                continue;
            }
            if (v[1] != channel) continue;
            if (is_trigger) {
                out.trigger.Observe(v[0], 1, v[3], false);
            } else if (!out.exposure_active_high) {
                out.readable = false; // an exposure edge with no declared level
            } else {
                const unsigned active = (v[2] == 1) == *out.exposure_active_high ? 1u : 0u;
                out.exposure.Observe(v[0], active, v[3], true);
            }
        }
        if (input.bad() || !input.eof()) out.readable = false;
        out.protocol_complete = protocol.Complete();
        out.health_counts_match = protocol.final_health_received &&
                                  protocol.final_triggers[channel] == out.trigger.last_seq &&
                                  protocol.final_strobes[channel] == out.exposure.last_seq;
        return out;
    }

    inline SensorSyncQuality InspectSensorSync(const std::filesystem::path &path, unsigned channel,
                                               bool external, std::uint64_t frames, std::uint64_t missing) {
        std::ifstream input(path);
        return InspectSensorSync(input, channel, external, frames, missing);
    }
} // namespace common
