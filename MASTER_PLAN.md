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
| [ ] | C4 | P1 | No `/dev/console` abstraction; kernel output hard-coded to minor `1`; `tty=` ignored. | Introduce one registered console object and derive its minor from `TTYDEV & 0xFF`. |
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
| [ ] | D4 | P1 | Flush request/ack flags written by both cores can race (lost request); core0 waits in a 200k-iteration busy spin. | Sequence numbers with fences; sleep/wakeup instead of spin. |
| [ ] | D5 | P2 | SPSC barriers and bounded flash lockout are correct. | Preserve unchanged; optionally name the barrier helpers. |

### S — Kernel crash / hang / stability

| Status | ID | Sev | Finding / what it targeted | Resolution |
|--------|----|-----|----------------------------|------------|
| [x] | S1 | P0 | Core1 heartbeat false-positive reboot: flash/GC/TRIM parks core1; core0 saw a "wedge" and rebooted. | Lockout-aware heartbeat flag (`bd2efae`). Device soak: 12 write/sync cycles, 0 crashes. |
| [x] | S2 | P0 | Tick ran entirely under `di()` including I2C; watchdog feed happened last. | Watchdog feed moved to tick top (`726140b`). Device-verified. |
| [ ] | S3 | P1 | Per-process kernel stack grows down into live `u_data`; core1 stack only 2 KB — historical "corrupt inode" cause. | Stack watermarks + canary scan; bump core1 stack to 4 KB. |
| [x] | S4 | P1 | Core1 HardFault shared core0's handler and stack; crash flush would lock out the faulting core. | Minimal core1 fault path, `C1C1` scratch record, immediate reset (`20d9222`). Code-reviewed. |
| [x] | S5 | P0 | Panic/fault state lost across reset — crashes left no evidence. | QSPI crash record → `/var/log/crash.log` at boot (`f7df7e9`, `da680af`). Device-verified. |
| [ ] | S6 | P2 | Flash lockout start fails if core1 is in a TinyUSB critical section; unlock has no timeout. | Bounded retry/backoff; log + reset on failure. |
| [x] | S7 | P2 | Tick re-arm used a stale init-time `now`; stale comments (watchdog 8 s vs 5 s, `TTY_READY_LATER` -1 vs 2). | Advance `now` each tick; fix comments (`326e78a`). |
| [x] | S8 | P0 | HardFault (exception 3) on long shell input lines: `getcwd()`→`opendir()`→`calloc(576)` corrupted the malloc arena shared with the shell's sbrk. | Static `DIR` via `opendir_r`, no allocation in prompt path (`4099881`). Device-verified. |

### I — Input (CardKB / touch / line discipline)

| Status | ID | Sev | Finding / what it targeted | Resolution |
|--------|----|-----|----------------------------|------------|
| [x] | I1 | P0 | Blocking I2C read in hard IRQ with interrupts off; SDK had an unbounded TX-FIFO spin. | Timeout cut 5000→2000 µs (`d2e6f68`). Physical keypress verified. |
| [ ] | I2 | P1 | CardKB has no backpressure; keys are silently dropped (only BEL) when the input queue is full. | Staging ring with high-water throttle. |
| [x] | I3 | P2 | Arrow keys injected ESC `[` in three separate calls → truncated escape sequence. | Atomic `di()/irqrestore()` (`4bbbb09`). Build verified. |
| [x] | I4 | P1 | `TCSETSF` didn't flush the device staging ring; flush used tty minor as USB number. | Call `tty_flushinput()`; map via `ttymap` (`4e8cac6`). Build verified. |
| [ ] | I5 | P2 | Touch has no event queue and its interrupt GPIO is unused; keyboard/touch share no input core. | Small evdev-like input core feeding both devices. |
| [ ] | I6 | P2 | `TIOCINQ`, canonical `select()`, `VSUSP`, and BEL-on-^D don't match POSIX. | Correct tty ioctl semantics. |

### F — Filesystem / block / SD

