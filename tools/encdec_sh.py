#!/usr/bin/env python3
"""Byte-compatible port of the supplied EncDecSH.cpp.

The same operation encodes and decodes; it does not authenticate a script.
"""
import argparse
from pathlib import Path

INITIAL_SEED = 0x001BE3AC
MASK32 = 0xFFFFFFFF


def transform(data: bytes) -> bytes:
    seed = INITIAL_SEED

    def prng_rand() -> int:
        nonlocal seed
        previous = seed
        r1 = ((seed >> 1) | (seed << 31)) & MASK32
        r3 = (((r1 >> 16) & 0xFF) + r1) & MASK32
        r1 = ((r3 >> 8) & 0xFF) << 16
        seed = (r3 - r1) & MASK32
        return previous

    prng_rand()  # The C++ original discards its first return value.
    return bytes(value ^ (prng_rand() & 0xFF) for value in data)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    if args.input.resolve() == args.output.resolve():
        parser.error("Input and output must be different files.")
    try:
        args.output.write_bytes(transform(args.input.read_bytes()))
    except OSError as exc:
        parser.exit(1, f"Error: {exc}\n")


if __name__ == "__main__":
    main()
