#!/usr/bin/env python3
"""Export the kernel API (entry-point + data addresses) from a vasm symbol
listing into build/kernel_api.inc, which every disk-loaded .APP includes.

Mirrors the C64 port's mkapi.py: a disk app is assembled separately at its
own fixed org and references the kernel ONLY by absolute address, so an app
can `jsr draw_string`, read `vars`, poke `pat_tab`, etc. and reach the real
kernel routine/variable. The app is linked against the kernel by address.

The kernel is assembled with `vasm -L build/kernel.lst`; the listing ends with
a symbol table of `<8-hex-address> <name>` lines. We pull the names in API and
emit `name equ $ADDR` for each. A missing symbol is a hard error (an app built
against a stale/short table would jump into garbage - the build must stay
consistent, exactly the failure mode the task warns about).

Usage: mkapi.py <kernel.lst> <out.inc>
"""
import sys, re

# Kernel entry points + data addresses a disk-loaded app may use. Grouped by
# subsystem. Kept stable across kernel builds; bump deliberately.
API = [
    # ---- draw primitives (1-bit renderer) ----
    "draw_string",      # a0=str d0=x d1=y d2=color
    "draw_string_bg",   # + d3=bg color (RMW; needed for white-on-black)
    "fill_rect",        # d0=x d1=y d2=w d3=h d4=color
    "rect_outline_fg",  # d0=x d1=y d2=w d3=h (black outline)
    "draw_window",      # a2=window: full frame + content repaint
    "str_len",          # a0=str -> d0=len (preserves a0)
    # ---- window manager ----
    "redraw_topmost",   # repaint just the topmost window
    "repaint_all",      # repaint desktop + every window
    "zwin_ptr",         # d2=z index -> a2 (preserves data regs)
    "win_ptr_raw",      # d2=table index -> a2
    "launch_app",       # d0=proc index: open/raise that app's window
    # ---- FAT12 storage (sony.i + fat12.i) ----
    "fat_mount",        # mount the boot floppy's FAT12 volume -> d0
    "fat_list_root",    # cache the root directory into fat_tab/fat_count
    "fat_find_file",    # a0=11-char name -> d0=cluster/-1 d1=size
    "fat_read_file",    # d0=cluster d1=budget a1=dest -> d0=bytes/-1
    "fat_save_file",    # a0=name a1=src d1=len -> d0=0/-1
    "files_mount",      # mount + list (idempotent), sets fat_mounted
    # ---- shared format helpers ----
    "fmt_dec",          # d0.w -> decimal string at a0
    "str_append",       # append NUL string a1 to a0 (a0 -> new NUL)
    # ---- audio (snd.i; the sequencers stay kernel-side) ----
    "snd_tone",         # d0 = ProTracker period -> square tone
    "snd_off",          # silence
    "gm_start",         # game-music: a0=note table d0=count d1=owner proc
    "gm_stop",
    # ---- kernel data the apps read/write ----
    "vars",             # base of the kernel variable block
    "ticks",            # frame counter (long)
    "zcount",           # number of open windows
    "pat_tab",          # Theme: mutable dither patterns (8 bytes)
    "fat_count",        # number of cached entries
    "files_sel",        # Files: selected row
    "fat_mounted",      # volume-mounted flag
    "np_len", "np_caret", "np_top", "np_goal", "np_fatidx", "np_dirty",
    "mus_count",                         # Music note count (gen_data.i)
    "mus_ix", "mus_end", "mus_playing",
    "tk_pat", "tk_row", "tk_ch", "tk_top", "tk_prow", "tk_playing", "tk_last",
    "dt_seed", "dt_last", "dt_piece", "dt_rot", "dt_col", "dt_row",
    "dt_state", "dt_score", "dt_lines", "dt_level", "dt_next",
    "pm_statet", "pm_last", "pm_x", "pm_y",
    "pm_dir", "pm_nextdir", "pm_score", "pm_hi", "pm_lives", "pm_level",
    "pm_state", "pm_mode", "pm_modet", "pm_fright", "pm_kills", "pm_dots",
    "pm_tmp",
    "ol_z", "ol_last", "ol_lastsec", "ol_x", "ol_speed",
    "ol_state", "ol_score", "ol_time", "ol_crash", "ol_roadl", "ol_roadr",
    "ol_traf0", "ol_traf1", "ol_traf2", "ol_traf3",
    # ---- shared scratch buffers (kernel-owned; apps render through them) ----
    "numbuf", "npline", "npstat",
    # ---- audio + tracker geometry shared with the .APP UI ----
    "music_start", "music_stop", "tk_cell", "tk_periods", "tk_stop",
    "tk_trigger_row",
    # ---- shared note tables in gen_data.i (apps reach them by address) ----
    "mus_notes",
    "koro_notes", "koro_count",          # Dostris (Korobeiniki)
    "drive_notes", "drive_count",         # OutLast
    # ---- Paint scratch vars (kernel-owned) + the mouse state it polls ----
    "pt_tool", "pt_pen", "pt_lsz", "pt_ldx", "pt_err", "pt_rnd",
    "pt_px0", "pt_py0", "pt_px1", "pt_py1", "pt_band", "pt_init", "pt_chbuf",
    "mouse_x", "mouse_y", "mouse_btn",
]


def main():
    lst, out = sys.argv[1], sys.argv[2]
    addr = {}
    # symbol-table lines look like:  "0002786C pt_err"  (8 hex, space, name)
    pat = re.compile(r"^([0-9A-Fa-f]{8})\s+(\S+)\s*$")
    for line in open(lst, errors="replace"):
        m = pat.match(line)
        if m:
            addr[m.group(2)] = int(m.group(1), 16)
    missing = [n for n in API if n not in addr]
    if missing:
        sys.exit("mkapi: kernel symbols not found (stale listing?): %s"
                 % ", ".join(missing))
    with open(out, "w") as f:
        f.write("; generated by mkapi.py - kernel API addresses - do not edit\n")
        f.write("; disk-loaded apps link against the kernel by these addresses.\n")
        for n in API:
            f.write("%-16s equ $%08X\n" % (n, addr[n]))
    print("wrote %s (%d API symbols)" % (out, len(API)))


if __name__ == "__main__":
    main()
