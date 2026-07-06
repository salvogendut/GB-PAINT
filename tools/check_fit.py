#!/usr/bin/env python3
import re
import sys


def main() -> int:
    if len(sys.argv) != 4:
        print("usage: check_fit.py MAP DATA_LOC APP", file=sys.stderr)
        return 2

    map_path = sys.argv[1]
    data_loc = int(sys.argv[2], 16)
    app = sys.argv[3]
    areas = {}

    with open(map_path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            m = re.match(
                r"^(_CODE|_DATA|_BSS|_INITIALIZED|_GSINIT|_GSFINAL|_INITIALIZER)\s+"
                r"([0-9A-Fa-f]{8})\s+([0-9A-Fa-f]{8})",
                line,
            )
            if m:
                areas[m.group(1)] = (int(m.group(2), 16), int(m.group(3), 16))

    load = ("_CODE", "_GSINIT", "_GSFINAL", "_INITIALIZER")
    img_end = max((areas[a][0] + areas[a][1]) for a in load if a in areas)
    top = max((start + size) for start, size in areas.values()) if areas else 0

    errors = []
    if img_end > data_loc:
        errors.append(
            f"loaded image ends 0x{img_end:04X} > data-loc 0x{data_loc:04X}"
        )
    if top > 0x8000:
        errors.append(f"data/bss ends 0x{top:04X} > kernel 0x8000")
    if errors:
        print(f"FIT ERROR ({app}): {'; '.join(errors)}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
