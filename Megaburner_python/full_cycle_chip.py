"""
MegaBurner full round-trip hardware test.

Runs: connect -> check chip ID -> erase -> write -> read back -> CRC32 compare.
Everything is console output only, with a progress bar per step, so you
can walk away and come back to a clear PASS/FAIL.

HOW TO USE
----------
1. Edit the CONFIGURATION block below (COM port at minimum).
2. Run:  python test_megaburner.py
3. All ROM/readback files are read/written next to this script,
   regardless of what folder you launch it from.

Only dependency: pyserial.   pip install pyserial
"""

from __future__ import annotations

import os
import sys
import time
import zlib

from megaburner import MegaBurner, CommException
from megaburner.fileio import load_rom_file, save_rom_file, generate_random_data
from megaburner.progress import ProgressBar, Spinner

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
COM_PORT = "COM7"
# Uncomment exactly ONE of the lines below (must match megaburner/chips.py)
CHIP_NAME = "MX29L3211"        # 32Mbit, 3.3V flash, page-buffer program
# CHIP_NAME = "MX29LV320ET"      # 32Mbit, 3V flash, SOP44, Top-Boot
# CHIP_NAME = "MX29LV320EB"      # 32Mbit, 3V flash, SOP44, Bottom-Boot
# CHIP_NAME = "MX26L6420"        # 64Mbit, 3V MTP EPROM, SOP44, ~100 cycles max - see README.md

# --- Test data source ---------------------------------------
# If True: generate random test data instead of using an existing ROM file.
USE_RANDOM_DATA = True

# Used only when USE_RANDOM_DATA is True. None = fill the entire chip
# capacity; or set a smaller number (bytes) for a quick partial test,
# e.g. 1 * 1024 * 1024 for a 1 MiB quick check.
RANDOM_DATA_SIZE = None

# Used only when USE_RANDOM_DATA is True. None = different random data
# every run; set an integer to get the same reproducible test pattern
# every time (handy for repeatable burn-in testing).
RANDOM_SEED = None

# Used only when USE_RANDOM_DATA is False: the ROM file to write, must
# already exist next to this script.
ROM_FILE = "test_rom.bin"

# Where the generated random test data gets saved (also next to this
# script), so you always have a copy of exactly what was written -
# whether it came from ROM_FILE or was freshly generated.
WRITTEN_FILE = "written.bin"

# Where the data read back from the chip after writing gets saved.
READBACK_FILE = "readback.bin"

# Set True to print every raw byte sent/received on the wire. Useful
# for diagnosing communication issues (e.g. an operation timing out) -
# leave False for normal use, the output is very verbose.
DEBUG = False

