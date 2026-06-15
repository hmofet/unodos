/* ===========================================================================
 * uno3d PS2 backend - the PlayStation 2 Graphics Synthesizer via gsKit.
 *
 * This is REAL hardware-accelerated 3D: the GS is a hardware rasteriser, so the
 * screen-space triangles the uno3d pipeline produces are drawn by the GPU with a
 * hardware z-buffer and hardware gouraud interpolation - the CPU (EE) only does
 * the transform, exactly as a 3D game would. (The rest of the UnoDOS PS2 port
 * uses the GS merely as a blitter; this proves the same chip does accelerated
 * 3D.)
 *
 * The pipeline hands us triangles already transformed, projected, culled and in
 * screen space (u3d_stri: pixel x/y, depth 0..1, gouraud colour). We map depth
 * onto the GS 32-bit z-buffer (larger = nearer, matching gsKit's GEQUAL test)
 * and emit one gouraud-shaded triangle primitive each.
 * ======================================================================== */
#include "uno3d_backend.h"
#include <gsKit.h>
#include <dmaKit.h>
#include <gsToolkit.h>

static GSGLOBAL *g_gs;

static int ps2_init(int w, int h)
{
    g_gs = gsKit_init_global();
    g_gs->Mode = GS_MODE_NTSC;
    g_gs->Width  = w;
    g_gs->Height = h;
    g_gs->PSM  = GS_PSM_CT32;
    g_gs->PSMZ = GS_PSMZ_32;              /* 32-bit hardware depth buffer */
    g_gs->Interlace = GS_INTERLACED;
    g_gs->Field = GS_FIELD;
    g_gs->DoubleBuffering = GS_SETTING_ON;
    g_gs->ZBuffering = GS_SETTING_ON;     /* HW z-test - the point of this path */

    dmaKit_init(D_CTRL_RELE_OFF, D_CTRL_MFD_OFF, D_CTRL_STS_UNSPEC,
                D_CTRL_STD_OFF, D_CTRL_RCYC_8, 1 << DMA_CHANNEL_GIF);
    dmaKit_chan_init(DMA_CHANNEL_GIF);
    gsKit_init_screen(g_gs);
    gsKit_mode_switch(g_gs, GS_ONESHOT);
    gsKit_set_test(g_gs, GS_ZTEST_ON);    /* enable the depth comparison */
    return 0;
}

static void ps2_shutdown(void) { }

static void ps2_clear(unsigned char r, unsigned char g, unsigned char b)
{
    gsKit_clear(g_gs, GS_SETREG_RGBAQ(r, g, b, 0x80, 0x00));
}

/* depth 0(near)..1(far) -> GS z 0..2^24-1, inverted so nearer = larger
   (gsKit's z-test passes the GREATER-or-equal z). */
static int zmap(float d)
{
    if (d < 0.0f) d = 0.0f;
    if (d > 1.0f) d = 1.0f;
    return (int)((1.0f - d) * 16777215.0f);
}

static void ps2_tri(const u3d_stri *t)
{
    const u3d_sv *v = t->v;
    gsKit_prim_triangle_gouraud_3d(g_gs,
        v[0].sx, v[0].sy, zmap(v[0].z),
        v[1].sx, v[1].sy, zmap(v[1].z),
        v[2].sx, v[2].sy, zmap(v[2].z),
        GS_SETREG_RGBAQ((u8)v[0].r, (u8)v[0].g, (u8)v[0].b, 0x80, 0x00),
        GS_SETREG_RGBAQ((u8)v[1].r, (u8)v[1].g, (u8)v[1].b, 0x80, 0x00),
        GS_SETREG_RGBAQ((u8)v[2].r, (u8)v[2].g, (u8)v[2].b, 0x80, 0x00));
}

static void ps2_flush(void) { gsKit_queue_exec(g_gs); }

static void ps2_present(void)
{
    gsKit_sync_flip(g_gs);
    gsKit_TexManager_nextFrame(g_gs);
}

const u3d_backend u3d_backend_ps2 = {
    "ps2-gs",
    U3D_CAP_HW | U3D_CAP_ZBUFFER | U3D_CAP_GOURAUD,
    ps2_init, ps2_shutdown, ps2_clear, ps2_tri, ps2_flush, ps2_present
};
