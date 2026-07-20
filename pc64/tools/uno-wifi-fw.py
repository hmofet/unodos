#!/usr/bin/env python3
# ===========================================================================
# uno-wifi-fw - put the Intel WiFi firmware onto an UnoDOS/pc64 USB stick.
#
# UnoDOS's iwlwifi driver loads the Intel firmware from FIRMWARE\IWL*.UCO on the
# ESP, but Intel's licence forbids us bundling it in the OS.  This tool fetches
# the blob from a Debian repo (the same non-redistributable package a Debian box
# installs), or copies it from a Linux machine's /lib/firmware, and drops it onto
# the stick under the exact name the driver expects - plus a starter WIFI.CFG.
#
# Runs on Windows, macOS and Linux with just the Python 3 standard library
# (the .deb is unpacked in-process: ar + tar + lzma, all stdlib).  No install.
#
#   python3 uno-wifi-fw.py                 # auto-detect card + USB, interactive
#   python3 uno-wifi-fw.py --card ax201 --dest E:\      (Windows)
#   python3 uno-wifi-fw.py --card ax210 --dest /Volumes/UNODOS   (macOS)
#   python3 uno-wifi-fw.py --list-cards
#   python3 uno-wifi-fw.py --source local  # copy from this machine's /lib/firmware
# ===========================================================================
import argparse, io, lzma, gzip, os, re, sys, tarfile, tempfile, shutil
import urllib.request

DEB_POOL = "http://ftp.debian.org/debian/pool/non-free-firmware/f/firmware-nonfree/"

# card key -> (list of upstream-file globs newest-wins, dest .UCO name, optional .pnvm)
# The globs match the linux-firmware / Debian file names; the dest name is what
# the UnoDOS iwlwifi driver opens under FIRMWARE\ on the ESP.
CARDS = {
    "7260":  ([r"iwlwifi-7260-(\d+)\.ucode"],                 "IWL7260.UCO", None),
    "7265d": ([r"iwlwifi-7265D-(\d+)\.ucode"],                "IWL7265D.UCO", None),
    "3168":  ([r"iwlwifi-3168-(\d+)\.ucode"],                 "IWL3168.UCO", None),
    "8265":  ([r"iwlwifi-8265-(\d+)\.ucode",
               r"iwlwifi-8000C-(\d+)\.ucode"],                "IWL8000.UCO", None),
    "9260":  ([r"iwlwifi-9260-th-b0-jf-b0-(\d+)\.ucode"],     "IWL9260.UCO", None),
    "9560":  ([r"iwlwifi-9000-pu-b0-jf-b0-(\d+)\.ucode"],     "IWL9000.UCO", None),
    "ax200": ([r"iwlwifi-cc-a0-(\d+)\.ucode"],                "IWLAX200.UCO", None),
    # AX201 (X1 Carbon Gen 8 etc.) is a Qu/QuZ CNVi part; ship whichever stepping
    # the repo has, preferring QuZ then Qu-b0 (the driver loads one IWLAX201.UCO).
    "ax201": ([r"iwlwifi-QuZ-a0-hr-b0-(\d+)\.ucode",
               r"iwlwifi-Qu-b0-hr-b0-(\d+)\.ucode",
               r"iwlwifi-Qu-c0-hr-b0-(\d+)\.ucode"],          "IWLAX201.UCO", None),
    "ax210": ([r"iwlwifi-ty-a0-gf-a0-(\d+)\.ucode"],          "IWLAX210.UCO",
              "iwlwifi-ty-a0-gf-a0.pnvm"),  # AX210 also needs the .pnvm
}
CARD_HELP = {
    "7260": "Intel 7260/7265", "7265d": "Intel 7265D", "3168": "Intel 3168",
    "8265": "Intel 8260/8265", "9260": "Intel 9260", "9560": "Intel 9461/9462/9560",
    "ax200": "Intel Wi-Fi 6 AX200 (discrete)",
    "ax201": "Intel Wi-Fi 6 AX201 (CNVi - most 2020+ laptops, incl. X1 Carbon Gen 8)",
    "ax210": "Intel Wi-Fi 6E AX210/AX211",
}

