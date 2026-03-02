/// @file lidar_protocol.h
/// @brief LIDARPCD binary file format constants for the SICK LMS4XXX driver.
///
/// Analogous to ins401_protocol.h: centralizes all protocol-level constants
/// so they are defined once and shared between writer, converter, and receiver.

#ifndef LIDAR_PROTOCOL_H
#define LIDAR_PROTOCOL_H

#include <array>
#include <cstddef>
#include <cstdint>


namespace LidarProtocol {
    // LIDARPCD binary file format.
    // Header: 8-byte magic "LIDARPCD" + 4-byte uint32 version
    // Per frame: 8-byte uint64 timestamp_ns + 4-byte uint32 num_points + N*16 bytes (PointXYZI)
    static constexpr std::array<char, 8> kMagic = {'L', 'I', 'D', 'A', 'R', 'P', 'C', 'D'};
    static constexpr uint32_t kVersion = 1;
    static constexpr size_t kHeaderSize = sizeof(kMagic) + sizeof(kVersion); // 12 bytes
    static constexpr size_t kFrameHeaderSize = sizeof(uint64_t) + sizeof(uint32_t); // 12 bytes
    static constexpr size_t kPointSize = 16; // sizeof(PointXYZI): x,y,z,intensity as float32
} // namespace LidarProtocol


#endif // LIDAR_PROTOCOL_H