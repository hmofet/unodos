/* uno3d_game - a simple cross-platform 3D game (an obstacle dodger).
 *
 * Like the demo, the game is written ONCE against the uno3d API and an abstract
 * input struct; every platform's glue fills in the input (pad / maple / keyboard)
 * and calls update + render + present. No platform or backend names appear here. */
#ifndef UNO3D_GAME_H
#define UNO3D_GAME_H

/* per-frame input: 1 = button held this frame */
typedef struct {
    int left, right, up, down, fire, start;
} game_input;

void game_init(int w, int h);          /* set viewport aspect, reset state */
void game_update(const game_input *in);/* advance one frame */
void game_render(void);                /* draw the frame (u3d_begin..u3d_end) */
int  game_score(void);
int  game_over(void);
/* helpers for attract-mode / autopilot / testing */
float game_ai_target(void);            /* x the ship should aim for (gap centre) */
float game_player_x(void);

#endif
