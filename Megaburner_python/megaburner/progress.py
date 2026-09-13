"""Minimal console progress bar. Pure stdlib - no tqdm/rich dependency."""

from __future__ import annotations

import sys
import time


class ProgressBar:
    """Determinate progress bar: pass a byte count as it grows.

    Usage:
        bar = ProgressBar("Writing", total=len(data))
        driver.write(data, progress=bar.update)
        bar.finish()
    """

    def __init__(self, label: str, total: int, width: int = 40):
        self.label = label
        self.total = max(total, 1)
        self.width = width
        self._last_len = 0
        self._start = time.monotonic()

    def update(self, done: int, total: int | None = None) -> None:
        if total:
            self.total = max(total, 1)
        frac = min(done / self.total, 1.0)
        filled = int(self.width * frac)
        bar = "#" * filled + "-" * (self.width - filled)
        elapsed = time.monotonic() - self._start
        line = f"\r{self.label}: [{bar}] {frac * 100:5.1f}%  {done}/{self.total} bytes  ({elapsed:5.1f}s)"
        self._last_len = len(line)
        sys.stdout.write(line)
        sys.stdout.flush()

    def finish(self) -> None:
        self.update(self.total, self.total)
        sys.stdout.write("\n")
        sys.stdout.flush()


class Spinner:
    """Indeterminate progress indicator for operations with no
    byte-level feedback from the firmware (erase is a single blocking
    command - the device only replies once, at the very end)."""

    FRAMES = "|/-\\"

    def __init__(self, label: str):
        self.label = label
        self._start = time.monotonic()
        self._i = 0

    def tick(self) -> None:
        elapsed = time.monotonic() - self._start
        frame = self.FRAMES[self._i % len(self.FRAMES)]
        self._i += 1
        sys.stdout.write(f"\r{self.label}... {frame}  ({elapsed:5.1f}s elapsed)")
        sys.stdout.flush()

    def finish(self, message: str = "done") -> None:
        elapsed = time.monotonic() - self._start
        sys.stdout.write(f"\r{self.label}... {message}  ({elapsed:5.1f}s elapsed)" + " " * 10 + "\n")
        sys.stdout.flush()
