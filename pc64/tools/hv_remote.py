#!/usr/bin/env python3
"""Run the unovirt A1 selftest on a remote box where KVM is at L0.

    UNO_DEBUG=1 UNO_DETACH=1 UNO_DBGCON=1 ./build.sh
    python3 tools/hv_remote.py devbuntu.local
    python3 tools/hv_remote.py devbuntu.local --shot   # + a screenshot mid-slice

WHY THIS EXISTS. Executing VMLAUNCH or VMRUN needs the virtualization
extension to actually work, not merely to be advertised, and the development
box cannot provide that: amanuensis is Hyper-V hosting WSL2 hosting KVM, so
UnoDOS is already two levels down and its own guest would be a third. The
first VMRUN there never returns (pc64/UNOVIRT.md, "The A1 wedge").

devbuntu is bare metal with nested KVM, so UnoDOS runs one level down and its
guest is the second - the configuration nested virtualization is actually
tested in. That is the difference between "the selftest hangs" and the run
this script produced on 2026-08-06:

    [hv] vmxon ok, rev=0000000011e57ed0
    [hv] vmentry rip=0000000142d31000   exit reason=a   (CPUID)
    [hv] vmentry rip=0000000142d31002   exit reason=c   (HLT)
    [hv] vmentry rip=0000000142d31000   exit reason=2   (triple fault, the crasher)
    selftest: vmx: entered, guest round trip -> 534f444f4e55 OK, crasher contained

It needs the image to carry `vm-selftest` in DEBUG.CFG, so this script checks
rather than trusting it.  (Until 2026-08-02 the parse window was 511 bytes
while the shipped header alone was longer, so a key appended to the end was
silently ignored - the harness lane reads the whole file now and says so when
it does not fit.  Putting keys at the top is still the habit worth keeping.)

IT ALSO NEEDS `noshutdown`, AND THAT ONE COST A WHOLE INVESTIGATION.  The
shipped DEBUG.CFG carries `passes=3`, which means the stress driver finishes
its passes and POWERS THE MACHINE OFF - about nineteen seconds in.  A guest
runs from the shell's frame loop at 4 ms a frame, so nineteen seconds of wall
time is roughly a tenth of a second of guest CPU: the kernel is still
decompressing itself when the lights go out.  The symptom is a single line of
guest output and a kernel that looks wedged, which is indistinguishable from
a real hang and was chased as one.  `noshutdown` leaves the desktop up and
lets the QEMU timeout bound the run instead.  The tell, in hindsight: the
guest's exits were nearly all preemption-timer exits, which means busy rather
than stuck.

    UNO_DEBUG=1 UNO_DETACH=1 UNO_DBGCON=1 ./build.sh
    cp build/bzImage  build/esp/EFI/UNODOS/VM/BZIMAGE
    cp build/initrd.gz build/esp/EFI/UNODOS/VM/INITRD
    printf 'vm-selftest\\nnoshutdown\\n' | cat - build/esp/DEBUG.CFG > /tmp/d.cfg
    cp /tmp/d.cfg build/esp/DEBUG.CFG && python3 tools/mkuefi.py
"""
import os, subprocess, sys

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(HERE)
IMG = "build/unodos-uefi.img"
REMOTE_IMG = "/tmp/unodos-uefi.img"

RUN = r"""cd /tmp && cp /usr/share/OVMF/OVMF_VARS_4M.fd hv_vars.fd &&
timeout {t} qemu-system-x86_64 -machine q35 -m 4096 -cpu host -enable-kvm \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
  -drive if=pflash,format=raw,file=hv_vars.fd \
  -drive format=raw,file={img} \
  -device qemu-xhci -device usb-tablet -nic none -display none \
  -debugcon file:/tmp/hv.log -global isa-debugcon.iobase=0x402 >/dev/null 2>&1
echo '--- trace ---'
grep 'hv\]' /tmp/hv.log | head -40 || echo '(no hv trace: is vm-selftest set, and near the TOP of DEBUG.CFG?)'
echo '--- verdict ---'
strings -a {img} | grep -e '^HV:' -e 'selftest:' | tail -2
echo '--- the guest said ---'
grep 'lin\]' /tmp/hv.log | grep -v '  [a-z]*=' | tail -12
echo "guest lines: $(grep -c 'lin\]' /tmp/hv.log)"
echo '--- the boot carried on? ---'
tail -3 /tmp/hv.log
"""


def shot(host):
    """Boot again and screendump while the A3 slice test is running.

    The log can say a guest was sliced 120 times; only a picture can say the
    desktop was still being painted between the slices."""
    subprocess.run(["scp", "-q", "tools/hv_shot_remote.py", host + ":/tmp/"],
                   check=True)
    r = subprocess.run(["ssh", "-o", "BatchMode=yes", host,
                        "python3 /tmp/hv_shot_remote.py 26"],
                       capture_output=True, text=True)
    print(r.stdout.strip())
    if subprocess.run(["scp", "-q", host + ":/tmp/hv_shot.ppm",
                       "shots/hv_slice.ppm"]).returncode:
        return
    sys.path.insert(0, "tools")
    import ppm2png
    w, h, px = ppm2png.read_ppm("shots/hv_slice.ppm")
    ppm2png.write_png("shots/hv_slice.png", w, h, px)
    os.remove("shots/hv_slice.ppm")
    print("shots/hv_slice.png (%dx%d)" % (w, h))


def main():
    host = sys.argv[1] if len(sys.argv) > 1 else "devbuntu.local"
    if not os.path.exists(IMG):
        sys.exit("no %s - build with UNO_DEBUG=1 UNO_DETACH=1 UNO_DBGCON=1, "
                 "then tools/mkuefi.py" % IMG)
    with open(IMG, "rb") as f:
        head = f.read(4 * 1024 * 1024)
    if b"vm-selftest" not in head:
        print("warning: no `vm-selftest` found near the start of the image; "
              "the selftest will stay opt-out")

    print("copying %s to %s ..." % (IMG, host))
    if subprocess.run(["scp", "-q", IMG, "%s:%s" % (host, REMOTE_IMG)]).returncode:
        sys.exit("scp failed")
    r = subprocess.run(["ssh", "-o", "BatchMode=yes", host,
                        RUN.format(t=90, img=REMOTE_IMG)],
                       capture_output=True, text=True)
    print(r.stdout)
    if r.stderr.strip():
        print("stderr:", r.stderr.strip()[:400])
    if "--shot" in sys.argv:
        shot(host)
    ok = "round trip" in r.stdout and "OK" in r.stdout
    print("hv remote: %s" % ("PASS" if ok else "no round trip - read the trace above"))
    # A6: the guest shell answering is a separate claim from the foothold, and
    # it is reported separately rather than folded into PASS - a machine with
    # no bzImage on it still passes everything this script is really for.
    if "GUEST" in r.stdout or "linux:" in r.stdout:
        shell = "UNODOS-GUEST-SHELL-OK" in r.stdout or "shell ANSWERED" in r.stdout
        print("guest shell: %s" % ("ANSWERED" if shell else "no reply seen"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
