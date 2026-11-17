"""Camera Tools: one discovery table for both cameras, Set IP, the GoX and FX10
exposure workbenches, and the GoX and FX10 focus assists.

Discovery is a GigE broadcast, so it also runs during a recording; a camera the
recording owns is marked "in use" and its snapshot and Set IP stay disabled.
Workbench values reach the snapshot tools as command-line overrides; only
"Apply to config" writes them into the recording and snapshot configs.
"""

from __future__ import annotations

import ipaddress
from collections.abc import Callable
from nicegui import app, ui

from . import fx10_focus as fx10_focus_ui, fx10_plot, gox_focus as gox_focus_ui, layout
from .components import NEXT_RUN, PREVIEW_DETAILS, heading, hint, info, preview_image
from ..constants import TOOL_TICK_S
from ..services import (camera_operations, config_store, ebus_tools, fx10_focus, fx10_tools, gox_focus, gox_tools,
                        preview_images, timing)
from ..state import STATE, ProcState

_COLUMNS = [
    {"name": "kind", "label": "Driver", "field": "kind", "align": "left"},
    {"name": "name", "label": "Model / device", "field": "name", "align": "left"},
    {"name": "ip", "label": "IP", "field": "ip", "align": "left"},
    {"name": "subnet", "label": "Subnet mask", "field": "subnet", "align": "left"},
    {"name": "mac", "label": "MAC", "field": "mac", "align": "left"},
    {"name": "ipcfg", "label": "IP config", "field": "ipcfg", "align": "left"},
    {"name": "detail", "label": "Details", "field": "detail", "align": "left"},
]

_BINNING = [1, 2, 4, 8]


class _ExposureRate:
    """Exposure slider + number, and a Frame rate field whose maximum follows the exposure.

    The rate starts at the highest value at which the exposure uses at most 90 % of
    the frame period (``max_rate``); a rate the operator lowered is kept while it
    stays below that maximum.
    """

    def __init__(self, model: str, low: float, high: float, exposure_ms: float, rate_floor: float,
                 rate_ceiling: float, max_rate: Callable[[float], float]) -> None:
        ui.label("Exposure (ms)").classes("font-medium")
        ui.label(f"{model} · {low:g}–{high:g} ms").classes("text-xs muted")
        self.exposure = ui.slider(min=low, max=high, step=0.001, value=min(high, exposure_ms))
        with ui.row().classes("w-full items-start gap-2 no-wrap"):
            ui.number("Exposure", min=low, max=high, step=0.001, suffix="ms").bind_value(
                self.exposure).classes("flex-1 min-w-0")
            self.rate = ui.number("Frame rate", min=rate_floor, max=rate_ceiling, step=timing.RATE_STEP_HZ,
                                  precision=3, suffix="Hz").classes("flex-1 min-w-0")
        self._hint = ui.label("").classes("text-xs muted")
        self._floor, self._max_rate, self._auto = rate_floor, max_rate, None
        self.exposure.on_value_change(lambda _event: self.sync())

    def sync(self, restore: float | None = None) -> None:
        try:
            auto = self._max_rate(float(self.exposure.value))
        except (TypeError, ValueError):
            return
        previous, current = self._auto, self.rate.value
        self._auto = auto
        self.rate.max = auto
        if restore is not None and self._floor <= restore <= auto:
            self.rate.set_value(restore)
        elif current is None or previous is None or current > auto or abs(float(current) - previous) < 1e-9:
            self.rate.set_value(auto)
        self._hint.set_text(f"Max {auto:.3f} Hz · exposure ≤ {timing.PERIOD_FRACTION:.0%} of period")


def _kind_guard(kind: str) -> str | None:
    """Why the discovery row's camera must not be touched now, or None."""
    return camera_operations.guard_reason(camera_operations.DRIVER_OF_KIND.get(kind))


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
        return (f"{ip}/{network.prefixlen} is outside the host subnet "
                f"({', '.join(host_subnets) or 'no IPv4'}); see Advanced to allow it")
    return ""


