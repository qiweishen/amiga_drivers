#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "cola_b.h"
#include "tcp_client.h"

namespace lms4xxx {
    // Synchronous CoLa command transactions only. Never reads the transport
    // while the receive worker owns it. The lifecycle owner supplies the current
    // connection, and resets uncertainty only after establishing a new one.
    class CommandChannel {
    public:
        static constexpr std::uint32_t kMaxFrameBytes = 64 * 1024;
        CommandChannel(int response_timeout_ms, std::string tag)
            : response_timeout_ms_(response_timeout_ms), tag_(std::move(tag)) {}
        [[nodiscard]] int MethodTimeout() const { return std::max(response_timeout_ms_, kMethodTimeoutMs); }
        [[nodiscard]] bool Uncertain() const { return uncertain_; }
        void Reset() { uncertain_ = false; }

        std::error_code SendAndReceive(TcpClient *tcp_client, const std::vector<std::uint8_t> &frame,
                                      ColaBMessage &response, int timeout_ms, bool quiet_sfa = false) const;
        std::error_code ReadVariable(TcpClient *tcp_client, std::string_view name,
                                    std::vector<std::uint8_t> &payload, int timeout_ms, bool quiet_sfa = false);
        std::error_code CallMethod(TcpClient *tcp_client, const std::vector<std::uint8_t> &frame,
                                  std::string_view name, std::optional<std::uint8_t> ok_byte);
        std::error_code ValidateResponse(const ColaBMessage &msg, std::string_view expected_type,
                                        std::string_view expected_name) const;

    private:
        // Methods (login, Run, LMCstartmeas) can take longer than variable access.
        static constexpr int kMethodTimeoutMs = 5000;
        void DrainStaleBytes(TcpClient *tcp_client) const;
        [[nodiscard]] const std::string &Tag() const { return tag_; }
        int response_timeout_ms_;
        std::string tag_;
        mutable bool uncertain_ = false;
    };
} // namespace lms4xxx
