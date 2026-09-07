/// @file test_frame_receiver.cpp
/// @brief FrameReceiver: framing, resynchronisation and error reporting.
///
/// This is the boundary between a TCP byte stream and the parser. Recovery must
/// preserve split/overlapping STX candidates, report corruption once and keep
/// reporting new garbage so a dead stream remains detectable.

#include <doctest/doctest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "cola_b.h"
#include "frame_receiver.h"


namespace ColaBCodec = lms4xxx::ColaBCodec;
namespace CommandType = lms4xxx::CommandType;
using lms4xxx::FrameReceiver;
using lms4xxx::RawFrame;

namespace {
    struct Collector {
        std::vector<std::vector<std::uint8_t> > frames;
        std::vector<FrameReceiver::FrameError> errors;
        std::uint64_t last_consecutive = 0;

        FrameReceiver Make(std::size_t max_frame = 64 * 1024) {
            return {
                [this](RawFrame &&frame) { frames.push_back(frame.data); },
                [this](FrameReceiver::FrameError error, std::uint64_t consecutive) {
                    errors.push_back(error);
                    last_consecutive = consecutive;
                },
                max_frame, "test"};
        }
    };

    std::vector<std::uint8_t> Frame(const char *name, const std::vector<std::uint8_t> &params = {}) {
        return ColaBCodec::Encode(CommandType::kEventNotify, name, params);
    }

    // The data part of an encoded frame, i.e. what the callback receives.
    std::vector<std::uint8_t> DataOf(const std::vector<std::uint8_t> &frame) {
        return {frame.begin() + 8, frame.end() - 1};
    }
} // namespace


TEST_CASE("FrameReceiver extracts a single well-formed frame") {
    Collector c;
    auto receiver = c.Make();
    const auto frame = Frame("LMDscandata", {0x01, 0x02, 0x03});
    receiver.Feed(frame.data(), frame.size());

    REQUIRE(c.frames.size() == 1);
    CHECK(c.frames[0] == DataOf(frame));
    CHECK(c.errors.empty());
}

TEST_CASE("FrameReceiver counts a sustained stream with no STX") {
    Collector c;
    auto receiver = c.Make();
    const std::vector<std::uint8_t> garbage(64, 0x55);
    for (int i = 0; i < 4; ++i) receiver.Feed(garbage.data(), garbage.size());
    CHECK(c.frames.empty());
    CHECK(c.errors.size() == 4);
    CHECK(c.last_consecutive == 4);
    const auto good = Frame("LMDscandata", {1, 2, 3});
    receiver.Feed(good.data(), good.size());
    REQUIRE(c.frames.size() == 1);
    CHECK(c.frames.front() == DataOf(good));
}

TEST_CASE("FrameReceiver reassembles a frame split across Feed calls") {
    // The stream arrives in whatever chunks the kernel hands over; a frame can
    // be split anywhere, the STX itself included.
    const auto frame = Frame("LMDscandata", {0xAA, 0xBB, 0xCC, 0xDD});
    for (std::size_t split = 1; split < frame.size(); ++split) {
        Collector c;
        auto receiver = c.Make();
        receiver.Feed(frame.data(), split);
        receiver.Feed(frame.data() + split, frame.size() - split);
        CHECK_MESSAGE(c.frames.size() == 1, "split after " << split << " byte(s)");
        if (c.frames.size() == 1) {
            CHECK(c.frames[0] == DataOf(frame));
        }
        CHECK(c.errors.empty());
    }
}

TEST_CASE("FrameReceiver delivers several frames from one Feed") {
    Collector c;
    auto receiver = c.Make();
    std::vector<std::uint8_t> stream;
    for (int i = 0; i < 5; ++i) {
        const auto frame = Frame("LMDscandata", {static_cast<std::uint8_t>(i)});
        stream.insert(stream.end(), frame.begin(), frame.end());
    }
    receiver.Feed(stream.data(), stream.size());
    REQUIRE(c.frames.size() == 5);
    CHECK(c.frames[4].back() == 4);
}

TEST_CASE("FrameReceiver skips leading garbage and finds the next STX") {
    Collector c;
    auto receiver = c.Make();
    std::vector<std::uint8_t> stream = {0x11, 0x22, 0x33};
    const auto frame = Frame("LMDscandata", {0x07});
    stream.insert(stream.end(), frame.begin(), frame.end());
    receiver.Feed(stream.data(), stream.size());

    REQUIRE(c.frames.size() == 1);
    CHECK(c.frames[0] == DataOf(frame));
    // Skipped bytes are reported once so a wrong-protocol peer is visible
    REQUIRE(c.errors.size() == 1);
    CHECK(c.errors[0] == FrameReceiver::FrameError::kGarbage);
}

