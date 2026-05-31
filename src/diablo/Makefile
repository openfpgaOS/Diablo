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
OUT      := $(ROOT)/build/$(APP)
RUNTIME  := $(ROOT)/runtime
TARGET   := pocket

# ── Toolchain ────────────────────────────────────────────────────────
CROSS   ?= $(shell which riscv64-unknown-elf-g++ >/dev/null 2>&1 && echo riscv64-unknown-elf- || echo riscv64-elf-)
CC      := $(CROSS)gcc
CXX     := $(CROSS)g++
SIZE    := $(CROSS)size
STRIP   := $(CROSS)strip
ARCH    := -march=rv32imafc -mabi=ilp32f

# C++ system header dirs: ask the compiler itself (robust across prefixes).
CXXINC  := $(shell echo | $(CXX) $(ARCH) -x c++ -E -Wp,-v - 2>&1 | sed -n 's,^ \(/.*c++.*\),-isystem \1,p')
GXXINC  := $(shell $(CXX) $(ARCH) -print-file-name=include)
LIBSTDCXX := $(shell $(CXX) $(ARCH) -print-file-name=libstdc++.a)
LIBSUPCXX := $(shell $(CXX) $(ARCH) -print-file-name=libsupc++.a)
LIBGCC    := $(shell $(CXX) $(ARCH) -print-libgcc-file-name)

# ── Include paths ─────────────────────────────────────────────────────
APPINC := -I$(CURDIR) -I$(CURDIR)/sdl/include -I$(DX)/Source \
          -I$(TP)/fmt/include -I$(TP)/simpleini -I$(TP)/libmpq -I$(TP)/libmpq/libmpq -I$(TP)/bzip2 -I$(TP)/zlib_shim -I$(TP)/dr_libs \
          -I$(DX)/3rdParty/tl -I$(DX)/3rdParty/hoehrmann_utf8 -I$(DX)/3rdParty/PicoSHA2 \
          -I$(DX)/3rdParty/PKWare -I$(SDK)/include
SYSINC := -nostdinc $(CXXINC) -isystem $(MUSL)/include -isystem $(GXXINC)

# ── Defines (mirror the CMake NONET+NOSOUND configuration) ────────────
DEFS := -DNONET -DOPENFPGAOS -DDVL_NO_FILESYSTEM -DDEVILUTIONX_DISABLE_RTTI -DDEVILUTIONX_DISABLE_EXCEPTIONS \
        -DDEVILUTIONX_RESAMPLER_SPEEX -DDEVILUTIONX_DEFAULT_RESAMPLER=Speex \
        -DDEVILUTIONX_DISPLAY_TEXTURE_FORMAT=SDL_PIXELFORMAT_RGB888 \
        -DDEVILUTIONX_PALETTE_TRANSPARENCY_BLACK_16_LUT=1

# -fno-math-errno / -fno-trapping-math: this is bare metal with no errno-based
# libm error checking and no installed FP traps, so these change no FP result
# bits but let GCC treat math calls as pure and skip errno/trap bookkeeping.
COMMON := $(ARCH) -Os -fno-math-errno -fno-trapping-math \
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
    -not -name 'storm_svid.cpp' -not -name 'movie.cpp' \
    -not -name 'restrict.cpp')

