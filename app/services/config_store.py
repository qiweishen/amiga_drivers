"""Read / validate / atomically save the driver config files (edited as raw text
so every comment survives; the C++ loaders remain the final validators)."""

from __future__ import annotations

import json
import math
import os
import re
import shutil
import tempfile
import yaml
from contextlib import suppress
from dataclasses import dataclass
from pathlib import Path
from yaml.nodes import MappingNode, ScalarNode
from yaml.tokens import AliasToken, AnchorToken

from . import control_owner
from ..constants import CONFIG_FILES, ENABLE_KEYS, FX10_SNAPSHOT_CONFIG, REPO_ROOT, SNAPSHOT_CONFIG, ConfigFile
from ..state import STATE


class ConflictError(Exception):
    """File changed on disk since it was loaded into the editor."""


class ConfigBusyError(RuntimeError):
    """The acquisition may still be reading its startup configuration."""


class ConfigPathChangedError(RuntimeError):
    """The main config now points at a different file; reload before saving."""


_PATH_KEYS = {
    "asterx": "ASTERX Driver Config Path", "fx10": "FX10 Driver Config Path",
    "gox": "GOX Driver Config Path", "lms4xxx": "LMS4XXX Driver Config Path",
}


@dataclass
class LoadedConfig:
    file: ConfigFile
    text: str
    mtime: int  # nanoseconds, for conflict detection


def checked_snapshot_path(path: Path) -> Path:
    """Resolve a tool-only config under the same root policy as editable configs."""
    path = path.resolve()
    root = os.environ.get("AMIGA_CONFIG_ROOT", "")
    if root and not path.is_relative_to(Path(root).resolve()):
        raise ValueError(f"Configuration must remain inside {root}: {path}")
    return path


def _checked_config(file: ConfigFile) -> ConfigFile:
    checked_snapshot_path(file.path)
    return file


def get(config_id: str) -> ConfigFile:
    template = CONFIG_FILES[config_id]
    if config_id == "main":
        return _checked_config(ConfigFile(template.id, template.label, template.path.resolve()))
    general = _main_document().get("General") or {}
    default = template.path.relative_to(REPO_ROOT).as_posix()
    raw = general.get(_PATH_KEYS[config_id], default)
    if not isinstance(raw, str) or not raw.strip():
        raise ValueError(f"{_PATH_KEYS[config_id]} must be a non-empty path")
    path = resolve_output_dir(raw)
    return _checked_config(ConfigFile(template.id, template.label, path.resolve()))


def _main_document(text: str | None = None) -> dict:
    if text is None:
        text = get("main").path.read_text(encoding="utf-8")
    doc = yaml.safe_load(text)
    if doc is None:
        doc = {}
    if not isinstance(doc, dict) or ("General" in doc and not isinstance(doc["General"], dict)):
        raise ValueError("Main config and General must be YAML mappings")
    return doc


def read(config_id: str) -> LoadedConfig:
    return _read_file(get(config_id))


def _read_file(cf: ConfigFile) -> LoadedConfig:
    def signature(stat) -> tuple:
        return stat.st_dev, stat.st_ino, stat.st_size, stat.st_mtime_ns

    with cf.path.open(encoding="utf-8") as stream:
        before = os.fstat(stream.fileno())
        text = stream.read()
        if signature(before) != signature(os.fstat(stream.fileno())) or signature(before) != signature(cf.path.stat()):
            raise ConflictError(f"{cf.path} changed while it was being read; reload it")
    return LoadedConfig(cf, text, before.st_mtime_ns)


def validate(config_id: str, text: str) -> list[str]:
    """Syntax-only validation. An empty list means 'parses fine'.

    The C++ loaders strictly validate the configuration schema on load;
    syntax validity alone does not certify an accepted acquisition config.
    """
    errors: list[str] = []
    try:
        yaml.safe_load(text)
    except Exception as e:  # yaml.YAMLError
        errors.append(str(e))
    return errors


