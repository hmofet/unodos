#!/usr/bin/env python3
"""ROM-free Cosmo Communicator (MediaTek MT6771, AArch64) test harness for UnoDOS/cosmo.

The Cosmo port adopts Planet LK's live framebuffer instead of bringing up a display,
so this harness is the simplest of the AArch64 ports: it plays LK. It

  * maps DRAM (payload + stack + vars + a stand-in panel framebuffer) and a sink page
    over the TOPRGU watchdog (the payload's first act is to disable it),
  * builds a REAL flattened device tree and hands it to the payload in x0, carrying
    LK's framebuffer handoff in /chosen, so fb_init's videolfb walk is exercised
    before the image ever reaches hardware,
  * also pre-seeds FBINFO with a panel framebuffer base + pitch, the older path that
    fb_init still falls back to when the tree carries no framebuffer,
  * runs the real payload for an instruction budget (cntpct_el0 advances on its own in
    Unicorn, so wait_vblank returns one frame per loop and the AUTOTEST pad plays out),
  * reads back the centred draw origin fb_init computed and renders the 640x480 UI to
    a PNG (the UI is centred in LK's 1080x2160 panel-native framebuffer),
  * checks that fb_init took the framebuffer from where this run intended, and exits
    non-zero if it did not. That check is the point of the FDT modes below.

Usage: python cosmo/harness.py <unodos.bin> <out.png> [instr_millions] [options]

  --fdt=blob    (default) /chosen carries "atag,videolfb", the packed little-endian
                blob a PRODUCTION LK emits (target_atag_videolfb). This is the shape
                the Cosmo will actually hand us.
  --fdt=props   /chosen carries the big-endian "atag,videolfb-fb_base_h/_l/-vramSize"
                triple instead — the MACH_FPGA_NO_DISPLAY shape documented in
                COSMO-BRINGUP.md (mt_disp_config_frame_buffer).
  --fdt=both    both forms, with the blob pointing somewhere else: proves the split
                properties win.
  --fdt=empty   a valid tree with no videolfb at all: proves we fall back.
  --fdt=none    x0 points at plain DRAM, no FDT magic: the pre-seed-only path.
  --no-preseed  leave FBINFO zeroed, so only the device tree can supply a base.
  --junk-fbinfo fill FBINFO with plausible-looking garbage instead of seeding it: the
                device hands us uninitialised DRAM there, and fb_init must ignore it.
"""
import sys, struct, zlib, os
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_PROT_ALL, UC_HOOK_MEM_UNMAPPED
from unicorn.arm64_const import UC_ARM64_REG_SP, UC_ARM64_REG_PC, UC_ARM64_REG_X0

W, H = 640, 480                       # SCRW x SCRH (the UI surface)
# LK's framebuffer is the panel's NATIVE PORTRAIT orientation with an aligned stride:
# src_pitch = ALIGN_TO(DISP_GetScreenWidth(), 32) * 4 = ALIGN_TO(1080, 32) * 4 = 4352
# [LK-SRC platform/mt6771/mt_disp_drv.c:489]. The 270-degree rotation to landscape is
# applied later by the Android kernel, not by LK. See kernel.s for the full derivation.
PANEL_W, PANEL_H = 1080, 2160
PANEL_PITCH = 4352
VRAM_SIZE = 0x1F90000                 # DISP_GetVRamSize() for this panel (~31.6 MB)
LOAD    = 0x40080000
DRAM    = 0x40000000
DRAM_SZ = 0x02000000                  # 32 MB: payload + stack + vars + panel FBs
FBINFO  = 0x40320000                  # fb_base(8) fb_pitch(4) .. fb_src(40) stage(44)
FDT_AT  = 0x40350000                  # where this harness parks the device tree
COSMO_FB    = 0x7E070000              # kernel.s's last-resort guess (0x80000000-vram)
PANEL_FB    = 0x40400000              # stand-in FB used by the FBINFO pre-seed path
DTB_FB      = 0x40E00000              # stand-in FB advertised through the device tree
DECOY_FB    = 0x41400000              # what --fdt=both puts in the losing blob
WDT_PAGE = 0x10007000                 # TOPRGU watchdog (payload writes the disable key)

# FB_SRC_* in kernel.s — where fb_init says it got the base.
FB_SRC = {0: "fallback (COSMO_FB)", 1: "DTB atag,videolfb blob",
          2: "DTB atag,videolfb-fb_base_l", 3: "FBINFO pre-seed"}
BCN_MAGIC = 0x554E4F31                # "UNO1"
FB_SEED_MAGIC = 0x53454544            # "SEED" - marks FBINFO as deliberately seeded
BCN_MAIN = 4                          # the launcher drew and the main loop was entered


def sink_read(uc, offset, size, ud):
    return 0


def sink_write(uc, offset, size, value, ud):
    pass


# ---------------------------------------------------------------------------
# a minimal but real flattened device tree
# ---------------------------------------------------------------------------
FDT_BEGIN_NODE, FDT_END_NODE, FDT_PROP, FDT_END = 1, 2, 3, 9


