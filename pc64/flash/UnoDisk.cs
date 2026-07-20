/*  UnoDisk.cs - build a whole-disk GPT + FAT32 volume and populate it.
 *
 *  The flasher used to clone a fixed-size raw image, which left the rest of a
 *  USB stick unallocated: write the 512 MiB release image to a 32 GB stick and
 *  31.5 GB is dead space.  This builds the volume to fit the target instead -
 *  GPT, one EFI System Partition from LBA 2048 to the last usable sector,
 *  formatted FAT32 - then copies files into it from any number of payload
 *  sources (the embedded ESP tree, a media/test kit folder, a chosen .zip).
 *
 *  Nothing here touches Windows: it formats and writes the filesystem itself,
 *  sector by sector, so it does not need the ESP mounted or lettered.  That
 *  also means the same code builds an image file for testing (see
 *  flash/UnoDiskTest.cs) as builds a physical disk.
 *
 *  Layout rules that are load-bearing, and why:
 *    * 512-byte sectors only - pc64/fat.c rejects anything else outright.
 *    * two FATs, reserved=32, FSInfo at 1, backup boot at 6.  The EFI FAT spec
 *      says NumFATs "should always be 2"; a boot volume is the wrong place to
 *      find out whose firmware reads "should" as "must".
 *    * cluster size from Microsoft's DSKSZTOSECPERCLUS table, then stepped down
 *      if that would leave under 65525 clusters (which would make it a FAT16
 *      volume wearing a FAT32 BPB, and fat.c classifies by cluster count).
 *    * partition type EF00 (EFI System) - firmware finds BOOTX64.EFI by
 *      scanning GPT for it, so a "basic data" partition would not boot.
 */
using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Compression;
using System.Text;

/* ---- one file or directory to place on the volume -------------------------- */
class PayloadItem
{
    public string Path;              // relative, '\' or '/' separated
    public bool   IsDir;
    public long   Size;
    public DateTime Time = DateTime.Now;
    public Func<Stream> Open;        // null for directories
}

// Disposable because a zip source must hold its archive open until the last
// file has been streamed onto the disk; UnoDisk.Build closes them when it is
// done, which also releases the lock on a user-chosen .zip.
interface IPayloadSource : IDisposable
{
    string Describe();
    IEnumerable<PayloadItem> Items();
}

/* Every entry of a .zip, from a file path or an already-open seekable stream
 * (the embedded ESP resource takes the stream form). */
class ZipPayload : IPayloadSource
{
    readonly string path; readonly Stream given; readonly string prefix;
    Stream stream; ZipArchive zip;
    public ZipPayload(string zipPath, string destPrefix) { path = zipPath; prefix = destPrefix; }
    public ZipPayload(Stream s, string destPrefix)       { given = s;      prefix = destPrefix; }

    public string Describe()
    {
        return (path != null ? System.IO.Path.GetFileName(path) : "embedded UnoDOS system files")
             + (string.IsNullOrEmpty(prefix) ? "" : " -> \\" + prefix);
    }

    public void Dispose()
    {
        if (zip != null) { zip.Dispose(); zip = null; }
        if (stream != null && given == null) { stream.Dispose(); }
        stream = null;
    }

    public IEnumerable<PayloadItem> Items()
    {
        stream = given ?? File.OpenRead(path);
        zip = new ZipArchive(stream, ZipArchiveMode.Read, true);
        foreach (var e in zip.Entries) {
            string rel = e.FullName.Replace('/', '\\');
            if (rel.Length == 0) continue;
            if (rel.EndsWith("\\")) {                      // explicit directory entry
                yield return new PayloadItem { Path = Join(prefix, rel.TrimEnd('\\')), IsDir = true };
                continue;
            }
            var entry = e;                                  // capture for the closure
            yield return new PayloadItem {
                Path = Join(prefix, rel), Size = entry.Length,
                Time = entry.LastWriteTime.LocalDateTime,
                Open = () => entry.Open() };
        }
    }

    internal static string Join(string a, string b)
    {
        a = (a ?? "").Trim('\\', '/', ' ');
        return a.Length == 0 ? b : a + "\\" + b;
    }
}

