"""FX10 reference collection and separate white/dark spectral results."""

from __future__ import annotations

from nicegui import ui

from ..constants import TOOL_TICK_S
from ..services import fx10_reference
from ..state import STATE
from . import fx10_plot, layout


@ui.page("/reference")
def reference_page() -> None:
    with layout.frame("Collect Reference — FX10"):
        page = {"alive": True, "pending": False, "shown": None}

        def dispose() -> None:
            page["alive"] = False

        ui.context.client.on_delete(dispose)
        with ui.column().classes("console-panel w-full gap-3"):
            ui.label("FX10 white / dark reference").classes("section-title")
            ui.label("Position a white reference target before collecting. The camera captures white "
                     "with the shutter open, then dark with the shutter closed, using the saved FX10 recording "
                     "configuration, including its freerun or external trigger mode. "
                     "When SensorSync is enabled, the trigger board uses the port in the main configuration.").classes(
                         "section-copy")
            config_label = ui.label("").classes("text-xs mono muted break-all")
            with ui.row().classes("console-toolbar w-full"):
                collect_btn = ui.button("Collect Reference", icon="contrast", on_click=lambda: collect())
                ui.button("Reload config", icon="refresh", on_click=lambda: show_config()).props("outline")
                ui.link("Edit FX10 recording / reference config", "/config")
                spinner = ui.spinner(size="sm").classes("hidden")
            guard_label = ui.label("").classes("text-sm text-red-700")
            status_label = ui.label("").classes("text-sm break-all")
            session_label = ui.label("").classes("text-xs mono muted break-all")

        ui.label("Both plots summarize every valid frame and spatial sample in their reference phase, using the "
                 "same mean, median, min, max and p90 statistics as the Spectral workbench, in raw digital numbers (DN). "
                 "Dashed guides mark 20%, 50% and 80% of the captured pixel format's full scale. "
                 "The wavelength axis spans 400–1000 nm with 100 nm ticks. "
                 "Setup and finalization add to the capture time.").classes("section-copy")

        charts = {}
        labels = {}
        axis_notes = {}
        with ui.element("div").classes("workspace-grid w-full"):
            for phase in ("white", "dark"):
                with ui.card().classes("console-panel w-full min-w-0").props("flat"):
                    with ui.row().classes("section-heading w-full items-center justify-between"):
                        ui.label(f"{phase.capitalize()} reference").classes("section-title")
                        ui.icon("light_mode" if phase == "white" else "dark_mode").classes("muted")
                    labels[phase] = ui.label("Waiting for collection").classes("section-copy")
                    charts[phase] = ui.echart(fx10_plot.workbench_options()).classes("plot-surface w-full h-96")
                    axis_notes[phase] = ui.label("").classes("section-copy")

        with ui.expansion("Raw output of the last collection", icon="terminal").classes("w-full"):
            raw_output = ui.label("").classes("terminal-surface w-full text-xs mono whitespace-pre-wrap break-all p-4")

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
                    chart.options.update(fx10_plot.workbench_options(
                        spectrum.spectrum_dn, full_scale=spectrum.full_scale, bands=spectrum.bands,
                        wavelengths_nm=spectrum.wavelengths_nm,
                    ))
                    axis_notes[phase].set_text(fx10_plot.axis_note(spectrum.wavelengths_nm))
                    labels[phase].set_text(
                        f"{result.duration_s:g} s requested · {spectrum.frames} frames · {spectrum.bands} bands · "
                        f"{spectrum.samples} px/line · {spectrum.pixel_format} · full scale {spectrum.full_scale} DN · "
                        f"mean {spectrum.mean_dn:.1f} DN · clipped {spectrum.clipped_pct:.2f}%")
                else:
                    chart.options.update(fx10_plot.workbench_options())
                    axis_notes[phase].set_text("")
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
