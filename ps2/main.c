/* ===========================================================================
 * UnoDOS/PS2 milestone-0 EE target - GS init + framebuffer blit + DualShock2
 * cursor (HANDOFF SS2/SS4).
 *
 * The software framebuffer (fb.c) is uploaded to GS VRAM each vsync as a
 * 640x448 RGBA32 texture and drawn as one fullscreen sprite - the GS is a
 * blitter, nothing more (the design that keeps unodos.c's incremental/XOR
 * drawing exact). Input is the DualShock2 read through SIO2MAN+PADMAN: the
 * d-pad and left analog stick move the cursor, Start quits.
 *
 * NOTE: this file needs the PS2SDK (ee-gcc + gsKit/libpad) to build and PCSX2
 * or real hardware (FMCB) to run - NEITHER is available on the dev machine
 * that wrote it, so it is UNVERIFIED. fb.c / uno_splash.c (which it shares
 * verbatim with the host shim) ARE verified on the PC. Build with `./build.sh
 * ee` once PS2SDK is installed; first-run checklist in README.md.
 * ===========================================================================
 */
#include <kernel.h>
#include <sifrpc.h>
#include <loadfile.h>
#include <libpad.h>
#include <gsKit.h>
#include <dmaKit.h>
#include <gsToolkit.h>
#include <string.h>

#include "fb.h"

void uno_render_splash(int cx, int cy);

/* pad DMA buffer (64-byte aligned, as libpad requires) */
static char g_padbuf[256] __attribute__((aligned(64)));

static void load_modules(void)
{
    /* SIO2MAN + PADMAN ship in rom0: on every PS2; fall back to embedded IRX
       only if a target lacks them (HANDOFF SS2). */
    SifInitRpc(0);
    SifLoadModule("rom0:SIO2MAN", 0, NULL);
    SifLoadModule("rom0:PADMAN", 0, NULL);
}

static int pad_ready(int port, int slot)
{
    int state = padGetState(port, slot);
    return (state == PAD_STATE_STABLE || state == PAD_STATE_FINDCTP1);
}

int main(int argc, char **argv)
{
    GSGLOBAL *gs;
    GSTEXTURE tex;
    int cx = FB_W / 2, cy = FB_H / 2;
    struct padButtonStatus btn;

    (void)argc; (void)argv;

    /* ---- GS: 640x448 NTSC interlaced, double-buffered ---- */
    gs = gsKit_init_global();
    gs->Mode = GS_MODE_NTSC;
    gs->Width = FB_W;
    gs->Height = FB_H;
    gs->PSM = GS_PSM_CT32;
    gs->PSMZ = GS_PSMZ_16S;
    gs->Interlace = GS_INTERLACED;
    gs->Field = GS_FIELD;
    gs->DoubleBuffering = GS_SETTING_ON;
    gs->ZBuffering = GS_SETTING_OFF;

    dmaKit_init(D_CTRL_RELE_OFF, D_CTRL_MFD_OFF, D_CTRL_STS_UNSPEC,
                D_CTRL_STD_OFF, D_CTRL_RCYC_8, 1 << DMA_CHANNEL_GIF);
    dmaKit_chan_init(DMA_CHANNEL_GIF);
    gsKit_init_screen(gs);
    gsKit_mode_switch(gs, GS_ONESHOT);

    /* ---- our framebuffer as a GS texture (re-uploaded each frame) ---- */
    memset(&tex, 0, sizeof(tex));
    tex.Width = FB_W;
    tex.Height = FB_H;
    tex.PSM = GS_PSM_CT32;
    tex.Filter = GS_FILTER_NEAREST;
    tex.Mem = (u32 *)fb;
    tex.Vram = gsKit_vram_alloc(gs, gsKit_texture_size(tex.Width, tex.Height, tex.PSM),
                                GSKIT_ALLOC_USERBUFFER);

    /* ---- pad ---- */
    load_modules();
    padInit(0);
    padPortOpen(0, 0, g_padbuf);

    for (;;) {
        u16 pressed = 0;

        if (pad_ready(0, 0) && padRead(0, 0, &btn) != 0) {
            pressed = 0xFFFF ^ btn.btns;        /* libpad: 0 = pressed */

            if (pressed & PAD_LEFT)  cx -= 4;
            if (pressed & PAD_RIGHT) cx += 4;
            if (pressed & PAD_UP)    cy -= 4;
            if (pressed & PAD_DOWN)  cy += 4;

            /* left analog stick (DualShock2): centre ~0x80, deadzone 0x20 */
            if (btn.ljoy_h < 0x60) cx -= (0x60 - btn.ljoy_h) / 12;
            if (btn.ljoy_h > 0xA0) cx += (btn.ljoy_h - 0xA0) / 12;
            if (btn.ljoy_v < 0x60) cy -= (0x60 - btn.ljoy_v) / 12;
            if (btn.ljoy_v > 0xA0) cy += (btn.ljoy_v - 0xA0) / 12;

            if (cx < 0) cx = 0;
            if (cx > FB_W - 1) cx = FB_W - 1;
            if (cy < 0) cy = 0;
            if (cy > FB_H - 1) cy = FB_H - 1;

            if (pressed & PAD_START) break;     /* Start = quit (Esc role) */
        }

        /* render into the software FB, blit it to the GS */
        uno_render_splash(cx, cy);
        gsKit_texture_upload(gs, &tex);
        gsKit_prim_sprite_texture(gs, &tex,
            0.0f, 0.0f, 0.0f, 0.0f,
            (float)FB_W, (float)FB_H, (float)FB_W, (float)FB_H,
            2, GS_SETREG_RGBAQ(0x80, 0x80, 0x80, 0x80, 0x00));

        gsKit_queue_exec(gs);
        gsKit_sync_flip(gs);
        gsKit_TexManager_nextFrame(gs);
    }

    padPortClose(0, 0);
    padEnd();
    return 0;
}
