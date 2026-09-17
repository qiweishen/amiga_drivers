#include <doctest/doctest.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "sensor_sync_hub.h"

using common::SensorSyncBackend;
using common::SensorSyncError;
using common::SensorSyncHub;

namespace {
    struct FakeBoard final : SensorSyncBackend {
        struct State {
            int opens = 0;
            int starts = 0;
            int stops = 0;
            std::string port;
            std::filesystem::path log_path;
            std::vector<std::pair<int, double> > rates;
            bool fail_open = false;
            bool fail_start = false;
            bool ok = true;
            double stalled = 0.0;
        };

        explicit FakeBoard(std::shared_ptr<State> state) : s(std::move(state)) {
        }

        void Open(const std::string &port) override {
            ++s->opens;
            s->port = port;
            if (s->fail_open) throw SensorSyncError("[TriggerLog] fake open failure");
        }

        void Start(const std::filesystem::path &log_path,
                   const std::vector<std::pair<int, double> > &channel_freqs_hz) override {
            ++s->starts;
            s->log_path = log_path;
            s->rates = channel_freqs_hz;
            if (s->fail_start) throw SensorSyncError("[TriggerLog] fake START rejected");
        }

        void Stop() override { ++s->stops; }

        bool Ok() const override { return s->ok; }

        double StalledSeconds() const override { return s->stalled; }

        std::shared_ptr<State> s;
    };

    struct Rig {
        std::shared_ptr<FakeBoard::State> board = std::make_shared<FakeBoard::State>();
        SensorSyncHub hub{"/tmp/run/raw/sensor_trigger.log", "/dev/ttyACM0", std::make_unique<FakeBoard>(board)};
    };

    double RateOf(const std::vector<std::pair<int, double> > &rates, int group_channel) {
        for (const auto &[ch, hz]: rates) {
            if (ch == group_channel) return hz;
        }
        return -1.0;
    }
} // namespace


TEST_CASE("SensorSyncHub: the board opens once, on the first registration, and STARTs when everyone armed") {
    Rig rig;
    rig.hub.Register("fx10", 0, 50.0);
    rig.hub.Register("gox:cam0", 2, 5.0);
    CHECK(rig.board->opens == 1);
    CHECK(rig.board->port == "/dev/ttyACM0"); // the rig's port from config-main.yaml, not a driver's
    CHECK(rig.hub.Port() == "/dev/ttyACM0");
    CHECK(rig.hub.Registered("fx10"));
    CHECK(rig.hub.Registered("gox:cam0"));
    CHECK_FALSE(rig.hub.Registered("lms"));
    // The rig log's one-line summary of what was asked
    const std::string described = rig.hub.Describe();
    CHECK(described.find("port /dev/ttyACM0") != std::string::npos);
    CHECK(described.find("FX pair 50 Hz: fx10 (ch0)") != std::string::npos);
    CHECK(described.find("JAI pair 5 Hz: gox:cam0 (ch2)") != std::string::npos);
    CHECK(described.find("once every participant has armed") != std::string::npos);

    // The Go-X arms during its Init, long before the FX10's Run thread exists
    CHECK_FALSE(rig.hub.Arm("gox:cam0"));
    CHECK(rig.board->starts == 0);
    CHECK_FALSE(rig.hub.Started());
    CHECK(rig.hub.WaitingSeconds("gox:cam0") >= 0.0);
    CHECK(rig.hub.WaitingSeconds("fx10") == 0.0); // not armed yet

    CHECK(rig.hub.Arm("fx10"));
    CHECK(rig.board->starts == 1);
    CHECK(rig.hub.Started());
    CHECK(rig.hub.Ok());
    CHECK(rig.board->log_path == std::filesystem::path("/tmp/run/raw/sensor_trigger.log"));
    // Both groups are programmed explicitly, each with its participant's rate
    CHECK(RateOf(rig.board->rates, 0) == doctest::Approx(50.0));
    CHECK(RateOf(rig.board->rates, 2) == doctest::Approx(5.0));
    CHECK(rig.hub.WaitingSeconds("gox:cam0") == 0.0);
    CHECK(rig.hub.Arm("gox:cam0")); // a repeated arm is harmless once running
    CHECK(rig.hub.Describe().find("pulses running") != std::string::npos);

    // The first teardown ends the session for everybody; the second is a no-op
    rig.hub.Disarm("fx10");
    CHECK(rig.board->stops == 1);
    CHECK(rig.hub.Ended());
    rig.hub.Disarm("gox:cam0");
    CHECK(rig.board->stops == 1);
    CHECK(rig.hub.StalledSeconds() == doctest::Approx(-1.0));
    CHECK(rig.hub.Describe().find("session ended") != std::string::npos);
}


