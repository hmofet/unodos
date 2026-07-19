#!/usr/bin/env python3
"""One-shot scripted UI verification for the reworked shell: boots QEMU,
opens the Start menu / Editor (typing + Ctrl-A/Ctrl-B) / Files (two-pane) /
System, and screenshots each step into shots/v2_*.png. Keyboard-first; the
mouse (usb-tablet) is used only where a click is the natural gesture."""
import os, subprocess, sys, time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import harness as H

W, Hh = 1280, 800     # GOP mode QEMU/OVMF picks on this setup

def mmove(q, x, y):
    q.cmd("input-send-event", events=[
        {"type": "abs", "data": {"axis": "x", "value": int(x * 32767 / W)}},
        {"type": "abs", "data": {"axis": "y", "value": int(y * 32767 / Hh)}}])
    time.sleep(0.15)

def mclick(q, x, y):
    mmove(q, x, y)
    q.cmd("input-send-event", events=[{"type": "btn", "data": {"down": True,  "button": "left"}}])
    time.sleep(0.12)
    q.cmd("input-send-event", events=[{"type": "btn", "data": {"down": False, "button": "left"}}])
    time.sleep(0.25)

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
        time.sleep(20)
        H.shot(q, "v2_desktop")

        # Start menu (branded logo + rows)
        combo(q, "ctrl", "esc")
        time.sleep(0.5)
        H.shot(q, "v2_startmenu")

        # open the Editor (menu: Control=0, Editor=1)
        H.keys(q, "down", "ret")
        time.sleep(1.2)
        H.shot(q, "v2_editor")

        # type a sentence, select all, bold it
        H.text(q, "Hello from the reworked Editor. ")
        H.text(q, "Proportional text, real spacing.")
        time.sleep(0.4)
        H.shot(q, "v2_editor_typed")
        combo(q, "ctrl", "a")
        combo(q, "ctrl", "b")
        time.sleep(0.5)
        H.shot(q, "v2_editor_bold")

        # Files (menu index 2)
        combo(q, "ctrl", "esc")
        H.keys(q, "down", "down", "ret")
        time.sleep(1.0)
        H.shot(q, "v2_files")

        # two-pane toggle: '2' on the pane canvas
        H.text(q, "2")
        time.sleep(0.6)
        H.shot(q, "v2_files_two")

        # System (menu index 3)
        combo(q, "ctrl", "esc")
        H.keys(q, "down", "down", "down", "ret")
        time.sleep(1.0)
        H.shot(q, "v2_system")

        # Browser from the Start menu (verify TTF headings): it's far down the
        # list - navigate by pressing down 15x then enter is fragile; instead
        # scroll with keys: the menu order is NAPPS entries; Browser is last
        # app (index NAPPS-1 = 15).
        combo(q, "ctrl", "esc")
        H.keys(q, *(["down"] * 15), gap=0.08)
        H.keys(q, "ret")
        time.sleep(2.0)
        H.shot(q, "v2_browser")

        # open Welcome.md (Down into the list is not needed - it starts
        # selected; Enter opens) to verify TTF headings in content
        H.keys(q, "down", "ret")
        time.sleep(1.5)
        H.shot(q, "v2_browser_doc")
    finally:
        qemu.terminate()
        try:
            qemu.wait(timeout=10)
        except Exception:
            qemu.kill()

if __name__ == "__main__":
    main()
