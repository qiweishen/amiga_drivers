# Architecture Analysis Report: INS401 vs SICK LMS4XXX

> **Date:** 2026-03-02
> **Author:** Architecture Analyst (Teammate 1)
> **Scope:** Full codebase read-through — INS401 driver, SICK LMS4XXX driver, common/ library, unified main

---

## 1. Project Overview

### 1.1 Repository Structure

```
amiga_drivers/
├── CMakeLists.txt              # Top-level build: unified executable + sub-drivers
├── main.cpp                    # Unified entry point (runs both drivers concurrently)
├── cmake/FetchContent.cmake    # Shared dependencies (spdlog, yaml-cpp, sick_scan_xd)
├── config/config-main.yaml     # Main config: run mode, paths, logging
├── 3rd_party/                  # Vendored Eigen, FetchContent cache
├── common/                     # Shared infrastructure library (amiga_common)
│   ├── include/
│   │   ├── binary_writer.h     # POSIX buffered binary writer
│   │   ├── data_type.h         # Common::Config struct
│   │   ├── signal_handler.h    # Shared POSIX signal handler
│   │   ├── thread_safe_queue.h # Bounded mutex-based queue
│   │   └── utility.h           # ConfigLoader, Logger, Log namespace
│   └── src/
│       ├── binary_writer.cpp
│       ├── signal_handler.cpp
│       └── utility.cpp
├── ins401_driver/              # INS401 GNSS/IMU driver (reference template)
│   ├── include/ (12 headers)
│   ├── src/ (11 source files)
│   ├── config/config-ins401.yaml
│   └── CMakeLists.txt
└── lms4xxx_driver/             # SICK LMS4XXX LiDAR driver (optimization target)
    ├── include/ (8 headers)
    ├── src/ (6 source files)
    ├── config/ (2 config files)
    └── CMakeLists.txt
```

### 1.2 Build Architecture

- **C++17**, CMake 3.14+, top-level `AmigaDrivers` project
- Three build targets: `INS401_Driver` (standalone), `LMS4xxx_Driver` (standalone), `AmigaDrivers` (unified)
- Static libraries: `ins401_lib`, `lms4xxx_lib`, `amiga_common`
- Dependencies: Eigen (vendored), spdlog v1.16.0 (FetchContent), yaml-cpp 0.9.0 (FetchContent), OpenSSL, Boost (INS401 only), sick_scan_xd 3.9.0 (ExternalProject)

### 1.3 Unified Entry Point (`main.cpp`)

The unified `main.cpp` (root) demonstrates the concurrent driver pattern:
- Loads `config-main.yaml` with `run_mode` (0=both, 1=INS only, 2=LiDAR only)
- Creates timestamped data folder
- Installs shared `Common::SignalHandler` with a single `g_terminate` atomic
- Creates `InsDriverApp` + `LidarDriverApp`, each with `init()/run()/shutdown()` lifecycle
- Runs each driver's `run()` in separate threads
- Monitor loop checks both `terminate_flag()`s and `g_terminate`
- On signal/self-termination: propagates terminate → joins threads → calls `shutdown()` sequentially

---

## 2. INS401 Core Design Patterns

### 2.1 Thread Model

| Thread | Responsibility | Created in |
|--------|---------------|------------|
| **Main thread** | `InsDriverApp::run()` — polls `init_monitor_->IsInitialized()`, runs `TerminalSpinner` | `main.cpp:154` or `ins401_driver_app.cpp:130` |
| **Receiver thread** | `INSDeviceReceiver::Run()` → `ReceiveLoop()` — raw Ethernet receive + packet dispatch | `ins401_driver_app.cpp:80` |
| **NTRIP thread** | `NTRIPClient::Connect()` + `StartReceiving()` (spawns 2 sub-threads inside) | `ins401_driver_app.cpp:107` |
| **NTRIP receive sub-thread** | `NTRIPClient::ReceiveThread()` — reads RTCM from NTRIP caster | `ntrip_client.cpp:168` |
| **NTRIP process sub-thread** | `NTRIPClient::ProcessThread()` — chunks RTCM and invokes data callback | `ntrip_client.cpp:169` |

