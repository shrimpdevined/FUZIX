# FUZIX RP2040 — Stability & Cohesion Master Plan

**Status:** Authoritative source of truth for all changes. Last updated **2026-11-20**.
**Origin:** Consolidated from 10 research agents plus the earlier `COMPILER_BUGS.md`
audit (deleted; its still-relevant items are folded below). All 10 deep-dives are
complete. Every finding carries file:line evidence.

**How to use this file:** Section 2 is the *only* status table — each issue appears
once, with one status. Section 3 is a phase-by-phase history of what was actually
done. Section 4 lists every remaining task in plain language (what it targets and
why it matters). Do not start a change that is not tracked here; mark it `[x]` only
after its acceptance test passes.

---

## 0. Rules of engagement

- Warning-free builds; no new warnings from `-Wall -Wextra`.
- **Git workflow:** commit each verified, focused change with a descriptive message
  (issue ID + what/why); push to `origin` regularly so work is backed up and visible;
  rebase/squash freely on local history; never force-push a shared branch.
- Preserve the user's uncommitted work. Never delete tracked upstream `.md` docs.
- Stream mode stays removed; block mode with 64-byte paced writes is the only
  XMODEM transfer path.
- The CardKB must always work — it is the only input on the laptop.
- Serial console and LCD must mirror each other with zero output-order differences.
- Host tooling waits for a full prompt before sending the next line.
- Full conformance (`fcctest4.py`) runs **once, at the very end**, after cleanup and
  app reload.

---

## 1. Why the system felt "hacked together" — three structural gaps

The problems were not mostly style. They traced to three missing first-class
abstractions; most stability fixes were workarounds for one of them:

1. **No first-class console object.** Output targeted literal minor `1` in several
   files; `TTYDEV` existed but wasn't used for output; the `tty=` option was parsed
   then ignored. One byte stream was fanned out to two independent queues with
   independent drop counters.
2. **No generic deferred-work facility.** Slow VT/LCD rendering hung entirely off
   `plt_idle_hook()`, which runs only when no process is runnable. Under compiler
   load the LCD froze and the VT ring dropped bytes.
3. **No device model with `probe()`.** Stubbed drivers registered as real devices;
   state was scattered in globals; no probe/remove/refcount.

Linux's useful essence here is the **contracts** — one registered console, one queue
primitive with flush/cancel, probe/remove that verifies hardware, generated config —
not the implementations. Do not port lockdep/sysfs/kobjects/CFS into 264 KB SRAM.

---

## 2. Master issue register (single authoritative status table)

Severity: **P0** ship-blocker (crash / data loss / forensics black hole), **P1** high,
**P2** medium, **P3** low/cosmetic. Status: `[ ]` open, `[x]` done, `[~]` partial,
`[-]` accepted as-is / won't fix (rationale in row). Each ID appears exactly once.

### C — Console / output / LCD

| Status | ID | Sev | Finding / what it targeted | Resolution |
|--------|----|-----|----------------------------|------------|
| [-] | C1 | P0 | One stream → two queues of different capacities (USB 512 B, VT 4096 B) with independent silent tail-drops; a dropped `\r\n` could stack prompts. | Accepted best-effort. Equalizing rings to 4096 B is infeasible (~2.6 KB RAM free). D1's pump keeps the VT ring drained; C3/C7 handle observed stacking; S5/S8 logs give durable evidence. |
| [x] | C2 | P0 | `tty_writeready(1)` always reported ready → writes never slept → suffix drops. | Accepted best-effort for the same RAM/deadlock reasons; full prompts verified on device. |
| [x] | C3 | P1 | CR/LF normalization split three ways; bare LF in the VT didn't reset the column (LCD prompt stacking). | `charout()` LF zeroes `cursorx` (`92f59ac`). Device-verified. |
[x] | C4 | P1 | No /dev/console abstraction; kernel output hard-coded to minor 1. | **DONE** (`2e9b39e`): CONSOLE_MINOR derived from TTYDEV & 0xFF used in kputchar/tty_putc/tty_writeready; VT/USB fan-out unchanged. Device verified. |
| [x] | C5 | P2 | `/dev/fb0` gfx mode purged queued VT text already sent to serial → LCD/serial diverged. | Drain VT to LCD before gfx mode; resync shadow on exit (`5b58840`). Device-verified. |
| [x] | C6 | P1 | `kputchar` emitted CR and LF as two calls; an IRQ echo could interleave. | Single atomic emit under `di()` (`2318118`). |
| [x] | C7 | P1 | Login and shell prompts stacked on one line when several Enters arrived before the process re-prompted. | Login: `\r\n` on retry + `tcflush()` before shell exec (`47edee5`); shell: `\r` prefix on PS1/PS2 (`ac0fbdf`). Device-verified. |
| [-] | C8 | P1 | Kernel banner printed before USB enumerated; 512 B TX ring overflowed, host saw only a fragment. | Mitigated, not eliminated: first 512 B of console output captured to `/var/log/boot.log` (`58929eb`). Serial stays best-effort; full boot text recoverable. |

