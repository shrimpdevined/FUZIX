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
	../../../Standalone/mkftl -s 1840 -e 0x1000 -g 10 filesystem.img -o filesystem.ftl
	${PICOTOOL} uf2 convert filesystem.ftl -t bin filesystem.uf2 -o 0x10034000 ${PICOTOOLFLAGS}
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
