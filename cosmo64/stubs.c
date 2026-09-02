/* cosmo64/stubs.c -- link-satisfying stubs for the subsystems the shell calls
 * unconditionally but the Cosmo build does not carry yet.
 *
 * pc64's #ifdefs mostly gate file INTERIORS, not link dependencies (survey,
 * 2026-08-31: only UNO_ACPI / UNO_BG_CACHE / UNO_DEBUG / UNO_DBGCON are honest
 * gates), so a shell-only build satisfies the rest here. Deliberately
 * HEADER-FREE: these definitions never see the real prototypes, so argument
 * lists are whatever the caller passes (AAPCS64 makes ignoring them safe) and
 * out-parameter writers are shaped from the survey. Each block dies as its
 * real subsystem is ported: storage in M3 kills the fs block, USB+net in M4
 * kill theirs, and so on. */

typedef unsigned long long su64;

#define I0(name) int name(void) { return 0; }
#define V0(name) void name(void) { }
#define P0(name) void *name(void) { return 0; }

/* ---- module loader (fresh stub; pc64_modload_static.c is a decoy) -------- */
I0(uno_mod_scan)
P0(uno_mod_find)
I0(uno_mod_load_uui)
I0(uno_mod_load_pyrt)
I0(uno_mod_load_pyapp)
I0(uno_mod_peek_flags)
I0(uno_mod_desc_read)
V0(uno_modload_reserve)
I0(uno_mod_count)
P0(uno_mod_file)

/* ---- storage: what M3b did NOT bring in ----------------------------------
 * The block registry (blk.c), fat.c and pc64_fs.c are real as of M3b, so the
 * uno_fs_* / uno_fat_* / uno_blk_* stubs that used to sit here are gone. What
 * is left of that lane:
 *
 *  - the EFI Simple-File-System backend, which pc64_fs.c calls for volumes the
 *    firmware mounted. There is no firmware here and uno_pc64_detached() is 1,
 *    so build_map() never enumerates one and every call below is unreachable;
 *    they exist because pc64_fs.c is compiled unchanged, and they answer
 *    "nothing there" rather than 0-as-success in case that ever stops being
 *    true.
 *  - the installer, which is EFI-shaped end to end (boot entries, device
 *    paths, firmware volumes) and belongs to a later milestone.
 *  - fat.c's two outside questions: which PCI storage controller the system
 *    sits on, and whether a USB boot volume survives a detach. Neither exists
 *    on this SoC, and both only feed the detach gate, which an LK payload has
 *    already passed by being born detached. */
long uno_efifs_read(void)     { return -1; }
long uno_efifs_read_at(void)  { return -1; }
long uno_efifs_write(void)    { return 0; }
long uno_efifs_size(void)     { return -1; }
I0(uno_efifs_volumes)
I0(uno_efifs_snapshot)
I0(uno_efifs_snapshot_dir)
I0(uno_efifs_serial)
I0(uno_usbboot_native_ok)
I0(uno_usbboot_is_usb)
I0(uno_inst_scan)
P0(uno_inst_desc)
I0(uno_inst_kind)
I0(uno_inst_usable)
I0(uno_inst_install)
P0(uno_inst_error)

/* ---- network -------------------------------------------------------------
 * DIED AT M5: net.c, ax88179.c and netup.c are real, so net_init/net_poll/
 * the DHCP and counter entry points, ax88179_* and pc64_net_boot are gone
 * from here. What is left is everything the net stack and the shell's
 * Network UI reach for that this machine cannot have:
 *
 *  - the OTHER NIC families. pc64_http.c's device table names eight; seven
 *    are PCIe parts or WiFi that cannot exist on this SoC. netup.c replaces
 *    that table with the one adapter a USB host can carry, but the shell's
 *    Control Panel still asks each family whether it is present, and the
 *    honest answer is no.
 *  - unoauto's tap points, which net.c fires on every frame. The subsystem
 *    itself is a later milestone (it is what URC rides on); until then the
 *    taps fire into nothing and the deadline is "no deadline".
 *  - EFI_USB_IO, ax88179.c's OTHER transport: while firmware-attached on x86
 *    it drives the adapter through the firmware's USB stack. There is no
 *    firmware here, so uno_usbio_count() answering 0 is what selects the
 *    native uno_usb_* path. */
V0(netdisc_tick)
I0(uno_usbio_count)
I0(uno_usbio_info)
I0(uno_usbio_control)
I0(uno_usbio_bulk_eps)
I0(uno_usbio_bulk_in)
I0(uno_usbio_bulk_out)
V0(uno_dbg_net_trace)
/* -1, NOT 0: unoauto.h's contract is "0 = the budget is spent, bail out" and
 * "-1 = no deadline armed, run free". net.c polls this inside net_dns_query's
 * wait loop, so the obvious I0() stub would have aborted every DNS lookup on
 * its first iteration -- a subsystem that is absent answering as though it
 * had already run out of time. */
