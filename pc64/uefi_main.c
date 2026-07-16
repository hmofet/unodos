/* ===========================================================================
 * UnoDOS/pc64 platform layer (UEFI) - GOP present + firmware input.
 *
 * The modern-PC analogue of the family's firmware-hosted worlds: the Pi asks
 * the VideoCore firmware for a framebuffer, the PowerPC Mac asks Open
 * Firmware, and pc64 asks UEFI - GOP hands the kernel a linear 32bpp
 * framebuffer, Simple Text Input (Ex) hands it the keyboard, and the
 * Simple/Absolute Pointer protocols hand it mice/tablets where the firmware
 * binds them. Boot services stay alive - UEFI *is* this port's BIOS, exactly
 * the role INT 10h/13h/15h play for the 16-bit x86 reference.
 * ExitBootServices + native xHCI/NVMe/e1000 drivers are the documented
 * driver tail, not a bring-up requirement.
 *
 * The portable core (unodos.c, -Dmain=uno_main) drives three hooks:
 *   uno_pc64_init()    - GOP geometry (native mode KEPT - see below),
 *                        driver connect sweep, input protocols.
 *   uno_pc64_poll()    - keyboard + every pointer instance -> shim events.
 *   uno_pc64_present() - the 640x480 software fb, integer-SCALED and centred
 *                        into the native mode, cursor composited at fb
 *                        resolution; ~60Hz Stall pacing.
 *
 * Real-hardware lessons baked in (first validated on a Lenovo X1 Carbon G8):
 *   - NO SetMode at boot: laptop eDP panels accept 640x480 at the API level
 *     and then stop scanning out. The native mode is the only safe mode; the
 *     desktop scales by integer factors instead (F9 cycles, F10 cycles GOP
 *     modes for external monitors).
 *   - NO port-0x402 debug writes unless -DUNO_DBGCON: legacy-port I/O can be
 *     SMM-trapped, and a firmware SMI handler that mishandles it hangs the
 *     machine. QEMU-only plumbing must never run on metal.
 *   - Input drains are BUDGETED: firmware exists whose ReadKeyStrokeEx
 *     returns SUCCESS forever with phantom keystroke data.
 *
 * Audio is the PC speaker: PIT channel 2 via port I/O - the same square-wave
 * voice as the x86 reference kernel's API 41/42.
 * ===========================================================================
 */
#include "uefi.h"
#include "fb.h"
#include "mac_compat.h"

int uno_main(void);                 /* the portable core (-Dmain=uno_main) */
void uno_screen_changed(void);      /* core hook: resolution changed (unodos.c) */

/* the RUNTIME desktop size (fb.h's FB_W/FB_H resolve to these on pc64) */
int uno_fb_w = 640, uno_fb_h = 480;

/* ---- firmware handles ---------------------------------------------------- */
static EFI_SYSTEM_TABLE                  *gST;
static EFI_BOOT_SERVICES                 *gBS;
static EFI_GRAPHICS_OUTPUT_PROTOCOL      *gGop;
static EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *gKeyEx;

/* every pointer instance the firmware exposes (ThinkPads publish several:
   TrackPoint, touchpad, USB mice - polling just one misses the live one) */
#define MAXPTR 4
static EFI_SIMPLE_POINTER_PROTOCOL   *gPtr[MAXPTR];
static EFI_ABSOLUTE_POINTER_PROTOCOL *gAbs[MAXPTR];
static int gNPtr, gNAbs;
static int gAccX[MAXPTR], gAccY[MAXPTR];    /* sub-pixel remainders per device */

/* ---- present-target geometry --------------------------------------------- */
#define MAX_SCALE 4
#define GROW_W   4096               /* output-row ceiling (4K panels) */
static volatile UINT32 *gVram;      /* GOP linear framebuffer (when usable) */
static UINT32 gStride;              /* pixels per scan line */
static UINT32 gModeW, gModeH;       /* native mode geometry */
static UINT32 gOffX, gOffY;         /* centring offset of the scaled desktop */
static int    gScale = 1;           /* integer zoom: fb pixel -> scale^2 block */
static int    gMaxScale = 1;
static int    gSwapRB;              /* output wants BGRX (Blt always does) */
static int    gUseBlt;              /* no usable linear FB: present via Blt() */
static UINT32 gRow[GROW_W];         /* one scaled output row */
static fb_px  gCurRow[FB_MAX_W];    /* cursor-composited fb row */

