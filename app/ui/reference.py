"""Camera reference capture: GOX ColorChecker and FX10 white/dark spectra."""

from __future__ import annotations

from nicegui import app, ui

from . import fx10_plot, gox_reference, layout
from ..constants import TOOL_TICK_S
from ..decoder.fx10 import exposure as fx10_exposure
from ..decoder.gox import exposure
from ..services import config_actions, fx10_exposure as fx10_series, fx10_reference
from ..state import STATE

_STATE_LABELS = {None: "ok", "clipped": "saturated", "near_saturation": "near saturation"}


def _series_rows(series: fx10_series.ExposureSeries) -> list[dict]:
    full_scale = series.full_scale or 4095

    def row(key, requested, point, note):
        return {"key": key, "exposure": requested,
                "readback": f"{point.exposure_time_us / 1000:.3f}" if point else "—",
                "peak": f"{point.peak_dn:.0f}" if point else "—",
                "fs": f"{point.peak_dn / full_scale * 100:.1f}" if point else "—",
                "state": _STATE_LABELS.get(point.status, point.status) if point else "not measured",
                "note": note}

    rows = [row(index, f"{item.requested_exposure_ms:g}", item.point, item.error)
            for index, item in enumerate(series.points)]
    rows += [row(f"check{index}", "white reference", check.point, check.message)
             for index, check in enumerate(series.checks)]
    return rows


def _check_text(check: fx10_series.ExposureCheck | None, full_scale: int) -> str:
    if check is None:
        return ""
    point = check.point
    measured = f"{point.peak_channel} {point.peak_dn:.0f} DN ({point.peak_dn / full_scale:.1%} FS)"
    if check.deviation is None:
        return f"{measured}, saturated. {check.message}"
    return (f"{measured} vs target {check.target_dn:.0f} DN: {check.deviation:+.1%} "
            f"({'within' if check.within else 'outside'} ±{exposure.TOLERANCE:.0%}). {check.message}")


def _series_text(series: fx10_series.ExposureSeries) -> str:
    if not series.ok or series.fit is None:
        return f"No exposure suggestion: {series.reason or series.fit_error}"
    fit = series.fit
    lines = [
        f"{series.band_label}: p90 = {fit.offset_dn:.0f} DN + {fit.slope_dn_per_us * 1000:.2f} DN/ms × exposure "
        f"(RMS residual {fit.residual_rms_dn:.1f} DN; {fit.points_used} points fitted, {fit.points_excluded} "
        f"saturated excluded). Fitted dark offset ≈ {fit.offset_dn:.0f} DN.",
        f"ETTR target {fit.target_dn:.0f} DN ({fit.target_fraction:.0%} FS) → {fit.suggested_exposure_ms:g} ms "
        f"(binning unchanged: spatial {series.binning[0]} × spectral {series.binning[1]}).",
    ]
    if series.limit_notes:
        lines.append("Exposure ceiling: " + "; ".join(series.limit_notes))
    if series.initial_suggestion is not None:
        initial = series.initial_suggestion
        lines.append(f"Suggested exposure: {initial.exposure_ms:g} ms." + (f" {initial.note}" if initial.note else ""))
    lines += [f"⚠ {warning}" for warning in fit.warnings]
    if series.checks:
        lines.append(f"Last verification: {_check_text(series.checks[-1], series.full_scale)}")
    return "\n".join(lines)


