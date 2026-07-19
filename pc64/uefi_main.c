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
#include "i2c_hid.h"        /* native I2C-HID trackpad (inert unless -DUNO_I2C_TRACKPAD) */
#include <string.h>         /* memcpy (freestanding, from pc64_libc.c) */
#include "fat.h"            /* native block + FAT stack bring-up */
#include "blkdev.h"
#ifdef UNO_ACPI
#include "acpi_host.h"      /* AML interpreter bring-up (battery/lid via unoacpi) */
#endif

int uno_main(void);                 /* the portable core (-Dmain=uno_main) */
void uno_screen_changed(void);      /* core hook: resolution changed (unodos.c) */

/* ---- UEFI runtime services (uefi.h leaves RuntimeServices void*) ---------- */
typedef struct {
    UINT16 Year; UINT8 Month, Day, Hour, Minute, Second, Pad1;
    UINT32 Nanosecond; short TimeZone; UINT8 Daylight, Pad2;
} EFI_TIME;
typedef enum { EfiResetCold, EfiResetWarm, EfiResetShutdown, EfiResetPlatformSpecific } EFI_RESET_TYPE;
typedef struct {
    UINT8 Hdr[24];
    EFI_STATUS (*GetTime)(EFI_TIME *, void *);
    EFI_STATUS (*SetTime)(EFI_TIME *);
    void *GetWakeupTime, *SetWakeupTime, *SetVirtualAddressMap, *ConvertPointer;
    void *GetVariable, *GetNextVariableName, *SetVariable, *GetNextHighMonotonicCount;
    void (*ResetSystem)(EFI_RESET_TYPE, EFI_STATUS, UINTN, void *);
} EFI_RUNTIME_SERVICES;

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

/* ---- present-target geometry ---------------------------------------------
 * The desktop framebuffer (uno_fb_w x uno_fb_h) is FRACTIONALLY scaled to
 * FILL the panel - nearest-neighbour, aspect preserved, centred - so a low
 * desktop resolution fills the screen (a big chunky UI) instead of sitting in
 * a small letterboxed box. gColMap/gRowMap are the per-output-pixel source
 * indices; gOutW x gOutH is the filled region at gOffX,gOffY. */
#define GROW_W  3840                /* output-row ceiling (4K-wide panels) */
#define GROW_H  2160
static volatile UINT32 *gVram;      /* GOP linear framebuffer (when usable) */
static UINT32 gStride;              /* pixels per scan line */
static UINT32 gModeW, gModeH;       /* native mode geometry */
static UINT32 gOffX, gOffY;         /* centring offset of the filled desktop */
static int    gOutW, gOutH;         /* filled (scaled) desktop size on the panel */
static int    gSwapRB;              /* output wants BGRX (Blt always does) */
static int    gUseBlt;              /* no usable linear FB: present via Blt() */
static UINT32 gRow[GROW_W];         /* one scaled output row */
static fb_px  gCurRow[FB_MAX_W];    /* cursor-composited fb row */
static int    gColMap[GROW_W];      /* output col -> source col */
static short  gRowMap[GROW_H];      /* output row -> source row */
static unsigned char gDirtyRow[FB_MAX_H];   /* per-source-row dirty flags */

/* dirty-row shadow: present writes VRAM only for rows that changed since the
   last frame. Full-screen VRAM rewrites are what made the first metal build's
   frame loop (and with it the input poll rate) crawl - the desktop is almost
   entirely static frame-to-frame. */
static fb_px gShadow[FB_BUF_PIX];
static int   gShadowValid;

/* scene snapshot for the rubber-band window drag: while a drag is live the
   underlying desktop/windows don't change, so we render the scene once, snapshot
   it here, and each frame restore it + redraw the moving outline instead of
   re-running the (expensive, alpha-blend heavy) full-scene painter. */
static fb_px gScene[FB_BUF_PIX];
void uno_pc64_scene_save(void)
{ memcpy(gScene, fb, (size_t)FB_W * (size_t)FB_H * sizeof(fb_px)); }
void uno_pc64_scene_restore(void)
{ memcpy(fb, gScene, (size_t)FB_W * (size_t)FB_H * sizeof(fb_px)); }

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
static EFI_HANDLE gIH;                  /* our image handle (installer needs it) */
static int gDetached;                   /* 1 after ExitBootServices (M3)        */
void *uno_pc64_st(void)           { return gST; }
void *uno_pc64_image_handle(void) { return gIH; }
int   uno_pc64_detached(void)     { return gDetached; }

