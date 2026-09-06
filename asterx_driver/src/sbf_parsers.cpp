#include "sbf_parsers.h"

#include <cstring>
#include <limits>

#include "byte_util.h"


namespace asterx::sbf {
    namespace {
        struct Reader {
            const std::uint8_t *p;
            std::size_t size;
            std::size_t pos{0};
            bool ok{true};

            bool Need(std::size_t k) {
                if (ok && size - pos < k) {
                    ok = false;
                }
                return ok;
            }

            std::uint8_t u8() {
                if (!Need(1)) return 0;
                return p[pos++];
            }

            std::int8_t i8() { return static_cast<std::int8_t>(u8()); }

            std::uint16_t U16() {
                if (!Need(2)) return 0;
                const auto v = common::ByteUtil::LoadLittleU16(p + pos);
                pos += 2;
                return v;
            }

            std::uint32_t U32() {
                if (!Need(4)) return 0;
                const auto v = common::ByteUtil::LoadLittleU32(p + pos);
                pos += 4;
                return v;
            }

            std::uint64_t U64() {
                if (!Need(8)) return 0;
                const auto v = common::ByteUtil::LoadLittleU64(p + pos);
                pos += 8;
                return v;
            }

            float F32() {
                static_assert(std::numeric_limits<float>::is_iec559);
                const std::uint32_t bits = U32();
                float v;
                std::memcpy(&v, &bits, sizeof v);
                return DnuToNan(v);
            }

            double F64() {
                static_assert(std::numeric_limits<double>::is_iec559);
                const std::uint64_t bits = U64();
                double v;
                std::memcpy(&v, &bits, sizeof v);
                return DnuToNan(v);
            }

            bool Skip(std::size_t k) {
                if (!Need(k)) return false;
                pos += k;
                return true;
            }
        };

        bool ReadHeader(Reader &r, BlockHeader &h) {
            h.sync_1 = r.u8();
            if (h.sync_1 != 0x24) return false; // '$'
            h.sync_2 = r.u8();
            if (h.sync_2 != 0x40) return false; // '@'
            h.crc = r.U16();
            const std::uint16_t raw = r.U16();
            h.id = raw & 8191; // lower 13 bits
            h.revision = raw >> 13; // upper 3 bits
            h.length = r.U16();
            h.tow = r.U32();
            h.wnc = r.U16();
            return r.ok;
        }

        // Header + declared length: a short frame must not read its padding as an optional group
        bool ReadFrameHeader(Reader &r, BlockHeader &h) {
            if (!ReadHeader(r, h) || h.length < 14 || h.length > r.size) {
                return false;
            }
            r.size = h.length;
            return true;
        }
    } // namespace


    std::uint16_t PeekId(const std::uint8_t *data, std::size_t size) {
        if (size < 6) {
            return 0;
        }
        return common::ByteUtil::LoadLittleU16(data + 4) & 8191;
    }


    bool ParseBlockHeader(const std::uint8_t *data, std::size_t size, BlockHeader &out) {
        Reader r{data, size};
        return ReadHeader(r, out);
    }


    bool ParseINSNavGeod(const std::uint8_t *data, std::size_t size, INSNavGeod &out) {
        Reader r{data, size};
        if (!ReadFrameHeader(r, out.block_header) ||
            (out.block_header.id != kIdINSNavGeod && out.block_header.id != kIdExtEventINSNavGeod)) {
            return false;
        }
        out.gnss_mode = r.u8();
        out.error = r.u8();
        out.info = r.U16();
        out.gnss_age = r.U16();
        out.latitude = r.F64();
        out.longitude = r.F64();
        out.height = r.F64();
        out.undulation = r.F32();
        out.accuracy = r.U16();
        out.latency = r.U16();
        out.datum = r.u8();
        r.Skip(1); // reserved
        out.sb_list = r.U16();
        // Optional groups appear on the wire in bit order; absent groups keep
        // their default NaN (the reference's setDoNotUse semantics)
        if (out.sb_list & 1) {
            out.latitude_std_dev = r.F32();
            out.longitude_std_dev = r.F32();
            out.height_std_dev = r.F32();
        }
        if (out.sb_list & 2) {
            out.heading = r.F32();
            out.pitch = r.F32();
            out.roll = r.F32();
        }
        if (out.sb_list & 4) {
            out.heading_std_dev = r.F32();
            out.pitch_std_dev = r.F32();
            out.roll_std_dev = r.F32();
        }
        if (out.sb_list & 8) {
            out.ve = r.F32();
            out.vn = r.F32();
            out.vu = r.F32();
        }
        if (out.sb_list & 16) {
            out.ve_std_dev = r.F32();
            out.vn_std_dev = r.F32();
            out.vu_std_dev = r.F32();
        }
        if (out.sb_list & 32) {
            out.latitude_longitude_cov = r.F32();
            out.latitude_height_cov = r.F32();
            out.longitude_height_cov = r.F32();
        }
        if (out.sb_list & 64) {
            out.heading_pitch_cov = r.F32();
            out.heading_roll_cov = r.F32();
            out.pitch_roll_cov = r.F32();
        }
        if (out.sb_list & 128) {
            out.ve_vn_cov = r.F32();
            out.ve_vu_cov = r.F32();
            out.vn_vu_cov = r.F32();
        }
        return r.ok;
    }


    bool ParseReceiverStatus(const std::uint8_t *data, std::size_t size, ReceiverStatus &out) {
        Reader r{data, size};
        if (!ReadFrameHeader(r, out.block_header) || out.block_header.id != kIdReceiverStatus) {
            return false;
        }
        out.cpu_load = r.u8();
        out.ext_error = r.u8();
        out.up_time = r.U32();
        out.rx_status = r.U32();
        out.rx_error = r.U32();
        out.n = r.u8();
        out.sb_length = r.u8();
        out.cmd_count = r.u8();
        out.temperature = r.u8();
        if (!r.ok) {
            return false;
        }

        if (out.n > 0 && out.sb_length < 4) {
            return false;
        }
        out.agc_state.resize(out.n);
        for (auto &agc: out.agc_state) {
            agc.frontend_id = r.u8();
            agc.gain = r.i8();
            agc.sample_var = r.u8();
            agc.blanking_stat = r.u8();
            r.Skip(out.sb_length - 4u); // padding
        }
        return r.ok;
    }
} // namespace asterx::sbf
