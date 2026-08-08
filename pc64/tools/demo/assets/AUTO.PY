# AUTO.PY - the machine automating itself, in Python.
#
# `unoauto` is UnoDOS's automation subsystem: the same surface the remote
# harness drives this box with, handed to any Python app. This app first
# OBSERVES the running system through unoauto.probe(), then DRIVES it - it
# opens the Start menu and launches an app by injecting keystrokes - and
# then looks again, and sees its own effect.

import uno, unoauto

CLOCK_ROW = 8                    # Clock's row in the Start menu
ESC, DOWN, ENTER = 0x17, 0x02, 13


class Automate(uno.App):

    def build(self, cv):
        self.lines = []
        self.until = 0
        self.steps = self.script()

    def say(self, s):
        self.lines.append(s)
        unoauto.log(s)           # ...and into the system log

    def windows(self):
        return [p[0] for p in unoauto.probe() if p[1] == 1 and p[0].strip()]

    # ---- what the machine does to itself; each yield is a pause in ms ----
    def script(self):
        self.say("unoauto.available() -> %s" % unoauto.available())
        yield 2500
        self.say("uptime: %d s" % (unoauto.uptime() // 1000))
        for p in unoauto.probe():
            if p[0] == "heap":
                self.say("heap: %d KB used, %d KB free"
                         % (p[3] // 1024, p[4] // 1024))
            elif p[0] == "fs":
                self.say("fs:   %d volumes, %d writable" % (p[2], p[3]))
            elif p[0] == "net":
                self.say("net:  link=%d  tx=%d  rx=%d"
                         % (p[2] & 1, p[3], p[4]))
        yield 3000
        self.say("windows: %s" % ", ".join(self.windows()))
        # The OBSERVE phase above is deliberately ~8.5 s long: the driver
        # spends it sliding this window off Studio's source, and a pointer
        # press on this title bar takes the keyboard focus - so the Start menu
        # must not be open yet. It was, once, and every Down and the Enter went
        # to this window instead: the menu sat there on row 0, Clock never
        # opened, and the transcript below still claimed it had.
        yield 3000
        self.say("now DRIVING it: Ctrl+Esc opens the Start menu")
        unoauto.key(ESC, 0, 1)   # key(scan, unicode, ctrl)
        yield 1500
        for i in range(CLOCK_ROW):
            unoauto.key(DOWN, 0)
            yield 320
        self.say("...walked down to Clock. Enter.")
        unoauto.key(0, ENTER)
        yield 2400
        self.say("windows: %s" % ", ".join(self.windows()))
        self.say("the machine opened that one itself.")

    def tick(self):
        if self.steps is None:
            return
        now = unoauto.uptime()   # paced on the wall clock, not on frames
        if now < self.until:
            return
        try:
            self.until = now + next(self.steps)
        except StopIteration:
            self.steps = None

    def draw(self, cv):
        cv.clear(uno.rgb(16, 18, 34))
        cv.text(10, 8, "unoauto - automation, from Python",
                uno.rgb(120, 200, 255))
        cv.hline(10, 26, cv.width() - 20, uno.rgb(60, 66, 96))
        y = 34
        for s in self.lines[-16:]:
            cv.text(10, y, s, uno.rgb(214, 218, 232))
            y += 16


app = Automate()
