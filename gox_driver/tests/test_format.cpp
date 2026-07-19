// Locks the on-disk byte layout (core/format.hpp) with golden bytes and
// exercises CRC-32C. sizeof/offsetof are already static_assert'ed in the
// header; the tests here pin the actual encoded bytes so any layout or
// endianness regression fails loudly, independent of the C++ struct rules.

#include "core/format.hpp"

#include "core/util.hpp"

#include <doctest/doctest.h>

#include <cstdint>
#include <cstring>
#include <string>

using namespace jai::format;

TEST_CASE("crc32c: standard vector, empty input and incremental chaining") {
    // RFC 3720 / common CRC-32C (Castagnoli) test vector.
    CHECK(crc32c("123456789", 9) == 0xE3069283u);

    // Empty input yields 0 with the default seed and preserves any seed
    // (crc32c(data, 0, seed) == seed), which is what makes chaining work.
    CHECK(crc32c("", 0) == 0u);
    CHECK(crc32c("", 0, 0xDEADBEEFu) == 0xDEADBEEFu);

    // Chained/seeded computation equals the one-shot result.
    CHECK(crc32c("6789", 4, crc32c("12345", 5)) == 0xE3069283u);

    // Byte-at-a-time chaining also equals the one-shot result.
    const char* s = "123456789";
    uint32_t crc = 0;
    for (size_t i = 0; i < 9; ++i) {
        crc = crc32c(s + i, 1, crc);
    }
    CHECK(crc == 0xE3069283u);

    // Longer buffer split at an odd boundary: exercises the unaligned tail
    // of the SSE4.2 path as well as the seed handling on multi-word input.
    uint8_t buf[1024];
    for (size_t i = 0; i < sizeof(buf); ++i) {
        buf[i] = static_cast<uint8_t>(i * 7 + 3);
    }
    const uint32_t whole = crc32c(buf, sizeof(buf));
    CHECK(crc32c(buf + 13, sizeof(buf) - 13, crc32c(buf, 13)) == whole);
}

TEST_CASE("format: FrameHeader golden bytes (little-endian layout lock)") {
    FrameHeader h{};
    h.frame_magic = kFrameMagic;      // 0x4D415246
    h.header_size = kFrameHeaderSize; // 96
    h.block_id = 0x1122334455667788ull;
    h.device_ts_ns = 0x99AABBCCDDEEFF00ull;
    h.host_realtime_ns = 0x0123456789ABCDEFull;
    h.host_monotonic_ns = 0x0F1E2D3C4B5A6978ull;
    h.pixel_format = 0x01080001u; // PFNC Mono8
    h.width = 640;
    h.height = 480;
    h.offset_x = 4;
    h.offset_y = 2;
    h.status_flags = kFrameFlagIncomplete | kFrameFlagBlockIdGap; // 0x5
    h.payload_size = 307200;                                      // 640 * 480 Mono8
    h.frame_seq = 41;
    h.payload_crc32c = 0xDEADBEEFu;
    seal_frame_header(h);

    uint8_t bytes[sizeof(FrameHeader)];
    std::memcpy(bytes, &h, sizeof(h));

    // Expected encoding of bytes [0, 92), derived by hand: every field is a
    // little-endian integer at the offset pinned by the header's
    // static_asserts. Field by field:
    //   [ 0] frame_magic       0x4D415246         -> 46 52 41 4d  ("FRAM")
    //   [ 4] header_size       96 = 0x60          -> 60 00 00 00
    //   [ 8] block_id          0x1122334455667788 -> 88 77 66 55 44 33 22 11
    //   [16] device_ts_ns      0x99AABBCCDDEEFF00 -> 00 ff ee dd cc bb aa 99
    //   [24] host_realtime_ns  0x0123456789ABCDEF -> ef cd ab 89 67 45 23 01
    //   [32] host_monotonic_ns 0x0F1E2D3C4B5A6978 -> 78 69 5a 4b 3c 2d 1e 0f
    //   [40] pixel_format      0x01080001         -> 01 00 08 01
    //   [44] width             640 = 0x280        -> 80 02 00 00
    //   [48] height            480 = 0x1E0        -> e0 01 00 00
    //   [52] offset_x          4                  -> 04 00 00 00
    //   [56] offset_y          2                  -> 02 00 00 00
    //   [60] status_flags      0x5                -> 05 00 00 00
    //   [64] payload_size      307200 = 0x4B000   -> 00 b0 04 00 00 00 00 00
    //   [72] frame_seq         41 = 0x29          -> 29 00 00 00 00 00 00 00
    //   [80] payload_crc32c    0xDEADBEEF         -> ef be ad de
    //   [84] reserved0         0                  -> 00 00 00 00
    //   [88] reserved1         0                  -> 00 00 00 00
    const std::string expected_hex =                                            //
        "4652414d" "60000000"                                                   //
        "8877665544332211" "00ffeeddccbbaa99" "efcdab8967452301" "78695a4b3c2d1e0f" //
        "01000801" "80020000" "e0010000" "04000000" "02000000" "05000000"       //
        "00b0040000000000" "2900000000000000"                                   //
        "efbeadde" "00000000" "00000000";
    CHECK(jai::hex_prefix(bytes, 92) == expected_hex);

    // The trailing 4 bytes are the CRC-32C over the 92 bytes above; computed
    // here (not hardcoded) so the golden test stays valid if the CRC
    // implementation is swapped for another correct one.
    uint32_t stored_crc = 0;
    std::memcpy(&stored_crc, bytes + 92, 4);
    CHECK(stored_crc == crc32c(bytes, 92));
    CHECK(verify_frame_header(h));

    // Any corrupted byte must break verification.
    FrameHeader corrupted = h;
    reinterpret_cast<uint8_t*>(&corrupted)[17] ^= 0x01; // inside device_ts_ns
    CHECK_FALSE(verify_frame_header(corrupted));

    // Re-sealing after a change makes the header verifiable again.
    corrupted.status_flags |= kFrameFlagResultNotOk;
    seal_frame_header(corrupted);
    CHECK(verify_frame_header(corrupted));
}

