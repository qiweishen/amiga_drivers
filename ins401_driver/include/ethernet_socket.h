/**
 * @file ethernet_socket.h
 * @brief High-performance Ethernet raw socket communication class.
 *
 * Provides a C++ wrapper for Linux AF_PACKET raw sockets with BPF filtering
 * and epoll-based event handling for efficient Ethernet frame communication.
 *
 * @author Qiwei
 * @date 2025
 *
 * @note Requires Linux kernel 2.6+ for AF_PACKET and epoll support.
 * @note Requires root privileges or CAP_NET_RAW capability.
 */

#pragma once

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


/**
 * @defgroup EthernetConstants Ethernet Protocol Constants
 * @brief Constants defining Ethernet frame structure and limits.
 * @{
 */

/// @brief Size of MAC address in bytes.
inline constexpr std::size_t kMacAddressSize = 6;

/// @brief Size of all Ethernet addresses in bytes (destination MAC + source MAC).
inline constexpr std::size_t kAllMacAddressesSize = 12;

/// @brief Size of Ethernet header in bytes (destination MAC + source MAC + EtherType).
inline constexpr std::size_t kEthernetHeaderSize = 14;

/// @brief Maximum Ethernet frame size in bytes (excluding FCS).
inline constexpr std::size_t kMaxFrameSize = 1518;

/// @brief Minimum Ethernet payload size in bytes (IEEE 802.3 requirement).
inline constexpr std::size_t kMinPayloadSize = 46;

/** @} */ // end of EthernetConstants group


/**
 * @defgroup EthernetTypes Ethernet Type Definitions
 * @brief Type aliases for Ethernet communication.
 * @{
 */

/**
 * @brief MAC address type (6 bytes).
 *
 * Stores a MAC address as a fixed-size array of 6 bytes in network byte order.
 */
using MacAddress = std::array<std::uint8_t, kMacAddressSize>;

/**
 * @brief Ethernet frame type.
 *
 * Represents a complete Ethernet frame as a dynamically-sized byte vector.
 */
using EthernetFrame = std::vector<std::uint8_t>;

/** @} */ // end of EthernetTypes group


/**
 * @class EthernetSocket
 * @brief Raw Ethernet socket for bidirectional communication with a target MAC address.
 *
 * This class provides high-performance Ethernet frame transmission and reception
 * using Linux AF_PACKET sockets. Key features:
 * - BPF (Berkeley Packet Filter) for kernel-level MAC address filtering
 * - epoll for efficient I/O event notification
 * - Non-blocking socket operations
 * - Configurable receive buffer size
 *
 * @note Requires root privileges or CAP_NET_RAW capability.
 * @note This class is non-copyable but movable.
 *
 * @par Thread Safety
 * This class is NOT thread-safe. External synchronization is required if
 * multiple threads access the same instance.
 *
 * @par Example Usage
 * @code
 * // Create socket for communication with target device
 * EthernetSocket sock("eth0", Ethernet::FormatMACAddress("00:11:22:33:44:55"));
 *
 * // Send a raw frame
 * std::vector<uint8_t> frame = {...};
 * sock.Send(frame);
 *
 * // Receive a frame with 100ms timeout
 * if (auto frame = sock.Receive(100)) {
 *     // Process frame->data()
 *     spdlog::info("Received {} bytes", frame->size());
 * }
 *
 * // Batch receive for high-throughput scenarios
 * auto frames = sock.ReceiveBatch(32);
 * for (const auto& f : frames) {
 *     // Process each frame
 * }
 * @endcode
 *
 * @see Ethernet::BuildAceinnaPacket() for constructing protocol-specific frames
 * @see Ethernet::GetNetworkInterfaces() for discovering available interfaces
 */
class EthernetSocket {
public:
    /**
     * @brief Constructs an EthernetSocket bound to a specific interface and target MAC.
     *
     * Creates a raw socket, optionally sets up BPF filtering for bidirectional
     * MAC matching, and initializes epoll for event-driven reception.
     *
     * @param[in] interface_name   Network interface name (e.g., "eth0", "enp0s3").
     * @param[in] target_mac       Target device MAC address for communication.
     * @param[in] recv_buffer_size Socket receive buffer size in bytes.
     *                             Use 0 for system default (typically 208KB).
     * @param[in] enable_bpf       Enable BPF filtering for MAC address matching.
     *                             When true, only frames from/to target_mac are received.
     *
     * @throws std::runtime_error If error occurs during socket creation or configuration.
     *
     * @pre @p interface_name must be a valid, existing network interface.
     * @pre Calling process must have CAP_NET_RAW capability or root privileges.
     *
     * @post Socket is bound to the specified interface and ready for I/O.
     * @post Local MAC address is retrieved and stored internally.
     */
    EthernetSocket(std::string_view interface_name, const MacAddress &target_mac, std::size_t recv_buffer_size = 0,
                   bool enable_bpf = false);

