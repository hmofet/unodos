/* ===========================================================================
 * UnoDOS Runner - a simple 3D obstacle-dodger built on uno3d.
 *
 * You pilot a ship down a corridor; walls of blocks rush toward you, each with
 * one gap. Steer left/right to line up with the gap and pass through; clip a
 * block and you crash (press Start/Fire to restart). Score = walls cleared, and
 * the corridor speeds up as you go.
 *
 * Pure game logic + uno3d draw calls - identical on every platform. The only
 * per-platform code is a ~40-line glue main that maps real input to game_input
 * and presents the frame.
 * ======================================================================== */
#include "uno3d.h"
#include "uno3d_game.h"

/* ---- a unit cube (+/-0.5), 12 triangles; colours filled per draw ---- */
#define P(x,y,z) (float)(x),(float)(y),(float)(z)
static const float kCubePos[36][3] = {
    /* +Z front */ {P(-.5,-.5,.5)},{P(.5,-.5,.5)},{P(.5,.5,.5)}, {P(-.5,-.5,.5)},{P(.5,.5,.5)},{P(-.5,.5,.5)},
    /* -Z back  */ {P(.5,-.5,-.5)},{P(-.5,-.5,-.5)},{P(-.5,.5,-.5)}, {P(.5,-.5,-.5)},{P(-.5,.5,-.5)},{P(.5,.5,-.5)},
    /* +X right */ {P(.5,-.5,.5)},{P(.5,-.5,-.5)},{P(.5,.5,-.5)}, {P(.5,-.5,.5)},{P(.5,.5,-.5)},{P(.5,.5,.5)},
    /* -X left  */ {P(-.5,-.5,-.5)},{P(-.5,-.5,.5)},{P(-.5,.5,.5)}, {P(-.5,-.5,-.5)},{P(-.5,.5,.5)},{P(-.5,.5,-.5)},
    /* +Y top   */ {P(-.5,.5,.5)},{P(.5,.5,.5)},{P(.5,.5,-.5)}, {P(-.5,.5,.5)},{P(.5,.5,-.5)},{P(-.5,.5,-.5)},
    /* -Y bottom*/ {P(-.5,-.5,-.5)},{P(.5,-.5,-.5)},{P(.5,-.5,.5)}, {P(-.5,-.5,-.5)},{P(.5,-.5,.5)},{P(-.5,-.5,.5)},
};
/* per-face brightness for a cheap lit look (top bright, sides dim) */
static const float kFaceShade[6] = { 0.80f, 0.55f, 0.65f, 0.65f, 1.00f, 0.45f };

static u3d_vert g_box[36];

static void draw_box(float cx, float cy, float cz, float sx, float sy, float sz,
                     int r, int g, int b)
{
    int i;
    for (i = 0; i < 36; i++) {
        float sh = kFaceShade[i / 6];
        g_box[i].x = kCubePos[i][0];
        g_box[i].y = kCubePos[i][1];
        g_box[i].z = kCubePos[i][2];
        g_box[i].r = (unsigned char)(r * sh);
        g_box[i].g = (unsigned char)(g * sh);
        g_box[i].b = (unsigned char)(b * sh);
    }
    u3d_load_identity();
    u3d_translate(cx, cy, cz);
    u3d_scale(sx, sy, sz);
    u3d_triangles(g_box, 12);
}

/* ---- world ------------------------------------------------------------- */
#define NWALLS    5
#define SLOTS     9           /* block columns across the corridor */
#define HALF_W    9.0f        /* corridor half-width in world units */
#define PLAYER_Z  (-3.0f)
#define SPAWN_Z   (-48.0f)
#define GAP_SLOTS 2           /* width of the gap, in columns */

typedef struct { float z; int gap; } Wall;   /* gap = leftmost open column */

static float g_aspect = 4.0f/3.0f;
static float g_px;            /* player x */
static Wall  g_wall[NWALLS];
static float g_speed;
static int   g_score;
static int   g_dead;
static unsigned g_rng = 0x1234567u;
static int   g_flash;
static int   g_attract = 1;        /* auto-dodge until the player steers */
static int   g_last_gap;           /* previous wall's gap, to keep jumps fair */

static int rnd(int n) { g_rng = g_rng * 1103515245u + 12345u; return (int)((g_rng >> 16) % (unsigned)n); }

static float slot_x(int s) { return -HALF_W + (HALF_W * 2.0f) * ((float)s + 0.5f) / (float)SLOTS; }

static void spawn(Wall *w, float z)
{
    /* shift the gap at most +/-2 columns from the previous wall, so the next
       gap is always reachable in the time before the wall arrives (fair, and
       the attract-mode autopilot can always make it). */
    int g = g_last_gap + (rnd(5) - 2);
    if (g < 0) g = 0;
    if (g > SLOTS - GAP_SLOTS) g = SLOTS - GAP_SLOTS;
    w->z = z;
    w->gap = g;
    g_last_gap = g;
}

