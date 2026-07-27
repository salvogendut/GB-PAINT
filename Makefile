APP := PAINT
GEOBENCH ?= ../geobench
BUILD := build
DIST := dist

GB := $(GEOBENCH)/lib/gb
APP_DIR := apps/paint
SRC := $(APP_DIR)/main.c
DATA_LOC ?= 0x79E0
PCW_DATA_LOC ?= 0x7B80
APPDEFS ?=

SDCC ?= sdcc
SDCC_BIN := $(dir $(shell command -v $(SDCC)))
GBLIB_TOOL := $(GEOBENCH)/tools/gblib_subset.py
GBLIB_SYMBOLS := $(APP_DIR)/gblib.symbols
GBLIB_SUBSET := $(BUILD)/gblib_paint.s
SDAS ?= $(SDCC_BIN)sdasz80
MAKEBIN ?= $(SDCC_BIN)makebin
RASM ?= rasm
PYTHON ?= python3

APP_ICON := $(APP_DIR)/icon.asm
APP_ICON16 := $(APP_DIR)/icon16.asm
ICON_TOOL := $(GEOBENCH)/tools/embed_app_icon.py
CODE_LOC := 0x4110
LINKED_RAW := $(BUILD)/$(APP)-linked.RAW
PCW_LINKED_RAW = $(PCW_BUILD)/$(APP)-linked.RAW

RAW := $(BUILD)/$(APP).RAW
APPFILE := $(BUILD)/$(APP).APP
IST := $(BUILD)/$(APP).IST
DSK := $(DIST)/GB-PAINT.DSK
PACK_DIR := $(BUILD)/pack_dsk
PACK_STAMP := $(PACK_DIR)/.stamp

PCW_BUILD := $(BUILD)/pcw
PCW_RAW := $(PCW_BUILD)/$(APP).RAW
PCW_APPFILE := $(PCW_BUILD)/$(APP).APP
PCW_DSK := $(DIST)/GB-PAINT-PCW.DSK

PAINT_TOOLS := \
	assets/paint/pencil.asm \
	assets/paint/line.asm \
	assets/paint/square.asm \
	assets/paint/boxfill.asm \
	assets/paint/circle.asm \
	assets/paint/circlefill.asm \
	assets/paint/bucket.asm \
	assets/paint/spray.asm \
	assets/paint/select.asm \
	assets/paint/cut.asm \
	assets/paint/copy.asm \
	assets/paint/paste.asm \
	assets/paint/undo.asm

SAMPLES := \
	samples/464.PIC \
	samples/PENGUIN.PIC \
	samples/TLEUNG.PIC \
	samples/LOGO.PIC

REL := \
	$(BUILD)/crt0.rel \
	$(BUILD)/main.rel \
	$(BUILD)/gbsizedlg.rel \
	$(BUILD)/gbui_stub.rel \
	$(BUILD)/gblib.rel

PCW_REL := \
	$(PCW_BUILD)/crt0.rel \
	$(PCW_BUILD)/main.rel \
	$(PCW_BUILD)/gbsizedlg.rel \
	$(PCW_BUILD)/gbui_stub.rel \
	$(PCW_BUILD)/gblib.rel

CFLAGS := -mz80 --opt-code-size --max-allocs-per-node 100000 --fomit-frame-pointer $(APPDEFS) -I $(GB)
PCW_CFLAGS := -mz80 --opt-code-size --max-allocs-per-node 100000 --fomit-frame-pointer -DGB_PCW $(APPDEFS) -I $(GB)

.PHONY: all cpc pcw app app-pcw dsk dsk-pcw assets assets-pcw clean check-sdk

all: cpc pcw

cpc: $(DSK)

pcw: $(PCW_DSK)

app: $(APPFILE)

app-pcw: $(PCW_APPFILE)

assets: $(IST)

assets-pcw: $(IST)

dsk: $(DSK)

dsk-pcw: $(PCW_DSK)

check-sdk:
	@test -f "$(GB)/gb.h" || { echo "GEOBENCH SDK not found at $(GEOBENCH)"; exit 1; }
	@test -x "$$(command -v $(SDCC))" || { echo "sdcc not found"; exit 1; }
	@test -x "$$(command -v $(RASM))" || { echo "rasm not found"; exit 1; }

$(BUILD) $(DIST) $(PCW_BUILD):
	mkdir -p $@

$(BUILD)/crt0.rel: $(GB)/crt0.s | $(BUILD)
	$(SDAS) -o $@ $<

$(GBLIB_SUBSET): $(GB)/gblib.s $(GBLIB_TOOL) $(GBLIB_SYMBOLS) | $(BUILD)
	$(PYTHON) $(GBLIB_TOOL) $(GB)/gblib.s $@ $(GBLIB_SYMBOLS)

$(BUILD)/gblib.rel: $(GBLIB_SUBSET) | $(BUILD)
	$(SDAS) -o $@ $<

$(BUILD)/main.rel: $(SRC) $(GB)/gb.h Makefile | $(BUILD)
	$(SDCC) $(CFLAGS) -c $< -o $@

