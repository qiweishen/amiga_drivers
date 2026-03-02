# Amiga Drivers

Unified data acquisition system for concurrent INS/GNSS and LiDAR sensor operation.

## Architecture

```
                          +----------------------+
                          |    AmigaDrivers      |
                          |   (unified main)     |
                          |                      |
                          |  Signal Handler      |
                          |  g_terminate flag    |
                          +---+-------------+----+
                              |             |
                 propagate    |             |   propagate
                 terminate    |             |   terminate
                              v             v
                 +------------+--+   +------+-----------+
                 |  InsDriverApp |   |    DriverApp      |
                 |   (INS401)    |   |   (LMS4xxx)      |
                 +---+--------+-+   +---+--------+------+
                     |        |         |        |
            +--------+--+ +---+----+ +--+-----+ +--------+
            | Receiver  | | NTRIP  | | Writer | | SICK   |
            | Thread    | | Thread | | Thread | | Callback|
            +-----+-----+ +---+---+ +---+----+ +----+---+
                  |            |         |           |
                  v            v         v           v
             INS401 Device   NTRIP   Binary File   SICK LMS4xxx
             (Ethernet)      Caster  (disk)        (TCP/Ethernet)
```

### Build Targets

| Target | Description |
|--------|-------------|
| `AmigaDrivers` | Unified executable running both drivers concurrently |
| `INS401_Driver` | Standalone INS401 driver |
| `LMS4xxx_Driver` | Standalone LMS4xxx LiDAR driver |

### Libraries

| Library | Description |
|---------|-------------|
| `ins401_lib` | Static library: INS401 receiver, NTRIP, discovery, initialization |
| `lms4xxx_lib` | Static library: SICK scanner interface, point cloud writer |
| `amiga_common` | Static library: logging (spdlog), config (yaml-cpp), signal handling, binary I/O |

## Dependencies

| Dependency | Version | Purpose |
|-----------|---------|---------|
| CMake | >= 3.14 | Build system |
| C++ compiler | C++17 | Language standard |
| Eigen3 | vendored | Linear algebra (INS401 orientation) |
| spdlog | v1.16.0 (FetchContent) | Logging |
| yaml-cpp | 0.9.0 (FetchContent) | Configuration parsing |
| sick_scan_xd | 3.9.0 (FetchContent + ExternalProject) | SICK LiDAR API |
| OpenSSL | system | NTRIP client SSL/crypto |
| Boost | system | CRC, networking utilities |

## Building

```bash
# Configure (Release build)
cmake -B build -DCMAKE_BUILD_TYPE=Release

# Build all targets
cmake --build build --parallel

# Or build specific targets
cmake --build build --target AmigaDrivers    # Unified
cmake --build build --target INS401_Driver   # INS only
cmake --build build --target LMS4xxx_Driver # LiDAR only
```

Output binaries are placed in `build/bin/`.

### Docker

The project builds in Docker/devcontainer environments. Use your IDE's remote development
or run the CMake commands above inside the container.

## Usage

### Unified Mode (Both Drivers)

```bash
# Run both drivers with default config paths
./build/bin/AmigaDrivers

# Run both with explicit configs
./build/bin/AmigaDrivers \
    --ins-config ins401_driver/config/config-ins401.yaml \
    --lidar-config lms4xxx_driver/config/config-lms4xxx.yaml

# Run with LiDAR options
./build/bin/AmigaDrivers \
    --ins-config ins401_driver/config/config-ins401.yaml \
    --lidar-config lms4xxx_driver/config/config-lms4xxx.yaml \
    --ntp-server 10.75.96.100 \
    --csv --quiet
```

### Single Driver Mode

Provide only one driver's config to run it alone:

```bash
# INS401 only
./build/bin/AmigaDrivers --ins-config ins401_driver/config/config-ins401.yaml

# LiDAR only
./build/bin/AmigaDrivers --lidar-config lms4xxx_driver/config/config-lms4xxx.yaml
```

Or use the standalone executables:

```bash
# INS401 standalone
./build/bin/INS401_Driver [config-path]

# LMS4xxx standalone
./build/bin/LMS4xxx_Driver --config lms4xxx_driver/config/config-lms4xxx.yaml
```

### CLI Options (Unified)

| Option | Description |
|--------|-------------|
| `--ins-config <path>` | INS401 YAML configuration file |
| `--lidar-config <path>` | LMS4xxx YAML configuration file |
| `--lidar-launch <path>` | SICK launch file (overrides config) |
| `--lidar-output <path>` | LiDAR output binary file path |
| `--ntp-server <ip>` | NTP server IP for LiDAR time sync |
| `--csv` | Convert LiDAR output to CSV after recording |
| `--quiet`, `-q` | Suppress SICK library console output |
| `--log <path>` | Log file path |
| `--help`, `-h` | Show help |

### Shutdown

Press `Ctrl+C` (SIGINT) or send SIGTERM. Both drivers shut down gracefully:
1. Signal handler sets shared terminate flag
2. Both driver run-loops detect the flag and exit
3. Each driver stops its internal threads, flushes data, closes files
4. INS401 post-processes binary files to CSV automatically
5. LiDAR CSV conversion runs if `--csv` was specified

## Configuration

### INS401 (`config-ins401.yaml`)