EFI_STATUS efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
    (void)ImageHandle;
    gST = SystemTable;
    gIH = ImageHandle;
    gBS = SystemTable->BootServices;
    gBS->SetWatchdogTimer(0, 0, 0, 0);
    con_puts("UnoDOS 3.1 / pc64: firmware handoff...\r\n");
    /* no banner hold - the graphical splash + loading bar takes over as soon as
       the fb is up (see platform_init / splash_step). */
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
 * boot splash + loading bar - the pc64 splash screen. Drawn into the software
 * fb and presented (same font + fill-scale path as the desktop), so it appears
 * centred and crisp on the panel. The four progress segments keep the old
 * bring-up meaning (GOP / drivers / input / ready) as red/green/cyan/white.
 * Requires the fb to exist, so it runs only after set_geometry().
 * ======================================================================== */
#define SPLASH_STEPS 4
static void splash_draw(int done)
{
    int W = uno_fb_w, H = uno_fb_h, cx = W / 2, i;
    int barw = (W < 360) ? (W - 80) : 280, bx, by = H / 2 + 46, bh = 14, seg;
    int sc = (W >= 640) ? 4 : 3;
    fb_px segc[SPLASH_STEPS] = { FB_RGB(220,60,60), FB_RGB(70,200,90),
                                 FB_RGB(60,190,210), FB_RGB(240,240,245) };
    if (barw < 120) barw = 120;
    bx = cx - barw / 2; seg = barw / SPLASH_STEPS;
    /* backdrop: deep blue with a lighter upper band */
    fb_fill_rect(0, 0, W, H, FB_RGB(10, 14, 34));
    fb_fill_rect(0, 0, W, (H / 2) - 58, FB_RGB(16, 22, 54));
    /* 4-square emblem above the wordmark (matches the Start button motif) */
    { int u = 10, ex = cx - u - 1, ey = H / 2 - 98;
      fb_fill_rect(ex, ey, u, u, FB_RGB(230,70,70));
      fb_fill_rect(ex + u + 2, ey, u, u, FB_RGB(70,200,90));
      fb_fill_rect(ex, ey + u + 2, u, u, FB_RGB(70,120,230));
      fb_fill_rect(ex + u + 2, ey + u + 2, u, u, FB_RGB(240,205,60)); }
    /* wordmark + subtitle */
    { const char *t = "UnoDOS"; int tw = fb_text_w(t) * sc;
      fb_big_text(cx - tw / 2, H / 2 - 46, t, FB_RGB(255,255,255), -1, sc); }
    { const char *s = "Modern PC world  -  x86-64 UEFI";
      fb_text(cx - fb_text_w(s) / 2, H / 2 + 6, s, FB_RGB(150,170,225), -1); }
    { const char *s = "pc64   -   UnoDOS 3.1";
      fb_text(cx - fb_text_w(s) / 2, H / 2 + 22, s, FB_RGB(110,130,185), -1); }
    /* loading bar: recessed track + filled coloured segments */
    fb_fill_rect(bx - 2, by - 2, barw + 4, bh + 4, FB_RGB(30, 38, 70));
    fb_frame_rect(bx - 2, by - 2, barw + 4, bh + 4, FB_RGB(70, 84, 130));
    fb_fill_rect(bx, by, barw, bh, FB_RGB(20, 26, 52));
    for (i = 0; i < done && i < SPLASH_STEPS; i++)
        fb_fill_rect(bx + i * seg + 1, by + 1, seg - 2, bh - 2, segc[i]);
    { const char *s = "loading";
      fb_text(cx - fb_text_w(s) / 2, by + bh + 9, s, FB_RGB(120,138,185), -1); }
}
static void splash_step(int done)
{
    splash_draw(done);
    uno_pc64_present();
    gBS->Stall(done >= SPLASH_STEPS ? 700000 : 400000);
}

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
    if (gModeW > GROW_W) gModeW = GROW_W;
    if (gModeH > GROW_H) gModeH = GROW_H;
}

/* commit a desktop resolution: set the fb dims + the shim screen rect, then
   compute the FILL mapping (fractional nearest-neighbour, aspect preserved,
   centred) and the per-pixel source maps; clear the surface. */
