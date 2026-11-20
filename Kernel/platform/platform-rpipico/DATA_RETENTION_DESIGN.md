# FUZIX RP2040 — Data Retention & Stream Processing Design

**Status:** DESIGN — direction approved, first increment in progress.
**Last updated:** 2026-11-20.
**Scope:** this RP2040 system is a **data logger / stream processor**, not a
flight controller. The goal is to retain large volumes of data and process
them through 264 KB of SRAM without corruption. Hard real-time / RMS / EDF /
thruster deadlines are explicitly **out of scope**.

**Predecessor docs:** `MASTER_PLAN.md` (authoritative status),
`SWAP_REDESIGN.md` (swap redesign; boot-loop root cause in §5a).

---

## 1. Engineering constraints (what actually matters here)

1. **Data integrity across unclean shutdown** is the primary enemy — not
   microsecond latency. The boot-loop we just fixed (`d18eab6`) was exactly
   this: `sync()` wrote a stale buffer over hda1's superblock.
2. **Large files through small RAM**: the self-hosting compiler chain already
   proves the model works (swap + 2 KB blocks). Streaming must not blow the
   202 KB user pool or fragment flash.
3. **Bounded I/O latency**: a wedged SD/NAND chip must not hang the OS.
   Existing mechanisms: F5 bounded SD retry, `rawflash.c` bounded lockout
   (3×200 ms), 5 s watchdog fed by the 200 Hz tick.
4. **Detectable corruption**: silent wrong data is worse than a clean failure.

---

## 2. Assessment of external proposals (Gemini, 2026-11-20)

A review proposed an "aerospace-grade" rebuild. Evaluated against this tree:

| Proposal | Decision | Rationale |
|---|---|---|
| Preemptive RMS/EDF scheduler, tickless kernel | **REJECT** | No hard deadlines; would break the proven scheduler and timers. |
| Kill swap entirely | **REJECT** | Without swap the compiler fails `ENOMEM` (measured). Swap is what enables large-file processing in 202 KB. |
| Replace V7 filesystem with LittleFS | **REJECT** | V7 is not the defect — our dropped `blkfromk()` was. A rewrite breaks every tool and the boot path. |
| TMR / software ECC on kernel state | **DEFER** | Large RAM/CPU cost; no requirement. Revisit only if SEU evidence appears. |
| Zero-copy DMA streaming | **DEFER** | XIP already avoids code copy; SD DMA is a large, separate project. |
| Static allocation, no malloc | **ALREADY TRUE** | `malloc` is unseeded/unused; data path uses static buffers. |
| Strict I/O timeouts | **ADOPT** | Extend the existing bounded-retry/lockout pattern to all block paths. |
| Watchdog tied to processing task | **ADOPT (cheap)** | Add a userspace heartbeat/pipeline-deadline check; keep the 5 s hardware watchdog. |
| CRC on stored data chunks | **ADOPT** | Userspace-only, incremental, directly improves retention. |
| MPU protection of critical tasks | **DEFER** | RP2040 has an 8-region MPU but 2 KB scattered blocks cannot be isolated; protect only kernel/swap stacks (see `SWAP_REDESIGN.md §2`). |

**Principle:** harden what exists; do not rewrite a working system to satisfy
a generic checklist.

---

## 3. Grounded gaps to close (in priority order)

### G1 — Boot `fsck -a` validates nothing (P0, directly blocks retention)

`Applications/util/fsck-fuzix.c:230,235`: in `-a` mode the tool returns
before `pass1()`–`pass5()`; it only clears `s_fmod` and writes the
superblock. So `rc:2`'s `fsck -a /` and `rc:21`'s `fsck -a /dev/hda2`
provide **zero validation**. The two wrong-block writes at
`fsck-fuzix.c:414` and `:877-878` were fixed in `d18eab6`, so making `-a`
actually run the passes is now safe.

**Fix:** remove both early returns; let `yflag` auto-confirm prompts and run
all passes; clear `FMOD_CLEAN` only after a clean pass. Validate on device
with a deliberately dirtied image.

### G2 — Data integrity verification tool (P1)

Add a userspace `CRC`-style verifier for data files:
* append a fixed-size trailer `{length, crc32, magic}` to each logged record;
* `cksum`-like utility to verify a file and report the first bad record;
* streaming implementation using a static 4 KB buffer (no malloc), so it
  works on files far larger than RAM.

### G3 — Bounded I/O audit (P1)

Confirm every block-device path has a timeout: SD retry (F5), core1 lockout,
flash erase/program. Ensure no unbounded `while()` remains in
`devsd*.c`/`rawflash.c`. Add explicit timeout to any path that lacks one.

### G4 — Pipeline watchdog heartbeat (P2)

Userspace heartbeat file/flag that a monitoring task updates; if it goes
stale beyond a deadline, trigger a controlled reset with a diagnostic. Keep
the existing 5 s hardware watchdog as the backstop.

### G5 — Resume/streaming API (P2)

Once G1–G3 are solid, add a streaming read/write API for large files that
uses fixed chunks and returns errors explicitly, suitable for data-logging
apps. Build on the existing `read`/`write` syscalls (no kernel rewrite).

---

## 4. Non-goals

* No scheduler rewrite; no tickless kernel; no RMS/EDF.
* No filesystem replacement.
* No TMR/ECC; no DMA rewrite unless a measured requirement appears.
* No swap removal (the compiler depends on it).

---

## 5. Acceptance criteria (overall)

* Power-cut / forced-reset soak: after 50 compile+write cycles and forced
  reboots, `fsck` reports a clean filesystem and all CRC-verified data files
  pass.
* `fsck -a` actually detects a deliberately corrupted inode/block and reports
  it (not a silent no-op).
* Every block I/O path has a bounded timeout; no infinite `while()` on
  device non-response.
* 17/17 image-layout tests, pipes 5/5, `vt_dropped == 0`, no canary trip.

---

## 6. Status

* **G1 — DONE & DEVICE-VERIFIED (`b4a5641`):** `fsck -a` runs all five
  passes with the `!rootfs`/`!aflag` guards (no boot reboot, no write through
  the mounted root), rescan-capped. Verified: fresh boot shows all pass
  messages, mounts OK, no panic.
* **G2 — DONE & DEVICE-VERIFIED (`b4a5641`):** `/bin/verify` streaming
  CRC32 tool. Verified: good file = 3 records OK/rc0; flipped byte = CRC
  mismatch/rc1; truncated = detected/rc1.
* **G3 — DONE & DEVICE-VERIFIED (`b4a5641`):** `bdput`/`freebuf` retain
  `bf_dirty` on failed writes; `bufsync` returns status folded into `sync()`;
  typed EIO on FTL/BLKFLSBUF failures; full LBA range check; one FTL
  checkpoint per barrier.
* G4 (pipeline watchdog heartbeat) — planned.
* G5 (streaming read/write API for data-logging apps) — planned.
