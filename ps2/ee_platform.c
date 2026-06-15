/* ===========================================================================
 * UnoDOS/PS2 EE platform layer for the FULL desktop (M1 on real hardware).
 *
 * The portable core ps2/unodos.c owns main(); built with -DUNO_EE it drives
 * these three hooks instead of returning after one frame (host) or looping on
 * the splash (the standalone main.c M0 target):
 *
 *   uno_ee_init()     - GS init (640x448 NTSC, double-buffered) + the fb texture
 *                       + SIO2MAN/PADMAN, once at startup.
 *   uno_ee_poll()     - read the DualShock 2 and translate to UnoDOS key events
 *                       (edge-detected): d-pad -> arrow keys (desktop icon nav /
 *                       in-app movement), Cross -> Return (launch/select),
 *                       Circle -> Return, Start -> Esc (close). Posted into the
 *                       shim's event queue, so the core's normal GetNextEvent
 *                       loop consumes them.
 *   uno_ee_present()  - upload fb to GS VRAM and blit the fullscreen sprite,
 *                       once per loop iteration (the vsync present).
 *
 * GS is a blitter only - all drawing is the software framebuffer (HANDOFF SS2),
 * so the entire desktop/WM/app suite that the host shim renders comes across
 * unchanged; only present + input are EE-specific.
 * ======================================================================== */
#include <kernel.h>
#include <sifrpc.h>
#include <loadfile.h>
#include <libpad.h>
#include <gsKit.h>
#include <dmaKit.h>
#include <gsToolkit.h>
#include <libmc.h>
#include <string.h>

#include "fb.h"
#include "mac_compat.h"

static GSGLOBAL *g_gs;
static GSTEXTURE g_tex;
static char g_padbuf[256] __attribute__((aligned(64)));
static u16  g_prev = 0xFFFF;          /* previous pad button word (0 = pressed) */

static void load_modules(void)
{
    SifInitRpc(0);
    SifLoadModule("rom0:SIO2MAN", 0, NULL);   /* pad + memory-card transport */
    SifLoadModule("rom0:PADMAN", 0, NULL);    /* DualShock 2 */
    SifLoadModule("rom0:MCMAN", 0, NULL);     /* memory-card manager (M2 store) */
    SifLoadModule("rom0:MCSERV", 0, NULL);    /* memory-card file server */
}

/* Bring up the memory card so mc0: file ops (mac_io.c EE backend) work. The
   first mcGetInfo also reports whether the card is formatted; an unformatted
   card (a brand-new PCSX2 Mcd) is formatted once so Files/Notepad can persist. */
static void mc_bringup(void)
{
    int ret = 0;
    mcInit(MC_TYPE_MC);
    /* Probe by making /UnoDOS. MCMAN reports mcGetInfo's format flag
       unreliably, so we trust the mkdir result instead: sceMcResNoFormat (-2)
       means the card is raw -> format once, then re-make the dir. Any other
       result (created, or "already exists") leaves an existing card UNTOUCHED,
       so saves persist across boots. */
    mcMkDir(0, 0, "/UnoDOS");
    mcSync(0, NULL, &ret);
    if (ret == -2) {                          /* sceMcResNoFormat */
        mcFormat(0, 0);
        mcSync(0, NULL, &ret);
        mcMkDir(0, 0, "/UnoDOS");
        mcSync(0, NULL, &ret);
    }
}

void uno_ee_init(void)
{
    g_gs = gsKit_init_global();
    g_gs->Mode = GS_MODE_NTSC;
    g_gs->Width = FB_W;
    g_gs->Height = FB_H;
    g_gs->PSM = GS_PSM_CT32;
    g_gs->PSMZ = GS_PSMZ_16S;
    g_gs->Interlace = GS_INTERLACED;
    g_gs->Field = GS_FIELD;
    g_gs->DoubleBuffering = GS_SETTING_ON;
    g_gs->ZBuffering = GS_SETTING_OFF;

    dmaKit_init(D_CTRL_RELE_OFF, D_CTRL_MFD_OFF, D_CTRL_STS_UNSPEC,
                D_CTRL_STD_OFF, D_CTRL_RCYC_8, 1 << DMA_CHANNEL_GIF);
    dmaKit_chan_init(DMA_CHANNEL_GIF);
    gsKit_init_screen(g_gs);
    gsKit_mode_switch(g_gs, GS_ONESHOT);

    memset(&g_tex, 0, sizeof(g_tex));
    g_tex.Width = FB_W;
    g_tex.Height = FB_H;
    g_tex.PSM = GS_PSM_CT32;
    g_tex.Filter = GS_FILTER_NEAREST;
    g_tex.Mem = (u32 *)fb;
    g_tex.Vram = gsKit_vram_alloc(g_gs,
        gsKit_texture_size(g_tex.Width, g_tex.Height, g_tex.PSM),
        GSKIT_ALLOC_USERBUFFER);

    load_modules();
    mc_bringup();                 /* memory card -> mc0: for the File Manager */
    padInit(0);
    padPortOpen(0, 0, g_padbuf);
}

static int pad_ready(int port, int slot)
{
    int s = padGetState(port, slot);
    return (s == PAD_STATE_STABLE || s == PAD_STATE_FINDCTP1);
}

/* post a keyDown into the shim queue: message = (keycode<<8)|charcode */
static void post_key(short keycode, char ch)
{
    Point p = { 0, 0 };
    long msg = ((long)(keycode & 0xFF) << 8) | (unsigned char)ch;
    uno_post_event(keyDown, msg, p, 0);
}

void uno_ee_poll(void)
{
    struct padButtonStatus btn;
    u16 now, edge;
    if (!pad_ready(0, 0) || padRead(0, 0, &btn) == 0) return;
    now = btn.btns;                       /* libpad: bit 0 => pressed */
    edge = (u16)(g_prev & ~now);          /* newly-pressed this frame */
    g_prev = now;

    /* d-pad -> arrow keys (code + classic arrow ascii) */
    if (edge & PAD_LEFT)  post_key(0x7B, 0x1C);
    if (edge & PAD_RIGHT) post_key(0x7C, 0x1D);
    if (edge & PAD_UP)    post_key(0x7E, 0x1E);
    if (edge & PAD_DOWN)  post_key(0x7D, 0x1F);
    /* Cross / Circle -> Return (launch icon / confirm) */
    if (edge & PAD_CROSS)  post_key(0x24, 0x0D);
    if (edge & PAD_CIRCLE) post_key(0x24, 0x0D);
    /* Start -> Esc (close focused window) */
    if (edge & PAD_START)  post_key(0x35, 0x1B);
}

void uno_ee_present(void)
{
    gsKit_texture_upload(g_gs, &g_tex);
    gsKit_prim_sprite_texture(g_gs, &g_tex,
        0.0f, 0.0f, 0.0f, 0.0f,
        (float)FB_W, (float)FB_H, (float)FB_W, (float)FB_H,
        2, GS_SETREG_RGBAQ(0x80, 0x80, 0x80, 0x80, 0x00));
    gsKit_queue_exec(g_gs);
    gsKit_sync_flip(g_gs);
    gsKit_TexManager_nextFrame(g_gs);
}