def log(*a): print(*a, file=sys.stderr)

# ---- fetch + unpack the Debian firmware package (pure stdlib) --------------
def http_get(url):
    req = urllib.request.Request(url, headers={"User-Agent": "uno-wifi-fw"})
    with urllib.request.urlopen(req, timeout=60) as r:
        return r.read()

def latest_deb_url():
    html = http_get(DEB_POOL).decode("utf-8", "replace")
    debs = re.findall(r'firmware-iwlwifi_([0-9][0-9a-z.~+-]*)_all\.deb', html)
    stable = [v for v in debs if "bpo" not in v and "~" not in v] or debs
    if not stable:
        raise SystemExit("could not find firmware-iwlwifi in the Debian pool")
    best = sorted(set(stable))[-1]     # YYYYMMDD-N sorts correctly as a string
    return DEB_POOL + "firmware-iwlwifi_%s_all.deb" % best, best

def ar_members(blob):
    """Yield (name, bytes) for each member of a Unix `ar` archive (a .deb)."""
    if blob[:8] != b"!<arch>\n":
        raise SystemExit("not an ar archive (bad .deb)")
    off = 8
    while off + 60 <= len(blob):
        hdr = blob[off:off+60]; off += 60
        name = hdr[0:16].decode("ascii", "replace").strip()
        size = int(hdr[48:58].decode("ascii", "replace").strip() or "0")
        data = blob[off:off+size]; off += size + (size & 1)
        yield name.rstrip("/"), data

def deb_data_tar(deb_blob):
    for name, data in ar_members(deb_blob):
        if name.startswith("data.tar"):
            if name.endswith(".xz"):  return tarfile.open(fileobj=io.BytesIO(lzma.decompress(data)))
            if name.endswith(".gz"):  return tarfile.open(fileobj=io.BytesIO(gzip.decompress(data)))
            if name.endswith(".zst"):
                try:
                    import zstandard  # optional; only needed if Debian switches to zstd
                    dctx = zstandard.ZstdDecompressor()
                    return tarfile.open(fileobj=io.BytesIO(dctx.decompress(data, max_output_size=1<<30)))
                except ImportError:
                    raise SystemExit("this .deb uses zstd; `pip install zstandard` or use --source local")
            if name.endswith(".tar"): return tarfile.open(fileobj=io.BytesIO(data))
    raise SystemExit("no data.tar.* inside the .deb")

def newest(matches):
    """From [(apiver:int, member)] pick the highest api version."""
    return max(matches, key=lambda m: m[0])[1] if matches else None

def fetch_from_deb(card, outdir):
    globs, dest_name, pnvm = CARDS[card]
    url, ver = latest_deb_url()
    log("Downloading %s ..." % url.rsplit("/", 1)[-1])
    tar = deb_data_tar(http_get(url))
    # the real files live under ./usr/lib/firmware/intel/iwlwifi/ (root names are symlinks)
    members = tar.getmembers()
    def find(glob):
        rx = re.compile(r"(?:^|/)" + glob + r"$")
        hits = []
        for m in members:
            if not m.isfile(): continue
            mm = rx.search(m.name)
            if mm: hits.append((int(mm.group(1)), m))
        return newest(hits)
    chosen = None
    for g in globs:
        chosen = find(g)
        if chosen: break
    if not chosen:
        raise SystemExit("firmware for '%s' not found in %s" % (card, url.rsplit('/',1)[-1]))
    out_uco = os.path.join(outdir, dest_name)
    with open(out_uco, "wb") as f: f.write(tar.extractfile(chosen).read())
    log("  %s  <-  %s  (Debian %s)" % (dest_name, os.path.basename(chosen.name), ver))
    result = [out_uco]
    if pnvm:
        pm = re.compile(r"(?:^|/)" + re.escape(pnvm) + r"$")
        for m in members:
            if m.isfile() and pm.search(m.name):
                out_pnv = os.path.join(outdir, dest_name.rsplit(".",1)[0] + ".PNV")
                with open(out_pnv, "wb") as f: f.write(tar.extractfile(m).read())
                log("  %s  <-  %s" % (os.path.basename(out_pnv), os.path.basename(m.name)))
                result.append(out_pnv); break
    return result

