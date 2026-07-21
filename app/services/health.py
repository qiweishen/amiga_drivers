"""Per-sensor health state machine driven by the unified log's marker lines.

The marker strings live in markers.py — the verbatim mirror of the C++ side's
common/include/driver_markers.h (run tools/check_contracts.py after editing
either). This module only implements the transition table.
"""

from __future__ import annotations

import re

from ..state import STATE, SensorState, SensorStatus
from . import markers
from .log_buffer import LogLine

LMS_INIT_RE = markers.LMS_INIT_RE
LMS_STOP_RE = markers.LMS_STOP_RE
LMS_MSG_INSTANCE_RE = re.compile(r"^\[(.+?)\] ")

# [Main] failure text -> sensor key prefix
MAIN_FAIL_MAP = markers.DRIVER_NAME_TO_SENSOR_KEY


class HealthMonitor:
    def __init__(self) -> None:
        self.clean_stop = False
        self.stopping = False

    # -- lifecycle -----------------------------------------------------------

    def reset(self, enables: dict[str, bool], lms_names: list[str]) -> None:
        """Called on every start/reattach with the Enable flags in effect."""
        self.clean_stop = False
        self.stopping = False
        sensors: dict[str, SensorStatus] = {}
        sensors["ins401"] = SensorStatus("ins401", "INS401")
        for name in lms_names or ["?"]:
            sensors[f"lms:{name}"] = SensorStatus(f"lms:{name}", f"LMS4xxx · {name}")
        sensors["gox"] = SensorStatus("gox", "GoX Cameras")
        sensors["asterx"] = SensorStatus("asterx", "AsteRx")
        for key, st in sensors.items():
            driver = "lms4xxx" if key.startswith("lms:") else key
            st.state = SensorState.WAITING if enables.get(driver, False) else SensorState.DISABLED
        STATE.sensors = sensors

    # -- helpers -------------------------------------------------------------

    def _set(self, key: str, state: SensorState, error: str = "") -> None:
        st = STATE.sensors.get(key)
        if st is None:  # dynamically discovered LMS instance
            st = SensorStatus(key, key)
            STATE.sensors[key] = st
        if st.state is SensorState.DISABLED:
            return  # a disabled sensor never transitions
        st.state = state
        if error:
            st.last_error = error

    def _set_lms_all(self, state: SensorState, error: str = "") -> None:
        for key in STATE.sensors:
            if key.startswith("lms:"):
                self._set(key, state, error)

    # -- the transition table ------------------------------------------------

    def on_line(self, line: LogLine) -> None:
        msg, module = line.msg, line.module

        if module == markers.MODULE_INS401:
            if markers.INS401_INITIALIZED in msg:
                self._set("ins401", SensorState.RUNNING)
            elif markers.INS401_SHUTDOWN in msg:
                self._set("ins401", SensorState.STOPPED)
            elif line.level in ("error", "critical"):
                self._set("ins401", SensorState.FAILED, msg)

        elif module == markers.MODULE_LMS4XXX:
            if m := LMS_INIT_RE.search(msg):
                self._set(f"lms:{m.group(1)}", SensorState.RUNNING)
            elif m := LMS_STOP_RE.search(msg):
                self._set(f"lms:{m.group(1)}", SensorState.STOPPED)
            elif line.level in ("error", "critical"):
                m = LMS_MSG_INSTANCE_RE.match(msg)
                if m and f"lms:{m.group(1)}" in STATE.sensors:
                    self._set(f"lms:{m.group(1)}", SensorState.FAILED, msg)
                else:
                    self._set_lms_all(SensorState.FAILED, msg)

        elif module == markers.MODULE_GOX:
            if markers.GOX_INITIALIZED in msg:
                self._set("gox", SensorState.RUNNING)
            elif markers.GOX_SHUTDOWN in msg or markers.GOX_SESSION_ISSUES in msg:
                self._set("gox", SensorState.STOPPED)
            elif line.level in ("error", "critical"):
                self._set("gox", SensorState.FAILED, msg)

        elif module == markers.MODULE_ASTERX:
            if markers.ASTERX_INITIALIZED in msg:
                self._set("asterx", SensorState.RUNNING)
            elif markers.ASTERX_SHUTDOWN in msg:
                self._set("asterx", SensorState.STOPPED)
            elif line.level in ("error", "critical"):
                self._set("asterx", SensorState.FAILED, msg)

        elif module == markers.MODULE_MAIN:
            if markers.RECEIVED_SIGNAL_PREFIX in msg:
                self.stopping = True
            elif markers.ALL_DRIVERS_SHUT_DOWN in msg:
                self.clean_stop = True
                for key, st in STATE.sensors.items():
                    if st.state not in (SensorState.FAILED, SensorState.DISABLED):
                        st.state = SensorState.STOPPED
            elif markers.INIT_FAILED_SUFFIX in msg or markers.RUN_EXCEPTION_SUFFIX in msg:
                STATE.last_error = msg
                for name, prefix in MAIN_FAIL_MAP.items():
                    if msg.startswith(name):
                        if prefix == "lms":
                            self._set_lms_all(SensorState.FAILED, msg)
                        else:
                            self._set(prefix, SensorState.FAILED, msg)


MONITOR = HealthMonitor()
