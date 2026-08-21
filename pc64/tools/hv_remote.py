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

THE "OCCASIONAL EMPTY CAPTURE" HAD A CAUSE, and it was not flakiness: a run
that is killed by its timeout can leave qemu-system-x86_64 ALIVE, and the next
run's QEMU then shares `-debugcon file:/tmp/hv.log` with it.  Two processes
writing one file from their own offsets is a log that looks empty, truncated
or interleaved at random.  Five of them had accumulated before this was
noticed.  The run now kills stragglers before it starts, and it matches on the
process NAME rather than with `pkill -f`: this whole script arrives as the
remote shell's own command line and mentions qemu-system-x86_64 further down,
so a full-command-line match kills the run itself before it starts - silently,
with no output to explain where it went.  (The usual `[q]emu` bracket trick
does not save you either, for the same reason.)

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

RUN = r"""# WAIT for the stragglers to go, do not guess.  A fixed `sleep 1` after the
# kill left runs failing in alternation: QEMU holds a write lock on the image
# and the debugcon log, a killed one takes a moment to release them, and the
# next run then either starts cleanly or starts against a locked image
# depending on timing.  Polling for the condition is both faster and certain.
#
# THE KILL IS SCOPED TO OUR OWN IMAGE, not to every QEMU on the box.  The
# harness can now run on a machine that HOSTS things in QEMU (leviathan runs
# the whole fleet, including the development VM this repo lives on), and a
# bare `pkill qemu-system` there powers off machines that were never ours.
# Matching the full command line is safe here and was not in the old inline
# form: this script arrives as a FILE, so no shell's own command line carries
# the pattern.
pkill -f 'qemu-system-x86_64.*unodos-uefi.img' 2>/dev/null
for i in $(seq 1 40); do
  pgrep -f 'qemu-system-x86_64.*unodos-uefi.img' >/dev/null 2>&1 || break
  sleep 0.25
done
pkill -9 -f 'qemu-system-x86_64.*unodos-uefi.img' 2>/dev/null; sleep 1
cd /tmp && cp /usr/share/OVMF/OVMF_VARS_4M.fd hv_vars.fd &&
timeout {t} qemu-system-x86_64 -machine q35 -m 4096 -cpu host -enable-kvm \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
  -drive if=pflash,format=raw,file=hv_vars.fd \
  -drive format=raw,file={img} \
  -device qemu-xhci -device usb-tablet -nic none -display none \
  -debugcon file:/tmp/hv.log -global isa-debugcon.iobase=0x402 \
  >/dev/null 2>/tmp/hv_qemu.err
rc=$?
# QEMU's own stderr used to go to /dev/null, which hid the one failure this
# harness actually suffers: a straggler still holding the image's write lock,
# so the new QEMU exits INSTANTLY and every section below prints nothing.
# That reads exactly like a guest which produced no output.  `timeout` reports
# 124 when it did its job, and anything else here is worth seeing.
if [ $rc -ne 124 ] && [ $rc -ne 0 ]; then
  echo "--- qemu exited $rc immediately ---"
  head -3 /tmp/hv_qemu.err
fi
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
    # How long the guest gets, in seconds of WALL time - of which it sees a
    # slice per frame, so a couple of minutes of wall is a couple of seconds
    # of guest. Every device added since A6 costs the guest more of its own
    # boot, and a run that ends mid-command reads exactly like a hang.
    secs = 90
    for a in sys.argv[2:]:
        if a.startswith("--time="):
            secs = int(a.split("=", 1)[1])
    if not os.path.exists(IMG):
        sys.exit("no %s - build with UNO_DEBUG=1 UNO_DETACH=1 UNO_DBGCON=1, "
                 "then tools/mkuefi.py" % IMG)
    # The WHOLE image, not the first few megabytes.  DEBUG.CFG's position in
    # the FAT depends on what else is staged, and once the appliance payload
    # (a 17 MB kernel, an initramfs, a disk image) went in beside it, it moved
    # past a 4 MB window and this check started crying wolf on a perfectly
    # armed image - which is worse than not checking, because the warning
    # sends you looking at the wrong thing.
    with open(IMG, "rb") as f:
        head = f.read()
    if b"vm-selftest" not in head:
        print("warning: no `vm-selftest` found near the start of the image; "
              "the selftest will stay opt-out")

    print("copying %s to %s ..." % (IMG, host))
    if subprocess.run(["scp", "-q", IMG, "%s:%s" % (host, REMOTE_IMG)]).returncode:
        sys.exit("scp failed")
    # THE SCRIPT GOES OVER AS A FILE, not as an ssh argument.  Passed inline
    # it is one argument containing newlines, line continuations and a `pkill`,
    # and the combination started failing with ssh exit 255 and no output at
    # all on either stream - the least diagnosable failure available.  A file
    # is also what you want when debugging: it can be run by hand, or with
    # `bash -x`, exactly as the harness runs it.
    script = "/tmp/hv_run.sh"
    with open("build/hv_run.sh", "w", newline="\n") as f:
        f.write(RUN.format(t=secs, img=REMOTE_IMG))
    if subprocess.run(["scp", "-q", "build/hv_run.sh",
                       "%s:%s" % (host, script)]).returncode:
        sys.exit("scp of the run script failed")
    r = subprocess.run(["ssh", "-o", "BatchMode=yes", host, "bash " + script],
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
