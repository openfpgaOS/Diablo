# DevilutionX (Diablo) port to openfpgaOS (Analogue Pocket) — build.
#
# Cross-compiles DevilutionX (NONET + NOSOUND config) for RISC-V rv32imafc,
# statically linking musl + the toolchain's libstdc++/libsupc++ (with the
# _impure_ptr glue) against the openfpgaOS HAL via the SDL2 shim in sdl/.
#
# Targets: make (build app.elf) | make clean

APP      := diablo
ROOT     := $(realpath $(CURDIR)/../..)
SDK      := $(ROOT)/src/sdk
DX       := $(CURDIR)/devilutionx
TP       := $(CURDIR)/3rdparty
OBJDIR   := $(ROOT)/.obj/$(APP)
MUSL     := $(SDK)/musl
DIST     := $(ROOT)/dist/$(APP)
TARGET   ?= pocket
MISTER_IP ?= mister.local
OUT      := $(ROOT)/build/$(TARGET)/$(APP)
RUNTIME  := $(ROOT)/runtime
GAME_ASSETS = $(wildcard *.dat *.bin *.cfg)

# ── Toolchain ────────────────────────────────────────────────────────
# Prefer xPack's riscv-none-elf- (the containerized toolchain, and the only
# common bare-metal prefix that ships libstdc++/libsupc++ for C++); fall back
# to the newlib riscv64-*-elf- prefixes on hosts that have them.
CROSS   ?= $(shell \
	if   command -v riscv-none-elf-g++       >/dev/null 2>&1; then echo riscv-none-elf-; \
	elif command -v riscv64-unknown-elf-g++  >/dev/null 2>&1; then echo riscv64-unknown-elf-; \
	else echo riscv64-elf-; fi)
CC      := $(CROSS)gcc
CXX     := $(CROSS)g++
SIZE    := $(CROSS)size
STRIP   := $(CROSS)strip
# BITMANIP=1 appends the Zba/Zbb bit-manipulation extensions (sh1add/sh2add
# address generation, min/max, andn) to -march. The LUT-heavy blit loops
# benefit directly, but instructions the core does not implement trap at
# runtime -- leave this off until the bitstream's VexiiRiscv config is
# confirmed to include B-ext.
#   2026-07-09 status: NO shipped core has B-ext.  Zba+Zbb was A/B'd on
#   os20 (50-seed sweeps) and REJECTED: Zbb's rotate/clz/rev8 widen the
#   early-ALU result mux inside the CPU's mandatory full-bypass loop,
#   regressing the whole timing distribution ~1.2 ns (−1.11 → −2.32 best).
#   A Zba-only core remains untried; until one ships, BITMANIP=1 traps.
ISA     := rv32imafc$(if $(filter 1,$(BITMANIP)),_zba_zbb)
ARCH    := -march=$(ISA) -mabi=ilp32f

# C++ system header dirs: ask the compiler itself (robust across prefixes).
CXXINC  := $(shell echo | $(CXX) $(ARCH) -x c++ -E -Wp,-v - 2>&1 | sed -n 's,^ \(/.*c++.*\),-isystem \1,p')
GXXINC  := $(shell $(CXX) $(ARCH) -print-file-name=include)
LIBSTDCXX := $(shell $(CXX) $(ARCH) -print-file-name=libstdc++.a)
LIBSUPCXX := $(shell $(CXX) $(ARCH) -print-file-name=libsupc++.a)
LIBGCC    := $(shell $(CXX) $(ARCH) -print-libgcc-file-name)

# ── Include paths ─────────────────────────────────────────────────────
APPINC := -I$(CURDIR) -I$(CURDIR)/sdl/include -I$(DX)/Source \
          -I$(TP)/fmt/include -I$(TP)/simpleini -I$(TP)/libmpq -I$(TP)/libmpq/libmpq -I$(TP)/bzip2 -I$(TP)/zlib_shim -I$(TP)/dr_libs \
          -I$(TP)/libsmackerdec/include \
          -I$(DX)/3rdParty/tl -I$(DX)/3rdParty/hoehrmann_utf8 -I$(DX)/3rdParty/PicoSHA2 \
          -I$(DX)/3rdParty/PKWare -I$(SDK)/include
SYSINC := -nostdinc $(CXXINC) -isystem $(MUSL)/include -isystem $(GXXINC)

