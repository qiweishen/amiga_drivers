"""Synthetic eBUS byte/layout contracts. No SDK or hardware; not executed by this migration."""

import contextlib
import copy
import io
import json
import struct
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import numpy as np

from app.formats import fx10
from ebus_driver.scripts.ebus_raw import envi, inspection, pixels, recording
from gox_driver.scripts import unpack_raw


def fixture_crc(data):
    value = 0xFFFFFFFF
    for byte in data:
        value ^= byte
        for _ in range(8):
            value = (value >> 1) ^ (0x82F63B78 if value & 1 else 0)
    return value ^ 0xFFFFFFFF


def fixture_header(index=1):
    header = struct.pack("<8sHHIQQ28sI", b"EBUSRAW\0", 1, 64, 0, 1800000000000000001, index, bytes(28), 0)
    return header[:60] + struct.pack("<I", fixture_crc(header[:60]))


def fixture_record(meta, payload, sequence=0):
    encoded = json.dumps(meta, separators=(",", ":")).encode("utf-8")
    header = struct.pack("<4sIIIQQIIII", b"EBFR", 48, len(encoded), 0, len(payload), sequence,
                         fixture_crc(encoded), fixture_crc(payload), 0, 0)
    header = header[:40] + struct.pack("<II", fixture_crc(header[:40]), 0)
    return header + encoded + payload


def fixture_image(payload_size):
    return {"record_kind": "buffer", "block_id": 1, "device_timestamp_raw": 9007199254740993,
            "host_realtime_ns": 1800000000000000001, "host_monotonic_ns": 123456789,
            "acquired_size": payload_size, "payload_type": 1, "operation_result": 0,
            "operation_ok": True, "operation_name": "OK", "chunk_count": 1,
            "valid_frame": True, "block_id_anomaly": False, "gap_before": 0,
            "image": {"pixel_format": 0x010C0006, "width": 2, "height": 2, "offset_x": 8, "offset_y": 2,
                      "padding_x": 1, "padding_y": 2, "image_size": 10, "effective_image_size": 6,
                      "payload_offset": 2, "partial_line_missing": False,
                      "full_line_missing": False, "data_overrun": False}}


def write_fixture(directory, *, invalid=True, kind="fx10"):
    directory.mkdir(parents=True)
    payload = b"\xaa\xbb" + bytes([0xAB, 0x3C, 0x12, 0xEE, 0xFF, 0x0F, 0x00, 0xDD]) + b"\xcc\xccTAIL"
    meta = fixture_image(len(payload))
    raw = fixture_header() + fixture_record(meta, payload)
    payload_bytes = len(payload)
    if invalid:
        rejected = dict(meta, block_id=2, acquired_size=3, operation_ok=False, operation_result=7,
                        operation_name="INCOMPLETE", valid_frame=False, image=None, payload_type=0)
        raw += fixture_record(rejected, b"BAD", 1)
        payload_bytes += 3
    (directory / "seg_00001.raw").write_bytes(raw)
    (directory / "device.json").write_text(json.dumps({
        "format": "ebus-device-v1", "kind": kind, "id": directory.name, "recording_format": "ebus-raw-v1",
        "profile": {"samples": 2, "bands": 2, "pixel_format": "Mono12Packed", "full_scale": 4095,
                    "expected_frame_rate_hz": 50.0, "wavelengths_nm": [500.0, 700.0]}}), encoding="utf-8")
    (directory / "events.jsonl").write_text(json.dumps({
        "event": "session_end", "complete": True, "records_written": 2 if invalid else 1,
        "frames_written": 1, "valid_frames_written": 1, "payload_bytes_written": payload_bytes,
        "segments_created": 1}) + "\n", encoding="utf-8")
    return payload, meta


class EbusRawFormatTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.camera = self.root / "camera"
        self.payload, self.meta = write_fixture(self.camera)
        self.segment = self.camera / "seg_00001.raw"
        self.expected = np.array([[0xABC, 0x123], [0xFFF, 0]], dtype=np.uint16)

    def test_prefix_layout_crc_and_exact_raw_bytes(self):
        self.assertEqual(struct.calcsize(recording.FILE_HEADER_FMT), 64)
        self.assertEqual(struct.calcsize(recording.RECORD_HEADER_FMT), 48)
        self.assertEqual(recording.crc32c(b"123456789"), 0xE3069283)
        self.assertEqual(recording.crc32c(b"456789", recording.crc32c(b"123")), 0xE3069283)
        records = list(recording.session_records(self.camera))
        self.assertEqual([record.sequence for record in records], [0, 1])
        self.assertEqual([record.valid_frame for record in records], [True, False])
        self.assertEqual(recording.read_payload(records[0], verify_crc=True), self.payload)
        self.assertEqual(recording.read_payload(records[1], verify_crc=True), b"BAD")
        self.assertEqual(records[0].dts, 9007199254740993)

    def test_padding_offset_and_chunk_tail_decode_without_changing_payload(self):
        record = next(recording.iter_records(self.segment))
        raw = recording.read_payload(record)
        image, name, _ = pixels.decode_record(record, raw)
        np.testing.assert_array_equal(image, self.expected)
        self.assertEqual(name, "Mono12Packed")
        self.assertEqual(raw, self.payload)
        self.assertEqual(record.metadata["image"]["offset_x"], 8)

    def test_unknown_image_offset_is_not_guessed(self):
        meta = copy.deepcopy(self.meta)
        meta["image"]["payload_offset"] = None
        self.segment.write_bytes(fixture_header() + fixture_record(meta, self.payload))
        record = next(recording.iter_records(self.segment))
        with self.assertRaises(ValueError):
            pixels.decode_record(record, recording.read_payload(record))

    def test_torn_tail_only_ignored_by_explicit_live_reads(self):
        complete = fixture_header() + fixture_record(self.meta, self.payload)
        self.segment.write_bytes(complete + fixture_record(self.meta, self.payload, 1)[:-3])
        self.assertEqual(len(list(recording.iter_records(self.segment, live=True))), 1)
        with self.assertRaises(ValueError):
            list(recording.iter_records(self.segment))

    def test_prefix_metadata_and_payload_corruption_have_separate_checks(self):
        original = fixture_header() + fixture_record(self.meta, self.payload)
        for position in (16, 64 + 16, 64 + 48):
            damaged = bytearray(original)
            damaged[position] ^= 1
            self.segment.write_bytes(damaged)
            with self.assertRaises(ValueError):
                list(recording.iter_records(self.segment))
        damaged = bytearray(original)
        damaged[-1] ^= 1
        self.segment.write_bytes(damaged)
        record = next(recording.iter_records(self.segment))
        recording.read_payload(record)  # explicit preview policy, not a payload integrity audit
        with self.assertRaises(ValueError):
            recording.verify_payload(record)
        with self.assertRaises(ValueError):
            recording.read_payload(record, verify_crc=True)

    def test_acquired_size_and_session_count_disagreements_are_rejected(self):
        meta = dict(self.meta, acquired_size=len(self.payload) + 1)
        original = self.segment.read_bytes()
        self.segment.write_bytes(fixture_header() + fixture_record(meta, self.payload))
        with self.assertRaises(ValueError):
            list(recording.iter_records(self.segment))
        self.segment.write_bytes(original)
        ending = json.loads((self.camera / "events.jsonl").read_text())
        ending["valid_frames_written"] = 2
        (self.camera / "events.jsonl").write_text(json.dumps(ending) + "\n")
        with self.assertRaises(ValueError):
            list(recording.session_records(self.camera))

    def test_fx10_snapshot_tail_and_reference_count_valid_frames(self):
        capture, cube = fx10.snapshot_cube(self.camera)
        self.assertEqual(capture.recording_format, "ebus-raw-v1")
        np.testing.assert_array_equal(cube, self.expected[None, ...])
        tail, timestamp = fx10.raw_tail_cube(self.camera, capture, 50)
        np.testing.assert_array_equal(tail, cube)
        self.assertEqual(timestamp, self.meta["host_realtime_ns"])
        _, count, values, mean, clipped = fx10.reference_spectrum(self.camera, 1, normalized=False)
        self.assertEqual(count, 1)
        self.assertEqual(values, fx10.spectrum(cube, 4095, normalized=False)[0])
        self.assertEqual(clipped, 25.0)
        with self.assertRaises(ValueError):
            fx10.reference_spectrum(self.camera, 2)

    def test_live_scan_budget_fails_explicitly_instead_of_returning_an_old_prefix(self):
        with patch.object(recording, "MAX_PREVIEW_SCAN_RECORDS", 1):
            with self.assertRaises(ValueError):
                recording.latest_records(self.camera, 1)

    def test_envi_export_round_trips_through_legacy_reader_and_preserves_source(self):
        before = self.segment.read_bytes()
        output = self.root / "export"
        self.assertEqual(envi.export_envi(self.camera, output), 1)
        self.assertEqual(self.segment.read_bytes(), before)
        capture, cube = fx10.snapshot_cube(output)
        self.assertEqual(capture.recording_format, "envi")
        np.testing.assert_array_equal(cube, self.expected[None, ...])
        lineage = [json.loads(line) for line in (output / "source.records.jsonl").read_text().splitlines()]
        self.assertEqual([row["output_line"] for row in lineage], [0, None])
        self.assertEqual(lineage[1]["metadata"]["operation_name"], "INCOMPLETE")
        with self.assertRaises(FileExistsError):
            envi.export_envi(self.camera, output)

    def test_multi_camera_unpack_keeps_identity_and_refuses_overwrite(self):
        root = self.root / "gox"
        write_fixture(root / "cam0", invalid=False, kind="gox")
        write_fixture(root / "cam1", invalid=False, kind="gox")
        output = self.root / "images"
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(unpack_raw.main([str(root), "-o", str(output)]), 0)
            self.assertEqual(unpack_raw.main([str(root), "-o", str(output)]), 1)
        for name in ("cam0", "cam1"):
            np.testing.assert_array_equal(np.load(output / name / "seq00000000_Mono12Packed.npy", allow_pickle=False), self.expected)

    def test_inspection_commands_preserve_invalid_record_bytes_and_source(self):
        before = self.segment.read_bytes()
        index = self.root / "rebuilt.jsonl"
        extracted = self.root / "invalid.bin"
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(inspection.main(["list", str(self.segment), "--limit", "1"]), 0)
            self.assertEqual(inspection.main(["verify", str(self.camera)]), 0)
            self.assertEqual(inspection.main(["rebuild-index", str(self.segment), "-o", str(index)]), 0)
            self.assertEqual(inspection.main(["extract", str(self.segment), "--seq", "1", "-o", str(extracted)]), 0)
        self.assertEqual(self.segment.read_bytes(), before)
        self.assertEqual(extracted.read_bytes(), b"BAD")
        rows = [json.loads(line) for line in index.read_text().splitlines()]
        self.assertEqual([row["metadata"]["valid_frame"] for row in rows], [True, False])

    def test_invalid_payload_crc_is_checked_before_export_can_complete(self):
        damaged = bytearray(self.segment.read_bytes())
        damaged[-1] ^= 1  # the invalid record is retained and must still be audited
        self.segment.write_bytes(damaged)
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(inspection.main(["verify", str(self.camera)]), 1)
        output = self.root / "bad-export"
        with self.assertRaises(ValueError):
            envi.export_envi(self.camera, output)
        self.assertFalse((output / "segment_0001.hdr").exists())

    def test_reference_validator_accepts_raw_phases_without_envi_sidecars(self):
        from app.services import fx10_reference
        session = self.root / "reference_fixture"
        phases = []
        for name in ("white", "dark"):
            path = session / "raw" / "fx10" / name / "fx10_fixture"
            write_fixture(path)
            phases.append({"phase": name, "status": "completed", "requested_duration_s": 5.0,
                           "frames_written": 1, "records_written": 2, "session_dir": str(path.relative_to(session))})
        manifest = {"format": "fx10-reference-v1", "status": "completed", "phase_duration_s": 5.0, "phases": phases}
        (session / "reference.json").write_text(json.dumps(manifest))
        with patch.object(fx10_reference.runtime, "to_host_path", side_effect=Path):
            path, doc = fx10_reference._validate_result(str(session), self.root, 5.0)
            self.assertEqual(path, session)
            self.assertEqual(doc["phases"][0]["frames_written"], 1)
            manifest["phases"][0]["records_written"] = 1
            (session / "reference.json").write_text(json.dumps(manifest))
            with self.assertRaises(ValueError):
                fx10_reference._validate_result(str(session), self.root, 5.0)


if __name__ == "__main__":
    unittest.main()
