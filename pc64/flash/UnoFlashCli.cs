// Headless raw-image flasher for UnoDOS/pc64 on Windows.
//
// Same proven sequence as pc64/flash/UnoDosFlash.cs (the GUI), minus the GUI:
// lock + dismount every volume on the target disk BY VOLUME GUID (so letterless
// ESP/System partitions are caught too), hold those handles open, then write the
// raw image through FILE_FLAG_WRITE_THROUGH.
//
//   UnoFlashCli.exe <diskIndex> <image> <statusFile> <expectModelSubstring>
//
// Two guards, because this writes raw sectors to a physical disk:
//   * the disk's WMI model must contain <expectModelSubstring>
//   * the disk must be USB and under 64 GB
// Either failing aborts before a single byte is written.
//
// The status file is the progress channel: an elevated child is detached from
// the parent console, so its stdout is not reliably observable.
using System;
using System.IO;
using System.Collections.Generic;
using System.Management;
using System.Runtime.InteropServices;
using System.Threading;
using Microsoft.Win32.SafeHandles;

static class Program
{
    static string statusPath;

    static void Say(string s)
    {
        Console.WriteLine(s);
        try { File.AppendAllText(statusPath, DateTime.Now.ToString("HH:mm:ss") + " " + s + "\r\n"); }
        catch { }
    }

    static int Main(string[] a)
    {
        if (a.Length < 4) { Console.WriteLine("usage: UnoFlashCli <diskIndex> <img> <statusFile> <modelSubstr>"); return 2; }
        int index = int.Parse(a[0]);
        string img = a[1];
        statusPath = a[2];
        string expect = a[3];

        try { File.WriteAllText(statusPath, ""); } catch { }

        try {
            // ---- identity guard -------------------------------------------------
            string model = null, iface = null; ulong size = 0;
            using (var s = new ManagementObjectSearcher(
                "SELECT Model,InterfaceType,Size,Index FROM Win32_DiskDrive WHERE Index=" + index))
                foreach (ManagementObject d in s.Get()) {
                    model = (string)d["Model"];
                    iface = (string)d["InterfaceType"];
                    size  = d["Size"] == null ? 0 : Convert.ToUInt64(d["Size"]);
                }
            if (model == null) { Say("ABORT: no disk with index " + index); return 3; }
            Say("target: disk " + index + " = '" + model + "' " + iface + " " +
                Math.Round(size / 1073741824.0, 1) + " GB");
            if (model.IndexOf(expect, StringComparison.OrdinalIgnoreCase) < 0) {
                Say("ABORT: model does not contain '" + expect + "'"); return 3; }
            if (iface != "USB") { Say("ABORT: not a USB disk"); return 3; }
            if (size == 0 || size > 64UL * 1024 * 1024 * 1024) {
                Say("ABORT: size out of range (expect < 64 GB)"); return 3; }

            var fi = new FileInfo(img);
            if (!fi.Exists) { Say("ABORT: image not found: " + img); return 3; }
            if ((ulong)fi.Length > size) { Say("ABORT: image larger than the disk"); return 3; }
            Say("image : " + img + " (" + Math.Round(fi.Length / 1048576.0, 1) + " MB)");

            // ---- lock + dismount -------------------------------------------------
            var held = DismountVolumes(index);
            Say("dismounted " + held.Count + " volume(s) on disk " + index);

            // ---- write -----------------------------------------------------------
            string dev = @"\\.\PHYSICALDRIVE" + index;
            using (var disk = Native.CreateFile(dev, Native.GENERIC_READ | Native.GENERIC_WRITE,
                       Native.FILE_SHARE_READ | Native.FILE_SHARE_WRITE, IntPtr.Zero,
                       Native.OPEN_EXISTING, Native.FILE_FLAG_WRITE_THROUGH, IntPtr.Zero)) {
                if (disk.IsInvalid) {
                    Say("ABORT: cannot open " + dev + " err=" + Marshal.GetLastWin32Error()); return 4; }
                // ownsHandle:false - otherwise disposing the FileStream closes
                // `disk`, and the IOCTL_DISK_UPDATE_PROPERTIES below then
                // throws ObjectDisposedException AFTER a perfectly good write,
                // which reads like a failed flash when it wasn't.
                using (var outs = new FileStream(disk, FileAccess.Write, 1 << 20, false))
                using (var ins  = File.OpenRead(img)) {
                    byte[] buf = new byte[1 << 20];
                    long done = 0; int n, pct = -1;
                    while ((n = ins.Read(buf, 0, buf.Length)) > 0) {
                        outs.Write(buf, 0, n);
                        done += n;
                        int p = (int)(done * 100 / fi.Length);
                        if (p != pct && p % 10 == 0) { pct = p; Say("writing " + p + "%"); }
                    }
                    outs.Flush();
                    Native.FlushFileBuffers(disk);
                }
                uint br;
                Native.DeviceIoControl(disk, Native.IOCTL_DISK_UPDATE_PROPERTIES,
                    IntPtr.Zero, 0, IntPtr.Zero, 0, out br, IntPtr.Zero);
            }
            foreach (var h in held) h.Dispose();          // let Windows remount
            Say("DONE");
            return 0;
        } catch (Exception e) {
            Say("ABORT: " + e.GetType().Name + ": " + e.Message);
            return 5;
        }
    }

