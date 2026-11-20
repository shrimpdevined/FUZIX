#ifndef GLOBALS_H
#define GLOBALS_H

/* The kernel binary is ~104 KB (~196 KB as a UF2); reserve 208 KB and give the
 * rest of the 2 MB QSPI flash to the Dhara FTL (firmware root + user
 * filesystem). */
#define FLASH_OFFSET (208*1024)

/* MASTER_PLAN S5: reserved pre-FTL QSPI sector for the fatal-exception log.
 * The kernel image ends at ~0x19600 and the FTL starts at FLASH_OFFSET
 * (0x34000), so 0x30000 is a safe 4 KB erase block in the unused gap.  The
 * fatal handler captures the fault frame into RAM; plt_monitor() flushes it
 * here BEFORE the watchdog reset, so the record survives USB re-enumeration. */
#define CRASH_LOG_MAGIC      0x43525348UL	/* "CRSH" */
#define CRASH_LOG_FLASH_OFF  0x30000UL
#define CRASH_LOG_SECTOR     4096UL
#define CRASH_LOG_REC_SIZE   512UL

extern void flash_dev_init(void);
extern void sd_rawinit(void);

/* Kernel-stack overflow canary (MASTER_PLAN M1) */
extern void plt_stack_canary_init(void);
extern uint8_t plt_stack_canary_ok(void);

extern void contextswitch(ptptr p);

struct svc_frame
{
	uint32_t r12;
	uint32_t pc;
	uint32_t lr;
	uint32_t r0;
	uint32_t r1;
	uint32_t r2;
	uint32_t r3;
};

struct exception_frame
{
	uint32_t r0;
	uint32_t r1;
	uint32_t r2;
	uint32_t r3;
	uint32_t r12;
	uint32_t lr;
	uint32_t pc;
	uint32_t psr;
};

struct extended_exception_frame
{
	uint32_t r8;
	uint32_t r9;
	uint32_t r10;
	uint32_t r11;
	uint32_t cause;
	uint32_t sp;
	uint32_t r4;
	uint32_t r5;
	uint32_t r6;
	uint32_t r7;
	uint32_t r0;
	uint32_t r1;
	uint32_t r2;
	uint32_t r3;
	uint32_t r12;
	uint32_t lr;
	uint32_t pc;
	uint32_t psr;
};

/* MASTER_PLAN S5/S8: QSPI crash-log API (crashlog.c). */
struct pico_crashlog;
extern void crashlog_capture(struct extended_exception_frame *eh);
extern void crashlog_flush(void);
extern int crashlog_get(struct pico_crashlog *dst);

#endif