def save(config_id: str, text: str, expected_mtime: int | None, *,
         expected_path: Path | None = None) -> int:
    """Atomic save with a .bak of the previous content. Returns the new mtime.

    Raises ConflictError when the on-disk file changed after `expected_mtime`
    (another editor / another GUI tab); caller decides reload-vs-overwrite.
    """
    _require_writable()
    cf = get(config_id)
    if expected_path is not None and cf.path.resolve() != expected_path.resolve():
        raise ConfigPathChangedError(f"Config path changed to {cf.path}; reload before saving")
    if expected_mtime is not None:
        try:
            current_mtime = cf.path.stat().st_mtime_ns
        except FileNotFoundError:
            raise ConflictError(f"{cf.path.name} was removed from disk") from None
        if current_mtime != expected_mtime:
            raise ConflictError(f"{cf.path.name} was modified on disk")
    if cf.path.exists():
        shutil.copy2(cf.path, cf.path.with_suffix(cf.path.suffix + ".bak"))
    tmp = cf.path.with_suffix(cf.path.suffix + ".tmp")
    tmp.write_text(text, encoding="utf-8")
    tmp.replace(cf.path)  # atomic within the same directory
    STATE.pending_config_notice = STATE.process_state.value in ("starting", "running", "stopping")
    return cf.path.stat().st_mtime_ns


def _require_writable() -> None:
    # Shared by editor saves and paired workbench saves, checked at commit.
    control_owner.require()
    if STATE.snapshot_busy or STATE.tool_uncertain:
        raise ConfigBusyError("Configuration is locked while a device tool is running or its exit is unknown")
    if STATE.control_uncertain:
        raise ConfigBusyError("Configuration is locked until the acquisition process can be verified")
    if STATE.config_locked:
        raise ConfigBusyError("Configuration is locked until acquisition initialization or shutdown finishes")


# --- config-main.yaml helpers -------------------------------------------------

def main_settings() -> dict:
    """Parsed view of config-main.yaml (read-only; editing stays text-level)."""
    text = read("main").text
    doc = _main_document(text)
    general = doc.get("General") or {}
    enables = {}
    for driver, key in ENABLE_KEYS.items():
        value = general.get(key, False)
        if type(value) is not bool:
            raise ValueError(f"General.{key} must be a boolean")
        enables[driver] = value
    sensor_trigger = doc.get("Sensor Trigger", {})
    if not isinstance(sensor_trigger, dict):
        raise ValueError("Sensor Trigger must be a YAML mapping")
    sensor_trigger_port = sensor_trigger.get("Port", "")
    if not isinstance(sensor_trigger_port, str):
        raise ValueError("Sensor Trigger.Port must be a string")
    if sensor_trigger_port and not Path(sensor_trigger_port).is_absolute():
        raise ValueError("Sensor Trigger.Port must be an absolute device path or empty")
    # yaml-cpp reads scalar text as-is, including unquoted numbers or words
    # such as yes. PyYAML's constructed booleans would lose that spelling.
    nodes = _main_field_nodes(text, allow_missing=True)
    metadata = {}
    for key, default in (("Operator", "Anonymous"), ("Field", "Unknown"), ("Output Directory", "./data")):
        node = nodes.get(key)
        if node is None:
            metadata[key] = default
        elif not isinstance(node, ScalarNode) or node.tag == "tag:yaml.org,2002:null":
            raise ValueError(f"General.{key} must be a non-null text scalar")
        else:
            metadata[key] = node.value
    output_dir = metadata["Output Directory"]
    return {
        "operator": metadata["Operator"],
        "field": metadata["Field"],
        "output_dir": resolve_output_dir(output_dir),
        "output_dir_raw": output_dir,
        "enables": enables,
        "enable_logging": True,  # The main executable always initializes session logging.
        "lms_config_path": str(
            general.get("LMS4XXX Driver Config Path", "./lms4xxx_driver/config/config-lms4xxx.yaml")),
        # The rig's one SensorSync board; "" = none. Recording drivers receive
        # it through main; standalone snapshot/reference tools use freerun without this port.
        "sensor_trigger_port": sensor_trigger_port,
    }


def resolve_output_dir(raw: str) -> Path:
    """Resolve paths exactly where the co-located acquisition sees them."""
    p = Path(raw)
    return p if p.is_absolute() else (REPO_ROOT / p).resolve()


def output_dir_problems(raw: str) -> list[str]:
    """Start-preflight checks for the Output Directory value."""
    path = resolve_output_dir(raw)
    root = os.environ.get("AMIGA_DATA_ROOT", "")
    if root and not path.resolve().is_relative_to(Path(root).resolve()):
        return [f"Output Directory must remain inside the shared recording mount {root}"]
    return []


