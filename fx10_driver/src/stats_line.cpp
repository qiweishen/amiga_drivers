#include "stats_line.h"

#include <spdlog/fmt/fmt.h>


namespace fx10 {
    namespace {
        std::string OptionalInt(const std::optional<std::int64_t> &v) {
            return v ? fmt::format("{}", *v) : std::string("n/a");
        }

        std::string OptionalCelsius(const std::optional<double> &v) {
            return v ? fmt::format("{:.1f}", *v) : std::string("n/a");
        }
    } // namespace


    std::string FormatStatsLine(const StatsSample &s) {
        // "fps={:.1f}" is the literal tools/check_contracts.py looks for in THIS
        // file; keep it in one piece and in one format call.
        return fmt::format("[Statistics] frames={}  rate={:.1f} Hz  fps={:.1f}", s.frames, s.rate_hz, s.write_fps)
               + "  missed_triggers=" + OptionalInt(s.missed_triggers)
               + "  temp_pcb=" + OptionalCelsius(s.temp_pcb_c)
               + "  temp_fpga=" + OptionalCelsius(s.temp_fpga_c);
    }


    std::string FormatFinalStatsLine(const FinalStatsSample &s) {
        // Carries no fps= field: the GUI parser skips it by design (there is no
        // meaningful "last window" at the end of a session).
        return fmt::format("[Statistics] Final: frames={}  frames_missed_rx={}", s.frames, s.frames_missed_rx)
               + "  missed_triggers=" + OptionalInt(s.missed_triggers)
               + fmt::format("  rx_timeouts={}", s.retrieve_timeouts);
    }
} // namespace fx10