class Fdt:
    """Just enough FDT writer to exercise the payload's walk (v17, no memrsv entries)."""

    def __init__(self):
        self.struct = bytearray()
        self.strings = bytearray()

    def _strid(self, name):
        key = name.encode() + b"\0"
        at = self.strings.find(key)
        if at < 0:
            at = len(self.strings)
            self.strings += key
        return at

    @staticmethod
    def _pad(b):
        return bytes(b) + b"\0" * (-len(b) % 4)

    def begin(self, name):
        self.struct += struct.pack(">I", FDT_BEGIN_NODE) + self._pad(name.encode() + b"\0")

    def end(self):
        self.struct += struct.pack(">I", FDT_END_NODE)

    def prop(self, name, data):
        self.struct += struct.pack(">III", FDT_PROP, len(data), self._strid(name))
        self.struct += self._pad(data)

    def prop_be32(self, name, value):
        self.prop(name, struct.pack(">I", value))

    def build(self):
        body = bytes(self.struct) + struct.pack(">I", FDT_END)
        hdr_len = 40
        off_rsv = hdr_len
        off_struct = off_rsv + 16                    # one terminating reserve entry
        off_strings = off_struct + len(body)
        total = off_strings + len(self.strings)
        hdr = struct.pack(">IIIIIIIIII", 0xD00DFEED, total, off_struct, off_strings,
                          off_rsv, 17, 16, 0, len(self.strings), len(body))
        return hdr + b"\0" * 16 + body + bytes(self.strings)


def videolfb_blob(fb_base, vram):
    """The production LK blob: target_atag_videolfb writes these fields with plain
    stores, so they are NATIVE little-endian inside a big-endian FDT property."""
    return (struct.pack("<Q", fb_base) +          # fb_addr_pa_k
            struct.pack("<III", 1, 6000, vram) +  # islcmfound, fps, vramSize
            b"aeon_nt36672_fhd_dsi_vdo_x600_xinli\0")


def build_fdt(mode):
    """Returns (bytes, expected FB_SRC, expected base, expected vramSize)."""
    f = Fdt()
    f.begin("")
    f.prop_be32("#address-cells", 2)
    f.prop_be32("#size-cells", 2)
    # A node BEFORE /chosen with a decoy property, so a walk that cannot skip nodes
    # or that prefix-matches property names fails here rather than on the device.
    f.begin("memory@40000000")
    f.prop("device_type", b"memory\0")
    f.prop("atag,videolfb-not-really", struct.pack(">I", 0xDEADBEEF))
    f.end()
    f.begin("chosen")
    f.prop("bootargs", b"console=ttyMT0,921600n1\0")
    if mode in ("blob", "both"):
        f.prop("atag,videolfb",
               videolfb_blob(DECOY_FB if mode == "both" else DTB_FB, VRAM_SIZE))
    if mode in ("props", "both"):
        f.prop_be32("atag,videolfb-fb_base_h", 0)
        f.prop_be32("atag,videolfb-fb_base_l", DTB_FB)
        f.prop_be32("atag,videolfb-vramSize", VRAM_SIZE)
    f.end()
    f.end()
    blob = f.build()
    if mode == "empty":
        return blob, None, None, 0
    if mode == "blob":
        return blob, 1, DTB_FB, VRAM_SIZE
    return blob, 2, DTB_FB, VRAM_SIZE      # props, both


def write_png(path, w, h, rgb):
    raw = bytearray()
    row = w * 3
    for y in range(h):
        raw.append(0)
        raw += rgb[y * row:(y + 1) * row]

    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d))
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(bytes(raw), 6)))
        f.write(chunk(b"IEND", b""))


