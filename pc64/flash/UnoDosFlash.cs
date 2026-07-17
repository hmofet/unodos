/*  UnoDOS USB Installer  -  a single self-contained Windows exe.
 *
 *  Picks a removable USB drive, confirms, and writes the bundled bootable
 *  UnoDOS/pc64 disk image to it (raw).  One image is embedded (gzip-compressed):
 *  a GPT-partitioned UEFI disk with an EFI System Partition (FAT32) holding
 *  /EFI/BOOT/BOOTX64.EFI.  pc64 is UEFI/x86-64 only, so - unlike the Writer's
 *  Unlock flasher this is modeled on - there is no BIOS or filesystem choice.
 *
 *  Build: pc64/flash/build-flasher.ps1  (csc + embedded image resource).
 *  Requires admin (raw disk write) - the manifest forces a UAC prompt.
 */
using System;
using System.Collections.Generic;
using System.Drawing;
using System.IO;
using System.IO.Compression;
using System.Management;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Threading;
using System.Windows.Forms;
using Microsoft.Win32.SafeHandles;

class UsbDrive
{
    public int Index;            // PhysicalDrive index
    public string Model;
    public ulong Size;
    public string DeviceId;      // \\.\PHYSICALDRIVE<n>
    public override string ToString()
    {
        double gb = Size / (1024.0 * 1024.0 * 1024.0);
        return string.Format("{0}  ({1:0.0} GB)   [PhysicalDrive{2}]", Model, gb, Index);
    }
}

class FlashForm : Form
{
    const string IMAGE_RESOURCE = "unodos_uefi";     // the single embedded image

    ComboBox driveBox;
    Button refreshBtn, flashBtn, showAllBtn;
    ProgressBar progress;
    Label status;

    readonly List<UsbDrive> allDrives = new List<UsbDrive>();   // full scan, smallest-first
    bool showAll = false;                                       // include 0 GB (empty) drives?

    public FlashForm()
    {
        Text = "UnoDOS - USB Installer";
        Size = new Size(580, 366);
        FormBorderStyle = FormBorderStyle.FixedDialog;
        MaximizeBox = false;
        StartPosition = FormStartPosition.CenterScreen;
        Font = new Font("Segoe UI", 9f);

        Controls.Add(new Label {
            Text = "Install UnoDOS to a USB drive",
            Font = new Font("Segoe UI", 13f, FontStyle.Bold),
            Location = new Point(16, 14), AutoSize = true });

        Controls.Add(new Label { Text = "Target USB drive:", Location = new Point(16, 52), AutoSize = true });
        driveBox = new ComboBox {
            Location = new Point(16, 74), Size = new Size(350, 24),
            DropDownStyle = ComboBoxStyle.DropDownList };
        Controls.Add(driveBox);
        // Reveals 0 GB drives (empty card-reader slots) that are hidden by default.
        showAllBtn = new Button { Text = "Show all", Location = new Point(372, 73), Size = new Size(86, 26), Visible = false };
        showAllBtn.Click += (s, e) => { showAll = !showAll; PopulateDriveBox(); };
        Controls.Add(showAllBtn);
        refreshBtn = new Button { Text = "Refresh", Location = new Point(462, 73), Size = new Size(90, 26) };
        refreshBtn.Click += (s, e) => LoadDrives();
        Controls.Add(refreshBtn);

        Controls.Add(new Label {
            Text = "UEFI boot (x86-64 PCs, 2012+). Turn OFF Secure Boot in firmware, then\n" +
                   "pick this USB from the boot menu. pc64 is UEFI-only - there is no BIOS build.",
            Location = new Point(16, 112), Size = new Size(536, 44),
            ForeColor = Color.FromArgb(60, 60, 60) });

        Controls.Add(new Label {
            Text = "Everything on the selected drive will be erased.",
            ForeColor = Color.FromArgb(176, 0, 32),
            Location = new Point(16, 168), AutoSize = true });

        progress = new ProgressBar { Location = new Point(16, 200), Size = new Size(536, 22) };
        Controls.Add(progress);
        status = new Label { Text = "", Location = new Point(16, 232), Size = new Size(420, 20) };
        Controls.Add(status);

        flashBtn = new Button {
            Text = "Install", Location = new Point(464, 230), Size = new Size(88, 32),
            BackColor = Color.FromArgb(0, 120, 215), ForeColor = Color.White, FlatStyle = FlatStyle.Flat };
        flashBtn.Click += FlashClicked;
        Controls.Add(flashBtn);

        LoadDrives();
    }