/* dirty-row shadow: present writes VRAM only for rows that changed since the
   last frame. Full-screen VRAM rewrites are what made the first metal build's
   frame loop (and with it the input poll rate) crawl - the desktop is almost
   entirely static frame-to-frame. */
static fb_px gShadow[FB_BUF_PIX];
static int   gShadowValid;

/* ---- cursor state --------------------------------------------------------- */
static int g_cx = 320, g_cy = 240;      /* re-clamped when geometry is set */
static int g_have_pointer = 0;
static int g_prev_mb = 0;

/* ---- port I/O (PC speaker) ------------------------------------------------ */
static inline void outb(unsigned short port, unsigned char v)
{ __asm__ volatile ("outb %0, %1" : : "a"(v), "Nd"(port)); }
static inline unsigned char inb(unsigned short port)
{ unsigned char v; __asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"(port)); return v; }

/* ---- debug log: QEMU debugcon (port 0x402) - METAL-UNSAFE, opt-in only ---- */
#ifdef UNO_DBGCON
static void dbg_puts(const char *s)
{
    while (*s) outb(0x402, (unsigned char)*s++);
}
#else
static void dbg_puts(const char *s) { (void)s; }
#endif

/* ---- boot-time text diagnostics ------------------------------------------ */
static void con_puts(const char *s)
{
    CHAR16 w[96]; int i;
    if (!gST || !gST->ConOut) return;
    for (i = 0; s[i] && i < 94; i++) w[i] = (CHAR16)(unsigned char)s[i];
    w[i] = 0;
    gST->ConOut->OutputString(gST->ConOut, w);
}

/* ===========================================================================
 * efi_main - the PE entry
 * ======================================================================== */
EFI_STATUS efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
    (void)ImageHandle;
    gST = SystemTable;
    gBS = SystemTable->BootServices;
    gBS->SetWatchdogTimer(0, 0, 0, 0);
    con_puts("UnoDOS 3.1 / pc64: firmware handoff...\r\n");
    con_puts("(bring-up stripes: red=GOP  green=mode  cyan=drivers  white=input)\r\n");
    gBS->Stall(2000000);            /* hold the banner readable on real panels */
    uno_main();
    return EFI_SUCCESS;
}

/* ===========================================================================
 * bring-up markers - the pc64 equivalent of the x86 boot chain's
 * diagnostic letters. Row 1 = init stages, row 2 = first-frame stages.
 * ======================================================================== */
static void stripe_at(UINT32 y0, int idx, UINT32 bgra)
{
    UINT32 x0 = (gModeW / 4) * idx, x1 = x0 + gModeW / 4, x, y;
    if (gUseBlt) {
        gGop->Blt(gGop, &bgra, EfiBltVideoFill, 0, 0, x0, y0, x1 - x0, 8, 0);
        return;
    }
    for (y = y0; y < y0 + 8; y++)
        for (x = x0; x < x1; x++) gVram[y * gStride + x] = bgra;
}
static void stage_stripe(int idx, UINT32 bgra)
{ stripe_at(0, idx, bgra); gBS->Stall(250000); }
static void stage_mark2(int idx, UINT32 bgra)
{ stripe_at(10, idx, bgra); }

/* ===========================================================================
 * display geometry - FULL resolution support, panel-safe.
 *
 * The GOP mode is kept (or F10-selected); the DESKTOP RESOLUTION is derived
 * from it: at zoom z the desktop runs at (mode/z), pixel-replicated z x z to
 * fill the panel. Zoom 1 = a native-resolution desktop (maximum real
 * estate); max zoom = the classic chunky look (desktop >= 640x480). This is
 * how a fixed-mode eDP panel gets real resolution choice: the mode never
 * changes, the desktop's logical size does.
 * ======================================================================== */
