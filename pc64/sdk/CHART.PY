# CHART.PY - a bar chart drawn from a text file.
#
# Put a DATA.CSV on the boot volume - one "label,value" line each, like
#
#     MON,12
#     TUE,19
#     WED,7
#
# and this app charts it, scaled to fit the window, with the biggest bar
# called out.  Edit the file in Notepad and press R here to reload it.
# With no file it charts built-in demo data so you can see the shape first.
#
# What it demonstrates: reading and parsing a file with uno.read(), integer
# scaling to a variable-size canvas (read cv.width()/height(), never
# hardcode - the shell picks your window size), and a static app that only
# repaints when its data changes.

import uno

FILE = "DATA.CSV"

BG   = uno.rgb(18, 20, 34)
INK  = uno.rgb(214, 218, 232)
DIM  = uno.rgb(110, 116, 140)
BAR  = uno.rgb(96, 170, 255)
TOP  = uno.rgb(255, 170, 90)

DEMO = [["MON", 12], ["TUE", 19], ["WED", 7], ["THU", 23],
        ["FRI", 17], ["SAT", 4], ["SUN", 9]]


class Chart(uno.App):

    def build(self, cv):
        self.load()

    def load(self):
        self.rows = []
        self.demo = False
        data = uno.read(FILE)
        if data:
            for ln in data.decode("latin1").split("\n"):
                parts = ln.strip().split(",")
                if len(parts) < 2:
                    continue
                try:
                    v = int(parts[1].strip())
                except ValueError:
                    continue             # a header line, or junk - skip it
                self.rows.append([parts[0].strip(), v if v > 0 else 0])
                if len(self.rows) == 16:  # more bars than that won't fit
                    break
        if not self.rows:
            self.rows = DEMO
            self.demo = True
        self.dirty = True

    def key(self, uni, scan, ctrl):
        if uni in (114, 82):             # r / R: reload the file
            self.load()
            return True
        return False

    def tick(self):
        if self.dirty:
            self.dirty = False
            return None
        return False

    def draw(self, cv):
        cv.clear(BG)
        head = ("%s not found - demo data (R reloads)" % FILE) if self.demo else FILE
        cv.text(10, 8, head, DIM if self.demo else INK)

        x0, y0 = 34, 30                  # plot origin (top-left)
        ph = cv.height() - y0 - 34       # plot height
        pw = cv.width() - x0 - 10
        base = y0 + ph                   # the x axis
        mx = 1
        for r in self.rows:
            if r[1] > mx:
                mx = r[1]

        cv.vline(x0, y0, ph, DIM)
        cv.hline(x0, base, pw, DIM)
        cv.text(4, y0 - 4, "%d" % mx, DIM)
        cv.text(4, base - 10, "0", DIM)

        n = len(self.rows)
        step = pw // n
        bw = step - 6 if step > 12 else step - 2
        for i in range(n):
            label, v = self.rows[i][0], self.rows[i][1]
            h = v * ph // mx
            x = x0 + 4 + i * step
            cv.fill_rect(x, base - h, bw, h, TOP if v == mx else BAR)
            cv.text(x, base - h - 12, "%d" % v, INK)
            cv.text(x, base + 6, label[:5], DIM)


app = Chart()