def lms_instance_names() -> list[str]:
    """Enabled instance names from the LMS yaml `lidar:` list (dashboard
    pre-seed). The C++ side uses each entry's `id` verbatim as the log tag,
    so the sensor keys are the raw ids.

    Reads the file selected by the main config."""
    try:
        doc = yaml.safe_load(read("lms4xxx").text) or {}
        lidars = doc.get("lidar")
        if isinstance(lidars, list):
            return [str(e["id"]) for e in lidars
                    if isinstance(e, dict) and e.get("id") and e.get("enabled", True)]
        return []
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


def _verified_text(lines: list[str], checks: list[tuple[list[str | int], object]]) -> str:
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
    return text


def _verify_and_save(loaded: LoadedConfig, lines: list[str], checks: list[tuple[list[str | int], object]]) -> None:
    save(loaded.file.id, _verified_text(lines, checks), expected_mtime=loaded.mtime, expected_path=loaded.file.path)


def _apply_file(config_id: str) -> ConfigFile:
    # Tool profiles stay out of CONFIG_FILES and the Configuration editor.
    if config_id in ("gox_snapshot", "fx10_snapshot"):
        source = SNAPSHOT_CONFIG if config_id == "gox_snapshot" else FX10_SNAPSHOT_CONFIG
        return ConfigFile(config_id, config_id, checked_snapshot_path(source))
    return get(config_id)


def _check_apply_source(loaded: LoadedConfig) -> None:
    current = _apply_file(loaded.file.id)
    if current.path != loaded.file.path:
        raise ConfigPathChangedError(f"Config path changed to {current.path}; retry Apply")
    try:
        latest = _read_file(current)
    except FileNotFoundError:
        raise ConflictError(f"{current.path} was removed from disk") from None
    if latest.mtime != loaded.mtime or latest.text != loaded.text:
        raise ConflictError(f"{current.path} was modified on disk; retry Apply")


def _stage_apply(loaded: LoadedConfig, text: str | None) -> Path:
    path = loaded.file.path
    fd, name = tempfile.mkstemp(prefix=f".{path.name}.", suffix=".tmp", dir=path.parent)
    os.close(fd)
    staged = Path(name)
    try:
        shutil.copy2(path, staged)  # retain the existing config's file mode
        if text is not None:
            staged.write_text(text, encoding="utf-8")
        return staged
    except Exception:
        with suppress(OSError):
            staged.unlink()
        raise


def _save_acquisition_pair(edits: list[tuple[LoadedConfig, str]]) -> None:
    """Stage both validated edits before publishing; restore on a caught failure.

    Each rename is atomic. Two paths cannot be published atomically together;
    .bak files also preserve the originals if the process is interrupted.
    GUI writes run synchronously in the control owner, without an await between
    commits. External edits are checked again before replacement and rollback.
    """
    _require_writable()
    unique: dict[Path, tuple[LoadedConfig, str]] = {}
    for loaded, text in edits:
        _check_apply_source(loaded)
        previous = unique.get(loaded.file.path)
        if previous is not None and previous[1] != text:
            raise ApplyError(f"Recording and snapshot edits disagree for {loaded.file.path}")
        unique.setdefault(loaded.file.path, (loaded, text))
    backups = {}
    for path in unique:
        backup = path.with_suffix(path.suffix + ".bak")
        if checked_snapshot_path(backup.parent) != backup.parent:
            raise ConfigPathChangedError(f"Backup directory changed for {path}; retry Apply")
        if backup in unique or backup.resolve() in unique:
            raise ApplyError(f"Backup path {backup} overlaps an active configuration")
        backups[path] = backup
    staged: list[tuple[LoadedConfig, str, Path]] = []
    committed: list[tuple[LoadedConfig, LoadedConfig]] = []
    temporary: list[Path] = []
    try:
        for loaded, text in unique.values():
            path = _stage_apply(loaded, text)
            temporary.append(path)
            staged.append((loaded, text, path))
        for loaded, _ in edits:
            _check_apply_source(loaded)
        for loaded, _, _ in staged:
            backup = _stage_apply(loaded, None)
            temporary.append(backup)
            # Replacing the directory entry never follows an existing .bak
            # symlink into another configuration or outside the allowed root.
            backup.replace(backups[loaded.file.path])
        for loaded, text, path in staged:
            _require_writable()
            _check_apply_source(loaded)
            written = LoadedConfig(loaded.file, text, path.stat().st_mtime_ns)
            path.replace(loaded.file.path)
            committed.append((loaded, written))
    except Exception as error:
        unrestored = []
        for original, written in reversed(committed):
            try:
                _check_apply_source(written)
                restored = _stage_apply(original, original.text)
                temporary.append(restored)
                _check_apply_source(written)
                restored.replace(original.file.path)
            except Exception as restore_error:
                unrestored.append(f"{original.file.path}: {restore_error}")
        if unrestored:
            raise ApplyError(f"Could not save both configs ({error}); could not restore "
                             + "; ".join(unrestored) + ". Original contents remain in .bak files.") from error
        if committed:
            raise ApplyError(f"Could not save both configs; earlier writes were restored: {error}") from error
        raise
    finally:
        for path in temporary:
            with suppress(OSError):
                path.unlink(missing_ok=True)
    STATE.pending_config_notice = STATE.process_state.value in ("starting", "running", "stopping")


