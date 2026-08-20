# GOODNITE.PY - an end-of-day automation app.
#
# One launch journals what you were working on, then offers to shut the
# machine down.  It is a worked example of `unoscript`, the production
# automation surface, doing real work under the permission model:
#
#   - reading the window tree     u.ui.screen()      tier 0, ambient
#   - writing into your home      u.fs.write()       tier 1, fs.user
#   - powering the machine off    u.sys.power(0)     tier 2, `power` -
#                                 granted only through an explicit consent
#                                 (or a role, or a signed manifest)
#
# A capability you don't hold never crashes the app: denied calls raise
# OSError, and u.request() simply answers False.  Handle both and the same
# script runs everywhere - it just does less on a machine that trusts it
# less.
#
# The steps run from a generator paced by tick() on unoauto.uptime(),
# the wall clock in milliseconds (ungated, real time in every build).
# That pattern is load-bearing for every automation app: an injected
# action lands on the NEXT shell frame, so a script must yield before it
# looks at the result.

import uno
import unoauto
import unoscript as u

JOURNAL = "journal/GOODNITE.LOG"
KEEP = 3000              # stay under unoscript's 4 KB read cap

BG   = uno.rgb(16, 18, 34)
HEAD = uno.rgb(120, 200, 255)
INK  = uno.rgb(214, 218, 232)
WARN = uno.rgb(255, 170, 90)


class GoodNight(uno.App):

    def build(self, cv):
        self.lines = []
        self.armed = False
        self.until = 0
        self.gen = self.script()
        self.dirty = True

    def say(self, s):
        self.lines.append(s)
        self.dirty = True

    # ---- the workflow; each yield is a pause in milliseconds -------------
    def script(self):
        uid = u.whoami()                 # 0xFFFFFFFF when nobody is signed in
        self.say("Good night." if uid in (-1, 0xFFFFFFFF)
                 else "Good night, user %d." % uid)
        yield 500

        # 1. what is open right now?  (ui.read - ambient, tier 0)
        try:
            wins = [ln.strip() for ln in u.ui.screen().split("\n") if ln.strip()]
        except OSError:
            wins = []
        self.say("open right now: %d window(s)" % len(wins))
        for w in wins[:6]:
            self.say("  " + w[:44])
        yield 1000

        # 2. journal it into your home  (fs.user, tier 1)
        entry = "-- session, uptime %d min, %d window(s)\n" % (
            unoauto.uptime() // 60000, len(wins))
        for w in wins[:6]:
            entry += "   " + w[:60] + "\n"
        journaled = False
        try:
            try:
                old = u.fs.read(JOURNAL).decode("latin1")
            except OSError:
                old = ""                     # first run: no journal yet
            u.fs.write(JOURNAL, (old + entry)[-KEEP:].encode())
            self.say("journaled to home:" + JOURNAL)
            journaled = True
        except (OSError, NotImplementedError):
            self.say("journal skipped - fs.user not granted")
        yield 500

        # 3. ask permission to power off  (power, tier 2)
        if u.request("power"):
            self.say("")
            self.say("Enter shuts the machine down.  Esc keeps it on.")
            self.armed = True
        else:
            self.say("")
            self.say("`power` not granted - leaving the machine on.")
            if journaled:
                self.say("(journal is written; close me when you're done)")

    def tick(self):
        if self.gen:
            now = unoauto.uptime()       # wall-clock milliseconds
            if now >= self.until:
                try:
                    self.until = now + next(self.gen)
                except StopIteration:
                    self.gen = None
        if self.dirty:
            self.dirty = False
            return None
        return False

    def key(self, uni, scan, ctrl):
        if self.armed and uni == 13:         # Enter: do it
            self.say("shutting down...")
            u.sys.power(0)                   # does not return when granted
            return True
        if self.armed and scan == 0x17:      # Esc: stand down
            self.armed = False
            self.say("cancelled - staying on.")
            return True
        return False

    def draw(self, cv):
        cv.clear(BG)
        cv.text(10, 8, "GOODNITE - end-of-day automation", HEAD)
        cv.hline(10, 24, cv.width() - 20, uno.rgb(60, 66, 96))
        y = 32
        for s in self.lines[-14:]:
            cv.text(10, y, s, WARN if self.armed and s.startswith("Enter") else INK)
            y += 16


app = GoodNight()
