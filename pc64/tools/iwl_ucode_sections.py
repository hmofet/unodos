#!/usr/bin/env python3
"""Parse an Intel iwlwifi .ucode TLV file and print the LMAC/UMAC/paging split.

The AX201 F12 handoff named the ucode section split and place_fw_dram() as the
prime suspect for the pre-ALIVE firmware park, on the theory that a wrong
section boundary would fail secure boot exactly like this.  It is not the cause:
run this against the same blob the driver loads and it reports the same
lmac=14 umac=15 paging=20 that iwlwifi.c logs on metal.

Section grouping mirrors iwl_pcie_get_num_sections() in Linux
pcie/ctxt-info.c: sections run in file order and are terminated by two magic
"separator" offsets, so the groups are [0..sep1) = LMAC, (sep1..sep2) = UMAC,
(sep2..] = paging.  Those separators are sentinel OFFSETS, not TLV types, which
is the part that is easy to get wrong when reading the format cold.

  python3 iwl_ucode_sections.py ../fw-blobs/IWLAX201.UCO
  python3 iwl_ucode_sections.py IWLAX201.UCO --list        # every section
"""
import argparse, struct, sys

TLV_MAGIC = 0x0a4c5749
SEC_RT, SEC_INIT, SEC_WOWLAN = 19, 20, 21
IMAGES = ((SEC_RT, "RT"), (SEC_INIT, "INIT"), (SEC_WOWLAN, "WOWLAN"))
CPU1_CPU2_SEPARATOR = 0xFFFFCCCC
PAGING_SEPARATOR = 0xAAAABBBB
SEPARATORS = (CPU1_CPU2_SEPARATOR, PAGING_SEPARATOR)

def parse(path):
    d = open(path, "rb").read()
    if len(d) < 88:
        sys.exit("%s: too short to be a TLV ucode" % path)
    zero, magic = struct.unpack_from("<II", d, 0)
    if zero != 0 or magic != TLV_MAGIC:
        sys.exit("%s: not a TLV ucode (zero=%d magic=%08x)" % (path, zero, magic))
    human = d[8:8 + 64].split(b"\0")[0].decode("ascii", "replace")
    # zero, magic, human_readable[64], ver, build, ignore[8]
    off = 8 + 64 + 4 + 4 + 8
    secs = {t: [] for t, _ in IMAGES}
    while off + 8 <= len(d):
        t, ln = struct.unpack_from("<II", d, off)
        off += 8
        if off + ln > len(d):
            break
        if t in secs:
            sec_off = struct.unpack_from("<I", d, off)[0]
            secs[t].append((sec_off, ln - 4))
        off += (ln + 3) & ~3      # TLVs are 4-byte aligned
    return human, secs

def group(secs):
    """Count sections up to the next separator, the way Linux does."""
    def count(start):
        n = 0
        while start < len(secs) and secs[start][0] not in SEPARATORS:
            start += 1; n += 1
        return n
    lmac = count(0)
    umac = count(lmac + 1)            # +1 for the LMAC/UMAC separator
    paging = count(lmac + umac + 2)   # +2 for both separators
    return lmac, umac, paging

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("ucode")
    ap.add_argument("--list", action="store_true", help="print every section")
    a = ap.parse_args()

    human, secs = parse(a.ucode)
    print("%s\n  build: %s" % (a.ucode, human))
    for t, name in IMAGES:
        s = secs[t]
        if not s:
            continue
        lmac, umac, paging = group(s)
        print("  %-6s num_sec=%-3d lmac=%d umac=%d paging=%d" %
              (name, len(s), lmac, umac, paging))
        if a.list:
            for i, (o, ln) in enumerate(s):
                tag = ("  <-- CPU1/CPU2 separator" if o == CPU1_CPU2_SEPARATOR else
                       "  <-- paging separator" if o == PAGING_SEPARATOR else "")
                print("      [%2d] offset=%08x len=%-7d%s" % (i, o, ln, tag))

if __name__ == "__main__":
    main()