/* A folder tree copied as-is (the media/test kit). */
class FolderPayload : IPayloadSource
{
    readonly string root, prefix;
    public FolderPayload(string folder, string destPrefix) { root = folder; prefix = destPrefix; }
    public void Dispose() { }                      // nothing held open
    public string Describe()
    {
        return System.IO.Path.GetFileName(root.TrimEnd('\\', '/')) + "\\"
             + (string.IsNullOrEmpty(prefix) ? "" : " -> \\" + prefix);
    }
    public IEnumerable<PayloadItem> Items()
    {
        int cut = root.TrimEnd('\\', '/').Length + 1;
        foreach (var d in Directory.GetDirectories(root, "*", SearchOption.AllDirectories))
            yield return new PayloadItem { Path = ZipPayload.Join(prefix, d.Substring(cut)), IsDir = true };
        foreach (var f in Directory.GetFiles(root, "*", SearchOption.AllDirectories)) {
            var fi = new FileInfo(f);
            yield return new PayloadItem {
                Path = ZipPayload.Join(prefix, f.Substring(cut)), Size = fi.Length,
                Time = fi.LastWriteTime, Open = () => File.OpenRead(f) };
        }
    }
}

/* ---- the builder ----------------------------------------------------------- */
static class UnoDisk
{
    public const int  SECTOR      = 512;
    public const long PART_START  = 2048;         // 1 MiB aligned
    const int  RESERVED    = 32;                  // boot, FSInfo, backup boot @6
    const int  NUM_FATS    = 2;
    const int  GPT_ENTRIES = 128;
    const int  GPT_ENTSZ   = 128;
    const long MAX_SECTORS = 0xFFFFFFFFL;         // FAT32 total-sector field is 32-bit
    public const long MIN_DISK_BYTES = 96L * 1024 * 1024;

    static readonly byte[] ESP_TYPE = new Guid("C12A7328-F81F-11D2-BA4B-00A0C93EC93B").ToByteArray();

    public delegate void Progress(string stage, long done, long total);

    /* ---- node tree ---------------------------------------------------------- */
    class Node
    {
        public string Name;
        public bool   IsDir;
        public long   Size;
        public DateTime Time = DateTime.Now;
        public Func<Stream> Open;
        public readonly List<Node> Kids = new List<Node>();
        public readonly Dictionary<string, Node> Index =
            new Dictionary<string, Node>(StringComparer.OrdinalIgnoreCase);
        public uint FirstCluster;
        public uint ParentCluster;   // for ".."; 0 when the parent is the root
        public int  Clusters;
        public string ShortName;     // 11-byte packed 8.3
        public byte  NtFlags;        // 0x08 base lowercase, 0x10 extension lowercase
        public bool  NeedsLfn;
    }

    /* ---- geometry ----------------------------------------------------------- */
    // Microsoft's DSKSZTOSECPERCLUS (fatgen103), then stepped down until the
    // volume has the >=65525 clusters that make it genuinely FAT32.
    static int SectorsPerCluster(long volSectors)
    {
        int spc = volSectors <=    532480 ? 1
                : volSectors <=  16777216 ? 8
                : volSectors <=  33554432 ? 16
                : volSectors <=  67108864 ? 32 : 64;
        while (spc > 1 && ClustersFor(volSectors, spc, FatSizeFor(volSectors, spc)) < 65525)
            spc /= 2;
        return spc;
    }

    static long ClustersFor(long volSectors, int spc, long fatSize)
    {
        long data = volSectors - RESERVED - NUM_FATS * fatSize;
        return data <= 0 ? 0 : data / spc;
    }

    // Exact rather than fatgen's approximation: pick the smallest FAT that can
    // still address every cluster the remaining space yields.
    static long FatSizeFor(long volSectors, int spc)
    {
        long lo = 1, hi = volSectors / NUM_FATS + 1;
        while (lo < hi) {
            long mid = (lo + hi) / 2;
            long clusters = ClustersFor(volSectors, spc, mid);
            if (mid * (SECTOR / 4) >= clusters + 2) hi = mid; else lo = mid + 1;
        }
        return lo;
    }

    /* ---- entry point -------------------------------------------------------- */
    // Closes every payload source before returning (a zip must stay open until
    // its last file is on the disk, so the caller cannot do it any earlier).
    public static string Build(Stream disk, long diskBytes, IEnumerable<IPayloadSource> sources,
                               string label, Progress report)
    {
        try { return BuildInner(disk, diskBytes, sources, label, report); }
        finally {
            foreach (var s in sources) { try { s.Dispose(); } catch { } }
        }
    }

