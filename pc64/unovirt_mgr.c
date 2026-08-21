/* ===========================================================================
 * unovirt_mgr - the registry, the console ring, and start/stop.  See
 * unovirt_mgr.h for what this is for and pc64/UNOVIRT.md for the machinery
 * underneath it.
 *
 * Deliberately small and boring: a list, a text file, and a ring of lines.
 * Everything hard about running a guest is in unovirt.c and hv_vmx.c, and
 * this file exists so an APPLICATION never has to know any of it.
 * ======================================================================== */
#include "unovirt.h"
#include "unovirt_mgr.h"
#include "unovdev.h"
#include "net.h"           /* net_nic(): the link the guest shares */
#include "pc64_fs.h"
#include <stdio.h>
#include <string.h>

#define VMS_CFG "EFI\\UNODOS\\VM\\VMS.CFG"

static uno_vm_def G[UNO_VM_MAX];
static int  g_n, g_loaded;
static int  g_run = -1;
static char g_status[96] = "no appliance running";

/* ---- the console ring ----------------------------------------------------
 * Whole lines, oldest dropped.  `first` is the index of the oldest line still
 * held, which is what makes a viewer's scroll position stable: a line number
 * that counts from the start of time scrolls under the reader as the ring
 * wraps, and the reader has no way to notice. */
static char CON[UNO_VM_CON_ROWS][UNO_VM_CON_COLS];
static int  con_n;
static unsigned con_seq;

void uno_vm_con_push(const char *line)
{
    int i;
    char *d;
    if (!line) return;
    if (con_n >= UNO_VM_CON_ROWS) {
        for (i = 1; i < UNO_VM_CON_ROWS; i++)
            memcpy(CON[i - 1], CON[i], UNO_VM_CON_COLS);
        con_n = UNO_VM_CON_ROWS - 1;
    }
    d = CON[con_n++];
    for (i = 0; i + 1 < UNO_VM_CON_COLS && line[i]; i++) d[i] = line[i];
    d[i] = 0;
    con_seq++;
}

int uno_vm_con_lines(void) { return con_n; }
const char *uno_vm_con_line(int i)
{ return (i >= 0 && i < con_n) ? CON[i] : ""; }
unsigned uno_vm_con_seq(void) { return con_seq; }
void uno_vm_con_clear(void) { con_n = 0; con_seq++; }

/* Typing goes into the UART's receive FIFO, which is exactly where a real
 * keystroke would arrive.  The guest's driver is interrupt-driven and the
 * FIFO raises IRQ4 when it gains a byte (S-HV-34), so nothing else is needed
 * to wake a shell that is blocked on a read. */
void uno_vm_con_key(int ch)
{
    if (ch == '\n' || ch == '\r') { uno_vdev_serial_push('\r'); return; }
    if (ch > 0 && ch < 0x100) uno_vdev_serial_push(ch);
}

/* ---- the registry, and its file ------------------------------------------
 * One line per VM, fields separated by '|'.  A text format because a user may
 * well want to read or fix it with the editor, and because a binary blob of
 * eight structs is a migration problem the first time a field is added. */

static void trim(char *s)
{
    int n = (int)strlen(s);
    while (n && (s[n - 1] == '\r' || s[n - 1] == '\n' || s[n - 1] == ' ')) s[--n] = 0;
}

