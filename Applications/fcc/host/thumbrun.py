#!/usr/bin/env python3
"""Host-side Thumb-1 verification harness for the fcc self-hosted compiler.

Loads a flat Thumb-1 binary (exactly what fcc emits and runbin loads) into
Unicorn, sets up an AAPCS stack, calls offset 0 as a function, and reports the
value returned in r0.  A minimal FUZIX syscall model (svc #n) lets test
programs do write()/exit() without any host libc.

This is the primary correctness gate for the code generator: every emitted
instruction can be validated here long before it is flashed to the Pico.
"""

import sys
import struct

from unicorn import (Uc, UC_ARCH_ARM, UC_MODE_THUMB,
                     UC_HOOK_CODE, UC_HOOK_INTR, UcError)
from unicorn.arm_const import (UC_ARM_REG_PC, UC_ARM_REG_SP, UC_ARM_REG_LR,
                               UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2,
                               UC_ARM_REG_R3)

from capstone import Cs, CS_ARCH_ARM, CS_MODE_THUMB, CS_MODE_MCLASS

BASE = 0x00010000        # code load address
STACK_TOP = 0x00030000   # stack grows down
MEM_SIZE = 0x00040000    # 256 KiB mapped region
STOP = 0x00038000        # sentinel address holding a BKPT; LR points here
MAX_INSNS = 5_000_000


class RunResult:
    def __init__(self, retval, insns, stdout, exited):
        self.retval = retval
        self.insns = insns
        self.stdout = stdout
        self.exited = exited


def disassemble(code, base=BASE):
    md = Cs(CS_ARCH_ARM, CS_MODE_THUMB | CS_MODE_MCLASS)
    out = []
    for insn in md.disasm(bytes(code), base | 1):
        out.append("0x%08x: %-10s %s %s" % (insn.address, insn.bytes.hex(),
                                             insn.mnemonic, insn.op_str))
    return "\n".join(out)


def run(code, trace=False, syscalls=None):
    uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB)
    uc.mem_map(0x0, MEM_SIZE)
    uc.mem_write(BASE, bytes(code))
    # Sentinel: BKPT 0.  Reaching it means the test function returned.
    uc.mem_write(STOP, b"\x00\xbe")
    uc.reg_write(UC_ARM_REG_SP, STACK_TOP)
    uc.reg_write(UC_ARM_REG_LR, STOP | 1)

    state = {"insns": 0, "stdout": bytearray(), "stopped": False,
             "code": bytes(code), "user_hooks": syscalls or {}}

    def hook_code(uc_, address, size, user):
        if address == STOP:
            uc_.emu_stop()
            return
        state["insns"] += 1
        if state["insns"] > MAX_INSNS:
            uc_.emu_stop()
            return
        if trace:
            b = uc_.mem_read(address, size)
            print("0x%08x: %s" % (address, b.hex()))

    def hook_intr(uc_, intno, user):
        # Unicorn reports only the generic SVC exception number on Thumb;
        # the FUZIX syscall number is the immediate in the 0xDFxx SVC insn.
        pc = uc_.reg_read(UC_ARM_REG_PC)
        imm = bytes(uc_.mem_read(pc - 2, 2))[0]
        handler = state["user_hooks"].get(imm)
        if handler:
            r0 = uc_.reg_read(UC_ARM_REG_R0)
            r1 = uc_.reg_read(UC_ARM_REG_R1)
            r2 = uc_.reg_read(UC_ARM_REG_R2)
            r3 = uc_.reg_read(UC_ARM_REG_R3)
            uc_.reg_write(UC_ARM_REG_R0, handler(uc_, r0, r1, r2, r3))
            return
        if imm == 8:  # write(fd, buf, len)
            r0 = uc_.reg_read(UC_ARM_REG_R0)
            r1 = uc_.reg_read(UC_ARM_REG_R1)
            r2 = uc_.reg_read(UC_ARM_REG_R2)
            if r2 and r1:
                state["stdout"] += bytes(uc_.mem_read(r1, r2))
            uc_.reg_write(UC_ARM_REG_R0, r2)
        elif imm == 0:  # _exit(status)
            state["stopped"] = True
            uc_.emu_stop()
        else:
            # Unknown syscall: return -1 like a failed FUZIX call.
            uc_.reg_write(UC_ARM_REG_R0, 0xFFFFFFFF)

    uc.hook_add(UC_HOOK_CODE, hook_code)
    uc.hook_add(UC_HOOK_INTR, hook_intr)

    try:
        uc.emu_start(BASE | 1, STOP | 1, count=MAX_INSNS)
    except UcError as e:
        raise RuntimeError("emulation error at pc=0x%x: %s" %
                           (uc.reg_read(UC_ARM_REG_PC), e))

    return RunResult(uc.reg_read(UC_ARM_REG_R0) & 0xFFFFFFFF,
                     state["insns"], bytes(state["stdout"]),
                     state["stopped"])


def main():
    if len(sys.argv) < 2:
        print("usage: thumbrun.py [-t] file.bin", file=sys.stderr)
        return 2
    trace = sys.argv[1] == "-t"
    path = sys.argv[2] if trace else sys.argv[1]
    code = open(path, "rb").read()
    if "--disasm" in sys.argv:
        print(disassemble(code))
        return 0
    res = run(code, trace=trace)
    print("retval=%d insns=%d exited=%s" % (res.retval, res.insns, res.exited))
    if res.stdout:
        print("stdout:", repr(res.stdout))
    return 0


if __name__ == "__main__":
    sys.exit(main())
