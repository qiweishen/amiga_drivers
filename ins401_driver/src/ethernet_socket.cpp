#include "ethernet_socket.h"

#include <algorithm>
#include <arpa/inet.h>
#include <boost/crc.hpp>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <ifaddrs.h>
#include <iomanip>
#include <linux/filter.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <net/if.h>
#include <string>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

#include "ins401_protocol.h"
#include "ins401_tool.h"

#include "utility.h"
#include <spdlog/spdlog.h>


namespace {
    constexpr std::string_view kModule = "Ethernet Socket";
}


EthernetSocket::EthernetSocket(std::string interface_name, const MacAddress &target_mac,
                               const std::size_t recv_buffer_size,
                               const bool enable_bpf) : interface_name_(interface_name), target_mac_(target_mac),
                                                        recv_buffer_size_(recv_buffer_size), enable_bpf_(enable_bpf) {
    CreateSocket();
    if (enable_bpf_) {
        Ethernet::SetupBpfFilter(target_mac_, local_mac_, socket_fd_);
    }
    SetupEpoll();
}

EthernetSocket::~EthernetSocket() {
    CloseEthernetSocket();
}


std::ptrdiff_t EthernetSocket::Send(const std::vector<uint8_t> &frame) const {
    sockaddr_ll sll{};
    sll.sll_family = AF_PACKET;
    sll.sll_ifindex = if_index_;
    sll.sll_protocol = htons(ETH_P_ALL);
    sll.sll_halen = ETH_ALEN;
    std::memcpy(sll.sll_addr, target_mac_.data(), ETH_ALEN);

    return sendto(socket_fd_, frame.data(), frame.size(), 0, reinterpret_cast<sockaddr *>(&sll), sizeof(sll));
}


std::optional<EthernetFrame> EthernetSocket::Receive(const int timeout_ms) const {
    epoll_event ev{};
    int ret = epoll_wait(epoll_fd_, &ev, 1, timeout_ms);
    if (ret <= 0) {
        return std::nullopt;
    }

    std::array<std::uint8_t, kMaxFrameSize> buffer{};
    const ssize_t len = recv(socket_fd_, buffer.data(), buffer.size(), 0);
    if (len <= 0) {
        return std::nullopt;
    }

    EthernetFrame frame;
    frame.assign(buffer.begin(), buffer.begin() + len);

    return frame;
}


std::vector<EthernetFrame> EthernetSocket::ReceiveBatch(std::size_t max_frames) const {
    std::vector<EthernetFrame> frames;
    frames.reserve(max_frames);

    std::array<std::uint8_t, kMaxFrameSize> buffer{};

    while (frames.size() < max_frames) {
        const ssize_t len = recv(socket_fd_, buffer.data(), buffer.size(), MSG_DONTWAIT);
        if (len <= 0) {
            break;
        }

        EthernetFrame frame;
        frame.assign(buffer.begin(), buffer.begin() + len);

        frames.push_back(std::move(frame));
    }

    return frames;
}


void EthernetSocket::CreateSocket() {
    socket_fd_ = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (socket_fd_ < 0) {
        if (errno == EPERM) {
            Common::Log::log_and_throw(kModule, "Root privileges required to create raw socket");
        } else {
            Common::Log::log_and_throw(kModule, "Failed to create raw socket", std::strerror(errno));
        }
    }

    int flags = fcntl(socket_fd_, F_GETFL, 0);
    if (flags < 0 || fcntl(socket_fd_, F_SETFL, flags | O_NONBLOCK) < 0) {
        Common::Log::log_message(spdlog::level::warn, kModule, "Failed to set non-blocking mode", std::strerror(errno));
    }

    ifreq ifr{};
    std::strncpy(ifr.ifr_name, interface_name_.c_str(), IFNAMSIZ - 1);

    if (ioctl(socket_fd_, SIOCGIFINDEX, &ifr) < 0) {
        Common::Log::log_and_throw(kModule,
                         fmt::format("Failed to get index for interface {}", interface_name_), std::strerror(errno));
    }
    if_index_ = ifr.ifr_ifindex;

    if (ioctl(socket_fd_, SIOCGIFHWADDR, &ifr) < 0) {
        Common::Log::log_and_throw(kModule,
                         fmt::format("Failed to get MAC for interface {}", interface_name_), std::strerror(errno));
    }
    std::memcpy(local_mac_.data(), ifr.ifr_hwaddr.sa_data, kMacAddressSize);

    sockaddr_ll sll{};
    sll.sll_family = AF_PACKET;
    sll.sll_ifindex = if_index_;
    sll.sll_protocol = htons(ETH_P_ALL);
    if (bind(socket_fd_, reinterpret_cast<sockaddr *>(&sll), sizeof(sll)) < 0) {
        Common::Log::log_and_throw(kModule,
                         fmt::format("Failed to bind socket to interface {}", interface_name_), std::strerror(errno));
    }

    if (recv_buffer_size_ > 0) {
        int buf_size = static_cast<int>(std::min(recv_buffer_size_,
                                                 static_cast<size_t>(std::numeric_limits<int>::max())));
        if (setsockopt(socket_fd_, SOL_SOCKET, SO_RCVBUF, &buf_size, sizeof(buf_size)) < 0) {
            Common::Log::log_message(spdlog::level::warn, kModule,
                             fmt::format("Failed to set receive buffer size on interface {}", interface_name_),
                             std::strerror(errno));
        }
    }
}


