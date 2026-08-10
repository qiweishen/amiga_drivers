#!/usr/bin/env python3
"""Verify the C++/Python GUI-contract mirrors agree verbatim.

Two contracts are checked:

1. Markers — every constant in common/include/driver_markers.h against its
   counterpart in app/services/markers.py (whose import also runs the internal
   consistency asserts).
2. [Statistics] write rate — the "fps=" field every frame-based driver prints
   in its periodic status line, which app/services/driver_stats.py parses to
   drive the dashboard cards.

Run from anywhere:

    uv run python tools/check_contracts.py

Exit code 0 = PASS, 1 = mismatch or parse failure.
"""

from __future__ import annotations

import importlib
import importlib.util
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
CPP_HEADER = REPO_ROOT / "common" / "include" / "driver_markers.h"
PY_MIRROR = REPO_ROOT / "app" / "services" / "markers.py"

# C++ constant name -> Python constant name
PAIRS = {
    "kModuleMain": "MODULE_MAIN",
    "kModuleAsterx": "MODULE_ASTERX",
    "kModuleFx10": "MODULE_FX10",
    "kModuleGox": "MODULE_GOX",
    "kModuleLms4xxx": "MODULE_LMS4XXX",
    "kAsterxInitialized": "ASTERX_INITIALIZED",
    "kAsterxShutdown": "ASTERX_SHUTDOWN",
    "kAsterxSessionIssues": "ASTERX_SESSION_ISSUES",
    "kFx10Initialized": "FX10_INITIALIZED",
    "kFx10Shutdown": "FX10_SHUTDOWN",
    "kFx10SessionIssues": "FX10_SESSION_ISSUES",
    "kGoxInitialized": "GOX_INITIALIZED",
    "kGoxShutdown": "GOX_SHUTDOWN",
    "kGoxSessionIssues": "GOX_SESSION_ISSUES",
    "kLmsInitialized": "LMS_INITIALIZED",
    "kLmsShutdown": "LMS_SHUTDOWN",
    "kGoxInitializedInstTpl": "GOX_INITIALIZED_INST_TPL",
    "kGoxShutdownInstTpl": "GOX_SHUTDOWN_INST_TPL",
    "kLmsInitializedInstTpl": "LMS_INITIALIZED_TPL",
    "kLmsShutdownInstTpl": "LMS_SHUTDOWN_TPL",
    "kStartingDrivers": "STARTING_DRIVERS",
    "kReceivedSignalTpl": "RECEIVED_SIGNAL_TPL",
    "kAllDriversShutDown": "ALL_DRIVERS_SHUT_DOWN",
    "kAsterxInitFailed": "ASTERX_INIT_FAILED",
    "kFx10InitFailed": "FX10_INIT_FAILED",
    "kGoxInitFailed": "GOX_INIT_FAILED",
    "kLms4xxxInitFailed": "LMS4XXX_INIT_FAILED",
    "kAsterxRunException": "ASTERX_RUN_EXCEPTION",
    "kFx10RunException": "FX10_RUN_EXCEPTION",
    "kGoxRunException": "GOX_RUN_EXCEPTION",
    "kLms4xxxRunException": "LMS4XXX_RUN_EXCEPTION",
}

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
        "format_file": "fx10_driver/src/fx10_driver_app.cpp",
        "literal": "fps={:.1f}",
        "emit_file": "fx10_driver/src/fx10_driver_app.cpp",
        "sample": "[Statistics] frames=1200  rate=50.0 Hz  fps=49.8  missed_triggers=0  "
                  "temp=41.2000 °C  disk_free=812.0 GB",
        "instance": None,  # fx10 is single-instance: no [tag] prefix
        "fps": 49.8,
    },
    "lms": {
        "format_file": "lms4xxx_driver/src/lms4xxx_driver_app.cpp",
        "literal": "fps={:.1f}",
        "emit_file": "lms4xxx_driver/src/lms4xxx_driver_app.cpp",
        "sample": "[Statistics] [Front_Right_Laser] up=00:00:10  rate=600.0 Hz  fps=598.0  ntp=ok  frames=6000",
        "instance": "Front_Right_Laser",
        "fps": 598.0,
    },
}

# Common::DriverLog g_log{"GoX"} | g_log{std::string(Common::Markers::kModuleFx10)}
GLOG_RE = re.compile(
    r'Common::DriverLog\s+g_log\s*\{\s*(?:std::string\s*\(\s*)?(?:Common::Markers::)?(k\w+|"[^"]*")'
)


def load_cpp_constants() -> dict[str, str]:
    text = CPP_HEADER.read_text(encoding="utf-8")
    consts = {name: value for name, value in CPP_CONST_RE.findall(text)}
    for value in consts.values():
        if "\\" in value:
            sys.exit(f"FAIL: escape sequences in C++ marker values are not supported: {value!r}")
    return consts


def load_py_mirror():
    spec = importlib.util.spec_from_file_location("amiga_markers", PY_MIRROR)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)  # import-time asserts run here
    return module


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
        if not fmt_path.is_file():
            failures.append(f"{spec['format_file']}: not found")
            continue
        if spec["literal"] not in fmt_path.read_text(encoding="utf-8"):
            failures.append(
                f"{spec['format_file']}: the periodic [Statistics] format no longer contains "
                f"{spec['literal']!r} — the GUI cards read the write rate from it"
            )

        # The module token routes the line to a sensor in the GUI.
        m = GLOG_RE.search(emit_path.read_text(encoding="utf-8")) if emit_path.is_file() else None
        if m is None:
            failures.append(f"{spec['emit_file']}: no 'Common::DriverLog g_log{{...}}' declaration found")
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
    cpp = load_cpp_constants()
    py = load_py_mirror()
    failures: list[str] = []

    unknown = set(cpp) - set(PAIRS)
    if unknown:
        failures.append(f"C++ constants missing from PAIRS (update this script): {sorted(unknown)}")

    for cpp_name, py_name in PAIRS.items():
        cpp_value = cpp.get(cpp_name)
        py_value = getattr(py, py_name, None)
        if cpp_value is None:
            failures.append(f"{cpp_name}: not found in {CPP_HEADER.name}")
        elif py_value is None:
            failures.append(f"{py_name}: not found in {PY_MIRROR.name}")
        elif cpp_value != py_value:
            failures.append(f"{cpp_name} != {py_name}:\n    C++: {cpp_value!r}\n    Py:  {py_value!r}")

    failures.extend(check_stats_contract(cpp))

    if failures:
        print(f"FAIL: {len(failures)} contract mismatch(es):")
        for f in failures:
            print(f"  - {f}")
        return 1

    print(f"PASS: {len(PAIRS)} marker constants agree verbatim "
          f"({CPP_HEADER.relative_to(REPO_ROOT)} <-> {PY_MIRROR.relative_to(REPO_ROOT)})")
    print(f"PASS: {len(STATS_CONTRACT)} drivers publish the [Statistics] 'fps=' write rate the GUI cards read "
          f"({PY_STATS_PARSER.relative_to(REPO_ROOT)})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