# Set True to abort if the chip's id doesn't match CHIP_NAME. Leave
# True unless you specifically know why the chip reports an
# unexpected id (e.g. a suspected non-genuine part) and want to
# proceed anyway - this test still ERASES AND WRITES, so overriding
# this is a real risk, not just a formality.
REQUIRE_ID_MATCH = True

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
    print(f"TEST FAILED: {message}")
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

    hr("MegaBurner hardware test")
    print(f"Script folder : {SCRIPT_DIR}")
    print(f"Port          : {'autodetect' if AUTODETECT_PORT else COM_PORT}")
    print(f"Chip          : {CHIP_NAME}")

    # ------------------------------------------------------------------
    # Step 0: list available ports (sanity check before we even try to connect)
    # ------------------------------------------------------------------
    hr("Step 0/6: Checking USB/serial ports")
    available = MegaBurner.list_ports()
    print("Available ports:", ", ".join(available) if available else "(none found)")

    port = resolve_port(available)

    mb = MegaBurner(CHIP_NAME, debug=DEBUG)

    try:
        # ------------------------------------------------------------------
        # Step 1: connect
        # ------------------------------------------------------------------
        hr("Step 1/6: Connecting")
        mb.connect(port)
        print(f"Connected to {port}.")

        # ------------------------------------------------------------------
        # Step 2: check chip ID
        # ------------------------------------------------------------------
        hr("Step 2/6: Checking chip ID")
        matched, chip_id = mb.is_chip_matched()
        print(f"Chip replied with id: {chip_id!r}")
        if not matched:
            message = (
                f"Chip id mismatch: expected '{mb.chip.id}' ({mb.chip.name}), "
                f"got {chip_id!r}. Is the chip seated correctly / is this the "
                "right CHIP_NAME?"
            )
            if REQUIRE_ID_MATCH:
                fail(message)
            print(f"WARNING: {message} Continuing anyway (REQUIRE_ID_MATCH is False).")
        else:
            print(f"OK - chip id matches {mb.chip.name} ({mb.chip.package}).")
        if mb.chip.max_cycles is not None:
            print()
            print(f"*** WARNING: {mb.chip.name} is rated for only ~{mb.chip.max_cycles} "
                  "erase/program cycles. This test run will consume ONE of them. ***")

        # ------------------------------------------------------------------
        # Step 3: erase
        # ------------------------------------------------------------------
        hr("Step 3/6: Erasing chip")
        print("(no byte-level progress is available from the firmware for erase -")
        print(" showing elapsed time only; this can take a while, don't panic)")
        spinner = Spinner("Erasing")
        mb.erase(on_tick=spinner.tick)
        spinner.finish("erase complete")

        # ------------------------------------------------------------------
        # Step 4: prepare test data
        # ------------------------------------------------------------------
        hr("Step 4/6: Preparing test data")
        if USE_RANDOM_DATA:
            size = RANDOM_DATA_SIZE if RANDOM_DATA_SIZE is not None else mb.chip.capacity
            print(f"Generating {size} random bytes"
                  + (f" (seed={RANDOM_SEED})" if RANDOM_SEED is not None else " (unseeded)")
                  + " ...")
            data = generate_random_data(size, seed=RANDOM_SEED)
        else:
            rom_path = path_in_script_dir(ROM_FILE)
            if not os.path.isfile(rom_path):
                fail(
                    f"USE_RANDOM_DATA is False but '{ROM_FILE}' was not found next "
                    f"to this script ({rom_path}). Either add the file or set "
                    "USE_RANDOM_DATA = True."
                )
            print(f"Loading '{ROM_FILE}' ...")
            data = load_rom_file(rom_path)

        if len(data) > mb.chip.capacity:
            fail(
                f"Test data is {len(data)} bytes, larger than {mb.chip.name}'s "
                f"capacity of {mb.chip.capacity} bytes."
            )

        written_path = path_in_script_dir(WRITTEN_FILE)
        save_rom_file(written_path, data)
        print(f"Test data: {len(data)} bytes. Saved a copy to '{WRITTEN_FILE}'.")

        # ------------------------------------------------------------------
        # Step 5: write
        # ------------------------------------------------------------------
        hr("Step 5/6: Writing to chip")
        bar = ProgressBar("Writing", total=len(data))
        mb.write(data, progress=bar.update)
        bar.finish()

        # ------------------------------------------------------------------
        # Step 6: read back and verify
        # ------------------------------------------------------------------
        hr("Step 6/6: Reading back and verifying (CRC32)")
        bar = ProgressBar("Reading", total=len(data))
        readback = mb.read_range(len(data), progress=bar.update)
        bar.finish()

        readback_path = path_in_script_dir(READBACK_FILE)
        save_rom_file(readback_path, readback)
        print(f"Saved readback to '{READBACK_FILE}'.")

        crc_written = zlib.crc32(data) & 0xFFFFFFFF
        crc_readback = zlib.crc32(readback) & 0xFFFFFFFF

        print()
        print(f"Written  CRC32 : {crc_written:08X}  ({len(data)} bytes, '{WRITTEN_FILE}')")
        print(f"Readback CRC32 : {crc_readback:08X}  ({len(readback)} bytes, '{READBACK_FILE}')")

        if crc_written != crc_readback:
            first_diff = next(
                (i for i in range(len(data)) if data[i] != readback[i]), None
            )
            detail = f" First differing byte at offset 0x{first_diff:08X}." if first_diff is not None else ""
            fail(f"CRC32 mismatch between written and readback data.{detail}")

    except CommException as exc:
        fail(str(exc))
    finally:
        mb.disconnect()

    elapsed = time.monotonic() - overall_start
    hr("TEST PASSED")
    print(f"Chip: {mb.chip.name}  |  Data size: {len(data)} bytes  |  CRC32: {crc_written:08X}")
    print(f"Total time: {elapsed:.1f}s")
    print(f"Files written next to this script: {WRITTEN_FILE}, {READBACK_FILE}")


if __name__ == "__main__":
    main()
