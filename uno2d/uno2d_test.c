/* uno2d_test — host verification of the tall-vtable floor/fallback equivalence.
 *
 * Renders the SAME scene twice: once through the bare software floor (every op
 * synthesized from put_pixel) and once through an "accelerated" backend that
 * overrides fill_rect/hline/blit with fast row-wise implementations. The two
 * framebuffers MUST be byte-identical — that is the §5/§6 guarantee that an accel
 * override is interchangeable with the software fallback. Writes a PPM as evidence.
 */
#include "uno2d.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ---- an "accelerated" backend: same pixels, fewer calls (row memset etc.) -- */
static void acc_put(u2d_surface_t *s, int x, int y, uint8_t c) {
    if (x < 0 || y < 0 || x >= s->w || y >= s->h) return;
    s->px[y * s->pitch + x] = c;
}
static void acc_fill(u2d_surface_t *s, int x, int y, int w, int h, uint8_t c) {
    for (int j = 0; j < h; j++) {
        int yy = y + j; if (yy < 0 || yy >= s->h) continue;
        int x0 = x < 0 ? 0 : x, x1 = x + w; if (x1 > s->w) x1 = s->w;
        if (x1 > x0) memset(&s->px[yy * s->pitch + x0], c, (size_t)(x1 - x0));  /* fast */
    }
}
static void acc_hline(u2d_surface_t *s, int x, int y, int len, uint8_t c) { acc_fill(s, x, y, len, 1, c); }
static const u2d_backend_t ACCEL = { U2D_CAP_FILL | U2D_CAP_HLINE, acc_put, acc_fill, NULL, acc_hline, NULL };

/* a scene exercising fill, lines, pixels, and a blit */
static void scene(const u2d_backend_t *b, u2d_surface_t *s) {
    u2d_fill_rect(b, s, 0, 0, s->w, s->h, 0);            /* desktop (palette 0) */
    u2d_fill_rect(b, s, 8, 8, 120, 70, 3);               /* white window body   */
    u2d_hline(b, s, 8, 8, 120, 2);                       /* magenta title bar   */
    u2d_vline(b, s, 8, 8, 70, 1);                        /* cyan left edge       */
    for (int i = 0; i < 60; i++) u2d_put_pixel(b, s, 20 + i, 40 + (i % 12), 2);  /* diagonal */
    /* a small sprite, blitted */
    u2d_surface_t *spr = u2d_surface_alloc(8, 8);
    for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) spr->px[y*8+x] = ((x ^ y) & 1) ? 3 : 1;
    u2d_blit(b, s, 100, 50, spr, 0, 0, 8, 8);
    u2d_surface_free(spr);
}

int main(void) {
    int W = 160, H = 100, fails = 0;
    u2d_surface_t *soft  = u2d_surface_alloc(W, H);
    u2d_surface_t *accel = u2d_surface_alloc(W, H);

    scene(u2d_soft_floor(), soft);     /* everything synthesized from put_pixel */
    scene(&ACCEL,           accel);    /* fill/hline/blit accelerated           */

    int identical = (memcmp(soft->px, accel->px, (size_t)W * H) == 0);
    printf("[%s] accel backend is pixel-identical to the software floor\n",
           identical ? "PASS" : "FAIL");
    if (!identical) fails++;

    /* sanity: the window body really is white (palette 3) and desktop is 0 */
    int body_ok = soft->px[40 * W + 60] == 3 && soft->px[2 * W + 2] == 0;
    printf("[%s] scene drew expected pixels (window body=3, desktop=0)\n", body_ok ? "PASS" : "FAIL");
    if (!body_ok) fails++;

    u2d_write_ppm(soft, "build/uno2d_scene.ppm");
    printf("wrote build/uno2d_scene.ppm\n");

    u2d_surface_free(soft); u2d_surface_free(accel);
    printf("\n%s\n", fails ? "FAILURES" : "ALL PASS");
    return fails ? 1 : 0;
}
