#!/usr/bin/env python3
"""Verify the C++/Python GUI-contract mirrors agree verbatim.

Compares every marker constant in common/include/driver_markers.h against its
counterpart in app/services/markers.py (whose import also runs the internal
consistency asserts). Run from anywhere:

    uv run python tools/check_contracts.py

Exit code 0 = PASS, 1 = mismatch or parse failure.
"""

from __future__ import annotations

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
    "kModuleGox": "MODULE_GOX",
    "kModuleIns401": "MODULE_INS401",
    "kModuleLms4xxx": "MODULE_LMS4XXX",
    "kAsterxInitialized": "ASTERX_INITIALIZED",
    "kAsterxShutdown": "ASTERX_SHUTDOWN",
    "kGoxInitialized": "GOX_INITIALIZED",
    "kGoxShutdown": "GOX_SHUTDOWN",
    "kGoxSessionIssues": "GOX_SESSION_ISSUES",
    "kIns401Initialized": "INS401_INITIALIZED",
    "kIns401Shutdown": "INS401_SHUTDOWN",
    "kLmsInitializedTpl": "LMS_INITIALIZED_TPL",
    "kLmsShutdownTpl": "LMS_SHUTDOWN_TPL",
    "kStartingDrivers": "STARTING_DRIVERS",
    "kReceivedSignalTpl": "RECEIVED_SIGNAL_TPL",
    "kAllDriversShutDown": "ALL_DRIVERS_SHUT_DOWN",
    "kAsterxInitFailed": "ASTERX_INIT_FAILED",
    "kGoxInitFailed": "GOX_INIT_FAILED",
    "kIns401InitFailed": "INS401_INIT_FAILED",
    "kLms4xxxInitFailed": "LMS4XXX_INIT_FAILED",
    "kAsterxRunException": "ASTERX_RUN_EXCEPTION",
    "kGoxRunException": "GOX_RUN_EXCEPTION",
    "kIns401RunException": "INS401_RUN_EXCEPTION",
    "kLms4xxxRunException": "LMS4XXX_RUN_EXCEPTION",
}

CPP_CONST_RE = re.compile(r'constexpr\s+std::string_view\s+(k\w+)\s*=\s*"((?:[^"\\]|\\.)*)"')


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

    if failures:
        print(f"FAIL: {len(failures)} contract mismatch(es):")
        for f in failures:
            print(f"  - {f}")
        return 1

    print(f"PASS: {len(PAIRS)} marker constants agree verbatim "
          f"({CPP_HEADER.relative_to(REPO_ROOT)} <-> {PY_MIRROR.relative_to(REPO_ROOT)})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
