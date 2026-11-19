# fcc — FUZIX self-hosted C compiler: program status

Goal: compile full C90 on the Pico under FUZIX, emit raw ARM Thumb-1 code,
and run the result from the SD card.  Scope locked: full C90 language
coverage verified by a conformance suite, POSIX-style interfaces, and a
codebase following a documented NASA-style coding standard.

## Stages and exit gates

| Stage | Gate | Status |
|---|---|---|
| 0. Loader | `runbin` executes a raw Thumb-1 binary loaded from `/mnt/sd`. | **DONE** — returned 42 from a hand-assembled 4-byte binary. |
| 1. Baseline | Host harness (Unicorn emulator + Capstone disassembler) runs flat binaries; compiler base chosen; coding standard written. | IN PROGRESS |
| 2. Backend | Every IR opcode emits correct Thumb-1; all host tests pass. | **DONE** — 12/12 Unicorn host tests pass (arithmetic, locals, comparisons, calls, branches, literals, syscalls, shifts). |
| 3. End-to-end | On device: `.c` on SD → compile → `.bin` on SD → `runbin`. | **DONE** — `fcc` compiled `int main(...){return 42;}` into a 24-byte flat Thumb-1 binary; `runbin` executed it, exit code 42. |
| 4. Full C90 | Preprocessor + full C90 type system; conformance suite passes. | **DONE — 57/57 on-device conformance tests pass** via the `cc` driver with the upgraded runner `fcctest4.py` (which, unlike `fcctest3.py`, checks compile diagnostics and anchors the exit marker to a full line). Covered: arithmetic, locals, control flow, recursion, functions, globals, pointers, char, strings, unsigned DIV/MOD, typedef, enums, switch, 1-D/multi-D arrays, `sizeof`, storage classes, signed char, casts/conversions, **bit-fields**, **struct return-by-value**, **struct parameters by value**, **float**, **double**, **prototypes**, **full escape sequences**, **int suffixes**, **sizeof unary**, **void**, **K&R definitions**, **1024 macros**, **wide characters**, **function pointers**, and **file-scope object initializers** (scalar/char, folded constant expressions, brace-enclosed 1-D/nested arrays, comma-separated declarators) — **FULL C90 LANGUAGE COVERAGE**. |
| 5. Hardening | Codebase conforms to CODING_STANDARD.md; static checks + coverage pass. | TODO |

## Key facts established

- RP2040 is a **32-bit** Cortex-M0+; it runs 16-bit-wide Thumb-1 instructions.
  No MPU, no caches, `CONFIG_USERMEM_DIRECT`: heap memory is executable.
- **32-bit int compliance is explicit and host-independent.** `tsize[INT]` and
  `tsize[UINT]` are hard-wired to `4` (not `sizeof(int)` of the compiler's own
  build). The target was always 32-bit — verified on-device `sizeof(int)==4` —
  but using the literal removes any dependence on how `fcc` itself is built.
  String-literal data-pool alignment likewise uses literal `4` (previously
  `sizeof(int)`, which was the widened `long`=8 on a 64-bit host build).
  `cpp` provides C90 `limits.h` with `INT_MAX 2147483647`; the Thumb backend
  uses 4-byte stack slots, 32-bit literals and a 32-step divider helper.
- **C90 file-scope initializers are supported** (`stmt()`, case Glo, and the
  `glo_init_list()` helper). A constant expression is folded by `expr()` to a
  `Num` node and its bytes are written directly into the data-pool image at
  `id->val`, which the backend appends verbatim to the flat binary — there is
  no startup-constructor runtime. Supports scalar/char, folded constant
  expressions, brace-enclosed 1-D and nested arrays, and comma-separated
  declarators. Address/string-pointer initializers are rejected with a clear
  diagnostic until data-pool relocation is implemented. Tests 063–066.
- Note: the earlier `fcctest3.py` runner discarded compile output and used a
  loose `EXIT=0` substring match; it reported 48/53 with 5 bogus "expected"
  failures. `fcctest4.py` replaced it and shows 57/57.
- FUZIX ARM syscalls: `svc #n`, number in the immediate, args in r0-r3,
  return in r0 (errno in r1).  Syscall numbers in `Kernel/include/syscall_name.h`.
- Process ceiling: **190 KB** (`PROGSIZE (190*1024 - UDATA_SIZE)`) in the
  210 KB user pool.  Swap on hda3 (256 KB) lets the kernel page out the
  suspended shell while `cpp`/`fcc` run, giving the compiler enough headroom.
- Executable format is bypassed: flat raw Thumb-1 binaries, loaded by `runbin`.
- SD card auto-mounts at `/mnt/sd` (`/dev/hdb1`, FAT32).
- Host test harness: `host/thumbrun.py` (Unicorn 2.1.4, Capstone 5.0.7).
- String literals / globals ARE embedded in the flat binary via a PC-relative
  `DATAPTR` opcode (`add r0,pc,#imm` encodes imm in 4-byte units).  Verified:
  `char *p="A"; return p[0];` → 65, and `write(1,"HI\n",3)` prints `HI`.
