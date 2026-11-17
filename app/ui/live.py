"""Data Live page: on-demand preview of the RUNNING acquisition session.

Nothing auto-refreshes. Each click reads completed data from the current session;
FX10 uses a bounded frame-count window (see services/live_view.py)."""

from __future__ import annotations

from nicegui import ui

from . import fx10_plot, layout
from .components import PREVIEW_DETAILS, heading, hint, info, preview_image
from ..constants import TOOL_TICK_S
from ..services import live_view, preview_images
from ..state import STATE, ProcState


@ui.page("/live")
def live_page() -> None:
    with layout.frame("Data Live"):
        pending = {"gox": False, "fx10": False}
        versions = {"gox": 0, "fx10": 0}
        page = {"alive": True, "generation": STATE.session_generation, "gox_urls": []}

        def release_gox_urls() -> None:
            preview_images.release(*page["gox_urls"])
            page["gox_urls"] = []

        def dispose() -> None:
            page["alive"] = False
            release_gox_urls()
            versions["gox"] += 1
            versions["fx10"] += 1

        ui.context.client.on_delete(dispose)
        guard_label = ui.label("").classes("text-sm muted")

        with ui.element("div").classes("workspace-grid"):
            # -------------------------------------------------------- GoX
            with ui.column().classes("console-panel w-full min-w-0"):
                heading("GoX image preview", icon="photo_camera")
                hint("Newest recorded frame.")
                with ui.row().classes("console-toolbar"):
                    gox_btn = ui.button("Fetch frame", icon="photo_camera", on_click=lambda: fetch("gox"))
                    gox_busy = ui.spinner(size="sm").classes("hidden")
                gox_image = preview_image("image_search", "No recorded frame displayed")
                with ui.row().classes("w-full items-center gap-1") as gox_download_row:
                    gox_download = ui.link("Download full-resolution 16-bit PNG", "#").classes("text-xs")
                    info(PREVIEW_DETAILS)
                gox_download_row.set_visibility(False)
                gox_meta = ui.label("").classes("text-sm muted mono break-all")

            # -------------------------------------------------------- FX10
            with ui.column().classes("console-panel w-full min-w-0"):
                heading("FX10 spectral statistics", icon="ssid_chart")
                hint("Raw DN per band over the last ~1 s of frames.")
                with ui.row().classes("console-toolbar"):
                    fx10_btn = ui.button("Fetch spectrum", icon="ssid_chart", on_click=lambda: fetch("fx10"))
                    fx10_busy = ui.spinner(size="sm").classes("hidden")
                with ui.column().classes("plot-surface w-full"):
                    chart = ui.echart(fx10_plot.workbench_options()).classes("w-full h-80")
                    ui.label("No spectrum displayed").classes(
                        "empty-state w-full text-xs").bind_visibility_from(
                        chart.options, "series", backward=lambda series: not series)
                fx10_axis_note = ui.label("").classes("text-xs muted")
                fx10_meta = ui.label("").classes("text-sm muted mono break-all")

        # ---------------------------------------------------------------- glue
        def refresh_guard() -> None:
            if not page["alive"]:
                return
            if page["generation"] != STATE.session_generation:
                page["generation"] = STATE.session_generation
                versions["gox"] += 1
                versions["fx10"] += 1
                gox_image.set_source("")
                gox_download_row.set_visibility(False)
                release_gox_urls()
                gox_meta.set_text("")
                fx10_meta.set_text("")
                fx10_axis_note.set_text("")
                chart.options.update(fx10_plot.workbench_options())
                chart.update()
            # Pure state reads, and NiceGUI drops setter calls that change
            # nothing — an idle tick sends nothing however often it runs.
            running = (STATE.process_state == ProcState.RUNNING and STATE.ownership_verified
                       and not STATE.control_uncertain)
            tone = "text-warning"
            if STATE.control_uncertain or (STATE.process_state is ProcState.RUNNING
                                           and not STATE.ownership_verified):
                reason = "Acquisition state unverified; previews unavailable."
            elif STATE.process_state is ProcState.STARTING:
                reason = "Recording is starting."
            elif STATE.process_state is ProcState.STOPPING:
                reason = "Recording is stopping."
            elif STATE.process_state is ProcState.FAILED:
                reason = "Recording failed; see Overview and Logs."
                tone = "text-negative"
            else:
                reason = "" if running else "Start a recording on the Overview page first."
                tone = "muted"
            guard_label.set_text(reason)
            guard_label.classes(add=tone, remove=" ".join(
                name for name in ("muted", "text-warning", "text-negative") if name != tone))
            gox_btn.set_enabled(running and STATE.enables_at_start.get("gox", False)
                                and not pending["gox"] and not live_view.busy())
            fx10_btn.set_enabled(running and STATE.enables_at_start.get("fx10", False)
                                 and not pending["fx10"] and not live_view.busy())

        ui.timer(TOOL_TICK_S, refresh_guard)
        refresh_guard()

        def show_gox(result) -> None:
            release_gox_urls()
            page["gox_urls"] = [result.image_url, result.full_image_url]
            gox_image.set_source(result.image_url)
            gox_download.props["href"] = preview_images.download_url(result.full_image_url)
            gox_download_row.set_visibility(True)
            gox_meta.set_text(f"{result.camera} · seq {result.seq} · {result.width}×{result.height} · "
                              f"{result.pixel_format} · {result.age_s:.1f}s ago")

        def show_fx10(result) -> None:
            chart.options.update(fx10_plot.workbench_options(
                result.spectrum_dn, full_scale=result.full_scale, bands=result.bands,
                wavelengths_nm=result.wavelengths_nm,
            ))
            chart.update()
            fx10_axis_note.set_text(fx10_plot.axis_note(result.wavelengths_nm))
            fx10_meta.set_text(
                f"{result.frames} frames · {result.bands} bands · {result.samples} px/line · "
                f"{result.pixel_format} · full scale {result.full_scale} DN · newest {result.age_s:.1f}s ago")

        # kind -> (button, spinner, status label, service fetch, display)
        panels = {"gox": (gox_btn, gox_busy, gox_meta, live_view.fetch_gox, show_gox),
                  "fx10": (fx10_btn, fx10_busy, fx10_meta, live_view.fetch_fx10, show_fx10)}

        async def fetch(kind: str) -> None:
            button, spinner, meta, fetch_preview, show = panels[kind]
            if pending[kind] or not page["alive"]:
                return
            pending[kind] = True
            versions[kind] += 1
            version = versions[kind]
            spinner.classes(remove="hidden")
            button.set_enabled(False)
            try:
                result = await fetch_preview()
            except live_view.PreviewObsolete:
                return
            except Exception as e:
                if page["alive"] and version == versions[kind]:
                    ui.notify(f"Preview unavailable: {e}", type="warning", multi_line=True)
                return
            finally:
                pending[kind] = False
                if page["alive"]:
                    spinner.classes(add="hidden")
                    refresh_guard()
            if not page["alive"] or version != versions[kind] or not live_view.is_current(result.request):
                return
            if not result.ok:
                ui.notify(result.reason, type="warning", multi_line=True)
                meta.set_text(result.reason)
                return
            show(result)
