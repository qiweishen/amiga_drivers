#include <doctest/doctest.h>
#include <sstream>
#include "sensor_sync_quality.h"

namespace {
    // A log-format-3 session as SensorSync.ino writes it: header (println lines end in
    // \r\n), rising-edge-only T lines, raw-level S lines, the final #H whose T=/S= counts
    // close the streams, then #SESSION,STOP.
    std::string Log(unsigned triggers, unsigned exposures, bool open_tail = false, bool active_high = true) {
        const unsigned on = active_high ? 1 : 0, off = 1 - on;
        std::ostringstream out;
        out << "#SESSION,START,test\n#LOG,SensorSync-logger,3,tick_hz=75000000.000\n"
            << "#GROUP,FX,requested=50.00000000,actual=49.99893336,pulse_us=50.346667\n"
            << "#TRIG,0,FX10E-1,33,49.99893336\n#STROBE,0,FX10E_EXP-1,18,ah=" << on << "\n#READY\r\n"
            << "P,1,75000000\nZ,1,75100000,$GPZDA,120000.00,15,11,2026,00,00*6F\n";
        for (unsigned i = 0; i < triggers; ++i) {
            out << "T," << i + 1 << ",0,1," << i * 100 + 1 << '\n';
        }
        unsigned s = 0;
        for (unsigned i = 0; i < exposures; ++i) {
            out << "S," << ++s << ",0," << on << ',' << i * 100 + 10 << '\n';
            if (!open_tail || i + 1 < exposures)
                out << "S," << ++s << ",0," << off << ',' << i * 100 + 50 << '\n';
        }
        out << "#H up=5000 pps=1 tod=1 T=" << triggers << ",0,0,0 S=" << s << ",0,0,0 drops=0\n"
            << "#SESSION,STOP,test\n";
        return out.str();
    }

    bool OkAfter(std::string log, const std::string &from, const std::string &to, unsigned frames = 3) {
        log.replace(log.find(from), from.size(), to);
        std::istringstream input(log);
        return common::InspectSensorSync(input, 0, true, frames, 0).Ok();
    }
}

TEST_CASE("SensorSync accounting rejects half-rate reference exposure despite clean health counters") {
    std::istringstream input(Log(602, 301));
    const auto quality = common::InspectSensorSync(input, 0, true, 301, 0);
    CHECK(quality.protocol_complete);
    CHECK(quality.health_counts_match);
    CHECK(quality.ExposureComplete());
    CHECK_FALSE(quality.CountsReconciled());
    CHECK_FALSE(quality.Ok());
}

TEST_CASE("SensorSync accounting distinguishes complete frames, RX loss, and missing exposure tail") {
    SUBCASE("matching counts do not establish an association anchor") {
        std::istringstream input(Log(3, 3));
        const auto quality = common::InspectSensorSync(input, 0, true, 3, 0);
        CHECK(quality.Ok());
        CHECK(quality.Json()["association_verified"] == false);
        REQUIRE(quality.trigger_output_hz.has_value());
        CHECK(*quality.trigger_output_hz == doctest::Approx(49.99893336));
    }
    SUBCASE("an explained missing frame still fails clean acquisition") {
        std::istringstream input(Log(3, 3));
        const auto quality = common::InspectSensorSync(input, 0, true, 2, 1);
        CHECK(quality.CountsReconciled());
        CHECK_FALSE(quality.Ok());
    }
    SUBCASE("zero health counters cannot hide an unfinished exposure") {
        std::istringstream input(Log(3, 3, true));
        const auto quality = common::InspectSensorSync(input, 0, true, 3, 0);
        CHECK(quality.CountsReconciled());
        CHECK_FALSE(quality.ExposureComplete());
        CHECK_FALSE(quality.Ok());
    }
    SUBCASE("sequence loss is independent of matching totals") {
        CHECK_FALSE(OkAfter(Log(3, 3), "S,3,0,1", "S,9,0,1"));
        CHECK_FALSE(OkAfter(Log(3, 3), "T,2,0,1", "T,5,0,1"));
    }
    SUBCASE("a trigger line is a rising edge only") {
        CHECK_FALSE(OkAfter(Log(3, 3), "T,2,0,1", "T,2,0,0"));
    }
}

TEST_CASE("SensorSync exposure level comes from the log's #STROBE declaration") {
    SUBCASE("an active-low input (Go-X opto output, ah=0) is read as exposure starts on low") {
        std::istringstream input(Log(3, 3, false, false));
        const auto quality = common::InspectSensorSync(input, 0, true, 3, 0);
        CHECK(quality.Ok());
        CHECK(quality.Json()["exposure_active_high"] == false);
        CHECK(quality.exposure.rises == 3u);
    }
    SUBCASE("a declaration that contradicts the edges leaves an exposure open") {
        CHECK_FALSE(OkAfter(Log(3, 3, false, false), "ah=0", "ah=1"));
    }
    SUBCASE("a missing or unknown declaration stays unverified") {
        CHECK_FALSE(OkAfter(Log(3, 3), "#STROBE,0,FX10E_EXP-1,18,ah=1\n", ""));
        CHECK_FALSE(OkAfter(Log(3, 3), "ah=1", "ah=x"));
    }
}

TEST_CASE("SensorSync session framing follows log format 3") {
    SUBCASE("reported drops fail the session") {
        CHECK_FALSE(OkAfter(Log(3, 3), "drops=0", "drops=2"));
    }
    SUBCASE("the final #H must account for every line of this channel") {
        CHECK_FALSE(OkAfter(Log(3, 3), "S=6,", "S=7,"));
        CHECK_FALSE(OkAfter(Log(3, 3), " T=3,", " T=4,"));
        // Another channel's counts are its own business
        CHECK(OkAfter(Log(3, 3), ",0,0,0 S=", ",5,0,0 S="));
    }
    SUBCASE("without a final #H right before STOP the tail is unverified") {
        CHECK_FALSE(OkAfter(Log(3, 3), "#SESSION,STOP,", "P,2,150000000\n#SESSION,STOP,"));
    }
    SUBCASE("another log format, a board error or a reboot banner fails the framing") {
        CHECK_FALSE(OkAfter(Log(3, 3), "SensorSync-logger,3,", "SensorSync-logger,2,"));
        CHECK_FALSE(OkAfter(Log(3, 3), "#READY", "#ERR,bad_freq\n#READY"));
        CHECK_FALSE(OkAfter(Log(3, 3), "P,1,", "# SensorSync-logger ready.\nP,1,"));
        CHECK_FALSE(OkAfter(Log(3, 3), "#SESSION,STOP,test\n", "#SESSION,STOP,test\nS,7,0,1,999\n"));
    }
}
