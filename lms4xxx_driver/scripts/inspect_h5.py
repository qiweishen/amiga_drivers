#!/usr/bin/env python3
"""Inspect, verify and export "lms4xxx-h5" recordings (scan_<id>_<ts>_NNN.h5).

The layout is documented in lms4xxx_driver/docs/FORMAT_H5.md; this script
reads it through h5py only, exactly as any third party would.

Subcommands:
    info      attributes, datasets and shapes of one file (or every split of a run)
    verify    integrity check: matching row counts, counter continuity, padding,
              monotonic device time, channel/attribute consistency
    csv       per-point CSV export with physical units (the successor of the
              retired DataConverter)

Examples:
    inspect_h5.py info  recordings/20260806_070253/raw/lms4xxx/scan_Front_Right_Laser_20260806_070253_000.h5
    inspect_h5.py verify recordings/20260806_070253/raw/lms4xxx/*.h5
    inspect_h5.py csv   scan_x_000.h5 --out scan_x_000.csv --max-scans 1000

Exit codes: 0 ok, 1 verification errors, 2 usage / unreadable file.
Requires: h5py >= 3, numpy (pip install h5py numpy).
"""

from __future__ import annotations

import argparse
import csv
import sys
from pathlib import Path

try:
    import h5py
    import numpy as np
except ImportError as exc:  # pragma: no cover - environment guard
    sys.exit(f"{exc}: install with 'pip install h5py numpy'")

FORMAT = "lms4xxx-h5"
# Bumped whenever the layout changes; 3 added /telemetry, the device-audit
# root attributes and the closed_cleanly/frames_total completeness marker.
FORMAT_VERSION = 3
CHANNELS = ("dist", "rssi", "refl", "angl", "qlty")


# ----------------------------------------------------------------------------------------------------
# helpers
# ----------------------------------------------------------------------------------------------------


def _attr(obj, name, default=None):
    value = obj.attrs.get(name, default)
    if isinstance(value, bytes):
        value = value.decode("utf-8", "replace")
    return value


def _open(path: Path) -> h5py.File:
    try:
        f = h5py.File(path, "r")
    except OSError as exc:
        sys.exit(f"{path}: cannot open as HDF5 ({exc})")
    if _attr(f, "format") != FORMAT:
        f.close()
        sys.exit(f"{path}: not an {FORMAT} file (format attribute = {_attr(f, 'format')!r})")
    return f


def _scan_count(f: h5py.File) -> int:
    return int(f["frames/scan_counter"].shape[0])


# ----------------------------------------------------------------------------------------------------
# info
# ----------------------------------------------------------------------------------------------------


def cmd_info(args: argparse.Namespace) -> int:
    for path in args.files:
        with _open(path) as f:
            n = _scan_count(f)
            print(f"== {path}")
            print(f"   scans: {n}   channels: {_attr(f, 'channels')}   compression: {_attr(f, 'compression')}"
                  f"   swmr: {int(_attr(f, 'swmr', 0))}")
            for key in ("instance_id", "session_timestamp", "split_index", "format_version",
                        "hdf5_library_version", "config_start_angle_deg", "config_stop_angle_deg",
                        "config_angle_step_deg", "config_output_rate", "config_remission", "points_per_scan",
                        "device_firmware", "device_order_number", "device_type", "device_name",
                        "device_keeps_flagged_points"):
                if key in f.attrs:
                    print(f"   {key:24s} {_attr(f, key)}")
            if n:
                up = f["frames/time_since_startup_us"][:].astype(np.int64)
                dur = int((up[-1] - up[0]) % (1 << 32)) / 1e6
                rate = (n - 1) / dur if dur > 0 else 0.0
                print(f"   device uptime span     {dur:.3f} s  (~{rate:.1f} scans/s)")
                dev = f["frames/device_time_unix_us"]
                print(f"   device time (first)    {int(dev[0])} us" + ("" if int(dev[0]) else "  (no telegram timestamp)"))
                sn = f["frames/serial_number"]
                print(f"   serial_number          {int(sn[0])}")
            print("   datasets:")

            def show(name, obj):
                if isinstance(obj, h5py.Dataset):
                    extra = ""
                    if "unit" in obj.attrs:
                        extra = f"  [{_attr(obj, 'unit')}]"
                    if "scale_factor" in obj.attrs:
                        extra += f"  scale {float(obj.attrs['scale_factor']):g} offset {float(obj.attrs['scale_offset']):g}"
                    print(f"     /{name:28s} {str(obj.dtype):8s} {obj.shape}{extra}")

            f.visititems(show)
            if args.config and "config_yaml" in f.attrs:
                print("   config_yaml:")
                for line in str(_attr(f, "config_yaml")).splitlines():
                    print("     | " + line)
    return 0


