"""FX10 tools page: eBUS discovery + freerun waterfall preview with exposure tuning."""

from __future__ import annotations

from nicegui import app, ui

from ..constants import TOOL_TICK_S
from ..services import fx10_tools
from ..state import STATE
from . import layout

_COLUMNS = [
    {"name": "display_id", "label": "Device", "field": "display_id", "align": "left"},
    {"name": "ip", "label": "IP", "field": "ip", "align": "left"},
    {"name": "mac", "label": "MAC", "field": "mac", "align": "left"},
]

_DEFAULT_IP = "10.95.76.105"


@ui.page("/fx10")
def fx10_page() -> None:
    with layout.frame("FX10 Tools"):
        guard_label = ui.label("").classes("text-sm text-red-700")

        # ------------------------------------------------------------ discover
        ui.label("Device discovery (fx10_snapshot --list)").classes("text-lg font-bold")
        with ui.row().classes("items-center gap-4"):
            scan_btn = ui.button("Scan cameras", icon="search", on_click=lambda: _scan())
            scan_status = ui.label("").classes("text-sm text-gray-600")
        table = ui.table(columns=_COLUMNS, rows=[], row_key="display_id",
                         selection="single").classes("w-full")

        # ------------------------------------------------------------ snapshot
        ui.separator()
        ui.label("Freerun spectral preview / exposure tuning (fx10_snapshot)").classes("text-lg font-bold")
        target_label = ui.label().classes("text-sm text-gray-600")
        with ui.row().classes("items-center gap-8 w-full"):
            with ui.column().classes("w-72"):
                ui.label("Exposure (ms)")
                # Camera limit: ExposureTime is 10..419000 µs on the FX10e
                exposure = ui.slider(min=0.1, max=419.0, step=0.1,
                                     value=min(419.0, app.storage.general.get("fx10_exposure_ms", 5.0)))
                exposure_num = ui.number(min=0.1, max=419.0, step=0.1, suffix="ms") \
                    .bind_value(exposure)
        ui.label("Preview always runs in freerun — the PPS/external trigger path is not "
                 "exercised; unlicensed machines watermark the pixels.").classes("text-sm text-gray-600")
        with ui.row().classes("items-center gap-4"):
            snap_btn = ui.button("Take snapshot", icon="photo_camera", on_click=lambda: _snap_once())
            busy = ui.spinner(size="sm").classes("hidden")
            meta_label = ui.label("").classes("text-sm text-gray-600")

        # Per-band statistics over ~1 s of frames: x = wavelength, y = % of full scale
        chart = ui.echart({
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

        # ---------------------------------------------------------------- glue
        def _target_ip() -> str:
            sel = table.selected
            if sel:
                return sel[0].get("ip", "")
            return app.storage.general.get("fx10_target_ip", _DEFAULT_IP)

        def refresh_guard() -> None:
            # Pure state reads, and NiceGUI drops setter calls that change
            # nothing — an idle tick sends nothing however often it runs.
            reason = fx10_tools.guard_reason()
            guard_label.set_text(reason or "")
            allowed = reason is None and STATE.env_ok and not STATE.snapshot_busy
            scan_btn.set_enabled(reason is None and STATE.env_ok and not STATE.snapshot_busy)
            snap_btn.set_enabled(allowed and bool(_target_ip()))
            target_label.set_text(
                f"Target camera: {_target_ip() or '(scan and select a camera first)'}"
            )

        ui.timer(TOOL_TICK_S, refresh_guard)
        refresh_guard()

        async def _scan() -> None:
            scan_status.set_text("Scanning…")
            result = await fx10_tools.discover()
            raw_pre.clear()
            with raw_pre:
                ui.html(f"<span>{_escape(result.raw_output)}</span>")
            if result.error:
                scan_status.set_text(result.error)
            rows = [
                {"display_id": d.display_id, "ip": d.ip, "mac": d.mac}
                for d in result.devices
            ]
            table.update_rows(rows)
            if not result.error:
                scan_status.set_text(f"Found {len(rows)} camera(s)")
            # auto-select the remembered camera
            remembered = app.storage.general.get("fx10_target_ip", _DEFAULT_IP)
            for row in rows:
                if row["ip"] == remembered:
                    table.selected = [row]
                    break

        def _on_select() -> None:
            ip = _target_ip()
            if ip:
                app.storage.general["fx10_target_ip"] = ip
            refresh_guard()

        table.on("selection", lambda _: _on_select())

        async def _snap_once() -> None:
            if STATE.snapshot_busy:
                return
            reason = fx10_tools.guard_reason()
            ip = _target_ip()
            if reason or not ip:
                ui.notify(reason or "Select a target camera first", type="warning")
                return
            STATE.snapshot_busy = True
            busy.classes(remove="hidden")
            snap_btn.set_enabled(False)
            try:
                app.storage.general["fx10_exposure_ms"] = exposure.value
                result = await fx10_tools.snapshot(ip, float(exposure.value))
                _show(result)
            finally:
                STATE.snapshot_busy = False
                busy.classes(add="hidden")
                refresh_guard()

        def _show(result: fx10_tools.SnapshotResult) -> None:
            raw_pre.clear()
            with raw_pre:
                ui.html(f"<span>{_escape(result.raw_output)}</span>")
            if not result.ok:
                ui.notify(f"Snapshot failed: {result.reason}", type="negative", multi_line=True)
                meta_label.set_text(f"Failed ({result.elapsed_s:.1f}s): {result.reason}")
                return
            chart.options["series"] = [
                {"name": name, "type": "line", "showSymbol": False, "animation": False,
                 "data": [[wl, v] for wl, v in zip(result.wavelengths_nm, values)]}
                for name, values in result.spectrum_pct.items()
            ]
            chart.update()
            meta_label.set_text(
                f"{result.lines} frames (~1s) · {result.bands} bands · {result.samples} px/line · "
                f"exposure {exposure.value:.1f}ms · mean {result.mean_pct:.1f}% · "
                f"clipped {result.clipped_pct:.2f}% · {result.elapsed_s:.1f}s"
            )

        # block "start acquisition" while a shot is in flight and FX10 enabled
        # (reverse guard lives in process.preflight via STATE.snapshot_busy)


def _escape(text: str) -> str:
    return text.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")
