#!/usr/bin/env python3
"""Generate the UnoDOS/pc64 user-manual GitHub Pages site into ./docs.

Static, self-contained (no Jekyll -> .nojekyll), responsive, light/dark. One
shared shell (sidebar + header + footer) wraps every page; content lives in the
PAGES table below. Screenshots are copied into docs/assets/img by the caller.
"""
import os, shutil, html, re

_HERE = os.path.dirname(os.path.abspath(__file__))
# When this script lives inside the docs/ folder (its committed home) it writes
# there directly; from a scratch dir it writes to a docs/ subfolder.
OUT = _HERE if os.path.basename(_HERE) == "docs" else os.path.join(_HERE, "docs")

# --------------------------------------------------------------------------- nav
NAV = [
    ("index.html",           "Overview"),
    ("getting-started.html", "Getting started"),
    ("desktop.html",         "The desktop"),
    ("appearance.html",      "Themes &amp; appearance"),
    ("apps.html",            "Applications"),
    ("browser.html",         "Web browser"),
    ("networking.html",      "Networking"),
    ("ports.html",           "The UnoDOS family"),
    (None,                   "Developer"),          # section header
    ("developer.html",       "Overview &amp; architecture"),
    ("dev-apps.html",        "Writing apps"),
    ("dev-api.html",         "API reference"),
    ("dev-build.html",       "Building &amp; tooling"),
]
PAGES_NAV = [(h, l) for h, l in NAV if h]           # real pages, for prev/next

# --------------------------------------------------------------------------- css
CSS = r"""
:root{
  --bg:#f7f8fb; --surface:#ffffff; --surface-2:#eef1f7; --border:#dde2ec;
  --text:#1d2333; --muted:#5b6478; --accent:#4c6ef5; --accent-2:#3b5bdb;
  --accent-soft:#e8edff; --code-bg:#eef1f7; --shadow:0 1px 2px rgba(20,30,60,.06),0 8px 24px rgba(20,30,60,.07);
  --maxw:900px; --sidebar:264px;
}
@media (prefers-color-scheme:dark){
  :root{
    --bg:#0e1017; --surface:#161a24; --surface-2:#1c212e; --border:#2a3040;
    --text:#e6e9f2; --muted:#9aa4bd; --accent:#7a8cff; --accent-2:#9db0ff;
    --accent-soft:#1b2135; --code-bg:#1c212e;
    --shadow:0 1px 2px rgba(0,0,0,.4),0 10px 30px rgba(0,0,0,.35);
  }
}
:root[data-theme="dark"]{
  --bg:#0e1017; --surface:#161a24; --surface-2:#1c212e; --border:#2a3040;
  --text:#e6e9f2; --muted:#9aa4bd; --accent:#7a8cff; --accent-2:#9db0ff;
  --accent-soft:#1b2135; --code-bg:#1c212e;
  --shadow:0 1px 2px rgba(0,0,0,.4),0 10px 30px rgba(0,0,0,.35);
}
:root[data-theme="light"]{
  --bg:#f7f8fb; --surface:#ffffff; --surface-2:#eef1f7; --border:#dde2ec;
  --text:#1d2333; --muted:#5b6478; --accent:#4c6ef5; --accent-2:#3b5bdb;
  --accent-soft:#e8edff; --code-bg:#eef1f7;
  --shadow:0 1px 2px rgba(20,30,60,.06),0 8px 24px rgba(20,30,60,.07);
}
*{box-sizing:border-box}
html{scroll-behavior:smooth}
body{margin:0;background:var(--bg);color:var(--text);
  font:16px/1.65 -apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,Helvetica,Arial,sans-serif;
  -webkit-font-smoothing:antialiased;}
a{color:var(--accent-2);text-decoration:none}
a:hover{text-decoration:underline}
code,kbd,pre,.mono{font-family:"SFMono-Regular",ui-monospace,"JetBrains Mono",Menlo,Consolas,monospace}

/* layout */
.layout{display:flex;min-height:100vh}
.sidebar{width:var(--sidebar);flex:0 0 var(--sidebar);border-right:1px solid var(--border);
  background:var(--surface);position:sticky;top:0;height:100vh;overflow-y:auto;padding:22px 0}
.brand{display:flex;align-items:center;gap:11px;padding:0 22px 18px;margin-bottom:8px;border-bottom:1px solid var(--border)}
.brand .logo{display:grid;grid-template-columns:11px 11px;grid-gap:3px;flex:0 0 auto}
.brand .logo i{width:11px;height:11px;border-radius:2px;display:block}
.brand .logo i:nth-child(1){background:#e8503a}.brand .logo i:nth-child(2){background:#37b24d}
.brand .logo i:nth-child(3){background:#4c6ef5}.brand .logo i:nth-child(4){background:#f4b400}
.brand b{font-size:18px;letter-spacing:.2px}
.brand .sub{display:block;font-size:12px;color:var(--muted);font-weight:400}
.nav{list-style:none;margin:14px 0 0;padding:0}
.nav a{display:block;padding:8px 22px;color:var(--text);font-size:14.5px;border-left:3px solid transparent}
.nav a:hover{background:var(--surface-2);text-decoration:none}
.nav a.active{border-left-color:var(--accent);color:var(--accent-2);background:var(--accent-soft);font-weight:600}
.nav .sec{padding:15px 22px 5px;font-size:11px;text-transform:uppercase;letter-spacing:.6px;color:var(--muted);font-weight:700;margin-top:6px;border-top:1px solid var(--border)}
.side-foot{padding:16px 22px 4px;margin-top:14px;border-top:1px solid var(--border);font-size:12.5px;color:var(--muted)}
.side-foot a{color:var(--muted)}

/* content */
.main{flex:1 1 auto;min-width:0}
.wrap{max-width:var(--maxw);margin:0 auto;padding:38px 30px 90px}
h1{font-size:32px;line-height:1.2;margin:.1em 0 .35em;letter-spacing:-.3px}
h2{font-size:23px;margin:2.1em 0 .5em;padding-top:.4em;letter-spacing:-.2px}
h3{font-size:18px;margin:1.6em 0 .3em}
h2:target,h3:target{scroll-margin-top:20px}
p{margin:.7em 0}
.lede{font-size:18.5px;color:var(--muted);margin:.2em 0 1.2em}
ul,ol{padding-left:1.3em}li{margin:.3em 0}
hr{border:0;border-top:1px solid var(--border);margin:2.4em 0}
.muted{color:var(--muted)}
strong{font-weight:650}

/* topbar (mobile) */
.topbar{display:none;align-items:center;gap:12px;position:sticky;top:0;z-index:20;
  background:var(--surface);border-bottom:1px solid var(--border);padding:10px 16px}
.topbar b{font-size:16px}
.menu-btn{appearance:none;border:1px solid var(--border);background:var(--surface-2);color:var(--text);
  border-radius:8px;padding:6px 10px;font-size:15px;cursor:pointer}

/* figure */
figure{margin:1.5em 0;background:var(--surface);border:1px solid var(--border);border-radius:12px;
  overflow:hidden;box-shadow:var(--shadow)}
figure img{display:block;width:100%;height:auto;background:#0b0d13}
figcaption{padding:10px 15px;font-size:13.5px;color:var(--muted);border-top:1px solid var(--border);background:var(--surface)}

/* click-to-enlarge: the figure's image is a button, so it is keyboard
   reachable and announces itself; the cursor and a soft hover lift are the
   only affordance needed. */
button.zoom{display:block;width:100%;padding:0;border:0;background:none;cursor:zoom-in}
button.zoom:focus-visible{outline:3px solid var(--accent);outline-offset:-3px}
button.zoom img{transition:filter .12s ease}
button.zoom:hover img{filter:brightness(1.06)}
#lb{position:fixed;inset:0;z-index:99;display:none;place-items:center;
  background:rgba(8,10,16,.86);padding:24px;cursor:zoom-out}
#lb.on{display:grid}
#lb img{max-width:100%;max-height:calc(100vh - 96px);width:auto;height:auto;
  border-radius:10px;box-shadow:0 18px 60px rgba(0,0,0,.6);background:#0b0d13}
#lb figcaption{position:fixed;left:0;right:0;bottom:0;text-align:center;
  background:rgba(8,10,16,.92);color:#cfd6e6;border-top:0;padding:12px 20px}
#lb .x{position:fixed;top:14px;right:18px;font-size:26px;line-height:1;
  color:#cfd6e6;background:none;border:0;cursor:pointer;padding:6px 10px}
@media print{button.zoom{cursor:default}#lb{display:none!important}}
.shot-sm{max-width:520px}
.grid{display:grid;gap:18px}
.grid.cols-2{grid-template-columns:repeat(2,minmax(0,1fr))}
.grid.cols-3{grid-template-columns:repeat(3,minmax(0,1fr))}
.grid figure{margin:0;align-self:start}

/* cards */
.cards{display:grid;grid-template-columns:repeat(auto-fill,minmax(220px,1fr));gap:14px;margin:1.2em 0}
.card{background:var(--surface);border:1px solid var(--border);border-radius:12px;padding:15px 16px;box-shadow:var(--shadow)}
.card h4{margin:.1em 0 .3em;font-size:15.5px}
.card p{margin:.2em 0;font-size:13.6px;color:var(--muted)}

/* callouts */
.note{border:1px solid var(--border);border-left:4px solid var(--accent);background:var(--surface);
  border-radius:10px;padding:12px 16px;margin:1.4em 0;font-size:14.6px}
.note.tip{border-left-color:#37b24d}.note.warn{border-left-color:#f08c00}
.note>b:first-child{display:block;margin-bottom:2px}

/* kbd + code */
kbd{display:inline-block;background:var(--surface-2);border:1px solid var(--border);border-bottom-width:2px;
  border-radius:6px;padding:1px 7px;font-size:12.8px;line-height:1.5;color:var(--text);white-space:nowrap}
code{background:var(--code-bg);border-radius:5px;padding:.1em .4em;font-size:13.4px}
pre{background:var(--code-bg);border:1px solid var(--border);border-radius:10px;padding:14px 16px;overflow-x:auto;font-size:13.4px;line-height:1.55}
pre code{background:none;padding:0}

/* tables */
.tw{overflow-x:auto;margin:1.3em 0}
table{border-collapse:collapse;width:100%;font-size:14px}
th,td{text-align:left;padding:9px 12px;border-bottom:1px solid var(--border);vertical-align:top}
th{font-size:12.5px;text-transform:uppercase;letter-spacing:.4px;color:var(--muted);font-weight:650}
tbody tr:hover{background:var(--surface-2)}
td code{white-space:nowrap}

/* hero */
.hero{background:linear-gradient(150deg,var(--accent-soft),transparent 70%);border:1px solid var(--border);
  border-radius:16px;padding:30px 30px 26px;margin:0 0 20px}
.hero h1{margin-top:0}
.hero .lede{margin-bottom:16px}
.pill{display:inline-block;background:var(--surface);border:1px solid var(--border);border-radius:999px;
  padding:3px 12px;font-size:12.5px;color:var(--muted);margin:0 6px 6px 0}
.btnrow{margin-top:14px;display:flex;flex-wrap:wrap;gap:10px}
.btn{display:inline-block;background:var(--accent);color:#fff;border-radius:9px;padding:9px 16px;font-size:14.5px;font-weight:600}
.btn:hover{background:var(--accent-2);text-decoration:none}
.btn.ghost{background:transparent;color:var(--accent-2);border:1px solid var(--border)}
.pagenav{display:flex;justify-content:space-between;gap:14px;margin-top:44px;border-top:1px solid var(--border);padding-top:18px;font-size:14.5px}
.pagenav .nxt{margin-left:auto;text-align:right}
.kv{font-size:13.6px;color:var(--muted)}

@media (max-width:860px){
  .sidebar{position:fixed;left:0;top:0;z-index:30;transform:translateX(-100%);transition:transform .2s ease;box-shadow:var(--shadow)}
  #mnav:checked ~ .layout .sidebar{transform:none}
  .topbar{display:flex}
  .grid.cols-2,.grid.cols-3{grid-template-columns:1fr}
  .wrap{padding:24px 18px 70px}
  h1{font-size:27px}
}
#mnav{display:none}
"""

