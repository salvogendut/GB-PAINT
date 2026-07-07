#!/usr/bin/env python3
"""Convert Screen-6 pen-space app assets to PCW raw CGA2 hardware bytes."""

import sys


def pcw_raw_byte(byte: int) -> int:
    return ((byte & 0x55) << 1) | (((byte ^ 0xFF) & 0xAA) >> 1)


def rawify_payload(data: bytearray, start: int) -> bytearray:
    for i in range(start, len(data)):
        data[i] = pcw_raw_byte(data[i])
    return data


def rawify_ist(data: bytearray) -> bytearray:
    if len(data) < 16 or data[:4] != b"GBIS" or data[4] != 2:
        raise ValueError("not a GBIS v2 icon set")
    start = 16 + data[5] * 4
    if start > len(data):
        raise ValueError("truncated GBIS directory")
    return rawify_payload(data, start)


def main(argv: list[str]) -> int:
    if len(argv) != 4 or argv[1] != "ist":
        print("usage: pcw_rawify.py ist IN.IST OUT.IST", file=sys.stderr)
        return 2
    data = bytearray(open(argv[2], "rb").read())
    try:
        out = rawify_ist(data)
    except ValueError as exc:
        print(f"{argv[2]}: {exc}", file=sys.stderr)
        return 1
    open(argv[3], "wb").write(out)
    print(f"{argv[2]} -> {argv[3]}  ({len(out)} bytes, PCW raw)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
