#ifndef CONFIG_H
#define CONFIG_H
#include "tusb_config.h"

/* The kernel now runs XIP from QSPI flash with core1 parked by the multicore
 * lockout during flash operations, so kernel text no longer squeezes the
 * 264 KB of RAM.  The fixed tables below stay trimmed to leave ample headroom
 * for the core1 stack and the self-hosting compiler; a stack overflow would
 * otherwise corrupt the inode cache and open-file table, producing
 * "panic: corrupt inode"/"bad desc table". */
#ifndef OFTSIZE
#define OFTSIZE   56
#endif
#ifndef ITABSIZE
#define ITABSIZE  45
#endif
#ifndef PTABSIZE
#define PTABSIZE  16
#endif
/*
 * Set this according to your SD card pins
 *  CONFIG_RC2040
 *      SCK GPIO 14
 *      TX  GPIO 15
 *      RX  GPIO 12
 *      CS  GPIO 13
 *  CONFIG_MAKER_PI
 *	    SCK GPIO 10
 *	    TX  GPIO 11
 *	    RX  GPIO 12
 *	    CS  GPIO 15
 *  CONFIG_PICOCALC
 *	    SCK GPIO 18
 *	    TX  GPIO 19
 *	    RX  GPIO 16
 *	    CS  GPIO 17
 *  If Undefined
 *      SCK GPIO 2
 *      TX  GPIO 3
 *      RX  GPIO 4
 *      CS  GPIO 5
 */

#define CONFIG_RC2040

/* We have a GPIO interface */
#define CONFIG_DEV_GPIO
/* I2C CardKB keypad on GPIO0/1 (I2C0, addr 0x5F).  This is a LAPTOP — the
 * CardKB is the ONLY input device, so it is ALWAYS enabled (no runtime switch;
 * the init/poll call sites are unconditional).  A NACK on the I2C bus returns
 * in ~90us even with no key, so polling from the timer tick is safe. */
#define CONFIG_I2C_KEYBOARD 1
/* Enable to make ^Z dump the inode table for debug */
#undef CONFIG_IDUMP
/* Enable to make ^A drop back into the monitor */
#undef CONFIG_MONITOR
/* Enable to support network stack */
#undef CONFIG_NET
#undef CONFIG_NET_NATIVE
/* Profil syscall support (not yet complete) */
#undef CONFIG_PROFIL
/* Multiple processes in memory at once */
#define CONFIG_MULTI
/* 32bit with flat memory */
#undef CONFIG_FLAT
/* Pure swap */
#define CONFIG_BANKS 1
/* brk() calls pagemap_realloc() to get more memory. */
#define CONFIG_BRK_CALLS_REALLOC
/* Inlined irq handling */
#define CONFIG_INLINE_IRQ
/* Trim disk blocks when no longer used */
#define CONFIG_TRIM
/* Enable single tasking */
#define CONFIG_SWAP_ONLY
#define CONFIG_SPLIT_UDATA
/* Enable SD card code. */
#define CONFIG_SD
#define SD_DRIVE_COUNT 1
/* FAT filesystem support (devfat.c). */
#define CONFIG_FATFS
/* Enable dynamic swap. */
#define CONFIG_PLATFORM_SWAPCTL
/* Platform manages process brk. */
#define CONFIG_PLATFORM_BRK
/* Platform IOCTL on /dev/sys (maj:min)(4:6) */
#define CONFIG_DEV_PLATFORM

#define CONFIG_32BIT
#define CONFIG_USERMEM_DIRECT
/* Virtual terminal (LCD) is enabled below with CONFIG_VT. */

/* Built in NAND flash. Warning, it's unstable. */
#define CONFIG_PICO_FLASH

/* Program layout */

#define UDATA_BLKS  6
#define UDATA_SIZE  (UDATA_BLKS << BLKSHIFT)

/* Single source of truth for the memory split.  The RP2040 has 264 KB SRAM.
 * The kernel (page tables, buffers, FTL, console rings, 4 KB mkfs work area)
 * needs ~50 KB; the remaining ~210 KB is the user pool.  USERMEM is sized so
 * the kernel link leaves a few KB of headroom.  TOTALMEM/NETMEM are removed —
 * the old values (128/0) were dead after the #undef and only caused confusion. */
#define USERMEM (202 * 1024)

/* A self-hosted C compiler needs more than the stock 64 KB per process.
 * PROGSIZE is the per-process ceiling, leaving a guard for UDATA and the
 * kernel-side swap bookkeeping inside the user pool. */
#define PROGSIZE (182 * 1024 - UDATA_SIZE)
extern uint8_t progbase[USERMEM];
#define udata (*(struct u_data*)progbase)

/* rm/cpp/fcc and FatFs path handling need more than 4 KB of user
 * stack; overflow used to corrupt udata (fd table, inode pointers). */
#define USERSTACK (7*1024) /* 7 kB */

#define CONFIG_CUSTOM_VALADDR
#define PROGBASE ((uaddr_t)&progbase[0])
#define PROGLOAD ((uaddr_t)&progbase[UDATA_SIZE])
#define PROGTOP (PROGLOAD + PROGSIZE)
#define SWAPBASE PROGBASE
#define SWAPTOP (PROGBASE + (uaddr_t)alignup(udata.u_break - PROGBASE, 1<<BLKSHIFT)) /* never swap in/out data above break */
#define SWAP_SIZE   ((PROGSIZE >> BLKSHIFT) + UDATA_BLKS)
/* hda3 swap is 512 blocks (256 KB); SWAP_SIZE ~= 182 KB, so at most one
 * full process image fits.  Keep the array small and honest. */