TEST_CASE("FrameReceiver reports a checksum mismatch and recovers") {
    Collector c;
    auto receiver = c.Make();
    auto bad = Frame("LMDscandata", {0x01});
    bad.back() ^= 0xFF; // corrupt the XOR checksum
    const auto good = Frame("LMDscandata", {0x02});

    std::vector<std::uint8_t> stream = bad;
    stream.insert(stream.end(), good.begin(), good.end());
    receiver.Feed(stream.data(), stream.size());

    REQUIRE(c.errors.size() == 1);
    CHECK(c.errors[0] == FrameReceiver::FrameError::kChecksumMismatch);
    // The good frame behind it must still arrive.
    REQUIRE(c.frames.size() == 1);
    CHECK(c.frames[0] == DataOf(good));
}

TEST_CASE("FrameReceiver accounts rejected tails once across every Feed boundary") {
    auto checksum_bad = Frame("LMDscandata", {0x01});
    checksum_bad.back() ^= 0xFF;
    const std::vector<std::uint8_t> length_bad = {0x02, 0x02, 0x02, 0x02, 0x00, 0x10, 0x00, 0x00};
    const std::vector<std::uint8_t> empty = {0x02, 0x02, 0x02, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00};
    const auto good = Frame("LMDscandata", {0x09});

    for (const auto &bad: {checksum_bad, length_bad, empty}) {
        auto stream = bad;
        stream.insert(stream.end(), good.begin(), good.end());
        for (std::size_t split = 1; split < stream.size(); ++split) {
            CAPTURE(bad.size());
            CAPTURE(split);
            Collector c;
            auto receiver = c.Make(1024);
            receiver.Feed(stream.data(), split);
            receiver.Feed(stream.data() + split, stream.size() - split);
            REQUIRE(c.errors.size() == 1);
            CHECK(c.errors.front() == (bad == checksum_bad ? FrameReceiver::FrameError::kChecksumMismatch
                                                         : FrameReceiver::FrameError::kLengthOutOfRange));
            REQUIRE(c.frames.size() == 1);
            CHECK(c.frames.front() == DataOf(good));

            // Recovery resets the consecutive counter; the carried tail must
            // not leak into the accounting of a later corrupted telegram.
            receiver.Feed(checksum_bad.data(), checksum_bad.size());
            CHECK(c.errors.size() == 2);
            CHECK(c.last_consecutive == 1);
        }
    }
}

TEST_CASE("FrameReceiver keeps detecting new garbage after a rejected candidate") {
    Collector c;
    auto receiver = c.Make();
    auto bad = Frame("LMDscandata", {0x01});
    bad.back() ^= 0xFF;
    receiver.Feed(bad.data(), bad.size());
    REQUIRE(c.errors.size() == 1);

    const std::vector<std::uint8_t> garbage(64, 0x55);
    for (std::size_t i = 0; i < 4; ++i) {
        receiver.Feed(garbage.data(), garbage.size());
        REQUIRE(c.errors.size() == i + 2);
        CHECK(c.errors.back() == FrameReceiver::FrameError::kGarbage);
        CHECK(c.last_consecutive == i + 2);
    }
    CHECK(c.frames.empty());
}

TEST_CASE("FrameReceiver recovers an STX overlapping a rejected candidate") {
    const auto good = Frame("LMDscandata", {0x09});
    for (std::size_t prefix = 1; prefix <= 3; ++prefix) {
        std::vector<std::uint8_t> stream(prefix, 0x02);
        stream.insert(stream.end(), good.begin(), good.end());
        for (std::size_t split = 1; split < stream.size(); ++split) {
            CAPTURE(prefix);
            CAPTURE(split);
            Collector c;
            auto receiver = c.Make(1024);
            receiver.Feed(stream.data(), split);
            receiver.Feed(stream.data() + split, stream.size() - split);
            CHECK_FALSE(c.errors.empty());
            REQUIRE(c.frames.size() == 1);
            CHECK(c.frames.front() == DataOf(good));
        }
    }
}

