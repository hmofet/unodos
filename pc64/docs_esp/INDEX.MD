# UnoDOS Studio — Developer Docs

Welcome to **Studio**, the IDE built into UnoDOS pc64. You write apps in
**UnoC** (a C subset) or in **Python 3** — both first-class — press **Ctrl-B**
to build them into a `.UNO` module, and **Ctrl-R** to run them — all on the
machine itself, no host toolchain.

## Contents

- **[App development](APPDEV.MD)** — the app model: the entry point, the
  `AppInterface` vtable, windows, the build-and-run loop.
- **[API reference](API.MD)** — every kernel function an app may call.
- **[The UnoC language](LANG.MD)** — the exact C subset the compiler accepts.
- **[Python apps](PYTHON.MD)** — writing apps in Python 3, the `uno.App` model.
- **[The Python API](PYAPI.MD)** — the `uno` module: canvas, sound, files.
- **[Using Studio](IDE.MD)** — the editor, project pane, build output, keys.
- **[The AI assistant](AI.MD)** — connect ChatGPT, Claude or Gemini for help.

## Quick start

1. Open **Studio** from the launcher.
2. It greets you with `SAMPLE.PY` — a bouncing-ball app in Python (or
   `SAMPLE.C`, the UnoC version, if Python is not installed).
3. Press **Ctrl-B**. The build pane reports `Packed SAMPLE.UNO` (or
   `Built SAMPLE.UNO ...` for a `.c` file).
4. Press **Ctrl-R**. Your app opens in its own window.
5. Edit, build, run again. That is the whole loop.

Your first app of your own: `File → New`, give it a name ending in `.py`
(Python) or `.c` (UnoC), and copy the shape of the matching sample. See
**[Python apps](PYTHON.MD)** or **[App development](APPDEV.MD)**.
