# SAMPLE.PY - the Python counterpart of SAMPLE.C: a bouncing ball.
#
# A UnoDOS Python app is a class deriving from uno.App with a module-global
# `app` instance.  The runtime (PYRT.UNO) calls your methods:
#
#   build(self, cv)  once, when the window opens - set up state
#   draw(self, cv)   whenever the window needs repainting - draw the frame
#   tick(self)       ~60 times a second - advance animation/game state
#   key(self, uni, scan, ctrl) -> True if you consumed the key
#
# `cv` is a Canvas: clear/fill_rect/rect/pixel/hline/vline/text, width()/height().
# uno.rgb(r, g, b) makes a colour.  See SDK\uno.pyi for the full API.

import uno


class Bouncer(uno.App):

    def build(self, cv):
        self.w = cv.width()
        self.h = cv.height()
        self.x = 20.0
        self.y = 24.0
        self.vx = 3.2
        self.vy = 2.3
        self.d = 22                    # ball diameter

    def draw(self, cv):
        w = cv.width()
        h = cv.height()
        cv.clear(uno.rgb(16, 18, 34))
        cv.text(8, 8, "Hello from Python", uno.rgb(210, 214, 230))
        cv.fill_rect(int(self.x), int(self.y), self.d, self.d, uno.rgb(255, 96, 96))
        cv.rect(int(self.x), int(self.y), self.d, self.d, uno.rgb(255, 200, 120))

    def tick(self):
        self.x += self.vx
        self.y += self.vy
        if self.x < 0:
            self.x = 0.0
            self.vx = -self.vx
        if self.x + self.d > self.w:
            self.x = self.w - self.d
            self.vx = -self.vx
        if self.y < 0:
            self.y = 0.0
            self.vy = -self.vy
        if self.y + self.d > self.h:
            self.y = self.h - self.d
            self.vy = -self.vy

    def key(self, uni, scan, ctrl):
        if uni == 32:                  # space = speed the ball up
            self.vx = self.vx * 1.25
            self.vy = self.vy * 1.25
            return True
        return False


app = Bouncer()
