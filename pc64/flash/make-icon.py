#!/usr/bin/env python3
"""Generate pc64/flash/unodos.ico - a little monitor with a boot cursor on the
UnoDOS blue.  Run with a Python that has Pillow.  Emits a multi-size .ico for
the exe.  (Drawn from primitives so it needs no font file.)"""
import os
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
S = 256
BLUE = (59, 111, 214, 255)      # UnoDOS "Aurora" blue (matches the shell taskbar)
SCREEN = (23, 32, 54, 255)      # dark screen
CURSOR = (120, 220, 150, 255)   # phosphor-green boot cursor

img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
d = ImageDraw.Draw(img)

# rounded blue background
d.rounded_rectangle([8, 8, S - 8, S - 8], radius=44, fill=BLUE)

# monitor bezel (white), screen (dark), stand
d.rounded_rectangle([48, 60, 208, 176], radius=14, fill=(240, 243, 250, 255))
d.rounded_rectangle([64, 76, 192, 160], radius=6, fill=SCREEN)
d.rectangle([116, 176, 140, 196], fill=(220, 226, 238, 255))          # neck
d.rounded_rectangle([88, 196, 168, 208], radius=5, fill=(240, 243, 250, 255))  # base

# a blinking boot cursor (block) + prompt underline on the screen
d.rectangle([80, 96, 100, 116], fill=CURSOR)                          # cursor block
d.rectangle([80, 132, 176, 138], fill=(90, 110, 150, 255))            # prompt line

out = os.path.join(HERE, "unodos.ico")
img.save(out, sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
print("wrote", out)
