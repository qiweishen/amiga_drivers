"""Driver-neutral GigE Vision helpers, backed by the two common/ tools:
discovery (`ebus_discover --json`, one scan for the GoX cameras AND the FX10)
and device re-addressing (`ebus_set_ip --json`: FORCEIP, then the persistent
IP nodes, see common/include/ebus/ip_config.hpp).

Both tools keep stdout for their JSON document (their logs go to stderr), so
the whole of stdout is parsed and stdout+stderr is shown in the raw panel."""

from __future__ import annotations

import asyncio
import json
import time
from dataclasses import dataclass, field

from ..constants import BIN_EBUS_DISCOVER, BIN_EBUS_SET_IP
from . import camera_operations, fx10_tools, gox_tools, runtime, tool_jobs, control_client, wire

SET_IP_TOOL = "ebus_set_ip"
# GUI-side hard timeout; the tool's own budget is discovery (4 s) + the
# reachable-timeout (10 s) + connect/persist, so this must stay above that.
SET_IP_TIMEOUT_S = 45.0
# Exit code of ebus_set_ip meaning "address active but NOT persisted"
_EXIT_PERSIST_FAILED = 6


@dataclass
class Device:
    kind: str  # "GoX" | "FX10" | "Other" (from the vendor string)
    vendor: str
    model: str
    ip: str
    subnet_mask: str
    gateway: str
    mac: str
    serial: str
    user_name: str
    firmware: str
    interface: str
    config_valid: bool  # false = camera IP unreachable from its NIC's subnet
    persistent_available: bool
    persistent_enabled: bool
    dhcp_enabled: bool
    lla_enabled: bool
    host_subnets: list[str] = field(default_factory=list)  # "ip/mask" of the NIC that saw it
    host_masks: list[str] = field(default_factory=list)  # the same NIC's masks, for the dialog default

    @property
    def ip_config_text(self) -> str:
        parts = [name for name, on in (("Persistent", self.persistent_enabled),
                                       ("DHCP", self.dhcp_enabled),
                                       ("LLA", self.lla_enabled)) if on]
        return "+".join(parts) if parts else "-"


@dataclass
class DiscoverResult:
    devices: list[Device] = field(default_factory=list)
    raw_output: str = ""
    error: str = ""


@dataclass
class SetIpResult:
    ok: bool
    exit_code: int = -1
    transient_only: bool = False  # FORCEIP applied, persistent write failed/unsupported
    message: str = ""
    report: dict = field(default_factory=dict)  # the tool's JSON document, when it produced one
    raw_output: str = ""
    elapsed_s: float = 0.0


def classify_kind(vendor: str, model: str) -> str:
    """Which driver owns this camera. The vendor string is the stable signal
    (JAI / Specim); the model is a fallback for an empty vendor field."""
    text = f"{vendor} {model}".lower()
    if "jai" in text:
        return "GoX"
    if "specim" in text or "fx10" in text:
        return "FX10"
    return "Other"


def guard_reason_for(kind: str) -> str | None:
    """Why a camera-touching action on a device of this kind must not run now."""
    if kind == "GoX":
        return gox_tools.guard_reason()
    if kind == "FX10":
        return fx10_tools.guard_reason()
    return camera_operations.guard_reason(None)


async def discover(timeout_ms: int = 1500) -> DiscoverResult:
    if control_client.enabled():
        return wire.restore(DiscoverResult, await control_client.call("camera.discover", timeout_ms))
    return await camera_operations.run("discovery", lambda: _discover(timeout_ms), kind="discover")