FMT_SRCS  := $(TP)/fmt/src/format.cc
MPQ_SRCS  := $(wildcard $(TP)/libmpq/libmpq/*.c)
# Real bzip2 decompressor: devilutionx.mpq compresses ~all CLX assets (fonts,
# backgrounds) with bzip2 (sector mask 0x10). The compress-only TUs link but
# are dropped by --gc-sections since only BZ2_bzDecompress* is referenced.
BZIP2_SRCS := $(wildcard $(TP)/bzip2/*.c)
PKW_SRCS  := $(DX)/3rdParty/PKWare/explode.cpp $(DX)/3rdParty/PKWare/implode.cpp
LOC_CXX   := $(CURDIR)/sdl/of_sdl2.cpp $(CURDIR)/sdl/of_aulib.cpp $(CURDIR)/platform/devx_stubs.cpp
LOC_C     := $(CURDIR)/platform/of_libc_compat.c $(CURDIR)/platform/zlib_bzip2_stub.c \
             $(CURDIR)/platform/of_platform.c $(SDK)/of_init.c

# ── Object mapping (mirror tree under OBJDIR; flatten the source root) ──
obj = $(OBJDIR)/$(patsubst $(ROOT)/%,%,$(1)).o
DX_OBJS  := $(foreach s,$(DX_SRCS),$(call obj,$(s)))
FMT_OBJS := $(foreach s,$(FMT_SRCS),$(call obj,$(s)))
MPQ_OBJS := $(foreach s,$(MPQ_SRCS),$(call obj,$(s)))
BZIP2_OBJS := $(foreach s,$(BZIP2_SRCS),$(call obj,$(s)))
PKW_OBJS := $(foreach s,$(PKW_SRCS),$(call obj,$(s)))
LOC_OBJS := $(foreach s,$(LOC_CXX) $(LOC_C),$(call obj,$(s)))
ALL_OBJS := $(DX_OBJS) $(FMT_OBJS) $(MPQ_OBJS) $(BZIP2_OBJS) $(PKW_OBJS) $(LOC_OBJS)

# ── Optional speed build for hot translation units ────────────────────
# The whole tree is -Os by default: RAM is plentiful, but the in-order
# 100 MHz core has a small I-cache, so blanket -O2 is not an obvious win.
# HOT_OPT compiles only the speed-critical TUs at a higher level for A/B
# testing on hardware: the audio mix/decode + software blit/present, plus
# the MPQ/bzip2 asset decompressors. These bound frame time -- and since the
# music ring is fed only between frames, shorter frames also shrink the
# audio-underrun window. Default empty => the baseline build is byte-for-byte
# unchanged. Enable with, e.g.:
#   make HOT_OPT='-O2 -ffp-contract=fast'
HOT_OPT ?=
HOT_SRCS := \
    $(CURDIR)/sdl/of_aulib.cpp $(CURDIR)/sdl/of_sdl2.cpp \
    $(DX)/Source/engine/render/dun_render.cpp $(DX)/Source/engine/render/clx_render.cpp \
    $(DX)/Source/engine/render/scrollrt.cpp $(DX)/Source/engine/render/text_render.cpp \
    $(TP)/bzip2/decompress.c $(TP)/bzip2/huffman.c $(TP)/libmpq/libmpq/explode.c
ifneq ($(strip $(HOT_OPT)),)
  HOT_OBJS := $(foreach s,$(HOT_SRCS),$(call obj,$(s)))
  $(HOT_OBJS): CXXFLAGS += $(HOT_OPT)
  $(HOT_OBJS): CFLAGS   += $(HOT_OPT)
endif

.DEFAULT_GOAL := all

# Generic compile rule: the object's stem (everything between $(OBJDIR)/ and
# .o) is the source path relative to $(ROOT), so $(ROOT)/% is the source and
# also the prerequisite (so edits trigger rebuilds).
$(OBJDIR)/%.o: $(ROOT)/%
	@mkdir -p $(dir $@)
	@case "$<" in \
	  *.cpp|*.cc) echo "CXX $*"; $(CXX) $(CXXFLAGS) -c -o $@ "$<" ;; \
	  *.c)        echo "CC  $*"; $(CC)  $(CFLAGS)  -c -o $@ "$<" ;; \
	esac

app.elf: $(ALL_OBJS)
	@echo "LD  $@ ($(words $(ALL_OBJS)) objects)"
	$(CXX) $(LDFLAGS) -o $@ $(CRT) $(ALL_OBJS) $(LIBS)
	@$(SIZE) $@

all: app.elf release

# ── SD-card image assembly ──────────────────────────────────────────
# Copies dist/diablo/ to build/diablo/, then layers in the runtime
# bitstream + loader (per Core) and os.bin + diablo.elf (per Assets
# common), producing a tree to drop onto the Pocket SD card.
#
# The large game data (DIABDAT.mpq, optional Hellfire MPQs, devilutionx.mpq) is NOT in the
# repo: place it in Assets/diablo/common/ on the SD card (or stage it under
# dist/diablo/Assets/diablo/common/ to have it copied automatically here).
release: app.elf
	@rm -rf $(OUT)
	@mkdir -p $(dir $(OUT))
	@cp -r $(DIST) $(OUT)
	@for d in $(OUT)/Cores/*/; do \
		cp $(RUNTIME)/bitstream.rbf_r $(RUNTIME)/loader.bin "$$d"; \
	done
	@for d in $(OUT)/Assets/*/; do \
		mkdir -p "$$d/common"; \
		cp $(RUNTIME)/os.bin "$$d/common/"; \
		$(STRIP) -o "$$d/common/$(APP).elf" app.elf; \
	done
	@echo "Ready: build/$(APP)/  (add DIABDAT.mpq + optional Hellfire MPQs to Assets/$(APP)/common on the SD)"

copy: release
	@$(SDK)/platforms/$(TARGET)/copy.sh "$(APP)" "$$(ls -d $(OUT)/Assets/*/common)/$(APP).elf" "$(POCKETDEV)"

package: release
	@$(ROOT)/scripts/package.sh $(APP)

debug: app.elf
	@$(ROOT)/scripts/debug.sh app.elf

clean:
	rm -rf $(OBJDIR) app.elf $(OUT)

print-srcs:
	@echo "DevilutionX sources: $(words $(DX_SRCS))"

.PHONY: all release copy package debug clean print-srcs