    void LoadDrives()
    {
        allDrives.Clear();
        try {
            using (var searcher = new ManagementObjectSearcher(
                "SELECT * FROM Win32_DiskDrive WHERE InterfaceType='USB'")) {
                foreach (ManagementObject d in searcher.Get()) {
                    string did = (string)d["DeviceID"];          // \\.\PHYSICALDRIVE<n>
                    var d2 = new UsbDrive {
                        Model = ((string)(d["Model"] ?? "USB drive")).Trim(),
                        Size = d["Size"] != null ? (ulong)(d["Size"]) : 0,
                        DeviceId = did };
                    int j = did.Length;
                    while (j > 0 && char.IsDigit(did[j - 1])) j--;
                    int idx;
                    if (int.TryParse(did.Substring(j), out idx)) d2.Index = idx;
                    allDrives.Add(d2);
                }
            }
        } catch (Exception ex) {
            status.Text = "Drive scan failed: " + ex.Message;
        }
        // Smallest-first: a USB stick / SD card is usually the smallest removable
        // disk, so it lands at the top and is auto-selected; a big external backup
        // drive sinks to the bottom where it's harder to pick by accident.
        allDrives.Sort((a, b) => a.Size.CompareTo(b.Size));
        PopulateDriveBox();
    }

    // Fill the combo from allDrives, hiding 0 GB drives (empty card-reader slots,
    // no media) unless the user clicked Show all.  Always auto-selects the top
    // (smallest) entry as the most likely flash target.
    void PopulateDriveBox()
    {
        driveBox.Items.Clear();
        int hidden = 0;
        foreach (var d in allDrives) {
            if (!showAll && d.Size == 0) { hidden++; continue; }
            driveBox.Items.Add(d);
        }
        if (driveBox.Items.Count > 0) driveBox.SelectedIndex = 0;

        showAllBtn.Visible = hidden > 0 || showAll;
        showAllBtn.Text = showAll ? "Hide empty" : "Show all (" + hidden + ")";

        if (driveBox.Items.Count > 0)
            status.Text = driveBox.Items.Count + " USB drive(s) found"
                + (hidden > 0 ? ", " + hidden + " empty hidden." : ".");
        else
            status.Text = hidden > 0
                ? hidden + " empty drive(s) hidden - click Show all."
                : "No USB drives found.";
    }

    // Does this volume handle have any disk extent on physical disk `index`?
    static bool VolumeOnDisk(SafeFileHandle h, int index)
    {
        byte[] buf = new byte[2048]; uint br;
        var gch = GCHandle.Alloc(buf, GCHandleType.Pinned);
        try {
            if (!Native.DeviceIoControl(h, Native.IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS,
                    IntPtr.Zero, 0, gch.AddrOfPinnedObject(), (uint)buf.Length, out br, IntPtr.Zero))
                return false;
            int n = BitConverter.ToInt32(buf, 0);            // NumberOfDiskExtents
            for (int i = 0; i < n; i++) {
                int off = 8 + i * 24;                        // DISK_EXTENT { DWORD DiskNumber; pad; LARGE_INTEGER x2 }
                if (off + 4 <= buf.Length && BitConverter.ToInt32(buf, off) == index) return true;
            }
        } finally { gch.Free(); }
        return false;
    }

    // Lock + dismount every mounted volume physically on disk `index`.  Enumerating
    // by volume GUID (not drive letters) catches partitions with no letter - e.g.
    // an ESP or a card's hidden volume - which otherwise still block the raw write.
    static List<SafeFileHandle> DismountVolumes(int index)
    {
        var held = new List<SafeFileHandle>();
        var name = new System.Text.StringBuilder(300);
        IntPtr find = Native.FindFirstVolume(name, name.Capacity);
        if (find == Native.INVALID_HANDLE) return held;
        do {
            string vol = name.ToString().TrimEnd('\\');      // \\?\Volume{GUID}  (no trailing \ for CreateFile)
            var h = Native.CreateFile(vol, Native.GENERIC_READ | Native.GENERIC_WRITE,
                Native.FILE_SHARE_READ | Native.FILE_SHARE_WRITE, IntPtr.Zero, Native.OPEN_EXISTING, 0, IntPtr.Zero);
            if (h.IsInvalid) continue;
            if (!VolumeOnDisk(h, index)) { h.Dispose(); continue; }
            uint x;
            for (int i = 0; i < 20 &&                         // wait out Explorer / the indexer
                 !Native.DeviceIoControl(h, Native.FSCTL_LOCK_VOLUME, IntPtr.Zero, 0, IntPtr.Zero, 0, out x, IntPtr.Zero);
                 i++) Thread.Sleep(100);
            Native.DeviceIoControl(h, Native.FSCTL_DISMOUNT_VOLUME, IntPtr.Zero, 0, IntPtr.Zero, 0, out x, IntPtr.Zero);
            held.Add(h);                                      // keep open so it stays unmounted
        } while (Native.FindNextVolume(find, name, name.Capacity));
        Native.FindVolumeClose(find);
        return held;
    }

    void FlashClicked(object sender, EventArgs e)
    {
        var drive = driveBox.SelectedItem as UsbDrive;
        if (drive == null) { status.Text = "Select a USB drive first."; return; }

        var confirm = MessageBox.Show(
            "This will ERASE everything on:\n\n    " + drive.ToString() + "\n\n" +
            "and write UnoDOS (UEFI).\n\nContinue?",
            "Confirm erase", MessageBoxButtons.YesNo, MessageBoxIcon.Warning, MessageBoxDefaultButton.Button2);
        if (confirm != DialogResult.Yes) return;

        SetBusy(true);
        var t = new Thread(() => DoFlash(drive, IMAGE_RESOURCE)); t.IsBackground = true; t.Start();
    }

