#pragma once
#include <atomic>
#include <filesystem>
#include "data_type.h"

// Composition root: snapshot configurations and construct the driver set.
int RunAcquisition(common::Config config, const std::filesystem::path &main_config_path,
                   std::atomic<bool> &terminate, std::atomic<int> &signal_received);