| Status | ID | Sev | Finding / what it targeted | Resolution |
|--------|----|-----|----------------------------|------------|
| [ ] | F1 | P1 | `sync()` doesn't flush the underlying device; `d_flush()` is a no-op. | Flush each device once at end of `sync()`, throttled; surface errors. |
| [x] | F2 | P0 | `fsck -a` didn't imply auto-yes → invisible prompt hung boot on a dirty FS. | `-a` sets auto-yes (`fcdbce7`). |
| [ ] | F3 | P1 | `sync()` has no write-order barrier; a crash can leave a superblock "clean" over rolled-back data. | inodes → flush → clean superblocks → flush → buffers. |
| [ ] | F4 | P2 | FAT special-cased by a magic bit in five places; block errors lack errno; one global block-op. | Filesystem-type ops vector + typed errors. |
| [ ] | F5 | P1 | SD/LCD share SPI0 via a cooperative flag; SD retry can run 24 s > 5 s watchdog. | Explicit bus ownership; cap total retry below watchdog. |
| [x] | F6 | P2 | ~1 s root-fs check every boot: `is_clean()` read offset 0 instead of superblock block 1. | `lseek(512)` (`6eb3d30`). Device-verified: 0.6 s, no full pass. |

### B — Boot / init

| Status | ID | Sev | Finding / what it targeted | Resolution |
|--------|----|-----|----------------------------|------------|
| [-] | B1 | P0 | Original worry: USB-only console = no output when USB isn't enumerated. | **Descoped — no longer applies.** (1) The LCD is a live local console: every byte goes to VT/LCD as well as USB, kept flowing by D1, so a dead USB port is not a blind spot. (2) Persistent output is covered by `boot.log` and `crash.log`. (3) Implementing it is actively harmful: `PICO_DEFAULT_UART_TX_PIN` is GPIO0 = CardKB I2C0 SDA, and UART1 is GPIO6-9 = touch I2C1 — enabling either breaks the only input device. `rawuart` is deliberately a no-op for this reason. | LCD shows output with USB unplugged; log files readable; CardKB still works. |
| [ ] | B2 | P1 | `firstboot` blocks unattended boot; getty has no respawn limit and an unbounded tty-open loop. | Move firstboot to first login; respawn limit; bounded open. |
| [x] | B3 | P1 | `doexit()` called `sync()` even with no dirty buffers → seconds-long exit gap. | Scan `bufpool` first (`8254768`). Build verified. |
| [~] | B4 | P2 | `devlcd_init()` spends ~240 ms with IRQs off before interrupts are enabled. | Deferred: strict ST7796 timing; boot text already captured to `boot.log`. |
| [x] | B5 | P2 | Software BOOTSEL timed out because the tick kept feeding the watchdog the ROM had armed (pico-sdk #2689). | Deferred BOOTSEL on core0 (`067b15d`). Device-verified. |

### K — Build / config / dead code

| Status | ID | Sev | Finding / what it targeted | Resolution |
|--------|----|-----|----------------------------|------------|
| [x] | K1 | P0 | "Warning-free" was silence — no `-Wall/-Wextra`; 43 warnings with 3 real bugs. | Enable flags + fix bugs + waive legacy style (`207bd96`). |
| [x] | K2 | P1 | Dead, uncompilable `CONFIG_DEV_PTY` block. | Deleted (`0ede38e`). |
| [x] | K3 | P2 | Dead USB function, redundant `extern`, misnamed include guard. | Removed/renamed (`a28d949`). |
| [ ] | K4 | P2 | Constants defined many times; circular config include; 26 copies of `used()`. | Single-source constants; break cycle; one helper. |
| [ ] | K5 | P2 | No device model; stub `rawuart` registered as a real device. | `probe()`/`remove()` so absent hardware probes out. |

### L — Logging

| Status | ID | Sev | Finding / what it targeted | Resolution |
|--------|----|-----|----------------------------|------------|
| [~] | L1 | P0 | No persistent log; crashes left zero evidence. | Stages 0-1 done: S5 crash record + boot console capture → plain files. Full `klog.c` ring + `klogd` rotation/SD archive still open. |
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
| [ ] | M4 | P1 | User stack (7 KB) can underflow into kernel tables; headroom never measured. | Sentinel + high-water check; `-fstack-usage`. |
| [ ] | M5 | P1 | Swap reserved 728 blocks for a 512-block partition; second map I/O ignored; OOM calls `panic()`. | `MAX_SWAPS 1`, check I/O, kill the offending process. |
| [ ] | M6 | P1 | ELF loader doesn't validate `p_filesz<=p_memsz` or relocations; ignores read results. | Validate fields and check every read. |
| [ ] | M7 | P2 | `kmalloc()` pool never initialized → always NULL. | Wire up the pool or remove dead reliance. |
| [ ] | M8 | P2 | No `BUG_ON`/`WARN_ON`, no stack protector. | Add macros + `-fstack-protector-strong`. |
| [ ] | M9 | P2 | OFT/ITAB/PTAB may be too small for the full compiler chain. | Resize if load tests show ENFILE/EAGAIN; keep map under stack limit. |

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

**L1 (full) — continuous kernel log with rotation.**
*Target:* today we only have two static files (`boot.log` from startup text,
`crash.log` from the last fatal fault). There is no running log of kernel events,
so intermittent warnings that don't crash the machine are lost.
*Do:* add a `klog.c` ring that `kputchar()` feeds (a single choke point for all
kernel messages), expose it as a `/dev/klog` device with peek/drain/flush ioctls,
and write a `klogd` daemon that appends to `/var/log/klog`, rotates it, and archives
old copies to `/mnt/sd/log/`. Requires shrinking `USERMEM` by one block to make RAM.
*Done when:* a forced panic plus a watchdog reboot leaves a timestamped log on disk
and an archived copy on SD, with the memory map still under `0x20040000`.

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

**I6 — POSIX tty ioctl semantics.**
*Target:* `TIOCINQ` returns raw queued bytes rather than bytes up to the next newline
in canonical mode, canonical `select()` is unimplemented, `VSUSP` is missing, and
^D prints a spurious BEL.
*Do:* correct canonical-mode byte counting, wire `SELECT_IN` readiness, implement
`VSUSP` if job control is enabled, and fix the `VEOF` echo condition.
*Done when:* `select()` and `read()` agree about pending input and ^D is quiet.

**M4 — user-stack high-water marks.**
*Target:* the 7 KB user stack can underflow below `PROGBASE` into kernel tables, and
the actual headroom under the compiler chain was never measured.
*Do:* fill a sentinel at exec, check the high-water mark in the tick, and produce a
`-fstack-usage` report to confirm headroom under fcc/cpp.
*Done when:* the report shows positive headroom and an overflow attempt warns before
corrupting kernel tables.

**M5 — swap sizing and OOM behavior.**
*Target:* swap reserved 728 blocks for a 512-block partition; the second map's I/O
was silently ignored, and out-of-memory called `panic()` and killed the whole system.
*Do:* set `MAX_SWAPS 1`, check every swap read/write return, and on OOM kill only the
offending process (with a log) instead of panicking.
*Done when:* compiling under memory pressure never panics the whole machine and swap
I/O failures are reported.

**M6 — ELF loader validation.**
*Target:* the loader doesn't check `p_filesz <= p_memsz`, doesn't bound/align
relocations, and ignores `readi()` results — a malformed binary could execute stale
memory.
*Do:* validate every ELF field, lower-bound and align relocations, and check all read
results.
*Done when:* a truncated or hostile ELF is rejected cleanly with `errno` set.

**M7 — initialize or remove `kmalloc()`.**
*Target:* the kernel malloc pool is compiled in but `kmemaddblk()` is never called, so
it always returns NULL; drivers that use it fail silently.
*Do:* either register a real memory block with the pool or remove the dead code and
its callers.
*Done when:* no driver silently fails because of an uninitialized allocator.

**M8 — assertions and stack protector.**
*Target:* there is no `BUG_ON`/`WARN_ON` framework and no `-fstack-protector`, despite
explicit stack-pointer switch points in `tricks.S`.
*Do:* add assertion macros and compile with `-fstack-protector-strong` plus a seeded
canary and `__stack_chk_fail`.
*Done when:* invariants fire warnings in a test build and the image still fits SRAM.

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

**K5 — a minimal device model with `probe()`.**
*Target:* there is no `probe()`/`remove()`; the stubbed `rawuart` driver registers as
a real device even though the hardware is absent.
*Do:* add `probe`/`remove` to `struct ttydriver`; have `rawuart_probe()` fail so it
isn't registered.
*Done when:* absent hardware probes out cleanly with no dead device node.

**D5 — keep the correct barriers (no work expected).**
*Target:* the SPSC ring barriers and the bounded flash lockout are already correct.
*Do:* leave unchanged; optionally give the barriers named wrappers for readability
and publish core1 readiness with a release fence.
*Done when:* no regression in the lockout/USB soak tests.

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

*All 10 agent reports integrated; no deep-dives pending.*