**Key observation:** The receiver thread runs a tight `ReceiveLoop()` that calls `ReceiveBatch(64)` (non-blocking `MSG_DONTWAIT`) then falls back to `Receive(100)` (epoll with 100ms timeout). This is the "hot path".

### 2.2 Data Flow Design

```
Network (raw Ethernet)
    → EthernetSocket::ReceiveBatch/Receive (epoll + BPF filter)
    → INSDeviceReceiver::VerifyDataFrame (message type dispatch)
    → Handle{GNSS,INS,IMU,Diagnostic,RTCM,NMEA}Packet
        → CRC verify
        → Binary write (std::ofstream with 256KB pubsetbuf)
        → [Optional] Parse + callback (IMU→InitializationMonitor, GNSS→InitializationMonitor)

Signal (SIGINT/SIGTERM)
    → InsDriverApp::shutdown()
        → receiver_->Stop() (sets running_ = false)
        → ntrip_client_->Disconnect()
        → Join threads
        → receiver_->ProcessBinaryFiles() (binary→CSV post-processing)
        → receiver_->LogStatistics()
```

**Critical design choice:** During data collection, only raw binary payloads are written to disk. Parsing into structs happens only when needed for real-time callbacks (IMU for initialization, GNSS for monitoring). All post-processing (binary→CSV) happens after collection stops. This minimizes per-packet CPU work on the hot path.

### 2.3 Buffering Strategy

- **Receive buffer:** `buffer_size_ = 64 * 1024` (64KB) socket receive buffer via `SO_RCVBUF`
- **Write buffers:** `write_buffer_size_ = 256 * 1024` (256KB) per file via `std::ofstream::pubsetbuf`
  - 6 separate binary/text files with independent 256KB buffers
  - Files: `gnss.bin`, `ins.bin`, `imu.bin`, `diagnostic.bin`, `rtcm_rover.rtcm3`, `nmea.nmea`
- **Post-processing batch:** `kPostProcessBatchSize = 1024` records accumulated in `fmt::memory_buffer` before flushing to CSV

**INS401 approach:** Uses C++ `std::ofstream` with `pubsetbuf` for buffered I/O. This is simpler but less robust than POSIX `write()` for crash safety.

### 2.4 Concurrency Patterns

| Pattern | Where | Details |
|---------|-------|---------|
| **Atomic flags** | `running_`, `terminate_`, `first_gga_blh_ready_`, all statistics counters | Lock-free status and statistics via `std::atomic<size_t>` with `memory_order_relaxed` |
| **Atomic pointers** | `initialization_monitor_`, `ntrip_client_` | Late-binding dependency injection with `memory_order_release/acquire` |
| **Mutex-protected callbacks** | `callback_mutex_` in `INSDeviceReceiver` and `NTRIPClient` | `std::scoped_lock` for callback registration/invocation |
| **Producer-consumer queue** | `NTRIPClient::data_queue_` | `std::queue` + `std::mutex` + `std::condition_variable` (NOT lock-free) |
| **Condition variable** | `InitializationMonitor::gravity_cv_` | Wait-with-timeout for first GNSS gravity estimate |

**Weakness identified:** `INSDeviceReceiver` does NOT use a separate writer thread. It writes binary data directly in the receiver callback on the receive thread. For the INS401's data rates (GNSS ~1Hz, INS ~1Hz, IMU ~100Hz, Diagnostic ~1Hz), this is acceptable. But for higher frequency data, this could become a bottleneck.

### 2.5 Error Handling

