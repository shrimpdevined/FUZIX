# FUZIX RP2040 — Stability & Cohesion Master Plan

**Status:** Authoritative source of truth for all changes. Last updated 2026-11-19.
**Origin:** Consolidated from 10 parallel research agents (8 complete, 2 pending) plus
the prior `COMPILER_BUGS.md` audit (now deleted; its still-relevant items are folded
below). Every finding carries file:line evidence. Do not start a change that is not
tracked here; mark it done only after its acceptance test passes.

---

## 0. Rules of engagement

- Warning-free builds; no new warnings from `-Wall -Wextra`.
- **Git workflow (standard agentic-AI practice):** commit each verified, focused change
  with a descriptive message (issue ID + what/why); push to `origin` regularly so work is
  backed up off-machine and visible; rebase/squash freely on local branches to keep a
  clean, reviewable history; never force-push to a shared branch without coordination.
- Preserve the user's uncommitted work. Never delete tracked upstream `.md` docs.
- Stream mode stays removed; block mode with 64-byte paced writes is the only transfer path.
- Keyboard (CardKB) must always work — it is the only input on the laptop.
- Full conformance (`fcctest4.py`) runs **once, at the very end**, after cleanup and app reload.
- Serial console and LCD must mirror each other with **zero output-order differences**.
- Host tooling waits for a full prompt before sending the next line.

---

## 1. Why the system feels "hacked together" — the three structural gaps

The single biggest insight across all agents: the problems are **not mostly style**. They
trace to three missing first-class abstractions. Every recent stability fix was a
workaround for one of them.

1. **No first-class console object.** Output targets the literal minor `1` in
   `devtty.c:95-106,108-135`, `devices.c:78-80`, `devkb.c:28-32,55-73`. `TTYDEV` exists
   (`config.h:169`) but is not used for output; the `tty=` option is parsed then ignored.
   One ordered byte stream is fanned out to two independent queues with independent drops.
2. **No generic deferred-work facility.** Slow VT/LCD rendering hangs entirely off
   `plt_idle_hook()` (`devtty.c:209-219`), which runs only when no process is runnable
   (`process.c:149-154`). Under compiler load the LCD freezes and the VT ring drops bytes.
3. **No device model with `probe()`.** Stubbed drivers (e.g. `rawuart.c:5-13`) register
   and are mapped to real devices; state is scattered in globals; no probe/remove/refcount.

Linux's useful essence here is the **contracts** — one registered console, one queue
primitive with flush/cancel, probe/remove that verifies hardware, generated config —
NOT the implementations. Do **not** port lockdep/sysfs/kobjects/CFS into 264 KB SRAM.

---

## 2. Master issue register

Severity: **P0** = ship-blocker (crash / data loss / forensics black hole), **P1** = high,
**P2** = medium, **P3** = low / cosmetic. Status: `[ ]` open, `[x]` done, `[~]` in progress.

### C — Console / output / LCD

| ID | Sev | Finding (evidence) | Fix | Test |
|----|-----|--------------------|-----|------|
| C1 | P0 | One byte stream → two queues with **different capacities** (USB `wbuf` 512 @ `core1.c:38-40`; VT `vtd_buf` 4096 @ `vt.c:90-96`) and **independent silent tail-drops** (`core1.c:200-201`, `vt.c:245-249`). A dropped `\r`/`\n` terminator makes the next prompt append on the same line. **CONFIRMED ON DEVICE (Phase 0 test):** LCD showed `devops@RP2040:~$` twice on one line after `picoctl diag` output while serial showed one prompt per line; `vt_dropped=0` so bytes were not lost — divergence is in VT cursor/LF rendering (C3), not queue drops. | `console_fanout(c)`: under one `di()`, check **both** rings for space first; enqueue to both, or drop from both with a single shared `console_dropped` counter. Equalize ring sizes. Fix C3 (VT `charout` LF must reset column / CRLF centralized). | Burst >512 B with host stalled: serial and LCD byte streams byte-identical; no prompts stacked on one LCD line. |
| C2 | P0 | `tty_writeready(1)` always `TTY_READY_NOW` (`devtty.c:139-150`, commit `484b51d`) → `tty_write()` never sleeps → suffix drops. This *introduced* C1's drop window. | Resolve with the deferred/liveness fix (D1) so lossless blocking is safe, then restore honest readiness; or formally accept symmetric best-effort (C1) and document it. | `getpass()` prints full `"Password: "` under host-not-draining; no partial prompts. |
| C3 | P1 | CR/LF normalization split 3 ways: `kputchar` always inserts `\r` (`devtty.c:103-105`); `tty_write` only with `OPOST\|ONLCR` (`tty.c:150-156`); echo only with `OPOST\|ONLCR` (`tty.c:470-473`). Bare LF in VT moves down **without** resetting column (`vt.c:136-139`). | One `console_emit(c)` owns `\n→\r\n`; all three callers use it; `kputchar` becomes one atomic emit; `charout()` LF zeroes `cursorx`. | Mixed kernel + echo + userspace output lands on correct columns on both sinks. |
| C4 | P1 | No `/dev/console` abstraction; console is magic minor `1`; `tty=` config ignored for kernel output. | Introduce small `struct console` (name, write, minor, setup/exit) registered once; derive minor from `TTYDEV & 0xFF`; replace literal `1`s. | `tty=` remap routes kernel messages correctly; no literal console minor in output paths. |
| C5 | P2 | `/dev/fb0` gfx mode purges queued VT text (`devlcd.c:306-311`, `vt.c:254-259`) and clears shadow on exit; inverse attr stored inconsistently (`devlcd.c:146,155,184`); `cursor_on/off` are no-ops (`devlcd.c:233-237`). | On gfx entry purge **both** sinks + resync VT/shadow; store inverse bit in `vt_shadow`; document no-cursor or add a visible one. | Serial/LCD stay in sync across a framebuffer app run. |
| C6 | P1 | `kputchar` emits `\r` and `\n` as two `tty_putc` calls (`devtty.c:104-105`); an IRQ echo can interleave between them. | Single atomic emit (C3). | No stray characters between CR and LF in stress logs. |

