"""FX10 tools page: eBUS discovery + freerun waterfall preview with exposure tuning."""

from __future__ import annotations

from nicegui import app, ui

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
        ui.label("Freerun waterfall preview / exposure tuning (fx10_snapshot)").classes("text-lg font-bold")
        target_label = ui.label().classes("text-sm text-gray-600")
        with ui.row().classes("items-center gap-8 w-full"):
            with ui.column().classes("w-72"):
                ui.label("Exposure (ms)")
                # Camera limit: ExposureTime is 10..419000 µs on the FX10e
                exposure = ui.slider(min=0.1, max=419.0, step=0.1,
                                     value=min(419.0, app.storage.general.get("fx10_exposure_ms", 5.0)))
                exposure_num = ui.number(min=0.1, max=419.0, step=0.1, suffix="ms") \
                    .bind_value(exposure)
            with ui.column().classes("w-72"):
                ui.label("Frames (waterfall lines)")
                frames = ui.slider(min=16, max=512, step=16,
                                   value=app.storage.general.get("fx10_frames", 64))
                frames_num = ui.number(min=16, max=512, step=16).bind_value(frames)
            with ui.column().classes("w-56"):
                ui.label("Band")
                band_sel = ui.select(["mean", "single band"],
                                     value=app.storage.general.get("fx10_band_sel", "mean"))
                band_num = ui.number(label="Band index", min=0, max=447, step=1,
                                     value=app.storage.general.get("fx10_band_index", 0))
        ui.label("Preview always runs in freerun — the PPS/external trigger path is not "
                 "exercised; unlicensed machines watermark the pixels.").classes("text-sm text-gray-600")
        with ui.row().classes("items-center gap-4"):
            snap_btn = ui.button("Take snapshot", icon="photo_camera", on_click=lambda: _snap_once())
            auto = ui.switch("Auto refresh")
            busy = ui.spinner(size="sm").classes("hidden")
            meta_label = ui.label("").classes("text-sm text-gray-600")

        with ui.row().classes("w-full gap-4 items-start"):
            image = ui.interactive_image().classes("max-w-[60%] border rounded")
            chart = ui.echart({
                "xAxis": {"type": "category", "show": False},
                "yAxis": {"type": "value", "show": False},
                "grid": {"left": 4, "right": 4, "top": 4, "bottom": 4},
                "series": [{"type": "bar", "data": [], "barCategoryGap": "0%"}],
                "tooltip": {},
            }).classes("w-80 h-40")

        with ui.expansion("Raw output of the last command", icon="terminal").classes("w-full"):
            raw_pre = ui.element("pre").classes("w-full text-xs font-mono whitespace-pre-wrap")

        # ---------------------------------------------------------------- glue
        def _target_ip() -> str:
            sel = table.selected
            if sel:
                return sel[0].get("ip", "")
            return app.storage.general.get("fx10_target_ip", _DEFAULT_IP)

        def _band_mode() -> str:
            if band_sel.value == "single band":
                return f"band:{int(band_num.value or 0)}"
            return "mean"

        def refresh_guard() -> None:
            reason = fx10_tools.guard_reason()
            guard_label.set_text(reason or "")
            allowed = reason is None and STATE.env_ok and not STATE.snapshot_busy
            scan_btn.set_enabled(reason is None and STATE.env_ok and not STATE.snapshot_busy)
            snap_btn.set_enabled(allowed and bool(_target_ip()))
            band_num.set_enabled(band_sel.value == "single band")
            target_label.set_text(
                f"Target camera: {_target_ip() or '(scan and select a camera first)'}"
            )
            if reason and auto.value:
                auto.set_value(False)

        ui.timer(1.0, refresh_guard)
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
                app.storage.general["fx10_frames"] = frames.value
                app.storage.general["fx10_band_sel"] = band_sel.value
                app.storage.general["fx10_band_index"] = band_num.value
                result = await fx10_tools.snapshot(ip, float(exposure.value),
                                                   int(frames.value), _band_mode())
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
            image.set_source(f"data:image/jpeg;base64,{result.jpeg_b64}")
            chart.options["series"][0]["data"] = result.histogram
            chart.options["xAxis"]["data"] = list(range(len(result.histogram)))
            chart.update()
            meta_label.set_text(
                f"{result.lines}×{result.samples} px · {result.bands} bands · "
                f"exposure {exposure.value:.1f}ms · mean {result.mean_pct:.1f}% · "
                f"clipped {result.clipped_pct:.2f}% · {result.elapsed_s:.1f}s"
            )

        # Auto preview: a client-bound timer (NiceGUI cancels it when the tab's
        # client is deleted, and an async callback is awaited to completion
        # before the next tick — no overlapping shots, no orphaned loops).
        async def _auto_tick() -> None:
            if auto.value and not STATE.snapshot_busy and fx10_tools.guard_reason() is None and _target_ip():
                await _snap_once()

        ui.timer(0.5, _auto_tick)

        # block "start acquisition" while a shot is in flight and FX10 enabled
        # (reverse guard lives in process.preflight via STATE.snapshot_busy)


def _escape(text: str) -> str:
    return text.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")
