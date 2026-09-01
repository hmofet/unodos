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

/* ---- filesystems / storage / installer (dies at M3) ---------------------- */
I0(uno_fs_volumes)
int uno_fs_read(void) { return -1; }
int uno_fs_write(void) { return -1; }
I0(uno_fs_kind)
I0(uno_fs_writable)
I0(uno_fs_is_boot)
I0(uno_fat_volumes)
P0(uno_fat_label)
I0(uno_fat_native_status)
I0(uno_blk_count)
P0(uno_blk_get)
I0(uno_inst_scan)
P0(uno_inst_desc)
I0(uno_inst_kind)
I0(uno_inst_usable)
I0(uno_inst_install)
P0(uno_inst_error)

/* ---- network (dies at M4) ------------------------------------------------ */
V0(net_init)
V0(net_poll)
I0(net_link)
I0(net_ip)
I0(net_gw)
V0(net_dhcp_start)
I0(net_dhcp_done)
I0(net_tx_frames)
I0(net_rx_frames)
I0(net_link_speed_mbps)
V0(netdisc_tick)
I0(pc64_net_boot)
P0(ax88179_nic)
I0(ax88179_status)
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

/* ---- usb / hid ----------------------------------------------------------- */
V0(uno_xhci_init)
I0(uno_xhci_status)
I0(uno_xhci_diag)
I0(uno_xhci_diag2)
P0(uno_xhci_dev)
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
V0(pc64_write_build)
V0(pc64_write_action)
I0(pc64_write_key)
V0(pc64_write_frame)
I0(pc64_write_canvas_index)
V0(pc64_files_build)
V0(pc64_files_action)
I0(pc64_files_canvas_index)
V0(pc64_music_build)
V0(pc64_music_action)
I0(pc64_music_key)
V0(pc64_music_tick)
V0(pc64_music_closed)
V0(pc64_clock_build)
V0(pc64_clock_action)
V0(pc64_clock_tick)
V0(pc64_browser_open)
V0(pc64_browser_open_path)
I0(pc64_browser_key)
I0(pc64_browser_canvas)
V0(pc64_game_open)
V0(pc64_game_close)
V0(pc64_game_tick)
I0(pc64_game_canvas)
I0(pc64_game_fullscreen)

/* ---- second link wave (the shell's full unresolved set) ------------------ */
V0(iwl_saved_forget)
P0(iwl_saved_psk)
I0(iwl_scan_begin)
I0(iwl_scan_results)
I0(iwl_scan_step)
V0(pc64_remote_open)
I0(uno_fs_list_begin)
I0(uno_fs_list_get)
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
I0(unoauto_hooks_live)

/* ---- detach: an LK payload is born detached ------------------------------ */
int uno_pc64_detached(void) { return 1; }
void uno_pc64_detach_status(int *a, int *b, const char **why)
{
    if (a) *a = 1;
    if (b) *b = 0;
    if (why) *why = "native (LK)";
}
P0(uno_pc64_bootinfo)