- **Logging framework:** `Common::Log::log_message()` for all levels, `Common::Log::log_and_throw()` for fatal errors
- **No exception-based flow control** in hot paths: CRC failures log warnings and `return`, don't throw
- **Exception handling** only at thread boundaries: `try/catch` in thread lambdas, setting `terminate_` on fatal error
- **RAII patterns:**
  - `EthernetSocket`: socket_fd_ + epoll_fd_ cleanup in destructor
  - `Ethernet::FdGuard`: RAII wrapper for file descriptors
  - `NTRIPClient`: SSL context/connection with `std::unique_ptr` + custom deleters
  - `INSDeviceReceiver`: file handles closed in `CloseAllFiles()` (called from destructor)
  - `TerminalSpinner`: clears terminal line in destructor

### 2.6 Class Structure and Naming

- **Classes:** PascalCase — `InsDriverApp`, `INSDeviceReceiver`, `NTRIPClient`, `EthernetSocket`, `InitializationMonitor`, `StaticInitializer`, `StationaryDetector`, `TerminalSpinner`
- **Methods:** PascalCase — `Run()`, `Stop()`, `ReceiveLoop()`, `HandleGNSSSolutionPacket()`, `ParseGNSSSolutionData()`, `ProcessBinaryFiles()`
- **Member variables:** snake_case with trailing underscore — `running_`, `socket_ptr_`, `gnss_bin_file_`
- **Constants:** `k` prefix or ALL_CAPS — `kModule`, `kPostProcessBatchSize`, `COMMAND_START`, `ACEINNA_HEADER_LEN`
- **Namespaces:** `Ethernet::`, `Ethernet::CRC::`, `InsTool::Earth::`, `InsTool::Utility::`
- **File organization:** Each class has its own `.h`/`.cpp` pair. Protocol constants in separate `ins401_protocol.h`. Data types in `ins401_data_type.h`.

### 2.7 Statistics and Monitoring

- Per-packet-type atomic counters (packets received, CRC errors)
- `LogStatistics()` called at shutdown
- `MonitorGNSSStatus()`: hysteresis state machine for RTK_FIXED/STD tracking (requires `GnssTransitionConfirmFrames_ = 3` consecutive frames before accepting state change)

---

## 3. SICK LMS4XXX Current Implementation Analysis

### 3.1 Thread Model

| Thread | Responsibility | Created in |
|--------|---------------|------------|
| **Main thread** | `LidarDriverApp::run()` — sleep loop checking terminate flag | `lidar_driver_app.cpp:197` |
| **Writer thread** | `writer_thread_func()` — pops from queue, writes binary | `lidar_driver_app.cpp:174` |
| **SICK API internal threads** | Data reception from scanner (managed by sick_scan_xd library) | Opaque, inside `SickScanApiInitByCli()` |

### 3.2 Data Flow Design

```
SICK Scanner (TCP/IP, managed by sick_scan_xd)
    → sick_scan_xd internal receive (library-managed threads)
    → pointCloudCallback() (C function pointer, called on SICK's thread)
        → Parse PointCloudMsg fields (x,y,z,intensity)
        → Build PointCloudFrame
        → Push to ThreadSafeQueue (bounded, drops on full)
    → Writer thread pops from queue
        → BufferedBinaryWriter::write_frame() (LIDARPCD format)
        → Common::BinaryWriter (POSIX write() with 8MB buffer)

Signal (SIGINT/SIGTERM)
    → LidarDriverApp::shutdown()
        → Set terminate_ flag
        → Deregister callbacks
        → Join writer thread (drains remaining queue items)
        → Close writer (flush remaining buffer)
        → SickScanApiClose/Release/UnloadLibrary
        → [Optional] BinaryToCsvConverter::convert() (binary→CSV)
```

### 3.3 Key Components

#### 3.3.1 `LidarDriverApp` (`lidar_driver_app.h/cpp`)
- Top-level lifecycle class matching InsDriverApp pattern: `init()/run()/shutdown()`
- Manages SICK API handle, library loading, callback registration
- Contains `SuppressStdout` RAII helper (redirects stdout to /dev/null for noisy SICK library)
- Writer thread function runs inside the class