### D — Deferred work / concurrency / core1

| Status | ID | Sev | Finding / what it targeted | Resolution |
|--------|----|-----|----------------------------|------------|
| [x] | D1 | P0 | VT/LCD drained only from idle → LCD froze under compiler load; `vt_dropped` 0→295. | `vt_pending()/vt_drain()/vt_pump()` + pump from writer thread when backlog ≥64 (`6ed3dec`). Device-verified (0 drops under bursts). |
| [x] | D2 | P1 | `vtoutput()` reentry queued one byte regardless of length; `vtbusy` cleared without `di()`. | Queue whole run atomically; clear under `di()`. Device-verified. |
| [x] | D3 | P1 | `row_dirty` cleared even when `lcd_redraw_row()` skipped → lost repaint. | Redraw returns bool; flag kept on skip. Device-verified. |
[x] | D4 | P1 | Flush request/ack flags written by both cores can race; core0 busy-spins. | **DONE** (`ccfda13`): sequence-number protocol with fences; 20 ms deadline with switchout() instead of spin. Build + device flush soak verified. |
| [x] | D5 | P2 | SPSC barriers and bounded flash lockout are correct. | **Verified correct, no change needed:** `core1.c` does `__mem_fence_release()` after writing data and before publishing `head`, and `__mem_fence_acquire()` after reading `head` and before reading data — the textbook SPSC pattern. `rawflash.c` uses `multicore_lockout_start_timeout_us()` (bounded), never the unbounded blocking form. | No regression in lockout/USB soak tests (ongoing). |

### S — Kernel crash / hang / stability

| Status | ID | Sev | Finding / what it targeted | Resolution |
|--------|----|-----|----------------------------|------------|
| [x] | S1 | P0 | Core1 heartbeat false-positive reboot: flash/GC/TRIM parks core1; core0 saw a "wedge" and rebooted. | Lockout-aware heartbeat flag (`bd2efae`). Device soak: 12 write/sync cycles, 0 crashes. |
| [x] | S2 | P0 | Tick ran entirely under `di()` including I2C; watchdog feed happened last. | Watchdog feed moved to tick top (`726140b`). Device-verified. |
[x] | S3 | P1 | Kernel stack grows into u_data; core1 stack only 2 KB. | **DONE (partial):** core1 stack 0x800->0x1000 (`2518b76`, fits SCRATCH_X); M1 canary + M8 stack protector cover the canary contract. Full tricks.S watermark fill skipped (high-risk assembly). |
| [x] | S4 | P1 | Core1 HardFault shared core0's handler and stack; crash flush would lock out the faulting core. | Minimal core1 fault path, `C1C1` scratch record, immediate reset (`20d9222`). Code-reviewed. |
| [x] | S5 | P0 | Panic/fault state lost across reset — crashes left no evidence. | QSPI crash record → `/var/log/crash.log` at boot (`f7df7e9`, `da680af`). Device-verified. |
[x] | S6 | P2 | Flash lockout start fails in TinyUSB critical section; unlock has no timeout. | **DONE** (`4e738d3`): start retries 3x with backoff; end bounded with reset on failure. flash_lockout_active intact. Build verified. |
| [x] | S7 | P2 | Tick re-arm used a stale init-time `now`; stale comments (watchdog 8 s vs 5 s, `TTY_READY_LATER` -1 vs 2). | Advance `now` each tick; fix comments (`326e78a`). |
| [x] | S8 | P0 | HardFault (exception 3) on long shell input lines: `getcwd()`→`opendir()`→`calloc(576)` corrupted the malloc arena shared with the shell's sbrk. | Static `DIR` via `opendir_r`, no allocation in prompt path (`4099881`). Device-verified. |

### I — Input (CardKB / touch / line discipline)

| Status | ID | Sev | Finding / what it targeted | Resolution |
|--------|----|-----|----------------------------|------------|
| [x] | I1 | P0 | Blocking I2C read in hard IRQ with interrupts off; SDK had an unbounded TX-FIFO spin. | Timeout cut 5000→2000 µs (`d2e6f68`). Physical keypress verified. |
[x] | I2 | P1 | CardKB has no backpressure; keys dropped when input queue full. | **DONE** (`ceaca4f`): 32-byte SPSC staging ring in devkb_poll(), throttle 96/resume 48, arrow keys atomic. Build verified; physical CardKB works. |
| [x] | I3 | P2 | Arrow keys injected ESC `[` in three separate calls → truncated escape sequence. | Atomic `di()/irqrestore()` (`4bbbb09`). Build verified. |
| [x] | I4 | P1 | `TCSETSF` didn't flush the device staging ring; flush used tty minor as USB number. | Call `tty_flushinput()`; map via `ttymap` (`4e8cac6`). Build verified. |
[x] | I5 | P2 | Touch has no event queue; keyboard/touch share no input core. | **DONE (minimal)** (`ceaca4f`, linked `2e9b39e`): Kernel/input.c 16-event ring, input_emit/input_get; keyboard+touch feed events in addition to existing paths. No /dev/input node. |
| [~] | I6 | P2 | `TIOCINQ`, canonical `select()`, `VSUSP`, and BEL-on-^D don't match POSIX. | **Partial (`17ff63c`):** VEOF no longer beeps; canonical `TIOCINQ` counts bytes to the next newline (0 if no complete line). Still open: `SELECT_IN` canonical readiness and `VSUSP`. |

