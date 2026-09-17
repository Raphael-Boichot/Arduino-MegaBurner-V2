"""Host-side driver for the Arduino Mega "MegaBurner" EEPROM programmer.

Ported from SerialHelper.java (the confirmed-working, hardware-tested
version, incl. the flushInput()/zero-length-read guard). The Arduino
firmware (MegaBurner.ino / MX29L3211.cpp) is unchanged by this port -
this only replicates the host <-> device serial protocol:

    'C'                      -> check chip id
    'R<block>,<blockSize>'   -> read one block (raw bytes back)
    'E'                      -> erase whole chip (replies '%')
    'W<offset>,<page>,<n>'   -> write n bytes at offset
                                (device replies '&' when ready to
                                receive, then '%' when done)

Only one dependency: pyserial (`pip install pyserial`).
"""

from __future__ import annotations

import time
from dataclasses import dataclass
from typing import Callable, Optional

import serial
import serial.tools.list_ports

from .chips import Chip, get_chip
from .exceptions import CommException

ProgressFn = Callable[[int, int], None]  # (bytes_done, bytes_total) -> None


@dataclass
class Timeouts:
    """All timeouts in seconds. Generous by design - this driver is
    meant to run unattended (see test_megaburner.py), so every
    operation fails LOUDLY with a clear error rather than hanging
    forever if the hardware doesn't respond."""
    connect_settle: float = 4.0      # wait after opening the port for the Mega to reboot -
                                      # must exceed the firmware's full boot sequence (LED
                                      # flash + startup chip-ID read + reset settle, ~1.6s)
    block_read: float = 10.0         # per serial.read() call while pulling a read/verify block
    check_id: float = 5.0            # waiting for the 4 id bytes
    erase_signal: float = 660.0      # waiting for '%'/'!' after erase - must stay above the
                                      # Arduino-side ERASE_TIMEOUT_MS (600s) so the firmware's
                                      # clearer failure report has a chance to arrive first
    write_signal: float = 20.0       # waiting for '&' or '%' around each write block
    select_signal: float = 10.0      # waiting for '%' after selecting a chip (runs that
                                      # chip's init(), which includes an id-read + reset)
    ping_reply: float = 1.0          # waiting for the "megaburner" reply while probing one
                                      # port during autodetect - kept short so scanning
                                      # several ports stays quick