#### 3.3.2 `PointCloudCallback` (`point_cloud_callback.h/cpp`)
- C-style callback function (SICK API limitation: no user_data parameter)
- Uses namespace-scoped global `CallbackContext*` pointer
- Parses SICK `SickScanPointCloudMsg` field offsets dynamically
- Filters NaN/Inf points
- Pushes `PointCloudFrame` to `ThreadSafeQueue`

#### 3.3.3 `BufferedBinaryWriter` (`buffered_writer.h/cpp`)
- Thin wrapper over `Common::BinaryWriter`
- Adds LIDARPCD file format: magic header ("LIDARPCD") + version + per-frame framing
- Periodic flush at configurable intervals

#### 3.3.4 `BinaryToCsvConverter` (`csv_converter.h/cpp`)
- Post-processing tool: reads LIDARPCD binary → writes CSV
- Uses POSIX `open()/read()` with `posix_fadvise(SEQUENTIAL)` for input
- Uses `stdio` with 256KB buffer for output
- Progress callback support

#### 3.3.5 `ThreadSafeQueue` (local shim → `Common::ThreadSafeQueue`)
- Backward-compatibility alias: `ThreadSafeQueue<T>` → `Common::ThreadSafeQueue<T>`
- Bounded queue with mutex + condition variable
- `push()` returns false when full (non-blocking for producer)
- `pop()` blocks with timeout; respects stop_flag

### 3.4 Buffering Strategy

- **Queue:** `ThreadSafeQueue` with `max_queue_size = 256` frames
- **Write buffer:** `Common::BinaryWriter` with 8MB POSIX buffer
- **CSV output buffer:** 256KB `setvbuf` for post-processing

### 3.5 Configuration

- `LiDARConfig` struct in `lidar_data_type.h`
- YAML loading via `LidarTool::LoadConfig()` (partially implemented — has copy-paste from INS config with wrong field names like `port`, `mount_point`, etc.)
- Standalone `main.cpp` has its own `LoadConfigFromYaml()` (different from unified main)
- **Issue:** `lidar_tool.cpp:13-31` reads INS-specific fields (`port`, `mount_point`, `use_vrs`, `username`, `password`, etc.) that don't belong in LiDARConfig. This appears to be unfinished/buggy copy-paste code.

---

## 4. Architecture Differences Comparison Table

| Aspect | INS401 Driver | SICK LMS4XXX Driver |
|--------|--------------|---------------------|
| **Network transport** | Raw Ethernet (Layer 2) with custom Aceinna protocol, BPF filter | TCP/IP via sick_scan_xd library (opaque) |
| **Receive mechanism** | Custom `EthernetSocket` with epoll + `ReceiveBatch()` | SICK API manages receive internally |
| **Receive thread** | Custom `ReceiveLoop()` in dedicated thread | SICK library internal threads |
| **Data callback thread** | Receiver thread directly invokes callbacks | SICK library calls `pointCloudCallback` on its own thread |
| **Binary write location** | Directly in receiver callbacks (same thread as receive) | Dedicated writer thread via `ThreadSafeQueue` |
| **Queue between receive and write** | **None** (direct write on hot path) | `ThreadSafeQueue<PointCloudFrame>` (bounded, mutex-based) |
| **Write I/O** | `std::ofstream` with `pubsetbuf` (256KB) | `Common::BinaryWriter` (POSIX `write()`, 8MB buffer) |
| **Binary format** | Raw fixed-size struct payloads (no framing/magic) | LIDARPCD with magic header + version + per-frame header |
| **Post-processing** | `ProcessBinaryFiles()` reads raw structs → CSV with `fmt::memory_buffer` batching | `BinaryToCsvConverter` reads LIDARPCD → CSV with `stdio` |
| **Statistics** | Per-type atomic counters + `LogStatistics()` | `WriterStats` + `dropped_frames` atomic counter |
| **GNSS monitoring** | Hysteresis state machine for RTK/STD transitions | N/A |
| **Signal handling** | `Common::SignalHandler` (shared) | `Common::SignalHandler` (shared) |
| **Config loading** | `InsTool::LoadConfig()` via `Common::ConfigLoader` (yaml-cpp) | `LidarTool::LoadConfig()` (buggy/incomplete) |
| **Lifecycle class** | `InsDriverApp` with `init()/run()/shutdown()` + `request_shutdown()` | `LidarDriverApp` with identical pattern |
| **Thread safety for statistics** | Lock-free atomics | Mutex-based (via `WriterStats` updated in single writer thread) |

