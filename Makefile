APP := PAINT
GEOBENCH ?= ../geobench
BUILD := build
DIST := dist

GB := $(GEOBENCH)/lib/gb
SRC := src/main.c
DATA_LOC ?= 0x72B0
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

CFLAGS := -mz80 --fomit-frame-pointer $(APPDEFS) -I $(GB)

.PHONY: all app dsk assets clean check-sdk

all: $(DSK)

app: $(APPFILE)

assets: $(IST)

dsk: $(DSK)

check-sdk:
	@test -f "$(GB)/gb.h" || { echo "GEOBENCH SDK not found at $(GEOBENCH)"; exit 1; }
	@test -x "$$(command -v $(SDCC))" || { echo "sdcc not found"; exit 1; }
	@test -x "$$(command -v $(RASM))" || { echo "rasm not found"; exit 1; }

$(BUILD) $(DIST):
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

clean:
	rm -rf $(BUILD) $(DIST)
