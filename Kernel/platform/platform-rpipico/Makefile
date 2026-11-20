# Set one of these depending.
export PICO_SDK_FETCH_FROM_GIT = no
#export PICO_SDK_PATH = ${HOME}/.pico-sdk/sdk/1.5.1
export PICO_SDK_PATH = ${HOME}/Documents/fuzix-build/pico-sdk

# Distribution picotool packages are often older than the version the SDK
# requires.  Let CMake build the matching version into a stable cache so it
# is reused by subsequent builds (and so the SDK does not warn about it).
export PICOTOOL_FORCE_FETCH_FROM_GIT = 1
export PICOTOOL_FETCH_FROM_GIT_PATH = $(HOME)/.cache/pico-sdk/picotool-tools

# Set 'SUBTARGET' to one of pico-sdk compatible boards
# valid values: pico or pico2
ifndef SUBTARGET
# Keep the comment on its own line: a trailing space in this value breaks
# the TOTALMEM comparisons below.
SUBTARGET=pico
endif

ifeq ($(SUBTARGET),pico)
TOTALMEM=128
endif
ifeq ($(SUBTARGET),pico_w)
TOTALMEM=128
endif
ifeq ($(SUBTARGET),pico2)
TOTALMEM=320
endif
ifeq ($(SUBTARGET),pico2_w)
TOTALMEM=320
endif

export SUBTARGET

PICOTOOLFLAGS= --family rp2040
ifeq (${SUBTARGET}, pico2)
PICOTOOLFLAGS= --family data
endif

PICOTOOL=picotool

ifeq (, $(shell which picotool))
PICOTOOL=build/_deps/picotool/picotool
endif

# MASTER_PLAN K4: single source of truth for the flash layout used by the
# `image` target below.  QSPI_FLASH_KB and KERNEL_FLASH_KB mirror the 2 MB
# flash size and globals.h (FLASH_OFFSET = 208 KB) / config.h's partition map;
# the FTL size and UF2 load offset are DERIVED from them, so changing the
# kernel reservation in one place updates every consumer here.  Values are
# unchanged: FTL = 2048-208 = 1840 KB, UF2 = 0x10000000+0x34000 = 0x10034000.
QSPI_FLASH_KB   := 2048
KERNEL_FLASH_KB := 208
FTL_FLASH_KB    := $(shell expr $(QSPI_FLASH_KB) - $(KERNEL_FLASH_KB))
FTL_ERASE_BLK   := 0x1000
FTL_UF2_OFFSET  := $(shell printf '0x%X' $$((0x10000000 + $(KERNEL_FLASH_KB)*1024)))

include ../../../version.mk

build/fuzix.elf: ../../version.c build/Makefile
	$(MAKE) -C build

../../version.c:
	$(MAKE) -C ../.. TARGET=rpipico version.c
	@if [ "$(SUBTARGET)" = "pico2" ] || [ "$(SUBTARGET)" = "pico2_w" ]; then \
		sed -i 's/"rpipico"/"RP2350"/' ../../version.c; \
	else \
		sed -i 's/"rpipico"/"RP2040"/' ../../version.c; \
	fi

build/Makefile: CMakeLists.txt $(wildcard ../../*.[chS]) $(wildcard ../../*/*.[chS])
	mkdir -p build
	(cd build && cmake -DPICOTOOL_FORCE_FETCH_FROM_GIT=1 -DPICO_BOARD=${SUBTARGET} -DTOTALMEM=${TOTALMEM} ..)

image:: world
	./update-flash.sh
	../../../Standalone/mkftl -s $(FTL_FLASH_KB) -e $(FTL_ERASE_BLK) -g 10 filesystem.img -o filesystem.ftl
	${PICOTOOL} uf2 convert filesystem.ftl -t bin filesystem.uf2 -o $(FTL_UF2_OFFSET) ${PICOTOOLFLAGS}
	cat build/fuzix.uf2 filesystem.uf2 > fuzix_pico_complete.uf2

diskimage: image

clean:
	rm -rf build
	rm -rf sdcard
	rm -f uf2conv
	rm -f fuzix_pico_complete.uf2
	$(MAKE) -C ../../../Library/libs -f Makefile.armm0 clean
	$(MAKE) -C ../../../Applications/util -f Makefile.armm0 clean
	$(MAKE) -C ../../../Applications/V7/cmd -f Makefile.armm0 clean
	$(MAKE) -C ../../../Applications/V7/cmd/sh -f Makefile.armm0 clean
	$(MAKE) -C ../../../Applications/games -f Makefile.armm0 clean
	$(MAKE) -C ../../../Applications/levee -f Makefile.armm0 clean
	$(MAKE) -C ../../../Applications/cave -f Makefile.armm0 clean
	$(MAKE) -C ../../../Applications/cursesgames -f Makefile.armm0 clean
	$(MAKE) -C ../../../Applications/fcc -f Makefile.armm0 clean
	$(MAKE) -C ../../../Standalone clean
	$(MAKE) -C utils clean

world: build/fuzix.elf
	$(MAKE) -C ../../../Library/libs -f Makefile.armm0
	$(MAKE) -C ../../../Applications/util -f Makefile.armm0
	$(MAKE) -C ../../../Applications/V7/cmd -f Makefile.armm0
	$(MAKE) -C ../../../Applications/V7/cmd/sh -f Makefile.armm0
	$(MAKE) -C ../../../Applications/games -f Makefile.armm0
	$(MAKE) -C ../../../Applications/levee -f Makefile.armm0
	$(MAKE) -C ../../../Applications/cave -f Makefile.armm0
	$(MAKE) -C ../../../Applications/cursesgames -f Makefile.armm0
	$(MAKE) -C ../../../Applications/fcc -f Makefile.armm0
	$(MAKE) -C ../../../Standalone
	$(MAKE) -C utils
