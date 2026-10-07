"""Synthetic raw/index fixtures and native decode vectors; no SDK or device."""

import contextlib
import io
import json
import struct
import tempfile
import unittest
from pathlib import Path

import numpy as np

from gox_driver.scripts import inspect_raw, unpack_raw
from gox_driver.scripts.jai_raw import layout, pfnc, pixels, recording


def fixture_crc(data):
    # Deliberately independent bitwise CRC to build the on-disk fixtures.
    crc = 0xFFFFFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ (0x82F63B78 if crc & 1 else 0)
    return crc ^ 0xFFFFFFFF


def file_header():
    raw = struct.pack("<8sIHHIIQ16s64s64sIII320sI", b"JAIRAWSG", 512, 1, 0, 0x0A0B0C0D,
                      1, 1, b"\0" * 16, b"cam0", b"serial", 96, 1, 2, b"\0" * 320, 0)
    return raw[:-4] + struct.pack("<I", fixture_crc(raw[:-4]))


def frame_header(payload, *, seq=0, offset=512, flags=0):
    values = (0x4D415246, 96, 10 + seq, 1000 + seq, 1800000000000000001, 20 + seq,
              0x010C0006, 2, 2, 0, 0, flags, len(payload), seq, fixture_crc(payload), 0, 0, 0)
    raw = struct.pack("<2I4Q6I2Q4I", *values)
    raw = raw[:-4] + struct.pack("<I", fixture_crc(raw[:-4]))
    row = {"seq": seq, "bid": 10 + seq, "dts": 1000 + seq, "hrt": 1800000000000000001,
           "hmn": 20 + seq, "off": offset, "psz": len(payload), "pf": 0x010C0006,
           "w": 2, "h": 2, "fl": flags, "ox": 0, "oy": 0,
           "payload_type": 1, "padding_x": 0, "padding_y": 0, "chunk_count": 0, "operation_result": 0}
    return raw, row


class GoxFormatTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        # Four 12-bit values, each legacy GVSP pair represented by three bytes.
        self.payload = bytes([0xAB, 0x3C, 0x12, 0xFF, 0x0F, 0x00])
        header, self.row = frame_header(self.payload)
        self.segment = self.root / "seg_00001.raw"
        self.segment.write_bytes(file_header() + header + self.payload)
        self.index = self.root / "seg_00001.idx.jsonl"
        self.index.write_text(json.dumps(self.row) + "\n", encoding="ascii")

    def test_cli_and_preview_share_native_decode_and_layout(self):
        with contextlib.redirect_stdout(io.StringIO()):
            code = unpack_raw.main([str(self.segment), "--format", "npy", "--limit", "1",
                                    "-o", str(self.root / "images")])
        self.assertEqual(code, 0)
        saved = np.load(self.root / "images" / "seq00000000_Mono12Packed.npy", allow_pickle=False)
        frame, raw, incomplete = recording.first_preview_frame(self.root)
        decoded, name, _ = pixels.decode(frame.pf, frame.w, frame.h, raw)
        np.testing.assert_array_equal(saved, decoded)
        np.testing.assert_array_equal(decoded, [[0xABC, 0x123], [0xFFF, 0]])
        self.assertEqual(name, "Mono12Packed")
        self.assertFalse(incomplete)
        self.assertEqual(inspect_raw.expected_payload_size(frame), len(self.payload))
        self.assertEqual(pfnc.format_for_code(frame.pf).full_scale, 4095)
        self.assertEqual(frame.hrt, 1800000000000000001)

    def test_hand_computed_packing_vectors(self):
        for code, raw, expected in (
            (0x010C0006, bytes([0xAB, 0x3C, 0x12]), [0xABC, 0x123]),
            (0x010C0004, bytes([0xAA, 0x13, 0x55]), [0x2AB, 0x155]),
            (0x010C0047, bytes([0xBC, 0x3A, 0x12]), [0xABC, 0x123]),
            (0x010A0046, bytes([0x01, 0x0C, 0x58, 0xD0, 0xFD]), [0x001, 0x203, 0x105, 0x3F7]),
        ):
            with self.subTest(code=code):
                image, _, _ = pixels.decode(code, len(expected), 1, raw)
                np.testing.assert_array_equal(image[0], expected)

    def test_complete_json_without_newline_is_not_a_live_commit(self):
        newer = dict(self.row, seq=1, off=614)
        self.index.write_text(json.dumps(self.row) + "\n" + json.dumps(newer), encoding="ascii")
        rows = list(recording.index_records(self.index, tail_bytes=recording.INDEX_TAIL_BYTES))
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["seq"], 0)

    def test_tail_start_at_record_boundary_does_not_discard_the_record(self):
        raw = self.index.read_bytes()
        self.index.write_bytes(b'{"ignored":"outside bounded tail"}\n' + raw)
        rows = list(recording.index_records(self.index, tail_bytes=len(raw)))
        self.assertEqual(rows, [self.row])

    def test_index_header_identity_is_checked_before_decode(self):
        for field, value in (("w", 4), ("seq", 3), ("hrt", 1800000000000000002),
                             ("off", 513), ("psz", 7), ("padding_x", 1), ("chunk_count", 1), ("fl", 1)):
            with self.subTest(field=field):
                with self.assertRaises(ValueError):
                    recording.read_indexed_frame(self.segment, dict(self.row, **{field: value}))

    def test_unknown_layout_is_rejected_instead_of_guessing_stride(self):
        row = dict(self.row)
        del row["padding_x"]
        with self.assertRaises(ValueError):
            recording.read_indexed_frame(self.segment, row)

    def test_duplicate_or_reversed_committed_index_records_fail(self):
        self.index.write_text((json.dumps(self.row) + "\n") * 2)
        with self.assertRaises(ValueError):
            list(recording.index_records(self.index))

    def test_header_crc_and_optional_full_payload_crc_have_distinct_policies(self):
        original = self.segment.read_bytes()
        changed = bytearray(original)
        changed[520] ^= 1
        self.segment.write_bytes(changed)
        with self.assertRaises(ValueError):
            recording.read_indexed_frame(self.segment, self.row)
        changed = bytearray(original)
        changed[-1] ^= 1
        self.segment.write_bytes(changed)
        recording.read_indexed_frame(self.segment, self.row)  # preview's explicit header-only integrity policy
        with self.assertRaises(ValueError):
            recording.read_indexed_frame(self.segment, self.row, verify_payload_crc=True)
        self.assertEqual(layout.crc32c(b"123456789"), 0xE3069283)

    def test_truncated_and_ambiguous_payloads_are_not_zero_padded_by_preview(self):
        self.segment.write_bytes(self.segment.read_bytes()[:-1])
        with self.assertRaises(ValueError):
            recording.read_indexed_frame(self.segment, self.row)
        for width, height, payload in ((2, 2, self.payload[:-1]), (2, 2, self.payload + b"\0"),
                                        (1, 4, self.payload)):
            with self.assertRaises(ValueError):
                pixels.decode(0x010C0006, width, height, payload)


if __name__ == "__main__":
    unittest.main()