def main():
    opts = [a for a in sys.argv if a.startswith("--")]
    argv = [a for a in sys.argv if not a.startswith("--")]
    rom_path, out_path = argv[1], argv[2]
    budget = int(float(argv[3]) * 1_000_000) if len(argv) > 3 else 60_000_000
    mode = "blob"
    for o in opts:
        if o.startswith("--fdt="):
            mode = o.split("=", 1)[1]
    preseed = "--no-preseed" not in opts
    junk = "--junk-fbinfo" in opts
    if junk:
        preseed = False
    if mode not in ("blob", "props", "both", "empty", "none"):
        sys.exit("unknown --fdt mode: %s" % mode)

    data = open(rom_path, "rb").read()
    uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    uc.mem_map(DRAM, DRAM_SZ, UC_PROT_ALL)
    uc.mmio_map(WDT_PAGE & ~0xFFF, 0x1000, sink_read, None, sink_write, None)
    uc.mem_write(LOAD, data)
    uc.reg_write(UC_ARM64_REG_SP, 0x40200000)

    # Play LK. Two handoff channels, and which one fb_init should believe:
    #   the device tree in x0 (what the real LK does), then FBINFO (the older path).
    uc.mem_write(FBINFO, b"\0" * 64)
    if junk:
        # No FB_SEED_MAGIC: a base and stride that would look fine to a credulous
        # fb_init, so this run fails loudly if the magic gate ever stops working.
        uc.mem_write(FBINFO, struct.pack("<QI", 0x41800000, 6144))
    if mode == "none":
        uc.reg_write(UC_ARM64_REG_X0, DRAM)      # no FDT magic here
        want_src, want_base, want_vram = None, None, 0
    else:
        fdt, want_src, want_base, want_vram = build_fdt(mode)
        uc.mem_write(FDT_AT, fdt)
        uc.reg_write(UC_ARM64_REG_X0, FDT_AT)
    if preseed:
        # fb_init ignores fb_base/fb_pitch without the magic, because on the device
        # FBINFO is uninitialised DRAM and garbage there must not become an address.
        uc.mem_write(FBINFO, struct.pack("<QI", PANEL_FB, PANEL_PITCH))
        uc.mem_write(FBINFO + 52, struct.pack("<I", FB_SEED_MAGIC))
    if want_src is None:                          # empty / none: expect the fallback
        want_src, want_base = (3, PANEL_FB) if preseed else (0, COSMO_FB)
        want_vram = 0
    if want_src == 0:
        # The last-resort guess points at real DRAM near the top of the Cosmo's map,
        # nowhere near this harness's window; map it so that path can run at all.
        uc.mem_map(COSMO_FB, 0x80000000 - COSMO_FB, UC_PROT_ALL)

    def on_unmapped(uc, access, address, size, value, ud):
        print("  !! unmapped access @ 0x%X (size %d) pc=0x%X"
              % (address, size, uc.reg_read(UC_ARM64_REG_PC)))
        return False
    uc.hook_add(UC_HOOK_MEM_UNMAPPED, on_unmapped)

    CHUNK = 4_000_000
    pc, ran = LOAD, 0
    while ran < budget:
        try:
            uc.emu_start(pc, DRAM + DRAM_SZ, count=CHUNK)
        except Exception as e:
            print("  (stopped at ~%dM: %s)" % (ran // 1_000_000, e))
            break
        pc = uc.reg_read(UC_ARM64_REG_PC)
        ran += CHUNK

    # fb_init wrote the centred draw origin + panel stride back into FBINFO, along
    # with the raw base, the vramSize it read and which source it believed.
    fb_base, fb_pitch = struct.unpack("<QI", bytes(uc.mem_read(FBINFO, 12)))
    vram, = struct.unpack("<I", bytes(uc.mem_read(FBINFO + 24, 4)))
    raw, = struct.unpack("<Q", bytes(uc.mem_read(FBINFO + 32, 8)))
    src, stage, magic = struct.unpack("<III", bytes(uc.mem_read(FBINFO + 40, 12)))

    rgb = bytearray(W * H * 3)
    for y in range(H):
        rowpx = uc.mem_read(fb_base + y * fb_pitch, W * 4)
        for x in range(W):
            w = rowpx[x*4] | (rowpx[x*4+1] << 8) | (rowpx[x*4+2] << 16)
            o = (y * W + x) * 3
            rgb[o]   = (w >> 16) & 0xFF
            rgb[o+1] = (w >> 8) & 0xFF
            rgb[o+2] = w & 0xFF
    write_png(out_path, W, H, rgb)
    print("wrote %s (%dx%d) after ~%dM instrs" % (out_path, W, H, ran // 1_000_000))
    print("  fdt=%-5s preseed=%-5s -> fb %s = 0x%X, vramSize 0x%X, draw origin 0x%X"
          " pitch %d" % (mode, preseed, FB_SRC.get(src, "?%d" % src), raw, vram,
                         fb_base, fb_pitch))

    fails = []
    if src != want_src:
        fails.append("framebuffer source: got %s, wanted %s"
                     % (FB_SRC.get(src, src), FB_SRC.get(want_src, want_src)))
    if raw != want_base:
        fails.append("framebuffer base: got 0x%X, wanted 0x%X" % (raw, want_base))
    if vram != want_vram:
        fails.append("vramSize: got 0x%X, wanted 0x%X" % (vram, want_vram))
    bar, = struct.unpack("<I", bytes(uc.mem_read(raw, 4)))
    if bar != 0xFFFFFFFF:
        fails.append("bar beacon: framebuffer starts 0x%08X, wanted white 0xFFFFFFFF" % bar)
    if magic != BCN_MAGIC:
        fails.append("beacon magic: got 0x%X, wanted 0x%X" % (magic, BCN_MAGIC))
    elif stage != BCN_MAIN:
        fails.append("beacon stage: reached %d, wanted %d (main loop)" % (stage, BCN_MAIN))
    if fails:
        for f in fails:
            print("  FAIL: %s" % f)
        sys.exit(1)
    print("  OK: videolfb walk, framebuffer adoption and beacon all as expected")


if __name__ == "__main__":
    main()
