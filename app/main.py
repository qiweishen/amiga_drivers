"""Entry point: `uv run amiga-gui` (registered in pyproject [project.scripts])."""

from __future__ import annotations

from nicegui import app, ui

from .constants import GUI_HOST, GUI_PORT, RUNTIME_DIR
from .services import process, runtime, storage
from .services.health import MONITOR
from .services.log_buffer import BUFFER
from .services.session_tailer import TAILER
from .state import STATE

# Pages register themselves via @ui.page on import.
from .ui import config_editor, dashboard, gox, logs  # noqa: F401


async def _startup() -> None:
    RUNTIME_DIR.mkdir(parents=True, exist_ok=True)
    STATE.mode = await runtime.detect_mode()
    await _check_env()
    await process.reattach()  # adopt an already-running acquisition, if any
    await storage.poll()


async def _check_env() -> None:
    STATE.env_ok, STATE.env_detail = await runtime.env_check()


def main() -> None:
    # The tailer is the single primary feed: health first (state transitions),
    # then the buffer (pages render what health already classified).
    TAILER.subscribe(MONITOR.on_line)
    TAILER.subscribe(BUFFER.append)

    app.on_startup(_startup)
    app.timer(5.0, storage.poll)
    app.timer(10.0, _check_env)

    ui.run(
        host=GUI_HOST,
        port=GUI_PORT,
        title="Amiga Sensor Console",
        reload=False,
        show=False,
        favicon="app/resource/appn.png"
    )


if __name__ == "__main__":
    main()