long unoauto_deadline_left_ms(void) { return -1; }
P0(rtl8152_nic)
P0(iwl_nic)
I0(iwl_present)
P0(iwl_mac)
P0(iwl_status_str)
I0(iwl_link_info)
I0(iwl_scan_start)
I0(iwl_scan_ready)
I0(iwl_scan_result)
I0(iwl_join_ssid)
V0(iwl_disconnect)
I0(iwl_saved_count)
P0(iwl_saved_get)
V0(iwl_progress_set)

/* ---- usb / hid ----------------------------------------------------------- *
 * xhci.c and usbhid.c are real as of M4 (usb.c, pci.c, ssusb.c). What they
 * still reach for and this build does not carry: the unodevices tree
 * (uno_devmgr.c), which uno_xhci_publish_tree() populates -- it returns
 * before touching anything when devmgr_find_class() answers 0 -- and the I2C
 * HID / detach-gate pieces of uefi_main.c. */
P0(devmgr_find_class)
I0(devmgr_count)
P0(devmgr_get)
I0(devmgr_drop_usb_children)
I0(devmgr_add_usb_dev)
I0(devmgr_add_usb_if)
I0(devmgr_bind_all)
I0(uno_i2c_hid_status)
I0(uno_i2c_hid_diag)
I0(uno_i2c_hid_timing)
P0(uno_dg_blocker)

/* ---- security / accounts (no login gate: single-user pocket machine) ----- */
I0(unosec_account_list)
V0(pc64_login_gate)
V0(pc64_consent_register)
V0(pc64_accounts_open)
V0(unoscript_app_caps_begin)
V0(unoscript_app_caps_end)

/* ---- automation / logging / transfer / virt ------------------------------ */
V0(unoauto_gate_tick)
V0(unoauto_remote_tick)
V0(unoauto_remote_boot)
V0(uno_screen_capture_tick)
V0(unoauto_hook_fire)
void unolog(void) { }
V0(unolog_tick)
V0(unostream_tick)
V0(unoxfer_job_tick)
V0(uno_vmm_tick)
V0(pc64_sshapp_open)
I0(pc64_sshapp_canvas)
V0(pc64_xferapp_open)
I0(pc64_xferapp_canvas)

/* ---- audio --------------------------------------------------------------- */
V0(uno_seq_init)
P0(uno_seq_backend)
V0(uno_seq_tick)
V0(uno_seq_stop)
I0(uno_snd_active)
P0(uno_snd_name)
I0(uno_snd_volume)
V0(uno_snd_sfx_stop_all)
V0(uno_snd_mus_tick)
V0(uno_snd_mus_stop)
V0(uno_snd_poll)
V0(unoamp_ui_build)
V0(unoamp_close)
V0(unoamp_tick)

/* ---- built-in apps not yet carried --------------------------------------- */
V0(pc64_music_build)
V0(pc64_music_action)
I0(pc64_music_key)
V0(pc64_music_tick)
V0(pc64_music_closed)
V0(pc64_browser_open)
V0(pc64_browser_open_path)
I0(pc64_browser_key)
I0(pc64_browser_canvas)
V0(pc64_game_open)
V0(pc64_game_close)
V0(pc64_game_tick)
I0(pc64_game_canvas)
I0(pc64_game_fullscreen)

/* ---- packaging (still needs the module loader, so still stubbed) --------- */
I0(uno_pkg_probe)
I0(uno_pkg_install)

/* ---- second link wave (the shell's full unresolved set) ------------------ */
V0(iwl_saved_forget)
P0(iwl_saved_psk)
I0(iwl_scan_begin)
I0(iwl_scan_results)
I0(iwl_scan_step)
V0(pc64_remote_open)
I0(uno_load_module)
I0(uno_mod_load_user)
V0(uno_mod_unload_user)
void uno_ps2_status(int *kbd, int *aux, int *auxport, int *auxid)
{
    if (kbd) *kbd = 0;
    if (aux) *aux = 0;
    if (auxport) *auxport = 0;
    if (auxid) *auxid = -1;
}
V0(uno_seq_beep)
V0(uno_seq_play)
V0(unoamp_ui_close)
V0(unoauto_hook_fire_)
/* A VARIABLE, not a function (unoauto.h: `extern int unoauto_hooks_live`).
 * The hook macro reads it as `if (unoauto_hooks_live)`, and against a
 * function definition of the same name that reads the function's address --
 * always non-zero -- so every tap point would call through to the no-op
 * instead of being skipped. net.c fires one on EVERY frame, tx and rx, so
 * this is now on the network's hot path. */
int unoauto_hooks_live;

/* ---- detach: an LK payload is born detached ------------------------------ */
int uno_pc64_detached(void) { return 1; }
void uno_pc64_detach_status(int *a, int *b, const char **why)
{
    if (a) *a = 1;
    if (b) *b = 0;
    if (why) *why = "native (LK)";
}
P0(uno_pc64_bootinfo)