void EthernetSocket::SetupEpoll() {
    epoll_fd_ = epoll_create1(0);
    if (epoll_fd_ < 0) {
        Common::Log::log_and_throw(kModule, "Failed to create epoll instance", std::strerror(errno));
    }

    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = socket_fd_;

    if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, socket_fd_, &ev) < 0) {
        Common::Log::log_and_throw(kModule, "Failed to add socket to epoll", std::strerror(errno));
        close(epoll_fd_);
        epoll_fd_ = -1;
    }
}


void EthernetSocket::CloseEthernetSocket() {
    if (epoll_fd_ >= 0) {
        close(epoll_fd_);
        epoll_fd_ = -1;
    }
    if (socket_fd_ >= 0) {
        close(socket_fd_);
        socket_fd_ = -1;
    }
}


namespace Ethernet {
    std::vector<std::pair<std::string, std::string> > GetNetworkInterfaces() {
        std::vector<std::pair<std::string, std::string> > interfaces;

        // Retrieve linked list of network interfaces
        ifaddrs *ifaddr = nullptr;
        if (getifaddrs(&ifaddr) == -1) {
            Common::Log::log_and_throw(kModule, "Failed to get network interfaces", std::strerror(errno));
            return interfaces;
        }

        auto ifaddr_guard = std::unique_ptr<ifaddrs, decltype(&freeifaddrs)>(ifaddr, freeifaddrs);
        const int fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (fd < 0) {
            Common::Log::log_and_throw(kModule, "Failed to create socket for ioctl", std::strerror(errno));
            return interfaces;
        }
        FdGuard fd_guard(fd);

        // Iterate through all network interfaces
        for (ifaddrs *ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
            // Skip invalid entries
            if (!ifa->ifa_addr || !ifa->ifa_name) {
                continue;
            }

            // Skip loopback interfaces
            if (strcmp(ifa->ifa_name, "lo") == 0 || strcmp(ifa->ifa_name, "lo0") == 0) {
                continue;
            }

            // Process only AF_PACKET (Linux) interfaces for MAC address
            if (ifa->ifa_addr->sa_family == AF_PACKET) {
                const auto *sll = reinterpret_cast<const sockaddr_ll *>(ifa->ifa_addr);

                // Verify MAC address length (6 bytes for standard Ethernet MAC)
                if (sll->sll_halen != 6) {
                    continue;
                }
                // Prepare interface request structure for ioctl
                ifreq ifr{};
                strncpy(ifr.ifr_name, ifa->ifa_name, IFNAMSIZ - 1);
                ifr.ifr_name[IFNAMSIZ - 1] = '\0';

                // Query interface flags using the managed socket
                if (ioctl(fd, SIOCGIFFLAGS, &ifr) >= 0) {
                    const bool is_up = (ifr.ifr_flags & IFF_UP) != 0;
                    const bool is_running = (ifr.ifr_flags & IFF_RUNNING) != 0;

                    if (is_up && is_running) {
                        std::string mac_str = ParseMacAddress(sll->sll_addr);
                        interfaces.emplace_back(ifa->ifa_name, std::move(mac_str));
                    }
                } else {
                    Common::Log::log_message(spdlog::level::warn, kModule,
                                     fmt::format("Failed to get flags for interface {}", ifa->ifa_name),
                                     std::strerror(errno));
                }
            }
        }
        return interfaces;
    }


