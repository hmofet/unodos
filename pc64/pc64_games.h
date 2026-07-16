/* Native unoui games for the pc64 shell (pc64_games.c). Each game is a
 * unoui_canvas that draws directly with fb primitives and SCALES to whatever
 * rect it is given - so it fills a window and, unchanged, a full screen.
 * Replaces the mac_compat canvas bridge for the games. */
#ifndef PC64_GAMES_H
#define PC64_GAMES_H
#include "unoui.h"

enum { GAME_DOSTRIS, GAME_PACMAN, GAME_OUTLAST, PC64_NGAMES };

unoui_canvas *pc64_game_canvas(int game);   /* the game's draw/event vtable   */
void          pc64_game_open(int game);      /* (re)start it                  */
void          pc64_game_tick(int game);      /* advance one frame (~60 Hz)    */
const char   *pc64_game_name(int game);

#endif
