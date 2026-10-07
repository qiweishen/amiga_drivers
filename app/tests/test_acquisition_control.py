"""Offline lifecycle contracts; all process and manifest I/O is substituted."""

import asyncio
from contextlib import ExitStack
from dataclasses import asdict, FrozenInstanceError
from pathlib import Path
from types import SimpleNamespace
import unittest
from unittest.mock import AsyncMock, patch

from app.state import AppState, ProcState
from app.services import control_service, process, run_status, runtime, session_info
from app.services.log_buffer import LogBuffer, parse_line


class RecordingViewTests(unittest.TestCase):
    def test_pages_cannot_change_owner_state_or_nested_results(self):
        owner = process.AcquisitionController()
        owner._update(enables_at_start={"gox": True}, run_result={"run": {"status": "running"}})
        view = AppState()
        view.bind_recording(owner.snapshot, owner.snapshot_value)
        with self.assertRaises(AttributeError):
            view.process_state = ProcState.RUNNING
        with self.assertRaises(FrozenInstanceError):
            view.recording.session_generation = 99
        view.enables_at_start["gox"] = False
        view.run_result["run"]["status"] = "completed"
        self.assertTrue(owner.snapshot().enables_at_start["gox"])
        self.assertEqual(owner.snapshot().run_result["run"]["status"], "running")


class LifecycleTests(unittest.IsolatedAsyncioTestCase):
    async def test_page_cancel_and_stop_during_preflight_do_not_abandon_launch(self):
        owner = process.AcquisitionController()
        owner._update(control_uncertain=False)
        entered, release = asyncio.Event(), asyncio.Event()

        async def preflight():
            entered.set()
            await release.wait()
            return [], []

        with ExitStack() as stack:
            stack.enter_context(patch.object(process, "STATE", AppState(env_ok=True, tool_uncertain=False)))
            for name in ("TAILER", "ASTERX_LIVE", "BUFFER", "STATS"):
                stack.enter_context(patch.object(process, name))
            stack.enter_context(patch.object(process.control_owner, "require"))
            stack.enter_context(patch.object(owner, "preflight", preflight))
            spawn = stack.enter_context(patch.object(runtime, "spawn", new_callable=AsyncMock))
            signal = stack.enter_context(patch.object(runtime, "signal_process", new_callable=AsyncMock))
            page = asyncio.create_task(owner.start())
            await asyncio.wait_for(entered.wait(), 1)
            page.cancel()
            with self.assertRaises(asyncio.CancelledError):
                await page
            self.assertFalse(owner._start_task.done())
            await owner.stop()
            self.assertEqual(owner.snapshot().process_state, ProcState.STOPPING)
            release.set()
            self.assertFalse(await asyncio.wait_for(owner._start_task, 1))
            await asyncio.wait_for(owner._stop_task, 1)
            spawn.assert_not_awaited()
            signal.assert_not_awaited()
            self.assertEqual(owner.snapshot().process_state, ProcState.EXITED)
            self.assertFalse(owner.snapshot().config_locked)

    async def test_recovery_result_from_earlier_generation_is_discarded(self):
        owner = process.AcquisitionController()
        ref = runtime.ProcessIdentity(42, "123", "boot")
        owner._ref = ref

        async def recover(_ref):
            owner._update(session_generation=1)
            return SimpleNamespace(path=Path("/data/stale"), started_at=1, enables={"gox": True}, lms_names=[])

        with patch.object(session_info, "recover", side_effect=recover), patch.object(run_status, "read_session") as read:
            await owner._observe_session(ref, 0)
            read.assert_not_called()
        self.assertIsNone(owner.snapshot().active_session)

    async def test_old_stop_worker_cannot_signal_new_generation(self):
        owner = process.AcquisitionController()
        owner._update(session_generation=2, stop_requested=True)
        owner._ref = runtime.ProcessIdentity(42, "123", "boot")
        with patch.object(runtime, "signal_process", new_callable=AsyncMock) as signal:
            await owner._stop_when_ready(1, 0)
            signal.assert_not_awaited()

    async def test_service_shutdown_rejects_new_start(self):
        owner = process.AcquisitionController()
        owner._update(control_uncertain=False)
        with patch.object(process.control_owner, "require"):
            await owner.shutdown()
            self.assertFalse(await owner.start())

    async def test_start_gate_closes_before_tool_shutdown_yields(self):
        owner = process.AcquisitionController()
        owner._update(control_uncertain=False)
        display = AppState(env_ok=True, tool_uncertain=False)
        display.bind_recording(owner.snapshot, owner.snapshot_value)

        async def stop_tools():
            await asyncio.sleep(0)
            self.assertFalse(await owner.start())

        with ExitStack() as stack:
            stack.enter_context(patch.object(process.control_owner, "require"))
            stack.enter_context(patch.object(process, "STATE", display))
            stack.enter_context(patch.object(control_service, "STATE", display))
            stack.enter_context(patch.object(process, "shutdown", owner.shutdown))
            stack.enter_context(patch.object(owner, "_launch", new_callable=AsyncMock, return_value=True))
            stack.enter_context(patch.object(control_service.tool_jobs, "shutdown", side_effect=stop_tools))
            stack.enter_context(patch.object(control_service, "TAILER"))
            stack.enter_context(patch.object(control_service, "ASTERX_LIVE"))
            await control_service.ApplicationServices().shutdown()


