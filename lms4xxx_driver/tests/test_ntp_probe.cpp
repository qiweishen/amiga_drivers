// RFC 4330 acceptance matrix over synthetic 48-byte answers (no socket)

#include <doctest/doctest.h>

#include <cstdint>
#include <string>

#include "ntp_probe.h"

using namespace lms4xxx::Ntp;

namespace {
    // A healthy stratum-2 server answer that echoes the request's transmit timestamp
    Packet GoodResponse(const Packet &request) {
        Packet r{};
        r[0] = 0x24; // LI 0, VN 4, mode 4 (server)
        r[1] = 2; // stratum
        for (int i = 0; i < 8; ++i) {
            r[24 + i] = request[40 + i]; // originate = our transmit
            r[40 + i] = static_cast<std::uint8_t>(0xA0 + i); // transmit: non-zero
        }
        return r;
    }
} // namespace

TEST_CASE("The request is a VN4 client packet carrying the nonce as transmit timestamp") {
    const Packet req = BuildRequest(0x0102030405060708ull);
    CHECK(req[0] == 0x23);
    CHECK(req[40] == 0x01);
    CHECK(req[47] == 0x08);
    for (int i = 1; i < 40; ++i) {
        CHECK(req[i] == 0);
    }
}

TEST_CASE("A healthy answer is accepted and reports the stratum") {
    const Packet req = BuildRequest(42);
    const Packet resp = GoodResponse(req);
    std::string detail;
    CHECK(EvaluateResponse(req, resp.data(), resp.size(), detail));
    CHECK(detail == "stratum 2");

    SUBCASE("VN3 and broadcast mode are fine too") {
        Packet r = resp;
        r[0] = 0x1D; // LI 0, VN 3, mode 5
        CHECK(EvaluateResponse(req, r.data(), r.size(), detail));
    }
}

TEST_CASE("Every rejection names its reason") {
    const Packet req = BuildRequest(7);
    std::string detail;
    SUBCASE("short datagram") {
        const Packet resp = GoodResponse(req);
        CHECK_FALSE(EvaluateResponse(req, resp.data(), 40, detail));
        CHECK(detail == "short response");
    }
    SUBCASE("client mode echoed back (a reflector, not a server)") {
        Packet r = GoodResponse(req);
        r[0] = 0x23;
        CHECK_FALSE(EvaluateResponse(req, r.data(), r.size(), detail));
        CHECK(detail.find("mode 3") != std::string::npos);
    }
    SUBCASE("unknown protocol version") {
        Packet r = GoodResponse(req);
        r[0] = 0x0C; // VN 1, mode 4
        CHECK_FALSE(EvaluateResponse(req, r.data(), r.size(), detail));
        CHECK(detail.find("version 1") != std::string::npos);
    }
    SUBCASE("leap indicator alarm") {
        Packet r = GoodResponse(req);
        r[0] = 0xE4; // LI 3
        CHECK_FALSE(EvaluateResponse(req, r.data(), r.size(), detail));
        CHECK(detail.find("alarm") != std::string::npos);
    }
    SUBCASE("kiss-of-death and unsynchronized strata") {
        Packet r = GoodResponse(req);
        r[1] = 0;
        CHECK_FALSE(EvaluateResponse(req, r.data(), r.size(), detail));
        r[1] = 16;
        CHECK_FALSE(EvaluateResponse(req, r.data(), r.size(), detail));
        CHECK(detail.find("stratum 16") != std::string::npos);
    }
    SUBCASE("an answer to somebody else's request") {
        Packet r = GoodResponse(req);
        r[31] ^= 0x01;
        CHECK_FALSE(EvaluateResponse(req, r.data(), r.size(), detail));
        CHECK(detail.find("originate") != std::string::npos);
    }
    SUBCASE("zero transmit timestamp") {
        Packet r = GoodResponse(req);
        for (int i = 40; i < 48; ++i) {
            r[i] = 0;
        }
        CHECK_FALSE(EvaluateResponse(req, r.data(), r.size(), detail));
        CHECK(detail.find("zero transmit") != std::string::npos);
    }
}

TEST_CASE("An unreachable address fails fast with a reason") {
    std::string detail;
    CHECK_FALSE(ProbeServer("not-an-ip", 100, detail));
    CHECK(detail.find("invalid server IP") != std::string::npos);
    // 127.0.0.1:123 is normally closed: ICMP port unreachable or the timeout
    CHECK_FALSE(ProbeServer("127.0.0.1", 200, detail));
    CHECK_FALSE(detail.empty());
}
