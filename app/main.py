"""Entry point: `uv run amiga-gui` (registered in pyproject [project.scripts])."""

from __future__ import annotations

import os
from pathlib import Path

from nicegui import app, ui

from .constants import GUI_HOST, GUI_PORT, RUNTIME_DIR
from .services import asterx_live, process, runtime, storage
from .services.driver_stats import STATS
from .services.health import MONITOR
from .services.log_buffer import BUFFER
from .services.session_tailer import TAILER
from .state import STATE

# Pages register themselves via @ui.page on import.
from .ui import asterx, camera, config_editor, dashboard, live, logs  # noqa: F401


async def _startup() -> None:
    RUNTIME_DIR.mkdir(parents=True, exist_ok=True)
    STATE.mode = await runtime.detect_mode()
    await _check_env()
    await process.reattach()  # adopt an already-running acquisition, if any
    await storage.poll()
    sim = os.environ.get("AMIGA_ASTERX_SIM")  # dev-only: point the live
    if sim:  # tailer at a fake session (tools/asterx_sim_feed.py)
        asterx_live.LIVE.start(Path(sim), replay=False)


async def _check_env() -> None:
    STATE.env_ok, STATE.env_detail = await runtime.env_check()


def main() -> None:
    TAILER.subscribe(MONITOR.on_line)
    TAILER.subscribe(STATS.on_line)
    TAILER.subscribe(BUFFER.append)

    app.on_startup(_startup)
    # Per-sensor byte totals were the dashboard's stalest field; 2 s keeps them
    # in step with the rest of the card. storage.poll self-throttles when the
    # scan is slow, so this rate is safe on a slow output mount.
    app.timer(2.0, storage.poll)
    app.timer(10.0, _check_env)

    ui.run(
        host=GUI_HOST,
        port=GUI_PORT,
        title="Amiga Sensor Console",
        reload=False,
        show=False,
        favicon="app/resource/appn.svg"
    )


if __name__ == "__main__":
    main()