/* read the active GOP mode's geometry and present path */
static void read_mode(void)
{
    gModeW  = gGop->Mode->Info->HorizontalResolution;
    gModeH  = gGop->Mode->Info->VerticalResolution;
    gStride = gGop->Mode->Info->PixelsPerScanLine;
    gUseBlt = (gGop->Mode->Info->PixelFormat != PixelRedGreenBlueReserved8BitPerColor &&
               gGop->Mode->Info->PixelFormat != PixelBlueGreenRedReserved8BitPerColor)
              || gGop->Mode->FrameBufferBase == 0;
    gSwapRB = gUseBlt ||
              (gGop->Mode->Info->PixelFormat != PixelRedGreenBlueReserved8BitPerColor);
    gVram = (volatile UINT32 *)(UINTN)gGop->Mode->FrameBufferBase;

    /* zoom range: every zoom whose desktop stays >= 640x480 */
    gMaxScale = (int)(gModeW / 640);
    if ((int)(gModeH / 480) < gMaxScale) gMaxScale = (int)(gModeH / 480);
    if (gMaxScale < 1) gMaxScale = 1;
    if (gMaxScale > MAX_SCALE) gMaxScale = MAX_SCALE;
}

/* the largest integer zoom that fits a fbw x fbh desktop on the panel */
static int fit_zoom(int fbw, int fbh)
{
    int z = (int)(gModeW / (UINT32)fbw);
    if ((int)(gModeH / (UINT32)fbh) < z) z = (int)(gModeH / (UINT32)fbh);
    if (z > MAX_SCALE) z = MAX_SCALE;
    while (z > 1 && fbw * z > GROW_W) z--;
    if (z < 1) z = 1;
    return z;
}

/* commit a desktop resolution + zoom: fb dims, shim screen rect, centring,
   full surface clear, shadow invalidation */
static void apply_desktop(int fbw, int fbh, int zoom)
{
    gScale = zoom;
    uno_fb_w = fbw;
    uno_fb_h = fbh;

    qd.screenBits.bounds.left = 0;
    qd.screenBits.bounds.top = 0;
    qd.screenBits.bounds.right  = (short)uno_fb_w;
    qd.screenBits.bounds.bottom = (short)uno_fb_h;

    gOffX = (gModeW > (UINT32)(uno_fb_w * gScale)) ? (gModeW - uno_fb_w * gScale) / 2 : 0;
    gOffY = (gModeH > (UINT32)(uno_fb_h * gScale)) ? (gModeH - uno_fb_h * gScale) / 2 : 0;

    gShadowValid = 0;               /* stride/geometry changed: full rewrite */

    /* clear the whole surface (letterbox border + stale pixels) */
    if (gUseBlt) {
        UINT32 black = 0;
        gGop->Blt(gGop, &black, EfiBltVideoFill, 0, 0, 0, 0, gModeW, gModeH, 0);
    } else {
        UINT32 x, y;
        for (y = 0; y < gModeH; y++)
            for (x = 0; x < gModeW; x++) gVram[y * gStride + x] = 0;
    }
    if (g_cx > uno_fb_w - 1) g_cx = uno_fb_w - 1;
    if (g_cy > uno_fb_h - 1) g_cy = uno_fb_h - 1;
}

/* boot / mode-change default: the mode/zoom-derived desktop */
static void set_geometry(int wantZoom)
{
    int z, fbw, fbh;
    read_mode();
    z = wantZoom;
    if (z < 1) z = gMaxScale;                   /* <1 = default: chunky look */
    if (z > gMaxScale) z = gMaxScale;
    fbw = (int)(gModeW / (UINT32)z);
    fbh = (int)(gModeH / (UINT32)z);
    if (fbw > FB_MAX_W) fbw = FB_MAX_W;
    if (fbh > FB_MAX_H) fbh = FB_MAX_H;
    if (fbw * z > GROW_W) fbw = GROW_W / z;
    apply_desktop(fbw, fbh, z);
}

/* ---- the Settings app's resolution list (KernelApi pc64 tail) ------------
   Standard desktop-OS resolutions filtered to the active mode + the fb
   ceiling, with the native/chunky sizes appended when the list misses them.
   Each entry applies at its best integer zoom, centred. */
typedef struct { short w, h; } ResEntry;
static const ResEntry kStdRes[] = {
    { 640,  480}, { 800,  600}, {1024,  768}, {1152,  864},
    {1280,  720}, {1280,  800}, {1280,  960}, {1280, 1024},
    {1366,  768}, {1440,  900}, {1600,  900}, {1600, 1200},
    {1680, 1050}, {1920, 1080}, {1920, 1200}
};
#define NSTDRES ((int)(sizeof kStdRes / sizeof kStdRes[0]))
static ResEntry gResList[NSTDRES + 2];
static int gResN;