THEME_TOGGLE = r"""
<script>
(function(){
  var r=document.documentElement, k='unodos-doc-theme';
  try{var s=localStorage.getItem(k); if(s)r.setAttribute('data-theme',s);}catch(e){}
  window.__t=function(){
    var cur=r.getAttribute('data-theme');
    if(!cur){cur=matchMedia('(prefers-color-scheme:dark)').matches?'dark':'light';}
    var nx=cur==='dark'?'light':'dark';
    r.setAttribute('data-theme',nx);
    try{localStorage.setItem(k,nx);}catch(e){}
  };
})();
</script>
<script>
/* Lightbox: click (or Enter/Space on) a screenshot to see it full size.
   Manual shots are scaled into the text column, where fine UI detail - a
   status line, an icon, a menu entry - is too small to read. Built once at
   load and reused; Esc or a click anywhere closes it. */
(function(){
  var lb,img,cap;
  function build(){
    lb=document.createElement('div'); lb.id='lb'; lb.setAttribute('role','dialog');
    lb.setAttribute('aria-modal','true');
    img=document.createElement('img');
    cap=document.createElement('figcaption');
    var x=document.createElement('button');
    x.className='x'; x.innerHTML='&times;'; x.setAttribute('aria-label','Close');
    lb.appendChild(x); lb.appendChild(img); lb.appendChild(cap);
    document.body.appendChild(lb);
    lb.addEventListener('click', close);
  }
  function open(src, text){
    if(!lb) build();
    img.src=src; img.alt=text||''; cap.textContent=text||'';
    lb.classList.add('on'); document.body.style.overflow='hidden';
  }
  function close(){
    if(!lb) return;
    lb.classList.remove('on'); img.src=''; document.body.style.overflow='';
  }
  document.addEventListener('click', function(e){
    var b=e.target.closest && e.target.closest('button.zoom');
    if(!b) return;
    e.preventDefault();
    var f=b.parentNode.querySelector('figcaption');
    open(b.getAttribute('data-full'), f?f.textContent:'');
  });
  document.addEventListener('keydown', function(e){
    if(e.key==='Escape') close();
  });
})();
</script>
"""

def sidebar(active):
    rows = []
    for href, label in NAV:
        if href is None:
            rows.append('<li class="sec">%s</li>' % label)
        else:
            cls = "active" if href == active else ""
            rows.append('<li><a class="%s" href="%s">%s</a></li>' % (cls, href, label))
    items = "".join(rows)
    return f"""
<aside class="sidebar">
  <a class="brand" href="index.html" style="text-decoration:none;color:inherit">
    <span class="logo"><i></i><i></i><i></i><i></i></span>
    <span><b>UnoDOS <span style="color:var(--accent-2)">pc64</span></b><span class="sub">User manual</span></span>
  </a>
  <ul class="nav">{items}</ul>
  <div class="side-foot">
    <a href="https://github.com/hmofet/unodos" target="_blank" rel="noopener">GitHub&nbsp;&rsaquo; hmofet/unodos</a><br>
    <a href="#" onclick="__t();return false">Toggle light / dark</a>
  </div>
</aside>"""

def page(fname, title, body):
    active = fname
    # prev/next
    idx = [h for h, _ in PAGES_NAV].index(fname)
    pn = ""
    prev = PAGES_NAV[idx-1] if idx > 0 else None
    nxt = PAGES_NAV[idx+1] if idx < len(PAGES_NAV)-1 else None
    parts = ['<nav class="pagenav">']
    parts.append(f'<a href="{prev[0]}">&lsaquo; {prev[1]}</a>' if prev else '<span></span>')
    parts.append(f'<a class="nxt" href="{nxt[0]}">{nxt[1]} &rsaquo;</a>' if nxt else '<span></span>')
    parts.append('</nav>')
    pn = "".join(parts)
    return f"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{title} · UnoDOS pc64 manual</title>
<meta name="description" content="User manual for UnoDOS pc64, the x86-64/UEFI world of the UnoDOS operating-system family.">
<link rel="stylesheet" href="assets/style.css">
{THEME_TOGGLE}
</head>
<body>
<input type="checkbox" id="mnav">
<div class="topbar">
  <label class="menu-btn" for="mnav">☰ Menu</label>
  <b>UnoDOS pc64</b>
