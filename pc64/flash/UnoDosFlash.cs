/*  UnoDOS USB Installer  -  a single self-contained Windows exe.
 *
 *  Picks a removable USB drive, confirms, then BUILDS a bootable UnoDOS drive
 *  on it: GPT, one EFI System Partition spanning the whole disk, formatted
 *  FAT32, with the UnoDOS system files copied in.  The system tree rides along
 *  as an embedded .zip resource.
 *
 *  It used to clone a fixed-size raw image, which left most of a modern stick
 *  unallocated - a 512 MiB image on a 32 GB stick wasted 31.5 GB and gave the
 *  OS nowhere to save documents.  Building the volume to fit the target fixes
 *  that, and makes it cheap to drop extra files on at install time (see
 *  Developer options: the media/test kit, and any .zip you choose).
 *
 *  Developer settings live in %APPDATA%\UnoDOS\flasher.ini, NOT beside the exe,
 *  because deploy-to-share.ps1 replaces the exe on the share after every build.
 *
 *  Build: pc64/flash/build-flasher.ps1  (csc + embedded esp.zip resource).
 *  Requires admin (raw disk write) - the manifest forces a UAC prompt.
 */
using System;
using System.Collections.Generic;
using System.Drawing;
using System.IO;
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
    const string ESP_RESOURCE = "unodos_esp";        // zip of the UnoDOS system tree

    ComboBox driveBox;
    Button refreshBtn, flashBtn, showAllBtn, devBtn;
    Label devSummary;
    ProgressBar progress;
    Label status;

    readonly List<UsbDrive> allDrives = new List<UsbDrive>();   // full scan, smallest-first
    bool showAll = false;                                       // include 0 GB (empty) drives?
    UnoSettings settings = UnoSettings.Load();
    int lastPct = -1;

    public FlashForm()
    {
        Text = "UnoDOS - USB Installer";
        Size = new Size(580, 430);
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
            Text = "The whole drive becomes one FAT32 volume, so UnoDOS can use all of it.\n" +
                   "UEFI boot (x86-64 PCs, 2012+). Turn OFF Secure Boot in firmware, then\n" +
                   "pick this USB from the boot menu. pc64 is UEFI-only - there is no BIOS build.",
            Location = new Point(16, 108), Size = new Size(536, 58),
            ForeColor = Color.FromArgb(60, 60, 60) });

        devBtn = new Button { Text = "Developer options...", Location = new Point(16, 172), Size = new Size(150, 26) };
        devBtn.Click += (s, e) => {
            using (var d = new DevForm(settings)) {
                if (d.ShowDialog(this) == DialogResult.OK) { settings.Save(); UpdateDevSummary(); }
                else settings = UnoSettings.Load();      // discard edits
            }
        };
        Controls.Add(devBtn);
        devSummary = new Label { Location = new Point(176, 174), Size = new Size(376, 40),
                                 ForeColor = Color.FromArgb(0, 100, 0) };
        Controls.Add(devSummary);

        Controls.Add(new Label {
            Text = "Everything on the selected drive will be erased.",
            ForeColor = Color.FromArgb(176, 0, 32),
            Location = new Point(16, 222), AutoSize = true });

        progress = new ProgressBar { Location = new Point(16, 250), Size = new Size(536, 22) };
        Controls.Add(progress);
        status = new Label { Text = "", Location = new Point(16, 282), Size = new Size(430, 34) };
        Controls.Add(status);

        flashBtn = new Button {
            Text = "Install", Location = new Point(464, 282), Size = new Size(88, 32),
            BackColor = Color.FromArgb(0, 120, 215), ForeColor = Color.White, FlatStyle = FlatStyle.Flat };
        flashBtn.Click += FlashClicked;
        Controls.Add(flashBtn);

        UpdateDevSummary();
        LoadDrives();
    }

    void UpdateDevSummary()
    {
        if (!settings.DevMode) { devSummary.Text = ""; return; }
        var bits = new List<string>();
        if (settings.KitEnabled) bits.Add("test kit: " + Short(settings.KitPath));
        if (settings.ZipEnabled) bits.Add("zip: " + Short(settings.ZipPath));
        devSummary.Text = bits.Count == 0
            ? "Developer mode on (nothing extra to copy)."
            : "Also copying - " + string.Join("; ", bits.ToArray());
    }

    static string Short(string p)
    {
        if (string.IsNullOrEmpty(p)) return "(not set)";
        return p.Length <= 44 ? p : "..." + p.Substring(p.Length - 41);
    }

    void LoadDrives()
    {
        allDrives.Clear();
        try {
            using (var searcher = new System.Management.ManagementObjectSearcher(
                "SELECT * FROM Win32_DiskDrive WHERE InterfaceType='USB'")) {
                foreach (System.Management.ManagementObject d in searcher.Get()) {
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

    // The real capacity, straight from the driver.  WMI's Size is the CHS-rounded
    // figure and can be a little short, which would waste the tail of the disk.
    static long DiskLength(SafeFileHandle disk, ulong wmiSize)
    {
        byte[] buf = new byte[8]; uint br;
        var gch = GCHandle.Alloc(buf, GCHandleType.Pinned);
        try {
            if (Native.DeviceIoControl(disk, Native.IOCTL_DISK_GET_LENGTH_INFO, IntPtr.Zero, 0,
                    gch.AddrOfPinnedObject(), 8, out br, IntPtr.Zero) && br >= 8) {
                long n = BitConverter.ToInt64(buf, 0);
                if (n > 0) return n;
            }
        } finally { gch.Free(); }
        return (long)wmiSize;
    }

    void FlashClicked(object sender, EventArgs e)
    {
        var drive = driveBox.SelectedItem as UsbDrive;
        if (drive == null) { status.Text = "Select a USB drive first."; return; }

        List<string> warn;
        var extras = settings.ExtraPayloads(out warn);
        string extraText = "";
        foreach (var s in extras) extraText += "\n    + " + s.Describe();
        if (warn.Count > 0) {
            if (MessageBox.Show(string.Join("\n", warn.ToArray()) +
                    "\n\nInstall without it?", "Developer extras missing",
                    MessageBoxButtons.YesNo, MessageBoxIcon.Warning) != DialogResult.Yes) return;
        }

        var confirm = MessageBox.Show(
            "This will ERASE everything on:\n\n    " + drive.ToString() + "\n\n" +
            "and build a full-disk FAT32 UnoDOS drive (UEFI)." +
            (extraText.Length > 0 ? "\n\nAlso copying:" + extraText : "") +
            "\n\nContinue?",
            "Confirm erase", MessageBoxButtons.YesNo, MessageBoxIcon.Warning, MessageBoxDefaultButton.Button2);
        if (confirm != DialogResult.Yes) return;

        SetBusy(true);
        lastPct = -1;
        var t = new Thread(() => DoFlash(drive)); t.IsBackground = true; t.Start();
    }

    void SetBusy(bool busy)
    {
        flashBtn.Enabled = refreshBtn.Enabled = showAllBtn.Enabled =
            driveBox.Enabled = devBtn.Enabled = !busy;
    }

    void Ui(Action a) { if (IsHandleCreated) BeginInvoke(a); }

    void DoFlash(UsbDrive drive)
    {
        List<SafeFileHandle> locks = null;
        try {
            Ui(() => status.Text = "Dismounting volumes...");
            locks = DismountVolumes(drive.Index);          // 1. free the drive's volumes

            // 2. open the raw physical drive
            var disk = Native.CreateFile(drive.DeviceId, Native.GENERIC_READ | Native.GENERIC_WRITE,
                Native.FILE_SHARE_READ | Native.FILE_SHARE_WRITE, IntPtr.Zero, Native.OPEN_EXISTING,
                Native.FILE_FLAG_WRITE_THROUGH, IntPtr.Zero);
            if (disk.IsInvalid)
                throw new IOException("Cannot open the drive (run as Administrator). Win32 error " + Marshal.GetLastWin32Error());

            long bytes = DiskLength(disk, drive.Size);

            // 3. the payload list: UnoDOS itself, then whatever developer mode adds
            var sources = new List<IPayloadSource>();
            Stream esp = Assembly.GetExecutingAssembly().GetManifestResourceStream(ESP_RESOURCE);
            if (esp == null) throw new IOException("Bundled system files ('" + ESP_RESOURCE + "') are missing from this exe.");
            sources.Add(new ZipPayload(esp, ""));
            List<string> warn;
            sources.AddRange(settings.ExtraPayloads(out warn));

            string summary;
            // bufferSize 1 = no FileStream buffering: a physical-drive handle only
            // accepts whole-sector writes, and .NET's buffering would happily
            // issue a partial one at the tail of a copy.
            using (var fs = new FileStream(disk, FileAccess.ReadWrite, 1)) {
                summary = UnoDisk.Build(fs, bytes, sources, settings.Label,
                    (stage, done, total) => {
                        int pct = total > 0 ? (int)(done * 100 / total) : 0;
                        if (pct == lastPct) return;
                        lastPct = pct;
                        Ui(() => { progress.Value = Math.Min(100, Math.Max(0, pct));
                                   status.Text = stage; });
                    });
                fs.Flush();
                // flush to the medium + re-read the partition table while the handle is
                // still open: the FileStream OWNS `disk` and closes it on dispose, so
                // touching it after the using-block would throw "Safe handle closed".
                Native.FlushFileBuffers(disk);
                uint br;
                Native.DeviceIoControl(disk, Native.IOCTL_DISK_UPDATE_PROPERTIES, IntPtr.Zero, 0, IntPtr.Zero, 0, out br, IntPtr.Zero);
            }   // FileStream disposes `disk` here

            string done2 = summary;
            Ui(() => { progress.Value = 100; status.Text = "Done - you can remove the drive.";
                       MessageBox.Show("UnoDOS was installed successfully.\n\n" + done2 +
                                       "\n\nBoot the target machine from this USB (UEFI, Secure Boot off).",
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

    [STAThread]
    static void Main()
    {
        Application.EnableVisualStyles();
        Application.SetCompatibleTextRenderingDefault(false);
        Application.Run(new FlashForm());
    }
}

/* ---- developer options ------------------------------------------------------ */
class DevForm : Form
{
    readonly UnoSettings s;
    CheckBox devChk, kitChk, zipChk;
    TextBox kitBox, zipBox, destBox, labelBox;
    Button kitBrowse, zipBrowse;

    public DevForm(UnoSettings settings)
    {
        s = settings;
        Text = "Developer options";
        Size = new Size(560, 350);
        FormBorderStyle = FormBorderStyle.FixedDialog;
        MaximizeBox = MinimizeBox = false;
        StartPosition = FormStartPosition.CenterParent;
        Font = new Font("Segoe UI", 9f);

        devChk = new CheckBox { Text = "Enable developer extras", Checked = s.DevMode,
                                Location = new Point(16, 14), AutoSize = true,
                                Font = new Font("Segoe UI", 9f, FontStyle.Bold) };
        devChk.CheckedChanged += (a, b) => Sync();
        Controls.Add(devChk);

        // --- the media / test kit -------------------------------------------
        kitChk = new CheckBox { Text = "Copy a folder onto the drive (the media test kit)",
                                Checked = s.KitEnabled, Location = new Point(16, 46), AutoSize = true };
        kitChk.CheckedChanged += (a, b) => Sync();
        Controls.Add(kitChk);
        kitBox = new TextBox { Text = s.KitPath, Location = new Point(34, 70), Size = new Size(410, 23) };
        Controls.Add(kitBox);
        kitBrowse = new Button { Text = "Browse...", Location = new Point(450, 69), Size = new Size(84, 25) };
        kitBrowse.Click += (a, b) => {
            using (var d = new FolderBrowserDialog { SelectedPath = Dir(kitBox.Text),
                       Description = "Folder whose CONTENTS are copied to the drive root" })
                if (d.ShowDialog(this) == DialogResult.OK) kitBox.Text = d.SelectedPath;
        };
        Controls.Add(kitBrowse);
        Controls.Add(new Label { Text = "The folder's contents land in the drive root.",
                                 Location = new Point(34, 96), AutoSize = true,
                                 ForeColor = Color.FromArgb(90, 90, 90) });

        // --- a chosen zip ----------------------------------------------------
        zipChk = new CheckBox { Text = "Extract a .zip onto the drive",
                                Checked = s.ZipEnabled, Location = new Point(16, 126), AutoSize = true };
        zipChk.CheckedChanged += (a, b) => Sync();
        Controls.Add(zipChk);
        zipBox = new TextBox { Text = s.ZipPath, Location = new Point(34, 150), Size = new Size(410, 23) };
        Controls.Add(zipBox);
        zipBrowse = new Button { Text = "Browse...", Location = new Point(450, 149), Size = new Size(84, 25) };
        zipBrowse.Click += (a, b) => {
            using (var d = new OpenFileDialog { Filter = "Zip archives (*.zip)|*.zip|All files (*.*)|*.*",
                                                Title = "Choose a .zip to unpack onto the drive" }) {
                if (File.Exists(zipBox.Text)) d.FileName = zipBox.Text;
                if (d.ShowDialog(this) == DialogResult.OK) zipBox.Text = d.FileName;
            }
        };
        Controls.Add(zipBrowse);
        Controls.Add(new Label { Text = "Into subfolder (blank = drive root):",
                                 Location = new Point(34, 180), AutoSize = true });
        destBox = new TextBox { Text = s.ZipDest, Location = new Point(232, 177), Size = new Size(212, 23) };
        Controls.Add(destBox);

        // --- volume label ----------------------------------------------------
        Controls.Add(new Label { Text = "Volume label:", Location = new Point(16, 216), AutoSize = true });
        labelBox = new TextBox { Text = s.Label, Location = new Point(104, 213), Size = new Size(120, 23), MaxLength = 11 };
        Controls.Add(labelBox);

        Controls.Add(new Label {
            Text = "Saved in %APPDATA%\\UnoDOS\\flasher.ini, so these survive a flasher update.",
            Location = new Point(16, 248), Size = new Size(520, 20),
            ForeColor = Color.FromArgb(90, 90, 90) });

        var ok = new Button { Text = "OK", DialogResult = DialogResult.OK,
                              Location = new Point(366, 274), Size = new Size(80, 28) };
        ok.Click += (a, b) => Commit();
        Controls.Add(ok);
        var cancel = new Button { Text = "Cancel", DialogResult = DialogResult.Cancel,
                                  Location = new Point(454, 274), Size = new Size(80, 28) };
        Controls.Add(cancel);
        AcceptButton = ok; CancelButton = cancel;

        Sync();
    }

    static string Dir(string p)
    {
        try { if (Directory.Exists(p)) return p; } catch { }
        return "";
    }

    void Sync()
    {
        bool on = devChk.Checked;
        kitChk.Enabled = zipChk.Enabled = labelBox.Enabled = on;
        kitBox.Enabled = kitBrowse.Enabled = on && kitChk.Checked;
        zipBox.Enabled = zipBrowse.Enabled = destBox.Enabled = on && zipChk.Checked;
    }

    void Commit()
    {
        s.DevMode    = devChk.Checked;
        s.KitEnabled = kitChk.Checked;
        s.KitPath    = kitBox.Text.Trim();
        s.ZipEnabled = zipChk.Checked;
        s.ZipPath    = zipBox.Text.Trim();
        s.ZipDest    = destBox.Text.Trim().Trim('\\', '/');
        string lab   = labelBox.Text.Trim();
        s.Label      = lab.Length > 0 ? lab : "UNODOS";
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
    public const uint IOCTL_DISK_GET_LENGTH_INFO = 0x0007405C;
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