    static string BuildInner(Stream disk, long diskBytes, IEnumerable<IPayloadSource> sources,
                             string label, Progress report)
    {
        if (diskBytes < MIN_DISK_BYTES)
            throw new IOException("The drive is too small - UnoDOS needs at least "
                                  + (MIN_DISK_BYTES / (1024 * 1024)) + " MB.");

        // The GPT must describe the REAL disk - its backup header belongs at the
        // true last LBA.  Only the FAT32 volume is capped at 32 bits' worth of
        // sectors (2 TiB), so on a larger disk the partition stops early rather
        // than the partition table landing in the middle of the device.
        long diskSectors = diskBytes / SECTOR;
        long lastLba     = diskSectors - 1;
        long gptTail     = 1 + (long)GPT_ENTRIES * GPT_ENTSZ / SECTOR;   // backup header + array
        long lastUsable  = lastLba - gptTail;
        // End the partition on a 1 MiB boundary: costs at most 1 MiB and keeps
        // both the start and the length aligned to flash erase blocks.
        long partLast    = ((lastUsable + 1) / PART_START) * PART_START - 1;   // inclusive
        if (partLast - PART_START + 1 > MAX_SECTORS)
            partLast = ((PART_START + MAX_SECTORS) / PART_START) * PART_START - 1;
        long volSectors  = partLast - PART_START + 1;
        if (volSectors < MIN_DISK_BYTES / SECTOR)
            throw new IOException("The drive is too small once the partition table is accounted for.");

        int  spc      = SectorsPerCluster(volSectors);
        long fatSize  = FatSizeFor(volSectors, spc);
        long clusters = ClustersFor(volSectors, spc, fatSize);
        if (clusters < 65525)
            throw new IOException("Cannot lay out a FAT32 volume on a drive this small.");
        if (clusters > 0x0FFFFFF5) throw new IOException("Drive too large for a single FAT32 volume.");

        long fat0     = PART_START + RESERVED;
        long dataLba  = fat0 + NUM_FATS * fatSize;
        int  clusBytes = spc * SECTOR;

        report("Preparing the drive", 0, 1);
        var w = new SectorWriter(disk);

        // 1. wipe the front (stale MBR/GPT/superblocks) and the whole FAT region,
        //    so every cluster we never touch reads as free rather than as whatever
        //    the previous filesystem left behind.
        w.Zero(0, PART_START);
        // ...and the BACKUP GPT at the far end, in the same breath.  Re-flashing a
        //    stick that already held UnoDOS would otherwise leave a valid backup
        //    table describing this very layout: if the build then fails, Windows
        //    sees "primary GPT corrupt, backup good", offers to restore it, and
        //    the user ends up with a mountable, bootable-looking drive holding a
        //    half-written filesystem - exactly what writing the GPT last prevents.
        w.Zero(lastLba - gptTail + 1, gptTail);
        w.Zero(PART_START, dataLba - PART_START);

        // 2. lay out the file tree and hand out clusters
        var root = new Node { IsDir = true, Name = "" };
        var descriptions = new List<string>();
        foreach (var src in sources) {
            descriptions.Add(src.Describe());
            foreach (var item in src.Items()) Place(root, item);
        }

        uint next = 2;                                    // cluster 2 = root dir
        long totalBytes = Allocate(root, clusBytes, ref next, true);
        long used = next - 2;
        if (used > clusters)
            throw new IOException("The selected files need "
                + (used * (long)clusBytes / (1024 * 1024)) + " MB but the drive holds "
                + (clusters * (long)clusBytes / (1024 * 1024)) + " MB.");

        // 3. the FAT itself.  Only the clusters we allocated are non-zero, and the
        //    region is already zeroed on the medium, so one short buffer covers it.
        report("Writing the file table", 0, 1);
        var fat = new byte[Math.Max(1, ((used + 2) * 4 + SECTOR - 1) / SECTOR) * SECTOR];
        PutU32(fat, 0, 0x0FFFFFF8);                       // media descriptor
        PutU32(fat, 4, 0x0FFFFFFF);                       // clean-shutdown / no hard error
        ChainAll(root, fat);
        for (int i = 0; i < NUM_FATS; i++) w.Write(fat0 + i * fatSize, fat, 0, fat.Length);

        // 4. boot sector, FSInfo and their backups at sector 6/7
        uint serial = (uint)DateTime.Now.Ticks;
        byte[] boot = BootSector(volSectors, spc, fatSize, serial, label);
        byte[] fsi  = FsInfo(clusters - used);
        w.Write(PART_START + 0, boot, 0, SECTOR);
        w.Write(PART_START + 1, fsi,  0, SECTOR);
        w.Write(PART_START + 6, boot, 0, SECTOR);
        w.Write(PART_START + 7, fsi,  0, SECTOR);

        // 5. directories and file data
        long done = 0;
        WriteTree(root, true, w, dataLba, spc, clusBytes, label, report, ref done, totalBytes);

        // 6. GPT last: until the partition table lands the volume is invisible, so
        //    an interrupted run leaves an unpartitioned drive rather than a
        //    bootable-looking one with a half-written filesystem.
        report("Writing the partition table", 0, 1);
        WriteGpt(w, diskSectors, PART_START, partLast, lastUsable);

        w.Flush();
        return string.Format("FAT32 on {0} MB, {1} clusters of {2}, {3} MB of files",
                             volSectors / 2048, clusters,
                             clusBytes >= 1024 ? (clusBytes / 1024) + " KB" : clusBytes + " B",
                             totalBytes / (1024 * 1024));
    }

