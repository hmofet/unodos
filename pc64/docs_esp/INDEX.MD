# UnoDOS Studio — Developer Docs

Welcome to **Studio**, the IDE built into UnoDOS pc64. You write apps in
**UnoC** (a C subset), press **Ctrl-B** to compile them to a `.UNO` module,
and **Ctrl-R** to run them — all on the machine itself, no host toolchain.

## Contents

- **[App development](APPDEV.MD)** — the app model: the entry point, the
  `AppInterface` vtable, windows, the build-and-run loop.
- **[API reference](API.MD)** — every kernel function an app may call.
- **[The UnoC language](LANG.MD)** — the exact C subset the compiler accepts.
- **[Using Studio](IDE.MD)** — the editor, project pane, build output, keys.
- **[The AI assistant](AI.MD)** — connect ChatGPT, Claude or Gemini for help.

## Quick start

1. Open **Studio** from the launcher.
2. It greets you with `SAMPLE.C` — a bouncing-ball app.
3. Press **Ctrl-B**. The build pane reports `Built SAMPLE.UNO ...`.
4. Press **Ctrl-R**. Your app opens in its own window.
5. Edit, build, run again. That is the whole loop.

Your first app of your own: `File → New`, give it a name ending in `.C`,
`#include "UNO.H"`, and copy the shape of `SAMPLE.C`.