static void field(const char *src, char *dst, int cap)
{
    int i = 0;
    while (src[i] && src[i] != '|' && i + 1 < cap) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

static const char *next_field(const char *s)
{
    while (*s && *s != '|') s++;
    return *s ? s + 1 : s;
}

static void vm_load(void)
{
    static unsigned char buf[4096];
    int vol, nvol = uno_fs_volumes();
    long got = -1;
    char *p;

    if (g_loaded) return;
    g_loaded = 1;
    for (vol = 0; vol < nvol; vol++) {
        got = uno_fs_read(vol, VMS_CFG, buf, (long)sizeof buf - 1);
        if (got > 0) break;
    }
    if (got <= 0) return;
    buf[got] = 0;

    p = (char *)buf;
    while (*p && g_n < UNO_VM_MAX) {
        char line[400];
        int i = 0;
        const char *f;
        uno_vm_def d;
        while (*p && *p != '\n' && i + 1 < (int)sizeof line) line[i++] = *p++;
        if (*p == '\n') p++;
        line[i] = 0;
        trim(line);
        if (!line[0] || line[0] == '#') continue;
        memset(&d, 0, sizeof d);
        f = line;
        field(f, d.name, UNO_VM_NAME);      f = next_field(f);
        field(f, d.kernel, UNO_VM_PATH);    f = next_field(f);
        field(f, d.initrd, UNO_VM_PATH);    f = next_field(f);
        field(f, d.disk, UNO_VM_PATH);      f = next_field(f);
        {   char num[16];
            field(f, num, sizeof num);      f = next_field(f);
            d.mem_mb = 0;
            for (i = 0; num[i] >= '0' && num[i] <= '9'; i++)
                d.mem_mb = d.mem_mb * 10 + (unsigned)(num[i] - '0');
            field(f, num, sizeof num);
            d.net = num[0] == '1';
        }
        if (d.name[0]) G[g_n++] = d;
    }
}

int uno_vm_save(void)
{
    static unsigned char buf[4096];
    int i, n = 0, vol, nvol = uno_fs_volumes();
    n += snprintf((char *)buf + n, sizeof buf - (unsigned)n,
                  "# UnoDOS appliances: name|kernel|initrd|disk|mem_mb|net\n");
    for (i = 0; i < g_n && n < (int)sizeof buf - 300; i++)
        n += snprintf((char *)buf + n, sizeof buf - (unsigned)n,
                      "%s|%s|%s|%s|%u|%d\n", G[i].name, G[i].kernel,
                      G[i].initrd, G[i].disk, G[i].mem_mb, G[i].net);
    for (vol = 0; vol < nvol; vol++)
        if (uno_fs_writable(vol) && uno_fs_write(vol, VMS_CFG, buf, n)) return 1;
    return 0;
}

int uno_vm_count(void) { vm_load(); return g_n; }

const uno_vm_def *uno_vm_get(int i)
{
    vm_load();
    return (i >= 0 && i < g_n) ? &G[i] : 0;
}

int uno_vm_add(const uno_vm_def *d)
{
    vm_load();
    if (!d || g_n >= UNO_VM_MAX) return -1;
    G[g_n] = *d;
    if (!G[g_n].name[0]) snprintf(G[g_n].name, UNO_VM_NAME, "vm%d", g_n + 1);
    g_n++;
    uno_vm_save();
    return g_n - 1;
}

int uno_vm_set(int i, const uno_vm_def *d)
{
    vm_load();
    if (!d || i < 0 || i >= g_n) return 0;
    G[i] = *d;
    uno_vm_save();
    return 1;
}

int uno_vm_del(int i)
{
    int k;
    vm_load();
    if (i < 0 || i >= g_n) return 0;
    if (g_run == i) uno_vm_stop();
    for (k = i; k + 1 < g_n; k++) G[k] = G[k + 1];
    g_n--;
    if (g_run > i) g_run--;
    uno_vm_save();
    return 1;
}

/* ---- lifecycle ------------------------------------------------------------ */

const char *uno_vm_status(void) { return g_status; }
int uno_vm_running(void) { return g_run; }

/* The paths the loader should use.  An empty definition field means "the
 * built-in default", so a VM the user has not fully configured still boots
 * the appliance that is already on the disk rather than failing. */
static const char *pick(const char *s) { return (s && s[0]) ? s : ""; }
const char *uno_vm_path_kernel(void)
{ return (g_run >= 0) ? pick(G[g_run].kernel) : ""; }
const char *uno_vm_path_initrd(void)
{ return (g_run >= 0) ? pick(G[g_run].initrd) : ""; }
const char *uno_vm_path_disk(void)
{ return (g_run >= 0) ? pick(G[g_run].disk) : ""; }

/* ---- the display (A8) -----------------------------------------------------
 * Thin wrappers, because the manager surface is what APPLICATIONS consume:
 * a module links uno_vm_*, and how a framebuffer or a keystroke reaches the
 * machinery below is this file's business, not the app's. */
void *uno_vm_fb(int *w, int *h) { return uno_vmm_fb(w, h); }
void uno_vm_input_char(int ch) { uno_vdev_kbd_char(ch); }
void uno_vm_input_scan(int efi_scan) { uno_vdev_kbd_scan(efi_scan); }
void uno_vm_input_mouse(int dx, int dy, unsigned buttons, int wheel)
{ uno_vdev_mouse(dx, dy, buttons, wheel); }
int uno_vm_input_str(char *buf, int cap)
{ return uno_vdev_input_str(buf, cap); }
const char *uno_vm_progress(void) { return uno_vmm_linux_str(); }

int uno_vm_start(int i)
{
    unsigned blockers = 0;
    vm_load();
    if (i < 0 || i >= g_n) return 0;
    if (g_run >= 0) {
        snprintf(g_status, sizeof g_status, "%s is already running - stop it first",
                 G[g_run].name);
        return 0;
    }
    if (!uno_vmm_eligible(&blockers)) {
        snprintf(g_status, sizeof g_status, "cannot: %s",
                 uno_vmm_blocker_str(blockers));
        return 0;
    }
    g_run = i;                     /* set BEFORE placing: the loader asks   */
    if (!uno_vmm_place_guest()) {
        g_run = -1;
        snprintf(g_status, sizeof g_status, "%s: no kernel could be loaded",
                 G[i].name);
        return 0;
    }
    /* Put it on the wire if it asked for a network and there is one.  A NULL
     * link is not an error: the guest falls back to the synthetic peer, which
     * is exactly what every appliance had before M3. */
    if (G[i].net) uno_vnet_bridge_start(net_nic());
    uno_vm_con_clear();
    snprintf(g_status, sizeof g_status, "%s is running", G[i].name);
    return 1;
}

void uno_vm_stop(void)
{
    if (g_run < 0) return;
    uno_vnet_bridge_stop();
    uno_vmm_stop_guest();
    snprintf(g_status, sizeof g_status, "%s stopped", G[g_run].name);
    g_run = -1;
}