    /* ---- tree building ------------------------------------------------------ */
    static void Place(Node root, PayloadItem item)
    {
        string[] parts = item.Path.Replace('/', '\\').Split('\\');
        Node cur = root;
        for (int i = 0; i < parts.Length; i++) {
            string name = parts[i].Trim();
            if (name.Length == 0 || name == ".") continue;
            if (name == "..") throw new IOException("Unsafe path in payload: " + item.Path);
            // An LFN carries at most 255 characters; past 63 slots the sequence
            // number wraps into the 0x40 "last entry" flag and the directory
            // becomes unparseable, so refuse rather than write it malformed.
            if (name.Length > 255)
                throw new IOException("Name too long for FAT: " + name.Substring(0, 40) + "...");
            bool leaf = i == parts.Length - 1;
            Node kid;
            if (!cur.Index.TryGetValue(name, out kid)) {
                kid = new Node { Name = name, IsDir = !leaf || item.IsDir };
                cur.Index[name] = kid; cur.Kids.Add(kid);
            }
            // Two payload sources disagreeing about whether a path is a file or a
            // folder used to silently orphan the whole subtree - the files kept
            // cluster 0 and were never written, on a drive reported as installed
            // successfully.  Say so instead.
            bool wantDir = !leaf || item.IsDir;
            if (kid.IsDir != wantDir || (wantDir && kid.Open != null))
                throw new IOException("Payloads disagree - '" + name +
                                      "' appears as both a file and a folder (" + item.Path + ")");
            if (leaf && !item.IsDir) {
                kid.Size = item.Size; kid.Open = item.Open; kid.Time = item.Time;
            }
            cur = kid;
        }
    }

    // Depth-first cluster assignment.  A directory's size depends only on its
    // children's names (how many LFN slots each needs), never on their clusters,
    // so one pass can size and allocate together.
    static long Allocate(Node dir, int clusBytes, ref uint next, bool isRoot)
    {
        AssignShortNames(dir);
        int slots = isRoot ? 1 : 2;                        // volume label, or "." and ".."
        foreach (var k in dir.Kids) slots += 1 + (k.NeedsLfn ? LfnSlots(k.Name) : 0);
        dir.Clusters = Math.Max(1, (slots * 32 + clusBytes - 1) / clusBytes);
        dir.FirstCluster = next; next += (uint)dir.Clusters;

        long bytes = 0;
        foreach (var k in dir.Kids) {
            if (k.IsDir) {
                // FAT requires ".." to read 0 when the parent IS the root, not
                // the root's actual cluster number.
                k.ParentCluster = isRoot ? 0 : dir.FirstCluster;
                bytes += Allocate(k, clusBytes, ref next, false);
                continue;
            }
            // FAT32 stores a file length in 32 bits; silently truncating a
            // too-big file would produce a drive that looks fine and isn't.
            if (k.Size > 0xFFFFFFFFL)
                throw new IOException(k.Name + " is larger than 4 GB, which FAT32 cannot store.");
            k.Clusters = (int)((k.Size + clusBytes - 1) / clusBytes);
            k.FirstCluster = k.Clusters > 0 ? next : 0;
            next += (uint)k.Clusters;
            bytes += k.Size;
        }
        return bytes;
    }

    static void ChainAll(Node n, byte[] fat)
    {
        if (n.Clusters > 0) {
            for (int i = 0; i < n.Clusters; i++) {
                uint c = n.FirstCluster + (uint)i;
                PutU32(fat, (int)c * 4, i == n.Clusters - 1 ? 0x0FFFFFFF : c + 1);
            }
        }
        foreach (var k in n.Kids) ChainAll(k, fat);
    }

    static void WriteTree(Node dir, bool isRoot, SectorWriter w, long dataLba, int spc,
                          int clusBytes, string label, Progress report, ref long done, long total)
    {
        byte[] table = BuildDirectory(dir, isRoot ? label : null, isRoot, clusBytes);
        w.Write(ClusterLba(dataLba, dir.FirstCluster, spc), table, 0, table.Length);

        var buf = new byte[clusBytes];
        foreach (var k in dir.Kids) {
            if (k.IsDir) {
                WriteTree(k, false, w, dataLba, spc, clusBytes, null, report, ref done, total);
                continue;
            }
            if (k.Size == 0) continue;
            report("Copying " + k.Name, done, total);
            using (var src = k.Open()) {
                long left = k.Size; uint c = k.FirstCluster;
                while (left > 0) {
                    int want = (int)Math.Min(left, clusBytes), got = 0, n;
                    while (got < want && (n = src.Read(buf, got, want - got)) > 0) got += n;
                    if (got < want) throw new IOException("Short read from " + k.Name);
                    // Zero the rest of the final cluster rather than just the
                    // final sector: the drive was advertised as erased, and a
                    // 1-byte file would otherwise leave most of a cluster of the
                    // previous owner's data readable inside its own allocation.
                    for (int i = got; i < clusBytes; i++) buf[i] = 0;
                    w.Write(ClusterLba(dataLba, c, spc), buf, 0, clusBytes);
                    left -= got; c++; done += got;
                    report("Copying " + k.Name, done, total);
                }
            }
        }
    }

