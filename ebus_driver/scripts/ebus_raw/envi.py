"""Explicit offline FX10 raw-to-ENVI derivative. Never part of acquisition."""

from __future__ import annotations

import argparse
import csv
import json
import math
import os
from pathlib import Path
import sys

import numpy as np

from . import pixels, recording
from .inspection import _publish

_COLUMNS = ("event", "segment_line", "global_line", "byte_offset", "block_id", "count",
            "device_timestamp_raw", "host_receive_realtime_ns", "host_receive_monotonic_ns", "block_id_anomaly",
            "sdk_acquired_size", "sdk_payload_type", "sdk_operation_result", "sdk_chunk_count", "sdk_image_present",
            "sdk_pixel_type", "sdk_width", "sdk_height", "sdk_padding_x", "sdk_padding_y", "sdk_image_size", "sdk_effective_image_size")


def _write_json(path, value):
    with path.open("x", encoding="utf-8") as out:
        json.dump(value, out, ensure_ascii=False, allow_nan=False, indent=2)
        out.write("\n")
        out.flush()
        os.fsync(out.fileno())


def export_envi(source: Path, output: Path, *, allow_incomplete=False):
    source, output = Path(source), Path(output)
    device = recording.device_metadata(source)
    if device["kind"] != "fx10":
        raise ValueError("ENVI export requires an FX10 profile")
    profile = device.get("profile")
    if not isinstance(profile, dict):
        raise ValueError("Missing FX10 profile")
    pixel = pixels.format_for_name(profile["pixel_format"])
    if pixel.pattern != pixels.MONO or pixel.bits not in (8, 10, 12):
        raise ValueError("Unsupported FX10 ENVI PixelFormat")
    samples, bands = profile["samples"], profile["bands"]
    if (type(samples) is not int or type(bands) is not int or min(samples, bands) < 1
            or max(samples, bands) > 65535 or samples * bands * 2 > recording.MAX_PAYLOAD_BYTES):
        raise ValueError("Invalid FX10 profile dimensions")
    if profile.get("full_scale") != pixel.full_scale:
        raise ValueError("FX10 profile full scale disagrees with PixelFormat")
    wavelengths = profile.get("wavelengths_nm", [])
    if not isinstance(wavelengths, list) or (wavelengths and len(wavelengths) != bands):
        raise ValueError("Invalid FX10 wavelength axis")
    if any(type(value) not in (int, float) or not math.isfinite(value) or value <= 0 for value in wavelengths):
        raise ValueError("Invalid FX10 wavelength value")
    fwhm = profile.get("fwhm_nm", [])
    if (not isinstance(fwhm, list) or (fwhm and len(fwhm) != bands)
            or any(type(value) not in (int, float) or not math.isfinite(value) or value <= 0 for value in fwhm)):
        raise ValueError("Invalid FX10 FWHM axis")
    rate = profile.get("expected_frame_rate_hz", 0)
    if type(rate) not in (int, float) or not math.isfinite(rate) or not 0 <= rate <= 100000:
        raise ValueError("Invalid FX10 expected frame rate")
    if not allow_incomplete:
        recording.session_end(source)
    output.mkdir(parents=True, exist_ok=False)  # separate derivative; never replace source or prior export
    bpp = 1 if pixel.bits == 8 else 2
    dtype = np.dtype("u1" if bpp == 1 else "<u2")
    line_bytes = samples * bands * bpp
    capture = {"format": "fx10-capture-v1", "samples": samples, "bands": bands, "bytes_per_pixel": bpp,
               "byte_order": "little", "pixel_format": pixel.name, "full_scale": pixel.full_scale,
               "expected_frame_rate_hz": profile.get("expected_frame_rate_hz", 0),
               "wavelengths_nm": wavelengths, "line_index_format": "fx10-line-index-v2",
               "wavelength_source": profile.get("wavelength_source", "unknown"), "source_profile": profile,
               "source_format": "ebus-raw-v1", "source_directory": str(source.resolve()),
               "recording_contract": {"output": "offline ENVI DN derivative", "sdk_payload_byte_reconstruction": False,
                                      "omitted_bytes": "non-image bytes and SDK image padding; original raw is retained"}}
    _write_json(output / "capture.json", capture)
    _write_json(output / "device.json", {"format": "fx10-export-device-v1", "source_device": device})
    bil_part = output / "segment_0001.bil.part"
    index_part = output / "segment_0001.lines.csv.part"
    count = total = rejected = 0
    with bil_part.open("xb") as bil, index_part.open("x", encoding="ascii", newline="") as index, (output / "source.records.jsonl.part").open("x", encoding="utf-8") as lineage:
        index.write("# fx10-line-index-v2; offline derivative; device timestamp unit/epoch unchanged\n")
        writer = csv.writer(index, lineterminator="\n")
        writer.writerow(_COLUMNS)
        for record in recording.session_records(source, require_complete=not allow_incomplete):
            total += 1
            meta, image = record.metadata, record.metadata["image"]
            row = ["frame" if record.valid_frame else "rejected", count, count, count * line_bytes, record.bid, 1,
                   record.dts, record.hrt, meta["host_monotonic_ns"], int(meta["block_id_anomaly"]),
                   meta["acquired_size"], meta["payload_type"], meta["operation_result"], meta["chunk_count"], int(image is not None)]
            row += [image.get(key, "") if image is not None else "" for key in
                    ("pixel_format", "width", "height", "padding_x", "padding_y", "image_size", "effective_image_size")]
            if meta["gap_before"]:
                gap = list(row)
                gap[0], gap[5] = "gap", meta["gap_before"]
                writer.writerow(gap)
            lineage.write(json.dumps({"segment": record.path.name, "offset": record.offset, "sequence": record.sequence,
                                      "output_line": count if record.valid_frame else None, "metadata": meta}, separators=(",", ":")) + "\n")
            if record.valid_frame:
                array, name, _ = pixels.decode_record(record, recording.read_payload(record, verify_crc=True))
                if array.shape != (bands, samples) or name != pixel.name:
                    raise ValueError("Raw image geometry/PixelFormat changed within the FX10 session")
                bil.write(array.astype(dtype, copy=False).tobytes())
                count += 1
            else:
                recording.verify_payload(record)
                rejected += 1
            writer.writerow(row)
        if not count:
            raise ValueError("No valid image frames to export")
        for stream in (bil, index, lineage):
            stream.flush()
            os.fsync(stream.fileno())
    _publish(bil_part, output / "segment_0001.bil")
    _publish(index_part, output / "segment_0001.lines.csv")
    _publish(output / "source.records.jsonl.part", output / "source.records.jsonl")
    fields = ["ENVI", "description = {Offline ebus-raw DN derivative; original raw and source.records.jsonl retain provenance}",
              f"samples = {samples}", f"lines = {count}", f"bands = {bands}", "header offset = 0", "file type = ENVI Standard",
              f"data type = {1 if bpp == 1 else 12}", "interleave = bil", "byte order = 0"]
    if wavelengths:
        fields += ["wavelength units = Nanometers", "wavelength = {" + ", ".join(str(value) for value in wavelengths) + "}"]
    if fwhm:
        fields += ["fwhm = {" + ", ".join(str(value) for value in fwhm) + "}"]
    header_part = output / "segment_0001.hdr.part"
    with header_part.open("x", encoding="utf-8") as header:
        header.write("\n".join(fields) + "\n")
        header.flush()
        os.fsync(header.fileno())
    _write_json(output / "export.json", {"format": "ebus-envi-export-v1", "source": str(source.resolve()),
                                        "source_complete_required": not allow_incomplete, "records_read": total,
                                        "frames_exported": count, "invalid_records_omitted": rejected,
                                        "payload_crc_verified": True})
    _publish(header_part, output / "segment_0001.hdr")  # publish completion last
    return count


def main(argv=None):
    parser = argparse.ArgumentParser(description="Offline FX10 eBUS raw to ENVI BIL; preserves source files")
    parser.add_argument("source", type=Path, help="One FX10 camera recording directory")
    parser.add_argument("output", type=Path, help="A new derivative directory, must not exist")
    parser.add_argument("--allow-incomplete", action="store_true", help="Explicitly export a complete raw prefix without a successful session_end")
    args = parser.parse_args(argv)
    try:
        count = export_envi(args.source, args.output, allow_incomplete=args.allow_incomplete)
        print(f"Exported {count} DN scan lines to {args.output}; original raw unchanged")
        return 0
    except (OSError, ValueError, KeyError, TypeError, UnicodeError) as error:
        print(f"error: {error}; any incomplete derivative is retained for inspection", file=sys.stderr)
        return 1
