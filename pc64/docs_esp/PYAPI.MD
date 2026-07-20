# The `uno` Python API

Everything a Python app reaches on the platform goes through the `uno` module.
This is the reference; for the app model and getting started see
**[Python apps](PYTHON.MD)**. A machine-readable stub is in `SDK\uno.pyi`.

```python
import uno
```

## Colours

- **`uno.rgb(r, g, b) -> int`** - pack a colour from three 0-255 channels.
  Every drawing call takes one of these values.

## The App base class

Subclass **`uno.App`** and create a module-global `app = MyApp()`. The runtime
calls whichever of these you define:

- **`build(self, cv)`** - once, when the window opens. `cv` is valid, so
  `cv.width()` / `cv.height()` give your drawable size.
- **`draw(self, cv)`** - paint one frame; called on every repaint.
- **`tick(self)`** - called about 60 times a second; advance your state.
- **`key(self, uni, scan, ctrl) -> bool`** - `uni` is the Unicode codepoint
  (0 if the key produces none), `scan` the raw scancode, `ctrl` nonzero when
  Ctrl is held. Return `True` if you handled the key.
- **`opened(self)` / `closed(self)`** - the window was shown / is closing.

## Canvas

The object passed to `build` and `draw`. Coordinates are canvas-relative -
(0, 0) is the top-left of your app's area.

| Method | Effect |
|---|---|
| `cv.width()` / `cv.height()` | the drawable size in pixels |
| `cv.clear(color)` | fill the whole canvas |
| `cv.fill_rect(x, y, w, h, color)` | filled rectangle |
| `cv.rect(x, y, w, h, color)` | rectangle outline |
| `cv.pixel(x, y, color)` | one pixel |
| `cv.hline(x, y, w, color)` | horizontal line |
| `cv.vline(x, y, h, color)` | vertical line |
| `cv.text(x, y, s, color)` | draw a string at (x, y) |

## Sound

- **`uno.beep(midi, ticks)`** - play a square-wave note at the given MIDI
  pitch for `ticks` (~60 per second).
- **`uno.quiet()`** - stop the sound.

## Files

`vol` is a volume index and defaults to 0 (the boot volume); the one-argument
forms take just a name.

- **`uno.read(vol, name) -> bytes`** / **`uno.read(name)`** - read a whole
  (small) file.
- **`uno.read_at(vol, name, off, n) -> bytes`** - read `n` bytes at byte
  offset `off`. Use this to stream a large file - a level, a `.wav`, a WAD -
  a piece at a time instead of loading it whole into memory.
- **`uno.size(vol, name) -> int`** / **`uno.size(name)`** - a file's size in
  bytes, or -1 if it is missing.
- **`uno.write(vol, name, data) -> bool`** / **`uno.write(name, data)`** -
  write bytes to a writable volume; returns `True` on success.

## A complete example

```python
import uno

class Bouncer(uno.App):
    def build(self, cv):
        self.w, self.h = cv.width(), cv.height()
        self.x, self.y = 20.0, 24.0
        self.vx, self.vy = 3.2, 2.3
        self.d = 22

    def draw(self, cv):
        cv.clear(uno.rgb(16, 18, 34))
        cv.text(8, 8, "Hello from Python", uno.rgb(210, 214, 230))
        cv.fill_rect(int(self.x), int(self.y), self.d, self.d, uno.rgb(255, 96, 96))

    def tick(self):
        self.x += self.vx
        self.y += self.vy
        if self.x < 0 or self.x + self.d > self.w:
            self.vx = -self.vx
        if self.y < 0 or self.y + self.d > self.h:
            self.vy = -self.vy

app = Bouncer()
```