$(BUILD)/gbui_stub.rel: $(GB)/gbui_stub.c $(GB)/gb.h Makefile | $(BUILD)
	$(SDCC) $(CFLAGS) -c $< -o $@

$(BUILD)/gbsizedlg.rel: $(GB)/gbsizedlg.c $(GB)/gb.h Makefile | $(BUILD)
	$(SDCC) $(CFLAGS) -c $< -o $@

$(BUILD)/app.ihx: check-sdk $(REL)
	$(SDCC) -mz80 --opt-code-size --no-std-crt0 --code-loc $(CODE_LOC) --data-loc $(DATA_LOC) $(REL) -o $@
	$(PYTHON) tools/check_fit.py $(BUILD)/app.map $(DATA_LOC) $(APP)

$(BUILD)/app.bin: $(BUILD)/app.ihx
	$(MAKEBIN) -p $< $@

$(LINKED_RAW): $(BUILD)/app.bin
	tail -c +16385 $< > $@

$(RAW): $(LINKED_RAW) $(APP_ICON) $(ICON_TOOL)
	$(PYTHON) $(ICON_TOOL) inject $(APP_ICON) $(LINKED_RAW) $@

$(APPFILE): $(RAW)
	cp $< $@

$(IST): $(PAINT_TOOLS) $(GEOBENCH)/tools/packicons.py | $(BUILD)
	$(PYTHON) $(GEOBENCH)/tools/packicons.py $@ $(PAINT_TOOLS)

$(PACK_STAMP): tools/mk_dsk_pack.py $(APPFILE) $(IST) $(SAMPLES) | $(BUILD) $(DIST)
	$(PYTHON) tools/mk_dsk_pack.py $(PACK_DIR) $(DSK) \
		PAINT.APP=$(APPFILE) \
		PAINT.IST=$(IST) \
		464.PIC=samples/464.PIC \
		PENGUIN.PIC=samples/PENGUIN.PIC \
		TLEUNG.PIC=samples/TLEUNG.PIC \
		LOGO.PIC=samples/LOGO.PIC

$(DSK): $(PACK_STAMP) $(APPFILE) $(IST) $(SAMPLES) | $(DIST)
	rm -f $@
	for asm in $(PACK_DIR)/*.asm; do $(RASM) "$$asm" -eo || exit $$?; done

$(PCW_BUILD)/crt0.rel: $(GB)/crt0.s | $(PCW_BUILD)
	$(SDAS) -o $@ $<

$(PCW_BUILD)/gblib.rel: $(GBLIB_SUBSET) | $(PCW_BUILD)
	$(SDAS) -o $@ $<

$(PCW_BUILD)/main.rel: $(SRC) $(GB)/gb.h Makefile | $(PCW_BUILD)
	$(SDCC) $(PCW_CFLAGS) -c $< -o $@

$(PCW_BUILD)/gbui_stub.rel: $(GB)/gbui_stub.c $(GB)/gb.h Makefile | $(PCW_BUILD)
	$(SDCC) $(PCW_CFLAGS) -c $< -o $@

$(PCW_BUILD)/gbsizedlg.rel: $(GB)/gbsizedlg.c $(GB)/gb.h Makefile | $(PCW_BUILD)
	$(SDCC) $(PCW_CFLAGS) -c $< -o $@

$(PCW_BUILD)/app.ihx: check-sdk $(PCW_REL)
	$(SDCC) -mz80 --opt-code-size --no-std-crt0 --code-loc $(CODE_LOC) --data-loc $(PCW_DATA_LOC) $(PCW_REL) -o $@
	$(PYTHON) tools/check_fit.py $(PCW_BUILD)/app.map $(PCW_DATA_LOC) $(APP)-PCW

$(PCW_BUILD)/app.bin: $(PCW_BUILD)/app.ihx
	$(MAKEBIN) -p $< $@

$(PCW_LINKED_RAW): $(PCW_BUILD)/app.bin
	tail -c +16385 $< > $@

$(PCW_RAW): $(PCW_LINKED_RAW) $(APP_ICON) $(ICON_TOOL)
	$(PYTHON) $(ICON_TOOL) inject $(APP_ICON) $(PCW_LINKED_RAW) $@

$(PCW_APPFILE): $(PCW_RAW)
	cp $< $@

$(PCW_DSK): $(PCW_APPFILE) $(IST) $(SAMPLES) $(GEOBENCH)/tools/mkpcwdsk.py | $(DIST)
	rm -f $@
	$(PYTHON) $(GEOBENCH)/tools/mkpcwdsk.py $@ \
		--add $(PCW_APPFILE)=PAINT.APP \
		--add $(IST)=PAINT.IST \
		--add samples/464.PIC=464.PIC \
		--add samples/PENGUIN.PIC=PENGUIN.PIC \
		--add samples/TLEUNG.PIC=TLEUNG.PIC \
		--add samples/LOGO.PIC=LOGO.PIC

clean:
	rm -rf $(BUILD) $(DIST)
