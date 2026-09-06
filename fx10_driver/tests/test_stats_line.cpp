#include "../include/stats_line.h"

#include <doctest/doctest.h>

#include <string>

// The [Statistics] line is a GUI contract, not a log message: the dashboard
// cards read the disk write rate out of it (app/services/driver_stats.py) and
// tools/check_contracts.py pins a rendered sample. Nothing but these tests
// stands between a reworded format string and a card that silently reads 0.

using fx10::FinalStatsSample;
using fx10::StatsSample;

namespace {
    StatsSample FullSample() {
        StatsSample s;
        s.frames = 1200;
        s.rate_hz = 50.04;
        s.write_fps = 49.84;
        s.missed_triggers = 0;
        s.temp_pcb_c = 41.23;
        s.temp_fpga_c = 52.71;
        return s;
    }
} // namespace

TEST_CASE("StatsLine: RendersTheContractedFieldsInOrder") {
    // This exact string is what tools/check_contracts.py carries as the fx10
    // sample; keep the two in step.
    CHECK(fx10::FormatStatsLine(FullSample()) == "[Statistics] frames=1200  rate=50.0 Hz  fps=49.8  missed_triggers=0  "
              "temp_pcb=41.2  temp_fpga=52.7");
}

TEST_CASE("StatsLine: PrefixAndSeparatorAreTheRoutingContract") {
    const std::string line = fx10::FormatStatsLine(FullSample());
    // driver_stats.py routes on the prefix and splits on the double space.
    CHECK(std::string(line).rfind("[Statistics] ", 0) == 0);
    CHECK(std::string(line).find("  fps=") != std::string::npos);
    // A single-spaced separator would make the fields ambiguous, because a
    // value can itself contain a space ("50.0 Hz").
    CHECK(std::string(line).find("Hz fps=") == std::string::npos);
    CHECK(std::string(line).find("Hz  fps=") != std::string::npos);
}

TEST_CASE("StatsLine: UnknownTelemetryReadsAsNotAvailableRatherThanNan") {
    // A camera that does not answer the counter/temperature reads must not
    // render as "nan" (reads like a measurement) or as 0 (reads like health).
    StatsSample s = FullSample();
    s.missed_triggers.reset();
    s.temp_pcb_c.reset();
    s.temp_fpga_c.reset();
    const std::string line = fx10::FormatStatsLine(s);
    CHECK(std::string(line).find("missed_triggers=n/a") != std::string::npos);
    CHECK(std::string(line).find("temp_pcb=n/a") != std::string::npos);
    CHECK(std::string(line).find("temp_fpga=n/a") != std::string::npos);
    CHECK(std::string(line).find("nan") == std::string::npos);
    // The write rate is ours, always known, and always present.
    CHECK(std::string(line).find("fps=49.8") != std::string::npos);
}

TEST_CASE("StatsLine: RatesKeepOneDecimal") {
    StatsSample s = FullSample();
    s.rate_hz = 0.0;
    s.write_fps = 0.0;
    CHECK(std::string(fx10::FormatStatsLine(s)).find("rate=0.0 Hz  fps=0.0") != std::string::npos);
}

TEST_CASE("StatsLine: MissedTriggersCanBeNegativeWhenTheCounterWrapsOrResets") {
    // Reported verbatim rather than hidden: a negative delta means the counter
    // was reset behind our back, which is worth seeing in the log.
    StatsSample s = FullSample();
    s.missed_triggers = -4;
    CHECK(std::string(fx10::FormatStatsLine(s)).find("missed_triggers=-4") != std::string::npos);
}

TEST_CASE("StatsLine: FinalLineCarriesTheSessionTotalsAndNoFpsField") {
    FinalStatsSample s;
    s.frames = 9000;
    s.frames_missed_rx = 3;
    s.missed_triggers = 1;
    s.retrieve_timeouts = 27;
    const std::string line = fx10::FormatFinalStatsLine(s);
    CHECK(line == "[Statistics] Final: frames=9000  frames_missed_rx=3  missed_triggers=1  "
              "rx_timeouts=27");
    // No fps= field: driver_stats.py must skip the summary rather than treat it
    // as a fresh sample of a window that no longer exists.
    CHECK(std::string(line).find("fps=") == std::string::npos);
}

TEST_CASE("StatsLine: FinalLineReportsAnUnreadCounterAsUnknown") {
    FinalStatsSample s;
    s.frames = 10;
    CHECK(std::string(fx10::FormatFinalStatsLine(s)).find("missed_triggers=n/a") != std::string::npos);
}