static void res_add(short w, short h)
{
    int i;
    for (i = 0; i < gResN; i++)
        if (gResList[i].w == w && gResList[i].h == h) return;
    gResList[gResN].w = w; gResList[gResN].h = h; gResN++;
}

static void build_res_list(void)
{
    int i;
    short capW = (short)(gModeW > FB_MAX_W ? FB_MAX_W : gModeW);
    short capH = (short)(gModeH > FB_MAX_H ? FB_MAX_H : gModeH);
    gResN = 0;
    for (i = 0; i < NSTDRES; i++)
        if (kStdRes[i].w <= capW && kStdRes[i].h <= capH)
            res_add(kStdRes[i].w, kStdRes[i].h);
    res_add(capW, capH);                        /* native (fb-capped) */
    if (gMaxScale > 1)                          /* the chunky default */
        res_add((short)(gModeW / gMaxScale), (short)(gModeH / gMaxScale));
}

int uno_pc64_res_count(void)
{
    build_res_list();
    return gResN;
}

void uno_pc64_res_get(int idx, short *w, short *h, short *zoom, Boolean *active)
{
    if (idx < 0 || idx >= gResN) { *w = *h = *zoom = 0; *active = 0; return; }
    *w = gResList[idx].w;
    *h = gResList[idx].h;
    *zoom = (short)fit_zoom(*w, *h);
    *active = (Boolean)(uno_fb_w == *w && uno_fb_h == *h);
}

void uno_pc64_res_set(int idx)
{
    short w, h;
    if (idx < 0 || idx >= gResN) return;
    w = gResList[idx].w; h = gResList[idx].h;
    if (uno_fb_w == w && uno_fb_h == h) return;  /* already active */
    apply_desktop(w, h, fit_zoom(w, h));
    uno_screen_changed();           /* core: new gScreen + full repaint */
}

/* F10: cycle GOP modes (external monitors; a laptop panel may not sync a
   non-native mode - keep pressing F10, the cycle returns to the native one) */
static void cycle_mode(void)
{
    UINT32 next = (gGop->Mode->Mode + 1) % gGop->Mode->MaxMode;
    gGop->SetMode(gGop, next);
    set_geometry(-1);               /* re-derive; default zoom for the mode */
    uno_screen_changed();
}

/* ===========================================================================
 * driver connect + protocol discovery
 * ======================================================================== */
static void connect_all(void)
{
    UINTN n = 0, i;
    EFI_HANDLE *hs = 0;
    if (EFI_ERROR(gBS->LocateHandleBuffer(EFI_LOCATE_ALL_HANDLES, 0, 0, &n, &hs)))
        return;
    for (i = 0; i < n; i++) gBS->ConnectController(hs[i], 0, 0, 1);
    gBS->FreePool(hs);
}

/* collect every instance of a pointer protocol (device handles first, the
   ConIn splitter aggregate last, so real hardware wins) */
static int collect(EFI_GUID *guid, void **out, int max)
{
    UINTN n = 0, i;
    EFI_HANDLE *hs = 0;
    void *p, *splitter = 0;
    int cnt = 0;
    if (EFI_ERROR(gBS->LocateHandleBuffer(EFI_LOCATE_BY_PROTOCOL, guid, 0, &n, &hs)))
        return 0;
    for (i = 0; i < n && cnt < max; i++) {
        if (EFI_ERROR(gBS->HandleProtocol(hs[i], guid, &p))) continue;
        if (hs[i] == gST->ConsoleInHandle) { splitter = p; continue; }
        out[cnt++] = p;
    }
    /* the splitter AGGREGATES the device instances - polling it alongside
       them applies every movement twice (the X1 Carbon's "jerky TrackPoint").
       It is only a fallback for firmware that publishes no device handles. */
    if (cnt == 0 && splitter) out[cnt++] = splitter;
    gBS->FreePool(hs);
    return cnt;
}

/* ===========================================================================
 * uno_pc64_init
 * ======================================================================== */
