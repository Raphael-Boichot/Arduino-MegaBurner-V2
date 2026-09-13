# MegaBurner - Python port (command-line only)

Host-side driver for the Arduino Mega "MegaBurner" EEPROM programmer,
ported from the (hardware-confirmed working) Java `SerialHelper.java`.
The Arduino firmware itself (`MegaBurner.ino` / `MX29L3211.cpp`) is
unchanged — this only replicates the same serial protocol from Python.

No GUI. One dependency: **pyserial**.

## Setup

```
pip install -r requirements.txt
```
(or just `pip install pyserial` — it's the only dependency)

## Quick test (the main thing you asked for)

Open `test_megaburner.py`, edit the **CONFIGURATION** block at the top
(at minimum, set `COM_PORT` to your board's port), then run:

```
python full_cycle_chip.py
```

It will, in order, with a progress bar/spinner for each step:

1. List available serial ports (sanity check).
2. Connect.
3. Check the chip ID against what's expected for `CHIP_NAME`.
4. Erase the whole chip.
5. Prepare test data — either your own ROM file (`USE_RANDOM_DATA =
   False`, reads `ROM_FILE`) or freshly generated random data
   (`USE_RANDOM_DATA = True`, default). Either way it's saved to
   `WRITTEN_FILE` so you always have exactly what was written.
6. Write that data to the chip.
7. Read the same range back and save it to `READBACK_FILE`.
8. Compare CRC32 of written vs. read-back data and print a clear
   **TEST PASSED** / **TEST FAILED** verdict (and exits with code 0/1).

All three files (`ROM_FILE` if used, `WRITTEN_FILE`, `READBACK_FILE`)
live next to the script itself, regardless of your current working
directory when you run it.

Every step has a generous but finite timeout, so if the hardware stops
responding partway through, the script fails with a clear error message
instead of hanging forever — important since the whole point is to be
able to walk away and come back later.

## Project layout

| File                     | Purpose                                              |
|---------------------------|-------------------------------------------------------|
| `megaburner/driver.py`     | `MegaBurner` class — connect/check/erase/write/read_range/verify. Protocol logic ported 1:1 from `SerialHelper.java`. |
| `megaburner/chips.py`      | Chip definition table (id, capacity, page/block sizes, bus width, cycle limits). |
| `megaburner/fileio.py`     | Load/save ROM files, random test-data generator.       |
| `megaburner/progress.py`   | Plain stdlib console progress bar + spinner (no extra deps). |
| `megaburner/exceptions.py` | `CommException`.                                       |
| `full_cycle_chip.py`       | Full round-trip test: check → erase → write → read back → CRC32. |
| `dump_chip.py`             | Read-only: check → read → save. No erase, no write — safe for a first look at an unfamiliar chip, or just backing up a cart. |

## Using the driver directly (for your own scripts later)

```python
from megaburner import MegaBurner

mb = MegaBurner("MX29L3211")
mb.connect("COM5")
print(mb.check())              # e.g. 'C2F9'
mb.erase()
mb.write(some_bytes)
readback = mb.read_range(len(some_bytes))
mb.disconnect()
```

`MegaBurner` is also a context manager: `with MegaBurner("MX29L3211") as mb: ...` closes the port automatically.

## Supported chips

Chip selection is **runtime**, not compile-time: the Arduino firmware
has all supported chip drivers built in and switches between them via
a serial command. No reflashing needed when you swap chips - just set
`CHIP_NAME` in your script and reconnect (or call `mb.select_chip()`
again if you swap the physical chip without reconnecting).

| Chip | Host `CHIP_NAME` | Firmware id sent over serial | Notes |
|------|-------------------|-------------------------------|-------|
| MX29L3211 | `"MX29L3211"` | `MX29L3211` | Has a real page-buffer program feature; `page_size=128`. Default chip if nothing is selected. |
| MX29LV320E Top-Boot | `"MX29LV320ET"` | `MX29LV320E` | Standard single-word program only; `page_size=2` is load-bearing, don't change it. |
| MX29LV320E Bottom-Boot | `"MX29LV320EB"` | `MX29LV320E` (same driver as Top-Boot) | Only the expected id differs from Top-Boot. |
| MX26L6420 | `"MX26L6420"` | `MX26L6420` | 16-bit only (no byte mode). **MTP EPROM rated for only ~100 erase/program cycles** - far less margin than the flash chips above. `page_size=2`, same reasoning as MX29LV320E. |

`connect()` automatically sends the select-chip command for whatever
`CHIP_NAME` you constructed `MegaBurner(...)` with - you don't need to
call anything extra for the common case. See `MX26L6420.h`/`MX29LV320E.h`
for the specific protocol differences between chips and why each one
was necessary (they are NOT command-compatible with each other -
different unlock addresses in one case, different reset sequence in
another).

### Bus width

Every `Chip` entry has a `bus_width` field, and it's `16` for all
three chips - intentionally, not a placeholder. `MX26L6420` has no
8-bit mode at all (no BYTE# pin), and for `MX29L3211`/`MX29LV320E`,
driving them in 16-bit mode is a deliberate choice to keep one unified
code path across every supported chip: `MegaBurner_arduino.ino`'s
`read()`/`write()` always call `read16()`/`write16()`, never
`read8()`/`write8()` (which exist in the chip driver classes but are
genuinely dead code, never called from anywhere). This matches what
real-hardware CRC32 round-trip tests confirmed working for all chips
tested so far, and what the original, unmodified Java app already did
for MX29L3211.

## Adding another chip

1. Get the chip's actual command-set table from its datasheet — don't
   assume it matches any existing chip. Confirm at minimum: unlock
   addresses, whether it has a page-buffer program feature or only
   single-word program, how busy/done status is polled (fixed address
   vs. the address being written; DQ7 alone vs. DQ7+DQ6 together),
   and the exact reset sequence (not all chips need a 3-cycle unlock+F0).
2. Add a chip driver (`.h`/`.ino` pair) on the Arduino side implementing
   whatever that chip's sequences actually are, inheriting from
   `FlashChip` (see `FlashChip.h`) — `MX26L6420.h`/`.ino` are the
   template to copy and adjust.
3. In `MegaBurner_arduino.ino`: `#include` the new header, instantiate
   the chip object, and add an `else if` branch to `selectChip()`.
4. Add a `Chip(...)` entry to `megaburner/chips.py` — every field is
   required (see the dataclass docstring in that file): `id`,
   `capacity`, `bus_width`, `page_size` (must match what the chip
   driver actually implements, not be picked independently),
   `firmware_id` matching the string you added to `selectChip()`, plus
   `package`/`max_cycles`/`notes` for anything worth documenting (an
   unusual cycle limit, a quirk, etc.).

## Protocol reference (unchanged from the Java app)

| Host sends            | Meaning                                                | Device replies |
|-------------------------|-----------------------------------------------------------|-----------------|
| `C`                    | Check chip ID                                              | 4 ASCII hex chars, e.g. `C2F9` |
| `R<block>,<size>`       | Read one block                                              | `<size>` raw bytes |
| `E`                    | Erase whole chip                                            | `%` when done (can take a while) |
| `W<offset>,<page>,<n>`  | Write `n` bytes at `offset`                                 | `&` when ready, then `n` raw bytes are sent, then `%` when done |
| `S<name>`              | Select active chip driver (e.g. `SMX26L6420`)               | `%` once that chip's init() (id-read + reset) completes |
