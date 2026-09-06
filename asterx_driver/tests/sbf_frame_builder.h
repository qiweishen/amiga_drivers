// Shared helper for the SBF tests: builds one complete frame in wire order
// (little endian). The CRC field is a placeholder — ssnrx validates the CRC
// before the driver's parsers ever see a frame, so they do not re-check it.
#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

namespace asterx_test {
    struct FrameBuilder {
        std::vector<std::uint8_t> b; // body only; the header is prepended by finalize()

        void u8(std::uint8_t v) { b.push_back(v); }
        void i8(std::int8_t v) { b.push_back(static_cast<std::uint8_t>(v)); }

        void U16(std::uint16_t v) {
            b.push_back(v & 0xFF);
            b.push_back(v >> 8);
        }

        void U32(std::uint32_t v) {
            U16(v & 0xFFFF);
            U16(v >> 16);
        }

        void F32(float v) {
            std::uint32_t bits;
            std::memcpy(&bits, &v, sizeof bits);
            U32(bits);
        }

        void F64(double v) {
            std::uint64_t bits;
            std::memcpy(&bits, &v, sizeof bits);
            U32(static_cast<std::uint32_t>(bits & 0xFFFFFFFFull));
            U32(static_cast<std::uint32_t>(bits >> 32));
        }

        std::vector<std::uint8_t> finalize(std::uint16_t id, std::uint8_t rev,
                                           std::uint32_t tow = 123456, std::uint16_t wnc = 2400) const {
            std::vector<std::uint8_t> f;
            std::size_t total = 14 + b.size();
            total = (total + 3) / 4 * 4; // SBF frames are padded to a multiple of 4
            f.push_back(0x24); // '$'
            f.push_back(0x40); // '@'
            f.push_back(0xEF); // crc placeholder
            f.push_back(0xBE);
            const auto raw = static_cast<std::uint16_t>(id | (rev << 13));
            f.push_back(raw & 0xFF);
            f.push_back(raw >> 8);
            f.push_back(total & 0xFF);
            f.push_back(static_cast<std::uint8_t>(total >> 8));
            f.push_back(tow & 0xFF);
            f.push_back((tow >> 8) & 0xFF);
            f.push_back((tow >> 16) & 0xFF);
            f.push_back((tow >> 24) & 0xFF);
            f.push_back(wnc & 0xFF);
            f.push_back(wnc >> 8);
            f.insert(f.end(), b.begin(), b.end());
            f.resize(total, 0); // padding
            return f;
        }
    };

    // INSNavGeod (4226) fixed part, up to sb_list exclusive
    inline void ins_fixed_body(FrameBuilder &fb, double lat = 0.55, std::uint16_t gnss_age = 5,
                               std::uint16_t accuracy = 8) {
        fb.u8(4); // gnss_mode
        fb.u8(0); // error
        fb.U16(0); // info
        fb.U16(gnss_age); // 0.01 s
        fb.F64(lat); // latitude rad
        fb.F64(2.12); // longitude rad
        fb.F64(16.5); // height
        fb.F32(9.1f); // undulation
        fb.U16(accuracy); // 0.01 m
        fb.U16(30); // latency, 0.0001 s
        fb.u8(0); // datum
        fb.u8(0); // reserved
    }

    // ReceiverStatus (4014) fixed part, up to temperature inclusive
    inline void rxs_fixed_body(FrameBuilder &fb, std::uint8_t n, std::uint8_t sb_length,
                               std::uint32_t up_time = 3600, std::uint32_t rx_status = 0,
                               std::uint8_t temperature = 142, std::uint8_t cpu_load = 35) {
        fb.u8(cpu_load);
        fb.u8(0); // ext_error
        fb.U32(up_time);
        fb.U32(rx_status);
        fb.U32(0); // rx_error
        fb.u8(n);
        fb.u8(sb_length);
        fb.u8(1); // cmd_count
        fb.u8(temperature); // wire = degC + 100
    }
} // namespace asterx_test
