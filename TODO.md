# Todo List

Modularization roadmap (decided 2026-07; details in the session notes):

- [x] P0 — Stop the bleeding: fix the in-flight logging-migration breakages,
      drop `binary_writer`/`thread_safe_queue` dead code
- [x] P1 — Contract lock-in: `driver_markers.h` + `app/services/markers.py`
      single source of truth, `tools/check_contracts.py`, doctest/nlohmann
      uplifted to `3rd_party/`, logger line-format regression test, docs rewrite
- [x] P2 — Logging unification: all four drivers on `Common::DriverLog`
      (gox backend switch first; lms4xxx write-thread `log_message(err)` traps;
      err decision table: init paths throw, worker/dtor paths never)
- [x] P3 — Pure-utility sink-down: time/string/byte/checksum helpers,
      `BoundedQueue` uplift, `thread_util` (RT priority + affinity)
- [x] P4 — Interface formalization: `IDriverApp` base class, unified
      `[[nodiscard]] bool init(external_stop)`, main.cpp scheduling loop,
      ins401 `namespace INS401`
- [x] P5 — Transport: lms4xxx `TCPClient` -> common, NTRIP rebuild on top
      (drop dead SSL code and the OpenSSL dependency, drop rtcm_base recording)
- [x] P6 — Recording: `BufferedFileWriter` + `RotatingFileWriter` in common;
      ins401/asterx/lms4xxx writer skeletons unified (golden byte-compare
      verification; gox Recorder stays frozen)
- [x] P7 — Config & parse: asterx via `Common::ConfigLoader`, shared ins401
      wire-format header, DataConverter recursive `bin/<driver>/` scan,
      re-enable `parse/` in the build
- [x] F — fx10 integration: root FindeBUS unification (single Pleora SDK for
      gox + fx10), `Fx10DriverApp` on `IDriverApp` + `Common::DriverLog`, GUI
      contract markers + `/fx10` tools page + `fx10_snapshot`; the ENVI on-disk
      format stays frozen (see `fx10_driver/docs/BRINGUP.md`)
- [x] P8 — Style finalization (2026-09): Google PascalCase for every function and
      method, lowercase namespaces (`common`, `asterx`, `fx10`, `gox`, `lms4xxx`),
      `.h` + `#pragma once` everywhere, lms4xxx file prefix dropped, comment
      slimming. The one-shot clang-format (tab) run is the last step: do it in the
      devcontainer as a separate whitespace-only commit and add its hash to
      `.git-blame-ignore-revs`
- [x] Tests: every driver and common on the vendored doctest, one
      `<driver>_tests` binary each with one ctest entry per source file; tests
      build in every configuration (`AMIGA_BUILD_TESTS`, default ON)
- [ ] fx10: `tools/snapshot_main.cpp` duplicates the `bringUpSession_()`
      sequence (connect → factory load → stream open → apply → geometry). The
      two are cross-referenced by comment for now; unify only if a third caller
      appears — the invariants and the failure contracts genuinely differ

___

### Done ✓

- [x] Four-driver review + unification (2026-09): one YAML schema policy
      (`common/include/config_util.h`: unknown keys are errors, omitted keys
      keep the defaults, dotted key paths, range checks) and one template layout
      for all six yaml files; three shipped templates that the parsers rejected
      (asterx `output`/`logging`, fx10 `output_dir`, lms `file_prefix`) and the
      fx10 build break (`device.id`/`base_name`) fixed; ~40 bugs across the
      drivers (asterx reply binding and header-length bound, gox recorder fd
      leak / fallocate reclaim / busy-spin / silent truncation, fx10 stop() race
      and MROI silent branch, lms Connect UAF, framer rewind, parser trailing
      bytes, SNTP nonce, H5 accessor race); dead code removed (fx10
      `features.map`, lms dead builders, common helpers with no caller, gox
      forwarders); lms `ntp_probe` / `scan_verify` split out and unit-tested

