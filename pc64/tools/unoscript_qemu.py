#!/usr/bin/env python3
"""unoscript surface QEMU gate: prove `u.proc.*` and `u.fs.*` are WIRED + GATED.

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
    finally:
        try:
            link.command("poweroff", timeout=2)
        except Exception:  # noqa: BLE001
            pass
        time.sleep(1)
        vm.kill()
        link.close()
    print(">> unoscript surface gate OK (proc + fs)" if not fails
          else ">> FAILED: " + "; ".join(fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
