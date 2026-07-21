/*  UnoReconfig.cs - rewrite \STRESS.CFG on an ALREADY-flashed UnoDOS disk,
 *  without reformatting and without Windows mounting the volume.
 *
 *  Why this exists: the UnoDOS volume is an EFI System Partition (type GUID
 *  C12A7328-...), and Windows deliberately hides ESPs from Explorer - you can't
 *  just open the drive and edit STRESS.CFG to change which tests run; that needs
 *  admin + diskpart/mountvol gymnastics. But the flasher already runs elevated
 *  and already talks to the raw \\.\PHYSICALDRIVE handle, so it can edit the
 *  file directly on the medium.
 *
 *  Scope, deliberately narrow so it CANNOT corrupt a disk: it locates the one
 *  file \STRESS.CFG in the FAT32 root and overwrites its existing first cluster
 *  in place, then updates the directory entry's size. No cluster allocation, no
 *  FAT-chain edits, no directory growth. STRESS.CFG is always a few hundred
 *  bytes = one cluster, so this is always sufficient; anything it can't do
 *  safely (file absent, or a config somehow larger than one cluster) it refuses
 *  with a clear message rather than guessing.
 */
using System;
using System.IO;
using System.Text;

static class UnoReconfig
{
    /*  The STRESS.CFG key GENERATION this flasher writes.  The debug OS stamps
     *  the generation it understands into \BUILD.TXT as `cfgver: N` (written
     *  by pc64/build.sh - keep the two in sync; bump both whenever a config
     *  key is added or renamed).  Reconfigure must refuse a disk stamped older
     *  than this: the keys are just data and the INTERPRETER is the OS already
     *  on the stick, so writing e.g. `nostress` or `passes=1` to a build that
     *  predates those keys is silently ignored - the observed failure was a
     *  "disabled" stick fuzzing endlessly because its embedded OS predated the
     *  off switch.  Generation 2 = the suite model (nostress/passes/spec=/
     *  net-*) of 2026-07-21. */
    const int CFG_GENERATION = 2;

    const int SECTOR = 512;
    static readonly byte[] ESP_TYPE = new Guid("C12A7328-F81F-11D2-BA4B-00A0C93EC93B").ToByteArray();

    /* ---- little-endian helpers + whole-sector raw IO ------------------------ */
    static byte[] ReadSectors(Stream s, long lba, int count)
    {
        var b = new byte[count * SECTOR];
        s.Position = lba * SECTOR;
        int off = 0, n;
        while (off < b.Length && (n = s.Read(b, off, b.Length - off)) > 0) off += n;
        if (off < b.Length) throw new IOException("short read at LBA " + lba);
        return b;
    }
    static void WriteSectors(Stream s, long lba, byte[] b)
    {
        if (b.Length % SECTOR != 0) throw new IOException("unaligned write of " + b.Length + " bytes");
        s.Position = lba * SECTOR;
        s.Write(b, 0, b.Length);
    }
    static ushort U16(byte[] b, int o) { return (ushort)(b[o] | (b[o + 1] << 8)); }
    static uint   U32(byte[] b, int o) { return (uint)b[o] | ((uint)b[o + 1] << 8) | ((uint)b[o + 2] << 16) | ((uint)b[o + 3] << 24); }
    static ulong  U64(byte[] b, int o) { return (ulong)U32(b, o) | ((ulong)U32(b, o + 4) << 32); }
    static void   PutU32(byte[] b, int o, uint v) { b[o] = (byte)v; b[o + 1] = (byte)(v >> 8); b[o + 2] = (byte)(v >> 16); b[o + 3] = (byte)(v >> 24); }

    /* Find the UnoDOS ESP partition's first LBA from the GPT (header at LBA 1,
     * entry array wherever the header points). Returns -1 if there's no ESP. */
    static long FindEspLba(Stream s)
    {
        var hdr = ReadSectors(s, 1, 1);
        if (Encoding.ASCII.GetString(hdr, 0, 8) != "EFI PART") return -1;
        ulong entLba = U64(hdr, 72);
        uint  entCnt = U32(hdr, 80);
        uint  entSz  = U32(hdr, 84);
        if (entSz < 128 || entSz > SECTOR || entCnt == 0 || entCnt > 512) return -1;
        int  perSec = SECTOR / (int)entSz;
        int  needSec = (int)((entCnt + perSec - 1) / perSec);
        var  arr = ReadSectors(s, (long)entLba, needSec);
        for (int i = 0; i < entCnt; i++) {
            int o = (int)(i * entSz);
            if (o + 40 > arr.Length) break;
            bool esp = true;
            for (int k = 0; k < 16; k++) if (arr[o + k] != ESP_TYPE[k]) { esp = false; break; }
            ulong first = U64(arr, o + 32);
            if (esp && first >= 2) return (long)first;
        }
        return -1;
    }

