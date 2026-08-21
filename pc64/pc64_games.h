/* Native unoui games for the pc64 shell (pc64_games.c). Each game is a
 * unoui_canvas that draws directly with fb primitives and SCALES to whatever
 * rect it is given - so it fills a window and, unchanged, a full screen.
 * Replaces the mac_compat canvas bridge for the games. */
#ifndef PC64_GAMES_H
#define PC64_GAMES_H
#include "unoui.h"

enum { GAME_DOSTRIS, GAME_PACMAN, GAME_OUTLAST, GAME_RUNNER, PC64_NGAMES };

unoui_canvas *pc64_game_canvas(int game);   /* the game's draw/event vtable   */
void          pc64_game_open(int game);      /* (re)start it                  */
void          pc64_game_close(int game);     /* teardown (uno3d shutdown, ...) */
/* Entering / leaving fullscreen, which for a 3D game is a RESOLUTION change:
 * Runner3D renders at a quarter of the panel in each axis while it owns the
 * screen and the platform upscales.  The shell must tell the game both ways,
 * because that low mode is only valid while the game IS the screen - leaving
 * fullscreen by any route (Esc, minimize, a desktop switch, Alt+D) has to put
 * the desktop's resolution back.  A no-op for games that do not switch modes. */
void          pc64_game_fullscreen(int game, int on);
void          pc64_game_tick(int game);      /* advance one frame (~60 Hz)    */
const char   *pc64_game_name(int game);

#endif
