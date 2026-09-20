#include <doctest/doctest.h>
#include <sstream>
#include "sensor_sync_quality.h"

namespace {
    std::string Log(unsigned triggers, unsigned exposures, bool open_tail = false) {
        std::ostringstream out;
        out << "#SESSION,START,test\n#LOG,SensorSync-logger,2\n#STROBE,0,FX10E_EXP-1,18,ah=1\n#READY\n";
        for (unsigned i = 0; i < triggers; ++i) {
            out << "T," << 2 * i + 1 << ",0,1," << i * 100 + 1 << '\n';
            out << "T," << 2 * i + 2 << ",0,0," << i * 100 + 2 << '\n';
        }
        for (unsigned i = 0; i < exposures; ++i) {
            out << "S," << 2 * i + 1 << ",0,1," << i * 100 + 10 << '\n';
            if (!open_tail || i + 1 < exposures)
                out << "S," << 2 * i + 2 << ",0,0," << i * 100 + 50 << '\n';
        }
        out << "#HFINAL ppsdrops=0 todtrunc=0 tdrops=0 sdrops=0 host=0 pwmerr=0 trunc=0\n#SESSION,STOP,test\n";
        return out.str();
    }
}

TEST_CASE("SensorSync accounting rejects half-rate reference exposure despite clean health counters") {
    std::istringstream input(Log(602, 301));
    const auto quality = common::InspectSensorSync(input, 0, true, 301, 0);
    CHECK(quality.protocol_complete);
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
        auto log = Log(3, 3);
        log.replace(log.find("S,3,0,1"), 7, "S,9,0,1");
        std::istringstream input(log);
        CHECK_FALSE(common::InspectSensorSync(input, 0, true, 3, 0).Ok());
    }
    SUBCASE("missing polarity or malformed tail stays unverified") {
        auto log = Log(3, 3);
        log.replace(log.find("ah=1"), 4, "ah=0");
        std::istringstream input(log);
        CHECK_FALSE(common::InspectSensorSync(input, 0, true, 3, 0).Ok());
    }
}
