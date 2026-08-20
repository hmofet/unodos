/* LIFE.C - Conway's Game of Life.
 *
 * The classic cellular automaton on a wrapping grid.  Space pauses,
 * S steps one generation while paused, N reseeds the board, C clears it.
 * Cells that survive a while cool from green to blue.
 *
 * What it demonstrates: 2-D arrays and real computation in UnoC, seeding
 * randomness with Random(), pacing a simulation by counting tick() calls
 * (the shell's ~60 Hz frame is the platform's timebase), and painting
 * only what is alive instead of every cell.  Build with Ctrl-B, run with
 * Ctrl-R.
 */
#include "UNO.H"

#define COLS 44
#define ROWS 30
#define CELL 6

static unsigned char g[ROWS][COLS];   /* 0 dead, else generations alive */
static unsigned char nx[ROWS][COLS];
static Boolean running = true;
static Boolean seeded  = false;
static long gen;
static long pop;
static short acc;                     /* frames since the last step */

static const GameRGB kBack  = {  12,  14,  24, C_BLUE };
static const GameRGB kYoung = { 120, 220, 140, C_CYAN };
static const GameRGB kOld   = {  70, 140, 220, C_CYAN };

static void life_seed(void)
{
    short r, c;
    pop = 0;
    for (r = 0; r < ROWS; r++)
        for (c = 0; c < COLS; c++) {
            /* stir the coordinates in so even a weak Random() seeds life */
            short v = (short)(((Random() + r * 31 + c * 17) & 0x7FFF) % 100);
            g[r][c] = (unsigned char)(v < 30 ? 1 : 0);
            if (g[r][c]) pop++;
        }
    gen = 0;
}

static short life_neigh(short r, short c)
{
    short dr, dc, n = 0;
    for (dr = -1; dr <= 1; dr++)
        for (dc = -1; dc <= 1; dc++) {
            short rr, cc;
            if (dr == 0 && dc == 0) continue;
            rr = (short)((r + dr + ROWS) % ROWS);   /* the grid wraps */
            cc = (short)((c + dc + COLS) % COLS);
            if (g[rr][cc]) n++;
        }
    return n;
}

static void life_step(void)
{
    short r, c, n;
    pop = 0;
    for (r = 0; r < ROWS; r++)
        for (c = 0; c < COLS; c++) {
            n = life_neigh(r, c);
            if (g[r][c])
                nx[r][c] = (unsigned char)
                    ((n == 2 || n == 3) ? (g[r][c] < 200 ? g[r][c] + 1 : 200) : 0);
            else
                nx[r][c] = (unsigned char)(n == 3 ? 1 : 0);
            if (nx[r][c]) pop++;
        }
    memcpy(g, nx, sizeof(g));
    gen++;
}

static void life_draw(UnoWin *w)
{
    Rect f, q;
    short r, c, x, y;
    char num[16];
    short x0 = w->bounds.left + 8;
    short y0 = w->bounds.top + TBAR_H + 6;
    short by = (short)(y0 + ROWS * CELL);

    SetRect(&f, x0 - 2, y0 - 2, x0 + COLS * CELL + 2, by + 2);
    uno_box(&f, C_WHITE);
    SetRect(&f, x0, y0, x0 + COLS * CELL, by);
    fill_rgb(&f, &kBack);

    for (r = 0; r < ROWS; r++)
        for (c = 0; c < COLS; c++)
            if (g[r][c]) {
                x = (short)(x0 + c * CELL);
                y = (short)(y0 + r * CELL);
                SetRect(&q, x, y, x + CELL - 1, y + CELL - 1);
                fill_rgb(&q, g[r][c] > 4 ? &kOld : &kYoung);
            }

    text_at(x0, by + 8, "Gen:", C_CYAN, C_BLUE, false);
    fmt_u(gen, num);
    text_at(x0 + 38, by + 8, num, C_WHITE, C_BLUE, false);
    text_at(x0 + 100, by + 8, "Pop:", C_CYAN, C_BLUE, false);
    fmt_u(pop, num);
    text_at(x0 + 138, by + 8, num, C_WHITE, C_BLUE, false);
    text_at(x0, by + 24,
            running ? "Space: pause   N: reseed   C: clear"
                    : "PAUSED - Space runs, S steps once",
            C_MAG, C_BLUE, false);
}

static Boolean life_key(char ch, short code, Boolean cmd)
{
    if (cmd) return false;
    if (ch == ' ') {
        running = !running;
        repaint_all();
        return true;
    }
    if (ch == 's' || ch == 'S') {
        running = false;
        life_step();
        repaint_all();
        return true;
    }
    if (ch == 'n' || ch == 'N') {
        life_seed();
        repaint_all();
        return true;
    }
    if (ch == 'c' || ch == 'C') {
        memset(g, 0, sizeof(g));
        gen = 0;
        pop = 0;
        running = false;
        repaint_all();
        return true;
    }
    (void)code;
    return false;
}

/* tick() arrives once per shell frame, ~60/s - count frames yourself
 * rather than trusting TickCount(), which counts calls, not time. */
static void life_tick(void)
{
    if (!running) return;
    if (++acc < 6) return;                /* ~10 generations a second */
    acc = 0;
    life_step();
    repaint_all();
}

static void life_opened(void)
{
    if (!seeded) {                        /* opened() can run again after a
                                           * close/reopen - keep the board */
        life_seed();
        seeded = true;
    }
}

static const AppInterface kIface = {
    life_draw, life_key, 0, life_tick, life_opened, 0,
    "Life", { 30, 30, 320, 280 }
};

const AppInterface *uno_app_main(const KernelApi *k)
{
    gK = k;
    return &kIface;
}
