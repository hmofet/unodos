#!/usr/bin/env python3
"""unoscript surface QEMU gate: prove u.proc.* / u.fs.* / u.mem|io|sys.* are WIRED + GATED.

Roadmap steps 3-4 (UNOSCRIPT-NEXT-STEPS.md §3-4) wire the proc surface
(`usc_proc_list`/`usc_proc_inspect` -> the shell's running-app run-set) and the
user-scoped fs surface (`usc_fs_read`/`usc_fs_write` -> per-uid home + /vol paths).
This gate drives the real Python surface over URC and asserts the observable
transition the wiring produces (proc; fs is the same shape, section 2b):

  * `u.cap_tier('proc.enum') == 2`            - the binding resolves, cap is ADMIN
  * `u.proc.list()` UNESCALATED raises with   - the delegation is LIVE and the guard
    "capability denied" (not "surface not       fires: before this change the same call
    wired")                                      raised NotImplementedError (UNWIRED).
  * `u.app.count()` still returns an int      - step-2 app surface not regressed
  * launch an app -> it appears in a fresh    - the enumeration SOURCE (the shell's
    PROBE window list                            open-app state proc.list reads) is live

The authenticated "returns the right rows once escalated" assertion needs a logged-in
session and belongs to the roadmap's deferred end-to-end unosecure gate; the C-level
escalation flip for PROC_ENUM is already proven by unosec_selftest (-DUNO_SECTEST).

Consumes unoautomate's link + disk builder (tools/unoauto_remote.py, tools/remote_qemu.py)
as neutral APIs - neither file is edited. Needs a debug build (PROBE is UNO_DEBUG):
    UNO_DEBUG=1 ./build.sh
Run under WSL (qemu-system-x86_64 + OVMF). Exit 0 iff green.
"""
import os, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import remote_qemu as rq                      # noqa: E402  (disk builder + paths)
from unoauto_remote import UnoAutoLink        # noqa: E402

fails = []


def check(ok, what, detail=""):
    print(("PASS " if ok else "FAIL ") + what + ("  " + detail if detail else ""))
    if not ok:
        fails.append(what)


def boot():
    subprocess.run(["cp", rq.OVMF_VARS, rq.VARS])
    cmd = [
        "qemu-system-x86_64", "-machine", "q35", "-m", "512", "-cpu", "max",
        "-drive", "if=pflash,format=raw,readonly=on,file=" + rq.OVMF_CODE,
        "-drive", "if=pflash,format=raw,file=" + rq.VARS,
        "-drive", "format=raw,file=" + rq.DISK,
        "-netdev", "user,id=n0", "-device", "e1000,netdev=n0",
        "-display", "none",
    ]
    return subprocess.Popen(cmd, stderr=subprocess.DEVNULL)


def one_line(link, expr, timeout=20):
    """Run `print(expr)` on the guest, return the single printed line (or None)."""
    out = link.eval("import unoscript as u; print(%s)" % expr, timeout=timeout)
    return out[0].strip() if out else None


