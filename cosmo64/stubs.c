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

/* ---- module loader: REAL as of M8 (pc64_modload.c, compiled with two seams
 * -- a log that reaches the eMMC and an I-cache sync; platform.c answers its
 * arena request). What it still reaches for that this payload has no answer
 * to is the EFI system table, and NULL is the answer that routes every one
 * of its allocation paths to the arena. */
P0(uno_pc64_st)

/* ---- the export table's other side ---------------------------------------
 * kExports[] in pc64_modload.c takes the ADDRESS of every function a .UNO may
 * import, so each one has to link even where the subsystem behind it does not
 * exist on this SoC. The stubs below answer "absent" in the shape the header
 * promises -- NULL for a handle, 0 or -1 for a status by that header's own
 * convention, "" for a name, an emptied buffer for an out-string -- so a
 * module that asks gets a refusal, never a 0 that reads as success. A module
 * whose import resolves to one of these loads fine and finds the feature
 * missing at run time, which is the right order: Network says "no adapter",
 * it does not fail to open. Each group dies as its subsystem is ported. */
#define S0(name) const char *name(void) { return ""; }
static void empty_str(char *buf, int cap) { if (buf && cap > 0) buf[0] = 0; }

/* unodevices: no device tree yet (devmgr_find_class above answers none) */
S0(devmgr_driver_name)
I0(devmgr_info)

/* the NIC families a USB host cannot carry (PCIe parts and radios); the one
 * adapter this box can have is the AX88179, and net.c reaches it through
 * netup.c's own table rather than these entry points */
P0(e1000_nic)  P0(e1000_mac)  P0(e1000e_nic) P0(e1000e_mac)
P0(igb_nic)    P0(igb_mac)    P0(r8169_nic)  P0(r8169_mac)
P0(rtl8152_mac)
void rtl8152_status(int *found, int *bound, int *link,
                    unsigned short *vid, unsigned short *pid, int *version)
{
    if (found) *found = 0;   if (bound) *bound = 0;   if (link) *link = 0;
    if (vid) *vid = 0;       if (pid) *pid = 0;       if (version) *version = 0;
}
I0(iwl_scan_aps)
I0(rtwifi_present)   P0(rtwifi_nic)   P0(rtwifi_mac)
I0(mrvlwifi_present) P0(mrvlwifi_nic) P0(mrvlwifi_mac)
void rtwifi_status_str(char *buf, int cap)   { empty_str(buf, cap); }
void mrvlwifi_status_str(char *buf, int cap) { empty_str(buf, cap); }

/* TLS: BearSSL is not compiled here (M5's note: it comes with the HTTP
 * stack). tls.h's convention is 0 = ok for a connect and a BR_ERR_* for the
 * error readouts, so the refusals are -1 and NULL, never 0. */
int  tls_connect(void)    { return -1; }
int  tls_connect_ca(void) { return -1; }
int  tls_read(void)       { return -1; }
int  tls_write(void)      { return -1; }
V0(tls_close)
I0(tls_cipher) I0(tls_version)
int  tls_last_error(void) { return -1; }
I0(tls_have_rdrand)
I0(tls_entropy_source)                       /* TLS_ENT_NONE */
const char *tls_entropy_name(void) { return "none"; }
P0(tls_open)
int  tls_open_error(void) { return -1; }
int  tls_poll(void)       { return -1; }
int  tls_send(void)       { return -1; }
int  tls_recv(void)       { return -1; }
V0(tls_free)
int  tls_conn_error(void) { return -1; }

/* uno3d: REAL as of the providers slice (uno3d.c + uno3d_soft.c, and
 * pc64_games.c + uno3d_game.c for Runner3D on top of them) */

/* unojs: no engine. ujs_new() answers NULL and every other entry point needs
 * the vm it would have returned, so the rest are unreachable in practice;
 * they are shaped by their headers regardless (ujs_val is one word, so a 0
 * return is a well-formed value). */
P0(ujs_new) V0(ujs_free)
I0(ujs_eval) I0(ujs_resume) I0(ujs_exception) V0(ujs_clear_exception)
const char *ujs_describe(void) { return ""; }
V0(ujs_scope_open) I0(ujs_scope_close) I0(ujs_root) V0(ujs_unroot)
I0(ujs_undefined) I0(ujs_null) I0(ujs_bool) I0(ujs_number) I0(ujs_string)
I0(ujs_object_new) I0(ujs_array_new) I0(ujs_typeof)
I0(ujs_is_undefined) I0(ujs_is_null) I0(ujs_is_number) I0(ujs_is_string)
I0(ujs_is_object) I0(ujs_is_array) I0(ujs_is_function)
double ujs_to_number(void) { return 0; }
I0(ujs_to_bool)
const char *ujs_string_bytes(void *vm, su64 v, su64 *len)
{ (void)vm; (void)v; if (len) *len = 0; return ""; }
I0(ujs_to_string) I0(ujs_get) I0(ujs_set) I0(ujs_get_index) I0(ujs_set_index)
I0(ujs_has) I0(ujs_delete) I0(ujs_array_length) I0(ujs_array_push)
I0(ujs_call) I0(ujs_function_new) I0(ujs_host_new) P0(ujs_host_user)
I0(ujs_set_fn) I0(ujs_set_accessor) I0(ujs_global)
I0(ujs_throw) I0(ujs_throw_error)
I0(ujs_fuel_used) V0(ujs_fuel_reset) V0(ujs_gc)
su64 ujs_heap_used(void) { return 0; }
I0(ujs_promise) V0(ujs_promise_resolve) V0(ujs_promise_reject)
I0(ujs_run_jobs) V0(ujs_function_set_data)