def _gox_camera_entry(lines: list[str], target_ip: str, target_mac: str) -> tuple[int, int, int, dict]:
    """The config-gox.yaml `cameras:` entry that identifies the selected camera:
    (index, start line, end line, parsed entry).

    A camera may be configured by ip OR by mac (mac wins in the driver), so both
    are matched — writing the wrong camera's block would silently mistune it.
    """
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
    return index, start, end, cameras[index]


def _sub_block_bounds(lines: list[str], key: str, start: int, end: int) -> tuple[int, int]:
    """Line range [s, e) of the body of a nested `key:` block inside lines[start:end],
    ended by the first non-blank line indented no deeper than the key itself."""
    head = re.compile(rf"^(?P<indent>\s*){re.escape(key)}\s*:\s*(#.*)?$")
    for i in range(start, end):
        m = head.match(lines[i])
        if not m:
            continue
        indent = len(m.group("indent"))
        for j in range(i + 1, end):
            stripped = lines[j].strip()
            if stripped and not stripped.startswith("#") and len(lines[j]) - len(lines[j].lstrip()) <= indent:
                return i + 1, j
        return i + 1, end
    raise ApplyError(f"no '{key}:' block in the expected section")


def apply_gox_acquisition(target_ip: str, target_mac: str, exposure_ms: float, gain: float | None) -> str:
    """Write the selected camera's exposure (and gain, unless None) to recording and snapshot YAML.

    ``gain=None`` writes the exposure only and leaves each config's gain as it is.
    """
    exposure_ms = float(exposure_ms)
    if not math.isfinite(exposure_ms) or exposure_ms <= 0:
        raise ApplyError("exposure_ms must be finite and positive")
    if gain is not None:
        gain = float(gain)
        if not math.isfinite(gain) or not 1 <= gain <= 126:
            raise ApplyError("gain must be finite and within [1, 126]")
    edits = []
    descriptions = []
    for config_id in ("gox", "gox_snapshot"):
        loaded = read("gox") if config_id == "gox" else _read_file(_apply_file(config_id))
        try:
            text, camera_id = _gox_acquisition_text(loaded, target_ip, target_mac, exposure_ms, gain,
                                                   freerun=config_id == "gox_snapshot")
        except ApplyError as error:
            raise ApplyError(f"{loaded.file.path}: {error}") from error
        edits.append((loaded, text))
        descriptions.append(f"{loaded.file.path} · {camera_id} ({target_ip})")
    _save_acquisition_pair(edits)
    return ("\n".join(descriptions) + f"\nexposure_ms={_fmt(exposure_ms)}"
            + (f", gain={_fmt(gain)}" if gain is not None else " (gain unchanged)"))


def gox_exposure_limit_ms(target_ip: str, target_mac: str = "") -> tuple[float | None, list[str]]:
    """The exposure ceiling apply_gox_acquisition enforces for this camera (read-only).

    Returns the strictest 900 / frame_rate_hz over the recording and snapshot
    configs that apply the period check, or None when neither does, plus a
    description of each binding constraint.
    """
    limits, notes = [], []
    for config_id in ("gox", "gox_snapshot"):
        loaded = read("gox") if config_id == "gox" else _read_file(_apply_file(config_id))
        lines = loaded.text.splitlines()
        _index, _start, _end, camera = _gox_camera_entry(lines, target_ip, target_mac)
        acquisition = camera.get("acquisition") or {}
        trigger = acquisition.get("trigger") or {}
        frame_rate = acquisition.get("frame_rate_hz")
        document = yaml.safe_load(loaded.text) or {}
        sensor_trigger = document.get("sensor_trigger") or {}
        controlled_trigger = (sensor_trigger.get("enabled", False) and camera.get("enabled", True)
                              and trigger.get("mode") == "external")
        checked = config_id == "gox_snapshot" or controlled_trigger or trigger.get("mode", "freerun") == "freerun"
        if frame_rate is None or not checked or isinstance(frame_rate, bool):
            continue
        try:
            rate = float(frame_rate)
        except (TypeError, ValueError):
            continue
        if math.isfinite(rate) and rate > 0:
            limits.append(900.0 / rate)
            notes.append(f"{loaded.file.path}: {rate:g} Hz → exposure < {900.0 / rate:g} ms")
    return (min(limits) if limits else None), notes


