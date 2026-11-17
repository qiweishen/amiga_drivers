"""GOX ColorChecker capture controls and in-memory calibration results."""

from __future__ import annotations

from pathlib import Path

from nicegui import app, ui

from ..constants import SNAPSHOT_CONFIG, TOOL_TICK_S
from ..decoder import exposure as ettr
from ..decoder.gox import color_checker, exposure
from ..decoder.gox.color_checker import CHARTS, GEOMETRY_SUPPLIED
from ..services import camera_operations, config_store, gox_exposure, gox_reference, preview_images
from ..state import STATE
from . import ettr_section
from .components import PREVIEW_DETAILS, heading, hint, info, raw_output

_RESOURCES = Path(__file__).resolve().parent.parent / "resource"


def _reference_exposure_text(provenance: dict) -> str:
    exposure = provenance.get("reference_exposure")
    if exposure:
        return (f"Reference exposure: {exposure['exposure_time_us']:.2f} µs × gain {exposure['gain']:.3g} · "
                f"{exposure['device_model']} #{exposure['device_id']}")
    reason = provenance.get("reference_exposure_unavailable_reason") or "unknown"
    return f"No reusable calibration: reference exposure unavailable ({reason})"


def _lab_text(lab) -> str:
    return " / ".join(f"{float(value):.1f}" for value in lab)


def _patch_error_rows(provenance: dict) -> list[dict]:
    rows = []
    for item in provenance.get("patch_errors") or []:
        reference, corrected = item["reference_lab"], item["corrected_lab"]
        rows.append({"patch": item["patch"], "name": item.get("name") or "—",
                     "de": round(item["delta_e2000"], 2),
                     "dl": round(corrected[0] - reference[0], 1), "da": round(corrected[1] - reference[1], 1),
                     "db": round(corrected[2] - reference[2], 1),
                     "reference": _lab_text(reference), "corrected": _lab_text(corrected)})
    return rows


def _patch_error_summary(rows: list[dict]) -> str:
    if not rows:
        return "Per-patch ΔE00 is not available for this result."
    values = sorted(row["de"] for row in rows)
    worst = max(rows, key=lambda row: row["de"])
    name = f" {worst['name']}" if worst["name"] != "—" else ""
    return (f"Per-patch ΔE00: max {worst['de']:.2f} (#{worst['patch']}{name}) · mean "
            f"{sum(values) / len(values):.2f} · median {values[len(values) // 2]:.2f} · "
            f"{sum(value > 5 for value in values)} patches above 5")


def _black_level_text(provenance: dict) -> tuple[str, str]:
    """(one visible line, ⓘ details) for the pedestal used by the fit."""
    black = provenance.get("black_level")
    if not black:
        return "Black level: not subtracted", "Calibration made before the documented pedestal default."
    warnings = black.get("warnings", [])
    text = f"Black level: {black['pedestal_dn']:g} DN subtracted ({black['status']})"
    if warnings:
        text += f" · {len(warnings)} warning(s)"
    return text, "\n".join([f"Source: {black['source']}", *warnings])


_FULL_SCALE = 4095  # native 12-bit DN
_COLUMNS = [
    {"name": "exposure", "label": "Requested (ms)", "field": "exposure", "align": "right"},
    {"name": "integration", "label": "Integration (µs)", "field": "integration", "align": "right"},
    {"name": "peak", "label": "Patch peak (DN)", "field": "peak", "align": "right"},
    {"name": "fs", "label": "% FS", "field": "fs", "align": "right"},
    {"name": "state", "label": "Status", "field": "state", "align": "left"},
    {"name": "loss", "label": "CCM loss (ΔE00)", "field": "loss", "align": "right"},
    {"name": "note", "label": "Note", "field": "note", "align": "left"},
]


def _series_rows(series: gox_exposure.ExposureSeries) -> list[dict]:
    def row(key, requested, point, loss, note):
        return {"key": key, "exposure": requested,
                "integration": f"{point.exposure_time_us:.0f}" if point else "—",
                "peak": f"{point.peak_dn:.0f} ({point.peak_channel})" if point else "—",
                "fs": f"{point.peak_dn / _FULL_SCALE * 100:.1f}" if point else "—",
                "state": ettr_section.STATE_LABELS.get(point.status, point.status) if point else "not measured",
                "loss": f"{loss:.3g}" if loss is not None else "—", "note": note}

    return ([row(index, f"{item.requested_exposure_ms:g}", item.point, item.loss,
                 item.error or ("chart selected on another capture" if item.geometry == GEOMETRY_SUPPLIED else ""))
             for index, item in enumerate(series.points)]
            + [row(f"check{index}", "verification", check.point, None, check.message)
               for index, check in enumerate(series.checks)])


