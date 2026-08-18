#!/usr/bin/env python3
"""duum_viper_probe.py - does this PYRT actually run @micropython.native and
@micropython.viper code?

This is the open question left by the 2026-07-20 Duum review. mpconfigport.h
turns the x64 native emitter ON specifically so Duum's hot loop can be
compiled, and DUUM.PY has never used it. Two things could stop it:

  1. ABI. emitnx64/asmx64 generate SysV AMD64 calls (args in rdi/rsi/rdx),
     but PYRT is built with x86_64-w64-mingw32-gcc, which is MS x64
     (rcx/rdx/r8/r9). Generated code calls C helpers through mp_fun_table; if
     those are emitted SysV against MS-ABI helpers, arguments land in the
     wrong registers.
  2. W^X. Native code is allocated from the GC heap (kernel BSS) and has to
     be mapped executable.

Either failure is worth knowing BEFORE investing in a viper rewrite, and the
fallback (move the loop into a C helper in mod_uno.c) is unaffected by both.

The URC `py` verb execs ONE LINE, so each probe is a single exec() with the
newlines inside the string.

  python3 duum_viper_probe.py
"""
import os, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import duum_demo as DD                                       # noqa: E402

PROBES = [
    ("interpreted baseline",
     "def f(x):\\n return x*2+1\\nprint('OK', f(20))"),
    ("@micropython.native",
     "@micropython.native\\ndef g(x):\\n return x*2+1\\nprint('OK', g(20))"),
    ("@micropython.viper int",
     "@micropython.viper\\ndef h(x:int)->int:\\n return x*2+1\\nprint('OK', h(20))"),
    ("viper ptr8 write",
     "@micropython.viper\\ndef k(b:ptr8, n:int):\\n for i in range(n):\\n  b[i]=i&255\\n"
     "\\nba=bytearray(8)\\nk(ba,8)\\nprint('OK', list(ba))"),
    ("viper hot-loop shape",
     "@micropython.viper\\ndef col(dst:ptr8, w:int, h:int, sh:int):\\n"
     " i=0\\n s=0\\n while i<w:\\n  s+=(dst[i]*sh)>>8\\n  i+=1\\n return s\\n"
     "\\nba=bytearray(64)\\nprint('OK', col(ba,64,1,128))"),
]


def main():
    d = DD.Duum().boot()
    try:
        print("\n=== emitter probes ===")
        for (name, src) in PROBES:
            one = "exec(\"%s\")" % src
            try:
                r = d.link.eval(one, timeout=25)
                print("  %-24s -> %s" % (name, r))
            except Exception as e:
                print("  %-24s -> ERROR %r" % (name, e))
        # A rough speed ratio, if viper runs at all: same loop both ways.
        print("\n=== speed, interpreted vs viper (same loop) ===")
        bench = (
            "import time\\n"
            "def pi(n):\\n s=0\\n i=0\\n while i<n:\\n  s+=(i*3)>>2\\n  i+=1\\n return s\\n"
            "@micropython.viper\\n"
            "def pv(n:int)->int:\\n s=0\\n i=0\\n while i<n:\\n  s+=(i*3)>>2\\n  i+=1\\n return s\\n"
            "t=time.ticks_ms()\\npi(200000)\\na=time.ticks_diff(time.ticks_ms(),t)\\n"
            "t=time.ticks_ms()\\npv(200000)\\nb=time.ticks_diff(time.ticks_ms(),t)\\n"
            "print('interp_ms',a,'viper_ms',b)"
        )
        try:
            print("  ", d.link.eval("exec(\"%s\")" % bench, timeout=60))
        except Exception as e:
            print("   ERROR %r" % (e,))
    finally:
        d.stop()


if __name__ == "__main__":
    main()
