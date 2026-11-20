cmake_minimum_required(VERSION 3.12)

# Pull in SDK (must be before project)
include(pico_sdk_import.cmake)

project(fuzix C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_BUILD_TYPE Debug)
# Execute from XIP flash.  Copying .text to RAM consumed ~87 KB of the
# 264 KB Pico RAM, squeezing the self-hosted compiler and forcing stack
# corruption workarounds.  Flash execution is fast enough for SPI-based
# console/LCD/SD and gives the user pool the RAM it needs.
set(PICO_COPY_TO_RAM 0)
option(TOTALMEM "specify how much TOTALMEM is present" 0)
# Linux-style warning baseline: keep the compiler honest so new code does not
# silently introduce bugs (the three real latent bugs fixed in K1 were only
# visible under -Wall/-Wextra).  -Wno-unused-parameter is waived because the
# FUZIX driver/vtable ABI requires unused parameters throughout.  A separate
# opt-in FUZIX_WERROR turns warnings into errors for CI / strict builds.
option(FUZIX_WERROR "treat compiler warnings as errors" OFF)

add_compile_definitions(
		PICO_HEAP_SIZE=0x0
		PICO_NO_BINARY_INFO=1
		PICO_TIME_DEFAULT_ALARM_POOL_DISABLED
    TOTALMEM=${TOTALMEM}
		)

pico_sdk_init()

include_directories(
	.
	../..
	../../dev
	../../include
	../../cpu-armm0
)

add_executable(fuzix
	devices.c
	devflash.c
	devsdspi.c
	devtty.c
	elf.c 
	main.c
	misc.c
	rawflash.c
	rawuart.c
	swapper.c
	tricks.S
	core1.c
	devgpio.c
	usbdescriptors.c
	devlcd.c
	devkb.c
	devtouch.c
	devfat.c
	../../dev/blkdev.c
	../../dev/mbr.c
	../../dev/devsd_discard.c
	../../dev/devsd.c
	../../lib/dhara/error.c
	../../lib/dhara/journal.c
	../../lib/dhara/map.c
	../../lib/hexdump.c
	../../blk512.c
	../../devio.c
	../../devsys.c
	../../filesys.c
	../../dev/fatfs/ff.c
	../../dev/fatfs/ffunicode.c
	../../dev/fatfs/diskio.c
	../../inode.c
	../../kdata.c
	../../cpu-armm0/lowlevel-armm0.c
	../../mm/malloc.c
	../../mm.c
	../../mm/memalloc_none.c
	../../process.c
	../../start.c
	../../swap.c
	../../syscall_execelf32.c
	../../syscall_exec.c
	../../syscall_fs.c
	../../syscall_fs2.c
	../../syscall_fs3.c
	../../syscall_other.c
	../../syscall_proc.c
	../../timer.c
	../../tty.c
    ../../vt.c
	../../usermem.c
	../../version.c
	../../dev/net/net_native.c
	../../syscall_net.c
	../../network.c
)

# Expose the standard Raspberry Pi USB reset interface so picotool can
# reboot the board into BOOTSEL mode without physical button access.
target_compile_definitions(fuzix PRIVATE
    PICO_ENABLE_USB_RESET_VIA_VENDOR_INTERFACE=1
    PICO_USB_RESET_SUPPORT_MS_OS_20_DESCRIPTOR=0
    PICO_USB_RESET_INCLUDE_DEFAULT_APP_DRIVER_CB=1
)

# Warning flags are applied to C sources only (Linux KBUILD_CFLAGS model);
# C++ sources in the target (TinyUSB glue) must not receive C-only warnings.
# The -Wno-* waivers cover legacy style categories that are noisy across the
# generic kernel but do not indicate bugs; each was triaged in MASTER_PLAN K1.
target_compile_options(fuzix PRIVATE
    $<$<COMPILE_LANGUAGE:C>:
        -Wall -Wextra -Wno-unused-parameter
        -Wno-sign-compare          # legacy off_t/uoff_t/int mixes across VFS
        -Wno-unused-value          # comma-expression macros (i_ref, _uget/_uput)
        -Wno-implicit-fallthrough  # switch fallthroughs in generic syscall code
        -Wno-pointer-sign          # char* vs uint8_t* in legacy string helpers
        -Wno-type-limits           # always-true range check in dhara/error.c
        -Wno-misleading-indentation
        -Wno-empty-body
        -Wno-old-style-declaration
    >
)
if(FUZIX_WERROR)
    target_compile_options(fuzix PRIVATE $<$<COMPILE_LANGUAGE:C>:-Werror>)
endif()

target_link_libraries(fuzix
    pico_stdlib
    pico_usb_reset
    pico_multicore
    hardware_flash
    hardware_spi
    hardware_i2c
    tinyusb_device
)

# The SDK's USB-reset driver calls rom_reset_usb_boot_extra() from core1 when
# the host asks for BOOTSEL.  Redirect it to our deferred handler in misc.c:
# the bootrom drives the watchdog that core0's tick feeds, so calling it
# directly from core1 never reboots (pico-sdk #2689).
target_link_options(fuzix PRIVATE "LINKER:--wrap=rom_reset_usb_boot_extra")

pico_set_float_implementation(fuzix none)
pico_set_double_implementation(fuzix none)
pico_add_extra_outputs(fuzix)

set_target_properties(fuzix PROPERTIES LINKER_LANGUAGE C)
