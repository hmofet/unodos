#!/usr/bin/env python3
"""Rewrite a disk-app source so kernel-symbol references use ABSOLUTE
addressing instead of PC-relative.

A disk-loaded .APP is assembled at its own fixed org (its resident slot,
far from the kernel at $20000). Its own labels are reached PC-relative as
before - those displacements are tiny - but any reference to a KERNEL symbol
(a routine or variable, exported in build/kernel_api.inc) must be absolute:
`lea draw_string(pc),a0` -> `lea draw_string,a0`, `move.w np_len(pc),d0`
-> `move.w np_len,d0`. vasm rejects PC-relative to a far absolute equate
("displacement out of range"), so this pass is mandatory and mechanical.

Rule: for every kernel symbol K (the names in kernel_api.inc), replace the
token `K(pc)` with `K`. Everything else (app-local labels) is untouched, so
the app's internal control flow and data refs stay PC-relative and PIC-clean.

Usage: mkapp.py <kernel_api.inc> <in.app.asm> <out.asm>
"""
import sys, re

def main():
    api_inc, src, out = sys.argv[1], sys.argv[2], sys.argv[3]
    ksyms = []
    for line in open(api_inc):
        m = re.match(r"^(\S+)\s+equ\s", line)
        if m:
            ksyms.append(m.group(1))
    # longest-first so e.g. np_len matches before np (not an issue here, but
    # keeps the alternation unambiguous).
    ksyms.sort(key=len, reverse=True)
    kset = set(ksyms)
    alt = "|".join(re.escape(k) for k in ksyms)
    # (1) KSYM(pc) -> KSYM  (pc-relative to a far equate is out of range).
    rx_pc = re.compile(r"(?<![\w.])(" + alt + r")\(pc\)", re.IGNORECASE)
    # (2) bsr/bra KSYM -> jsr/jmp KSYM  (a 16-bit pc-relative branch can't
    #     reach the kernel at $20000 from an app slot at $4xxxx). App-LOCAL
    #     branch targets are left as bsr/bra (they stay in range).
    rx_br = re.compile(r"^(\s*)(bsr|bra)(\.[wsbl])?(\s+)([A-Za-z_][\w]*)",
                       re.IGNORECASE)

    def fix_branch(m):
        head, op, sz, gap, tgt = m.groups()
        if tgt in kset:
            newop = "jsr" if op.lower() == "bsr" else "jmp"
            return "%s%s%s%s" % (head, newop, gap, tgt)
        return m.group(0)        # local target: leave the branch as-is

    n_pc = n_br = 0
    with open(out, "w") as f:
        for line in open(src):
            line, c = rx_pc.subn(r"\1", line)
            n_pc += c
            line2 = rx_br.sub(fix_branch, line)
            if line2 != line:
                n_br += 1
            f.write(line2)
    print("mkapp: %s -> %s (%d kernel refs de-PC'd, %d branches -> jsr/jmp)"
          % (src, out, n_pc, n_br))


if __name__ == "__main__":
    main()