```yaml
General:
  Output Directory: "./data"

Logging System:
  Enable Logging: true

NTRIP Client:
  Enable RTK: false
  Host: "localhost"
  Port: 2101
  Mount Point: "MOUNT"
  Use VRS: false

Static Initialization:
  Enable GNSS Checking: false
  Minimal Stationary Duration: 10.0   # seconds
  Recompute Interval: 5.0             # seconds
  Required Stable Count: 5
```

### LMS4xxx (`config-lms4xxx.yaml`)

```yaml
General:
  Output Directory: ./data

Logging System:
  Enable Logging: true

Sick Launch:
  SICK Launch file: ../../lms4xxx_driver/config/sick_lms_4xxx.launch

Time Synchronization:
  NTP Server: 10.75.96.100
```

The SICK launch file (`sick_lms_4xxx.launch`) configures scanner-specific parameters:
hostname, port, angle range, binary protocol, intensity, and range limits.

## Driver-Specific Notes

### INS401 Driver

- Communicates via **raw Ethernet sockets** (AF_PACKET) — requires root or `CAP_NET_RAW`
- **Auto-discovers** INS401 devices on the network at startup
- Performs **static initialization**: detects stationary period via IMU, computes roll/pitch alignment
- Optional **NTRIP RTK** corrections: connects to caster, forwards RTCM3 to device
- **VRS support**: sends GGA position to NTRIP caster for virtual reference station
- Output: binary files (IMU, GNSS, INS, diagnostic, RTCM, NMEA) auto-converted to CSV

### LMS4xxx Driver

- Uses **SICK scan_xd** library (dynamically loaded at runtime via dlopen)
- Communicates via **TCP** binary protocol to SICK LMS4xxx scanners
- Supports **NTP time synchronization** via SOPAS commands to the scanner
- Output: binary LIDARPCD format, optionally converted to CSV
- Point cloud data: x, y, z, intensity per point, organized in timestamped frames
- Invalid points (NaN/Inf) are filtered; zero-filled echoes are preserved as null placeholders

## Project Structure

```
amiga_drivers/
├── CMakeLists.txt                  # Top-level build (all targets)
├── cmake/FetchContent.cmake        # spdlog, yaml-cpp, sick_scan_xd
├── src/
│   └── main.cpp                    # Unified entry point
├── 3rd_party/
│   └── eigen/                      # Vendored Eigen3
├── common/                         # Shared library
│   ├── include/common/
│   │   ├── logger.h                # spdlog wrapper
│   │   ├── config_loader.h         # yaml-cpp wrapper
│   │   ├── binary_writer.h         # Buffered POSIX I/O
│   │   ├── thread_safe_queue.h     # Bounded producer-consumer queue
│   │   └── signal_handler.h        # POSIX signal handling
│   └── src/
├── ins401_driver/
│   ├── include/
│   │   ├── ins_driver_app.h        # App wrapper (init/run/shutdown)
│   │   ├── ins401_receiver.h       # Packet receiver and parser
│   │   ├── ntrip_client.h          # NTRIP v2.0 client
│   │   ├── ethernet_socket.h       # Raw socket I/O
│   │   └── ...
│   ├── src/
│   │   ├── main.cpp                # Standalone entry point
│   │   ├── ins_driver_app.cpp      # App wrapper implementation
│   │   └── ...
│   └── config/
│       └── config-ins401.yaml
└── lms4xxx_driver/
    ├── include/
    │   ├── driver_app.h            # App wrapper (init/run/shutdown)
    │   ├── point_cloud_types.h     # PointXYZI, PointCloudFrame
    │   └── ...
    ├── src/
    │   ├── main.cpp                # Standalone entry point
    │   └── ...
    └── config/
        ├── config-lms4xxx.yaml
        └── sick_lms_4xxx.launch
```

## Threading Model

The unified executable manages 5+ threads:

| Thread | Owner | Purpose |
|--------|-------|---------|
| Main | Unified main | Signal propagation, monitor both drivers |
| INS run | InsDriverApp | Polls initialization, shows spinner |
| INS receiver | INSDeviceReceiver | Raw Ethernet packet receive loop |
| INS NTRIP | NTRIPClient | RTCM correction data receive + forward |
| LiDAR run | DriverApp | Polls terminate flag |
| LiDAR writer | DriverApp | Pops frames from queue, writes to disk |
| SICK callback | SICK library | Delivers point cloud frames (internal) |

Thread safety is ensured via `std::atomic<bool>` terminate flags, `std::mutex` for shared
state, and `std::condition_variable` for producer-consumer synchronization.

## Known Limitations and TODOs

- **NTRIP SSL/TLS**: Stubbed but not yet implemented in the NTRIP client
- **Single INS401 device**: Discovery returns the first device found; multiple INS401 devices are not supported
- **Raw sockets require privileges**: The INS401 driver needs root or `CAP_NET_RAW` capability
- **SICK library console output**: Some raw `printf()` calls in the SICK library cannot be fully suppressed via the API; the `--quiet` flag redirects stdout to `/dev/null` during init as a workaround
- **Spinner config**: The INS401 terminal spinner looks for `./spinner_frames.conf` relative to the working directory
- **No cross-driver data fusion**: The unified mode runs both drivers concurrently but does not correlate INS and LiDAR data streams
