#!/usr/bin/env python3
"""Resolve raw RVAs in a pc64 crash report against SYMBOLS.TXT.

The debug build self-symbolizes (its reports already carry "name+0xoff"), so
this is only needed when a report was captured before the in-image table was
present, or when you want to re-resolve against a different build.  It rewrites
every "[rva 0x1234]" token in place with the nearest preceding symbol.

Usage:
  crashsym.py SYMBOLS.TXT CRASH/CR001.TXT [more reports...]
  crashsym.py SYMBOLS.TXT < report.txt          # filter mode
"""
import sys, re, bisect

def load(path):
    rvas, names = [], []
    with open(path) as f:
        for line in f:
            if line.startswith("#") or "\t" not in line:
                continue
            h, n = line.rstrip("\n").split("\t", 1)
            try:
                rvas.append(int(h, 16))
            except ValueError:
                continue
            names.append(n)
    order = sorted(range(len(rvas)), key=lambda i: rvas[i])
    return [rvas[i] for i in order], [names[i] for i in order]

def resolver(rvas, names):
    tok = re.compile(r"\[rva 0x([0-9a-fA-F]+)\]")
    def sub(m):
        rva = int(m.group(1), 16)
        i = bisect.bisect_right(rvas, rva) - 1
        if i < 0:
            return m.group(0)
        return "%s+0x%x [rva 0x%x]" % (names[i], rva - rvas[i], rva)
    return lambda line: tok.sub(sub, line)

def main():
    if len(sys.argv) < 2:
        print(__doc__); sys.exit(1)
    rvas, names = load(sys.argv[1])
    res = resolver(rvas, names)
    if len(sys.argv) == 2:
        for line in sys.stdin:
            sys.stdout.write(res(line))
        return
    for rep in sys.argv[2:]:
        print("==== %s ====" % rep)
        with open(rep, errors="replace") as f:
            for line in f:
                sys.stdout.write(res(line))
        print()

if __name__ == "__main__":
    main()