</div>
<div class="layout">
{sidebar(active)}
<main class="main"><div class="wrap">
{body}
{pn}
</div></main>
</div>
</body>
</html>"""

# --------------------------------------------------------------------------- helpers
def fig(src, cap, cls=""):
    """A screenshot figure. Clicking the image opens it full size in the
    lightbox (see LIGHTBOX_JS): manual screenshots are scaled down to fit the
    text column, which makes small UI detail - a status line, an icon, a menu
    item - unreadable at the size it is printed."""
    c = f' class="{cls}"' if cls else ""
    alt = html.escape(re.sub(r"<[^>]+>", "", cap))          # plain text for alt
    return (f'<figure{c}><button type="button" class="zoom" '
            f'data-full="assets/img/{src}" aria-label="Enlarge: {alt}">'
            f'<img src="assets/img/{src}" alt="{alt}" loading="lazy">'
            f'</button><figcaption>{cap}</figcaption></figure>')

def note(body, kind="", title="Note"):
    k = f" {kind}" if kind else ""
    return f'<div class="note{k}"><b>{title}</b>{body}</div>'

def code(s):
    # plain concat (not an f-string) so C braces in `s` pass through untouched
    return "<pre><code>" + html.escape(s.strip("\n")) + "</code></pre>"

# ---- code snippets (defined here so their { } don't clash with page f-strings) ----
CODE_HELLO_NATIVE = code('''#include "unoui.h"

enum { ID_HELLO_BTN = 100 };

/* An app is just a builder that populates a window.
   The shell owns the event loop and calls this once. */
void build_hello(unoui_window *w)
{
    /* title, x, y, width, height (screen coords, includes the title bar) */
    unoui_window_init(w, "Hello", 160, 60, 200, 110);

    /* a static label at content-relative (12, 10) */
    unoui_add_label(w, 12, 10, "Hello, UnoDOS pc64!");

    /* a default push button; its id is echoed back on click */
    unoui_widget *b = unoui_add_button(w, 12, 40, 80, "OK", UI_F_DEFAULT);
    b->id = ID_HELLO_BTN;
}''')

CODE_SHELL_LOOP = code('''unoui_ui_init(&ui, &theme_unodos, FB_W, FB_H);
unoui_ui_add(&ui, &window);
for (;;) {
    unoui_event  ev = port_next_event();       /* platform input adapter   */
    unoui_action a  = unoui_handle(&ui, &ev);  /* portable widget behavior */
    if (a.changed && a.id == ID_HELLO_BTN) { /* the OK button was pressed */ }
    if (a.changed && a.kind == UI_ACT_CLOSE) { /* close box; value = z-index */ }
    unoui_render_ui(&ui);
    port_present(fb);                          /* platform present adapter */
}''')

CODE_HELLO_CANVAS = code('''#include "unoui.h"

/* the app owns every pixel inside its canvas rectangle */
static void hello_draw(struct unoui_widget *w, unoui_rect r, void *ctx) {
    fb_fill_rect(r.x, r.y, r.w, r.h, FB_RGB(20, 30, 60));
    fb_text(r.x + 8, r.y + 8, "these pixels are mine", UNO_WHITE, -1);
}
/* return 1 if the event was consumed */
static int hello_event(struct unoui_widget *w, const void *ev, void *ctx) {
    const unoui_event *e = ev;
    if (e->kind == UI_EV_KEY && e->key == UI_KEY_ESC) return 1;
    return 0;
}
static unoui_canvas g_hello = { hello_draw, hello_event, 0 };

void build_hello_canvas(unoui_window *w) {
    unoui_window_init(w, "Canvas", 120, 40, 280, 180);
    unoui_add_canvas(w, 6, 20, 256, 120, &g_hello);
}''')

CODE_OPEN_APP = code('''static void open_app(int a) {
    if (!g_built[a]) {
        if (a < NNATIVE) g_build[a](&g_win[a]);   /* native builder     */
        else             build_legacy(a);         /* bridged legacy app */
        g_built[a] = 1;
    }
    if (!g_open[a]) {
        unoui_ui_add(&UI, &g_win[a]);             /* add window to the UI */
        g_open[a] = 1;
    }
}''')

CODE_APPIFACE = code('''/* pc64/uno_app.h -- the legacy shared ABI (for bridged apps) */
typedef struct AppInterface {
    void    (*draw)(UnoWin *w);
    Boolean (*key)(char ch, short code, Boolean cmd);
    void    (*click)(UnoWin *w, Point p);
    void    (*tick)(void);
    void    (*opened)(void);
    void    (*closed)(void);
    const char *win_title;
    short   win_rect[4];
} AppInterface;

/* every legacy app module exports exactly one entry: */
const AppInterface *uno_app_main(const KernelApi *k);   /* UNO_APP_ENTRY_NAME */''')

CODE_UNOUI_WIN = code('''void unoui_window_init(unoui_window *win, const char *title,
                       int x, int y, int w, int h);

void         unoui_ui_init (unoui_ui *, const unoui_theme *, int sw, int sh);
void         unoui_ui_theme(unoui_ui *, const unoui_theme *);   /* live re-skin */
void         unoui_ui_add  (unoui_ui *, unoui_window *);        /* topmost = focus */
void         unoui_bring_to_front(unoui_ui *, unoui_window *win);
unoui_action unoui_handle  (unoui_ui *, const unoui_event *);   /* feed one event */
void         unoui_render_ui(unoui_ui *);
void         unoui_fullscreen(unoui_ui *, unoui_window *win);   /* NULL = restore */
void         unoui_reflow_window(const unoui_theme *, unoui_window *);

/* window flags: UI_WIN_BARE UI_WIN_BOTTOM UI_WIN_TOP UI_WIN_RESIZE
   limits: UNOUI_MAX_WIDGETS = 64, UNOUI_MAX_WINDOWS = 8 */''')

CODE_UNOUI_WIDGETS = code('''/* all return unoui_widget *; set ->id afterwards to identify it in actions */
unoui_widget *unoui_add_label   (unoui_window *, int x, int y, const char *text);
unoui_widget *unoui_add_button  (unoui_window *, int x, int y, int w, const char *text, int flags);
unoui_widget *unoui_add_check   (unoui_window *, int x, int y, const char *text, int on);
unoui_widget *unoui_add_radio   (unoui_window *, int x, int y, const char *text, int on);
unoui_widget *unoui_add_field   (unoui_window *, int x, int y, int w, const char *text, int focus);
unoui_widget *unoui_add_edit    (unoui_window *, int x, int y, int w, unoui_text *t);
unoui_widget *unoui_add_textarea(unoui_window *, int x, int y, int w, int h, unoui_text *t);
unoui_widget *unoui_add_progress(unoui_window *, int x, int y, int w, int v, int vmax);
unoui_widget *unoui_add_vscroll (unoui_window *, int x, int y, int h, int v, int vmax);
unoui_widget *unoui_add_hscroll (unoui_window *, int x, int y, int w, int v, int vmax);
unoui_widget *unoui_add_slider  (unoui_window *, int x, int y, int w, int vmin, int vmax, int v);
unoui_widget *unoui_add_spinner (unoui_window *, int x, int y, int w, int vmin, int vmax, int v);
unoui_widget *unoui_add_dropdown(unoui_window *, int x, int y, int w, const char **items, int n, int sel);
unoui_widget *unoui_add_tabs    (unoui_window *, int x, int y, int w, const char **items, int n, int sel);
unoui_widget *unoui_add_menubar (unoui_window *, const unoui_menu *menus, int n);
unoui_widget *unoui_add_list    (unoui_window *, int x, int y, int w, int h, const char **items, int n, int sel);
unoui_widget *unoui_add_group   (unoui_window *, int x, int y, int w, int h, const char *title);
unoui_widget *unoui_add_sep     (unoui_window *, int x, int y, int w);
unoui_widget *unoui_add_canvas  (unoui_window *, int x, int y, int w, int h, unoui_canvas *c);

/* widget state flags: UI_F_DEFAULT UI_F_PRESSED UI_F_FOCUS UI_F_DISABLED
                       UI_F_CHECKED UI_F_CARET UI_F_HOT */''')

CODE_UNOUI_TEXT = code('''/* editable text: the APP owns the char buffer; the toolkit edits it in place */
typedef struct {
    char *buf; int cap; int len; int caret; int sel;
    int scroll_x; int scroll_y; int multiline;
} unoui_text;

void unoui_text_init(unoui_text *t, char *buf, int cap, int multiline);
void unoui_text_set (unoui_text *t, const char *s);''')

CODE_UNOUI_EVENT = code('''typedef enum {
    UI_EV_NONE = 0,
    UI_EV_MOUSE_DOWN, UI_EV_MOUSE_UP, UI_EV_MOUSE_MOVE,
    UI_EV_KEY,    /* virtual key (UI_KEY_*)  */
    UI_EV_CHAR,   /* printable char (.ch)    */
    UI_EV_WHEEL,  /* scroll (.wheel notches) */
    UI_EV_TICK    /* frame tick, drives caret blink */
} ui_event_kind;

enum { UI_MOD_SHIFT = 1, UI_MOD_CTRL = 2, UI_MOD_ALT = 4 };

enum {  /* virtual keys, kept above ASCII so CHAR vs KEY split cleanly */
    UI_KEY_LEFT = 0x100, UI_KEY_RIGHT, UI_KEY_UP, UI_KEY_DOWN,
    UI_KEY_HOME, UI_KEY_END, UI_KEY_PGUP, UI_KEY_PGDN,
    UI_KEY_BACKSPACE, UI_KEY_DELETE, UI_KEY_ENTER, UI_KEY_TAB, UI_KEY_ESC
};

typedef struct {                 /* one input event */
    ui_event_kind kind;
    int x, y, button;            /* mouse position (screen) + button */
    int key;                     /* UI_KEY_* for UI_EV_KEY   */
    int ch;                      /* ASCII for UI_EV_CHAR     */
    int mods;                    /* UI_MOD_* bitmask         */
    int wheel;                   /* +down / -up              */
} unoui_event;

typedef struct {                 /* result of feeding one event */
    int changed;                 /* nonzero => id/kind/value are meaningful */
    int id;                      /* the widget's app id      */
    int kind;                    /* the widget's kind, or UI_ACT_CLOSE */
    int value;                   /* new value / selection / z-index    */
} unoui_action;

#define UI_ACT_CLOSE 9999        /* close box clicked; action.value = z-index */''')

CODE_FB = code('''typedef uint32_t fb_px;

void fb_clear(fb_px c);
void fb_fill_rect(int x,int y,int w,int h,fb_px c);
void fb_frame_rect(int x,int y,int w,int h,fb_px c);
void fb_pixel(int x,int y,fb_px c);
void fb_hline(int x,int y,int w,fb_px c);
void fb_vline(int x,int y,int h,fb_px c);
void fb_invert_rect(int x,int y,int w,int h);
void fb_set_clip(int x,int y,int w,int h);   /* confine drawing */
void fb_reset_clip(void);
void fb_blend_pixel(int x,int y,fb_px c,int a);          /* alpha 0..255 */
void fb_blend_rect(int x,int y,int w,int h,fb_px c,int a);
void fb_grad_v(int x,int y,int w,int h,fb_px top,fb_px bot);
void fb_round_rect(int x,int y,int w,int h,int rad,fb_px c);
void fb_set_font(const fb_font *f);
int  fb_text(int x,int y,const char *s,fb_px fg,long bg);   /* bg = -1 => transparent */
int  fb_text_w(const char *s);
int  fb_text_h(void);                        /* line height of the active font */
void fb_get_clip(int *x,int *y,int *w,int *h);   /* save/restore the clip */
int  fb_big_text(int x,int y,const char *s,fb_px fg,long bg,int scale);

/* FB_RGB(r,g,b); named colors UNO_WHITE UNO_BLACK UNO_BLUE UNO_CYAN UNO_MAG
   corner masks FB_CORNER_TL/TR/BL/BR/ALL; externs fb[], uno_fb_w, uno_fb_h */''')

CODE_UNO3D = code('''typedef struct { float x, y, z; } u3d_vec3;
typedef struct { float x, y, z; unsigned char r, g, b; } u3d_vert;

void u3d_init(int w, int h);
void u3d_shutdown(void);
void u3d_begin(unsigned char r, unsigned char g, unsigned char b);  /* clear */
void u3d_perspective(float fov_deg, float aspect, float znear, float zfar);
void u3d_load_identity(void);
void u3d_translate(float x, float y, float z);
void u3d_scale(float x, float y, float z);
void u3d_rotate_x(float deg);
void u3d_rotate_y(float deg);
void u3d_rotate_z(float deg);
void u3d_triangles(const u3d_vert *verts, int tri_count);   /* Gouraud, no textures */
void u3d_end(void);
void u3d_present(void);''')

CODE_BUILD = code('''./build.sh                 # build the unoui desktop shell -> build/esp/
./build.sh run             # build, then boot it in QEMU + OVMF
./build.sh legacy          # build the older 14-app "legacy" core
python3 tools/mkuefi.py 512   # pack build/esp/ into build/unodos-uefi.img (512 MiB)
python3 harness.py boot    # scripted QEMU boot + screenshot
python3 nettest.py         # headless network + TLS verification''')

# --------------------------------------------------------------------------- pages
PAGES = {}

PAGES["index.html"] = ("Overview", f"""
<div class="hero">
  <h1>UnoDOS <span style="color:var(--accent-2)">pc64</span></h1>
  <p class="lede">A GUI-first operating system that boots on any 64-bit UEFI PC, straight into a themed
  desktop with its own window manager, web browser, networking and built-in apps. No install and no
  command line required. This manual covers the <strong>pc64</strong> version.</p>
  <div>
    <span class="pill">x86-64 · UEFI</span>
    <span class="pill">~660&nbsp;KB image</span>
    <span class="pill">10 themes</span>
    <span class="pill">Keyboard-first</span>
    <span class="pill">QEMU &amp; real-metal verified</span>
  </div>
  <div class="btnrow">
    <a class="btn" href="getting-started.html">Get started &rsaquo;</a>
    <a class="btn ghost" href="desktop.html">Tour the desktop</a>
  </div>
</div>

<p>UnoDOS is a family of GUI-first operating systems that runs on more than 20 kinds of hardware.
<strong>pc64</strong> is the modern-PC version: it runs on essentially any x86-64 PC built since about
2007, and it has been tested in emulators and on real hardware booting from a USB stick.</p>

{note('Download <strong>unodos-pc64.iso</strong> and write it to a spare USB stick with Rufus or balenaEtcher (or boot it in a VM) - or use the one-click <strong>USB flasher</strong>. No building required. See <a href="getting-started.html">Getting started</a>.', kind="tip", title="Just want to try it?")}

{fig("desktop.png", "The pc64 desktop in the default <b>Aurora Light</b> theme. Right-click anywhere on the desktop for the programs menu - there is no Start button, so the taskbar is just your open windows and the clock.")}

<h2 id="what">What you get</h2>
<div class="cards">
  <div class="card"><h4>A real desktop</h4><p>Window manager, taskbar, desktop icons you can arrange, and windows you can move and resize, all by keyboard or pointer. Right-click the desktop for the programs menu.</p></div>
  <div class="card"><h4>10 live themes</h4><p>A modern <em>Aurora</em> look (light and dark) plus eight retro skins, switchable live from the Control Panel &mdash; with proportional TrueType text and a real UI-scale setting.</p></div>
  <div class="card"><h4>Applications</h4><p>A WordPad-class rich-text <strong>Editor</strong>, a real <strong>Files</strong> manager with a two-pane mode, System, Clock, a Canvas demo, plus Paint, Music, a Tracker, three games and a 3D runner.</p></div>
  <div class="card"><h4>A web browser</h4><p>Shows HTML, Markdown and CSS, runs JavaScript, and loads pages over HTTP and HTTPS.</p></div>
  <div class="card"><h4>Networking</h4><p>Connect over Ethernet, get an address automatically, and browse the web, including secure (HTTPS) sites.</p></div>
  <div class="card"><h4>Hardware support</h4><p>Your screen, keyboard, mouse and trackpad, and USB. Sound plays through the machine's audio hardware (HD&nbsp;Audio or AC'97), and UnoDOS drives SATA, NVMe and eMMC/SD storage with its own drivers.</p></div>
