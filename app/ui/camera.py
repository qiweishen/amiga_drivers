"""Camera Tools: one discovery table for both camera drivers, then a per-driver
exposure workbench (preview + Apply to the recording config).

Discovery is shared because the two backends answer the same question about the
same GigE segment — `jai_discover --json` for the GoX cameras and
`fx10_snapshot --list` for the FX10 — so one Scan fills one table and selecting
a row targets that camera's own section.

Both sections tune AGAINST A LIVE CAMERA without touching any config: the
snapshot tools take the values as CLI overrides. "Apply to config" is the
separate, explicit step that writes the winning values into the driver's
recording config (comment-preserving, see services/config_store.py).
"""

from __future__ import annotations

import asyncio

from nicegui import app, ui

from ..constants import TOOL_TICK_S
from ..services import config_store, fx10_tools, gox_tools
from ..state import STATE, ProcState
from . import layout

_COLUMNS = [
    {"name": "kind", "label": "Driver", "field": "kind", "align": "left"},
    {"name": "name", "label": "Model / device", "field": "name", "align": "left"},
    {"name": "ip", "label": "IP", "field": "ip", "align": "left"},
    {"name": "mac", "label": "MAC", "field": "mac", "align": "left"},
    {"name": "detail", "label": "Details", "field": "detail", "align": "left"},
]

_FX10_DEFAULT_IP = "10.95.0.100"
_BINNING = [1, 2, 4, 8]


def _escape(text: str) -> str:
    return text.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