    static long ClusterLba(long dataLba, uint clus, int spc)
    { return dataLba + (long)(clus - 2) * spc; }

    /* ---- directory tables --------------------------------------------------- */
    static byte[] BuildDirectory(Node dir, string label, bool isRoot, int clusBytes)
    {
        var outp = new MemoryStream();
        if (isRoot) {                                      // root volume-label entry
            var e = new byte[32];
            Array.Copy(Encoding.ASCII.GetBytes(Label11(label)), e, 11);
            e[11] = 0x08;
            PutTime(e, DateTime.Now);
            outp.Write(e, 0, 32);
        } else {
            outp.Write(DotEntry(".",  dir.FirstCluster,  dir.Time), 0, 32);
            outp.Write(DotEntry("..", dir.ParentCluster, dir.Time), 0, 32);
        }
        foreach (var k in dir.Kids) {
            if (k.NeedsLfn) {
                byte sum = LfnChecksum(k.ShortName);
                int slots = LfnSlots(k.Name);
                for (int s = slots; s >= 1; s--)
                    outp.Write(LfnEntry(k.Name, s, s == slots, sum), 0, 32);
            }
            outp.Write(ShortEntry(k), 0, 32);
        }
        long size = dir.Clusters * (long)clusBytes;
        var buf = new byte[size];
        var body = outp.ToArray();
        Array.Copy(body, buf, body.Length);
        return buf;
    }

    static byte[] DotEntry(string name, uint clus, DateTime t)
    {
        var e = new byte[32];
        Array.Copy(Encoding.ASCII.GetBytes(Pack83(name, "")), e, 11);
        e[11] = 0x10;
        PutTime(e, t);
        PutU16(e, 20, (ushort)(clus >> 16));
        PutU16(e, 26, (ushort)(clus & 0xFFFF));
        return e;
    }

    static byte[] ShortEntry(Node n)
    {
        var e = new byte[32];
        Array.Copy(Encoding.ASCII.GetBytes(n.ShortName), e, 11);
        e[11] = (byte)(n.IsDir ? 0x10 : 0x20);
        e[12] = n.NtFlags;
        PutTime(e, n.Time);
        PutU16(e, 20, (ushort)(n.FirstCluster >> 16));
        PutU16(e, 26, (ushort)(n.FirstCluster & 0xFFFF));
        PutU32(e, 28, n.IsDir ? 0 : (uint)n.Size);
        return e;
    }

    static byte[] LfnEntry(string name, int seq, bool last, byte sum)
    {
        var e = new byte[32];
        e[0]  = (byte)(seq | (last ? 0x40 : 0));
        e[11] = 0x0F;
        e[13] = sum;
        int start = (seq - 1) * 13;
        int[] at = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
        for (int i = 0; i < 13; i++) {
            int ci = start + i;
            ushort ch = ci < name.Length ? name[ci] : (ci == name.Length ? (ushort)0 : (ushort)0xFFFF);
            PutU16(e, at[i], ch);
        }
        return e;
    }

    static int  LfnSlots(string name) { return (name.Length + 12) / 13; }

    static byte LfnChecksum(string short11)
    {
        byte sum = 0;
        for (int i = 0; i < 11; i++)
            sum = (byte)(((sum & 1) != 0 ? 0x80 : 0) + (sum >> 1) + (byte)short11[i]);
        return sum;
    }

    /* ---- 8.3 names ---------------------------------------------------------- */
    const string OK83 = "$%'-_@~`!(){}^#&";

    // ASCII-only on purpose.  char.IsUpper/IsDigit are Unicode-aware and answer
    // true for 'E', 'N', Cyrillic capitals and Arabic-Indic digits - which would
    // then be written through Encoding.ASCII as '?', an illegal wildcard in a
    // short name.  Worse, two names differing only in accent would collapse to
    // the SAME on-disk entry without the ~n disambiguator noticing.
    static bool IsUpperAscii(char c) { return c >= 'A' && c <= 'Z'; }
    static bool IsLowerAscii(char c) { return c >= 'a' && c <= 'z'; }
    static bool IsDigitAscii(char c) { return c >= '0' && c <= '9'; }
    static bool Ok83Char(char c) { return IsUpperAscii(c) || IsDigitAscii(c) || OK83.IndexOf(c) >= 0; }

