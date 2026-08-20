# TODO.PY - a to-do list that survives reboots.
#
# Type a task and press Enter to add it.  Up/Down pick a task, Tab checks
# it off, Delete removes it, Esc clears what you were typing.  The list is
# written to TODO.TXT on the boot volume after every change and loaded
# back the next time the app opens - it is plain text, so Notepad can read
# and edit the same file.
#
# What it demonstrates, beyond SAMPLE.PY: persistence with uno.read() and
# uno.write(), telling printable keys (uni) from special keys (scan) apart,
# and the idle idiom - tick() returns False whenever nothing changed, so
# the shell doesn't repaint a static window 60 times a second.

import uno

FILE = "TODO.TXT"

BG   = uno.rgb(20, 22, 36)
INK  = uno.rgb(214, 218, 232)
DIM  = uno.rgb(120, 126, 150)
HEAD = uno.rgb(120, 200, 255)
GOOD = uno.rgb(110, 210, 130)
BAD  = uno.rgb(240, 120, 120)
SELB = uno.rgb(44, 50, 80)


class Todo(uno.App):

    def build(self, cv):
        self.items = []              # [done, text] pairs
        self.sel = 0
        self.edit = ""
        self.saved = True
        self.dirty = True
        data = uno.read(FILE)        # None if the file doesn't exist yet
        if data:
            for ln in data.decode("latin1").split("\n"):
                ln = ln.strip()
                if ln.startswith("[x] "):
                    self.items.append([True, ln[4:]])
                elif ln.startswith("[ ] "):
                    self.items.append([False, ln[4:]])

    def save(self):
        out = ""
        for done, text in self.items:
            out += ("[x] " if done else "[ ] ") + text + "\n"
        # write() returns False on a read-only volume - show it, don't hide it
        self.saved = uno.write(FILE, out.encode())

    def key(self, uni, scan, ctrl):
        if scan == 1 and self.sel > 0:                    # Up
            self.sel -= 1
        elif scan == 2 and self.sel + 1 < len(self.items):  # Down
            self.sel += 1
        elif scan == 8 and self.items:                    # Delete: remove
            self.items.pop(self.sel)
            if self.sel and self.sel >= len(self.items):
                self.sel -= 1
            self.save()
        elif scan == 0x17:                                # Esc: drop the entry
            self.edit = ""
        elif uni == 13 and self.edit:                     # Enter: add
            self.items.append([False, self.edit])
            self.sel = len(self.items) - 1
            self.edit = ""
            self.save()
        elif uni == 9 and self.items:                     # Tab: toggle done
            self.items[self.sel][0] = not self.items[self.sel][0]
            self.save()
        elif uni == 8:                                    # Backspace
            self.edit = self.edit[:-1]
        elif 32 <= uni < 127 and len(self.edit) < 48:     # typing
            self.edit += chr(uni)
        else:
            return False
        self.dirty = True
        return True

    def tick(self):
        if self.dirty:
            self.dirty = False
            return None              # repaint this frame...
        return False                 # ...and sleep the rest of the time

    def draw(self, cv):
        cv.clear(BG)
        done = 0
        for it in self.items:
            if it[0]:
                done += 1
        cv.text(10, 8, "TO-DO", HEAD)
        cv.text(cv.width() - 110, 8, "%d of %d done" % (done, len(self.items)), DIM)
        cv.hline(10, 24, cv.width() - 20, DIM)

        # the list (the last 14 entries fit; older ones scroll away)
        shown = self.items[-14:]
        base = len(self.items) - len(shown)
        y = 32
        for i in range(len(shown)):
            if base + i == self.sel:
                cv.fill_rect(6, y - 2, cv.width() - 12, 15, SELB)
            mark, text = shown[i][0], shown[i][1]
            cv.text(10, y, "[x]" if mark else "[ ]", GOOD if mark else DIM)
            cv.text(42, y, text, DIM if mark else INK)
            y += 16
        if not self.items:
            cv.text(42, 40, "nothing yet - type a task, press Enter", DIM)

        # the entry line and the save state
        y = cv.height() - 44
        cv.hline(10, y, cv.width() - 20, DIM)
        cv.text(10, y + 6, "> " + self.edit + "_", INK)
        cv.text(10, y + 22, "Enter add   Tab done   Del remove", DIM)
        cv.text(10, y + 34,
                "saved to " + FILE if self.saved else "NOT SAVED - volume is read-only",
                GOOD if self.saved else BAD)


app = Todo()
