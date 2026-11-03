"""Explicit offline export entry point; importing it does not export anything."""

from pathlib import Path
import sys

if not __package__:
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from ebus_driver.scripts.ebus_raw.envi import main

if __name__ == "__main__":
    sys.exit(main())
