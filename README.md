# GB-PAINT

Standalone GEOBENCH Paint application.

This repository builds `PAINT.APP` against the GEOBENCH app ABI and packages
companion disk images containing:

- `PAINT.APP`
- `PAINT.IST` with the thirteen 24x21 toolchest icons used by the app
- a small portable `.PIC` sample set that fits on a CPC/PCW data disk

The full Paint tool artwork lives under `assets/paint/`, with the source sheet
at `assets/paint-tools.png`. It is kept here with Paint because it is app-specific
source material; the standalone DSK only ships the packed runtime `PAINT.IST`.

The application follows the same source layout as GEOBENCH and GB-BASIC:
`apps/paint/` contains `main.c`, the libgb symbol manifest, and its app-owned
icons. `apps/paint/icon.asm` is the canonical four-colour fallback and
`apps/paint/icon16.asm` is the native MSX Screen-7 variant. GEOBENCH's MSX
build embeds both resources in a GBAP v2 header; CPC and PCW builds embed only
the portable four-colour icon.

Edit either canonical source with GEOBENCH's Python Icon Editor:

```sh
../geobench/tools/iconedit.py apps/paint/icon.asm
../geobench/tools/iconedit.py apps/paint/icon16.asm
```

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
display.

Paint starts with a movable Toolchest window. File > New opens one dimensions
panel with framed width and height fields defaulting to 100 by 100. File > Load
opens a scrollable Area Selector with a fixed 10x10 red navigator. Releasing that
navigator opens a 100x100 Canvas window where each selected source pixel is
shown at 10x magnification. Edits are written back to the banked source picture
and immediately reflected in the preview.

The editor accepts complete GBPC files up to 16 KiB. CPC and PCW edit portable
four-colour Mode-1 pictures. MSX Paint requires GEOBENCH's 16-colour Mode 7,
supports all sixteen inks, and is the only build that accepts Mode-7 pictures.
Launching it under MSX Mode 6 shows an explanatory error and closes safely.

The sample payload is intentionally small. Adding every picture from GEOBENCH's
main assets would exceed a normal CPC data disk.

## Requirements

- SDCC Z80 toolchain (`sdcc`, `sdasz80`, `makebin`)
- RASM
- Python 3
- GEOBENCH checkout matching the target kernel ABI