# ----------------------------------------------------------------------------------------------------
# verify
# ----------------------------------------------------------------------------------------------------


def _verify_file(path: Path, prev_counter: int | None, expect_split: int | None) -> tuple[list[str], int | None]:
    errors: list[str] = []
    with _open(path) as f:
        n = _scan_count(f)
        split = int(_attr(f, "split_index", -1))
        if expect_split is not None and split != expect_split:
            errors.append(f"split_index {split}, expected {expect_split} (missing split file?)")

        # A reader written for v3 must not silently mis-read a future layout.
        version = int(_attr(f, "format_version", 0))
        if version != FORMAT_VERSION:
            errors.append(f"format_version {version}, this reader understands {FORMAT_VERSION}")

        # HDF5 has no rename-on-finish, so the marker is the only way to tell a
        # complete recording from one whose writer was killed (FORMAT_H5.md).
        # SWMR files never carry it: attributes cannot be added after the layout
        # is frozen.
        if not int(_attr(f, "swmr", 0)):
            if "closed_cleanly" not in f.attrs:
                errors.append("no closed_cleanly attribute: the file was not closed (truncated recording)")
            elif int(f.attrs["closed_cleanly"]) != 1:
                errors.append("closed_cleanly is not 1: the writer reported an incomplete recording")
            else:
                total = int(_attr(f, "frames_total", -1))
                if total >= 0 and total != n:
                    errors.append(f"frames_total {total} but {n} rows on disk")

        # every dataset shares the scan index
        for name, obj in f["frames"].items():
            if obj.shape[0] != n:
                errors.append(f"/frames/{name} has {obj.shape[0]} rows, expected {n}")
        listed = str(_attr(f, "channels", "")).split(",") if _attr(f, "channels", "") else []
        present = list(f["channels"].keys())
        if sorted(listed) != sorted(present):
            errors.append(f"channels attribute {listed} != datasets {present}")
        points = int(_attr(f, "points_per_scan", 841))
        for name, obj in f["channels"].items():
            if obj.shape != (n, points):
                errors.append(f"/channels/{name} shape {obj.shape}, expected {(n, points)}")
            for a in ("content", "scale_factor", "scale_offset", "unit", "description"):
                if a not in obj.attrs:
                    errors.append(f"/channels/{name} lacks the {a} attribute")

        if n == 0:
            return errors, prev_counter

        # counter continuity (16-bit wrap), also across split files
        tc = f["frames/telegram_counter"][:].astype(np.int64)
        seq = np.concatenate(([prev_counter], tc)) if prev_counter is not None else tc
        gaps = ((np.diff(seq) - 1) & 0xFFFF)
        n_gaps = int(np.count_nonzero(gaps))
        missed = int(gaps.sum())
        if n_gaps:
            errors.append(f"{n_gaps} telegram counter gap(s), ~{missed} scans missed (device/network loss, not file damage)")

        # device uptime monotonic (32-bit wrap allowed)
        up = f["frames/time_since_startup_us"][:].astype(np.int64)
        if np.any((np.diff(up) % (1 << 32)) >= (1 << 31)):
            errors.append("time_since_startup_us goes backwards")
        # Only the rows that HAVE a device time: the timestamp block is absent
        # while NTP is off, and mixing the zeros in made every such file look
        # non-monotonic.
        dev_all = f["frames/device_time_unix_us"][:]
        dev = dev_all[dev_all != 0]
        if dev.size > 1 and np.any(np.diff(dev) < 0):
            errors.append("device_time_unix_us is not monotonic")

        # padding beyond num_points must be zero (sampled)
        num_points = f["frames/num_points"][:]
        short = np.nonzero(num_points < points)[0]
        for idx in short[:50]:
            for name, obj in f["channels"].items():
                row = obj[int(idx)]
                if np.any(row[int(num_points[idx]):]):
                    errors.append(f"/channels/{name} row {idx}: non-zero padding beyond num_points={num_points[idx]}")
                    break

        # dist raw 0..15 are non-measurements: report the share, not an error
        if "dist" in f["channels"]:
            sample = f["channels/dist"][: min(n, 2000)]
            invalid = float(np.mean(sample < 16)) * 100.0
            print(f"   {path.name}: {n} scans, {invalid:.2f}% invalid distance samples in the first {sample.shape[0]} scans")
        return errors, int(tc[-1])


