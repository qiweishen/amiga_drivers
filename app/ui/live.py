"""Data Live page: on-demand preview of the RUNNING acquisition session.

Nothing auto-refreshes. Each click reads completed data from the current session;
FX10 uses a bounded frame-count window (see services/live_view.py)."""

from __future__ import annotations

from nicegui import ui

from . import fx10_plot, layout
from ..constants import TOOL_TICK_S
from ..decoder.gox.preview import DISPLAY_LONG_EDGE
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
                ui.label("GoX image preview").classes("section-title")
                ui.label("Newest frame found in the last ~1 s of recorded data.").classes("section-copy")
                with ui.row().classes("console-toolbar"):
                    gox_btn = ui.button("Fetch frame", icon="photo_camera", on_click=lambda: _fetch_gox())
                    gox_busy = ui.spinner(size="sm").classes("hidden")
                with ui.column().classes("preview-surface w-full justify-center"):
                    gox_image = ui.interactive_image().classes("w-full")
                    gox_image.bind_visibility_from(gox_image, "source", backward=bool)
                    with ui.column().classes("empty-state w-full").bind_visibility_from(
                            gox_image, "source", backward=lambda source: not source):
                        ui.icon("image_search", size="36px")
                        ui.label("No recorded frame displayed")
                        ui.label("Use Fetch frame during an active GoX recording.").classes("text-xs")
                with ui.row().classes("w-full items-center gap-3") as gox_download_row:
                    gox_download = ui.link("Download full-resolution 16-bit PNG", "#").classes("text-xs")
                    ui.label(f"Screen preview: linear DN, 8-bit, downscaled to ≤ {DISPLAY_LONG_EDGE} px. "
                             "The download is lossless (DN × 16) and is decoded on first request.").classes(
                        "text-xs muted")
                gox_download_row.set_visibility(False)
                gox_meta = ui.label("").classes("text-sm muted mono break-all")

            # -------------------------------------------------------- FX10
            with ui.column().classes("console-panel w-full min-w-0"):
                ui.label("FX10 spectral statistics").classes("section-title")
                ui.label("Raw DN statistics over recent frames; window size follows the expected frame rate.").classes(
                    "section-copy")
                with ui.row().classes("console-toolbar"):
                    fx10_btn = ui.button("Fetch spectrum", icon="ssid_chart", on_click=lambda: _fetch_fx10())
                    fx10_busy = ui.spinner(size="sm").classes("hidden")
                with ui.column().classes("plot-surface w-full"):
                    chart = ui.echart(fx10_plot.workbench_options()).classes("w-full h-80")
                    ui.label("No spectrum displayed · use Fetch spectrum during an active FX10 recording.").classes(
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
                reason = "Acquisition state is unverified — previews are unavailable until the controller confirms it."
            elif STATE.process_state is ProcState.STARTING:
                reason = "Recording is starting — previews become available after sensor initialization."
            elif STATE.process_state is ProcState.STOPPING:
                reason = "Recording is stopping — previews are unavailable."
            elif STATE.process_state is ProcState.FAILED:
                reason = "Recording failed — review Overview and System logs before restarting."
                tone = "text-negative"
            else:
                reason = "" if running else "Recording is not running — start it on the Overview page first"
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
            release_gox_urls()
            page["gox_urls"] = [result.image_url, result.full_image_url]
            gox_image.set_source(result.image_url)
            gox_download.props["href"] = preview_images.download_url(result.full_image_url)
            gox_download_row.set_visibility(True)
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
            chart.options.update(fx10_plot.workbench_options(
                result.spectrum_dn, full_scale=result.full_scale, bands=result.bands,
                wavelengths_nm=result.wavelengths_nm,
            ))
            chart.update()
            fx10_axis_note.set_text(fx10_plot.axis_note(result.wavelengths_nm))
            fx10_meta.set_text(
                f"{result.frames} frames · {result.bands} bands · {result.samples} px/line · "
                f"{result.pixel_format} · full scale {result.full_scale} DN · newest {result.age_s:.1f}s ago"
            )