---

## 5. Shared Components Analysis (`common/`)

### 5.1 `Common::BinaryWriter` (binary_writer.h/cpp)

**Status:** Complete and well-implemented.
- POSIX `write()` with configurable buffer (default 8MB)
- Proper partial-write loop and EINTR retry
- Not thread-safe by design (single-writer pattern)
- **Used by:** SICK driver (`BufferedBinaryWriter` wraps it)
- **NOT used by:** INS401 driver (still uses `std::ofstream` with `pubsetbuf`)
- **Recommendation:** Migrate INS401 to use `Common::BinaryWriter` for consistency. The current `std::ofstream` approach works but `BinaryWriter` offers better crash safety due to explicit flush control.

### 5.2 `Common::ThreadSafeQueue` (thread_safe_queue.h)

**Status:** Header-only, functional.
- Bounded queue with `std::mutex` + `std::condition_variable`
- `push()` non-blocking (returns false if full)
- `pop()` blocking with timeout + stop_flag awareness
- **Used by:** SICK driver
- **NOT used by:** INS401 driver (writes directly in receive callback)
- **Performance concern:** Uses `std::mutex` — adequate for 600Hz LiDAR but not ideal for highest-frequency paths. For future optimization, an SPSC lock-free ring buffer could replace this for single-producer/single-consumer paths.

### 5.3 `Common::SignalHandler` (signal_handler.h/cpp)

**Status:** Complete and shared.
- Static pointers to `std::atomic<bool>` and optional `std::atomic<int>`
- Async-signal-safe: handler only does atomic stores
- Supports configurable signal list
- **Used by:** Both drivers and unified main

### 5.4 `Common::ConfigLoader` / `Common::Logger` / `Common::Log` (utility.h/cpp)

**Status:** Complete and shared.
- `ConfigLoader`: yaml-cpp wrapper with `get<T>(section, key, default)` convenience
- `Logger::init()`: Dual-sink (stderr + optional file), configurable quiet mode
- `Log::log_message()`: Module-tagged logging. **Note:** `log_message()` with `level >= err` calls `log_and_throw()`, which is a surprising behavior — logging at error level always throws!
- `Log::log_and_throw()`: Log + `throw std::runtime_error`
- `Log::sick_msg()`: SICK library message forwarding

### 5.5 `Common::Config` (data_type.h)

**Status:** Minimal shared config struct.
- Fields: `output_directory`, `run_mode`, `ins_config_path`, `lidar_config_path`, `lidar_launch_path`, `enable_logging`, `timestamp`, `data_folder_path`

---

## 6. Recommended Refactoring Direction

### Priority 1: Fix Critical Issues

1. **`lidar_tool.cpp` LoadConfig bug** — The function reads INS-specific fields (`port`, `mount_point`, `use_vrs`, `username`, `password`, `enable_gnss_checking`, `gnss_horizontal_std_threshold`, etc.) that don't exist in `LiDARConfig`. These fields don't compile unless `LiDARConfig` has been incorrectly extended. This needs cleanup.