    // A name is usable as-is only if it is already the uppercase 8.3 the OS will
    // see.  Anything else (lowercase, spaces, long, a second dot) keeps its real
    // name in an LFN and gets a generated alias - so Windows shows "My Song.mp3"
    // while UnoDOS, which reads short names only, sees MYSONG~1.MP3.
    static bool IsPlain83(string name, out string packed)
    {
        packed = null;
        string b = name, x = "";
        int dot = name.LastIndexOf('.');
        if (dot >= 0) { b = name.Substring(0, dot); x = name.Substring(dot + 1); }
        if (b.Length == 0 || b.Length > 8 || x.Length > 3) return false;
        if (name.IndexOf('.') != dot) return false;                 // only one dot
        foreach (char c in b + x)
            if (!Ok83Char(c)) return false;
        packed = Pack83(b, x);
        return true;
    }

    // base + extension -> the raw 11-byte on-disk field (8 padded, 3 padded).
    // Takes the two halves separately on purpose: packing "HELLO.MD" as one
    // string and re-splitting it loses the extension.
    static string Pack83(string b, string x)
    {
        if (b.Length > 8) b = b.Substring(0, 8);
        if (x.Length > 3) x = x.Substring(0, 3);
        return b.PadRight(8) + x.PadRight(3);
    }

    static string Label11(string label)
    {
        var sb = new StringBuilder();
        foreach (char c in (label ?? "UNODOS").ToUpperInvariant())
            sb.Append(Ok83Char(c) || c == ' ' ? c : '_');       // ASCII, or ASCII wins
        string s = sb.Length == 0 ? "UNODOS" : sb.ToString();
        return s.Length > 11 ? s.Substring(0, 11) : s.PadRight(11);
    }

    // A name that is 8.3 apart from its case needs no LFN: FAT carries two bits
    // saying "display the base / extension lowercase", and the name field itself
    // stays uppercase.  Worth handling, because it is what mtools does when it
    // builds the shipped image - without it `sample.uno` would land as
    // SAMPLE~1.UNO and the OS, which reads short names only, would see a
    // different filename than it does today.  The bits are all-or-nothing per
    // half, so genuinely mixed case still falls through to an LFN.
    static bool TryCaseFolded83(string name, out string packed, out byte ntFlags)
    {
        packed = null; ntFlags = 0;
        string b = name, x = "";
        int dot = name.LastIndexOf('.');
        if (dot >= 0) { b = name.Substring(0, dot); x = name.Substring(dot + 1); }
        if (name.IndexOf('.') != dot) return false;
        if (MixedCase(b) || MixedCase(x)) return false;
        string ub = b.ToUpperInvariant(), ux = x.ToUpperInvariant();
        string probe;
        if (!IsPlain83(ub + (ux.Length > 0 ? "." + ux : ""), out probe)) return false;
        if (HasLower(b)) ntFlags |= 0x08;
        if (HasLower(x)) ntFlags |= 0x10;
        packed = probe;
        return true;
    }

    static bool HasLower(string s) { foreach (char c in s) if (IsLowerAscii(c)) return true; return false; }
    static bool MixedCase(string s)
    {
        bool up = false, lo = false;
        foreach (char c in s) { if (IsUpperAscii(c)) up = true; else if (IsLowerAscii(c)) lo = true; }
        return up && lo;
    }

    static void AssignShortNames(Node dir)
    {
        var taken = new HashSet<string>(StringComparer.Ordinal);
        foreach (var k in dir.Kids) {
            string packed; byte flags;
            if (IsPlain83(k.Name, out packed) && !taken.Contains(packed)) {
                k.ShortName = packed; k.NeedsLfn = false; taken.Add(packed);
                continue;
            }
            if (TryCaseFolded83(k.Name, out packed, out flags) && !taken.Contains(packed)) {
                k.ShortName = packed; k.NtFlags = flags; k.NeedsLfn = false; taken.Add(packed);
                continue;
            }
            k.NeedsLfn = true;
            k.ShortName = MakeAlias(k.Name, taken);
            taken.Add(k.ShortName);
        }
    }

