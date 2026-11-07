"""Synthetic eBUS byte/layout contracts. No SDK or hardware; not executed by this migration."""

import copy
import json
import numpy as np
import struct
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from app.decoder import fx10
from app.decoder.ebus import pixels, recording


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


def write_fixture(directory, *, invalid=True, kind="fx10", sdk_version="6.5.1.6797"):
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
    recording_format = f"ebus-raw-eBUS_SDK_v{sdk_version}"
    (directory / "device.json").write_text(json.dumps({
        "format": f"ebus-device-eBUS_SDK_v{sdk_version}", "kind": kind, "id": directory.name,
        "recording_format": recording_format, "sdk": {"name": "Pleora eBUS", "header_version": sdk_version},
        "profile": {"samples": 2, "bands": 2, "pixel_format": "Mono12Packed", "full_scale": 4095,
                    "expected_frame_rate_hz": 50.0, "wavelengths_nm": [500.0, 700.0]}}), encoding="utf-8")
    (directory / "events.jsonl").write_text(json.dumps({
        "event": "session_end", "format": recording_format, "complete": True,
        "records_written": 2 if invalid else 1,
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

    def test_sdk_version_identifies_producer_without_pinning_one_sdk_release(self):
        for version in ("6.5.1.6797", "6.6.0.7000"):
            with self.subTest(version=version):
                camera = self.root / version
                write_fixture(camera, sdk_version=version)
                device = recording.device_metadata(camera)
                self.assertEqual(device["sdk"]["header_version"], version)
                self.assertTrue(recording.is_session(camera))
                self.assertEqual(len(list(recording.session_records(camera))), 2)

    def test_metadata_rejects_legacy_and_inconsistent_sdk_identities(self):
        path = self.camera / "device.json"
        original = json.loads(path.read_text(encoding="utf-8"))
        changes = [
            {"format": "ebus-device", "recording_format": "ebus-raw"},
            {"format": "ebus-device-v1", "recording_format": "ebus-raw-v1"},
            {"format": "ebus-device-eBUS_SDK_v6.5.1.6798"},
            {"recording_format": "ebus-raw-eBUS_SDK_v6.5.1.6798"},
            {"sdk": None},
            {"sdk": {"header_version": 6516797}},
            {"sdk": {"header_version": "6.5.1.6798"}},
        ]
        for version in ("6.5.1", "6.5.1.bad", "6.5.1.6797\n", "６.5.1.6797"):
            changes.append({"format": f"ebus-device-eBUS_SDK_v{version}",
                            "recording_format": f"ebus-raw-eBUS_SDK_v{version}",
                            "sdk": {"header_version": version}})
        for change in changes:
            with self.subTest(change=change):
                path.write_text(json.dumps(dict(original, **change)), encoding="utf-8")
                with self.assertRaisesRegex(ValueError, "Unsupported eBUS device metadata"):
                    recording.device_metadata(self.camera)
                # A valid RAW magic must not hide invalid metadata already present beside it.
                self.assertFalse(recording.is_session(self.camera))

    def test_session_end_format_must_match_device_sdk_identity(self):
        path = self.camera / "events.jsonl"
        ending = json.loads(path.read_text(encoding="utf-8"))
        for identity in (None, "ebus-raw", "ebus-raw-v1", "ebus-raw-eBUS_SDK_v6.5.1.6798"):
            with self.subTest(identity=identity):
                ending["format"] = identity
                path.write_text(json.dumps(ending) + "\n", encoding="utf-8")
                with self.assertRaisesRegex(ValueError, "session_end format disagrees"):
                    list(recording.session_records(self.camera))

    def test_sdk_identity_does_not_bypass_binary_layout_version(self):
        header = bytearray(fixture_header())
        struct.pack_into("<H", header, 8, 2)
        struct.pack_into("<I", header, 60, fixture_crc(header[:60]))
        self.segment.write_bytes(header + fixture_record(self.meta, self.payload))
        with self.assertRaisesRegex(ValueError, "Unsupported eBUS file header"):
            list(recording.session_records(self.camera))

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
        self.assertEqual(capture.pixel_format, "Mono12Packed")
        self.assertEqual(capture.full_scale, 4095)
        np.testing.assert_array_equal(cube, self.expected[None, ...])
        tail, timestamp = fx10.raw_tail_cube(self.camera, capture, 50)
        np.testing.assert_array_equal(tail, cube)
        self.assertEqual(timestamp, self.meta["host_realtime_ns"])
        _, count, values, mean, clipped = fx10.reference_spectrum(self.camera, 1)
        self.assertEqual(count, 1)
        self.assertEqual(values, fx10.spectrum(cube, 4095)[0])
        self.assertEqual(values["mean"], [1519.5, 2047.5])
        self.assertEqual(mean, 1783.5)
        self.assertEqual(clipped, 25.0)
        with self.assertRaises(ValueError):
            fx10.reference_spectrum(self.camera, 2)

    def test_live_scan_budget_fails_explicitly_instead_of_returning_an_old_prefix(self):
        with patch.object(recording, "MAX_PREVIEW_SCAN_RECORDS", 1):
            with self.assertRaises(ValueError):
                recording.latest_records(self.camera, 1)

    def test_invalid_record_payload_crc_can_be_checked_by_reader(self):
        damaged = bytearray(self.segment.read_bytes())
        damaged[-1] ^= 1
        self.segment.write_bytes(damaged)
        records = list(recording.session_records(self.camera))
        self.assertFalse(records[-1].valid_frame)
        with self.assertRaises(ValueError):
            recording.read_payload(records[-1], verify_crc=True)

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
