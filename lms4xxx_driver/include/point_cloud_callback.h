#ifndef POINT_CLOUD_CALLBACK_H
#define POINT_CLOUD_CALLBACK_H

#include "point_cloud_types.h"
#include "ring_buffer.h"

#include <atomic>

// Forward declarations to avoid pulling sick_scan_api.h into this header.
struct SickScanPointCloudMsgType;
typedef struct SickScanPointCloudMsgType SickScanPointCloudMsg;
typedef void *SickScanApiHandle;


// Shared context between the callback and the driver application.
// The SICK API callback is a C function pointer (no user_data parameter),
// so we store context in a namespace-scoped global and access it from the
// free function.
struct CallbackContext {
    Common::RingBuffer<PointCloudFrame> *ring = nullptr;
    // Statistics (atomic for thread safety — incremented on SICK's callback thread)
    std::atomic<uint64_t> frames_received{0};
    std::atomic<uint64_t> dropped_frames{0};
};


// The callback function registered with SickScanApiRegisterCartesianPointCloudMsg.
void pointCloudCallback(SickScanApiHandle apiHandle, const SickScanPointCloudMsg *msg);

// Set/clear the global callback context. Must be called before/after
// registering/deregistering the callback.
void setCallbackContext(CallbackContext *ctx);

void clearCallbackContext();


#endif // POINT_CLOUD_CALLBACK_H