    bool SetupBpfFilter(const MacAddress target_mac, const MacAddress local_mac, const int socket_fd) {
        /*
         * BPF filter for bidirectional MAC matching:
         * ACCEPT if: (src=target AND dst=local) OR (src=local AND dst=target)
         *
         * Ethernet frame layout:
         *   Offset 0-5:  Destination MAC
         *   Offset 6-11: Source MAC
         */
        // Prepare MAC addresses for BPF comparison (network byte order)
        std::uint32_t target_mac_hi, local_mac_hi;
        std::uint16_t target_mac_lo, local_mac_lo;

        std::memcpy(&target_mac_hi, target_mac.data(), 4);
        std::memcpy(&target_mac_lo, target_mac.data() + 4, 2);
        std::memcpy(&local_mac_hi, local_mac.data(), 4);
        std::memcpy(&local_mac_lo, local_mac.data() + 4, 2);

        target_mac_hi = ntohl(target_mac_hi);
        target_mac_lo = ntohs(target_mac_lo);
        local_mac_hi = ntohl(local_mac_hi);
        local_mac_lo = ntohs(local_mac_lo);

        sock_filter filter[] = {
            // === Check: src=target AND dst=local (incoming) ===
            // [0] Load src MAC first 4 bytes
            BPF_STMT(BPF_LD | BPF_W | BPF_ABS, 6),
            // [1] If not equal, jump to [8]
            BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, target_mac_hi, 0, 6),
            // [2] Load src MAC last 2 bytes
            BPF_STMT(BPF_LD | BPF_H | BPF_ABS, 10),
            // [3] If not equal, jump to [8]
            BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, target_mac_lo, 0, 4),
            // [4] Load dst MAC first 4 bytes
            BPF_STMT(BPF_LD | BPF_W | BPF_ABS, 0),
            // [5] If not equal, jump to [8]
            BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, local_mac_hi, 0, 2),
            // [6] Load dst MAC last 2 bytes
            BPF_STMT(BPF_LD | BPF_H | BPF_ABS, 4),
            // [7] If equal, jump to ACCEPT [16]; else continue
            BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, local_mac_lo, 8, 0),

            // === Check: src=local AND dst=target (outgoing, optional) ===
            // [8] Load src MAC first 4 bytes
            BPF_STMT(BPF_LD | BPF_W | BPF_ABS, 6),
            // [9] If not equal, jump to DROP [15]
            BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, local_mac_hi, 0, 5),
            // [10] Load src MAC last 2 bytes
            BPF_STMT(BPF_LD | BPF_H | BPF_ABS, 10),
            // [11] If not equal, jump to DROP
            BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, local_mac_lo, 0, 3),
            // [12] Load dst MAC first 4 bytes
            BPF_STMT(BPF_LD | BPF_W | BPF_ABS, 0),
            // [13] If not equal, jump to DROP
            BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, target_mac_hi, 0, 1),
            // [14] Load dst MAC last 2 bytes
            BPF_STMT(BPF_LD | BPF_H | BPF_ABS, 4),
            // [15] If equal, jump to ACCEPT; else DROP
            BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, target_mac_lo, 0, 1),

            // [16] ACCEPT: return max frame size
            BPF_STMT(BPF_RET | BPF_K, 0xFFFF),
            // [17] DROP: return 0
            BPF_STMT(BPF_RET | BPF_K, 0),
        };

        sock_fprog prog = {
            .len = static_cast<unsigned short>(std::size(filter)),
            .filter = filter,
        };

        if (setsockopt(socket_fd, SOL_SOCKET, SO_ATTACH_FILTER, &prog, sizeof(prog)) < 0) {
            Common::Log::log_message(spdlog::level::warn, kModule, "Failed to attach BPF filter", std::strerror(errno));
            return false;
        }

        return true;
    }


    // Wire layout: [DstMAC(6)|SrcMAC(6)|EthLen(2)|0x5555(2)|MsgID(2)|PayloadLen(4)|Payload(N)|CRC16(2)|Pad]
    std::vector<uint8_t> BuildAceinnaPacket(const MacAddress target_mac, const MacAddress local_mac,
                                            const std::array<uint8_t, 2> message_id, const uint8_t *payload,
                                            const size_t payload_length) {
        std::vector<uint8_t> frame;
        frame.insert(frame.end(), target_mac.data(), target_mac.data() + kMacAddressSize);
        frame.insert(frame.end(), local_mac.data(), local_mac.data() + kMacAddressSize);

        std::vector<uint8_t> aceinna_packet;
        aceinna_packet.insert(aceinna_packet.end(), COMMAND_START_BYTES.data(), COMMAND_START_BYTES.data() + 2);
        aceinna_packet.insert(aceinna_packet.end(), message_id.data(), message_id.data() + 2);

        if (payload != nullptr && payload_length > 0) {
            // Payload length field - LSB-first
            const auto length = static_cast<uint32_t>(payload_length);
            aceinna_packet.push_back(static_cast<uint8_t>(length & 0xFF));
            aceinna_packet.push_back(static_cast<uint8_t>((length >> 8) & 0xFF));
            aceinna_packet.push_back(static_cast<uint8_t>((length >> 16) & 0xFF));
            aceinna_packet.push_back(static_cast<uint8_t>((length >> 24) & 0xFF));
            // Payload field
            aceinna_packet.insert(aceinna_packet.end(), payload, payload + payload_length);
        } else {
            aceinna_packet.push_back(0x00);
            aceinna_packet.push_back(0x00);
            aceinna_packet.push_back(0x00);
            aceinna_packet.push_back(0x00);
        }

        // CRC16 - LSB-first - From Message ID to Payload
        const uint16_t crc16 = Ethernet::CRC::CalculateINS401_CRC16(&aceinna_packet[2], aceinna_packet.size() - 2);
        aceinna_packet.push_back(static_cast<uint8_t>(crc16 & 0xFF));
        aceinna_packet.push_back(static_cast<uint8_t>((crc16 >> 8) & 0xFF));

        // ETH length field - LSB-first
        auto eth_payload_length = static_cast<uint16_t>(aceinna_packet.size());
        frame.push_back(static_cast<uint8_t>(eth_payload_length & 0xFF));
        frame.push_back(static_cast<uint8_t>((eth_payload_length >> 8) & 0xFF));

        frame.insert(frame.end(), aceinna_packet.begin(), aceinna_packet.end());

        // Padding to reach minimum Ethernet frame size (46 bytes payload)
        if (aceinna_packet.size() < kMinPayloadSize) {
            size_t padding_size = kMinPayloadSize - aceinna_packet.size();
            frame.insert(frame.end(), padding_size, 0x00);
        }
        return frame;
    }


    std::string ParseMacAddress(const std::array<uint8_t, 6> &mac) {
        return fmt::format("{:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x}", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }

    std::string ParseMacAddress(const uint8_t *mac_ptr) {
        if (!mac_ptr) {
            Common::Log::log_and_throw(kModule, "Null MAC address pointer");
            return "00:00:00:00:00:00";
        }
        return fmt::format("{:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x}", mac_ptr[0], mac_ptr[1], mac_ptr[2], mac_ptr[3],
                           mac_ptr[4],
                           mac_ptr[5]);
    }


    std::array<uint8_t, 6> FormatMACAddress(std::string mac_str) {
        if (mac_str.empty()) {
            Common::Log::log_and_throw(kModule, "Empty MAC address string");
        }
        std::string hex_only;
        hex_only.reserve(12);
        for (char c: mac_str) {
            if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) {
                hex_only += c;
            } else if (c != ':' && c != '-' && c != ' ') {
                Common::Log::log_and_throw(kModule,
                                 fmt::format("Invalid character '{}' in MAC address: '{}'", c, mac_str));
            }
        }
        if (hex_only.length() != 12) {
            Common::Log::log_and_throw(kModule,
                             fmt::format("Invalid MAC address length (expected 12 hex digits, got {}): '{}'",
                                         hex_only.length(), mac_str));
        }
        std::array<uint8_t, 6> result{};
        for (size_t i = 0; i < 6; ++i) {
            result[i] = static_cast<uint8_t>(std::stoul(hex_only.substr(i * 2, 2), nullptr, 16));
        }
        return result;
    }


    namespace CRC {
        uint16_t CalculateINS401_CRC16(const uint8_t *buf, const uint16_t &length) {
            uint16_t crc = 0x1D0F;
            for (int i = 0; i < length; i++) {
                crc ^= buf[i] << 8;
                for (int j = 0; j < 8; j++) {
                    if (crc & 0x8000) {
                        crc = (crc << 1) ^ 0x1021;
                    } else {
                        crc = crc << 1;
                    }
                }
            }
            // Byte-swap to match INS401 LSB-first wire format
            return ((crc << 8) & 0xFF00) | ((crc >> 8) & 0xFF);
        }


        uint32_t CalculateRTCM3_CRC24(const void *data, std::size_t nBytes) {
            // Bits = 24
            // TruncPoly = 0x1864CFB
            // InitRem = 0
            // FinalXor = 0
            // ReflectIn = false
            // ReflectRem = false
            using crc24_t = boost::crc_optimal<24, 0x1864CFB, 0, 0, false, false>;
            crc24_t crc;
            crc.process_bytes(data, nBytes);
            const uint32_t result = crc.checksum();
            return result;
        }
    } // namespace CRC
} // namespace Ethernet