    /* Follow one FAT32 chain link. */
    static uint NextClus(Stream s, long fatLba, uint clus)
    {
        long byteOff = (long)clus * 4;
        var b = ReadSectors(s, fatLba + byteOff / SECTOR, 1);
        return U32(b, (int)(byteOff % SECTOR)) & 0x0FFFFFFF;
    }

    /* 8.3 name -> the 11-byte space-padded uppercase form FAT stores. */
    static byte[] Pack83(string name)
    {
        var e = new byte[11];
        for (int i = 0; i < 11; i++) e[i] = (byte)' ';
        int dot = name.LastIndexOf('.');
        string bas = (dot < 0 ? name : name.Substring(0, dot)).ToUpperInvariant();
        string ext = (dot < 0 ? "" : name.Substring(dot + 1)).ToUpperInvariant();
        for (int i = 0; i < 8 && i < bas.Length; i++) e[i]     = (byte)bas[i];
        for (int i = 0; i < 3 && i < ext.Length; i++) e[8 + i] = (byte)ext[i];
        return e;
    }

    /* FAT32 geometry, parsed once per operation. */
    struct Geom
    {
        public long fatLba, dataLba, clusters;
        public int  secPerClus, clusBytes;
        public uint rootClus;
    }

    static string ReadGeom(Stream s, out Geom g)
    {
        g = new Geom();
        long part = FindEspLba(s);
        if (part < 0) return "No UnoDOS EFI partition on this drive - is it a UnoDOS disk?";

        var bpb = ReadSectors(s, part, 1);
        if (bpb[510] != 0x55 || bpb[511] != 0xAA) return "That partition has no FAT boot signature.";
        if (Encoding.ASCII.GetString(bpb, 82, 5) != "FAT32") return "Not a FAT32 UnoDOS volume.";
        int  secPerClus = bpb[13];
        int  reserved   = U16(bpb, 14);
        int  numFats    = bpb[16];
        uint fatSz      = U32(bpb, 36);
        uint rootClus   = U32(bpb, 44);
        if (secPerClus < 1 || reserved < 1 || numFats < 1 || fatSz < 1 || rootClus < 2)
            return "Unreadable FAT32 parameters on this disk.";
        // Total data clusters, so a directory entry's cluster number can be range-
        // checked before it steers a raw whole-disk write (a corrupt STRESS.CFG
        // entry with a bogus high-cluster word must never point the write off into
        // another partition).
        long totSec = U32(bpb, 32); if (totSec == 0) totSec = U16(bpb, 19);
        long clusters = (totSec - (reserved + (long)numFats * fatSz)) / secPerClus;
        if (clusters < 1) return "Unreadable FAT32 geometry on this disk.";

        g.secPerClus = secPerClus;
        g.clusBytes  = secPerClus * SECTOR;
        g.fatLba     = part + reserved;
        g.dataLba    = part + (long)reserved + (long)numFats * fatSz;
        g.clusters   = clusters;
        g.rootClus   = rootClus;
        return null;
    }

    /* A root-directory hit: where the entry lives (so its size can be patched)
     * and what it points at. */
    class RootEntry
    {
        public long   dirLba;          // LBA of the directory cluster holding it
        public byte[] dirClusterBuf;   // that whole cluster, as read
        public int    off;             // entry offset within the cluster
        public uint   firstClus, size;
    }

    static RootEntry FindRootEntry(Stream s, Geom g, string name)
    {
        byte[] want = Pack83(name);
        uint clus = g.rootClus;
        int guard = 100000;                     // chain-length backstop
        while (clus >= 2 && clus < 0x0FFFFFF8 && guard-- > 0) {
            long clba = g.dataLba + (long)(clus - 2) * g.secPerClus;
            var cbuf = ReadSectors(s, clba, g.secPerClus);
            for (int off = 0; off + 32 <= cbuf.Length; off += 32) {
                byte first = cbuf[off];
                if (first == 0x00) return null; // end of directory: not here
                if (first == 0xE5) continue;    // deleted
                byte attr = cbuf[off + 11];
                if ((attr & 0x0F) == 0x0F) continue;   // long-name slot
                if ((attr & 0x18) != 0) continue;      // directory or volume label
                bool match = true;
                for (int k = 0; k < 11; k++) if (cbuf[off + k] != want[k]) { match = false; break; }
                if (!match) continue;
                var e = new RootEntry();
                e.dirLba = clba; e.dirClusterBuf = cbuf; e.off = off;
                e.firstClus = (uint)U16(cbuf, off + 26) | ((uint)U16(cbuf, off + 20) << 16);
                e.size      = U32(cbuf, off + 28);
                return e;
            }
            clus = NextClus(s, g.fatLba, clus);
        }
        return null;
    }

