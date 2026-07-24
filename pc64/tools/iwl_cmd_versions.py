#!/usr/bin/env python3
"""Dump the CMD_VERSIONS TLV from an Intel iwlwifi .ucode file.

Every host command has a firmware-advertised version; sending the wrong
structure version is a prime cause of a firmware wedge (the AX201 ADD_STA case:
fw wants cmd_ver 12, the driver was sending a ~44-byte v7-era struct). This maps
what the loaded firmware actually expects, so command builders can be written
version-correct instead of blind.

Layout mirrors Linux iwl-fw-file.h: TLV type 48 (IWL_UCODE_TLV_CMD_VERSIONS) is
an array of struct iwl_fw_cmd_version { u8 cmd, group, cmd_ver, notif_ver; u32
reserved }. cmd_ver / notif_ver == 99 is IWL_FW_CMD_VER_UNKNOWN, i.e. the
original unversioned command.

  python3 iwl_cmd_versions.py ../fw-blobs/IWLAX201.UCO
"""
import argparse, struct, sys

TLV_MAGIC = 0x0a4c5749
CMD_VERSIONS = 48
UNKNOWN = 99

# group ids (iwl-config.h) + a few command opcodes we care about for bring-up
GROUPS = {0x0: "LEGACY", 0x1: "LONG", 0x2: "SYSTEM", 0x3: "MACCONF",
          0x4: "DATAPATH", 0x5: "SCAN", 0xb: "LOCATION", 0xc: "REGNVM",
          0xe: "DEBUG", 0xf: "STATS"}
KNOWN = {(0x1, 0x0d): "SCAN_REQ_UMAC", (0x1, 0x18): "ADD_STA",
         (0x1, 0x08): "PHY_CONTEXT", (0x1, 0x28): "MAC_CONTEXT",
         (0x1, 0x2b): "BINDING", (0x1, 0x2c): "TIME_QUOTA",
         (0x1, 0x29): "TIME_EVENT", (0x1, 0x17): "SCAN_ABORT",
         (0x1, 0x77): "POWER_TABLE", (0x1, 0x98): "TX_ANT_CONFIG",
         (0x1, 0x0c): "SCAN_CFG", (0x3, 0xfb): "SESSION_PROT",
         (0xc, 0x00): "NVM_ACCESS", (0xc, 0x02): "NVM_GET_INFO"}

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("ucode")
    ap.add_argument("--known", action="store_true", help="only the bring-up commands in KNOWN")
    a = ap.parse_args()
    d = open(a.ucode, "rb").read()
    if len(d) < 88 or struct.unpack_from("<II", d, 0) != (0, TLV_MAGIC):
        sys.exit("%s: not a TLV ucode" % a.ucode)
    off = 8 + 64 + 4 + 4 + 8
    rows = []
    while off + 8 <= len(d):
        t, l = struct.unpack_from("<II", d, off); off += 8
        if t == CMD_VERSIONS:
            for i in range(l // 8):
                cmd, grp, cver, nver = struct.unpack_from("<4B", d, off + i * 8)[:4]
                rows.append((grp, cmd, cver, nver))
        off += (l + 3) & ~3
    if not rows:
        sys.exit("no CMD_VERSIONS TLV in %s" % a.ucode)
    print("%-9s %-4s %-16s cmd_ver notif_ver" % ("group", "cmd", "name"))
    for grp, cmd, cver, nver in sorted(rows):
        name = KNOWN.get((grp, cmd), "")
        if a.known and not name:
            continue
        cv = "orig" if cver == UNKNOWN else str(cver)
        nv = "orig" if nver == UNKNOWN else str(nver)
        print("%-9s 0x%02x %-16s %-7s %s" % (GROUPS.get(grp, "%#x" % grp), cmd, name, cv, nv))

if __name__ == "__main__":
    main()
