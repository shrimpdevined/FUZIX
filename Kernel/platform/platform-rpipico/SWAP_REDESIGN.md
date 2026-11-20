# FUZIX RP2040 — Swap Redesign (ground-up)

**Status:** DESIGN AUTHORITATIVE — implementation in progress.
**Baseline:** commit `859cc46` ("Plan: sync phase summaries...").
**Last updated:** 2026-11-20.
**Research lineage:** 5-agent swarm (Linux swap internals; no-MMU MCU memory
management; RP2040 hardware feasibility; FUZIX design critique; synthesis) plus
an earlier 4-agent trace-analysis swarm. Raw findings are summarized in §1–§3;
this document is the single source of truth for the redesign.

---

## Naming convention (Linux, exactly)

One swap system, one vocabulary. All swap code uses Linux kernel names and
Linux-style semantics — no FUZIX-local aliases, no second implementation of the
same operation. The rename applies tree-wide so there is only one API.

| Old FUZIX name | New name (Linux) | Linux reference | Meaning |
|---|---|---|---|
| `swapmap_add()` | **`swap_free(entry)`** | `mm/swapfile.c:swap_free()` | Return one slot to the free pool. Idempotent: a second free of the same entry is a no-op (Linux warns). |
| `swapmap_alloc()` | **`swap_alloc()`** | `mm/swapfile.c:folio_alloc_swap()` | Reserve one slot from the free pool. Returns `SWP_ENTRY_NONE` on exhaustion. |
| `swapmap_init()` | **`swap_init(slot)`** | `mm/swapfile.c:enable_swap_info()` | Register one slot at `swapon` time. |
| `uint_fast8_t map` / `int16_t map` | **`swp_entry_t entry`** | `include/linux/swap.h:swp_entry_t` | Opaque slot handle. `typedef uint16_t swp_entry_t;` |
| `0` as "no map" | **`SWP_ENTRY_NONE`** | `SWP_ENTRY(0,0)` sentinel role | Distinct from slot 0, which is a valid slot. |
| `swapmap[]` | **`swap_map[]`** | `mm/swapfile.c:swap_map` | Slot state array (free list / owner / refcount). |
| `swapptr` | **`nr_swap_free`** | `nr_swap_pages` | Count of free slots. |
| `swapper2()` / `swapper()` | **removed** | — | Legacy banked-MMU swap-in path, dead on this port. One release contract only. |
| `#ifdef CONFIG_SWAPPER` daemon | **removed** | — | Duplicate swapper task, never compiled in this tree. |

Rules:

* `swap_free()` is the **only** function that returns a slot. Every success and
  error path calls it. There is no second release path in any platform file.
* `swap_alloc()` / `swap_free()` operate on `swp_entry_t`, never on a bare
  integer that could be confused with `0 == no slot`.
* Process-facing helper names follow Linux's object ownership:
  `put_swap_page(p)` drops the slot held by a process (the Linux folio analogue),
  `swap_in(p)` / `swap_out(p)` describe the two I/O directions, and
  `proc_release(p)` is the single teardown entry that calls `put_swap_page()`.
* State names mirror Linux reclaim states: `SW_RESIDENT`, `SWAPPED`,
  `SWAP_IN_PENDING`, `SW_BARE`.

---

## 0. Why this document exists

`MASTER_PLAN.md` recorded swap as an *accepted* hardware/design limit (issue M5):
`MAX_SWAPS 1`, checked I/O, OOM logged before reset. That conclusion was wrong in
one important respect: the `Swap: 182 182 0` state after a compile is **not**
solely a hardware limit — it is a **slot-ownership and victim-selection design
flaw**. The RP2040's lack of an MMU makes some strategies impossible (§2), but
it does not force the leak, and it does not force the HardFaults we observed
while trying partial fixes.

This document records the evidence, the design that removes those flaws, and
the acceptance tests. Do not make a swap-related code change that is not
tracked here.

---

## 1. Evidence: what actually fails

