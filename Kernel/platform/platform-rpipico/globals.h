#ifndef GLOBALS_H
#define GLOBALS_H

/* The kernel binary is ~98 KB (~196 KB as a UF2); reserve 208 KB and give the
 * rest of the 2 MB QSPI flash to the Dhara FTL (firmware root + user
 * filesystem). */
#define FLASH_OFFSET (208*1024)

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

#endif

