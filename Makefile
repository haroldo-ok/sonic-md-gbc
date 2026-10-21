# Sonic the Hedgehog — Complete Half-Scale (0.5x) Game Boy Color (MBC5 CGB) Port Makefile
# Source Assets: vendor/s1disasm (https://github.com/sonicretro/s1disasm)

LCC ?= $(if $(wildcard $(CURDIR)/tools/gbdk/bin/lcc),$(CURDIR)/tools/gbdk/bin/lcc,$(if $(GBDK_HOME),$(GBDK_HOME)/bin/lcc,$(if $(wildcard /opt/gbdk/bin/lcc),/opt/gbdk/bin/lcc,lcc)))
PYTHON   ?= python3
OBJ_DIR  := obj
ROM_DIR  := rom
ROM_OUT  := sonic.gbc

CFLAGS   := -msm83:gb -Iinclude -Isrc -Igenerated
LDFLAGS  := -msm83:gb -Wm-yC -Wm-ynSONIC1GBC -Wl-yt0x1B -Wl-yo128 -Wl-ya4 -Wl-j -Wl-m

ENGINE_SRCS := src/main.c src/physics.c src/objects.c src/screens_and_ss.c src/bosses_and_gimmicks.c src/dle_osc_sram.c
GEN_SRCS    := $(sort $(wildcard generated/*.c))

ENGINE_OBJS := $(patsubst src/%.c,$(OBJ_DIR)/src_%.o,$(ENGINE_SRCS))
GEN_OBJS    := $(patsubst generated/%.c,$(OBJ_DIR)/gen_%.o,$(GEN_SRCS))
ALL_OBJS    := $(ENGINE_OBJS) $(GEN_OBJS)

.PHONY: all assets rom test clean

all: rom

assets:
	rm -rf $(OBJ_DIR)
	$(PYTHON) tools/convert_all.py
	$(PYTHON) tools/build_complete_supplement.py

$(OBJ_DIR)/src_%.o: src/%.c src/engine.h generated/assets.h generated/supplement.h
	@mkdir -p $(OBJ_DIR)
	@if [ -d "$(CURDIR)/tools/gbdk" ]; then chmod -R +x "$(CURDIR)/tools/gbdk"; fi
	$(LCC) $(CFLAGS) -c -o $@ $<

$(OBJ_DIR)/gen_%.o: generated/%.c generated/assets.h
	@mkdir -p $(OBJ_DIR)
	$(LCC) $(CFLAGS) -c -o $@ $<

rom: $(ALL_OBJS)
	@mkdir -p $(ROM_DIR)
	$(LCC) $(LDFLAGS) -o $(ROM_OUT) $(ALL_OBJS)
	@cp -f sonic.map $(ROM_DIR)/sonic.map 2>/dev/null || true
	@cp -f sonic.noi $(ROM_DIR)/sonic.noi 2>/dev/null || true
	@rm -f $(OBJ_DIR)/*.lst $(OBJ_DIR)/*.asm $(OBJ_DIR)/*.sym sonic.ihx $(ROM_DIR)/sonic.gbc
	@ls -lh $(ROM_OUT)

test: rom
	pytest -v tests/test_e2e_emulator.py

clean:
	rm -rf $(OBJ_DIR) $(ROM_OUT) sonic.map sonic.noi sonic.ihx $(ROM_DIR)
