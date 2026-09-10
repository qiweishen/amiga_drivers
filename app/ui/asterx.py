"""AsteRx live telemetry and independent, read-only history playback."""

from __future__ import annotations

import math
from datetime import datetime, timezone

from nicegui import ui

from ..constants import TOOL_TICK_S
from ..services.asterx_live import LIVE, STALE_S, sample_age_s
from ..services.asterx_replay import HistoryPlayer
from ..state import STATE, ProcState
from . import layout

_FIX_TEXT = {0: "No PVT", 1: "Stand-Alone", 2: "DGNSS", 3: "Fixed pos",
             4: "RTK Fixed", 5: "RTK Float", 6: "SBAS", 7: "MB RTK Fixed",
             8: "MB RTK Float", 10: "PPP"}
_FIX_COLOR = {4: "green", 7: "green", 5: "orange", 8: "orange"}

# Body axes of the FRD triad: label, color, unit vector in the body frame
_FRD_AXES = (("F", "#e53935"), ("R", "#43a047"), ("D", "#1e88e5"))


def _is_live() -> bool:
    return (STATE.env_ok and STATE.process_state is ProcState.RUNNING and STATE.ownership_verified
            and not STATE.control_uncertain and STATE.enables_at_start.get("asterx", False)
            and LIVE.session_dir is not None and LIVE.session_dir == STATE.active_session)


def _guard_text() -> str:
    if LIVE.session_dir is None:
        return "No live recording yet — start recording with AsteRx enabled, or load history below"
    if STATE.control_uncertain:
        return "Acquisition ownership is unknown — displayed telemetry is not verified live"
    if not _is_live():
        return f"Showing session {LIVE.session_dir.name} — not live"
    return "Live telemetry · freshness uses recorded host time"


def _stamp(ns: int | None) -> str:
    if ns is None or ns <= 0:
        return "Unknown"
    try:
        seconds, nano = divmod(ns, 1_000_000_000)
        return datetime.fromtimestamp(seconds, timezone.utc).strftime("%Y-%m-%d %H:%M:%S") + f".{nano // 1_000_000:03d} UTC"
    except (OverflowError, OSError, ValueError):
        return "Unknown"


def _finite(*vals: float) -> bool:
    return all(math.isfinite(v) for v in vals)


def _fmt(v: float | None, fmt: str = "{:.2f}", suffix: str = "") -> str:
    if v is None or not math.isfinite(v):
        return "—"
    return fmt.format(v) + suffix


def _uptime(s: int | None) -> str:
    if s is None:
        return "—"
    d, rem = divmod(s, 86400)
    h, m = divmod(rem, 3600)
    return f"{d}d {h:02d}:{m // 60:02d}"


def _frd_svg(heading_deg: float, pitch_deg: float, roll_deg: float) -> str:
    """Isometric view of the body FRD triad in the local NED frame.

    Projection: N to the upper-left, E to the upper-right, D straight down —
    the grey dashed axes are the fixed NED reference, the colored arrows are
    the body Front/Right/Down axes rotated by heading/pitch/roll (ZYX)."""
    size, c, scale = 240, 120, 78

    def project(n: float, e: float, d: float) -> tuple[float, float]:
        return c + (e - n) * 0.866 * scale, c - (n + e) * 0.5 * scale + d * scale

    parts = [f'<svg width="{size}" height="{size}" viewBox="0 0 {size} {size}" '
             f'xmlns="http://www.w3.org/2000/svg">']
    parts.append("<defs>" + "".join(
        f'<marker id="frd_ah{i}" markerWidth="8" markerHeight="8" refX="6" refY="3" orient="auto">'
        f'<path d="M0,0 L6,3 L0,6 z" fill="{color}"/></marker>'
        for i, (_, color) in enumerate(_FRD_AXES)) + "</defs>")

    for label, vec in (("N", (1.0, 0.0, 0.0)), ("E", (0.0, 1.0, 0.0)), ("D", (0.0, 0.0, 1.0))):
        x, y = project(*(v * 0.95 for v in vec))
        parts.append(f'<line x1="{c}" y1="{c}" x2="{x:.1f}" y2="{y:.1f}" '
                     f'stroke="#bbbbbb" stroke-dasharray="4 3"/>')
        parts.append(f'<text x="{x:.1f}" y="{y:.1f}" fill="#999999" font-size="11">{label}</text>')

    if _finite(heading_deg, pitch_deg, roll_deg):
        sy, cy = math.sin(math.radians(heading_deg)), math.cos(math.radians(heading_deg))
        sp, cp = math.sin(math.radians(pitch_deg)), math.cos(math.radians(pitch_deg))
        sr, cr = math.sin(math.radians(roll_deg)), math.cos(math.radians(roll_deg))
        # Columns of the body->NED DCM (ZYX Euler: yaw=heading, pitch, roll)
        body_axes_ned = (
            (cp * cy, cp * sy, -sp),  # Front
            (sr * sp * cy - cr * sy, sr * sp * sy + cr * cy, sr * cp),  # Right
            (cr * sp * cy + sr * sy, cr * sp * sy - sr * cy, cr * cp),  # Down
        )
        for i, ((label, color), vec) in enumerate(zip(_FRD_AXES, body_axes_ned)):
            x, y = project(*vec)
            parts.append(f'<line x1="{c}" y1="{c}" x2="{x:.1f}" y2="{y:.1f}" '
                         f'stroke="{color}" stroke-width="3" marker-end="url(#frd_ah{i})"/>')
            tx, ty = project(*(v * 1.15 for v in vec))
            parts.append(f'<text x="{tx:.1f}" y="{ty:.1f}" fill="{color}" font-size="13" '
                         f'font-weight="bold" text-anchor="middle">{label}</text>')
    else:
        parts.append(f'<text x="{c}" y="{c}" fill="#999999" font-size="12" '
                     f'text-anchor="middle">no attitude</text>')
    parts.append("</svg>")
    return "".join(parts)


