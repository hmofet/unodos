# uno.pyi - type stubs for the UnoDOS `uno` module (PYRT.UNO).
#
# This file documents the API for editors/humans; it is not imported at
# runtime (the real `uno` module is built into PYRT.UNO).  See DOCS\PYAPI.MD.

from typing import Optional


def rgb(r: int, g: int, b: int) -> int:
    """Pack an (r, g, b) triple (0-255 each) into a colour value."""
    ...


def beep(midi: int, ticks: int) -> None:
    """Play a square-wave note (MIDI pitch) for `ticks` (~60 per second)."""
    ...


def quiet() -> None:
    """Stop any sound started with beep()."""
    ...


def read(vol: int, name: str) -> bytes: ...
def read(name: str) -> bytes:
    """Read a whole file into bytes.  `vol` defaults to 0 (the boot volume)."""
    ...


def read_at(vol: int, name: str, off: int, n: int) -> bytes:
    """Read `n` bytes at byte offset `off` - stream large files (e.g. a WAD)
    without loading them whole."""
    ...


def size(vol: int, name: str) -> int: ...
def size(name: str) -> int:
    """Return a file's size in bytes, or -1 if it does not exist."""
    ...


def write(vol: int, name: str, data: bytes) -> bool: ...
def write(name: str, data: bytes) -> bool:
    """Write bytes to a file on a writable volume.  Returns True on success."""
    ...


class Canvas:
    """The drawable surface passed to build() and draw().  Coordinates are
    canvas-relative; (0, 0) is the top-left of your app's area."""

    def width(self) -> int: ...
    def height(self) -> int: ...
    def clear(self, color: int) -> None: ...
    def fill_rect(self, x: int, y: int, w: int, h: int, color: int) -> None: ...
    def rect(self, x: int, y: int, w: int, h: int, color: int) -> None: ...
    def pixel(self, x: int, y: int, color: int) -> None: ...
    def hline(self, x: int, y: int, w: int, color: int) -> None: ...
    def vline(self, x: int, y: int, h: int, color: int) -> None: ...
    def text(self, x: int, y: int, s: str, color: int) -> None: ...

    def wall_col(self, x: int, y0: int, count: int, grid, tw: int, th: int,
                 texcol: int, v0_fp: int, dv_fp: int, pal, sh: int) -> None:
        """Fast textured vertical column (the whole per-pixel loop in C): draw
        `count` pixels down from (x, y0) sampling an 8-bit texture
        `grid[texcol*th + v]` with .8 fixed-point texel coords v0_fp/dv_fp,
        shaded by sh/256, through the 768-byte RGB palette `pal`.  Built for
        Duum's renderer; grid and pal are bytes-like."""
        ...


class App:
    """Base class for a UnoDOS Python app.  Subclass it, define the callbacks
    you need, and create a module-global `app = MySubclass()`.  Every callback
    is optional except that an app without draw() shows nothing.

        import uno
        class Hello(uno.App):
            def draw(self, cv):
                cv.clear(uno.rgb(0, 0, 0))
                cv.text(8, 8, "hi", uno.rgb(255, 255, 255))
        app = Hello()
    """

    def build(self, cv: "Canvas") -> None:
        """Called once when the window opens.  Set up your state here; cv is
        valid so cv.width()/height() give your drawable size."""
        ...

    def draw(self, cv: "Canvas") -> None:
        """Paint one frame.  Called whenever the window needs repainting."""
        ...

    def tick(self) -> None:
        """Advance animation/game state.  Called ~60 times a second."""
        ...

    def key(self, uni: int, scan: int, ctrl: int) -> bool:
        """A key was pressed: `uni` is the Unicode codepoint (0 if none),
        `scan` the raw scancode, `ctrl` nonzero if Ctrl is held.  Return True
        if you handled it."""
        ...

    def opened(self) -> None:
        """Called after the window is added to the desktop."""
        ...

    def closed(self) -> None:
        """Called when the window is closing - release anything here."""
        ...