</div>

<p class="kv">New here? Start with <a href="getting-started.html">Getting started</a>, then take the
<a href="desktop.html">desktop tour</a>.</p>
""")

PAGES["getting-started.html"] = ("Getting started", f"""
<h1>Getting started</h1>
<p class="lede">Download the <strong>ISO</strong>, write it to a spare USB stick with a tool you already
know (Rufus, balenaEtcher) or boot it straight in a virtual machine &mdash; or use the one-click
UnoDOS flasher. No building, no command line.</p>

<h2 id="need">What you need</h2>
<ul>
  <li>A spare <strong>USB stick</strong> of any size (the image is tiny). <strong>Everything on it will be erased.</strong></li>
  <li>A 64-bit <strong>UEFI</strong> PC to boot it on, meaning essentially any x86-64 machine from about 2007 on.</li>
</ul>

<h2 id="iso">Download the ISO and write it</h2>
<p><a class="btn" href="https://github.com/hmofet/unodos/releases/latest/download/unodos-pc64.iso"><strong>Download unodos-pc64.iso</strong></a></p>
<p><code>unodos-pc64.iso</code> is a hybrid UEFI image: the same file boots as a virtual machine's
CD-ROM <em>and</em> writes to a USB stick with any standard imaging tool.</p>
<p><strong>With Rufus</strong> (Windows):</p>
<ol>
  <li>Open <a href="https://rufus.ie">Rufus</a> and plug in the USB stick; pick it under <strong>Device</strong>.</li>
  <li>Click <strong>SELECT</strong> and choose <code>unodos-pc64.iso</code>. Leave the other options as they are.</li>
  <li>Click <strong>START</strong>. If Rufus asks how to write the image, choose
      <strong>Write in DD Image mode</strong> (an exact copy). Confirm the erase and wait a few seconds.</li>
</ol>
<p><strong>With balenaEtcher</strong> (Windows, macOS, Linux):</p>
<ol>
  <li>Open <a href="https://etcher.balena.io">balenaEtcher</a> and click <strong>Flash from file</strong>; choose <code>unodos-pc64.iso</code>.</li>
  <li>Click <strong>Select target</strong> and pick the USB stick.</li>
  <li>Click <strong>Flash!</strong> and confirm.</li>
</ol>
{note('<b>The whole selected drive is erased.</b> Double-check you picked the USB stick and not an internal disk before confirming.', kind="warn", title="This wipes the drive")}
{note('On Linux or macOS you can also write it with <code>dd</code>: <code>dd if=unodos-pc64.iso of=/dev/&lt;your-usb-disk&gt; bs=4M</code>. There is also <b>unodos-pc64-uefi.img.gz</b>, a raw disk image for the same tools. All downloads are on the <a href="https://github.com/hmofet/unodos/releases">releases page</a>.', kind="tip", title="Command line and raw image")}

<h2 id="flasher">Or: the one-click flasher</h2>
<p>The UnoDOS flasher does the download-and-write in one step - it carries the image inside it:</p>
<ol>
  <li>Download it for your operating system:
    <ul>
      <li><strong>Windows</strong>: <a href="https://github.com/hmofet/unodos/releases/latest/download/UnoDosFlasher.exe">UnoDosFlasher.exe</a>.
          No install; it prompts for Administrator (raw disk writes need it).</li>
      <li><strong>macOS</strong>: <a href="https://github.com/hmofet/unodos/releases/latest/download/UnoDosFlasher-macOS.zip">UnoDosFlasher-macOS.zip</a>.
          Unzip and open <code>UnoDosFlasher.app</code>; it prompts for your administrator password.</li>
    </ul>
  </li>
  <li>Run it and <strong>plug in your USB stick</strong>. The flasher picks the smallest removable disk automatically; check it is the right one.</li>
  <li>Click <strong>Install</strong> and confirm the erase. The flasher writes a bootable UnoDOS image to the drive.</li>
</ol>
<div class="grid cols-2">
  {fig("flasher-windows.png", "The flasher on <b>Windows</b>: choose your USB drive and click Install.")}
  {fig("flasher-macos.png", "The flasher on <b>macOS</b>: the same steps.")}
</div>
{note('These builds are not signed with a paid developer certificate yet, so your OS may warn on first launch. On <b>Windows</b>, if SmartScreen appears, click <i>More info &rarr; Run anyway</i>. On <b>macOS</b>, right-click the app and choose <i>Open</i> the first time (Gatekeeper). The same applies to Rufus/Etcher warnings about an unrecognised image signature. Prefer to build it yourself? See the <a href="developer.html">Developer guide</a>.', kind="tip", title="First launch")}

<h2 id="boot">Boot the target PC</h2>
<ol>
  <li>Plug the USB stick into the PC you want to run UnoDOS on.</li>
  <li>Turn off <strong>Secure Boot</strong> in firmware setup (steps below). The image is unsigned, so it
      will not boot with Secure Boot on.</li>
  <li>Restart, open the one-time boot menu (often <kbd>F12</kbd>, <kbd>F9</kbd>, <kbd>F10</kbd> or
      <kbd>Esc</kbd> at power-on), and pick the USB stick.</li>
</ol>

<h3 id="securebook">Turning off Secure Boot</h3>
<ol>
  <li>Restart the PC and press the firmware-setup key at power-on. It is commonly <kbd>Del</kbd> on
      desktops and <kbd>F2</kbd> on laptops (some use <kbd>F1</kbd>, <kbd>F10</kbd> or <kbd>Esc</kbd>).</li>
  <li>Find <strong>Secure Boot</strong>, usually under a <em>Security</em>, <em>Boot</em> or
      <em>Authentication</em> menu, and set it to <strong>Disabled</strong>. Some firmwares first need you to
      set a supervisor/administrator password, or to switch the boot mode from "Windows UEFI" to "Other OS".</li>
  <li>Save and exit (usually <kbd>F10</kbd>).</li>
</ol>
<p>When you are finished with UnoDOS, turn Secure Boot back on the same way to return the PC to normal.</p>
{note('<b>If the PC runs Windows with BitLocker:</b> changing Secure Boot can make Windows ask for your BitLocker recovery key the next time it starts. This is reversible and does not erase anything. Enter the 48-digit recovery key (find it in your Microsoft account at <b>aka.ms/myrecoverykey</b>, or from your IT department), or simply turn Secure Boot back on to stop the prompt. To avoid it altogether, suspend BitLocker before changing Secure Boot (search Windows for <b>Manage BitLocker</b>, then <b>Suspend protection</b>) and resume it afterwards.', kind="warn", title="BitLocker recovery prompt")}

<h2 id="vm">Try it in a virtual machine first</h2>
<p>Prefer not to touch hardware yet? <strong>unodos-pc64.iso</strong> boots in any UEFI-capable
hypervisor. In <strong>VirtualBox</strong>:</p>
<ol>
  <li><strong>New</strong> machine &rarr; Type <em>Other</em>, Version <em>Other/Unknown (64-bit)</em>.
      A hard disk is optional - UnoDOS runs entirely from the ISO.</li>
  <li>Give it at least <strong>256 MB</strong> of memory (the <em>Other</em> profile's default is too small).</li>
  <li>Settings &rarr; <strong>System</strong> &rarr; tick <strong>Enable EFI (special OSes only)</strong>.
      UnoDOS is UEFI-native; without this the VM shows a black screen.</li>
  <li>Settings &rarr; <strong>Storage</strong> &rarr; put <code>unodos-pc64.iso</code> in the optical drive.</li>
  <li>Start. The splash, chime and desktop should appear in a few seconds. For sound, either audio
      controller works - UnoDOS has drivers for both <em>Intel HD Audio</em> and <em>ICH AC97</em>.</li>
</ol>
{fig("virtualbox.png", "The ISO booted in a <b>VirtualBox</b> EFI virtual machine - the same desktop as real hardware, captured straight from the VM's screen.")}
<p>Other hypervisors are the same idea: attach the ISO as a CD and boot with UEFI firmware -
<strong>VMware</strong> (firmware type UEFI), <strong>Hyper-V</strong> (a Generation&nbsp;2 VM with
Secure Boot turned off), or QEMU + OVMF (see <a href="developer.html#qemu">Run it in QEMU</a>).</p>

<h2 id="install">Install onto the PC (optional)</h2>
<p>Running from the USB stick is fine forever - but the <strong>Install</strong> app (in the Start
menu) can put UnoDOS on the computer itself, so it boots without the stick:</p>
<ul>
  <li><strong>Onto an existing EFI partition</strong> (the rows marked <em>keeps data</em>): UnoDOS is
      copied into its own folder next to your other operating systems and added to the firmware boot
      menu. <strong>Nothing is deleted</strong> - Windows keeps booting as before, and you can undo it
      by deleting the <code>\\EFI\\UNODOS</code> folder.</li>
  <li><strong>Onto a whole disk</strong> (the <em>Disk</em> rows): the entire disk is erased and
      becomes a UnoDOS disk. This is destructive and asks you to confirm twice.</li>
</ul>
<p>Pick a target with <kbd>↑</kbd>/<kbd>↓</kbd> and press <kbd>I</kbd> to install
(<kbd>R</kbd> rescans). The system files, fonts, documents and all the apps are copied along.</p>
{fig("install.png", "The <b>Install</b> app: pick a target, press <kbd>I</kbd>. Rows marked <i>keeps data</i> are non-destructive.", cls="shot-sm")}

<h2 id="firstboot">First boot</h2>
<p>A splash screen with a loading bar appears while UnoDOS starts up, then a short start-up chime
plays as the desktop appears - through the machine's sound hardware (HD&nbsp;Audio or AC'97) on a
modern PC, or the PC speaker on machines that have one. On a laptop the TrackPoint, touchpad and
keyboard all work.</p>
<div class="grid cols-2">
  {fig("splash.png", "The boot splash, with a loading bar and version.")}
  {fig("controlpanel.png", "First desktop paint: the Control Panel opens over the Aurora Light desktop.")}
</div>
<p>From here, everything is a keystroke away. <kbd>Ctrl</kbd>+<kbd>Esc</kbd> opens the programs menu (so does a right-click on the desktop);
arrow keys and <kbd>Enter</kbd> launch apps; <kbd>Ctrl</kbd>+<kbd>W</kbd> closes a window. The
<a href="desktop.html">desktop tour</a> covers the rest.</p>

<p class="kv">Want to build UnoDOS from source instead of flashing a prebuilt image? That's in the
<a href="developer.html">Developer guide</a>.</p>
""")

PAGES["desktop.html"] = ("The desktop", f"""
<h1>The desktop</h1>
<p class="lede">A themed desktop with a window manager, a scrollable programs menu, a taskbar and a
live clock. Every control works by keyboard or pointer.</p>