def fetch_from_local(card, outdir):
    globs, dest_name, pnvm = CARDS[card]
    roots = ["/lib/firmware", "/usr/lib/firmware", "/usr/lib/firmware/intel/iwlwifi"]
    files = []
    for r in roots:
        if os.path.isdir(r):
            for fn in os.listdir(r): files.append((r, fn))
    def find(glob):
        rx = re.compile("^" + glob + "$"); hits = []
        for r, fn in files:
            mm = rx.match(fn)
            if mm: hits.append((int(mm.group(1)), os.path.join(r, fn)))
        return max(hits, key=lambda h: h[0])[1] if hits else None
    src = None
    for g in globs:
        src = find(g)
        if src: break
    if not src:
        raise SystemExit("no local firmware for '%s' under /lib/firmware (try --source debian)" % card)
    out_uco = os.path.join(outdir, dest_name); shutil.copyfile(src, out_uco)
    log("  %s  <-  %s (local)" % (dest_name, src)); result = [out_uco]
    if pnvm:
        for r in roots:
            p = os.path.join(r, pnvm)
            if os.path.isfile(p):
                out_pnv = os.path.join(outdir, dest_name.rsplit(".",1)[0] + ".PNV")
                shutil.copyfile(p, out_pnv); result.append(out_pnv)
                log("  %s  <-  %s (local)" % (os.path.basename(out_pnv), p)); break
    return result

# ---- find the UnoDOS USB (a removable FAT volume, ideally with the OS on it) -
def is_unodos_volume(path):
    return os.path.isfile(os.path.join(path, "EFI", "BOOT", "BOOTX64.EFI"))

def candidate_volumes():
    vols = []
    if sys.platform.startswith("win"):
        import ctypes
        k32 = ctypes.windll.kernel32
        bitmask = k32.GetLogicalDrives()
        for i in range(26):
            if not (bitmask >> i) & 1: continue
            root = "%s:\\" % chr(ord('A') + i)
            if k32.GetDriveTypeW(ctypes.c_wchar_p(root)) == 2:   # DRIVE_REMOVABLE
                vols.append(root)
    elif sys.platform == "darwin":
        for name in sorted(os.listdir("/Volumes")):
            p = os.path.join("/Volumes", name)
            if os.path.ismount(p) and os.access(p, os.W_OK): vols.append(p)
    else:  # linux
        user = os.environ.get("USER") or os.environ.get("LOGNAME") or ""
        for base in ("/run/media/%s" % user, "/media/%s" % user, "/media", "/mnt"):
            if os.path.isdir(base):
                for name in sorted(os.listdir(base)):
                    p = os.path.join(base, name)
                    if os.path.ismount(p) and os.access(p, os.W_OK): vols.append(p)
    # UnoDOS sticks (with the OS on them) sort first
    vols.sort(key=lambda v: (not is_unodos_volume(v), v))
    return vols

def pick_volume():
    vols = candidate_volumes()
    if not vols:
        raise SystemExit("No removable disk found. Plug in the UnoDOS USB, or pass --dest <path>.")
    if len(vols) == 1 and is_unodos_volume(vols[0]):
        log("Using UnoDOS USB at %s" % vols[0]); return vols[0]
    log("Removable volumes:")
    for i, v in enumerate(vols):
        tag = "  <- UnoDOS OS found" if is_unodos_volume(v) else ""
        log("  [%d] %s%s" % (i + 1, v, tag))
    while True:
        s = input("Pick the UnoDOS USB [1-%d], or q to quit: " % len(vols)).strip()
        if s.lower() == "q": raise SystemExit(1)
        if s.isdigit() and 1 <= int(s) <= len(vols): return vols[int(s) - 1]

