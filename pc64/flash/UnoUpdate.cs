/*  UnoUpdate.cs - self-update against the staged flasher on the NAS share.
 *
 *  deploy-to-share.ps1 stages every new flasher build (plus flasher-version.txt
 *  with its build stamp + sha256) on \\behemoth\unreplicated\unodos\pc64.  This
 *  checks that folder - by name first, then by IP in case DNS is down - and
 *  swaps the running exe for the staged one when it is newer.
 *
 *  This LAN share is the interim update channel; an internet-based update
 *  endpoint will join or replace it later - keep the check/apply split so only
 *  Check() has to learn HTTP.
 *
 *  The swap: a running Windows exe can be RENAMED but not deleted.  So we copy
 *  the staged exe next to ourselves (*.dl), verify its sha256, rename ourselves
 *  to *.old, move the download into our place, and start it.  The new instance
 *  deletes the *.old as soon as the old process has exited (CleanupAfterUpdate).
 *  Any failure rolls the rename back, so the original exe survives.
 */
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Reflection;
using System.Security.Cryptography;

class UpdateInfo
{
    public string BaseDir;
    public string ExePath;
    public string Build;     // yyyyMMdd-HHmmss stamp from flasher-version.txt
    public string Sha256;    // null when the share predates flasher-version.txt
    public long   Size;
}

static class UnoUpdate
{
    public const string DefaultBase  = @"\\behemoth\unreplicated\unodos\pc64";
    public const string FallbackBase = @"\\192.168.2.75\unreplicated\unodos\pc64"; // behemoth by IP, for when DNS is down

    public static string SelfPath
    {
        get { return Path.GetFullPath(Assembly.GetExecutingAssembly().Location); }
    }

    // A hand-compiled exe carries UnoVersion.Build = "0-dev" instead of a real
    // 15-char stamp; never auto-replace one of those with the share's build.
    public static bool IsDevBuild { get { return UnoVersion.Build.Length != 15; } }

    static List<string> Bases(UnoSettings s)
    {
        var all = new List<string>();
        if (!string.IsNullOrEmpty(s.UpdatePath)) all.Add(s.UpdatePath.TrimEnd('\\'));
        all.Add(DefaultBase);
        all.Add(FallbackBase);
        var seen = new List<string>(); var bases = new List<string>();
        foreach (var b in all) {
            string k = b.ToLowerInvariant();
            if (!seen.Contains(k)) { seen.Add(k); bases.Add(b); }
        }
        return bases;
    }

    // True when this process IS the copy on the share - deploy-to-share.ps1
    // owns that one, so self-update must leave it alone.
    public static bool RunningFromShare(UnoSettings s)
    {
        string dir;
        try { dir = Path.GetDirectoryName(SelfPath).TrimEnd('\\'); }
        catch { return false; }
        foreach (var b in Bases(s))
            if (string.Equals(dir, b.TrimEnd('\\'), StringComparison.OrdinalIgnoreCase))
                return true;
        return false;
    }

    /* Find the staged flasher.  File.Exists on a dead UNC path blocks for the
     * full SMB timeout, so only ever call this off the UI thread. */
    public static UpdateInfo Check(UnoSettings s, out string error)
    {
        error = null;
        var tried = new List<string>();
        foreach (var b in Bases(s)) {
            string exe = Path.Combine(b, "UnoDosFlasher.exe");
            bool found = false;
            try { found = File.Exists(exe); } catch { }
            if (!found) { tried.Add(b); continue; }

            var info = new UpdateInfo { BaseDir = b, ExePath = exe };
            try {
                string vf = Path.Combine(b, "flasher-version.txt");
                if (File.Exists(vf)) {
                    foreach (var raw in File.ReadAllLines(vf)) {
                        int eq = raw.IndexOf('=');
                        if (eq <= 0) continue;
                        string k = raw.Substring(0, eq).Trim().ToLowerInvariant();
                        string v = raw.Substring(eq + 1).Trim();
                        if      (k == "build")  info.Build  = v;
                        else if (k == "sha256") info.Sha256 = v.ToLowerInvariant();
                        else if (k == "size")   long.TryParse(v, out info.Size);
                    }
                }
            } catch { }
            if (string.IsNullOrEmpty(info.Build)) {
                // share predates flasher-version.txt - the exe's write time is
                // its build time (Copy-Item preserves it), close enough to compare
                try { info.Build = File.GetLastWriteTime(exe).ToString("yyyyMMdd-HHmmss"); }
                catch { info.Build = "00000000-000000"; }
            }
            return info;
        }
        error = "No staged flasher found. Tried:\r\n  " + string.Join("\r\n  ", tried.ToArray());
        return null;
    }

    public static bool IsNewer(UpdateInfo info)
    {
        if (IsDevBuild) return false;
        return string.CompareOrdinal(info.Build, UnoVersion.Build) > 0;   // stamps sort lexically
    }

    /* Download the staged exe next to ourselves, verify it, swap it into place
     * and start the new one.  Throws with the original exe intact on failure.
     * The caller exits the app after this returns. */
    public static void Apply(UpdateInfo info)
    {
        string self = SelfPath;
        string dl = self + ".dl", old = self + ".old";

        File.Copy(info.ExePath, dl, true);
        if (!string.IsNullOrEmpty(info.Sha256)) {
            string got = HashFile(dl);
            if (!string.Equals(got, info.Sha256, StringComparison.OrdinalIgnoreCase)) {
                try { File.Delete(dl); } catch { }
                throw new IOException(
                    "The downloaded flasher failed its checksum (a deploy may be mid-copy on the share) - try again in a minute.");
            }
        }

        if (File.Exists(old)) File.Delete(old);
        File.Move(self, old);                       // rename the running exe
        try { File.Move(dl, self); }
        catch (Exception) {
            try { File.Move(old, self); } catch { } // roll the rename back
            throw;
        }
        Process.Start(self);   // we are already elevated, so no second UAC prompt
    }

    static string HashFile(string path)
    {
        using (var sha = SHA256.Create())
        using (var fs = File.OpenRead(path)) {
            byte[] h = sha.ComputeHash(fs);
            var sb = new System.Text.StringBuilder(h.Length * 2);
            foreach (byte b in h) sb.Append(b.ToString("x2"));
            return sb.ToString();
        }
    }

    /* Every startup: delete the previous version Apply() left as *.old (the old
     * process may still be exiting, hence the retries) and any stray *.dl. */
    public static void CleanupAfterUpdate()
    {
        string self = SelfPath;
        try { if (File.Exists(self + ".dl")) File.Delete(self + ".dl"); } catch { }
        bool anyOld = false;
        try { anyOld = File.Exists(self + ".old"); } catch { }
        if (!anyOld) return;
        var t = new System.Threading.Thread(() => {
            for (int i = 0; i < 20; i++) {
                try { File.Delete(self + ".old"); return; }
                catch { System.Threading.Thread.Sleep(500); }
            }
        });
        t.IsBackground = true; t.Start();
    }
}
