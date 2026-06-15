#!/usr/bin/env python3
"""A tiny SPC700 assembler + the UnoDOS/SNES audio driver, in Python.

ca65 does not target the SPC700, and the audio "engine" is small (a mailbox
poll loop + DSP register writes), so per snes/HANDOFF.md SS6 the driver is a
hand-written blob - but assembled from readable mnemonics here rather than
hex, with two label passes. mkdata.py imports build_spc_image() and emits the
bytes into gen_data.inc; the 65816 side (sound.inc) uploads them through the
IPL handshake and then talks to the running driver over APU ports $2140-$2143.

DSP model: 4 of the 8 voices each play one looped BRR square-wave sample
(generated below). A note = a DSP PITCH value; key-on/off per voice via the
KON/KOF registers. This is the SNES analogue of the Genesis PSG's tone
channels (PORT-SPEC SS2): Music uses one voice, the Tracker four.

Mailbox protocol (host -> SPC):
  port1 = (opcode<<4) | voice     opcode 0 = key-off, 1 = key-on
  port2 = pitch low                (key-on only)
  port3 = pitch high               (key-on only)
  port0 = token                    host bumps it; SPC echoes it back when done
The host writes args first, then the token, then waits for port0 to read back
equal - a complete round-trip ack.
"""

# ---------------------------------------------------------------- assembler
# Each entry: mnemonic -> (opcode, operand-encoding). Operand encodings:
#   ''      no operand
#   'i'     one immediate byte                         (op, #imm)
#   'd'     one direct-page byte                       (op, dp)
#   'di'    direct-page then immediate  (MOV dp,#imm: op, imm, dp)  *special*
#   'r'     one relative branch byte to a label
# Only the opcodes the driver needs are defined.
OPS = {
    "nop":      (0x00, ""),
    "mova_i":   (0xE8, "i"),    # MOV A,#imm
    "mova_d":   (0xE4, "d"),    # MOV A,dp
    "movd_a":   (0xC4, "d"),    # MOV dp,A
    "movd_i":   (0x8F, "di"),   # MOV dp,#imm   (bytes: 8F imm dp)
    "movy_i":   (0x8D, "i"),    # MOV Y,#imm
    "movx_d":   (0xF8, "d"),    # MOV X,dp
    "mova_y":   (0xDD, ""),     # MOV A,Y
    "incy":     (0xFC, ""),     # INC Y
    "decx":     (0x1D, ""),     # DEC X
    "asla":     (0x1C, ""),     # ASL A
    "ora_i":    (0x08, "i"),    # OR A,#imm
    "anda_i":   (0x28, "i"),    # AND A,#imm
    "cmpa_i":   (0x68, "i"),    # CMP A,#imm
    "cmpa_d":   (0x64, "d"),    # CMP A,dp
    "xcna":     (0x9F, ""),     # XCN A (swap nibbles)
    "beq":      (0xF0, "r"),
    "bne":      (0xD0, "r"),
    "bra":      (0x2F, "r"),
}


def assemble(program, org):
    """program: list of (label_or_None, mnemonic, *operands). Returns bytes."""
    # pass 1: addresses
    addr = org
    labels = {}
    items = []
    for entry in program:
        label, mnem = entry[0], entry[1]
        ops = list(entry[2:])
        if label:
            labels[label] = addr
        if mnem is None:               # label-only line
            continue
        opcode, enc = OPS[mnem]
        size = 1 + len(enc.replace("di", "xx"))  # 'di' is two trailing bytes
        items.append((addr, mnem, ops))
        addr += size
    # pass 2: emit
    out = bytearray()
    for at, mnem, ops in items:
        opcode, enc = OPS[mnem]
        out.append(opcode)
        if enc == "":
            pass
        elif enc == "i" or enc == "d":
            out.append(ops[0] & 0xFF)
        elif enc == "di":              # imm, dp
            out.append(ops[0] & 0xFF)  # imm
            out.append(ops[1] & 0xFF)  # dp
        elif enc == "r":
            target = labels[ops[0]]
            rel = target - (at + 2)
            assert -128 <= rel <= 127, f"branch out of range to {ops[0]}"
            out.append(rel & 0xFF)
    return bytes(out)


# ---------------------------------------------------------------- the driver
SPC_ENTRY = 0x0200          # driver code
SPC_DIR   = 0x0500          # sample directory (DSP DIR reg = page $05)
SPC_BRR   = 0x0510          # BRR sample data

# direct-page scratch
LAST, VOICE, TMP = 0x00, 0x01, 0x02
# memory-mapped registers (direct page)
DSPA, DSPD = 0xF2, 0xF3
P0, P1, P2, P3 = 0xF4, 0xF5, 0xF6, 0xF7


