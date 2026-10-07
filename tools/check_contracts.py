#!/usr/bin/env python3
"""Check the remaining C++/GUI statistics-display contract.

Lifecycle and final results come only from amiga-run-v1; human-readable log
messages are not a control protocol. This check covers fps fields, module routing
and multi-instance tags used by app.services.driver_stats.
"""

from __future__ import annotations

import importlib
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
CPP_HEADER = REPO_ROOT / "common" / "include" / "driver_markers.h"

CPP_CONST_RE = re.compile(r'constexpr\s+std::string_view\s+(k\w+)\s*=\s*"((?:[^"\\]|\\.)*)"')

# --- [Statistics] "fps=" contract --------------------------------------------
# The dashboard cards show frames-actually-written-per-second, taken verbatim
# from each driver's periodic [Statistics] line. Three things must hold, and
# none of them is caught by a compiler: the field literal in the format string,
# the log module token the line carries (that is how the GUI routes it to a
# sensor), and the GUI's own parser regexes.
PY_STATS_PARSER = REPO_ROOT / "app" / "services" / "driver_stats.py"

STATS_CONTRACT = {
    # driver key in driver_stats._MODULE_TO_DRIVER -> where the line is built /
    # emitted, the expected fps literal, and a representative rendered line
    "gox": {
        "format_file": "gox_driver/src/stats.cpp",
        "literal": "fps=%.1f",
        "emit_file": "gox_driver/src/capture_runner.cpp",
        "sample": "[Statistics] [cam0] up=00:01:05  rate=24.1 Hz  fps=24.0  disk=119.8 MB/s  ok=1560",
        "instance": "cam0",
        "fps": 24.0,
    },
    "fx10": {
        # fx10 renders in stats_line.cpp and emits from the acquisition session
        "format_file": "fx10_driver/src/stats_line.cpp",
        "literal": "fps={:.1f}",
        "emit_file": "fx10_driver/src/session_monitor.cpp",
        "logger_file": "fx10_driver/src/session.cpp",
        "sample": "[Statistics] frames=1200  rate=50.0 Hz  fps=49.8  missed_triggers=0  "
                  "temp_pcb=41.2  temp_fpga=52.7",
        "instance": None,  # fx10 is single-instance: no [tag] prefix
        "fps": 49.8,
    },
    "lms": {
        "format_file": "lms4xxx_driver/src/lms4xxx_driver_app.cpp",
        "literal": "fps={:.1f}",
        "emit_file": "lms4xxx_driver/src/lms4xxx_driver_app.cpp",
        "sample": "[Statistics] [Front_Right_Laser] up=00:00:10  rate=600.0 Hz  fps=598.0  ntp=OK  frames=6000  "
                  "parsed=6000  drop_ring=0  gaps=0  crc=0  frame_err=0  parse_err=0  written=5980  drop_q=0  "
                  "files=1  bytes=34.20 MiB  queued=600.0  temp=41.2  unexpected=0",
        "instance": "Front_Right_Laser",
        "fps": 598.0,
    },
}

# common::DriverLog g_log{"GoX"} | g_log{std::string(common::Markers::kModuleFx10)}
GLOG_RE = re.compile(
    r'common::DriverLog\s+g_log\s*\{\s*(?:std::string\s*\(\s*)?(?:common::Markers::)?(k\w+|"[^"]*")'
)


def load_cpp_constants() -> dict[str, str]:
    text = CPP_HEADER.read_text(encoding="utf-8")
    consts = {name: value for name, value in CPP_CONST_RE.findall(text)}
    for value in consts.values():
        if "\\" in value:
            sys.exit(f"FAIL: escape sequences in C++ marker values are not supported: {value!r}")
    return consts


def load_stats_parser():
    """Import app.services.driver_stats (implicit namespace package, stdlib-only
    dependencies — no nicegui is pulled in)."""
    if str(REPO_ROOT) not in sys.path:
        sys.path.insert(0, str(REPO_ROOT))
    return importlib.import_module("app.services.driver_stats")


def check_stats_contract(cpp: dict[str, str]) -> list[str]:
    failures: list[str] = []
    try:
        ds = load_stats_parser()
    except Exception as e:  # noqa: BLE001 — reported, not raised
        return [f"cannot import {PY_STATS_PARSER.relative_to(REPO_ROOT)}: {e}"]

    for driver, spec in STATS_CONTRACT.items():
        fmt_path = REPO_ROOT / spec["format_file"]
        emit_path = REPO_ROOT / spec["emit_file"]
        logger_path = REPO_ROOT / spec.get("logger_file", spec["emit_file"])
        if not emit_path.is_file():
            failures.append(f"{spec['emit_file']}: not found")
        if not fmt_path.is_file():
            failures.append(f"{spec['format_file']}: not found")
            continue
        if spec["literal"] not in fmt_path.read_text(encoding="utf-8"):
            failures.append(
                f"{spec['format_file']}: the periodic [Statistics] format no longer contains "
                f"{spec['literal']!r} — the GUI cards read the write rate from it"
            )

        # The module token routes the line to a sensor in the GUI.
        m = GLOG_RE.search(logger_path.read_text(encoding="utf-8")) if logger_path.is_file() else None
        if m is None:
            failures.append(f"{logger_path.relative_to(REPO_ROOT)}: no 'common::DriverLog g_log{{...}}' declaration found")
        else:
            token = m.group(1)
            module = cpp.get(token) if token.startswith("k") else token.strip('"')
            if module is None:
                failures.append(f"{spec['emit_file']}: g_log module constant {token} not in {CPP_HEADER.name}")
            elif ds._MODULE_TO_DRIVER.get(module) != driver:
                failures.append(
                    f"{spec['emit_file']}: emits [Statistics] as module {module!r}, but "
                    f"driver_stats._MODULE_TO_DRIVER maps it to {ds._MODULE_TO_DRIVER.get(module)!r} "
                    f"(expected {driver!r})"
                )

        # The parser must actually read a rendered line of this driver.
        fps = ds._FPS_RE.search(spec["sample"])
        if fps is None or float(fps.group(1)) != spec["fps"]:
            failures.append(f"{driver}: driver_stats._FPS_RE does not read the write rate from a {driver} line")
        inst = ds._INSTANCE_RE.match(spec["sample"])
        got = inst.group(1) if inst else None
        if got != spec["instance"]:
            failures.append(
                f"{driver}: driver_stats._INSTANCE_RE read instance {got!r}, expected {spec['instance']!r}"
            )
    return failures


def main() -> int:
    failures = check_stats_contract(load_cpp_constants())
    if failures:
        print(f"FAIL: {len(failures)} statistics contract mismatch(es):")
        for failure in failures:
            print(f"  - {failure}")
        return 1
    print(f"PASS: {len(STATS_CONTRACT)} drivers publish the statistics display contract")
    return 0


if __name__ == "__main__":
    sys.exit(main())
