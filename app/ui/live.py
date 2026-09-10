"""Data Live page: on-demand preview of the RUNNING acquisition session.

Nothing auto-refreshes — each click reads the last second of data straight
from the files the drivers are writing (see services/live_view.py)."""

from __future__ import annotations

from nicegui import ui

from ..constants import TOOL_TICK_S
from ..services import live_view
from ..state import STATE, ProcState
from . import layout


@ui.page("/live")
def live_page() -> None:
    with layout.frame("Data Live"):
        pending = {"gox": False, "fx10": False}
        versions = {"gox": 0, "fx10": 0}
        page = {"alive": True, "generation": STATE.session_generation}

        def dispose() -> None:
            page["alive"] = False
            versions["gox"] += 1
            versions["fx10"] += 1

        ui.context.client.on_delete(dispose)
        guard_label = ui.label("").classes("text-sm text-red-700")

        # ------------------------------------------------------------ GoX
        ui.label("GoX — newest frame (last ~1 s)").classes("text-lg font-bold")
        with ui.row().classes("items-center gap-4"):
            gox_btn = ui.button("Fetch frame", icon="photo_camera", on_click=lambda: _fetch_gox())
            gox_busy = ui.spinner(size="sm").classes("hidden")
            gox_meta = ui.label("").classes("text-sm text-gray-600")
        gox_image = ui.interactive_image().classes("max-w-[70%]")

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
            if not page["alive"]:
                return
            if page["generation"] != STATE.session_generation:
                page["generation"] = STATE.session_generation
                versions["gox"] += 1
                versions["fx10"] += 1
                gox_image.set_source("")
                gox_meta.set_text("")
                fx10_meta.set_text("")
                chart.options["series"] = []
                chart.update()
            # Pure state reads, and NiceGUI drops setter calls that change
            # nothing — an idle tick sends nothing however often it runs.
            running = (STATE.process_state == ProcState.RUNNING and STATE.ownership_verified
                       and not STATE.control_uncertain)
            guard_label.set_text(
                "" if running else "Recording is not running — start it on the Overview page first")
            gox_btn.set_enabled(running and STATE.enables_at_start.get("gox", False)
                                and not pending["gox"] and not live_view.busy())
            fx10_btn.set_enabled(running and STATE.enables_at_start.get("fx10", False)
                                 and not pending["fx10"] and not live_view.busy())

        ui.timer(TOOL_TICK_S, refresh_guard)
        refresh_guard()

        async def _fetch_gox() -> None:
            if pending["gox"] or not page["alive"]:
                return
            pending["gox"] = True
            versions["gox"] += 1
            version = versions["gox"]
            gox_busy.classes(remove="hidden")
            gox_btn.set_enabled(False)
            try:
                result = await live_view.fetch_gox()
            except live_view.PreviewObsolete:
                return
            except Exception as e:
                if page["alive"] and version == versions["gox"]:
                    ui.notify(f"Preview unavailable: {e}", type="warning", multi_line=True)
                return
            finally:
                pending["gox"] = False
                if page["alive"]:
                    gox_busy.classes(add="hidden")
                    refresh_guard()
            if (not page["alive"] or version != versions["gox"]
                    or not live_view.is_current(result.request)):
                return
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
            if pending["fx10"] or not page["alive"]:
                return
            pending["fx10"] = True
            versions["fx10"] += 1
            version = versions["fx10"]
            fx10_busy.classes(remove="hidden")
            fx10_btn.set_enabled(False)
            try:
                result = await live_view.fetch_fx10()
            except live_view.PreviewObsolete:
                return
            except Exception as e:
                if page["alive"] and version == versions["fx10"]:
                    ui.notify(f"Preview unavailable: {e}", type="warning", multi_line=True)
                return
            finally:
                pending["fx10"] = False
                if page["alive"]:
                    fx10_busy.classes(add="hidden")
                    refresh_guard()
            if (not page["alive"] or version != versions["fx10"]
                    or not live_view.is_current(result.request)):
                return
            if not result.ok:
                ui.notify(result.reason, type="warning", multi_line=True)
                fx10_meta.set_text(result.reason)
                return
            axis = result.wavelengths_nm or list(range(result.bands))
            chart.options["xAxis"]["name"] = "Wavelength (nm)" if result.wavelengths_nm else "Image row (uncalibrated)"
            chart.options["series"] = [
                {"name": name, "type": "line", "showSymbol": False, "animation": False,
                 "data": [[x, v] for x, v in zip(axis, values)]}
                for name, values in result.spectrum_pct.items()
            ]
            chart.update()
            fx10_meta.set_text(
                f"{result.frames} frames · {result.bands} bands · {result.samples} px/line · "
                f"newest {result.age_s:.1f}s ago"
            )