### D — Deferred work / concurrency / core1

| ID | Sev | Finding (evidence) | Fix | Test |
|----|-----|--------------------|-----|------|
| D1 | P0 | VT/LCD drained **only** from `plt_idle()` (`devtty.c:209-219`, `tricks.S:10-15`, `process.c:149-154`) = only when `nready==0`. Compiler load → LCD **freezes**, `vtd_buf` fills and drops (`vt.c:96,245-249`). No tick fallback; `u_ininterrupt` guard blocks naively adding one. `vt_task` not self-rearming (`:263-278`). | `deferred_pending` bitmask (VT/LCD/FLUSH) set on enqueue; `deferred_run(budget)` with self-requeue; drain from idle **and** a deferred worker task (model on `swapper()`, `process.c:1068-1083`) that participates in scheduling. SPI rendering stays in thread context. | Hold `fcc` compile for 60 s: LCD keeps updating, `vtd_dropped` stays 0. |
| D2 | P1 | `vtoutput()` reentry enqueues only `*p` (one byte) regardless of `len` (`vt.c:289-294`); `vtbusy` cleared without `di()` (`:340`). Latent byte loss. | On this platform all output already goes through `vtdefer()` — remove `vtbusy` reentry path behind `CONFIG_VT_DEFERRED`, or enqueue whole `p[0..len-1]` in FIFO order and clear under `di()`. | Assert `vtbusy` never observed set; no byte loss in echo stress. |
| D3 | P1 | `lcd_task()` checks bus busy once, but `lcd_redraw_row()` rechecks and can skip; `row_dirty[y]` cleared unconditionally (`devlcd.c:282-288`) → lost repaint. | `lcd_redraw_row()` returns bool; clear flag only on success; snapshot dirty bitmap under `di()`. | SD activity + LCD output: no permanently stale rows. |
| D4 | P1 | Flush handshake `flush_req`/`flush_ack` written by both cores with a lost-request race + 200k-iteration busy spin (`core1.c:53-54,120-156,279-285`). | Sequence protocol: `flush_seq`/`flush_done_seq` published with fences; core0 waits via `psleep`/`wakeup` (completion analogue), not spin. | Rapid `tcflush()` under load: no lost flush, no wasted spin. |
| D5 | P2 | SPSC `__dmb` barriers are **correct** on M0+ (`core1.c:85-105`); bounded `multicore_lockout_start_timeout_us` avoids classic cross-core deadlock (`rawflash.c:20-33`). | **Preserve unchanged.** Optionally wrap in named `smp_store_release`/`smp_load_acquire`; publish `core1_ready`/`heartbeat` with release fence. | No regression in existing lockout/USB tests. |

### S — Kernel crash / hang / stability

| ID | Sev | Finding (evidence) | Fix | Test |
|----|-----|--------------------|-----|------|
| S1 | **P0** | **Core1 heartbeat false-positive reboot.** Flash erase/program, Dhara GC and especially `CONFIG_TRIM` swap-in (one `HDIO_TRIM` per block, ~364 trims) park core1 in `multicore_lockout_handler` (`rawflash.c:25-57`); heartbeat stops; core0 reboots chip after 2 s (`devices.c:95-104`). Most plausible "random reboot under compiler load". | Export `flash_lockout_active` volatile flag around lockout start/end; **skip `stale_ticks` while set**, reset on end; raise threshold to 4-5 s and require ≥2 consecutive observations. Batch/disable per-swap TRIM. | Sustained `fcc`/swap workload: zero unexpected reboots; genuine core1 wedge still reboots within ~5 s. |
| S2 | P0 | Tick runs entirely under `di()` (`devices.c:60`) incl. I2C read (up to 5 ms), 5×512 B tty drain, alarm walk. Single MSP stack (no PSP split) → exception frames land on the shallow per-process kernel stack. | Move `watchdog_update()` to top of tick; move `devkb_poll()` + tty drain into deferred context (D1/I1); keep timekeeping + preemption in tick. Add IRQ-off duration detector. | Max IRQ-off window measured <1 ms under input spam; watchdog never delayed. |
| S3 | P1 | Per-process kernel stack grows down into live `u_data` (`tricks.S:65-66`, `kernel.h:542-618`, ~2.7 KB headroom), no MPU (M0+) / canary / high-water check — the historical `panic: corrupt inode` mechanism. Core1 stack only 2 KB (`PICO_STACK_SIZE=0x800`). | Fill stack with watermark at context switch; scan in idle/tick; warn before corruption reaches `u_files[]`. Bump `PICO_CORE1_STACK_SIZE=0x1000`. | Overflow injection test warns/resets cleanly; no inode corruption under deep syscall + tick. |
| S4 | P1 | Core1 HardFault shares `fatal_exception_handler_trampoline` and resets SP to `__StackTop` (`tricks.S:19-34`), the same stack core0's swapper uses (`tricks.S:119-120`) → cross-core corruption. | Separate core1 fault handler: write fault code to shared `.bss`, `watchdog_reboot()` directly; never touch core0 stack. | Inject core1 fault: core0 state intact until controlled reset. |
| S5 | P1 | Panic/fault state lost across watchdog reset (RAM survives but nothing recorded) → "random crashes" undiagnosable (`main.c:66-78`, `process.c:1047-1054`, `misc.c:31-40`). | `.bss` no-init record: magic, cause, PC/LR/SP, pid, tick; store in fault/panic; print at boot then clear. (Feeds logging, L1.) | Force panic → reset → boot prints last fault cause. |
| S6 | P2 | `flash_lockout_start` fails if core1 in TinyUSB critical section (`rawflash.c:25,48`) → `DHARA_E_TOO_BAD` write failure; `end_blocking()` unbounded (`:39,57`). | Bounded retry/backoff on start; use `end_timeout_us()`; log + reset on timeout. | USB input spam during fs write: no spurious write failures. |
| S7 | P2 | Tick re-arm uses `static now` set once at `devices.c:125` and never advanced (`devices.c:64,106-110`) → primary alarm target always in the past, relies on fallback. Watchdog doc says 8 s but code is 5000 ms (`main.c:126-132`). | Advance `now` each tick; fix comments to 5 s. | Tick cadence stable; comments match code. |