def _check_exposure_period(exposure_ms: float, frame_rate: object) -> None:
    try:
        if isinstance(frame_rate, bool):
            raise ValueError
        rate = float(frame_rate)
    except (TypeError, ValueError, OverflowError) as error:
        raise ApplyError("acquisition.frame_rate_hz must be finite and positive") from error
    if not math.isfinite(rate) or rate <= 0:
        raise ApplyError("acquisition.frame_rate_hz must be finite and positive")
    limit_ms = 900.0 / rate
    if exposure_ms >= limit_ms:
        raise ApplyError(
            f"exposure {exposure_ms:g} ms must be below {limit_ms:g} ms "
            f"at {rate:g} Hz (10% timing margin). Lower the exposure, "
            "or lower acquisition.frame_rate_hz in this config first."
        )


def _gox_acquisition_text(loaded: LoadedConfig, target_ip: str, target_mac: str,
                         exposure_ms: float, gain: float | None, *, freerun: bool) -> tuple[str, str]:
    lines = loaded.text.splitlines()
    index, start, end, camera = _gox_camera_entry(lines, target_ip, target_mac)
    if freerun and index != 0:
        raise ApplyError("The selected camera must be the first cameras: entry in the snapshot config; "
                         "jai_snapshot and ColorChecker reference capture use cameras[0]")

    # Match the driver's 10% timing margin. Snapshot tools force freerun even
    # when their source YAML still names an external trigger mode.
    acquisition = camera.get("acquisition") or {}
    trigger = acquisition.get("trigger") or {}
    frame_rate = acquisition.get("frame_rate_hz")
    document = yaml.safe_load(loaded.text) or {}
    sensor_trigger = document.get("sensor_trigger") or {}
    controlled_trigger = (sensor_trigger.get("enabled", False) and camera.get("enabled", True)
                          and trigger.get("mode") == "external")
    if frame_rate is not None and (freerun or controlled_trigger or trigger.get("mode", "freerun") == "freerun"):
        _check_exposure_period(exposure_ms, frame_rate)

    start, end = _sub_block_bounds(lines, "acquisition", start, end)
    _replace_scalar(lines, "exposure_ms", _fmt(exposure_ms), start=start, end=end)
    checks = [(["cameras", index, "acquisition", "exposure_ms"], exposure_ms)]
    if gain is not None:
        _replace_scalar(lines, "gain", _fmt(gain), start=start, end=end)
        checks.append((["cameras", index, "acquisition", "gain"], gain))
    text = _verified_text(lines, checks)
    return text, str(camera.get("id", f"#{index}"))


def _gox_entry_for_readdress(lines: list[str], target_ip: str, target_mac: str,
                             new_ip: str) -> tuple[int, int, int, dict, bool]:
    """Which config-gox.yaml entry a re-addressed camera belongs to. Strict
    match (MAC, else the camera's PRE-change address) first; but the reason an
    operator uses Set IP is usually that the camera sits at a foreign / LLA
    address that was never in the config, so when the strict match fails fall
    back to the entries not MAC-bound to another camera: one that already
    reads `new_ip` (nothing to do), else the single candidate. Anything
    ambiguous is refused rather than guessed. Last element: already at new_ip."""
    try:
        return (*_gox_camera_entry(lines, target_ip, target_mac), False)
    except ApplyError as strict:
        doc = yaml.safe_load("\n".join(lines)) or {}
        cameras = doc.get("cameras") or []

        def _mac(value) -> str:
            return re.sub(r"[^0-9a-f]", "", str(value or "").lower())

        candidates = [
            (i, c) for i, c in enumerate(cameras)
            if isinstance(c, dict)
               and (not _mac((c.get("device") or {}).get("mac")) or _mac((c.get("device") or {}).get("mac")) == _mac(
                target_mac))
        ]
        if any(str((c.get("device") or {}).get("ip", "")) == new_ip for _, c in candidates):
            return (*_gox_camera_entry(lines, new_ip, ""), True)
        enabled = [(i, c) for i, c in candidates if c.get("enabled", True)]
        pick = candidates if len(candidates) == 1 else (enabled if len(enabled) == 1 else [])
        if not pick:
            raise ApplyError(f"{strict}; cannot decide which entry to point at {new_ip} — "
                             "edit config-gox.yaml by hand") from None
        index, camera = pick[0]
        starts = [k for k, ln in enumerate(lines) if re.match(r"^\s*-\s+id\s*:", ln)]
        if len(starts) != len(cameras):
            raise ApplyError("cannot map the 'cameras:' entries to their lines (unexpected layout)")
        end = starts[index + 1] if index + 1 < len(starts) else len(lines)
        return index, starts[index], end, camera, False


