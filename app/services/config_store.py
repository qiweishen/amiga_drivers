"""Read / validate / atomically save the driver config files (edited as raw text
so every comment survives; the C++ loaders remain the final validators)."""

from __future__ import annotations

import posixpath
import re
import shutil
from dataclasses import dataclass
from pathlib import Path, PurePosixPath

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

    The C++ loaders re-validate on load (lenient parsing, but critical
    invariants still throw) — surface that in the UI as the final word.
    """
    errors: list[str] = []
    try:
        yaml.safe_load(text)
    except Exception as e:  # yaml.YAMLError
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
            raise ConflictError(f"{cf.path.name} was modified on disk")
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
        "enables": {drv: bool(general.get(key, drv == "lms4xxx")) for drv, key in ENABLE_KEYS.items()},
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
        f"Output Directory ({raw}) is outside the container mounts. Accepted forms: a repo-relative "
        "path (e.g. ./recordings), /workspace/..., or the shared disk ./dataset|/workspace/dataset/...; "
        "host-style absolute paths (/mnt/..., /home/...) do not exist inside the container"
    ]


def _lms_snake_case(name: str) -> str:
    """Verbatim mirror of Common::StringUtil::ToSnakeCase (string_util.h) — the
    C++ side converts instance names with it, and every marker/error line
    carries the converted form, so the sensor keys must match exactly."""
    out: list[str] = []
    for i, ch in enumerate(name):
        if ch in (" ", "-"):
            out.append("_")
        elif ch.isupper():
            if i > 0 and name[i - 1] not in (" ", "-", "_") and name[i - 1].islower():
                out.append("_")
            out.append(ch.lower())
        else:
            out.append(ch)
    return "".join(out)


def lms_instance_names() -> list[str]:
    """Enabled instance names from the LMS yaml `lidar:` list (dashboard
    pre-seed). The C++ side uses each entry's `id` verbatim as the log tag,
    so the sensor keys are the raw ids.

    Falls back to the older `instances:`/`Instances:` map forms (snake_cased,
    mirroring the C++ conversion of that era) so a stale field config still
    seeds the dashboard."""
    try:
        doc = yaml.safe_load(CONFIG_FILES["lms4xxx"].path.read_text(encoding="utf-8")) or {}
        lidars = doc.get("lidar")
        if isinstance(lidars, list):
            return [str(e["id"]) for e in lidars
                    if isinstance(e, dict) and e.get("id") and e.get("enabled", True)]
        raw = (doc.get("instances") or doc.get("Instances") or {}).keys()
        return [_lms_snake_case(name) for name in raw]
    except Exception:
        return []


# --- acquisition parameter writeback (Camera Tools "Apply") -------------------
# The Camera Tools page tunes exposure/gain/binning against a live camera and
# writes the winning values back here. Edits stay TEXT-level so the extensive
# inline documentation in these files survives; the result is re-parsed and the
# value read back before it is saved.

class ApplyError(Exception):
    """The config file does not have the shape the writeback expects."""


def _scalar_re(key: str) -> re.Pattern:
    # "    exposure_ms: 150.0              # float, ms [1..115279]"
    return re.compile(
        rf"^(?P<head>\s*{re.escape(key)}\s*:\s*)(?P<value>[^#\n]*?)(?P<pad>\s*)(?P<comment>#.*)?$"
    )


def _block_bounds(lines: list[str], key: str) -> tuple[int, int]:
    """Line range [start, end) of the body of a top-level `key:` block.

    Bounding every edit matters: config-fx10.yaml carries `spatial_binning` and
    `spectral_binning` a second time in the GenICam node-name map, where a
    replacement would rewrite a node NAME and break the driver at startup.
    """
    head = re.compile(rf"^{re.escape(key)}\s*:\s*(#.*)?$")
    start = next((i + 1 for i, ln in enumerate(lines) if head.match(ln)), None)
    if start is None:
        raise ApplyError(f"no top-level '{key}:' block")
    for j in range(start, len(lines)):
        if lines[j].strip() and not lines[j][0].isspace():
            return start, j
    return start, len(lines)


def _replace_scalar(lines: list[str], key: str, value: str, *,
                    start: int = 0, end: int | None = None) -> int:
    """Replace `key`'s value in lines[start:end], keeping indent and the inline
    comment. Returns the line index. Raises ApplyError when absent."""
    pattern = _scalar_re(key)
    for i in range(start, len(lines) if end is None else end):
        m = pattern.match(lines[i])
        if m:
            # Keep the comment column stable when the new value is not wider.
            pad = m.group("pad") or ""
            if m.group("comment"):
                shift = len(m.group("value")) - len(value)
                pad = " " * max(1, len(pad) + shift)
            lines[i] = m.group("head") + value + (pad + m.group("comment") if m.group("comment") else "")
            return i
    raise ApplyError(f"no '{key}:' line found in the expected section")


def _fmt(value: float | int) -> str:
    """YAML scalar in the style these files already use (floats stay floats, so
    the C++ `as<double>()` reader keeps seeing a float).

    repr() is the shortest form that re-parses to the SAME double — a fixed
    precision here would round the value and then fail _verify_and_save's
    read-back with what looks like a corruption error.
    """
    if isinstance(value, int):
        return str(value)
    text = repr(float(value))
    if "e" in text and "." not in text:
        # 1e-05 has no dot, and YAML 1.1 resolves that as a STRING, not a float.
        mantissa, _, exponent = text.partition("e")
        text = f"{mantissa}.0e{exponent}"
    return text


def _verify_and_save(config_id: str, lines: list[str], checks: list[tuple[list[str], object]]) -> None:
    """Re-parse the edited text and confirm every (key path, value) round-trips
    before writing. A silently wrong acquisition parameter would corrupt every
    dataset recorded with it."""
    text = "\n".join(lines) + "\n"  # splitlines() dropped the file's terminator
    try:
        doc = yaml.safe_load(text) or {}
    except Exception as e:
        raise ApplyError(f"the edit did not produce valid YAML: {e}") from e
    for path, expected in checks:
        node = doc
        for part in path:
            node = node[int(part)] if isinstance(node, list) else node.get(part)
            if node is None:
                raise ApplyError(f"cannot read back {'.'.join(map(str, path))} after the edit")
        if isinstance(expected, float):
            ok = isinstance(node, (int, float)) and abs(float(node) - expected) < 1e-9
        else:
            ok = node == expected
        if not ok:
            raise ApplyError(f"{'.'.join(map(str, path))} read back as {node!r}, expected {expected!r}")
    save(config_id, text, expected_mtime=None)


def apply_gox_acquisition(target_ip: str, target_mac: str, exposure_ms: float, gain: float) -> str:
    """Write exposure/gain into the config-gox.yaml camera entry that identifies
    the selected camera. Returns a human-readable summary of what changed.

    A camera may be configured by ip OR by mac (mac wins in the driver), so both
    are matched — writing the wrong camera's block would silently mistune it.
    """
    cf = CONFIG_FILES["gox"]
    lines = cf.path.read_text(encoding="utf-8").splitlines()
    doc = yaml.safe_load("\n".join(lines)) or {}
    cameras = doc.get("cameras")
    if not isinstance(cameras, list) or not cameras:
        raise ApplyError("config-gox.yaml has no 'cameras:' list")

    def _mac(value: str) -> str:
        return re.sub(r"[^0-9a-f]", "", str(value).lower())

    def _rank(camera: dict) -> int:
        """2 = exact MAC, 1 = IP, 0 = no match.

        A mac-bound entry is decided by MAC ALONE: the driver ignores device.ip
        whenever device.mac is set (gox_driver/src/ebus/camera_session.cpp), so
        an entry whose MAC differs is a hard non-match however its stale ip
        reads. Ranking (rather than first-hit) also keeps an exact MAC match
        from being preempted by an earlier entry that only matches by IP.
        """
        device = camera.get("device") or {}
        entry_mac = _mac(device.get("mac", ""))
        if target_mac and entry_mac:
            return 2 if entry_mac == _mac(target_mac) else 0
        return 1 if target_ip and str(device.get("ip", "")) == target_ip else 0

    ranked = [(_rank(c), -i) for i, c in enumerate(cameras) if isinstance(c, dict)]
    best = max(ranked, default=(0, 0))
    index = -best[1] if best[0] else None
    if index is None:
        known = ", ".join(
            f"{(c.get('device') or {}).get('ip') or '-'}/{(c.get('device') or {}).get('mac') or '-'}"
            for c in cameras if isinstance(c, dict)
        )
        raise ApplyError(
            f"no camera matching {target_ip or '?'} / {target_mac or '?'} in config-gox.yaml "
            f"(configured ip/mac: {known})"
        )

    # Text bounds of that camera entry: from its "- id:" line to the next one.
    starts = [i for i, ln in enumerate(lines) if re.match(r"^\s*-\s+id\s*:", ln)]
    if len(starts) != len(cameras):
        raise ApplyError("cannot map the 'cameras:' entries to their lines (unexpected layout)")
    start = starts[index]
    end = starts[index + 1] if index + 1 < len(starts) else len(lines)

    _replace_scalar(lines, "exposure_ms", _fmt(float(exposure_ms)), start=start, end=end)
    _replace_scalar(lines, "gain", _fmt(float(gain)), start=start, end=end)
    _verify_and_save("gox", lines, [
        (["cameras", index, "acquisition", "exposure_ms"], float(exposure_ms)),
        (["cameras", index, "acquisition", "gain"], float(gain)),
    ])
    camera_id = str(cameras[index].get("id", f"#{index}"))
    return f"{camera_id} ({target_ip}): exposure_ms={_fmt(float(exposure_ms))}, gain={_fmt(float(gain))}"


def apply_fx10_acquisition(exposure_ms: float, spatial_binning: int, spectral_binning: int) -> str:
    """Write exposure/binning into config-fx10.yaml's acquisition block."""
    cf = CONFIG_FILES["fx10"]
    lines = cf.path.read_text(encoding="utf-8").splitlines()
    doc = yaml.safe_load("\n".join(lines)) or {}
    acquisition = doc.get("acquisition")
    if not isinstance(acquisition, dict):
        raise ApplyError("config-fx10.yaml has no 'acquisition:' block")
    for value, name in ((spatial_binning, "spatial_binning"), (spectral_binning, "spectral_binning")):
        if value not in (1, 2, 4, 8):
            raise ApplyError(f"{name} must be 1, 2, 4, or 8")
    # The camera cannot do both; the C++ loader rejects the combination, which
    # would leave a config that no longer starts.
    if spectral_binning != 1 and (acquisition.get("mroi") or {}).get("enabled"):
        raise ApplyError("acquisition.mroi is enabled, which requires spectral_binning: 1 (FX10 constraint)")

    start, end = _block_bounds(lines, "acquisition")
    _replace_scalar(lines, "exposure_ms", _fmt(float(exposure_ms)), start=start, end=end)
    _replace_scalar(lines, "spatial_binning", str(spatial_binning), start=start, end=end)
    _replace_scalar(lines, "spectral_binning", str(spectral_binning), start=start, end=end)
    _verify_and_save("fx10", lines, [
        (["acquisition", "exposure_ms"], float(exposure_ms)),
        (["acquisition", "spatial_binning"], spatial_binning),
        (["acquisition", "spectral_binning"], spectral_binning),
    ])
    return (f"exposure_ms={_fmt(float(exposure_ms))}, spatial_binning={spatial_binning}, "
            f"spectral_binning={spectral_binning}")


def set_enable(driver: str, value: bool) -> None:
    """Line-anchored text edit of an Enable flag in config-main.yaml — the file
    is permissive YAML but we edit as text so comments survive."""
    key = ENABLE_KEYS[driver]
    cf = CONFIG_FILES["main"]
    text = cf.path.read_text(encoding="utf-8")
    pattern = re.compile(rf"^(\s*{re.escape(key)}\s*:\s*).*$", re.M)
    new_text, n = pattern.subn(rf"\g<1>{'true' if value else 'false'}", text, count=1)
    if n == 0:  # key missing: append under General is risky — refuse loudly
        raise KeyError(f"no '{key}:' line found in config-main.yaml")
    save("main", new_text, expected_mtime=None)
