"""Entry point for the single-container GUI and acquisition service."""

from __future__ import annotations

import asyncio

from fastapi import HTTPException, Response
from nicegui import app, ui

from .constants import GUI_HOST, GUI_PORT, PALETTE
from .services import control_service, preview_images
from .state import STATE
# Pages register themselves via @ui.page on import.
from .ui import asterx, camera, config_editor, dashboard, live, logs, reference, sessions  # noqa: F401

_ready = False


@app.get("/healthz")
async def health():
    return {"ready": _ready, "controller_connected": STATE.controller_connected}


@app.get(preview_images.ROUTE + "/{key}")
async def preview_image(key: str, download: bool = False) -> Response:
    """Binary preview bytes; keys are unique per image, so responses never change."""
    try:
        found = await asyncio.to_thread(preview_images.fetch, key)
    except Exception as error:  # a full-resolution producer re-reads the saved RAW
        raise HTTPException(status_code=500, detail=f"Preview image could not be produced: {error}") from error
    if found is None:
        raise HTTPException(status_code=404, detail="Preview image expired; capture or open it again")
    data, media_type, filename = found
    disposition = "attachment" if download else "inline"
    return Response(content=data, media_type=media_type, headers={
        "Cache-Control": "private, max-age=86400, immutable",
        "Content-Disposition": f'{disposition}; filename="{filename}"',
    })


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