def apply_device_ip(kind: str, target_ip: str, target_mac: str, new_ip: str) -> str:
    """After Camera Tools > Set IP re-addressed a camera, point the driver config
    at the new address: `device.ip` of the matching config-gox.yaml camera
    entry, or of config-fx10.yaml's top-level `device:` block. Matched by MAC
    or by the camera's address BEFORE the change; when that address is foreign
    to the config (the field case), the single entry not MAC-bound to another
    camera is used, and an entry already at `new_ip` is left untouched. Edited
    inside the `device:` sub-block only, read back before saving. Returns a
    summary."""
    quoted = f'"{new_ip}"'  # both templates quote the address
    if kind == "GoX":
        loaded = read("gox")
        lines = loaded.text.splitlines()
        index, start, end, camera, already = _gox_entry_for_readdress(lines, target_ip, target_mac, new_ip)
        camera_id = camera.get("id", f"#{index}")
        if already:
            return f"{loaded.file.path} · {camera_id}: device.ip already {new_ip} (unchanged)"
        dev_start, dev_end = _sub_block_bounds(lines, "device", start, end)
        _replace_scalar(lines, "ip", quoted, start=dev_start, end=dev_end)
        _verify_and_save(loaded, lines, [(["cameras", index, "device", "ip"], new_ip)])
        return f"{loaded.file.path} · {camera_id}: device.ip={new_ip}"
    if kind == "FX10":
        loaded = read("fx10")
        lines = loaded.text.splitlines()
        doc = yaml.safe_load("\n".join(lines)) or {}
        device = doc.get("device") or {}
        entry_mac = re.sub(r"[^0-9a-f]", "", str(device.get("mac", "")).lower())
        if entry_mac and target_mac and entry_mac != re.sub(r"[^0-9a-f]", "", target_mac.lower()):
            raise ApplyError(f"config-fx10.yaml is bound to MAC {device.get('mac')}, not to {target_mac}")
        start, end = _block_bounds(lines, "device")
        _replace_scalar(lines, "ip", quoted, start=start, end=end)
        _verify_and_save(loaded, lines, [(["device", "ip"], new_ip)])
        return f"{loaded.file.path} · device.ip={new_ip}"
    raise ApplyError(f"no driver config for a {kind} device")


def apply_fx10_acquisition(exposure_ms: float, spatial_binning: int | None, spectral_binning: int | None) -> str:
    """Write exposure/binning to both recording and snapshot acquisition blocks.

    Both binnings None writes the exposure only and leaves each config's binning as it is.
    """
    exposure_ms = float(exposure_ms)
    if not math.isfinite(exposure_ms) or exposure_ms <= 0:
        raise ApplyError("exposure_ms must be finite and positive")
    if (spatial_binning is None) != (spectral_binning is None):
        raise ApplyError("spatial_binning and spectral_binning are applied together or not at all")
    for value, name in ((spatial_binning, "spatial_binning"), (spectral_binning, "spectral_binning")):
        if value is not None and (type(value) is not int or value not in (1, 2, 4, 8)):
            raise ApplyError(f"{name} must be 1, 2, 4, or 8")
    edits = []
    for config_id in ("fx10", "fx10_snapshot"):
        loaded = read("fx10") if config_id == "fx10" else _read_file(_apply_file(config_id))
        try:
            text = _fx10_acquisition_text(loaded, exposure_ms, spatial_binning, spectral_binning)
        except ApplyError as error:
            raise ApplyError(f"{loaded.file.path}: {error}") from error
        edits.append((loaded, text))
    _save_acquisition_pair(edits)
    return ("\n".join(str(loaded.file.path) for loaded, _ in edits) + f"\nexposure_ms={_fmt(exposure_ms)}"
            + (f", spatial_binning={spatial_binning}, spectral_binning={spectral_binning}"
               if spatial_binning is not None else " (binning unchanged)"))


