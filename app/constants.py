"""Shared constants: paths, container identity, config registry, mount map."""

from __future__ import annotations

import os
from dataclasses import dataclass
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

# --- Docker ------------------------------------------------------------------
CONTAINER = "amiga-sensor-dev"
COMPOSE_FILE = REPO_ROOT / ".devcontainer" / "docker-compose.yml"

# Host path <-> container path. Order matters: longest host prefix first.
# Only paths under one of these mounts are visible on both sides.
MOUNT_MAP: list[tuple[Path, str]] = [
    (Path("/mnt/SharedData/Post_Processing_Data"), "/workspace/dataset"),
    (REPO_ROOT, "/workspace"),
]


def to_container(host_path: Path | str) -> str:
    """Map a host path to its in-container path. Raises ValueError if unmapped."""
    p = Path(host_path).resolve()
    for host_root, cont_root in MOUNT_MAP:
        try:
            rel = p.relative_to(host_root)
        except ValueError:
            continue
        return cont_root if str(rel) == "." else f"{cont_root}/{rel.as_posix()}"
    raise ValueError(f"path is outside the container mounts: {p}")


def to_host(container_path: str) -> Path:
    """Map an in-container path back to the host. Raises ValueError if unmapped."""
    for host_root, cont_root in MOUNT_MAP:
        if container_path == cont_root:
            return host_root
        if container_path.startswith(cont_root + "/"):
            return host_root / container_path[len(cont_root) + 1 :]
    raise ValueError(f"container path is outside the known mounts: {container_path}")


def mapped_on_both_sides(host_path: Path | str) -> bool:
    try:
        to_container(host_path)
        return True
    except ValueError:
        return False


# --- Binaries & configs (HOST paths; convert per execution backend via
# services.runtime.exec_path) -------------------------------------------------
BUILD_BIN = REPO_ROOT / "build" / "bin"
BIN_AMIGA = BUILD_BIN / "AmigaDrivers"
BIN_DISCOVER = BUILD_BIN / "jai_discover"
BIN_SNAPSHOT = BUILD_BIN / "jai_snapshot"

MAIN_CONFIG = REPO_ROOT / "config" / "config-main.yaml"
SNAPSHOT_CONFIG = REPO_ROOT / "gox_driver" / "config" / "config-snapshot.json"

# --- GUI ---------------------------------------------------------------------
GUI_HOST = os.environ.get("AMIGA_GUI_HOST", "0.0.0.0")
GUI_PORT = int(os.environ.get("AMIGA_GUI_PORT", "8619"))

RUNTIME_DIR = REPO_ROOT / "app" / "_runtime"
SNAPSHOT_DIR = RUNTIME_DIR / "snapshot"
SNAPSHOT_KEEP = 10  # retained snapshot session dirs

UNPACK_SCRIPT = REPO_ROOT / "gox_driver" / "scripts" / "unpack_raw.py"
VENV_PYTHON = REPO_ROOT / ".venv" / "bin" / "python"

SESSION_DIR_RE = r"^\d{8}_\d{6}$"  # <Output Directory>/<YYYYMMDD_HHMMSS>/


# --- Config registry ---------------------------------------------------------
@dataclass(frozen=True)
class ConfigFile:
    id: str
    label: str
    path: Path  # host path
    fmt: str  # "yaml" | "jsonc"


CONFIG_FILES: dict[str, ConfigFile] = {
    c.id: c
    for c in [
        ConfigFile("main", "主配置 (config-main.yaml)", REPO_ROOT / "config" / "config-main.yaml", "yaml"),
        ConfigFile("ins401", "INS401", REPO_ROOT / "ins401_driver" / "config" / "config-ins401.yaml", "yaml"),
        ConfigFile("lms4xxx", "LMS4xxx", REPO_ROOT / "lms4xxx_driver" / "config" / "config-lms4xxx.yaml", "yaml"),
        ConfigFile("gox", "GoX", REPO_ROOT / "gox_driver" / "config" / "config-gox.json", "jsonc"),
        ConfigFile("asterx", "AsteRx", REPO_ROOT / "asterx_driver" / "config" / "config-asterx.yaml", "yaml"),
        ConfigFile(
            "snapshot", "GoX 快照 (jai_snapshot)", REPO_ROOT / "gox_driver" / "config" / "config-snapshot.json", "jsonc"
        ),
    ]
}

# Sensors shown on the dashboard; lms4xxx expands into one card per instance.
DRIVERS = ("ins401", "lms4xxx", "gox", "asterx")
ENABLE_KEYS = {
    "ins401": "Enable INS401",
    "lms4xxx": "Enable LMS4XXX",
    "gox": "Enable GOX",
    "asterx": "Enable ASTERX",
}