All observations below are from the device at commit `859cc46` unless noted.

| Symptom | Evidence | Mechanism |
|---|---|---|
| After one `cc`, `free` shows `Swap: 182 182 0` permanently | `swap_trace_run.log`: `SWTRACE swout ALLOC pid 1 slot 0 map 0 used 182`, `SWTRACE swout DONE pid 1`, **no `swin` for pid 1**; `ps -ef` shows all processes resident | `swapvictim()` picks the longest-sleeping process (`Kernel/swap.c:134-155`). init (pid 1) has slept since boot → always chosen → never wakes → never swapped in → slot never released |
| Only release site is swap-in | `Kernel/platform/platform-rpipico/swapper.c:249-258` is the sole live `swapmap_add()`; the generic `swap.c:192` is dead code here | A process exiting/killed while swapped does **not** release its slot |
| Skipping init → compile OOM | `swap_trace_fixed.log`: `swout ALLOC pid 13 (klogd)`, then `swout ALLOC pid 20 map -1`, `warning: out of memory`, `fcc: out of memory` | klogd is small; frees too few blocks; the only slot is held so a second victim cannot be swapped out |
| Skipping daemons → hard fault | `reset_capture.log`, `EXCEPTION 3` with PC/LR in user RAM (`pc=2000dc70`, `lr=2000dba1`, `user-rel pc=43d8`) | Swapping an active process in mid-compile relocates its blocks; the live syscall stack is inside that memory (§3.2) |
| 2 maps + protect init → hard fault on first compile | `repro2.log`, device boot-looped; `known_good_backup.uf2` restored | Map count was not the fault source; relocation was |
| Historical corruption | `swapper.c:397-399` comment: unchecked `b - allocation_map` on NULL produced a wild pointer, "panic: inode freed / EXCEPTION 3" | Swap-in OOM wrote 2 KB to a wild address |

**Conclusion:** the leak is a slot-ownership flaw; the faults are relocation-context
flaws. Both are fixable in software. Neither requires an MMU.

---

## 2. Hardware facts (correcting the record)

`MASTER_PLAN.md:147-148` says the RP2040 "has no MMU/MPU and no unprivileged mode".
That is factually wrong and is corrected as part of Phase 0:

* The RP2040 **has** an ARMv6-M PMSAv7 MPU: 8 unified regions per core, 8
  subregions each, XN and access permissions, and the optional unprivileged/
  privileged extension (`CONTROL.nPRIV`). `MPU_TYPE` is nonzero. Datasheet §2.4.2.5,
  §2.4.6, Table 86; ARM DDI 0484 §B3-635.
* It has **no address translation**: no page tables, no TLB, no relocation
  registers. Virtual address == physical address, always.
* MPU violations raise HardFault (IPSR 3); there is no MemManage/BusFault on
  Armv6-M. DMA and the other core bypass the core's MPU.
* What the MPU *can* do for us: protect `__StackTop`/kernel RAM during swap
  windows (§5.7). What it *cannot* do: isolate processes at 2 KB-block
  granularity or implement COW/guard pages — one region is a single
  power-of-two-aligned range, and a process owns up to 91 scattered blocks.

Consequences that constrain the design:

1. A process's logical addresses are **fixed** (`PROGBASE + i*2048`). Only a
   *suspended* process's blocks may be moved. The running process's block 0
   holds `udata`, which must stay at `progbase` (`config.h:131`, `tricks.S:6`).
2. There are no page faults. Swap-in must be triggered explicitly (scheduler
   transition), not by hardware.
3. Flash writes require IRQs off, core1 locked out, and code executing from
   SRAM (`rawflash.c:17-22,76-99`). A 182 KB image spans ~46 × 4 KB sectors;
   swap-out is ~0.2–3 s, swap-in ~20–60 ms including FTL overhead.
4. The swapper stack at `__StackTop` is 2 KB (`.stack_dummy = 0x800`); the
   Dhara/flash call chain from `swapin` is deep enough to be a risk.