    /**
     * @brief Destructor. Closes socket and epoll file descriptors.
     *
     * Releases all system resources associated with this socket.
     * Safe to call even if construction failed partially.
     */
    ~EthernetSocket();

    /// @name Deleted Copy Operations
    /// @brief EthernetSocket is non-copyable to prevent resource duplication.
    /// @{
    EthernetSocket(const EthernetSocket &) = delete;

    EthernetSocket &operator=(const EthernetSocket &) = delete;

    /// @}

    /// @name Move Operations
    /// @brief EthernetSocket supports move semantics for ownership transfer.
    /// @{
    EthernetSocket(EthernetSocket &&other) noexcept = default;

    EthernetSocket &operator=(EthernetSocket &&other) noexcept = default;

    /// @}

    /**
     * @brief Sends a pre-built raw Ethernet frame.
     *
     * Transmits a complete Ethernet frame including the header. Use this method
     * when you have already constructed the complete frame with destination MAC,
     * source MAC, EtherType, and payload.
     *
     * @param[in] frame Complete Ethernet frame data (minimum 14 bytes for header).
     *
     * @return Number of bytes sent on success.
     * @retval -1 On failure (check errno: EAGAIN for buffer full, ENETDOWN for
     *            interface down, etc.).
     *
     * @note This is a non-blocking operation. If the send buffer is full,
     *       returns -1 with errno set to EAGAIN.
     * @note Frame will be padded by the kernel if smaller than minimum Ethernet size.
     *
     * @see Ethernet::BuildAceinnaPacket() for constructing Aceinna protocol frames.
     */
    std::ptrdiff_t Send(const std::vector<uint8_t> &frame) const;

    /**
     * @brief Receives an Ethernet frame with timeout.
     *
     * Waits for an incoming frame using epoll. When BPF filtering is enabled,
     * only frames matching the configured MAC addresses will be received.
     *
     * @param[in] timeout_ms Timeout in milliseconds.
     *                       - -1: Block indefinitely until frame arrives
     *                       -  0: Return immediately (non-blocking poll)
     *                       - >0: Wait up to specified milliseconds
     *
     * @return Received frame including Ethernet header, or std::nullopt on
     *         timeout or error.
     *
     * @note The returned frame includes the full Ethernet header (14 bytes).
     *       Use pointer arithmetic or Ethernet constants to access the payload.
     *
     * @par Example
     * @code
     * if (auto frame = sock.Receive(1000)) {
     *     // Skip Ethernet header to get payload
     *     const uint8_t* payload = frame->data() + kEthernetHeaderSize;
     *     size_t payload_len = frame->size() - kEthernetHeaderSize;
     * }
     * @endcode
     */
    std::optional<EthernetFrame> Receive(int timeout_ms = -1) const;

    /**
     * @brief Receives multiple frames in non-blocking mode.
     *
     * Reads all immediately available frames from the socket buffer without
     * waiting. This is more efficient than multiple Receive(0) calls when
     * processing high-throughput data streams.
     *
     * @param[in] max_frames Maximum number of frames to receive per call.
     *                       Limits memory usage and processing latency.
     *
     * @return Vector of received frames. May be empty if no frames are available.
     *         Each frame includes the complete Ethernet header.
     *
     * @note This method never blocks. Returns immediately with available data.
     * @note Useful for draining the receive buffer in high-frequency scenarios.
     *
     * @par Performance Consideration
     * For high-throughput applications, use larger @p max_frames values to
     * reduce system call overhead, but balance against memory usage and
     * processing latency requirements.
     */
    std::vector<EthernetFrame> ReceiveBatch(std::size_t max_frames = 64) const;

    /**
     * @name Accessor Methods
     * @brief Methods to query socket state and configuration.
     * @{
     */

    /**
     * @brief Gets the local interface MAC address.
     * @return Copy of the local MAC address.
     */
    MacAddress GetLocalMac() const { return local_mac_; }

    /**
     * @brief Gets the target device MAC address.
     * @return Copy of the target MAC address.
     */
    MacAddress GetTargetMac() const { return target_mac_; }

    /**
     * @brief Gets the network interface name.
     * @return Copy of the interface name string.
     */
    std::string GetInterface() const { return interface_name_; }

