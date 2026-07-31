#!/usr/bin/env python3
"""Feed fake live_*.csv rows to exercise the /asterx page without hardware.

Usage:
    mkdir -p /tmp/asterx_sim/bin/asterx
    AMIGA_ASTERX_SIM=/tmp/asterx_sim uv run amiga-gui &
    uv run python tools/asterx_sim_feed.py /tmp/asterx_sim/bin/asterx

Rows follow the final CSV contract: physical units (deg, m, m/s, deg/s, degC),
float DNU as the 'nan' literal, integer sentinels kept. Circular ~50 m track +
sine IMU + 1 Hz PVT/RxStatus + 10 Hz INS/AttEuler. Stdlib only.
"""

from __future__ import annotations

import math
import sys
import time
from pathlib import Path

HEADERS = {
    "live_pvtgeodetic.csv":
        "tow_ms,wnc,gps_unix_ns,host_unix_ns,mode,error,nr_sv,lat_deg,lon_deg,"
        "height_m,undulation_m,vn_mps,ve_mps,vu_mps,cog_deg,h_acc_m,v_acc_m,"
        "mean_corr_age_s,alert_flag",
    "live_extsensormeas.csv":
        "tow_ms,wnc,gps_unix_ns,host_unix_ns,acc_x_mps2,acc_y_mps2,acc_z_mps2,"
        "gyro_x_degps,gyro_y_degps,gyro_z_degps,temp_c,zero_vel_flag",
    "live_insnavgeod.csv":
        "tow_ms,wnc,gps_unix_ns,host_unix_ns,gnss_mode,error,info,gnss_age_s,"
        "lat_deg,lon_deg,height_m,undulation_m,accuracy_m,latency_s,sb_list,"
        "lat_std_m,lon_std_m,height_std_m,heading_deg,pitch_deg,roll_deg,"
        "heading_std_deg,pitch_std_deg,roll_std_deg,ve_mps,vn_mps,vu_mps,"
        "ve_std_mps,vn_std_mps,vu_std_mps",
    "live_receiverstatus.csv":
        "tow_ms,wnc,gps_unix_ns,host_unix_ns,cpu_load_pct,up_time_s,rx_status,"
        "rx_error,ext_error,temp_c,cmd_count,agc",
    "live_atteuler.csv":
        "tow_ms,wnc,gps_unix_ns,host_unix_ns,nr_sv,error,mode,heading_deg,"
        "pitch_deg,roll_deg,heading_dot_degps,pitch_dot_degps,roll_dot_degps",
}

LAT0, LON0 = 31.2304, 121.4737  # degrees — physical units per the contract
RADIUS_DEG = 4.5e-4  # ~50 m circle


def main() -> None:
    if len(sys.argv) != 2:
        sys.exit(f"usage: {sys.argv[0]} <session>/bin/asterx")
    d = Path(sys.argv[1])
    d.mkdir(parents=True, exist_ok=True)
    files = {}
    for name, hdr in HEADERS.items():
        fh = open(d / name, "a")
        if fh.tell() == 0:
            fh.write(hdr + "\n")
        files[name] = fh

    t0 = time.time()
    while True:  # 10 Hz outer loop
        el = time.time() - t0
        tow = int(el * 1000) + 100_000_000
        now_ns = time.time_ns()
        th = el * 0.05  # slow circle
        lat = LAT0 + RADIUS_DEG * math.sin(th)
        lon = LON0 + RADIUS_DEG * math.cos(th)
        heading = math.degrees(th) % 360.0

        for k in range(20):  # 200 Hz IMU, batched per tick
            tt = tow + 5 * k
            s = math.sin(tt / 300.0)
            files["live_extsensormeas.csv"].write(
                f"{tt},2366,{now_ns},{now_ns},{0.1 * s:.4f},"
                f"{0.1 * math.cos(tt / 300.0):.4f},{9.81 + 0.05 * s:.4f},"
                f"{2 * s:.4f},{2 * math.cos(tt / 400.0):.4f},{0.5 * s:.4f},31.2,0\n")

        if int(el * 10) % 10 == 0:  # 1 Hz PVT + RxStatus
            files["live_pvtgeodetic.csv"].write(
                f"{tow},2366,{now_ns},{now_ns},4,0,23,{lat:.9f},{lon:.9f},"
                f"15.20,9.00,0.50,0.10,0.00,{heading:.1f},0.12,0.18,2.0,0\n")
            files["live_receiverstatus.csv"].write(
                f"{tow},2366,{now_ns},{now_ns},35,{int(el)},0,0,0,42,5,"
                f"L1:42;L2:40\n")

        files["live_insnavgeod.csv"].write(  # 10 Hz INS
            f"{tow},2366,{now_ns},{now_ns},4,0,0,0.05,{lat:.9f},{lon:.9f},"
            f"15.10,9.00,0.08,0.003,0,0.01,0.01,0.02,{heading:.1f},1.2,-0.4,"
            f"0.2,0.1,0.1,0.10,0.50,0.00,0.02,0.02,0.03\n")
        files["live_atteuler.csv"].write(  # 10 Hz AttEuler
            f"{tow},2366,{now_ns},{now_ns},12,0,2,{heading:.1f},1.1,-0.5,"
            f"0.1,0.0,0.0\n")

        for fh in files.values():
            fh.flush()
        time.sleep(0.1)


if __name__ == "__main__":
    main()
