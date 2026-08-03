"""Data Live page: on-demand preview of the RUNNING acquisition session.

Nothing auto-refreshes — each click reads the last second of data straight
from the files the drivers are writing (see services/live_view.py)."""

from __future__ import annotations

import asyncio

from nicegui import ui

from ..services import live_view
from ..state import STATE, ProcState
from . import layout


@ui.page("/live")
def live_page() -> None:
    with layout.frame("Data Live"):
        guard_label = ui.label("").classes("text-sm text-red-700")

        # ------------------------------------------------------------ GoX
        ui.label("GoX — newest frame (last ~1 s)").classes("text-lg font-bold")
        with ui.row().classes("items-center gap-4"):
            gox_btn = ui.button("Fetch frame", icon="photo_camera", on_click=lambda: _fetch_gox())
            gox_busy = ui.spinner(size="sm").classes("hidden")
            gox_meta = ui.label("").classes("text-sm text-gray-600")
        gox_image = ui.interactive_image().classes("max-w-[70%] border rounded")

        # ------------------------------------------------------------ FX10
        ui.separator()
        ui.label("FX10 — spectral statistics (last 1 s of frames)").classes("text-lg font-bold")
        with ui.row().classes("items-center gap-4"):
            fx10_btn = ui.button("Fetch spectrum", icon="ssid_chart", on_click=lambda: _fetch_fx10())
            fx10_busy = ui.spinner(size="sm").classes("hidden")
            fx10_meta = ui.label("").classes("text-sm text-gray-600")
        chart = ui.echart({
            "xAxis": {"type": "value", "name": "Wavelength (nm)", "nameLocation": "middle",
                      "nameGap": 28, "min": "dataMin", "max": "dataMax"},
            "yAxis": {"type": "value", "name": "Normalized (%)", "min": 0, "max": 100},
            "grid": {"left": 56, "right": 24, "top": 32, "bottom": 44},
            "legend": {"top": 0},
            "tooltip": {"trigger": "axis"},
            "series": [],
        }).classes("w-full h-96")

        # ---------------------------------------------------------------- glue
        def refresh_guard() -> None:
            running = STATE.process_state == ProcState.RUNNING
            guard_label.set_text("" if running else "Recording is not running — start it on the Overview page first")
            gox_btn.set_enabled(running and STATE.enables_at_start.get("gox", False))
            fx10_btn.set_enabled(running and STATE.enables_at_start.get("fx10", False))

        ui.timer(1.0, refresh_guard)
        refresh_guard()

        async def _fetch_gox() -> None:
            gox_busy.classes(remove="hidden")
            gox_btn.set_enabled(False)
            try:
                result = await asyncio.to_thread(live_view.gox_latest_frame)
            finally:
                gox_busy.classes(add="hidden")
                refresh_guard()
            if not result.ok:
                ui.notify(result.reason, type="warning", multi_line=True)
                gox_meta.set_text(result.reason)
                return
            gox_image.set_source(f"data:image/jpeg;base64,{result.jpeg_b64}")
            gox_meta.set_text(
                f"{result.camera} · seq {result.seq} · {result.width}×{result.height} · "
                f"{result.pixel_format} · {result.age_s:.1f}s ago"
            )

        async def _fetch_fx10() -> None:
            fx10_busy.classes(remove="hidden")
            fx10_btn.set_enabled(False)
            try:
                result = await asyncio.to_thread(live_view.fx10_spectrum)
            finally:
                fx10_busy.classes(add="hidden")
                refresh_guard()
            if not result.ok:
                ui.notify(result.reason, type="warning", multi_line=True)
                fx10_meta.set_text(result.reason)
                return
            chart.options["series"] = [
                {"name": name, "type": "line", "showSymbol": False, "animation": False,
                 "data": [[wl, v] for wl, v in zip(result.wavelengths_nm, values)]}
                for name, values in result.spectrum_pct.items()
            ]
            chart.update()
            fx10_meta.set_text(
                f"{result.frames} frames · {result.bands} bands · {result.samples} px/line · "
                f"newest {result.age_s:.1f}s ago"
            )