# ---- best-effort card auto-detect -----------------------------------------
def detect_card():
    try:
        if sys.platform.startswith("linux"):
            base = "/sys/bus/pci/devices"
            for d in os.listdir(base):
                v = open(os.path.join(base, d, "vendor")).read().strip()
                dev = open(os.path.join(base, d, "device")).read().strip().lower()
                if v != "0x8086": continue
                m = {"0x2723": "ax200", "0x02f0": "ax201", "0x4df0": "ax201",
                     "0x34f0": "ax201", "0xa0f0": "ax201", "0x43f0": "ax201",
                     "0x2725": "ax210", "0x7a70": "ax210", "0x7af0": "ax210",
                     "0x2526": "9560", "0x30dc": "9560", "0x31dc": "9560",
                     "0x9df0": "9560", "0xa370": "9560",
                     "0x24fd": "8265", "0x24f3": "8265",
                     "0x08b1": "7260", "0x095a": "7265d"}.get(dev)
                if m: return m
        elif sys.platform.startswith("win"):
            import subprocess
            out = subprocess.run(["powershell", "-NoProfile", "-Command",
                    "Get-PnpDevice -Class Net | Select-Object -ExpandProperty FriendlyName"],
                    capture_output=True, text=True, timeout=20).stdout
            for key in ("AX211","AX210","AX201","AX200","9560","9462","9461","9260","8265","7265","7260","3168"):
                if key in out:
                    return {"AX211":"ax210","AX210":"ax210","AX201":"ax201","AX200":"ax200",
                            "9560":"9560","9462":"9560","9461":"9560","9260":"9260",
                            "8265":"8265","7265":"7265d","7260":"7260","3168":"3168"}[key]
    except Exception:
        pass
    return None

# ---- main -----------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(description="Put Intel WiFi firmware on an UnoDOS USB.")
    ap.add_argument("--card", choices=sorted(CARDS), help="Intel card (default: auto-detect, else ax201)")
    ap.add_argument("--dest", help="path to the UnoDOS USB (e.g. E:\\ or /Volumes/UNODOS)")
    ap.add_argument("--source", choices=["debian", "local"], default="debian",
                    help="debian repo (default) or this machine's /lib/firmware")
    ap.add_argument("--list-cards", action="store_true", help="list supported cards and exit")
    ap.add_argument("--no-cfg", action="store_true", help="don't write a starter WIFI.CFG")
    args = ap.parse_args()

    if args.list_cards:
        for k in sorted(CARDS): print("  %-7s %s" % (k, CARD_HELP.get(k, "")))
        return

    card = args.card or detect_card()
    if not card:
        log("Could not auto-detect the card; defaulting to ax201 (override with --card).")
        card = "ax201"
    log("Card: %s (%s)" % (card, CARD_HELP.get(card, "")))

    dest = args.dest or pick_volume()
    if not os.path.isdir(dest):
        raise SystemExit("destination not found: %s" % dest)
    if not is_unodos_volume(dest):
        log("WARNING: %s has no EFI\\BOOT\\BOOTX64.EFI - is this the UnoDOS stick?" % dest)

    fwdir = os.path.join(dest, "FIRMWARE")
    os.makedirs(fwdir, exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        got = (fetch_from_local if args.source == "local" else fetch_from_deb)(card, tmp)
        for src in got:
            shutil.copyfile(src, os.path.join(fwdir, os.path.basename(src)))

    cfg = os.path.join(dest, "WIFI.CFG")
    if not args.no_cfg and not os.path.isfile(cfg):
        with open(cfg, "w", newline="\r\n") as f:
            f.write("# UnoDOS WiFi config - edit these two lines with your network\n")
            f.write("ssid=YourNetwork\n"); f.write("psk=YourPassphrase\n")
        log("Wrote a starter %s - edit it with your SSID and passphrase." % cfg)

    log("")
    log("Done. Firmware is on the stick under FIRMWARE\\  (%s)." % ", ".join(os.path.basename(x) for x in got))
    log("Edit WIFI.CFG with your network, then boot UnoDOS and open Network > Connect WiFi.")

if __name__ == "__main__":
    main()