### I — Input (CardKB / touch / line discipline)

| ID | Sev | Finding (evidence) | Fix | Test |
|----|-----|--------------------|-----|------|
| I1 | P0 | `devkb_poll()` does blocking I2C read in hard IRQ with interrupts off (`devices.c:60,66`, `devkb.c:49`); SDK has an **unbounded** `while(!i2c_get_write_available())` spin (`pico-sdk .../i2c.c:291-292`). Wedged TX FIFO → lockup until watchdog. | Set `kb_poll_pending` in tick; do the actual I2C read in deferred/thread context (`plt_idle_hook` or worker). If it must stay in IRQ, wrap with a hard deadline and cut timeout to ~1 ms. Add I2C-timeout counter. | ≥200 keys/s + USB spray for minutes: no lockup, keyboard responsive. |
| I2 | P1 | CardKB has no source backpressure; injects into full `ttyinq` (only BEL), ignores `tty_inproc()` return (`devkb.c:57,60-72`); `tty_data_consumed()` empty (`devtty.c:173`). | Small CardKB SPSC ring drained in `tty_interrupt()` like USB; throttle at high-water mark, clear from `tty_data_consumed()` low-water (Linux `tty_throttle`/`unthrottle`). | Input spray: no silent key loss; keyboard recovers after drain. |
| I3 | P2 | Arrow keys inject 3 bytes via 3 separate `tty_inproc()` calls, non-atomic (`devkb.c:28-32`) → truncated `ESC [` if queue fills. | `tty_inproc_buf(minor, buf, len)` reserves `len` slots under one `di()`. | Arrow keys under load produce intact escape sequences. |
| I4 | P1 | `TCSETSF` clears queue but not core1 staging ring (`tty.c:295-297`); `tty_flushinput()` passes tty minor straight as USB number (`core1.c:120-123`) → remapped console flushes wrong device. | Make `TCSETSF` call `tty_flushinput()` (as `TIOCFLUSH` does at `:318-325`); map via `ttymap[minor]`; make flush spin time-based. | `tcsetattr(TCSAFLUSH)`/type-ahead: no stale bytes at next prompt. |
| I5 | P2 | Touch is a disconnected snapshot read, no event queue, `CTP_INT` (GPIO9) unused (`devtouch.c:17-19,44-72`); keyboard bypasses staging ring. No Linux-like input core. | Small evdev-like `input.c` shared by both devices (`input_event{type,code,value,ts}`, per-device ring + drop counter, `input_sync`); keyboard/touch become transports; optional `/dev/input/event0`. | Both devices report through one core; touch taps not lost between reads. |
| I6 | P2 | `TIOCINQ` returns raw `q_count`, not bytes to next newline in ICANON (`tty.c:316-317`); canonical `select()` is a FIXME (`:367-370`); `VSUSP` unimplemented; spurious BEL on `VEOF` (`:476`). | Fix ICANON semantics, `SELECT_IN` readiness, `VEOF` echo condition; implement `VSUSP` if job control enabled. | `select()`/`read()` agree; no BEL on ^D. |

### F — Filesystem / block / SD

