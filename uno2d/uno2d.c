/* uno2d core + software floor. See uno2d.h. */
#include "uno2d.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "unodef.h"                 /* generated: uno_ui_palette_rgb24 for PPM output */

/* ---- software floor: the ONLY mandatory op ------------------------------- */
static void soft_put_pixel(u2d_surface_t *s, int x, int y, uint8_t c) {
    if (x < 0 || y < 0 || x >= s->w || y >= s->h) return;   /* clip */
    s->px[y * s->pitch + x] = c;
}
static const u2d_backend_t SOFT = { 0, soft_put_pixel, NULL, NULL, NULL, NULL };
const u2d_backend_t *u2d_soft_floor(void) { return &SOFT; }

/* ---- core: highest rung the backend offers, else synthesize from put_pixel */
void u2d_put_pixel(const u2d_backend_t *b, u2d_surface_t *s, int x, int y, uint8_t c) {
    b->put_pixel(s, x, y, c);
}
void u2d_fill_rect(const u2d_backend_t *b, u2d_surface_t *s, int x, int y, int w, int h, uint8_t c) {
    if (b->fill_rect) { b->fill_rect(s, x, y, w, h, c); return; }
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) b->put_pixel(s, x + i, y + j, c);   /* synth */
}
void u2d_hline(const u2d_backend_t *b, u2d_surface_t *s, int x, int y, int len, uint8_t c) {
    if (b->hline) { b->hline(s, x, y, len, c); return; }
    for (int i = 0; i < len; i++) b->put_pixel(s, x + i, y, c);
}
void u2d_vline(const u2d_backend_t *b, u2d_surface_t *s, int x, int y, int len, uint8_t c) {
    if (b->vline) { b->vline(s, x, y, len, c); return; }
    for (int i = 0; i < len; i++) b->put_pixel(s, x, y + i, c);
}
void u2d_blit(const u2d_backend_t *b, u2d_surface_t *d, int dx, int dy,
              const u2d_surface_t *s, int sx, int sy, int w, int h) {
    if (b->blit) { b->blit(d, dx, dy, s, sx, sy, w, h); return; }
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            if (sx + i < 0 || sy + j < 0 || sx + i >= s->w || sy + j >= s->h) continue;
            b->put_pixel(d, dx + i, dy + j, s->px[(sy + j) * s->pitch + (sx + i)]);
        }
}

/* ---- surfaces + PPM evidence --------------------------------------------- */
u2d_surface_t *u2d_surface_alloc(int w, int h) {
    u2d_surface_t *s = calloc(1, sizeof *s);
    s->w = w; s->h = h; s->pitch = w; s->px = calloc((size_t)w * h, 1);
    return s;
}
void u2d_surface_free(u2d_surface_t *s) { if (s) { free(s->px); free(s); } }

int u2d_write_ppm(const u2d_surface_t *s, const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return 1;
    fprintf(f, "P6\n%d %d\n255\n", s->w, s->h);
    for (int y = 0; y < s->h; y++)
        for (int x = 0; x < s->w; x++) {
            uint8_t c = s->px[y * s->pitch + x], r, g, bl;
            if (c < 4) {                              /* UI palette indices via Contract */
                uint32_t rgb = uno_ui_palette_rgb24[c];
                r = (rgb >> 16) & 0xFF; g = (rgb >> 8) & 0xFF; bl = rgb & 0xFF;
            } else { r = g = bl = c; }                /* extended indices: grayscale */
            fputc(r, f); fputc(g, f); fputc(bl, f);
        }
    fclose(f);
    return 0;
}