def _fx10_acquisition_text(loaded: LoadedConfig, exposure_ms: float,
                          spatial_binning: int | None, spectral_binning: int | None) -> str:
    lines = loaded.text.splitlines()
    doc = yaml.safe_load("\n".join(lines)) or {}
    acquisition = doc.get("acquisition")
    if not isinstance(acquisition, dict):
        raise ApplyError("config has no 'acquisition:' block")
    # ParseAcquisition enforces this before CLI overrides, in both trigger modes.
    # 50 Hz is fx10::AcquisitionConfig's default when the field is omitted.
    _check_exposure_period(exposure_ms, acquisition.get("frame_rate_hz", 50.0))
    # The camera cannot do both; the C++ loader rejects the combination, which
    # would leave a config that no longer starts. MROI needs 1 x 1 binning on
    # BOTH axes (FX10 manual p.9, p.26), not just the spectral one.
    if spatial_binning is not None and (acquisition.get("mroi") or {}).get("enabled") \
            and (spectral_binning != 1 or spatial_binning != 1):
        raise ApplyError("acquisition.mroi is enabled, which requires 1 x 1 binning: spatial_binning and "
                         "spectral_binning must both be 1 (FX10 constraint)")

    start, end = _block_bounds(lines, "acquisition")
    _replace_scalar(lines, "exposure_ms", _fmt(exposure_ms), start=start, end=end)
    checks = [(["acquisition", "exposure_ms"], exposure_ms)]
    if spatial_binning is not None:
        _replace_scalar(lines, "spatial_binning", str(spatial_binning), start=start, end=end)
        _replace_scalar(lines, "spectral_binning", str(spectral_binning), start=start, end=end)
        checks += [(["acquisition", "spatial_binning"], spatial_binning),
                   (["acquisition", "spectral_binning"], spectral_binning)]
    return _verified_text(lines, checks)


def fx10_exposure_limit_ms() -> tuple[float, list[str]]:
    """The exposure ceiling apply_fx10_acquisition enforces (read-only): the
    strictest 900 / frame_rate_hz over the recording and snapshot configs."""
    limits, notes = [], []
    for config_id in ("fx10", "fx10_snapshot"):
        loaded = read("fx10") if config_id == "fx10" else _read_file(_apply_file(config_id))
        acquisition = (yaml.safe_load(loaded.text) or {}).get("acquisition")
        if not isinstance(acquisition, dict):
            raise ApplyError(f"{loaded.file.path}: config has no 'acquisition:' block")
        frame_rate = acquisition.get("frame_rate_hz", 50.0)
        try:
            if isinstance(frame_rate, bool):
                raise ValueError
            rate = float(frame_rate)
        except (TypeError, ValueError, OverflowError) as error:
            raise ApplyError(f"{loaded.file.path}: acquisition.frame_rate_hz must be finite and positive") from error
        if not math.isfinite(rate) or rate <= 0:
            raise ApplyError(f"{loaded.file.path}: acquisition.frame_rate_hz must be finite and positive")
        limits.append(900.0 / rate)
        notes.append(f"{loaded.file.path}: {rate:g} Hz → exposure < {900.0 / rate:g} ms")
    return min(limits), notes


def set_enable(driver: str, value: bool) -> None:
    """Line-anchored text edit of an Enable flag in config-main.yaml — the file
    is permissive YAML but we edit as text so comments survive."""
    key = ENABLE_KEYS[driver]
    loaded = read("main")
    text = loaded.text
    pattern = re.compile(rf"^(\s*{re.escape(key)}\s*:\s*).*$", re.M)
    new_text, n = pattern.subn(rf"\g<1>{'true' if value else 'false'}", text, count=1)
    if n == 0:  # key missing: append under General is risky — refuse loudly
        raise KeyError(f"no '{key}:' line found in config-main.yaml")
    save("main", new_text, expected_mtime=loaded.mtime, expected_path=loaded.file.path)


