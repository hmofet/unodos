#!/usr/bin/env python3
"""Full pc64 networking + TLS verification: e1000 under SLIRP with a TFTP
server, a TCP `cat` echo, and a per-connection TLS echo server, plus RDRAND
(-cpu max). Launches the Network app and screenshots the results."""
import sys, os, time, subprocess
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import harness

HERE = os.path.dirname(os.path.abspath(__file__))
TT = HERE + "/tls_test"
TAG = sys.argv[1] if len(sys.argv) > 1 else "tls_1"

# fresh clone: the throwaway key/cert are gitignored; regenerate + re-pin.
# (If this regenerates a new key, re-run build.sh so the pin matches.)
if not os.path.exists(TT + "/server.crt"):
    subprocess.run(["sh", TT + "/gen.sh"], check=True)
    print("NOTE: regenerated tls_test cert; rebuild (build.sh) if the pin changed")

subprocess.run(["cp", harness.OVMF_VARS, "build/vars.fd"], check=True)
if os.path.exists(harness.QMP_SOCK):
    os.remove(harness.QMP_SOCK)

# Run on a scratch COPY of the ESP: vvfat fat:rw writes guest telemetry back
# into the host dir, and build/esp must stay pristine (it gets shipped).
# The copy also drops STRESS.CFG - this scenario drives the Network app
# MANUALLY on a fixed timeline, so the stress driver must not run (bounded
# default powers the guest off mid-test) and the boot net test must not race
# it (it skips when no STRESS.CFG exists). nettest_stage.py covers the
# armed-boot path.
import shutil
ESP = "build/esp-nettest"
if os.path.exists(ESP):
    shutil.rmtree(ESP)
shutil.copytree("build/esp", ESP)
if os.path.exists(ESP + "/STRESS.CFG"):
    os.remove(ESP + "/STRESS.CFG")

netdev = (
    "user,id=n0,tftp=build/tftp,"
    "guestfwd=tcp:10.0.2.100:9000-cmd:cat,"
    "guestfwd=tcp:10.0.2.100:9443-cmd:python3 "
    + TT + "/tls_server.py " + TT + "/server.crt " + TT + "/server.key"
)

qemu = subprocess.Popen([
    "qemu-system-x86_64", "-machine", "q35", "-m", "256", "-cpu", "max",
    "-drive", "if=pflash,format=raw,readonly=on,file=" + harness.OVMF_CODE,
    "-drive", "if=pflash,format=raw,file=build/vars.fd",
    "-drive", "format=vvfat,file=fat:rw:" + ESP,
    "-netdev", netdev, "-device", "e1000,netdev=n0",
    "-display", "none",
    "-qmp", "unix:%s,server,nowait" % harness.QMP_SOCK,
], stderr=subprocess.PIPE)

time.sleep(3)
if qemu.poll() is not None:
    print("QEMU EXIT", qemu.returncode, qemu.stderr.read().decode()[:600])
    sys.exit(1)

q = harness.Qmp(harness.QMP_SOCK)
time.sleep(16)
# the shell auto-opens Control Panel now; close it so the arrow keys drive the
# desktop icon selection, not the panel's widgets
harness.keys(q, "esc")
time.sleep(1)
for _ in range(12):
    harness.keys(q, "right")
harness.keys(q, "ret")
time.sleep(int(sys.argv[2]) if len(sys.argv) > 2 else 30)
harness.shot(q, TAG)
q.cmd("quit")
qemu.wait()