def driver_program():
    P = []
    a = P.append
    # ---- DSP init ----
    a((None, "movd_i", DSPA, 0x6C)); a((None, "movd_i", DSPD, 0x20))  # FLG: echo off
    a((None, "movd_i", DSPA, 0x0C)); a((None, "movd_i", DSPD, 0x7F))  # MVOL L
    a((None, "movd_i", DSPA, 0x1C)); a((None, "movd_i", DSPD, 0x7F))  # MVOL R
    a((None, "movd_i", DSPA, 0x5C)); a((None, "movd_i", DSPD, 0xFF))  # KOF all
    a((None, "movd_i", DSPA, 0x4C)); a((None, "movd_i", DSPD, 0x00))  # KON none
    a((None, "movd_i", DSPA, 0x5D)); a((None, "movd_i", DSPD, 0x05))  # DIR = $05
    # ---- per-voice init (voices 0..3): VOLL/R, ADSR1/2, SRCN=0 ----
    a((None, "movy_i", 0x00))
    a(("vinit", "mova_y"))                       # A = voice
    a((None, "asla")); a((None, "asla")); a((None, "asla")); a((None, "asla"))
    a((None, "movd_a", TMP))                     # TMP = voice<<4 (base reg)
    for off, val in ((0x00, 0x7F), (0x01, 0x7F), (0x05, 0x8F),
                     (0x06, 0xE0), (0x04, 0x00)):
        a((None, "mova_d", TMP)); a((None, "ora_i", off)); a((None, "movd_a", DSPA))
        a((None, "mova_i", val)); a((None, "movd_a", DSPD))
    a((None, "incy"))
    a((None, "mova_y")); a((None, "cmpa_i", 0x04)); a((None, "bne", "vinit"))
    # ---- mailbox loop ----
    # seed LAST from the live port0 (the IPL left its run-kick value there;
    # without this the driver would act on a phantom command at startup)
    a((None, "mova_d", P0)); a((None, "movd_a", LAST))
    a(("loop", "mova_d", P0)); a((None, "cmpa_d", LAST)); a((None, "beq", "loop"))
    a((None, "movd_a", LAST))                    # remember token
    a((None, "mova_d", P1)); a((None, "anda_i", 0x0F)); a((None, "movd_a", VOICE))
    a((None, "mova_d", P1)); a((None, "xcna")); a((None, "anda_i", 0x0F))
    a((None, "beq", "cmdoff"))                   # opcode 0 -> key off
    # ---- key on: set pitch, then KON ----
    a((None, "mova_d", VOICE))
    a((None, "asla")); a((None, "asla")); a((None, "asla")); a((None, "asla"))
    a((None, "movd_a", TMP))                     # base
    a((None, "ora_i", 0x02)); a((None, "movd_a", DSPA))
    a((None, "mova_d", P2)); a((None, "movd_a", DSPD))   # PITCHL
    a((None, "mova_d", TMP)); a((None, "ora_i", 0x03)); a((None, "movd_a", DSPA))
    a((None, "mova_d", P3)); a((None, "movd_a", DSPD))   # PITCHH
    # bit = 1<<voice
    a((None, "mova_i", 0x01)); a((None, "movx_d", VOICE)); a((None, "beq", "konset"))
    a(("kshl", "asla")); a((None, "decx")); a((None, "bne", "kshl"))
    a(("konset", "movd_a", TMP))                 # TMP = voice bit
    a((None, "movd_i", DSPA, 0x5C)); a((None, "movd_i", DSPD, 0x00))  # clear KOF
    a((None, "movd_i", DSPA, 0x4C)); a((None, "mova_d", TMP)); a((None, "movd_a", DSPD))  # KON
    a((None, "bra", "ack"))
    # ---- key off: KOF = 1<<voice ----
    a(("cmdoff", "mova_i", 0x01)); a((None, "movx_d", VOICE)); a((None, "beq", "kofset"))
    a(("fshl", "asla")); a((None, "decx")); a((None, "bne", "fshl"))
    a(("kofset", "movd_a", TMP))
    a((None, "movd_i", DSPA, 0x5C)); a((None, "mova_d", TMP)); a((None, "movd_a", DSPD))
    # ---- ack: echo token ----
    a(("ack", "mova_d", LAST)); a((None, "movd_a", P0)); a((None, "bra", "loop"))
    return P


def square_brr():
    """One looped BRR block = a square wave (16 samples: 8 high, 8 low)."""
    header = (0x0C << 4) | (0 << 2) | 0b11      # range 12, filter 0, loop+end
    return bytes([header, 0x77, 0x77, 0x77, 0x77, 0x88, 0x88, 0x88, 0x88])


def build_spc_image():
    """Return (image_bytes, load_addr, entry). One contiguous block uploaded
    in a single IPL transfer; the directory + sample sit just past the code."""
    code = assemble(driver_program(), SPC_ENTRY)
    assert SPC_ENTRY + len(code) <= SPC_DIR, f"driver too big: {len(code)} bytes"
    img = bytearray(SPC_BRR + 9 - SPC_ENTRY)
    img[0:len(code)] = code
    # sample directory entry 0: start, loop (both little-endian)
    d = SPC_DIR - SPC_ENTRY
    img[d:d + 4] = bytes([SPC_BRR & 0xFF, SPC_BRR >> 8,
                          SPC_BRR & 0xFF, SPC_BRR >> 8])
    b = SPC_BRR - SPC_ENTRY
    img[b:b + 9] = square_brr()
    return bytes(img), SPC_ENTRY, SPC_ENTRY


def note_pitches():
    """DSP PITCH per MIDI note 36..96. The BRR period is 16 samples, so the
    tone frequency = 32000*P/65536 = P/2.048 Hz, hence P = round(f*2.048)."""
    import math
    out = []
    for midi in range(36, 97):
        f = 440.0 * (2.0 ** ((midi - 69) / 12.0))
        out.append(min(0x3FFF, round(f * 2.048)))
    return out


if __name__ == "__main__":
    img, addr, entry = build_spc_image()
    print(f"SPC image {len(img)} bytes @ ${addr:04X}, entry ${entry:04X}")
    code = assemble(driver_program(), SPC_ENTRY)
    print(f"driver code {len(code)} bytes; pitches {len(note_pitches())}")
