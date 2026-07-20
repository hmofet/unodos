/*  UnoSettings.cs - developer settings that outlive the flasher binary.
 *
 *  deploy-to-share.ps1 overwrites UnoDosFlasher.exe on the share after every
 *  pc64 build, and people run it straight from \\behemoth, so anything stored
 *  beside the exe - or embedded in it - is gone the next time you flash.
 *  These live in %APPDATA%\UnoDOS\flasher.ini instead, which no build touches.
 *
 *  Deliberately a hand-rolled key=value file: the flasher compiles with the
 *  in-box csc against .NET Framework, and a JSON serializer would drag in a
 *  reference the build does not otherwise need.  Unknown keys are preserved on
 *  save, so an older flasher cannot silently drop a newer one's settings.
 */
using System;
using System.Collections.Generic;
using System.IO;
using System.Text;

class UnoSettings
{
    public const int VERSION = 1;

    public bool   DevMode;            // show + apply the developer extras
    public bool   KitEnabled;
    public string KitPath   = @"\\behemoth\unreplicated\unodos\pc64\testkit";
    public bool   ZipEnabled;
    public string ZipPath   = "";
    public string ZipDest   = "";     // subfolder on the disk, blank = root
    public string Label     = "UNODOS";
    public bool   AutoUpdate = true;  // check the share + self-update at startup
    public string UpdatePath = "";    // ini-only override of the update share dir

    readonly Dictionary<string, string> extra = new Dictionary<string, string>();

    public static string Dir
    {
        get {
            return Path.Combine(
                Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "UnoDOS");
        }
    }
    public static string FilePath { get { return Path.Combine(Dir, "flasher.ini"); } }

    public static UnoSettings Load()
    {
        var s = new UnoSettings();
        try {
            if (!File.Exists(FilePath)) return s;
            foreach (var raw in File.ReadAllLines(FilePath)) {
                string line = raw.Trim();
                if (line.Length == 0 || line[0] == '#' || line[0] == ';') continue;
                int eq = line.IndexOf('=');
                if (eq <= 0) continue;
                string k = line.Substring(0, eq).Trim(), v = line.Substring(eq + 1).Trim();
                switch (k.ToLowerInvariant()) {
                    case "version":     break;                       // informational
                    case "devmode":     s.DevMode    = Truthy(v); break;
                    case "kitenabled":  s.KitEnabled = Truthy(v); break;
                    case "kitpath":     s.KitPath    = v;         break;
                    case "zipenabled":  s.ZipEnabled = Truthy(v); break;
                    case "zippath":     s.ZipPath    = v;         break;
                    case "zipdest":     s.ZipDest    = v;         break;
                    case "label":       if (v.Length > 0) s.Label = v; break;
                    case "autoupdate":  s.AutoUpdate = Truthy(v); break;
                    case "updatepath":  s.UpdatePath = v;         break;
                    default:            s.extra[k] = v;           break;
                }
            }
        } catch { /* a corrupt settings file must never stop someone flashing */ }
        return s;
    }

    public void Save()
    {
        try {
            Directory.CreateDirectory(Dir);
            var sb = new StringBuilder();
            sb.AppendLine("# UnoDOS flasher settings - kept out of the exe so it survives an update.");
            sb.AppendLine("version="    + VERSION);
            sb.AppendLine("devmode="    + (DevMode ? "1" : "0"));
            sb.AppendLine("kitenabled=" + (KitEnabled ? "1" : "0"));
            sb.AppendLine("kitpath="    + KitPath);
            sb.AppendLine("zipenabled=" + (ZipEnabled ? "1" : "0"));
            sb.AppendLine("zippath="    + ZipPath);
            sb.AppendLine("zipdest="    + ZipDest);
            sb.AppendLine("label="      + Label);
            sb.AppendLine("autoupdate=" + (AutoUpdate ? "1" : "0"));
            sb.AppendLine("updatepath=" + UpdatePath);
            foreach (var kv in extra) sb.AppendLine(kv.Key + "=" + kv.Value);
            File.WriteAllText(FilePath, sb.ToString(), Encoding.UTF8);
        } catch { }
    }

    static bool Truthy(string v)
    {
        v = v.ToLowerInvariant();
        return v == "1" || v == "true" || v == "yes" || v == "on";
    }

    /* What the settings actually add to a flash, in order.  Returns the reason a
     * source was skipped rather than throwing: a missing kit folder should be a
     * visible warning, not a failed install. */
    public List<IPayloadSource> ExtraPayloads(out List<string> warnings)
    {
        var list = new List<IPayloadSource>();
        warnings = new List<string>();
        if (!DevMode) return list;

        if (KitEnabled) {
            if (Directory.Exists(KitPath)) list.Add(new FolderPayload(KitPath, ""));
            else warnings.Add("Test kit folder not found: " + KitPath);
        }
        if (ZipEnabled) {
            if (File.Exists(ZipPath)) list.Add(new ZipPayload(ZipPath, ZipDest));
            else warnings.Add("Zip not found: " + ZipPath);
        }
        return list;
    }
}