| ID | Sev | Finding (evidence) | Fix | Test |
|----|-----|--------------------|-----|------|
| F1 | P1 | `sync()` no real device flush, errors discarded; `d_flush()` is a no-op ioctl; flush callback removed from Dhara (`devflash.c:89-96`, `inode.c:379-414`, `devio.c:396-406`). Justification (per-buffer GC) was really a *call-site* bug. | Write all dirty buffers, then call device `flush()` **once per device** at end of `sync()`; re-arm `dhara_map_sync()` checkpoint once per sync (throttled), not per buffer. | No `DHARA_E_MAP_FULL`; bounded journal replay at boot; sync errors visible. |
| F2 | **P0** | `fsck -a` does **not** imply auto-yes (`fsck.c:168-193`); `rc:11` runs `fsck -a /dev/hda2 >/dev/null 2>&1` → invisible prompt + waits forever on dirty fs → boot hang. Dirty root forced r/o then `remount -n / rw` anyway (`filesys.c:1412-1415`, `rc:5`). | Make `-a` set `yflag` (Linux `fsck -a`); check exit status before remount r/w; on unfixable corruption drop to visible recovery shell or mount r/o. | Dirty hda2: boot reaches getty with no hidden prompt; corruption is visible. |
| F3 | P1 | No write ordering in `sync()`: inodes → superblocks (clean) → bufsync, no barrier (`inode.c:387-413`). Dhara journal (FTL rollback) and Fuzix `s_fmod` dirty bit are uncoordinated. | Order: inodes → flush → superblocks clean → flush → bufsync+flush. Only flip `s_fmod` clean after final flush succeeds. | Crash mid-sync: no clean-looking fs with rolled-back data. |
| F4 | P2 | FAT special-cased by magic `m_flags & 0x80` spread across `inode.c:396, filesys.c:981, syscall_other.c:483,589, devfat.c:53,71`; `blkdev_rw()` returns -1 without `u_error` (`blkdev.c:98-125`); one global `blk_op`. | Add `m_type`/ops vector; typed errors from block layer; surface `dhara_error_t`. | FAT and native fs dispatch uniformly; SD/Dhara errors diagnosable. |
| F5 | P1 | Shared SPI0 (SD vs LCD) is a cooperative flag (`sd_drive`, `devlcd.c:44-46`, `devsd.c:48-52`); no explicit owner; `devfb_write` returns EBUSY; SD retry up to 8×3 s (`devsd.c:54-64,115-134`) can exceed 5 s watchdog. | Explicit `spi0_owner` claim under `di()`; LCD worker re-queues on contention; cap total SD retry < watchdog. | Heavy SD + LCD: no `sd: timeout` spiral, no watchdog reset. |

### B — Boot / init

| ID | Sev | Finding (evidence) | Fix | Test |
|----|-----|--------------------|-----|------|
| B1 | P0 | Console is USB-only and best-effort; no serial fallback, no `/dev/console` → every boot problem is a forensic black hole (`devtty.c:95-106`, `core1.c:66`). Durable log is the single highest-value fix. | QSPI boot log from `devtty_early_init` onward + `console=uart\|usb\|both` early flag; surface `usbconsole_tx_dropped` to userspace. (See L1.) | No-host boot + later crash: full staged log recoverable. |
| B2 | P1 | `/bin/firstboot` interactive in `rc:23` blocks getty unattended; getty respawn has no limit (`init.c:193-220,608-610`); unbounded `open("/dev/tty1")` loop. | Move firstboot to first login or add timeout/safe default; add sysvinit-style respawn limit ("respawning too fast") + bounded tty open with diagnostic. | Unattended boot always reaches getty; dying getty doesn't fork-loop. |
| B3 | P1 | `doexit()` calls `sync()` unconditionally when "due" (`process.c:933-948`, `EXIT_SYNC_TICKS=2000`=10 s) with no check for dirty buffers → ~5 s shell-exit gap; mount also syncs (`filesys.c:1429`). | Walk `bufpool` for `BF_DIRTY`; skip if none; remove mount-time sync (FTL journals metadata). | Shell exit returns to prompt immediately on clean sessions. |
| B4 | P2 | `devlcd_init()` ~240 ms `busy_wait_ms` with IRQs off before EI (`devlcd.c:101-133`, `main.c:120-122`) — purely visual. | Defer non-essential LCD init past `__hard_ei()`; keep only 120 ms Sleep Out. | Faster console availability; no boot bytes dropped to LCD delay. |

### K — Build / config / dead code

