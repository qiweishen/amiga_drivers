#pragma once

#include <cmath>
#include <cstdio>
#include <string>


// SensorSync-Logger (log format 3) trigger pair-rate rule, host side.
//
// Upstream (3rd_party/External/sensor_trigger) keeps this rule in the firmware only:
// triggers.cpp calculate() accepts a pair rate that FlexPWM2 can produce and the board
// answers any other START with "#ERR,bad_freq". Its headers pull in <Arduino.h>, so the
// host cannot include them; RateAccepted() mirrors calculate() so that a config is
// refused at load time / GUI Apply instead of at the camera's Arm. The board stays the
// authority: upstream follows main without a pin, and drift between this copy and the
// firmware still ends in #ERR,bad_freq, which fails SensorSyncLog::Start().
// app/services/sensor_sync_rates.py is the GUI's copy of this file.
namespace common::sensor_sync {
    // Group 0 = FX pair (channels 0/1, FlexPWM2 SM0), group 1 = JAI pair (channels 2/3, SM3);
    // both channels of a pair share one PWM rate
    enum class Group { kFx = 0, kJai = 1 };

    inline constexpr unsigned kGroupCount = 2;

    // F_BUS_ACTUAL at platformio.ini board_build.f_cpu = 600 MHz; the board reports the
    // derived GPT1 rate (bus / TB_PERCLK_DIV 2) as "#LOG,...,tick_hz=75000000.000"
    inline constexpr double kBusHz = 150e6;
    inline constexpr double kPulseUs = 50.0; // config.h PULSE_US
    inline constexpr double kPwmBaseHz = kBusHz / 128.0; // SM0 at prescale 2^7; SM3 clocks from SM0 AUX
    inline constexpr unsigned kMaxJaiPrescale = 7; // SM3 divides its clock by up to 2^7 more
    inline constexpr double kMaxPeriodCounts = 65535.0; // 16-bit period register

    constexpr Group GroupOf(int channel) { return channel < 2 ? Group::kFx : Group::kJai; }

    constexpr const char *Name(Group group) { return group == Group::kFx ? "FX" : "JAI"; }

    constexpr const char *Channels(Group group) { return group == Group::kFx ? "0/1" : "2/3"; }

    // True when the board accepts this pair rate in START; 0 turns the pair off and is
    // always accepted. Same arithmetic as triggers.cpp calculate()
    inline bool RateAccepted(Group group, double hz) {
        if (!(hz >= 0.0) || !std::isfinite(hz)) {
            return false;
        }
        const double rate = hz > 0.0 ? hz : (group == Group::kFx ? 50.0 : 1.0); // a stopped SM0 still clocks SM3
        double clock_hz = kPwmBaseHz;
        for (unsigned prescale = 0;
             group == Group::kJai && clock_hz / rate > kMaxPeriodCounts && prescale < kMaxJaiPrescale; ++prescale) {
            clock_hz /= 2;
        }
        const double period = std::ceil(clock_hz / rate);
        const double width = std::ceil(clock_hz * kPulseUs / 1e6);
        return period <= kMaxPeriodCounts && period > 2 * width + 1; // rise at W, fall at 2W
    }

    // Slowest accepted pulse rate (> 0): the 16-bit period at the pair's slowest PWM clock
    inline double MinHz(Group group) {
        const double slowest_hz = group == Group::kFx ? kPwmBaseHz : kPwmBaseHz / double(1u << kMaxJaiPrescale);
        return slowest_hz / kMaxPeriodCounts;
    }

    // Exclusive upper limit: the period must exceed 2 pulse widths + 1 count at the fastest clock
    inline double MaxHzExclusive(Group) {
        return kPwmBaseHz / (2 * std::ceil(kPwmBaseHz * kPulseUs / 1e6) + 1);
    }

    // "[17.882, 9847.689] Hz" (FX) / "[0.140, 9847.689] Hz" (JAI): both ends rounded
    // inwards to 0.001 Hz, so each printed bound is itself an accepted rate
    inline std::string RangeText(Group group) {
        char text[64];
        std::snprintf(text, sizeof(text), "[%.3f, %.3f] Hz", std::ceil(MinHz(group) * 1000.0) / 1000.0,
                      std::floor(MaxHzExclusive(group) * 1000.0) / 1000.0);
        return text;
    }
} // namespace common::sensor_sync