    static string MakeAlias(string name, HashSet<string> taken)
    {
        string b = name, x = "";
        int dot = name.LastIndexOf('.');
        if (dot > 0) { b = name.Substring(0, dot); x = name.Substring(dot + 1); }
        b = Clean(b); x = Clean(x);
        if (b.Length == 0) b = "FILE";
        if (x.Length > 3) x = x.Substring(0, 3);
        for (int n = 1; n < 1000000; n++) {
            string tail = "~" + n;
            if (tail.Length >= 8) break;
            string stem = b.Length + tail.Length > 8 ? b.Substring(0, 8 - tail.Length) : b;
            string cand = Pack83(stem + tail, x);
            if (!taken.Contains(cand)) return cand;
        }
        throw new IOException("Too many similarly named files: " + name);
    }

    static string Clean(string s)
    {
        var sb = new StringBuilder();
        foreach (char c in s.ToUpperInvariant()) {
            if (Ok83Char(c)) sb.Append(c);
            else if (c == ' ' || c == '.') continue;
            else sb.Append('_');          // anything non-ASCII ends up here
        }
        return sb.ToString();
    }

    /* ---- boot sector / FSInfo ----------------------------------------------- */
    static byte[] BootSector(long volSectors, int spc, long fatSize, uint serial, string label)
    {
        var b = new byte[SECTOR];
        b[0] = 0xEB; b[1] = 0x58; b[2] = 0x90;                    // jmp short +0x58
        Array.Copy(Encoding.ASCII.GetBytes("MSWIN4.1"), 0, b, 3, 8);
        PutU16(b, 11, SECTOR);
        b[13] = (byte)spc;
        PutU16(b, 14, RESERVED);
        b[16] = NUM_FATS;
        PutU16(b, 17, 0);                                          // root entries: FAT32 = 0
        PutU16(b, 19, 0);                                          // 16-bit total: FAT32 = 0
        b[21] = 0xF8;                                              // fixed disk
        PutU16(b, 22, 0);                                          // FATSz16: FAT32 = 0
        PutU16(b, 24, 63); PutU16(b, 26, 255);                     // CHS geometry, cosmetic
        PutU32(b, 28, (uint)PART_START);                           // hidden sectors
        PutU32(b, 32, (uint)volSectors);
        PutU32(b, 36, (uint)fatSize);
        PutU16(b, 40, 0);                                          // ExtFlags: FATs mirrored
        PutU16(b, 42, 0);                                          // FSVer 0.0
        PutU32(b, 44, 2);                                          // root cluster
        PutU16(b, 48, 1);                                          // FSInfo sector
        PutU16(b, 50, 6);                                          // backup boot sector
        b[64] = 0x80; b[66] = 0x29;
        PutU32(b, 67, serial);
        Array.Copy(Encoding.ASCII.GetBytes(Label11(label)), 0, b, 71, 11);
        Array.Copy(Encoding.ASCII.GetBytes("FAT32   "), 0, b, 82, 8);
        b[510] = 0x55; b[511] = 0xAA;                              // fat.c insists on this
        return b;
    }

    static byte[] FsInfo(long free)
    {
        var b = new byte[SECTOR];
        PutU32(b, 0, 0x41615252);
        PutU32(b, 484, 0x61417272);
        PutU32(b, 488, (uint)free);
        PutU32(b, 492, 0xFFFFFFFF);                                // next free: unknown
        b[510] = 0x55; b[511] = 0xAA;
        return b;
    }

    /* ---- GPT ---------------------------------------------------------------- */
    static void WriteGpt(SectorWriter w, long diskSectors, long first, long last, long lastUsable)
    {
        long entryArrSectors = (long)GPT_ENTRIES * GPT_ENTSZ / SECTOR;   // 32
        long altHdr = diskSectors - 1;
        long altArr = altHdr - entryArrSectors;
        long firstUsable = 2 + entryArrSectors;                          // 34: after the primary GPT

        // protective MBR
        var mbr = new byte[SECTOR];
        mbr[446 + 1] = 0x00; mbr[446 + 2] = 0x02; mbr[446 + 3] = 0x00;   // CHS 0/0/2
        mbr[446 + 4] = 0xEE;
        mbr[446 + 5] = 0xFF; mbr[446 + 6] = 0xFF; mbr[446 + 7] = 0xFF;
        PutU32(mbr, 446 + 8, 1);
        // 0xFFFFFFFF is the spec's "size not representable" value, not a clamp
        PutU32(mbr, 446 + 12, diskSectors - 1 >= 0xFFFFFFFFL
                              ? 0xFFFFFFFF : (uint)(diskSectors - 1));
        mbr[510] = 0x55; mbr[511] = 0xAA;
        w.Write(0, mbr, 0, SECTOR);

        // the entry array: one ESP spanning everything between the GPT copies
        var arr = new byte[GPT_ENTRIES * GPT_ENTSZ];
        Array.Copy(ESP_TYPE, 0, arr, 0, 16);
        Array.Copy(Guid.NewGuid().ToByteArray(), 0, arr, 16, 16);
        PutU64(arr, 32, (ulong)first);
        PutU64(arr, 40, (ulong)last);
        var nm = Encoding.Unicode.GetBytes("UNO-ESP");
        Array.Copy(nm, 0, arr, 56, nm.Length);
        uint arrCrc = Crc32.Of(arr, 0, arr.Length);

        byte[] diskGuid = Guid.NewGuid().ToByteArray();
        w.Write(2, arr, 0, arr.Length);
        w.Write(altArr, arr, 0, arr.Length);
        w.Write(1, GptHeader(1, altHdr, firstUsable, lastUsable, diskGuid, 2, arrCrc, diskSectors), 0, SECTOR);
        w.Write(altHdr, GptHeader(altHdr, 1, firstUsable, lastUsable, diskGuid, altArr, arrCrc, diskSectors), 0, SECTOR);
    }