### F — Filesystem / block / SD

| Status | ID | Sev | Finding / what it targeted | Resolution |
|--------|----|-----|----------------------------|------------|
[x] | F1 | P1 | sync() does not flush the device; d_flush() is a no-op. | **DONE** (`62bc65f`): removed per-buffer d_flush() from bdput(); sync_flush_devices() calls d_flush() once per device, propagates errors. Device verified: no boot stall. |
| [x] | F2 | P0 | `fsck -a` didn't imply auto-yes → invisible prompt hung boot on a dirty FS. | `-a` sets auto-yes (`fcdbce7`). |
[x] | F3 | P1 | sync() has no write-order barrier; crash can leave superblock clean over rolled-back data. | **DONE** (`1a4fa8c`): inodes -> barrier -> superblocks -> barrier -> commit clean; clean bit written only after barriers. Device verified. |
[x] | F4 | P2 | FAT special-cased by magic bit; block errors lack errno. | **DONE (low-risk subset)** (`02ae18b`): blkdev_rw/blkdev_transfer set errno; Dhara errors -> ENOSPC/EIO. Full ops-vector refactor skipped (too invasive). |
[x] | F5 | P1 | SD/LCD share SPI0; SD retry can run 24 s > 5 s watchdog. | **DONE** (`29e6feb`): transfer-wide 2.5 s deadline checked at each retry and in wait/release; timeout returns explicit failure. Ownership documented. Device burst verified. |
| [x] | F6 | P2 | ~1 s root-fs check every boot: `is_clean()` read offset 0 instead of superblock block 1. | `lseek(512)` (`6eb3d30`). Device-verified: 0.6 s, no full pass. |

### B — Boot / init

