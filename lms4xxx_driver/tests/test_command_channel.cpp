#include <doctest/doctest.h>

#include "command_channel.h"
#include "error.h"

TEST_CASE("An unrelated command reply leaves the CoLa channel uncertain until reconnect") {
    lms4xxx::CommandChannel channel(100, "test");
    lms4xxx::ColaBMessage reply;
    reply.command_type = lms4xxx::CommandType::kMethodAnswer;
    reply.command_name = "LMCstartmeas";
    CHECK(channel.ValidateResponse(reply, lms4xxx::CommandType::kMethodAnswer, "Run") ==
          lms4xxx::make_error_code(lms4xxx::ErrorCode::kUnexpectedResponse));
    CHECK(channel.Uncertain());

    reply.command_name = "Run";
    CHECK_FALSE(channel.ValidateResponse(reply, lms4xxx::CommandType::kMethodAnswer, "Run"));
    CHECK(channel.Uncertain()); // a subsequent valid frame does not prove realignment
    channel.Reset(); // called by the lifecycle only after a new connection
    CHECK_FALSE(channel.Uncertain());

    CHECK(channel.ValidateResponse(reply, lms4xxx::CommandType::kReadAnswer, "Run") ==
          lms4xxx::make_error_code(lms4xxx::ErrorCode::kUnexpectedResponse));
    CHECK(channel.Uncertain());
}

TEST_CASE("A missing command transport is rejected without a read or write") {
    lms4xxx::CommandChannel channel(100, "test");
    lms4xxx::ColaBMessage reply;
    CHECK(channel.SendAndReceive(nullptr, {}, reply, 100) ==
          lms4xxx::make_error_code(lms4xxx::ErrorCode::kNotConnected));
    CHECK(channel.Uncertain());
}

TEST_CASE("CoLa method deadlines retain the method floor and longer configured waits") {
    CHECK(lms4xxx::CommandChannel(100, "test").MethodTimeout() == 5000);
    CHECK(lms4xxx::CommandChannel(6000, "test").MethodTimeout() == 6000);
}