static void apply_desktop(int fbw, int fbh)
{
    UINT32 sx, sy;
    int i;
    if (fbw < 64) fbw = 64;  if (fbw > FB_MAX_W) fbw = FB_MAX_W;
    if (fbh < 48) fbh = 48;  if (fbh > FB_MAX_H) fbh = FB_MAX_H;
    uno_fb_w = fbw;
    uno_fb_h = fbh;

#ifndef UNO_UUI
    /* tell the legacy Toolbox shim the screen size (unoui reads FB_W/FB_H) */
    qd.screenBits.bounds.left = 0;
    qd.screenBits.bounds.top = 0;
    qd.screenBits.bounds.right  = (short)uno_fb_w;
    qd.screenBits.bounds.bottom = (short)uno_fb_h;
#endif

    /* fill the panel, preserve aspect: scale = min(modeW/fbw, modeH/fbh),
       computed in 16.16 fixed point so the largest axis fills exactly */
    {
        unsigned long long sxf = ((unsigned long long)gModeW << 16) / (unsigned)fbw;
        unsigned long long syf = ((unsigned long long)gModeH << 16) / (unsigned)fbh;
        unsigned long long sc = (sxf < syf) ? sxf : syf;   /* fit */
        gOutW = (int)(((unsigned long long)fbw * sc) >> 16);
        gOutH = (int)(((unsigned long long)fbh * sc) >> 16);
        if (gOutW > (int)gModeW) gOutW = gModeW;
        if (gOutH > (int)gModeH) gOutH = gModeH;
    }
    gOffX = (gModeW - (UINT32)gOutW) / 2;
    gOffY = (gModeH - (UINT32)gOutH) / 2;

    /* source-index maps (nearest neighbour) */
    for (i = 0; i < gOutW; i++) {
        int c = (int)(((long long)i * fbw) / gOutW);
        gColMap[i] = (c < fbw) ? c : fbw - 1;
    }
    for (i = 0; i < gOutH; i++) {
        int r = (int)(((long long)i * fbh) / gOutH);
        gRowMap[i] = (short)((r < fbh) ? r : fbh - 1);
    }

    gShadowValid = 0;               /* geometry changed: full rewrite */

    /* clear the whole surface (any residual border pixels) */
    if (gUseBlt) {
        UINT32 black = 0;
        gGop->Blt(gGop, &black, EfiBltVideoFill, 0, 0, 0, 0, gModeW, gModeH, 0);
    } else {
        for (sy = 0; sy < gModeH; sy++)
            for (sx = 0; sx < gModeW; sx++) gVram[sy * gStride + sx] = 0;
    }
    if (g_cx > uno_fb_w - 1) g_cx = uno_fb_w - 1;
    if (g_cy > uno_fb_h - 1) g_cy = uno_fb_h - 1;
}

/* boot / mode-change default: a chunky desktop (~half the panel) that fills
   the screen. Half-res on a 16:9 panel fills exactly (2x) with no borders and
   gives a comfortably large UI. */
static void set_geometry(int unused)
{
    (void)unused;
    read_mode();
    apply_desktop((int)(gModeW / 2), (int)(gModeH / 2));
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
    /* the chunky default (half the panel) first, then the standard list.
       Every entry is FILL-scaled to the panel, so all of them fill the
       screen - the list is just how big you want the UI. */
    res_add((short)(gModeW / 2), (short)(gModeH / 2));
    for (i = 0; i < NSTDRES; i++)
        if (kStdRes[i].w <= capW && kStdRes[i].h <= capH)
            res_add(kStdRes[i].w, kStdRes[i].h);
    res_add(capW, capH);                        /* native (fb-capped) */
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
    *zoom = 0;                       /* fractional fill: no integer zoom label */
    *active = (Boolean)(uno_fb_w == *w && uno_fb_h == *h);
}

void uno_pc64_res_set(int idx)
{
    short w, h;
    if (idx < 0 || idx >= gResN) return;
    w = gResList[idx].w; h = gResList[idx].h;
    if (uno_fb_w == w && uno_fb_h == h) return;  /* already active */
    apply_desktop(w, h);
    uno_screen_changed();           /* core: new gScreen + full repaint */
}

/* Runner3D (and any full-screen 3D) renders far fewer pixels at a low fb
   resolution; the fractional fill-scaler then upscales it to the panel. */
