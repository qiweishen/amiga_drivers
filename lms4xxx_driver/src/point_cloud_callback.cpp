#include "point_cloud_callback.h"
#include "utility.h"
#include <spdlog/spdlog.h>

#include <atomic>
#include <cmath>
#include <cstring>

#include "sick_scan_xd_api/sick_scan_api.h"


// Namespace-scoped global context pointer.
// Lifetime managed by DriverApp: set before registration, cleared after deregistration.
static CallbackContext *g_callback_ctx = nullptr;


void setCallbackContext(CallbackContext *ctx) {
    g_callback_ctx = ctx;
}


void clearCallbackContext() {
    g_callback_ctx = nullptr;
}


void pointCloudCallback(SickScanApiHandle apiHandle, const SickScanPointCloudMsg *msg) {
    (void) apiHandle;

    CallbackContext *ctx = g_callback_ctx;
    if (!ctx || !ctx->ring) {
        return;
    }

    // Parse field offsets from the message descriptor.
    int offset_x = -1, offset_y = -1, offset_z = -1, offset_i = -1;
    for (uint64_t n = 0; n < msg->fields.size; ++n) {
        const auto &field = msg->fields.buffer[n];
        if (field.datatype != SICK_SCAN_POINTFIELD_DATATYPE_FLOAT32) {
            continue;
        }
        if (std::strcmp(field.name, "x") == 0) {
            offset_x = static_cast<int>(field.offset);
        } else if (std::strcmp(field.name, "y") == 0) {
            offset_y = static_cast<int>(field.offset);
        } else if (std::strcmp(field.name, "z") == 0) {
            offset_z = static_cast<int>(field.offset);
        } else if (std::strcmp(field.name, "intensity") == 0 || std::strcmp(field.name, "i") == 0) {
            offset_i = static_cast<int>(field.offset);
        }
    }

    if (offset_x < 0 || offset_y < 0 || offset_z < 0) {
        static std::atomic<bool> warned{false};
        if (!warned.exchange(true)) {
            Common::Log::log_message(spdlog::level::warn, "Callback", "Missing required x/y/z fields, dropping frame");
        }
        return;
    }

    // Build the frame.
    // Uses std::memcpy instead of reinterpret_cast for alignment safety --
    // the point data buffer may not be float-aligned, and strict aliasing rules
    // make reinterpret_cast of arbitrary byte pointers to float pointers UB.
    // std::memcpy is the correct, portable approach (optimized to a single load
    // instruction by the compiler on x86).
    PointCloudFrame frame;
    frame.timestamp_ns = static_cast<uint64_t>(msg->header.timestamp_sec) * 1'000'000'000ULL
                         + msg->header.timestamp_nsec;
    frame.points.reserve(msg->width * msg->height);

    for (uint32_t row = 0; row < msg->height; ++row) {
        const uint8_t *row_ptr = msg->data.buffer + row * msg->row_step;
        for (uint32_t col = 0; col < msg->width; ++col) {
            const uint8_t *ptr = row_ptr + col * msg->point_step;
            PointXYZI pt;
            std::memcpy(&pt.x, ptr + offset_x, sizeof(float));
            std::memcpy(&pt.y, ptr + offset_y, sizeof(float));
            std::memcpy(&pt.z, ptr + offset_z, sizeof(float));
            if (offset_i >= 0) {
                std::memcpy(&pt.intensity, ptr + offset_i, sizeof(float));
            } else {
                pt.intensity = 0.0f;
            }

            if (!std::isfinite(pt.x) || !std::isfinite(pt.y) || !std::isfinite(pt.z)) {
                continue;
            }
            frame.points.push_back(pt);
        }
    }

    // Count received frame before enqueue attempt.
    ctx->frames_received.fetch_add(1, std::memory_order_relaxed);

    // Enqueue. If the ring buffer is full, count the drop.
    if (!ctx->ring->try_push(std::move(frame))) {
        ctx->dropped_frames.fetch_add(1, std::memory_order_relaxed);
    }
}