    /**
     * @brief Gets the raw socket file descriptor.
     * @return Socket file descriptor, or -1 if socket is invalid/closed.
     * @warning Direct use of the file descriptor bypasses this class's
     *          internal state management. Use with caution.
     */
    int Getfd() const { return socket_fd_; }

    /**
     * @brief Checks if the socket is valid and operational.
     * @return true if socket is open and ready for I/O, false otherwise.
     */
    bool IsValid() const { return socket_fd_ >= 0; }

    /** @} */ // end of Accessor Methods group

private:
    /**
     * @brief Creates and configures the raw AF_PACKET socket.
     *
     * Performs the following setup:
     * 1. Creates SOCK_RAW socket with ETH_P_ALL protocol
     * 2. Retrieves interface index and local MAC address
     * 3. Binds socket to the specified interface
     * 4. Sets socket to non-blocking mode
     * 5. Optionally configures receive buffer size
     * 6. Optionally attaches BPF filter
     *
     * @throws std::runtime_error On any setup failure.
     */
    void CreateSocket();

    /**
     * @brief Initializes epoll for socket event monitoring.
     *
     * Creates an epoll instance and registers the socket for EPOLLIN events.
     *
     * @throws std::runtime_error If epoll_create1() or epoll_ctl() fails.
     */
    void SetupEpoll();

    /**
     * @brief Closes all file descriptors and releases resources.
     *
     * Safely closes both socket and epoll file descriptors if they are valid.
     * Sets internal file descriptors to -1 after closing.
     */
    void CloseEthernetSocket();

    /// @name Member Variables
    /// @{
    std::string interface_name_; ///< Network interface name (e.g., "eth0").
    MacAddress target_mac_; ///< Target device MAC address for filtering.
    MacAddress local_mac_{}; ///< Local interface MAC address (auto-detected).
    int if_index_ = 0; ///< Network interface index from if_nametoindex().
    std::size_t recv_buffer_size_ = 0; ///< Configured receive buffer size (0 = default).
    bool enable_bpf_ = false; ///< Flag indicating if BPF filtering is enabled.

    int socket_fd_{-1}; ///< Raw socket file descriptor (-1 if invalid).
    int epoll_fd_{-1}; ///< epoll instance file descriptor (-1 if invalid).
    /// @}
};


/**
 * @namespace Ethernet
 * @brief Utility functions for Ethernet communication and frame construction.
 *
 * Provides helper functions for:
 * - Network interface enumeration
 * - BPF filter setup
 * - Aceinna protocol frame construction
 * - MAC address parsing and formatting
 * - CRC calculation
 */
namespace Ethernet {
    /**
     * @class FdGuard
     * @brief RAII wrapper for file descriptor lifetime management.
     *
     * Ensures file descriptors are properly closed when the guard goes out of scope.
     * Supports move semantics for ownership transfer but prevents copying.
     *
     * @par Example Usage
     * @code
     * {
     *     FdGuard guard(open("/dev/null", O_RDONLY));
     *     if (!guard.IsValid()) {
     *         // Handle error
     *     }
     *     // Use guard.Get() to access the fd
     * } // fd automatically closed here
     *
     * // Ownership transfer
     * FdGuard guard1(socket(AF_PACKET, SOCK_RAW, 0));
     * FdGuard guard2 = std::move(guard1);  // guard1 no longer owns the fd
     * @endcode
     */
    class FdGuard {
    public:
        /**
         * @brief Constructs a guard owning the specified file descriptor.
         * @param[in] fd File descriptor to manage, or -1 for empty guard.
         */
        explicit FdGuard(int fd = -1) noexcept : fd_(fd) {
        }

        /**
         * @brief Destructor. Closes the owned file descriptor if valid.
         */
        ~FdGuard() {
            if (fd_ >= 0) {
                close(fd_);
            }
        }

        /// @name Deleted Copy Operations
        /// @brief FdGuard is non-copyable to ensure unique ownership.
        /// @{
        FdGuard(const FdGuard &) = delete;

        FdGuard &operator=(const FdGuard &) = delete;

        /// @}

        /**
         * @brief Move constructor. Transfers ownership from another guard.
         * @param[in,out] other Source guard (will be invalidated).
         */
        FdGuard(FdGuard &&other) noexcept : fd_(other.fd_) { other.fd_ = -1; }

        /**
         * @brief Move assignment operator. Transfers ownership from another guard.
         *
         * Closes any currently owned file descriptor before taking ownership.
         *
         * @param[in,out] other Source guard (will be invalidated).
         * @return Reference to this guard.
         */
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

        /**
         * @brief Gets the managed file descriptor without releasing ownership.
         * @return The file descriptor, or -1 if empty.
         */
        int Get() const noexcept { return fd_; }

