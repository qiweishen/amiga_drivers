"""Entry point: `uv run amiga-gui` (registered in pyproject [project.scripts])."""

from __future__ import annotations

from nicegui import app, ui

from .constants import GUI_HOST, GUI_PORT, PALETTE
from .services import asterx_live, runtime, control_client, control_service
from .services.log_buffer import BUFFER
from .services.session_tailer import TAILER
from .state import STATE

# Pages register themselves via @ui.page on import.
from .ui import asterx, camera, config_editor, dashboard, live, logs, reference, sessions  # noqa: F401

_remote_session = None


@app.get("/healthz")
async def health():
    return {"ready": STATE.mode != "?", "controller_connected": STATE.controller_connected}


async def _startup() -> None:
    if control_client.enabled():
        STATE.mode = await runtime.detect_mode()
        TAILER.subscribe(BUFFER.append)
        await _remote_poll()
    else:
        await control_service.startup()


async def _remote_poll() -> None:
    global _remote_session
    if not control_client.enabled():
        return
    await control_client.poll()
    if not STATE.controller_connected:
        return
    key = (STATE.controller_id, STATE.session_generation, STATE.active_session)
    if key == _remote_session:
        return
    _remote_session = key
    TAILER.stop()
    asterx_live.LIVE.stop()
    BUFFER.clear()
    if STATE.active_session is not None:
        session = STATE.active_session
        TAILER.start(session / "raw" / f"log_{session.name}.log", replay=True, markers=False)
        asterx_live.LIVE.start(session, replay=True)


async def _shutdown() -> None:
    if control_client.enabled():
        TAILER.stop()
        asterx_live.LIVE.stop()
    else:
        await control_service.shutdown(stop_recording=False)


def main() -> None:
    app.on_startup(_startup)
    app.on_shutdown(_shutdown)
    app.timer(1.0, _remote_poll)

    app.colors(**PALETTE)
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
