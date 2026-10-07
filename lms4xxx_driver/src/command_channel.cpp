#include "command_channel.h"

#include "command_builder.h"
#include "error.h"
#include "logger.h"

namespace {
    common::DriverLog g_log{"LMS4xxx"};
}

namespace lms4xxx {
    void CommandChannel::DrainStaleBytes(TcpClient *tcp_client) const {
        constexpr std::size_t kMaxDrain = 64 * 1024;
        std::uint8_t scratch[1024];
        std::size_t dropped = 0;
        while (dropped < kMaxDrain) {
            std::error_code ec;
            const auto n = tcp_client->ReadSome(scratch, sizeof(scratch), ec);
            if (ec) {
                break;
            }
            if (n == 0) {
                // SO_RCVTIMEO expired with nothing pending: the socket is
                // quiet, which is exactly where the next command wants it.
                break;
            }
            dropped += n;
        }
        if (dropped > 0) {
            g_log.Warn("[{}] Dropped {} stale byte(s) to realign the command stream", Tag(), dropped);
        }
    }


    std::error_code CommandChannel::SendAndReceive(TcpClient *tcp_client, const std::vector<std::uint8_t> &frame,
                                                 ColaBMessage &response, int timeout_ms, bool quiet_sfa) const {
        const auto failed = [this](std::error_code ec) {
            uncertain_ = true;
            return ec;
        };
        if (!tcp_client || !tcp_client->IsConnected()) {
            return failed(make_error_code(ErrorCode::kNotConnected));
        }

        auto ec = tcp_client->Write(frame);
        if (ec) {
            return failed(ec);
        }

        // Response frame: STX(4) + Length(4) + Data(N) + Checksum(1)
        std::uint8_t header[8];
        std::error_code read_ec;
        auto bytes = tcp_client->Read(header, 8, read_ec, timeout_ms);
        if (read_ec) {
            // A partial read leaves the stream mid-frame; the next command
            // would take this answer's tail for its own header and every
            // command after it would fail. Realign before giving up.
            if (bytes > 0) {
                DrainStaleBytes(tcp_client);
            }
            return failed(read_ec);
        }
        if (bytes < 8) {
            return failed(make_error_code(ErrorCode::kFrameTooShort));
        }

        if (header[0] != 0x02 || header[1] != 0x02 || header[2] != 0x02 || header[3] != 0x02) {
            g_log.Warn("[{}] Invalid STX in response: {:02X} {:02X} {:02X} {:02X}", Tag(), header[0], header[1],
                       header[2], header[3]);
            DrainStaleBytes(tcp_client);
            return failed(make_error_code(ErrorCode::kProtocolError));
        }

        const auto data_len = ColaBCodec::DecodeUint32(header + 4);

        if (data_len > kMaxFrameBytes) {
            g_log.Warn("[{}] Response data length {} exceeds max", Tag(), data_len);
            DrainStaleBytes(tcp_client); // the body is still on the wire: realign like the other error paths
            return failed(make_error_code(ErrorCode::kFrameTooLong));
        }

        std::vector<std::uint8_t> data_and_cs(data_len + 1);
        bytes = tcp_client->Read(data_and_cs.data(), data_and_cs.size(), read_ec, timeout_ms);
        if (read_ec) {
            if (bytes > 0 && bytes < data_and_cs.size()) {
                DrainStaleBytes(tcp_client);
            }
            return failed(read_ec);
        }
        if (bytes < data_and_cs.size()) {
            return failed(make_error_code(ErrorCode::kFrameTooShort));
        }

        const auto computed_cs = ColaBCodec::ComputeChecksum(data_and_cs.data(), data_len);
        const auto received_cs = data_and_cs[data_len];
        if (computed_cs != received_cs) {
            g_log.Warn("[{}] CRC mismatch in response: computed 0x{:02X}, received 0x{:02X}", Tag(), computed_cs,
                       received_cs);
            return failed(make_error_code(ErrorCode::kCrcError));
        }

        const auto decode_ec = ColaBCodec::Decode(data_and_cs.data(), data_len, response, Tag());
        if (decode_ec) {
            return failed(decode_ec);
        }
        if (response.command_type == CommandType::kErrorAnswer) {
            const std::uint16_t code = response.payload.size() >= 2
                                           ? ColaBCodec::DecodeUint16(response.payload.data())
                                           : 0xFFFF;
            g_log.Log(quiet_sfa ? spdlog::level::trace : spdlog::level::warn,
                      "[{}] Device answered sFA {} ({})", Tag(), code, SopasErrorText(code));
            return make_error_code(code == 1 ? ErrorCode::kAccessDenied : ErrorCode::kSopasError);
        }
        return {};
    }


    std::error_code CommandChannel::ReadVariable(TcpClient *tcp_client, std::string_view name,
                                               std::vector<std::uint8_t> &payload, int timeout_ms, bool quiet_sfa) {
        ColaBMessage r;
        auto ec = SendAndReceive(tcp_client, CommandBuilder::BuildReadVariable(name), r, timeout_ms, quiet_sfa);
        if (ec) {
            return ec;
        }
        ec = ValidateResponse(r, CommandType::kReadAnswer, name);
        if (ec) {
            return ec;
        }
        payload = r.payload;
        return {};
    }


    std::error_code CommandChannel::CallMethod(TcpClient *tcp_client, const std::vector<std::uint8_t> &frame, std::string_view name,
                                             std::optional<std::uint8_t> ok_byte) {
        ColaBMessage r;
        auto ec = SendAndReceive(tcp_client, frame, r, MethodTimeout());
        if (ec) {
            g_log.Error("[{}] {} failed: {}", Tag(), name, ec.message());
            return ec;
        }
        ec = ValidateResponse(r, CommandType::kMethodAnswer, name);
        if (ec) {
            return ec;
        }
        if (ok_byte && (r.payload.empty() || r.payload[0] != *ok_byte)) {
            g_log.Error("[{}] {} rejected by the device (status {})", Tag(), name,
                        r.payload.empty() ? -1 : static_cast<int>(r.payload[0]));
            return make_error_code(ErrorCode::kCommandRejected);
        }
        return {};
    }


    std::error_code CommandChannel::ValidateResponse(const ColaBMessage &msg, std::string_view expected_type,
                                                   std::string_view expected_name) const {
        if (msg.command_type != expected_type) {
            uncertain_ = true;
            g_log.Warn("[{}] Unexpected response type '{}', expected '{}'", Tag(), msg.command_type, expected_type);
            return make_error_code(ErrorCode::kUnexpectedResponse);
        }
        if (msg.command_name != expected_name) {
            uncertain_ = true;
            g_log.Warn("[{}] Unexpected response name '{}', expected '{}'", Tag(), msg.command_name, expected_name);
            return make_error_code(ErrorCode::kUnexpectedResponse);
        }
        return {};
    }

} // namespace lms4xxx