    void SetBusy(bool busy)
    {
        flashBtn.Enabled = refreshBtn.Enabled = showAllBtn.Enabled = driveBox.Enabled = !busy;
    }

    void Ui(Action a) { if (IsHandleCreated) BeginInvoke(a); }

    void DoFlash(UsbDrive drive, string res)
    {
        List<SafeFileHandle> locks = null;
        try {
            Ui(() => status.Text = "Dismounting volumes...");
            locks = DismountVolumes(drive.Index);          // 1. free the card's volumes

            // 2. open the raw physical drive and stream the decompressed image to it
            var disk = Native.CreateFile(drive.DeviceId, Native.GENERIC_READ | Native.GENERIC_WRITE,
                Native.FILE_SHARE_READ | Native.FILE_SHARE_WRITE, IntPtr.Zero, Native.OPEN_EXISTING,
                Native.FILE_FLAG_WRITE_THROUGH, IntPtr.Zero);
            if (disk.IsInvalid)
                throw new IOException("Cannot open the drive (run as Administrator). Win32 error " + Marshal.GetLastWin32Error());

            Stream gz = Assembly.GetExecutingAssembly().GetManifestResourceStream(res);
            if (gz == null) throw new IOException("Bundled image '" + res + "' is missing from this exe.");

            long total = ReadGzipSize(gz);
            gz.Position = 0;
            using (var diskStream = new FileStream(disk, FileAccess.Write))
            using (var inflate = new GZipStream(gz, CompressionMode.Decompress)) {
                byte[] buf = new byte[1024 * 1024];        // 1 MiB, sector-aligned
                long done = 0; int n, partial = 0;
                while ((n = inflate.Read(buf, partial, buf.Length - partial)) > 0) {
                    partial += n;
                    if (partial == buf.Length) {
                        diskStream.Write(buf, 0, partial);
                        done += partial; partial = 0;
                        long pct = total > 0 ? done * 100 / total : 0;
                        Ui(() => { progress.Value = (int)Math.Min(100, pct);
                                   status.Text = string.Format("Writing... {0} MB", done / (1024 * 1024)); });
                    }
                }
                if (partial > 0) {
                    int pad = (512 - (partial % 512)) % 512;
                    for (int i = 0; i < pad; i++) buf[partial + i] = 0;
                    diskStream.Write(buf, 0, partial + pad);
                }
                diskStream.Flush();
                // flush to the medium + re-read the partition table while the handle is
                // still open: the FileStream OWNS `disk` and closes it on dispose, so
                // touching it after the using-block would throw "Safe handle closed".
                Native.FlushFileBuffers(disk);
                uint br;
                Native.DeviceIoControl(disk, Native.IOCTL_DISK_UPDATE_PROPERTIES, IntPtr.Zero, 0, IntPtr.Zero, 0, out br, IntPtr.Zero);
            }   // FileStream disposes `disk` here

            Ui(() => { progress.Value = 100; status.Text = "Done - you can remove the drive.";
                       MessageBox.Show("UnoDOS was written successfully.\n\nBoot the target machine from this USB (UEFI, Secure Boot off).",
                                       "Finished", MessageBoxButtons.OK, MessageBoxIcon.Information); });
        } catch (Exception ex) {
            bool denied = ex is UnauthorizedAccessException ||
                          ex.Message.IndexOf("denied", StringComparison.OrdinalIgnoreCase) >= 0;
            string help = denied
                ? "Windows denied writing to the drive.\n\n" +
                  "• If it's an SD card, slide the write-protect (lock) switch on the side OFF.\n" +
                  "• Close any Explorer / antivirus windows using the drive, then retry.\n" +
                  "• Make sure the flasher is running as Administrator.\n\n(" + ex.Message + ")"
                : ex.Message + "\n\nClose any Explorer windows on the drive and try again.";
            Ui(() => { status.Text = "Failed: " + ex.Message;
                       MessageBox.Show(help, "Install failed", MessageBoxButtons.OK, MessageBoxIcon.Error); });
        } finally {
            if (locks != null) foreach (var h in locks) { try { h.Dispose(); } catch { } }   // remount
            Ui(() => SetBusy(false));
        }
    }

    // gzip stores the uncompressed size (mod 2^32) in the last 4 bytes (ISIZE)
    static long ReadGzipSize(Stream s)
    {
        long pos = s.Position;
        s.Seek(-4, SeekOrigin.End);
        byte[] b = new byte[4]; s.Read(b, 0, 4);
        s.Position = pos;
        return (uint)(b[0] | (b[1] << 8) | (b[2] << 16) | (b[3] << 24));
    }

    [STAThread]
    static void Main()
    {
        Application.EnableVisualStyles();
        Application.SetCompatibleTextRenderingDefault(false);
        Application.Run(new FlashForm());
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
