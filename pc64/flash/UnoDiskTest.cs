/*  UnoDiskTest.cs - build a UnoDOS disk into an image FILE instead of a drive.
 *
 *  UnoDisk writes through a plain Stream, so the exact code that formats a USB
 *  stick can format a sparse file that fsck.vfat, sgdisk, mdir and QEMU can all
 *  inspect.  That is the whole point: the filesystem writer is verified against
 *  real tools rather than by flashing a stick and hoping.
 *
 *    UnoDiskTest.exe <out.img> <sizeMiB> <espFolderOrZip> [extraFolderOrZip ...]
 *
 *  Build: see flash/build-flasher.ps1 -TestTool
 */
using System;
using System.Collections.Generic;
using System.IO;

static class UnoDiskTestProgram
{
    static int Main(string[] argv)
    {
        if (argv.Length < 3) {
            Console.WriteLine("usage: UnoDiskTest <out.img|sink> <sizeMiB> <espFolderOrZip> [extra ...]");
            Console.WriteLine("  sink = discard the data but report the GPT, so multi-TB");
            Console.WriteLine("         geometries can be checked without the disk space");
            return 2;
        }
        string outPath = argv[0];
        long mib = long.Parse(argv[1]);

        var sources = new List<IPayloadSource>();
        for (int i = 2; i < argv.Length; i++) sources.Add(Source(argv[i]));

        if (outPath == "sink") return Sink(mib, sources);

        try {
            using (var fs = new FileStream(outPath, FileMode.Create, FileAccess.ReadWrite)) {
                fs.SetLength(mib * 1024 * 1024);
                string last = "";
                string summary = UnoDisk.Build(fs, mib * 1024 * 1024, sources, "UNODOS",
                    (stage, done, total) => {
                        if (stage == last) return;
                        last = stage;
                        Console.WriteLine("  " + stage);
                    });
                Console.WriteLine("OK: " + outPath + " - " + summary);
            }
            return 0;
        } catch (Exception e) {
            Console.WriteLine("FAIL: " + e.GetType().Name + ": " + e.Message);
            Console.WriteLine(e.StackTrace);
            return 1;
        }
    }

    /* Build against a stream that throws the data away but remembers the sectors
     * near each end of the disk.  That is enough to check where the GPT landed on
     * a disk far too big to allocate - the case where the partition has to stop
     * at the FAT32 32-bit sector limit while the backup GPT must still sit at the
     * true last LBA. */
    static int Sink(long mib, List<IPayloadSource> sources)
    {
        long bytes = mib * 1024 * 1024;
        var sink = new SinkStream(bytes);
        string summary = UnoDisk.Build(sink, bytes, sources, "UNODOS", (s, d, t) => { });
        Console.WriteLine("OK: " + summary);

        long lastLba = bytes / UnoDisk.SECTOR - 1;
        byte[] pri = sink.Get(1), alt = sink.Get(lastLba), ent = sink.Get(2);
        Console.WriteLine("disk last LBA      : " + lastLba);
        Console.WriteLine("primary hdr sig    : " + Sig(pri));
        Console.WriteLine("backup  hdr sig    : " + Sig(alt) + "  (at the true last LBA)");
        if (pri != null) {
            Console.WriteLine("primary AlternateLBA: " + BitConverter.ToInt64(pri, 32));
            Console.WriteLine("LastUsableLBA       : " + BitConverter.ToInt64(pri, 48));
        }
        if (ent != null) {
            long first = BitConverter.ToInt64(ent, 32), last = BitConverter.ToInt64(ent, 40);
            Console.WriteLine("partition           : " + first + " .. " + last
                              + "  (" + (last - first + 1) + " sectors)");
            Console.WriteLine("fits in 32 bits     : " + ((last - first + 1) <= 0xFFFFFFFFL));
        }
        return 0;
    }

    static string Sig(byte[] b)
    {
        if (b == null) return "(never written!)";
        return System.Text.Encoding.ASCII.GetString(b, 0, 8);
    }

    static IPayloadSource Source(string spec)
    {
        string prefix = "";
        int bar = spec.IndexOf('|');                 // "path|SUBDIR" puts it in a subfolder
        if (bar > 0) { prefix = spec.Substring(bar + 1); spec = spec.Substring(0, bar); }
        if (Directory.Exists(spec)) return new FolderPayload(spec, prefix);
        if (File.Exists(spec))      return new ZipPayload(spec, prefix);
        throw new IOException("No such folder or zip: " + spec);
    }
}

/* Seekable, writable, and forgetful - except for the first and last few sectors,
 * which is where the partition table lives. */
class SinkStream : Stream
{
    readonly long len;
    readonly System.Collections.Generic.Dictionary<long, byte[]> kept =
        new System.Collections.Generic.Dictionary<long, byte[]>();
    long pos;

    public SinkStream(long length) { len = length; }

    bool Interesting(long lba)
    {
        long last = len / UnoDisk.SECTOR - 1;
        return lba < 64 || lba > last - 64;
    }

    public byte[] Get(long lba)
    {
        byte[] b;
        return kept.TryGetValue(lba, out b) ? b : null;
    }

    public override void Write(byte[] buffer, int offset, int count)
    {
        long lba = pos / UnoDisk.SECTOR;
        for (int i = 0; i < count; i += UnoDisk.SECTOR, lba++) {
            if (!Interesting(lba)) continue;
            var s = new byte[UnoDisk.SECTOR];
            Array.Copy(buffer, offset + i, s, 0, Math.Min(UnoDisk.SECTOR, count - i));
            kept[lba] = s;
        }
        pos += count;
    }

    public override bool CanRead { get { return false; } }
    public override bool CanSeek { get { return true; } }
    public override bool CanWrite { get { return true; } }
    public override long Length { get { return len; } }
    public override long Position { get { return pos; } set { pos = value; } }
    public override void Flush() { }
    public override long Seek(long o, SeekOrigin w)
    {
        pos = w == SeekOrigin.Begin ? o : w == SeekOrigin.Current ? pos + o : len + o;
        return pos;
    }
    public override void SetLength(long v) { }
    public override int Read(byte[] b, int o, int c) { throw new NotSupportedException(); }
}
