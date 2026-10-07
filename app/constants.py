"""Shared paths, configuration registry, and presentation settings."""

from __future__ import annotations

import os
from dataclasses import dataclass
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parent.parent

# GUI and binaries run in one container and share these paths.
BUILD_BIN = REPO_ROOT / "build" / "bin"
BIN_AMIGA = BUILD_BIN / "AmigaDrivers"
BIN_EBUS_DISCOVER = BUILD_BIN / "ebus_discover"  # common/: GigE enumeration for both camera drivers
BIN_EBUS_SET_IP = BUILD_BIN / "ebus_set_ip"  # common/: FORCEIP + persistent IP write
BIN_SNAPSHOT = BUILD_BIN / "jai_snapshot"
BIN_FX10_SNAPSHOT = BUILD_BIN / "fx10_snapshot"
BIN_FX10_REFERENCE = BUILD_BIN / "fx10_reference"

MAIN_CONFIG = Path(os.environ.get("AMIGA_MAIN_CONFIG", str(REPO_ROOT / "config" / "config-main.yaml")))
SNAPSHOT_CONFIG = Path(os.environ.get("AMIGA_SNAPSHOT_CONFIG", str(REPO_ROOT / "gox_driver" / "config" / "config-gox-snapshot.yaml")))

# --- GUI ---------------------------------------------------------------------
GUI_HOST = os.environ.get("AMIGA_GUI_HOST", "0.0.0.0")
GUI_PORT = int(os.environ.get("AMIGA_GUI_PORT", "8619"))

# --- theme -------------------------------------------------------------------
# Quasar brand colors, applied application-wide in main() via app.colors().
# THE place to retheme the console: every element asks for a brand NAME
# ("primary", "positive", "negative", "warning") rather than a literal hue, so
# editing a value here moves everything that means it.
#
# Two things are deliberately NOT reachable from here:
#   - the neutral "grey" / "blue-grey" badges (idle, disabled, stopped) —
#     Quasar has no brand slot for a neutral;
#   - Tailwind utility classes (text-gray-600, bg-red-50, text-amber-700, ...)
#     used for subtle text and panel backgrounds; those are per-class edits.
PALETTE = {
    "primary": "#01a7d7",    # header bar, default buttons, progress bars
    "secondary": "#26a69a",
    "accent": "#9c27b0",
    "positive": "#21ba45",   # sensor RUNNING / recording healthy
    "negative": "#c10015",   # FAILED, Stop recording, disk below the hard floor
    "warning": "#f2c037",    # INITIALIZING / STOPPING, disk warning floor
    "info": "#31ccec",
}

# --- refresh cadence ---------------------------------------------------------
# Lifecycle changes are read from the run manifest by AcquisitionController;
# LOG_POLL_S affects diagnostics and Statistics write-rate updates only.
# Ticks are cheap by construction: every page updates its elements in place
# (nothing is rebuilt per tick) and NiceGUI drops setter calls that would not
# change anything, so an idle tick sends nothing at all. Lowering these past the
# stages above only re-renders identical values.
LOG_POLL_S = 0.2  # session log tail
UI_TICK_S = 0.25  # pages showing live sensor/session state (Overview, headers)
TOOL_TICK_S = 0.5  # tool pages whose tick only mirrors process state


RUNTIME_DIR = Path(os.environ.get("AMIGA_RUNTIME_DIR", str(REPO_ROOT / "app" / "_runtime")))
SNAPSHOT_DIR = RUNTIME_DIR / "snapshot"
FX10_SNAPSHOT_DIR = RUNTIME_DIR / "snapshot_fx10"
SNAPSHOT_KEEP = 10  # retained snapshot session dirs

SESSION_DIR_RE = r"^\d{8}_\d{6}$"  # <Output Directory>/<YYYYMMDD_HHMMSS>/


# --- Config registry ---------------------------------------------------------
@dataclass(frozen=True)
class ConfigFile:
    id: str
    label: str
    path: Path  # container path; all configs are YAML


CONFIG_FILES: dict[str, ConfigFile] = {
    c.id: c
    for c in [
        ConfigFile("main", "Main (config-main.yaml)", MAIN_CONFIG),
        ConfigFile("asterx", "AsteRx", REPO_ROOT / "asterx_driver" / "config" / "config-asterx.yaml"),
        ConfigFile("fx10", "FX10", REPO_ROOT / "fx10_driver" / "config" / "config-fx10.yaml"),
        ConfigFile("gox", "GoX", REPO_ROOT / "gox_driver" / "config" / "config-gox.yaml"),
        ConfigFile("lms4xxx", "LMS4xxx", REPO_ROOT / "lms4xxx_driver" / "config" / "config-lms4xxx.yaml")
    ]
}


# Sensors shown on the dashboard; lms4xxx expands into one card per instance.
DRIVERS = ("asterx", "fx10", "gox", "lms4xxx")
ENABLE_KEYS = {
    "asterx": "Enable ASTERX",
    "fx10": "Enable FX10",
    "gox": "Enable GOX",
    "lms4xxx": "Enable LMS4XXX"
}