def _alert(label: ui.label, on: bool, color: str = "text-red-700") -> None:
    label.classes(**({"add": color} if on else {"remove": color}))


def _kv_card(title: str, keys: list[str]) -> tuple[ui.card, ui.label, dict[str, ui.label]]:
    """Pre-built labels; the timer only calls set_text (never rebuilds)."""
    with ui.card().classes("w-72") as card:
        title_label = ui.label(title).classes("font-bold")
        labels: dict[str, ui.label] = {}
        for k in keys:
            with ui.row().classes("items-center justify-between w-full gap-2"):
                ui.label(k).classes("text-xs text-gray-500")
                labels[k] = ui.label("—").classes("text-sm font-mono")
    return card, title_label, labels


@ui.page("/asterx")
def asterx_page(session: str = "") -> None:
    player = HistoryPlayer()
    page = {"alive": True, "scrubbing": False}

    def close() -> None:
        page["alive"] = False
        player.close()

    ui.context.client.on_delete(close)
    with layout.frame("AsteRx · Live & History"):
        with ui.row().classes("items-center gap-4"):
            source_mode = ui.toggle({"live": "Live", "history": "History replay"}, value="history" if session else "live",
                                    on_change=lambda _: _mode_changed())
            source_badge = ui.badge("LIVE").props('color="blue-grey"')
        guard_label = ui.label("").classes("text-sm text-amber-700")

        with ui.column().classes("w-full gap-2 border rounded p-3") as history_controls:
            ui.label("Replay a finalized recording").classes("font-bold")
            ui.label("Enter a path on the GUI server: a session directory, raw/asterx directory, or a single CSV. "
                     "A directory loads both live_insnavgeod.csv and live_receiverstatus.csv when available."
                     ).classes("text-sm text-gray-600")
            with ui.row().classes("w-full items-center gap-2"):
                history_path = ui.input("Session directory or CSV path",
                                        value=session,
                                        placeholder="recordings/<session>/raw/asterx").classes("flex-1")
                load_btn = ui.button("Load history", icon="folder_open", on_click=lambda: _load())
                cancel_btn = ui.button("Cancel read", icon="close", on_click=player.cancel_read).props("flat")
                loading = ui.spinner(size="sm")
            with ui.row().classes("items-center gap-3"):
                play_btn = ui.button("Play", icon="play_arrow", on_click=lambda: _play())
                pause_btn = ui.button("Pause", icon="pause", on_click=player.pause).props("outline")
                restart_btn = ui.button("Restart", icon="replay", on_click=lambda: _seek(0.0)).props("outline")
                speed = ui.select({0.25: "0.25×", 0.5: "0.5×", 1.0: "1×", 2.0: "2×", 4.0: "4×", 8.0: "8×"},
                                  value=1.0, label="Speed",
                                  on_change=lambda e: player.set_speed(float(e.value))).classes("w-24")
                seek_position = ui.number("Jump to", value=0.0, min=0.0, step=0.1, suffix="s").classes("w-32")
                seek_btn = ui.button("Jump", icon="skip_next",
                                     on_click=lambda: _seek(seek_position.value)).props("outline")
            progress = ui.linear_progress(value=0.0, show_value=False).classes("w-full")
            timeline = ui.slider(min=0, max=1, step=0.01, value=0).props("label").classes("w-full")
            timeline.on("pan", lambda e: page.__setitem__("scrubbing", e.args["phase"] == "start"), ["phase"])
            timeline.on("change", lambda e: _seek(e.args), [None])
            index_progress = ui.label().classes("text-xs text-gray-600")
            gap_label = ui.label().classes("text-xs text-amber-700 whitespace-pre-line")
            playback_label = ui.label("").classes("text-sm font-mono")
            history_info = ui.label("").classes("text-xs text-gray-600 whitespace-pre-line break-all")
            history_error = ui.label("").classes("text-sm text-red-700 whitespace-pre-line")
            ui.label("Timeline: recorded host time. Playback only changes this page; it does not control the receiver. "
                     "The route is simplified for display.").classes("text-xs text-gray-500")

        with ui.column().classes("w-full"):
            with ui.row().classes("items-center gap-4"):
                fix_badge = ui.badge("No fix").props('color="grey"')
                follow = ui.switch("Follow vehicle", value=True)
            m = ui.leaflet(center=(0.0, 0.0), zoom=2).classes("w-full h-96")
            track_layer = m.generic_layer(name="polyline", args=[[], {"color": "#1976d2", "weight": 3}])
            marker_holder: list = [None]

        ui.separator()
        ui.label("Attitude and receiver health").classes("text-lg font-bold")
        with ui.row().classes("w-full gap-4 flex-wrap items-start"):
            with ui.card().classes("w-72") as frd_card:
                frd_title = ui.label("Attitude (INSNavGeod)").classes("font-bold")
                frd_html = ui.html(_frd_svg(float("nan"), float("nan"), float("nan")))
                frd_hpr = ui.label("—").classes("text-sm font-mono")
            ins_card, ins_title, ins_f = _kv_card("INS (4226)", [
                "Fix (GNSS)", "Error", "GNSS age", "Lat / Lon", "Height", "Accuracy", "Host CSV time"])
            rx_card, rx_title, rx_f = _kv_card("Receiver (4014)", [
                "CPU load", "Temperature", "Uptime", "RX error", "Ext error", "Host CSV time"])

        frd_seen = [-1]
        track_seen = [-1]
        source_seen = [None]

        def _history() -> bool:
            return source_mode.value == "history"

        def _data():
            return player.data if _history() else LIVE

        def _age(sample) -> float | None:
            return sample_age_s(sample, reference_ns=player.timestamp_ns) if _history() else sample_age_s(sample)

        def _stale_title(card: ui.card, title_label: ui.label, name: str, sample) -> None:
            age = _age(sample) if sample is not None else None
            if sample is None:
                suffix, dim = "no data", True
            elif _history():
                suffix = "history" if age is not None and age <= STALE_S else (
                    "history · time unknown" if age is None else f"history · gap {age:.1f}s")
                dim = age is None or age > STALE_S
            elif not _is_live():
                suffix, dim = "not live", True
            elif age is None:
                suffix, dim = "freshness unknown", True
            elif age > STALE_S:
                suffix, dim = f"stale {age:.1f}s", True
            else:
                suffix, dim = "live", False
            card.classes(**({"add": "opacity-40"} if dim else {"remove": "opacity-40"}))
            title_label.set_text(f"{name} — {suffix}")

        def _refresh_badge(data) -> None:
            ins = data.ins
            mode = (ins.gnss_mode & 0x0F) if ins is not None and ins.gnss_mode is not None else None
            text = _FIX_TEXT.get(mode, f"mode {mode}") if mode is not None else "Unknown fix"
            age = _age(ins) if ins is not None else None
            live = not _history() and _is_live() and age is not None and age <= STALE_S
            prefix = "History · " if _history() else ("" if live else "Last sample · ")
            fix_badge.set_text(prefix + text)
            fix_badge.props(f'color="{_FIX_COLOR.get(mode, "blue-grey") if live else "grey"}"')

        def _clear_labels(labels) -> None:
            for label in labels.values():
                label.set_text("—")
                label.classes(remove="text-red-700 text-amber-700")

        def _refresh_health(data) -> None:
            ins, rx = data.ins, data.rx
            _stale_title(ins_card, ins_title, "INS (4226)", ins)
            _stale_title(frd_card, frd_title, "Attitude (INSNavGeod)", ins)
            _stale_title(rx_card, rx_title, "Receiver (4014)", rx)
            if ins is None:
                _clear_labels(ins_f)
            else:
                mode = (ins.gnss_mode & 0x0F) if ins.gnss_mode is not None else None
                ins_f["Fix (GNSS)"].set_text(_FIX_TEXT.get(mode, "Unknown" if mode is None else f"mode {mode}"))
                _alert(ins_f["Error"], ins.error not in (None, 0))
                ins_f["Error"].set_text("Unknown" if ins.error is None else "OK" if ins.error == 0 else f"INS error {ins.error}")
                _alert(ins_f["GNSS age"], math.isfinite(ins.gnss_age_s) and ins.gnss_age_s > 1.0, "text-amber-700")
                ins_f["GNSS age"].set_text(_fmt(ins.gnss_age_s, "{:.2f}", " s"))
                ins_f["Lat / Lon"].set_text(f"{ins.lat_deg:.7f}, {ins.lon_deg:.7f}" if _finite(ins.lat_deg, ins.lon_deg) else "—")
                ins_f["Height"].set_text(_fmt(ins.height_m, "{:.2f}", " m"))
                ins_f["Accuracy"].set_text(_fmt(ins.accuracy_m, "{:.2f}", " m"))
                ins_f["Host CSV time"].set_text(_stamp(ins.host_unix_ns))
            if frd_seen[0] != data.snap_version:
                frd_seen[0] = data.snap_version
                angles = (ins.heading_deg, ins.pitch_deg, ins.roll_deg) if ins else (float("nan"),) * 3
                frd_html.set_content(_frd_svg(*angles))
                frd_hpr.set_text(f"H {_fmt(angles[0], '{:.1f}', '°')}  "
                                 f"P {_fmt(angles[1], '{:.1f}', '°')}  R {_fmt(angles[2], '{:.1f}', '°')}")
            if rx is None:
                _clear_labels(rx_f)
            else:
                _alert(rx_f["CPU load"], (rx.cpu_load_pct or 0) > 80, "text-amber-700")
                rx_f["CPU load"].set_text("—" if rx.cpu_load_pct is None else f"{rx.cpu_load_pct} %")
                rx_f["Temperature"].set_text(_fmt(rx.temp_c, "{:.0f}", " °C"))
                rx_f["Uptime"].set_text(_uptime(rx.up_time_s))
                _alert(rx_f["RX error"], rx.rx_error not in (None, 0))
                rx_f["RX error"].set_text("Unknown" if rx.rx_error is None else "OK" if rx.rx_error == 0 else f"RX ERROR 0x{rx.rx_error:X}")
                _alert(rx_f["Ext error"], rx.ext_error not in (None, 0), "text-amber-700")
                rx_f["Ext error"].set_text("Unknown" if rx.ext_error is None else "OK" if rx.ext_error == 0 else f"EXT ERROR 0x{rx.ext_error:X}")
                rx_f["Host CSV time"].set_text(_stamp(rx.host_unix_ns))

        def _remove_marker() -> None:
            if marker_holder[0] is not None:
                m.remove_layer(marker_holder[0])
                marker_holder[0] = None

        def _refresh_map(data) -> None:
            if not m.is_initialized:
                return
            if track_seen[0] != data.track_version:
                track_seen[0] = data.track_version
                latlngs = [[lat, lon] for lat, lon in data.track]
                track_layer.args[0] = latlngs
                m.run_layer_method(track_layer.id, "setLatLngs", latlngs)
            ins = data.ins
            age = _age(ins) if ins else None
            valid = (ins is not None and _finite(ins.lat_deg, ins.lon_deg)
                     and -90 <= ins.lat_deg <= 90 and -180 <= ins.lon_deg <= 180)
            if not valid or (not _history() and (not _is_live() or age is None or age > STALE_S)):
                _remove_marker()
                return
            lat, lon = ins.lat_deg, ins.lon_deg
            if marker_holder[0] is None:
                with m:
                    marker_holder[0] = m.marker(latlng=(lat, lon))
                m.set_center((lat, lon))
                m.set_zoom(17)
            else:
                marker_holder[0].move(lat, lon)
                if follow.value:
                    m.set_center((lat, lon))

        def _controls() -> None:
            history_controls.set_visibility(_history())
            ready = player.index is not None
            load_btn.set_enabled(not player.busy)
            cancel_btn.set_enabled(player.busy)
            play_btn.set_enabled(ready and not player.busy and not player.playing)
            pause_btn.set_enabled(player.playing)
            restart_btn.set_enabled(ready and not player.busy)
            seek_btn.set_enabled(ready and not player.busy)
            seek_position.set_enabled(ready and not player.busy)
            speed.set_enabled(not player.busy)
            loading.set_visibility(player.busy)
            history_error.set_text(player.error)
            name, done, total = player.progress
            index_progress.set_text(f"Indexing {name}: {done / total:.0%} ({done:,} / {total:,} bytes)"
                                    if player.busy and not ready and total else "")
            timeline.set_enabled(ready and not player.busy)
            if not ready:
                progress.set_value(0.0)
                playback_label.set_text("Loading…" if player.busy else "No history loaded")
                history_info.set_text("")
                gap_label.set_text("")
                return
            index = player.index
            timeline.props(f"max={max(0.01, index.duration_s)}")
            if not page["scrubbing"]:
                timeline.set_value(player.position_s)
            gap_label.set_text(f"{index.gap_count} stream gaps longer than 2 s; showing up to 512 intervals.\n" +
                               "\n".join(f"{name}: {(start - index.first_ns) / 1e9:.1f}–{(end - index.first_ns) / 1e9:.1f} s"
                                         for name, start, end in index.gaps if start <= (player.timestamp_ns or 0) < end)
                               if index.gap_count else "No stream gaps longer than 2 s")
            status = "Playing" if player.playing else "End" if player.position_s >= index.duration_s else "Paused"
            progress.set_value(min(1.0, player.position_s / index.duration_s) if index.duration_s else 1.0)
            playback_label.set_text(f"{status} · {player.position_s:.1f} / {index.duration_s:.1f} s · {_stamp(player.timestamp_ns)}")
            history_info.set_text("\n".join(
                f"{item.path} · {item.rows} rows · skipped {item.skipped} malformed/time-invalid rows · "
                f"ignored {item.partial} incomplete tail rows" for item in index.streams))

        def _render() -> None:
            if not page["alive"]:
                return
            data = _data()
            key = (source_mode.value, LIVE.session_dir if not _history() else id(player.index))
            if key != source_seen[0]:
                source_seen[0] = key
                frd_seen[0] = track_seen[0] = -1
                _remove_marker()
                track_layer.args[0] = []
                if m.is_initialized:
                    m.run_layer_method(track_layer.id, "setLatLngs", [])
            _controls()
            source_badge.set_text("HISTORY REPLAY" if _history() else "LIVE" if _is_live() else "NOT LIVE")
            source_badge.props(f'color="{"orange" if _history() else "primary" if _is_live() else "grey"}"')
            guard_label.set_text("Historical CSV playback — this page is independent of the running acquisition"
                                 if _history() else " · ".join(filter(None, (_guard_text(), LIVE.error))))
            _refresh_badge(data)
            _refresh_health(data)
            _refresh_map(data)

        async def _load() -> None:
            try:
                await player.load(history_path.value or "")
            except Exception as e:
                if page["alive"]:
                    ui.notify(f"History unavailable: {e}", type="negative", multi_line=True)
            _render()

        async def _seek(seconds) -> bool:
            page["scrubbing"] = False
            try:
                await player.seek(float(seconds))
            except Exception as e:
                if page["alive"]:
                    ui.notify(f"Cannot jump: {e}", type="warning", multi_line=True)
                return False
            _render()
            return True

        async def _play() -> None:
            if player.busy:
                return
            if player.index is not None and player.position_s >= player.index.duration_s:
                if not await _seek(0.0):
                    return
            player.play()
            _render()

        def _mode_changed() -> None:
            if not _history():
                player.pause()
            _render()

        async def _refresh() -> None:
            if not page["alive"]:
                return
            if _history():
                try:
                    await player.tick()
                except Exception:
                    pass  # player.error is persistent and displayed below the controls
            _render()

        ui.timer(TOOL_TICK_S, _refresh)
        if session:
            ui.timer(0.1, _load, once=True)
        _render()
