"""Entry point for the single-container GUI and acquisition service."""

from __future__ import annotations

from nicegui import app, ui

from .constants import GUI_HOST, GUI_PORT, PALETTE
from .services import control_service
from .state import STATE

# Pages register themselves via @ui.page on import.
from .ui import asterx, camera, config_editor, dashboard, live, logs, reference, sessions  # noqa: F401

_ready = False


@app.get("/healthz")
async def health():
    return {"ready": _ready, "controller_connected": STATE.controller_connected}


async def _startup() -> None:
    global _ready
    _ready = False
    await control_service.startup()
    _ready = True


async def _shutdown() -> None:
    global _ready
    _ready = False
    await control_service.shutdown()


def main() -> None:
    app.on_startup(_startup)
    app.on_shutdown(_shutdown)
    app.colors(**PALETTE)
    ui.run(
        host=GUI_HOST,
        port=GUI_PORT,
        title="Amiga Sensor Console",
        reload=False,
        show=False,
        favicon="app/resource/appn.svg",
    )


if __name__ == "__main__":
    main()
