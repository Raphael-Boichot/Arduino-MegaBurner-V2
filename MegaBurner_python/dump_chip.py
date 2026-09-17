"""
MegaBurner - check chip ID, read the whole chip, save to a file.

No erase, no write - this only ever reads. Console output only, with a
progress bar for the read step. Same structure/conventions as
test_megaburner.py (config block at top, files saved next to this
script regardless of current working directory).

HOW TO USE
----------
1. Edit the CONFIGURATION block below (COM port + chip name at minimum).
2. Run:  python dump_chip.py
3. The dump is saved next to this script.

Only dependency: pyserial.   pip install pyserial
"""

from __future__ import annotations

import os
import sys
import time
import zlib

from megaburner import MegaBurner, CommException
from megaburner.fileio import save_rom_file
from megaburner.progress import ProgressBar

# ============================================================
# CONFIGURATION - edit these, then just run the script.
# ============================================================

# Leave AUTODETECT_PORT = True to find the Arduino automatically: the
# script pings every available serial port and picks the one that
# identifies itself as a MegaBurner. Set it to False to always use
# COM_PORT below instead (useful if you have several MegaBurners
# connected, or just want a fixed, predictable port).
AUTODETECT_PORT = True

# Used when AUTODETECT_PORT is False, or as a fallback if autodetection
# finds nothing. e.g. "COM5" on Windows, "/dev/ttyACM0" on Linux.
COM_PORT = "COM6"
# Uncomment exactly ONE of the lines below (must match megaburner/chips.py)
CHIP_NAME = "MX29L3211"        # 32Mbit, 3.3V flash, page-buffer program
# CHIP_NAME = "MX29LV320ET"      # 32Mbit, 3V flash, SOP44, Top-Boot
# CHIP_NAME = "MX29LV320EB"      # 32Mbit, 3V flash, SOP44, Bottom-Boot
# CHIP_NAME = "MX26L6420"        # 64Mbit, 3V MTP EPROM, SOP44, ~100 cycles max - see README.md

# How much to read, starting at offset 0. None = the chip's full
# capacity; or set a smaller number (bytes) to only dump part of it,
# e.g. 1 * 1024 * 1024 for a quick 1 MiB read.
READ_SIZE = None

# Where the dump gets saved (next to this script).
OUTPUT_FILE = "dump.bin"

# Set True to abort if the chip's id doesn't match CHIP_NAME. Leave
# True unless you specifically want to read from an unrecognized/
# unexpected chip anyway.
REQUIRE_ID_MATCH = True

# Set True to print every raw byte sent/received on the wire. Useful
# for diagnosing communication issues - leave False for normal use,
# the output is very verbose.
DEBUG = False

# ============================================================
# End of configuration - no need to edit below this line.
# ============================================================

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))


def path_in_script_dir(filename: str) -> str:
    return os.path.join(SCRIPT_DIR, filename)


def hr(title: str) -> None:
    print()
    print("=" * 60)
    print(title)
    print("=" * 60)


def fail(message: str) -> None:
    print()
    print("!" * 60)
    print(f"FAILED: {message}")
    print("!" * 60)
    sys.exit(1)


def resolve_port(available: list) -> str:
    """Decide which serial port to use, honouring AUTODETECT_PORT."""
    if AUTODETECT_PORT:
        print("Autodetecting the MegaBurner (pinging each port)...")

        def show_probe(p: str) -> None:
            print(f"  probing {p} ...")

        found = MegaBurner.autodetect_port(debug=DEBUG, on_probe=show_probe)
        if found:
            print(f"Found MegaBurner on {found}.")
            return found
        print(
            f"No MegaBurner answered the ping. Falling back to COM_PORT ({COM_PORT}).\n"
            "If that fails, check the board is plugged in and running firmware that\n"
            "supports the ping command, or set AUTODETECT_PORT = False and pick the\n"
            "port manually."
        )
        return COM_PORT

    if COM_PORT not in available:
        print(
            f"WARNING: {COM_PORT} was not in the list above. It may still work "
            "(some ports don't always enumerate), but double-check the port name "
            "if the next step fails."
        )
    return COM_PORT


def main() -> None:
    overall_start = time.monotonic()

    hr("MegaBurner - check ID, read, save")
    print(f"Script folder : {SCRIPT_DIR}")
    print(f"Port          : {'autodetect' if AUTODETECT_PORT else COM_PORT}")
    print(f"Chip          : {CHIP_NAME}")

    # ------------------------------------------------------------------
    # Step 0: find the board
    # ------------------------------------------------------------------
    hr("Step 0/3: Checking USB/serial ports")
    available = MegaBurner.list_ports()
    print("Available ports:", ", ".join(available) if available else "(none found)")

    port = resolve_port(available)

    mb = MegaBurner(CHIP_NAME, debug=DEBUG)

    try:
        # ------------------------------------------------------------------
        # Step 1: connect (this also selects CHIP_NAME on the firmware)
        # ------------------------------------------------------------------
        hr("Step 1/3: Connecting")
        mb.connect(port)
        print(f"Connected to {port}.")

        # ------------------------------------------------------------------
        # Step 2: check chip ID
        # ------------------------------------------------------------------
        hr("Step 2/3: Checking chip ID")
        matched, chip_id = mb.is_chip_matched()
        print(f"Chip replied with id: {chip_id!r}")
        if matched:
            print(f"OK - chip id matches {mb.chip.name} ({mb.chip.package}).")
        else:
            message = (
                f"Chip id mismatch: expected '{mb.chip.id}' ({mb.chip.name}), "
                f"got {chip_id!r}."
            )
            if REQUIRE_ID_MATCH:
                fail(message)
            print(f"WARNING: {message} Continuing anyway (REQUIRE_ID_MATCH is False).")

        # ------------------------------------------------------------------
        # Step 3: read and save
        # ------------------------------------------------------------------
        hr("Step 3/3: Reading chip")
        size = READ_SIZE if READ_SIZE is not None else mb.chip.capacity
        if size > mb.chip.capacity:
            fail(
                f"READ_SIZE ({size} bytes) is larger than {mb.chip.name}'s "
                f"capacity of {mb.chip.capacity} bytes."
            )

        bar = ProgressBar("Reading", total=size)
        dump = mb.read_range(size, progress=bar.update)
        bar.finish()

        output_path = path_in_script_dir(OUTPUT_FILE)
        save_rom_file(output_path, dump)

        crc = zlib.crc32(dump) & 0xFFFFFFFF
        print()
        print(f"Saved {len(dump)} bytes to '{OUTPUT_FILE}'.")
        print(f"CRC32: {crc:08X}")

    except CommException as exc:
        fail(str(exc))
    finally:
        mb.disconnect()

    elapsed = time.monotonic() - overall_start
    hr("DONE")
    print(f"Chip: {mb.chip.name}  |  Read: {len(dump)} bytes  |  CRC32: {crc:08X}")
    print(f"Total time: {elapsed:.1f}s")
    print(f"File written next to this script: {OUTPUT_FILE}")


if __name__ == "__main__":
    main()
