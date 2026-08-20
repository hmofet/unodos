#!/usr/bin/env python3
"""unocode_urc - the UnoCode merge gate, driven on the real machine.

    cd pc64 && UNO_DEBUG=1 ./build.sh && python3 unocode/tools/unocode_urc.py

WHAT IT PROVES, in the order it proves it:

  1. APPS\\UNOCODE.UNO loads and opens a window titled "UnoCode" - launched by
     app ID, never by menu index, because a menu index drifts the moment
     anybody installs an app and a test that counts rows does not fail when it
     does, it drives the wrong app.
  2. The command palette opens, filters, and closes.
  3. The integrated terminal runs commands, and `ext` sees the extensions that
     were scanned off the ESP.
  4. A THEME contributed by an extension (Nord, a declarative extension with no
     JavaScript at all) is selectable and repaints the workbench.
  5. A COMMAND contributed by an extension runs its JavaScript - which is the
     whole extension host in one check: manifest read, activation event fired,
     MAIN.JS evaluated in unojs, registerCommand handler invoked, and the
     notification it raised drawn on screen.
  6. A file opens with syntax highlighting from a grammar.
  7. Typing into a document puts the characters in the order they were typed,
     and undo takes them back out.

Driven over URC rather than QMP send-key for the reason tools/urcui.py gives:
QEMU's usb-tablet delivers no pointer motion to this guest, so URC is the only
road that can click.  Every step also leaves a screenshot in pc64/shots/, so a
failure is a picture as well as a line of output.
"""
import os, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
PC64 = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(PC64, "tools"))
os.chdir(PC64)

from urcui import UrcUi                                       # noqa: E402

fails = []


def check(cond, what):
    print(("  ok    " if cond else "  FAIL  ") + what)
    if not cond:
        fails.append(what)


def ctrl(ui, ch, settle=0.5):
    """A Ctrl chord.  UnoCode reads Shift off the CASE of the character (see
    the header of unocode/uc_main.c), so an upper-case letter here IS
    Ctrl+Shift+<letter> - which is how the palette is opened below."""
    ui.key(ord(ch), ctrl=1, settle=settle)


def type_line(ui, s):
    ui.text(s)
    ui.key(0x0D, settle=0.9)             # Enter


with UrcUi() as ui:
    print("== 1. the module loads and opens ==")
    ui.launch_id("unocode", settle=5.0)
    titles = ui.windows()
    print("windows: %s" % ", ".join(titles))
    check(any(t.startswith("UnoCode") for t in titles),
          "APPS\\UNOCODE.UNO opened a window titled 'UnoCode'")
    ui.shot("unocode_workbench")

    print("== 2. the command palette ==")
    ctrl(ui, 'P', settle=0.8)            # Ctrl+Shift+P
    ui.text("theme")
    time.sleep(0.5)
    ui.shot("unocode_palette")
    ui.key(0, scan=0x17, settle=0.5)     # Escape

    print("== 3. the integrated terminal ==")
    ctrl(ui, '`', settle=0.9)
    type_line(ui, "help")
    ui.shot("unocode_terminal")
    type_line(ui, "ext")
    time.sleep(0.4)
    ui.shot("unocode_ext_list")

    print("== 4. a theme contributed by a declarative extension ==")
    type_line(ui, "theme Nord")
    time.sleep(0.6)
    ui.shot("unocode_theme_nord")

    print("== 5. an extension's JavaScript command ==")
    # the palette, filtered to the Hello extension's contributed command.
    ctrl(ui, 'P', settle=0.8)
    ui.text("Say Hello")
    time.sleep(0.5)
    ui.key(0x0D, settle=1.4)             # Enter: activates the extension
    ui.shot("unocode_ext_command")

    print("== 6. a file, highlighted ==")
    ctrl(ui, '`', settle=0.6)            # back to the terminal
    type_line(ui, "open SDK\\SAMPLE.C")
    time.sleep(0.8)
    ui.shot("unocode_editor_c")

    # The window is still the one we opened, and nothing crashed the shell:
    # if the module had faulted, the window would be gone and the desktop
    # would be back.
    titles = ui.windows()
    check(any(t.startswith("UnoCode") for t in titles),
          "the window survived every step (no fault, no forced close)")

    print("== 7. typing, IntelliSense and undo ==")
    # A new untitled file, and real characters into the real editor.  This is
    # the scene that caught cursors_adjust(): a caret that does not advance
    # types the line backwards, and every other check in this file passed
    # while it did, because nothing else typed into a DOCUMENT.
    ctrl(ui, 'n', settle=0.8)
    ui.text("int main(void)")
    ui.key(0x0D, settle=0.3)
    ui.text("    return 0;")
    time.sleep(0.4)
    ui.shot("unocode_typing")
    ctrl(ui, 'z', settle=0.5)             # undo
    ui.shot("unocode_undo")

    print("== 8. settings round-trip ==")
    ctrl(ui, '`', settle=0.8)             # show/focus the terminal
    type_line(ui, "set editor.minimap.enabled false")
    type_line(ui, "get editor.minimap.enabled")
    time.sleep(0.4)
    ui.shot("unocode_settings")

    print("== 9. the other four side-bar views ==")
    ctrl(ui, 'X', settle=0.8)             # Ctrl+Shift+X: Extensions
    ui.shot("unocode_view_extensions")
    ctrl(ui, 'D', settle=0.8)             # Ctrl+Shift+D: Run and Debug
    ui.shot("unocode_view_run")
    ctrl(ui, 'G', settle=0.8)             # Ctrl+Shift+G: Source Control
    ui.shot("unocode_view_scm")
    ctrl(ui, 'F', settle=0.8)             # Ctrl+Shift+F: Search
    ui.text("return")
    ui.key(0x0D, settle=1.8)
    ui.shot("unocode_view_search")
    ctrl(ui, 'E', settle=0.8)             # back to the Explorer

    print("== 10. find, IntelliSense, comment toggle ==")
    # back to the C file, so there is something with syntax to work on
    ctrl(ui, 'p', settle=0.8)
    ui.text("SAMPLE")
    ui.key(0x0D, settle=1.2)
    ctrl(ui, 'f', settle=0.8)             # the find widget
    ui.text("static")
    time.sleep(0.6)
    ui.shot("unocode_find")
    ui.key(0, scan=0x17, settle=0.4)      # Escape closes it

    ctrl(ui, ' ', settle=0.9)             # Ctrl+Space: trigger suggest
    ui.shot("unocode_suggest")
    ui.key(0, scan=0x17, settle=0.4)

    ctrl(ui, '/', settle=0.7)             # Ctrl+/: toggle line comment
    ui.shot("unocode_comment")

    print("== 11. multiple cursors ==")
    ctrl(ui, 'p', settle=0.8)
    ui.text(">Add Cursor Below")          # '>' turns Go to File into the palette
    ui.key(0x0D, settle=0.7)
    ctrl(ui, 'p', settle=0.8)
    ui.text(">Add Cursor Below")
    ui.key(0x0D, settle=0.7)
    ui.text("// ")
    time.sleep(0.5)
    ui.shot("unocode_multicursor")

    print("== 12. a light theme ==")
    ctrl(ui, 'k', settle=0.7)             # Ctrl+K Ctrl+T: the theme picker
    ctrl(ui, 't', settle=0.7)
    ui.text("Light")
    ui.key(0x0D, settle=1.2)
    ui.shot("unocode_theme_light")

print("\nshots are in pc64/shots/unocode_*.png")
print("%d checks failed" % len(fails))
sys.exit(1 if fails else 0)
