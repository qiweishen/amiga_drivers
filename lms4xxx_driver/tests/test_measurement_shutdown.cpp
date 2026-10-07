#include <doctest/doctest.h>

#include <ostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "measurement_shutdown.h"

TEST_CASE("No measurement request needs no cleanup and claims no standby confirmation") {
    lms4xxx::MeasurementShutdown shutdown;
    int calls = 0;
    CHECK_FALSE(shutdown.Park([&](std::string_view, int) {
        ++calls;
        return std::error_code{};
    }));
    CHECK(calls == 0);
    CHECK_FALSE(shutdown.Status().measurement_start_requested);
    CHECK_FALSE(shutdown.Status().cleanup_attempted);
    CHECK_FALSE(shutdown.Status().standby_confirmed);
}

TEST_CASE("A requested measurement is parked even without a start acknowledgement or stream") {
    lms4xxx::MeasurementShutdown shutdown;
    shutdown.Arm(); // done before sending LMCstartmeas, not after receiving sAN
    CHECK(shutdown.Pending());
    std::vector<std::string> commands;
    std::vector<int> expected_status;
    const auto call = [&](std::string_view name, int status) {
        commands.emplace_back(name);
        expected_status.push_back(status);
        return std::error_code{};
    };
    CHECK_FALSE(shutdown.Park(call));
    REQUIRE(commands.size() == 2);
    CHECK(commands[0] == "SetAccessMode");
    CHECK(commands[1] == "LMCstandby");
    CHECK(expected_status[0] == 1);
    CHECK(expected_status[1] == 0);
    CHECK(shutdown.Status().cleanup_attempted);
    CHECK(shutdown.Status().standby_command_attempted);
    CHECK(shutdown.Status().standby_confirmed);
    CHECK_FALSE(shutdown.Pending());
    CHECK_FALSE(shutdown.Park(call)); // Stop, Disconnect and destructor share this result
    CHECK(commands.size() == 2);
}

TEST_CASE("Login failure leaves standby unconfirmed and cleanup is not retried by repeated stop") {
    lms4xxx::MeasurementShutdown shutdown;
    shutdown.Arm();
    const auto failure = std::make_error_code(std::errc::permission_denied);
    int calls = 0;
    const auto call = [&](std::string_view name, int) {
        ++calls;
        CHECK(name == "SetAccessMode");
        return failure;
    };
    CHECK(shutdown.Park(call) == failure);
    CHECK(shutdown.Park(call) == failure);
    CHECK(calls == 1);
    CHECK(shutdown.Status().cleanup_attempted);
    CHECK_FALSE(shutdown.Status().standby_command_attempted);
    CHECK_FALSE(shutdown.Status().standby_confirmed);
}

TEST_CASE("A refused or unanswered standby cannot become successful resource teardown") {
    lms4xxx::MeasurementShutdown shutdown;
    shutdown.Arm();
    const auto failure = std::make_error_code(std::errc::timed_out);
    int calls = 0;
    const auto call = [&](std::string_view name, int) {
        ++calls;
        return name == "SetAccessMode" ? std::error_code{} : failure;
    };
    CHECK(shutdown.Park(call) == failure);
    CHECK(shutdown.Park(call) == failure);
    CHECK(calls == 2);
    CHECK(shutdown.Status().standby_command_attempted);
    CHECK_FALSE(shutdown.Status().standby_confirmed);
}

TEST_CASE("Transport setup and command exceptions retain an unsuccessful cleanup result") {
    for (const bool fail_login : {true, false}) {
        lms4xxx::MeasurementShutdown shutdown;
        shutdown.Arm();
        int calls = 0;
        const auto call = [&](std::string_view name, int) -> std::error_code {
            ++calls;
            if (fail_login || name == "LMCstandby") throw std::runtime_error("transport failure");
            return {};
        };
        const auto failure = shutdown.Park(call);
        CHECK(static_cast<bool>(failure));
        CHECK(shutdown.Park(call) == failure);
        CHECK(calls == (fail_login ? 1 : 2));
        CHECK(shutdown.Status().cleanup_attempted);
        CHECK(shutdown.Status().standby_command_attempted == !fail_login);
        CHECK_FALSE(shutdown.Status().standby_confirmed);
    }
}
