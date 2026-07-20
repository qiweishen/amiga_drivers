"""GoX tools page: camera discovery + single-shot preview with exposure tuning."""

from __future__ import annotations

from nicegui import app, ui

from ..services import gox_tools, process
from ..state import STATE
from . import layout

_COLUMNS = [
    {"name": "model", "label": "Model", "field": "model", "align": "left"},
    {"name": "ip", "label": "IP", "field": "ip", "align": "left"},
    {"name": "mac", "label": "MAC", "field": "mac", "align": "left"},
    {"name": "serial", "label": "Serial", "field": "serial", "align": "left"},
    {"name": "user_name", "label": "User name", "field": "user_name", "align": "left"},
    {"name": "config", "label": "Subnet config", "field": "config", "align": "left"},
]


@ui.page("/gox")
def gox_page() -> None:
    with layout.frame("GoX Tools"):
        guard_label = ui.label("").classes("text-sm text-red-700")

        # ------------------------------------------------------------ discover
        ui.label("Device discovery (jai_discover)").classes("text-lg font-bold")
        with ui.row().classes("items-center gap-4"):
            scan_btn = ui.button("Scan cameras", icon="search", on_click=lambda: _scan())
            scan_status = ui.label("").classes("text-sm text-gray-600")
        table = ui.table(columns=_COLUMNS, rows=[], row_key="mac",
                         selection="single").classes("w-full")
        fallback_pre = ui.element("pre").classes(
            "w-full text-xs font-mono bg-gray-100 p-2 rounded hidden whitespace-pre-wrap")

        # ------------------------------------------------------------ snapshot
        ui.separator()
        ui.label("Single-shot preview / exposure tuning (jai_snapshot)").classes("text-lg font-bold")
        target_label = ui.label().classes("text-sm text-gray-600")
        with ui.row().classes("items-center gap-8 w-full"):
            with ui.column().classes("w-72"):
                ui.label("Exposure (µs)")
                exposure = ui.slider(min=100, max=200_000, step=100,
                                     value=app.storage.general.get("gox_exposure_us", 20_000))
                exposure_num = ui.number(min=100, max=1_000_000, step=100, suffix="µs") \
                    .bind_value(exposure)
            with ui.column().classes("w-72"):
                ui.label("Gain (dB)")
                gain = ui.slider(min=0.0, max=24.0, step=0.1,
                                 value=app.storage.general.get("gox_gain", 1.0))
                gain_num = ui.number(min=0.0, max=48.0, step=0.1, suffix="dB").bind_value(gain)
        with ui.row().classes("items-center gap-4"):
            snap_btn = ui.button("Take snapshot", icon="photo_camera", on_click=lambda: _snap_once())
            auto = ui.switch("Auto refresh")
            busy = ui.spinner(size="sm").classes("hidden")
            meta_label = ui.label("").classes("text-sm text-gray-600")
        warn_label = ui.label("").classes("text-sm text-amber-700")

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
            return app.storage.general.get("gox_target_ip", "")

        def refresh_guard() -> None:
            reason = gox_tools.guard_reason()
            guard_label.set_text(reason or "")
            allowed = reason is None and STATE.env_ok and not STATE.snapshot_busy
            scan_btn.set_enabled(reason is None and STATE.env_ok and not STATE.snapshot_busy)
            snap_btn.set_enabled(allowed and bool(_target_ip()))
            target_label.set_text(
                f"Target camera: {_target_ip() or '(scan and select a camera first)'}"
            )
            if reason and auto.value:
                auto.set_value(False)

        ui.timer(1.0, refresh_guard)
        refresh_guard()

        async def _scan() -> None:
            scan_status.set_text("Scanning…")
            result = await gox_tools.discover()
            raw_pre.clear()
            with raw_pre:
                ui.html(f"<span>{_escape(result.raw_output)}</span>")
            if result.error:
                scan_status.set_text(result.error)
            if not result.json_supported:
                fallback_pre.classes(remove="hidden")
                fallback_pre.clear()
                with fallback_pre:
                    ui.html(f"<span>{_escape(result.raw_output)}</span>")
                scan_status.set_text("Old jai_discover binary (no --json): rebuild and retry; raw output below")
                return
            rows = [
                {
                    "model": d.model, "ip": d.ip, "mac": d.mac, "serial": d.serial,
                    "user_name": d.user_name,
                    "config": "OK" if d.config_valid else "INVALID-SUBNET (unreachable: fix the camera/NIC subnet)",
                }
                for d in result.devices
            ]
            table.update_rows(rows)
            if not result.error:
                scan_status.set_text(f"Found {len(rows)} camera(s)")
            # auto-select the remembered camera
            remembered = app.storage.general.get("gox_target_ip", "")
            for row in rows:
                if row["ip"] == remembered:
                    table.selected = [row]
                    break

        def _on_select() -> None:
            ip = _target_ip()
            if ip:
                app.storage.general["gox_target_ip"] = ip
            refresh_guard()

        table.on("selection", lambda _: _on_select())

        async def _snap_once() -> None:
            if STATE.snapshot_busy:
                return
            reason = gox_tools.guard_reason()
            ip = _target_ip()
            if reason or not ip:
                ui.notify(reason or "Select a target camera first", type="warning")
                return
            STATE.snapshot_busy = True
            busy.classes(remove="hidden")
            snap_btn.set_enabled(False)
            try:
                app.storage.general["gox_exposure_us"] = exposure.value
                app.storage.general["gox_gain"] = gain.value
                result = await gox_tools.snapshot(ip, float(exposure.value), float(gain.value))
                _show(result)
            finally:
                STATE.snapshot_busy = False
                busy.classes(add="hidden")
                refresh_guard()

        def _show(result: gox_tools.SnapshotResult) -> None:
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
                f"{result.decode_name} · exposure {exposure.value:.0f}µs · gain {gain.value:.1f}dB · "
                f"mean {result.mean_16 / 65535 * 100:.1f}% · clipped {result.clipped_pct:.2f}% · {result.elapsed_s:.1f}s"
            )
            warn_label.set_text("Incomplete frame (packet loss? check MTU/rmem and the preflight hints)" if result.incomplete else "")

        # Auto preview: a client-bound timer (NiceGUI cancels it when the tab's
        # client is deleted, and an async callback is awaited to completion
        # before the next tick — no overlapping shots, no orphaned loops).
        async def _auto_tick() -> None:
            if auto.value and not STATE.snapshot_busy and gox_tools.guard_reason() is None and _target_ip():
                await _snap_once()

        ui.timer(0.5, _auto_tick)

        # block "start acquisition" while a shot is in flight and GOX enabled
        # (reverse guard lives in process.preflight via STATE.snapshot_busy)


def _escape(text: str) -> str:
    return text.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")
