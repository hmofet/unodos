# Using Studio

Studio is a single window with a menu bar, a project list, a code editor, a
build-output pane, and (on a roomy desktop) an AI column.

## Layout

- **Menu bar** — File, Edit, Build, Run, AI, Help. Click a title for its menu.
- **Project** (left) — the source files on the working volume. Click one to
  open it (the current file is saved first if it has a name).
- **Editor** (centre) — monospace, with syntax highlighting and a line-number
  gutter.
- **Build output** (bottom) — build results and errors. A red error line is
  clickable: it jumps the caret to that line.
- **Assistant** (right, wide desktops only) — see [AI.MD](AI.MD).

The side columns appear only when the window is wide enough. Raise the desktop
resolution in the **Control Panel** to bring the AI column in.

## Keys

| Key | Action |
|-----|--------|
| **Ctrl-B** | Build the open file to a `.UNO` |
| **Ctrl-R** | Build (if needed) and run it |
| **Ctrl-S** | Save |
| **Ctrl-X / C / V** | Cut / copy / paste |
| **Ctrl-A** | Select all |
| Arrows, Home/End, PgUp/PgDn | Move the caret |
| Shift + movement | Extend the selection |
| Tab | Insert four spaces |

Enter keeps the previous line's indentation.

## Files

New files and Save-As prompt for a name in the status bar — type it and press
Enter. Names are 8.3 (e.g. `GAME.C`); a source file should end in `.C`.
Building `GAME.C` writes `GAME.UNO` beside it and adds it to the project list.

## The build/run loop

1. Edit.
2. **Ctrl-B** — read the output pane; click any error to jump to it.
3. **Ctrl-R** — your app opens in its own window with its own taskbar chip.
4. Repeat. Each run replaces the previous instance of your app.

## Help

The Help menu opens these docs in the browser: the API reference, the app
guide, the language reference, and this page.