def _check_text(check) -> str:
    return ettr_section.check_text(check, _FULL_SCALE, f"patch #{check.point.patch} peak" if check else "")


def _series_text(series: gox_exposure.ExposureSeries) -> str:
    if series.awaiting_selection:
        return "Select the chart among the detected candidates."
    if not series.ok or series.fit is None:
        return f"No exposure suggestion: {series.reason or series.fit_error}"
    fit = series.fit
    lines = ettr_section.fit_lines(
        series, f"Patch #{series.patch} peak = {fit.offset_dn:.0f} DN + {fit.slope_dn_per_us * 1000:.3f} DN/ms × t · "
                f"RMS {fit.residual_rms_dn:.1f} DN · {fit.points_used} fitted, {fit.points_excluded} saturated",
        f"gain x{series.gain:g} unchanged")
    if series.checks:
        lines.append(f"Last verification: {_check_text(series.checks[-1])}")
    return "\n".join(lines)


def panel() -> None:
    page = {"alive": True, "pending": False, "shown": None,
            "attempt_error": "", "hidden_result": None, "chart_key": None,
            "analysis": None,
            "series_running": False, "notified": None,
            # The reference result or exposure series whose candidates the chooser shows, and which.
            "offered": None, "chooser": None}

    def dispose() -> None:
        page["alive"] = False

    ui.context.client.on_delete(dispose)
    capability = color_checker.capability_reason()

    with ui.column().classes("console-panel w-full gap-3"):
        with heading("GOX · ColorChecker reference", icon="color_lens"):
            ui.label("Target: sRGB · D65").classes("text-xs mono muted")
        hint("One chart in view, every patch visible and evenly lit.")
        with ui.row().classes("hint-row"):
            ui.label("1. Select the chart in front of the camera").classes("font-medium text-sm")
            info("Click a chart image to select its format; a selection is required before capture. "
                 "The images only identify layouts; they are not colour references.")
        chart_buttons = {}
        chart_marks = {}
        with ui.element("div").classes("control-grid w-full"):
            for spec in CHARTS.values():
                with ui.button(on_click=lambda _, key=spec.key: select_chart(key)).props(
                        'flat no-caps align=left aria-pressed="false"').classes(
                            "chart-choice w-full min-w-0") as button:
                    chart_buttons[spec.key] = button
                    ui.label(spec.label).classes("font-medium text-sm")
                    ui.image(str(_RESOURCES / spec.image_filename)).props(
                        "fit=contain").classes("w-full h-48").props(f'alt="{spec.label}"')
                    with ui.row().classes("w-full items-center justify-between"):
                        ui.label(f"{spec.key} · {spec.patch_count} patches").classes("text-xs mono muted")
                        chart_marks[spec.key] = ui.icon("check_circle").classes(
                            "text-primary text-xl invisible").props('aria-hidden="true"')

        with ui.row().classes("hint-row"):
            ui.label("2. Auto exposure (ETTR, optional)").classes("font-medium text-sm")
            info(f"Captures the chart at the initial exposure ± {exposure.BRACKET_STEP_MS:g} ms, fits the "
                 "brightest patch's DN against exposure and suggests the exposure that reaches "
                 f"{exposure.TARGET_FRACTION:.0%} of full scale. Only the exposure changes; the gain stays as "
                 "configured. You select the chart once for all captures.")
        with ui.row().classes("console-toolbar w-full items-end"):
            center_input = ui.number("Initial exposure (ms)", min=0.001, step=1.0, format="%.3f",
                                     value=float(app.storage.general.get("gox_exposure_ms", 10.0))).classes("w-48")
            series_btn = ui.button("Run exposure bracket", icon="exposure", on_click=lambda: run_series())
            series_spinner = ui.spinner(size="sm").classes("hidden")
        series_panel = ettr_section.SeriesPanel(_COLUMNS, lambda: apply_suggestion())
        with ui.row().classes("hint-row"):
            ui.label("3. Capture and calibrate").classes("font-medium text-sm")
            info("Camera, exposure, gain and image settings come from the GOX snapshot configuration "
                 "(freerun, no PTP or trigger). After the capture you select the chart among the detected "
                 "candidates; calibration uses only your selection. The RAW is kept unchanged.")
        ui.label(f"Configuration: {SNAPSHOT_CONFIG}").classes("text-xs mono muted break-all")
        with ui.row().classes("console-toolbar w-full"):
            capture_btn = ui.button("Capture & calibrate", icon="color_lens", on_click=lambda: capture())
            spinner = ui.spinner(size="sm").classes("hidden")
        input_label = ui.label("").classes("text-sm muted")
        guard_label = ui.label("").classes("text-sm text-red-700")
        status_label = ui.label("").classes("text-sm break-all")
        session_label = ui.label("").classes("text-xs mono muted break-all")

    with ui.column().classes("w-full gap-3") as result_panel:
        result_label = ui.label("").classes("text-sm break-all")
        capture_label = ui.label("").classes("text-xs mono muted break-all")
        config_label = ui.label("").classes("text-xs mono muted break-all")
        check_label = ui.label("").classes("text-sm break-all")
        with ui.row().classes("console-toolbar w-full items-center"):
            save_btn = ui.button("Save reference to the output directory", icon="save_alt",
                                 on_click=lambda: save_reference())
            save_label = ui.label("").classes("text-sm muted break-all")
        with ui.element("div").classes("control-grid w-full"):
            previews = {}
            downloads = {}
            for key, title in (("original", "Captured image · 12-bit DN, linear"),
                               ("annotated", "Located chart"),
                               ("corrected", "Calibrated preview · sRGB")):
                with ui.card().classes("console-panel w-full min-w-0").props("flat"):
                    ui.label(title).classes("section-title")
                    previews[key] = ui.interactive_image().classes("w-full")
                    with ui.row().classes("hint-row"):
                        downloads[key] = ui.link("Download full-resolution 16-bit PNG", "#").classes("text-xs")
                        info(PREVIEW_DETAILS)
        with ui.column().classes("console-panel w-full gap-2"):
            heading("Color correction matrix · 3 × 3",
                    details="(Camera RGB − black level) / (full scale − black level), row vector × matrix → "
                            "linear sRGB (D65). The calibrated preview only renders that result.")
            matrix_label = ui.label("").classes("mono text-sm whitespace-pre")
            fit_label = ui.label("").classes("section-copy")
            with ui.row().classes("hint-row"):
                patch_summary = ui.label("").classes("section-copy")
                info("ΔE00 compares each corrected patch with the chart reference (CIE L*a*b* D65). "
                     "Δ = corrected − reference: ΔL* > 0 too light, Δa* > 0 too red, Δb* > 0 too yellow. "
                     "The RMS equals the fit loss, which describes this capture, not an independent "
                     "accuracy check; re-capture when light, exposure or camera processing changes.")
            patch_table = ui.table(columns=[
                {"name": "patch", "label": "#", "field": "patch", "align": "right", "sortable": True},
                {"name": "name", "label": "Patch", "field": "name", "align": "left"},
                {"name": "de", "label": "ΔE00", "field": "de", "align": "right", "sortable": True},
                {"name": "dl", "label": "ΔL*", "field": "dl", "align": "right", "sortable": True},
                {"name": "da", "label": "Δa*", "field": "da", "align": "right", "sortable": True},
                {"name": "db", "label": "Δb*", "field": "db", "align": "right", "sortable": True},
                {"name": "reference", "label": "Reference L* / a* / b*", "field": "reference", "align": "right"},
                {"name": "corrected", "label": "Corrected L* / a* / b*", "field": "corrected", "align": "right"},
            ], rows=[], row_key="patch", pagination={"rowsPerPage": 0, "sortBy": "de", "descending": True}
            ).props("dense flat").classes("w-full")
            with ui.row().classes("hint-row"):
                black_label = ui.label("").classes("section-copy")
                black_details = info("")
            exposure_label = ui.label("").classes("section-copy")
    result_panel.set_visibility(False)
    show_raw = raw_output("Raw output of the last GOX reference capture")

    # Chart selection: the detector also accepts regular textures as charts, so the operator
    # picks the chart among its candidates, even when there is only one.
    with ui.dialog().props("persistent") as chooser, ui.card().classes("w-full max-w-5xl gap-3"):
        chooser_title = ui.label("").classes("section-title")
        hint("Select the candidate that lies on the chart.",
             "Each thumbnail shows one detector candidate: a magenta outline around a green patch grid. Pick "
             "the one with every patch inside its square; if none fits, choose None of these and recapture.")
        chooser_grid = ui.element("div").classes("control-grid w-full")
        with ui.row().classes("w-full justify-end"):
            ui.button("None of these", icon="close", on_click=lambda: decide(None)).props("flat no-caps")

    def input_reason() -> str | None:
        if page["chart_key"] not in CHARTS:
            return "Select MCC24, SG140 or VINYL18 before capture."
        return None

    def show_image(key: str, url: str, full_url: str) -> None:
        previews[key].set_source(url)
        downloads[key].props["href"] = preview_images.download_url(full_url) if full_url else "#"

    def hide_result() -> None:
        page["analysis"] = None
        result_panel.set_visibility(False)
        session_label.set_text("")
        show_raw("")
        for image in previews.values():
            image.set_source("")

    def show_chooser(kind: str, offered) -> None:
        """Offer the candidates of a reference capture or an exposure series for selection."""
        page["offered"], page["chooser"] = offered, kind
        chooser_title.set_text(
            f"Select the {offered.chart_key} chart · {len(offered.candidates)} candidate(s)"
            + (" from the exposure bracket" if kind == "series" else ""))
        chooser_grid.clear()
        with chooser_grid:
            for candidate in offered.candidates:
                with ui.card().classes("console-panel w-full min-w-0").props("flat"):
                    ui.image(candidate.image).props(f'fit=contain alt="{candidate.label}"').classes("w-full")
                    ui.label(candidate.label).classes("text-xs mono muted")
                    ui.button("Use this chart", icon="check",
                              on_click=lambda _, number=candidate.number: decide(number)).props("no-caps")
        chooser.open()

    def sync_chooser() -> None:
        """Open the chooser for a new pending selection; close it once anyone has decided."""
        current = (gox_reference.last_result(), gox_exposure.last_series())
        if page["chooser"] is not None and not any(page["offered"] is item for item in current):
            chooser.close()
            page["offered"] = page["chooser"] = None
        if page["pending"] or page["chooser"] is not None:
            return
        result, series = current
        if result is not None and result.awaiting_selection and page["offered"] is not result:
            show_chooser("reference", result)
        elif series is not None and series.awaiting_selection and page["offered"] is not series:
            show_chooser("series", series)

    def notify_reference(result) -> None:
        if result.awaiting_selection:
            ui.notify(f"{len(result.candidates)} candidate(s) detected: select the chart.", type="info")
            return
        ui.notify(f"{CHARTS[result.chart_key].label} reference captured and calibrated" if result.ok
                  else result.reason, type="positive" if result.ok else "negative", multi_line=True)
        check = result.exposure_check
        if check is not None and check.within and result.ok:
            ui.notify(f"Exposure within ±{ettr.TOLERANCE:.0%} of the target; save the reference below.",
                      type="positive", multi_line=True, timeout=15000)
        elif check is not None:
            ui.notify(f"Exposure check: {check.message}", type="warning", multi_line=True, timeout=15000)

    def notify_series(series) -> None:
        if series.awaiting_selection:
            ui.notify(f"{len(series.candidates)} chart candidate(s) detected in the bracket: select the chart.",
                      type="info")
        else:
            ui.notify(f"Suggested ETTR exposure: {series.suggestion.exposure_ms:g} ms" if series.ok
                      else f"Exposure series failed: {series.reason}",
                      type="positive" if series.ok else "negative", multi_line=True)

    async def decide(number: int | None) -> None:
        """Calibrate (or fit the bracket) with the chosen candidate; None rejects them all."""
        kind = page["chooser"]
        chooser.close()
        page["chooser"] = None  # page["offered"] stays, so the same offer is not reopened
        if kind is None or page["pending"] or not page["alive"]:
            return
        page["pending"] = True
        page["series_running"] = kind == "series"
        (series_spinner if kind == "series" else spinner).classes(remove="hidden")
        refresh()
        try:
            if kind == "reference":
                result = await (gox_reference.choose(number) if number is not None else gox_reference.reject())
                if page["alive"]:
                    notify_reference(result)
            else:
                series = await (gox_exposure.choose(number) if number is not None else gox_exposure.reject())
                if page["alive"]:
                    notify_series(series)
        except Exception as error:
            if page["alive"]:
                ui.notify(f"Chart selection was not applied: {error}", type="negative", multi_line=True)
        finally:
            page["pending"] = False
            page["series_running"] = False
            if page["alive"]:
                spinner.classes(add="hidden")
                series_spinner.classes(add="hidden")
                series_panel.redraw()  # show the final status
                refresh()

    def refresh() -> None:
        if not page["alive"]:
            return
        sync_chooser()
        busy = page["pending"] or STATE.snapshot_busy
        reason = capability or camera_operations.guard_reason("gox")
        invalid = input_reason()
        for key, button in chart_buttons.items():
            selected = page["chart_key"] == key
            button.set_enabled(not busy)
            button.props(f'aria-pressed="{str(selected).lower()}"')
            button.classes(add="is-selected" if selected else "", remove="" if selected else "is-selected")
            chart_marks[key].classes(add="" if selected else "invisible", remove="invisible" if selected else "")
        capture_btn.set_enabled(not busy and reason is None and invalid is None)
        input_label.set_text(invalid or ("Ready to capture the selected chart." if not busy and reason is None else ""))
        guard_label.set_text(reason or ("A camera tool is in progress" if busy else ""))
        series_btn.set_enabled(not busy and reason is None and invalid is None)
        series_panel.apply_button.set_enabled(not busy)
        refresh_series()
        result = gox_reference.last_result()
        if page["attempt_error"] and result is not page["hidden_result"]:
            page["attempt_error"] = ""
        if page["attempt_error"]:
            status_label.set_text(f"Reference was not completed: {page['attempt_error']}")
            hide_result()
            return
        if page["pending"] and not page["series_running"] and result is page["hidden_result"]:
            status_label.set_text("Capturing and calibrating the selected GOX reference…")
            hide_result()
            return
        status_label.set_text(gox_reference.status())
        if result is page["shown"]:
            return
        page["shown"] = result
        show_raw(result.raw_output if result else "")
        session_label.set_text(f"Raw session: {result.session_dir}" if result and result.session_dir else "")
        analysis = result.analysis if result else None
        result_panel.set_visibility(analysis is not None)
        if result and not result.ok and not result.awaiting_selection:
            status_label.set_text(f"Reference was not completed: {result.reason}")
        if analysis is None:
            page["analysis"] = None
            for image in previews.values():
                image.set_source("")
            return
        # The result's chart and saved configuration belong to this capture, even if the
        # current input selection has changed or another client took a capture.
        result_label.set_text(f"{analysis.chart_label} · {analysis.patch_count} patches · "
                              f"{analysis.used_patches} used for fitting · {result.elapsed_s:.1f} s")
        capture_label.set_text(f"{analysis.pixel_format} · full scale {analysis.full_scale} DN · freerun")
        config_label.set_text(f"Saved capture configuration: {result.config_snapshot}")
        check_label.set_text(f"Exposure check: {_check_text(result.exposure_check)}" if result.exposure_check
                             else "Exposure check: no exposure series (run step 2).")
        save_btn.set_enabled(not result.saved)
        save_label.set_text(f"Saved: {result.session_dir}" if result.saved else
                            "Kept under tmp; save it once the exposure check passes.")
        page["analysis"] = analysis
        for key in previews:
            show_image(key, result.images.get(key, ""), result.images.get(f"{key}_full", ""))
        exposure_label.set_text(_reference_exposure_text(analysis.provenance))
        matrix_label.set_text("\n".join("  ".join(f"{float(value): .7f}" for value in row)
                                        for row in analysis.matrix))
        fit_label.set_text(f"Fit loss (RMS CIEDE2000): {analysis.loss:.6g} · "
                           f"{analysis.used_patches}/{analysis.patch_count} patches used")
        black_text, black_source = _black_level_text(analysis.provenance)
        black_label.set_text(black_text)
        black_details.set_text(black_source)
        rows = _patch_error_rows(analysis.provenance)
        patch_table.rows = rows
        patch_table.update()
        patch_table.set_visibility(bool(rows))
        patch_summary.set_text(_patch_error_summary(rows))

    async def capture() -> None:
        if page["pending"] or not page["alive"]:
            return
        reason = capability or camera_operations.guard_reason("gox") or input_reason()
        if reason or STATE.snapshot_busy:
            ui.notify(reason or "A camera tool is in progress", type="warning", multi_line=True)
            return
        # Keep the explicit chart selection for this capture.
        selected = page["chart_key"]
        page["pending"] = True
        page["attempt_error"] = ""
        page["hidden_result"] = gox_reference.last_result()
        hide_result()
        spinner.classes(remove="hidden")
        refresh()
        try:
            result = await gox_reference.capture(selected)
            if page["alive"]:
                notify_reference(result)
        except Exception as error:
            if page["alive"]:
                # Preflight failures may happen before the shared service
                # publishes a new result. Keep this attempt's error local to
                # the page until a new attempt or service result replaces it.
                page["attempt_error"] = str(error) or "Capture failed before a result was available"
                page["hidden_result"] = gox_reference.last_result()
                ui.notify(f"Color reference capture was not completed: {error}", type="negative", multi_line=True)
        finally:
            page["pending"] = False
            if page["alive"]:
                spinner.classes(add="hidden")
                refresh()

    def refresh_series() -> None:
        series_panel.refresh(gox_exposure.last_series(), page["pending"] and page["series_running"],
                             gox_exposure.status(), _series_rows, _series_text)

    async def run_series() -> None:
        if page["pending"] or not page["alive"]:
            return
        reason = capability or camera_operations.guard_reason("gox") or input_reason()
        if reason or STATE.snapshot_busy:
            ui.notify(reason or "A camera tool is in progress", type="warning", multi_line=True)
            return
        try:
            center = float(center_input.value)
            exposures = exposure.bracket(center)
        except (TypeError, ValueError) as error:
            ui.notify(f"Invalid initial exposure: {error}", type="warning")
            return
        page["pending"] = True
        page["series_running"] = True
        series_spinner.classes(remove="hidden")
        series_panel.status.set_text(f"Bracket {', '.join(f'{value:g}' for value in exposures)} ms…")
        series_panel.apply_note.set_text("")
        refresh()
        try:
            series = await gox_exposure.run(page["chart_key"], center)
            if page["alive"]:
                notify_series(series)
        except Exception as error:
            if page["alive"]:
                ui.notify(f"Exposure series was not completed: {error}", type="negative", multi_line=True)
        finally:
            page["pending"] = False
            page["series_running"] = False
            if page["alive"]:
                series_spinner.classes(add="hidden")
                series_panel.redraw()  # show the final status
                refresh()

    async def apply_suggestion() -> None:
        series = gox_exposure.last_series()
        suggestion = series.suggestion if series is not None else None
        if suggestion is None or not series.camera_ip:
            ui.notify("No exposure suggestion with a known camera address is available", type="warning")
            return
        try:
            summary = config_store.apply_gox_acquisition(series.camera_ip, "", float(suggestion.exposure_ms), None)
        except Exception as error:
            ui.notify(f"Apply failed: {error}", type="negative", multi_line=True)
            return
        app.storage.general["gox_exposure_ms"] = float(suggestion.exposure_ms)
        center_input.set_value(float(suggestion.exposure_ms))
        series_panel.apply_note.set_text(f"Applied: {summary}")
        ui.notify(f"Applied {suggestion.exposure_ms:g} ms; capture again (step 3) to verify.",
                  type="positive", multi_line=True)

    async def save_reference() -> None:
        try:
            result = await gox_reference.save_to_data()
        except Exception as error:
            ui.notify(f"Reference was not saved: {error}", type="negative", multi_line=True)
            return
        ui.notify(f"Reference saved: {result.session_dir}", type="positive", multi_line=True)
        refresh()

    def select_chart(key: str) -> None:
        if not page["alive"] or page["pending"] or STATE.snapshot_busy:
            return
        page["chart_key"] = key
        refresh()

    ui.timer(TOOL_TICK_S, refresh)
    refresh()
