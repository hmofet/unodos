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

    /* Overwrite \STRESS.CFG on the UnoDOS volume reachable through `s`.
     * Returns null on success (with `info` set), else a human-readable reason. */
    public static string WriteStressCfg(Stream s, string content, out string info)
    {
        info = null;
        if (content == null) return "No test settings to write (turn on Developer options first).";

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
        int  clusBytes = secPerClus * SECTOR;
        long fatLba    = part + reserved;
        long dataLba   = part + (long)reserved + (long)numFats * fatSz;
        // Total data clusters, so a directory entry's cluster number can be range-
        // checked before it steers a raw whole-disk write (a corrupt STRESS.CFG
        // entry with a bogus high-cluster word must never point the write off into
        // another partition).
        long totSec = U32(bpb, 32); if (totSec == 0) totSec = U16(bpb, 19);
        long clusters = (totSec - (reserved + (long)numFats * fatSz)) / secPerClus;
        if (clusters < 1) return "Unreadable FAT32 geometry on this disk.";

        byte[] data = Encoding.ASCII.GetBytes(content);
        // The OS reads STRESS.CFG into a 512-byte buffer (511 usable), so cap here
        // to match - and it is always well under one cluster.
        if (data.Length > SECTOR - 1)
            return "The new config (" + data.Length + " B) exceeds the 511-byte STRESS.CFG limit - reflash instead.";

        byte[] want = Pack83("STRESS.CFG");
        uint clus = rootClus;
        int guard = 100000;                     // chain-length backstop
        while (clus >= 2 && clus < 0x0FFFFFF8 && guard-- > 0) {
            long clba = dataLba + (long)(clus - 2) * secPerClus;
            var cbuf = ReadSectors(s, clba, secPerClus);
            for (int off = 0; off + 32 <= cbuf.Length; off += 32) {
                byte first = cbuf[off];
                if (first == 0x00)              // end of directory: not here
                    return "STRESS.CFG is not on this disk (a production build has none - " +
                           "flash with Developer options to add tests).";
                if (first == 0xE5) continue;    // deleted
                byte attr = cbuf[off + 11];
                if ((attr & 0x0F) == 0x0F) continue;   // long-name slot
                if ((attr & 0x18) != 0) continue;      // directory or volume label
                bool match = true;
                for (int k = 0; k < 11; k++) if (cbuf[off + k] != want[k]) { match = false; break; }
                if (!match) continue;

                uint fc = (uint)U16(cbuf, off + 26) | ((uint)U16(cbuf, off + 20) << 16);
                if (fc < 2 || fc >= clusters + 2)
                    return "STRESS.CFG points at an out-of-range cluster (" + fc +
                           ") - the disk looks corrupt; reflash instead.";
                // Refuse a multi-cluster STRESS.CFG: this only rewrites the first
                // cluster, so a longer chain would be left with orphaned tail
                // clusters. It is always one cluster in practice (config < 512 B).
                uint nxt = NextClus(s, fatLba, fc);
                if (nxt >= 2 && nxt < 0x0FFFFFF8)
                    return "STRESS.CFG spans more than one cluster - reflash instead.";
                // overwrite the file's first (only) cluster, zero-padded
                var w = new byte[clusBytes];
                Array.Copy(data, w, data.Length);
                WriteSectors(s, dataLba + (long)(fc - 2) * secPerClus, w);
                // update the directory entry's size, keep it a one-cluster file
                PutU32(cbuf, off + 28, (uint)data.Length);
                WriteSectors(s, clba, cbuf);
                info = "Updated STRESS.CFG (" + data.Length + " bytes) on the existing UnoDOS disk.";
                return null;
            }
            clus = NextClus(s, fatLba, clus);
        }
        return "Could not find STRESS.CFG in the UnoDOS root directory.";
    }
}
