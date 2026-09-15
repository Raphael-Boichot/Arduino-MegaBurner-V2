"""Chip definitions.

Ported from Chips.java / chips/Chip.java / chips/MX29L3211.java.

To add a new chip, add another entry to CHIPS below - every field is
required and documented so the shape is easy to copy. Verify every
field against the chip's own datasheet first - never assume it matches
an existing entry. See README.md "Adding another chip".
"""

from dataclasses import dataclass
from typing import Optional


@dataclass(frozen=True)
class Chip:
    name: str            # host-side name - one entry per distinguishable id (e.g. per boot variant)
    firmware_id: str      # name sent to the Arduino's "S<name>" select-chip command -
                           # several `name`s can share one firmware_id (see MX29LV320E variants)
    display_name: str
    id: str                # ASCII id string the firmware's check() command returns
    package: str            # physical package, e.g. "SOP44", "TSOP48"
    capacity: int            # bytes
    bus_width: int            # data bus width this firmware drives the chip at (always 16
                               # here, by design - unifies the code path across every chip,
                               # MegaBurner_arduino.ino's read()/write() always call
                               # read16()/write16() regardless of a chip's own 8-bit
                               # capability; see README.md "Bus width")
    page_size: int             # bytes per page-program cycle
    read_block: int             # bytes per read block command
    write_block: int            # bytes per write block command
    max_cycles: Optional[int] = None  # erase/program cycle endurance limit, if the
                                       # datasheet specifies an unusually low one (e.g.
                                       # MTP EPROMs). None = not a known concern (typical
                                       # flash is rated 100,000+, effectively a non-issue
                                       # for hobby use).
    notes: str = ""             # anything else worth flagging (quirks, warnings)

    @property
    def read_block_count(self) -> int:
        """Equivalent to Chips.getReadBlockCount() -> capacity / readBlock."""
        return self.capacity // self.read_block


CHIPS = {
    "MX29L3211": Chip(
        name="MX29L3211",
        firmware_id="MX29L3211",
        display_name="   MX29L2311 (C2F9)",
        id="C2F9",
        package="unknown - not confirmed in this project",
        capacity=4 * 1024 * 1024,
        bus_width=16,  # intentional - unifies the code path across all chips (see README.md "Bus width")
        page_size=128,
        read_block=4096,
        write_block=4096,
        notes="3.3V, 32Mbit. Has a real page-buffer program feature (unlike the other "
              "two chips), hence page_size=128 instead of 2.",
    ),
    "MX29LV320ET": Chip(
        name="MX29LV320ET",
        firmware_id="MX29LV320E",
        display_name="   MX29LV320E Top-Boot (C2A7)",
        id="C2A7",
        package="SOP44",
        capacity=4 * 1024 * 1024,
        bus_width=16,  # intentional - unifies the code path across all chips (see README.md "Bus width")
        page_size=2,
        read_block=4096,
        write_block=4096,
        notes="3V, 32Mbit, Top-Boot variant. Standard single-word program only "
              "(no page buffer) - page_size=2 is load-bearing, don't change it. "
              "Confirmed against datasheet PM1575 REV 1.3.",
    ),
    "MX29LV320EB": Chip(
        name="MX29LV320EB",
        firmware_id="MX29LV320E",  # same firmware driver as Top-Boot - see MX29LV320E.h
        display_name="   MX29LV320E Bottom-Boot (C2A8)",
        id="C2A8",
        package="SOP44",
        capacity=4 * 1024 * 1024,
        bus_width=16,  # intentional - unifies the code path across all chips (see README.md "Bus width")
        page_size=2,
        read_block=4096,
        write_block=4096,
        notes="Same as MX29LV320ET except Bottom-Boot device id.",
    ),
    "MX26L6420": Chip(
        name="MX26L6420",
        firmware_id="MX26L6420",
        display_name="   MX26L6420 (C2FC)",
        id="C2FC",
        package="SOP44 (also available as TSOP48)",
        capacity=8 * 1024 * 1024,
        bus_width=16,  # only option - no BYTE# pin at all, this chip has no 8-bit mode
        page_size=2,
        read_block=4096,
        write_block=4096,
        max_cycles=100,  # MTP EPROM - far less endurance than the flash chips above
        notes="3V, 64Mbit MTP EPROM. Confirmed against datasheet P/N PM0823 REV 0.5. "
              "Single-word program only, page_size=2. Reset is a single F0 write with "
              "no unlock prefix - different from the other two chips.",
    ),
}


def get_chip(name: str = "MX29L3211") -> Chip:
    try:
        return CHIPS[name]
    except KeyError as exc:
        raise ValueError(f'Unknown chip "{name}". Known chips: {", ".join(CHIPS)}') from exc