static void reset_run(void)
{
    int i;
    g_px = 0.0f;
    g_speed = 0.45f;
    g_score = 0;
    g_dead = 0;
    g_flash = 0;
    g_last_gap = (SLOTS - GAP_SLOTS) / 2;        /* start centred */
    for (i = 0; i < NWALLS; i++) spawn(&g_wall[i], SPAWN_Z * (float)(i + 1) / NWALLS);
}

void game_init(int w, int h)
{
    g_aspect = (h > 0) ? (float)w / (float)h : 4.0f/3.0f;
    g_attract = 1;
    reset_run();
}

int game_score(void) { return g_score; }
int game_over(void) { return g_dead; }
float game_player_x(void) { return g_px; }

/* the gap centre of the nearest wall still approaching the player plane */
float game_ai_target(void)
{
    int i, best = -1;
    float bz = -1e9f;
    for (i = 0; i < NWALLS; i++)
        if (g_wall[i].z < PLAYER_Z && g_wall[i].z > bz) { bz = g_wall[i].z; best = i; }
    if (best < 0) return g_px;
    return (slot_x(g_wall[best].gap) + slot_x(g_wall[best].gap + GAP_SLOTS - 1)) * 0.5f;
}

void game_update(const game_input *in)
{
    int i;
    if (g_flash > 0) g_flash--;

    if (in->left || in->right) g_attract = 0;       /* player takes over */

    if (g_dead) {
        if (in->start || in->fire) reset_run();     /* restart, keep aspect */
        return;
    }

    /* steer: player input, or the attract-mode autopilot toward the gap */
    if (g_attract) {
        float tx = game_ai_target();
        if      (g_px > tx + 0.2f) g_px -= 0.42f;
        else if (g_px < tx - 0.2f) g_px += 0.42f;
    } else {
        if (in->left)  g_px -= 0.42f;
        if (in->right) g_px += 0.42f;
    }
    if (g_px < -HALF_W + 0.5f) g_px = -HALF_W + 0.5f;
    if (g_px >  HALF_W - 0.5f) g_px =  HALF_W - 0.5f;

    /* advance walls toward the camera */
    for (i = 0; i < NWALLS; i++) {
        float prev = g_wall[i].z;
        g_wall[i].z += g_speed;

        /* crossing the player plane: score or crash */
        if (prev < PLAYER_Z && g_wall[i].z >= PLAYER_Z) {
            float gl = slot_x(g_wall[i].gap) - (slot_x(1) - slot_x(0)) * 0.5f;
            float gr = slot_x(g_wall[i].gap + GAP_SLOTS - 1) + (slot_x(1) - slot_x(0)) * 0.5f;
            if (g_px > gl && g_px < gr) {
                g_score++;
                g_speed += 0.012f;          /* ramp difficulty */
                if (g_speed > 0.80f) g_speed = 0.80f;   /* keep gaps reachable */
            } else {
                g_dead = 1;
                g_flash = 8;
            }
        }
        if (g_wall[i].z > 2.0f) spawn(&g_wall[i], SPAWN_Z);
    }
}

static void draw_wall(const Wall *w)
{
    int s;
    float colw = (HALF_W * 2.0f) / (float)SLOTS;
    /* fade far walls toward the background so depth reads clearly */
    float fog = 1.0f - (-w->z) / 55.0f; if (fog < 0.25f) fog = 0.25f;
    for (s = 0; s < SLOTS; s++) {
        if (s >= w->gap && s < w->gap + GAP_SLOTS) continue;   /* the gap */
        draw_box(slot_x(s), 0.0f, w->z, colw * 0.99f, 5.4f, 1.2f,
                 (int)((90 + (s * 23) % 150) * fog), (int)(60 * fog),
                 (int)((230 - (s * 17) % 120) * fog));
    }
}

void game_render(void)
{
    int i;
    if (g_flash > 0) u3d_begin(120, 0, 0);     /* red crash flash */
    else             u3d_begin(0, 0, 35);       /* deep-blue corridor */

    u3d_perspective(65.0f, g_aspect, 0.5f, 80.0f);

    /* floor + ceiling slabs give the corridor depth */
    draw_box(0.0f, -2.5f, -28.0f, HALF_W * 2.2f, 0.5f, 64.0f, 45, 50, 110);
    draw_box(0.0f,  2.5f, -28.0f, HALF_W * 2.2f, 0.5f, 64.0f, 22, 24, 64);

    for (i = 0; i < NWALLS; i++) draw_wall(&g_wall[i]);

    /* the player ship: a bright wedge near the camera */
    draw_box(g_px, -1.6f, PLAYER_Z + 0.5f, 1.7f, 0.7f, 2.0f,
             g_dead ? 220 : 0, g_dead ? 50 : 240, g_dead ? 50 : 220);

    u3d_end();
}