2. **`utility.cpp` Log::log_message() auto-throws on error level** — `log_message()` at line 42 checks `if (level >= spdlog::level::err)` and calls `log_and_throw()`. This means ANY call to `log_message(spdlog::level::err, ...)` will throw, which is dangerous in shutdown paths and callback contexts. This should be decoupled: error logging should NOT automatically throw.

3. **`main.cpp` (unified) references undefined variable** — Line 213: `lidar_driver_config` is referenced but never defined. This is a compilation error.

### Priority 2: Architecture Alignment

4. **INS401 binary writer modernization** — Replace `std::ofstream` + `pubsetbuf` with `Common::BinaryWriter` for consistency. This also opens the door for INS401 to use the same queue-based writer pattern as SICK.

5. **INS401 writer thread separation** — Currently, binary writes happen on the receiver thread. For the current data rates this is fine, but introducing a writer thread (matching the SICK pattern) would future-proof for higher data rates and improve the separation of concerns.

6. **LiDARConfig cleanup** — Remove copy-paste INS fields from `LiDARConfig`/`lidar_tool.cpp`. Define clean SICK-specific fields: `launch_file`, `hostname`, `ntp_server_ip`, `output_file`, `quiet`, `library_search_paths`, `library_name`, `max_queue_size`, `write_buffer_size`.

### Priority 3: Performance Optimizations

7. **Lock-free queue for hot paths** — Replace `Common::ThreadSafeQueue` with an SPSC ring buffer for the callback→writer path in the SICK driver. At 600Hz this is not critical, but it eliminates mutex contention and reduces latency variance.

8. **Batch receive for SICK driver** — The SICK API callback processes one frame at a time. If the API supports batch callbacks or if we buffer multiple frames before queue push, this could reduce queue contention.

9. **INS401 write path optimization** — If writer thread separation (point 5) is adopted, use `Common::BinaryWriter` with a large buffer instead of per-file `ofstream`.

### Priority 4: Code Quality

10. **SICK driver statistics** — Add per-type counters matching INS401 pattern (frames received, dropped, CRC errors, bytes).

11. **SICK driver GNSS/time logging** — Add timestamp validation and monitoring similar to INS401's `MonitorGNSSStatus()`.

12. **Unified config file** — The `config-main.yaml` + per-driver configs work but the loading logic is inconsistent between standalone and unified modes.

---

## 7. Interface Design Suggestions

### 7.1 For Concurrency Specialist (Teammate 2)

**Components to design/implement in `common/`:**

1. **SPSC Ring Buffer** (`common/include/ring_buffer.h`)
   - Template `RingBuffer<T, N>` with compile-time size
   - Lock-free, cache-line-padded head/tail
   - `try_push(T&&)` / `try_pop(T&)` non-blocking API
   - Used for callback→writer hot path

2. **Binary writer improvements** — Current `Common::BinaryWriter` is solid. Consider:
   - Adding `fdatasync()` option for periodic durability guarantees
   - Supporting multiple output files from a single writer instance

3. **Graceful shutdown coordinator** — Current approach (atomic flag + thread join) works but is ad-hoc. Consider a shared shutdown sequence:
   ```
   stop_receiving() → drain_queues() → flush_writers() → post_process() → log_stats()
   ```

### 7.2 For SICK Driver Implementer (Teammate 3)

**Recommended class structure for refactored SICK driver:**

