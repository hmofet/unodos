/* ===========================================================================
 * UnoDOS/pc64 - module "loader": the static-link registry.
 *
 * The UEFI image is loaded whole by the firmware (like a cartridge, or the
 * classic-Mac native build), so for bring-up the 11 apps are compiled into
 * the image with DISTINCT entry symbols - each app builds with
 * -DUNO_APP_SYM=uno_app_main_<name> (the exact mechanism mac_modload.c
 * documents for single-binary targets) - and uno_load_module() resolves from
 * this table instead of storage.
 *
 * Storage-loaded .uno modules (the family's endgame on every
 * storage-equipped port) arrive with the block-driver tail: once the ESP is
 * readable natively, this file shrinks to a FAT read + relocate, and the
 * kernel goes app-free like the PS2/DC builds.
 * ===========================================================================
 */
#include "uno_app.h"

const AppInterface *uno_app_main_sysinfo(const KernelApi *k);
const AppInterface *uno_app_main_clock(const KernelApi *k);
const AppInterface *uno_app_main_files(const KernelApi *k);
const AppInterface *uno_app_main_notepad(const KernelApi *k);
const AppInterface *uno_app_main_music(const KernelApi *k);
const AppInterface *uno_app_main_dostris(const KernelApi *k);
const AppInterface *uno_app_main_outlast(const KernelApi *k);
const AppInterface *uno_app_main_pacman(const KernelApi *k);
const AppInterface *uno_app_main_tracker(const KernelApi *k);
const AppInterface *uno_app_main_paint(const KernelApi *k);
const AppInterface *uno_app_main_theme(const KernelApi *k);
const AppInterface *uno_app_main_settings(const KernelApi *k);
const AppInterface *uno_app_main_network(const KernelApi *k);
const AppInterface *uno_app_main_runner(const KernelApi *k);

static const UnoAppEntry gEntry[APP_NAPPS] = {
    uno_app_main_sysinfo,           /* APP_SYSINFO */
    uno_app_main_clock,             /* APP_CLOCK   */
    uno_app_main_files,             /* APP_FILES   */
    uno_app_main_notepad,           /* APP_NOTEPAD */
    uno_app_main_music,             /* APP_MUSIC   */
    uno_app_main_dostris,           /* APP_DOSTRIS */
    uno_app_main_outlast,           /* APP_OUTLAST */
    uno_app_main_pacman,            /* APP_PACMAN  */
    uno_app_main_tracker,           /* APP_TRACKER */
    uno_app_main_paint,             /* APP_PAINT   */
    uno_app_main_theme,             /* APP_THEME   */
    uno_app_main_settings,          /* APP_SETTINGS */
    uno_app_main_network,           /* APP_NETWORK */
    uno_app_main_runner             /* APP_RUNNER */
};

UnoAppEntry uno_load_module(short proc)
{
    if (proc < 0 || proc >= APP_NAPPS) return 0;
    return gEntry[proc];
}
