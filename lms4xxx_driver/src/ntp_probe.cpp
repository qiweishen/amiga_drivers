#include "ntp_probe.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <spdlog/fmt/fmt.h>


namespace lms4xxx::Ntp {
    namespace {
        constexpr std::uint16_t kPort = 123;
    }


    Packet BuildRequest(std::uint64_t nonce) {
        Packet pkt{};
        pkt[0] = 0x23; // LI 0, VN 4, mode 3 (client)
        for (int i = 0; i < 8; ++i) {
            pkt[40 + i] = static_cast<std::uint8_t>(nonce >> (56 - 8 * i));
        }
        return pkt;
    }


    bool EvaluateResponse(const Packet &request, const std::uint8_t *response, std::size_t len, std::string &detail) {
        if (len < kPacketBytes) {
            detail = "short response";
            return false;
        }
        const int li = response[0] >> 6;
        const int version = (response[0] >> 3) & 0x07;
        const int mode = response[0] & 0x07;
        const int stratum = response[1];
        if (version < 3 || version > 4) {
            detail = fmt::format("unexpected NTP version {}", version);
            return false;
        }
        if (mode != 4 && mode != 5) {
            detail = fmt::format("unexpected mode {}", mode);
            return false;
        }
        if (li == 3) {
            detail = "server reports LI=alarm (unsynchronized)";
            return false;
        }
        if (stratum == 0 || stratum > 15) {
            detail = fmt::format("bad stratum {} (kiss-of-death / unsynchronized)", stratum);
            return false;
        }
        if (!std::equal(request.begin() + 40, request.begin() + 48, response + 24)) {
            detail = "originate timestamp does not echo the request";
            return false;
        }
        if (std::all_of(response + 40, response + 48, [](std::uint8_t b) { return b == 0; })) {
            detail = "zero transmit timestamp";
            return false;
        }
        detail = fmt::format("stratum {}", stratum);
        return true;
    }


    bool ProbeServer(const std::string &server_ip, int timeout_ms, std::string &detail) {
        const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (fd < 0) {
            detail = std::strerror(errno);
            return false;
        }
        timeval tv{};
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(kPort);
        if (::inet_pton(AF_INET, server_ip.c_str(), &addr.sin_addr) != 1) {
            ::close(fd);
            detail = "invalid server IP '" + server_ip + "'";
            return false;
        }
        // connect() so recv() only accepts the server's answer
        if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
            detail = std::strerror(errno);
            ::close(fd);
            return false;
        }

        const auto nonce = static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()) ^
                           (static_cast<std::uint64_t>(::getpid()) << 48);
        const Packet request = BuildRequest(nonce);
        if (::send(fd, request.data(), request.size(), 0) != static_cast<ssize_t>(request.size())) {
            detail = std::strerror(errno);
            ::close(fd);
            return false;
        }
        std::uint8_t response[kPacketBytes];
        const ssize_t n = ::recv(fd, response, sizeof(response), 0);
        ::close(fd);
        if (n < 0) {
            detail = std::string("no response: ") + std::strerror(errno);
            return false;
        }
        return EvaluateResponse(request, response, static_cast<std::size_t>(n), detail);
    }
} // namespace lms4xxx::Ntp
