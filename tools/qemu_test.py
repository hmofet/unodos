#!/usr/bin/env python3
"""UnoDOS headless QEMU test driver (Windows-native port of qemu_test.sh).

Usage: python tools/qemu_test.py <image> <artifact-dir> <instance-id> [< script]
Reads the same command language as qemu_test.sh from stdin:
  wait N | key X | keys a b c | type text | mousemove DX DY |
  click [btn] | btn N | dblclick | shot NAME | quit
Talks to the QEMU human monitor over a localhost TCP socket and converts
PPM screendumps to PNG (pure-Python, no netpbm needed).
"""
import os, socket, struct, subprocess, sys, time, zlib

IMG, ART, ID = sys.argv[1], sys.argv[2], (sys.argv[3] if len(sys.argv) > 3 else "0")
PORT = 45450 + (abs(hash(ID)) % 100)
os.makedirs(ART, exist_ok=True)

QEMU = os.environ.get("QEMU", r"C:\Program Files\qemu\qemu-system-i386.exe")
proc = subprocess.Popen([
    QEMU, "-M", "isapc", "-m", "640K", "-snapshot",
    "-drive", f"file={IMG},format=raw,if=floppy", "-boot", "a",
    "-display", "none", "-monitor", f"tcp:127.0.0.1:{PORT},server,nowait",
    "-rtc", "base=localtime",
])
time.sleep(2)

def mon(cmd):
    try:
        s = socket.create_connection(("127.0.0.1", PORT), timeout=5)
        s.recv(4096)  # banner + prompt
        s.sendall((cmd + "\n").encode())
        time.sleep(0.1)
        s.close()
    except OSError as e:
        print(f"monitor error: {e}", file=sys.stderr)

def ppm_to_png(ppm_path, png_path):
    with open(ppm_path, "rb") as f:
        data = f.read()
    # parse P6 header (magic, width, height, maxval), tolerate comments
    tokens, i = [], 0
    while len(tokens) < 4:
        while i < len(data) and data[i:i+1].isspace(): i += 1
        if data[i:i+1] == b"#":
            while i < len(data) and data[i] != 0x0A: i += 1
            continue
        j = i
        while j < len(data) and not data[j:j+1].isspace(): j += 1
        tokens.append(data[i:j]); i = j
    i += 1  # single whitespace after maxval
    assert tokens[0] == b"P6", "not a P6 PPM"
    w, h = int(tokens[1]), int(tokens[2])
    raw = data[i:i + w * h * 3]
    # PNG: each scanline prefixed with filter byte 0
    stride = w * 3
    scanlines = b"".join(b"\x00" + raw[y*stride:(y+1)*stride] for y in range(h))
    def chunk(tag, payload):
        c = tag + payload
        return struct.pack(">I", len(payload)) + c + struct.pack(">I", zlib.crc32(c))
    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(scanlines, 6))
           + chunk(b"IEND", b""))
    with open(png_path, "wb") as f:
        f.write(png)

# Tracked guest cursor position. The kernel boots with the cursor at
# (160,100). NOTE: do NOT use the old "mousemove -2000 -2000" homing trick -
# on QEMU 11.x the PS/2 layer retains the out-of-range remainder (and/or
# desyncs SeaBIOS's packet reassembly), leaving the pointer dead. Use
# "moveto X Y" which tracks position and sends chunked relative deltas.
cur_x, cur_y = 160, 100

for line in sys.stdin:
    parts = line.split()
    if not parts:
        continue
    cmd, rest = parts[0], parts[1:]
    if cmd == "wait":
        time.sleep(float(rest[0]))
    elif cmd == "moveto":
        tx, ty = int(rest[0]), int(rest[1])
        dx, dy = tx - cur_x, ty - cur_y
        while dx or dy:
            sx = max(-100, min(100, dx))
            sy = max(-100, min(100, dy))
            mon(f"mouse_move {sx} {sy}")
            time.sleep(0.15)
            dx -= sx; dy -= sy
        cur_x, cur_y = tx, ty
        time.sleep(0.2)
    elif cmd == "key":
        mon("sendkey " + " ".join(rest)); time.sleep(0.3)
    elif cmd == "keys":
        for k in rest:
            mon("sendkey " + k); time.sleep(0.3)
    elif cmd == "type":
        for c in " ".join(rest):
            mon("sendkey " + ("spc" if c == " " else c)); time.sleep(0.2)
    elif cmd == "mousemove":
        mon("mouse_move " + " ".join(rest)); time.sleep(0.2)
        try:
            cur_x = max(0, min(319, cur_x + int(rest[0])))
            cur_y = max(0, min(199, cur_y + int(rest[1])))
        except (ValueError, IndexError):
            pass
    elif cmd == "click":
        b = rest[0] if rest else "1"
        mon("mouse_button " + b); time.sleep(0.15); mon("mouse_button 0"); time.sleep(0.3)
    elif cmd == "btn":
        mon("mouse_button " + (rest[0] if rest else "0")); time.sleep(0.3)
    elif cmd == "dblclick":
        mon("mouse_button 1"); time.sleep(0.1); mon("mouse_button 0"); time.sleep(0.15)
        mon("mouse_button 1"); time.sleep(0.1); mon("mouse_button 0"); time.sleep(0.3)
    elif cmd == "shot":
        name = rest[0]
        ppm = os.path.abspath(os.path.join(ART, name + ".ppm"))
        mon("screendump " + ppm.replace("\\", "/"))
        time.sleep(0.8)
        if os.path.exists(ppm):
            ppm_to_png(ppm, os.path.join(ART, name + ".png"))
            os.remove(ppm)
        else:
            print(f"screendump failed: {name}", file=sys.stderr)
    elif cmd == "quit":
        break

mon("quit")
time.sleep(1)
if proc.poll() is None:
    proc.kill()
