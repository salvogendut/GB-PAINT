APP := PAINT
GEOBENCH ?= ../geobench
BUILD := build
DIST := dist

GB := $(GEOBENCH)/lib/gb
SRC := src/main.c
DATA_LOC ?= 0x72B0
PCW_DATA_LOC ?= 0x7290
APPDEFS ?=

SDCC ?= sdcc
SDCC_BIN := $(dir $(shell command -v $(SDCC)))
SDAS ?= $(SDCC_BIN)sdasz80
MAKEBIN ?= $(SDCC_BIN)makebin
RASM ?= rasm
PYTHON ?= python3

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
	assets/paint/square.asm \
	assets/paint/circle.asm \
	assets/paint/fill.asm \
	assets/paint/undo.asm

SAMPLES := \
	samples/464.PIC \
	samples/PENGUIN.PIC \
	samples/TLEUNG.PIC \
	samples/LOGO.PIC

REL := \
	$(BUILD)/crt0.rel \
	$(BUILD)/main.rel \
	$(BUILD)/gbwin.rel \
	$(BUILD)/gbui_stub.rel \
	$(BUILD)/gblib.rel

PCW_REL := \
	$(PCW_BUILD)/crt0.rel \
	$(PCW_BUILD)/main.rel \
	$(PCW_BUILD)/gbwin.rel \
	$(PCW_BUILD)/gbui_stub.rel \
	$(PCW_BUILD)/gblib.rel

CFLAGS := -mz80 --fomit-frame-pointer $(APPDEFS) -I $(GB)
PCW_CFLAGS := -mz80 --fomit-frame-pointer -DGB_PCW $(APPDEFS) -I $(GB)

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

$(BUILD)/gblib.rel: $(GB)/gblib.s | $(BUILD)
	$(SDAS) -o $@ $<

$(BUILD)/main.rel: $(SRC) $(GB)/gb.h | $(BUILD)
	$(SDCC) $(CFLAGS) -c $< -o $@

$(BUILD)/gbwin.rel: $(GB)/gbwin.c $(GB)/gb.h | $(BUILD)
	$(SDCC) $(CFLAGS) -c $< -o $@

$(BUILD)/gbui_stub.rel: $(GB)/gbui_stub.c $(GB)/gb.h | $(BUILD)
	$(SDCC) $(CFLAGS) -c $< -o $@

$(BUILD)/app.ihx: check-sdk $(REL)
	$(SDCC) -mz80 --no-std-crt0 --code-loc 0x4000 --data-loc $(DATA_LOC) $(REL) -o $@
	$(PYTHON) tools/check_fit.py $(BUILD)/app.map $(DATA_LOC) $(APP)

$(BUILD)/app.bin: $(BUILD)/app.ihx
	$(MAKEBIN) -p $< $@

$(RAW): $(BUILD)/app.bin
	tail -c +16385 $< > $@

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

$(PCW_BUILD)/gblib.rel: $(GB)/gblib.s | $(PCW_BUILD)
	$(SDAS) -o $@ $<

$(PCW_BUILD)/main.rel: $(SRC) $(GB)/gb.h | $(PCW_BUILD)
	$(SDCC) $(PCW_CFLAGS) -c $< -o $@

$(PCW_BUILD)/gbwin.rel: $(GB)/gbwin.c $(GB)/gb.h | $(PCW_BUILD)
	$(SDCC) $(PCW_CFLAGS) -c $< -o $@

$(PCW_BUILD)/gbui_stub.rel: $(GB)/gbui_stub.c $(GB)/gb.h | $(PCW_BUILD)
	$(SDCC) $(PCW_CFLAGS) -c $< -o $@

$(PCW_BUILD)/app.ihx: check-sdk $(PCW_REL)
	$(SDCC) -mz80 --no-std-crt0 --code-loc 0x4000 --data-loc $(PCW_DATA_LOC) $(PCW_REL) -o $@
	$(PYTHON) tools/check_fit.py $(PCW_BUILD)/app.map $(PCW_DATA_LOC) $(APP)-PCW

$(PCW_BUILD)/app.bin: $(PCW_BUILD)/app.ihx
	$(MAKEBIN) -p $< $@

$(PCW_RAW): $(PCW_BUILD)/app.bin
	tail -c +16385 $< > $@

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
