"""Read / validate / atomically save the driver config files (edited as raw text
so every comment survives; the C++ loaders remain the final validators)."""

from __future__ import annotations

import posixpath
import re
import shutil
from dataclasses import dataclass
from pathlib import Path, PurePosixPath

import pyjson5
import yaml

from ..constants import CONFIG_FILES, ENABLE_KEYS, REPO_ROOT, ConfigFile, to_host
from . import runtime


class ConflictError(Exception):
    """File changed on disk since it was loaded into the editor."""


@dataclass
class LoadedConfig:
    file: ConfigFile
    text: str
    mtime: float


def get(config_id: str) -> ConfigFile:
    return CONFIG_FILES[config_id]


def read(config_id: str) -> LoadedConfig:
    cf = CONFIG_FILES[config_id]
    return LoadedConfig(cf, cf.path.read_text(encoding="utf-8"), cf.path.stat().st_mtime)


def validate(config_id: str, text: str) -> list[str]:
    """Structure-only validation. An empty list means 'parses fine'.

    The C++ side re-validates strictly on load (the GoX parser rejects unknown
    keys with a did-you-mean hint) — surface that in the UI as the final word.
    """
    cf = CONFIG_FILES[config_id]
    errors: list[str] = []
    try:
        if cf.fmt == "yaml":
            yaml.safe_load(text)
        else:  # jsonc
            pyjson5.decode(text)
    except Exception as e:  # yaml.YAMLError / pyjson5 errors
        errors.append(str(e))
    return errors


def save(config_id: str, text: str, expected_mtime: float | None) -> float:
    """Atomic save with a .bak of the previous content. Returns the new mtime.

    Raises ConflictError when the on-disk file changed after `expected_mtime`
    (another editor / another GUI tab); caller decides reload-vs-overwrite.
    """
    cf = CONFIG_FILES[config_id]
    if expected_mtime is not None and cf.path.exists():
        if abs(cf.path.stat().st_mtime - expected_mtime) > 1e-6:
            raise ConflictError(f"{cf.path.name} 在磁盘上已被修改")
    if cf.path.exists():
        shutil.copy2(cf.path, cf.path.with_suffix(cf.path.suffix + ".bak"))
    tmp = cf.path.with_suffix(cf.path.suffix + ".tmp")
    tmp.write_text(text, encoding="utf-8")
    tmp.replace(cf.path)  # atomic within the same directory
    return cf.path.stat().st_mtime


# --- config-main.yaml helpers -------------------------------------------------

def main_settings() -> dict:
    """Parsed view of config-main.yaml (read-only; editing stays text-level)."""
    doc = yaml.safe_load(CONFIG_FILES["main"].path.read_text(encoding="utf-8")) or {}
    general = doc.get("General") or {}
    logging_ = doc.get("Logging System") or {}
    output_dir = str(general.get("Output Directory", "./data"))
    return {
        "output_dir": resolve_output_dir(output_dir),
        "output_dir_raw": output_dir,
        "enables": {drv: bool(general.get(key, drv in ("ins401", "lms4xxx"))) for drv, key in ENABLE_KEYS.items()},
        "enable_logging": bool(logging_.get("Enable Logging", True)),
        "lms_config_path": str(general.get("LMS4XXX Driver Config Path", "./lms4xxx_driver/config/config-lms4xxx.yaml")),
    }


def resolve_output_dir(raw: str) -> Path | None:
    """Host-side view of the Output Directory, or None when it is not visible
    from the host.

    The raw string is consumed by the BINARY in ITS namespace: relative
    entries resolve against the binary's working dir (/workspace in docker,
    the repo root natively — runtime.spawn sets both). Natively every path is
    already a host path; in docker mode absolute paths must be container
    paths that map back through the mounts."""
    if runtime.is_docker():
        if PurePosixPath(raw).is_absolute():
            container_abs = posixpath.normpath(raw)
        else:
            container_abs = posixpath.normpath("/workspace/" + raw)
        try:
            return to_host(container_abs)
        except ValueError:
            return None
    p = Path(raw)
    return p if p.is_absolute() else (REPO_ROOT / p).resolve()


def output_dir_problems(raw: str) -> list[str]:
    """Start-preflight checks for the Output Directory value."""
    if resolve_output_dir(raw) is not None:
        return []
    return [
        f"Output Directory ({raw}) 不在容器挂载可见范围内。可用形式：仓库相对路径（如 ./recordings）、"
        "/workspace/... 或共享盘 ./dataset|/workspace/dataset/...；"
        "宿主机风格的绝对路径（/mnt/...、/home/...）在容器内不存在"
    ]


def lms_instance_names() -> list[str]:
    """Instance names from the LMS yaml `Instances:` map (dashboard pre-seed)."""
    try:
        doc = yaml.safe_load(CONFIG_FILES["lms4xxx"].path.read_text(encoding="utf-8")) or {}
        return list((doc.get("Instances") or {}).keys())
    except Exception:
        return []


def set_enable(driver: str, value: bool) -> None:
    """Line-anchored text edit of an Enable flag in config-main.yaml — the file
    is permissive YAML but we edit as text so comments survive."""
    key = ENABLE_KEYS[driver]
    cf = CONFIG_FILES["main"]
    text = cf.path.read_text(encoding="utf-8")
    pattern = re.compile(rf"^(\s*{re.escape(key)}\s*:\s*).*$", re.M)
    new_text, n = pattern.subn(rf"\g<1>{'true' if value else 'false'}", text, count=1)
    if n == 0:  # key missing: append under General is risky — refuse loudly
        raise KeyError(f"config-main.yaml 中找不到 '{key}:' 行")
    save("main", new_text, expected_mtime=None)
