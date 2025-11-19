#pragma once

#include <boost/asio.hpp>
#include <pcap/pcap.h>
#include <functional>
#include <memory>
#include <mutex>



/**
 * AsyncRawSocket - Pure communication layer for custom Ethernet frames
 * Handles socket setup, raw data reception and transmission only
 * Data parsing should be done by external classes
 */
class AsyncRawSocket {
public:
    using receive_handler = std::function<void(boost::system::error_code, const uint8_t*, size_t)>;
    using send_handler = std::function<void(boost::system::error_code, size_t)>;

	explicit AsyncRawSocket(boost::asio::io_context& io_context);
    ~AsyncRawSocket();

    /**
     * Open raw socket on specified network interface
     * @param interface Network interface name (e.g., "eth0")
     * @param filter_expr BPF filter expression
     * @return Error code if failed
     */
    boost::system::error_code Open(const std::string& interface, const std::string& filter_expr = "");

    /**
     * Start async receive operation
     * Callback will be called with raw Ethernet frame data
     * Automatically continues receiving after each callback
     */
    void AsyncReceive(const receive_handler& handler);

    /**
     * Send raw Ethernet frame
     * @param data Pointer to complete Ethernet frame
     * @param length Frame length in bytes
     * @return Error code if failed
     */
    boost::system::error_code Send(const uint8_t* data, size_t length);

    /**
     * Async send operation
     * @param data Pointer to complete Ethernet frame
     * @param length Frame length in bytes
     * @param handler Completion handler
     */
    void AsyncSend(const uint8_t* data, size_t length, const send_handler& handler);

    /**
     * Set BPF filter
     * @param filter_expr BPF filter expression
     * Example: "ether proto 0x88B5" for custom EtherType
     * Example: "ether src 00:11:22:33:44:55" for specific MAC
     */
    boost::system::error_code SetFilter(const std::string& filter_expr);

    /**
     * Get PCAP statistics
     */
	struct Stats {
		unsigned int received;     // Packets received
		unsigned int dropped;      // Packets dropped by kernel
		unsigned int if_dropped;   // Packets dropped by interface
	};
    [[nodiscard]] Stats GetStats() const;

    /**
     * Close the socket
     */
    void Close();

    [[nodiscard]] bool is_open() const;


private:
    // Core components
    boost::asio::io_context& io_context_;
    pcap_t* pcap_handle_ = nullptr;
    std::unique_ptr<boost::asio::posix::stream_descriptor> async_fd_;

    // Configuration
    std::string interface_;
    int pcap_fd_ = -1;

    // Thread safety for send operations
    std::mutex send_mutex_;

    // Buffer size for high-frequency data
    static constexpr int KERNEL_BUFFER_SIZE_ = 16 * 1024 * 1024;  // 16MB
    static constexpr int BATCH_PROCESS_COUNT_ = 32;  // Process up to 32 packets per callback

    boost::system::error_code LogMsg(const std::string& msg);
};
