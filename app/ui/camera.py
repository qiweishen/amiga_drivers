"""Camera Tools: one discovery table for both camera drivers, a Set IP action
for field re-addressing, then a per-driver exposure workbench (preview +
Apply to the recording config).

Discovery is shared because the two backends answer the same question about the
same GigE segment: one `ebus_discover --json` scan (common/) fills one table,
rows are classified by vendor (JAI -> GoX, Specim -> FX10), and selecting a row
targets that camera's own section. The scan is a broadcast the cameras answer
without a control channel, so it runs even while a recording is on; a row whose
driver owns the camera is marked "in use" and its camera-touching actions
(snapshot, Set IP) stay disabled.

Set IP runs `ebus_set_ip`: a FORCEIP (immediate, transient) followed by the
persistent-IP GenICam nodes (kept across power cycles), then optionally rewrites
`device.ip` in the driver's config. Both sections tune AGAINST A LIVE CAMERA
without touching any config: the snapshot tools take the values as CLI
overrides. "Apply to config" is the separate, explicit step that writes the
winning values into the driver's recording config (comment-preserving, see
services/config_store.py).
"""

from __future__ import annotations

import ipaddress

from nicegui import app, ui

from ..constants import TOOL_TICK_S
from ..services import config_store, ebus_tools, fx10_tools, gox_tools
from ..state import STATE, ProcState
from . import layout

_COLUMNS = [
    {"name": "kind", "label": "Driver", "field": "kind", "align": "left"},
    {"name": "name", "label": "Model / device", "field": "name", "align": "left"},
    {"name": "ip", "label": "IP", "field": "ip", "align": "left"},
    {"name": "subnet", "label": "Subnet mask", "field": "subnet", "align": "left"},
    {"name": "mac", "label": "MAC", "field": "mac", "align": "left"},
    {"name": "ipcfg", "label": "IP config", "field": "ipcfg", "align": "left"},
    {"name": "detail", "label": "Details", "field": "detail", "align": "left"},
]

_FX10_DEFAULT_IP = "10.95.0.100"
_BINNING = [1, 2, 4, 8]
_CONFIG_KINDS = {"GoX": "gox", "FX10": "fx10"}  # kinds that have a driver config to point at the camera


def _escape(text: str) -> str:
    return text.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def _check_ip_inputs(ip: str, mask: str, gateway: str, host_subnets: list[str], allow_foreign: bool) -> str:
    """Client-side mirror of ebus_set_ip's validation (common/include/ebus/ipv4.hpp),
    so a typo is refused before the camera is touched. Returns "" when fine."""
    try:
        new_ip = ipaddress.IPv4Address(ip)
    except ValueError:
        return f"\"{ip}\" is not an IPv4 address"
    try:
        network = ipaddress.IPv4Network(f"{ip}/{mask}", strict=False)
    except ValueError:
        return f"\"{mask}\" is not a contiguous IPv4 subnet mask"
    if not 8 <= network.prefixlen <= 30:
        return f"subnet mask {mask} must be between /8 and /30"
    if new_ip in (network.network_address, network.broadcast_address) or new_ip.is_loopback \
            or new_ip.is_multicast or new_ip.is_reserved or new_ip.is_unspecified:
        return f"{ip}/{network.prefixlen} is not a usable host address"
    try:
        gw = ipaddress.IPv4Address(gateway or "0.0.0.0")
    except ValueError:
        return f"\"{gateway}\" is not an IPv4 gateway address"
    if not gw.is_unspecified and (gw not in network or gw == new_ip
                                  or gw in (network.network_address, network.broadcast_address)):
        return f"gateway {gateway} is not another host on {network.with_prefixlen}"
    reachable = False
    for entry in host_subnets:
        try:
            host = ipaddress.IPv4Interface(entry)
        except ValueError:
            continue
        if host.ip == new_ip:
            return f"{ip} is the address of the host adapter itself"
        if new_ip in host.network and host.ip in network:
            reachable = True
    if not reachable and not allow_foreign:
        return (f"{ip}/{network.prefixlen} is not on the host NIC's subnet ({', '.join(host_subnets) or 'no IPv4'}): "
                "the camera would become unreachable. Pick an address in that subnet, or tick "
                "'Allow an address outside the host subnet' under Advanced")
    return ""