class MegaBurner:
    """Driver for one chip type. Create one instance per session."""

    BAUD_RATE = 115200
    CHECK_COMMAND = b"C"
    READ_COMMAND = "R"
    ERASE_COMMAND = b"E"
    WRITE_COMMAND = "W"
    SELECT_COMMAND = "S"
    PING_COMMAND = b"P"
    PING_REPLY = "megaburner"  # what the firmware answers to PING_COMMAND
    SIGNAL_BEGIN = ord("&")
    SIGNAL_END = ord("%")
    SIGNAL_FAIL = ord("!")  # chip reported "never completed" (see busyCheck() timeout
                              # in the Arduino firmware) - distinct from SIGNAL_END so a
                              # real failure can't be mistaken for success

    def __init__(
        self,
        chip_name: str = "MX29L3211",
        timeouts: Optional[Timeouts] = None,
        debug: bool = False,
    ):
        self.chip: Chip = get_chip(chip_name)
        self.timeouts = timeouts or Timeouts()
        self.debug = debug
        self._ser: Optional[serial.Serial] = None

    # ------------------------------------------------------------------
    # Connection management
    # ------------------------------------------------------------------

    @staticmethod
    def list_ports() -> list[str]:
        """Equivalent to SerialHelper.getAvailableSerialPorts()."""
        return [p.device for p in serial.tools.list_ports.comports()]

    @classmethod
    def ping_port(cls, port: str, timeouts: Optional["Timeouts"] = None,
                  debug: bool = False) -> bool:
        """Return True if `port` answers the ping command like a MegaBurner.

        Opens the port on its own (nothing else may hold it at the same
        time), sends PING_COMMAND, and looks for PING_REPLY in the
        answer. Any failure - port busy, permission denied, no reply,
        wrong reply, garbage - is treated as "not a MegaBurner" and
        returns False rather than raising, because this is used to probe
        ports that may well belong to completely unrelated devices.

        Note the ping is deliberately a standalone command that never
        touches the chip, so probing an unknown device is harmless in
        both directions.
        """
        t = timeouts or Timeouts()
        try:
            with serial.Serial(
                port=port,
                baudrate=cls.BAUD_RATE,
                bytesize=serial.EIGHTBITS,
                parity=serial.PARITY_NONE,
                stopbits=serial.STOPBITS_ONE,
                timeout=0.3,
            ) as ser:
                # The Mega resets when the port opens - it can't answer
                # anything until its boot sequence finishes. Boot timing
                # has some real variance (see connect()'s own
                # _drain_until_quiet() for the same issue) - a single
                # fixed sleep + one-shot reset_input_buffer() + a single
                # readline() call is fragile against that variance and
                # can easily race with the firmware's own unsolicited
                # startup chip-ID output. So instead of one clean read,
                # accumulate everything received over the whole
                # ping_reply window and just look for PING_REPLY
                # anywhere in it - robust to boot noise, partial reads,
                # or the reply arriving in more than one chunk.
                time.sleep(t.connect_settle)
                ser.reset_input_buffer()

                ser.write(cls.PING_COMMAND)
                ser.flush()

                buf = bytearray()
                target = cls.PING_REPLY.encode("ascii")
                deadline = time.monotonic() + t.ping_reply
                while time.monotonic() < deadline:
                    chunk = ser.read(64)
                    if chunk:
                        buf += chunk
                        if target in buf:
                            break

                text = buf.decode("ascii", errors="replace")
                if debug:
                    print(f"[DEBUG ping] {port}: {text!r}")
                return cls.PING_REPLY in text
        except Exception as exc:  # noqa: BLE001 - see docstring
            if debug:
                print(f"[DEBUG ping] {port}: skipped ({exc})")
            return False

    @classmethod
    def autodetect_port(cls, timeouts: Optional["Timeouts"] = None,
                        debug: bool = False,
                        on_probe: Optional[Callable[[str], None]] = None) -> Optional[str]:
        """Scan every available serial port and return the first one that
        answers the ping, or None if no MegaBurner was found.

        `on_probe`, if given, is called with each port name just before
        it is probed - useful for printing progress, since each port
        takes a couple of seconds to test.
        """
        for port in cls.list_ports():
            if on_probe:
                on_probe(port)
            if cls.ping_port(port, timeouts=timeouts, debug=debug):
                return port
        return None

    @property
    def connected(self) -> bool:
        return self._ser is not None and self._ser.is_open

    def connect(self, port: str) -> None:
        """Open the serial port and wait for the Arduino to reset/boot."""
        if self.connected:
            self.disconnect()

        try:
            self._ser = serial.Serial(
                port=port,
                baudrate=self.BAUD_RATE,
                bytesize=serial.EIGHTBITS,
                parity=serial.PARITY_NONE,
                stopbits=serial.STOPBITS_ONE,
                timeout=self.timeouts.block_read,
            )
        except serial.SerialException as exc:
            raise CommException(f'Could not open port "{port}": {exc}') from exc

        # The Mega resets when the serial port opens. Wait out the
        # firmware's boot sequence, then actively drain anything still
        # trickling in (the startup chip-ID read in MX29L3211::init()
        # is unsolicited and can arrive late) so no stray boot-time
        # bytes contaminate the first real command.
        time.sleep(self.timeouts.connect_settle)
        self._drain_until_quiet()

        # Tell the firmware which physical chip is connected. The
        # Arduino has all supported chip drivers built in and switches
        # between them at runtime - no reflash needed. This runs that
        # chip's init() on the device (id-read + reset), so it's worth
        # doing even for the default chip (MX29L3211) to make sure the
        # driver is talking to a freshly-reset chip.
        self.select_chip()

    def select_chip(self) -> None:
        """Tell the firmware to switch to this driver's chip (self.chip).

        Normally you don't need to call this yourself - connect() does
        it automatically. Call it again directly if you physically
        swap the chip in the socket without reconnecting the port.
        """
        self._assert_connected()
        self._flush_input()

        self._write_cmd(f"{self.SELECT_COMMAND}{self.chip.firmware_id}")
        self._wait_for_signal(self.SIGNAL_END, self.timeouts.select_signal)

    def _drain_until_quiet(self, quiet_period: float = 0.3, max_total: float = 3.0) -> None:
        """Keep discarding incoming bytes until the line has been
        silent for `quiet_period` seconds (bounded by `max_total`).
        More robust than a single flush() when boot-time output can
        arrive late/jittery.
        """
        assert self._ser is not None
        old_timeout = self._ser.timeout
        self._ser.timeout = quiet_period
        deadline = time.monotonic() + max_total
        try:
            while time.monotonic() < deadline:
                b = self._ser.read(1)
                if not b:
                    return  # quiet_period elapsed with nothing arriving
                if self.debug:
                    print(f"[DEBUG RX startup-drain] {b!r}")
        finally:
            self._ser.timeout = old_timeout

    def disconnect(self) -> None:
        if self._ser is not None:
            try:
                self._ser.close()
            finally:
                self._ser = None

    def __enter__(self) -> "MegaBurner":
        return self

    def __exit__(self, exc_type, exc, tb) -> None:
        self.disconnect()

    # ------------------------------------------------------------------
    # Internal helpers
    # ------------------------------------------------------------------

    def _assert_connected(self) -> None:
        if not self.connected:
            raise CommException("Not connected. Call connect(port) first.")

    def _flush_input(self) -> None:
        """Discard bytes already sitting in the input buffer.

        Mirrors the fixed Java flushInput(): only touches the buffer
        when something is actually there (the original bug that caused
        a native crash on the Java side was calling skip(0); pyserial
        doesn't share that bug, but this keeps behaviour symmetric with
        the validated Java implementation).
        """
        assert self._ser is not None
        if self._ser.in_waiting > 0:
            self._ser.reset_input_buffer()

    def _write_cmd(self, text: str) -> None:
        assert self._ser is not None
        if self.debug:
            print(f"[DEBUG TX] {text!r}")
        self._ser.write(text.encode("ascii"))
        self._ser.flush()

    def _read_exact(self, n: int, timeout: float) -> bytes:
        """Read exactly n bytes (bulk), bounded by `timeout` seconds total.

        Returns whatever was received even if short (mirrors the Java
        SerialHelper's pre-sized buffer, which is left zero-padded on a
        timeout) - callers check the returned length themselves.
        """
        assert self._ser is not None
        old_timeout = self._ser.timeout
        self._ser.timeout = timeout
        try:
            data = self._ser.read(n)
            if self.debug:
                print(f"[DEBUG RX] requested {n}, got {len(data)}: {data[:64]!r}"
                      + (" ..." if len(data) > 64 else ""))
            return data
        finally:
            self._ser.timeout = old_timeout

    def _wait_for_signal(
        self,
        sig: int,
        max_wait: float,
        on_tick: Optional[Callable[[], None]] = None,
        fail_sig: Optional[int] = None,
    ) -> None:
        """Block byte-by-byte until `sig` is received, discarding
        anything else (matches SerialHelper.wait4Signal(), which also
        silently skips stray CR/LF bytes after '&'/'%'). Unlike the
        Java version, this has a firm ceiling so an unattended test run
        fails cleanly instead of hanging forever.

        `on_tick`, if given, is called roughly every 0.5s while waiting
        - lets a caller drive a spinner during long blocking waits
        (e.g. erase) without needing threads.

        `fail_sig`, if given, is a second byte value that - if seen -
        raises immediately with a clear "the device reported failure"
        message, instead of being treated as a stray/ignorable byte.
        Used for SIGNAL_FAIL, which the firmware sends when its own
        internal busyCheck() timeout fires (see erase()/write() in
        this class) - this is a real, reported failure, not noise to
        skip past.
        """
        assert self._ser is not None
        deadline = time.monotonic() + max_wait
        old_timeout = self._ser.timeout
        self._ser.timeout = 0.5
        stray = bytearray()
        try:
            while True:
                b = self._ser.read(1)
                if b:
                    if self.debug:
                        print(f"[DEBUG RX] {b!r} (0x{b[0]:02X})")
                    if b[0] == sig:
                        return
                    if fail_sig is not None and b[0] == fail_sig:
                        raise CommException(
                            "Device reported that the operation failed (its internal "
                            "busy-check timed out - the chip never reported completion). "
                            "Data may be incomplete or corrupted at this point."
                        )
                    stray += b
                if on_tick:
                    on_tick()
                if time.monotonic() > deadline:
                    stray_info = (
                        f" Received {len(stray)} unexpected byte(s) while waiting"
                        f" (first 64: {bytes(stray[:64])!r})."
                        if stray
                        else " Received nothing at all while waiting - the command may "
                        "never have reached the device, or the device never responded."
                    )
                    raise CommException(
                        f"Timed out after {max_wait:.0f}s waiting for signal "
                        f"'{chr(sig)}' from the device.{stray_info}"
                    )
        finally:
            self._ser.timeout = old_timeout

    # ------------------------------------------------------------------
    # Operations
    # ------------------------------------------------------------------

    def check(self) -> str:
        """Send the check command, return the chip id string.

        The firmware answers with the id as ASCII hex characters (e.g.
        'C2F9'), NOT raw bytes.
        """
        self._assert_connected()
        self._flush_input()

        self._write_cmd_bytes(self.CHECK_COMMAND)
        raw = self._read_exact(4, self.timeouts.check_id)
        if len(raw) < 4:
            raise CommException(
                f"Only received {len(raw)} of 4 id bytes (timeout). "
                "Check the board is connected and the firmware is running."
            )
        return raw.decode("ascii", errors="replace")

    def is_chip_matched(self) -> tuple[bool, str]:
        chip_id = self.check()
        return chip_id.strip("\x00") == self.chip.id, chip_id

    def erase(self, on_tick: Optional[Callable[[], None]] = None) -> None:
        """Erase the whole chip. Blocks until the device replies '%'.

        `on_tick`, if given, is called roughly every 0.5s while erasing
        - pass a Spinner.tick for console feedback (see progress.py),
        since the firmware gives no byte-level erase progress.
        """
        self._assert_connected()
        self._flush_input()

        self._write_cmd_bytes(self.ERASE_COMMAND)
        self._wait_for_signal(
            self.SIGNAL_END, self.timeouts.erase_signal, on_tick=on_tick, fail_sig=self.SIGNAL_FAIL
        )

    def write(self, data: bytes, progress: Optional[ProgressFn] = None) -> None:
        """Write `data` to the chip starting at offset 0."""
        self._assert_connected()
        self._flush_input()

        c = self.chip
        total = len(data)
        i = 0
        while i < total:
            write_count = min(c.write_block, total - i)

            self._write_cmd(f"{self.WRITE_COMMAND}{i},{c.page_size},{write_count}")
            self._wait_for_signal(self.SIGNAL_BEGIN, self.timeouts.write_signal)

            assert self._ser is not None
            t0 = time.monotonic()
            n_sent = self._ser.write(data[i : i + write_count])
            self._ser.flush()
            if self.debug:
                print(f"[DEBUG TX] sent {n_sent}/{write_count} data bytes for block at "
                      f"offset {i} in {time.monotonic() - t0:.3f}s")

            self._wait_for_signal(self.SIGNAL_END, self.timeouts.write_signal, fail_sig=self.SIGNAL_FAIL)

            i += write_count
            if progress:
                progress(i, total)

    def read_range(self, length: int, progress: Optional[ProgressFn] = None) -> bytes:
        """Read back `length` bytes starting at offset 0, in
        chip.read_block-sized chunks. Unlike verify() (see below) this
        always reads the full requested length and never stops early -
        useful for producing a complete readback file to inspect later,
        even if some blocks turn out to mismatch.
        """
        self._assert_connected()
        self._flush_input()

        c = self.chip
        out = bytearray(length)
        offset = 0
        block = 0
        while offset < length:
            read_size = min(c.read_block, length - offset)

            self._write_cmd(f"{self.READ_COMMAND}{block},{read_size}")
            chunk = self._read_exact(read_size, self.timeouts.block_read)
            if len(chunk) < read_size:
                raise CommException(
                    f"Connection reset: read only {len(chunk)} of {read_size} "
                    f"bytes for block {block}."
                )
            out[offset : offset + read_size] = chunk

            offset += read_size
            block += 1
            if progress:
                progress(offset, length)

        return bytes(out)

    def read_chip(self, progress: Optional[ProgressFn] = None) -> bytes:
        """Read the entire chip capacity. Equivalent to SerialHelper.read()."""
        return self.read_range(self.chip.capacity, progress=progress)

    def verify(self, ref_data: bytes, progress: Optional[ProgressFn] = None) -> bytes:
        """Read blocks back and compare against ref_data, stopping at
        the first mismatching block (matches SerialHelper.verify()).
        For a full round-trip readback + CRC32 check instead, use
        read_range()/read_chip() - see test_megaburner.py.
        """
        self._assert_connected()
        self._flush_input()

        c = self.chip
        total = len(ref_data)
        out = bytearray(total)
        block = 0

        while block * c.read_block < total:
            begin = block * c.read_block
            end = begin + c.read_block
            read_size = c.read_block if total >= end else total - begin

            self._write_cmd(f"{self.READ_COMMAND}{block},{read_size}")

            if read_size > 0:
                chunk = self._read_exact(read_size, self.timeouts.block_read)
                out[begin : begin + read_size] = chunk

                mismatch = chunk != ref_data[begin : begin + read_size]
                if mismatch:
                    raise CommException(
                        f"Address {begin:08X} to {end - 1:08X} compared different, "
                        "verify failed!"
                    )
                if progress:
                    progress(begin + read_size, total)

            block += 1

        return bytes(out)

    # small helper so CHECK/ERASE (single-byte b"C"/b"E" constants) and
    # WRITE/READ (built as ascii strings) both funnel through one place
    def _write_cmd_bytes(self, data: bytes) -> None:
        assert self._ser is not None
        if self.debug:
            print(f"[DEBUG TX] {data!r}")
        self._ser.write(data)
        self._ser.flush()
