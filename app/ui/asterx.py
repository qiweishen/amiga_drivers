"""AsteRx live telemetry page: map track + IMU strip charts + health panels.

Read-only view over asterx_live.LIVE — unlike /gox there is no hardware
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

# The IMU chart re-sends all 6 series (300 points each) on every update, so it
# keeps a ~1 s pace of its own while the panels and map follow the page tick.
_IMU_CHART_MIN_PERIOD_S = 1.0

_FIX_TEXT = {0: "No PVT", 1: "Stand-Alone", 2: "DGNSS", 3: "Fixed pos",
             4: "RTK Fixed", 5: "RTK Float", 6: "SBAS", 7: "MB RTK Fixed",
             8: "MB RTK Float", 10: "PPP"}
_FIX_COLOR = {4: "green", 7: "green", 5: "orange", 8: "orange"}

_IMU_SERIES = [("acc_x", "m/s²"), ("acc_y", "m/s²"), ("acc_z", "m/s²"),
               ("gyro_x", "°/s"), ("gyro_y", "°/s"), ("gyro_z", "°/s")]

_DEG_TO_M = 111_320.0  # meters per degree of latitude (INS-PVT divergence row)


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


def _imu_options() -> dict:
    grids, xaxes, yaxes, series = [], [], [], []
    for i, (name, unit) in enumerate(_IMU_SERIES):
        col, row = divmod(i, 3)  # left column acc, right column gyro
        grids.append({"left": f"{7 + col * 50}%", "top": f"{6 + row * 32}%",
                      "width": "38%", "height": "24%", "containLabel": False})
        xaxes.append({"type": "value", "gridIndex": i, "min": "dataMin",
                      "max": "dataMax", "axisLabel": {"show": row == 2}})
        yaxes.append({"type": "value", "gridIndex": i, "scale": True,
                      "name": f"{name} ({unit})", "nameTextStyle": {"fontSize": 10}})
        series.append({"type": "line", "xAxisIndex": i, "yAxisIndex": i,
                       "showSymbol": False, "animation": False,
                       "sampling": "lttb", "lineStyle": {"width": 1}, "data": []})
    return {"animation": False, "grid": grids, "xAxis": xaxes, "yAxis": yaxes,
            "series": series, "axisPointer": {"link": [{"xAxisIndex": "all"}]},
            "tooltip": {"trigger": "axis"}}


def _finite(*vals: float) -> bool:
    return all(math.isfinite(v) for v in vals)


def _fmt(v: float | None, fmt: str = "{:.2f}", suffix: str = "") -> str:
    if v is None or not math.isfinite(v):
        return "—"
    return fmt.format(v) + suffix


def _hpr(h: float, p: float, r: float) -> str:
    return f"{_fmt(h, '{:.1f}')} / {_fmt(p, '{:.1f}')} / {_fmt(r, '{:.1f}')} °"


def _uptime(s: int | None) -> str:
    if s is None:
        return "—"
    d, rem = divmod(s, 86400)
    h, m = divmod(rem, 3600)
    return f"{d}d {h:02d}:{m // 60:02d}"


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

        # ------------------------------------------------------------- IMU
        ui.separator()
        ui.label("IMU (ExtSensorMeas, decimated to 10 Hz, 30 s window)").classes("text-lg font-bold")
        imu_chart = ui.echart(_imu_options()).classes("w-full h-[28rem]")

        # ---------------------------------------------------------- health
        ui.separator()
        ui.label("Receiver health").classes("text-lg font-bold")
        with ui.row().classes("w-full gap-4 flex-wrap items-start"):
            pvt_card, pvt_title, pvt_f = _kv_card("PVT (4007)", [
                "Fix", "Error", "Lat / Lon", "Height", "Satellites",
                "H / V acc", "Speed / COG"])
            ins_card, ins_title, ins_f = _kv_card("INS (4226)", [
                "Fix (GNSS)", "Error", "GNSS age", "Lat / Lon", "Height",
                "Accuracy", "Heading / Pitch / Roll", "Δpos vs PVT"])
            rx_card, rx_title, rx_f = _kv_card("Receiver (4014)", [
                "CPU load", "Temperature", "Uptime", "RX error", "Status"])
            att_card, att_title, att_f = _kv_card("AttEuler (5938)", [
                "Heading / Pitch / Roll", "Satellites", "Error"])
            imu_card, imu_title, imu_f = _kv_card("IMU (4050)", ["Temperature"])

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
            mode = None
            if LIVE.pvt is not None:
                mode = LIVE.pvt.mode & 0x0F
            elif LIVE.ins is not None:
                mode = LIVE.ins.gnss_mode & 0x0F
            if not mode:  # no data yet, or mode 0 = No PVT
                fix_badge.set_text("No fix")
                fix_badge.props('color="grey"')
                return
            fix_badge.set_text(_FIX_TEXT.get(mode, f"mode {mode}"))
            fix_badge.props(f'color="{_FIX_COLOR.get(mode, "blue-grey")}"')

        def _refresh_health() -> None:
            pvt = LIVE.pvt
            _stale_title(pvt_card, pvt_title, "PVT (4007)", pvt.mono if pvt else None)
            if pvt is not None:
                pvt_f["Fix"].set_text(_FIX_TEXT.get(pvt.mode & 0x0F, f"mode {pvt.mode & 0x0F}"))
                _alert(pvt_f["Error"], pvt.error != 0)
                pvt_f["Error"].set_text("OK" if pvt.error == 0 else f"PVT error {pvt.error}")
                pvt_f["Lat / Lon"].set_text(
                    f"{pvt.lat_deg:.7f}, {pvt.lon_deg:.7f}"
                    if _finite(pvt.lat_deg, pvt.lon_deg) else "—")
                pvt_f["Height"].set_text(_fmt(pvt.height_m, "{:.2f}", " m"))
                pvt_f["Satellites"].set_text("—" if pvt.nr_sv is None else str(pvt.nr_sv))
                pvt_f["H / V acc"].set_text(
                    f"{_fmt(pvt.h_acc_m)} / {_fmt(pvt.v_acc_m)} m")
                pvt_f["Speed / COG"].set_text(
                    f"{_fmt(math.hypot(pvt.vn_mps, pvt.ve_mps), '{:.2f}', ' m/s')}"
                    f" @ {_fmt(pvt.cog_deg, '{:.1f}', '°')}")

            ins = LIVE.ins
            _stale_title(ins_card, ins_title, "INS (4226)", ins.mono if ins else None)
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
                ins_f["Heading / Pitch / Roll"].set_text(
                    _hpr(ins.heading_deg, ins.pitch_deg, ins.roll_deg))
                # Metric INS-PVT position divergence: a one-glance fusion sanity check.
                if pvt is not None and _finite(pvt.lat_deg, pvt.lon_deg,
                                               ins.lat_deg, ins.lon_deg):
                    dn = _DEG_TO_M * (ins.lat_deg - pvt.lat_deg)
                    de = _DEG_TO_M * math.cos(math.radians(ins.lat_deg)) * (ins.lon_deg - pvt.lon_deg)
                    ins_f["Δpos vs PVT"].set_text(f"{math.hypot(dn, de):.2f} m")
                else:
                    ins_f["Δpos vs PVT"].set_text("—")

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
                warn = bool(rx.rx_status & 0x100)  # bit 8 = WARN
                _alert(rx_f["Status"], warn, "text-amber-700")
                rx_f["Status"].set_text("warning" if warn else "OK")

            att = LIVE.att
            _stale_title(att_card, att_title, "AttEuler (5938)", att.mono if att else None)
            if att is not None:
                att_f["Heading / Pitch / Roll"].set_text(
                    _hpr(att.heading_deg, att.pitch_deg, att.roll_deg))
                att_f["Satellites"].set_text("—" if att.nr_sv is None else str(att.nr_sv))
                _alert(att_f["Error"], att.error != 0)
                att_f["Error"].set_text("OK" if att.error == 0 else f"error {att.error}")

            _stale_title(imu_card, imu_title, "IMU (4050)", LIVE.imu_mono)
            imu_f["Temperature"].set_text(_fmt(LIVE.imu_temp_c, "{:.1f}", " °C"))

        imu_seen = [-1]

        imu_sent_at = [0.0]

        def _refresh_imu() -> None:
            if imu_seen[0] == LIVE.imu_version:
                return
            now = time.monotonic()
            if now - imu_sent_at[0] < _IMU_CHART_MIN_PERIOD_S:
                return  # new data, but the payload is too big to send every tick
            imu_sent_at[0] = now
            imu_seen[0] = LIVE.imu_version
            rows = list(LIVE.imu)  # snapshot: the worker thread appends concurrently
            for i in range(6):
                imu_chart.options["series"][i]["data"] = [[r[0], r[1 + i]] for r in rows]
            imu_chart.update()

        def _position() -> tuple[float, float] | None:
            ins = LIVE.ins  # fresh INS preferred, PVT fallback
            if ins is not None and time.monotonic() - ins.mono < STALE_S \
                    and _finite(ins.lat_deg, ins.lon_deg):
                return ins.lat_deg, ins.lon_deg
            pvt = LIVE.pvt
            if pvt is not None and _finite(pvt.lat_deg, pvt.lon_deg):
                return pvt.lat_deg, pvt.lon_deg
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
            _refresh_imu()
            _refresh_map()

        # Panels/badge/map follow the state cadence; the IMU chart re-sends its
        # full 6x300-point series, so it keeps its own ~1 s pace below.
        ui.timer(TOOL_TICK_S, _refresh)
        _refresh()
