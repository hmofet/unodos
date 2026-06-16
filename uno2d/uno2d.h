/* uno2d — the 2D Primitive Vtable (CONTRACT-ARCH §5/§6: one TALL boundary).
 *
 * Generalizes fb.h. A backend MUST provide the floor (put_pixel) and MAY override
 * higher-altitude ops (fill/blit/hline/vline) where its hardware accelerates them
 * (the Amiga blitter, a console GPU). The core calls the highest rung a backend
 * offers and SYNTHESIZES the rest from put_pixel — so a new port runs everything by
 * implementing only the floor, and the portable core is never the ceiling.
 *
 * The load-bearing invariant (verified by uno2d_test): an accelerated override must
 * produce pixel-identical output to the software synthesis. That is what lets the
 * software floor be the conformance oracle for hardware backends.
 */
#ifndef UNO2D_H
#define UNO2D_H
#include <stdint.h>
#include <stddef.h>

typedef struct { int w, h, pitch; uint8_t *px; } u2d_surface_t;   /* 8bpp indexed */

enum { U2D_CAP_FILL = 1, U2D_CAP_BLIT = 2, U2D_CAP_HLINE = 4, U2D_CAP_VLINE = 8 };

typedef struct u2d_backend {
    uint32_t caps;                                   /* which optional ops are present */
    /* MANDATORY floor */
    void (*put_pixel)(u2d_surface_t *s, int x, int y, uint8_t c);
    /* OPTIONAL accel overrides (NULL => synthesized from the floor) */
    void (*fill_rect)(u2d_surface_t *s, int x, int y, int w, int h, uint8_t c);
    void (*blit)(u2d_surface_t *d, int dx, int dy, const u2d_surface_t *s, int sx, int sy, int w, int h);
    void (*hline)(u2d_surface_t *s, int x, int y, int len, uint8_t c);
    void (*vline)(u2d_surface_t *s, int x, int y, int len, uint8_t c);
} u2d_backend_t;

/* core ops: call the highest rung the backend offers, else synthesize */
void u2d_put_pixel(const u2d_backend_t *b, u2d_surface_t *s, int x, int y, uint8_t c);
void u2d_fill_rect(const u2d_backend_t *b, u2d_surface_t *s, int x, int y, int w, int h, uint8_t c);
void u2d_hline(const u2d_backend_t *b, u2d_surface_t *s, int x, int y, int len, uint8_t c);
void u2d_vline(const u2d_backend_t *b, u2d_surface_t *s, int x, int y, int len, uint8_t c);
void u2d_blit(const u2d_backend_t *b, u2d_surface_t *d, int dx, int dy,
              const u2d_surface_t *s, int sx, int sy, int w, int h);

/* the universal software floor backend (put_pixel only — everything synthesized) */
const u2d_backend_t *u2d_soft_floor(void);

u2d_surface_t *u2d_surface_alloc(int w, int h);
void           u2d_surface_free(u2d_surface_t *s);
int            u2d_write_ppm(const u2d_surface_t *s, const char *path);  /* evidence */

#endif /* UNO2D_H */