def cmd_verify(args: argparse.Namespace) -> int:
    files = sorted(args.files)
    prev_counter: int | None = None
    # Anchor the chain at 0: without it a run whose _000 file is missing
    # verifies clean, because every later index still follows its predecessor.
    expect_split: int | None = 0 if args.chain else None
    failed = False
    for path in files:
        errors, prev_counter = _verify_file(path, prev_counter if args.chain else None, expect_split)
        expect_split = expect_split + 1 if expect_split is not None else None
        if errors:
            failed = True
            print(f"FAIL {path}")
            for e in errors:
                print(f"   - {e}")
        else:
            print(f"OK   {path}")
    return 1 if failed else 0


# ----------------------------------------------------------------------------------------------------
# csv
# ----------------------------------------------------------------------------------------------------


def cmd_csv(args: argparse.Namespace) -> int:
    path = args.file
    out = args.out or path.with_suffix(".csv")
    with _open(path) as f, open(out, "w", newline="") as fh:
        n = _scan_count(f)
        if args.max_scans:
            n = min(n, args.max_scans)
        channels = {c: f["channels"][c] for c in CHANNELS if c in f["channels"]}
        scale = {c: (float(d.attrs["scale_factor"]), float(d.attrs["scale_offset"])) for c, d in channels.items()}
        fr = f["frames"]
        w = csv.writer(fh)
        header = ["scan_index", "device_time_unix_us", "time_since_startup_us",
                  "telegram_counter", "scan_counter", "scan_freq_hz", "point_index", "angle_deg"]
        cols = [c for c in ("dist", "rssi", "refl", "angl", "qlty") if c in channels]
        names = {"dist": "distance_mm", "rssi": "rssi", "refl": "reflectance_pct",
                 "angl": "angle_correction_deg", "qlty": "quality"}
        w.writerow(header + [names[c] for c in cols])

        step = 256
        for s0 in range(0, n, step):
            s1 = min(n, s0 + step)
            meta = {k: fr[k][s0:s1] for k in ("device_time_unix_us", "time_since_startup_us",
                                              "telegram_counter", "scan_counter", "scan_frequency",
                                              "num_points", "start_angle", "angle_step")}
            data = {c: channels[c][s0:s1] for c in cols}
            for i in range(s1 - s0):
                npnt = int(meta["num_points"][i])
                angles = (int(meta["start_angle"][i]) + np.arange(npnt) * int(meta["angle_step"][i])) / 1e4
                base = [s0 + i, int(meta["device_time_unix_us"][i]),
                        int(meta["time_since_startup_us"][i]), int(meta["telegram_counter"][i]),
                        int(meta["scan_counter"][i]), int(meta["scan_frequency"][i]) / 100.0]
                for p in range(npnt):
                    row = base + [p, f"{angles[p]:.4f}"]
                    for c in cols:
                        raw = int(data[c][i, p])
                        if c == "qlty":
                            row.append(f"0x{raw:02X}")
                        elif c == "angl":
                            row.append(f"{(raw * scale[c][0] + scale[c][1]) / 1e4:.4f}")
                        else:
                            row.append(f"{raw * scale[c][0] + scale[c][1]:g}")
                    w.writerow(row)
    print(f"wrote {out} ({n} scans)")
    return 0


# ----------------------------------------------------------------------------------------------------


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("info", help="print attributes, datasets and shapes")
    p.add_argument("files", nargs="+", type=Path)
    p.add_argument("--config", action="store_true", help="also print the embedded config_yaml")
    p.set_defaults(func=cmd_info)

    p = sub.add_parser("verify", help="integrity check")
    p.add_argument("files", nargs="+", type=Path)
    p.add_argument("--no-chain", dest="chain", action="store_false",
                   help="do not check counter/split continuity across the given files")
    p.set_defaults(func=cmd_verify)

    p = sub.add_parser("csv", help="per-point CSV export with physical units")
    p.add_argument("file", type=Path)
    p.add_argument("--out", type=Path)
    p.add_argument("--max-scans", type=int, default=0)
    p.set_defaults(func=cmd_csv)

    args = ap.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