TEST_CASE("FrameReceiver rejects a length field beyond the maximum") {
    Collector c;
    auto receiver = c.Make(1024); // small ceiling so the header alone is enough
    std::vector<std::uint8_t> stream = {0x02, 0x02, 0x02, 0x02, 0x00, 0x10, 0x00, 0x00};
    receiver.Feed(stream.data(), stream.size());

    REQUIRE(c.errors.size() == 1);
    CHECK(c.errors[0] == FrameReceiver::FrameError::kLengthOutOfRange);
    CHECK(c.frames.empty());
}

TEST_CASE("Consecutive framing errors are counted so a dead stream can be detected") {
    // A run of 0x02 looks like an STX at every offset, so resync keeps finding
    // rejected candidates and never converges. The driver latches a fault above a
    // threshold; that only works if `consecutive` really counts from the last
    // GOOD frame.
    Collector c;
    auto receiver = c.Make(1024);
    const std::vector<std::uint8_t> garbage(4096, 0x02);
    receiver.Feed(garbage.data(), garbage.size());

    CHECK(c.frames.empty());
    CHECK(c.errors.size() > 100);
    CHECK(c.last_consecutive == c.errors.size());

    // A valid frame ends the burst; the following simple checksum failure
    // must start at one, without adding its discarded tail as another error.
    const auto good = Frame("LMDscandata", {0x09});
    receiver.Feed(good.data(), good.size());
    REQUIRE(c.frames.size() == 1);

    auto bad = Frame("LMDscandata", {0x01});
    bad.back() ^= 0xFF;
    receiver.Feed(bad.data(), bad.size());
    CHECK(c.last_consecutive == 1);
}

TEST_CASE("FrameReceiver rejects a frame with an empty data part") {
    Collector c;
    auto receiver = c.Make();
    // STX + length 0 + checksum of nothing: no CoLa B telegram is empty
    const std::vector<std::uint8_t> stream = {0x02, 0x02, 0x02, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00};
    receiver.Feed(stream.data(), stream.size());
    CHECK(c.frames.empty());
    REQUIRE(c.errors.size() == 1);
    CHECK(c.errors[0] == FrameReceiver::FrameError::kLengthOutOfRange);
}

TEST_CASE("FrameReceiver survives a stream fed one byte at a time") {
    // Worst-case chunking: every internal buffer boundary is exercised, and the
    // three-byte tail that keeps a split STX alive is used on every call.
    Collector c;
    auto receiver = c.Make();
    std::vector<std::uint8_t> stream;
    for (int i = 0; i < 3; ++i) {
        const auto frame = Frame("LMDscandata", {static_cast<std::uint8_t>(0x40 + i), 0x00, 0xFF});
        stream.insert(stream.end(), frame.begin(), frame.end());
    }
    for (const auto byte: stream) {
        receiver.Feed(&byte, 1);
    }
    CHECK(c.frames.size() == 3);
    CHECK(c.errors.empty());
}

TEST_CASE("FrameReceiver counts bytes between frames as garbage and never rewinds into a delivered frame") {
    Collector c;
    auto receiver = c.Make();
    const auto first = Frame("LMDscandata", {0x01});
    const auto second = Frame("LMDscandata", {0x02});

    // Garbage before the STX is a framing error, so a stream that is not CoLa B trips the fault
    std::vector<std::uint8_t> stream = {0xAA, 0xBB, 0xCC};
    stream.insert(stream.end(), first.begin(), first.end());
    receiver.Feed(stream.data(), stream.size());
    REQUIRE(c.frames.size() == 1);
    REQUIRE(c.errors.size() == 1);
    CHECK(c.errors[0] == FrameReceiver::FrameError::kGarbage);
    CHECK(c.last_consecutive == 1);

    // The 3-byte STX carry-over must not resurrect the tail of the frame just delivered
    receiver.Feed(second.data(), 2);
    receiver.Feed(second.data() + 2, second.size() - 2);
    REQUIRE(c.frames.size() == 2);
    CHECK(c.frames[1] == DataOf(second));
    CHECK(c.errors.size() == 1); // the good frame reset the burst; no new error
}

TEST_CASE("FrameReceiver rejects an empty data part") {
    Collector c;
    auto receiver = c.Make();
    const std::vector<std::uint8_t> empty = {0x02, 0x02, 0x02, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00};
    receiver.Feed(empty.data(), empty.size());
    CHECK(c.frames.empty());
    REQUIRE(c.errors.size() == 1);
    CHECK(c.errors[0] == FrameReceiver::FrameError::kLengthOutOfRange);
}
