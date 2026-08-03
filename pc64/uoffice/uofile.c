/* ===========================================================================
 * uofile.c - the Office 97 Open / Save As dialog (phase 6d).
 * ======================================================================== */
#include "uofile.h"

#define MAXVOL   8
#define MAXFILE  64
#define NAMELEN  32

enum { ID_LOOKIN = 1001, ID_LIST, ID_NAME, ID_TYPE, ID_UP, ID_OPEN, ID_CANCEL };

static const uof_fs *g_fs;

/* The mutable half: the list contents change with the volume, so the item
 * table is a static this file owns and rewrites, not const data an app
 * declared.  uodlg reads it through the same const pointer either way. */
static char        g_vname[MAXVOL][NAMELEN];
static const char *g_vptr[MAXVOL];
static int         g_nvol;
static char        g_fname[MAXFILE][NAMELEN];
static const char *g_fptr[MAXFILE];
static int         g_nfile;
static int         g_shown_vol = -1;

static uod_item g_item[10];
static uod_dlg  g_dlg;
static char     g_title[32];
static int      g_result_vol;
static int      g_result_type;

static void f_cpy(char *d, const char *s, int cap)
{ int i = 0; if (!d || cap <= 0) return;
  while (s && s[i] && i < cap - 1) { d[i] = s[i]; i++; } d[i] = 0; }

void uof_set_fs(const uof_fs *fs) { g_fs = fs; }

static void load_volumes(void)
{
    int i;
    g_nvol = 0;
    if (!g_fs || !g_fs->volumes) return;
    g_nvol = g_fs->volumes();
    if (g_nvol > MAXVOL) g_nvol = MAXVOL;
    for (i = 0; i < g_nvol; i++) {
        const char *n = g_fs->volume_name ? g_fs->volume_name(i) : 0;
        f_cpy(g_vname[i], n ? n : "(volume)", NAMELEN);
        g_vptr[i] = g_vname[i];
    }
}

/* Directories sort first and wear a trailing '\', which is how Office's list
 * told them apart before it had icons for everything. */
static void load_files(int vol)
{
    int n, i, dirs = 0;
    g_nfile = 0;
    if (!g_fs || !g_fs->list_begin || !g_fs->list_get) return;
    n = g_fs->list_begin(vol);
    for (i = 0; i < n && g_nfile < MAXFILE; i++) {
        char nm[NAMELEN];
        if (!g_fs->list_get(vol, i, nm, NAMELEN)) continue;
        if (g_fs->is_dir && g_fs->is_dir(vol, nm)) {
            int j;
            for (j = g_nfile; j > dirs; j--) f_cpy(g_fname[j], g_fname[j-1], NAMELEN);
            f_cpy(g_fname[dirs], nm, NAMELEN);
            {   int L = 0; while (g_fname[dirs][L]) L++;
                if (L < NAMELEN - 2) { g_fname[dirs][L] = '\\'; g_fname[dirs][L+1] = 0; } }
            dirs++;
        } else {
            f_cpy(g_fname[g_nfile], nm, NAMELEN);
        }
        g_nfile++;
    }
    for (i = 0; i < g_nfile; i++) g_fptr[i] = g_fname[i];
}