```
lms4xxx_driver/
├── include/
│   ├── lidar_driver_app.h      # Keep: top-level lifecycle (init/run/shutdown)
│   ├── lidar_receiver.h        # NEW: wraps SICK API + callback (analogous to INSDeviceReceiver)
│   ├── lidar_data_type.h       # Keep: LiDARConfig struct (cleaned up)
│   ├── lidar_protocol.h        # NEW: LIDARPCD format constants (analogous to ins401_protocol.h)
│   ├── point_cloud_types.h     # Keep: PointXYZI, PointCloudFrame, WriterStats
│   ├── buffered_writer.h       # Keep: wraps Common::BinaryWriter with LIDARPCD format
│   ├── csv_converter.h         # Keep: binary→CSV post-processing
│   └── lidar_tool.h            # Keep: config loading (fixed)
└── src/
    ├── lidar_driver_app.cpp
    ├── lidar_receiver.cpp       # NEW: encapsulates SICK API + callback
    ├── buffered_writer.cpp
    ├── csv_converter.cpp
    ├── lidar_tool.cpp           # Fixed config loading
    └── main.cpp
```

**Key refactoring:**
- Extract SICK API management into `LidarReceiver` class (matches `INSDeviceReceiver`)
- Move `SuppressStdout` to `lidar_receiver.cpp`
- Move callback and context management into `LidarReceiver`
- Add statistics tracking with atomic counters (matching INS401)
- `LidarDriverApp` delegates to `LidarReceiver` + `BufferedBinaryWriter` (cleaner separation)

---

## 8. Potential Improvement Points for INS401

While the INS401 driver is the reference template, the analysis revealed several potential improvements:

1. **Write path:** Replace `std::ofstream`+`pubsetbuf` with `Common::BinaryWriter` for consistency and better flush control.

2. **Statistics struct:** The `Statistics` struct in `INSDeviceReceiver` requires loading 13 atomics. Consider using a statistics snapshot pattern (periodic copy to a non-atomic struct).

3. **NTRIP queue:** `NTRIPClient::data_queue_` uses `std::queue<std::vector<uint8_t>>` with mutex. This allocates heap memory per message. Consider pre-allocated buffers or a ring buffer.

4. **BPF filter hardcoding:** The BPF filter in `ethernet_socket.cpp:273-313` is manually constructed with jump offsets. Consider using `cbpf_insn` helpers or documenting the filter logic more explicitly (already has good comments, but fragile if modified).

5. **`INSConfig` missing field:** `enable_logging` is referenced in `ins401_driver/src/main.cpp:54` but not defined in `INSConfig`. The standalone main uses it but the unified path gets it from `Common::Config`.

6. **Callback mutex on hot path:** `callback_mutex_` is acquired for every IMU packet (100Hz) in `HandleRawIMUPacket()`. This is a `std::scoped_lock` acquisition per packet. Consider setting callbacks once and using `std::atomic<ImuCallback*>` or similar to avoid per-packet locking.

---

## 9. Summary of Key Findings

### Strengths
- Clean lifecycle pattern (`init/run/shutdown`) shared by both drivers
- Shared infrastructure (`common/`) for logging, config, signal handling
- INS401 binary-first design with post-processing is efficient for high-frequency data
- SICK driver's queue+writer pattern properly decouples receive from I/O
- Good use of RAII throughout (sockets, SSL, file descriptors)
- Atomic counters for lock-free statistics on hot paths (INS401)

### Weaknesses
- `lidar_tool.cpp` has buggy copy-paste from INS config
- `utility.cpp` `log_message()` auto-throws on error level (surprising behavior)
- `main.cpp` (unified) has compilation errors (`lidar_driver_config` undefined)
- INS401 writes on receiver thread (no write thread separation)
- `ThreadSafeQueue` uses mutex (adequate but not optimal for highest frequency)
- Inconsistent I/O approaches: INS401 uses `ofstream`, SICK uses POSIX `write()`

### Architecture Alignment Gap
The SICK driver is already fairly well-aligned with INS401 patterns. The main gaps are:
1. No equivalent of `INSDeviceReceiver` class (SICK API management is mixed into `LidarDriverApp`)
2. No protocol constants header (LIDARPCD format defined inline in `point_cloud_types.h`)
3. Config loading has INS-specific remnants
4. Statistics tracking is simpler than INS401
5. No real-time data monitoring equivalent (INS401 has GNSS hysteresis state machine)
