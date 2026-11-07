"""GOX ColorChecker capture controls and in-memory calibration results."""

from __future__ import annotations

from pathlib import Path

from nicegui import app, ui

from ..constants import SNAPSHOT_CONFIG, TOOL_TICK_S
from ..decoder.gox import exposure
from ..decoder.gox.color_checker import CHARTS, DOCUMENTED_PEDESTAL_DN, PEDESTAL_SOURCE
from ..decoder.gox.preview import DISPLAY_LONG_EDGE
from ..services import config_actions, gox_exposure, gox_reference, preview_images
from ..state import STATE

_RESOURCES = Path(__file__).resolve().parent.parent / "resource"


def _reference_exposure_text(provenance: dict) -> str:
    exposure = provenance.get("reference_exposure")
    if exposure:
        return (f"Reference exposure: {exposure['exposure_time_us']:.2f} µs integration × gain "
                f"{exposure['gain']:.3g} · {exposure['device_model']} #{exposure['device_id']} · "
                f"reusable calibration saved with exposure-ratio compensation (black level "
                f"{provenance.get('black_level', {}).get('pedestal_dn', 0):g} DN subtracted).")
    reason = provenance.get("reference_exposure_unavailable_reason") or "unknown"
    return f"No reusable calibration saved: reference exposure unavailable ({reason})."


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


def _black_level_text(provenance: dict) -> str:
    black = provenance.get("black_level")
    if not black:
        return "Black level: not subtracted (calibration made before the documented pedestal default)."
    text = (f"Black level: {black['pedestal_dn']:g} DN subtracted from every channel before the fit and the "
            f"correction ({black['status']}). Source: {black['source']}")
    return text + "".join(f" Warning: {warning}." for warning in black.get("warnings", []))


_STATE_LABELS = {None: "ok", "clipped": "saturated (clipped photosite)", "near_saturation": "near saturation"}


def _series_rows(series: gox_exposure.ExposureSeries) -> list[dict]:
    rows = []
    for index, item in enumerate(series.points):
        point = item.point
        note = item.error or ("geometry from another capture" if item.geometry == "supplied" else "")
        rows.append({
            "key": index, "exposure": f"{item.requested_exposure_ms:g}",
            "integration": f"{point.exposure_time_us:.0f}" if point else "—",
            "peak": f"{point.peak_dn:.0f} ({point.peak_channel})" if point else "—",
            "fs": f"{point.peak_dn / 4095 * 100:.1f}" if point else "—",
            "state": _STATE_LABELS.get(point.status, point.status) if point else "not measured",
            "loss": f"{item.loss:.3g}" if item.loss is not None else "—", "note": note,
        })
    for index, check in enumerate(series.checks):
        point = check.point
        rows.append({
            "key": f"check{index}", "exposure": "verification",
            "integration": f"{point.exposure_time_us:.0f}", "peak": f"{point.peak_dn:.0f} ({point.peak_channel})",
            "fs": f"{point.peak_dn / 4095 * 100:.1f}", "state": _STATE_LABELS.get(point.status, point.status),
            "loss": "—", "note": check.message,
        })
    return rows


def _series_text(series: gox_exposure.ExposureSeries) -> str:
    if not series.ok or series.fit is None:
        return f"No exposure suggestion: {series.reason or series.fit_error}"
    fit = series.fit
    lines = [
        f"Brightest patch #{series.patch}: peak = {fit.offset_dn:.0f} DN + {fit.slope_dn_per_us * 1000:.3f} DN/ms "
        f"× integration (RMS residual {fit.residual_rms_dn:.1f} DN; {fit.points_used} points fitted, "
        f"{fit.points_excluded} saturated excluded). Fitted black offset ≈ {fit.offset_dn:.0f} DN.",
        f"ETTR target {fit.target_dn:.0f} DN ({fit.target_fraction:.0%} FS) → "
        f"{fit.suggested_exposure_ms:g} ms (gain unchanged at x{series.gain:g}).",
    ]
    if series.limit_notes:
        lines.append("Exposure ceiling: " + "; ".join(series.limit_notes))
    initial = series.initial_suggestion
    if initial is not None:
        lines.append(f"Suggested exposure: {initial.exposure_ms:g} ms."
                     + (f" {initial.note}" if initial.note else ""))
    lines += [f"⚠ {warning}" for warning in fit.warnings]
    if series.checks:
        lines.append(f"Last verification: {_check_text(series.checks[-1])}")
    return "\n".join(lines)


