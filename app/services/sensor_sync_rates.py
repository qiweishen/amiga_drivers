"""SensorSync trigger pair-rate rule used when the GUI writes a pulse rate.

Mirrors common/include/sensor_sync_rates.h, the host copy of the firmware's
triggers.cpp calculate() (3rd_party/External/sensor_trigger, log format 3). The
board stays the authority: it answers a rate its PWM cannot produce with
#ERR,bad_freq at START, and the C++ loader is the final validator.
"""

from __future__ import annotations

import math

FX = "FX"  # channels 0/1, FlexPWM2 SM0
JAI = "JAI"  # channels 2/3, SM3 (clocked from SM0)

BUS_HZ = 150e6  # F_BUS_ACTUAL at 600 MHz (platformio.ini board_build.f_cpu)
PULSE_US = 50.0  # config.h PULSE_US
PWM_BASE_HZ = BUS_HZ / 128.0
MAX_JAI_PRESCALE = 7
MAX_PERIOD_COUNTS = 65535.0


def group_of(channel: int) -> str:
    return FX if channel < 2 else JAI


def rate_accepted(group: str, hz: float) -> bool:
    """True when the board accepts this pair rate in START; 0 turns the pair off."""
    if not math.isfinite(hz) or not hz >= 0.0:
        return False
    rate = hz if hz > 0.0 else (50.0 if group == FX else 1.0)  # a stopped SM0 still clocks SM3
    clock_hz = PWM_BASE_HZ
    prescale = 0
    while group == JAI and clock_hz / rate > MAX_PERIOD_COUNTS and prescale < MAX_JAI_PRESCALE:
        prescale += 1
        clock_hz /= 2
    counts = clock_hz / rate
    if counts > MAX_PERIOD_COUNTS:  # ceil(counts) > 65535; also keeps math.ceil away from inf
        return False
    width = math.ceil(clock_hz * PULSE_US / 1e6)
    return math.ceil(counts) > 2 * width + 1  # rise at W, fall at 2W


def min_hz(group: str) -> float:
    slowest_hz = PWM_BASE_HZ if group == FX else PWM_BASE_HZ / 2 ** MAX_JAI_PRESCALE
    return slowest_hz / MAX_PERIOD_COUNTS


def max_hz_exclusive(_group: str) -> float:
    return PWM_BASE_HZ / (2 * math.ceil(PWM_BASE_HZ * PULSE_US / 1e6) + 1)


def range_text(group: str) -> str:
    """'[17.882, 9847.689] Hz' (FX) / '[0.140, 9847.689] Hz' (JAI), rounded inwards."""
    low = math.ceil(min_hz(group) * 1000.0) / 1000.0
    high = math.floor(max_hz_exclusive(group) * 1000.0) / 1000.0
    return f"[{low:.3f}, {high:.3f}] Hz"
