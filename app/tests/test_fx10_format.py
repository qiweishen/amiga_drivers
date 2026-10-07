"""Offline format fixtures only; no capture program or device is needed."""

import json
import tempfile
import unittest
from pathlib import Path

import numpy as np

from app.formats import fx10


V1_COLUMNS = ("event,segment_line,global_line,byte_offset,block_id,count,device_timestamp_raw,"
              "host_receive_realtime_ns,host_receive_monotonic_ns,block_id_anomaly")
V2_COLUMNS = V1_COLUMNS + (",sdk_acquired_size,sdk_payload_type,sdk_operation_result,sdk_chunk_count,"
    "sdk_image_present,sdk_pixel_type,sdk_width,sdk_height,sdk_padding_x,sdk_padding_y,sdk_image_size,sdk_effective_image_size")


class Fx10FormatTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def recording(self, *, version=2, live=False, events=None):
        self.capture = {"format": "fx10-capture-v1", "samples": 2, "bands": 2,
                        "bytes_per_pixel": 2, "byte_order": "little", "pixel_format": "Mono12Packed",
                        "full_scale": 4095, "expected_frame_rate_hz": 50.0,
                        "wavelengths_nm": [500.0, 700.0], "line_index_format": f"fx10-line-index-v{version}"}
        (self.root / "capture.json").write_text(json.dumps(self.capture), encoding="utf-8")
        self.cube = np.array([[[0, 4095], [10, 20]], [[1, 2], [3, 4]],
                              [[4096, 30], [40, 65535]]], dtype="<u2")
        self.bil = self.root / ("segment_0001.bil.part" if live else "segment_0001.bil")
        self.bil.write_bytes(self.cube.tobytes())
        self.hdr = self.root / "segment_0001.hdr"
        self.hdr.write_text("ENVI\nsamples = 2\nbands = 2\nlines = 3\nheader offset = 0\n"
                            "data type = 12\ninterleave = bil\nbyte order = 0\n"
                            "wavelength units = Nanometers\nwavelength = {500, 700}\n", encoding="utf-8")
        self.index = self.root / ("segment_0001.lines.csv.part" if live else "segment_0001.lines.csv")
        header = f"# fx10-line-index-v{version}; zero-based indices\n" + (V1_COLUMNS if version == 1 else V2_COLUMNS) + "\n"
        events = events or [("frame", 0), ("padding", 0), ("frame", 0)]
        self.rows = []
        for number, (event, anomaly) in enumerate(events):
            row = f"{event},{number},{number + 10},{number * 8},{100 + number},1,9007199254740993,0,0,{anomaly}"
            self.rows.append(row + ("," * 12 if version == 2 else "") + "\n")
        self.index.write_text(header + "".join(self.rows), encoding="ascii")
        return fx10.load_capture(self.root)

    def test_full_stream_and_tail_agree_for_both_index_versions(self):
        for version in (1, 2):
            with self.subTest(version=version):
                capture = self.recording(version=version)
                _, cube = fx10.snapshot_cube(self.root)
                expected = self.cube[[0, 2]]
                np.testing.assert_array_equal(cube, expected)
                tail = fx10.tail_cube(fx10.open_segment(self.bil, capture), 50)
                np.testing.assert_array_equal(tail, expected)
                _, count, values, mean, clipped = fx10.reference_spectrum(self.root, 2)
                self.assertEqual(count, 2)
                direct = fx10.spectrum(expected, 4095)
                self.assertEqual(values, direct[0])
                self.assertAlmostEqual(mean, direct[1])
                self.assertAlmostEqual(clipped, direct[2])
                # Values above the nominal bit depth are retained, never clipped.
                self.assertGreater(values["max"][1], 100.0)

    def test_anomalies_and_padding_keep_positions_but_do_not_enter_statistics(self):
        capture = self.recording(events=[("frame", 0), ("frame", 1), ("padding", 0)])
        _, cube = fx10.snapshot_cube(self.root)
        np.testing.assert_array_equal(cube, self.cube[:1])
        rows = list(fx10.committed_lines(fx10.open_segment(self.bil, capture), self.bil.stat().st_size))
        self.assertEqual([line.number for line in rows], [0, 1, 2])

    def test_layout_disagreements_are_rejected_by_every_finalized_reader(self):
        for original, changed in (("interleave = bil", "interleave = bsq"),
                                  ("data type = 12", "data type = 1"),
                                  ("wavelength = {500, 700}", "wavelength = {500, 701}"),
                                  ("byte order = 0", "byte order = 1")):
            with self.subTest(changed=changed):
                capture = self.recording()
                self.hdr.write_text(self.hdr.read_text().replace(original, changed))
                for read in (lambda: fx10.snapshot_cube(self.root),
                             lambda: fx10.reference_spectrum(self.root, 2),
                             lambda: fx10.tail_cube(fx10.open_segment(self.bil, capture), 2)):
                    with self.assertRaises(ValueError):
                        read()

    def test_capture_types_pixel_format_and_wavelengths_are_not_guessed(self):
        for key, value in (("samples", True), ("samples", 2.5), ("bytes_per_pixel", 1),
                           ("pixel_format", "Mono8"), ("wavelengths_nm", [500.0]),
                           ("expected_frame_rate_hz", float("nan"))):
            with self.subTest(key=key, value=value):
                self.recording()
                self.capture[key] = value
                (self.root / "capture.json").write_text(json.dumps(self.capture))
                with self.assertRaises(ValueError):
                    fx10.load_capture(self.root)

    def test_live_tail_uses_complete_index_rows_not_payload_length(self):
        capture = self.recording(live=True, events=[("frame", 0)] * 3)
        self.index.write_bytes(self.index.read_bytes()[:-1])  # third row lacks commit newline
        self.bil.write_bytes(self.bil.read_bytes() + b"\x01")  # unindexed partial payload
        cube = fx10.tail_cube(fx10.open_segment(self.bil, capture), 10)
        np.testing.assert_array_equal(cube, self.cube[:2])

    def test_live_tail_ignores_rows_newer_than_the_data_size_snapshot(self):
        capture = self.recording(live=True, events=[("frame", 0)] * 3)
        segment = fx10.open_segment(self.bil, capture)
        rows = list(fx10.committed_lines(segment, 16))
        self.assertEqual([row.number for row in rows], [0, 1])

    def test_tail_still_validates_version_and_column_header_outside_its_window(self):
        capture = self.recording(live=True)
        self.index.write_text(self.index.read_text().replace("fx10-line-index-v2", "fx10-line-index-v9"))
        with self.assertRaises(ValueError):
            fx10.tail_cube(fx10.open_segment(self.bil, capture), 1, tail_bytes=len(self.rows[-1]))

    def test_tail_boundary_preserves_last_complete_row(self):
        capture = self.recording(live=True)
        cube = fx10.tail_cube(fx10.open_segment(self.bil, capture), 1, tail_bytes=len(self.rows[-1]))
        np.testing.assert_array_equal(cube, self.cube[2:])

    def test_duplicate_missing_and_incomplete_index_rows_are_not_silently_skipped(self):
        for mutation in (lambda text: text.replace(self.rows[1], self.rows[0]),
                         lambda text: text.replace(self.rows[1], ""), lambda text: text[:-1]):
            self.recording()
            self.index.write_text(mutation(self.index.read_text()))
            with self.assertRaises(ValueError):
                fx10.snapshot_cube(self.root)

    def test_finalized_length_and_reference_count_must_match(self):
        self.recording()
        with self.assertRaises(ValueError):
            fx10.reference_spectrum(self.root, 3)
        self.bil.write_bytes(self.bil.read_bytes()[:-1])
        with self.assertRaises(ValueError):
            fx10.snapshot_cube(self.root)


if __name__ == "__main__":
    unittest.main()