def _check_text(check) -> str:
    if check is None:
        return ""
    point = check.point
    measured = f"patch #{point.patch} peak {point.peak_dn:.0f} DN ({point.peak_dn / 4095:.1%} FS)"
    if check.deviation is None:
        return f"{measured}, saturated. {check.message}"
    return (f"{measured} vs target {check.target_dn:.0f} DN: {check.deviation:+.1%} "
            f"({'within' if check.within else 'outside'} ±{exposure.TOLERANCE:.0%}). {check.message}")


def panel() -> None:
    page = {"alive": True, "pending": False, "shown": None,
            "attempt_error": "", "hidden_result": None, "chart_key": None,
            "analysis": None,
            "series_shown": False, "notified": None}

    def dispose() -> None:
        page["alive"] = False

    ui.context.client.on_delete(dispose)
    capability = gox_reference.capability_reason()

    with ui.column().classes("console-panel w-full gap-3"):
        with ui.row().classes("section-heading w-full"):
            ui.label("GOX · ColorChecker reference").classes("section-title")
            ui.label("Target: sRGB · D65").classes("text-xs mono muted")
        ui.label("Place one chart in the camera view, with every patch visible and evenly lit. "
                 "Choose its format explicitly before capture. The images below identify chart layouts; "
                 "they are illustrations, not calibration color references.").classes("section-copy")
        ui.label("1. Select the chart in front of the camera").classes("font-medium text-sm")
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

        ui.label("Click a chart image to select its format. A selection is required before capture.").classes(
            "section-copy")
        ui.label("2. Auto exposure (ETTR, optional)").classes("font-medium text-sm")
        ui.label(f"Set an initial exposure in the Camera workbench without the chart first. The bracket captures "
                 f"the chart at the initial exposure and ± {exposure.BRACKET_STEP_MS:g} ms (half the initial "
                 f"exposure as the low step when it is {exposure.BRACKET_STEP_MS:g} ms or less), fits the "
                 "brightest patch's DN against exposure, and suggests the exposure that puts it at "
                 f"{exposure.TARGET_FRACTION:.0%} of full scale. Only the exposure changes: the gain stays as "
                 "the snapshot configuration sets it. Captures run one after another; each capture's chart "
                 "analysis runs alongside the next capture.").classes("section-copy")
        with ui.row().classes("console-toolbar w-full items-end"):
            center_input = ui.number("Initial exposure (ms)", min=0.001, step=1.0, format="%.3f",
                                     value=float(app.storage.general.get("gox_exposure_ms", 10.0))).classes("w-48")
            series_btn = ui.button("Run exposure bracket", icon="exposure", on_click=lambda: run_series())
            series_spinner = ui.spinner(size="sm").classes("hidden")
        series_status = ui.label("").classes("text-sm break-all")
        series_table = ui.table(columns=[
            {"name": "exposure", "label": "Requested (ms)", "field": "exposure", "align": "right"},
            {"name": "integration", "label": "Integration (µs)", "field": "integration", "align": "right"},
            {"name": "peak", "label": "Patch peak (DN)", "field": "peak", "align": "right"},
            {"name": "fs", "label": "% FS", "field": "fs", "align": "right"},
            {"name": "state", "label": "Status", "field": "state", "align": "left"},
            {"name": "loss", "label": "CCM loss (ΔE00)", "field": "loss", "align": "right"},
            {"name": "note", "label": "Note", "field": "note", "align": "left"},
        ], rows=[], row_key="key").props("dense flat").classes("w-full")
        series_table.set_visibility(False)
        series_fit = ui.label("").classes("section-copy whitespace-pre-line")
        with ui.row().classes("console-toolbar w-full items-center") as apply_row:
            apply_btn = ui.button("Apply suggested exposure to config", icon="save",
                                  on_click=lambda: apply_suggestion())
            apply_note = ui.label("").classes("text-sm muted break-all")
        apply_row.set_visibility(False)
        ui.label("3. Capture using the GOX snapshot configuration").classes("font-medium text-sm")
        ui.label("The camera, exposure, gain and image settings come from the GOX snapshot configuration. "
                 "Capture uses freerun with PTP and hardware trigger disabled.").classes("section-copy")
        ui.label(f"Configuration: {SNAPSHOT_CONFIG}").classes("text-xs mono muted break-all")
        ui.label(f"Black level: calibration subtracts a {DOCUMENTED_PEDESTAL_DN:g} DN pedestal from every "
                 f"channel, (DN − {DOCUMENTED_PEDESTAL_DN:g}) / (4095 − {DOCUMENTED_PEDESTAL_DN:g}), before "
                 f"fitting and correcting. Source: {PEDESTAL_SOURCE} The RAW keeps the original DN."
                 ).classes("section-copy")
        with ui.row().classes("console-toolbar w-full"):
            capture_btn = ui.button("Capture & calibrate", icon="color_lens", on_click=lambda: capture())
            spinner = ui.spinner(size="sm").classes("hidden")
        input_label = ui.label("").classes("text-sm muted")
        guard_label = ui.label("").classes("text-sm text-red-700")
        status_label = ui.label("").classes("text-sm break-all")
        session_label = ui.label("").classes("text-xs mono muted break-all")
        ui.label("The capture keeps the original ebus-raw data. Calibration affects the preview below; "
                 "it does not change the recorded RAW or apply camera settings to future recordings.").classes(
            "section-copy")

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
                    downloads[key] = ui.link("Download full-resolution 16-bit PNG", "#").classes("text-xs")
        ui.label(f"On-screen images are 8-bit previews downscaled to at most {DISPLAY_LONG_EDGE} px. "
                 "Downloads are lossless full-resolution 16-bit PNGs regenerated from the saved RAW; "
                 "the first download of each takes a few seconds.").classes("section-copy")
        with ui.column().classes("console-panel w-full gap-2"):
            ui.label("Color correction matrix · 3 × 3").classes("section-title")
            matrix_label = ui.label("").classes("mono text-sm whitespace-pre")
            ui.label("(Camera RGB − black level) / (full scale − black level), row vector × matrix → linear "
                     "sRGB (D65). The calibrated preview is a fixed display rendering of that result; it does "
                     "not change the matrix, the fit or the RAW.").classes("section-copy")
            fit_label = ui.label("").classes("section-copy")
            patch_summary = ui.label("").classes("section-copy")
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
            ui.label("ΔE00 compares each patch's corrected color with the chart reference (CIE L*a*b* D65). "
                     "Δ = corrected − reference: ΔL* > 0 is too light, Δa* > 0 too red/magenta, "
                     "Δb* > 0 too yellow. The RMS of this column equals the fit loss.").classes("section-copy")
            black_label = ui.label("").classes("section-copy")
            exposure_label = ui.label("").classes("section-copy")
            ui.label("Fit loss describes this chart capture, not an independent color accuracy validation. "
                     "Re-capture when the illumination, exposure or camera processing changes.").classes("section-copy")
    result_panel.set_visibility(False)
    with ui.expansion("Raw output of the last GOX reference capture", icon="terminal").classes("w-full"):
        raw_output = ui.label("").classes("terminal-surface w-full text-xs mono whitespace-pre-wrap break-all p-4")

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
        raw_output.set_text("")
        for image in previews.values():
            image.set_source("")

    def refresh() -> None:
        if not page["alive"]:
            return
        busy = page["pending"] or STATE.snapshot_busy
        reason = capability or gox_reference.guard_reason()
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
        apply_btn.set_enabled(not busy)
        refresh_series()
        result = gox_reference.last_result()
        if page["attempt_error"] and result is not page["hidden_result"]:
            page["attempt_error"] = ""
        if page["attempt_error"]:
            status_label.set_text(f"Reference was not completed: {page['attempt_error']}")
            hide_result()
            return
        if page["pending"] and not page.get("series_running") and result is page["hidden_result"]:
            status_label.set_text("Capturing and calibrating the selected GOX reference…")
            hide_result()
            return
        status_label.set_text(gox_reference.status())
        if result is page["shown"]:
            return
        page["shown"] = result
        raw_output.set_text(result.raw_output if result else "")
        session_label.set_text(f"Raw session: {result.session_dir}" if result and result.session_dir else "")
        analysis = result.analysis if result else None
        result_panel.set_visibility(analysis is not None)
        if result and not result.ok:
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
                             else "Exposure check: no applicable exposure series (run step 2 for ETTR).")
        save_btn.set_enabled(not result.saved)
        save_label.set_text(f"Saved: {result.session_dir}" if result.saved else
                            "Captured under the output directory's tmp folder; save it when the exposure "
                            "check passes.")
        page["analysis"] = analysis
        for key in previews:
            show_image(key, result.images.get(key, ""), result.images.get(f"{key}_full", ""))
        exposure_label.set_text(_reference_exposure_text(analysis.provenance))
        matrix_label.set_text("\n".join("  ".join(f"{float(value): .7f}" for value in row)
                                        for row in analysis.matrix))
        fit_label.set_text(f"Fit loss (RMS CIEDE2000): {analysis.loss:.6g} · "
                           f"{analysis.used_patches}/{analysis.patch_count} patches used")
        black_label.set_text(_black_level_text(analysis.provenance))
        rows = _patch_error_rows(analysis.provenance)
        patch_table.rows = rows
        patch_table.update()
        patch_table.set_visibility(bool(rows))
        patch_summary.set_text(_patch_error_summary(rows))

    async def capture() -> None:
        if page["pending"] or not page["alive"]:
            return
        reason = capability or gox_reference.guard_reason() or input_reason()
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
                ui.notify(f"{CHARTS[result.chart_key].label} reference captured and calibrated" if result.ok
                          else result.reason, type="positive" if result.ok else "negative", multi_line=True)
                check = result.exposure_check
                if check is not None and check.within and result.ok:
                    ui.notify("Exposure is within ±5% of the ETTR target. Save this reference to the output "
                              "directory with the button below the result.", type="positive", multi_line=True,
                              timeout=15000)
                elif check is not None:
                    ui.notify(f"Exposure check: {check.message}", type="warning", multi_line=True,
                              timeout=15000)
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
        series = gox_exposure.last_series()
        if page["pending"] and page.get("series_running"):
            series_status.set_text(gox_exposure.status())
        if series is page["series_shown"]:
            return
        page["series_shown"] = series
        series_table.set_visibility(series is not None and bool(series.points))
        series_table.rows = _series_rows(series) if series is not None else []
        series_table.update()
        series_fit.set_text(_series_text(series) if series is not None else "")
        if series is not None and not page.get("series_running"):
            series_status.set_text(f"Exposure series: {series.directory}" if series.directory else "")
        suggestion = series.suggestion if series is not None else None
        apply_row.set_visibility(suggestion is not None)
        if suggestion is not None:
            apply_btn.set_text(f"Apply exposure {suggestion.exposure_ms:g} ms to config")

    async def run_series() -> None:
        if page["pending"] or not page["alive"]:
            return
        reason = capability or gox_reference.guard_reason() or input_reason()
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
        series_status.set_text(f"Bracket {', '.join(f'{value:g}' for value in exposures)} ms…")
        apply_note.set_text("")
        refresh()
        try:
            series = await gox_exposure.run(page["chart_key"], center)
            if page["alive"]:
                ui.notify(f"Suggested ETTR exposure: {series.suggestion.exposure_ms:g} ms" if series.ok
                          else f"Exposure series failed: {series.reason}",
                          type="positive" if series.ok else "negative", multi_line=True)
        except Exception as error:
            if page["alive"]:
                ui.notify(f"Exposure series was not completed: {error}", type="negative", multi_line=True)
        finally:
            page["pending"] = False
            page["series_running"] = False
            if page["alive"]:
                series_spinner.classes(add="hidden")
                page["series_shown"] = False  # force a redraw with the final status
                refresh()

    async def apply_suggestion() -> None:
        series = gox_exposure.last_series()
        suggestion = series.suggestion if series is not None else None
        if suggestion is None or not series.camera_ip:
            ui.notify("No exposure suggestion with a known camera address is available", type="warning")
            return
        try:
            summary = await config_actions.change("apply_gox_acquisition", series.camera_ip, "",
                                                  float(suggestion.exposure_ms), None)
        except Exception as error:
            ui.notify(f"Apply failed: {error}", type="negative", multi_line=True)
            return
        app.storage.general["gox_exposure_ms"] = float(suggestion.exposure_ms)
        center_input.set_value(float(suggestion.exposure_ms))
        apply_note.set_text(f"Applied: {summary}")
        ui.notify(f"Applied exposure {suggestion.exposure_ms:g} ms (gain unchanged). Now use Capture & "
                  "calibrate (step 3) to verify the exposure.", type="positive", multi_line=True)

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
