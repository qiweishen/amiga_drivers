#pragma once

// The SensorSync-Logger session wrapper lives in common/ now (one board is
// shared by every camera it triggers; see common/include/sensor_sync_hub.h).
// The fx10 names stay for the standalone tools (fx10_reference) that own the
// board alone.
#include "sensor_sync_log.h"

namespace fx10 {
    using TriggerLogError = common::SensorSyncError;
    using SensorTriggerLog = common::SensorSyncLog;
} // namespace fx10