TEST_CASE("SensorSyncHub: a group without a participant is disabled explicitly, never left at the board's old rate") {
    Rig rig;
    rig.hub.Register("gox:cam0", 2, 3.0);
    CHECK(rig.hub.Describe().find("FX pair off (no participant)") != std::string::npos);
    CHECK(rig.hub.Arm("gox:cam0")); // the only participant: pulses start at once
    CHECK(RateOf(rig.board->rates, 0) == doctest::Approx(0.0)); // FX pair off
    CHECK(RateOf(rig.board->rates, 2) == doctest::Approx(3.0));
    CHECK(rig.board->rates.size() == 2);
}


TEST_CASE("SensorSyncHub: strobe-only participants keep their pair off but are still waited for") {
    Rig rig;
    rig.hub.Register("fx10", 0, 0.0); // freerun FX10: log its strobes, no pulses
    rig.hub.Register("gox:cam0", 2, 2.0);
    CHECK(rig.hub.Describe().find("FX pair strobes only, no pulses: fx10 (ch0)") != std::string::npos);
    CHECK_FALSE(rig.hub.Arm("fx10"));
    CHECK(rig.hub.Arm("gox:cam0"));
    CHECK(RateOf(rig.board->rates, 0) == doctest::Approx(0.0));
    CHECK(RateOf(rig.board->rates, 2) == doctest::Approx(2.0));
}


TEST_CASE("SensorSyncHub: two Go-X cameras share the JAI pair at one rate") {
    Rig rig;
    rig.hub.Register("gox:cam0", 2, 5.0);
    rig.hub.Register("gox:cam1", 3, 5.0);
    CHECK_FALSE(rig.hub.Arm("gox:cam0"));
    CHECK(rig.hub.Arm("gox:cam1"));
    CHECK(RateOf(rig.board->rates, 2) == doctest::Approx(5.0));
    const auto summary = rig.hub.Summary();
    CHECK(summary["participants"].size() == 2);
    CHECK(summary["started"] == true);
    CHECK(summary["ok"] == true);
    CHECK(rig.hub.Describe().find("JAI pair 5 Hz: gox:cam0 (ch2), gox:cam1 (ch3)") != std::string::npos);
}


TEST_CASE("SensorSyncHub: registrations the hardware cannot honour are refused before anything is sent") {
    Rig rig;
    rig.hub.Register("fx10", 0, 50.0);
    // Channel range, pair rate limits, conflicting pair rates, duplicate names/channels
    CHECK_THROWS_AS(rig.hub.Register("x", 4, 1.0), SensorSyncError);
    CHECK_THROWS_AS(rig.hub.Register("x", 1, 10.0), SensorSyncError); // FX pair needs >= 20 Hz
    CHECK_THROWS_AS(rig.hub.Register("x", 2, 11.0), SensorSyncError); // JAI pair 1..10 Hz
    CHECK_THROWS_AS(rig.hub.Register("x", 1, 25.0), SensorSyncError); // conflicts with 50 Hz
    CHECK_THROWS_AS(rig.hub.Register("fx10", 2, 5.0), SensorSyncError); // duplicate owner
    CHECK_THROWS_AS(rig.hub.Register("x", 0, 50.0), SensorSyncError); // duplicate channel
    CHECK_THROWS_AS(rig.hub.Register("", 2, 5.0), SensorSyncError);
    CHECK(rig.board->opens == 1);
    CHECK(rig.board->starts == 0);
    CHECK(rig.hub.Ok()); // refusals are the caller's failure, the board is fine
    CHECK_THROWS_AS(rig.hub.Arm("nobody"), SensorSyncError);
}