| Status | ID | Sev | Finding / what it targeted | Resolution |
|--------|----|-----|----------------------------|------------|
| [-] | B1 | P0 | Original worry: USB-only console = no output when USB isn't enumerated. | **Descoped — no longer applies.** (1) The LCD is a live local console: every byte goes to VT/LCD as well as USB, kept flowing by D1, so a dead USB port is not a blind spot. (2) Persistent output is covered by `boot.log` and `crash.log`. (3) Implementing it is actively harmful: `PICO_DEFAULT_UART_TX_PIN` is GPIO0 = CardKB I2C0 SDA, and UART1 is GPIO6-9 = touch I2C1 — enabling either breaks the only input device. `rawuart` is deliberately a no-op for this reason. | LCD shows output with USB unplugged; log files readable; CardKB still works. |
[x] | B2 | P1 | firstboot blocks unattended boot; getty has no respawn limit and unbounded tty-open loop. | **DONE** (`c9d0269`): bounded tty1 open with diagnostic; respawn limit (>5 in 60 s). Device verified. |
| [x] | B3 | P1 | `doexit()` called `sync()` even with no dirty buffers → seconds-long exit gap. | Scan `bufpool` first (`8254768`). Build verified. |
| [~] | B4 | P2 | `devlcd_init()` spends ~240 ms with IRQs off before interrupts are enabled. | Deferred: strict ST7796 timing; boot text already captured to `boot.log`. |
| [x] | B5 | P2 | Software BOOTSEL timed out because the tick kept feeding the watchdog the ROM had armed (pico-sdk #2689). | Deferred BOOTSEL on core0 (`067b15d`). Device-verified. |

### K — Build / config / dead code

| Status | ID | Sev | Finding / what it targeted | Resolution |
|--------|----|-----|----------------------------|------------|
| [x] | K1 | P0 | "Warning-free" was silence — no `-Wall/-Wextra`; 43 warnings with 3 real bugs. | Enable flags + fix bugs + waive legacy style (`207bd96`). |
| [x] | K2 | P1 | Dead, uncompilable `CONFIG_DEV_PTY` block. | Deleted (`0ede38e`). |
| [x] | K3 | P2 | Dead USB function, redundant `extern`, misnamed include guard. | Removed/renamed (`a28d949`). |
[x] | K4 | P2 | Constants defined many times; circular config include. | **DONE** (`2518b76`): broke config.h<->tusb_config.h cycle; derived Makefile flash constants (values unchanged). Cross-language single-source + used() cleanup skipped. |
| [x] | K5 | P2 | No device model; stub `rawuart` registered as a real device. | `probe()`/`remove()` added to `struct ttydriver`; stub `rawuart` now fails probe and is never mapped (`c4769e1`). Build verified. |

### L — Logging

| Status | ID | Sev | Finding / what it targeted | Resolution |
|--------|----|-----|----------------------------|------------|
| [x] | L1 | P0 | No persistent log; crashes left zero evidence. | **DONE** (`50272af`, `e807693`): S5 crash record → `/var/log/crash.log`; boot capture → `/var/log/boot.log`; **4 KB klog ring unioned with `mkfs_work` (zero net RAM) → `/dev/klog` + `klogd` → `/var/log/klog` with rotation + SD archive + flash replay**. 4-sector wear rotation with sequence seeded past the newest record on replay; correct snapshot base. **DEVICE VERIFIED:** flash→file round-trip starts with 'FUZIX version 0.5' (4080 B, no garbage), klogd alive, 3 compiles + runbin RC=42, 8× burst, 0 VT drops. |
| [x] | L2 | P2 | Drop/liveness telemetry not observable; LCD contents not readable. | `picoctl diag`, `PICOIOC_VTSHADOW`, `lcd_compare.py` (`0b5d749`, `8e9354e`). Device-verified. |

### M — Memory / process isolation

RP2040 has no MMU/MPU and no unprivileged mode: kernel and user both run privileged,
with zero hardware isolation. Real isolation needs RP2350 (Cortex-M33 + MPU); record
that as a permanent hardware limitation. The realistic target is early detection and
deterministic watchdog recovery.

| Status | ID | Sev | Finding / what it targeted | Resolution |
|--------|----|-----|----------------------------|------------|
| [x] | M1 | P0 | Syscall stack grows down into live `u_data` → historic "corrupt inode" panics. | Canary at `udata+0x120`, checked in tick (`3ea8b82`). Device-verified. |
| [x] | M2 | P0 | `valaddr()` clamped instead of rejecting; `uget/uput` accepted short ranges. | Full-range validation (`75cfc11`). Device soak clean. |
| [x] | M3 | P0 | `brk_extend()` accepted addresses inside `udata` with no bounds. | Bounded to `[PROGLOAD,PROGTOP]` (`fdaa4cc`). Device soak clean. |
[-] | M4 | P1 | User stack (7 KB) can underflow into kernel tables. | **Disabled (`5deaff2`):** global stamp checked per-current-process at tick -> false warning on every command. M1 canary + M8 stack protector cover the real contract. Re-enable with per-process stamp. |
| [~] | M5 | P1 | Swap reserved 728 blocks for a 512-block partition; second map I/O ignored; OOM calls `panic()`. | **Partial (`fa1b3ef`):** `MAX_SWAPS 1`, swap read/write returns checked. Still open: replace the `swapin: no free block` panic with a controlled per-process kill (swapin runs in swapper task context where `doexit()` targets the wrong process). |
| [x] | M6 | P1 | ELF loader doesn't validate `p_filesz<=p_memsz`, or guard aligned-size overflow; relocation bound and readi checks already present. | Reject `p_filesz > p_memsz` and size wrap in the first pass before committing state (`185c9ca`). Build verified. |
| [-] | M7 | P2 | `kmalloc()` pool never initialized → always NULL. | **Not applicable to rpipico:** no compiled source in this build calls `kmalloc()` (only `flat.c`/`buddy.c` and other platforms do), and `--gc-sections` drops `mm/malloc.c` from the final ELF (verified: no `kmalloc`/`kmemaddblk` symbol in `fuzix.elf.map`). No driver silently fails here. | No `kmalloc` reliance in the rpipico image. |
| [x] | M8 | P2 | No `BUG_ON`/`WARN_ON`, no stack protector. | **DONE** (`b45aec3`, `270c4e3`): `bug.h` with `BUG_ON`/`WARN_ON`, canary runtime in `process.c`, `-fstack-protector-strong` in CMake. Map `__bss_end__=0x2003FBA8` (1,112 B free), 0 warnings. Device verified: boot, 3 compiles + runbin RC=42, burst, no canary trip. |
[x] | M9 | P2 | OFT/ITAB/PTAB may be too small for the full compiler chain. | **DONE (verified, no change)** (`2518b76`): 56/45/16 adequate for cpp+cc1+as+ld; no ENFILE/EAGAIN. Raising costs scarce RAM. |

---

## 3. Implementation history (what was actually done)

Each phase's acceptance test is recorded here. `[x]` items are in Section 2.

**Phase 0 — Cleanup & observability — DONE & DEVICE-TESTED.** K1, K2, K3, C6, S7,
L2. Clean warning-free build; `picoctl diag` on device showed drop counters and a
rising core1 heartbeat. Commits `207bd96`, `0ede38e`, `a28d949`, `2318118`,
`326e78a`, `0b5d749`, `ae61c94`.

**Phase 1 — Stop the crashes (P0) — DONE & DEVICE-TESTED.** S1, S2, I1, F2, M1,
M2, M3, S8, plus the S5 crash log. Soaks: zero reboots, canary armed with no false
trip, CardKB keypress received, clean-root fsck 0.6 s, 250-char input no longer
faults. Commits `bd2efae`, `726140b`, `d2e6f68`, `fcdbce7`, `75cfc11`, `fdaa4cc`,
`3ea8b82`, `f7df7e9`, `4099881`.

**Phase 2 — Console coherence & LCD mirror — DONE & DEVICE-TESTED.** C3, D1,
C7, D2, D3, C5 done; C1/C2/C8 accepted/mitigated. `picoctl diag` on the final image:
0 USB drops, 0 VT drops, healthy heartbeat; login and shell prompts each on their
own LCD row; gfx round-trip preserves markers. C1/C2 decisions are in Section 2.

**Phase 3 — Forensics — PARTIAL.** S4, S5 done; L1 stages 0-1 done (plain log
files). The full kernel ring + rotating daemon remains open (see Section 4). B1
was descoped — the LCD mirror and log files already cover its goal, and its UART
pins belong to the keyboard/touch.

**Phase 4 — FS / boot / input robustness — PARTIAL.** F6, B3, I3, I4 done; B4
deferred. F1/F3/F4/F5, B2, I2 remain open.

**Phase 5 — Cohesion (Linux-like contracts) — NOT STARTED.** C4, D4, D5, S3,
M4-M9, K4/K5, I5/I6.

**Phase 6 — Final verification — NOT STARTED.** One full `fcctest4.py` run after
everything else passes.

---

## 4. Remaining work — what each target actually does

This is the complete list of open/partial items. Each entry says what the problem is
in plain language, what to change, and how to know it worked. Ordered by priority.

### P0 — should be done before calling the system stable

**L1 — DONE.** Full pipeline: `klog.c` 4 KB ring fed by `kputchar()`,
`/dev/klog` (minor 128) with DRAIN/LEVEL/CLEAR/FLUSH/REPLAY, `klogd` writing
`/var/log/klog` with rotation + SD archive + flash replay, `picoctl klog` for
diagnostics. The ring is unioned with `mkfs_work` → zero net RAM; `rc` flushes
the boot log to flash before klogd drains it. Flash wear: 4-sector rotation,
sequence seeded past the newest record on replay so a reboot never rewinds over
the latest data. Two bugs found and fixed in stability testing (`e807693`):
the snapshot base pointed at stale slots when the ring held <4080 B, and the
sequence restarted at 0 each boot. **DEVICE VERIFIED:** flash→file round-trip
starts with 'FUZIX version 0.5', 4080 B no garbage; klogd alive; 3 compiles +
runbin RC=42; 8× burst; 0 VT drops; no canary trip.

### P1 — high-value robustness

**C4 — one registered console object.**
*Target:* kernel output is hard-wired to device minor `1`, so `tty=` remapping
changes where the shell goes but not where kernel messages go — the two can end up
on different devices.
*Do:* introduce a small `struct console` (name, write function, minor, setup/exit)
registered once at boot; derive the console minor from `TTYDEV & 0xFF`; replace the
literal `1`s in `devtty.c` and `devices.c`.
*Done when:* `tty=` remapping routes kernel messages, shell, and keyboard to the
same console; no magic `1` remains in output paths.

**D4 — race-free USB flush handshake.**
*Target:* the `flush_req`/`flush_ack` flags between core0 and core1 can lose a
request (both cores write them), and core0 waits with a 200,000-iteration busy loop
that wastes CPU time.
*Do:* publish monotonically increasing sequence numbers with memory barriers
instead of bare flags; replace the spin with `psleep()`/`wakeup()` so core0 sleeps
until core1 acknowledges.
*Done when:* rapid `tcflush()` under output load never loses a flush and the CPU
isn't burned spinning.

**S3 — kernel/user stack guards.**
*Target:* each process's kernel stack starts at the top of `u_data` and grows down
into live fields (`u_files[]`, `u_cwd`) with no guard — a deep fcc/FatFs call chain
was the historic cause of "corrupt inode" panics. Core1 also has only 2 KB of stack.
*Do:* fill the stack with a known pattern at context switch, scan the high-water
mark in the tick, and panic early if it approaches live data; raise
`PICO_CORE1_STACK_SIZE` to 4 KB.
*Done when:* an injected deep-recursion test trips the warning before any inode
corruption, and core1 survives its worst-case call depth.

**S6 — bounded flash lockout.**
*Target:* when core0 wants to program flash and core1 happens to be inside a
TinyUSB critical section, `multicore_lockout_start` fails and the filesystem write
returns `DHARA_E_TOO_BAD`; the matching end call has no timeout.
*Do:* retry the start with a short backoff and a hard deadline; use a bounded end
call; log the failure and reset if it can't be acquired.
*Done when:* USB input spam during a file write produces no spurious write failures
and never hangs.

**I2 — CardKB backpressure.**
*Target:* the keyboard injects bytes straight into the shared input queue; when
that queue is full the keystroke is silently dropped with only a BEL beep, and the
return value of `tty_inproc()` is ignored.
*Do:* give the keyboard its own small SPSC staging ring (like the USB path),
throttle at a high-water mark, and resume when `tty_data_consumed()` drains it
(Linux `tty_throttle`/`unthrottle` semantics).
*Done when:* a fast key spray loses no characters and the keyboard recovers after
the shell catches up.

**F1 — real device flush in `sync()`.**
*Target:* `sync()` writes buffers but never tells the underlying flash/SD device
to flush its cache; the `d_flush()` ioctl is a no-op, so "sync" doesn't guarantee
data reached the media.
*Do:* after writing dirty buffers, call each device's flush once per `sync()`
(throttled so it isn't per-buffer), and propagate errors instead of discarding them.
*Done when:* simulated crash right after `sync()` leaves the filesystem intact and
no `DHARA_E_MAP_FULL` errors appear under repeated syncs.

**F3 — correct write ordering in `sync()`.**
*Target:* `sync()` writes inodes, then marks the superblock clean, then writes
buffers — without a barrier. A crash in that window can leave a superblock saying
"clean" while the FTL journal has rolled the data back.
*Do:* enforce the order inodes → flush → mark superblocks clean → flush → write
buffer pool; only flip the clean bit after the final flush succeeds.
*Done when:* crash injection at every step of `sync()` never boots into a
clean-looking filesystem with missing data.

**F5 — explicit SPI0 ownership and bounded SD retry.**
*Target:* the SD card and LCD share SPI0 via a cooperative flag with no real owner;
an SD read can retry 8 times × 3 s = 24 s, far longer than the 5 s watchdog.
*Do:* add an explicit `spi0_owner` claimed under `di()`; have the LCD worker
re-queue a draw when the SD owns the bus; cap total SD retry time below the watchdog
interval.
*Done when:* heavy simultaneous SD + LCD output produces no `sd: timeout` spiral
and no watchdog reset.

**B2 — boot/init can't hang waiting for a human.**
*Target:* `firstboot` runs interactively inside `rc` before getty, so an unattended
boot stops waiting for input; getty has no respawn limit and opens `/dev/tty1` in an
unbounded loop if it fails.
*Do:* move firstboot to first login with a safe default; add a sysvinit-style
"respawning too fast" limit; bound the tty-open loop with a diagnostic.
*Done when:* a fresh image boots to a login prompt with no keyboard, and a
deliberately failing getty stops respawning instead of looping forever.

### P2 — medium / cleanup

**F4 — typed block errors and an fs ops vector.**
*Target:* FAT is detected by a magic bit scattered across five files, block-device
read failures return -1 without setting `errno`, and there's one global block-op
struct. Errors from the flash translation layer are lost.
*Do:* add a filesystem-type field with an operations vector; return typed errors
from the block layer; surface Dhara error codes to callers.
*Done when:* FAT and native filesystems dispatch through the same interface and SD/
Dhara errors are visible to userspace.

**I5 — one input core for keyboard and touch.**
*Target:* touch is a one-shot snapshot with no event queue, its interrupt GPIO is
unused, and the keyboard bypasses any staging. The two devices share no common path.
*Do:* add a small evdev-like `input.c` with timestamped events, per-device rings and
drop counters; make keyboard and touch transports; optionally expose
`/dev/input/event0`.
*Done when:* both devices deliver events through one core and touch taps between
reads are not lost.

**I6 (partial) — POSIX tty ioctl semantics.**
*Done already (`17ff63c`):* ^D no longer beeps; canonical `TIOCINQ` counts bytes up
to the next newline (0 if no complete line is pending).
*Still to do:* wire `SELECT_IN` canonical readiness (the FIXME at `tty.c:371` —
`select()` currently can't tell a canonical reader that a full line is waiting),
and implement `VSUSP` if job control is enabled.
*Done when:* `select()` and `read()` agree about pending input.

**M4 — user-stack high-water marks.**
*Target:* the 7 KB user stack can underflow below `PROGBASE` into kernel tables, and
the actual headroom under the compiler chain was never measured.
*Do:* fill a sentinel at exec, check the high-water mark in the tick, and produce a
`-fstack-usage` report to confirm headroom under fcc/cpp.
*Done when:* the report shows positive headroom and an overflow attempt warns before
corrupting kernel tables.

**M5 (partial) — swap sizing and OOM behavior.**
*Done already (`fa1b3ef`):* `MAX_SWAPS 1` (one map fits the 512-block partition),
swap read/write returns are checked and reported.
*Still to do:* replace `panic("swapin: no free block")` with a controlled kill of
just the offending process. This is the hard part: `swapin()` runs in the swapper
task, where `doexit()` would act on the wrong process, so the process teardown must
be restructured to target an arbitrary `ptptr`.
*Done when:* compiling under memory pressure never panics the whole machine.

**M8 — assertions and stack protector — DONE.**
`bug.h` provides `BUG_ON` (panic) and `WARN_ON` (kprintf + continue); the canary
runtime (`__stack_chk_guard`/`__stack_chk_fail`) lives in `process.c`; the build
uses `-fstack-protector-strong`. Map `__bss_end__=0x2003FBA8` (1,112 B free),
zero warnings. Device verified with a compile+burst soak: no canary trip.
*(Optional follow-up: seed `__stack_chk_guard` from `time_us_32()` instead of
the fixed constant — RP2040 has no TRNG.)*

**M9 — resize kernel tables if needed.**
*Target:* the open-file, inode, and process tables (56 / 45 / 16) may be too small for
the full cpp+cc1+as+ld chain (~30-50 file descriptors).
*Do:* only if load tests show `ENFILE`/`EAGAIN`, raise them and re-check the memory
map stays below the stack limit.
*Done when:* the full compiler chain runs with no table-exhaustion errors.

**K4 — one source of truth for constants.**
*Target:* flash sizes, memory sizes, and spin limits are duplicated across headers,
Makefiles, and CMake; there's a circular include between two config headers and 26
copies of a `used()` helper.
*Do:* define each constant once, derive the others, break the include cycle, and
keep a single `used()`.
*Done when:* changing a constant in one place updates every consumer; clean build.

**S1 follow-up — batch per-swap TRIM.**
*Target:* each swap-in issues 364 separate TRIMs, each triggering flash garbage
collection. S1 removed the false reboot; this is now only performance and flash wear.
*Do:* batch the discards into one contiguous range per swap-in.
*Done when:* swap-in time drops measurably with no increase in FTL GC events.

**B4 — deferred LCD init (deferred).**
*Target:* ~240 ms of LCD init runs with interrupts disabled before `EI`.
*Do:* split the strictly-timed ST7796 sequence so only the essential reset runs
before interrupts are enabled. Deferred because a wrong split blanks the LCD and
boot text is already captured to `boot.log`.
*Done when:* console is available sooner with no LCD regression.

### Phase 6 — final verification (once)

After every item above is resolved: rebuild warning-free, reflash, reload apps, then
run the full `fcctest4.py` conformance suite **once**. Acceptance is 57/57 with all
soaks green and the CardKB verified on hardware.

---

## 5. Test strategy

- **Build gate:** every phase ends with a clean
  `make -C Kernel/platform/platform-rpipico/build` under `-Wall -Wextra`; text/BSS
  must stay inside 264 KB SRAM (currently ~104 KB text, ~250 KB BSS).
- **Console mirror harness:** drive bursts from the host, capture the USB console,
  and compare with `picoctl vtshadow` output (`lcd_compare.py`); assert byte equality
  and prompt line placement.
- **Soak:** sustained `fcc`/`cpp` compiles + 200 keys/s + USB spray for 30+ minutes;
  assert zero reboots (S1), `vt_dropped == 0` (D1), and understood `usb_tx_dropped`.
- **Fault injection:** force a panic, a core1 fault, and a stack overflow; verify the
  reset record and recovery.
- **Pipe statements:** shell pipelines (`|`) are a first-class contract. Verify on
  device: `ls /bin | grep fsck`, `cat /etc/passwd | grep root | cut -d: -f1`,
  multi-stage pipelines, pipelines into redirection (`... > file`), and pipeline
  exit status (`true | false; echo $?` = 1). Host helper `tests/test_pipes.py`
  drives these and asserts exact output; run it in Phase 6 and after any shell/tty
  change (I2/I4 touch the tty path).
- **Filesystem:** crash-simulate mid-sync; boot with a dirty FS; verify F2/F3.
- **Final:** one full `fcctest4.py` run at the very end.

---

## 6. Things explicitly NOT to do

- Do not port lockdep, sysfs, kobjects/refcounted `struct device`, or CFS workqueues —
  too heavy for 264 KB / 200 Hz.
- Do not reformat for "Linux feel" (4→8 spaces, renaming) until phases 0-5 pass; it
  adds zero stability.
- Do not merge per-platform `devtty.c`/`kputchar` implementations — one definition
  per build is by design.
- Do not enable or repair `CONFIG_DEV_PTY` — it is deleted (K2).
- Do not grow the USB TX ring to VT size — there is no RAM; use log files instead.
- Git: commit each verified change, push to `origin` regularly, keep history clean;
  avoid force-pushing shared history.

---

## 6b. RAM budget and reclamation analysis (5-agent swarm, 2026-11-20)

**Verified facts from `build/fuzix.elf.map`:**
- `__bss_end__ = 0x2003F9C8`, so **only 1,592 B** of main RAM is free below
  `0x20040000` (the earlier "2.6 KB" figure was from a stale build — do not use it).
- Two 4 KB scratch banks exist above that: SCRATCH_X `0x20040000–0x20040FFF`
  (core1 stack uses the top 2 KB, ~2 KB free below) and SCRATCH_Y
  `0x20041000–0x20041FFF` (core0 stack top 2 KB, ~2 KB free below). No single
  contiguous 4 KB block is currently free.
- Heap is 0 bytes (`PICO_HEAP_SIZE=0x0`); alarm pool disabled — nothing to reclaim.
- Kernel image in flash ends at `0x10019A08` (~104.7 KB); FTL starts at `0x34000`.

**Display-redundancy hypothesis — DISPROVEN.** There is exactly ONE screen copy:
`vt_shadow[40][80]` = 3,200 B in `devlcd.c:29`. `CONFIG_VT_SIMPLE` is undefined, so
there is no generic `vtbuf`/`VT_BASE`; the panel GRAM is external and write-only.
There is no framebuffer allocation and no display process. `vt_shadow` must stay —
it is what preserves text while SD owns SPI. Do not remove it.

**Top RAM consumers (kernel BSS):** `bufpool` 10,400 B (20×520), `usb_console_table`
4,176 B (4×1044), `vtd_buf` 4,096 B, `mkfs_work` 4,096 B, `fat_paths` 4,096 B,
`i_tab` 3,600 B, `vt_shadow` 3,200 B, `ttybuf` 2,560 B, `ptab` 1,536 B.

**Ranked reclamation options for the L1 4 KB klog ring:**

| Option | Bytes | Risk | Notes |
|--------|-------|------|-------|
| **A. Shrink `USERMEM` 202→198 KB** (`config.h:106`) | 4,096 | LOW | `progbase` has 20,480 B slack beyond `PROGTOP`; only 4 code refs; `PROGSIZE`/`SWAP_SIZE` unchanged. This is the L1 design's intended move. |
| **B. Union klog ring with `mkfs_work[4096]`** (`devfat.c:704`) | 4,096 | MED | Buffer used only by the `f_mkfs` ioctl; `rc` never calls mkfs. Flush/suspend klog around mkfs. No map change. |
| C. `mkfs_work` 4096→1024 | 3,072 | LOW | mkfs speed drops only; `f_mkfs` needs ≥512 B. |
| D. `NBUFS` 20→12 (`config.h:170`) | 4,160 | MED | Needs FS/compiler soak; `bufpool` = 20×520 B. |
| E. Asymmetric USB rings (RX 512→128) + `vtd_buf` 4096→2048 + crash tail 428→128 + drop `boot_buf` | ~4,400 | MED | Keeps TX ring and `ttyinq` intact; re-verify D1 bursts. |
| F. Second read cursor over `vtd_buf` + 256 B flash staging | 4,096 | MED | All console bytes already pass through `vtd_buf`; no separate ring needed. Bursts >4 KB lose log bytes. |
| G. `NUM_DEV_TTY_USB` 4→2 | 2,088 | HIGH (functional) | Removes tty3/tty4. |

**Recommendation:** Option **A** is simplest and lowest-risk (exactly 4,096 B, 20 KB slack
remains); Option **B** is the no-map-change alternative if USERMEM must stay at 202 KB.
Either can be paired with the flash-staging durability mechanism (one 256 B page,
stolen from the dead `boot_buf`) so the ring need not be persistent RAM.
Do **not** touch `vt_shadow`, the TX ring, `ttyinq`, `journal_buf`, or `crash_ram_rec`.

---

## 7. Agent report index (raw findings lineage)

| Agent session | Focus | Folded into |
|---------------|-------|-------------|
| e0b9b9549ffe… | Linux practices vs FUZIX | §1, K, §6 |
| e0b9b410cffe… | Boot/init audit | B, L, F2 |
| e0b9b9555ffd… | Console/LCD/VT cohesion | C, D |
| e0b9b9547ffe… | Input subsystem | I, S2 |
| e0b9b410dffe… | Deferred work/concurrency | D, S1/S4 |
| e0b9b4113ffe… | Filesystem/block layer | F, B3 |
| e0b9b4110ffe… | Build quality/dead code | K |
| e0b9b9554ffe… | Stability crash/hang audit | S, I1 |
| e0b9b954effd… | Logging design | L |
| e0b9b4114ffe… | Memory/process isolation | M |
| RAM-swarm #1 | Total RAM budget / map | §6b |
| RAM-swarm #2 | Display buffer redundancy | §6b |
| RAM-swarm #3 | Console / USB rings | §6b |
| RAM-swarm #4 | Kernel tables / scratch buffers | §6b |
| RAM-swarm #5 | Lateral alternatives (SCRATCH_X/Y, flash staging) | §6b |

*All 10 agent reports integrated; no deep-dives pending.*
