#!/usr/bin/env python3
"""unovirt A0 gate: the capability probe, across every CPU QEMU will pretend to be.

    UNO_DEBUG=1 ./build.sh && python3 tools/mkuefi.py && python3 tools/hv_test.py
    python3 tools/hv_test.py --kvm        # one boot on the REAL extension

What this can and cannot prove is worth being exact about, because the whole
subsystem is about not overclaiming.

CAN: that the probe runs on every machine shape without faulting, that it reads
the vendor and the capability MSRs consistently with what the model advertises
in CPUID, that it never says "eligible: yes" on a machine that could not
actually host a guest, and that it names a blocker a person could act on.

CANNOT: that VMXON works, that EPT tables translate, or that a guest runs.
QEMU under TCG emulates the CPUID bits and the read-only capability MSRs, not
the operation - `-cpu ...,+vmx` gives us a machine that SAYS it has VMX, which
is exactly the machine this phase needs to be tested against and exactly the
machine phase A1 must not be tested against. A1 gates on `--kvm` (below) and
on metal, per docs/UNOVIRT-PLAN.md R2.

The models below are chosen to hit each arm of the gate:

  qemu64        AMD-flavoured, SVM present, no NPT      -> NO-SLAT
  EPYC          SVM + NPT in CPUID.8000000A             -> looks hostable
  Nehalem,+vmx  VMX present, no EPT (secondary ctls)    -> NO-SLAT
  max           whatever this QEMU thinks is maximal
  Nehalem       neither extension advertised            -> NO-CPU

A model that reports itself hostable is NOT a failure: the machine really does
advertise those bits, and "still attached to the firmware" is the honest
remaining blocker under this harness (nothing here detaches). What WOULD be a
failure is eligible=yes, a missing HV line, or any crash report at all.

TCG SILENTLY DROPS `+vmx`, with a warning on stderr nobody reads
("TCG doesn't support requested feature: CPUID.01H:ECX.vmx"), so the Intel arm
of the gate CANNOT be exercised this way and the Nehalem,+vmx row lands on the
no-extension arm instead. That is not a harness defect to fix, it is the
reason `--kvm` exists.

`--kvm` boots ONE machine with `-enable-kvm -cpu host`, where the capability
MSRs are the host's own. On amanuensis (Ryzen 5 5600G under WSL2 with nested
virtualization on) that exercises the SVM arm against real silicon values, and
with a UNO_DETACH=1 build it is the only configuration short of metal where
the gate can legitimately answer `eligible: yes`. On an Intel box the same
flag exercises the VMX arm.
"""
import os, re, shutil, subprocess, sys, time

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(HERE)
sys.path.insert(0, HERE)
from harness import Qmp, QMP_SOCK, OVMF_CODE, OVMF_VARS

IMG = "build/unodos-uefi.img"
MODELS = ["qemu64", "EPYC", "Nehalem,+vmx", "max", "Nehalem"]


def mtool(argv):
    """mtools against the ESP inside the GPT image (1 MiB in, per mkuefi.py)."""
    r = subprocess.run(argv[:1] + ["-i", IMG + "@@1048576"] + argv[1:],
                       capture_output=True, text=True)
    return r.stdout


def boot(model, log, kvm=False):
    """One headless boot of `model`, long enough for the env block to land."""
    shutil.copy(OVMF_VARS, "build/vars.fd")
    if os.path.exists(QMP_SOCK):
        os.remove(QMP_SOCK)
    argv = [
        "qemu-system-x86_64", "-machine", "q35", "-m", "4096", "-cpu", model,
    ] + (["-enable-kvm"] if kvm else []) + [
        "-drive", "if=pflash,format=raw,readonly=on,file=" + OVMF_CODE,
        "-drive", "if=pflash,format=raw,file=build/vars.fd",
        "-drive", "format=raw,file=" + IMG,
        "-device", "qemu-xhci", "-device", "usb-tablet",
        "-nic", "none", "-display", "none",
        "-qmp", "unix:%s,server,nowait" % QMP_SOCK,
        "-debugcon", "file:" + log, "-global", "isa-debugcon.iobase=0x402",
    ]
    qemu = subprocess.Popen(argv, stdout=subprocess.DEVNULL,
                            stderr=subprocess.PIPE)
    try:
        q = Qmp(QMP_SOCK)
        time.sleep(22)                     # boot + pre-detach telemetry write
        q.cmd("quit")
    except Exception as e:                 # a model this QEMU refuses to build
        qemu.kill()
        err = (qemu.stderr.read() or b"").decode(errors="replace").strip()
        return None, err.splitlines()[-1] if err else str(e)
    finally:
        try:
            qemu.wait(timeout=15)
        except subprocess.TimeoutExpired:
            qemu.kill()
    return mtool(["mtype", "::/BOOTENV.TXT"]), None


def main():
    if not os.path.exists(IMG):
        sys.exit("no %s - run tools/mkuefi.py after a UNO_DEBUG=1 build" % IMG)
    kvm = "--kvm" in sys.argv
    models = ["host"] if kvm else MODELS
    rows, fails = [], []
    for model in models:
        env, why = boot(model, "build/hv_%s.log" % re.sub(r"\W", "_", model),
                        kvm=kvm)
        if env is None:
            rows.append((model, "SKIP", why or "qemu refused this model"))
            continue

        m = re.search(r"^HV: (.*)$", env, re.M)
        if not m:
            fails.append("%s: no HV line in BOOTENV.TXT (the probe did not "
                         "run, or the boot never reached the env block)" % model)
            rows.append((model, "FAIL", "no HV line"))
            continue
        caps = m.group(1).strip()
        e = re.search(r"eligible: (yes|no)(?: - (.*))?", env)
        if not e:
            fails.append("%s: HV line carries no verdict: %s" % (model, caps))
            rows.append((model, "FAIL", caps))
            continue
        ok, reason = e.group(1), (e.group(2) or "").strip()

        # The matrix expects the DEFAULT debug build, where detach is disabled
        # (build.sh F8), so "still attached" is a standing blocker and the
        # answer must be no whatever the model claims. A yes here means either
        # the wrong build or a gate that dropped the attached check.
        #
        # Note what is NOT asserted: that a model advertising SVM+NPT under TCG
        # is refused. It is not refused, and it should not be - the gate reads
        # what the machine says about itself, and no architectural read
        # distinguishes "advertises the extension" from "the extension works".
        # Discovering that is phase A1's job, by trying it.
        if ok == "yes" and not kvm:
            fails.append("%s: eligible=yes - either this is a UNO_DETACH=1 "
                         "build (use the default) or the attached blocker is "
                         "gone" % model)
        if ok == "no" and not reason:
            fails.append("%s: refused without naming a reason" % model)

        # A crash report from a probe that reads MSRs is the failure this
        # phase exists to rule out: it means a capability MSR was read on a
        # machine that does not implement it.
        crash = mtool(["mdir", "-/", "::/CRASH"])
        if re.search(r"^CR\d+\s+TXT", crash, re.M):
            fails.append("%s: a crash report was written during boot" % model)

        rows.append((model, ok.upper(), (caps.split("\n")[0])[:78]))
        if reason:
            rows.append(("", "", "  -> " + reason))

    print("\n%-14s %-5s %s" % ("model", "elig", "what the gate read"))
    for a, b, c in rows:
        print("%-14s %-5s %s" % (a, b, c))
    print()
    for f in fails:
        print("FAIL " + f)
    print("hv gate: %d models, %d failures" % (len(models), len(fails)))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