#define MAX_SWAPS   2

#define BOOT_TTY (512 + 1)   /* Set this to default device for stdio, stderr */
                          /* In this case, the default is the first TTY device */

#define TICKSPERSEC 200   /* Ticks per second */
/* 
 * Boot cmd line.
 * [BOOTDEVICE] [tty=<TTYLIST>]
 * 
 * <BOOTDEVICE> - use `hda` for built-in flash or `hdbX` for SD card, where X is partition number
 * <TTYLIST> - list of TTY devices in order. If not specified system will
 *      map USB devices to tty1-4 and UART0 to tty5 if USB is connected. Or UART0 to tty1 etc if not.
 *      Example: `tty=usb1,uart1,usb2`
*/
#define CMDLINE	NULL	  /* Location of root dev name */

#define BOOTDEVICENAMES "hd#"

/* Device parameters */
#define NUM_DEV_TTY_UART 1 /* min 1 max 2*/
#define DEV_UART_0_TX_PIN PICO_DEFAULT_UART_TX_PIN
#define DEV_UART_0_RX_PIN PICO_DEFAULT_UART_RX_PIN
#define DEV_UART_1_TX_PIN 6
#define DEV_UART_1_RX_PIN 7
#define DEV_UART_1_CTS_PIN 8
#define DEV_UART_1_RTS_PIN 9
#define NUM_DEV_TTY_USB 4 /* min 1 max 4. */
#define NUM_DEV_TTY (NUM_DEV_TTY_UART + NUM_DEV_TTY_USB)

/* The generic default is only 132 bytes (CONFIG_LEVEL_2 is not enabled).
 * That is smaller than the shell's 256-byte edit buffer, so a burst of
 * typed input (or an XMODEM block) overflows the line discipline, drops
 * the terminating newline with a BEL and leaves the shell waiting for a
 * line that never completes — indistinguishable from a hung console.
 * Give each tty a full 512-byte queue; the extra 6*(512-132) bytes fit
 * comfortably in the RP2040's SRAM. */
#define TTYSIZ 512
/* USB console: no boot-time host-detect wait — core1/TinyUSB enumerate the
 * CDC asynchronously and the TX ring buffers early output. */

#define TTYDEV   BOOT_TTY /* Device used by kernel for messages, panics */
#define NBUFS    20       /* Number of block buffers */
#define NMOUNTS	 4	  /* Number of mounts at a time */

#define MAX_BLKDEV	4

#define CONFIG_SMALL

#define plt_copyright() /* */
#define swap_map(x) ((uint8_t*)(x))

/* Prevent name clashes wish the Pico SDK */

#define MANGLED 1
#include "mangle.h"

/* ============================================================================
 * 2 MB Onboard QSPI Flash Partitioning (Self-Contained, Zero SD Dependency)
 *   0x000000 - 0x033FFF (208 KB): Fuzix Kernel
 *   0x034000 - 0x1FFFFF (1,840 KB): Dhara FTL block device
 *       hda1: firmware root filesystem (essential OS + commands)
 *       hda2: user filesystem, mounted at /home
 *
 * The FTL presents a partitioned disk; the user filesystem is deliberately
 * separate so user data can never consume the firmware partition.
 * ============================================================================ */
#define CONFIG_FLASH_DISK

#define BOOTDEVICE          0x0001        /* hda1: firmware root */
/* Dynamic swap: SWAPDEV is set by the swapon() syscall.  /etc/rc enables
 * the hda3 partition (256 KB) at boot.  See update-flash.sh for the layout. */
#define SWAPDEV             (swap_dev)

/* ============================================================================
 * Hardware Pin Assignments
 * ============================================================================ */
/* Shared SPI0 Bus (Hosyond 3.5" ST7796S Display + SD Card) */
#define PIN_SPI0_MISO       16
#define PIN_SPI0_SCK        18
#define PIN_SPI0_MOSI       19
#define PIN_LCD_CS          17
#define PIN_SD_CS           21

/* ST7796S Control & Backlight */
#define PIN_LCD_RST         20
#define PIN_LCD_DC          22
#define PIN_LCD_BL          27

/* M5Stack CardKB (I2C0 @ 0x5F) */
#define PIN_KB_SDA          0
#define PIN_KB_SCL          1
#define I2C_KB_ADDR         0x5F

/* FT6336U Capacitive Touch (I2C1 @ 0x38) */
#define PIN_CTP_SDA         6
#define PIN_CTP_SCL         7
#define PIN_CTP_RST         8
#define PIN_CTP_INT         9
#define I2C_CTP_ADDR        0x38

/* ============================================================================
 * Virtual Terminal Geometry (480x320 Landscape using 6x8 Font = 80x40 Grid)
 * ============================================================================ */
#define CONFIG_VT
#define VT_WIDTH            80
#define VT_HEIGHT           40
#define VT_RIGHT            79
#define VT_BOTTOM           39
#define MAX_VT              1

#endif
// vim: sw=4 ts=4 et

