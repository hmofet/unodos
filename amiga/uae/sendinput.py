#!/usr/bin/env python3
"""Focus WinUAE and inject scancode key presses via SendInput.
Usage: sendinput.py KEY [KEY...]   (KEY: enter, esc, right, left, up, down)
"""
import ctypes, ctypes.wintypes as wt, subprocess, sys, time

user32 = ctypes.windll.user32

SCAN = {  # PC set-1 scancodes (WinUAE maps these to Amiga keys)
    "enter": 0x1C, "esc": 0x01, "space": 0x39,
    "up": 0xE048, "down": 0xE050, "left": 0xE04B, "right": 0xE04D,
}

ULONG_PTR = ctypes.POINTER(ctypes.c_ulong)

class KEYBDINPUT(ctypes.Structure):
    _fields_ = [("wVk", wt.WORD), ("wScan", wt.WORD), ("dwFlags", wt.DWORD),
                ("time", wt.DWORD), ("dwExtraInfo", ctypes.c_void_p)]

class INPUT(ctypes.Structure):
    class _U(ctypes.Union):
        # pad the union to MOUSEINPUT size (32 bytes on x64) or SendInput
        # rejects the struct with ERROR_INVALID_PARAMETER
        _fields_ = [("ki", KEYBDINPUT), ("pad", ctypes.c_byte * 32)]
    _anonymous_ = ("u",)
    _fields_ = [("type", wt.DWORD), ("u", _U)]

KEYEVENTF_SCANCODE = 0x8
KEYEVENTF_KEYUP = 0x2
KEYEVENTF_EXTENDEDKEY = 0x1

def send_scan(scan, up=False):
    flags = KEYEVENTF_SCANCODE | (KEYEVENTF_KEYUP if up else 0)
    if scan & 0xE000:
        flags |= KEYEVENTF_EXTENDEDKEY
        scan &= 0xFF
    inp = INPUT(type=1)
    inp.ki = KEYBDINPUT(0, scan, flags, 0, None)
    n = user32.SendInput(1, ctypes.byref(inp), ctypes.sizeof(INPUT))
    if n != 1:
        print(f"SendInput failed: {ctypes.GetLastError()}", file=sys.stderr)

def focus_winuae():
    # find window by title substring
    target = []
    @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    def cb(hwnd, lp):
        buf = ctypes.create_unicode_buffer(256)
        user32.GetWindowTextW(hwnd, buf, 256)
        if "WinUAE" in buf.value:
            target.append(hwnd)
        return True
    user32.EnumWindows(cb, 0)
    if not target:
        sys.exit("WinUAE window not found")
    user32.SetForegroundWindow(target[0])
    time.sleep(0.4)

focus_winuae()
for key in sys.argv[1:]:
    if key.startswith("wait"):
        time.sleep(float(key[4:] or 1))
        continue
    sc = SCAN[key.lower()]
    send_scan(sc, False)
    time.sleep(0.08)
    send_scan(sc, True)
    time.sleep(0.35)
print("ok")
