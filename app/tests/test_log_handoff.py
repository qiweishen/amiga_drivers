"""Display handoff fixtures; no acquisition process or device is needed."""

import unittest

from app.services.log_buffer import STDERR_HANDOFF_LINES, LogBuffer, parse_line


class LogHandoffTests(unittest.TestCase):
    def setUp(self):
        self.buffer = LogBuffer()
        self.visible = []
        self.buffer.subscribe(lambda line: self.visible.append(line.raw))

    def test_startup_pipe_copy_is_not_replayed_to_subscribers(self):
        startup = parse_line("[10:28:43] [info] [MainApp]: Starting Amiga Drivers")
        self.buffer.append_stderr(startup)
        self.buffer.append_session(startup)
        self.assertEqual(self.visible, [startup.raw])
        self.assertEqual(self.buffer.snapshot(), [startup])

    def test_stderr_only_diagnostic_survives_file_handoff(self):
        diagnostic = parse_line("loader diagnostic", fallback_module="stderr")
        startup = parse_line("[10:28:43] [info] [MainApp]: Starting Amiga Drivers")
        self.buffer.append_stderr(diagnostic)
        self.buffer.append_stderr(startup)
        self.buffer.append_session(startup)
        self.assertEqual(self.visible, [diagnostic.raw, startup.raw])

    def test_matching_bare_text_is_not_assumed_to_be_a_duplicate(self):
        diagnostic = parse_line("loader diagnostic", fallback_module="stderr")
        file_line = parse_line("loader diagnostic")
        self.buffer.append_stderr(diagnostic)
        self.buffer.append_session(file_line)
        self.assertEqual(self.visible, [diagnostic.raw, file_line.raw])

    def test_each_startup_occurrence_cancels_only_one_file_copy(self):
        repeated = parse_line("[10:28:43] [warning] [AsteRx]: Waiting")
        self.buffer.append_stderr(repeated)
        self.buffer.append_stderr(repeated)
        self.buffer.append_session(repeated)
        self.buffer.append_session(repeated)
        self.buffer.append_session(repeated)
        self.assertEqual(self.visible, [repeated.raw] * 3)

    def test_file_only_line_and_delayed_flush_are_both_retained_once(self):
        first = parse_line("[10:28:43] [info] [MainApp]: Starting")
        file_only = parse_line("[10:28:43] [debug] [MainApp]: Details")
        delayed = parse_line("[10:28:43] [info] [AsteRx]: Connected")
        self.buffer.append_stderr(first)
        self.buffer.append_stderr(delayed)
        self.buffer.append_session(first)
        self.buffer.append_session(file_only)
        # A later file poll catches a line that was still in the C++ file buffer.
        self.buffer.append_session(delayed)
        self.assertEqual(self.visible, [first.raw, delayed.raw, file_only.raw])

    def test_new_run_does_not_consume_a_previous_runs_unmatched_line(self):
        line = parse_line("[10:28:43] [error] [MainApp]: Failed")
        self.buffer.append_stderr(line)
        self.buffer.clear()
        self.visible.clear()
        self.buffer.append_session(line)
        self.assertEqual(self.visible, [line.raw])

    def test_overlap_tracking_is_bounded_without_discarding_file_events(self):
        lines = [parse_line(f"[10:28:43] [info] [MainApp]: Step {i}")
                 for i in range(STDERR_HANDOFF_LINES + 1)]
        for line in lines:
            self.buffer.append_stderr(line)
        # Beyond the remembered window, prefer showing the file event over
        # guessing whether it was already displayed.
        self.buffer.append_session(lines[0])
        self.buffer.append_session(lines[-1])
        self.assertEqual(self.visible, [line.raw for line in lines] + [lines[0].raw])


if __name__ == "__main__":
    unittest.main()