    static bool VolumeOnDisk(SafeFileHandle h, int index)
    {
        byte[] buf = new byte[2048]; uint br;
        var gch = GCHandle.Alloc(buf, GCHandleType.Pinned);
        try {
            if (!Native.DeviceIoControl(h, Native.IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS,
                    IntPtr.Zero, 0, gch.AddrOfPinnedObject(), (uint)buf.Length, out br, IntPtr.Zero))
                return false;
            int n = BitConverter.ToInt32(buf, 0);
            for (int i = 0; i < n; i++) {
                int off = 8 + i * 24;
                if (off + 4 <= buf.Length && BitConverter.ToInt32(buf, off) == index) return true;
            }
        } finally { gch.Free(); }
        return false;
    }

    static List<SafeFileHandle> DismountVolumes(int index)
    {
        var held = new List<SafeFileHandle>();
        var name = new System.Text.StringBuilder(300);
        IntPtr find = Native.FindFirstVolume(name, name.Capacity);
        if (find == Native.INVALID_HANDLE) return held;
        do {
            string vol = name.ToString().TrimEnd('\\');
            var h = Native.CreateFile(vol, Native.GENERIC_READ | Native.GENERIC_WRITE,
                Native.FILE_SHARE_READ | Native.FILE_SHARE_WRITE, IntPtr.Zero,
                Native.OPEN_EXISTING, 0, IntPtr.Zero);
            if (h.IsInvalid) continue;
            if (!VolumeOnDisk(h, index)) { h.Dispose(); continue; }
            uint x;
            for (int i = 0; i < 20 &&
                 !Native.DeviceIoControl(h, Native.FSCTL_LOCK_VOLUME, IntPtr.Zero, 0, IntPtr.Zero, 0, out x, IntPtr.Zero);
                 i++) Thread.Sleep(100);
            Native.DeviceIoControl(h, Native.FSCTL_DISMOUNT_VOLUME, IntPtr.Zero, 0, IntPtr.Zero, 0, out x, IntPtr.Zero);
            held.Add(h);
        } while (Native.FindNextVolume(find, name, name.Capacity));
        Native.FindVolumeClose(find);
        return held;
    }
}

static class Native
{
    public const uint GENERIC_READ = 0x80000000, GENERIC_WRITE = 0x40000000;
    public const uint FILE_SHARE_READ = 1, FILE_SHARE_WRITE = 2;
    public const uint OPEN_EXISTING = 3;
    public const uint FILE_FLAG_WRITE_THROUGH = 0x80000000;
    public const uint FSCTL_LOCK_VOLUME = 0x00090018;
    public const uint FSCTL_DISMOUNT_VOLUME = 0x00090020;
    public const uint IOCTL_DISK_UPDATE_PROPERTIES = 0x00070140;
    public const uint IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS = 0x00560000;
    public static readonly IntPtr INVALID_HANDLE = new IntPtr(-1);

    [DllImport("kernel32", SetLastError = true, CharSet = CharSet.Unicode)]
    public static extern SafeFileHandle CreateFile(string name, uint access, uint share,
        IntPtr sec, uint disposition, uint flags, IntPtr template);
    [DllImport("kernel32", SetLastError = true, CharSet = CharSet.Unicode)]
    public static extern IntPtr FindFirstVolume(System.Text.StringBuilder name, int len);
    [DllImport("kernel32", SetLastError = true, CharSet = CharSet.Unicode)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool FindNextVolume(IntPtr find, System.Text.StringBuilder name, int len);
    [DllImport("kernel32", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool FindVolumeClose(IntPtr find);
    [DllImport("kernel32", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool DeviceIoControl(SafeFileHandle h, uint code, IntPtr inBuf, uint inSize,
        IntPtr outBuf, uint outSize, out uint returned, IntPtr overlapped);
    [DllImport("kernel32", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool FlushFileBuffers(SafeFileHandle h);
}
