#ifndef POINT_CLOUD_TYPES_H
#define POINT_CLOUD_TYPES_H

#include "lidar_protocol.h"

#include <cstdint>
#include <vector>


struct PointXYZI {
    float x;
    float y;
    float z;
    float intensity;
};

static_assert(sizeof(PointXYZI) == 16, "PointXYZI must be 16 bytes for binary format compatibility");


struct PointCloudFrame {
    uint64_t timestamp_ns;
    std::vector<PointXYZI> points;
};


// Backward-compatible alias: existing code uses FileFormat::kMagic etc.
namespace FileFormat = LidarProtocol;


struct WriterStats {
    uint64_t frames_written = 0;
    uint64_t bytes_written = 0;
    uint64_t dropped_frames = 0;
    uint64_t flush_count = 0;
};


#endif // POINT_CLOUD_TYPES_H