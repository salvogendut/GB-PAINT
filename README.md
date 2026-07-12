# GB-PAINT

Standalone GEOBENCH Paint application.

This repository builds `PAINT.APP` against the GEOBENCH app ABI and packages
companion disk images containing:

- `PAINT.APP`
- `PAINT.IST` with the five toolchest icons currently used by the app
- a small portable `.PIC` sample set that fits on a CPC/PCW data disk

The full Paint tool artwork lives under `assets/paint/`, with the source sheet
at `assets/paint-tools.png`. It is kept here with Paint because it is app-specific
source material; the standalone DSK only ships the packed runtime `PAINT.IST`.

## Build

By default the Makefile expects the GEOBENCH checkout next to this repo:

```sh
make
```

Override the SDK path if needed:

```sh
make GEOBENCH=/path/to/geobench
```

Outputs:

- `build/PAINT.APP`
- `build/PAINT.IST`
- `dist/GB-PAINT.DSK`
- `dist/GB-PAINT-PCW.DSK`

Useful partial targets:

```sh
make app
make assets
make dsk
make pcw
make dsk-pcw
```

## Test

Boot GEOBENCH from its main disk/card, mount `dist/GB-PAINT.DSK` as another
drive, open that drive in File Manager, and double-click `PAINT.APP`.

On the Amstrad PCW target, boot `../geobench/QA/PCW/GEOBENCH.DSK` and mount
`dist/GB-PAINT-PCW.DSK` in drive B. The PCW disk is a CF2 CP/M data disk built
with GEOBENCH's `tools/mkpcwdsk.py`.

The disk includes `PAINT.IST` because Paint loads its toolchest icons at runtime.
GEOBENCH shared modules such as `GBUI.MOD` are expected to come from the boot
GEOBENCH system media.

`PAINT.IST` uses GEOBENCH's portable canonical Mode-1 encoding and is identical
on CPC, MSX2, and PCW. Paint converts only the tool-icon rows sent to a non-CPC
display, just as it does for its picture canvas.

Paint keeps its canvas in GEOBENCH's canonical GBPC v2 Mode-1 packing and saves
the same byte representation on CPC, MSX2, and PCW. MSX2 and PCW translate only
the rows sent to their displays, so the sample `.PIC` files are packaged
byte-for-byte on both standalone disks.

The PCW build edits normal 100x100 pictures. Large banked pictures are opened
view-only on CPC/MSX; on PCW they are reported as too large because GEOBENCH's
banked picture edit helper is not resident there.

The sample payload is intentionally small. Adding every picture from GEOBENCH's
main assets would exceed a normal CPC data disk.

## Requirements

- SDCC Z80 toolchain (`sdcc`, `sdasz80`, `makebin`)
- RASM
- Python 3
- GEOBENCH checkout matching the target kernel ABI
