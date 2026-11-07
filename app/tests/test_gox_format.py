"""Native pixel vectors and rejection of unsupported camera recordings; no SDK."""

import json
import numpy as np
import tempfile
import unittest
from pathlib import Path

from app.decoder.ebus import pfnc, pixels, recording
from app.decoder.gox import recording as gox_recording
from app.tests.test_ebus_raw_format import fixture_header, fixture_image, fixture_record, write_fixture


def write_gox_fixture(directory, *, pixel_format=0x010C002B):
    """A BayerRG12Packed image with a leading offset, row padding and preserved tail.

    GVSP 12Packed rows of 2 px / 3 B: (0xABC, 0x123) -> AB 3C 12, (0xFFF, 0) -> FF 0F 00.
    """
    write_fixture(directory, kind="gox", invalid=False)
    payload = b"\xaa\xbb" + bytes([0xAB, 0x3C, 0x12, 0xEE, 0xFF, 0x0F, 0x00, 0xDD]) + b"\xcc\xccTAIL"
    meta = fixture_image(len(payload))
    meta["image"].update(pixel_format=pixel_format)
    (directory / "seg_00001.raw").write_bytes(fixture_header() + fixture_record(meta, payload))
    metadata_path = directory / "device.json"
    device = json.loads(metadata_path.read_text(encoding="utf-8"))
    device["profile"] = {"pixel_format": "BayerRG12Packed"}
    metadata_path.write_text(json.dumps(device), encoding="utf-8")
    event_path = directory / "events.jsonl"
    ending = json.loads(event_path.read_text(encoding="utf-8"))
    ending["payload_bytes_written"] = len(payload)
    event_path.write_text(json.dumps(ending) + "\n", encoding="utf-8")
    return payload, meta


class GoxFormatTests(unittest.TestCase):
    def test_current_raw_snapshot_and_live_share_native_decode(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            camera = root / "camera"
            write_gox_fixture(camera)
            snapshot = gox_recording.snapshot_frame(camera)
            live = gox_recording.latest_frame(camera)
            self.assertEqual(snapshot.pixel_format, "BayerRG12Packed")
            self.assertEqual(snapshot.record.hrt, 1800000000000000001)
            self.assertEqual(snapshot.record.dts, 9007199254740993)
            np.testing.assert_array_equal(snapshot.image, [[0xABC, 0x123], [0xFFF, 0]])
            np.testing.assert_array_equal(live.image, snapshot.image)

    def test_gox_reader_rejects_non_native_modes_before_pixel_decode(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            # BayerRG8, unpacked BayerRG12, PFNC BayerRG12p, Mono12Packed
            for code in (0x01080009, 0x01100011, 0x010C0059, 0x010C0006):
                camera = root / f"{code:x}"
                write_gox_fixture(camera, pixel_format=code)
                for reader in (gox_recording.snapshot_frame, gox_recording.latest_frame):
                    with self.subTest(code=code, reader=reader.__name__):
                        with self.assertRaisesRegex(ValueError, "GOX requires BayerRG12Packed"):
                            reader(camera)

    def test_gox_reader_rejects_fx10_profile(self):
        with tempfile.TemporaryDirectory() as temporary:
            camera = Path(temporary) / "camera"
            write_fixture(camera, kind="fx10")
            for reader in (gox_recording.snapshot_frame, gox_recording.latest_frame):
                with self.subTest(reader=reader.__name__):
                    with self.assertRaises(ValueError):
                        reader(camera)

    def test_hand_computed_packing_vectors(self):
        for code, raw, expected in (
                (0x010C0006, bytes([0xAB, 0x3C, 0x12]), [0xABC, 0x123]),
                (0x010C002B, bytes([0xAB, 0x3C, 0x12]), [0xABC, 0x123]),  # GOX BayerRG12Packed
                (0x010C0004, bytes([0xAA, 0x13, 0x55]), [0x2AB, 0x155]),
                (0x010C0047, bytes([0xBC, 0x3A, 0x12]), [0xABC, 0x123]),
                (0x010A0046, bytes([0x01, 0x0C, 0x58, 0xD0, 0xFD]), [0x001, 0x203, 0x105, 0x3F7]),
        ):
            with self.subTest(code=code):
                image, _, _ = pixels.decode(code, len(expected), 1, raw)
                np.testing.assert_array_equal(image[0], expected)

    def test_native_decode_preserves_dn_and_effective_full_scale(self):
        payload = bytes([0xAB, 0x3C, 0x12, 0xFF, 0x0F, 0x00])
        image, name, _ = pixels.decode(0x010C0006, 2, 2, payload)
        np.testing.assert_array_equal(image, [[0xABC, 0x123], [0xFFF, 0]])
        self.assertEqual(image.dtype, np.dtype("uint16"))
        self.assertEqual(name, "Mono12Packed")
        self.assertEqual(pfnc.format_for_code(0x010C0006).full_scale, 4095)

    def test_truncated_and_ambiguous_payloads_are_not_zero_padded(self):
        payload = bytes([0xAB, 0x3C, 0x12, 0xFF, 0x0F, 0x00])
        for width, height, raw in ((2, 2, payload[:-1]), (2, 2, payload + b"\0"),
                                   (1, 4, payload)):
            with self.subTest(width=width, height=height, length=len(raw)):
                with self.assertRaises(ValueError):
                    pixels.decode(0x010C0006, width, height, raw)

    def test_old_jai_segment_is_rejected_by_readers(self):
        # The old magic is enough to exercise format rejection; no legacy parser
        # or legacy fixture writer belongs in the supported recording path.
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            segment = root / "seg_00001.raw"
            segment.write_bytes(b"JAIRAWSG" + bytes(504))
            (root / "device.json").write_text('{"format":"jai-raw-seg"}', encoding="utf-8")
            self.assertFalse(recording.is_segment(segment))
            with self.assertRaises(ValueError):
                list(recording.iter_records(segment))
            for reader in (gox_recording.snapshot_frame, gox_recording.latest_frame):
                with self.subTest(reader=reader.__name__):
                    with self.assertRaises(ValueError):
                        reader(root)


if __name__ == "__main__":
    unittest.main()