| ID | Sev | Finding (evidence) | Fix | Test |
|----|-----|--------------------|-----|------|
| [x] K1 | P0 | "Warning-free" was silence — no `-Wall/-Wextra`. 43 warnings incl. 3 real bugs (uninit `i`, unused `symtab`, weak-address test). | Enabled `-Wall -Wextra -Wno-unused-parameter` on C only + opt-in `FUZIX_WERROR`; fixed the 3 bugs; waived 40 legacy style warnings with per-category `-Wno`. **DONE** commit `207bd96`. | Clean default + `-Werror` builds, 0 warnings. ✅ |
| [x] K2 | P1 | `CONFIG_DEV_PTY` block dead + uncompilable; guard mismatch. | Deleted the block + dead branch + `tty.h` decls; kept live `CONFIG_PTY_DEV`. **DONE** commit `0ede38e`. | Clean build; no dead PTY symbols. ✅ |
| [x] K3 | P2 | `usbconsole_tx_space()` unused; `extern` on definitions; `USBCONSOLE_H` misnamed guard. | Removed dead fn+decl; dropped redundant `extern`; guard → `CORE1_H`. **DONE** commit `a28d949`. | Clean build. ✅ |
| [x] C6 | P1 | `kputchar` emitted `\r`/`\n` as two `tty_putc` calls → IRQ echo can interleave. | Wrapped CRLF pair in one `di()`/`irqrestore()` atomic unit. **DONE** commit `2318118`. | No stray char between CR/LF. ✅ |
| [x] S7 | P2 | Tick re-arm used stale init-time `now`; watchdog comments said 8 s (code 5 s); `TTY_READY_LATER` doc said -1 (enum 2). | Advance `now` from `time_us_64()` each tick; fixed all three stale comments. **DONE** commit `326e78a`. | Primary alarm path works; comments match code. ✅ |
| [x] L2 | P2 | Drop counters write-only/invisible. | Added `PICOIOC_DIAG` ioctl returning `struct pico_diag`; `vt_dropped_count()` accessor. **DONE** commit `0b5d749`. | Counters readable via /dev/sys minor 6. ✅ |
| K3 | P2 | `usbconsole_tx_space()` unused (`core1.c:180-187`, `core1.h:11`); `core1_state` write-only; weak `tty_flushinput` no-op + address test; `extern` on definitions; `USBCONSOLE_H` misnamed guard. | Delete dead fn/decl; remove redundant `extern`; rename guard; document retained diagnostics. | No unused symbols; clean build. |
| K4 | P2 | Duplicated magic/sources of truth: flash 208 KB/1840 KB split in `config.h`, `globals.h:7`, `Makefile:65-66`; `TOTALMEM` "removed" in `config.h:105` but still injected by CMake; `CONFIG_CONSOLE_BUF_SHIFT 9` vs VT 12; spin limits 200000/500000; `config.h`↔`tusb_config.h` circular include; 26 copies of `used(x)`. | Define each constant once in `config.h`, derive Makefile/CMake; break circular include; one `used()`; named spin limits. | Single definition per constant; comment/code agreement. |
| K5 | P2 | No device model; stubbed `rawuart` registered as real driver (`rawuart.c:5-13`, `devtty.c:30,84`); no `probe`/`remove`. | Add minimal `probe`/`remove` to `struct ttydriver`; `rawuart_probe()` fails → leave unregistered. | No dead UART device; graceful absent-hardware handling. |

### L — Logging (QSPI → SD rotation) — *deep-dive complete*

All kernel messages converge on **one choke point**: `kprintf()` → `kputs()`/`kputchar()`
(`Kernel/devio.c:659,600`), with `kputchar()` overridden per-platform at
`devtty.c:95`. Input echo goes through `tty_interrupt()`→`tty_putc()`, **not**
`kputchar`, so echoes won't pollute the log. This makes a Linux-`printk`-style hook clean.

| ID | Sev | Finding / requirement | Fix | Test |
|----|-----|----------------------|-----|------|
| L1 | P0 | No persistent log; crashes leave zero evidence. Requirement: log on QSPI, transfer to SD when too big, Linux-style rotation. | `klog.c` RAM ring → flush 512 B records to a **reserved pre-FTL QSPI gap** (not the FTL — Dhara has ~18 sectors headroom only) → `klogd` rotates to `/var/log/klog.1..N` and archives to `/mnt/sd/log/`. Full design below. | Crash leaves timestamped staged log; rotation to SD; survives power cycle. |
| L2 | P2 | Drop/liveness telemetry not observable (`usbconsole_tx_dropped`/`rx_dropped` `core1.c:66-67`, `vtd_dropped` `vt.c:96`, keyboard I2C timeouts, core1 heartbeat). | `KLOGIOC` ioctls + periodic log summary + `dmesg`-style peek. | Counters readable from userspace; anomalies visible. |

**L1 design details (authoritative):**

- **Record format (512 B = 1 flash page):** magic `FLOG`@0, `seq`@4, `ts_low/high`@8/12, `len`@16, `sev`@18 (0=EMERG..7=DEBUG), `src`@19 (1=KERN 2=PANIC 3=EXCEPTION 4=USER 5=WATCHDOG), `flags`@20, `crc16`@22, payload@24 (488 B). `seq` matters more than wall time — `setdate -y` (`rc:4`) resets the clock on reboot; document timestamps as "monotonic since boot."
- **RAM ring:** `static struct klog_rec klog_ring[KLOG_NREC]` in `.bss`; default 8 records = 4 KB. **Measured free RAM is only ~2.6 KB** (`fuzix.elf.map`: `progbase 0x20008c98`, `__bss_end__ 0x2003f5b0`, core1 stack `0x20040000`) → must shrink `USERMEM` (`config.h:106`) from 202 KB to 198 KB. Recheck map every stage.
- **QSPI region (pre-FTL gap, costs zero FTL capacity):**
  `0x0002C000..0x00033FFF` = 32 KB = 8 erase blocks = 64 pages. Kernel image is 103,224 B so it fits below `0x2C000`; add a Makefile assert `test $(stat -c%s build/fuzix.bin) -le 176128`. Define `KLOG_FLASH_START/LEN` in `globals.h` near `FLASH_OFFSET` (`:7`). Do **not** shrink `nand.num_blocks` (`devflash.c:19`).