---

## 3. Linux lessons: portable vs MMU-only

### 3.1 Portable (adopt)

* **Slot = first-class, refcounted resource**, independent of any process
  (`swp_entry_t`, `swap_map`/swap table in `mm/swapfile.c`).
* **Ownership belongs to the memory object**, not the process's schedule state;
  a process entry holds a reference. Slot lives iff a holder exists.
* **One idempotent release path**: `folio_put_swap`/`folio_free_swap` →
  `swap_free`; double-drop is a warning, not silent corruption. Every unmap,
  exit, COW, swapoff path funnels through it.
* **Reserve before restore**: `sys_swapoff()`/`try_to_unuse()` may fail ENOMEM
  rather than restore partially — the direct analogue of our all-or-nothing
  swap-in.
* **Reclaim only cold, promptly-resumable victims** (LRU active/inactive,
  `folio_referenced()`, pinned/`mlock` exclusions).
* **Protect critical tasks**: `oom_score_adj = -1000` for init/daemons,
  `PF_KTHREAD` skip in `oom_evaluate_task()`.
* **Pressure escalation**: throttle → reclaim → fail allocation → OOM. Never
  orphan a slot; no periodic "swap cleaner" (slots recycle on last put).

### 3.2 MMU-only (do not port)

Page-fault-triggered swap-in (`do_swap_page`), per-page demand paging, PTEs,
COW via write-protection, `mmap`/VMA population, GUP/rmap, hardware
accessed/young-bit hotness, PTE-encoded `swp_entry_t`.

### 3.3 Why no mainstream MCU RTOS swaps

uClinux, Zephyr (MPU targets), FreeRTOS, RIOT, ThreadX, NuttX (flat/protected),
and Tock all fail allocation rather than overcommit; their paging modes require
an MMU. The historical exception is exactly UZI/FUZIX-style *total swapping*,
whose correctness depends on a single never-swappable resume path. NuttX's
paging worker and Zephyr's removed `__pinned_*` scheme both show the same rule:
the resumer must be permanently resident, priority-boosted, and have a timeout
for "fills that never complete".

---

## 4. Design

### 4.1 Invariants (the contract)

1. **Single owner.** A swap slot is owned by exactly one of: the free list, or
   one process in `SWAPPED` / `SWAP_IN_PENDING`. Never by a bare `p_page2` with
   no state.
2. **Single release.** `swap_slot_release(p)` is the only function that returns
   a slot to the free list. It is idempotent and is called on *every* teardown:
   swap-in completion, `doexit`, fork failure, `wait` reap, and fatal signal of
   a swapped process.
3. **Explicit states.** `p_swstate` replaces the overloaded `p_page` boolean:
   `BARE`, `RESIDENT`, `SWAPPED`, `SWAP_IN_PENDING`. `p_page2 = SWAP_NO_MAP
   (0xFFFF)` means "no map"; `0` is a valid map and must never mean "none".
4. **Reserve, then act.** Before the first `swapread`, reserve every destination
   block; commit `allocation_map` and state in one atomic pass at the end. On
   any failure, roll back — never a partial image, never a panic.
5. **Guaranteed progress.** Never evict: the current process, init,
   `PFL_NOEVICT` daemons, `P_FORKING`, pending-signal targets, or anything in
   `SWAP_IN_PENDING`. A swapped process that receives a fatal signal is
   released in O(1) **without** being swapped in.
6. **Safe relocation.** No `udata` dereference, `kprintf`, or
   `debug_blocks()` after the block-0 exchange in `contextswitch`. Swap I/O
   scratch lives in kernel `.bss`, not `udata`. IRQs are masked during block
   permutation. The syscall kernel stack is moved out of the relocatable pool
   (or `__StackTop` is used for *every* `contextswitch`, including `brk`).
7. **One ownership contract.** Delete the dead generic `swapper()`/`swapper2()`
   and `#ifdef CONFIG_SWAPPER` paths so two implementations cannot diverge.