class ManifestTests(unittest.TestCase):
    def setUp(self):
        self.path = Path("/data/20261007_120000")
        self.ref = runtime.ProcessIdentity(42, "123", "boot")
        self.doc = {
            "status_schema": run_status.SCHEMA, "process": asdict(self.ref),
            "run": {"timestamp": self.path.name, "session_directory": str(self.path),
                    "output_directory": str(self.path.parent), "started": "2026-10-07T12:00:00Z",
                    "ended": "2026-10-07T12:01:00Z", "recording_failed": False, "status": "completed"},
            "drivers": {"gox": {"enabled": True}},
            "lifecycle": {"phase": "finished", "configuration_read": True,
                          "sensors": {"gox": {"state": "stopped", "error": ""}}},
        }

    def test_only_current_protocol_and_exact_process_identity_are_accepted(self):
        with patch.object(run_status, "read_json", return_value=self.doc):
            self.assertTrue(session_info.finished_cleanly(self.path, self.ref))
            for key, value in (("pid", 43), ("start_ticks", "124"), ("boot_id", "other")):
                with self.subTest(identity_field=key), patch.dict(self.doc["process"], {key: value}):
                    with self.assertRaises(ValueError):
                        run_status.read_session(self.path, self.ref)
            for schema in (None, "old-run"):
                with self.subTest(schema=schema), patch.dict(self.doc, {"status_schema": schema}):
                    with self.assertRaises(ValueError):
                        run_status.read_session(self.path, self.ref)

    def test_running_manifest_cannot_certify_a_clean_exit(self):
        self.doc["lifecycle"]["phase"] = "running"
        with patch.object(run_status, "read_json", return_value=self.doc):
            self.assertFalse(session_info.finished_cleanly(self.path, self.ref))

    def test_invalid_sensor_update_does_not_partially_change_display(self):
        sensors = run_status.sensor_statuses({"gox": True}, [])
        self.doc["lifecycle"]["sensors"]["extra"] = {"state": "running"}
        with self.assertRaises(ValueError):
            run_status.sensor_update(self.doc, sensors)
        self.assertEqual(sensors["gox"].state.value, "waiting")

    def test_failed_finish_preserves_result_without_repeating_existing_log(self):
        owner = process.AcquisitionController()
        owner._ref = self.ref
        owner._update(active_session=self.path)
        self.doc["run"].update(recording_failed=True, status="failed (gox)")
        self.doc["lifecycle"]["sensors"]["gox"] = {"state": "failed", "error": "startup failed"}
        display = AppState()
        display.sensors = run_status.sensor_statuses({"gox": True}, [])
        buffer = LogBuffer()
        diagnostic = "[10:21:08] [error] [MainApp]: Run failed; recording is INCOMPLETE"
        buffer.append(parse_line(diagnostic))

        with patch.object(process, "STATE", display), patch.object(process, "BUFFER", buffer), \
                patch.object(run_status, "read_json", return_value=self.doc):
            owner._finish(1, clean=False)

        final = owner.snapshot()
        self.assertEqual([line.raw for line in buffer.snapshot()], [diagnostic])
        self.assertEqual(final.last_error, diagnostic)
        self.assertEqual(final.process_state, ProcState.FAILED)
        self.assertEqual(final.exit_code, 1)
        self.assertTrue(final.run_result["run"]["recording_failed"])
        self.assertEqual(final.run_result["run"]["status"], "failed (gox)")
        self.assertEqual(display.sensors["gox"].state.value, "failed")

    def test_failed_finish_without_log_emits_one_fallback_diagnostic(self):
        owner = process.AcquisitionController()
        buffer = LogBuffer()
        with patch.object(process, "BUFFER", buffer):
            owner._finish(1, clean=False)
            owner._finish(1, clean=False)
        self.assertEqual(len(buffer.snapshot()), 1)
        self.assertEqual(owner.snapshot().last_error, buffer.snapshot()[0].raw)
        self.assertEqual(owner.snapshot().process_state, ProcState.FAILED)


if __name__ == "__main__":
    unittest.main()
