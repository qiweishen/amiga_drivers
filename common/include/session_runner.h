#pragma once
#include <atomic>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include "driver_app.h"

namespace common {
    struct Config;
    class DriversJson;
    struct DriverSlot {
        std::unique_ptr<IDriverApp> app;
        std::string_view name; // human-readable diagnostic prefix ("AsteRx"...)
        std::string_view init_failed; // common::Markers::k*InitFailed
        std::string_view run_exception; // common::Markers::k*RunException
        std::thread thread;
        std::string key{}; // stable GUI identity, including the configured LiDAR id
    };

    // Sole ownership: sequential Init, concurrent Run, join all workers, reverse
    // Shutdown (including partial/failed Init), then read final counters.
    int RunSession(std::vector<DriverSlot> drivers, const Config &config,
                   DriversJson &manifest, std::atomic<bool> &terminate,
                   std::atomic<int> &signal_received);
}
