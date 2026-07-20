# Python apps

UnoDOS treats **Python as a first-class app language**, right alongside UnoC.
You write real Python 3 - classes, functions, floats, the `math` module - in
Studio, press **Ctrl-B** to package it, and **Ctrl-R** to run it in its own
window. The interpreter is MicroPython, shipped as an optional module
`APPS\PYRT.UNO` (the Python runtime).

> If `PYRT.UNO` is not installed on your system, Studio still edits and builds
> UnoC apps normally; running a `.py` app reports
> `Python runtime not installed`.

## The app model

A Python app is a class that subclasses `uno.App`, plus a module-global
`app` holding an instance of it:

```python
import uno

class Hello(uno.App):
    def draw(self, cv):
        cv.clear(uno.rgb(0, 0, 0))
        cv.text(8, 8, "Hello from Python", uno.rgb(255, 255, 255))

app = Hello()
```

The runtime looks up `app` and calls its methods for you. Every method is
optional (though an app with no `draw` shows nothing):

| Method | When it runs | Use it for |
|---|---|---|
| `build(self, cv)` | once, as the window opens | set up state; `cv.width()/height()` are valid here |
| `draw(self, cv)` | whenever the window repaints | paint one frame |
| `tick(self)` | ~60 times a second | advance animation or game state |
| `key(self, uni, scan, ctrl)` | on a key press | handle input; return `True` if you consumed it |
| `opened(self)` / `closed(self)` | window shown / closing | acquire / release resources |

`cv` is a **Canvas**: `clear`, `fill_rect`, `rect`, `pixel`, `hline`, `vline`,
`text`, and `width()` / `height()`. Colours come from `uno.rgb(r, g, b)`.
The full surface is in **[the Python API reference](PYAPI.MD)** and in
`SDK\uno.pyi`.

## The sample

Studio greets you with `SDK\SAMPLE.PY`, a bouncing ball. It shows the whole
shape: `build` sets the ball's position and velocity, `tick` moves it and
bounces it off the walls, `draw` paints the background, a label and the ball,
and `key` speeds it up on the space bar.

1. Open **Studio** from the launcher.
2. It opens `SAMPLE.PY` with Python syntax highlighting.
3. Press **Ctrl-B** - the output pane reports `Packed SAMPLE.UNO`.
4. Press **Ctrl-R** - the ball bounces in its own window.

## Writing your own

`File → New`, give the file a name ending in **`.py`**, and follow the shape
of `SAMPLE.PY`. Studio routes by extension: a `.py` file is packaged for the
Python runtime, a `.c` file is compiled by the UnoC compiler. Everything else
- the editor, the project pane, build output, the AI assistant - is the same.

## How "compiling" works

Pressing **Ctrl-B** on a `.py` file does not translate it to machine code.
Studio wraps your source in a small `.UNO` container (the same format
`.c` apps compile to, flagged as a Python app) and writes `NAME.UNO`.
Running it hands that source to `PYRT.UNO`, which compiles and executes it
with the `uno` module bound in. Your app runs interpreted, with the whole
platform - framebuffer, sound, files - one `uno.` call away.

## Limits (v1)

- One Python app runs at a time; launching another replaces it.
- No `import` of other `.py` files yet - keep an app to a single file (it can
  be large). The standard `math` module and the built-ins are available.
- The `uno` module is the door to the platform; there is no `os`/`sys`/file
  system access except through `uno.read` / `uno.write`.