TEST_CASE("SensorSyncHub: without a port in config-main.yaml a participant cannot register") {
    // The port is the rig's, not a driver's: a driver that needs the board on a
    // rig that names none fails its bring-up with the key to set.
    auto board = std::make_shared<FakeBoard::State>();
    SensorSyncHub hub{"/tmp/run/raw/sensor_trigger.log", "", std::make_unique<FakeBoard>(board)};
    CHECK(hub.Describe().find("no participant") != std::string::npos);
    CHECK_THROWS_AS(hub.Register("fx10", 0, 50.0), SensorSyncError);
    CHECK(board->opens == 0);
    CHECK_FALSE(hub.HasParticipants());
    CHECK(hub.Ok()); // nothing was attempted on any board
}


TEST_CASE("SensorSyncHub: a board that will not open fails the registering participant and latches") {
    Rig rig;
    rig.board->fail_open = true;
    CHECK_THROWS_AS(rig.hub.Register("fx10", 0, 50.0), SensorSyncError);
    CHECK_FALSE(rig.hub.Ok());
    CHECK_FALSE(rig.hub.LastError().empty());
    CHECK_FALSE(rig.hub.HasParticipants());
}


TEST_CASE("SensorSyncHub: a rejected START fails the arming participant and every later one") {
    Rig rig;
    rig.board->fail_start = true;
    rig.hub.Register("fx10", 0, 50.0);
    CHECK_THROWS_AS(rig.hub.Arm("fx10"), SensorSyncError);
    CHECK_FALSE(rig.hub.Ok());
    CHECK_FALSE(rig.hub.Started());
    CHECK(rig.hub.Describe().find("FAILED") != std::string::npos);
    CHECK_THROWS_AS(rig.hub.Arm("fx10"), SensorSyncError); // no retry into a broken session
    rig.hub.Disarm("fx10"); // never started: nothing to stop
    CHECK(rig.board->stops == 0);
}


TEST_CASE("SensorSyncHub: a participant that tears down before the START blocks a later START") {
    // The Go-X armed in its Init; the FX10 failed in StartStreaming and tore down.
    // The rig is going down: an FX10-less START must not begin pulsing the Go-X.
    Rig rig;
    rig.hub.Register("fx10", 0, 50.0);
    rig.hub.Register("gox:cam0", 2, 5.0);
    CHECK_FALSE(rig.hub.Arm("gox:cam0"));
    rig.hub.Disarm("fx10");
    CHECK(rig.hub.Ended());
    CHECK_THROWS_AS(rig.hub.Arm("fx10"), SensorSyncError);
    CHECK(rig.board->starts == 0);
    CHECK_THROWS_AS(rig.hub.Register("late", 3, 5.0), SensorSyncError);
}


TEST_CASE("SensorSyncHub: integrity and stall are the board's verdicts, shared by every participant") {
    Rig rig;
    rig.hub.Register("fx10", 0, 50.0);
    CHECK(rig.hub.Arm("fx10"));
    rig.board->stalled = 20.0;
    CHECK(rig.hub.StalledSeconds() == doctest::Approx(20.0));
    CHECK(rig.hub.Ok());
    rig.board->ok = false; // the reader thread latched a protocol/loss failure
    CHECK_FALSE(rig.hub.Ok());
    rig.hub.Disarm("fx10");
    CHECK_FALSE(rig.hub.Ok());
    CHECK_FALSE(rig.hub.LastError().empty());
}
