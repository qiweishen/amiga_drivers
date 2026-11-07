"""One operation may run several device tools in sequence (exposure bracket); never two at once."""

import asyncio
import unittest
from contextlib import ExitStack
from unittest.mock import AsyncMock, patch

from app.services import runtime, tool_jobs


class _Stream:
    def __init__(self, data: bytes):
        self._chunks = [data]

    async def read(self, _size):
        return self._chunks.pop() if self._chunks else b""


class _Stdin:
    def write(self, _data):
        pass

    async def drain(self):
        pass

    def close(self):
        pass


class _Proc:
    def __init__(self, pid: int):
        self.pid, self.returncode = pid, 0
        self.stdin = _Stdin()
        self._pid_line = f"{pid}\n".encode()
        self.stdout = _Stream(b"SNAPSHOT: OK /captures/x\n")
        self.stderr = _Stream(b"")

        async def readline():
            return self._pid_line
        self.stdout.readline = readline

    async def wait(self):
        return 0


class SequentialToolTests(unittest.IsolatedAsyncioTestCase):
    def setUp(self):
        self.stack = ExitStack()
        self.addCleanup(self.stack.close)
        self.pids = iter(range(1000, 1010))
        self.stack.enter_context(patch.object(tool_jobs, "_handle", None))
        self.stack.enter_context(patch.object(tool_jobs, "_save"))
        self.stack.enter_context(patch.object(tool_jobs.STATE, "tool_operation", {"id": "op", "state": "starting"}))
        self.stack.enter_context(patch.object(runtime, "gated_tool", new_callable=AsyncMock,
                                              side_effect=lambda args: _Proc(next(self.pids))))
        self.stack.enter_context(patch.object(runtime, "process_identity", new_callable=AsyncMock,
                                              side_effect=lambda pid: runtime.ProcessIdentity(pid, "1", "boot")))
        self.alive = self.stack.enter_context(patch.object(runtime, "is_process_alive", new_callable=AsyncMock,
                                                           return_value=False))

    async def test_a_finished_tool_frees_the_slot_for_the_next_capture(self):
        for _ in range(3):
            proc = await tool_jobs.spawn(["/bin/jai_snapshot", "--exposure-ms", "50"])
            out, _ = await tool_jobs.communicate(proc, 5)
            self.assertIn(b"SNAPSHOT: OK", out)
        self.assertIsNone(tool_jobs._handle)

    async def test_overlapping_tools_are_still_refused(self):
        await tool_jobs.spawn(["/bin/jai_snapshot"])
        with self.assertRaisesRegex(RuntimeError, "overlapping"):
            await tool_jobs.spawn(["/bin/jai_snapshot"])

    async def test_a_tool_still_alive_keeps_the_slot(self):
        proc = await tool_jobs.spawn(["/bin/jai_snapshot"])
        self.alive.return_value = True
        with patch.object(tool_jobs, "terminate", new_callable=AsyncMock), \
                patch.object(tool_jobs, "_wait_absent", new_callable=AsyncMock, return_value=False):
            with self.assertRaisesRegex(RuntimeError, "still alive"):
                await tool_jobs.communicate(proc, 5)
        self.assertIsNotNone(tool_jobs._handle)


if __name__ == "__main__":
    unittest.main()