- **Hook edits:** `devtty.c:95` `kputchar` calls `klog_putc()` (normalize CRLF — don't store the injected `\r` twice); `process.c:1047` `panic()` calls `klog_begin(EMERG,PANIC)` + writes crash signature to `watchdog_hw->scratch[]`; `main.c:66` `fatal_exception_handler` stashes pc/lr/cause in scratch; `devices.c:95-104` logs before the core1-heartbeat `watchdog_reboot`; `main.c:96` detects `watchdog_caused_reboot()` and emits a "previous boot ended in watchdog reset" record from scratch.
- **Recursion/deadlock safety:** `klog_putc()` uses only `di()` + fixed array + `irqrestore()`, never printf/file/flash; `klog_in_hook` guard drops reentrant bytes. Flash writes happen **only** from `klog_deferred_flush()` in `plt_idle_hook()` (`devtty.c:213`, IRQs enabled) or `KLOGIOC_FLUSH` in process context — never from IRQ/panic/exception. Model flash sequence on `rawflash.c:22-73`: `multicore_lockout_start_timeout_us(200000)` (never `_blocking()`), `di(); flash_range_program(); irqrestore(); lockout_end()`. Bound to **one 512 B page per call**.
- **Wear leveling:** append sequentially through 64 pages; 4-byte epoch header per 4 KB block; on full, erase lowest-epoch block and reset append pointer there; `seq` restores chronological order; CRC skips torn pages.
- **Userspace device:** `/dev/klog` = major 4 minor 128 (within `/dev/sys`; `devsys.c:30` reserves 128+, `validdev` auto-covers it via `sizeof(dev_tab)`). `mknod klog 20644 1152`. ioctls: `PEEK/ DRAIN / FLUSH / LEVEL / CLEAR / REPLAY` (`0x4B01..06`). `sys_write` accepts `logger(1)`-style records.
- **`klogd` daemon** (`Applications/util/klogd.c`, `kl::respawn:/bin/klogd` in inittab): startup `KLOGIOC_REPLAY`; loop `KLOGIOC_DRAIN` + `sleep(5)`, append to `/var/log/klog` on hda2 (`O_APPEND`); rotate at ~64 KB or daily (`klog.2→3…`, `klog→klog.1`, retention 4), `CLEAR` only after success; archive `.1` to `/mnt/sd/log/klog.<bootseq>.<ts>` (retention 8); on watchdog reboot write a synthetic "BOOT after watchdog reset; scratch=…" record. No gzip in image → plain text v1 (optional RLE later). `klogd -p` = `dmesg`.
- **Stages:** (0) instrumentation only + hook + USERMEM shrink; (1) device + daemon; (2) rotation + SD offload; (3) persistent flash ring; (4) crash context + watchdog signature; (5) hardening (1-page bound, coalescing, soak).
- **Open risks:** exact RAM headroom (shrink USERMEM, recheck map); kernel-size assert mandatory; `setdate -y` timestamp semantics; no compression binary; Dhara capacity forces pre-FTL gap.

### M — Memory / process isolation — *deep-dive complete*

**Verified physical map (from `fuzix.elf.map`):** `of_tab` ends at `0x20008c90`, 8 bytes
below `progbase` (`0x20008c98`); `ptab` starts 4 bytes after the user pool ends. There is
**zero guard** between kernel globals and the user pool. `CONFIG_32BIT` + flat/direct
usermem + ARMv6-M (no MPU on RP2040, no unprivileged mode) means **no hardware isolation
at all** — both kernel and user run privileged. Real isolation requires RP2350 (Cortex-M33
+ MPU); record this as a hardware limitation (P3).

| ID | Sev | Finding (evidence) | Fix | Test |
|----|-----|--------------------|-----|------|
| M1 | P0 | Syscall kernel stack starts at `udata+UDATA_SIZE`=`0x20009898` and grows **down into live `u_data`** (`tricks.S:65-66`, `kernel.h:542+`); a deep fcc/FatFs chain overwrites `u_files[]`/`u_cwd`/`u_root` → the historic `corrupt inode`/`bad desc table`. No canary/high-water/`-fstack-protector`. | Canary word at top of `u_data`; high-water sentinel fill at `makeproc`/`create_init`, scan at syscall entry (`process.c:548`) + tick (`devices.c:58`); early panic before corruption. | Inject deep recursion → clean warning/reset, no inode corruption. |
| M2 | P0 | `valaddr()` **clamps** instead of rejecting (`misc.c:58-69`); `ugetw/ugetl/uputw/uputl` accept 1–3 bytes short of `p_top` (`usermem.c:64-142`); broken dead `MISALIGNED` blocks; `rargs` argv walk unbounded (`syscall_exec.c:26-37`). | Reject when `base+size > p_top` (Linux `access_ok`); require `valaddr>=2/4`; fix/remove `MISALIGNED`; bound argv walk. | `valaddr` edge tests: all oversized ranges return EFAULT, no OOB. |
| M3 | P0 | `brk_extend()` accepts `addr` in `[PROGBASE,PROGLOAD)` (inside udata) and no `PROGTOP`/block-count clamp (`swapper.c:410-425,171-220`); `u_break` set before `uzero` (`syscall_proc.c:264-269`). | Reject `addr<PROGLOAD || addr>PROGTOP` and `blocks>NUM_ALLOCATION_BLOCKS` before loop; set `u_break` last. | `brk` bounds tests: no inconsistent state, no swap thrash on absurd request. |
| M4 | P1 | User stack (7 KB, `config.h:117`) underflows below `PROGBASE` into `of_tab`/`i_tab`; unmeasured (no `-fstack-usage`). | Sentinel fill at exec (`syscall_execelf32.c:306-308`), high-water check in tick; consider 8 KB if fcc needs it. | `-fstack-usage` report + runtime headroom >0 under fcc/cpp. |
| M5 | P1 | Swap: `SWAP_SIZE` 364 blocks × `MAX_SWAPS` 2 = 728 > hda3's 512; second map I/O silently ignored (`swapper.c:369,399`); `panic("swapin: no free block")` kills whole system; `swapread/write` returns ignored. | `MAX_SWAPS 1`; check swap I/O returns; replace OOM panic with controlled process kill/reset + log. | OOM under compile: no whole-system panic; swap I/O failures logged. |
| M6 | P1 | ELF loader: no `p_filesz<=p_memsz` check (`syscall_execelf32.c:207-239`), no lower-bound/align on relocations (`:269-284`), `readi` results ignored. | Validate fields, lower-bound relocations, check read results. | Malformed ELF rejected cleanly, no stale-memory execution. |
| M7 | P2 | `kmalloc()` pool compiled but `kmemaddblk()` never called → always NULL (`mm/malloc.c:110-127`). | Wire up pool or remove reliance. | No driver silently fails on kmalloc. |
| M8 | P2 | No `BUG_ON`/`WARN_ON` assertion framework; canaries/stack protector absent despite explicit SP switch points (`tricks.S:65,119,182`). | Add macros + `-fstack-usage`; `-fstack-protector-strong` with seeded canary + `__stack_chk_fail`. | Assertions fire at invariants; build clean. |
| M9 | P2 | OFT 56 / ITAB 45 / PTAB 16 tight for full toolchain (~30-50 fds); ~2.7 KB global slack available. | Raise to 64-72 / ~64 / ~20 if load tests show ENFILE/EAGAIN; verify map stays under `0x20040000`. | Full cpp+cc1+as+ld chain with no ENFILE/EAGAIN; map fits. |

**Bottom line:** historic panics are fully explained by M1 (syscall stack into udata) and
M4 (user-stack underflow into of_tab) — both unguarded because RP2040 has no MMU/MPU.
Realistic target is **early detection + deterministic watchdog recovery**; true isolation
needs RP2350. Credit existing hardening: `uget/uput` range enforcement, pagemap rollback,
`swapin` NULL check, `getinode()` corruption recovery, FDCHECK, watchdog.

---

## 3. Implementation phases (strict order — do not skip ahead)

Each phase ends with its acceptance test. Only mark issues `[x]` when the test passes.

### Phase 0 — Cleanup & observability ground work (hours) — ✅ DONE & DEVICE-TESTED
- K1: enable `-Wall -Wextra`; fix the 3 real latent bugs + waive the rest.
- K2: delete dead PTY block; K3: delete dead USB fn/decl, tidy `extern`/guard.
- C6: make `kputchar` a single atomic emit (prerequisite for C1/C3).
- S7: fix tick re-arm + watchdog comments.
- L2: expose the existing drop counters + `picoctl diag` userspace tool.
- **Acceptance MET:** clean warning-free build (default + `-Werror`); flashed kernel UF2
  to device, booted to prompt with no crash; `picoctl diag` worked on device showing
  `usb_tx_dropped=111` (boot), `vt_dropped=0`, `core1_state=3`, heartbeat rising.
  Under compile/input activity: no crash, no new drops, counters healthy. Committed
  as `207bd96`,`0ede38e`,`a28d949`,`2318118`,`326e78a`,`0b5d749`,`ae61c94`.

### Phase 1 — Stop the crashes (P0 stability) — IN PROGRESS
- [x] S1: lockout-aware core1 heartbeat (flag in rawflash, suppress stale_ticks, 2s→4s).
      Commit `bd2efae`. **DEVICE SOAK: 12 write/sync (FTL lockout) cycles, 0 crashes,
      core1 heartbeat rose steadily — false-reboot fix validated.**
- [x] S2: watchdog feed moved to top of tick. Commit `726140b`. Device-verified in soak.
- [x] I1: CardKB I2C timeout 5000→2000 µs (bounds IRQ-off window). Commit `d2e6f68`.
      **DEVICE VERIFIED: physical CardKB keypress ('g') received on console after this
      build — keyboard still functional.**
- [x] F2: `fsck -a` implies auto-yes (no invisible boot hang). Commit `fcdbce7`.
      Cross-built armm0; deploys with next fs image.
- [x] M2: full-range validation in ugetw/getl/uputw/uputl + MISALIGNED syntax fix.
      Commit `75cfc11`. Device soak: no corruption.
- [x] M3: brk bounded to [PROGLOAD,PROGTOP]; u_break committed after uzero.
      Commit `fdaa4cc`. Device soak: no corruption.
- [ ] M1: syscall-stack canary + high-water check (ABI-adjacent; do carefully next).
- [ ] S1 follow-up: batch/disable per-swap TRIM (364 trims/swap-in amplifies lockout).
- **Acceptance (partial):** device soak 12 FTL write/sync cycles zero reboots/crashes;
  heartbeats healthy. Full compile/input-spam 30-min soak + M1 still pending.

### Phase 2 — Console coherence & the LCD mirror (the reported symptom)
- C1: `console_fanout` both-or-neither + equal ring sizes; C2: resolve liveness/backpressure.
- D1: deferred pending bitmask + guaranteed worker; D2/D3: fix reentry + repaint bugs.
- C3: centralized CR/LF; C5: gfx-mode resync.
- **Acceptance:** byte-identical serial/LCD under bursts + compiler load; no same-line prompts; LCD keeps updating under load.

### Phase 3 — Forensics (so future crashes are diagnosable)
- S5: persist fault record across reset; S4: separate core1 fault handler.
- L1 stages 0-5: `klog.c` RAM ring (shrink `USERMEM` 202→198 KB) + `kputchar` hook →
  `/dev/klog` device + `klogd` daemon → rotation + SD archive → pre-FTL 32 KB flash ring
  with CRC/seq wear leveling → crash signature in `watchdog_hw->scratch[]`.
- B1: serial-console fallback / `console=` flag; `klogd -p` = `dmesg`.
- **Acceptance:** induced panic + core1 wedge → reboot → boot prints cause + full staged
  log from flash; log rotates to `/var/log/klog.1..N` and archives to `/mnt/sd/log/`;
  survives power cycle; `__bss_end__` stays below `0x20040000` every stage.

### Phase 4 — Filesystem, boot & input robustness
- F1/F3/F5: single device flush ordering, barriers, explicit SPI ownership, bounded SD retry.
- B2/B3/B4: getty respawn limit, non-blocking firstboot, cheap exit sync, deferred LCD init.
- I2/I3/I4: keyboard backpressure, atomic escape sequences, flush mapping fixes.
- **Acceptance:** no shell-exit gap; no stale bytes after `TCSAFLUSH`; input soak clean; SPI contention clean.

### Phase 5 — Cohesion (Linux-like contracts)
- C4: `struct console`; D5: formalized barriers; S3: stack watermarks + core1 stack bump.
- M4-M9: user-stack high-water marks; `MAX_SWAPS 1` + swap I/O checks; ELF loader field
  validation; `BUG_ON`/`WARN_ON` + `-fstack-usage`/`-fstack-protector-strong`; table resize
  if load tests justify. Document RP2040 isolation limits (migrate to RP2350 for real MPU).
- K4/K5: single-source constants, break circular include, driver probe/remove.
- I5/I6: input core, tty ioctl semantics.
- **Acceptance:** `tty=` remap works; absent hardware probes out cleanly; full `-Wall -Wextra -Werror` opt-in passes; `-fstack-usage` shows headroom >0.

### Phase 6 — Final verification (once)
- Rebuild warning-free; reflash; reload apps; run **full conformance `fcctest4.py` once**.
- **Acceptance:** 57/57 on-device conformance; all soak tests green; keyboard verified on hardware.

---

## 4. Test strategy

- **Build gate:** every phase ends with `make -C Kernel/platform/platform-rpipico/build` clean under `-Wall -Wextra`; text/bss within 264 KB SRAM (currently text ~104 KB, bss ~250 KB).
- **Console mirror harness:** drive bursts from host, capture USB console + (via framebuffer readback where possible) LCD shadow, assert byte equality and prompt line placement.
- **Soak:** sustained `fcc`/`cpp` compiles + 200 keys/s + USB spray for 30+ min; assert zero reboots (S1), `vtd_dropped==0` (D1), `usbconsole_tx_dropped` understood.
- **Fault injection:** force panic, core1 fault, stack overflow → verify reset record + recovery.
- **Filesystem:** crash-simulate mid-sync; dirty-fs boot; verify F2/F3.
- **Final:** one full `fcctest4.py` run at the very end.

---

## 5. Things explicitly NOT to do

- Do not port lockdep, sysfs, kobjects/refcounted `struct device`, or CFS workqueues — too heavy for 264 KB / 200 Hz.
- Do not reformat for "Linux feel" (4→8 spaces, naming) until phases 0-5 pass; it adds zero stability.
- Do not merge per-platform `devtty.c`/`kputchar` implementations — one definition per build is by design.
- Do not enable/repair `CONFIG_DEV_PTY` — delete it (K2).
- Git: commit each verified change and push to `origin` regularly; keep history clean with
  rebase on local work; avoid force-pushing shared history.
- Cosmetic comment fixes ARE cheap correctness wins: watchdog "8 s"→5 s, `TTY_READY_LATER` doc `-1`→`2`.

---

## 6. Agent report index (raw findings lineage)

| Agent session | Focus | Folded into |
|---------------|-------|-------------|
| e0b9b9549ffe… | Linux practices vs FUZIX | §1, K, §5 |
| e0b9b410cffe… | Boot/init audit | B, L, F2 |
| e0b9b9555ffd… | Console/LCD/VT cohesion | C, D |
| e0b9b9547ffe… | Input subsystem | I, S2 |
| e0b9b410dffe… | Deferred work/concurrency | D, S1/S4 |
| e0b9b4113ffe… | Filesystem/block layer | F, B3 |
| e0b9b4110ffe… | Build quality/dead code | K |
| e0b9b9554ffe… | Stability crash/hang audit | S, I1 |
| e0b9b954effd… | Logging design | L — **complete** (full design in L1) |
| e0b9b4114ffe… | Memory/process isolation | M — **complete** (full findings in M1-M9) |

*All 10 agent reports integrated. No deep-dives pending.*
