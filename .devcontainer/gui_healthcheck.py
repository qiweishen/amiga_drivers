"""Probe GUI readiness using only the standard library, without importing the app."""

import json
import os
import urllib.request


def main() -> None:
    host = os.environ.get("AMIGA_GUI_HOST", "0.0.0.0")
    port = os.environ.get("AMIGA_GUI_PORT", "8619")
    # Probe wildcard listeners through loopback; preserve explicit bind addresses.
    host = {"0.0.0.0": "127.0.0.1", "::": "::1"}.get(host, host)
    if ":" in host:
        host = "[" + host + "]"
    url = f"http://{host}:{port}/healthz"
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    with opener.open(url, timeout=2) as response:
        status = json.loads(response.read(4096))
    if not isinstance(status, dict) or status.get("ready") is not True:
        raise SystemExit("GUI is not ready")


if __name__ == "__main__":
    main()