- [x] lms4xxx review + device self-report (2026-09): the measurement setup was
      already the most raw the device offers (DIST + remission + angle + quality,
      841 points, every scan, all eight filters off — filtering is irreversible,
      manual p.24), so nothing new is exposed; what changed is everything around
      it. A run that faults now exits non-zero and writes `failed (<driver>)` to
      `drivers.json` instead of reporting `completed` (this one is in `main.cpp`
      and covers all four drivers); a stall watchdog ends a run when the device
      stops streaming with the connection still open; a latched fault stops the
      parse thread instead of letting ~120 known-invalid scans reach the file;
      `TCPClient::Read` keeps the bytes it already took off the wire and the
      command path realigns, so one slow answer no longer wedges the control
      channel; `SO_SNDTIMEO`/`SO_RCVTIMEO` are reachable from the yaml at last;
      the shutdown standby is now a checked handshake (it used to be issued while
      logged out and refused in silence, leaving the laser on after every run);
      HDF5 gained `fdatasync`, a checked `H5Fclose`, a `closed_cleanly` +
      `frames_total` completeness marker and `H5F_ACC_EXCL`; `fps=` now means
      frames on disk like every other driver; the yaml has a strict schema with
      every numeric bound; and the device self-report (temperature, operating
      hours, device state, active warnings, pre-filter scan config, laser
      control, motor-sync role) lands in the `.h5` root attributes and a new
      `/telemetry` group (`format_version` 3). Motor synchronisation is read but
      never written — the rig's layout does not need it, and it is the one
      parameter `mSCloadappdef` cannot reset (manual p.79). New tests cover the
      telegram parser, the framer, the config schema and the record layer, which
      had none. See `lms4xxx_driver/docs/DEVICE_CONFIG.md`

- [x] fx10 review + raw-data alignment (2026-09): the camera write ORDER became
      a pure, unit-tested plan (`apply_plan.{hpp,cpp}`, `tests/test_apply_plan.cpp`)
      instead of an imperative SDK-coupled function; a camera-side clamp of a
      driver-derived value is now fatal (it used to only warn while enum
      mismatches were fatal); `spectral_binning` back to the factory 2 (224
      bands, the calibration was made at 2) and AIE exposed as
      `acquisition.image_enhancement` (written only when turned off); Counter1
      is bound + reset instead of assumed; both temperatures polled through
      `DeviceTemperatureSelector` with a thermal warning; disk floor freed from
      the statistics cadence; recorder failures classified by an `ErrorKind`
      enum instead of by log-substring matching, and a finalize failure (no
      `.hdr` = invalid segment) now fails the run; periodic `fdatasync` via
      `output.flush_interval_mb` with size-based rotation; `[Statistics]` gained
      `temp_pcb=`/`temp_fpga=` and the final line `rx_timeouts=`;
      `fx10_snapshot --dump-features`. See `fx10_driver/docs/DEVICE_CONFIG.md`;
      hardware verification items are under its "Open points"

- [x] Unified four-driver framework (single process, shared spdlog, session
      folders, web GUI with health monitoring)
- [x] lms4xxx recorder on HDF5 (`lms4xxx-h5`, 2026-08): self-describing
      `scan_<id>_<ts>_NNN.h5` split files replace the private `scan_*.bin`
      container; `parse/` DataConverter retired (`scripts/inspect_h5.py`)
- [x] gox raw-data completeness (2026-09): `acquisition.blemish_correction`
      (default `false` = manual's "Disable all"), Counter0 bound to the received
      FrameTrigger events, and two self-describing sidecars per camera —
      `<cam>/device.json` (factory load, applied writes with read-backs, a
      ~49-entry raw audit of everything the driver never writes, identity,
      transport, runtime shape, PTP) and `<cam>/telemetry.jsonl` (5 s:
      temperatures, Counter0, PAUSE frames, PTP). `[Statistics]` gained
      `temp=`/`trig=` and `triggers=`/`missed=`. See
      `gox_driver/docs/DEVICE_CONFIG.md`; hardware verification items are listed
      under its "Open points"
- [x] Shared eBUS layer (2026-09): `common/include/ebus` + `common/src/ebus`
      (target `amiga_ebus`, SDK-dependent, separate from `amiga_common`) now
      holds the GenICam env bootstrap, PvResult errors, GigE discovery and the
      device IP configuration for both camera drivers; `jai_discover` became
      the common `ebus_discover`, `fx10_snapshot --list` is gone, and the
      per-camera `device.force_ip` rescue was retired from both configs.
      Field re-addressing is the GUI's Camera Tools → Set IP (`ebus_set_ip`:
      FORCEIP, then the persistent-IP GenICam nodes, optional `device.ip`
      writeback into the driver config)