@ui.page("/camera")
def camera_page() -> None:
    with layout.frame("Camera Tools"):
        # =================================================== device discovery
        ui.label("Device discovery (GoX and FX10)").classes("text-lg font-bold")
        ui.label("One scan lists every GigE Vision camera on the host's adapters. A camera owned by "
                 "the running recording is marked \"in use\"; its snapshot and Set IP stay disabled."
                 ).classes("text-sm text-gray-700 whitespace-pre-line")
        with ui.row().classes("items-center gap-4"):
            scan_btn = ui.button("Scan cameras", icon="search", on_click=lambda: _scan())
            set_ip_btn = ui.button("Set IP…", icon="lan", on_click=lambda: _set_ip_dialog()).props("outline")
            set_ip_busy = ui.spinner(size="sm").classes("hidden")
            scan_status = ui.label("").classes("text-sm text-gray-600")
        table = ui.table(columns=_COLUMNS, rows=[], row_key="key", selection="single").classes("w-full")

        # ======================================================== GoX section
        ui.separator()
        ui.label("GoX camera preview").classes("text-lg font-bold")
        gox_guard = ui.label("").classes("text-sm text-red-700")
        gox_target_label = ui.label().classes("text-sm text-gray-700")
        with ui.row().classes("items-center gap-8 w-full"):
            with ui.column().classes("flex-1 min-w-0"):
                ui.label("Exposure (ms)")
                ui.label("Camera limit: 0.001 - 332.738 ms on the GOX-12405C-PGE").classes("text-xs text-gray-500")
                # Camera limit: ExposureTime is 1..332738 µs (0.001..332.738 ms) on the GOX-12405C-PGE
                gox_exposure = ui.slider(min=0.001, max=332.738, step=0.001,
                                         value=min(332.738, app.storage.general.get("gox_exposure_ms", 50.000)))
                ui.number(min=0.001, max=332.738, step=0.001, suffix="ms").bind_value(gox_exposure).classes("w-full")
            with ui.column().classes("flex-1 min-w-0"):
                ui.label("Gain (magnification)")
                # Gain[AnalogAll] is a MAGNIFICATION on the GO-X, x1.0 .. x126.0
                # (about 0..42 dB) - manual p.148, dB conversion table p.181.
                ui.label("Camera limit: x1.0 ... x126.0 (about 0 ... 42 dB)").classes("text-xs text-gray-500")
                gox_gain = ui.slider(min=1.0, max=126.0, step=0.1,
                                     value=min(126.0, app.storage.general.get("gox_gain", 1.0)))
                ui.number(min=1.0, max=126.0, step=0.1, prefix="x").bind_value(gox_gain).classes("w-full")
        with ui.row().classes("items-center gap-4"):
            gox_snap_btn = ui.button("Take snapshot", icon="photo_camera", on_click=lambda: _gox_snap())
            gox_apply_btn = ui.button("Apply to config", icon="save", on_click=lambda: _gox_apply()).props("outline")
            gox_busy = ui.spinner(size="sm").classes("hidden")
            gox_meta = ui.label("").classes("text-sm text-gray-600")
        gox_warn = ui.label("").classes("text-sm text-amber-700")
        with ui.row().classes("w-full gap-4 items-start"):
            gox_image = ui.interactive_image().classes("max-w-[60%] border rounded")
            gox_chart = ui.echart({
                "xAxis": {"type": "category", "show": False},
                "yAxis": {"type": "value", "show": False},
                "grid": {"left": 4, "right": 4, "top": 4, "bottom": 4},
                "series": [
                    {"type": "bar", "data": [], "barCategoryGap": "0%"}
                ],
                "tooltip": {},
            }).classes("w-80 h-40")

        # ======================================================= FX10 section
        ui.separator()
        ui.label("FX10 camera preview").classes("text-lg font-bold")
        fx10_guard = ui.label("").classes("text-sm text-red-700")
        fx10_target_label = ui.label().classes("text-sm text-gray-700")
        with ui.row().classes("items-center gap-8 w-full"):
            with ui.column().classes("flex-1 min-w-0"):
                ui.label("Exposure (ms)")
                ui.label("Camera limit: 0.010 ... 419.000 ms on the FX10e").classes("text-xs text-gray-500")
                # Camera limit: ExposureTime is 10..419000 µs (0.01..419 ms) on the FX10e
                fx10_exposure = ui.slider(min=0.010, max=419.000, step=0.001,
                                          value=min(419.000, app.storage.general.get("fx10_exposure_ms", 50.000)))
                ui.number(min=0.010, max=419.000, step=0.001, suffix="ms").bind_value(fx10_exposure).classes("w-full")
            with ui.column().classes("flex-1 min-w-0"):
                ui.label("Spatial binning")
                ui.label("1024 / 512 / 256 / 128 px wide").classes("text-xs text-gray-500")
                fx10_spatial = ui.select(_BINNING, value=app.storage.general.get("fx10_spatial_binning", 1)).props(
                    "outlined dense")
            with ui.column().classes("flex-1 min-w-0"):
                ui.label("Spectral binning")
                ui.label("448 / 224 / 112 / 56 bands · 2 = factory").classes("text-xs text-gray-500")
                # Default 2, like the driver: the factory offset / black-level /
                # bad-pixel calibration was performed at 2x spectral binning.
                fx10_spectral = ui.select(_BINNING, value=app.storage.general.get("fx10_spectral_binning", 2)).props(
                    "outlined dense")
        with ui.row().classes("items-center gap-4"):
            fx10_snap_btn = ui.button("Take snapshot", icon="photo_camera", on_click=lambda: _fx10_snap())
            fx10_apply_btn = ui.button("Apply to config", icon="save", on_click=lambda: _fx10_apply()).props("outline")
            fx10_busy = ui.spinner(size="sm").classes("hidden")
            fx10_meta = ui.label("").classes("text-sm text-gray-600")
        ui.link("Collect white and dark references on the Collect Reference page", "/reference").classes("text-sm")
        # Per-band statistics over ~1 s of frames: x = wavelength, y = % of full scale
        fx10_chart = ui.echart({
            "xAxis": {"type": "value", "name": "Wavelength (nm)", "nameLocation": "middle",
                      "nameGap": 28, "min": "dataMin", "max": "dataMax"},
            "yAxis": {"type": "value", "name": "Normalized (%)", "min": 0, "max": 100},
            "grid": {"left": 56, "right": 24, "top": 32, "bottom": 44},
            "legend": {"top": 0},
            "tooltip": {"trigger": "axis"},
            "series": [],
        }).classes("w-full h-96")

        with ui.expansion("Raw output of the last command", icon="terminal").classes("w-full"):
            raw_pre = ui.element("pre").classes("w-full text-xs font-mono whitespace-pre-wrap")

        # ================================================================ glue
        def _show_raw(text: str) -> None:
            raw_pre.clear()
            with raw_pre:
                ui.html(f"<span>{_escape(text)}</span>")

        def _selected_row() -> dict:
            sel = table.selected
            return sel[0] if sel else {}

        def _selected(kind: str) -> dict:
            """The selected table row when it belongs to `kind`, else {}."""
            row = _selected_row()
            return row if row.get("kind") == kind else {}

        def _gox_target() -> tuple[str, str]:
            row = _selected("GoX")
            if row:
                return row.get("ip", ""), row.get("mac", "")
            return app.storage.general.get("gox_target_ip", ""), app.storage.general.get("gox_target_mac", "")

        def _fx10_target() -> str:
            row = _selected("FX10")
            if row:
                return row.get("ip", "")
            return app.storage.general.get("fx10_target_ip", _FX10_DEFAULT_IP)

        def refresh_guard() -> None:
            # Pure state reads, and NiceGUI drops setter calls that change
            # nothing — an idle tick sends nothing however often it runs.
            gox_reason = gox_tools.guard_reason()
            fx10_reason = fx10_tools.guard_reason()
            gox_guard.set_text(gox_reason or "")
            fx10_guard.set_text(fx10_reason or "")
            busy = STATE.snapshot_busy
            gox_ip, _gox_mac = _gox_target()
            fx10_ip = _fx10_target()
            row = _selected_row()
            # Discovery is a broadcast: it never needs the control channel.
            scan_btn.set_enabled(STATE.env_ok and not busy)
            # Set IP touches the camera: gated like a snapshot, per the row's driver.
            set_ip_btn.set_enabled(bool(row) and STATE.env_ok and not busy
                                   and ebus_tools.guard_reason_for(row.get("kind", "")) is None)
            gox_snap_btn.set_enabled(gox_reason is None and STATE.env_ok and not busy and bool(gox_ip))
            fx10_snap_btn.set_enabled(fx10_reason is None and STATE.env_ok and not busy and bool(fx10_ip))
            # Once initialization finishes, Apply edits the next run's config.
            config_writable = not STATE.config_locked and not STATE.control_uncertain
            gox_apply_btn.set_enabled(bool(gox_ip) and config_writable)
            fx10_apply_btn.set_enabled(config_writable)
            gox_target_label.set_text(
                f"Target camera: {gox_ip or '(Scan and select a GoX camera first)'}")
            fx10_target_label.set_text(
                f"Target camera: {fx10_ip or '(Scan and select a FX10 camera first)'}")

        ui.timer(TOOL_TICK_S, refresh_guard)
        refresh_guard()

        # ------------------------------------------------------ discovery glue
        async def _scan() -> None:
            scan_status.set_text("Scanning ...")
            res = await ebus_tools.discover()

            rows: list[dict] = []
            for d in res.devices:
                in_use = ebus_tools.guard_reason_for(d.kind) is not None
                ipcfg = ("valid" if d.config_valid else "INVALID-SUBNET") + " · " + d.ip_config_text
                if in_use:
                    ipcfg += " · in use"
                rows.append({
                    "key": f"{d.kind}:{d.mac or d.ip}", "kind": d.kind,
                    "name": d.model or d.user_name or d.mac,
                    "ip": d.ip, "subnet": d.subnet_mask, "mac": d.mac, "ipcfg": ipcfg,
                    "detail": " · ".join(x for x in (d.serial, d.user_name, d.vendor) if x),
                    # carried for the Set IP dialog, not shown as columns
                    "host_subnets": d.host_subnets, "host_masks": d.host_masks,
                    "persistent_available": d.persistent_available,
                })

            table.update_rows(rows)
            _show_raw("--- ebus_discover ---\n" + res.raw_output)
            found = f"Found {len(rows)} camera(s)"
            scan_status.set_text(found + (" — " + res.error if res.error else ""))

            # Re-select the remembered camera of whichever driver has one listed.
            remembered = {app.storage.general.get("gox_target_ip", ""),
                          app.storage.general.get("fx10_target_ip", _FX10_DEFAULT_IP)}
            for row in rows:
                if row["ip"] in remembered and row["ip"]:
                    table.selected = [row]
                    break
            refresh_guard()

        def _on_select() -> None:
            row = _selected_row()
            if row.get("kind") == "GoX":
                app.storage.general["gox_target_ip"] = row.get("ip", "")
                app.storage.general["gox_target_mac"] = row.get("mac", "")
            elif row.get("kind") == "FX10":
                app.storage.general["fx10_target_ip"] = row.get("ip", "")
                app.storage.general["fx10_target_mac"] = row.get("mac", "")
            refresh_guard()

        table.on("selection", lambda _: _on_select())

        # --------------------------------------------------------- Set IP glue
        async def _set_ip_dialog() -> None:
            row = _selected_row()
            if not row:
                ui.notify("Scan and select a camera first", type="warning")
                return
            reason = ebus_tools.guard_reason_for(row.get("kind", ""))
            if reason or STATE.snapshot_busy:
                ui.notify(reason or "Another camera tool is still running", type="warning")
                return
            kind = row.get("kind", "")
            host_subnets: list[str] = row.get("host_subnets") or []
            host_masks: list[str] = row.get("host_masks") or []
            default_mask = host_masks[0] if host_masks else (row.get("subnet") or "255.255.255.0")

            with ui.dialog() as dialog, ui.card().classes("w-[36rem]"):
                ui.label(f"Set IP — {row.get('name', '')} ({kind})").classes("text-lg font-bold")
                ui.label(f"MAC {row.get('mac', '')} · now {row.get('ip', '')}/{row.get('subnet', '')} · "
                         f"{row.get('ipcfg', '')}").classes("text-sm text-gray-700")
                ui.label("Host NIC: " + (", ".join(host_subnets) or "no IPv4 address")).classes("text-sm text-gray-700")
                ui.label("Persistent IP: " + ("supported by this camera" if row.get("persistent_available")
                                              else "NOT advertised — the change may stay transient")
                         ).classes("text-sm text-gray-700")
                ip_in = ui.input("New IP", value=row.get("ip", "")).classes("w-full")
                mask_in = ui.input("Subnet mask", value=default_mask).classes("w-full")
                with ui.expansion("Advanced").classes("w-full"):
                    gw_in = ui.input("Gateway", value="0.0.0.0").classes("w-full")
                    allow_cb = ui.checkbox("Allow an address outside the host subnet (camera becomes "
                                           "unreachable until the host is re-addressed)", value=False)
                writeback_cb = None
                if kind in _CONFIG_KINDS:
                    writeback_cb = ui.checkbox(
                        f"Also update device.ip in the selected {kind} driver config", value=True)
                ui.label("Two steps: FORCEIP (immediate, lost on power cycle), then the persistent-IP nodes "
                         "(kept across power cycles). Never run this on a camera that is recording."
                         ).classes("text-xs text-gray-500")
                with ui.row():
                    ui.button("Set IP", color="negative", on_click=lambda: dialog.submit(True))
                    ui.button("Cancel", on_click=lambda: dialog.submit(False)).props("flat")
            if not await dialog:
                return

            new_ip = str(ip_in.value or "").strip()
            mask = str(mask_in.value or "").strip()
            gateway = str(gw_in.value or "0.0.0.0").strip() or "0.0.0.0"
            allow_foreign = bool(allow_cb.value)
            problem = _check_ip_inputs(new_ip, mask, gateway, host_subnets, allow_foreign)
            if problem:
                ui.notify(problem, type="negative", multi_line=True)
                return
            if new_ip == row.get("ip") and mask == row.get("subnet"):
                ui.notify("That is already the camera's address", type="info")
                return
            await _run_set_ip(row, new_ip, mask, gateway, allow_foreign,
                              bool(writeback_cb.value) if writeback_cb is not None else False)

        async def _run_set_ip(row: dict, new_ip: str, mask: str, gateway: str, allow_foreign: bool,
                              writeback: bool) -> None:
            if STATE.snapshot_busy:
                return
            kind = row.get("kind", "")
            set_ip_busy.classes(remove="hidden")
            refresh_guard()
            try:
                result = await ebus_tools.set_ip(row.get("mac", ""), new_ip, mask, gateway, allow_foreign,
                                                kind=kind)
            except Exception as e:
                ui.notify(f"Set IP was not completed: {e}", type="negative", multi_line=True)
                return
            finally:
                set_ip_busy.classes(add="hidden")
                refresh_guard()
            _show_raw(result.raw_output)

            if result.ok:
                ui.notify(result.message, type="positive", multi_line=True)
                if writeback:
                    try:
                        summary = config_store.apply_device_ip(kind, row.get("ip", ""), row.get("mac", ""), new_ip)
                    except Exception as e:
                        ui.notify(f"Camera re-addressed, but the config write failed: {e}",
                                  type="negative", multi_line=True)
                    else:
                        _after_apply(summary)
            elif result.transient_only:
                # The config follows the persistent state only: a transient address
                # would leave it pointing at nothing after the next power cycle.
                ui.notify(f"Address changed but NOT persisted — a power cycle restores {row.get('ip', '')}; "
                          f"the driver config was left unchanged. {result.message}",
                          type="warning", multi_line=True)
            else:
                ui.notify(f"Set IP failed: {result.message}", type="negative", multi_line=True)
                return

            # Remember the new address so the rescan re-selects this camera.
            if kind == "GoX":
                app.storage.general["gox_target_ip"] = new_ip
            elif kind == "FX10":
                app.storage.general["fx10_target_ip"] = new_ip
            await _scan()

        # ------------------------------------------------------------ GoX glue
        async def _gox_snap() -> None:
            if STATE.snapshot_busy:
                return
            reason = gox_tools.guard_reason()
            ip, _mac = _gox_target()
            if reason or not ip:
                ui.notify(reason or "Select a target GoX camera first", type="warning")
                return
            gox_busy.classes(remove="hidden")
            refresh_guard()
            try:
                app.storage.general["gox_exposure_ms"] = gox_exposure.value
                app.storage.general["gox_gain"] = gox_gain.value
                result = await gox_tools.snapshot(ip, float(gox_exposure.value), float(gox_gain.value))
                _gox_show(result)
            except Exception as e:
                ui.notify(f"Snapshot was not completed: {e}", type="negative", multi_line=True)
            finally:
                gox_busy.classes(add="hidden")
                refresh_guard()

        def _gox_show(result: gox_tools.SnapshotResult) -> None:
            _show_raw(result.raw_output)
            if not result.ok:
                ui.notify(f"Snapshot failed: {result.reason}", type="negative", multi_line=True)
                gox_meta.set_text(f"Failed ({result.elapsed_s:.1f}s): {result.reason}")
                return
            gox_image.set_source(f"data:image/jpeg;base64,{result.jpeg_b64}")
            gox_chart.options["series"][0]["data"] = result.histogram
            gox_chart.options["xAxis"]["data"] = list(range(len(result.histogram)))
            gox_chart.update()
            request = result.request
            requested = (f"target {request.target_ip} · requested exposure {request.exposure_ms:g}ms · "
                         f"gain x{request.gain:g} · " if request else "request metadata unavailable · ")
            gox_meta.set_text(
                f"{result.decode_name} · {requested}"
                f"mean {result.mean_16 / 65535 * 100:.1f}% · clipped {result.clipped_pct:.2f}% · "
                f"{result.elapsed_s:.1f}s"
            )
            gox_warn.set_text("Incomplete frame (packet loss? check MTU/rmem)" if result.incomplete else "")

        def _gox_apply() -> None:
            ip, mac = _gox_target()
            if not ip and not mac:
                ui.notify("Select a target GoX camera first", type="warning")
                return
            try:
                summary = config_store.apply_gox_acquisition(
                    ip, mac, float(gox_exposure.value), float(gox_gain.value))
            except Exception as e:
                ui.notify(f"Apply failed: {e}", type="negative", multi_line=True)
                return
            _after_apply(summary)

        # ----------------------------------------------------------- FX10 glue
        async def _fx10_snap() -> None:
            if STATE.snapshot_busy:
                return
            reason = fx10_tools.guard_reason()
            ip = _fx10_target()
            if reason or not ip:
                ui.notify(reason or "Select a target FX10 camera first", type="warning")
                return
            fx10_busy.classes(remove="hidden")
            refresh_guard()
            try:
                app.storage.general["fx10_exposure_ms"] = fx10_exposure.value
                app.storage.general["fx10_spatial_binning"] = fx10_spatial.value
                app.storage.general["fx10_spectral_binning"] = fx10_spectral.value
                result = await fx10_tools.snapshot(
                    ip, float(fx10_exposure.value),
                    int(fx10_spatial.value), int(fx10_spectral.value))
                _fx10_show(result)
            except Exception as e:
                ui.notify(f"Snapshot was not completed: {e}", type="negative", multi_line=True)
            finally:
                fx10_busy.classes(add="hidden")
                refresh_guard()

        def _fx10_show(result: fx10_tools.SnapshotResult) -> None:
            _show_raw(result.raw_output)
            if not result.ok:
                ui.notify(f"Snapshot failed: {result.reason}", type="negative", multi_line=True)
                fx10_meta.set_text(f"Failed ({result.elapsed_s:.1f}s): {result.reason}")
                return
            axis = result.wavelengths_nm or list(range(result.bands))
            fx10_chart.options["xAxis"]["name"] = "Wavelength (nm)" if result.wavelengths_nm else "Image row (uncalibrated)"
            fx10_chart.options["series"] = [
                {"name": name, "type": "line", "showSymbol": False, "animation": False,
                 "data": [[x, v] for x, v in zip(axis, values)]}
                for name, values in result.spectrum_pct.items()
            ]
            fx10_chart.update()
            request = result.request
            requested = (f"target {request.target_ip} · requested exposure {request.exposure_ms:g}ms · "
                         f"binning {request.spatial_binning}×{request.spectral_binning} · "
                         if request else "request metadata unavailable · ")
            fx10_meta.set_text(
                f"{result.lines} frames (~1s) · {result.bands} bands · {result.samples} px/line · "
                f"{requested}"
                f"mean {result.mean_pct:.1f}% · clipped {result.clipped_pct:.2f}% · {result.elapsed_s:.1f}s"
            )

        def _fx10_apply() -> None:
            try:
                summary = config_store.apply_fx10_acquisition(
                    float(fx10_exposure.value), int(fx10_spatial.value), int(fx10_spectral.value))
            except Exception as e:
                ui.notify(f"Apply failed: {e}", type="negative", multi_line=True)
                return
            _after_apply(summary)

        def _after_apply(summary: str) -> None:
            """Same contract as the dashboard's Enable switches: the running
            binary already loaded its config, so a write lands on the NEXT start."""
            if STATE.process_state in (ProcState.RUNNING, ProcState.STARTING, ProcState.STOPPING):
                STATE.pending_config_notice = True
                ui.notify(f"Saved ({summary}) — takes effect on the next recording start",
                          type="info", multi_line=True)
            else:
                ui.notify(f"Saved to {summary}", type="positive", multi_line=True)