void uno_pc64_init(void)
{
    static EFI_GUID gopGuid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    static EFI_GUID ptrGuid = EFI_SIMPLE_POINTER_PROTOCOL_GUID;
    static EFI_GUID absGuid = EFI_ABSOLUTE_POINTER_PROTOCOL_GUID;
    int i;

    if (EFI_ERROR(gBS->LocateProtocol(&gopGuid, 0, (void **)&gGop)) || !gGop) {
        con_puts("pc64: no GOP - cannot continue\r\n");
        for (;;) gBS->Stall(1000000);
    }

    set_geometry(-1);               /* keep the native mode, auto zoom */
    stage_stripe(0, 0x00FF0000);    /* red: GOP up, surface cleared */
    stage_stripe(1, 0x0000FF00);    /* green: geometry set */

    connect_all();
    stage_stripe(2, 0x0000FFFF);    /* cyan: driver connect sweep done */

    gNAbs = collect(&absGuid, (void **)gAbs, MAXPTR);
    gNPtr = collect(&ptrGuid, (void **)gPtr, MAXPTR);
    for (i = 0; i < gNAbs; i++) gAbs[i]->Reset(gAbs[i], 0);
    for (i = 0; i < gNPtr; i++) gPtr[i]->Reset(gPtr[i], 0);
    if (gST->ConIn) gST->ConIn->Reset(gST->ConIn, 0);

    {
        static EFI_GUID exGuid = EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL_GUID;
        if (EFI_ERROR(gBS->HandleProtocol(gST->ConsoleInHandle, &exGuid,
                                          (void **)&gKeyEx)))
            gKeyEx = 0;
    }
    stage_stripe(3, 0x00FFFFFF);    /* white: input located - core takes over */

    dbg_puts("unodos-pc64: init done\n");
    stage_mark2(0, 0x00808080);     /* gray: platform init fully returned */
}

/* ===========================================================================
 * input
 * ======================================================================== */
static void post_key_mod(short keycode, char ch, short mods)
{
    Point p = { 0, 0 };
    long msg = ((long)(keycode & 0xFF) << 8) | (unsigned char)ch;
    uno_post_event(keyDown, msg, p, mods);
}
static void post_key(short keycode, char ch) { post_key_mod(keycode, ch, 0); }

static void map_key(UINT16 scan, CHAR16 uni, short mods)
{
    switch (scan) {                           /* Mac keycode + arrow ASCII */
    case SCAN_LEFT:  post_key_mod(0x7B, 0x1C, mods); return;
    case SCAN_RIGHT: post_key_mod(0x7C, 0x1D, mods); return;
    case SCAN_UP:    post_key_mod(0x7E, 0x1E, mods); return;
    case SCAN_DOWN:  post_key_mod(0x7D, 0x1F, mods); return;
    case SCAN_ESC:   post_key_mod(0x35, 0x1B, mods); return;
    case SCAN_DELETE: post_key_mod(0x33, 0x08, mods); return;
    case SCAN_F10:   cycle_mode();  return;   /* platform: GOP mode cycle
                                                 (desktop resolution lives in
                                                  the Settings app) */
    }
    /* Ctrl+letter arrives as a control code (^S = 0x13) - normalize */
    if (mods && uni >= 1 && uni <= 26 && uni != 0x0D && uni != 0x0A &&
        uni != 0x08 && uni != 0x09)
        uni = (CHAR16)(uni - 1 + 'a');
    switch (uni) {
    case 0x0D: case 0x0A: post_key_mod(0x24, 0x0D, mods); break;  /* Enter */
    case 0x08:            post_key_mod(0x33, 0x08, mods); break;  /* Backspace */
    case 0x09:            post_key_mod(0x30, 0x09, mods); break;  /* Tab */
    default:
        if (uni >= 32 && uni < 127) post_key_mod(0, (char)uni, mods);
    }
}

static void poll_keyboard(void)
{
    /* budget the drain: firmware exists whose ReadKeyStrokeEx returns
       SUCCESS forever with phantom "partial keystroke" data */
    int budget = 32;
    if (gKeyEx) {
        EFI_KEY_DATA kd;
        while (budget-- > 0 &&
               gKeyEx->ReadKeyStrokeEx(gKeyEx, &kd) == EFI_SUCCESS) {
            short mods = 0;
            if ((kd.KeyState.KeyShiftState & EFI_SHIFT_STATE_VALID) &&
                (kd.KeyState.KeyShiftState &
                 (EFI_LEFT_CONTROL_PRESSED | EFI_RIGHT_CONTROL_PRESSED)))
                mods = cmdKey;
            if (!kd.Key.ScanCode && !kd.Key.UnicodeChar) continue;
            map_key(kd.Key.ScanCode, kd.Key.UnicodeChar, mods);
        }
        return;
    }
    if (gST->ConIn) {
        EFI_INPUT_KEY k;
        while (budget-- > 0 &&
               gST->ConIn->ReadKeyStroke(gST->ConIn, &k) == EFI_SUCCESS)
            map_key(k.ScanCode, k.UnicodeChar, 0);
    }
}