def _main_field_nodes(text: str, *, allow_missing: bool = False) -> dict:
    """Return unique General entries from this exact file snapshot."""
    document = yaml.compose(text, Loader=yaml.SafeLoader)

    def entries(node, label: str) -> dict:
        if not isinstance(node, MappingNode):
            raise ApplyError(f"{label} must be a YAML mapping")
        result = {}
        for name, value in node.value:
            if not isinstance(name, ScalarNode) or name.tag != "tag:yaml.org,2002:str":
                raise ApplyError(f"{label} must have string keys without YAML merges")
            if name.value in result:
                raise ApplyError(f"Duplicate key {label}.{name.value}; resolve it in the config editor")
            result[name.value] = value
        return result

    if document is None and allow_missing:
        return {}
    root = entries(document, "Main config")
    if "General" not in root and allow_missing:
        return {}
    return entries(root.get("General"), "General")


def _main_field_node(text: str, key: str) -> ScalarNode:
    """Locate one unambiguous scalar without consuming inline comments.

    Anchors/aliases and multiline scalars need more than a local replacement;
    leave those layouts to the full config editor instead of changing meaning.
    """
    if any(isinstance(token, (AliasToken, AnchorToken)) for token in yaml.scan(text, Loader=yaml.SafeLoader)):
        raise ApplyError("Overview editing does not support YAML anchors or aliases; use the config editor")
    general = _main_field_nodes(text)
    node = general.get(key)
    if node is None:
        raise ApplyError(f"General.{key} is missing; add it in the config editor before editing here")
    if not isinstance(node, ScalarNode) or node.tag == "tag:yaml.org,2002:null":
        raise ApplyError(f"General.{key} must be a non-null text scalar")
    if node.start_mark.line != node.end_mark.line or node.style in ("|", ">"):
        raise ApplyError(f"General.{key} must occupy one line for Overview editing")
    return node


def set_main_field(key: str, value: str, *, expected_value: str) -> None:
    """Compatibility wrapper for saving one Overview field."""
    set_main_fields({key: value}, expected_values={key: expected_value})


def set_main_fields(values: dict[str, str], *, expected_values: dict[str, str]) -> None:
    """Save changed Overview fields together, preserving unrelated edits.

    Each displayed value is its field's conflict baseline. Validate and check
    every submitted field before replacing any text, then commit once through
    save() so a failed Save cannot leave only some fields written.
    """
    if not isinstance(values, dict) or not isinstance(expected_values, dict):
        raise ValueError("Overview values and their expected values must be mappings")
    if values.keys() != expected_values.keys():
        raise ValueError("Each Overview field must have exactly one expected value")
    for key, value in values.items():
        if key not in ("Operator", "Field", "Output Directory"):
            raise ValueError(f"Unsupported Overview field: {key}")
        if not isinstance(value, str) or not isinstance(expected_values[key], str):
            raise ValueError(f"General.{key} and its expected value must be strings")
        if not value.strip():
            raise ValueError(f"{key} must not be empty or whitespace")
        if key == "Output Directory":
            if any(ord(char) < 32 or 127 <= ord(char) < 160 for char in value):
                raise ValueError("Output Directory must not contain control characters")
            problems = output_dir_problems(value)
            if problems:
                raise ValueError("; ".join(problems))
    if not values:
        return

    loaded = read("main")
    replacements = []
    changed_keys = set()
    for key, value in values.items():
        node = _main_field_node(loaded.text, key)
        if node.value == value:
            continue
        if node.value != expected_values[key]:
            raise ConflictError(f"General.{key} was changed on disk; reload before saving")

        # Keep Unicode characters intact (JSON surrogate pairs are not YAML's
        # representation of astral code points), escaping YAML-sensitive breaks.
        replacement = json.dumps(value, ensure_ascii=False)
        replacement = "".join(
            f"\\u{ord(char):04x}" if 127 <= ord(char) < 160 or char in "\u2028\u2029\ufffe\uffff" else char
            for char in replacement
        )
        replacements.append((node.start_mark.index, node.end_mark.index, replacement))
        changed_keys.add(key)
    if not replacements:
        return

    text = loaded.text
    for start, end, replacement in sorted(replacements, reverse=True):
        text = text[:start] + replacement + text[end:]
    for key, value in values.items():
        edited = _main_field_node(text, key)
        # Already-matching legacy scalars may remain unquoted. Replaced values
        # must be string scalars and every submitted field must keep its text.
        if edited.value != value or (key in changed_keys and edited.tag != "tag:yaml.org,2002:str"):
            raise ApplyError(f"General.{key} did not round-trip as the requested string")
    save("main", text, expected_mtime=loaded.mtime, expected_path=loaded.file.path)
