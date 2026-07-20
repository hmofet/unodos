/* Studio - the AI assistant pane (studio_ai.c).  Fleshed out in milestone 4;
 * studio.c calls these to draw + drive the right-hand column, and feeds it
 * the current file / build errors as context. */
#ifndef STUDIO_AI_H
#define STUDIO_AI_H
#include "unoui.h"

void studio_ai_init(void);                       /* malloc buffers, read cfg  */
void studio_ai_draw(unoui_rect r, const void *theme);
int  studio_ai_click(int x, int y, unoui_rect r);/* 1 = took focus/consumed   */
int  studio_ai_char(int ch);                     /* 1 = consumed              */
int  studio_ai_key(int vk);                      /* UI_KEY_*; 1 = consumed    */
int  studio_ai_accel(int uni, int ctrl);         /* Ctrl-shortcuts; 1 = used  */
void studio_ai_frame(void);                      /* pump an in-flight request */
int  studio_ai_focused(void);                    /* input has keyboard focus  */
void studio_ai_blur(void);
void studio_ai_attach_file(const char *name, const char *text, int len);
void studio_ai_attach_errors(const char *text);
void studio_ai_settings(void);                   /* open the settings dialog  */
/* studio.c provides this so the AI can paste a code block at the caret */
void studio_insert_text(const char *s, int n);

#endif
