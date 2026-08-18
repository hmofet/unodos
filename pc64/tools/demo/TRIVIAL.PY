# Control app: the cheapest possible PYAPP that still forces a repaint every
# tick. If this runs at the same frame rate as Duum, the limit is the shell /
# present / stream path, not Duum's renderer.
import uno

class Trivial(uno.App):
    def build(self, cv):
        self.w = cv.width(); self.h = cv.height(); self.n = 0
    def tick(self):
        self.n += 1
        return True                      # always repaint
    def draw(self, cv):
        c = (self.n * 7) & 255
        cv.clear(uno.rgb(c, 40, 255 - c))
        cv.fill_rect(10, 10, 60 + (self.n % 40), 40, uno.rgb(255, 255, 0))
    def key(self, uni, scan, ctrl):
        return False

app = Trivial()