def main():
    if not os.path.isdir(rq.ESP):
        print("FAIL: no debug build at build/esp (run UNO_DEBUG=1 ./build.sh)")
        return 1
    rq.build_disk()
    link = UnoAutoLink("127.0.0.1", rq.PORT)
    link.listen()
    vm = boot()
    try:
        if not link.wait_connected(180):
            print("FAIL: pc64 never dialed in")
            return 1

        # 1) the surface resolves and proc.enum is an ADMIN (tier-2) capability
        try:
            check(one_line(link, "u.secured()") == "True",
                  "unosecure present (u.secured())")
            check(one_line(link, "u.cap_tier('proc.enum')") == "2",
                  "proc.enum is tier 2 (ADMIN)")
        except Exception as e:  # noqa: BLE001
            check(False, "proc surface resolves", str(e))

        # 2) proc.list() is WIRED + GATED: no session => guard denies. The error
        #    text is unoscript's own ("capability denied"), NOT the "surface not
        #    wired" NotImplementedError the stub raised before this change.
        try:
            out = link.eval("import unoscript as u; print(u.proc.list())")
            check(False, "proc.list denied unescalated",
                  "returned %r (a no-session script must NOT enumerate)" % out)
        except RuntimeError as e:
            txt = str(e).lower()
            check("denied" in txt or "eperm" in txt,
                  "proc.list is wired + gated (denied without a session)",
                  str(e).strip().replace("\n", " ")[:120])
            check("not wired" not in txt and "notimplement" not in txt,
                  "proc.list is no longer UNWIRED (delegation landed)",
                  str(e).strip().replace("\n", " ")[:120])
        except Exception as e:  # noqa: BLE001
            check(False, "proc.list raised the expected guest error", repr(e))

        # 2b) fs surface (step 4): wired + gated the same way. The FS_USER floor
        #     (tier 1) denies the no-session URC context before the path even
        #     resolves, so both a relative (home) and an absolute (/vol) path
        #     raise "capability denied" - not the "surface not wired" stub.
        try:
            check(one_line(link, "u.cap_tier('fs.user')") == "1", "fs.user is tier 1")
            check(one_line(link, "u.cap_tier('fs.sys')") == "2", "fs.sys is tier 2 (ADMIN)")
        except Exception as e:  # noqa: BLE001
            check(False, "fs caps resolve", str(e))
        for expr, label in (("u.fs.read('todo.txt')", "fs.read (home)"),
                            ("u.fs.write('todo.txt', b'x')", "fs.write (home)"),
                            ("u.fs.read('/usb/x')", "fs.read (/vol)")):
            try:
                out = link.eval("import unoscript as u; print(%s)" % expr)
                check(False, "%s denied unescalated" % label,
                      "returned %r (expected denial)" % out)
            except RuntimeError as e:
                txt = str(e).lower()
                check("denied" in txt or "eperm" in txt,
                      "%s is wired + gated" % label, str(e).strip().replace("\n", " ")[:90])
                check("not wired" not in txt and "notimplement" not in txt,
                      "%s is no longer UNWIRED" % label, "")
            except Exception as e:  # noqa: BLE001
                check(False, "%s raised the expected guest error" % label, repr(e))

        # 2c) kernel surface (step 5): mem/io/power. All tier 2/3, so the
        #     no-session URC context is denied at the guard BEFORE the action.
        #     Every probe is chosen to be inert even if the gate somehow failed:
        #     reads have no side effect, io.out targets the POST port 0x80,
        #     mem.write uses addr 0 (rejected by validation), power(2)=suspend is
        #     a no-op - so this gate can never poke live memory or reboot the VM.
        try:
            check(one_line(link, "u.cap_tier('mem.read')") == "3", "mem.read is tier 3 (KERNEL)")
            check(one_line(link, "u.cap_tier('mem.write')") == "3", "mem.write is tier 3 (KERNEL)")
            check(one_line(link, "u.cap_tier('io.read')") == "2", "io.read is tier 2 (ADMIN)")
            check(one_line(link, "u.cap_tier('io.write')") == "3", "io.write is tier 3 (KERNEL)")
            check(one_line(link, "u.cap_tier('power')") == "2", "power is tier 2 (ADMIN)")
        except Exception as e:  # noqa: BLE001
            check(False, "kernel caps resolve", str(e))
        for expr, label in (("u.mem.read(0, 0x100000, 4)", "mem.read"),
                            ("u.mem.write(0, 0, b'x')", "mem.write"),
                            ("u.io.in_(0x80, 1)", "io.in"),
                            ("u.io.out(0x80, 1, 0)", "io.out"),
                            ("u.sys.power(2)", "sys.power")):
            try:
                out = link.eval("import unoscript as u; print(%s)" % expr)
                check(False, "%s denied unescalated" % label,
                      "returned %r (expected denial)" % out)
            except RuntimeError as e:
                txt = str(e).lower()
                check("denied" in txt or "eperm" in txt,
                      "%s is wired + gated" % label, str(e).strip().replace("\n", " ")[:90])
                check("not wired" not in txt and "notimplement" not in txt,
                      "%s is no longer UNWIRED" % label, "")
            except Exception as e:  # noqa: BLE001
                check(False, "%s raised the expected guest error" % label, repr(e))

        # 2d) hook surface (step 6): tier 2, debug-only tap registry. Over URC
        #     with no session it is denied at the guard (wired + gated); the
        #     production build reports EUNAVAIL by design (a deliberate non-goal).
        try:
            check(one_line(link, "u.cap_tier('hook')") == "2", "hook is tier 2 (ADMIN)")
        except Exception as e:  # noqa: BLE001
            check(False, "hook cap resolves", str(e))
        try:
            out = link.eval("import unoscript as u; print(u.hook.add('fs.write'))")
            check(False, "hook.add denied unescalated", "returned %r (expected denial)" % out)
        except RuntimeError as e:
            txt = str(e).lower()
            check("denied" in txt or "eperm" in txt, "hook.add is wired + gated",
                  str(e).strip().replace("\n", " ")[:90])
            check("not wired" not in txt and "notimplement" not in txt,
                  "hook.add is no longer UNWIRED", "")
        except Exception as e:  # noqa: BLE001
            check(False, "hook.add raised the expected guest error", repr(e))

        # 3) regression: the step-2 app surface (tier 0) still answers
        try:
            n = one_line(link, "u.app.count()")
            check(n is not None and int(n) >= 1, "app.count() still works (step 2)", "count=%s" % n)
        except Exception as e:  # noqa: BLE001
            check(False, "app.count() still works", str(e))

        # 4) the enumeration SOURCE is live: launch an app, see its window in a
        #    fresh PROBE (proc.list reads the same open-app state).
        try:
            link.launch(0, timeout=10)
            time.sleep(0.5)
            wins = [r for r in link.probe(timeout=10) if r["kind"] == 1]
            check(len(wins) > 0, "launch -> window visible (proc.list's source is live)",
                  "%d window row(s)" % len(wins))
        except Exception as e:  # noqa: BLE001
            check(False, "launch -> window visible", str(e))

        # 5) THE POSITIVE PATH (end-to-end authenticated). Everything above proves
        #    the surfaces DENY without a session. u.e2e() logs in a throwaway
        #    unosecure session in C and drives the surfaces WITH authority:
        #    fs denies as a guest -> grants on request -> write+read ROUND-TRIP;
        #    proc/io return real data under a dev autogrant policy; mem stays
        #    denied (KERNEL > autogrant's ADMIN ceiling). It returns 0 on a full
        #    pass, <0 on skip (non-fresh store), else a failure bitmask.
        try:
            out = one_line(link, "u.e2e()", timeout=40)
            check(out == "0",
                  "e2e authenticated pass (fs round-trip + proc/io real data + mem still denied)",
                  "u.e2e() -> %s (0=pass, <0=skip, else fail-bitmask)" % out)
        except Exception as e:  # noqa: BLE001
            check(False, "e2e authenticated self-test ran", str(e))
    finally:
        try:
            link.command("poweroff", timeout=2)
        except Exception:  # noqa: BLE001
            pass
        time.sleep(1)
        vm.kill()
        link.close()
    print(">> unoscript surface gate OK (proc + fs + kernel + hook + e2e authenticated)" if not fails
          else ">> FAILED: " + "; ".join(fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
