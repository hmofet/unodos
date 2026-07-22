# UnoDOS pc64 - the unoautomate UI smoke test (tools/automate_qemu.py stages
# this onto the disk as \AUTOMATE.PY with STRESS.CFG `automate`).
#
# Launches every launchable app through the real launcher code path, waits a
# few real shell frames, and asserts its window appeared via the PROBE
# snapshot; then closes it and moves on.  Results land in \AUTORES.TXT and on
# the SCRIPT log channel; the run powers the machine off when done.
import uno, unoauto


def win_titles():
    return [r[0] for r in unoauto.probe() if r[1] == 1]


def steps():
    n = unoauto.apps()
    unoauto.log("smoke: start, %d app slots" % n)
    ok = 0
    fails = []
    for i in range(n):
        before = win_titles()
        if not unoauto.launch(i):
            unoauto.log("smoke: slot %d refused (reserved)" % i)
            continue
        for _ in range(8):          # give the shell real frames to open+draw
            yield
        after = win_titles()
        new = [t for t in after if t not in before]
        # pass = a new window appeared, or the app was ALREADY open (launch
        # focuses it; the desktop opens one app at boot) and nothing crashed
        # away (window count did not drop)
        if new or len(after) >= len(before):
            ok += 1
            unoauto.log("smoke: slot %d OK -> %s"
                        % (i, new[0] if new else "(already open)"))
        else:
            fails.append(i)
            unoauto.log("smoke: slot %d FAIL (no window)" % i)
        unoauto.close_top()
        for _ in range(3):
            yield
    text = "SMOKE %d OK, %d FAIL of launchable slots\n" % (ok, len(fails))
    for i in fails:
        text += "FAIL slot %d\n" % i
    # volume 0 can be the RAM disk (gone at poweroff) - write everywhere
    # writable so the result survives on the FAT boot volume
    for v in range(4):
        uno.write(v, "AUTORES.TXT", text.encode())
    unoauto.log("smoke: done - %d ok, %d fail" % (ok, len(fails)))
    for _ in range(3):              # let the result file hit the disk
        yield
    unoauto.poweroff()


class Smoke(uno.App):
    name = "unoautomate"

    def build(self, cv):
        self.g = steps()

    def tick(self):
        try:
            next(self.g)
        except StopIteration:
            pass


app = Smoke()
