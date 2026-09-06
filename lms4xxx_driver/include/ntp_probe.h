#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>


// SNTP liveness probe (RFC 4330): the device syncs to this server, so a dead server is a run fault
namespace lms4xxx::Ntp {
    inline constexpr std::size_t kPacketBytes = 48;
    using Packet = std::array<std::uint8_t, kPacketBytes>;

    // Client request (LI 0, VN 4, mode 3); `nonce` goes into the transmit timestamp so the answer can be matched
    Packet BuildRequest(std::uint64_t nonce);

    // Healthy = VN 3/4, mode 4/5, LI != alarm, stratum 1..15, originate == request transmit, non-zero transmit.
    // `detail` carries the reason on failure and "stratum N" on success
    bool EvaluateResponse(const Packet &request, const std::uint8_t *response, std::size_t len, std::string &detail);

    // One round trip over UDP/123
    bool ProbeServer(const std::string &server_ip, int timeout_ms, std::string &detail);
} // namespace lms4xxx::Ntp
