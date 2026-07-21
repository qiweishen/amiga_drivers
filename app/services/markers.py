"""GUI contract — single source of truth for the C++ lifecycle marker strings.

Mirrors common/include/driver_markers.h VERBATIM; the two files must agree.
Run `uv run python tools/check_contracts.py` after editing either side.
Values here are matched against the unified log by health.py (per-sensor state
machine), session_tailer.py (replay filter) and process.py (clean-exit detection
via ALL_DRIVERS_SHUT_DOWN) — do not reword without updating the C++ side.
"""

from __future__ import annotations

import re

# Module tokens: the "[Module]:" tag of every log line. Health transitions are
# keyed on these App-level tokens only; per-driver internal modules (e.g.
# "GoX", "INS Receiver") intentionally do not drive the state machine.
MODULE_MAIN = "Main"
MODULE_ASTERX = "AsterxApp"
MODULE_GOX = "GoXApp"
MODULE_INS401 = "INS401App"
MODULE_LMS4XXX = "LMS4xxxApp"

# Driver lifecycle markers (verbatim, matched by substring)
ASTERX_INITIALIZED = "AsteRx driver initialized (receiver configured, recording)"
ASTERX_SHUTDOWN = "AsteRx driver shutdown completely"
GOX_INITIALIZED = "GoX driver initialized"
GOX_SHUTDOWN = "GoX driver shutdown completely"
GOX_SESSION_ISSUES = "GoX session ended with issues"  # full line appends " (standalone exit code N)"
INS401_INITIALIZED = "INS401 driver initialized"
INS401_SHUTDOWN = "INS401 driver shutdown completely"

# fmt templates ({} = LiDAR instance name)
LMS_INITIALIZED_TPL = "LiDAR instance [{}] initialized successfully"
LMS_SHUTDOWN_TPL = "LiDAR instance [{}] driver shutdown completely"

# Unified-main markers. ALL_DRIVERS_SHUT_DOWN is the ONLY evidence of a clean
# exit (reattach after a GUI restart depends on it).
STARTING_DRIVERS = "Starting Amiga Drivers"
RECEIVED_SIGNAL_TPL = "Received signal {}, shutting down all drivers..."
ALL_DRIVERS_SHUT_DOWN = "All drivers shut down"

# Per-driver failure markers emitted by [Main]
ASTERX_INIT_FAILED = "AsteRx driver initialization failed"
GOX_INIT_FAILED = "GoX driver initialization failed"
INS401_INIT_FAILED = "INS401 driver initialization failed"
LMS4XXX_INIT_FAILED = "LMS4xxx driver initialization failed"
ASTERX_RUN_EXCEPTION = "AsteRx run() exception"
GOX_RUN_EXCEPTION = "GoX run() exception"
INS401_RUN_EXCEPTION = "INS401 run() exception"
LMS4XXX_RUN_EXCEPTION = "LMS4xxx run() exception"

# ---------------------------------------------------------------------------
# Derived values. Matching is calibrated to what the C++ contract actually
# emits (not to arbitrary historical formats). The two suffix literals below
# are bound to the PAIRS-checked full failure markers by the asserts at the
# bottom of this file (and by common/tests/test_logger.cpp on the C++ side).

# Failure-kind substrings ("<Name>" + suffix == the full failure markers)
INIT_FAILED_SUFFIX = " driver initialization failed"
RUN_EXCEPTION_SUFFIX = " run() exception"

# Driver display names: the leading token of the [Main] failure markers,
# mapped to the GUI sensor key prefix (health.py fans "lms" out per instance).
DRIVER_NAME_TO_SENSOR_KEY = {
    ASTERX_INIT_FAILED.removesuffix(INIT_FAILED_SUFFIX): "asterx",
    GOX_INIT_FAILED.removesuffix(INIT_FAILED_SUFFIX): "gox",
    INS401_INIT_FAILED.removesuffix(INIT_FAILED_SUFFIX): "ins401",
    LMS4XXX_INIT_FAILED.removesuffix(INIT_FAILED_SUFFIX): "lms",
}

# "Received signal" prefix (the C++ side formats the signal number in)
RECEIVED_SIGNAL_PREFIX = RECEIVED_SIGNAL_TPL.split("{}")[0].rstrip()


def template_re(tpl: str) -> re.Pattern[str]:
    """Compile an fmt "{}" template into a regex capturing the placeholder."""
    head, tail = tpl.split("{}")
    return re.compile(re.escape(head) + r"(.+?)" + re.escape(tail))


LMS_INIT_RE = template_re(LMS_INITIALIZED_TPL)
LMS_STOP_RE = template_re(LMS_SHUTDOWN_TPL)

# Substrings that make a line load-bearing for HealthMonitor during replay
# (session_tailer.py scans the whole file for these only).
REPLAY_MARKER_SUBSTRINGS = (
    "driver initialized",
    "initialized successfully",
    "shutdown completely",
    "initialization failed",
    "run() exception",
    ALL_DRIVERS_SHUT_DOWN,
    RECEIVED_SIGNAL_PREFIX,
    STARTING_DRIVERS,
)

# Internal consistency: the composed failure markers must factor cleanly into
# name + suffix, and the templates must contain exactly one placeholder.
for _full, _suffix in (
    (ASTERX_INIT_FAILED, INIT_FAILED_SUFFIX),
    (GOX_INIT_FAILED, INIT_FAILED_SUFFIX),
    (INS401_INIT_FAILED, INIT_FAILED_SUFFIX),
    (LMS4XXX_INIT_FAILED, INIT_FAILED_SUFFIX),
    (ASTERX_RUN_EXCEPTION, RUN_EXCEPTION_SUFFIX),
    (GOX_RUN_EXCEPTION, RUN_EXCEPTION_SUFFIX),
    (INS401_RUN_EXCEPTION, RUN_EXCEPTION_SUFFIX),
    (LMS4XXX_RUN_EXCEPTION, RUN_EXCEPTION_SUFFIX),
):
    assert _full.endswith(_suffix), f"marker {_full!r} does not end with {_suffix!r}"
for _tpl in (LMS_INITIALIZED_TPL, LMS_SHUTDOWN_TPL, RECEIVED_SIGNAL_TPL):
    assert _tpl.count("{}") == 1, f"template {_tpl!r} must have exactly one placeholder"