        /**
         * @brief Releases ownership and returns the file descriptor.
         *
         * After this call, the guard no longer owns the file descriptor and
         * will not close it on destruction.
         *
         * @return The file descriptor (caller assumes ownership), or -1 if empty.
         */
        int Release() noexcept {
            int tmp = fd_;
            fd_ = -1;
            return tmp;
        }

        /**
         * @brief Replaces the managed file descriptor.
         *
         * Closes any currently owned file descriptor before taking ownership
         * of the new one.
         *
         * @param[in] fd New file descriptor to manage, or -1 for empty.
         */
        void Reset(int fd = -1) noexcept {
            if (fd_ >= 0) {
                close(fd_);
            }
            fd_ = fd;
        }

        /**
         * @brief Checks if the guard owns a valid file descriptor.
         * @return true if fd >= 0, false otherwise.
         */
        bool IsValid() const noexcept { return fd_ >= 0; }

    private:
        int fd_; ///< Managed file descriptor (-1 if empty).
    };

    /**
     * @brief Retrieves the list of active network interfaces and their MAC addresses.
     *
     * Queries the host operating system for all currently active Ethernet interfaces
     * using getifaddrs(). Filters out loopback and non-Ethernet interfaces.
     *
     * @return Vector of pairs containing:
     *         - first: Interface name (e.g., "eth0", "enp0s3")
     *         - second: MAC address string in format "aa:bb:cc:dd:ee:ff"
     *
     * @note Only returns interfaces that are UP and have a valid MAC address.
     * @note Virtual interfaces (bridges, VLANs) are included if they meet criteria.
     *
     * @par Example
     * @code
     * for (const auto& [name, mac] : Ethernet::GetNetworkInterfaces()) {
     *     spdlog::info("Interface: {} MAC: {}", name, mac);
     * }
     * @endcode
     */
    std::vector<std::pair<std::string, std::string> > GetNetworkInterfaces();

    /**
     * @brief Attaches a BPF filter for bidirectional MAC address filtering.
     *
     * Installs a Berkeley Packet Filter that accepts only frames where:
     * - Source MAC matches @p target_mac AND destination MAC matches @p local_mac, OR
     * - Source MAC matches @p local_mac AND destination MAC matches @p target_mac
     *
     * This effectively filters to only bidirectional traffic between the local
     * interface and the specified target device.
     *
     * @param[in] target_mac Target device MAC address to filter.
     * @param[in] local_mac  Local interface MAC address.
     * @param[in] socket_fd  Socket file descriptor to attach filter to.
     *
     * @return true if filter was attached successfully, false on failure.
     *
     * @note BPF filtering occurs in kernel space, significantly reducing
     *       userspace processing overhead for high-traffic scenarios.
     *
     * @see EthernetSocket constructor's @p enable_bpf parameter.
     */
    bool SetupBpfFilter(MacAddress target_mac, MacAddress local_mac, int socket_fd);

    /**
     * @brief Builds an Ethernet frame containing an Aceinna-formatted payload.
     *
     * Constructs a complete Ethernet frame ready for transmission via raw socket.
     * The frame follows the Aceinna protocol specification with proper header,
     * length fields, payload, and CRC.
     *
     * @par Frame Structure
     * @verbatim
     * +-------------------------------------+
     * | Ethernet Header (14 bytes)          |
     * +-------------------------------------+
     * | Destination MAC     | 6 bytes       |
     * | Source MAC          | 6 bytes       |
     * | Length/Type         | 2 bytes       | (little-endian)
     * +-------------------------------------+
     * | Aceinna Payload                     |
     * +-------------------------------------+
     * | Preamble            | 2 bytes       | (0x5555)
     * | Message ID          | 2 bytes       | (little-endian)
     * | Payload Length      | 4 bytes       | (little-endian)
     * | Payload Data        | N bytes       | (variable)
     * | CRC16               | 2 bytes       | (little-endian)
     * +-------------------------------------+
     * | Padding             | M bytes       | (if needed for min size)
     * +-------------------------------------+
     * | Frame CRC (FCS)     | 4 bytes       | (normally added by switch, not included here)
     * +-------------------------------------+
     * @endverbatim
     *
     * @param[in] target_mac     Destination device MAC address.
     * @param[in] local_mac      Source (local) MAC address.
     * @param[in] message_id     Two-byte Aceinna message identifier.
     * @param[in] payload        Pointer to payload data buffer (may be nullptr if
     *                           @p payload_length is 0).
     * @param[in] payload_length Length of payload data in bytes.
     *
     * @return Complete Ethernet frame as a byte vector, ready for Send().
     *
     * @note Frame is automatically padded to meet minimum Ethernet size (60 bytes).
     * @note CRC16 is calculated over message_id + payload_length + payload.
     *
     * @see CRC::CalculateINS401_CRC16() for CRC algorithm details.
     */
    std::vector<uint8_t> BuildAceinnaPacket(MacAddress target_mac, MacAddress local_mac,
                                            std::array<uint8_t, 2> message_id,
                                            const uint8_t *payload, size_t payload_length);

