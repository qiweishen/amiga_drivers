/// @file ethernet_socket.h
/// @brief Raw Ethernet socket abstraction and Aceinna packet-level utilities for INS401 communication.

#ifndef ETHERNET_SOCKET_H
#define ETHERNET_SOCKET_H

#include <array>
#include <cstdio>
#include <cstring>
#include <optional>
#include <spdlog/spdlog.h>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>


// Ethernet frame sizing constants.
inline constexpr std::size_t kMacAddressSize = 6;

inline constexpr std::size_t kAllMacAddressesSize = 12;

inline constexpr std::size_t kEthernetHeaderSize = 14;

inline constexpr std::size_t kMaxFrameSize = 1518;

inline constexpr std::size_t kMinPayloadSize = 46;

// Ethernet types.
using MacAddress = std::array<std::uint8_t, kMacAddressSize>;

using EthernetFrame = std::vector<std::uint8_t>;


// Raw Ethernet socket for bidirectional communication.
class EthernetSocket {
public:
    EthernetSocket(std::string interface_name, const MacAddress &target_mac, std::size_t recv_buffer_size = 0,
                   bool enable_bpf = false);

    ~EthernetSocket();

    EthernetSocket(const EthernetSocket &) = delete;

    EthernetSocket &operator=(const EthernetSocket &) = delete;

    EthernetSocket(EthernetSocket &&other) noexcept
        : interface_name_(std::move(other.interface_name_)),
          target_mac_(other.target_mac_), local_mac_(other.local_mac_),
          if_index_(other.if_index_), recv_buffer_size_(other.recv_buffer_size_),
          enable_bpf_(other.enable_bpf_),
          socket_fd_(std::exchange(other.socket_fd_, -1)),
          epoll_fd_(std::exchange(other.epoll_fd_, -1)) {
    }

    EthernetSocket &operator=(EthernetSocket &&other) noexcept {
        if (this != &other) {
            CloseEthernetSocket();
            interface_name_ = std::move(other.interface_name_);
            target_mac_ = other.target_mac_;
            local_mac_ = other.local_mac_;
            if_index_ = other.if_index_;
            recv_buffer_size_ = other.recv_buffer_size_;
            enable_bpf_ = other.enable_bpf_;
            socket_fd_ = std::exchange(other.socket_fd_, -1);
            epoll_fd_ = std::exchange(other.epoll_fd_, -1);
        }
        return *this;
    }

    [[nodiscard]] std::ptrdiff_t Send(const std::vector<uint8_t> &frame) const;

    [[nodiscard]] std::optional<EthernetFrame> Receive(int timeout_ms = -1) const;

    [[nodiscard]] std::vector<EthernetFrame> ReceiveBatch(std::size_t max_frames = 64) const;

    [[nodiscard]] MacAddress GetLocalMac() const { return local_mac_; }

    [[nodiscard]] MacAddress GetTargetMac() const { return target_mac_; }

    [[nodiscard]] std::string GetInterface() const { return interface_name_; }

    [[nodiscard]] int GetFd() const { return socket_fd_; }

    [[nodiscard]] bool IsValid() const { return socket_fd_ >= 0; }

private:
    void CreateSocket();

    void SetupEpoll();

    void CloseEthernetSocket();

    std::string interface_name_;
    MacAddress target_mac_;
    MacAddress local_mac_{};
    int if_index_ = 0;
    std::size_t recv_buffer_size_ = 0;
    bool enable_bpf_ = false;

    int socket_fd_{-1};
    int epoll_fd_{-1};
};

// Ethernet utility helpers.
namespace Ethernet {
    // RAII wrapper for file descriptors.
    class FdGuard {
    public:
        explicit FdGuard(int fd = -1) noexcept : fd_(fd) {
        }

        ~FdGuard() {
            if (fd_ >= 0) {
                close(fd_);
            }
        }

        FdGuard(const FdGuard &) = delete;

        FdGuard &operator=(const FdGuard &) = delete;

        FdGuard(FdGuard &&other) noexcept : fd_(other.fd_) { other.fd_ = -1; }

        FdGuard &operator=(FdGuard &&other) noexcept {
            if (this != &other) {
                if (fd_ >= 0) {
                    close(fd_);
                }
                fd_ = other.fd_;
                other.fd_ = -1;
            }
            return *this;
        }

        int Get() const noexcept { return fd_; }

        int Release() noexcept {
            int tmp = fd_;
            fd_ = -1;
            return tmp;
        }

        void Reset(int fd = -1) noexcept {
            if (fd_ >= 0) {
                close(fd_);
            }
            fd_ = fd;
        }

        bool IsValid() const noexcept { return fd_ >= 0; }

    private:
        int fd_;
    };

    [[nodiscard]] std::vector<std::pair<std::string, std::string> > GetNetworkInterfaces();

    bool SetupBpfFilter(MacAddress target_mac, MacAddress local_mac, int socket_fd);

    /// Build a complete Aceinna Ethernet frame with the following wire layout:
    /// [DstMAC(6) | SrcMAC(6) | EthLen(2) | 0x5555(2) | MsgID(2) | PayloadLen(4) | Payload(N) | CRC16(2) | Pad]
    [[nodiscard]] std::vector<uint8_t> BuildAceinnaPacket(MacAddress target_mac, MacAddress local_mac,
                                                          std::array<uint8_t, 2> message_id,
                                                          const uint8_t *payload, size_t payload_length);

    [[nodiscard]] std::string ParseMacAddress(const std::array<uint8_t, 6> &mac_uint8);

    [[nodiscard]] std::string ParseMacAddress(const uint8_t *mac_ptr);

    [[nodiscard]] std::array<uint8_t, 6> FormatMACAddress(std::string mac_str);

    namespace CRC {
        /// CRC-16/CCITT (poly 0x1021, init 0x1D0F). Returns byte-swapped per INS401 LSB-first wire format.
        [[nodiscard]] uint16_t CalculateINS401_CRC16(const uint8_t *buf, const uint16_t &length);

        /// CRC-24Q as defined by the RTCM3 standard (poly 0x1864CFB).
        [[nodiscard]] uint32_t CalculateRTCM3_CRC24(const void *data, std::size_t nBytes);
    } // namespace CRC
} // namespace Ethernet


#endif