@ui.page("/camera")
def camera_page() -> None:
    with layout.frame("Camera Tools"):
        # =================================================== device discovery
        ui.label("Device discovery (GoX and FX10)").classes("text-lg font-bold")
        ui.label("The device whose recording driver is running is skipped, "
                 "as it is owned by the acquisition process.").classes("text-sm text-gray-700 whitespace-pre-line")
        with ui.row().classes("items-center gap-4"):
            scan_btn = ui.button("Scan cameras", icon="search", on_click=lambda: _scan())
            scan_status = ui.label("").classes("text-sm text-gray-600")
        table = ui.table(columns=_COLUMNS, rows=[], row_key="key", selection="single").classes("w-full")
        fallback_pre = ui.element("pre").classes(
            "w-full text-xs font-mono bg-gray-100 p-2 rounded hidden whitespace-pre-wrap")

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
                ui.number(min=0.001, max=1_000_000, step=0.001, suffix="ms").bind_value(gox_exposure).classes("w-full")
            with ui.column().classes("flex-1 min-w-0"):
                ui.label("Gain (dB)")
                ui.label("Camera limit: 1.0 ... 126.0 dB on the GOX-12405C-PGE").classes("text-xs text-gray-500")
                # Camera limit: ExposureTime is 1.0..126.0 dB on the GOX-12405C-PGE
                gox_gain = ui.slider(min=1.0, max=126.0, step=0.1,
                                     value=min(126.0, app.storage.general.get("gox_gain", 1.0)))
                ui.number(min=1.0, max=126.0, step=0.1, suffix="dB").bind_value(gox_gain).classes("w-full")
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
                ui.label("448 / 224 / 112 / 56 bands").classes("text-xs text-gray-500")
                fx10_spectral = ui.select(_BINNING, value=app.storage.general.get("fx10_spectral_binning", 1)).props(
                    "outlined dense")
        with ui.row().classes("items-center gap-4"):
            fx10_snap_btn = ui.button("Take snapshot", icon="photo_camera", on_click=lambda: _fx10_snap())
            fx10_apply_btn = ui.button("Apply to config", icon="save", on_click=lambda: _fx10_apply()).props("outline")
            fx10_busy = ui.spinner(size="sm").classes("hidden")
            fx10_meta = ui.label("").classes("text-sm text-gray-600")
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

        def _selected(kind: str) -> dict:
            """The selected table row when it belongs to `kind`, else {}."""
            sel = table.selected
            if sel and sel[0].get("kind") == kind:
                return sel[0]
            return {}

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
            # Discovery needs at least one driver to be free.
            scan_btn.set_enabled(STATE.env_ok and not busy and not (gox_reason and fx10_reason))
            gox_snap_btn.set_enabled(gox_reason is None and STATE.env_ok and not busy and bool(gox_ip))
            fx10_snap_btn.set_enabled(fx10_reason is None and STATE.env_ok and not busy and bool(fx10_ip))
            # Apply only edits a file — it never touches the camera, so it stays
            # available while recording (it takes effect on the next start).
            gox_apply_btn.set_enabled(bool(gox_ip))
            gox_target_label.set_text(
                f"Target camera: {gox_ip or '(Scan and select a GoX camera first)'}")
            fx10_target_label.set_text(
                f"Target camera: {fx10_ip or '(Scan and select a FX10 camera first)'}")

        ui.timer(TOOL_TICK_S, refresh_guard)
        refresh_guard()

        # ------------------------------------------------------ discovery glue
        async def _scan() -> None:
            scan_status.set_text("Scanning ...")
            fallback_pre.classes(add="hidden")
            gox_reason = gox_tools.guard_reason()
            fx10_reason = fx10_tools.guard_reason()

            async def _skip(reason: str):
                return reason

            gox_res, fx10_res = await asyncio.gather(
                _skip(gox_reason) if gox_reason else gox_tools.discover(),
                _skip(fx10_reason) if fx10_reason else fx10_tools.discover(),
            )

            rows: list[dict] = []
            notes: list[str] = []
            raw_parts: list[str] = []

            if isinstance(gox_res, str):
                notes.append("GoX skipped (driver owns the cameras)")
            else:
                raw_parts.append("--- jai_discover ---\n" + gox_res.raw_output)
                if gox_res.error:
                    notes.append(gox_res.error)
                if not gox_res.json_supported:
                    fallback_pre.classes(remove="hidden")
                    fallback_pre.clear()
                    with fallback_pre:
                        ui.html(f"<span>{_escape(gox_res.raw_output)}</span>")
                    notes.append("old jai_discover binary (no --json): rebuild and retry; raw output below")
                rows += [
                    {
                        "key": f"GoX:{d.mac or d.ip}", "kind": "GoX", "name": d.model,
                        "ip": d.ip, "mac": d.mac,
                        "detail": f"{d.serial} · {d.user_name}"
                                  + (
                                      "" if d.config_valid else " · INVALID-SUBNET (unreachable: fix camera/NIC subnet)"),
                    }
                    for d in gox_res.devices
                ]

            if isinstance(fx10_res, str):
                notes.append("FX10 skipped (driver owns the camera)")
            else:
                raw_parts.append("--- fx10_snapshot --list ---\n" + fx10_res.raw_output)
                if fx10_res.error:
                    notes.append(fx10_res.error)
                rows += [
                    {
                        "key": f"FX10:{d.mac or d.ip}", "kind": "FX10", "name": d.display_id,
                        "ip": d.ip, "mac": d.mac, "detail": d.connection_id,
                    }
                    for d in fx10_res.devices
                ]

            table.update_rows(rows)
            _show_raw("\n\n".join(raw_parts))
            found = f"Found {len(rows)} camera(s)"
            scan_status.set_text(found + (" — " + "; ".join(notes) if notes else ""))

            # Re-select the remembered camera of whichever driver has one listed.
            remembered = {app.storage.general.get("gox_target_ip", ""),
                          app.storage.general.get("fx10_target_ip", _FX10_DEFAULT_IP)}
            for row in rows:
                if row["ip"] in remembered and row["ip"]:
                    table.selected = [row]
                    break
            refresh_guard()

        def _on_select() -> None:
            row = table.selected[0] if table.selected else {}
            if row.get("kind") == "GoX":
                app.storage.general["gox_target_ip"] = row.get("ip", "")
                app.storage.general["gox_target_mac"] = row.get("mac", "")
            elif row.get("kind") == "FX10":
                app.storage.general["fx10_target_ip"] = row.get("ip", "")
            refresh_guard()

        table.on("selection", lambda _: _on_select())

        # ------------------------------------------------------------ GoX glue
        async def _gox_snap() -> None:
            if STATE.snapshot_busy:
                return
            reason = gox_tools.guard_reason()
            ip, _mac = _gox_target()
            if reason or not ip:
                ui.notify(reason or "Select a target GoX camera first", type="warning")
                return
            STATE.snapshot_busy = True
            gox_busy.classes(remove="hidden")
            refresh_guard()
            try:
                app.storage.general["gox_exposure_ms"] = gox_exposure.value
                app.storage.general["gox_gain"] = gox_gain.value
                result = await gox_tools.snapshot(ip, float(gox_exposure.value), float(gox_gain.value))
                _gox_show(result)
            finally:
                STATE.snapshot_busy = False
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
            gox_meta.set_text(
                f"{result.decode_name} · exposure {gox_exposure.value:.0f}ms · gain {gox_gain.value:.1f}dB · "
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
            _after_apply(f"config-gox.yaml · {summary}")

        # ----------------------------------------------------------- FX10 glue
        async def _fx10_snap() -> None:
            if STATE.snapshot_busy:
                return
            reason = fx10_tools.guard_reason()
            ip = _fx10_target()
            if reason or not ip:
                ui.notify(reason or "Select a target FX10 camera first", type="warning")
                return
            STATE.snapshot_busy = True
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
            finally:
                STATE.snapshot_busy = False
                fx10_busy.classes(add="hidden")
                refresh_guard()

        def _fx10_show(result: fx10_tools.SnapshotResult) -> None:
            _show_raw(result.raw_output)
            if not result.ok:
                ui.notify(f"Snapshot failed: {result.reason}", type="negative", multi_line=True)
                fx10_meta.set_text(f"Failed ({result.elapsed_s:.1f}s): {result.reason}")
                return
            fx10_chart.options["series"] = [
                {"name": name, "type": "line", "showSymbol": False, "animation": False,
                 "data": [[wl, v] for wl, v in zip(result.wavelengths_nm, values)]}
                for name, values in result.spectrum_pct.items()
            ]
            fx10_chart.update()
            fx10_meta.set_text(
                f"{result.lines} frames (~1s) · {result.bands} bands · {result.samples} px/line · "
                f"exposure {fx10_exposure.value:.1f}ms · binning {fx10_spatial.value}×{fx10_spectral.value} · "
                f"mean {result.mean_pct:.1f}% · clipped {result.clipped_pct:.2f}% · {result.elapsed_s:.1f}s"
            )

        def _fx10_apply() -> None:
            try:
                summary = config_store.apply_fx10_acquisition(
                    float(fx10_exposure.value), int(fx10_spatial.value), int(fx10_spectral.value))
            except Exception as e:
                ui.notify(f"Apply failed: {e}", type="negative", multi_line=True)
                return
            _after_apply(f"config-fx10.yaml · {summary}")

        def _after_apply(summary: str) -> None:
            """Same contract as the dashboard's Enable switches: the running
            binary already loaded its config, so a write lands on the NEXT start."""
            if STATE.process_state in (ProcState.RUNNING, ProcState.STARTING, ProcState.STOPPING):
                STATE.pending_config_notice = True
                ui.notify(f"Saved ({summary}) — takes effect on the next recording start",
                          type="info", multi_line=True)
            else:
                ui.notify(f"Saved to {summary}", type="positive", multi_line=True)