void uof_open(uod_ui *s, int save, const char *const *types, int ntypes,
              int sw, int sh)
{
    const uoc_look *k = uoc_look_97();
    int n = 0, W = 300, row = fb_text_h() + k->pad;

    load_volumes();
    g_shown_vol = -1;
    load_files(0);
    f_cpy(g_title, save ? "Save As" : "Open", (int)sizeof g_title);

    /* Look in: */
    g_item[n].kind = UOD_LABEL; g_item[n].id = 0; g_item[n].text = "Look &in:";
    g_item[n].x = 8; g_item[n].y = 6; g_item[n].w = 60; g_item[n].h = 0;
    g_item[n].page = -1; g_item[n].flags = 0; g_item[n].list = 0;
    g_item[n].nlist = 0; g_item[n].group = 0; g_item[n].lo = 0; g_item[n].hi = 0;
    n++;
    g_item[n].kind = UOD_COMBO; g_item[n].id = ID_LOOKIN;
    g_item[n].text = g_nvol ? g_vptr[0] : "(no volumes)";
    g_item[n].x = 70; g_item[n].y = 4; g_item[n].w = 150; g_item[n].h = 0;
    g_item[n].page = -1; g_item[n].flags = 0;
    g_item[n].list = g_nvol ? g_vptr : 0; g_item[n].nlist = g_nvol;
    g_item[n].group = 0; g_item[n].lo = 0; g_item[n].hi = 0;
    n++;

    /* the file list */
    g_item[n].kind = UOD_LIST; g_item[n].id = ID_LIST; g_item[n].text = 0;
    g_item[n].x = 8; g_item[n].y = 26; g_item[n].w = W - 16; g_item[n].h = 110;
    g_item[n].page = -1; g_item[n].flags = 0;
    g_item[n].list = g_fptr; g_item[n].nlist = g_nfile;
    g_item[n].group = 0; g_item[n].lo = 0; g_item[n].hi = 0;
    n++;

    /* File name: */
    g_item[n].kind = UOD_LABEL; g_item[n].id = 0; g_item[n].text = "File &name:";
    g_item[n].x = 8; g_item[n].y = 146; g_item[n].w = 70; g_item[n].h = 0;
    g_item[n].page = -1; g_item[n].flags = 0; g_item[n].list = 0;
    g_item[n].nlist = 0; g_item[n].group = 0; g_item[n].lo = 0; g_item[n].hi = 0;
    n++;
    g_item[n].kind = UOD_EDIT; g_item[n].id = ID_NAME; g_item[n].text = "";
    g_item[n].x = 80; g_item[n].y = 144; g_item[n].w = 130; g_item[n].h = 0;
    g_item[n].page = -1; g_item[n].flags = 0; g_item[n].list = 0;
    g_item[n].nlist = 0; g_item[n].group = 0; g_item[n].lo = 0; g_item[n].hi = 0;
    n++;

    /* Files of type: */
    g_item[n].kind = UOD_LABEL; g_item[n].id = 0;
    g_item[n].text = "Files of &type:";
    g_item[n].x = 8; g_item[n].y = 168; g_item[n].w = 70; g_item[n].h = 0;
    g_item[n].page = -1; g_item[n].flags = 0; g_item[n].list = 0;
    g_item[n].nlist = 0; g_item[n].group = 0; g_item[n].lo = 0; g_item[n].hi = 0;
    n++;
    g_item[n].kind = UOD_COMBO; g_item[n].id = ID_TYPE;
    g_item[n].text = ntypes ? types[0] : "All Files (*.*)";
    g_item[n].x = 80; g_item[n].y = 166; g_item[n].w = 130; g_item[n].h = 0;
    g_item[n].page = -1; g_item[n].flags = 0;
    g_item[n].list = types; g_item[n].nlist = ntypes;
    g_item[n].group = 0; g_item[n].lo = 0; g_item[n].hi = 0;
    n++;

    /* the buttons */
    g_item[n].kind = UOD_BUTTON; g_item[n].id = UOD_ID_OK;
    g_item[n].text = save ? "&Save" : "&Open";
    g_item[n].x = 220; g_item[n].y = 144; g_item[n].w = 70; g_item[n].h = row + 4;
    g_item[n].page = -1; g_item[n].flags = UOD_DEFAULT; g_item[n].list = 0;
    g_item[n].nlist = 0; g_item[n].group = 0; g_item[n].lo = 0; g_item[n].hi = 0;
    n++;
    g_item[n].kind = UOD_BUTTON; g_item[n].id = UOD_ID_CANCEL;
    g_item[n].text = "Cancel";
    g_item[n].x = 220; g_item[n].y = 166; g_item[n].w = 70; g_item[n].h = row + 4;
    g_item[n].page = -1; g_item[n].flags = 0; g_item[n].list = 0;
    g_item[n].nlist = 0; g_item[n].group = 0; g_item[n].lo = 0; g_item[n].hi = 0;
    n++;

    g_dlg.title = g_title;
    g_dlg.item = g_item;
    g_dlg.n = n;
    g_dlg.tab = 0; g_dlg.ntab = 0;
    g_dlg.w = W;
    g_dlg.h = 200 + fb_text_h() + 12;
    g_dlg.help = 1;

    uod_open(s, &g_dlg, sw, sh);
    g_shown_vol = 0;
    g_result_vol = 0;
    g_result_type = 0;
}

void uof_sync(uod_ui *s)
{
    int vol, sel, i;
    if (!s || !s->d || s->d != &g_dlg) return;

    vol = uod_value(s, ID_LOOKIN);
    if (vol != g_shown_vol) {
        load_files(vol);
        for (i = 0; i < g_dlg.n; i++)
            if (g_item[i].id == ID_LIST) {
                g_item[i].list = g_fptr;
                g_item[i].nlist = g_nfile;
            }
        uod_set_value(s, ID_LIST, 0);
        g_shown_vol = vol;
    }
    /* a picked row mirrors into the name field, as Office's did */
    sel = uod_value(s, ID_LIST);
    if (sel >= 0 && sel < g_nfile) {
        int L = 0;
        while (g_fname[sel][L]) L++;
        if (L && g_fname[sel][L - 1] != '\\')
            uod_set_text(s, ID_NAME, g_fname[sel]);
    }
    g_result_vol  = vol;
    g_result_type = uod_value(s, ID_TYPE);
    if (uod_result(s) == UOD_ID_OK) {
        const char *t = uod_text(s, ID_NAME);
        if (t && *t) f_cpy(g_fname[MAXFILE - 1], t, NAMELEN);
    }
}

int uof_volume(void) { return g_result_vol; }
int uof_type(void)   { return g_result_type; }
const char *uof_name(void) { return g_fname[MAXFILE - 1]; }