    /**
     * @name MAC Address Conversion Functions
     * @brief Functions for converting between MAC address representations.
     * @{
     */

    /**
     * @brief Converts a MAC address array to a formatted string.
     *
     * @param[in] mac_uint8 MAC address as a 6-byte array.
     *
     * @return MAC address string in format "xx:xx:xx:xx:xx:xx" (lowercase hex).
     *
     * @par Example
     * @code
     * MacAddress mac = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
     * std::string str = Ethernet::ParseMacAddress(mac);
     * // str == "aa:bb:cc:dd:ee:ff"
     * @endcode
     */
    std::string ParseMacAddress(const std::array<uint8_t, 6> &mac_uint8);

    /**
     * @brief Converts a MAC address from raw bytes to a formatted string.
     *
     * @param[in] mac_ptr Pointer to a 6-byte buffer containing the MAC address.
     *                    Must not be nullptr.
     *
     * @return MAC address string in format "xx:xx:xx:xx:xx:xx" (lowercase hex).
     *
     * @pre @p mac_ptr must point to at least 6 valid bytes.
     *
     * @warning No bounds checking is performed. Caller must ensure buffer validity.
     */
    std::string ParseMacAddress(const uint8_t *mac_ptr);

    /**
     * @brief Parses a MAC address string into a byte array.
     *
     * Accepts MAC addresses in standard colon-separated format.
     *
     * @param[in] mac_str MAC address string in format "xx:xx:xx:xx:xx:xx".
     *                    Both uppercase and lowercase hex digits are accepted.
     *
     * @return MAC address as a 6-byte array.
     *
     * @throws std::invalid_argument If format is invalid (wrong length, invalid
     *                               characters, missing colons).
     *
     * @par Example
     * @code
     * auto mac = Ethernet::FormatMACAddress("00:11:22:33:44:55");
     * // mac == {0x00, 0x11, 0x22, 0x33, 0x44, 0x55}
     * @endcode
     */
    std::array<uint8_t, 6> FormatMACAddress(std::string_view mac_str);

    /** @} */ // end of MAC Address Conversion Functions


    /**
     * @namespace CRC
     * @brief CRC calculation functions for protocol checksums.
     *
     * Provides CRC algorithms used by INS401 and RTCM3 protocols for
     * data integrity verification.
     */
    namespace CRC {
        /**
         * @brief Computes the INS401-specific CRC16 checksum.
         *
         * Implements CRC-16/CCITT-FALSE algorithm used by the Aceinna INS401 protocol
         * for packet integrity verification.
         *
         * @par Algorithm Parameters
         * - Polynomial: 0x1021
         * - Initial value: 0xFFFF
         * - Input reflected: No
         * - Output reflected: No
         * - Final XOR: 0x0000
         *
         * @param[in] buf    Pointer to the input data buffer.
         * @param[in] length Number of bytes to process.
         *
         * @return 16-bit CRC value.
         *
         * @pre @p buf must point to at least @p length valid bytes.
         *
         * @note Result should be appended to packet in big-endian byte order.
         */
        uint16_t CalculateINS401_CRC16(const uint8_t *buf, const uint16_t &length);

        /**
         * @brief Computes the RTCM3 CRC24Q checksum.
         *
         * Implements the CRC-24Q algorithm as specified by RTCM Standard 10403.x
         * for RTCM version 3 message integrity.
         *
         * @par Algorithm Parameters
         * - Polynomial: 0x1864CFB (CRC-24Q)
         * - Initial value: 0x000000
         * - Used for RTCM 3.x message validation
         *
         * @param[in] data   Pointer to the RTCM message buffer.
         * @param[in] nBytes Length of the buffer in bytes.
         *
         * @return 24-bit CRC value (in lower 24 bits of uint32_t).
         *
         * @pre @p data must point to at least @p nBytes valid bytes.
         *
         * @see RTCM Standard 10403.3 for protocol specification.
         */
        uint32_t CalculateRTCM3_CRC24(const void *data, std::size_t nBytes);
    } // namespace CRC
} // namespace Ethernet