/* key bindings + app preferences: REAL (uno_binds.c) as of the providers
 * slice -- BINDS/prefs persist on the SD card beside SHELL.CFG */

/* unopkg: the two entries a foreign-app shim may import */
I0(uno_pkg_launch)
void uno_pkg_runtime_str(const char *target, char *buf, int max) { (void)target; empty_str(buf, max); }

/* sampled audio + the sequencer's readout: no audio path on this SoC yet */
I0(uno_seq_playing)
I0(uno_snd_sfx_load) I0(uno_snd_sfx_play) I0(uno_snd_sfx_playing)
I0(uno_snd_mus_play) I0(uno_snd_mus_playing)

/* unovirt: no hypervisor (GenieZone holds EL2's virtualisation; the payload
 * runs there but cannot start guests). The manager surface reports an empty
 * roster and refuses to add to it. */
I0(uno_vm_count) P0(uno_vm_get)
int  uno_vm_add(void) { return -1; }
I0(uno_vm_set) I0(uno_vm_del) I0(uno_vm_save) I0(uno_vm_start) V0(uno_vm_stop)
int  uno_vm_running(void) { return -1; }
S0(uno_vm_status) S0(uno_vm_progress)
I0(uno_vm_con_lines) S0(uno_vm_con_line) I0(uno_vm_con_seq)
V0(uno_vm_con_key) V0(uno_vm_con_clear)
P0(uno_vm_fb) V0(uno_vm_input_char) V0(uno_vm_input_scan) V0(uno_vm_input_mouse)
I0(uno_vm_input_str) V0(uno_vm_focus_display)

/* unolog: REAL (unolog.c) as of the providers slice; platform.c calls
 * unolog_init() after the storage report, as uefi_main.c does */

/* unoscript: the production scripting surface. unoscript.c is adjudicated by
 * unosecure, which has no store here (see the accounts block below), so the
 * runtime reports itself absent and every capability check refuses. */
I0(unoscript_available) S0(unoscript_cap_name) I0(unoscript_cap_tier)
I0(unosec_present) I0(unosec_require)
I0(usc_ui_pointer) I0(usc_ui_key)
int  usc_ui_screen_text(char *out, int cap)   { empty_str(out, cap); return 0; }
int  usc_ui_clipboard_get(char *out, int cap) { empty_str(out, cap); return 0; }
I0(usc_ui_clipboard_set)
I0(usc_app_count) I0(usc_app_launch) I0(usc_app_close_top) I0(usc_app_message)
I0(usc_fs_read) I0(usc_fs_write)
I0(usc_proc_list) I0(usc_mem_read) I0(usc_mem_write)
I0(usc_io_in) I0(usc_io_out) I0(usc_power)
I0(usc_hook_add) V0(usc_hook_remove)

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
 *  - EFI_USB_IO, ax88179.c's OTHER transport: while firmware-attached on x86
 *    it drives the adapter through the firmware's USB stack. There is no
 *    firmware here, so uno_usbio_count() answering 0 is what selects the
 *    native uno_usb_* path.
 * unoauto itself (the tap points, the deadline, netdisc) DIED AT M6: the real
 * files compile in, and urc.c carries what they reach for. */
I0(uno_usbio_count)
I0(uno_usbio_info)
I0(uno_usbio_control)
I0(uno_usbio_bulk_eps)
I0(uno_usbio_bulk_in)
I0(uno_usbio_bulk_out)
P0(rtl8152_nic)
P0(iwl_nic)
I0(iwl_present)
/* the `iwl` URC verb's F12 debug hook: -1 = "NIC not mapped", its own
 * contract for a machine with no Intel radio (this one has no radio at all) */
int iwl_dbg_cmd(void) { return -1; }
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
/* The unosecure calls the URC gate makes (M6). unosecure.c itself compiles
 * here, but it is a store on a FAT volume and there is none, so every
 * session-shaped answer is "no session" -- which is exactly what keeps the
 * gate's production arming path fail-CLOSED (unoauto_gate_arm refuses with
 * no session) while the debug/urc-auth arm, which has no session by design,
 * is untouched: unoauto_gate_tick only consults unosec_session_valid when a
 * console session was recorded, and that arm records none. The `py` verb
 * enters the link session only if one exists (0 = none). */
I0(unosec_session_valid)
I0(unosec_current_user)
V0(unosec_audit)
I0(unosec_request)
I0(unosec_session_open)
const char *unosec_account_name(void) { return ""; }
V0(unosec_drop)
V0(unosec_logout)
I0(unosec_enter_session)
V0(unosec_leave)
I0(unosec_current_session)
V0(pc64_remote_open)

/* ---- logging / transfer / virt (unoauto + URC are real as of M6) --------- */
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
/* (pc64_games.c is real now: Runner3D on the soft rasteriser) */

/* ---- packaging (pc64_pkg.c: the installer side, not carried) ------------- */
I0(uno_pkg_probe)
I0(uno_pkg_install)

/* ---- second link wave (the shell's full unresolved set) ------------------ */
V0(iwl_saved_forget)
P0(iwl_saved_psk)
I0(iwl_scan_begin)
I0(iwl_scan_results)
I0(iwl_scan_step)
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

/* ---- detach: an LK payload is born detached ------------------------------ */
int uno_pc64_detached(void) { return 1; }
void uno_pc64_detach_status(int *a, int *b, const char **why)
{
    if (a) *a = 1;
    if (b) *b = 0;
    if (why) *why = "native (LK)";
}
P0(uno_pc64_bootinfo)
