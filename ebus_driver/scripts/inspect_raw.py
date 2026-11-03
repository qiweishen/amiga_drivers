"""Offline eBUS raw inspector; importing it does not run a command."""

from pathlib import Path
import sys

if not __package__:
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from ebus_driver.scripts.ebus_raw.inspection import main

if __name__ == "__main__":
    sys.exit(main())
