// StopController and the stop-reason Names (src/signal_stop.cpp). The signal
// handlers themselves are process-global and not exercised here.

#include "signal_stop.h"

#include <doctest/doctest.h>

#include <string>

TEST_CASE("signal_stop: a fresh controller has no reason") {
    gox::StopController stop;
    CHECK_FALSE(stop.StopRequested());
    CHECK(stop.Reason() == gox::StopReason::kNone);
}

TEST_CASE("signal_stop: recording errors override an ordinary stop") {
    // The acquisition thread, the writer thread and the signal handler can all
    // request a stop; recording failure takes priority even during final close.
    gox::StopController stop;
    stop.RequestStop(gox::StopReason::kLimitReached);
    CHECK(stop.StopRequested());
    CHECK(stop.Reason() == gox::StopReason::kLimitReached);

    stop.RequestStop(gox::StopReason::kError);
    CHECK(stop.Reason() == gox::StopReason::kError);
    stop.RequestStop(gox::StopReason::kSignal);
    CHECK(stop.Reason() == gox::StopReason::kError);
}

TEST_CASE("signal_stop: every reason has a name") {
    CHECK(std::string(gox::StopReasonName(gox::StopReason::kNone)) == "none");
    CHECK(std::string(gox::StopReasonName(gox::StopReason::kSignal)) == "signal");
    CHECK(std::string(gox::StopReasonName(gox::StopReason::kLimitReached)) == "limit_reached");
    CHECK(std::string(gox::StopReasonName(gox::StopReason::kError)) == "error");
    CHECK(std::string(gox::StopReasonName(gox::StopReason::kExternal)) == "external");
    // An out-of-range value must still render, not fall off the switch.
    CHECK(std::string(gox::StopReasonName(static_cast<gox::StopReason>(99))) == "unknown");
}