{fig("desktop.png", "The desktop: app icons, the <b>Start</b> button (bottom-left), the taskbar with a button per open window, and the clock.")}

<h2 id="furniture">Desktop furniture</h2>
<ul>
  <li><strong>Desktop icons</strong> launch apps directly. Arrange them in columns or rows,
      in launcher order or by name, from the Control Panel.</li>
  <li><strong>Right-click anywhere on the desktop</strong> for the scrollable <strong>programs
      menu</strong> ("Programs"), which opens at the pointer, lists every app and ends with
      <strong>Restart</strong> and <strong>Shut Down</strong>. <kbd>Ctrl</kbd>+<kbd>Esc</kbd>
      opens it too. There is no Start button - the taskbar is your open windows and the clock.</li>
  <li>The <strong>taskbar</strong> along the bottom shows a button for each open window; click one to bring it to the front.</li>
  <li>The <strong>clock</strong> in the corner shows the current time (and the battery level, on laptops).</li>
</ul>

{fig("startmenu.png", "The Start menu lists every app, then Restart and Shut Down. Open it with <kbd>Ctrl</kbd>+<kbd>Esc</kbd>; move with the arrows and launch with <kbd>Enter</kbd>.")}

<h2 id="windows">Windows</h2>
<p>Each window has a title bar with a <strong>close box</strong>. Drag the title bar to move a window.
Every app window can be resized: grab the ridged <strong>grip</strong> in the bottom-right corner, or
simply drag the window's <strong>right or bottom edge</strong> (an edge drag resizes that direction
only). Resizing reflows the contents live: the Editor re-wraps its document, the Files panes grow,
the browser re-wraps its text and Runner3D rescales its 3D view.</p>

<h2 id="keys">Keyboard &amp; pointer reference</h2>
<p>The desktop is designed to be fully usable without a mouse: <kbd>Tab</kbd> moves between controls,
the arrow keys adjust the focused control, and <kbd>Enter</kbd> activates it. On a laptop, the TrackPoint
or touchpad moves the pointer.</p>
<div class="tw"><table>
<thead><tr><th>Key</th><th>Action</th></tr></thead>
<tbody>
<tr><td><kbd>Ctrl</kbd>+<kbd>Esc</kbd></td><td>Open / close the Start menu</td></tr>
<tr><td><kbd>Ctrl</kbd>+<kbd>W</kbd></td><td>Close the focused window</td></tr>
<tr><td><kbd>F2</kbd> / <kbd>Ctrl</kbd>+<kbd>Tab</kbd></td><td>Raise the next open window</td></tr>
<tr><td><kbd>Tab</kbd> / <kbd>Shift</kbd>+<kbd>Tab</kbd></td><td>Move focus between controls in a window</td></tr>
<tr><td><kbd>↑</kbd> <kbd>↓</kbd> <kbd>←</kbd> <kbd>→</kbd></td><td>Adjust the focused control (dropdown value, slider, spinner, list, menu)</td></tr>
<tr><td><kbd>Enter</kbd></td><td>Activate a button, checkbox or menu item</td></tr>
<tr><td><kbd>Esc</kbd></td><td>Leave a full-screen game (Runner3D)</td></tr>
</tbody>
</table></div>
{note('Because a focused menu changes its value with <kbd>↑</kbd>/<kbd>↓</kbd>, you can switch themes, fonts and resolution entirely from the keyboard. See <a href="appearance.html">Themes &amp; appearance</a>.', kind="tip")}
""")

PAGES["appearance.html"] = ("Themes & appearance", f"""
<h1>Themes &amp; appearance</h1>
<p class="lede">Ten live themes, a Dark-mode toggle, TrueType fonts, and real resolution choice, all from
the Control Panel and all without a reboot.</p>

