/* ===========================================================================
 * Studio - the AI assistant pane.
 *
 * Milestone 3 ships the pane shell (layout, focus, the "configure me" hint);
 * milestone 4 fills in the HTTPS/JSON client for OpenAI / Anthropic / Gemini.
 * The interface (studio_ai.h) is stable so studio.c doesn't change when the
 * client lands.
 * ======================================================================== */
#include "unoui.h"
#include "unoui_theme.h"
#include "fb.h"
#include "studio_ai.h"

void  fb_fill_rect(int x, int y, int w, int h, fb_px c);
void  fb_hline(int x, int y, int w, fb_px c);
void  fb_vline(int x, int y, int h, fb_px c);
int   fb_text(int x, int y, const char *s, fb_px fg, long bg);
int   fb_text_h(void);
const struct unoui_theme *pc64_shell_theme(void);

static int g_focused;

void studio_ai_init(void) { }

void studio_ai_draw(unoui_rect r, const void *theme)
{
    const struct unoui_theme *t = (const struct unoui_theme *)theme;
    const unoui_palette *p = &t->pal;
    int lh = fb_text_h() + 2, y;
    fb_vline(r.x, r.y, r.h, p->dark);
    fb_fill_rect(r.x + 1, r.y, r.w - 1, r.h, p->win_bg);
    fb_fill_rect(r.x + 1, r.y, r.w - 1, lh + 4, p->title_bg_in);
    fb_text(r.x + 8, r.y + 3, "Assistant", p->title_fg_in, -1);
    y = r.y + lh + 12;
    fb_text(r.x + 8, y, "AI help arrives in the", p->text_dim, -1); y += lh;
    fb_text(r.x + 8, y, "next Studio update.", p->text_dim, -1); y += lh * 2;
    fb_text(r.x + 8, y, "It will connect to", p->text, -1); y += lh;
    fb_text(r.x + 8, y, "ChatGPT, Claude and", p->text, -1); y += lh;
    fb_text(r.x + 8, y, "Gemini with your key.", p->text, -1);
}

int  studio_ai_click(int x, int y, unoui_rect r) { (void)x; (void)y; (void)r; g_focused = 1; return 1; }
int  studio_ai_char(int ch) { (void)ch; return 0; }
int  studio_ai_key(int vk) { (void)vk; return 0; }
int  studio_ai_accel(int uni, int ctrl) { (void)uni; (void)ctrl; return 0; }
void studio_ai_frame(void) { }
int  studio_ai_focused(void) { return g_focused; }
void studio_ai_blur(void) { g_focused = 0; }
void studio_ai_attach_file(const char *n, const char *t, int l) { (void)n; (void)t; (void)l; }
void studio_ai_attach_errors(const char *t) { (void)t; }
void studio_ai_settings(void) { }