static void pointer_moved_clicked(int mb)
{
    uno_set_mouse((short)g_cx, (short)g_cy, (Boolean)mb);
    if (mb != g_prev_mb) {
        Point p; p.h = (short)g_cx; p.v = (short)g_cy;
        uno_post_event(mb ? mouseDown : mouseUp, 0, p, 0);
        g_prev_mb = mb;
    }
}

static void clamp_cursor(void)
{
    if (g_cx < 0) g_cx = 0;
    if (g_cx > FB_W - 1) g_cx = FB_W - 1;
    if (g_cy < 0) g_cy = 0;
    if (g_cy > FB_H - 1) g_cy = FB_H - 1;
}

static void poll_pointer(void)
{
    int i;
    /* absolute pointers: map panel coords -> the letterboxed desktop rect */
    for (i = 0; i < gNAbs; i++) {
        EFI_ABSOLUTE_POINTER_STATE st;
        if (gAbs[i]->GetState(gAbs[i], &st) != EFI_SUCCESS) continue;
        {
            UINT64 rx = gAbs[i]->Mode->AbsoluteMaxX - gAbs[i]->Mode->AbsoluteMinX;
            UINT64 ry = gAbs[i]->Mode->AbsoluteMaxY - gAbs[i]->Mode->AbsoluteMinY;
            if (rx && ry) {
                int sx = (int)(((st.CurrentX - gAbs[i]->Mode->AbsoluteMinX) * gModeW) / rx);
                int sy = (int)(((st.CurrentY - gAbs[i]->Mode->AbsoluteMinY) * gModeH) / ry);
                g_cx = (sx - (int)gOffX) / gScale;
                g_cy = (sy - (int)gOffY) / gScale;
                clamp_cursor();
                g_have_pointer = 1;
            }
            pointer_moved_clicked((st.ActiveButtons & 1) ? 1 : 0);
        }
    }
    /* relative pointers: sub-count accumulation so slow motion still moves
       (a plain dx/Resolution divide rounds small deltas to zero - the reason
       a "working" mouse can look dead) */
    for (i = 0; i < gNPtr; i++) {
        EFI_SIMPLE_POINTER_STATE st;
        if (gPtr[i]->GetState(gPtr[i], &st) != EFI_SUCCESS) continue;
        {
            int div = (int)(gPtr[i]->Mode->ResolutionX ? gPtr[i]->Mode->ResolutionX : 1);
            int mv;
            gAccX[i] += (int)st.RelativeMovementX * 3;   /* ~3 px per mm */
            gAccY[i] += (int)st.RelativeMovementY * 3;
            mv = gAccX[i] / div; gAccX[i] -= mv * div; g_cx += mv;
            mv = gAccY[i] / div; gAccY[i] -= mv * div; g_cy += mv;
            if (st.RelativeMovementX || st.RelativeMovementY) {
                clamp_cursor();
                g_have_pointer = 1;
            }
            pointer_moved_clicked(st.LeftButton ? 1 : 0);
        }
    }
}

void uno_pc64_poll(void)
{
    static int first = 1;
    if (first) stage_mark2(1, 0x00FFFF00);    /* yellow: core init survived */
    poll_keyboard();
    if (first) stage_mark2(2, 0x00FF00FF);    /* magenta: keyboard poll survived */
    poll_pointer();
    if (first) { stage_mark2(3, 0x00FF8000); first = 0; }  /* orange: pointer too */
}

/* ===========================================================================
 * present - cursor composited at fb resolution, rows scaled + centred
 * ======================================================================== */
static const char *kCursor[] = {
    "B","BB","BWB","BWWB","BWWWB","BWWWWB","BWWWWWB","BWWWWWWB",
    "BWWWWBBBB","BWWBWB","BWB BWB","BB  BWB","B    BWB","      BWB","       BB", 0
};

