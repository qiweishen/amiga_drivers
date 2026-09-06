"""AsteRx live telemetry page: INS map track + FRD attitude triad + health.

Read-only view over asterx_live.LIVE — unlike /camera there is no hardware
mutex here, so the guard is informational (a status line), never blocking.
"""

from __future__ import annotations

import math
import time

from nicegui import ui

from ..constants import TOOL_TICK_S
from ..services.asterx_live import LIVE, STALE_S
from ..state import STATE, ProcState
from . import layout

_FIX_TEXT = {0: "No PVT", 1: "Stand-Alone", 2: "DGNSS", 3: "Fixed pos",
             4: "RTK Fixed", 5: "RTK Float", 6: "SBAS", 7: "MB RTK Fixed",
             8: "MB RTK Float", 10: "PPP"}
_FIX_COLOR = {4: "green", 7: "green", 5: "orange", 8: "orange"}

# Body axes of the FRD triad: label, color, unit vector in the body frame
_FRD_AXES = (("F", "#e53935"), ("R", "#43a047"), ("D", "#1e88e5"))


def _guard_text() -> str:
    # Keyed on LIVE.session_dir (not STATE.active_session) so the sim-feed
    # hook works without faking process state.
    if LIVE.session_dir is None:
        return "No recording session yet — start recording (with AsteRx enabled) to see live data"
    if STATE.process_state in (ProcState.RUNNING, ProcState.STARTING) \
            and not STATE.enables_at_start.get("asterx", False):
        return "AsteRx is disabled in the current recording"
    if STATE.process_state not in (ProcState.RUNNING, ProcState.STARTING):
        return f"Showing the last session ({LIVE.session_dir.name}) — not live"
    return ""


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
def asterx_page() -> None:
    with layout.frame("AsteRx Live"):
        guard_label = ui.label("").classes("text-sm text-amber-700")

        # ------------------------------------------------------------- map
        with ui.column().classes("w-full"):
            with ui.row().classes("items-center gap-4"):
                fix_badge = ui.badge("No fix").props('color="grey"')
                follow = ui.switch("Follow vehicle", value=True)
            m = ui.leaflet(center=(0.0, 0.0), zoom=2).classes("w-full h-96")
            # The constructor already adds the OSM tile layer — keep it.
            track_layer = m.generic_layer(
                name="polyline", args=[[], {"color": "#1976d2", "weight": 3}])
            marker_holder: list = [None]  # lazy: no marker until the first fix

        # -------------------------------------------------- attitude + health
        ui.separator()
        ui.label("Attitude and receiver health").classes("text-lg font-bold")
        with ui.row().classes("w-full gap-4 flex-wrap items-start"):
            with ui.card().classes("w-72") as frd_card:
                frd_title = ui.label("Attitude (INSNavGeod)").classes("font-bold")
                frd_html = ui.html(_frd_svg(float("nan"), float("nan"), float("nan")))
                frd_hpr = ui.label("—").classes("text-sm font-mono")
            ins_card, ins_title, ins_f = _kv_card("INS (4226)", [
                "Fix (GNSS)", "Error", "GNSS age", "Lat / Lon", "Height", "Accuracy"])
            rx_card, rx_title, rx_f = _kv_card("Receiver (4014)", [
                "CPU load", "Temperature", "Uptime", "RX error", "Ext error"])

        # ------------------------------------------------------------- glue
        def _stale_title(card: ui.card, title_label: ui.label, name: str,
                         mono: float | None) -> None:
            if mono is None:
                card.classes(add="opacity-40")
                title_label.set_text(f"{name} — no data")
                return
            age = time.monotonic() - mono
            if age > STALE_S:
                card.classes(add="opacity-40")
                title_label.set_text(f"{name} — stale {age:.0f}s")
            else:
                card.classes(remove="opacity-40")
                title_label.set_text(name)

        def _refresh_badge() -> None:
            mode = (LIVE.ins.gnss_mode & 0x0F) if LIVE.ins is not None else None
            if not mode:  # no data yet, or mode 0 = No PVT
                fix_badge.set_text("No fix")
                fix_badge.props('color="grey"')
                return
            fix_badge.set_text(_FIX_TEXT.get(mode, f"mode {mode}"))
            fix_badge.props(f'color="{_FIX_COLOR.get(mode, "blue-grey")}"')

        frd_seen = [-1]

        def _refresh_health() -> None:
            ins = LIVE.ins
            _stale_title(ins_card, ins_title, "INS (4226)", ins.mono if ins else None)
            _stale_title(frd_card, frd_title, "Attitude (INSNavGeod)", ins.mono if ins else None)
            if ins is not None:
                ins_f["Fix (GNSS)"].set_text(
                    _FIX_TEXT.get(ins.gnss_mode & 0x0F, f"mode {ins.gnss_mode & 0x0F}"))
                _alert(ins_f["Error"], ins.error != 0)
                ins_f["Error"].set_text("OK" if ins.error == 0 else f"INS error {ins.error}")
                # gnss_age > 1 s = GNSS outage indicator
                _alert(ins_f["GNSS age"],
                       math.isfinite(ins.gnss_age_s) and ins.gnss_age_s > 1.0, "text-amber-700")
                ins_f["GNSS age"].set_text(_fmt(ins.gnss_age_s, "{:.2f}", " s"))
                ins_f["Lat / Lon"].set_text(
                    f"{ins.lat_deg:.7f}, {ins.lon_deg:.7f}"
                    if _finite(ins.lat_deg, ins.lon_deg) else "—")
                ins_f["Height"].set_text(_fmt(ins.height_m, "{:.2f}", " m"))
                ins_f["Accuracy"].set_text(_fmt(ins.accuracy_m, "{:.2f}", " m"))
                if frd_seen[0] != LIVE.snap_version:
                    frd_seen[0] = LIVE.snap_version
                    frd_html.set_content(_frd_svg(ins.heading_deg, ins.pitch_deg, ins.roll_deg))
                    frd_hpr.set_text(
                        f"H {_fmt(ins.heading_deg, '{:.1f}', '°')}  "
                        f"P {_fmt(ins.pitch_deg, '{:.1f}', '°')}  "
                        f"R {_fmt(ins.roll_deg, '{:.1f}', '°')}")

            rx = LIVE.rx
            _stale_title(rx_card, rx_title, "Receiver (4014)", rx.mono if rx else None)
            if rx is not None:
                _alert(rx_f["CPU load"], (rx.cpu_load_pct or 0) > 80, "text-amber-700")
                rx_f["CPU load"].set_text(
                    "—" if rx.cpu_load_pct is None else f"{rx.cpu_load_pct} %")
                rx_f["Temperature"].set_text(_fmt(rx.temp_c, "{:.0f}", " °C"))
                rx_f["Uptime"].set_text(_uptime(rx.up_time_s))
                _alert(rx_f["RX error"], rx.rx_error != 0)
                rx_f["RX error"].set_text(
                    "OK" if rx.rx_error == 0 else f"RX ERROR 0x{rx.rx_error:X}")
                # ExtError (p.373) reports errors in incoming data: SISERROR,
                # DIFFCORRERROR, EXTSENSORERROR, SETUPERROR. RxState bit 8 is
                # INTERNALDISK_FULL, not a general warning, so it is not used here.
                _alert(rx_f["Ext error"], rx.ext_error != 0, "text-amber-700")
                rx_f["Ext error"].set_text(
                    "OK" if rx.ext_error == 0 else f"EXT ERROR 0x{rx.ext_error:X}")

        def _position() -> tuple[float, float] | None:
            ins = LIVE.ins
            if ins is not None and time.monotonic() - ins.mono < STALE_S \
                    and _finite(ins.lat_deg, ins.lon_deg):
                return ins.lat_deg, ins.lon_deg
            return None

        track_seen = [-1]

        def _refresh_map() -> None:
            if not m.is_initialized:
                return  # init replays constructor-registered layers; resume next tick
            if track_seen[0] != LIVE.track_version:
                track_seen[0] = LIVE.track_version
                latlngs = [[lat, lon] for lat, lon in LIVE.track]
                # Keep to_dict() truthful: on client reconnect _handle_init replays
                # add_layer from args — without this the drawn track would reset.
                track_layer.args[0] = latlngs
                m.run_layer_method(track_layer.id, "setLatLngs", latlngs)
            pos = _position()
            if pos is None:
                return
            lat, lon = pos
            if marker_holder[0] is None:
                with m:
                    marker_holder[0] = m.marker(latlng=(lat, lon))
                m.set_center((lat, lon))  # one-shot jump to the first fix
                m.set_zoom(17)
            else:
                marker_holder[0].move(lat, lon)
                if follow.value:
                    m.set_center((lat, lon))

        def _refresh() -> None:
            guard_label.set_text(_guard_text())
            _refresh_badge()
            _refresh_health()
            _refresh_map()

        ui.timer(TOOL_TICK_S, _refresh)
        _refresh()
