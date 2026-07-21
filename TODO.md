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
- [ ] P8 — Style finalization: one-shot clang-format (tab), comment slimming
      per the three-way policy, header-guard/namespace cleanup

___

### Done ✓

- [x] Unified four-driver framework (single process, shared spdlog, session
      folders, web GUI with health monitoring)