### 4.2 State machine

```
  ptab_alloc / pagemap_alloc                 swapout (all writes OK)
  BARE ───────────────────────► RESIDENT ───────────────────────► SWAPPED
   ▲                              ▲                                  │
   │ proc_release                 │ swapin OK, slot released         │ wake/kill/schedule
   │ (exit, fork fail, reap,      │                                  ▼
   │  fatal signal)               └──────────── SWAP_IN_PENDING ◄────┘
   │                                             (slot reserved, not schedulable)
   └────────────────────────────────────────────────┘
       proc_release from SWAPPED releases slot, sets BARE
```

Scheduler rule: `getproc()` returns only `RESIDENT` processes. A `SWAPPED`
process that wakes is moved to `SWAP_IN_PENDING` and enqueued (a 2-byte
bitmask), never made `P_READY` while non-resident. `contextswitch()` is the only
place that completes `SWAP_IN_PENDING → RESIDENT`.

### 4.3 Data structures

```c
/* kernel.h — append to struct p_tab, after p_timerq so kernel.def offsets stay */
uint8_t  p_swstate;       /* SW_BARE / SW_RESIDENT / SWAPPED / SWAP_IN_PENDING */
uint8_t  p_swapin_err;

/* kernel.h */
#define SWAP_NO_MAP 0xFFFFu
#define PFL_NOEVICT 0x20u  /* free bit in p_flags (1/2/4/8/16 used) */

/* swap.c */
static uint8_t swap_owner[MAX_SWAPS];  /* ptab slot or SWAP_NO_OWNER */
```

`p_page` remains a compatibility bit (`1` only when resident). `PFL_SWAPIN`
is redefined as "swap-in pending"; the old `CONFIG_SWAPPER` code that set it
is deleted.

### 4.4 Key routines

| Routine | File | Contract |
|---|---|---|
| `swap_slot_reserve()` / `swap_slot_release()` | `Kernel/swap.c` | Only slot allocators; idempotent release with owner table |
| `proc_release(ptptr p)` | `Kernel/process.c` | Releases slot if held, frees RAM if resident, sets `BARE`; called from every teardown path |
| `swapvictim()` (rewrite) | `Kernel/swap.c` | Hard exclusions (§4.1.5); prefer bounded sleeps; fail cleanly with `ENOMEM` if no candidate |
| `swapout()` (atomic) | `platform-rpipico/swapper.c` | Write all blocks, then commit tags + state; on error leave state untouched |
| `swapin()` (atomic) | `platform-rpipico/swapper.c` | Require `SWAP_IN_PENDING`; pre-reserve blocks; read all; commit tags + `RESIDENT`; release slot once |
| `contextswitch()` | `platform-rpipico/swapper.c` | Preflight state + free-block budget; no `udata` use after block-0 exchange |
| `ssig()` fatal path | `Kernel/process.c` | If target non-resident: set `P_ZOMBIE`, `proc_release()`, wake parent — no swap-in |

---

## 5. Phased implementation (each stage revertible)

| Phase | Change | Gate |
|---|---|---|
| **0** | Documentation (this file + `MASTER_PLAN.md` corrections). Backup known-good UF2 | **DONE** — docs written, hardware facts corrected, backup at `known_good_backup.uf2` |
| **1** | Tree-wide Linux rename (`swap_free`, `swap_alloc`, `swap_init`, `swp_entry_t`, `SWP_ENTRY_NONE`, `put_swap_page`); slot owner table + idempotent release; `swap_hold()` at swap-out; `put_swap_page()` on swap-in, `doexit`, fork-failure; dead `CONFIG_SWAPPER` task removed | **DONE & DEVICE-VERIFIED** — clean build, boot, compile produces binary, no warnings, no regression. `free` still `182/182/0` after compile (old victim-selection leak persists until Phase 3, as expected) |
| **2** | Stack/udata safety: swap scratch to `.bss`, stack switch in `pagemap_realloc`, larger swap stack, IRQ masking, MPU protection of kernel RAM | Compile soak, no HardFault |
| **3** | Atomic swapout/swapin with reservation; new victim policy; swapped-kill | `free` returns to `182/0/182` after each compile |
| **4** | Truthful accounting from slot table; clean `ENOMEM` instead of panic; delete dead paths | 50-compile stress, pipes, fsck, diag all clean |
| **5** | Full device soak + sign-off; run equivalent conformance suite | All acceptance tests below pass |