async def _discover(timeout_ms: int) -> DiscoverResult:
    if type(timeout_ms) is not int or not 100 <= timeout_ms <= 10000:
        raise ValueError("Discovery timeout must be between 100 and 10000 ms")
    proc = await tool_jobs.spawn([runtime.exec_path(BIN_EBUS_DISCOVER), "--timeout", str(timeout_ms), "--json"])
    out, err = await tool_jobs.communicate(proc, timeout_ms / 1000 + 15)
    res = runtime.ExecResult(proc.returncode or 0, out.decode(errors="replace"), err.decode(errors="replace"))
    raw = (res.stdout + ("\n--- stderr ---\n" + res.stderr if res.stderr.strip() else "")).strip()
    if not res.ok:
        return DiscoverResult(raw_output=raw, error=f"ebus_discover failed (exit {res.code}): {res.stderr.strip()}")
    try:
        doc = json.loads(res.stdout)
    except json.JSONDecodeError as e:
        return DiscoverResult(raw_output=raw, error=f"Cannot parse the --json output: {e}")
    devices: list[Device] = []
    for iface in doc.get("interfaces", []):
        host_subnets = [f"{a.get('ip', '')}/{a.get('subnet_mask', '')}" for a in iface.get("addresses", [])]
        host_masks = [a.get("subnet_mask", "") for a in iface.get("addresses", []) if a.get("subnet_mask")]
        for d in iface.get("devices", []):
            cfg = d.get("ip_config", {}) or {}
            vendor = d.get("vendor", "")
            model = d.get("model", "")
            devices.append(Device(
                kind=classify_kind(vendor, model),
                vendor=vendor,
                model=model,
                ip=d.get("ip", ""),
                subnet_mask=d.get("subnet_mask", ""),
                gateway=d.get("gateway", ""),
                mac=d.get("mac", ""),
                serial=d.get("serial", ""),
                user_name=d.get("user_name", ""),
                firmware=d.get("firmware", ""),
                interface=iface.get("name", ""),
                config_valid=bool(d.get("config_valid", False)),
                persistent_available=bool(cfg.get("persistent_available", False)),
                persistent_enabled=bool(cfg.get("persistent_enabled", False)),
                dhcp_enabled=bool(cfg.get("dhcp_enabled", False)),
                lla_enabled=bool(cfg.get("lla_enabled", False)),
                host_subnets=host_subnets,
                host_masks=host_masks,
            ))
    return DiscoverResult(devices=devices, raw_output=raw)


async def set_ip(mac: str, ip: str, subnet_mask: str, gateway: str = "0.0.0.0",
                 allow_foreign_subnet: bool = False, *, kind: str) -> SetIpResult:
    """Recheck recording ownership after dialogs and reserve before any await."""
    if control_client.enabled():
        return wire.restore(SetIpResult, await control_client.call("camera.set_ip", mac, ip, subnet_mask,
                                                                   gateway, allow_foreign_subnet, kind=kind))
    driver = {"GoX": "gox", "FX10": "fx10"}.get(kind)
    return await camera_operations.run(
        driver, lambda: _set_ip(mac, ip, subnet_mask, gateway, allow_foreign_subnet), kind="set-ip")


async def _set_ip(mac: str, ip: str, subnet_mask: str, gateway: str,
                  allow_foreign_subnet: bool) -> SetIpResult:
    """FORCEIP + persistent write with a service-owned camera reservation."""
    t0 = time.monotonic()
    args = [runtime.exec_path(BIN_EBUS_SET_IP), "--mac", mac, "--ip", ip, "--subnet-mask", subnet_mask,
            "--gateway", gateway, "--json"]
    if allow_foreign_subnet:
        args.append("--allow-foreign-subnet")
    proc = await tool_jobs.spawn(args)
    try:
        out_b, err_b = await tool_jobs.communicate(proc, SET_IP_TIMEOUT_S)
    except asyncio.TimeoutError:
        return SetIpResult(False, message=f"Timed out (>{SET_IP_TIMEOUT_S:.0f}s) — ebus_set_ip was terminated; "
                                          "power-cycle the camera if it is no longer reachable",
                           elapsed_s=time.monotonic() - t0)

    stdout = out_b.decode(errors="replace")
    stderr = err_b.decode(errors="replace")
    raw_output = (stdout + "\n--- stderr ---\n" + stderr).strip()
    elapsed = time.monotonic() - t0
    try:
        report = json.loads(stdout)
    except json.JSONDecodeError:
        # No document at all (crash before the report): only the process code speaks.
        return SetIpResult(False, exit_code=proc.returncode if proc.returncode is not None else -1,
                           message=f"ebus_set_ip produced no JSON (exit {proc.returncode}): {stderr.strip()[-400:]}",
                           raw_output=raw_output, elapsed_s=elapsed)
    code = int(report.get("exit_code", proc.returncode or 0))
    if code == 0:
        return SetIpResult(True, exit_code=0, report=report, raw_output=raw_output, elapsed_s=elapsed,
                           message=f"{report.get('device', {}).get('model', 'camera')} is now at {ip}/{subnet_mask}; "
                                   f"persistent IP written (active from the next power-up)")
    return SetIpResult(False, exit_code=code, transient_only=(code == _EXIT_PERSIST_FAILED),
                       message=str(report.get("error", "")) or f"ebus_set_ip failed (exit {code})",
                       report=report, raw_output=raw_output, elapsed_s=elapsed)