/* returns the source row for fb row y, cursor applied if it crosses it */
static const fb_px *cursor_row(int y, const fb_px *src)
{
    int r = y - g_cy, c;
    const char *row;
    if (!g_have_pointer || r < 0 || r >= 15 || !kCursor[r]) return src;
    for (c = 0; c < FB_W; c++) gCurRow[c] = src[c];
    row = kCursor[r];
    for (c = 0; row[c]; c++) {
        int xx = g_cx + c;
        if (xx < 0 || xx >= FB_W) continue;
        if (row[c] == 'B')      gCurRow[xx] = FB_RGB(0, 0, 0);
        else if (row[c] == 'W') gCurRow[xx] = FB_RGB(0xFF, 0xFF, 0xFF);
    }
    return gCurRow;
}

void uno_pc64_present(void)
{
    int x, y, k, s = gScale;
    {
        static int first = 1;
        if (first) { stage_mark2(0, 0x000080FF); first = 0; }  /* gray -> blue */
    }
    for (y = 0; y < FB_H; y++) {
        const fb_px *src = cursor_row(y, fb + y * FB_W);
        fb_px *sh = gShadow + y * FB_W;
        /* dirty check: skip rows unchanged since the last frame */
        if (gShadowValid) {
            for (x = 0; x < FB_W && src[x] == sh[x]; x++) ;
            if (x == FB_W) continue;
        }
        for (x = 0; x < FB_W; x++) sh[x] = src[x];
        /* one output row at fb-row y, scaled horizontally */
        if (gSwapRB) {
            for (x = 0; x < FB_W; x++) {
                fb_px p = src[x];   /* 0xAABBGGRR -> 0x00RRGGBB */
                UINT32 c = ((p & 0xFF) << 16) | (p & 0xFF00) | ((p >> 16) & 0xFF);
                for (k = 0; k < s; k++) gRow[x * s + k] = c;
            }
        } else {
            for (x = 0; x < FB_W; x++) {
                UINT32 c = src[x] & 0x00FFFFFF;
                for (k = 0; k < s; k++) gRow[x * s + k] = c;
            }
        }
        /* replicate vertically */
        for (k = 0; k < s; k++) {
            UINT32 dy = gOffY + (UINT32)(y * s + k);
            if (gUseBlt) {
                gGop->Blt(gGop, gRow, EfiBltBufferToVideo, 0, 0,
                          gOffX, dy, (UINTN)FB_W * s, 1, 0);
            } else {
                volatile UINT32 *dst = gVram + dy * gStride + gOffX;
                for (x = 0; x < FB_W * s; x++) dst[x] = gRow[x];
            }
        }
    }
    gShadowValid = 1;
    gBS->Stall(8000);               /* idle frames are now nearly free */
}

/* ===========================================================================
 * PC speaker (Sound Manager backend, pc64_io.c) - PIT channel 2
 * ======================================================================== */
static int midi_hz(int midi)
{
    static const int kRatio[12] = {     /* round(4096 * 2^(i/12)) */
        4096, 4340, 4598, 4871, 5161, 5468,
        5793, 6137, 6502, 6889, 7298, 7732
    };
    int n = midi - 69, oct = 0, hz;
    while (n < 0)  { n += 12; oct--; }
    while (n >= 12){ n -= 12; oct++; }
    hz = (440 * kRatio[n]) >> 12;
    while (oct > 0) { hz <<= 1; oct--; }
    while (oct < 0) { hz >>= 1; oct++; }
    return hz;
}

void uno_pc64_snd_note(int midi)
{
    int hz = midi_hz(midi);
    unsigned divisor;
    if (hz < 20) { return; }
    divisor = 1193182u / (unsigned)hz;
    if (divisor > 0xFFFF) divisor = 0xFFFF;
    outb(0x43, 0xB6);                       /* ch2, lo/hi, square wave */
    outb(0x42, (unsigned char)(divisor & 0xFF));
    outb(0x42, (unsigned char)(divisor >> 8));
    outb(0x61, (unsigned char)(inb(0x61) | 0x03));   /* gate + data on */
}

void uno_pc64_snd_quiet(void)
{
    outb(0x61, (unsigned char)(inb(0x61) & ~0x03));
}