    /* First cluster of a small root file as ASCII text, or null.  Enough for
     * BUILD.TXT (a one-cluster stamp); deliberately never follows the chain. */
    static string ReadRootText(Stream s, Geom g, string name)
    {
        var e = FindRootEntry(s, g, name);
        if (e == null || e.firstClus < 2 || e.firstClus >= g.clusters + 2) return null;
        var b = ReadSectors(s, g.dataLba + (long)(e.firstClus - 2) * g.secPerClus, g.secPerClus);
        int n = e.size < (uint)b.Length ? (int)e.size : b.Length;
        return Encoding.ASCII.GetString(b, 0, n);
    }

    /* The `cfgver: N` line of the disk's BUILD.TXT.  0 = stamped before
     * generations existed (a build too old to know the current keys). */
    static int DiskCfgGeneration(Stream s, Geom g)
    {
        string txt = ReadRootText(s, g, "BUILD.TXT");
        if (txt == null) return 0;
        foreach (var raw in txt.Split('\n')) {
            string t = raw.Trim();
            if (!t.StartsWith("cfgver:")) continue;
            int v;
            if (int.TryParse(t.Substring(7).Trim(), out v)) return v;
        }
        return 0;
    }

    /* Overwrite \STRESS.CFG on the UnoDOS volume reachable through `s`.
     * Returns null on success (with `info` set), else a human-readable reason. */
    public static string WriteStressCfg(Stream s, string content, out string info)
    {
        info = null;
        if (content == null) return "No test settings to write (turn on Developer options first).";

        Geom g;
        string gerr = ReadGeom(s, out g);
        if (gerr != null) return gerr;

        var e = FindRootEntry(s, g, "STRESS.CFG");
        if (e == null)
            return "STRESS.CFG is not on this disk (a production build has none - " +
                   "flash with Developer options to add tests).";

        // The keys are interpreted by the OS ALREADY ON THE STICK, which this
        // button deliberately does not touch - so a build that predates the
        // current keys must be refused, not configured. Writing `nostress` or
        // `passes=1` to such a build is silently ignored and the stick fuzzes
        // endlessly, looking exactly like "the setting has no effect".
        int gen = DiskCfgGeneration(s, g);
        if (gen < CFG_GENERATION)
            return "This disk's UnoDOS build is too old for the current test settings " +
                   (gen == 0 ? "(its BUILD.TXT carries no cfgver stamp)"
                             : "(disk cfgver " + gen + ", flasher writes " + CFG_GENERATION + ")") +
                   " - it would ignore keys like 'nostress'/'passes=' and run the stress " +
                   "driver endlessly.\n\nReflash the disk (Install) to update the OS first.";

        byte[] data = Encoding.ASCII.GetBytes(content);
        // The OS reads STRESS.CFG into a 512-byte buffer (511 usable), so cap here
        // to match - and it is always well under one cluster.
        if (data.Length > SECTOR - 1)
            return "The new config (" + data.Length + " B) exceeds the 511-byte STRESS.CFG limit - reflash instead.";

        uint fc = e.firstClus;
        if (fc < 2 || fc >= g.clusters + 2)
            return "STRESS.CFG points at an out-of-range cluster (" + fc +
                   ") - the disk looks corrupt; reflash instead.";
        // Refuse a multi-cluster STRESS.CFG: this only rewrites the first
        // cluster, so a longer chain would be left with orphaned tail
        // clusters. It is always one cluster in practice (config < 512 B).
        uint nxt = NextClus(s, g.fatLba, fc);
        if (nxt >= 2 && nxt < 0x0FFFFFF8)
            return "STRESS.CFG spans more than one cluster - reflash instead.";
        // overwrite the file's first (only) cluster, zero-padded
        var w = new byte[g.clusBytes];
        Array.Copy(data, w, data.Length);
        WriteSectors(s, g.dataLba + (long)(fc - 2) * g.secPerClus, w);
        // update the directory entry's size, keep it a one-cluster file
        PutU32(e.dirClusterBuf, e.off + 28, (uint)data.Length);
        WriteSectors(s, e.dirLba, e.dirClusterBuf);
        info = "Updated STRESS.CFG (" + data.Length + " bytes) on the existing UnoDOS disk.";
        return null;
    }
}