@ui.page("/camera")
def camera_page() -> None:
    with layout.frame("Camera Tools"):
        # =================================================== device discovery
        with ui.column().classes("console-panel w-full"):
            with heading("Device discovery", icon="lan"):
                ui.label("GoX + FX10 · GigE Vision").classes("text-xs mono muted")
            hint("Scan, then select a camera for its workbench.",
                 "A camera owned by the running recording is marked \"in use\"; its snapshot and Set IP "
                 "stay disabled.")
            with ui.row().classes("console-toolbar"):
                scan_btn = ui.button("Scan cameras", icon="search", on_click=lambda: _scan())
                set_ip_btn = ui.button("Set IP…", icon="lan", on_click=lambda: _set_ip_dialog()).props("outline")
                set_ip_busy = ui.spinner(size="sm").classes("hidden")
                scan_status = ui.label("").classes("text-sm muted")
            table = ui.table(columns=_COLUMNS, rows=[], row_key="key", selection="single").classes("w-full")
            table.add_slot("no-data",
                           '<div class="section-copy">No cameras listed. Use Scan cameras to refresh the list.</div>')

        with ui.element("div").classes("workspace-grid"):
            # ==================================================== GoX section
            with ui.column().classes("console-panel w-full min-w-0"):
                heading("GoX · Exposure workbench", icon="photo_camera")
                hint("Snapshots use these values temporarily.",
                     "Apply to config saves exposure, frame rate and gain to the recording and snapshot "
                     "configs, used by the next recording and reference capture.")
                gox_target_label = ui.label().classes("text-sm mono break-all")
                gox_guard = ui.label("").classes("text-sm text-red-700")
                with ui.element("div").classes("control-grid w-full"):
                    with ui.column().classes("w-full min-w-0 gap-2"):
                        gox_timing = _ExposureRate(
                            "GOX-12405C-PGE", timing.GOX_MIN_EXPOSURE_MS, timing.GOX_MAX_EXPOSURE_MS,
                            app.storage.general.get("gox_exposure_ms", 50.0),
                            timing.GOX_MIN_FRAME_RATE_HZ, timing.GOX_MAX_FRAME_RATE_HZ, timing.gox_max_frame_rate_hz)
                        gox_timing.sync(restore=app.storage.general.get("gox_frame_rate_hz"))
                        gox_exposure, gox_rate = gox_timing.exposure, gox_timing.rate
                    with ui.column().classes("w-full min-w-0 gap-2"):
                        ui.label("Gain (magnification)").classes("font-medium")
                        # Gain[AnalogAll] is a MAGNIFICATION on the GO-X, x1.0 .. x126.0
                        # (about 0..42 dB) - manual p.148, dB conversion table p.181.
                        ui.label("x1.0–x126.0 · about 0–42 dB").classes("text-xs muted")
                        gox_gain = ui.slider(min=1.0, max=126.0, step=0.1,
                                             value=min(126.0, app.storage.general.get("gox_gain", 1.0)))
                        ui.number(min=1.0, max=126.0, step=0.1, prefix="x").bind_value(gox_gain).classes("w-full")
                with ui.row().classes("console-toolbar"):
                    gox_snap_btn = ui.button("Take snapshot", icon="photo_camera", on_click=lambda: _gox_snap())
                    gox_apply_btn = ui.button("Apply to config", icon="save", on_click=lambda: _gox_apply()).props(
                        "outline")
                    gox_busy = ui.spinner(size="sm").classes("hidden")
                gox_warn = ui.label("").classes("text-sm text-amber-700")
                gox_image = preview_image("photo_camera", "No snapshot displayed")
                with ui.row().classes("w-full items-center gap-1") as gox_download_row:
                    gox_download = ui.link("Download full-resolution 16-bit PNG", "#").classes("text-xs")
                    info(PREVIEW_DETAILS)
                gox_download_row.set_visibility(False)
                gox_urls: list[str] = []

                def _release_gox_urls() -> None:
                    preview_images.release(*gox_urls)
                    gox_urls.clear()

                ui.context.client.on_delete(_release_gox_urls)
                gox_meta = ui.label("").classes("text-sm muted mono break-all")
                with ui.column().classes("plot-surface w-full"):
                    ui.label("Bayer photosite distribution · native DN 0–4095").classes("text-xs muted")
                    gox_chart = ui.echart({
                        "xAxis": {"type": "category", "show": False},
                        "yAxis": {"type": "value", "show": False},
                        "grid": {"left": 4, "right": 4, "top": 4, "bottom": 4},
                        "series": [
                            {"type": "bar", "data": [], "barCategoryGap": "0%"}
                        ],
                        "tooltip": {},
                    }).classes("w-full h-40")

            # =================================================== FX10 section
            with ui.column().classes("console-panel w-full min-w-0"):
                heading("FX10 · Spectral workbench", icon="ssid_chart")
                hint("Snapshots use these values temporarily.",
                     "Apply to config saves exposure, frame rate and both binnings to the recording and "
                     "snapshot configs, used by the next recording and reference capture.")
                fx10_target_label = ui.label().classes("text-sm mono break-all")
                fx10_guard = ui.label("").classes("text-sm text-red-700")
                with ui.element("div").classes("control-grid w-full"):
                    with ui.column().classes("w-full min-w-0 gap-2"):
                        fx10_timing = _ExposureRate(
                            "FX10e", timing.FX10_MIN_EXPOSURE_MS, timing.FX10_MAX_EXPOSURE_MS,
                            app.storage.general.get("fx10_exposure_ms", 50.0),
                            timing.FX10_MIN_FRAME_RATE_HZ, timing.FX10_MAX_FRAME_RATE_HZ_BINNED,
                            lambda exposure_ms: timing.fx10_max_frame_rate_hz(
                                exposure_ms, int(fx10_spatial.value), int(fx10_spectral.value)))
                        fx10_exposure, fx10_rate = fx10_timing.exposure, fx10_timing.rate
                    with ui.column().classes("w-full min-w-0 gap-2"):
                        ui.label("Spatial binning").classes("font-medium")
                        ui.label("1024 / 512 / 256 / 128 px wide").classes("text-xs muted")
                        fx10_spatial = ui.select(_BINNING,
                                                 value=app.storage.general.get("fx10_spatial_binning", 1)).props(
                            "outlined dense").classes("w-full")
                    with ui.column().classes("w-full min-w-0 gap-2"):
                        ui.label("Spectral binning").classes("font-medium")
                        ui.label("448 / 224 / 112 / 56 bands · 2 = factory").classes("text-xs muted")
                        # Default 2, like the driver: the factory offset / black-level /
                        # bad-pixel calibration was performed at 2x spectral binning.
                        fx10_spectral = ui.select(_BINNING,
                                                  value=app.storage.general.get("fx10_spectral_binning", 2)).props(
                            "outlined dense").classes("w-full")
                fx10_timing.sync(restore=app.storage.general.get("fx10_frame_rate_hz"))
                for control in (fx10_spatial, fx10_spectral):  # the binning sets the rate ceiling
                    control.on_value_change(lambda _event: fx10_timing.sync())
                with ui.row().classes("console-toolbar"):
                    fx10_snap_btn = ui.button("Take snapshot", icon="photo_camera", on_click=lambda: _fx10_snap())
                    fx10_apply_btn = ui.button("Apply to config", icon="save", on_click=lambda: _fx10_apply()).props(
                        "outline")
                    fx10_busy = ui.spinner(size="sm").classes("hidden")
                # Per-band raw DN over ~1 s; thresholds use the capture's PixelFormat.
                with ui.column().classes("plot-surface w-full"):
                    fx10_chart = ui.echart(fx10_plot.workbench_options()).classes("w-full h-96")
                    ui.label("No spectrum displayed").classes(
                        "empty-state w-full text-xs").bind_visibility_from(
                        fx10_chart.options, "series", backward=lambda series: not series)
                fx10_axis_note = ui.label("").classes("section-copy")
                fx10_meta = ui.label("").classes("text-sm muted mono break-all")
                ui.link("Collect white and dark references →", "/reference").classes("text-sm")

        # ============================================== GoX focus assist
        gox_focus_ui.focus_panel(request=lambda remember: _gox_focus_request(remember),
                                 target_ip=lambda: _gox_target()[0], show_raw=lambda text: _show_raw(text))

        # ============================================= FX10 focus assist
        fx10_focus_ui.focus_panel(request=lambda remember: _fx10_focus_request(remember),
                                  target_ip=lambda: _fx10_target(), show_raw=lambda text: _show_raw(text))

        with ui.expansion("Raw output of the last command", icon="terminal").classes("w-full"):
            raw_pre = ui.element("pre").classes("terminal-surface w-full text-xs font-mono whitespace-pre-wrap")

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
            return app.storage.general.get("fx10_target_ip", "")

        def refresh_guard() -> None:
            # Pure state reads, and NiceGUI drops setter calls that change
            # nothing — an idle tick sends nothing however often it runs.
            gox_reason = camera_operations.guard_reason("gox")
            fx10_reason = camera_operations.guard_reason("fx10")
            gox_guard.set_text(gox_reason or "")
            fx10_guard.set_text(fx10_reason or "")
            busy = STATE.snapshot_busy
            gox_ip, _gox_mac = _gox_target()
            fx10_ip = _fx10_target()
            row = _selected_row()
            # Discovery is a broadcast: it never needs the control channel.
            scan_btn.set_enabled(not busy)
            # Set IP touches the camera: gated like a snapshot, per the row's driver.
            set_ip_btn.set_enabled(bool(row) and not busy
                                   and _kind_guard(row.get("kind", "")) is None)
            gox_snap_btn.set_enabled(gox_reason is None and not busy and bool(gox_ip))
            fx10_snap_btn.set_enabled(fx10_reason is None and not busy and bool(fx10_ip))
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
                in_use = _kind_guard(d.kind) is not None
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
                          app.storage.general.get("fx10_target_ip", "")}
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
            reason = _kind_guard(row.get("kind", ""))
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
                    with ui.row().classes("hint-row"):
                        allow_cb = ui.checkbox("Allow an address outside the host subnet", value=False)
                        info("The camera stays unreachable until the host is re-addressed.")
                writeback_cb = None
                if kind in camera_operations.DRIVER_OF_KIND:
                    writeback_cb = ui.checkbox(f"Also update device.ip in the {kind} config", value=True)
                hint("Never run this on a recording camera.",
                     "Two steps: FORCEIP (immediate, lost on power cycle), then the persistent-IP nodes "
                     "(kept across power cycles).")
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
                ui.notify(f"Address not persisted; config unchanged. {result.message}",
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
        def _gox_timing_values() -> tuple[float, float]:
            """(exposure ms, frame rate Hz) after the same checks the driver applies."""
            exposure_ms = timing.gox_check_exposure(gox_exposure.value)
            if gox_rate.value is None:
                raise ValueError("Enter a frame rate")
            rate = timing.gox_check_frame_rate(gox_rate.value)
            if not timing.exposure_fits(exposure_ms, rate):
                raise ValueError(timing.period_error(exposure_ms, rate))
            return exposure_ms, rate

        def _gox_focus_request(remember: bool) -> gox_focus.FocusRequest:
            """Workbench camera, exposure and gain; the frame rate is always the focus maximum
            (BayerRG8, ≤ 9.078 Hz), independent of the workbench Frame rate field."""
            ip, _mac = _gox_target()
            if not ip:
                raise ValueError("Select a target GoX camera first")
            request = gox_focus.request_for(ip, gox_exposure.value, gox_gain.value)
            if remember:
                app.storage.general["gox_exposure_ms"] = gox_exposure.value
                app.storage.general["gox_gain"] = gox_gain.value
            return request

        async def _gox_snap() -> None:
            if STATE.snapshot_busy:
                return
            reason = camera_operations.guard_reason("gox")
            ip, _mac = _gox_target()
            if reason or not ip:
                ui.notify(reason or "Select a target GoX camera first", type="warning")
                return
            try:
                exposure_ms, rate = _gox_timing_values()
            except (TypeError, ValueError) as e:
                ui.notify(str(e), type="warning")
                return
            gox_busy.classes(remove="hidden")
            refresh_guard()
            try:
                app.storage.general["gox_exposure_ms"] = gox_exposure.value
                app.storage.general["gox_gain"] = gox_gain.value
                app.storage.general["gox_frame_rate_hz"] = rate
                result = await gox_tools.snapshot(ip, exposure_ms, float(gox_gain.value), rate)
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
            _release_gox_urls()
            gox_urls.extend((result.image_url, result.full_image_url))
            gox_image.set_source(result.image_url)
            gox_download.props["href"] = preview_images.download_url(result.full_image_url)
            gox_download_row.set_visibility(True)
            gox_chart.options["series"][0]["data"] = result.histogram
            gox_chart.options["xAxis"]["data"] = [index * 64 for index in range(len(result.histogram))]
            gox_chart.update()
            request = result.request
            requested = (f"target {request.target_ip} · requested exposure {request.exposure_ms:g}ms · "
                         + (f"{request.frame_rate_hz:g} Hz · " if request.frame_rate_hz is not None else "")
                         + f"gain x{request.gain:g} · " if request else "request metadata unavailable · ")
            gox_meta.set_text(
                f"{result.decode_name} · {requested}"
                f"mean {result.mean_dn:.1f} DN · clipped {result.clipped_pct:.2f}% · "
                f"{result.elapsed_s:.1f}s"
            )
            gox_warn.set_text("Incomplete frame (packet loss? check MTU/rmem)" if result.incomplete else "")

        async def _gox_apply() -> None:
            ip, mac = _gox_target()
            if not ip and not mac:
                ui.notify("Select a target GoX camera first", type="warning")
                return
            try:
                exposure_ms, rate = _gox_timing_values()
                summary = config_store.apply_gox_acquisition(ip, mac, exposure_ms, float(gox_gain.value), rate)
                app.storage.general["gox_frame_rate_hz"] = rate
            except Exception as e:
                ui.notify(f"Apply failed: {e}", type="negative", multi_line=True)
                return
            _after_apply(summary)

        # ----------------------------------------------------------- FX10 glue
        def _fx10_timing_values() -> tuple[float, int, int, float]:
            """(exposure ms, spatial, spectral binning, frame rate Hz) after the driver's checks."""
            exposure_ms = timing.fx10_check_exposure(fx10_exposure.value)
            spatial, spectral = int(fx10_spatial.value), int(fx10_spectral.value)
            if fx10_rate.value is None:
                raise ValueError("Enter a frame rate")
            rate = timing.fx10_check_frame_rate(fx10_rate.value, spatial, spectral)
            if not timing.exposure_fits(exposure_ms, rate):
                raise ValueError(timing.period_error(exposure_ms, rate))
            return exposure_ms, spatial, spectral, rate

        def _fx10_focus_request(remember: bool) -> fx10_focus.FocusRequest:
            """Workbench camera, exposure, binning and frame rate, after Take snapshot's checks."""
            ip = _fx10_target()
            if not ip:
                raise ValueError("Select a target FX10 camera first")
            exposure_ms, spatial, spectral, rate = _fx10_timing_values()
            if remember:
                app.storage.general["fx10_exposure_ms"] = fx10_exposure.value
                app.storage.general["fx10_spatial_binning"] = fx10_spatial.value
                app.storage.general["fx10_spectral_binning"] = fx10_spectral.value
                app.storage.general["fx10_frame_rate_hz"] = rate
            return fx10_focus.FocusRequest(ip, exposure_ms, spatial, spectral, rate)

        async def _fx10_snap() -> None:
            if STATE.snapshot_busy:
                return
            reason = camera_operations.guard_reason("fx10")
            ip = _fx10_target()
            if reason or not ip:
                ui.notify(reason or "Select a target FX10 camera first", type="warning")
                return
            try:
                exposure_ms, spatial, spectral, rate = _fx10_timing_values()
            except (TypeError, ValueError) as e:
                ui.notify(str(e), type="warning")
                return
            fx10_busy.classes(remove="hidden")
            refresh_guard()
            try:
                app.storage.general["fx10_exposure_ms"] = fx10_exposure.value
                app.storage.general["fx10_spatial_binning"] = fx10_spatial.value
                app.storage.general["fx10_spectral_binning"] = fx10_spectral.value
                app.storage.general["fx10_frame_rate_hz"] = rate
                result = await fx10_tools.snapshot(ip, exposure_ms, spatial, spectral, rate)
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
            fx10_chart.options.update(fx10_plot.workbench_options(
                result.spectrum_dn, full_scale=result.full_scale, bands=result.bands,
                wavelengths_nm=result.wavelengths_nm,
            ))
            fx10_chart.update()
            fx10_axis_note.set_text(fx10_plot.axis_note(result.wavelengths_nm))
            request = result.request
            requested = (f"target {request.target_ip} · requested exposure {request.exposure_ms:g}ms · "
                         + (f"{request.frame_rate_hz:g} Hz · " if request.frame_rate_hz is not None else "")
                         + f"binning {request.spatial_binning}×{request.spectral_binning} · "
                         if request else "request metadata unavailable · ")
            fx10_meta.set_text(
                f"{result.lines} frames (~1s) · {result.bands} bands · {result.samples} px/line · "
                f"{requested}"
                f"{result.pixel_format} · full scale {result.full_scale} DN · "
                f"mean {result.mean_dn:.1f} DN · clipped {result.clipped_pct:.2f}% · {result.elapsed_s:.1f}s"
            )

        async def _fx10_apply() -> None:
            try:
                exposure_ms, spatial, spectral, rate = _fx10_timing_values()
                summary = config_store.apply_fx10_acquisition(exposure_ms, spatial, spectral, rate)
                app.storage.general["fx10_frame_rate_hz"] = rate
            except Exception as e:
                ui.notify(f"Apply failed: {e}", type="negative", multi_line=True)
                return
            _after_apply(summary)

        def _after_apply(summary: str) -> None:
            recording = STATE.process_state in (ProcState.RUNNING, ProcState.STARTING, ProcState.STOPPING)
            if recording:
                STATE.pending_config_notice = True
            ui.notify(f"{NEXT_RUN} ({summary})", type="info" if recording else "positive", multi_line=True)