- Implemented in Stage 4 so far: `cpp` preprocessor (macros, `#if`/`#ifdef`,
  quoted includes), `cc` C driver (fork cpp, exec fcc), software DIV/MOD
  helper (`divhelp.S` bytes embedded in the backend), PC-relative data
  references for string literals.
- Type system implemented: `typedef` (cpp alias), `long`/`short`→`int`,
  qualifier stripping, arrays (1-D and multidimensional), structs,
  enums, pointers, `sizeof`, switch/case.  Unsigned arithmetic (UDIV/UMOD/LSR
  and unsigned comparisons) is implemented in the Thumb back end.
- Nested/multidimensional arrays: array types use descriptors (`arr_new`)
  carrying element type, dimension length and total byte size.  `int a[2][3]`
  yields `sizeof(a)==24` and `a[1][2]` indexes correctly.  Parameter decay and
  pointer arithmetic use the element stride carried by the descriptor.
  Test: `host/conformance/042_2darray.c`.
- Persistent static locals: `static` inside a function allocates storage in
  the data segment (IR class `Stc`) instead of the stack, so a counter keeps
  its value across calls.  Function-scope name uniqueness is preserved by the
  existing hclass/htype/hval save-restore.  Test: `043_static_local.c`.
- Struct assignment by value: `b = a` copies all words of a struct via a new
  `SCPY` IR opcode.  Test: `044_struct_copy.c`.
- Struct return-by-value: the caller reserves a temporary and passes a hidden
  destination pointer as the last argument (`StructFunc` AST marker); `return`
  copies the result words there with `SCPY`.  Test: `047_struct_return.c`.
  Struct *parameters* by value remain rejected (`struct parameters must be
  pointers`).
- Conversions: `(char)`, `(unsigned char)`, `(unsigned)` casts are supported.
  Narrow casts emit `SXT8`/`ZXT8` IR; signed `char` loads now sign-extend via
  `sxtb` in `thumb.c`.  Test: `045_cast_conv.c`.
- Floating point (single precision): `float` declarations, literals, casts,
  arithmetic (`+ - * /`), comparisons, unary minus, globals, struct members,
  and function args/returns are supported.  Values are IEEE-754 bit patterns
  in one 32-bit IR word.  Soft-float helpers (from `fp_lib.c`, linked against
  the v6-m nofp libgcc) are embedded as position-independent Thumb-1 bytes in
  `fp_helpers.h` and called by new IR opcodes 220–231.  Division uses a
  bitwise long-division routine (libgcc's `divsf3` needs an absolute `.rodata`
  jump table, which cannot work in a malloc'd flat binary).  FUZIX's `strtod`
  has a fractional-digit bug, so `fcc_strtod()` parses constants correctly.
  `double` is still rejected with a clear fatal diagnostic (it needs a
  two-word value / AAPCS register-pair ABI).  Tests: `048_float.c`,
  `049_float_more.c`.
- Bit-fields: `int x:N` in structs are packed into 32-bit units; extract/insert
  is synthesized from existing `LSR`/`SHR`/`AND`/`SHL`/`OR`/`XOR`/`SI` IR.
  Signed fields are sign-extended.  Test: `046_bitfield.c` (was a known failure).
- Double precision: `double` values are represented by the ADDRESS of an 8-byte
  slot (like structs), so the single-word IR is preserved.  Helpers take
  `(dst, left, right)` pointers and return `dst` in r0; division uses 53-bit
  restoring division on the mantissa (avoids libgcc's `.rodata` jump tables).
  Double globals, locals, struct members, casts to/from int/float, function
  args/returns (hidden destination pointer) and all arithmetic/comparisons
  work.  Tests: `050_double.c`, `051_double_more.c`.
- Struct parameters by value: the caller pushes each word of the struct with
  a new `PUSHS` IR opcode; the callee's parameter slot is rebased so member
  offsets increase upward.  Test: `052_struct_param.c`.
- Multi-character constants (`'AB'`) are implementation-defined in C90;
  fcc takes the last character, which is a conforming implementation choice.
- Function pointers: `int (*fp)(int)`, `&func`, `(*fp)()`, function-pointer
  parameters work (new CODEADDR/JSRI opcodes, indirect `blx r0`); test
  `062_fnptr.c`.  File-scope initializers of function pointers (e.g.
  `int (*fp)(int) = add;`) are not yet supported — they produce a data
  address that needs relocation; use a local assignment instead.
- C90 translation limits: 1024 macros (compact pool storage, BSS lower than
  before), 31 macro parameters.  Swap on hda3 (256 KB) gives the compiler
  enough headroom to use them on-device; test `060_1024_macros.py`.
- Wide characters: `L'a'`, `L'\101'`, `L'\x41'`, `L"..."`, `sizeof(L"abc")==16`,
  `wchar_t` (via `<stddef.h>`); test `061_wchar.c`.
- Basic escape sequences: implemented (full C90 set); test `055_escape.c`.
- DEV-ONLY image changes to revert for final delivery: pre-provisioned
  `devops` account in `etc-passwd`/`etc-shadow`/`etc-group`, `autologin`
  file, home skeleton, and the removed `firstboot-marker` in update-flash.sh
  (originals saved as `*.devorig`).
