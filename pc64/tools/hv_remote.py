#!/usr/bin/env python3
"""Run the unovirt A1 selftest on a remote box where KVM is at L0.

    UNO_DEBUG=1 UNO_DETACH=1 UNO_DBGCON=1 ./build.sh
    python3 tools/hv_remote.py devbuntu.local

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

It needs the image to carry `vm-selftest` in the FIRST 512 bytes of
DEBUG.CFG - the config reader truncates there and says nothing (reported in
pc64/UNOAUTOMATE-REQUESTS.md), so this script checks rather than trusting it.
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
grep 'hv\]' /tmp/hv.log || echo '(no hv trace: is vm-selftest set, and near the TOP of DEBUG.CFG?)'
echo '--- verdict ---'
strings -a {img} | grep -e '^HV:' -e 'selftest:' | tail -2
echo '--- the boot carried on? ---'
tail -3 /tmp/hv.log
"""


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
    ok = "round trip" in r.stdout and "OK" in r.stdout
    print("hv remote: %s" % ("PASS" if ok else "no round trip - read the trace above"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