TEST_CASE("format: FileHeader golden byte spot checks via make_file_header") {
    uint8_t uuid[16];
    for (int i = 0; i < 16; ++i) {
        uuid[i] = static_cast<uint8_t>(i);
    }
    FileHeader fh = make_file_header(/*segment_index=*/7,
                                     /*created_realtime_ns=*/0x0102030405060708ull, uuid, "cam0",
                                     "SN-GOX-0042", /*record_align=*/4096, kSegFlagPayloadCrc);

    uint8_t bytes[sizeof(FileHeader)];
    std::memcpy(bytes, &fh, sizeof(fh));

    // Spot checks at the offsets pinned by the header's static_asserts;
    // expected bytes derived by hand from the little-endian encoding.
    CHECK(std::memcmp(bytes, "JAIRAWSG", 8) == 0);               // [0]   file_magic
    CHECK(jai::hex_prefix(bytes + 8, 4) == "00020000");          // [8]   file_header_size = 512
    CHECK(jai::hex_prefix(bytes + 12, 2) == "0100");             // [12]  version_major = 1
    CHECK(jai::hex_prefix(bytes + 14, 2) == "0000");             // [14]  version_minor = 0
    CHECK(jai::hex_prefix(bytes + 16, 4) == "0d0c0b0a");         // [16]  BOM 0x0A0B0C0D
    CHECK(jai::hex_prefix(bytes + 20, 4) == "07000000");         // [20]  segment_index = 7
    CHECK(jai::hex_prefix(bytes + 24, 8) == "0807060504030201"); // [24]  created_realtime_ns
    CHECK(std::memcmp(bytes + 32, uuid, 16) == 0);               // [32]  session_uuid
    CHECK(std::memcmp(bytes + 48, "cam0\0", 5) == 0);            // [48]  camera_id, NUL padded
    CHECK(std::memcmp(bytes + 112, "SN-GOX-0042\0", 12) == 0);   // [112] camera_serial
    CHECK(jai::hex_prefix(bytes + 176, 4) == "60000000");        // [176] frame_header_size = 96
    CHECK(jai::hex_prefix(bytes + 180, 4) == "00100000");        // [180] record_align = 4096
    CHECK(jai::hex_prefix(bytes + 184, 4) == "02000000");        // [184] segment_flags = PAYLOAD_CRC

    // The string tails and the reserved block must be all zero.
    size_t nonzero = 0;
    for (size_t i = 48 + 4; i < 112; ++i) {
        nonzero += bytes[i] != 0 ? 1 : 0;
    }
    for (size_t i = 112 + 11; i < 176; ++i) {
        nonzero += bytes[i] != 0 ? 1 : 0;
    }
    for (size_t i = 188; i < 508; ++i) { // reserved[320]
        nonzero += bytes[i] != 0 ? 1 : 0;
    }
    CHECK(nonzero == 0u);

    // [508] header_crc32c over bytes [0, 508); computed, not hardcoded.
    uint32_t stored_crc = 0;
    std::memcpy(&stored_crc, bytes + 508, 4);
    CHECK(stored_crc == crc32c(bytes, 508));
    CHECK(verify_file_header(fh));

    // A single corrupted byte (even inside reserved space) breaks verification.
    FileHeader corrupted = fh;
    reinterpret_cast<uint8_t*>(&corrupted)[200] ^= 0x40;
    CHECK_FALSE(verify_file_header(corrupted));
}

TEST_CASE("format: make_file_header truncates over-long camera identifiers") {
    uint8_t uuid[16] = {};
    const std::string long_id(80, 'x');
    const std::string long_serial(100, 'y');
    FileHeader fh = make_file_header(1, 0, uuid, long_id.c_str(), long_serial.c_str(),
                                     /*record_align=*/1, /*segment_flags=*/0);
    CHECK(fh.camera_id[63] == '\0'); // always NUL terminated after truncation
    CHECK(std::string(fh.camera_id) == std::string(63, 'x'));
    CHECK(fh.camera_serial[63] == '\0');
    CHECK(std::string(fh.camera_serial) == std::string(63, 'y'));
    CHECK(fh.record_align == 1u);
    CHECK(fh.segment_flags == 0u);
    CHECK(verify_file_header(fh));
}

TEST_CASE("format: align_up edge cases") {
    CHECK(align_up(0, 4096) == 0u);
    CHECK(align_up(0, 1) == 0u);
    CHECK(align_up(1, 1) == 1u); // align 1: identity (alignment disabled)
    CHECK(align_up(12345, 1) == 12345u);
    CHECK(align_up(7, 0) == 7u);            // align 0 is treated as "no alignment"
    CHECK(align_up(4096, 4096) == 4096u);   // exact multiple stays put
    CHECK(align_up(8192, 4096) == 8192u);
    CHECK(align_up(4097, 4096) == 8192u);   // one past a boundary rounds up
    CHECK(align_up(4095, 4096) == 4096u);   // one before a boundary rounds up
    CHECK(align_up(96 + 100, 512) == 512u); // typical frame record rounding
    static_assert(align_up(513, 512) == 1024, "align_up must be usable in constant expressions");
}
