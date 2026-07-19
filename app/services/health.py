"""Per-sensor health state machine driven by the unified log's marker lines.

Marker strings below mirror the C++ sources verbatim:
  ins401_driver_app.cpp  "INS401 driver initialized" / "INS401 driver shutdown completely"
  lms4xxx_driver_app.cpp "LiDAR instance [<name>] initialized successfully"
                         "LiDAR instance [<name>] driver shutdown completely"
  gox_driver_app.cpp     "GoX driver initialized" / "GoX driver shutdown completely"
                         "GoX session ended with issues (standalone exit code N)"
  asterx_driver_app.cpp  "AsteRx driver initialized (receiver configured, recording)"
                         "AsteRx driver shutdown completely"
  main.cpp ("Main")      "<X> driver initialization failed" / "<X> run() exception"
                         "Received signal <n>, shutting down all drivers..."
                         "All drivers shut down"
"""

from __future__ import annotations

import re

from ..state import STATE, SensorState, SensorStatus
from .log_buffer import LogLine

LMS_INIT_RE = re.compile(r"LiDAR instance \[(.+?)\] initialized successfully")
LMS_STOP_RE = re.compile(r"LiDAR instance \[(.+?)\] driver shutdown completely")
LMS_MSG_INSTANCE_RE = re.compile(r"^\[(.+?)\] ")

# [Main] failure text -> sensor key prefix
MAIN_FAIL_MAP = {
    "INS401": "ins401",
    "LMS4xxx": "lms",
    "GoX": "gox",
    "AsteRx": "asterx",
}


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
        sensors["gox"] = SensorStatus("gox", "GoX 相机")
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

        if module == "INS401App":
            if "INS401 driver initialized" in msg:
                self._set("ins401", SensorState.RUNNING)
            elif "INS401 driver shutdown completely" in msg:
                self._set("ins401", SensorState.STOPPED)
            elif line.level in ("error", "critical"):
                self._set("ins401", SensorState.FAILED, msg)

        elif module == "LMS4xxxApp":
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

        elif module == "GoXApp":
            if "GoX driver initialized" in msg:
                self._set("gox", SensorState.RUNNING)
            elif "GoX driver shutdown completely" in msg or "GoX session ended with issues" in msg:
                self._set("gox", SensorState.STOPPED)
            elif line.level in ("error", "critical"):
                self._set("gox", SensorState.FAILED, msg)

        elif module == "AsterxApp":
            if "AsteRx driver initialized" in msg:
                self._set("asterx", SensorState.RUNNING)
            elif "AsteRx driver shutdown completely" in msg:
                self._set("asterx", SensorState.STOPPED)
            elif line.level in ("error", "critical"):
                self._set("asterx", SensorState.FAILED, msg)

        elif module == "Main":
            if "Received signal" in msg:
                self.stopping = True
            elif "All drivers shut down" in msg:
                self.clean_stop = True
                for key, st in STATE.sensors.items():
                    if st.state not in (SensorState.FAILED, SensorState.DISABLED):
                        st.state = SensorState.STOPPED
            elif "initialization failed" in msg or "run() exception" in msg:
                STATE.last_error = msg
                for name, prefix in MAIN_FAIL_MAP.items():
                    if msg.startswith(name):
                        if prefix == "lms":
                            self._set_lms_all(SensorState.FAILED, msg)
                        else:
                            self._set(prefix, SensorState.FAILED, msg)


MONITOR = HealthMonitor()