    static byte[] GptHeader(long my, long alt, long firstUsable, long lastUsable,
                            byte[] diskGuid, long entryLba, uint arrCrc, long diskSectors)
    {
        var h = new byte[SECTOR];
        Array.Copy(Encoding.ASCII.GetBytes("EFI PART"), h, 8);
        PutU32(h, 8, 0x00010000);                                   // revision 1.0
        PutU32(h, 12, 92);                                          // header size
        PutU32(h, 16, 0);                                           // CRC placeholder
        PutU64(h, 24, (ulong)my);
        PutU64(h, 32, (ulong)alt);
        PutU64(h, 40, (ulong)firstUsable);
        PutU64(h, 48, (ulong)lastUsable);
        Array.Copy(diskGuid, 0, h, 56, 16);
        PutU64(h, 72, (ulong)entryLba);
        PutU32(h, 80, GPT_ENTRIES);
        PutU32(h, 84, GPT_ENTSZ);
        PutU32(h, 88, arrCrc);
        PutU32(h, 16, Crc32.Of(h, 0, 92));
        return h;
    }

    /* ---- little helpers ------------------------------------------------------ */
    static void PutU16(byte[] b, int o, ushort v) { b[o] = (byte)v; b[o + 1] = (byte)(v >> 8); }
    static void PutU32(byte[] b, int o, uint v)
    { b[o] = (byte)v; b[o+1] = (byte)(v>>8); b[o+2] = (byte)(v>>16); b[o+3] = (byte)(v>>24); }
    static void PutU64(byte[] b, int o, ulong v)
    { for (int i = 0; i < 8; i++) b[o + i] = (byte)(v >> (8 * i)); }

    static void PutTime(byte[] e, DateTime t)
    {
        ushort time = (ushort)((t.Hour << 11) | (t.Minute << 5) | (t.Second / 2));
        ushort date = (ushort)(((Math.Max(1980, t.Year) - 1980) << 9) | (t.Month << 5) | t.Day);
        PutU16(e, 14, time); PutU16(e, 16, date);      // created
        PutU16(e, 18, date);                           // accessed
        PutU16(e, 22, time); PutU16(e, 24, date);      // modified
    }
}

/* Whole-sector writes only: a handle on \\.\PHYSICALDRIVE rejects anything else,
 * and .NET's own FileStream buffering would happily issue an unaligned one. */
class SectorWriter
{
    readonly Stream s;
    readonly byte[] zeros = new byte[1024 * 1024];
    public SectorWriter(Stream stream) { s = stream; }

    public void Write(long lba, byte[] data, int off, int len)
    {
        if (len % UnoDisk.SECTOR != 0) throw new IOException("unaligned write of " + len + " bytes");
        s.Position = lba * UnoDisk.SECTOR;
        s.Write(data, off, len);
    }

    public void Zero(long lba, long sectors)
    {
        s.Position = lba * UnoDisk.SECTOR;
        long left = sectors * UnoDisk.SECTOR;
        while (left > 0) {
            int n = (int)Math.Min(left, zeros.Length);
            s.Write(zeros, 0, n);
            left -= n;
        }
    }

    public void Flush() { s.Flush(); }
}

static class Crc32
{
    static readonly uint[] T = Make();
    static uint[] Make()
    {
        var t = new uint[256];
        for (uint i = 0; i < 256; i++) {
            uint c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) != 0 ? 0xEDB88320 ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }
    public static uint Of(byte[] b, int off, int len)
    {
        uint c = 0xFFFFFFFF;
        for (int i = 0; i < len; i++) c = T[(c ^ b[off + i]) & 0xFF] ^ (c >> 8);
        return c ^ 0xFFFFFFFF;
    }
}
