# GB-PAINT

Standalone GEOBENCH Paint application.

This repository builds `PAINT.APP` against the GEOBENCH app ABI and packages a
companion disk image containing:

- `PAINT.APP`
- `PAINT.IST` with the five toolchest icons currently used by the app
- a small sample `.PIC` set that fits on a CPC data disk

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

Useful partial targets:

```sh
make app
make assets
make dsk
```

## Test

Boot GEOBENCH from its main disk/card, mount `dist/GB-PAINT.DSK` as another
drive, open that drive in File Manager, and double-click `PAINT.APP`.

The disk includes `PAINT.IST` because Paint loads its toolchest icons at runtime.
GEOBENCH shared modules such as `GBUI.MOD` are expected to come from the boot
GEOBENCH system media.

The sample payload is intentionally small. Adding every picture from GEOBENCH's
main assets would exceed a normal CPC data disk.

## Requirements

- SDCC Z80 toolchain (`sdcc`, `sdasz80`, `makebin`)
- RASM
- Python 3
- GEOBENCH checkout matching the target kernel ABI