<h2 id="control">The Control Panel</h2>
<p>The Control Panel is where you change how UnoDOS looks: a <strong>Theme</strong> menu, a
<strong>Dark mode</strong> toggle, a <strong>Resolution</strong> menu, a system-wide <strong>Font</strong>
picker, a <strong>UI scale</strong> menu (100&ndash;200%), a <strong>Volume</strong> slider (it adjusts
the sound output live, even mid-note, on machines with HD&nbsp;Audio or AC'97 audio), and the clock:
the time is set with two spinners, and the date with the <strong>Set date&hellip;</strong> calendar
picker &mdash; click a day and it is applied. It opens on first boot and from the Start menu.</p>
{fig("controlpanel.png", "The Control Panel. Focus a menu with <kbd>Tab</kbd> and change it with <kbd>↑</kbd>/<kbd>↓</kbd>; the desktop re-skins instantly.")}

<h2 id="themes">The ten themes</h2>
<p>Aurora is the modern default, with soft shadows, rounded windows, a frosted taskbar and a coloured
underline under the active window title. The other eight are faithful retro looks. Choosing a theme
instantly re-skins the whole desktop.</p>
<div class="grid cols-2">
  {fig("theme_aurora_light.png", "<b>Aurora Light</b>: the default modern look.")}
  {fig("theme_aurora_dark.png", "<b>Aurora Dark</b>: the Dark-mode counterpart.")}
  {fig("theme_unodos.png", "<b>UnoDOS</b>: the house retro theme.")}
  {fig("theme_win31.png", "<b>Windows 3.1</b>: teal desktop, raised widgets.")}
  {fig("theme_macos7.png", "<b>Mac OS 7</b>: the classic platinum look.")}
  {fig("theme_macplus.png", "<b>Mac Plus</b>: black and white.")}
  {fig("theme_amiga.png", "<b>Amiga</b>: the Workbench palette.")}
  {fig("theme_c64.png", "<b>C64</b>: Commodore blue on blue.")}
  {fig("theme_apple2.png", "<b>Apple II</b>: the retro Apple look.")}
  {fig("theme_next.png", "<b>NeXTSTEP</b>: greyscale, chiselled bezels.")}
</div>

<h2 id="fonts">TrueType fonts</h2>
<p>All the on-screen text is drawn by a TrueType engine with proper proportional spacing and kerning.
Four faces are included: <strong>Chicago</strong> (the crisp bitmap-style default), <strong>Sans</strong>,
<strong>Mono</strong> and <strong>Ubuntu</strong>. Pick one and everything restyles &mdash; titles,
labels, buttons, lists &mdash; and the whole layout re-measures itself to fit the new face.</p>
{fig("font_ttf.png", "The same interface using the proportional <b>Sans</b> TrueType face.")}

<h2 id="scale">UI scale</h2>
<p>The <strong>UI scale</strong> menu makes everything bigger without changing the resolution: 100%,
125%, 150% or 200%. Every font scales and every window, menu and toolbar re-lays itself out to match
&mdash; handy on high-resolution laptop panels.</p>
{fig("uiscale.png", "The desktop at <b>150%</b> UI scale: same resolution, larger text and controls everywhere.")}

<h2 id="resolution">Resolution &amp; scaling</h2>
<p>Pick a resolution from the Control Panel and the desktop resizes to match, scaled to fill your screen
while keeping the correct proportions.</p>
{fig("resolution.png", "A smaller desktop mode scaled to fit the panel.")}
""")

PAGES["apps.html"] = ("Applications", f"""
<h1>Applications</h1>
<p class="lede">The full set of apps runs on the desktop: everyday tools, creative apps, games and a
3D runner. Launch any of them from the Start menu or a desktop icon.</p>

<h2 id="editor">The Editor: real word processing</h2>
<p>The Editor is a WordPad-class <strong>rich-text word processor</strong>. Select text and make it
<strong>bold</strong>, <em>italic</em> or underlined; mix <strong>four typefaces</strong> and
<strong>eight sizes</strong> in one document; set each paragraph left, centred or right. The document
word-wraps to the window and everything is on the menu bar, the toolbar, or a shortcut
(<kbd>Ctrl</kbd>+<kbd>B</kbd>/<kbd>I</kbd>/<kbd>U</kbd>, <kbd>Ctrl</kbd>+<kbd>S</kbd>/<kbd>O</kbd>/<kbd>N</kbd>,
<kbd>Ctrl</kbd>+<kbd>A</kbd>, <kbd>Ctrl</kbd>+<kbd>X</kbd>/<kbd>C</kbd>/<kbd>V</kbd>).
There's find &amp; replace, a ruler, and a status bar with the cursor position.</p>
<div class="grid cols-2">
  {fig("editor.png", "<b>Editor</b>: menu bar, toolbar (faces, sizes, B/I/U, alignment), ruler, word-wrapped document and status bar.")}
  {fig("editor_rich.png", "Rich text for real: the whole document selected and set bold from the keyboard.")}
</div>
<p>Documents save through the <strong>Open / Save As</strong> dialog to any writable volume: the
styled <strong>UWD</strong> format keeps the formatting, or name a file <code>.TXT</code> to save
plain text.</p>

<h2 id="filesapp">Files: a real file manager</h2>
<p>Files shows every mounted volume: the built-in RAM disk, the UnoDOS disk and any FAT-formatted
sticks or drives. Browse into folders (<kbd>Enter</kbd> opens, <kbd>Backspace</kbd> goes up) and use
the toolbar for real operations: <strong>new folder</strong>, <strong>rename</strong>,
<strong>delete</strong> (it asks twice), <strong>copy</strong> and <strong>move</strong>. The
<strong>Two panes</strong> button switches to a classic two-pane commander layout: copy and move then
target the other pane's folder.</p>
<div class="grid cols-2">
  {fig("files.png", "<b>Files</b>: volumes, folders and files with sizes, and a toolbar of real file operations.")}
  {fig("files_two.png", "The <b>two-pane</b> layout: the active pane's header is highlighted; Copy/Move target the other pane.")}
</div>

<h2 id="native">Everyday apps</h2>
<div class="grid cols-2">
  {fig("system.png", "<b>System</b>: device information in tidy groups - Input &amp; USB, Storage, Power &amp; ACPI, and Audio. The Storage group shows which native driver has taken over (<i>DETACHED (native): ahci0 / nvme0 / emmc0</i>), and Audio shows which backend the sound reaches (<i>HD Audio</i>, <i>AC'97</i>, or the PC speaker).")}
  {fig("clock.png", "<b>Clock</b>: an analog face beside a world map showing the day/night terminator, with world times for twenty cities.")}
</div>

<h2 id="creative">Creative tools</h2>
<div class="grid cols-3">
  {fig("paint.png", "<b>Paint</b>: pencil, shapes, fills and a colour palette.")}
  {fig("tracker.png", "<b>Tracker</b>: a 4-channel pattern sequencer.")}
  {fig("music.png", "<b>Music</b>: plays WAV, MIDI and MP3 files from disk, plus the built-in tunes.")}
</div>
<p>The games, Music and Tracker all make sound - through the machine's <strong>HD&nbsp;Audio</strong>
or <strong>AC'97</strong> audio hardware on modern PCs (which have no PC speaker), with the classic
PC-speaker beep as the fallback on machines that still have one. The Control Panel's Volume slider
sets the level.</p>

<h2 id="games">Games</h2>
<p>The classic games each run in their own window; <strong>Runner3D</strong> takes the whole screen
(press <kbd>Esc</kbd> to come back to the desktop).</p>
<div class="grid cols-2">
  {fig("dostris.png", "<b>Dostris</b>: the falling-block game, with score, lines and level.")}
  {fig("pacman.png", "<b>Pac-Man</b>: maze, dots, power pellets and ghosts.")}
  {fig("outlast.png", "<b>OutLast</b>: an arcade driving game.")}
  {fig("runner3d.png", "<b>Runner3D</b>: a real-time 3D game.")}
</div>
{note('Runner3D draws real-time 3D graphics entirely in software, so it needs no graphics card.', title="3D graphics")}

<h2 id="modules">Apps live on the disk</h2>
<p>The games and creative tools are not baked into the system: each one is a small
<code>.UNO</code> file in the <code>APPS</code> folder of the UnoDOS disk, loaded the first time you
open it. Installing UnoDOS onto a PC copies them along automatically. If an app's file is missing,
its window simply says so - nothing crashes.</p>
""")

PAGES["browser.html"] = ("Web browser", f"""
<h1>Web browser</h1>
<p class="lede">A built-in web browser that shows HTML, Markdown and basic CSS, runs JavaScript, and loads
pages from local disks or over the web (HTTP and HTTPS).</p>

<h2 id="files">Files and the address bar</h2>
<p>The browser opens on a file list that shows the built-in RAM disk and any FAT-formatted USB sticks or
disks together. Type a web address in the bar at the top, or press <kbd>↓</kbd> to move into the file
list; <kbd>Enter</kbd> opens a file and <kbd>Backspace</kbd> goes back.</p>
{fig("browser_files.png", "The file list: built-in documents (✳) and USB or disk files (›) together, with the web address bar on top.")}

<h2 id="render">HTML, Markdown &amp; CSS</h2>
<p>The browser lays out headings, word-wrapped paragraphs, bold and italic, code, links, lists and
preformatted text, for both HTML and Markdown &mdash; all typeset with real TrueType typography:
large bold headings, a monospace face for code, and true italics.</p>
<div class="grid cols-2">
  {fig("browser_markdown.png", "A <b>Markdown</b> document: headings, bold and italic, inline code and lists.")}
  {fig("browser_html.png", "An <b>HTML</b> page: emphasis, code, links, lists and preformatted text. Unknown tags are ignored.")}
</div>

<h2 id="js">JavaScript</h2>
<p>The browser runs the JavaScript on a page and adds its output to the page, with a console panel at the
bottom for <code>console.log</code> messages.</p>
{fig("browser_js.png", "<b>Script.html</b>: its JavaScript generated this Fibonacci table on the page.")}

<h2 id="net">Over the network</h2>
<p>Type a web address and press <kbd>Enter</kbd>; UnoDOS connects, looks up the site, loads the page and
runs any JavaScript on it.</p>
<div class="grid cols-2">
  {fig("browser_http.png", "A live page loaded over <b>HTTP</b>.")}
  {fig("browser_https.png", "A secure page loaded over <b>HTTPS</b>.")}
</div>
{note('Secure (https://) pages load over an encrypted TLS connection, and UnoDOS checks the site certificate against a built-in list of common certificate authorities. See <a href="networking.html">Networking</a>.', title="Secure sites")}
""")

PAGES["networking.html"] = ("Networking", f"""
<h1>Networking</h1>
<p class="lede">UnoDOS can get online by itself: it connects over Ethernet, gets an address automatically,
and browses the web, including secure sites.</p>

<h2 id="online">Getting online</h2>
<p>UnoDOS has its own built-in networking. On a PC with a supported wired network adapter, it gets an
address automatically (DHCP), finds the gateway and other machines, and browses the web over HTTP and
HTTPS. Nothing else needs to be installed.</p>

<h2 id="selftest">The Network self-test</h2>
<p>The Network app checks the whole connection step by step and shows the result. In an emulator it tests
against the emulator's network; on a real PC it uses the real adapter.</p>
{fig("network.png", "The Network self-test passing every step: network link, an automatic address, reaching the gateway, transferring data, and a secure (TLS) connection.")}

<h2 id="tls">Secure sites</h2>
<p>Secure (<code>https://</code>) pages load over an encrypted TLS connection, and UnoDOS checks the site's
certificate against a built-in list of common certificate authorities (Let's Encrypt, DigiCert and others),
so you can browse the secure web.</p>

<h2 id="usb">USB Ethernet</h2>
<p>If a PC has no built-in wired network, UnoDOS can use a USB Ethernet adapter instead. The driver
speaks to adapters built on the <strong>ASIX AX88179 / AX88178A</strong> chip &mdash; when you shop,
check the listing's chipset line for those names. Known AX88179-based products include:</p>
<ul>
  <li><strong>Plugable USB3-E1000</strong> (USB&nbsp;3.0 gigabit)</li>
  <li><strong>StarTech USB31000S</strong></li>
  <li><strong>TRENDnet TU3-ETG</strong></li>
  <li><strong>j5create JUE130</strong></li>
</ul>
<p>Adapters built on Realtek chips (the RTL8153 family - for example the TP-Link UE300) use a
different chip and are <em>not</em> supported yet. Wi-Fi is not supported yet either.</p>
""")

PAGES["ports.html"] = ("The UnoDOS family", f"""
<h1>The UnoDOS family</h1>
<p class="lede">pc64 is one of many. The same GUI-first UnoDOS runs on more than 20 kinds of hardware,
from 8-bit consoles to modern ARM boards. Here is how the others differ.</p>

<h2 id="pattern">One idea, many machines</h2>
<p>The same UnoDOS desktop and apps run on machines as different as a Commodore 64 and a Raspberry Pi. On
each one, UnoDOS asks the machine for a screen and for input, then runs the same way. A shared design keeps
every version consistent instead of drifting apart.</p>

<h2 id="tiers">How much desktop each machine gets</h2>
<p>It depends on how much memory the machine has:</p>
<ul>
  <li><strong>The full desktop:</strong> more capable machines (pc64, PlayStation&nbsp;2, Dreamcast, and the
  ARM and PowerPC boards) run the full desktop shown in this manual.</li>
  <li><strong>A simpler desktop:</strong> the smallest machines (NES, Game&nbsp;Boy, the C64) have very little
  memory, so they run a simpler icon-and-button desktop instead.</li>
</ul>

<h2 id="gallery">A few of the machines</h2>
<div class="grid cols-2">
  {fig("classic_xt.png", "<b>The original</b>: UnoDOS on an IBM PC/XT, fitting a full desktop and 19 apps on a single 1.44 MB floppy.")}
  {fig("port_c64.png", "<b>Commodore 64</b>: the simpler icon-and-button desktop.")}
  {fig("port_dreamcast.png", "<b>Sega Dreamcast</b>: a game console running the full desktop.")}
  {fig("port_pinephone.png", "<b>PinePhone</b>: the desktop in portrait on a phone.")}
  {fig("port_rpi.png", "<b>Raspberry Pi</b>: UnoDOS on the popular ARM board.")}
  {fig("port_ppcmac.png", "<b>PowerPC Mac</b>: on a classic Power Macintosh.")}
  {fig("port_iigs.png", "<b>Apple IIGS</b>.")}
  {fig("port_mac.png", "<b>Classic Macintosh</b>: on a compact 68000 Mac.")}
</div>

<h2 id="table">The full lineup</h2>
<p>Every port's ready-to-run image is committed in the repository, so each
<em>Download</em> link below always gets you the latest build - a ROM for a
console runs in any emulator or on a flash cart, and each port's folder has a
README with the details.</p>
<div class="tw"><table>
<thead><tr><th>World</th><th>Hardware</th><th>CPU</th><th>Boot / display</th><th>Download</th></tr></thead>
<tbody>
<tr><td><strong>pc64</strong></td><td>Modern PC (2007+)</td><td>x86-64</td><td>UEFI GOP <span class="muted">(this manual)</span></td><td><a href="https://github.com/hmofet/unodos/releases/latest">ISO &amp; flasher</a></td></tr>
<tr><td>Classic</td><td>IBM PC/XT</td><td>Intel 8088+</td><td>BIOS · CGA</td><td><a href="https://github.com/hmofet/unodos/raw/master/build/unodos-144.img">floppy image</a></td></tr>
<tr><td>Amiga</td><td>Commodore Amiga</td><td>68000</td><td>native chipset</td><td><a href="https://github.com/hmofet/unodos/raw/master/amiga/build/unodos68k.adf">ADF</a></td></tr>
<tr><td>Mac Plus</td><td>Compact Macintosh</td><td>68000</td><td>native</td><td><a href="https://github.com/hmofet/unodos/raw/master/macplus/build/unodos_macplus.dsk">disk image</a></td></tr>
<tr><td>PowerPC Mac</td><td>Power Macintosh</td><td>PowerPC 32-bit</td><td>Open Firmware</td><td><a href="https://github.com/hmofet/unodos/raw/master/ppcmac/build/unodos.bin">boot image</a></td></tr>
<tr><td>Apple II</td><td>Apple II</td><td>MOS 6502</td><td>native</td><td><a href="https://github.com/hmofet/unodos/raw/master/apple2/build/unodos_apple2.dsk">disk image</a></td></tr>
<tr><td>Apple IIGS</td><td>Apple IIGS</td><td>65C816</td><td>native</td><td><a href="https://github.com/hmofet/unodos/raw/master/iigs/build/unodos_iigs.po">disk image</a></td></tr>
<tr><td>C64</td><td>Commodore 64</td><td>6510</td><td>VIC-II · SID</td><td><a href="https://github.com/hmofet/unodos/raw/master/c64/build/unodos_c64.d64">D64</a></td></tr>
<tr><td>VIC-20</td><td>Commodore VIC-20</td><td>6502</td><td>VIC 6560/1</td><td><a href="https://github.com/hmofet/unodos/raw/master/vic20/build/unodos.prg">PRG</a></td></tr>
<tr><td>NES</td><td>Nintendo NES</td><td>6502 / 2A03</td><td>PPU</td><td><a href="https://github.com/hmofet/unodos/raw/master/nes/build/unodos.nes">ROM</a></td></tr>
<tr><td>SNES</td><td>Super Nintendo</td><td>65C816</td><td>native</td><td><a href="https://github.com/hmofet/unodos/raw/master/snes/build/unodos.sfc">ROM</a></td></tr>
<tr><td>Master System</td><td>Sega Master System</td><td>Z80</td><td>315-5124 VDP</td><td><a href="https://github.com/hmofet/unodos/raw/master/sms/build/unodos.sms">ROM</a></td></tr>
<tr><td>Game Gear</td><td>Sega Game Gear</td><td>Z80</td><td>315-5124 VDP</td><td><a href="https://github.com/hmofet/unodos/raw/master/gg/build/unodos.gg">ROM</a></td></tr>
<tr><td>Genesis</td><td>Sega Mega Drive</td><td>68000 + Z80</td><td>native</td><td><a href="https://github.com/hmofet/unodos/raw/master/genesis/build/unodos.gen">ROM</a></td></tr>
<tr><td>Game Boy / Color</td><td>Nintendo Game Boy</td><td>Sharp SM83</td><td>native</td><td><a href="https://github.com/hmofet/unodos/raw/master/gb/build/unodos.gb">ROM</a></td></tr>
<tr><td>Game Boy Advance</td><td>Nintendo GBA</td><td>ARM7TDMI</td><td>native</td><td><a href="https://github.com/hmofet/unodos/raw/master/gba/build/unodos.gba">ROM</a></td></tr>
<tr><td>PC Engine</td><td>NEC TurboGrafx-16</td><td>HuC6280</td><td>HuC6270 VDC</td><td><a href="https://github.com/hmofet/unodos/raw/master/pce/build/unodos.pce">ROM</a></td></tr>
<tr><td>WonderSwan</td><td>Bandai WonderSwan</td><td>NEC V30MZ</td><td>native</td><td><a href="https://github.com/hmofet/unodos/raw/master/ws/build/unodos.ws">ROM</a></td></tr>
<tr><td>Dreamcast</td><td>Sega Dreamcast</td><td>SH-4</td><td>native</td><td><a href="https://github.com/hmofet/unodos/raw/master/dreamcast/build/unodos-dc-uui.elf">ELF</a> · <a href="https://github.com/hmofet/unodos/raw/master/dreamcast/build/unodos-dc-uui.iso">ISO</a></td></tr>
<tr><td>PlayStation 2</td><td>Sony PS2</td><td>Emotion Engine</td><td>native</td><td><a href="https://github.com/hmofet/unodos/raw/master/ps2/build/unodos-ps2-uui.elf">ELF</a></td></tr>
<tr><td>Raspberry Pi</td><td>Raspberry Pi</td><td>ARM Cortex-A (AArch64)</td><td>VideoCore mailbox FB</td><td><a href="https://github.com/hmofet/unodos/raw/master/rpi/build/kernel8.img">kernel8.img</a></td></tr>
<tr><td>PinePhone</td><td>PinePhone</td><td>Allwinner A64 (AArch64)</td><td>DE2 display engine</td><td><a href="https://github.com/hmofet/unodos/raw/master/pinephone/build/unodos.bin">boot image</a></td></tr>
</tbody>
</table></div>
{note('Full details for every machine live in the repository: <a href="https://github.com/hmofet/unodos" target="_blank" rel="noopener">github.com/hmofet/unodos</a>.', title="Where to read more")}
""")

PAGES["developer.html"] = ("Developer guide", f"""
<h1>Developer guide: overview &amp; architecture</h1>
<p class="lede">This section is for people building or extending UnoDOS pc64 itself, or writing apps for it.
End users do not need any of it; the <a href="getting-started.html">flasher</a> covers them.</p>

<p>pc64 is a bare-metal <strong>x86-64 UEFI</strong> operating system written entirely in freestanding C:
no host C library, no underlying OS. It ships two interchangeable desktops, selected at build time:</p>
<ul>
  <li><strong>unoui</strong> (the default, <code>./build.sh</code>): the modern themed desktop this manual documents,
      built on the cross-platform <em>unoui</em> widget toolkit.</li>
  <li><strong>legacy</strong> (<code>./build.sh legacy</code>): the older core with 14 hand-drawn apps, kept only
      as reference. Everything it did now runs in the unoui desktop.</li>
</ul>

<h2 id="layers">The layers</h2>
<p>From the top down:</p>
<div class="tw"><table>
<thead><tr><th>Layer</th><th>What it is</th></tr></thead>
<tbody>
<tr><td><strong>Apps</strong></td><td>Native <em>unoui</em> widget apps, custom-drawn canvas apps (games, Paint, browser, Runner3D), and bridged legacy apps.</td></tr>
<tr><td><strong>Shell</strong></td><td><code>pc64_uui.c</code>: the themed desktop, icons, taskbar, window z-order, and app open/close.</td></tr>
<tr><td><strong>Toolkit (unoui)</strong></td><td>The portable widget core: windows, widgets, events, and a swappable theme (ten themes ship).</td></tr>
<tr><td><strong>Framebuffer (fb)</strong></td><td>A 32-bit software framebuffer: clipping, alpha blend, gradients, anti-aliased rounded rects, fractional fill-scaling, and dirty-row present-on-change.</td></tr>
<tr><td><strong>Platform (UEFI)</strong></td><td>A hand-rolled UEFI surface: the GOP framebuffer, keyboard, pointer, and Boot Services. No gnu-efi or EDK2.</td></tr>
<tr><td><strong>Drivers (tail)</strong></td><td>e1000 NIC and the TCP/IP + TLS stack, xHCI USB and USB Ethernet, native AHCI/NVMe/SDHCI storage, HD&nbsp;Audio and AC'97 PCM audio, uno3d 3D, UnoSound, and the TrueType engine.</td></tr>
</tbody>
</table></div>

<h2 id="boot">Boot flow</h2>
<ol>
  <li><strong>Firmware handoff.</strong> UEFI GOP provides a linear 32-bit framebuffer; Simple Text Input provides
      the keyboard with modifier state; the Simple and Absolute Pointer protocols provide a mouse where the
      firmware binds one.</li>
  <li><strong>Boot Services stay alive.</strong> UEFI Boot Services take the role the old INT 10h/13h/15h calls
      played for the 16-bit kernel. <code>ExitBootServices</code> and native drivers are the driver <em>tail</em>,
      not a bring-up requirement.</li>
  <li><strong>Platform + shell init,</strong> then the desktop paints and a start-up chime plays. The UEFI
      watchdog is disabled at startup (<code>SetWatchdogTimer(0, ...)</code>) so a long-running app is not reset
      after five minutes.</li>
</ol>

<h2 id="freestanding">The freestanding model</h2>
<p>The build target is a <strong>PE32+ UEFI application</strong> (<code>EFI/BOOT/BOOTX64.EFI</code>) produced by
mingw-w64. There is no host libc: the project ships its own headers in <code>pc64/include/</code>, a small libc
in <code>pc64_libc.c</code>, and float math in <code>pc64_math.c</code>. Hot paths avoid dynamic allocation
(the TLS engine, the browser JavaScript engine, and the 3D math are all no-malloc).</p>
{note('The single most important portability rule: under mingw, <b>long is 32-bit</b> (LLP64). Use <code>unsigned long long</code> or <code>uintptr_t</code> for every address and 64-bit value. A truncated 64-bit DMA address was a real, hard-to-find bug in the NIC driver.', kind="warn", title="LLP64: long is 32-bit")}

<h2 id="contract">The Contract (unodef)</h2>
<p>The wider UnoDOS family is generated from, or checked against, a single machine-readable <strong>Contract</strong>
in <code>unodef/</code>. It keeps every port (from the 8-bit consoles to pc64) consistent. For a pc64 app author
it is background: you code against the concrete C headers, <code>unoui.h</code> and <code>uno_app.h</code>.</p>

<p class="kv">Next: <a href="dev-apps.html">Writing apps</a>, the <a href="dev-api.html">API reference</a>, and
<a href="dev-build.html">Building &amp; tooling</a>.</p>
""")

PAGES["dev-apps.html"] = ("Writing apps", f"""
<h1>Writing apps</h1>
<p class="lede">pc64 apps are linked statically into the single boot image. There are three styles; the
native widget app is the normal path.</p>

<ul>
  <li><strong>Native widget app</strong>: build a window out of toolkit widgets and let the shell run the event loop.</li>
  <li><strong>Canvas app</strong>: the toolkit owns the window chrome, focus, dragging and z-order; your app owns
      the pixels inside a canvas rectangle and receives raw input. Games, Paint, Tracker, the browser and
      Runner3D are canvas apps.</li>
  <li><strong>Legacy bridge app</strong>: an existing <code>AppInterface</code> app hosted inside a canvas through
      the compatibility bridge.</li>
</ul>

<h2 id="native">A native widget app</h2>
<p>An app is simply a <em>builder function</em> that populates a window. It uses only <code>unoui.h</code> calls,
and every widget is reachable by pointer or keyboard for free.</p>
{CODE_HELLO_NATIVE}
<p>The shell turns each input event into an <code>unoui_action</code> and dispatches by the widget <code>id</code>
you assigned:</p>
{CODE_SHELL_LOOP}
<p>Widget positions are relative to the window <em>content</em> origin (inside the frame and title bar); the
toolkit computes that origin from the active theme, so hit-testing always matches what is drawn.</p>

<h2 id="register">Registering and building an app in</h2>
<p>There is no runtime module loading on bare metal: every app is compiled into <code>BOOTX64.EFI</code>. The shell
keeps a table of builder functions and opens an app on demand:</p>
{CODE_OPEN_APP}
<p>Native builders live in a function-pointer table (<code>g_build[]</code> in <code>pc64_uui.c</code>); a desktop
icon or Start-menu row calls <code>open_app(index)</code>. Bridged legacy apps are compiled once each with a
distinct entry symbol via <code>-DUNO_APP_SYM=uno_app_main_&lt;name&gt;</code> so they can all coexist in one binary.</p>

<h2 id="canvas">A canvas app</h2>
<p>Give the toolkit a <code>unoui_canvas</code> (a draw callback plus an event callback) and it manages the window
around your pixels:</p>
{CODE_HELLO_CANVAS}
<p>For a game or a 3D view, call <code>unoui_fullscreen(&amp;ui, win)</code> to make the canvas fill the screen with
all input routed to it (<kbd>Esc</kbd> returns), and <code>unoui_fullscreen(&amp;ui, NULL)</code> to restore the
desktop. Mark widgets that should stretch on resize with <code>unoui_widget_fill</code> and set the
<code>UI_WIN_RESIZE</code> window flag so canvas apps reflow.</p>

<h2 id="bridge">The legacy bridge</h2>
<p>The older family apps implement a small shared ABI in <code>pc64/uno_app.h</code>. Each exports one entry that
returns a vtable of callbacks; the kernel dispatches purely through the pointers, with no per-app
<code>switch</code>.</p>
{CODE_APPIFACE}
<p>On pc64 these apps are handed a <code>KernelApi</code> callback table (drawing primitives, a FAT reader, and the
music engine) and hosted inside a canvas, so they run unchanged inside the modern desktop.</p>
""")

PAGES["dev-api.html"] = ("API reference", f"""
<h1>API reference</h1>
<p class="lede">The public surface an app or driver codes against. Signatures are quoted from the headers
(<code>unoui/unoui.h</code>, <code>pc64/fb.h</code>, <code>pc64/uno_app.h</code>, <code>uno3d.h</code>).</p>

<h2 id="windows">unoui: windows</h2>
{CODE_UNOUI_WIN}

<h2 id="widgets">unoui: widgets</h2>
<p>Every constructor returns a <code>unoui_widget *</code>; set its <code>-&gt;id</code> so you can recognise it in
the returned <code>unoui_action</code>.</p>
{CODE_UNOUI_WIDGETS}

<h2 id="text">unoui: editable text</h2>
<p>Fields and text areas edit a buffer the app owns; the toolkit tracks caret and selection in place.</p>
{CODE_UNOUI_TEXT}

<h2 id="events">unoui: the event model</h2>
<p>This is the portability contract. A platform adapter produces <code>unoui_event</code>s;
<code>unoui_handle</code> returns an <code>unoui_action</code> describing what changed.</p>
{CODE_UNOUI_EVENT}

<h2 id="theming">unoui: theming</h2>
<p>A theme is a semantic colour palette plus metrics plus an optional vtable of chrome painters; a NULL painter
falls back to the portable default, so the same widgets render on 1-bit through 32-bit targets. Ten themes ship:
<code>theme_aurora_light</code>, <code>theme_aurora_dark</code>, <code>theme_unodos</code>, <code>theme_macos7</code>,
<code>theme_macplus</code>, <code>theme_win31</code>, <code>theme_amiga</code>, <code>theme_c64</code>,
<code>theme_apple2</code>, <code>theme_next</code>. Switch live with <code>unoui_ui_theme(&amp;ui, &amp;theme_c64)</code>.</p>

<h2 id="fb">Framebuffer (fb)</h2>
<p>The software drawing surface underneath the toolkit; canvas apps draw with it directly.</p>
{CODE_FB}

<h2 id="platform">Platform subsystems</h2>
<p>Names and roles (see the corresponding <code>.c</code> for exact prototypes):</p>
<div class="tw"><table>
<thead><tr><th>Subsystem</th><th>Role</th></tr></thead>
<tbody>
<tr><td><code>pc64_fs</code> / <code>pc64_io</code></td><td>Unified file namespace: volume 0 is the RAM disk, volumes 1+ are FAT/FAT32 disks mounted by UnoDOS's own FAT stack (read/write) over the native AHCI/NVMe/SDHCI drivers, with firmware Simple File System volumes as read-only extras while attached.</td></tr>
<tr><td><code>unosound</code></td><td>Single-voice sequencer; the shared audio path for the games, Music and Tracker (<code>uno_seq_beep</code> / <code>_play</code> / <code>_stop</code>). On pc64 the voice renders into an HD&nbsp;Audio / AC'97 PCM ring when one exists (<code>snd_pcm.c</code>), else the PC speaker.</td></tr>
<tr><td><code>pc64_pci</code></td><td>PCI config scan; locates the e1000 NIC, xHCI controllers and the Intel iGPU.</td></tr>
<tr><td><code>net</code> / <code>e1000</code></td><td>e1000 driver publishing a <code>uno_nic_t</code>, plus a from-scratch stack: ARP, IPv4, ICMP, UDP, DHCP, minimal TCP.</td></tr>
<tr><td><code>pc64_http</code> / <code>pc64_browser</code> / <code>js</code></td><td>HTTP/1.0 GET with DNS, the immediate-mode HTML/Markdown/CSS renderer, and the JavaScript interpreter.</td></tr>
<tr><td><code>tls</code> / <code>bearssl</code></td><td>Freestanding BearSSL, TLS 1.2, with pinned-key and CA-validated (14 roots) modes; clock from the UEFI RTC.</td></tr>
<tr><td><code>xhci</code> / <code>ax88179</code></td><td>Opt-in (<code>-DUNO_XHCI</code>) polled xHCI host and an ASIX USB-gigabit driver that also publishes a <code>uno_nic_t</code>.</td></tr>
<tr><td><code>pc64_font</code></td><td>Optional TrueType engine; registers as the fb text provider with subpixel smoothing, falling back to the built-in bitmap font.</td></tr>
</tbody>
</table></div>

<h2 id="uno3d">3D (uno3d)</h2>
<p>A small write-once 3D pipeline with a software rasteriser (Gouraud shading, no textures). Runner3D drives it
directly.</p>
{CODE_UNO3D}
""")

PAGES["dev-build.html"] = ("Building & tooling", f"""
<h1>Building &amp; tooling</h1>
<p class="lede">Building UnoDOS pc64 from source, running it under QEMU, packing a bootable USB image,
building the flashers, and regenerating this manual.</p>

<h2 id="need">Toolchain</h2>
<ul>
  <li><strong>Build:</strong> <code>x86_64-w64-mingw32-gcc</code> (UEFI apps are PE32+ images, the mingw target's
      native output, so no gnu-efi or EDK2) and <code>python3</code>.</li>
  <li><strong>USB image:</strong> <code>sgdisk</code> (gptfdisk), <code>mtools</code>, <code>python3</code>.</li>
  <li><strong>Emulator:</strong> <code>qemu-system-x86</code> + OVMF.</li>
</ul>
<p class="muted">On Ubuntu: <code>sudo apt install gcc-mingw-w64-x86-64 qemu-system-x86 ovmf mtools gdisk python3</code></p>

<h2 id="build">Build &amp; run</h2>
<p>From the <code>pc64/</code> directory:</p>
{CODE_BUILD}
<p>The build compiles the platform, framebuffer, toolkit, ten themes, apps and drivers freestanding, and links a
single <code>BOOTX64.EFI</code> into a bootable <strong>ESP</strong> tree in <code>build/esp/</code>. The default
image is about 660&nbsp;KB.</p>
{note('The desktop is fully keyboard-driven, so QEMU needs no special input setup. QEMU is also scriptable over QMP (<code>send-key</code> + <code>screendump</code>), which is how the screenshots in this manual are made.', title="Keyboard-first")}

<h2 id="image">Pack a real USB disk image</h2>
<p>QEMU fakes a disk from the <code>build/esp/</code> directory, but real firmware needs a partition table.
<code>tools/mkuefi.py</code> turns the ESP tree into a raw disk image: GPT with one FAT32 EFI System Partition
holding the whole tree. This is the image the flashers embed.</p>

<h2 id="flags">Feature flags</h2>
<div class="tw"><table>
<thead><tr><th>Flag</th><th>Effect</th></tr></thead>
<tbody>
<tr><td><code>-DUNO_XHCI</code></td><td>Compile the xHCI USB host + USB-Ethernet stack (inert stubs otherwise).</td></tr>
<tr><td><code>-DUNO_I2C_TRACKPAD</code></td><td>Enable the native I2C-HID trackpad driver (inert stubs otherwise).</td></tr>
<tr><td><code>-DUNO_DBGCON</code></td><td>Enable the port-0x402 debug console. Off by default: it SMM-traps and hangs some real hardware.</td></tr>
<tr><td><code>-DUNO_APP_SYM=...</code></td><td>Name a bridged app's entry symbol so multiple apps link into one binary.</td></tr>
</tbody>
</table></div>

<h2 id="gotchas">Gotchas worth knowing</h2>
<ul>
  <li><strong>LLP64:</strong> <code>long</code> is 32-bit under mingw. Use <code>unsigned long long</code> /
      <code>uintptr_t</code> for addresses.</li>
  <li><strong>Freestanding:</strong> no host libc or CRT; use <code>pc64_libc.c</code> / <code>pc64_math.c</code>.</li>
  <li><strong>Resolution:</strong> some laptop panels black out on a real <code>SetMode</code>. pc64 keeps the GOP
      mode and scales the desktop framebuffer to fit instead.</li>
  <li><strong>Watchdog:</strong> disable it at startup or UEFI resets the machine after about five minutes.</li>
  <li><strong>Present-on-change:</strong> only changed framebuffer rows are written to video memory; full-frame
      rewrites every frame ruin input smoothness on real hardware.</li>
</ul>

<h2 id="flashers">Build the flashers</h2>
<p>The <a href="getting-started.html">flashers</a> each embed a gzipped copy of the USB image and stream it to
the raw device. Source: <a href="https://github.com/hmofet/unodos/tree/master/pc64/flash" target="_blank" rel="noopener"><code>pc64/flash/</code></a>.</p>
<div class="tw"><table>
<thead><tr><th>Target</th><th>Build on</th><th>Command</th><th>Output</th></tr></thead>
<tbody>
<tr><td><strong>Windows</strong></td><td>Windows + WSL (in-box <code>csc.exe</code>, no SDK)</td><td><code>pc64\\flash\\build-flasher.ps1</code></td><td><code>UnoDosFlasher.exe</code></td></tr>
<tr><td><strong>macOS</strong></td><td>a Mac (Swift toolchain)</td><td><code>pc64/flash/mac/build-app.sh</code></td><td><code>UnoDosFlasher.app</code> (universal)</td></tr>
</tbody>
</table></div>
<p>Pass <code>--skip-build</code> to reuse an existing <code>build/unodos-uefi.img</code> (useful when the image is
built on another machine). The built binaries are published on the project's GitHub Releases.</p>

<h2 id="shots">Regenerate the manual screenshots</h2>
<p>The desktop screenshots in this manual are produced headlessly by <code>pc64/docs_shots.py</code>: it boots the
real image under QEMU + OVMF and drives the desktop over QMP, capturing each scene to
<code>pc64/shots/manual/</code>. Because the emulated pointer does not reach the shell, every scene is driven from
the keyboard.</p>
<pre><code>python3 docs_shots.py                               # all scenes
python3 docs_shots.py themes editor browser_docs    # selected scenes
UNO_NIC=1 python3 docs_shots.py browser_http        # networking scenes</code></pre>
<p>Copy the PNGs you need into <code>docs/assets/img/</code>, then rebuild and commit. If the shell's app roster or
the Control Panel tab order changes, update the scene offsets in <code>docs_shots.py</code> first.</p>
""")

# --------------------------------------------------------------------------- emit
def main():
    os.makedirs(os.path.join(OUT, "assets", "img"), exist_ok=True)
    with open(os.path.join(OUT, "assets", "style.css"), "w", newline="\n", encoding="utf-8") as f:
        f.write(CSS)
    open(os.path.join(OUT, ".nojekyll"), "w").close()
    for fname, (title, body) in PAGES.items():
        with open(os.path.join(OUT, fname), "w", newline="\n", encoding="utf-8") as f:
            f.write(page(fname, title, body))
        print("wrote", fname)
    print("done ->", OUT)

if __name__ == "__main__":
    main()