static int gSavedW, gSavedH, gLowres;
void uno_pc64_lowres(int on)
{
    if (on && !gLowres) {
        gSavedW = uno_fb_w; gSavedH = uno_fb_h; gLowres = 1;
        apply_desktop((int)(gModeW / 4), (int)(gModeH / 4));  /* ~1/16 the pixels */
        uno_screen_changed();
    } else if (!on && gLowres) {
        gLowres = 0;
        apply_desktop(gSavedW, gSavedH);
        uno_screen_changed();
    }
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

/* Detach the firmware's own driver from a PCI device (bus/dev/fn) so a native
 * driver can take it over without the firmware fighting it. Needed for xHCI:
 * the firmware's USB stack keeps touching the controller otherwise, causing
 * intermittent HC errors when our driver reprograms it. */
typedef struct _EFI_PCI_IO_PROTOCOL {
    void *pad[14];                                 /* PollMem..Flush (14 members) */
    EFI_STATUS (*GetLocation)(struct _EFI_PCI_IO_PROTOCOL *,
                              UINTN *seg, UINTN *bus, UINTN *dev, UINTN *fn);
} EFI_PCI_IO_PROTOCOL;

int uno_pc64_pci_disconnect(int bus, int dev, int fn)
{
    static EFI_GUID pio = { 0x4cf5b200, 0x68b8, 0x4ca5,
        { 0x9e, 0xec, 0xb2, 0x3e, 0x3f, 0x50, 0x02, 0x9a } };
    EFI_STATUS (*disc)(EFI_HANDLE, EFI_HANDLE, EFI_HANDLE) =
        (EFI_STATUS (*)(EFI_HANDLE, EFI_HANDLE, EFI_HANDLE))gBS->DisconnectController;
    UINTN n = 0, i; EFI_HANDLE *hs = 0; int done = 0;
    if (!gBS || EFI_ERROR(gBS->LocateHandleBuffer(EFI_LOCATE_BY_PROTOCOL, &pio, 0, &n, &hs)))
        return 0;
    for (i = 0; i < n; i++) {
        EFI_PCI_IO_PROTOCOL *p; UINTN s, b, d, f;
        if (EFI_ERROR(gBS->HandleProtocol(hs[i], &pio, (void **)&p))) continue;
        if (EFI_ERROR(p->GetLocation(p, &s, &b, &d, &f))) continue;
        if ((int)b == bus && (int)d == dev && (int)f == fn) { if (!EFI_ERROR(disc(hs[i], 0, 0))) done++; }
    }
    gBS->FreePool(hs);
    return done;
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
    splash_step(1);                 /* GOP + geometry: the splash appears */

    connect_all();
    splash_step(2);                 /* drivers connected */

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
    splash_step(3);                 /* input located */
    uno_i2c_hid_init();             /* native trackpad; inert unless built in */
    uno_fat_init();                 /* native block + FAT stack (AHCI + fw sectors) */
#ifdef UNO_STORTEST
    uno_fat_selftest();             /* WRTEST.REQ -> WRTEST.OK (harness write proof) */
#endif
#ifdef UNO_ACPI
    /* Bring up the AML interpreter (unoacpi): parse the firmware's DSDT/SSDTs
     * and locate the battery/lid devices.  Read-only (NO_ACPI_MODE, no SCI/GPE)
     * and every EC/SMBus wait is TSC-bounded, so a hostile or absent EC times
     * out instead of hanging; on QEMU (no battery) this is a clean no-find. */
    uno_acpi_start(gST);
#endif
    splash_step(4);                 /* ready - the bar fills, core takes over */
    uno_pc64_chime();               /* startup chime: loading complete */

    dbg_puts("unodos-pc64: init done\n");
}

/* ===========================================================================
 * input
 * ======================================================================== */
static void post_key_mod(short keycode, char ch, short mods)
{
#ifndef UNO_UUI                          /* legacy event queue; unoui uses the raw ring */
    Point p = { 0, 0 };
    long msg = ((long)(keycode & 0xFF) << 8) | (unsigned char)ch;
    uno_post_event(keyDown, msg, p, mods);
#else
    (void)keycode; (void)ch; (void)mods;
#endif
}
static void post_key(short keycode, char ch) { post_key_mod(keycode, ch, 0); }

/* ---- raw key ring: a platform-neutral (scan, unicode, ctrl) stream the
   unoui shell consumes directly. The legacy core uses the Mac-coded events
   posted below; both are filled, harmlessly. ------------------------------- */
#define RAWK 32
static struct { int scan, uni, ctrl; } gRawK[RAWK];
static int gRawHead, gRawTail;
static void raw_push(int scan, int uni, int ctrl)
{
    int n = (gRawTail + 1) % RAWK;
    if (n == gRawHead) return;
    gRawK[gRawTail].scan = scan; gRawK[gRawTail].uni = uni;
    gRawK[gRawTail].ctrl = ctrl; gRawTail = n;
}
int uno_pc64_next_key(int *scan, int *uni, int *ctrl)
{
    if (gRawHead == gRawTail) return 0;
    *scan = gRawK[gRawHead].scan; *uni = gRawK[gRawHead].uni;
    *ctrl = gRawK[gRawHead].ctrl;
    gRawHead = (gRawHead + 1) % RAWK;
    return 1;
}
void uno_pc64_mouse(int *x, int *y, int *btn) { *x = g_cx; *y = g_cy; *btn = g_prev_mb; }

/* Live pointer sample for a legacy blocking drag (Paint's GetMouse/StillDown
 * spin). Those apps loop reading the mouse WITHOUT returning to the main event
 * pump, so nothing would otherwise refresh the cursor - here we pump the pointer
 * once and present the frame so the drag both tracks and shows live. Returns the
 * button; fills fb-space coords. Wired to mac_compat's GetMouse via uno_mac_mouse. */
static void poll_pointer(void);          /* fwd (defined below) */
int uno_pc64_mac_mouse(short *h, short *v)
{
    poll_pointer();
    uno_pc64_present();
    if (h) *h = (short)g_cx;
    if (v) *v = (short)g_cy;
    return g_prev_mb;
}
void uno_pc64_delay_ms(int ms) { if (gBS && ms > 0) gBS->Stall((UINTN)ms * 1000); }

static void map_key(UINT16 scan, CHAR16 uni, short mods)
{
    raw_push((int)scan, (int)uni, mods ? 1 : 0);
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
#ifndef UNO_UUI                          /* legacy path; unoui reads g_cx/g_cy/g_prev_mb */
    uno_set_mouse((short)g_cx, (short)g_cy, (Boolean)mb);
#endif
    if (mb != g_prev_mb) {
#ifndef UNO_UUI
        Point p; p.h = (short)g_cx; p.v = (short)g_cy;
        uno_post_event(mb ? mouseDown : mouseUp, 0, p, 0);
#endif
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

/* Per-instance LATCHED button state. UEFI GetState returns EFI_NOT_READY when
   there's no new input, so a held button is only reported on the press frame;
   we must remember it until the release frame or the click "doesn't work". */
static int gAbsBtn[MAXPTR], gPtrBtn[MAXPTR];

static void poll_pointer(void)
{
    int i, mb = 0, moved = 0;

    /* native I2C-HID trackpad (when built in + present): its absolute coords
       take priority over the firmware pointer path */
    if (uno_i2c_hid_present()) {
        int ax, ay, b;
        if (uno_i2c_hid_poll(&ax, &ay, &b)) {
            /* ax/ay normalised 0..32767 -> panel -> fb (calibrate once real
               report ranges are known from the X1) */
            int px = (int)((long long)ax * gModeW / 32767);
            int py = (int)((long long)ay * gModeH / 32767);
            if (gOutW > 0 && gOutH > 0) {
                g_cx = (px - (int)gOffX) * uno_fb_w / gOutW;
                g_cy = (py - (int)gOffY) * uno_fb_h / gOutH;
                clamp_cursor();
                g_have_pointer = 1; moved = 1;
            }
            if (b) mb = 1;
        }
    }

    /* absolute pointers (touchpads, tablets): position is absolute. Only the
       FIRST instance that reports new data drives the cursor this frame, so
       two devices (touchpad + TrackPoint) never fight over the position. */
    for (i = 0; i < gNAbs; i++) {
        EFI_ABSOLUTE_POINTER_STATE st;
        if (gAbs[i]->GetState(gAbs[i], &st) != EFI_SUCCESS) continue;   /* no change */
        gAbsBtn[i] = st.ActiveButtons ? 1 : 0;                          /* latch */
        if (!moved) {
            UINT64 rx = gAbs[i]->Mode->AbsoluteMaxX - gAbs[i]->Mode->AbsoluteMinX;
            UINT64 ry = gAbs[i]->Mode->AbsoluteMaxY - gAbs[i]->Mode->AbsoluteMinY;
            if (rx && ry && gOutW > 0 && gOutH > 0) {
                int sx = (int)(((st.CurrentX - gAbs[i]->Mode->AbsoluteMinX) * gModeW) / rx);
                int sy = (int)(((st.CurrentY - gAbs[i]->Mode->AbsoluteMinY) * gModeH) / ry);
                int tx = (sx - (int)gOffX) * uno_fb_w / gOutW;   /* panel -> fb */
                int ty = (sy - (int)gOffY) * uno_fb_h / gOutH;
                /* Firmware touchpad Absolute Pointer coords are jumpy. A 2-pole
                   low-pass (weight the previous position 2:1) smooths the
                   jitter; a small dead-zone drops sub-pixel wobble; and a snap
                   on a big delta keeps finger repositions instant. Tuned
                   conservatively - the constants want a metal pass on the X1. */
                int dxx = tx - g_cx, dyy = ty - g_cy;
                int adx = dxx < 0 ? -dxx : dxx, ady = dyy < 0 ? -dyy : dyy;
                if (adx > 48)      g_cx = tx;               /* reposition: snap */
                else if (adx >= 2) g_cx = (g_cx * 2 + tx) / 3;   /* glide */
                if (ady > 48)      g_cy = ty;
                else if (ady >= 2) g_cy = (g_cy * 2 + ty) / 3;
                clamp_cursor();
                g_have_pointer = 1; moved = 1;
            }
        }
    }

    /* relative pointers (mice, TrackPoint): sub-count accumulation so slow
       motion still moves; only the first mover drives the cursor. */
    for (i = 0; i < gNPtr; i++) {
        EFI_SIMPLE_POINTER_STATE st;
        if (gPtr[i]->GetState(gPtr[i], &st) != EFI_SUCCESS) continue;
        gPtrBtn[i] = (st.LeftButton || st.RightButton) ? 1 : 0;         /* latch */
        if (!moved && (st.RelativeMovementX || st.RelativeMovementY)) {
            int div = (int)(gPtr[i]->Mode->ResolutionX ? gPtr[i]->Mode->ResolutionX : 1);
            int mv;
            gAccX[i] += (int)st.RelativeMovementX * 3;   /* ~3 px per mm */
            gAccY[i] += (int)st.RelativeMovementY * 3;
            mv = gAccX[i] / div; gAccX[i] -= mv * div; g_cx += mv;
            mv = gAccY[i] / div; gAccY[i] -= mv * div; g_cy += mv;
            clamp_cursor();
            g_have_pointer = 1; moved = 1;
        }
    }

    /* a click on ANY device's ANY button counts (clickpads report the whole
       surface as one button, sometimes on a different instance/bit) */
    for (i = 0; i < gNAbs; i++) mb |= gAbsBtn[i];
    for (i = 0; i < gNPtr; i++) mb |= gPtrBtn[i];
    pointer_moved_clicked(mb);
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
    int x, oy, sy, fbw = FB_W, fbh = FB_H, any_dirty = 0;
    {
        static int first = 1;
        if (first) { stage_mark2(0, 0x000080FF); first = 0; }  /* gray -> blue */
    }

    /* pass 1: composite the cursor into each source row and flag the rows
       that changed since last frame (stored composited in gShadow) */
    for (sy = 0; sy < fbh; sy++) {
        const fb_px *src = cursor_row(sy, fb + sy * fbw);
        fb_px *sh = gShadow + sy * fbw;
        int dirty = !gShadowValid;
        if (!dirty)
            for (x = 0; x < fbw; x++) if (src[x] != sh[x]) { dirty = 1; break; }
        gDirtyRow[sy] = (unsigned char)dirty;
        if (dirty) { any_dirty = 1; for (x = 0; x < fbw; x++) sh[x] = src[x]; }
    }

    /* nothing changed since the last present: skip the VRAM write pass entirely
       (common - a drag only touches a few rows, an idle frame touches none). */
    if (!any_dirty) { gShadowValid = 1; gBS->Stall(1000); return; }

    /* pass 2: for each output row whose source row changed, build the scaled
       output row (nearest-neighbour via gColMap) and write the filled span */
    for (oy = 0; oy < gOutH; oy++) {
        const fb_px *sh;
        UINT32 dy;
        sy = gRowMap[oy];
        if (!gDirtyRow[sy]) continue;
        sh = gShadow + sy * fbw;
        if (gSwapRB) {
            for (x = 0; x < gOutW; x++) {
                fb_px p = sh[gColMap[x]];   /* 0xAABBGGRR -> 0x00RRGGBB */
                gRow[x] = ((p & 0xFF) << 16) | (p & 0xFF00) | ((p >> 16) & 0xFF);
            }
        } else {
            for (x = 0; x < gOutW; x++) gRow[x] = sh[gColMap[x]] & 0x00FFFFFF;
        }
        dy = gOffY + (UINT32)oy;
        if (gUseBlt) {
            gGop->Blt(gGop, gRow, EfiBltBufferToVideo, 0, 0,
                      gOffX, dy, (UINTN)gOutW, 1, 0);
        } else {
            volatile UINT32 *dst = gVram + dy * gStride + gOffX;
            for (x = 0; x < gOutW; x++) dst[x] = gRow[x];
        }
    }
    gShadowValid = 1;
    /* light pacing only - the main loop already sleeps 16 ms on idle frames, so
       an 8 ms tail here just added latency to every interactive (drag) frame. */
    gBS->Stall(1000);
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

/* ===========================================================================
 * power + wall clock (UEFI runtime services) + the startup chime
 * ======================================================================== */
static EFI_RUNTIME_SERVICES *rts(void) { return (EFI_RUNTIME_SERVICES *)gST->RuntimeServices; }

void uno_pc64_shutdown(void) { rts()->ResetSystem(EfiResetShutdown, 0, 0, 0); for(;;){} }
void uno_pc64_restart(void)  { rts()->ResetSystem(EfiResetCold,     0, 0, 0); for(;;){} }

/* wall-clock time from the firmware RTC; returns 1 on success */
int uno_pc64_time(int *y, int *mo, int *d, int *h, int *mi, int *s)
{
    EFI_TIME t;
    if (rts()->GetTime(&t, 0) != EFI_SUCCESS) return 0;
    if (y)  *y  = t.Year;   if (mo) *mo = t.Month;  if (d)  *d  = t.Day;
    if (h)  *h  = t.Hour;   if (mi) *mi = t.Minute; if (s)  *s  = t.Second;
    return 1;
}
int uno_pc64_set_time(int y, int mo, int d, int h, int mi, int s)
{
    EFI_TIME t;
    if (rts()->GetTime(&t, 0) != EFI_SUCCESS) return 0;   /* keep tz/dst fields */
    t.Year = (UINT16)y; t.Month = (UINT8)mo; t.Day = (UINT8)d;
    t.Hour = (UINT8)h;  t.Minute = (UINT8)mi; t.Second = (UINT8)s; t.Nanosecond = 0;
    return rts()->SetTime(&t) == EFI_SUCCESS;
}

/* a short rising arpeggio played after the splash completes (PC speaker) */
void uno_pc64_chime(void)
{
    static const int notes[] = { 60, 64, 67, 72 };   /* C E G C */
    int i;
    for (i = 0; i < 4; i++) { uno_pc64_snd_note(notes[i]); gBS->Stall(110000); }
    uno_pc64_snd_quiet();
}

/* ===========================================================================
 * EFI Simple File System - read FAT (incl. FAT32) / local disks the firmware
 * mounted, the same firmware-as-BIOS approach as GOP. pc64_fs.c wraps this as
 * volumes 1.. (the RAM disk is volume 0). Read-only.
 * ======================================================================== */
typedef struct _EFI_FILE_PROTOCOL EFI_FILE_PROTOCOL;
struct _EFI_FILE_PROTOCOL {
    UINT64 Revision;
    EFI_STATUS (*Open)(EFI_FILE_PROTOCOL *, EFI_FILE_PROTOCOL **, CHAR16 *, UINT64, UINT64);
    EFI_STATUS (*Close)(EFI_FILE_PROTOCOL *);
    EFI_STATUS (*Delete)(EFI_FILE_PROTOCOL *);
    EFI_STATUS (*Read)(EFI_FILE_PROTOCOL *, UINTN *, void *);
    EFI_STATUS (*Write)(EFI_FILE_PROTOCOL *, UINTN *, void *);
    EFI_STATUS (*GetPosition)(EFI_FILE_PROTOCOL *, UINT64 *);
    EFI_STATUS (*SetPosition)(EFI_FILE_PROTOCOL *, UINT64);
    EFI_STATUS (*GetInfo)(EFI_FILE_PROTOCOL *, EFI_GUID *, UINTN *, void *);
    EFI_STATUS (*SetInfo)(EFI_FILE_PROTOCOL *, EFI_GUID *, UINTN, void *);
    EFI_STATUS (*Flush)(EFI_FILE_PROTOCOL *);
};
typedef struct _EFI_SIMPLE_FS EFI_SIMPLE_FS;
struct _EFI_SIMPLE_FS {
    UINT64 Revision;
    EFI_STATUS (*OpenVolume)(EFI_SIMPLE_FS *, EFI_FILE_PROTOCOL **);
};
typedef struct {
    UINT64 Size, FileSize, PhysicalSize;
    EFI_TIME Create, Access, Modify;
    UINT64 Attribute;                    /* bit 0x10 = directory */
    CHAR16 FileName[1];
} EFI_FILE_INFO;

static EFI_GUID gSfsGuid = { 0x964e5b22, 0x6459, 0x11d2,
                             { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } };
static EFI_HANDLE gFsH[16]; static UINTN gNFs; static int gFsScanned;

static void efifs_scan(void)
{
    EFI_HANDLE *b = 0; UINTN n = 0, i;
    if (gFsScanned) return; gFsScanned = 1;
    if (gBS->LocateHandleBuffer(2 /*ByProtocol*/, &gSfsGuid, 0, &n, &b) == EFI_SUCCESS && b) {
        for (i = 0; i < n && gNFs < 16; i++) gFsH[gNFs++] = b[i];
        gBS->FreePool(b);
    }
}
static EFI_FILE_PROTOCOL *fs_root(int vol)
{
    EFI_SIMPLE_FS *sfs; EFI_FILE_PROTOCOL *root = 0;
    efifs_scan();
    if (vol < 0 || vol >= (int)gNFs) return 0;
    if (gBS->HandleProtocol(gFsH[vol], &gSfsGuid, (void **)&sfs) != EFI_SUCCESS) return 0;
    if (sfs->OpenVolume(sfs, &root) != EFI_SUCCESS) return 0;
    return root;
}
static void a_from16(char *d, const CHAR16 *s, int max)
{ int i; for (i = 0; i < max - 1 && s[i]; i++) d[i] = (s[i] > 0 && s[i] < 128) ? (char)s[i] : '?'; d[i] = 0; }
static void a_to16(CHAR16 *d, const char *s, int max)
{ int i; for (i = 0; i < max - 1 && s[i]; i++) d[i] = (unsigned char)s[i]; d[i] = 0; }

int  uno_efifs_volumes(void) { efifs_scan(); return (int)gNFs; }

/* BPB volume serial of a firmware SFS volume, for de-duplicating it against a
 * native FAT mount of the same partition.  The firmware File-System-Info does
 * not expose it, and re-reading the boot sector via the SFS parent Block-IO is
 * more than the dedup is worth here: in practice the native stack and firmware
 * SFS reach DISJOINT media on the machines we target (native AHCI SATA vs the
 * firmware-only USB boot stick behind xHCI), so 0 = "unknown, don't dedupe" is
 * correct.  If a future machine mounts one partition both ways, wire this to a
 * real boot-sector read. */
unsigned int uno_efifs_serial(int vol) { (void)vol; return 0; }

int uno_efifs_snapshot(int vol, char (*names)[32], int maxn)
{
    EFI_FILE_PROTOCOL *root = fs_root(vol); int cnt = 0;
    static unsigned char info[1024];
    if (!root) return 0;
    for (;;) {
        UINTN sz = sizeof info;
        if (root->Read(root, &sz, info) != EFI_SUCCESS || sz == 0) break;
        { EFI_FILE_INFO *fi = (EFI_FILE_INFO *)info;
          if (!(fi->Attribute & 0x10) && fi->FileName[0] != '.') {
              if (cnt < maxn) { a_from16(names[cnt], fi->FileName, 32); cnt++; }
          } }
    }
    root->Close(root);
    return cnt;
}
long uno_efifs_read(int vol, const char *name, unsigned char *buf, long max)
{
    EFI_FILE_PROTOCOL *root = fs_root(vol), *f = 0; CHAR16 wn[80]; long total = 0;
    if (!root) return -1;
    a_to16(wn, name, 80);
    if (root->Open(root, &f, wn, 1, 0) != EFI_SUCCESS || !f) { root->Close(root); return -1; }
    while (total < max) {
        UINTN sz = (UINTN)(max - total);
        if (f->Read(f, &sz, buf + total) != EFI_SUCCESS || sz == 0) break;
        total += (long)sz;
    }
    f->Close(f); root->Close(root);
    return total;
}
