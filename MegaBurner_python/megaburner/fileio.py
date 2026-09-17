"""File I/O helpers. Equivalent to FileHelper.java, plus a random test
data generator that the Java app didn't need (it always worked from a
real ROM file)."""

from __future__ import annotations

import os


def load_rom_file(path: str) -> bytes:
    with open(path, "rb") as f:
        return f.read()


def save_rom_file(path: str, data: bytes) -> None:
    with open(path, "wb") as f:
        f.write(data)


def generate_random_data(size: int, seed: int | None = None) -> bytes:
    """Generate `size` bytes of pseudo-random test data.

    Uses Python's os.urandom by default (seed=None) for real randomness
    each run. Pass an int seed for a reproducible pattern instead (uses
    Python's random module in that case, not cryptographically random,
    but reproducible - fine for a burn-in test pattern).
    """
    if seed is None:
        return os.urandom(size)

    import random

    rng = random.Random(seed)
    if hasattr(rng, "randbytes"):  # Python 3.9+
        return rng.randbytes(size)
    return bytes(rng.getrandbits(8) for _ in range(size))
