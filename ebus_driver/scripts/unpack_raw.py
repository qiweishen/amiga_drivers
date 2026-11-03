"""Offline array/image export for eBUS raw and legacy jai-raw-seg recordings."""

from pathlib import Path
import sys

if not __package__:
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from gox_driver.scripts.unpack_raw import main

if __name__ == "__main__":
    sys.exit(main())
