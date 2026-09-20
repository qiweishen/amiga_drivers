#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <istream>
#include <limits>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>
#include "session_protocol.h"

namespace common {
    // Capture-time accounting only. Matching counts do NOT establish a frame
    // anchor, a PPS/UTC association, or calibrated exposure-edge timing.
    struct SensorSyncQuality {
        struct Edges {
            std::uint64_t rises = 0, falls = 0, last_seq = 0, last_tick = 0;
            unsigned last_level = 0;
            bool seen = false;
            bool valid = true;

            void Observe(std::uint64_t seq, unsigned level, std::uint64_t tick) {
                if (seq != last_seq + 1 || (seen && (tick <= last_tick || level == last_level))) valid = false;
                if (level) ++rises; else ++falls;
                last_seq = seq;
                last_tick = tick;
                last_level = level;
                seen = true;
            }
        };
        Edges trigger, exposure;
        bool readable = true;
        bool protocol_complete = false;
        bool polarity_supported = false; // log declares active-high; not physical verification
        bool trigger_truncated = false;
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
            const bool trigger_complete = !externally_triggered ||
                (trigger.seen && trigger.valid &&
                 ((trigger.rises == trigger.falls && trigger.last_level == 0) ||
                  (trigger_truncated && trigger.rises == trigger.falls + 1 && trigger.last_level == 1)));
            return readable && protocol_complete && polarity_supported && trigger_complete &&
                   ExposureComplete() && CountsReconciled() && missing == 0;
        }
        nlohmann::json Json() const {
            return {{"schema_version", 1}, {"ok", Ok()}, {"log_readable", readable},
                {"protocol_complete", protocol_complete}, {"exposure_polarity_supported", polarity_supported},
                {"externally_triggered", externally_triggered}, {"trigger_rises", trigger.rises},
                {"trigger_falls", trigger.falls}, {"trigger_truncated", trigger_truncated},
                {"trigger_sequence_valid", trigger.valid}, {"exposure_rises", exposure.rises},
                {"exposure_falls", exposure.falls}, {"exposure_sequence_valid", exposure.valid},
                {"exposure_complete", ExposureComplete()}, {"frames_written", frames},
                {"frames_missing_rx", missing}, {"counts_reconciled", CountsReconciled()},
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
        bool stopping = false;
        if (channel >= 4 || !input.good()) { out.readable = false; return out; }
        std::string line;
        // Bound malformed lines without allocating in proportion to their size.
        std::array<char, 2049> buffer{};
        while (input.getline(buffer.data(), static_cast<std::streamsize>(buffer.size()))) {
            line = buffer.data();
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const std::string_view text(line);
            stopping = stopping || text.starts_with("#TRUNC,") || text.starts_with("#HFINAL ") ||
                       text.starts_with("#SESSION,STOP,");
            protocol.observe(text, stopping);
            // Current rig inputs are active-high. A different/unknown polarity
            // must not silently be interpreted as an exposure-start rising edge.
            const std::string strobe = "#STROBE," + std::to_string(channel) + ",";
            if (text.starts_with(strobe)) {
                out.polarity_supported = text.ends_with(",ah=1");
            }
            if (!text.starts_with("T,") && !text.starts_with("S,")) continue;
            if (protocol.starts != 1 || !protocol.ready || protocol.final_health_received || protocol.stop_received)
                out.readable = false;
            auto fields = text.substr(2);
            std::uint64_t v[4]{};
            bool parsed = true;
            for (unsigned i = 0; i < 4; ++i) {
                const auto end = fields.find(',');
                if ((i < 3 && end == std::string_view::npos) || (i == 3 && end != std::string_view::npos) ||
                    !SensorSyncProtocol::number(fields.substr(0, end), v[i])) { parsed = false; break; }
                if (i < 3) fields.remove_prefix(end + 1);
            }
            if (!parsed || v[1] >= 4 || v[2] > 1) { out.readable = false; continue; }
            if (v[1] != channel) continue;
            auto &edges = text.front() == 'T' ? out.trigger : out.exposure;
            edges.Observe(v[0], static_cast<unsigned>(v[2]), v[3]);
        }
        if (input.bad() || !input.eof()) out.readable = false;
        out.protocol_complete = !protocol.failed && protocol.starts == 1 && protocol.ready &&
                                protocol.final_health_received && protocol.stop_received;
        out.trigger_truncated = (protocol.truncation_mask & (1u << (channel / 2))) != 0;
        return out;
    }

    inline SensorSyncQuality InspectSensorSync(const std::filesystem::path &path, unsigned channel,
                                               bool external, std::uint64_t frames, std::uint64_t missing) {
        std::ifstream input(path);
        return InspectSensorSync(input, channel, external, frames, missing);
    }
} // namespace common
