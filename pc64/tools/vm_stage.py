#!/usr/bin/env python3
"""Stage the appliance payload into build/esp, and arm DEBUG.CFG for it.

    UNO_DEBUG=1 UNO_DETACH=1 UNO_DBGCON=1 ./build.sh
    python3 tools/vm_stage.py
    python3 tools/hv_remote.py 192.168.2.100

WHY THIS EXISTS.  The five steps it replaces were folklore, done by hand, and
every one of them is silent when it goes wrong:

  - `EFI\\UNODOS\\VM\\` is not created by build.sh and never was.  A missing
    directory means the loader finds no kernel, which reports as "no bzImage"
    - accurate, and a long way from "you forgot to make a folder".
  - build.sh REGENERATES DEBUG.CFG, so the keys have to be re-prepended after
    every build.  Forget, and the selftest silently does not run.
  - `noshutdown` is not optional.  Without it the stress driver finishes its
    passes and powers the machine off about nineteen seconds in, and a guest
    that gets 4 ms per frame is still decompressing its kernel when the lights
    go out.  It presents as a hang and has been chased as one twice.
  - the payload files are build ARTEFACTS, deliberately not in the tree (the
    kernel and rootfs are GPL; UNOVIRT-PLAN §6 keeps them out of it), so they
    come from build/ or from wherever you say.

Every one of those is reported here rather than left to be discovered from a
guest that does not boot.
"""
import os, shutil, subprocess, sys

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(HERE)

ESP = "build/esp"
VMDIR = os.path.join(ESP, "EFI", "UNODOS", "VM")
KEYS = ["vm-selftest", "noshutdown"]

# (destination name, candidate sources, required?)
PAYLOAD = [
    ("BZIMAGE",    ["build/bzImage"],                  True),
    ("INITRD",     ["build/initrd.gz"],                True),
    ("ROOTFS.IMG", ["build/rootfs.img", "/tmp/rootfs.img"], False),
]


def main():
    if not os.path.isdir(ESP):
        sys.exit("no %s - run ./build.sh first" % ESP)
    os.makedirs(VMDIR, exist_ok=True)

    staged, missing = [], []
    for dest, sources, required in PAYLOAD:
        src = next((s for s in sources if os.path.exists(s)), None)
        if not src:
            (missing if required else staged).append(dest)
            if not required:
                staged.pop()
                print("  %-10s -  absent (%s)" % (dest, " or ".join(sources)))
            continue
        shutil.copyfile(src, os.path.join(VMDIR, dest))
        print("  %-10s <- %s (%d KB)" % (dest, src, os.path.getsize(src) // 1024))
        staged.append(dest)
    if missing:
        sys.exit("missing required payload: %s" % ", ".join(missing))

    # DEBUG.CFG: the keys go at the top.  The parse window was 511 bytes until
    # 2026-08-02 and is the whole file now, but keys-first is still the habit.
    cfg = os.path.join(ESP, "DEBUG.CFG")
    body = open(cfg, "rb").read() if os.path.exists(cfg) else b""
    have = body.split(b"\n")
    add = [k for k in KEYS if k.encode() not in have]
    if add:
        open(cfg, "wb").write(("\n".join(add) + "\n").encode() + body)
    print("  DEBUG.CFG  -> keys %s%s" % (
        ", ".join(KEYS), "" if add else " (already present)"))

    r = subprocess.run([sys.executable, "tools/mkuefi.py"],
                       capture_output=True, text=True)
    print((r.stdout or r.stderr).strip().splitlines()[-1] if (r.stdout or r.stderr)
          else "mkuefi: done")
    return r.returncode


if __name__ == "__main__":
    sys.exit(main())
