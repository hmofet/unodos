#!/usr/bin/env python3
"""Drive UnoWord in QEMU and screenshot every step (OFFICE97-PLAN §5 phase 8).

Phase 8 landed as "the module builds and ships", which is not the same as
"it works".  This is the script that settles the difference: it boots the
real ESP, finds UnoWord on the desktop, opens it, types into it, and shoots
each step into shots/uow_*.png.

Keyboard-first where a key is the natural gesture; the usb-tablet is used to
click the things that only exist as pixels (a desktop icon, a toolbar
button, a menu).
"""
import os, subprocess, sys, time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import harness as H

W, Hh = 1280, 800     # the GOP mode OVMF picks on this setup


def mmove(q, x, y):
    q.cmd("input-send-event", events=[
        {"type": "abs", "data": {"axis": "x", "value": int(x * 32767 / W)}},
        {"type": "abs", "data": {"axis": "y", "value": int(y * 32767 / Hh)}}])
    time.sleep(0.15)


def mclick(q, x, y, settle=0.35):
    mmove(q, x, y)
    q.cmd("input-send-event",
          events=[{"type": "btn", "data": {"down": True, "button": "left"}}])
    time.sleep(0.12)
    q.cmd("input-send-event",
          events=[{"type": "btn", "data": {"down": False, "button": "left"}}])
    time.sleep(settle)


def mdouble(q, x, y):
    mclick(q, x, y, settle=0.12)
    mclick(q, x, y, settle=0.45)


def combo(q, *names, hold=40):
    q.cmd("send-key", keys=[{"type": "qcode", "data": n} for n in names],
          **{"hold-time": hold})
    time.sleep(0.3)


def main():
    subprocess.run(["cp", H.OVMF_VARS, "build/vars.fd"], check=True)
    if os.path.exists(H.QMP_SOCK):
        os.remove(H.QMP_SOCK)
    argv = [
        "qemu-system-x86_64", "-machine", "q35", "-m", "256",
        "-drive", "if=pflash,format=raw,readonly=on,file=" + H.OVMF_CODE,
        "-drive", "if=pflash,format=raw,file=build/vars.fd",
        "-drive", "format=vvfat,file=fat:rw:build/esp",
        "-device", "qemu-xhci", "-device", "usb-tablet",
        "-nic", "none", "-display", "none",
        "-qmp", "unix:%s,server,nowait" % H.QMP_SOCK,
    ]
    qemu = subprocess.Popen(argv)
    try:
        q = H.Qmp(H.QMP_SOCK)
        print("qemu up; booting...")
        time.sleep(22)
        H.shot(q, "uow_00_desktop")

        # close the Control Panel the shell opens on boot (Ctrl-W)
        combo(q, "ctrl", "w")
        time.sleep(0.6)

        H.shot(q, "uow_01_desktop_clean")

        # Down to UnoWord by an EXACT count.  Two things this script learned
        # the hard way: the Programs list ends with Tile / Cascade / Minimize
        # all / Restart / Shut Down, so walking "well past the bottom" to
        # reach the last app lands on Shut Down and ends the run; and a
        # double-click on the desktop icon does not open it.
        #   0 Control  1 Editor  2 Files    3 System   4 Clock   5 Install
        #   6 Music    7 UnoAmp  8 Dostris  9 Pac-Man 10 OutLast 11 Tracker
        #  12 Paint   13 Runner 14 Browser 15 Studio  16 Photos 17 SSH
        #  18 UnoWord
        combo(q, "ctrl", "esc")
        time.sleep(0.6)
        H.keys(q, *(["down"] * 18), gap=0.06)
        time.sleep(0.4)
        H.shot(q, "uow_02_menu_on_uoword")

        H.keys(q, "ret")
        print("opening UnoWord (the module is 127 KB off vvfat)...")
        time.sleep(5.0)
        H.shot(q, "uow_03_open")

        # type into it: the whole model + layout + paint path, end to end
        H.text(q, "The quick brown fox jumps over the lazy dog. ")
        H.text(q, "Pack my box with five dozen liquor jugs. ")
        H.text(q, "How vexingly quick daft zebras jump!")
        time.sleep(0.8)
        H.shot(q, "uow_04_typed")

        # a second paragraph, so pagination and paragraph splitting are real
        H.keys(q, "ret")
        H.text(q, "A second paragraph, to prove the mark splits the text.")
        time.sleep(0.8)
        H.shot(q, "uow_05_two_paras")

        # select all and embolden: Ctrl+A then Ctrl+B
        combo(q, "ctrl", "a")
        time.sleep(0.4)
        H.shot(q, "uow_06_selected")
        combo(q, "ctrl", "b")
        time.sleep(0.8)
        H.shot(q, "uow_07_bold")

        # undo it, then redo, then justify from the toolbar
        combo(q, "ctrl", "z")
        time.sleep(0.6)
        H.shot(q, "uow_08_undo")

        # ---- the mouse paths ------------------------------------------
        # Every one of these was dead before the canvas rect came from the
        # painter instead of being reconstructed from the window frame.
        mclick(q, 387, 225)                    # the Bold button
        time.sleep(0.6)
        H.shot(q, "uow_09_click_bold")

        mclick(q, 108, 133)                    # the File menu title
        time.sleep(0.6)
        H.shot(q, "uow_10_filemenu")
        H.keys(q, "esc")
        time.sleep(0.4)

        mclick(q, 369, 133)                    # Format > Font...
        time.sleep(0.5)
        H.shot(q, "uow_11_formatmenu")
        H.keys(q, "down", "ret")
        time.sleep(1.0)
        H.shot(q, "uow_12_fontdialog")

        # drive the dialog with the mouse: tick Italic, then press OK
        mclick(q, 470, 470)
        time.sleep(0.4)
        H.shot(q, "uow_13_dialog_check")
        H.keys(q, "esc")
        time.sleep(0.5)

        mclick(q, 564, 133)                    # Help > About
        time.sleep(0.5)
        H.keys(q, "down", "ret")
        time.sleep(0.9)
        H.shot(q, "uow_14_about")
        H.keys(q, "ret")
        time.sleep(0.5)
        H.shot(q, "uow_15_final")

    finally:
        qemu.terminate()
        try:
            qemu.wait(timeout=10)
        except Exception:
            qemu.kill()


if __name__ == "__main__":
    main()
