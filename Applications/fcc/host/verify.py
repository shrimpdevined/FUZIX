#!/usr/bin/env python3
"""Verify all backend test binaries against expected results with Unicorn."""
import sys, os
sys.path.insert(0, os.path.dirname(__file__))
from thumbrun import run

EXPECT = {
    "ret42":      (42, False),
    "arith":      (19, False),
    "sub":        (7,  False),
    "local":      (6,  False),
    "cmp_false":  (0,  False),
    "cmp_true":   (1,  False),
    "cmp_eq":     (1,  False),
    "literal":    (1000, False),
    "call":       (9,  False),
    "branch":     (2,  False),
    "exit7":      (7,  True),
    "shift":      (16, False),
}

def main():
    d = os.path.join(os.path.dirname(__file__), "tests")
    ok = 0
    for name, (want_ret, want_exit) in EXPECT.items():
        path = os.path.join(d, name + ".bin")
        code = open(path, "rb").read()
        try:
            res = run(code)
        except Exception as ex:
            print("FAIL %-12s emulation error: %s" % (name, ex))
            continue
        good = (res.retval == want_ret and
                (res.exited == want_exit or not want_exit))
        if good:
            ok += 1
            print("PASS %-12s retval=%d insns=%d" % (name, res.retval, res.insns))
        else:
            print("FAIL %-12s got retval=%d exited=%s, want retval=%d exited=%s" %
                  (name, res.retval, res.exited, want_ret, want_exit))
    print("\n%d/%d passed" % (ok, len(EXPECT)))
    return 0 if ok == len(EXPECT) else 1

if __name__ == "__main__":
    sys.exit(main())
