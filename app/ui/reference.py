"""FX10 reference collection and separate white/dark spectral results."""

from __future__ import annotations

from nicegui import ui

from ..constants import TOOL_TICK_S
from ..services import fx10_reference
from ..state import STATE
from . import layout


@ui.page("/reference")
def reference_page() -> None:
    with layout.frame("Collect Reference — FX10"):
        page = {"alive": True, "pending": False, "shown": None}

        def dispose() -> None:
            page["alive"] = False

        ui.context.client.on_delete(dispose)
        ui.label("Position an illuminated white reference target before collecting. The camera captures white "
                 "with the shutter open, then dark with the shutter closed, using the saved FX10 configuration.").classes(
                     "text-sm text-gray-700")
        ui.label("Both plots summarize every valid frame and spatial sample in their reference phase, using the "
                 "same mean, median, min, max and p90 statistics as Snapshot and Data Live. Values are normalized "
                 "to the recorded bit depth. Setup and finalization add to the capture time.").classes("text-sm text-gray-600")
        config_label = ui.label("").classes("text-sm text-gray-700 break-all")
        with ui.row().classes("items-center gap-4"):
            collect_btn = ui.button("Collect Reference", icon="contrast", on_click=lambda: collect())
            ui.button("Reload config", icon="refresh", on_click=lambda: show_config()).props("outline")
            ui.link("Edit FX10 config", "/config")
            spinner = ui.spinner(size="sm").classes("hidden")
        guard_label = ui.label("").classes("text-sm text-red-700")
        status_label = ui.label("").classes("text-sm text-gray-700 break-all")
        session_label = ui.label("").classes("text-sm font-mono break-all")

        charts = {}
        labels = {}
        with ui.element("div").classes("grid grid-cols-1 lg:grid-cols-2 gap-4 w-full"):
            for phase in ("white", "dark"):
                with ui.card().classes("w-full min-w-0"):
                    ui.label(f"{phase.capitalize()} reference").classes("text-lg font-bold")
                    labels[phase] = ui.label("Waiting for collection").classes("text-sm text-gray-600")
                    charts[phase] = ui.echart({
                        "xAxis": {"type": "value", "name": "Image row (uncalibrated)",
                                  "nameLocation": "middle", "nameGap": 28, "min": "dataMin", "max": "dataMax"},
                        "yAxis": {"type": "value", "name": "Normalized (%)", "min": 0, "max": 100},
                        "grid": {"left": 52, "right": 20, "top": 48, "bottom": 48},
                        "legend": {"top": 0},
                        "tooltip": {"trigger": "axis"},
                        "series": [],
                    }).classes("w-full h-96")

        with ui.expansion("Raw output of the last collection", icon="terminal").classes("w-full"):
            raw_output = ui.label("").classes("w-full text-xs font-mono whitespace-pre-wrap break-all")

        def show_config() -> None:
            try:
                path, duration = fx10_reference.configured_duration()
                timing = f"{duration:g} s white + {duration:g} s dark" if duration is not None else "driver default duration per phase"
                config_label.set_text(f"{path} · {timing} · reference.duration_s")
            except Exception as error:
                config_label.set_text(f"Cannot read reference configuration: {error}")

        def refresh() -> None:
            if not page["alive"]:
                return
            reason = fx10_reference.guard_reason()
            collect_btn.set_enabled(reason is None and not STATE.snapshot_busy and not page["pending"])
            guard_label.set_text(reason or ("A camera tool is in progress" if STATE.snapshot_busy else ""))
            status_label.set_text(fx10_reference.status())
            result = fx10_reference.last_result()
            if result is page["shown"]:
                return
            page["shown"] = result
            raw_output.set_text(result.raw_output if result else "")
            session_label.set_text(f"Session: {result.session_dir}" if result and result.session_dir else "")
            for phase, chart in charts.items():
                spectrum = result.spectra.get(phase) if result else None
                if spectrum and spectrum.ok:
                    axis = spectrum.wavelengths_nm or list(range(spectrum.bands))
                    chart.options["xAxis"]["name"] = "Wavelength (nm)" if spectrum.wavelengths_nm else "Image row (uncalibrated)"
                    chart.options["series"] = [
                        {"name": name, "type": "line", "showSymbol": False, "animation": False,
                         "data": [[x, value] for x, value in zip(axis, values)]}
                        for name, values in spectrum.spectrum_pct.items()
                    ]
                    labels[phase].set_text(
                        f"{result.duration_s:g} s requested · {spectrum.frames} frames · {spectrum.bands} bands · "
                        f"{spectrum.samples} px/line · mean {spectrum.mean_pct:.1f}% · clipped {spectrum.clipped_pct:.2f}%")
                else:
                    chart.options["series"] = []
                    labels[phase].set_text(spectrum.reason if spectrum else (
                        result.reason if result and not result.ok else "Waiting for collection or statistics"))
                chart.update()

        async def collect() -> None:
            if page["pending"] or STATE.snapshot_busy or not page["alive"]:
                return
            page["pending"] = True
            collect_btn.set_enabled(False)
            spinner.classes(remove="hidden")
            show_config()
            try:
                result = await fx10_reference.collect_reference()
                if page["alive"]:
                    unavailable = any(not spectrum.ok for spectrum in result.spectra.values())
                    ui.notify("References saved; some plots could not be calculated" if result.ok and unavailable
                              else f"References saved to {result.session_dir}" if result.ok else result.reason,
                              type="warning" if result.ok and unavailable else "positive" if result.ok else "negative",
                              multi_line=True)
            except Exception as error:
                if page["alive"]:
                    ui.notify(f"Reference collection was not completed: {error}", type="negative", multi_line=True)
            finally:
                page["pending"] = False
                if page["alive"]:
                    spinner.classes(add="hidden")
                    refresh()

        show_config()
        ui.timer(TOOL_TICK_S, refresh)
        refresh()