@ui.page("/reference")
def reference_page() -> None:
    with layout.frame("Collect Reference — FX10 / GOX"):
        gox_reference.panel()
        ui.separator()
        page = {"alive": True, "pending": False, "shown": None, "series_running": False, "series_shown": False}

        def dispose() -> None:
            page["alive"] = False

        ui.context.client.on_delete(dispose)
        with ui.column().classes("console-panel w-full gap-3"):
            ui.label("FX10 white / dark reference").classes("section-title")
            ui.label("Position a white reference target before collecting. The camera captures white "
                     "with the shutter open, then dark with the shutter closed, using the FX10 snapshot "
                     "configuration in freerun mode, without PTP or a trigger board.").classes(
                "section-copy")
            guard_label = ui.label("").classes("text-sm text-red-700")
            ui.label("1. Auto exposure on the white reference (ETTR, optional)").classes("font-medium text-sm")
            ui.label(f"Set an initial exposure in the Spectral workbench first. The bracket captures the white "
                     f"reference at the initial exposure and ± {fx10_exposure.BRACKET_STEP_MS:g} ms (half the "
                     f"initial exposure as the low step when it is {fx10_exposure.BRACKET_STEP_MS:g} ms or less) "
                     "with the snapshot configuration, fits the brightest band's p90 DN against exposure, and "
                     "suggests the exposure that puts it at the target fraction of full scale. Only the exposure "
                     "changes; binning stays as configured. Captures run one after another; each capture's "
                     "spectrum is computed alongside the next capture.").classes("section-copy")
            with ui.row().classes("console-toolbar w-full items-end"):
                center_input = ui.number("Initial exposure (ms)", min=0.01, step=0.5, format="%.3f",
                                         value=float(app.storage.general.get("fx10_exposure_ms", 10.0))
                                         ).classes("w-48")
                target_input = ui.number("ETTR target (% FS)", min=fx10_exposure.MIN_TARGET_FRACTION * 100,
                                         max=fx10_exposure.MAX_TARGET_FRACTION * 100, step=1,
                                         value=fx10_exposure.DEFAULT_TARGET_FRACTION * 100).classes("w-40")
                series_btn = ui.button("Run exposure bracket", icon="exposure", on_click=lambda: run_series())
                series_spinner = ui.spinner(size="sm").classes("hidden")
            series_status = ui.label("").classes("text-sm break-all")
            series_table = ui.table(columns=[
                {"name": "exposure", "label": "Requested (ms)", "field": "exposure", "align": "right"},
                {"name": "readback", "label": "ExposureTime readback (ms)", "field": "readback", "align": "right"},
                {"name": "peak", "label": "Band p90 (DN)", "field": "peak", "align": "right"},
                {"name": "fs", "label": "% FS", "field": "fs", "align": "right"},
                {"name": "state", "label": "Status", "field": "state", "align": "left"},
                {"name": "note", "label": "Note", "field": "note", "align": "left"},
            ], rows=[], row_key="key").props("dense flat").classes("w-full")
            series_table.set_visibility(False)
            series_fit = ui.label("").classes("section-copy whitespace-pre-line")
            with ui.row().classes("console-toolbar w-full items-center") as apply_row:
                apply_btn = ui.button("Apply suggested exposure to config", icon="save",
                                      on_click=lambda: apply_suggestion())
                apply_note = ui.label("").classes("text-sm muted break-all")
            apply_row.set_visibility(False)

            ui.label("2. Collect the white / dark pair").classes("font-medium text-sm")
            config_label = ui.label("").classes("text-xs mono muted break-all")
            with ui.row().classes("console-toolbar w-full"):
                collect_btn = ui.button("Collect Reference", icon="contrast", on_click=lambda: collect())
                ui.button("Reload config", icon="refresh", on_click=lambda: show_config()).props("outline")
                spinner = ui.spinner(size="sm").classes("hidden")
            status_label = ui.label("").classes("text-sm break-all")
            session_label = ui.label("").classes("text-xs mono muted break-all")
            check_label = ui.label("").classes("text-sm break-all")
            with ui.row().classes("console-toolbar w-full items-center") as save_row:
                save_btn = ui.button("Save reference pair to the output directory", icon="save_alt",
                                     on_click=lambda: save_reference())
                save_label = ui.label("").classes("text-sm muted break-all")
            save_row.set_visibility(False)

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

        def refresh_series() -> None:
            series = fx10_series.last_series()
            if page["series_running"]:
                series_status.set_text(fx10_series.status())
            if series is page["series_shown"]:
                return
            page["series_shown"] = series
            series_table.set_visibility(series is not None and bool(series.points))
            series_table.rows = _series_rows(series) if series is not None else []
            series_table.update()
            series_fit.set_text(_series_text(series) if series is not None else "")
            if series is not None and not page["series_running"]:
                series_status.set_text(f"Exposure series: {series.directory}" if series.directory else "")
            suggestion = series.suggestion if series is not None else None
            apply_row.set_visibility(suggestion is not None)
            if suggestion is not None:
                apply_btn.set_text(f"Apply exposure {suggestion.exposure_ms:g} ms to config")

        def refresh() -> None:
            if not page["alive"]:
                return
            reason = fx10_reference.guard_reason()
            idle = reason is None and not STATE.snapshot_busy and not page["pending"]
            collect_btn.set_enabled(idle)
            series_btn.set_enabled(idle)
            guard_label.set_text(reason or ("A camera tool is in progress" if STATE.snapshot_busy else ""))
            refresh_series()
            status_label.set_text(fx10_reference.status())
            result = fx10_reference.last_result()
            if result is page["shown"]:
                return
            page["shown"] = result
            raw_output.set_text(result.raw_output if result else "")
            session_label.set_text(f"Session: {result.session_dir}" if result and result.session_dir else "")
            series = fx10_series.last_series()
            full_scale = (series.full_scale if series is not None and series.full_scale else 4095)
            check_label.set_text("" if result is None or not result.ok else
                                 f"Exposure check: {_check_text(result.exposure_check, full_scale)}"
                                 if result.exposure_check else
                                 "Exposure check: no applicable exposure series (run step 1 for ETTR).")
            save_row.set_visibility(bool(result and result.ok))
            if result and result.ok:
                save_btn.set_enabled(not result.saved)
                save_label.set_text(f"Saved: {result.session_dir}" if result.saved else
                                    "Collected under the output directory's tmp folder; save it when the "
                                    "exposure check passes.")
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
                    ui.notify("References collected; some plots could not be calculated" if result.ok and unavailable
                              else f"References collected under tmp: {result.session_dir}" if result.ok
                              else result.reason,
                              type="warning" if result.ok and unavailable else "positive" if result.ok else "negative",
                              multi_line=True)
                    check = result.exposure_check
                    if result.ok and check is not None and check.within:
                        ui.notify(f"Exposure is within ±{exposure.TOLERANCE:.0%} of the ETTR target. Save this "
                                  "reference pair to the output directory? Use “Save reference pair” below the "
                                  "exposure check.", type="info", multi_line=True, timeout=15000)
            except Exception as error:
                if page["alive"]:
                    ui.notify(f"Reference collection was not completed: {error}", type="negative", multi_line=True)
            finally:
                page["pending"] = False
                if page["alive"]:
                    spinner.classes(add="hidden")
                    refresh()

        async def run_series() -> None:
            if page["pending"] or STATE.snapshot_busy or not page["alive"]:
                return
            try:
                center = float(center_input.value)
                fraction = fx10_exposure.target_fraction(float(target_input.value) / 100.0)
                exposures = fx10_exposure.bracket(center)
            except (TypeError, ValueError) as error:
                ui.notify(f"Invalid initial exposure or target: {error}", type="warning")
                return
            page["pending"] = page["series_running"] = True
            series_spinner.classes(remove="hidden")
            series_status.set_text(f"Bracket {', '.join(f'{value:g}' for value in exposures)} ms, "
                                   f"target {fraction:.0%} FS…")
            apply_note.set_text("")
            refresh()
            try:
                series = await fx10_series.run(center, fraction)
                if page["alive"]:
                    ui.notify(f"Suggested ETTR exposure: {series.suggestion.exposure_ms:g} ms" if series.ok
                              else f"Exposure series failed: {series.reason}",
                              type="positive" if series.ok else "negative", multi_line=True)
            except Exception as error:
                if page["alive"]:
                    ui.notify(f"Exposure series was not completed: {error}", type="negative", multi_line=True)
            finally:
                page["pending"] = page["series_running"] = False
                if page["alive"]:
                    series_spinner.classes(add="hidden")
                    page["series_shown"] = False  # force a redraw with the final status
                    refresh()

        async def apply_suggestion() -> None:
            series = fx10_series.last_series()
            suggestion = series.suggestion if series is not None else None
            if suggestion is None:
                ui.notify("No exposure suggestion is available", type="warning")
                return
            try:
                summary = await config_actions.change("apply_fx10_acquisition", float(suggestion.exposure_ms),
                                                      None, None)
            except Exception as error:
                ui.notify(f"Apply failed: {error}", type="negative", multi_line=True)
                return
            app.storage.general["fx10_exposure_ms"] = float(suggestion.exposure_ms)
            center_input.set_value(float(suggestion.exposure_ms))
            apply_note.set_text(f"Applied: {summary}")
            ui.notify(f"Applied exposure {suggestion.exposure_ms:g} ms (binning unchanged). Now use Collect "
                      "Reference (step 2) to verify the exposure.", type="positive", multi_line=True)

        async def save_reference() -> None:
            try:
                result = await fx10_reference.save_to_data()
            except Exception as error:
                if page["alive"]:
                    ui.notify(f"Reference pair was not saved: {error}", type="negative", multi_line=True)
                return
            if page["alive"]:
                ui.notify(f"Reference pair saved: {result.session_dir}", type="positive", multi_line=True)
                refresh()

        show_config()
        ui.timer(TOOL_TICK_S, refresh)
        refresh()