# ── Defines (mirror the CMake NONET+NOSOUND configuration) ────────────
DEFS := -DNONET -DOPENFPGAOS -DDVL_NO_FILESYSTEM -DDEVILUTIONX_DISABLE_RTTI -DDEVILUTIONX_DISABLE_EXCEPTIONS \
        -DDEVILUTIONX_RESAMPLER_SPEEX -DDEVILUTIONX_DEFAULT_RESAMPLER=Speex \
        -DDEVILUTIONX_DISPLAY_TEXTURE_FORMAT=SDL_PIXELFORMAT_RGB888 \
        -DDEVILUTIONX_PALETTE_TRANSPARENCY_BLACK_16_LUT=1

# `make PERF=1`: serial frame/audio telemetry every 2 s
#   [of] perf: fps=A.B draw=C.Cms flip=D.Dms aud=Ems gap=Fms
# draw = world render (in-game) or SVid decode (movies); aud = MIN buffered
# audio in the window (~0 = ring ran dry); gap = silence the movie push-
# decoder injected (supply deficit the ring level cannot show). Costs a few
# us/frame; default OFF.
ifeq ($(PERF),1)
  DEFS += -DOF_PERF_TRACE
endif

# ── GPU bring-up (EXPERIMENTAL, default OFF) ──────────────────────────
# GPU=1 links platform/of_gpu_bridge.c (the one TU allowed to include
# of_gpu.h) and defines OF_DIABLO_GPU: boot-time GPU feature report +
# command-path smoke test, plus LightTables -> palookup and transparency
# LUT -> translucency-RAM uploads at level load. Stage-1 scaffolding for
# a GPU floor pass; the per-frame render path is untouched.
ifeq ($(GPU),1)
  DEFS += -DOF_DIABLO_GPU
endif

# -fno-math-errno / -fno-trapping-math: this is bare metal with no errno-based
# libm error checking and no installed FP traps, so these change no FP result
# bits but let GCC treat math calls as pure and skip errno/trap bookkeeping.
# Tree-wide optimization level. -Os was the long-standing default, chosen
# when the core was believed to have an 8 KB I-cache; os20 actually has
# 32 KB I + 64 KB D (dual-issue), and the blanket -O2 build measured faster
# on HW (2026-07), so -O2 is now the default. A/B the old size build with:
#   make clean && make BASE_OPT=-Os
BASE_OPT ?= -O2
COMMON := $(ARCH) $(BASE_OPT) -fno-math-errno -fno-trapping-math \
          -ffunction-sections -fdata-sections -fno-tree-loop-distribute-patterns \
          -include $(CURDIR)/platform/of_ctype_compat.h $(DEFS) $(APPINC) $(SYSINC) \
          -Wno-unknown-pragmas -Wno-unused-parameter -Wno-unused-variable -Wno-sign-compare
CXXFLAGS := -std=gnu++20 -fno-exceptions -fno-rtti -fno-threadsafe-statics $(COMMON)
CFLAGS   := -std=gnu11 -DVERSION='"0.4.2"' $(COMMON)

LDFLAGS := $(ARCH) -nostdlib -static -T $(SDK)/app.ld -L$(MUSL)/lib \
           -Wl,--gc-sections -Wl,--no-warn-rwx-segments
LIBS := -Wl,--start-group -lc -lm $(LIBSTDCXX) $(LIBSUPCXX) $(LIBGCC) -Wl,--end-group
CRT  := $(MUSL)/lib/crt1.o $(MUSL)/lib/crti.o $(MUSL)/lib/crtn.o

# ── Source lists ──────────────────────────────────────────────────────
# DevilutionX core set: everything under Source/ except network, discord,
# console-specific platforms, the SDL1 backport TU, the Aulib sound files
# (NOSOUND uses *_stubs.cpp instead), and the smacker movie decoder.
DX_SRCS := $(shell find $(DX)/Source -name '*.cpp' \
    -not -path '*/discord/*' \
    -not -path '*/platform/android/*' -not -path '*/platform/ctr/*' \
    -not -path '*/platform/ios/*' -not -path '*/platform/macos_sdl1/*' \
    -not -path '*/platform/switch/*' -not -path '*/platform/vita/*' \
    -not -name 'tcp_client.cpp' -not -name 'tcp_server.cpp' \
    -not -name 'protocol_zt.cpp' -not -name 'zerotier_native.cpp' -not -name 'zerotier_lwip.cpp' \
    -not -name 'sdl2_to_1_2_backports.cpp' \
    -not -name 'effects_stubs.cpp' -not -name 'sound_stubs.cpp' \
    -not -name 'restrict.cpp')