Rollback at any stage: revert the stage's commit; the stage-0 UF2
(`/home/qlms/Documents/fuzix-build/known_good_backup.uf2`) is the hardware fallback.

---

## 6. Acceptance tests

1. **Idle baseline:** `free` → `Swap: 182 0 182`.
2. **Compile cycle ×3:** after each `cc`, `free` → `Swap: 182 0 182` once the
   shell is idle. No `out of memory`, no sticky `used`.
3. **Stress ×50:** repeated compiles; no reset, no `EXCEPTION 3` in
   `/var/log/crash.log`, `picoctl diag` heartbeat rising, `vt_dropped == 0`.
4. **Kill swapped process:** with swap on, force a victim out, `kill -9` it;
   slot must be immediately reusable (`free` used drops) without swapping it in.
5. **OOM path:** request > ~180 KB via `brk`; expect clean `ENOMEM` to the
   process, no panic, no half-swapped state.
6. **Pipes regression:** `tests/test_pipes.py` 5/5.
7. **Filesystem:** `fsck -a /` clean after the soak.
8. **No canary trip:** M1 canary and M8 stack protector clean.

---

## 6b. Failed experiment: victim policy (DO NOT re-apply as-is)

Tried after Phase 2: skip init (`p_pid == 1`) and no-TTY daemons
(`p_tty == 0`) in `swapvictim()` (`Kernel/swap.c`). Result on device:

* Build clean; boot OK; first two compiles produced binaries.
* The shell (pid 20) became the victim. Since it wakes on every child exit
  during the compiler pipeline, it was swapped out and back in repeatedly —
  the machine became very slow, then reset/boot-looped on the third compile.
* A fresh crash log was not produced (watchdog reset, not a panic), so the
  exact fault remains unproven; the observation is consistent with
  swap-in OOM/thrash, not with the Phase 2 relocation fix failing.

This confirms that simply changing *who* is evicted is insufficient: the
victim must be a process whose resume is bounded and whose RAM can be
guaranteed at swap-in time (§4.1.4–5). The change was reverted; master stays
on the stable Phase 2 baseline.

---

## 7. Open decisions (resolved during implementation)

* **Kernel stack location:** prefer a fixed stack in SRAM4 (`0x20040000`) over
  per-call stack switching; revisit if SRAM4 is needed elsewhere.
* **MAX_SWAPS:** keep `1` until Phase 3 proves the state machine; raise to `2`
  only if the 2-map liveness deadlock recurs with a strict owner table.
* **MPU use:** Phase 2 may add a temporary no-access region over the user pool
  during the swap window to catch stray accesses; not required for correctness.

---

## 8. "Never again" checklist

* `p_page == 0` without a valid, exclusively-owned slot and an explicit state.
* A slot both in the free list and in a process's `p_page2`.
* Exit/kill/reap of a swapped process without `swap_slot_release()`.
* init, a no-timeout daemon, `P_READY`/`P_RUNNING`, or `P_FORKING` as victim.
* Scheduler selecting a non-`RESIDENT` process.
* `swapin()` reading into an unreserved block or panicking after block *k*.
* `swapout()` freeing a block before all its writes succeed.
* `p_page2 == 0` meaning "no map".
* `udata` dereference / `kprintf` after the block-0 exchange.
* Swap I/O on the 2 KB stack without `inswap` or with IRQs enabled during
  permutation.
* Two owners of the release contract (generic `swapper2()` vs platform code).
* Relying on "oldest waiter" scoring for progress rather than explicit release.