FMT_SRCS  := $(TP)/fmt/src/format.cc
MPQ_SRCS  := $(wildcard $(TP)/libmpq/libmpq/*.c)
# Real bzip2 decompressor: devilutionx.mpq compresses ~all CLX assets (fonts,
# backgrounds) with bzip2 (sector mask 0x10). The compress-only TUs link but
# are dropped by --gc-sections since only BZ2_bzDecompress* is referenced.
BZIP2_SRCS := $(wildcard $(TP)/bzip2/*.c)
# Smacker cutscene decoder (vendored diasurgical/libsmackerdec @91e732b, the
# exact commit upstream CMake pins; SDL_RWops-based, no exceptions/iostream).
# -DNDEBUG on these TUs disarms BitReader's assert(0)-on-overread so a corrupt
# video degrades instead of aborting.
SMK_SRCS  := $(wildcard $(TP)/libsmackerdec/src/*.cpp)
PKW_SRCS  := $(DX)/3rdParty/PKWare/explode.cpp $(DX)/3rdParty/PKWare/implode.cpp
LOC_CXX   := $(CURDIR)/sdl/of_sdl2.cpp $(CURDIR)/sdl/of_aulib.cpp $(CURDIR)/platform/devx_stubs.cpp \
             $(CURDIR)/platform/of_oom.cpp
LOC_C     := $(CURDIR)/platform/of_libc_compat.c $(CURDIR)/platform/zlib_bzip2_stub.c \
             $(CURDIR)/platform/of_platform.c $(SDK)/of_init.c
ifeq ($(GPU),1)
  LOC_C   += $(CURDIR)/platform/of_gpu_bridge.c
endif

# ── Object mapping (mirror tree under OBJDIR; flatten the source root) ──
obj = $(OBJDIR)/$(patsubst $(ROOT)/%,%,$(1)).o
DX_OBJS  := $(foreach s,$(DX_SRCS),$(call obj,$(s)))
FMT_OBJS := $(foreach s,$(FMT_SRCS),$(call obj,$(s)))
MPQ_OBJS := $(foreach s,$(MPQ_SRCS),$(call obj,$(s)))
BZIP2_OBJS := $(foreach s,$(BZIP2_SRCS),$(call obj,$(s)))
SMK_OBJS := $(foreach s,$(SMK_SRCS),$(call obj,$(s)))
$(SMK_OBJS): CXXFLAGS += -DNDEBUG
PKW_OBJS := $(foreach s,$(PKW_SRCS),$(call obj,$(s)))
LOC_OBJS := $(foreach s,$(LOC_CXX) $(LOC_C),$(call obj,$(s)))
ALL_OBJS := $(DX_OBJS) $(FMT_OBJS) $(MPQ_OBJS) $(BZIP2_OBJS) $(SMK_OBJS) $(PKW_OBJS) $(LOC_OBJS)

# ── Speed build for hot translation units (DEFAULT ON) ────────────────
# The whole tree defaults to -Os (see BASE_OPT above); HOT_OPT compiles
# only the speed-critical TUs at a higher level:
#   - frame time:  the software renderer (dun_render/clx_render/scrollrt/
#     text_render/primitive_render), per-frame light propagation
#     (lighting), palette fades/cycling (palette) + the SDL/audio shims —
#     these bound fps, and shorter frames also shrink the audio-feed gap;
#   - load time:   every decompress/convert pass a load runs (bzip2,
#     PKWARE explode, libmpq huffman/ADPCM for SFX, CEL/CL2/PCX -> CLX
#     conversion) — loads measure CPU-dominated, not SD-dominated;
#   - cutscenes:   the Smacker bit-reader/decoder inner loops.
# Disable for an A/B baseline with:  make HOT_OPT=
# With the tree now at -O2 (BASE_OPT above), HOT_OPT's remaining job is
# lifting the hot list to -O3 (unrolling helps the RLE decode / blit inner
# loops; code growth stays confined to these TUs).
HOT_OPT ?= -O3 -ffp-contract=fast
HOT_SRCS := \
    $(CURDIR)/sdl/of_aulib.cpp $(CURDIR)/sdl/of_sdl2.cpp \
    $(DX)/Source/engine/render/dun_render.cpp $(DX)/Source/engine/render/clx_render.cpp \
    $(DX)/Source/engine/render/scrollrt.cpp $(DX)/Source/engine/render/text_render.cpp \
    $(DX)/Source/engine/render/primitive_render.cpp \
    $(DX)/Source/engine/palette.cpp $(DX)/Source/lighting.cpp \
    $(TP)/bzip2/decompress.c $(TP)/bzip2/huffman.c $(TP)/libmpq/libmpq/explode.c \
    $(TP)/libmpq/libmpq/huffman.c $(TP)/libmpq/libmpq/wave.c \
    $(DX)/Source/utils/cel_to_clx.cpp $(DX)/Source/utils/cl2_to_clx.cpp \
    $(DX)/Source/utils/pcx_to_clx.cpp \
    $(TP)/libsmackerdec/src/SmackerDecoder.cpp $(TP)/libsmackerdec/src/BitReader.cpp
ifneq ($(strip $(HOT_OPT)),)
  HOT_OBJS := $(foreach s,$(HOT_SRCS),$(call obj,$(s)))
  $(HOT_OBJS): CXXFLAGS += $(HOT_OPT)
  $(HOT_OBJS): CFLAGS   += $(HOT_OPT)
endif
# Smacker decode peaks (scene-cut keyframes) are the only frames that exceed
# the 66 ms movie budget on HW (measured avg 18-53 ms/frame at -O2); -O3 on
# just these two small TUs shaves the peaks. Appended after HOT_OPT so it
# wins (last -O on the command line applies).
$(call obj,$(TP)/libsmackerdec/src/SmackerDecoder.cpp): CXXFLAGS += -O3
$(call obj,$(TP)/libsmackerdec/src/BitReader.cpp): CXXFLAGS += -O3

.DEFAULT_GOAL := all

# Generic compile rule: the object's stem (everything between $(OBJDIR)/ and
# .o) is the source path relative to $(ROOT), so $(ROOT)/% is the source and
# also the prerequisite (so edits trigger rebuilds).  The Makefile itself is
# a prerequisite so flag changes (HOT_OPT, DEFS, ...) rebuild stale objects —
# without it, A/B runs like `make HOT_OPT=` silently compare identical
# binaries built with the old flags.
$(OBJDIR)/%.o: $(ROOT)/% $(CURDIR)/Makefile
	@mkdir -p $(dir $@)
	@case "$<" in \
	  *.cpp|*.cc) echo "CXX $*"; $(CXX) $(CXXFLAGS) -c -o $@ "$<" ;; \
	  *.c)        echo "CC  $*"; $(CC)  $(CFLAGS)  -c -o $@ "$<" ;; \
	esac

# ── Container build guard (mirrors sdk.mk) — CONTAINERIZED BY DEFAULT ──
# Diablo is the ONE game repo that can't reuse the shared openfpgaos-firmware
# image: DevilutionX is C++ and statically links the toolchain's REAL
# libstdc++/libsupc++ (and needs the C++ standard headers), whereas that image
# (and Ubuntu's gcc-riscv64-unknown-elf, and Homebrew's riscv64-elf-gcc) ships
# NO bare-metal C++ runtime — `#include <string>` fails and no rv32 libstdc++.a
# exists to link. So Diablo gets its OWN image (tools/docker/Dockerfile.diablo)
# built around the xPack riscv-none-elf-gcc toolchain, which carries newlib +
# a full libstdc++/libsupc++ multilib. SDK_IMG/SDK_DOCKERFILE point the shared
# sdk-container.sh wrapper at that image (it auto-builds on first use).
#
# Result: `make build` Just Works on any host with Docker (or Apple
# `container`) and no hand-installed RISC-V toolchain — the reason the build
# previously required a bespoke host setup and broke on stock macOS make.
# Escape hatch: USE_SDK_CONTAINER=0 builds on the host toolchain (needs a
# C++-capable bare-metal riscv-none-elf-/riscv64-*-elf- GCC on PATH).
USE_SDK_CONTAINER ?= 1
SDK_CONTAINER    ?= $(ROOT)/tools/sdk-container.sh
export SDK_IMG        ?= openfpgaos-diablo
export SDK_DOCKERFILE ?= $(ROOT)/tools/docker/Dockerfile.diablo
ifeq ($(USE_SDK_CONTAINER)_$(OF_SDK_IN_CONTAINER),1_)

app.elf: SDK_FORCE
	@bash $(SDK_CONTAINER) make app.elf
SDK_FORCE:
.PHONY: SDK_FORCE

else

app.elf: $(ALL_OBJS)
	@echo "LD  $@ ($(words $(ALL_OBJS)) objects)"
	$(CXX) $(LDFLAGS) -o $@ $(CRT) $(ALL_OBJS) $(LIBS)
	@$(SIZE) $@ 2>/dev/null || true

endif

all: app.elf release

# ── SD-card image assembly ──────────────────────────────────────────
# Copies dist/diablo/ to build/diablo/, then layers in the runtime
# bitstream + loader (per Core) and os.bin + diablo.elf (per Assets
# common), producing a tree to drop onto the Pocket SD card.
#
# The large game data (DIABDAT.mpq, optional Hellfire MPQs, devilutionx.mpq) is NOT in the
# repo: place it in Assets/diablo/common/ on the SD card (or stage it under
# dist/diablo/Assets/diablo/common/ to have it copied automatically here).
ifeq ($(TARGET),mister)
release: game-dist        # MiSTer: the per-game bundle IS the release artifact
else
release: app.elf
	@$(SDK)/platforms/$(TARGET)/image.sh "$(APP)" "app.elf" "$(ROOT)" "$(DIST)" $(GAME_ASSETS)
endif

# ── MiSTer per-game locked distribution ──────────────────────────────
# Two instances share one /Diablo/common (the MPQs): DevilutionX would
# otherwise auto-enable Hellfire merely because hellfire.mpq is visible, so
# each instance forces its mode with --diablo / --hellfire.  Saves stay
# separate because each instance gets its own directory.
# NB the OS instance inis are *_os.ini: plain diablo.ini / hellfire.ini are
# DevilutionX's own config files (Pocket binds them to the nonvolatile
# config slot), so reusing those names would collide.
GAME        = Diablo
GAME_INIS   = $(DIST)/Assets/diablo/common
GAME_STAGE  = $(ROOT)/build/mister/$(APP)
GAME_COMMON = $(ROOT)/build/mister/.$(APP)-common
GAME_ELF    = diablo.elf
GAME_CSV    = $(DIST)/mister/external_files.csv
# boot.vhd must hold the user's MPQs, which setup.sh injects later: DIABDAT
# ~517 MB + hellfire 65 + hfmonk/hfmusic/hfvoice ~110 + devilutionx ~6.
# Without an explicit size mkgame sizes from the COMMON payload alone and
# lands on the 48 MB FAT32 floor, so injection would run out of room.
GAME_BOOT_MB = 900

game-dist: app.elf
	@rm -rf "$(GAME_STAGE)" "$(GAME_COMMON)"
	@mkdir -p "$(GAME_STAGE)/$(GAME)/wads" "$(GAME_COMMON)"
	@cp "$(RUNTIME)/bank.ofsf" "$(GAME_COMMON)/bank.ofsf"
	@# devilutionx.mpq ships WITH the port (engine assets, not user data), so it
	@# belongs baked into the read-only image rather than waiting in wads/ for
	@# the user to supply -- unlike DIABDAT/hellfire which they must own.
	@cp "$(GAME_INIS)"/*.mpq "$(GAME_COMMON)/" 2>/dev/null || true
	@$(SDK)/platforms/mister/mkgame.sh --no-elf --saves-out "$(GAME_STAGE)/$(GAME)/$(GAME).saves.vhd" "$(GAME)" "$(GAME_INIS)" "$(GAME_COMMON)" "$(GAME_STAGE)/$(GAME)/boot.vhd" $(GAME_BOOT_MB)
	@$(SDK)/platforms/mister/mkmgl.sh "$(GAME)" "$(GAME_INIS)" "$(GAME_STAGE)"
	@cp app.elf "$(GAME_STAGE)/$(GAME)/$(GAME_ELF)"
	@cp "$(GAME_INIS)"/*.ini "$(GAME_STAGE)/$(GAME)/"
	@sed 's|^GAME_DEFAULT=.*|GAME_DEFAULT="$(GAME)"|' "$(SDK)/platforms/mister/setup.sh" > "$(GAME_STAGE)/$(GAME)/setup.sh"
	@chmod +x "$(GAME_STAGE)/$(GAME)/setup.sh"
	@if [ -f "$(GAME_CSV)" ]; then cp "$(GAME_CSV)" "$(GAME_STAGE)/$(GAME)/external_files.csv"; fi
	@rm -rf "$(GAME_COMMON)"
	@echo "  game-dist staged -> $(GAME_STAGE)"

copy: all
	@MISTER_IP="$(MISTER_IP)" $(SDK)/platforms/$(TARGET)/copy.sh "$(APP)" "app.elf"

package: release
	@$(ROOT)/scripts/package.sh $(APP)

debug: all
	@$(ROOT)/scripts/debug.sh "$$(ls -d $(OUT)/Assets/*/common 2>/dev/null)/$(APP).elf"

clean:
	rm -rf $(OBJDIR) app.elf $(OUT)

print-srcs:
	@echo "DevilutionX sources: $(words $(DX_SRCS))"

.PHONY: all release copy package debug clean print-srcs
