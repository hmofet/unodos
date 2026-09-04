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
    ("try-browser.html",     "Try it in your browser"),
    ("getting-started.html", "Getting started"),
    ("desktop.html",         "The desktop"),
    ("windows.html",         "Windows &amp; desktops"),
    ("appearance.html",      "Themes &amp; appearance"),
    ("apps.html",            "Applications"),
    ("office.html",          "UnoOffice"),
    ("code.html",            "UnoCode"),
    ("browser.html",         "Web browser"),
    ("ssh.html",             "SSH client"),
    ("transfer.html",        "UnoTransfer"),
    ("networking.html",      "Networking"),
    ("logging.html",         "System log"),
    ("appliances.html",      "Appliances"),
    ("ports.html",           "The UnoDOS family"),
    (None,                   "Developer"),          # section header
    ("developer.html",       "Overview &amp; architecture"),
    ("studio.html",          "Studio: the built-in IDE"),
    ("unocode.html",         "UnoCode: the VS Code-class editor"),
    ("dev-apps.html",        "Writing apps"),
    ("dev-python.html",      "Python apps"),
    ("dev-samples.html",     "Sample programs"),
    ("dev-sdk-c.html",       "UnoC SDK reference"),
    ("dev-sdk-python.html",  "Python SDK reference"),
    ("dev-api.html",         "API reference"),
    ("dev-build.html",       "Building &amp; tooling"),
    ("dev-remote.html",      "Remote control &amp; automation"),
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
figure.film video{display:block;width:100%;height:auto;background:#0b0d13}
figure.film .fallback{padding:15px;margin:0;color:var(--muted)}
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

DEMO_MP4 = "https://unodos.arinbakht.com/assets/unodos-demo.mp4"
DUUM_MP4 = "https://unodos.arinbakht.com/assets/duum-demo.mp4"

def film(poster, cap, mp4=DEMO_MP4):
    """A demo film, embedded the way fig() embeds a screenshot.

    The films are large and this manual is a small static site that also has to
    read sensibly offline, so the video is streamed from the UnoDOS website
    rather than committed here, while the poster frame IS local. An offline
    reader still gets the figure, the poster and the caption; only playback
    needs a connection. preload="none" means the page costs nothing extra
    until the reader presses play. `mp4` picks which film (the whole-OS demo by
    default, or the shorter Duum film on the Python page)."""
    alt = html.escape(re.sub(r"<[^>]+>", "", cap))
    return (f'<figure class="film"><video controls preload="none" '
            f'poster="assets/img/{poster}" aria-label="{alt}">'
            f'<source src="{mp4}" type="video/mp4">'
            f'<p class="fallback">Your browser cannot play this video. '
            f'<a href="{mp4}">Download the film</a> instead.</p>'
            f'</video><figcaption>{cap}</figcaption></figure>')

def note(body, kind="", title="Note"):
    k = f" {kind}" if kind else ""
    return f'<div class="note{k}"><b>{title}</b>{body}</div>'

def code(s):
    # plain concat (not an f-string) so C braces in `s` pass through untouched
    return "<pre><code>" + html.escape(s.strip("\n")) + "</code></pre>"

def sdk_source(name):
    """Quote a shipped SDK sample verbatim (pc64/sdk/), so the manual can
    never drift from the file the user actually opens in Studio."""
    with open(os.path.join(_HERE, "..", "pc64", "sdk", name), "r", encoding="utf-8") as f:
        return code(f.read())

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

CODE_UNOC_SAMPLE = code('''#include "UNO.H"          /* the whole app SDK in one header */

static short bx = 40, by = 30, vx = 3, vy = 2;   /* a bouncing ball */

static void draw(UnoWin *w) {
    Rect f;
    short x0 = w->bounds.left + 10, y0 = w->bounds.top + TBAR_H + 8;
    SetRect(&f, x0, y0, x0 + 260, y0 + 160);
    fill_rgb(&f, &kField);                        /* true-colour fill    */
    SetRect(&f, x0 + bx - 9, y0 + by - 9, x0 + bx + 9, y0 + by + 9);
    fill_rgb(&f, &kBall);
}
static void tick(void) {                          /* ~60 times a second  */
    bx += vx; by += vy;
    if (bx < 9 || bx > 251) vx = -vx;
    if (by < 9 || by > 151) vy = -vy;
    { UnoWin *w = find_app_window(APP_RUNNER); if (w) draw_window(w); }
}
static const GameRGB kBall  = { 240, 200, 40, C_WHITE };
static const GameRGB kField = {  10,  40, 120, C_BLUE  };

/* the vtable the desktop drives: draw / key / click / tick / opened / closed */
static const AppInterface kIface = {
    draw, 0, 0, tick, 0, 0, "Bouncer", { 30, 30, 330, 260 }
};
const AppInterface *uno_app_main(const KernelApi *k) { gK = k; return &kIface; }''')

# ---- Python app snippets (defined here so their indentation is preserved) ----
CODE_PY_HELLO = code('''import uno

class Hello(uno.App):
    def draw(self, cv):
        cv.clear(uno.rgb(0, 0, 0))
        cv.text(8, 8, "Hello from Python", uno.rgb(255, 255, 255))

# the runtime looks up `app` and drives its methods for you
app = Hello()''')

CODE_PY_SAMPLE = code('''import uno

class Bouncer(uno.App):
    def build(self, cv):                 # once, as the window opens
        self.w, self.h = cv.width(), cv.height()
        self.x, self.y = 20.0, 24.0      # real floats - UnoC has none
        self.vx, self.vy = 3.2, 2.3
        self.d = 22

    def draw(self, cv):                  # paint one frame
        cv.clear(uno.rgb(16, 18, 34))
        cv.text(8, 8, "Hello from Python", uno.rgb(210, 214, 230))
        cv.fill_rect(int(self.x), int(self.y), self.d, self.d, uno.rgb(255, 96, 96))

    def tick(self):                      # ~60 times a second
        self.x += self.vx; self.y += self.vy
        if self.x < 0 or self.x + self.d > self.w: self.vx = -self.vx
        if self.y < 0 or self.y + self.d > self.h: self.vy = -self.vy

    def key(self, uni, scan, ctrl):      # space = speed the ball up
        if uni == 32:
            self.vx *= 1.25; self.vy *= 1.25
            return True
        return False

app = Bouncer()''')

CODE_PY_STREAM = code('''import uno

class WadInfo(uno.App):
    def build(self, cv):
        # read only the 12-byte header of a multi-MB file, not the whole thing
        n = uno.size("DOOM1.WAD")                 # -1 if it is not there
        head = uno.read_at(0, "DOOM1.WAD", 0, 12) if n > 0 else b""
        self.line = "no WAD" if n < 0 else \\
            "%s  %d lumps  %d bytes" % (head[0:4].decode(), head[4], n)

    def draw(self, cv):
        cv.clear(uno.rgb(0, 0, 0))
        cv.text(8, 8, self.line, uno.rgb(120, 230, 160))

app = WadInfo()''')

CODE_PY_UNO_API = code('''import uno

uno.rgb(r, g, b)                 # pack a colour (0-255 each) -> int

# --- Canvas (passed to build() and draw(); coords are canvas-relative) ---
cv.width(); cv.height()          # your drawable size, in pixels
cv.clear(color)                  # fill the whole canvas
cv.fill_rect(x, y, w, h, color)  # filled / outline rectangle
cv.rect(x, y, w, h, color)
cv.pixel(x, y, color)
cv.hline(x, y, w, color); cv.vline(x, y, h, color)
cv.text(x, y, "string", color)

# --- Sound (the shared UnoSound voice) ---
uno.beep(midi, ticks)            # square-wave note, ticks ~= 1/60 s
uno.quiet()

# --- Files (vol defaults to 0, the boot volume) ---
uno.read(name) -> bytes                 # whole (small) file
uno.read_at(vol, name, off, n) -> bytes # stream a big file a slice at a time
uno.size(name) -> int                   # bytes, or -1 if missing
uno.write(name, data) -> bool           # to a writable volume
uno.mkdir(vol, name) -> bool            # create one directory (parent must exist)

# --- Devices: what hardware is on this machine (read-only) ---
uno.devices() -> str                    # the device tree, one line per PCI function:
                                        #   "bb:dd.f ven:dev cc/ss class driver|UNCLAIMED"
uno.pci() -> list                       # the same, parsed for filtering:
                                        #   [(loc, ven, dev, cls, subcls, progif, driver_or_None), ...]''')

CODE_BUILD = code('''./build.sh                 # build the unoui desktop shell -> build/esp/
./build.sh run             # build, then boot it in QEMU + OVMF
./build.sh legacy          # build the older 14-app "legacy" core
UNO_DEBUG=1 ./build.sh     # the debug/test harness build (adds unoautomate)
UNO_PYRT=0 ./build.sh      # skip PYRT.UNO (no Python runtime; smaller image)
python3 tools/mkuefi.py 512   # pack build/esp/ into build/unodos-uefi.img (512 MiB)
python3 harness.py boot    # scripted QEMU boot + screenshot
python3 nettest.py         # headless network + TLS verification''')

# ---- unoautomate (remote control) snippets ----
CODE_REMOTE_CFG = code('''remote=192.168.2.43:5099''')

CODE_REMOTE_CLI = code('''$ python tools/unoauto_remote.py --listen 0.0.0.0:5099
unoauto_remote listening on 0.0.0.0:5099. Set pc64 DEBUG.CFG:
    remote=<this-machine-ip>:5099   (QEMU SLIRP guest: 10.0.2.2:5099)
[SCRIPT ] remote: link up
[NET    ] eth: DHCP lease 192.168.2.157
probe
  2 0 0 4980736 heap
  2 3 1 12 shell
  1 1 1 0 Files
  ok
apps list
  control Control Panel
  files Files
  browser Browser
  ...
  ok
launch browser
  launched
  ok
py print(6*7)
  42
  ok''')

CODE_REMOTE_PY = code('''from unoauto_remote import UnoAutoLink

link = UnoAutoLink(port=5099)
link.on_log(lambda chan, text: print(chan, text))   # stream the OS log
link.listen()
link.wait_connected()                                # pc64 dials in

print(link.probe())              # [{'kind':2,'name':'heap',...}, ...]
link.launch("browser")           # open an app by its id, never by number
print(link.eval("print(6*7)"))   # run Python on the device -> ['42']

# commands can go the other way too: the device can drive your PC
link.on_command("save", lambda args: "saved " + args)''')

CODE_REMOTE_PUSH = code('''# find which volume is the spare stick B (a writable "kind 2" volume)
python tools/unoauto_remote.py --listen 0.0.0.0:5099      # then type: vols

# push a freshly built kernel to stick B and reboot into it
python tools/unoauto_remote.py \\
    --push 2 "EFI\\BOOT\\BOOTX64.EFI" build/BOOTX64.EFI --reboot''')

CODE_REMOTE_LOOP = code('''from unoauto_remote import UnoAutoLink

link = UnoAutoLink(port=5099)
link.on_log(lambda chan, text: print(f"[{chan}] {text}"))  # watch the OS think
link.listen(); link.wait_connected()                     # stick B dials in

while True:                       # your edit / build / test loop
    input("built a new BOOTX64.EFI? press enter to push it > ")
    print(link.command("probe"))                   # inspect the live system...
    link.push_file(2, r"EFI\\BOOT\\BOOTX64.EFI", "build/BOOTX64.EFI")
    link.reboot()                                  # ...then boot the new build
    link.wait_connected()                          # it dials back in''')

CODE_REMOTE_TEST = code('''link.wait_connected()
report = link.test("network")            # run one built-in conformance suite
for line in report:
    print(line)                          # e.g.  S-NET-10 dhcp lease .......... ok
# probe the live system and assert on it
subs = {r["name"]: r for r in link.probe() if r["kind"] == 2}
assert subs["net"]["state"] == 1, "network subsystem did not come up"''')

# --------------------------------------------------------------------------- pages
# ---- UnoCode snippets (VS Code's own file formats, quoted as they ship) ----
CODE_UNOCODE_THEME = code('''{
    "name": "Nord",
    "type": "dark",
    "colors": {
        "editor.background": "#2E3440",
        "editor.foreground": "#D8DEE9",
        "activityBar.background": "#3B4252",
        "statusBar.background": "#3B4252"
    },
    "tokenColors": [
        { "scope": "comment",
          "settings": { "foreground": "#616E88", "fontStyle": "italic" } },
        { "scope": ["keyword", "storage"],
          "settings": { "foreground": "#81A1C1" } }
    ]
}''')

CODE_UNOCODE_MANIFEST = code('''{
    "name": "nord",
    "displayName": "Nord Theme",
    "description": "An arctic, north-bluish colour theme.",
    "version": "1.0.0",
    "publisher": "you",

    // no "main", so no code ever runs for this extension
    "contributes": {
        "themes": [
            { "label": "Nord", "uiTheme": "vs-dark", "path": "THEMES/NORD.JSN" }
        ]
    }
}''')

CODE_UNOCODE_EXT = code('''var vscode = require('vscode');

function activate(context) {
    vscode.commands.registerCommand('hello.sayHello', function () {
        var ed = vscode.window.activeTextEditor;
        vscode.window.showInformationMessage(
            'Hello from an extension - ' + (ed ? ed.document.fileName : 'no editor'));
    });

    // completions, offered only in C files
    vscode.languages.registerCompletionItemProvider('c', {
        provideCompletionItems: function (document, position) {
            return [{ label: 'uno_fs_read', detail: 'UnoDOS',
                      insertText: 'uno_fs_read(vol, name, buf, max)' }];
        }
    });

    vscode.workspace.onDidSaveTextDocument(function (doc) {
        console.log('saved: ' + doc.fileName);
    });
}

exports.activate = activate;''')

CODE_UNOCODE_SETTINGS = code('''// UnoCode settings.  Comments and trailing commas are allowed.
{
    "editor.fontSize": 15,
    "editor.tabSize": 4,
    "editor.insertSpaces": true,
    "editor.minimap.enabled": true,
    "editor.renderWhitespace": "boundary",
    "workbench.colorTheme": "Nord",
    "files.trimTrailingWhitespace": true,
}''')

PAGES = {}

PAGES["index.html"] = ("Overview", f"""
<div class="hero">
  <h1>UnoDOS <span style="color:var(--accent-2)">pc64</span></h1>
  <p class="lede">A GUI-first operating system that boots on any 64-bit UEFI PC, straight into a themed
  desktop with its own window manager, web browser, networking and built-in apps. No install and no
  command line required. This manual covers the <strong>pc64</strong> version.</p>
  <div>
    <span class="pill">x86-64 · UEFI</span>
    <span class="pill">~14&nbsp;MB image</span>
    <span class="pill">10 themes</span>
    <span class="pill">Keyboard-first</span>
    <span class="pill">QEMU &amp; real-metal verified</span>
  </div>
  <div class="btnrow">
    <a class="btn" href="getting-started.html">Get started &rsaquo;</a>
    <a class="btn ghost" href="#film">Watch the demo</a>
    <a class="btn ghost" href="desktop.html">Tour the desktop</a>
  </div>
</div>

<p>UnoDOS is a family of GUI-first operating systems that runs on more than 20 kinds of hardware.
<strong>pc64</strong> is the modern-PC version: it runs on essentially any x86-64 PC built since about
2007, and it has been tested in emulators and on real hardware booting from a USB stick. The most
recent pass, in August 2026, drove the whole desktop end to end on a ZimaBlade single-board PC and a
Lenovo ThinkPad X13 Yoga: 86 checks, every application launched, and the defects it turned up fixed
and re-checked on the same machines.</p>

<h2 id="film">See it running</h2>
<p>Before you read any further, watch it work. Nothing in this film is a mock-up or an animation: it
is the real system, recorded off a running machine, in one take per scene.</p>

{film("demo-poster.jpg", 'pc64, recorded from the running system in one take per scene: a cold boot, a shipped game, then a level of Doom played by a renderer written in Python. After that the <a href="windows.html">window manager</a> snapping and switching windows, the <a href="appearance.html">ten themes</a> changing live, real Word and Excel documents opening in <a href="office.html">UnoOffice</a>, the <a href="browser.html">browser</a> swapping JavaScript engines mid-session and then loading Wikipedia over HTTPS, music and images decoded by the machine itself, <a href="studio.html">Studio</a> compiling and running the same app twice - once in UnoC and once in Python - and the <a href="ssh.html">SSH client</a> opening a shell on a Linux box across the network. The film streams from the UnoDOS website, so playing it needs a connection.')}

{note('Download <strong>unodos-pc64.iso</strong> and write it to a spare USB stick with Rufus or balenaEtcher (or boot it in a VM) - or use the one-click <strong>USB flasher</strong>. No building required. See <a href="getting-started.html">Getting started</a>.', kind="tip", title="Just want to try it?")}

{fig("desktop.png", "The pc64 desktop in the default <b>Aurora Light</b> theme. The <b>Start</b> button (bottom-left, the UnoDOS brand mark) opens the programs menu; a right-click anywhere on the desktop opens the same menu at the pointer. The rest of the taskbar is your open windows and the clock.")}

<h2 id="what">What you get</h2>
<div class="cards">
  <div class="card"><h4>A real desktop</h4><p>Window manager, taskbar, desktop icons you can arrange, and windows you can move and resize, all by keyboard or pointer. The Start button, a right-click on the desktop, or <kbd>Ctrl</kbd>+<kbd>Esc</kbd> all open the programs menu.</p></div>
  <div class="card"><h4>10 live themes</h4><p>A modern <em>Aurora</em> look (light and dark) plus eight retro skins, switchable live from the Control Panel &mdash; with proportional TrueType text and a real UI-scale setting.</p></div>
  <div class="card"><h4>An office suite</h4><p><a href="office.html">UnoOffice</a>: a word processor, a spreadsheet and a presentation designer that read and write the real <strong>.doc</strong>, <strong>.xls</strong> and <strong>.ppt</strong> formats.</p></div>
  <div class="card"><h4>Applications</h4><p>A WordPad-class rich-text <strong>Editor</strong>, a real <strong>Files</strong> manager with a two-pane mode, System, Clock, a Canvas demo, plus Paint, Photos, Music, a Tracker, UnoAmp, three games and a 3D runner.</p></div>
  <div class="card"><h4>A web browser</h4><p>Shows HTML, Markdown and CSS, runs JavaScript, and loads pages over HTTP and HTTPS.</p></div>
  <div class="card"><h4>A built-in IDE</h4><p><a href="studio.html">Studio</a> lets you write, compile and run your own apps in <strong>UnoC or Python</strong> on the machine itself - a syntax-highlighting editor, a real compiler and an AI assistant, no PC needed.</p></div>
  <div class="card"><h4>Networking</h4><p>Connect over Ethernet, get an address automatically, and browse the web, including secure (HTTPS) sites.</p></div>
  <div class="card"><h4>Hardware support</h4><p>Your screen, keyboard, mouse and trackpad, and USB. Sound plays through the machine's audio hardware (HD&nbsp;Audio or AC'97), and UnoDOS drives SATA, NVMe and eMMC/SD storage with its own drivers.</p></div>
  <div class="card"><h4>Add your own apps</h4><p>Copy a <code>.UNO</code> file into the <code>APPS</code> folder and it becomes an app, with its own icon and menu row - no restart. Rename, hide or pin anything in the list. <a href="apps.html#installing">Installing an app</a>.</p></div>
  <div class="card"><h4>Appliances <span class="pill">preview</span></h4><p>UnoDOS can boot a Linux kernel inside a window and give you a console to type at. It needs hardware virtualisation and a kernel you supply; the limits are on the <a href="appliances.html">Appliances</a> page.</p></div>
</div>

<p class="kv">New here? Start with <a href="getting-started.html">Getting started</a>, then take the
<a href="desktop.html">desktop tour</a>.</p>
""")

PAGES["try-browser.html"] = ("Try it in your browser", f"""
<h1>Try it in your browser</h1>
<p class="lede">The quickest look at UnoDOS costs nothing and installs nothing:
<a href="https://unodos.arinbakht.com/try/">open it in a browser tab</a>. It is the real release
image running on an emulated PC compiled to WebAssembly, not a video and not a mock-up, and the
desktop arrives in a few seconds on a current machine.</p>
<p><a class="btn" href="https://unodos.arinbakht.com/try/"><strong>Boot UnoDOS in your browser</strong></a></p>

<h2 id="what">What you are looking at</h2>
<ul>
  <li>The screen is <strong>1600x900</strong>, one desktop pixel to one screen pixel. The
      <strong>Size</strong> control above it picks the zoom; whole multiples stay pixel-sharp.</li>
  <li>The disk is a RAM disk. You can write to it, and <strong>nothing survives a reload</strong> -
      which makes it a good place to try the things this manual warns about.</li>
  <li>The machine has a working network card and takes an address, but that network
      <strong>ends inside the tab</strong>. See <a href="networking.html#emulator">Networking</a>.</li>
  <li><a href="apps.html#duum">Duum</a> is on the disk with its game data, but an emulated PC runs it
      at well under one frame a second. It is worth seeing rather than playing;
      <a href="https://unodos.arinbakht.com/duum/">the browser port of Duum</a> plays properly.</li>
</ul>
{note('A tab in the background is throttled by the browser, which stops the emulated machine rather than just the picture. The page says <strong>paused</strong> when that happens; bring the tab back to the front and it carries on.', title="Leave the tab in front")}

<h2 id="limits">What the browser cannot show you</h2>
<p>Every instruction is being emulated, so the machine is far slower than the same OS on real
hardware, and anything that leans on the CPU shows it. Sound, the hardware drivers and the true
speed of the desktop are properties of a real PC. When you want those,
<a href="getting-started.html">Getting started</a> puts UnoDOS on a USB stick in a few minutes.</p>
""")

PAGES["getting-started.html"] = ("Getting started", f"""
<h1>Getting started</h1>
<p class="lede">Download the <strong>ISO</strong>, write it to a spare USB stick with a tool you already
know (Rufus, balenaEtcher) or boot it straight in a virtual machine &mdash; or use the one-click
UnoDOS flasher. No building, no command line. (Just want a look first?
<a href="try-browser.html">Try it in your browser</a>, nothing to install.)</p>

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
          Unzip it and open <code>UnoDosFlasher.app</code>; it asks for your
          administrator password (raw disk writes need root). If macOS says it
          cannot verify the developer, allow it once under <em>System Settings
          &rarr; Privacy &amp; Security &rarr; Open Anyway</em>.</li>
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
Secure Boot turned off), or QEMU + OVMF (see <a href="dev-build.html#build">Build &amp; run</a>).</p>

<h2 id="install">Install onto the PC (optional)</h2>
<p>Running from the USB stick is fine forever - but the <strong>Install</strong> app (in the Start
menu) can put UnoDOS on the computer itself, so it boots without the stick:</p>
<ul>
  <li><strong>Onto an existing EFI partition</strong> (the rows marked <em>keeps data</em>): UnoDOS is
      copied into its own folder next to your other operating systems and added to the firmware boot
      menu. <strong>Nothing is deleted</strong> - Windows keeps booting as before, and you can undo it
      by deleting the <code>\\EFI\\UNODOS</code> folder.</li>
  <li><strong>Onto a whole disk</strong> (the <em>Disk</em> rows): the entire disk is erased and
      becomes a UnoDOS disk. This one cannot be done by accident - you have to type the word
      <strong>ERASE</strong> first (below).</li>
</ul>
<p>Pick a target with <kbd>↑</kbd>/<kbd>↓</kbd> and press <kbd>I</kbd> to install
(<kbd>R</kbd> rescans). The system files, fonts, documents and all the apps are copied along.</p>
<p>For a <strong>whole disk</strong>, one more step stands between you and an erased drive:</p>
<ol>
  <li>Press <kbd>C</kbd> to put the caret in the confirm box (<kbd>Esc</kbd> leaves it again).</li>
  <li>Type <strong>ERASE</strong>.</li>
  <li>Press <kbd>I</kbd> to arm, then <kbd>I</kbd> again to commit.</li>
</ol>
{note('Typing <b>ERASE</b> is the only thing that unlocks a whole-disk install, and it applies to the selected row only - change the selection and you type it again. A row listed <b>[too small]</b>, <b>[read-only]</b> or <b>[not 512B/s]</b> cannot be installed to at all. If you change your mind, press <kbd>Esc</kbd> and close the window; nothing is written until the second <kbd>I</kbd>.', kind="warn", title="A whole-disk install erases everything on that disk")}
{fig("install.png", "The <b>Install</b> app. The key line across the top is the whole procedure, and the <b>type ERASE</b> box above the buttons is the gate on a whole-disk install - until that word is typed, <kbd>I</kbd> does nothing. This shot is from the emulator, which offers no disk UnoDOS can install to, so the list is empty; on a real PC each eligible disk and EFI partition appears as a row.", cls="shot-sm")}

<h2 id="firstboot">First boot</h2>
<p>A splash screen with a loading bar appears while UnoDOS starts up, then a short start-up chime
plays as the desktop appears - through the machine's sound hardware (HD&nbsp;Audio or AC'97) on a
modern PC, or the PC speaker on machines that have one. On a laptop the TrackPoint, touchpad and
keyboard all work.</p>
<div class="grid cols-2">
  {fig("splash.png", "The boot splash, with a loading bar and version.")}
  {fig("controlpanel.png", "First desktop paint: the Control Panel opens over the Aurora Light desktop.")}
</div>
<p>From here, everything is a keystroke away. The <strong>Start</strong> button, a right-click on the
desktop, or <kbd>Ctrl</kbd>+<kbd>Esc</kbd> all open the programs menu;
arrow keys and <kbd>Enter</kbd> launch apps; <kbd>Ctrl</kbd>+<kbd>W</kbd> closes a window. The
<a href="desktop.html">desktop tour</a> covers the rest.</p>

<p class="kv">Want to build UnoDOS from source instead of flashing a prebuilt image? That's in the
<a href="developer.html">Developer guide</a>.</p>
""")

PAGES["desktop.html"] = ("The desktop", f"""
<h1>The desktop</h1>
<p class="lede">A themed desktop with a window manager, a two-pane Start menu, a taskbar and a
live clock. Every control works by keyboard or pointer.</p>

{fig("desktop.png", "The desktop: app icons, the <b>Start</b> button (bottom-left), the taskbar with a button per open window, and the clock.")}

<h2 id="furniture">Desktop furniture</h2>
<ul>
  <li><strong>Desktop icons</strong> launch apps directly. Arrange them in columns or rows,
      in launcher order or by name, from the Control Panel - or <strong>drag one where you want it</strong>,
      and it stays there across a restart. So do each app's window size and position, so the desktop you
      arrange is the desktop you come back to.</li>
  <li>The <strong>Start button</strong> at the bottom-left (the UnoDOS brand mark and the word
      <strong>Start</strong>) opens the <strong>Start menu</strong>, which has <strong>two panes</strong>:
      the things you <em>open</em> on the left - every app, in one scrolling list - and the things you
      <em>do to the machine</em> on the right, grouped under <strong>Windows</strong> (Tile, Cascade,
      Minimize all) and <strong>Power</strong> (Restart, Shut Down). A
      <strong>right-click anywhere on the desktop</strong> opens the same menu at the pointer, and
      <kbd>Ctrl</kbd>+<kbd>Esc</kbd> toggles it from the keyboard.</li>
  <li>The <strong>taskbar</strong> along the bottom shows a button for each open window, to the right of the Start button; click one to bring it to the front.</li>
  <li>The <strong>system tray</strong> in the far corner holds, from left to right, a <strong>network chip</strong>
      (see <a href="networking.html#online">Networking</a>), a <strong>battery gauge</strong> on laptops, and the
      <strong>clock</strong>. The clock reads the current time in 24-hour or 12-hour form, and the battery gauge shows
      a percentage, an icon, or both - all chosen in the <a href="appearance.html#datetime">Control Panel</a>.</li>
</ul>

{fig("startmenu.png", "Two panes: apps on the left, machine commands on the right. Open the menu with <kbd>Ctrl</kbd>+<kbd>Esc</kbd>, move with <kbd>&uarr;</kbd>/<kbd>&darr;</kbd>, cross between the panes with <kbd>&larr;</kbd>/<kbd>&rarr;</kbd>, and choose with <kbd>Enter</kbd>.")}

{note("Opening a program and turning the computer off are not the same kind of act, so they are not in the same list. The menu always opens with the highlight on the left, in the apps, and the arrow keys will not wander into <strong>Power</strong> unless you press <kbd>&rarr;</kbd> to go there.", kind="tip", title="Why the menu is split")}

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
<tr><td><kbd>&larr;</kbd> <kbd>&rarr;</kbd> (Start menu)</td><td>Cross between the apps pane and the machine-commands pane</td></tr>
<tr><td><kbd>Enter</kbd></td><td>Activate a button, checkbox or menu item</td></tr>
<tr><td><kbd>Esc</kbd></td><td>Leave a full-screen game (Runner3D)</td></tr>
</tbody>
</table></div>
{note('Because a focused menu changes its value with <kbd>↑</kbd>/<kbd>↓</kbd>, you can switch themes, fonts and resolution entirely from the keyboard. See <a href="appearance.html">Themes &amp; appearance</a>.', kind="tip")}
""")

PAGES["windows.html"] = ("Windows & desktops", f"""
<p class="lede">Windows move, snap, stack and split across four desktops. Everything
here works with the mouse and with the keyboard, and the layout you leave behind
comes back after a restart.</p>

<h2>Moving and sizing</h2>
<p>Drag a window by its title bar and the window itself moves with the pointer -
you see the real thing at the real position, not an outline. Double-click the title
bar to maximize it, and double-click again to put it back. The window's buttons sit
together at the right-hand end of the title bar - <b>minimize</b>, <b>maximize</b>
and then <b>close</b> in the outermost position - and a resizable window also has a
grip in its bottom-right corner.</p>
{note("The retro themes put their close box where the system they imitate put it, which for the Mac-lineage themes is the LEFT of the title bar. The buttons follow the theme, so if you switch skins expect the close box to move.", "", "Where the close box is")}

<h2>Windows move rather than jump</h2>
<p>A window that snaps, un-snaps or maximizes travels to its new shape over about an
eighth of a second instead of teleporting, so you can see where it went. Opening a
window rises it into place, closing it leaves a briefly collapsing outline, the Start
menu slides up out of the taskbar, and the window switcher's highlight slides between
entries rather than jumping. None of it delays anything: the window is already
wherever it is going as far as clicking and typing are concerned.</p>

<h2>Snapping to an edge</h2>
<p>Drag a window against the left or right edge of the screen and a translucent
preview shows the half it will fill; let go and it takes that half. The top edge
maximizes, and the four corners give you quarters. Dragging a snapped window away
from the edge gives it its old size back, so nothing is lost by trying it.</p>
{fig("winsnap.png", "The Editor snapped to the left half of the screen. Keyboard users get the same result with Alt and an arrow key. The four numbered cells beside the Start button are the desktop pager.")}
<p>The keyboard equivalents are <kbd>Alt</kbd>+<kbd>&larr;</kbd> and
<kbd>Alt</kbd>+<kbd>&rarr;</kbd> for the halves, <kbd>Alt</kbd>+<kbd>&uarr;</kbd> to
maximize and <kbd>Alt</kbd>+<kbd>&darr;</kbd> to minimize.</p>

<h2>Switching windows</h2>
<p>Hold <kbd>Alt</kbd> and press <kbd>Tab</kbd> to bring up the switcher, which lists
the windows on the current desktop in the order you last used them. Keep tapping
<kbd>Tab</kbd> to walk the list and let go of <kbd>Alt</kbd> to choose.</p>
{note("Some keyboards never report Alt to the system - a few USB models, and any machine whose firmware does not pass modifier keys through. <kbd>F2</kbd> and <kbd>Ctrl</kbd>+<kbd>Tab</kbd> open the same switcher and commit after a short pause, so the feature is reachable either way.", "tip", "If Alt does nothing")}
{fig("switcher.png", "The switcher, showing the windows on this desktop most-recently-used first. It is scoped to the current desktop on purpose: a switcher that reached across all four would undo the point of having them.")}

<h2>Four desktops</h2>
<p>The four numbered cells beside the Start button are desktops. Click one to switch,
or press <kbd>Ctrl</kbd> and <kbd>F1</kbd> to <kbd>F4</kbd>. A small dot on a cell
means that desktop has windows on it. To send the window you are using to another
desktop and follow it there, press <kbd>Alt</kbd>+<kbd>Ctrl</kbd> and the number.</p>
{fig("desktops.png", "Desktop 2, empty, with the pager showing which desktop is current and which others are occupied. The wallpaper, icons and taskbar are shared; only the windows move.")}
<p>Right-click a window's title bar for a menu with <b>Snap left</b>, <b>Snap
right</b>, <b>To desktop 1-4</b> and the group commands below. Right-clicking blank
space on the taskbar offers <b>Tile</b> and <b>Cascade</b> for everything on the
current desktop.</p>

<h2>Linking windows into a group</h2>
<p>Two windows you always use together can be linked. Give both the same group from
the title-bar menu (<b>Group: A</b> or <b>Group: B</b>) and from then on they move,
raise, minimize and change desktops as one. A small coloured marker on each title bar
shows which group it belongs to. Choose <b>Group: none</b> to break the link.</p>

<h2>Your layout is remembered</h2>
<p>Where each window sits, how big it is, whether it is snapped or minimized, which
desktop it is on and which group it belongs to are all saved, and restored the next
time you start up.</p>
{note("The session is written to a real disk, so it survives a power cycle. On a machine with nothing but the RAM disk - booting from read-only media, say - there is nowhere persistent to write it and the layout will not come back.", "", "Where it is saved")}
""")

PAGES["ssh.html"] = ("SSH client", f"""
<p class="lede">Log in to another computer from UnoDOS, run commands on it and see
the output, using the same SSH the rest of the world uses. Keys and saved
connections live on this machine and survive a restart.</p>

{fig("ssh.png", "The SSH client on first run. The Manage tab holds two panes - your saved connections and your keys - and the + button beside the tabs opens a connection to whichever session is selected. Each connection gets its own tab.")}

<h2>What it can talk to</h2>
<p>Any current OpenSSH server, and anything else that speaks the same modern set:
<code>curve25519-sha256</code> for key exchange, <code>ssh-ed25519</code> host and
user keys, <code>aes256-ctr</code> encryption and <code>hmac-sha2-256</code> for
integrity. It has been tested against OpenSSH 9.5 and 9.6.</p>

<h2>Getting a key</h2>
<p>SSH identifies you by a key rather than a password. You can either make one here
or bring one you already have.</p>
<ol>
  <li>Open <b>SSH</b> from the Start menu or the desktop.</li>
  <li>To make a new key, use the <b>Keys</b> pane. The public half is what you give
      to the other computer.</li>
  <li>To use an existing key, copy its private key file onto a disk UnoDOS can read
      and import it. Keys are listed as <b>(open)</b> or <b>(locked)</b> depending on
      whether they need a passphrase.</li>
</ol>
{note("An OpenSSH private key that was saved <i>with</i> a passphrase cannot be imported yet - that format needs a password-hashing function UnoDOS does not carry. Export a copy with no passphrase if you want to bring it across, and keep that copy somewhere safe.", "warn", "Importing a protected key")}

<h2>Saving a connection</h2>
<p>A saved session records the machine name or address, the port, the user name to log
in as, and which of your keys to use. Select it in the <b>Sessions</b> pane and press
<b>+</b> to connect. A new tab opens with the session running in it; close the tab to
hang up. The <b>Manage</b> tab cannot be closed - it is where you always come back to.</p>

<h2>Host keys and what a warning means</h2>
<p>Every server proves who it is with its own key. The first time you connect to a
machine, UnoDOS records that key and says so. On later visits it checks the key
matches.</p>
{note("If the key has CHANGED, the client refuses to connect and says so in red. That can mean the server was legitimately rebuilt - or that something is impersonating it. Do not work around the warning until you know which; find out from the machine's owner, then remove the stored key and connect again to record the new one.", "warn", "A changed host key")}

<h2>Automation</h2>
<p>The same client is available to the remote-control channel, so a script driving one
UnoDOS machine can log in to others and run commands there. See
<a href="dev-remote.html">Remote control &amp; automation</a>.</p>

<h2>What it does not do yet</h2>
<ul>
  <li>No file transfer (no SFTP or SCP) and no port forwarding.</li>
  <li>The terminal shows plain text. Programs that draw with cursor-control codes -
      a full-screen text editor, for instance - will not display correctly.</li>
  <li>Connecting uses keys that have no passphrase; the app cannot prompt for one yet.</li>
  <li>Ed25519 keys only, for both host and user keys.</li>
</ul>
""")

PAGES["transfer.html"] = ("UnoTransfer", f"""
<h1>UnoTransfer</h1>
<p class="lede">Move files between this machine and another one. Two panes side by side - a UnoDOS
volume on the left, a remote server on the right - and a queue that carries whole folders across
without you watching it.</p>

<p>UnoTransfer is in the Start menu under <strong>Transfer</strong>. It speaks several transfer
protocols through one interface, so copying a folder off a web server and copying it off a Linux box
over SSH are the same three actions with a different address typed in.</p>

<h2 id="panes">The two panes</h2>
<p>The left pane is always a volume on this machine. The right pane is wherever you connected to.
<kbd>Tab</kbd> moves between them, the arrow keys move within one, and <kbd>Enter</kbd> opens a folder.
Choosing a file or a folder and pressing the copy key puts a <strong>job</strong> on the queue that
copies it to the other side, recursively, creating folders as it goes.</p>
<p>The queue is the third part of the window. Each job shows what it is copying, how far along it is
and what went wrong if anything did. Jobs run one file at a time and keep going while you use the
rest of the window.</p>

<h2 id="protocols">What it can connect to</h2>
<table>
  <tr><th>Protocol</th><th>What it is for</th><th>Notes</th></tr>
  <tr><td><b>Local</b></td><td>Copying between two UnoDOS volumes</td><td>Both panes can be local</td></tr>
  <tr><td><b>SCP</b></td><td>Any machine you can reach over SSH</td><td>Uses the same saved sessions and keys as the <a href="ssh.html">SSH client</a></td></tr>
  <tr><td><b>HTTP / HTTPS</b></td><td>Fetching a file by its URL</td><td>Download only, and no folder listing - a web server does not offer one</td></tr>
  <tr><td><b>WebDAV / WebDAVS</b></td><td>A file server that speaks WebDAV</td><td>Lists, uploads, downloads and creates folders</td></tr>
  <tr><td><b>TFTP</b></td><td>Small transfers on a local network, usually to equipment</td><td>No listing and no delete: the protocol has neither</td></tr>
</table>
{note('A protocol that cannot do something says so before you start rather than failing halfway. TFTP has no listing, HTTP has no listing, and SCP cannot resume an interrupted transfer - so those options are simply not offered on those connections instead of being offered and then breaking.', kind="tip", title="Unsupported means unavailable, not broken")}

<h2 id="terminal">The terminal tab</h2>
<p>An SCP connection is an SSH connection, so UnoTransfer also gives you a <strong>terminal</strong> on
the machine you connected to. It is a real VT100/xterm emulator rather than a log window, so a
full-screen program that draws its own interface works in it.</p>

<h2 id="remote">Transferring without touching the machine</h2>
<p>UnoTransfer is also reachable through UnoDOS's remote-control channel as the <code>xfer</code>
command, which is how a script on another computer tells this one to fetch something. The important
part is that the file does <strong>not</strong> travel over the control channel: UnoDOS is told where
the file is and goes and gets it itself, straight from the machine that has it. See
<a href="dev-remote.html">Remote control &amp; automation</a>.</p>

<h2 id="limits">What it does not do yet</h2>
<ul>
  <li><strong>SFTP is not available.</strong> The connection type exists but the layer underneath it
      does not offer it yet, so UnoTransfer offers SCP instead - the same job, over the same SSH
      connection, to the same machine.</li>
  <li><strong>One very large file may not fit.</strong> A file is staged in memory as it arrives,
      because the filesystem underneath can only write a file whole. The staging area is about
      8&nbsp;MB by default and adjustable, and a folder of ordinary files of any total size copies
      fine - it is a single file bigger than the staging area that fails. Streaming is waiting on one
      change further down in the filesystem code, and the app reports which mode it is in.</li>
  <li><strong>No resuming.</strong> An interrupted transfer starts that file again.</li>
  <li><strong>No pasting a web page's address and picking a quality.</strong> UnoTransfer fetches a URL
      that is already a file. It is not a media downloader.</li>
  <li><strong>One window.</strong> Like the browser and the SSH client, it is a single window with a
      single connection rather than tabs.</li>
</ul>
""")

PAGES["appearance.html"] = ("Themes & appearance", f"""
<h1>Themes &amp; appearance</h1>
<p class="lede">Ten live themes, a Dark-mode toggle, procedural wallpapers, TrueType fonts, and real resolution
choice, all from the Control Panel and all without a reboot.</p>

<h2 id="control">The Control Panel</h2>
<p>The Control Panel is where you change how UnoDOS looks and behaves. It opens on first boot and from
the Start menu, and its settings are grouped into <strong>six tabs</strong>:</p>
<div class="tw"><table>
<thead><tr><th>Tab</th><th>What it holds</th></tr></thead>
<tbody>
<tr><td><strong>Display</strong></td><td>Resolution, system-wide Font, UI scale (100&ndash;200%), and an "Aurora lite" switch that turns off live compositing on slower machines.</td></tr>
<tr><td><strong>Personalization</strong></td><td>Theme, Dark mode, Wallpaper, and how the desktop icons arrange themselves.</td></tr>
<tr><td><strong>Network</strong></td><td>The connection summary in the Control Panel's words, and the network self-test behind a Run tests button. See <a href="networking.html#status">Networking</a>.</td></tr>
<tr><td><strong>Audio</strong></td><td>The Volume slider (it adjusts the output live, even mid-note, on HD&nbsp;Audio or AC'97 hardware) and the active output device.</td></tr>
<tr><td><strong>Date &amp; Time</strong></td><td>Set the time and date, and choose a 24-hour or 12-hour clock.</td></tr>
<tr><td><strong>System</strong></td><td>Battery display, session restore, lid-sleep, pointer speed, and buttons for accounts, licences and About.</td></tr>
</tbody>
</table></div>
{fig("controlpanel.png", "The Control Panel's <b>Display</b> tab: resolution, font, UI scale. The tab strip runs across the top; the rest of this page walks through each tab.")}
{note('Everything works by keyboard. <kbd>Tab</kbd> first focuses the tab strip - switch tabs with <kbd>←</kbd>/<kbd>→</kbd> - then a further <kbd>Tab</kbd> steps into the controls on that tab, which you change with <kbd>↑</kbd>/<kbd>↓</kbd>. The desktop re-skins instantly.', kind="tip")}

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

<h2 id="wallpaper">Wallpapers</h2>
<p>The <strong>Personalization</strong> tab holds the Theme and Dark-mode controls, and a
<strong>Wallpaper</strong> menu with seven backdrops. They are drawn by the OS, so there are no image files
to manage: <strong>Theme default</strong> (the theme's own desktop), <strong>Midnight</strong> (a navy sky
with stars), <strong>Sunrise</strong> (a warm wash), <strong>Evergreen</strong>, <strong>Aurora</strong> (soft
accent-coloured glows, tinted by the current theme), <strong>Graphite grid</strong> and <strong>Slate</strong>.
This tab is also where you set how desktop icons arrange themselves - in columns or rows, in launcher order or
by name - and whether they snap to a grid or stay locked in place.</p>
{fig("cp_personalization.png", "The <b>Personalization</b> tab: Theme, Dark mode, Wallpaper and the desktop-icon arrangement, over the Aurora wallpaper.")}

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
<p>Pick a resolution from the <strong>Display</strong> tab and press <strong>Apply</strong>. The desktop
resizes to match, scaled to fill your screen while keeping the correct proportions. Choosing from the
menu on its own changes nothing until you apply it, so you can arrow through the list without the
screen rearranging itself at every step.</p>
<p>A new resolution is then held <strong>on probation</strong> for fifteen seconds: a row appears asking
<em>Keep this resolution?</em> with a countdown, a <strong>Keep</strong> button and <strong>Revert now</strong>.
Do nothing and the desktop goes back by itself.</p>
{fig("resolution.png", "A smaller desktop mode scaled to fit the panel. Apply commits it; the countdown puts it back if you say nothing.")}

{note("The countdown is there for the case where the new mode is unreadable - if you cannot see the screen you cannot click <strong>Keep</strong> either, so waiting is the answer. Just leave it alone for fifteen seconds and you are back where you started.", kind="tip", title="If the screen goes wrong")}

<h2 id="datetime">Date, time, battery and sessions</h2>
<p>The <strong>Date &amp; Time</strong> tab sets the clock: two spinners for the time with a <strong>Set time</strong>
button, a <strong>Set date&hellip;</strong> calendar picker (click a day and it applies), and a
<strong>Clock format</strong> menu that switches the tray clock between 24-hour and 12-hour.</p>
<p>The <strong>System</strong> tab covers the machine's behaviour. <strong>Battery display</strong> chooses whether
the tray shows a percentage, an icon, or both. <strong>Restore last session at startup</strong> (on by default)
reopens the windows you had open - the everyday apps and the Browser - the next time you boot. There is also a
<strong>Lid sleep</strong> switch and a <strong>Pointer speed</strong> slider.</p>
{note('Your choices are saved across a reboot, in a small <code>SHELL.CFG</code> file on the disk you booted from: screen resolution, theme, interface scale, wallpaper, volume, clock format, battery display, pointer speed, lid sleep, the desktop icon options, and the session itself (which windows were open, where they sat, which virtual desktop they were on and which were minimized).', title="What persists across a reboot")}
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
  {fig("system.png", "<b>System</b>: device information in one scrolling list, under headings - Timing, Input &amp; USB, Storage, Network, Power &amp; ACPI and Audio. Storage names the native driver that has taken over (<i>DETACHED (native): ahci0 / nvme0 / emmc0</i>), and Audio names the backend the sound reaches (<i>HD Audio</i>, <i>AC'97</i>, or the PC speaker). Scroll the list to reach the readouts below the fold.")}
  {fig("clock.png", "<b>Clock</b>: an analog face beside a world map showing the day/night terminator, with world times for twenty cities.")}
</div>

<h2 id="creative">Creative tools</h2>
<div class="grid cols-3">
  {fig("paint.png", "<b>Paint</b>: pencil, shapes, fills and a colour palette.")}
  {fig("tracker.png", "<b>Tracker</b>: a 4-channel pattern sequencer.")}
  {fig("music.png", "<b>Music</b>: plays WAV, MIDI and MP3 files from disk, plus the built-in tunes.")}
</div>
<p><strong>UnoAmp</strong> is a second music player in the Winamp 2 mould, for when you want more
than Play and Stop: a playlist, a ten-band graphic equaliser and a visualiser.</p>
{fig("unoamp.png", "<b>UnoAmp</b> as it opens, in its built-in look: a dark chassis with lit displays for the time, the track title and the visualiser. Drop a real Winamp <code>.wsz</code> skin file on the machine and it will wear that instead.")}
<p>The games, Music and Tracker all make sound - through the machine's <strong>HD&nbsp;Audio</strong>
or <strong>AC'97</strong> audio hardware on modern PCs (which have no PC speaker), with the classic
PC-speaker beep as the fallback on machines that still have one. The Control Panel's Volume slider
sets the level.</p>

<h2 id="office">UnoOffice: word processor, spreadsheet, slides</h2>
<p>Three more apps - <strong>UnoWord</strong>, <strong>UnoCalc</strong> and
<strong>UnoShow</strong> - make up <a href="office.html">UnoOffice</a>, and they read and write the
real <code>.doc</code>, <code>.xls</code> and <code>.ppt</code> formats. They have a page of their
own: <a href="office.html">UnoOffice</a>.</p>
<div class="grid cols-3">
  {fig("uoword.png", "<b>UnoWord</b>, the word processor.")}
  {fig("uocalc.png", "<b>UnoCalc</b>, the spreadsheet.")}
  {fig("uoshow.png", "<b>UnoShow</b>, the presentation designer.")}
</div>

<h2 id="photos">Photos: an image viewer</h2>
<p><strong>Photos</strong> opens image files from any disk and steps through the rest of the folder with
<kbd>←</kbd>/<kbd>→</kbd>. It decodes a wide range of formats itself, through the built-in
<em>unomedia</em> library and with no plug-ins: <strong>PNG</strong>, baseline <strong>JPEG</strong>,
animated <strong>GIF</strong>, <strong>BMP</strong>, <strong>TGA</strong>, <strong>QOI</strong>,
<strong>ICO</strong> and <strong>PNM</strong>. Large images are scaled to fit the window, and animated GIFs
play. (Progressive JPEG and WebP are recognised but not decoded.)</p>

<h2 id="games">Games</h2>
<p>The classic games each run in their own window; <strong>Runner3D</strong> takes the whole screen.
<kbd>Esc</kbd> quits it and puts you back on the desktop, at the resolution you were using before -
it does not leave the game running in a window, because it paints straight to the screen rather than
into a window.</p>
<div class="grid cols-2">
  {fig("dostris.png", "<b>Dostris</b>: the falling-block game, with score, lines and level. It plays Korobeiniki underneath and blips when you clear a line.")}
  {fig("pacman.png", "<b>Pac-Man</b>: maze, dots, power pellets and ghosts, with score, high score, lives and level in the panel beside the maze. A siren loops under the play and speeds up while the ghosts are frightened.")}
  {fig("outlast.png", "<b>OutLast</b>: an arcade driving game, with its own theme and a thud when you crash.")}
  {fig("runner3d.png", "<b>Runner3D</b>: a real-time 3D game.")}
</div>
{note('Runner3D draws real-time 3D graphics entirely in software, so it needs no graphics card.', title="3D graphics")}
{note("The games make sound through the machine's own sound hardware, so they need a working sound device - the same one the music player uses. On a machine with no sound device at all they play in silence rather than refusing to start.", title="Sound")}

<h3 id="duum">Duum</h3>
<p><strong>Duum</strong> is a Doom engine, written in Python, running on the machine's own Python
runtime. It plays <strong>Knee-Deep in the Dead</strong> end to end: textured walls, floors and sky,
monsters that see you, chase you and shoot back, weapons, doors, lifts, switches, teleporters,
keycards, pickups, exploding barrels, the status bar along the bottom, and level-to-level progression.
Move with the arrow keys, turn with left and right, strafe with <kbd>,</kbd> and <kbd>.</kbd>,
fire with <kbd>F</kbd>, open doors with <kbd>Space</kbd>, and pick a weapon with <kbd>1</kbd> to
<kbd>6</kbd>. <kbd>Esc</kbd> pauses the game and opens its menu.</p>
  {fig("duum_start.png", "Duum a moment after it opens: the first room of E1M1, drawn from the game file, with the status bar built from the game's own artwork.")}
  {fig("duum_play.png", "The same level after walking forward and turning - textured walls, floors and sky, all rendered by Python code running on the operating system's own runtime.")}

<h4>It sounds like the game, too</h4>
<p>Duum plays the WAD's own audio, with nothing to set up: the effects come out of the same file the
levels do, and so does the music. The effects are mixed <em>on top of</em> the score rather than
instead of it, so firing does not cut the music off. On a machine with no sound hardware at all,
Duum falls back to the single note each event has always made rather than going quiet.</p>

<h4>The pause menu</h4>
<p><kbd>Esc</kbd> pauses and opens a menu: <strong>Resume</strong>, and <strong>Options</strong> with
an FPS counter and a <strong>Controls</strong> screen. Controls lists what every action is bound to
and lets you change it - and the list is the machine's own key table rather than a picture of one, so
it always tells the truth about your keyboard. Arrows move, <kbd>Enter</kbd> chooses, and
<kbd>Esc</kbd> backs out one screen at a time.</p>

<p>Duum is on the desktop and in the <strong>Start</strong> menu under its own reticle icon, the
same as any other app. It also sits on the disk as <code>APPS\\DUUM.UNO</code>, so opening that file
in <a href="#filesapp">Files</a> starts it too.</p>

<h4>Duum needs a game file</h4>
<p>Doom's levels, textures and sounds live in a data file called an <strong>IWAD</strong>, and it is
separate from the engine. UnoDOS ships with <strong>Freedoom</strong>, a free, from-scratch,
Doom-compatible IWAD, so Duum plays out of the box. If you own Doom you can use your own data
instead, and it will look and play like the game you remember.</p>
<ol>
  <li>Find your <code>DOOM1.WAD</code> (or <code>DOOM.WAD</code> from the registered game, or
      <code>freedoom1.wad</code> from <a href="https://freedoom.github.io/">freedoom.github.io</a>).</li>
  <li>Rename it to <strong><code>DOOM1.WAD</code></strong> if it is not called that already.</li>
  <li>Copy it into the <strong>top level</strong> of the UnoDOS disk - the same place the
      <code>APPS</code> folder lives - replacing the one that is there.</li>
  <li>Start Duum. It reads the file straight off the disk as it plays.</li>
</ol>
{note('Duum reads the WAD a piece at a time rather than loading it into memory, so a large one is fine on a small machine. If the file is missing or is not a Doom IWAD, Duum opens and tells you so instead of failing.', title="Big files are fine")}
{note('Game data belongs to whoever made it. Freedoom is included because its licence allows it to be given away with other software; the id Software WADs are yours to use but not ours to ship, so bring your own copy.', kind="warn", title="Whose data is whose")}

<h2 id="studio">Studio: make your own apps</h2>
<p>UnoDOS comes with its own IDE. <strong>Studio</strong> is a code editor, a compiler and an AI
assistant in one window: write an app in <strong>UnoC or Python</strong>, press
<kbd>Ctrl</kbd>+<kbd>B</kbd> to build it and <kbd>Ctrl</kbd>+<kbd>R</kbd> to run it, all on the machine,
with no PC or toolchain. The full story, including the built-in ChatGPT / Claude / Gemini assistant, is on
the <a href="studio.html">Studio</a> page.</p>
{fig("studio.png", "<b>Studio</b>: the built-in IDE - a syntax-highlighting editor, a project list, and a compiler that turns your code into a runnable app right on the machine.")}

<h2 id="unocode">UnoCode: the bigger editor</h2>
<p>Beside Studio there is <strong>UnoCode</strong>, a full workbench in the shape of Visual Studio Code:
an activity bar and side bar, tabbed editors with a minimap, a command palette, an integrated terminal,
and <strong>extensions</strong> - colour themes, languages, syntax grammars and snippets that you drop
into a folder on the disk. Its themes and settings files are Visual Studio Code's own formats, so a theme
written for VS Code works here unchanged. See the <a href="code.html">UnoCode</a> page - or the <a href="unocode.html">developer page</a> if you want to write an extension.</p>
{fig("unocode.png", "<b>UnoCode</b>: the workbench on first run - the activity bar down the left, the Explorer, a tabbed editor with a minimap, and a status bar showing the position, indentation, encoding, line ending and language.")}

<h2 id="appliances">Appliances: another operating system in a window</h2>
<p><strong>Appliances</strong> boots a Linux kernel inside a window on the desktop, with a console you
can type into. It needs hardware virtualisation and a kernel you supply, and it is the newest and
least finished thing here, so it has its own page with the limits spelled out:
<a href="appliances.html">Appliances</a>.</p>

<h2 id="modules">Apps live on the disk</h2>
<p>The games and creative tools are not baked into the system: each one is a small
<code>.UNO</code> file in the <code>APPS</code> folder of the UnoDOS disk, loaded the first time you
open it. Installing UnoDOS onto a PC copies them along automatically. If an app's file is missing,
its window simply says so - nothing crashes. (Studio itself is one of these files, so a build can
include or omit it freely.)</p>

<h2 id="installing">Installing an app</h2>
<p><strong>Copy a <code>.UNO</code> file into the <code>APPS</code> folder and it becomes an app.</strong>
It gets a desktop icon, a row in the Start menu, an entry on the taskbar and a window, and nothing has to
be told about it: the file carries its own name, icon and category, and the desktop reads them.</p>
<p>You do not have to restart. Press <strong>Rescan apps</strong> in the Control Panel and anything new
in the folder appears; the button tells you how many were found. Restarting works too, and finds them
the same way.</p>
{note('An app you install is ordinary software from somewhere else. UnoDOS will run it, and running it is your decision - the same one you make when you install anything on any computer.', kind="warn", title="Where the file came from is up to you")}

<h2 id="arranging">Making the list yours</h2>
<p>Apps are listed the way whoever wrote them suggested, which will not always be what you want. A file
called <code>APPS.CFG</code>, beside the apps themselves, is where you say otherwise. Each line names an
app and one thing to change about it. It sits in the top level of the disk, next to
<code>SHELL.CFG</code>:</p>
<pre><code>name.dostris=Blocks
hide.install=1
pin.uoword=1
cat.paint=3</code></pre>
<ul>
<li><strong>Rename</strong> an app to whatever you call it.</li>
<li><strong>Hide</strong> one you never use - it leaves the desktop and the menu, and is still there if
you change your mind.</li>
<li><strong>Pin</strong> one to the taskbar so it is one click away whether or not it is open.</li>
<li><strong>Recategorise</strong> one: <code>0</code> System, <code>1</code> Network, <code>2</code>
Tools, <code>3</code> Media, <code>4</code> Games, <code>5</code> Other. This matters if you turn on the
Start menu's category headings in the Control Panel; they are off to begin with, and the menu is one
flat list.</li>
</ul>
<p>The name after the dot is the app's own short id, which is what the file it came from calls itself.
<code>name.</code>, <code>short.</code>, <code>hide.</code>, <code>pin.</code>, <code>cat.</code> and
<code>rank.</code> are the whole set.</p>
<p>Where you drag a desktop icon to is remembered as well, along with each app's window size and
position, so the desktop you arrange is the desktop you get back after a restart.</p>
""")

PAGES["office.html"] = ("UnoOffice", f"""
<h1>UnoOffice</h1>
<p class="lede">A word processor, a spreadsheet and a presentation designer, built into UnoDOS.
They read and write the real Office file formats, old and new, so a document written here opens on
a PC.</p>

<p>UnoOffice is three apps - <strong>UnoWord</strong>, <strong>UnoCalc</strong> and
<strong>UnoShow</strong> - sharing one look and one set of habits. If you have used a
mid-1990s office suite you already know where everything is: a menu bar, docked toolbars you can
drag off into floating palettes, a status bar along the bottom, and the same
<kbd>Ctrl</kbd>+<kbd>N</kbd>/<kbd>O</kbd>/<kbd>S</kbd> shortcuts throughout.</p>

<p>Open any of them from the Start menu or its desktop icon. Each is a
<code>.UNO</code> file in the <code>APPS</code> folder, loaded the first time you open it.</p>

<h2 id="unoword">UnoWord</h2>
<p>A word processor with real page layout: it paginates, shows a ruler with indent markers, and
tracks where you are (<i>Page 1 Sec 1, Ln 1 Col 30</i>) in the status bar. Text can be bold, italic
or underlined, in a choice of faces and sizes, and paragraphs can be left, centred, right or
justified.</p>
<div class="grid cols-2">
  {fig("uoword.png", "<b>UnoWord</b> on an empty document: menu bar, the Standard and Formatting toolbars, the ruler, the page itself and the status bar.")}
  {fig("uoword_typed.png", "Typing straight onto the page. The status bar tracks the line and column as you go.")}
</div>

<h2 id="unocalc">UnoCalc</h2>
<p>A spreadsheet with a real calculation engine, not a grid of text. Type numbers into cells, type a
formula beginning with <code>=</code>, and the result appears in the cell while the formula stays
attached to it. Three sheets come with a new workbook, and the status bar keeps a running
<strong>Sum</strong> of the selection.</p>
<div class="grid cols-2">
  {fig("uocalc.png", "<b>UnoCalc</b>: the Name Box and formula bar above the grid, sheet tabs below it.")}
  {fig("uocalc_formula.png", "A3 holds <code>=A1+A2</code>: the cell shows <b>42</b>, the formula bar shows the formula that produced it and the status bar totals the selection. Formulas are stored as formulas, so a saved workbook reopens with the formula intact, not just the number.")}
</div>
<p>Move around with the <strong>arrow keys</strong>, a page at a time with
<kbd>PgUp</kbd>/<kbd>PgDn</kbd>, to the start of the row with <kbd>Home</kbd> and to the far corners
with <kbd>Ctrl</kbd>+<kbd>Home</kbd> / <kbd>Ctrl</kbd>+<kbd>End</kbd>. <kbd>Enter</kbd> commits a cell
and steps down, <kbd>Tab</kbd> commits and steps right; either way what you were typing is kept, so
arrowing out of a half-typed cell stores it rather than throwing it away.</p>

<h2 id="unoshow">UnoShow</h2>
<p>A presentation designer: slides with title and body placeholders, an outline to structure them,
speaker notes, and a full-screen slide show driven from the keyboard.</p>
{fig("uoshow.png", "<b>UnoShow</b> with a new presentation open.")}

<h2 id="files">Real file formats</h2>
<p>The suite reads and writes the real Office formats themselves, through UnoDOS's own
<em>unodoc</em> library. Nothing is converted to a private format on the way in or out, so a file
written on the machine opens on a PC and a file from a PC opens here.</p>
<table>
<tr><th>App</th><th>Office 97</th><th>Modern (OOXML)</th></tr>
<tr><td><strong>UnoWord</strong></td><td><code>.doc</code></td><td><code>.docx</code></td></tr>
<tr><td><strong>UnoCalc</strong></td><td><code>.xls</code></td><td><code>.xlsx</code></td></tr>
<tr><td><strong>UnoShow</strong></td><td><code>.ppt</code></td><td><code>.pptx</code></td></tr>
</table>
<p>You do not choose a format when you start a document, only when you save it: type the extension
you want in the Save dialog, or leave the name bare and let the <strong>Files of type</strong> menu
supply one. Opening works the other way round - the file is identified by its contents rather than
its name, so a <code>.xlsx</code> that somebody renamed <code>.xls</code> still opens correctly.</p>
<p>Save and open through the suite's file dialog onto any writable volume, exactly as the Editor and
Files do. In the Open dialog you can either click a file in the list or type its name in the
<strong>File name</strong> box.</p>
{note('Printing does not exist in UnoDOS yet, so every <strong>Print</strong> menu item is present but does nothing. Save the file and print it from another machine.', kind="warn", title="No printing")}
""")

PAGES["code.html"] = ("UnoCode", f"""
<h1>UnoCode</h1>
<p class="lede">A code editor built into UnoDOS, in the shape of Visual Studio Code: files in tabs,
a folder tree down the side, search across the whole folder, a terminal at the bottom, and
<strong>extensions</strong> - including colour themes taken straight from VS Code.</p>

<p>You do not need to write software to get something out of UnoCode. It is also simply the best
text editor on the machine: it opens any text file, colours it according to what it is, finds and
replaces across a whole folder, and remembers how you like it. If you do write software, everything
on this page still applies and there is a deeper page for you at the end.</p>

{fig("unocode.png", "<b>UnoCode</b> the first time you open it. The strip of icons down the left is the <b>activity bar</b>; it switches the panel beside it between the folder tree, search, source control, run and extensions. The file itself is in the middle, with line numbers on the left and a shrunken map of the whole file on the right. The bar along the bottom tells you where the cursor is, what the file is and how it is saved.")}

<h2 id="open">Opening it</h2>
<p>UnoCode is in the <strong>Start menu</strong> and has a desktop icon. Open it the way you open
anything else; the first time takes a moment while the app is read off the disk.</p>
<p>It opens on a folder rather than a single file. Use the folder tree on the left to move around,
and <kbd>Enter</kbd> to open what is selected. To jump straight to a file by name, press
<kbd>Ctrl</kbd>+<kbd>P</kbd> and start typing.</p>

<h2 id="keys">The five keys worth learning</h2>
<p>Almost everything in UnoCode has a name, and every name is in one searchable list. If you
remember one line from this page, make it the first one.</p>
<table>
  <tr><th>Key</th><th>What it does</th></tr>
  <tr><td><kbd>Ctrl</kbd>+<kbd>Shift</kbd>+<kbd>P</kbd></td><td>The <strong>command palette</strong>: everything UnoCode can do, searchable by name</td></tr>
  <tr><td><kbd>Ctrl</kbd>+<kbd>P</kbd></td><td>Open a file by typing part of its name</td></tr>
  <tr><td><kbd>Ctrl</kbd>+<kbd>S</kbd></td><td>Save</td></tr>
  <tr><td><kbd>Ctrl</kbd>+<kbd>F</kbd></td><td>Find in this file</td></tr>
  <tr><td><kbd>Ctrl</kbd>+<kbd>B</kbd></td><td>Hide the side panel, to give the text the whole window</td></tr>
</table>
<p>The palette is the way out of every "how do I ...?" moment: open it, type roughly what you want,
and read the answers. Each row shows the command's name and, on the right, the key that runs it - so
it teaches you the shortcut while you use it.</p>

{fig("unocode_palette.png", "The command palette with <code>theme</code> typed into it. The grey text is the command's internal name and the right-hand column is its keyboard shortcut, which is how most people end up learning the shortcuts.")}

{note('Some keyboards do not reach every shortcut. UnoDOS receives no F-keys at all from a USB keyboard - that is a limitation of the machine, not of UnoCode - so UnoCode\'s own shortcuts are <kbd>Ctrl</kbd> combinations wherever possible. Anything you cannot press, you can still run from the command palette.', kind="tip", title="If a shortcut does nothing")}

<h2 id="editing">Editing</h2>
<p>Typing works the way you expect. What UnoCode adds is context: it colours the text according to
what kind of file it is, marks the lines you have changed since you opened it, and keeps a map of
the whole file down the right-hand edge so you can see where you are in something long.</p>

{fig("unocode_editor.png", "Two files open in tabs. Comments, names, numbers and text are each coloured differently; the narrow strip on the far right is the entire file drawn in miniature, with the part you are looking at marked; the coloured bar just left of the text marks lines you have changed since opening it.")}

<p>Useful habits, all of them ordinary editor behaviour done properly:</p>
<ul>
  <li><kbd>Ctrl</kbd>+<kbd>Z</kbd> undoes, <kbd>Ctrl</kbd>+<kbd>Y</kbd> redoes. A burst of typing
      undoes as one piece rather than one character at a time.</li>
  <li><kbd>Ctrl</kbd>+<kbd>D</kbd> selects the next copy of the word you are on, so you can rename
      several at once by typing once.</li>
  <li><kbd>Home</kbd> and <kbd>End</kbd> go to the ends of the line, <kbd>Ctrl</kbd>+<kbd>Home</kbd>
      and <kbd>Ctrl</kbd>+<kbd>End</kbd> to the ends of the file.</li>
  <li>The status bar along the bottom shows the line and column, how the file is indented, how it is
      encoded and which line ending it uses. Click nothing: it is there to tell you, and it is the
      first place to look when a file behaves oddly on another machine.</li>
  <li><kbd>Ctrl</kbd>+<kbd>Shift</kbd>+<kbd>O</kbd> lists the things <em>in</em> the file - functions,
      types, headings - and jumps to the one you pick. It is the fastest way around a long file.</li>
  <li>You can <strong>split the editor</strong> and put two files side by side, or the same file twice
      at two different places. Each side scrolls on its own. <b>View: Split Editor</b> in the palette.</li>
</ul>

<h2 id="find">Finding things</h2>
<p><kbd>Ctrl</kbd>+<kbd>F</kbd> searches the file you are in and highlights every match at once, with
a running count. The three small toggles beside the box control whether case matters, whether it
must match a whole word, and whether what you typed is a <em>pattern</em> rather than plain text.</p>

{fig("unocode_find.png", "Find, showing <code>0 of 11</code> matches and the case, whole-word and pattern toggles. Every match in the file is marked as you type, not just the next one.")}

<p>To search the whole folder rather than one file, use the magnifying glass in the activity bar. It
searches into subfolders, a slice at a time so the window stays responsive on a big tree. Results are
grouped by file; <kbd>Enter</kbd> on one opens the file at that line.</p>
<p>You can <strong>replace across every file at once</strong> from the same view. Each file that changes
is one step of undo, so a replacement that went wrong in one file is undone in that file without
disturbing the rest.</p>

<h2 id="themes">Changing the colours</h2>
<p>Open the palette and type <code>theme</code>. UnoCode ships with six colour themes, dark and
light, and switching is instant - the whole window recolours, not just the text.</p>

{fig("unocode_theme.png", "The same file under <b>Nord</b>. The activity bar, side panel, tabs, text and status bar all change together, because a theme describes the whole workbench rather than only the code.")}

<p>You are not limited to the six. UnoCode reads <strong>Visual Studio Code's own theme files</strong>,
so a theme downloaded for VS Code works here unchanged - see the next section for how to put one on
the machine.</p>

<h2 id="extensions">Extensions</h2>
<p>An extension adds something to UnoCode: a colour theme, support for another kind of file, a set of
ready-made snippets, or a new command. The Extensions view - the last icon in the activity bar -
lists what is installed, what each one does and whether it is switched on.</p>

{fig("unocode_extensions.png", "The Extensions view. Each row is one extension, with its name, version and description. <kbd>Enter</kbd> switches one on or off; <kbd>Ctrl</kbd>+<kbd>R</kbd> re-reads them all from disk.")}

<p><strong>Installing one is copying a folder.</strong> There is no store and nothing to sign in to:
an extension is a folder containing a <code>package.json</code> file and whatever it adds, and you
install it by putting that folder in UnoCode's <code>EXT</code> folder and pressing
<kbd>Ctrl</kbd>+<kbd>R</kbd>. Removing it is deleting the folder. Switching one off in the list is
enough if you only want it out of the way for now.</p>

{note('An extension that contains only a theme, a language definition or snippets is just data - it cannot do anything but describe colours and words. An extension that contains <b>code</b> can run commands, and UnoCode runs that code with a hard ceiling on how long it may take, so a badly written extension slows nothing down for long. It still runs on your machine with access to your files, so install code extensions you have reason to trust - the same judgement you would apply anywhere else.', kind="warn", title="What an extension can do")}

<h2 id="terminal">The panel at the bottom</h2>
<p><kbd>Ctrl</kbd>+<kbd>`</kbd> opens a <strong>terminal</strong> underneath the editor. It is not a
Unix shell - UnoDOS does not have one - but it lists and changes folders, reads files, searches, opens
and runs things, and reads and writes UnoCode's own settings. <code>help</code> lists what it
knows.</p>

{fig("unocode_terminal.png", "The terminal after <code>help</code> and <code>ext</code>. It is a quick way to do the things that are fiddly with the mouse, and the fastest way to see which extensions actually loaded.")}

<h2 id="assistant">The assistant</h2>
<p>The last view in the activity bar is an <strong>assistant</strong>: a chat panel beside your code that
reads the file you have open and can propose changes to it. It is <strong>built into the editor</strong>
rather than being an extension, though extensions can reach a model themselves through an interface
UnoCode offers them.</p>
<p><strong>It sends the file you are looking at.</strong> Every question carries the whole of the active
file with it, rebuilt each time you ask, so an answer is about the file as it stands and not as it was
when the conversation started. It is not the folder: the other files are not read, and nothing is sent
that you do not have open. A file too large to fit is cut, and the assistant says it was cut.</p>
<p>Two more things about it are deliberate. Every edit it proposes is shown to you as a
<strong>diff first</strong> and applied only when you accept it, as a single step of undo. And it needs
a key for whichever model service you use, which is stored in the machine's own secret store rather
than in your settings file - <b>AI: Set API Key</b> in the palette, typed into a masked box, and the
store it went into is named on screen when it is saved.</p>
{note('The assistant talks to a service over the internet, which means the text it sends leaves this machine: your question, and the entire contents of the file you have open. The panel names that file every time it sends one, so you can see what went. If that is not appropriate for what you are working on, do not use it - there is nothing to configure to make a network request private.', kind="warn", title="It sends the open file, every time")}

<h2 id="settings">Settings</h2>
<p>Settings live in a text file, and UnoCode edits it like any other file: open the palette and choose
<strong>Preferences: Open Settings</strong>. Every setting is a line of the form
<code>"editor.fontSize": 14</code>, comments are allowed, and anything UnoCode does not recognise is
ignored rather than rejected - so a settings file copied from Visual Studio Code works, with the parts
that do not apply here quietly doing nothing.</p>
<p>Keyboard shortcuts work the same way, in their own file, through
<strong>Preferences: Open Keyboard Shortcuts</strong>.</p>

<h2 id="studio">UnoCode or Studio?</h2>
<p>UnoDOS has two editors and keeps both on purpose. <a href="studio.html">Studio</a> is the small
one: an editor, the built-in compiler and an assistant, three keys from typing something to running
it. UnoCode is the big one: more editor, more searching, and extensible. If you are writing a small
program and want it running now, use Studio. If you are working through a folder full of files, use
UnoCode.</p>

<h2 id="limits">What it does not do</h2>
<ul>
  <li><strong>No printing.</strong> Nothing in UnoDOS prints yet.</li>
  <li><strong>No downloading extensions from inside it.</strong> There is no marketplace; you copy a
      folder onto the disk.</li>
  <li><strong>Not every VS Code extension.</strong> Themes, languages, grammars and snippets are the
      real formats and work as they are. Extensions that contain code are written against UnoCode's
      own smaller set of commands, so a complex VS Code extension will not simply run.</li>
  <li><strong>No debugger.</strong> The Run view launches things; it does not step through them.</li>
</ul>

<p>Writing an extension, or want the file formats and the internals? That is the
<a href="unocode.html">developer page</a>.</p>
""")

PAGES["browser.html"] = ("Web browser", f"""
<h1>Web browser</h1>
<p class="lede">A built-in web browser that shows HTML, Markdown and basic CSS, runs JavaScript, and loads
pages from local disks or over the web (HTTP and HTTPS).</p>

<h2 id="start">Tabs, the toolbar and the start page</h2>
<p>The browser opens on its <strong>start page</strong>: the built-in documents (marked ✳) and every
file on the RAM disk, USB sticks and hard disks, in one scrolling list. <kbd>Enter</kbd> opens the
highlighted document.</p>
<p>Above it are a <strong>tab strip</strong> and a <strong>toolbar</strong>: back and forward, reload,
the start page, the address bar, a bookmark button, and the <strong>Marks</strong> and
<strong>History</strong> panels. Type a web address in the bar and press <kbd>Enter</kbd> to go
there.</p>
{fig("browser_files.png", "The start page: built-in documents and disk files in one scrolling list, under the tab strip and toolbar.")}
<p>Bookmarks are saved on disk, so they are still there after a restart; history is kept for as long as
the machine is running. Both open as a panel under the toolbar, and both are lists you can scroll.</p>
{fig("browser_marks.png", "The <b>Marks</b> panel. The ribbon button on the toolbar turns gold on a page that is bookmarked.")}
<table>
<tr><th>Key</th><th>What it does</th></tr>
<tr><td><kbd>Backspace</kbd></td><td>Back to the previous page</td></tr>
<tr><td><kbd>←</kbd> / <kbd>→</kbd></td><td>Step through the links on the page</td></tr>
<tr><td><kbd>Enter</kbd></td><td>Follow the selected link</td></tr>
<tr><td><kbd>↑</kbd> / <kbd>↓</kbd> / <kbd>PgUp</kbd> / <kbd>PgDn</kbd></td><td>Scroll the page</td></tr>
<tr><td><kbd>Ctrl</kbd>+<kbd>T</kbd></td><td>New tab</td></tr>
<tr><td><kbd>Ctrl</kbd>+<kbd>L</kbd></td><td>Jump to the address bar</td></tr>
<tr><td><kbd>Ctrl</kbd>+<kbd>D</kbd></td><td>Bookmark this page</td></tr>
<tr><td><kbd>Ctrl</kbd>+<kbd>F</kbd></td><td>Find on this page - type to search, <kbd>Enter</kbd> for the next match, <kbd>Esc</kbd> to close</td></tr>
<tr><td><kbd>Ctrl</kbd>+<kbd>S</kbd></td><td>Save this page to disk</td></tr>
<tr><td><kbd>Ctrl</kbd>+<kbd>B</kbd> / <kbd>Ctrl</kbd>+<kbd>H</kbd></td><td>Bookmarks / History panel</td></tr>
<tr><td><kbd>F5</kbd></td><td>Reload</td></tr>
</table>

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
<p>Scripts can also work with the page itself while it is open: finding elements
(<code>getElementById</code>, <code>querySelector</code>), reading and changing their text, attributes and
HTML, creating and removing elements, responding to clicks, and running work later with
<code>setTimeout</code> and <code>setInterval</code>. A change a script makes appears straight away -
the page is laid out again and redrawn.</p>
{fig("browser_js.png", "<b>Script.html</b>: its JavaScript generated this Fibonacci table on the page.")}

<h2 id="engines">Choosing the engines</h2>
<p>The page at <code>uno:engine</code> - reachable from the welcome page, or by typing it in the address
bar - lets you switch <strong>three</strong> things while the browser is running. No rebuild, no
restart: each takes effect on the next page you open.</p>
<ul>
  <li>the <strong>renderer</strong>: the <strong>flow painter</strong>, UnoDOS's own (the default), or
      the <strong>unoweb engine</strong>, which does a full cascade, block layout and display list, and
      handles images and forms;</li>
  <li>the <strong>script engine</strong>: <strong>unojs</strong>, UnoDOS's own (the default), or
      <strong>quickjs</strong>, a vendored full modern JavaScript engine;</li>
  <li>the <strong>CSS cascade</strong>: the <strong>built-in</strong> one (the default), or
      <strong>libcss</strong>, the CSS engine from the NetSurf project.</li>
</ul>
<p>All three default to UnoDOS's own. The alternatives are there so a page that leans on newer
JavaScript or more complete CSS has somewhere to go, and so the two can be compared on the same page -
open a page, switch, reload, and look at the difference.</p>
{note('The cascade choice only changes what you see while the <b>unoweb engine</b> is the renderer: the flow painter does not have a cascade for it to feed. The page says so when that is the case, rather than reporting a setting that is doing nothing.', title="The three are not independent")}

<h2 id="layout">Tables, floats and forms</h2>
<p>Pages that use the ordinary furniture of the web lay out properly: <strong>tables</strong> as real
grids with columns sized to their contents, <strong>floats</strong> with text flowing around them,
<strong>positioned</strong> elements, and <strong>forms</strong> - click a field, type into it, and press
<kbd>Enter</kbd> or the submit button to send it.</p>

<h2 id="net">Over the network</h2>
<p>Type a web address and press <kbd>Enter</kbd>; UnoDOS connects, looks up the site, loads the page and
runs any JavaScript on it. A page's own images and stylesheets are fetched too.</p>
<p>Real sites are bigger than they look. A page whose visible content starts sixty kilobytes into the
file - which is ordinary for a large site - now arrives whole; the browser grows its buffers to the page
rather than stopping at a fixed size. If a page is genuinely too big to hold, the status line says so
instead of quietly showing you part of one.</p>
<p>Three things make that feel quicker than it used to. The page is <strong>drawn as it arrives</strong>
rather than only when the last byte lands, so you start reading immediately. A page and everything it
references travel over <strong>one connection</strong> instead of opening a new one each time. And a page
you have already seen is <strong>remembered</strong> for a short while, so going back to it is instant.
Sites that need you to sign in work too - <strong>cookies</strong> are kept for as long as the browser is
running.</p>
<div class="grid cols-2">
  {fig("browser_http.png", "A live page loaded over <b>HTTP</b>.")}
  {fig("browser_https.png", "A secure page loaded over <b>HTTPS</b>.")}
</div>
{note('Secure (https://) pages load over an encrypted TLS connection, and UnoDOS checks the site certificate against a built-in list of common certificate authorities. See <a href="networking.html">Networking</a>.', title="Secure sites")}
""")

PAGES["networking.html"] = ("Networking", f"""
<h1>Networking</h1>
<p class="lede">UnoDOS can get online by itself: it connects over Ethernet, and over Wi-Fi on Intel adapters,
gets an address automatically, and browses the web, including secure sites.</p>

<h2 id="online">Getting online</h2>
<p>UnoDOS has its own built-in networking. On a PC with a supported wired network adapter, or a supported
Intel Wi-Fi adapter, it gets an address automatically (DHCP), resolves names (DNS), finds the gateway and
other machines, and browses the web over HTTP and HTTPS. Nothing else needs to be installed.</p>
<p>A small <strong>network chip</strong> sits in the taskbar tray so you can see the state at a
glance: it names the medium - <strong>LAN</strong> over a cable, <strong>Wi-Fi</strong> over the air -
with a question mark after it (<strong>LAN?</strong>, <strong>Wi-Fi?</strong>) while the link is up but no
address has arrived yet, and nothing when the link is down. A dot beside it blinks on traffic - amber while
sending, green while receiving. Hovering the chip shows a tooltip: on Wi-Fi it leads with the network's name
and signal, on a cable with the address and link speed.
The <a href="apps.html#native">System</a> app spells it out in full, for example
<code>Network: link up, IP 192.168.2.157</code>.</p>

<h3 id="wired">Wired network adapters</h3>
<p>UnoDOS drives the common Intel Gigabit families directly - the built-in wired port on most PCs and
laptops with an Ethernet socket:</p>
<ul>
  <li><strong>Intel 8254x</strong> (the classic <em>e1000</em> family)</li>
  <li><strong>Intel 82571&ndash;4 / 82574 / 82583</strong> and the <strong>I217 / I218 / I219</strong>
      laptop LOM chips (the <em>e1000e</em> family)</li>
  <li><strong>Intel I210 / I211 / I350</strong> and the 8257x server parts (the <em>igb</em> family)</li>
</ul>
<p><strong>Realtek</strong> wired chips (RTL8168 / 8111 / 8125) are supported too - the port built into most
desktop boards and small-form-factor PCs. This one is <strong>verified on real hardware</strong>: a ZimaBlade
with an onboard RTL8168 links at gigabit, takes a DHCP address and browses, with no adapter plugged in.</p>
{note('The Intel drivers are verified in the emulator against the QEMU e1000 / e1000e / igb models, and the Realtek driver on a real machine. Most test <em>laptops</em> have no wired port, so the built-in NIC on any given laptop may not have been exercised end to end - if a built-in port does not come up, a supported USB Ethernet adapter (below) is the reliable path.', title="How this is verified")}

<h2 id="status">Checking the connection</h2>
<p>The <strong>Network</strong> tab in the <a href="appearance.html#control">Control Panel</a> answers the
question first and keeps the detail for later. On a wired machine it is one line - <em>Ethernet: connected -
1 Gbps</em>, or <em>cable in, no address</em> while DHCP is still at work - and a <strong>Details</strong>
button that opens the addresses underneath: the IP address, the router, and on a machine with Wi-Fi the
adapter's own address, with <strong>Refresh</strong> and <strong>Renew IP</strong> under them.</p>
{fig("cp_network.png", "The Control Panel's Network tab on a wired machine, with Details open: the Ethernet line, then the addresses. The green <b>LAN</b> chip in the tray corner mirrors the same state.")}
<p>On a machine with a Wi-Fi adapter the tab grows. It leads with <em>Connected to</em> and the network's
name, with the signal, WPA2 or WPA3, and the address on the line under it. Then the networks in range, each
with a padlock if it is secured, a four-bar signal meter, and a <em>Connected</em> or <em>Saved</em> marker.
A password box appears only when the highlighted network is locked and new to this machine, and the buttons
are only the ones that can do something at that moment: <strong>Connect</strong>, <strong>Disconnect</strong>,
<strong>Forget</strong>, <strong>Rescan</strong>. The wired line and Details follow underneath.</p>
{fig("cp_wifi.png", "The same tab on a machine with Wi-Fi: the answer, the networks, the controls, then the wired line and Details. Captured in the emulator, which has no Wi-Fi card, with a debug switch that draws the pane and seeds example networks - the pane says so on screen. On a laptop the list is what the radio hears.")}
{note('The <strong>Network</strong> app in the Start menu shows the same summary in the same words, and keeps the network self-test - DHCP, a ping, a small file fetch, a TCP echo and a TLS handshake - behind a <strong>Run tests</strong> button rather than running it every time the window opens. The echo and TLS steps only have a peer under QEMU, so on a real machine those two time out rather than pass; the rest are real.', title="The Network app")}

<h2 id="tls">Secure sites</h2>
<p>Secure (<code>https://</code>) pages load over an encrypted TLS connection, and UnoDOS checks the site's
certificate against a built-in list of common certificate authorities (Let's Encrypt, DigiCert and others),
so you can browse the secure web.</p>

<h2 id="usb">USB Ethernet</h2>
<p>If a PC has no built-in wired network, or its port is not supported, UnoDOS can use a USB Ethernet
adapter instead. This is the most reliable way to get a laptop online, and it is tested on real hardware.
Two chip families are supported - check a listing's chipset line before you buy:</p>
<ul>
  <li><strong>ASIX AX88179 / AX88179A</strong> (USB&nbsp;3.0 gigabit). Verified on real hardware. Common
      products: <strong>Plugable USB3-E1000</strong>, <strong>StarTech USB31000S</strong>,
      <strong>TRENDnet TU3-ETG</strong>, <strong>j5create JUE130</strong>.</li>
  <li><strong>Realtek RTL8152 / 8153 / 8155 / 8156</strong> (up to 2.5&nbsp;Gigabit), including the chips
      built into many <strong>Lenovo, Microsoft Surface, Dell and TP-Link</strong> USB-C docks - for example
      the <strong>TP-Link UE300</strong>. The faster RTL8157 / 8159 (5G/10G) parts are recognised but
      declined.</li>
</ul>
{note('An ASIX AX88179A adapter has been taken all the way to a DHCP lease and a gateway ping on a real laptop (a ThinkPad X13 Yoga). The older ASIX AX88772 / AX88178A chips are <em>not</em> supported.', kind="tip", title="Tested on metal")}

<h2 id="wifi">Wi-Fi</h2>
<p><strong>Wi-Fi works on Intel hardware, on one machine so far.</strong> UnoDOS ships drivers for
Intel (AX201 / AX210), Realtek and Marvell wireless chips. On a Surface Laptop Go with an Intel AX201 the
driver loads the adapter's firmware, scans, <strong>joins a WPA2 network and takes an address</strong> - a
real association followed by a real DHCP lease, on the laptop rather than in the emulator. The Realtek and
Marvell drivers still stop after loading firmware: they do not associate yet. <strong>On anything else, a
wired port or a USB Ethernet adapter is still the sure way to get online.</strong></p>
<p>Joining is done from the Control Panel's <strong>Network</strong> tab, described <a href="#status">above</a>:
pick a network, type its password if it is locked and new to this machine, and press <strong>Connect</strong>.
The password is kept, so the network shows as <em>Saved</em> and the next join asks for nothing;
<strong>Forget</strong> discards it, and <strong>Disconnect</strong> leaves the network while keeping the
radio on, so the list stays live. A machine that boots with a saved network in range is meant to rejoin it
by itself; that boot-time join has been fixed but not yet watched on hardware.</p>
<p>UnoDOS also speaks <strong>WPA3</strong> now, which matters more than it sounds: a modern access
point with protected management frames required will refuse a machine that cannot offer them, and a
6 GHz network is WPA3-only by regulation, so a WPA2-only machine is not "older", it is locked out.
The supplicant reads what the access point is actually offering and negotiates, rather than
announcing WPA2 and hoping - WPA2-PSK or WPA3-SAE, with management-frame protection when it is asked
for, and SAE preferred on a network that offers both.</p>
{note('The WPA3 exchange authenticates against a real access point, but the handshake after it does not complete yet, so on a network that offers both WPA3 and WPA2 UnoDOS falls back to WPA2 and joins. A WPA3-only network cannot be joined today. Two more limits are worth knowing: changing to a different network after a successful join does not work reliably yet, and a reboot is the sure way to do it; and the join itself holds the desktop for the few seconds the association takes, with a spinner and a running description of each step.', kind="warn", title="What does not work yet")}

<h2 id="emulator">Networking in the browser</h2>
<p>UnoDOS <a href="https://unodos.arinbakht.com/try/">running in a browser tab</a> has a working network
card. It takes an address by DHCP, answers a ping, resolves names and loads pages, and everything on this
page behaves the way it does on a real machine. What is different is where the wire goes:
<strong>it ends inside the tab</strong>.</p>
<p>That is not a limitation somebody forgot to lift. A web page cannot open a network connection of the
ordinary kind at all - the browser has no such capability to offer - so every emulator you have seen with
real internet access is relaying its traffic through a server somebody runs and pays for. Rather than
route your traffic through ours, the far side of the wire is a small network living in the same tab:
a router at <code>10.0.2.2</code>, a name server at <code>10.0.2.3</code>, and one web server. Every name
you type resolves to it, so any address reaches a page explaining where you are.</p>
{note('Nothing the emulated machine sends can leave your browser. There is no route out of it in the code - not to your own network, not to this website, not to anywhere else - so the machine is a safe place to point at things. Getting real internet access would mean deliberately adding a relay, which is exactly the decision this arrangement leaves in the open.', title="Nothing leaves the tab")}
<p>On a real PC, or in a virtual machine of your own, none of this applies: the drivers above talk to
real hardware and the machine is on your network like any other.</p>
""")

PAGES["logging.html"] = ("System log", f"""
<h1>System log</h1>
<p class="lede">UnoDOS keeps a record of what it did - what loaded, what failed, what the network was
doing - and shows it to you. You choose how much is kept, and it can be sent to a logging server on your
network.</p>

<h2 id="viewer">Reading the log</h2>
<p>Open <strong>System Log</strong> from the desktop or the Start menu. The newest entries are at the
bottom and the view follows them as they arrive, so you can leave it open and watch. Each line carries
the time, how serious it is, which part of the system wrote it, and the message.</p>
<p><strong>Colour tells you where to look</strong>: errors are red, warnings amber, ordinary lines plain,
and debug lines grey. You should be able to find the interesting line without reading every one.</p>
{fig("logview.png", "The System Log. Three entries the browser wrote while opening documents, above the line the log itself wrote at startup. The footer shows the current level and how many records exist.")}

<h2 id="level">How much is kept</h2>
<p><strong>Less</strong> and <strong>More</strong> change how much the machine records. The scale runs
from <em>emerg</em> (the machine is unusable) down to <em>debug</em> (everything). The default is
<em>notice</em>, which keeps the things worth knowing and leaves out the routine chatter.</p>
<table>
<tr><th>Level</th><th>What it keeps</th></tr>
<tr><td>emerg, alert, crit</td><td>Only serious trouble</td></tr>
<tr><td>err</td><td>... and things that failed</td></tr>
<tr><td>warning</td><td>... and things that nearly failed</td></tr>
<tr><td>notice <em>(default)</em></td><td>... and significant normal events</td></tr>
<tr><td>info</td><td>... and ordinary activity, like each page you open</td></tr>
<tr><td>debug</td><td>Everything</td></tr>
</table>
{note('Turning the level up does not recover what was already dropped - a record filtered out is gone. If you are chasing something intermittent, turn the level up <b>first</b> and then reproduce it.', kind="warn", title="Set the level before you reproduce the problem")}
<p>Your choice is saved to <code>\\LOGS\\LOG.CFG</code> straight away, so it survives a restart.
<strong>All</strong> cycles through the parts of the system, so you can look at just the network or just
the browser. <strong>Write</strong> saves the log to disk immediately.</p>

<h2 id="file">The log on disk</h2>
<p>The log is written to <code>\\LOGS\\SYSTEM.LOG</code> as plain text you can open in the Editor or
copy to another machine. It is written every few seconds, and <strong>immediately</strong> for anything
at error level or worse - so the lines explaining a machine that is about to stop working reach the disk
before it does. When the file gets large it rolls over to <code>SYSTEM.1</code> and starts again.</p>

<h2 id="syslog">Sending the log to a server</h2>
<p>If you run a logging server - <strong>rsyslog</strong>, <strong>syslog-ng</strong> or anything else
that speaks syslog - UnoDOS can send its log there, so a machine's record survives the machine. Put the
server's address in <code>\\LOGS\\LOG.CFG</code>:</p>
<pre><code>level=5
remote_level=4
remote=192.168.1.10:514
listen=0</code></pre>
<p>Entries are sent as standard <strong>RFC 5424</strong> syslog over UDP, so they arrive as ordinary
log lines with the right severity and no special handling at the other end.</p>
<p><code>remote_level</code> is separate from <code>level</code> on purpose: what is worth writing down
locally and what is worth putting on the network are different questions. The default sends warnings and
worse while keeping more than that on disk.</p>

<h2 id="collector">Being the server</h2>
<p>It also works the other way. Set <code>listen=1</code> (or press <strong>Sink off</strong> in the
viewer) and UnoDOS accepts syslog from other machines on the network and files it alongside its own,
tagged with the sender's address. One UnoDOS machine can be the place you read the logs of several.</p>
{note('The listener accepts messages from anyone who can reach the machine - there is no password on it. Turn it on for a network you trust, not one you share. It is off unless you turn it on.', kind="warn", title="An open listener")}
""")

PAGES["appliances.html"] = ("Appliances", f"""
<h1>Appliances</h1>
<p class="lede">UnoDOS can run another operating system inside a window on the desktop. An
<strong>appliance</strong> is a Linux system that UnoDOS boots itself, on the same machine, at the same
time - with a screen you can see, a keyboard it answers, and a connection of its own on your real
network. Chromium runs in one.</p>

{note('This is the newest part of UnoDOS and the least finished. Everything described here works and none of it is simulated - but it runs one appliance at a time, it has only been proven on Intel machines, and the browser appliance is built on a Chromium with a known crash in it. Read <a href="#limits">What it cannot do yet</a> before you plan anything around it.', kind="warn", title="A preview, honestly labelled")}

<h2 id="what">What it actually does</h2>
<p>Open <strong>Appliances</strong> from the Start menu or its desktop icon. It has three views, and
<kbd>Tab</kbd> moves between them: a <strong>list</strong> of the appliances this machine has, the
<strong>console</strong> of the one that is running, and its <strong>display</strong>.</p>
{fig("appliances.png", "<b>Appliances</b> on a machine that has none yet: <b>New</b> makes one, and the line above the buttons is the status - here <i>no appliance running</i>, and on a machine that cannot host a guest at all, the reason why.")}
<p>The console is the guest's serial port. What you type goes in at exactly the place a real keystroke
would arrive, so the guest's own driver wakes up and its own shell reads the byte. Nothing is being
simulated at the top: a Linux shell reads your command and answers it.</p>
{fig("appliance_display.png", "The <b>Display</b> view. Everything inside the black border is the guest drawing on its own screen: UnoDOS hands it a linear framebuffer and describes it in the boot parameters the way firmware would, so a stock Linux kernel drives it with no driver from us. The keyboard is an emulated PS/2 controller, which is why the guest's own input stack sees ordinary key events.")}
<p><kbd>F12</kbd> leaves the display and gives the keyboard back to the desktop.</p>

<h2 id="browser">The browser appliance</h2>
<p>The appliance that makes the point is a small Alpine system running <strong>Chromium</strong> in a
kiosk compositor. It takes its own address from your router, resolves names, validates certificates and
renders live pages - inside a window on the UnoDOS desktop.</p>
<p>You can drive it from the host. <kbd>Ctrl</kbd>+<kbd>L</kbd> focuses the address bar, and what you
type goes to the guest's browser letter by letter through the emulated keyboard.</p>
{fig("appliance_browser.png", "An address typed on the <b>host</b> - in UnoDOS, not in the guest - committed in Chromium's address bar, with the page fetched and rendered under it. The keystrokes crossed into the guest through an emulated PS/2 controller and arrived as ordinary key events; the page came off the internet over the guest's own network connection.")}
{note("The browser appliance is built on Alpine's Chromium, which links against a system copy of a formatting library whose internal checks are left switched on. A value that ordinary Chromium would format and forget can therefore abort the process. It is restarted automatically inside the compositor, so what you see is a browser that occasionally restarts rather than one that dies - but it is a real fault, it is upstream of UnoDOS, and it is not fixed.", kind="warn", title="Chromium restarts sometimes, and why")}

<h2 id="apps">One appliance, different applications</h2>
<p>Which application the appliance runs is a small description file rather than a different build, so the
same kernel and the same machinery run something else by naming it. <strong>GIMP</strong> is the second
one, and it needed nothing from the hypervisor that the browser had not already needed.</p>

<h2 id="requires">What your machine needs</h2>
<p>Hardware virtualisation, which most PCs made since about 2010 have but many ship with turned off. The
status line above the buttons tells you which it is, in one sentence, when you try to start an
appliance. If it is off, turn on <strong>Intel VT-x</strong> or <strong>AMD-V</strong> (sometimes
<em>SVM Mode</em>) in the firmware setup and start again.</p>
<p>Memory is the other requirement. UnoDOS sets aside a block of it for guests while it is starting up,
and how much it can spare depends on the machine. On a machine with too little to set aside there is no
guest at all, and the status line says so.</p>

<h2 id="making">Making an appliance</h2>
<p><strong>New</strong> adds a row, and each row is four things you can edit:</p>
<table>
<tr><th>Field</th><th>What it is</th></tr>
<tr><td>Name</td><td>What you want to call it</td></tr>
<tr><td>Kernel</td><td>A Linux <code>bzImage</code> on a UnoDOS volume</td></tr>
<tr><td>Initrd</td><td>An initial ramdisk, usually a small busybox image. May be empty.</td></tr>
<tr><td>Disk</td><td>A disk image file, which the guest sees as a drive. May be empty.</td></tr>
</table>
<p>Copy the kernel and initrd onto the UnoDOS disk the same way you would copy anything else, then name
them here. <strong>Start</strong> boots it, <strong>Console</strong> switches to its console and
<strong>Stop</strong> shuts it down. A brand-new appliance with its paths left empty boots whatever is
already staged in <code>EFI\\UNODOS\\VM</code>, so it starts rather than failing at you.</p>
<p>Your appliances are kept in <code>EFI\\UNODOS\\VM\\VMS.CFG</code>, one line each, as plain text. It
is deliberately a file you can read and fix in the Editor rather than something only the app
understands, and it holds up to eight appliances.</p>

<h2 id="foreign">Foreign apps, and what works today</h2>
<p>The same machinery is meant to end somewhere specific: you double-click an Android <code>.APK</code>
in Files, an icon appears on the desktop, and opening it opens a window. No launcher, and nothing on
screen that says "Android".</p>
{note('<b>Installing works, and the runtime runs; the two are not joined yet.</b> Installing is real: UnoDOS reads the package, registers it, the icon appears without a reboot, and it is still there after a restart. The Android runtime is real too: the appliance boots, starts the Android container, installs the app it carries and puts it on screen full-size and on the network, from a cold boot with nobody driving it - the two figures below are that boot. What is missing is the channel between the desktop icon and that runtime, so <b>opening an installed foreign app today tells you the runtime is not connected</b>. Treat this section as a description of where it is going, not as a feature to use.', kind="warn", title="Installing works, the runtime runs, opening does not yet")}
{fig("android_launch.png", "The Android appliance, booted with nobody driving it: Firefox full-screen, with no launcher and no status bar in sight.")}
{fig("android_firefox.png", "The same appliance a moment later: a real page over TLS, its address typed on the keyboard UnoDOS presents to the guest.")}
<p>One consequence is worth knowing even at this stage: the package file is <strong>not copied</strong>
onto the UnoDOS disk, because the filesystem underneath writes whole files only and a large package would
have to be held in memory all at once. The installed app remembers where you put the package, so deleting
or unplugging it leaves an app that reports its package is missing.</p>

<h2 id="limits">What it cannot do yet</h2>
<ul>
<li><strong>One appliance at a time.</strong> Start refuses while another is running rather than
quietly replacing it. This is a real limit of how the guest is set up, not an oversight.</li>
<li><strong>Guest disks are read-only.</strong> The guest can mount a disk image and read it; it cannot
write to it. Writing needs a change further down in the filesystem code.</li>
<li><strong>The pointer drifts.</strong> The emulated mouse reports movement rather than position, so
the guest's pointer and yours gradually disagree and a target moves as you reach for it. The keyboard
does not have this problem.</li>
<li><strong>No kernel is included.</strong> UnoDOS does not ship a Linux to run, so a fresh appliance
has nothing to boot until you put a kernel on the disk.</li>
<li><strong>Proven on Intel.</strong> Everything above has been demonstrated on Intel VT-x. The AMD
side is written and builds, but has not yet been seen to run a guest, so on an AMD machine treat this
as untested rather than supported.</li>
<li><strong>The guest gets a slice, not a core.</strong> It runs a short budget of time each frame
alongside the desktop, so it is unhurried by design. It is for a shell, a browser and a service, not
for work you are timing.</li>
<li><strong>The guest has a coarse clock.</strong> It settles on a 10&nbsp;ms timer rather than anything
finer, which is fine for everything above and wrong for anything that measures itself.</li>
</ul>
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
<tr><td><strong>Drivers (tail)</strong></td><td>Intel e1000 / e1000e / igb NICs (plus a Realtek RTL816x driver) and the TCP/IP + TLS stack, xHCI USB with ASIX and Realtek USB Ethernet, native AHCI / NVMe / SDHCI and USB mass storage, HD&nbsp;Audio and AC'97 PCM audio, Intel Wi-Fi (AX201 / AX210: joins a WPA2 network and takes an address, on one laptop so far) and early Realtek / Marvell Wi-Fi (firmware loads, not yet connecting), uno3d 3D, UnoSound, and the TrueType engine.</td></tr>
<tr><td><strong>Device manager (unodevices)</strong></td><td>Enumerates the PCI tree into a registry that reports every device and which driver, if any, claimed it - surfaced on-device through the <code>devices</code> remote verb and <code>uno.devices()</code>/<code>uno.pci()</code>. Read-only introspection today; driver auto-binding is a planned phase. A <a href="dev-remote.html#hwwdt">hardware-watchdog</a> primitive (the PCH TCO) lives alongside it as the remote guard's last-resort backstop.</td></tr>
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

<p class="kv">Next: <a href="dev-apps.html">Writing apps</a>, the worked
<a href="dev-samples.html">sample programs</a>, the <a href="dev-sdk-c.html">UnoC</a> and
<a href="dev-sdk-python.html">Python</a> SDK references, and <a href="dev-build.html">Building &amp;
tooling</a>.</p>
""")

PAGES["studio.html"] = ("Studio: the built-in IDE", f"""
<h1>Studio: the built-in IDE</h1>
<p class="lede">Write, compile and run UnoDOS apps <strong>on the machine itself</strong> - no PC, no
toolchain, no command line. Studio is a code editor, a real compiler and an AI assistant in one window,
and the app it builds opens right next to it. Apps can be written in <strong>UnoC</strong> or in
<strong>Python 3</strong> - both first-class.</p>

{fig("studio.png", "<b>Studio</b>: a monospace code editor with syntax highlighting, a project file list, a menu bar (File / Edit / Build / Run / AI / Help), and a build-output pane. It opens on a bundled bouncing-ball sample.")}

<h2 id="loop">Edit, build, run - all on the machine</h2>
<p>Studio compiles your source straight to a runnable app inside UnoDOS. There is nothing to install and
no separate computer in the loop:</p>
<ol>
  <li>Open <strong>Studio</strong> from the programs menu. It greets you with <code>SAMPLE.PY</code>, a
      small bouncing-ball app in Python (or <code>SAMPLE.C</code>, the UnoC version, if the Python runtime
      is not installed).</li>
  <li>Press <kbd>Ctrl</kbd>+<kbd>B</kbd> to <strong>build</strong>. The output pane reports the result -
      <code>Packed SAMPLE.UNO</code> for a Python file, or <code>Built SAMPLE.UNO code=2303 ...</code> for
      a UnoC file - and the new <code>.UNO</code> file appears in the project list.</li>
  <li>Press <kbd>Ctrl</kbd>+<kbd>R</kbd> to <strong>run</strong> it. Your app opens in its own window with
      its own taskbar button.</li>
</ol>
<div class="grid cols-2">
  {fig("studio_build.png", "After <kbd>Ctrl</kbd>+<kbd>B</kbd>: the build-output pane reports the packed or compiled <code>SAMPLE.UNO</code>, and it joins the project list on the left.")}
  {fig("studio_run.png", "After <kbd>Ctrl</kbd>+<kbd>R</kbd>: <code>SAMPLE.PY</code> runs in its own window with its own taskbar button, drawing and moving a block, while Studio stays open behind it.")}
</div>
<p>If a build fails, the output pane lists each error with its line number; press or click a red error line
and the caret jumps straight to it.</p>

<h2 id="editor">The editor</h2>
<p>The editor is monospace with live <strong>syntax highlighting</strong> - keywords, types, strings,
numbers, comments and preprocessor lines each in their own colour - a line-number gutter, and the usual
editing keys: arrows and <kbd>Home</kbd>/<kbd>End</kbd>/<kbd>PgUp</kbd>/<kbd>PgDn</kbd> to move,
<kbd>Shift</kbd>+movement to select, and <kbd>Ctrl</kbd>+<kbd>X</kbd>/<kbd>C</kbd>/<kbd>V</kbd>/<kbd>A</kbd>
to cut, copy, paste and select all. <kbd>Ctrl</kbd>+<kbd>S</kbd> saves. The <strong>Project</strong> list
on the left shows the source files on your working disk; click one to open it.</p>

<h2 id="languages">Two languages: UnoC and Python</h2>
<p>Studio routes by file extension: a <code>.c</code> file is compiled by the built-in UnoC compiler, a
<code>.py</code> file is packaged for the Python runtime. The editor, project pane, build output and AI
assistant are the same either way. Use <strong>File &rarr; New</strong> and end the name in <code>.c</code>
or <code>.py</code> to pick a language.</p>

<h3 id="unoc">UnoC</h3>
<p><strong>UnoC</strong> is a practical subset of C that the built-in compiler accepts: the integer types
(with <code>long</code> at 4 bytes, matching the system), pointers, arrays, <code>struct</code>/<code>union</code>,
<code>enum</code>, <code>typedef</code> and function pointers, the full C expression and statement set,
constant initializers, and a small preprocessor (<code>#include "X.H"</code>, object-like <code>#define</code>,
<code>#ifdef</code>). Floating point, variadic functions and function-like macros are left out. An app
<code>#include</code>s <code>UNO.H</code> (in the <code>SDK</code> folder on the disk) and defines one entry
point that returns a vtable the desktop drives:</p>
{CODE_UNOC_SAMPLE}
<p>That is the whole shape of a UnoC app: draw into your window, react to keys and ticks, and hand back the
vtable. The <a href="dev-apps.html">Writing apps</a> page and the <a href="dev-api.html">API reference</a>
cover the app model and the calls in depth.</p>

<h3 id="python">Python 3</h3>
<p>The same three-key loop works for <strong>Python</strong>, with real floats, classes and the
<code>math</code> module. A Python app subclasses <code>uno.App</code> and reaches the platform through the
<code>uno</code> module:</p>
{CODE_PY_HELLO}
<p>The interpreter is MicroPython, shipped as the optional module <code>APPS\\PYRT.UNO</code>; the whole story,
the sample, the <code>uno</code> API and the Duum demo are on the <a href="dev-python.html">Python apps</a>
page. The same reference ships inside UnoDOS under <strong>Help</strong> in Studio's menu bar.</p>

<h2 id="ai">The AI assistant</h2>
<p>Studio has a built-in assistant that knows UnoC and the app API, so it can write and fix UnoDOS apps
for you. It connects to <strong>ChatGPT</strong> (OpenAI), <strong>Claude</strong> (Anthropic) or
<strong>Gemini</strong> (Google) over a secure connection. On a roomy desktop it appears as a column on
the right; on the compact default desktop, raise the resolution in the Control Panel to bring it in.</p>
{fig("studio_ai.png", "Studio on a larger desktop, with the <b>assistant</b> column on the right beside the editor and project list. The editor's colouring shows keywords, types and numbers each in their own colour.")}
<p>You supply your own API key from your provider and enter it once in the assistant's input line (each line
is a slash command):</p>
<pre><code>/provider anthropic          (or: openai, gemini)
/model claude-sonnet-4-5     (optional - a sensible default is used)
/key sk-ant-...              (your API key)
/save                        (writes AI.CFG so it persists)</code></pre>
<p>The sensible defaults are <code>gpt-4o-mini</code> (OpenAI), <code>claude-sonnet-4-5</code> (Anthropic) and
<code>gemini-2.0-flash</code> (Gemini); <code>/model</code> overrides them, <code>/clear</code> resets the
conversation. Then type a question and press <kbd>Enter</kbd>. The <strong>AI</strong> menu's
<em>Ask Assistant</em> attaches the file you are editing to your next message, so you can ask "why won't this
build?" with the code included.</p>
{note('Your API key is stored in plain text in <code>AI.CFG</code> on the disk - anyone with the disk can read it, so use a low-value, revocable key rather than your main one. Your questions (and any attached file) are sent to the provider you choose. Secure connections check the site certificate against the built-in trust store and use the machine clock, so a wrong clock will stop them.', kind="warn", title="About your key and privacy")}

<h2 id="ship">Studio is optional</h2>
<p>Studio ships as a single loadable file, <code>APPS\\STUDIO.UNO</code>, on the UnoDOS disk. A build that
does not want the IDE simply leaves the file out - the rest of the system is identical, and Studio just
does not appear in the programs menu.</p>
""")

CODE_GUIDE_FIRSTC = code('''#include "UNO.H"

static void my_draw(UnoWin *w)
{
    short x0 = w->bounds.left + 10;
    short y0 = w->bounds.top + TBAR_H + 10;
    text_at(x0, y0, "hello, UnoDOS", C_WHITE, C_BLUE, false);
}

static const AppInterface kIface = {
    my_draw, 0, 0, 0, 0, 0,
    "Hello", { 40, 40, 260, 140 }
};

const AppInterface *uno_app_main(const KernelApi *k)
{
    gK = k;
    return &kIface;
}''')

CODE_GUIDE_FIRSTPY = code('''import uno

class Hello(uno.App):
    def draw(self, cv):
        cv.clear(uno.rgb(16, 18, 34))
        cv.text(8, 8, "hello, UnoDOS", uno.rgb(214, 218, 232))

app = Hello()''')

CODE_GUIDE_DESC = code('''# on your PC, in pc64/:
python3 tools/mkicon.py myicon.ppm MYAPP.QOI
python3 tools/mkuno.py pyapp MYAPP.PY APPS/MYAPP.UNO MYAPP.DESC

# MYAPP.DESC:
#   id: myapp
#   name: My App
#   icon: file:MYAPP.QOI
#   cat: tools
#   rank: 50''')

PAGES["unocode.html"] = ("UnoCode: the VS Code-class editor", f"""
<h1>UnoCode: the VS Code-class editor</h1>
<p class="lede">A full code workbench that runs <strong>on the machine itself</strong>, in the shape of
Visual Studio Code: an activity bar and side bar, tabbed editors with a minimap, a command palette, an
integrated terminal - and <strong>extensions</strong>. Colour themes, languages, syntax grammars and
snippets are the same file formats Visual Studio Code uses, so a theme or a snippet file written for VS
Code works here unchanged.</p>

{fig("unocode.png", "<b>UnoCode</b> on first run. The activity bar down the left switches the side bar between Explorer, Search, Source Control, Run and Extensions; the editor has a line-number gutter, a change-marked left edge and a minimap; the status bar shows the folder, the problem counts, the cursor position, the indentation, the encoding, the line ending and the language.")}

{note('This page is for people <b>extending</b> UnoCode - the file formats, the extension API and the internals. If you just want to use the editor, the <a href="code.html">UnoCode</a> page is the one you want.', kind="tip", title="Looking for how to use it?")}

{note('UnoCode and <a href="studio.html">Studio</a> are both here on purpose. Studio is the small one - an editor, the built-in compiler and an AI assistant, three keys from source to a running app. UnoCode is the big one - more editor, and extensible. Neither replaces the other, and a distro can ship either, both or neither.', title="Two editors, and why")}

<h2 id="start">The five keys worth learning</h2>
<p>Everything in UnoCode is a <strong>command</strong>, and every command is in one searchable list. If
you remember nothing else, remember the first line:</p>
<table>
  <tr><th>Key</th><th>What it does</th></tr>
  <tr><td><kbd>Ctrl</kbd>+<kbd>Shift</kbd>+<kbd>P</kbd></td><td>The <strong>command palette</strong> - every command, searchable, with its keyboard shortcut beside it</td></tr>
  <tr><td><kbd>Ctrl</kbd>+<kbd>P</kbd></td><td>Go to a file in the open folder</td></tr>
  <tr><td><kbd>Ctrl</kbd>+<kbd>B</kbd></td><td>Show or hide the side bar</td></tr>
  <tr><td><kbd>Ctrl</kbd>+<kbd>`</kbd></td><td>The integrated terminal (type <code>help</code>)</td></tr>
  <tr><td><kbd>Ctrl</kbd>+<kbd>,</kbd></td><td>Open <code>settings.json</code></td></tr>
</table>
{fig("unocode_palette.png", "The command palette, filtered to <code>theme</code>. Each row shows the command's <b>title</b>, its <b>id</b> in grey, and its <b>keyboard shortcut</b> on the right. The second row is <b>contributed by an extension</b> - the palette does not distinguish between a built-in command and one an extension registered, because nothing else in UnoCode does either.")}
<p>If the palette can find it, a key can be bound to it and an extension can call it. That is the whole
design: nothing is wired to a key directly, so anything can be rebound.</p>

<h2 id="editing">The editor</h2>
<p>Open a file from the Explorer, with <kbd>Ctrl</kbd>+<kbd>P</kbd>, or by typing
<code>open SDK\\SAMPLE.C</code> in the terminal. Each file gets a tab; the breadcrumb bar above the text
shows where it came from.</p>
{fig("unocode_editor.png", "A UnoC source file open beside the welcome document. Comments, preprocessor lines, types, numbers and strings are each coloured by the language's grammar; the <b>minimap</b> on the right is the whole file in miniature with the visible region marked; the bar down the left edge of the gutter marks lines changed since the file was opened.")}
<p>The editing keys are the ones you already know - arrows and
<kbd>Home</kbd>/<kbd>End</kbd>/<kbd>PgUp</kbd>/<kbd>PgDn</kbd> to move,
<kbd>Shift</kbd>+movement to select, <kbd>Ctrl</kbd>+<kbd>X</kbd>/<kbd>C</kbd>/<kbd>V</kbd>/<kbd>A</kbd>,
<kbd>Ctrl</kbd>+<kbd>Z</kbd> to undo - plus the ones that make an editor worth using:</p>
<div class="grid cols-2">
  <div class="card"><h4>Multiple cursors</h4><p><kbd>Ctrl</kbd>+<kbd>Alt</kbd>+<kbd>Down</kbd> adds a cursor on the next line, <kbd>Ctrl</kbd>+<kbd>D</kbd> adds one at the next match of what is selected, and everything you type then happens at all of them at once. <kbd>Esc</kbd> collapses back to one.</p></div>
  <div class="card"><h4>Find and replace</h4><p><kbd>Ctrl</kbd>+<kbd>F</kbd> finds, <kbd>Ctrl</kbd>+<kbd>H</kbd> replaces, and the three small toggles switch on case sensitivity, whole-word matching and <strong>regular expressions</strong>. The match count is live.</p></div>
  <div class="card"><h4>Whole lines</h4><p><kbd>Alt</kbd>+<kbd>Up</kbd>/<kbd>Down</kbd> moves the current line, <kbd>Shift</kbd>+<kbd>Alt</kbd>+<kbd>Down</kbd> copies it down, <kbd>Ctrl</kbd>+<kbd>Shift</kbd>+<kbd>K</kbd> deletes it, and <kbd>Ctrl</kbd>+<kbd>/</kbd> comments or uncomments the selection in the language's own comment syntax.</p></div>
  <div class="card"><h4>It closes what you open</h4><p>Type <code>(</code> and you get <code>()</code> with the cursor between them; type the closing one and the cursor steps over it rather than doubling it. <kbd>Enter</kbd> keeps the indentation, and opens a block out onto its own lines.</p></div>
</div>
{fig("unocode_find.png", "Find, with the live match count (<code>0 of 11</code>) and the case / whole-word / regular-expression toggles. Every match in the file is highlighted as you type, not just the next one.")}

<h2 id="suggest">Suggestions</h2>
<p><kbd>Ctrl</kbd>+<kbd>Space</kbd> asks for suggestions, and they appear as you type. They come from four
places at once and are ranked together: the language's keywords, <strong>every distinct word already in
the file</strong> (which is what makes completion useful in a language nothing knows about), snippets, and
anything an extension offers.</p>
{fig("unocode_suggest.png", "The suggestion list in a C file. The rows marked <code>S</code> are <b>snippets</b> contributed by an installed extension - <code>unoapp</code> expands to a skeleton app - and the rows marked <code>k</code> are the language's own keywords. Accept with <kbd>Tab</kbd> or <kbd>Enter</kbd>.")}

<h2 id="terminal">The integrated terminal</h2>
<p><kbd>Ctrl</kbd>+<kbd>`</kbd> opens a panel at the bottom with <strong>Problems</strong>,
<strong>Output</strong> and <strong>Terminal</strong> tabs. The terminal is UnoCode's own small shell -
UnoDOS has no shell process for it to host - so <code>help</code> lists exactly what it has, and a word it
does not know is an error rather than a silent nothing.</p>
{fig("unocode_terminal.png", "The terminal, after <code>help</code> and <code>ext</code>. It can list and change directories, read and copy files, search the folder, open and run files, read and write settings, switch the theme, run any UnoCode command by id, and evaluate a JavaScript expression in the same interpreter the extensions run in.")}
<p>Two of its commands are worth knowing about:</p>
<ul>
  <li><code>run <em>file</em></code> runs a <code>.UNO</code> app, or wraps a <code>.PY</code> file for the
      Python runtime and runs that - the same thing <kbd>F5</kbd> does to the file you are editing.</li>
  <li><code>js <em>expression</em></code> evaluates JavaScript in the extension host itself, which is how
      you find out what an extension is actually doing.</li>
</ul>

<h2 id="themes">Colour themes</h2>
<p>Six themes are built in - Dark+, Light+, Monokai, Solarized Dark, High Contrast and UnoDOS Blue - and
more arrive as extensions. <kbd>Ctrl</kbd>+<kbd>K</kbd> then <kbd>Ctrl</kbd>+<kbd>T</kbd>, or
<strong>Preferences: Color Theme</strong> in the palette, switches between them; your choice is saved.</p>
{fig("unocode_theme.png", "The same file under <b>Nord</b>, a theme that arrives as an extension containing one JSON file and no code at all. The whole workbench recolours - activity bar, side bar, tabs, editor, status bar - because a theme sets semantic colours rather than painting anything itself.")}
<p><strong>A theme file is a Visual Studio Code theme file</strong>, unchanged:</p>
{CODE_UNOCODE_THEME}
<p>Two rules make writing one by hand reasonable. A colour key UnoCode does not know is <strong>ignored</strong>,
so a theme written for a newer editor still loads; and a key you leave out is <strong>worked out</strong> from
the ones you set, so a theme with three colours in it still renders a complete, coherent workbench.</p>

<h2 id="extensions">Extensions</h2>
<p>An extension is a folder. Copy it onto the disk under <code>EXT</code> and UnoCode finds it at startup -
on any drive, so an extension on a USB stick needs no installing.</p>
{fig("unocode_extensions.png", "The Extensions view. Each row shows the name, version and description from the extension's manifest. <b>Nord</b> and <b>UnoDOS Snippets</b> contain no code at all; <b>Hello UnoCode</b> does, and once it has run its row shows how long its activation took. <kbd>Enter</kbd> enables or disables one, <kbd>Ctrl</kbd>+<kbd>R</kbd> reloads them all.")}
<p>Three of the four kinds of extension need no programming whatever - they are JSON files that describe
something:</p>
<div class="grid cols-2">
  <div class="card"><h4>A theme</h4><p>One colour-theme file and a manifest naming it. No code runs, ever.</p></div>
  <div class="card"><h4>A language</h4><p>An id, the file extensions it claims and its comment syntax - and UnoCode knows a new language.</p></div>
  <div class="card"><h4>A grammar</h4><p>A TextMate-style file of patterns that colours that language. Same shape as VS Code's.</p></div>
  <div class="card"><h4>Snippets</h4><p>A map of short prefixes to the text they expand to, offered in the suggestion list.</p></div>
</div>
<p>The manifest is <code>package.json</code>, with VS Code's keys:</p>
{CODE_UNOCODE_MANIFEST}
<p>The fourth kind runs <strong>JavaScript</strong>. An extension with a <code>main</code> gets a real
programming interface - commands, messages, quick picks, the active editor and its text, settings, files,
and events when a document is opened, changed or saved:</p>
{CODE_UNOCODE_EXT}
{fig("unocode_ext_run.png", "That extension's command, run from the palette. It was not loaded until the moment the command was chosen: the manifest declared it, so it was in the palette from startup, and choosing it read the file, ran it and called the handler it registered. The message on the right is the extension talking.")}
{note("An extension that misbehaves cannot take the machine with it. Every call into an extension runs on a <b>step budget</b>; one that does not finish is stopped, reported and switched off. On a system with no preemption that is not a nicety - it is the reason it is safe to run code somebody else wrote at all.", title="A runaway extension is stopped, not fatal")}
<p>One thing about the interface is deliberately different from Visual Studio Code, and it is stated
rather than hidden: <code>require()</code> resolves <code>'vscode'</code> and nothing else, because there
is no package manager to resolve anything else against.</p>
{note("The asynchronous calls in this API return <b>real Promises</b>, and <code>async</code>/<code>await</code> work. That was not true at first - the interpreter had no microtask queue, so the calls returned an object with a <code>.then()</code> on it and the manual said so. It has one now, and <code>await</code> genuinely suspends, so an extension written the way a VS Code extension is written behaves the way it expects.", kind="tip", title="Promises are real now")}

<h2 id="lsp">Language servers</h2>
<p>UnoCode contains a <strong>Language Server Protocol client</strong>: a server per language, spoken to
in JSON-RPC over pipes, feeding diagnostics, completions, hover, go-to-definition, find-references,
rename and format back into the editor. It is configured from settings, one command line per language,
and an empty command switches one off.</p>
{note("<b>None of that runs on pc64.</b> A language server is a separate program and pc64 has no processes to start one in, so the client asks the platform, is told there is nothing, and never starts anything - at no cost, and with the editor's own grammar-derived completions untouched. This section describes a capability that is real in the shared UnoCode codebase and inert on this platform. It becomes live here if and when UnoDOS grows a way to run a child program.", kind="warn", title="Not on pc64, and why")}
<p>Two decisions in the client are worth knowing if you are reading the source. It syncs the
<strong>whole document</strong> after a pause in typing rather than sending incremental edits, because
incremental sync means client and server each keep a copy and one mis-ranged edit makes them diverge
silently. And a server that dies is <strong>restarted</strong> with a widening delay and has its
documents re-opened on the replacement, because a language server exiting is ordinary rather than
exceptional.</p>

<h2 id="settings">Settings and keyboard shortcuts</h2>
<p>Both are files, both are yours to edit, and both are the formats Visual Studio Code uses - including
its habit of allowing comments and trailing commas in them.</p>
<table>
  <tr><th>File</th><th>What it is</th></tr>
  <tr><td><code>UNOCODE\\SETTINGS.JSN</code></td><td>Settings, as a flat map of dotted keys. <kbd>Ctrl</kbd>+<kbd>,</kbd> opens it.</td></tr>
  <tr><td><code>UNOCODE\\KEYBIND.JSN</code></td><td>Keyboard shortcuts. <b>Preferences: Open Keyboard Shortcuts (JSON)</b> writes the whole shipped keymap into it the first time, so changing one binding does not start with guessing what a key is called.</td></tr>
</table>
{CODE_UNOCODE_SETTINGS}
<p>A keybinding is <code>{{ "key", "command", "when" }}</code>. <code>when</code> is a real condition -
<code>editorTextFocus</code>, <code>editorHasSelection</code>, <code>terminalFocus</code> and the rest,
combined with <code>!</code>, <code>&amp;&amp;</code> and <code>||</code> - so the same key can do
different things in different places. A command written with a leading <code>-</code> removes a shipped
binding. Yours win over an extension's, which win over the defaults.</p>
{note('The file names are short because the disk is FAT, which allows eight characters and a three-letter extension. The CONTENTS are not shortened: <code>SETTINGS.JSN</code> is <code>settings.json</code>, key for key.', title="Why the names look truncated")}

<h2 id="limits">What it does not do</h2>
<p>Honestly, so you are not looking for them:</p>
<ul>
  <li><strong>No word wrap.</strong> Long lines scroll sideways.</li>
  <li><strong>No language server runs on this machine</strong>, so there is no hover documentation, no
      go-to-definition and no rename-across-a-project here. UnoCode <em>has</em> a language-server client -
      see <a href="#lsp">Language servers</a> below - but a server is a separate program, and pc64 has no
      way to start one. Completions come from the language's keywords, the words already in the file and
      any snippets, which is what you get.</li>
  <li><strong>Source Control tracks your unsaved edits</strong> against the file as it was opened, and
      marks changed lines in the gutter. There is no repository on this machine and it says so rather
      than pretending.</li>
  <li><strong>Function keys depend on the keyboard.</strong> PS/2 keyboards deliver them; USB keyboards
      do not, in this build. Every default shortcut is a <kbd>Ctrl</kbd> chord for that reason, and
      nothing needs an F-key.</li>
</ul>

<h2 id="developers">For extension authors</h2>
<p>The complete file formats - the manifest keys, the theme colour keys, the grammar model and its one
documented difference from TextMate, the settings table and the whole JavaScript interface - are in
<code>pc64/unocode/UNOCODE.md</code> in the source tree, beside the three sample extensions the disk
ships: a theme, a language with a grammar and snippets, and one with code in it.</p>
""")

PAGES["dev-apps.html"] = ("Writing apps", f"""
<h1>Writing apps</h1>
<p class="lede">Everything between "I have an idea" and "it's in the Start menu", in the order you'll
meet it: pick a language, learn the lifecycle the desktop drives you through, draw, react, keep time,
save, make sound, then package. The per-call contracts live in the
<a href="dev-sdk-c.html">UnoC</a> and <a href="dev-sdk-python.html">Python</a> SDK references, and
complete programs to take apart are on <a href="dev-samples.html">Sample programs</a> - this page is
the map. The second half covers the deeper tiers the built-in apps use.</p>

{note('The friendliest way to write an app is <a href="studio.html">Studio</a>, the built-in IDE: edit, press <kbd>Ctrl</kbd>+<kbd>B</kbd> to compile it to a <code>.UNO</code>, and <kbd>Ctrl</kbd>+<kbd>R</kbd> to run it - no PC or toolchain needed.', kind="tip", title="Start with Studio")}

<h2 id="pick">1. Pick a language</h2>
<div class="tw"><table>
<thead><tr><th></th><th>UnoC</th><th>Python</th></tr></thead>
<tbody>
<tr><td>Runs</td><td>native x86-64, compiled on the device by Studio</td><td>on the PYRT runtime
(MicroPython)</td></tr>
<tr><td>Feels like</td><td>classic C against a Toolbox-style API</td><td>modern Python with a small,
flat <code>uno</code> module</td></tr>
<tr><td>Best for</td><td>games and tools that want every cycle</td><td>almost everything else - less
ceremony, floats, exceptions, big ints</td></tr>
<tr><td>Watch out for</td><td>no floating point, no printf, LLP64 (<code>long</code> is 4 bytes)</td>
<td>no f-strings, no <code>time</code>/<code>random</code>/<code>json</code>, one file per app</td></tr>
</tbody>
</table></div>
<p>Both compile and run entirely on the device: open Studio, write, <b>Ctrl-B</b> builds, <b>Ctrl-R</b>
runs. Errors land in the output pane with a line number; click one and the caret jumps there.</p>

<h2 id="lifecycle">2. The lifecycle: the desktop calls you</h2>
<p>There is no <code>main()</code> and no event loop to write. Your app hands the desktop a small set of
callbacks and the desktop drives them: <b>build/opened</b> once at open, <b>draw</b> on every repaint,
<b>tick</b> ~60 times a second, <b>key</b>/<b>click</b> when input arrives for your window,
<b>closed</b> on the way out. The two smallest complete apps:</p>
{CODE_GUIDE_FIRSTC}
{CODE_GUIDE_FIRSTPY}
<p>Rules that hold in both languages:</p>
<ul>
<li><b>Never block.</b> Every callback runs on the shell's frame loop; a busy-wait freezes the whole
desktop. Structure long work as increments driven from <code>tick()</code>.</li>
<li><b>Draw only in draw.</b> State changes in <code>tick()</code>/<code>key()</code>, pixels in
<code>draw()</code>; ask for a repaint rather than painting eagerly.</li>
<li><b>Closing is not the end.</b> Treat <code>opened()</code> as "again", not "first time", and clean
up - stop your music, free your memory - in <code>closed()</code>, because nobody does it for
you.</li>
</ul>

<h2 id="drawing">3. Drawing</h2>
<p>You draw into your window's content area - below the title bar - through the calls in the references
(<a href="dev-sdk-c.html#drawing">UnoC</a>: rect fills, ovals, lines, <code>text_at</code>;
<a href="dev-sdk-python.html#canvas">Python</a>: the <code>Canvas</code>). Two disciplines carry every
sample:</p>
<ul>
<li><b>Read your size, don't assume it.</b> UnoC: <code>w-&gt;bounds</code> plus <code>TBAR_H</code>;
Python: <code>cv.width()</code>/<code>cv.height()</code>. The shell picks window sizes and the user
picks resolutions and fonts.</li>
<li><b>Repaint on change, not on schedule.</b> UnoC: call <code>repaint_all()</code> from
<code>tick()</code> when something moved. Python: return <code>False</code> from <code>tick()</code> on
idle frames. A static window should cost nothing.</li>
</ul>

<h2 id="input">4. Input</h2>
<p>Keys arrive as events; return true to consume one. Printable characters and special keys are
distinguished for you (UnoC: <code>ch</code> vs <code>code</code>; Python: <code>uni</code> vs
<code>scan</code> - full tables in the references). Some chords never reach you - Ctrl+Esc, Alt+Tab and
friends belong to the shell. For held-key movement in games, Python has <code>uno.keys_down()</code>;
UnoC games time out their own key events.</p>

<h2 id="time">5. Time</h2>
<p>Your <code>tick()</code> callback arrives once per shell frame, nominally 60 a second - <b>the frame
is the platform's timebase, so count your own tick() calls</b>. Do not reach for
<code>TickCount()</code>/<code>uno.ticks()</code> expecting a clock: it is a
<a href="dev-sdk-c.html#tickcount">call counter shared by every app</a>, so it only behaves like frame
pacing if you call it exactly once per tick and nothing else is calling it.
<a href="dev-samples.html#timer">TIMER.C</a> is the worked example of frame counting. The one real
clock an app can read is <code>unoauto.uptime()</code> (milliseconds since boot, available in every
build) - automation scripts pace on it.</p>

<h2 id="files">6. Files and settings</h2>
<p>Volume 0 is the boot volume; names are 8.3. Whole-file read/write plus offset reads for streaming -
the contracts (missing files, read-only volumes, size probes) are in the references, and
<a href="dev-samples.html#todo">TODO.PY</a> shows the polite way to surface a failed save. For small
settings, Python apps have <code>uno.pref_get</code>/<code>pref_set</code>; log through
<code>uno.log()</code> so your traces land in the <a href="logging.html">system log</a> with everyone
else's.</p>

<h2 id="sound">7. Sound</h2>
<p>The square-wave voice (<code>music_note_on</code> / <code>uno.beep</code>) works on every machine.
Sampled effects and MIDI music need a DAC and fail detectably when there is none - always keep the beep
fallback. Looping game music in UnoC is <code>gm_start</code>; stop it in <code>closed()</code>,
because the shell won't.</p>

<h2 id="package">8. Packaging: from the user slot to the Start menu</h2>
<p>What Studio builds runs immediately - <b>the user slot</b> - and that is the whole story for a
personal tool: the <code>.UNO</code> lands next to your source, Files can launch it, one such app is
resident at a time.</p>
<p>To make an <i>installed</i> app - a Start-menu row, a desktop icon, a durable name - the module must
carry an <b>app descriptor</b>, and that is a host-side step today (Studio doesn't write
descriptors):</p>
{CODE_GUIDE_DESC}
<p>Drop the result into <code>APPS\\</code> on the device (Files' Copy, or <code>put</code> over the
<a href="dev-remote.html">remote link</a>) and press Rescan in the Control Panel - no reboot. The
descriptor's <code>id:</code> is your app's durable identity: window geometry, session restore and
<code>launch &lt;id&gt;</code> all key off it. Icons are 32x32 QOI with hard-edged alpha; categories
order the Start menu; the user's <code>APPS.CFG</code> gets the last word over your name and placement.
The full descriptor grammar and limits are in the
<a href="dev-sdk-c.html#descriptor">UnoC SDK reference</a>.</p>

<h2 id="debug">9. When it goes wrong</h2>
<ul>
<li><b>Build errors</b> land in Studio's output pane with file and line; UnoC checks every call you make
against the kernel's export table at build time, so a typo'd function is a compile error, never a
mysterious crash.</li>
<li><b>Python tracebacks</b> (with line numbers) print to the output pane; a raising callback is fenced -
it never takes the desktop down.</li>
<li><b>A <code>draw()</code> that raises paints its own traceback</b> into the app's window, so the
window that would otherwise have been blank tells you what went wrong and which line did it. Drawing
then stops rather than raising sixty times a second; fix the code and run again (<kbd>Ctrl</kbd>+<kbd>R</kbd>)
and it starts clean. The exception line also goes to the system log, which is how a machine you are
not sitting in front of reports it.</li>
<li><b>Studio will not open a binary file as text.</b> The project pane lists <code>.UNO</code> modules
on purpose, so you can see what you just built, but picking one now says so in the output pane and
leaves your open document alone. It used to load the module as mojibake, and saving that back
truncated the module at its first zero byte and destroyed it.</li>
<li><b>print() works</b> in Python (an 8&nbsp;KB buffer shown by Studio); UnoC apps put numbers on
screen with <code>fmt_u</code> or into the system log.</li>
<li>For anything deeper, the <a href="dev-remote.html">remote link</a> streams logs to your PC and can
drive the app with injected input while you watch.</li>
</ul>

<h2 id="limits">10. The numbers</h2>
<div class="tw"><table>
<thead><tr><th>Limit</th><th>Value</th></tr></thead>
<tbody>
<tr><td>Source file (Studio editor)</td><td>192 KB</td></tr>
<tr><td>Built <code>.UNO</code> (Studio)</td><td>256 KB</td></tr>
<tr><td>UnoC user-app slot</td><td>512 KB, one resident app</td></tr>
<tr><td>Python heap</td><td>16 MB</td></tr>
<tr><td>Function parameters/arguments (UnoC)</td><td>8</td></tr>
<tr><td>App descriptor</td><td>1024 bytes; id 15 chars, name 31</td></tr>
<tr><td>Icon</td><td>32x32 QOI, 16 KB file, 12 custom slots system-wide</td></tr>
<tr><td>Installed apps</td><td>48 registry rows</td></tr>
</tbody>
</table></div>

<h2 id="tiers">Beyond the SDK: how the built-ins are made</h2>
<p>Everything above is the classic tier - the SDK Studio compiles for. The built-in apps use two deeper
tiers that today require the PC toolchain (see <a href="dev-build.html">Building &amp; tooling</a>).
There are a few UnoC styles; the native widget app is the normal path for a built-in.</p>

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

<h2 id="register">How an app reaches the desktop</h2>
<p>Each app (the games, Paint, Tracker, Music, Photos, Studio) is a <code>.UNO</code> file in the
<code>APPS</code> folder of the disk, loaded the first time you open it. A <code>.UNO</code> is a small
relocatable code module: the loader (<code>pc64_modload.c</code>) reads it, checks it, places it in memory,
and resolves the kernel functions it calls by name against an export table - so the app carries no kernel
code and the kernel carries no app code. The shell keeps a table of window builders and opens an app's
window on demand:</p>
{CODE_OPEN_APP}
<p>The core shell windows (Control Panel, the Editor, Files) are built by function-pointer builders in
<code>g_build[]</code> (<code>pc64_uui.c</code>); a desktop icon or programs-menu row calls
<code>open_app(index)</code>. Loadable apps are compiled once each with a distinct entry symbol via
<code>-DUNO_APP_SYM=uno_app_main_&lt;name&gt;</code>, then flattened into a <code>.UNO</code> by
<code>tools/mkuno.py</code>. Studio builds exactly this format on the machine itself.</p>

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

PAGES["dev-python.html"] = ("Python apps", f"""
<h1>Writing apps in Python</h1>
<p class="lede">UnoDOS treats <strong>Python 3 as a first-class app language</strong>, right alongside
<a href="dev-apps.html">UnoC</a>. Write real Python - classes, functions, floats, the <code>math</code>
module - in <a href="studio.html">Studio</a>, press <kbd>Ctrl</kbd>+<kbd>B</kbd> to package it and
<kbd>Ctrl</kbd>+<kbd>R</kbd> to run it in its own window, all on the machine.</p>

{note('The interpreter is <strong>MicroPython</strong>, shipped as an optional module <code>APPS\\PYRT.UNO</code> (the Python runtime). If it is not installed, Studio still edits and builds UnoC apps normally; running a <code>.py</code> app reports <code>Python runtime not installed</code>. A build that wants a smaller image can leave <code>PYRT.UNO</code> out.', title="The Python runtime is a module")}

<h2 id="model">The app model</h2>
<p>A Python app is a class that subclasses <code>uno.App</code>, plus a module-global <code>app</code>
holding an instance of it. The runtime finds <code>app</code> and calls its methods for you:</p>
{CODE_PY_HELLO}
<p>Every method is optional (though an app with no <code>draw</code> shows nothing):</p>
<div class="tw"><table>
<thead><tr><th>Method</th><th>When it runs</th><th>Use it for</th></tr></thead>
<tbody>
<tr><td><code>build(self, cv)</code></td><td>once, as the window opens</td><td>set up state; <code>cv.width()</code>/<code>height()</code> are valid here</td></tr>
<tr><td><code>draw(self, cv)</code></td><td>whenever the window repaints</td><td>paint one frame</td></tr>
<tr><td><code>tick(self)</code></td><td>~60 times a second</td><td>advance animation or game state</td></tr>
<tr><td><code>key(self, uni, scan, ctrl)</code></td><td>on a key press</td><td>handle input; return <code>True</code> if you consumed it</td></tr>
<tr><td><code>opened(self)</code> / <code>closed(self)</code></td><td>window shown / closing</td><td>acquire / release resources</td></tr>
</tbody>
</table></div>
<p><code>cv</code> is a <strong>Canvas</strong>; colours come from <code>uno.rgb(r, g, b)</code>. The whole
platform - the framebuffer, sound and files - is one <code>uno.</code> call away.</p>

<h2 id="sample">The sample: a bouncing ball</h2>
<p>Studio greets you with <code>SDK\\SAMPLE.PY</code>, the Python counterpart of the UnoC sample. It shows
the whole shape: <code>build</code> sets the ball's position and velocity, <code>tick</code> moves it and
bounces it off the walls, <code>draw</code> paints the background and the ball, and <code>key</code> speeds
it up on the space bar. Note the real floats - something UnoC does not have.</p>
{CODE_PY_SAMPLE}
<p>Press <kbd>Ctrl</kbd>+<kbd>B</kbd> and the output pane reports <code>Packed SAMPLE.UNO</code>; press
<kbd>Ctrl</kbd>+<kbd>R</kbd> and the ball bounces in its own window.</p>

<h2 id="uno">The <code>uno</code> module</h2>
<p>Everything a Python app reaches on the platform goes through <code>import uno</code>. The full reference
is in <a href="dev-api.html#uno-py">the API reference</a> and in <code>SDK\\uno.pyi</code> (a stub for
editors); here is the whole surface at a glance:</p>
{CODE_PY_UNO_API}

<h3 id="stream">Reading files without loading them whole</h3>
<p><code>uno.read_at(vol, name, off, n)</code> reads a slice of a file at a byte offset, so a Python app can
work with data far larger than memory - a level, a <code>.wav</code>, a multi-megabyte WAD - a piece at a
time. This is the same call the <a href="#duum">Duum</a> engine uses to stream a Doom WAD:</p>
{CODE_PY_STREAM}

<h2 id="build">How "building" a Python app works</h2>
<p>Pressing <kbd>Ctrl</kbd>+<kbd>B</kbd> on a <code>.py</code> file does not translate it to machine code.
Studio wraps your source in a small <code>.UNO</code> container - the same format <code>.c</code> apps
compile to, flagged as a Python app - and writes <code>NAME.UNO</code> beside it. Running it hands that
source to <code>PYRT.UNO</code>, which compiles and executes it with the <code>uno</code> module bound in.
Studio routes purely by extension: a <code>.py</code> file is packaged for the runtime, a <code>.c</code>
file is compiled by the UnoC compiler. Everything else - the editor, the project pane, build output, the
AI assistant - is identical.</p>

<h2 id="declare">Giving a Python app an icon</h2>
<p>A packaged Python app runs, but by default the only way to reach it is to open its
<code>.UNO</code> file in Files. To put it on the desktop and in the Start menu, ship a
<strong>descriptor</strong> beside the source and name it when packaging:</p>
<pre><code>id: myapp
name: My App
icon: file:MYAPP.QOI
cat: tools
rank: 20</code></pre>
<pre><code>python3 tools/mkuno.py pyapp apps/MYAPP.PY APPS/MYAPP.UNO apps/MYAPP.DESC</code></pre>
<p>The shell reads that block off the disk at startup - two sector reads, executing nothing - and the app
gets a row of its own. <code>icon:</code> takes either the name of one of the system emblems or
<code>file:NAME.QOI</code>, a 32x32 image you ship next to the module; the shell has to draw the icon
before it would load a byte of your code, which is why the artwork is a separate file rather than
something the app draws. Duum is packaged exactly this way, and
<code>tools/mkicon.py</code> will author the QOI for you.</p>
{note('The descriptor is a file beside the source rather than a comment inside it, and that is deliberate: an app whose source is generated or vendored from somewhere else would lose a magic comment at the next update. Keeping it separate means the launcher metadata belongs to whoever packages the app, not to whoever wrote the code.', title="Beside the source, not inside it")}

<h2 id="limits">Limits (v1)</h2>
<ul>
  <li>One Python app runs at a time; launching another replaces it.</li>
  <li>No <code>import</code> of other <code>.py</code> files yet - keep an app to a single file (it can be
      large). The standard <code>math</code> module and the built-ins are available.</li>
  <li>The <code>uno</code> module is an app's door to the <em>platform</em> - its window, canvas, sound and
      <code>uno.read</code>/<code>uno.write</code> files. To script the <em>machine</em> (drive the UI, launch
      apps, user-scoped files, and more, all behind a permission gate) a script uses
      <a href="dev-remote.html#unoscript"><code>unoscript</code></a> instead; there is no general
      <code>os</code>/<code>sys</code>.</li>
</ul>

<h2 id="duum">Duum: Doom, in Python</h2>
<p><strong>Duum</strong> (<code>SDK\\DUUM.PY</code>) is a Doom engine written entirely in Python - the proof
that Python is a first-class app language here. It loads a real Doom <strong>IWAD</strong>, parses the map,
and renders a first-person, BSP-traversed view of the level you can walk around, all through the
<code>uno</code> API. It exercises the whole platform at once: file I/O (it streams the multi-MB WAD with
<code>uno.read_at</code>, never loading it whole), heavy compute (the BSP walk and the column renderer), the
framebuffer, keyboard input, and floating-point math.</p>
{film("duum-demo-poster.jpg", "Forty-eight seconds of Duum on the x86-64 build, recorded from the running system: the start room, a walk down the corridor drawn from the WAD, a firefight, and the status bar built from the game's own artwork. The film streams from the UnoDOS website, so playing it needs a connection.", DUUM_MP4)}
{note('Duum needs a Doom-format IWAD on the disk as <code>DOOM1.WAD</code> - none ships with UnoDOS, game data belongs to its makers. Use <strong>Freedoom</strong> (freedoom.github.io, a free BSD-licensed IWAD; rename <code>freedoom1.wad</code> to <code>DOOM1.WAD</code>) or the freely distributable id Software shareware <code>DOOM1.WAD</code>. Put it next to the apps on the boot disk. Without a WAD, Duum opens and says it is missing.', title="Bring your own WAD")}
<p>Walls, floors, ceilings and sky are all texture-mapped from the WAD (perspective-correct, distance-
and orientation-shaded), with sprites for monsters, items and the weapon. It is a complete game rather
than a demo: monster AI, hitscan and projectile weapons, doors, lifts, switches, teleporters, keycards,
pickups, exploding barrels, damage, the real STBAR status bar, and E1M1 through E1M9 in order. Three
inner loops run in C (<code>cv.wall_span</code>, <code>cv.mask_span</code>, <code>cv.flat_span</code> -
one call per rendered column) so the per-pixel work does not go through the interpreter, and input comes
through the live held-key level (<code>uno.keys_down()</code>) with the 60&nbsp;Hz clock from
<code>uno.ticks()</code>. Move with the arrows, strafe with <kbd>,</kbd> and <kbd>.</kbd>, fire with
<kbd>F</kbd>, use with <kbd>Space</kbd>.</p>

<p class="kv">Next: complete programs to take apart on <a href="dev-samples.html">Sample programs</a>,
and every call with its contract in the <a href="dev-sdk-python.html">Python SDK reference</a>.</p>
""")

# ---- sample programs (dev-samples.html) ------------------------------------
# The device samples are quoted verbatim from pc64/sdk/ via sdk_source() so the
# page can never drift from the files the user opens in Studio.
# bench_snapshot.py is PC-side and has no repo home yet, so it rides inline.

CODE_BENCH_SNAPSHOT = code(r'''
#!/usr/bin/env python3
"""bench_snapshot.py - a scheduled health snapshot of a UnoDOS machine.

Run this from your PC's scheduler each night.  It waits for the box to
dial in over the URC link, authenticates, and files one snapshot: a
screenshot, plus a CSV row of uptime, heap, filesystem and network
counters.  A month later you have a folder that answers "when did that
start" for free.

Everything it needs ships with UnoDOS: tools/unoauto_remote.py is the
client library - keep this script next to it, or point PYTHONPATH at it.

On the machine: Control Panel -> Remote control..., tick Watch (and
nothing else - this script only observes), note the 6-digit code, and
choose discover so the box finds this PC by broadcast.  Put the code in
the UNO_PIN environment variable on the PC.  Debug builds skip the code.

Schedule it:
  Windows:  schtasks /Create /TN UnoSnapshot /SC DAILY /ST 23:00 ^
            /TR "py C:\\unodos\\pc64\\tools\\bench_snapshot.py"
  Linux:    0 23 * * *  python3 /home/you/unodos/pc64/tools/bench_snapshot.py
"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from unoauto_remote import UnoAutoLink

PIN = os.environ.get("UNO_PIN", "")
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "snapshots")
CSV = os.path.join(OUT, "health.csv")


def main():
    os.makedirs(OUT, exist_ok=True)
    stamp = time.strftime("%Y%m%d-%H%M%S")

    link = UnoAutoLink(port=5099)
    link.listen()                          # the box dials us
    if not link.wait_connected(timeout=300):
        print("no box dialled in - is it on, and is discover armed?")
        return 1
    link.wait_hello(timeout=30)            # booted and draining its link
    if PIN:                                # production builds authenticate
        link.command("auth", PIN)

    # one CSV row of the numbers that drift
    row = {"uptime_s": link.uptime() // 1000}
    for p in link.probe():                 # kind 2 = subsystem rows
        if p["kind"] == 2 and p["name"] in ("heap", "fs", "net"):
            row[p["name"] + "_v1"] = p["v1"]
            row[p["name"] + "_v2"] = p["v2"]
    new = not os.path.exists(CSV)
    keys = sorted(row)
    with open(CSV, "a") as f:
        if new:
            f.write("stamp," + ",".join(keys) + "\n")
        f.write(stamp + "," + ",".join(str(row[k]) for k in keys) + "\n")

    # and one screenshot, as a portable PPM
    w, h, rgba = link.screen_grab()
    shot = os.path.join(OUT, stamp + ".ppm")
    rgb = bytearray()
    for i in range(0, len(rgba), 4):
        rgb += rgba[i:i + 3]
    with open(shot, "wb") as f:
        f.write(b"P6\n%d %d\n255\n" % (w, h))
        f.write(bytes(rgb))

    link.close()
    print("snapshot %s: %dx%d screen, %s" % (stamp, w, h,
          ", ".join("%s=%s" % (k, row[k]) for k in keys)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
''')

NOTE_BENCH_SCHED = note('Schedule it with the tools your PC already has: '
    '<code>schtasks /Create /TN UnoSnapshot /SC DAILY /ST 23:00 /TR "py ...\\bench_snapshot.py"</code> '
    'on Windows, or <code>0 23 * * * python3 .../bench_snapshot.py</code> in a crontab.',
    title="The scheduler is your PC's")

PAGES["dev-samples.html"] = ("Sample programs", f"""
<h1>Sample programs</h1>
<p class="lede">Complete, working programs to read, run and take apart. Every one of them ships on the
device in <code>SDK\\</code> - open it in <a href="studio.html">Studio</a>, press Ctrl-B to build and
Ctrl-R to run. Each sample earns its place by teaching something the one before it didn't.</p>

<div class="tw"><table>
<thead><tr><th>Sample</th><th>Language</th><th>What it teaches</th></tr></thead>
<tbody>
<tr><td><a href="studio.html">SAMPLE.C</a> / <a href="dev-python.html">SAMPLE.PY</a></td><td>UnoC / Python</td>
<td>The app skeleton: the vtable / the <code>uno.App</code> class, drawing, keys, ticks.</td></tr>
<tr><td><a href="#timer">TIMER.C</a></td><td>UnoC</td><td>Real timekeeping, number formatting without
printf, sound, deliberate repainting.</td></tr>
<tr><td><a href="#life">LIFE.C</a></td><td>UnoC</td><td>Arrays and computation, randomness, pacing a
simulation.</td></tr>
<tr><td><a href="#todo">TODO.PY</a></td><td>Python</td><td>Files that survive reboots, both kinds of key,
the idle idiom.</td></tr>
<tr><td><a href="#chart">CHART.PY</a></td><td>Python</td><td>Parsing a file, scaling to the canvas,
graceful fallbacks.</td></tr>
<tr><td><a href="#goodnite">GOODNITE.PY</a></td><td>Python</td><td>Automation under the permission model -
three calls at three tiers.</td></tr>
<tr><td><a href="#dostris">DOSTRIS.C</a></td><td>UnoC</td><td>A complete shipped game, unchanged - the
big worked example.</td></tr>
<tr><td><a href="#bench">bench_snapshot.py</a></td><td>Python, on your PC</td><td>A genuinely scheduled
task: your PC's scheduler drives the box over the remote link.</td></tr>
</tbody>
</table></div>

<h2 id="timer">TIMER.C - a kitchen timer</h2>
<p>The natural second program after SAMPLE.C, because it forces the three habits a real UnoC app needs.
<b>The frame is the timebase - count your own ticks.</b> <code>tick()</code> arrives once per shell
frame, ~60 a second, and the timer counts those calls; pausing works by simply not counting. It
deliberately does <i>not</i> use <code>TickCount()</code> - on pc64 that is a
<a href="dev-sdk-c.html#tickcount">call counter shared by every app</a>, not a clock.
<b>Repaint deliberately</b>: a tick-driven app calls <code>repaint_all()</code>, and only when the
displayed state changed - once a second here, not sixty times. And <b>numbers without printf</b>:
UnoC has no varargs, so <code>put2()</code> composes the <code>MM:SS</code> readout and
<code>fmt_u()</code> does general unsigned decimal.</p>
<p>Also in there: one-shot notes with <code>music_open_chan()</code> + <code>music_note_on()</code> for
the alarm, and a countdown that rounds <i>up</i> (one remaining tick still reads 0:01) while the
stopwatch rounds down - the small honesty every clock UI owes its user.</p>
{fig("samples_timer.png", "TIMER.C mid-countdown: 4:56 left, the bar draining, and the hint line switched from start to pause. The window title is the app's own <code>win_title</code>.")}
{sdk_source("TIMER.C")}

<h2 id="life">LIFE.C - Conway's Game of Life</h2>
<p>Real computation in UnoC: 2-D <code>unsigned char</code> arrays, a double buffer flipped with one
<code>memcpy</code>, and a wrapping neighbourhood via modular arithmetic. Three details worth stealing:
the simulation is <b>paced</b> (a generation every 6 frames, counted by the app itself - the shell stays
responsive and the motion reads as motion); the draw paints <b>only what is alive</b> over one background
fill; and the board seeds
<b>defensively</b>, stirring the cell coordinates into <code>Random()</code> so even a weak generator
yields a live board. Note <code>opened()</code> can run again after a close and reopen - the board seeds
only once.</p>
{fig("samples_life.png", "LIFE.C after a minute: 66 generations in, 209 cells alive. Cells that have survived a while cool from green to blue.")}
{sdk_source("LIFE.C")}

<h2 id="todo">TODO.PY - a to-do list that survives reboots</h2>
<p>Persistence is two calls: <code>uno.read()</code> returns <code>None</code> if the file doesn't exist
(a fine first run), and <code>uno.write()</code> returns <code>False</code> on a read-only volume - the
app <i>shows</i> that state instead of hiding it. The format is plain text (<code>[x] task</code> /
<code>[ ] task</code>), so Notepad can edit the same file. The <code>key()</code> handler routes both
kinds of key - printable characters arrive in <code>uni</code>, special keys in <code>scan</code> - and
<code>tick()</code> returns <code>False</code> whenever nothing changed, so a static window costs the
machine nothing.</p>
{fig("samples_todo.png", "TODO.PY with two tasks typed in and one checked off - and the green line confirming the list reached the disk.")}
{sdk_source("TODO.PY")}

<h2 id="chart">CHART.PY - a bar chart from a file</h2>
<p>A small, genuinely useful tool: drop a <code>DATA.CSV</code> on the boot volume - one
<code>label,value</code> per line - and it draws the chart, scaled to fit, with the biggest bar called
out. Press R after editing the file in Notepad. It parses without a <code>csv</code> module (there
isn't one): <code>split(",")</code>, <code>strip()</code>, and a <code>try/except ValueError</code>
that makes header lines and junk simply vanish. Everything is scaled from
<code>cv.width()</code>/<code>cv.height()</code> with integer math - never hardcode the canvas size,
the shell picks your window. With no file it charts built-in demo data and says so, so the first launch
teaches you what to do next.</p>
{fig("samples_chart.png", "CHART.PY on a machine with no DATA.CSV: it charts its built-in demo data and says so in the header, rather than showing an empty plot.")}
{sdk_source("CHART.PY")}

<h2 id="goodnite">GOODNITE.PY - an end-of-day automation app</h2>
<p>The bridge from apps to automation: one launch journals what you were working on, then offers to shut
the machine down. It is the <a href="dev-remote.html#unoscript">permission model</a> made concrete -
three calls at three tiers:</p>
<div class="tw"><table>
<thead><tr><th>Call</th><th>Capability</th><th>Tier</th></tr></thead>
<tbody>
<tr><td><code>u.ui.screen()</code> - read the window tree</td><td><code>ui.read</code></td><td>0,
ambient</td></tr>
<tr><td><code>u.fs.write("journal/...")</code> - into your home</td><td><code>fs.user</code></td>
<td>1</td></tr>
<tr><td><code>u.sys.power(0)</code> - shut down</td><td><code>power</code></td><td>2, consent</td></tr>
</tbody>
</table></div>
<p>Two things to take away. <b>Denial is not an error state</b>: denied calls raise <code>OSError</code>
(or <code>u.request()</code> answers <code>False</code>), the app handles both, and the same script runs
everywhere - it just does less on a machine that trusts it less. And <b>the generator/tick pattern is
load-bearing</b>: an injected action lands on the <i>next</i> shell frame, so an automation script yields
between step and check, pacing those yields on <code>unoauto.uptime()</code> - the wall clock in
milliseconds, and the only real clock an app can read. For unattended runs, a signed manifest
(<a href="dev-remote.html#automation">.MFT sidecar</a>) grants the declared capabilities at launch, no
prompts.</p>
{fig("samples_goodnite.png", "GOODNITE.PY with nobody signed in: the tier-0 window read works, and the two capabilities it was not granted are reported rather than raised.")}
{sdk_source("GOODNITE.PY")}

<h2 id="bench">bench_snapshot.py - a genuinely scheduled task</h2>
<p>There is no cron on the device - deliberately: pc64 has no preemptive scheduler, and background
scripts are a reserved capability for a facility that doesn't exist yet. The honest scheduled-task story
is <b>your PC schedules, the box answers</b>. Task Scheduler or cron runs this script nightly; it waits
for the box to dial in over the <a href="dev-remote.html">remote link</a>, authenticates with the
Remote-control code, appends one CSV row of uptime/heap/fs/net counters, saves a screenshot, and exits.
A month later the folder answers "when did that start?" for free.</p>
<p>Arm only <b>Watch</b> on the device - the script only observes, so give it only that - and put the
6-digit code in <code>UNO_PIN</code> on the PC. The standing caveat applies: the link is plaintext and
LAN-only by intent; the code proves who may drive the box, it does not encrypt the wire.</p>
{CODE_BENCH_SNAPSHOT}
{NOTE_BENCH_SCHED}

<h2 id="dostris">DOSTRIS.C and the shipped sources</h2>
<p><code>SDK\\DOSTRIS.C</code> is the shipped game, ported to UnoC unchanged - the big worked example for
game state, a piece table in nested initializers, line clearing and a looping <code>gm_start</code>
song. <code>SDK\\DUUM.PY</code> is the whole Doom-style engine in Python, streaming a real WAD with
<code>uno.read_at</code>. Both reward reading with the <a href="dev-sdk-c.html">C</a> and
<a href="dev-sdk-python.html">Python</a> references open beside them.</p>

<p class="kv">Next: <a href="dev-apps.html">Writing apps</a> for the app model these all share, and the
<a href="dev-sdk-c.html">UnoC SDK reference</a> / <a href="dev-sdk-python.html">Python SDK reference</a>
for every call they make.</p>
""")

CODE_SDKC_ENTRY = code('''#include "UNO.H"

static void my_draw(UnoWin *w) { ... }

static const AppInterface kIface = {
    my_draw,   /* draw   - required */
    my_key,    /* key    - or 0     */
    0,         /* click  - or 0     */
    my_tick,   /* tick   - or 0     */
    my_opened, /* opened - or 0     */
    my_closed, /* closed - or 0     */
    "My App",  { 40, 40, 360, 240 } /* title; l,t,r,b (only w x h used) */
};

const AppInterface *uno_app_main(const KernelApi *k)
{
    gK = k;          /* the UNO.H prelude macros expand through this */
    return &kIface;
}''')

CODE_SDKC_ORIGIN = code('''static void my_draw(UnoWin *w)
{
    /* your content area starts below the title bar the shell draws */
    short x0 = w->bounds.left;
    short y0 = w->bounds.top + TBAR_H;
    short cw = w->bounds.right - w->bounds.left;          /* canvas width  */
    short ch = w->bounds.bottom - (w->bounds.top + TBAR_H); /* canvas height */
    ...
}''')

CODE_SDKC_FILL = code('''Rect r;
SetRect(&r, x0, y0, x0 + 100, y0 + 40);   /* half-open: [l,r) x [t,b) */
uno_fill(&r, C_BLUE);                     /* palette fill              */
uno_box(&r, C_WHITE);                     /* 1px outline               */
uno_invert(&r);                           /* XOR the pixels            */''')

CODE_SDKC_FILLRGB = code('''static const GameRGB kGold = { 240, 200, 40, C_WHITE };
/* r, g, b are 0-255; .mono is the palette fallback for 1-bit ports -
 * unused on pc64, but fill it in for portability */
Rect q;
SetRect(&q, x0, y0, x0 + 32, y0 + 32);
fill_rgb(&q, &kGold);''')

CODE_SDKC_TEXT = code('''text_at(x0, y, "Score:", C_CYAN, C_BLUE, false);   /* transparent  */
text_at(x0, y, "MENU",  C_WHITE, C_MAG, true);     /* opaque cell  */
text_at_max(x0, y, longname, C_WHITE, 120);        /* hard-cut at 120 px */''')

CODE_SDKC_FMT = code('''char num[11];              /* fmt_u: up to 10 digits + NUL   */
fmt_u(score, num);
text_at(x0 + 60, y, num, C_WHITE, C_BLUE, false);

char t[8];                 /* put2: "MM:SS" from two calls    */
put2(secs / 60, t);  t[2] = ':';  put2(secs % 60, t + 3);''')

CODE_SDKC_REDRAW = code('''static long frame;

static void my_tick(void)          /* ~60/s while the app is open */
{
    frame++;
    if (frame % 6) return;         /* pace: ~10 updates a second  */
    advance_state();
    repaint_all();                 /* deferred: painted at frame end */
}''')

CODE_SDKC_KEY = code('''static Boolean my_key(char ch, short code, Boolean cmd)
{
    if (ch == ' ')    { toggle();     repaint_all(); return true; }
    if (ch == 0x1E)   { move_up();    repaint_all(); return true; }  /* Up   */
    if (ch == 0x1F)   { move_down();  repaint_all(); return true; }  /* Down */
    if (ch == 0x1C)   { move_left();  repaint_all(); return true; }  /* Left */
    if (ch == 0x1D)   { move_right(); repaint_all(); return true; }  /* Right*/
    if (ch == 0x0D)   { confirm();    repaint_all(); return true; }  /* Enter*/
    (void)code; (void)cmd;
    return false;                  /* not ours */
}''')

CODE_SDKC_CLICK = code('''static void my_click(UnoWin *w, Point p)
{
    /* p is in SCREEN pixels - convert to canvas coordinates yourself */
    short cx = p.h - w->bounds.left;
    short cy = p.v - (w->bounds.top + TBAR_H);
    ...
}''')

CODE_SDKC_MOUSE = code('''Point m;
GetMouse(&m);                      /* screen coords; also refreshes buttons */
while (StillDown()) {              /* a classic drag loop                    */
    GetMouse(&m);                  /* GetMouse also presents the frame       */
    track(m.h - x0, m.v - y0);
}''')

CODE_SDKC_SOUND = code('''static const Note kTune[] = { {76,16},{72,16},{69,16},{0,8} };  /* 0 = rest */

music_note_on(84, 12);             /* one beep: MIDI 84, 12 frames  */
gm_start(kTune, 3, 0);             /* loop a score (owner ignored)  */
gm_stop();                         /* stop it - also do this in closed() */''')

CODE_SDKC_FILES = code('''static unsigned char buf[4096];
long n = fat12_read("NOTES.TXT", buf, sizeof buf);
if (n < 0) { /* missing, empty, or unreadable */ }

/* WARNING: writing an existing name APPENDS (see remarks) */
fat12_write("SCORES.TXT", data, len);''')

CODE_SDKC_MEM = code('''Ptr p = NewPtr(4096);
if (!p) return;                    /* NULL on failure - always check */
memset(p, 0, 4096);               /* NewPtr does not zero            */
...
DisposePtr(p);                     /* NULL-safe */''')


PAGES["dev-sdk-c.html"] = ("UnoC SDK reference", f"""
<h1>UnoC SDK reference</h1>
<p class="lede">Every call a UnoC app can make, with parameters, return values, remarks and a working
snippet for each. <code>SDK\\UNO.H</code> is the whole SDK - the one header you include - and Studio's
compiler checks every call against the kernel's export table at build time, so an unavailable function
is a compile error with a line number, never a mystery crash. The app model is on
<a href="dev-apps.html">Writing apps</a>; the language subset is summarised at the end.</p>

<h2 id="entry">The entry point and the vtable</h2>
<p>An app defines exactly one entry, <code>uno_app_main</code>, stashes the <code>KernelApi</code>
pointer in <code>gK</code> (the UNO.H prelude macros expand through it), and returns an
<code>AppInterface</code>:</p>
{CODE_SDKC_ENTRY}
<div class="tw"><table>
<thead><tr><th>Member</th><th>Contract</th></tr></thead>
<tbody>
<tr><td><code>void draw(UnoWin *w)</code></td><td><b>Required</b> - the load is refused without it.
Called once per repaint of the whole scene, in z-order, even when your window is occluded; not called
when closed or on another virtual desktop. The canvas is <b>not</b> cleared for you - paint everything
you own. <code>w-&gt;bounds</code> is in absolute screen pixels; content starts at
<code>bounds.top + TBAR_H</code> (18&nbsp;px).</td></tr>
<tr><td><code>Boolean key(char ch, short code, Boolean cmd)</code></td><td>Focused window only. Printable
ASCII (32..126) arrives in <code>ch</code> with <code>code</code> 0. Exactly six special keys are
delivered: arrows (<code>ch</code> 0x1C left, 0x1D right, 0x1E up, 0x1F down, with <code>code</code>
0x7B-0x7E), Enter (0x0D) and Backspace (0x08). Esc, Tab, Delete, Home/End and the function keys never
reach an app. <code>cmd</code> is the Ctrl flag <i>for those six keys only</i> - printable characters
always arrive with <code>cmd == false</code>, so Ctrl-letter accelerators are not receivable. Return
<code>true</code> to consume (which also marks the screen dirty).</td></tr>
<tr><td><code>void click(UnoWin *w, Point p)</code></td><td>Left-button <b>press only</b> - no release,
no motion, no right button. <code>p</code> is in <b>screen</b> pixels, not window pixels (see below).
Only clicks inside your canvas arrive; the title bar belongs to the shell. A delivered click always
triggers a repaint.</td></tr>
<tr><td><code>void tick(void)</code></td><td>Once per shell frame while the app is open - focused or
not - nominally 60/s, slower under load. <b>Does not repaint</b>: call <code>repaint_all()</code>
yourself when something visible changed.</td></tr>
<tr><td><code>void opened(void)</code> / <code>void closed(void)</code></td><td><code>opened</code> runs
once per Run, right after <code>uno_app_main</code>. Closing the window calls <code>closed()</code> but
keeps the module resident; the next Run replaces it wholesale and re-zeroes all globals, so every
<code>opened()</code> starts from pristine state. <b>Nothing is cleaned up for you on close</b> - stop
your music and free your memory in <code>closed()</code>.</td></tr>
<tr><td><code>const char *win_title</code></td><td>Window title, taskbar chip and launcher label. The
pointer is borrowed - use a string literal. NULL falls back to "My App".</td></tr>
<tr><td><code>short win_rect[4]</code></td><td><code>{{left, top, right, bottom}}</code> - <b>only the
width and height are used</b>; the shell places the window itself. Height includes the 18&nbsp;px title
bar you never draw, so the canvas is <code>w x (h-18)</code>. Minimum window 140x100; the window is not
resizable.</td></tr>
</tbody>
</table></div>
{note("There is no memory protection: your app runs at kernel privilege in the kernel's address "
      "space. A wild pointer or an out-of-range palette index doesn't crash your app, it crashes "
      "<i>the machine</i> (a debug build prints a register dump and writes a crash report first; a "
      "production build simply resets). Treat every call here as a kernel call.", kind="warn",
      title="You are the kernel")}

<h2 id="coords">Coordinates and the window</h2>
{CODE_SDKC_ORIGIN}
<p>All drawing is clipped to your canvas - with two exceptions noted below. Rects are half-open:
<code>SetRect(&amp;r, 0, 0, 10, 10)</code> covers pixels 0..9. An inverted or empty rect draws
nothing.</p>

<h2 id="drawing">Drawing</h2>
<h3><code>void uno_fill(Rect *r, short c)</code> / <code>void uno_box(Rect *r, short c)</code> /
<code>void uno_invert(Rect *r)</code></h3>
<div class="tw"><table>
<thead><tr><th>Parameter</th><th>Meaning</th></tr></thead>
<tbody>
<tr><td><code>r</code></td><td>The rectangle, screen coordinates, half-open.</td></tr>
<tr><td><code>c</code></td><td>A palette index: <code>C_BLUE</code>(0) <code>C_CYAN</code>(1)
<code>C_MAG</code>(2) <code>C_WHITE</code>(3). <b>Not bounds-checked</b> - anything else reads past a
4-entry table.</td></tr>
</tbody>
</table></div>
<p><b>Remarks.</b> <code>uno_fill</code> fills, <code>uno_box</code> draws a 1-pixel outline,
<code>uno_invert</code> XORs the pixels (colour channels only). All three are clipped to your canvas
and treat an empty rect as a no-op. The fill and box honour the global pen mode - after
<code>PenMode(patXor)</code> they invert instead of painting - and both leave the global fore colour
set to black on return.</p>
{CODE_SDKC_FILL}

<h3><code>void fill_rgb(Rect *q, const GameRGB *c)</code></h3>
<p>True-colour fill: <code>GameRGB</code> carries 0-255 <code>r,g,b</code> and a <code>mono</code>
palette fallback used by the 1-bit ports (ignored on pc64 - set it to the nearest <code>C_*</code> so
the same source works everywhere). Clipped; empty rect is a no-op; leaves fore colour black.</p>
{CODE_SDKC_FILLRGB}

<h3><code>void text_at(short x, short y, const char *s, short fg, short bg, Boolean opaque)</code></h3>
<div class="tw"><table>
<thead><tr><th>Parameter</th><th>Meaning</th></tr></thead>
<tbody>
<tr><td><code>x, y</code></td><td>Screen coordinates. <code>y</code> is neither the cell top nor the
baseline: with the default font the 16-px text cell occupies roughly <code>[y-7, y+9)</code> and the
baseline lands at <code>y+5</code>. Space rows at least 16&nbsp;px apart (the samples use 18).</td></tr>
<tr><td><code>fg, bg</code></td><td>Palette indices 0..3, unchecked. <code>bg</code> is used <b>only</b>
when <code>opaque</code> is true.</td></tr>
<tr><td><code>opaque</code></td><td><code>false</code>: only glyph pixels are written.
<code>true</code>: each character's advance cell is filled with <code>bg</code> first.</td></tr>
</tbody>
</table></div>
<p><b>Remarks.</b> The default face is a proportional Chicago-style TrueType at 15&nbsp;px - <b>there is
no fixed character width</b>, and the user can change both the face and the UI scale in the Control
Panel (at 150% your hardcoded layout <i>will</i> collide). Measure with <code>TextWidth</code> (exported;
declare <code>short TextWidth(Ptr, short, short);</code> yourself) or bound with <code>text_at_max</code>.
Glyphs cover ASCII 32..126; anything else draws as a blank advance. Clipped to the canvas. On return the
global text state is reset (fore black, back white, mode transparent).</p>

<h3><code>void text_at_max(short x, short y, const char *s, short fg, short maxw)</code></h3>
<p>As <code>text_at</code> (always transparent), but drops trailing characters until the run fits in
<code>maxw</code> pixels - a hard cut, no ellipsis. If not even one character fits, nothing is drawn.</p>
{CODE_SDKC_TEXT}

<h3 id="toolbox">Toolbox shapes: <code>PaintRect FrameRect InvertRect PaintOval FrameOval MoveTo
LineTo</code></h3>
<p>The classic QuickDraw slice. All paint with the <b>global fore colour</b> - set it with
<code>RGBForeColor(&amp;kPalette[C_CYAN])</code> or any <code>RGBColor</code> immediately before
drawing, because <b>every <code>uno_*</code>/<code>text_at</code>/<code>fill_rgb</code> call resets it
to black</b>. <code>PenMode</code> supports exactly two behaviours: <code>patXor</code>/<code>srcXor</code>
invert, everything else copies. <code>PenNormal()</code> resets size and mode (not colour).
<code>MoveTo</code> positions the pen; <code>LineTo</code> draws from the pen and moves it, so calls
chain into a polyline.</p>
{note("<code>PaintOval</code>, <code>FrameOval</code> and <code>MoveTo</code>/<code>LineTo</code> "
      "bypass the canvas clip: with coordinates outside your window they will happily paint over "
      "other windows and the taskbar. The rect and text calls are properly clipped; clip your own "
      "line and oval geometry.", kind="warn", title="Ovals and lines are not clipped")}

<h2 id="geometry">Geometry</h2>
<div class="tw"><table>
<thead><tr><th>Call</th><th>Effect</th></tr></thead>
<tbody>
<tr><td><code>void SetRect(Rect *r, short l, short t, short rt, short b)</code></td><td>Pure assignment -
does <b>not</b> normalize; an inverted rect is treated as empty by every drawing call.</td></tr>
<tr><td><code>void OffsetRect(Rect *r, short dh, short dv)</code></td><td>Translate. 16-bit arithmetic,
wraps silently past +/-32767.</td></tr>
<tr><td><code>void InsetRect(Rect *r, short dh, short dv)</code></td><td>Shrink by <code>dh/dv</code> on
each side (negative values grow). Over-insetting past the centre inverts the rect - no clamp.</td></tr>
</tbody>
</table></div>
<p class="muted"><code>PtInRect</code> is deliberately absent (UnoC cannot pass a <code>Point</code> by
value in that form) - compare <code>p.h</code>/<code>p.v</code> against the rect yourself.</p>

<h2 id="numbers">Numbers to text</h2>
<h3><code>void fmt_u(long v, char *out)</code></h3>
<p>Unsigned decimal, no padding, NUL-terminated. <b><code>v &lt;= 0</code> prints "0"</b> - negatives are
not rendered; format a sign yourself. <code>out</code> needs 11 bytes.</p>
<h3><code>void put2(long v, char *out)</code></h3>
<p>Exactly two zero-padded digits + NUL (<code>out</code> needs 3 bytes): the value modulo 100, so
<code>123</code> prints <code>"23"</code>. <b>Never pass a negative</b> - the digits come out as
garbage characters, not numbers.</p>
{CODE_SDKC_FMT}

<h2 id="windows">Windows and repainting</h2>
<div class="tw"><table>
<thead><tr><th>Call</th><th>Effect</th></tr></thead>
<tbody>
<tr><td><code>void repaint_all(void)</code></td><td>Mark the screen dirty; the shell repaints every
window at the end of the current frame. <b>This is the redraw call</b> - cheap, deferred, never
re-entrant. Animation from <code>tick()</code> must call it or nothing moves.</td></tr>
<tr><td><code>void draw_window(UnoWin *w)</code></td><td>Identical effect to <code>repaint_all()</code>
(the argument is ignored). Kept for source compatibility with the other ports.</td></tr>
<tr><td><code>UnoWin *find_app_window(short proc)</code></td><td>Your own window answers
<code>find_app_window(APP_NAPPS)</code> - and only after the first paint (before that its bounds are
zero). The <code>APP_*</code> enum values name the built-in apps; only the five shipped games/tools
can answer, and only while open. <b>Do not use <code>APP_RUNNER</code></b>: it always returns NULL for
a user app (older SDK examples got this wrong).</td></tr>
<tr><td><code>void launch_app(short proc)</code></td><td>A no-op on pc64 - the shell owns
launching.</td></tr>
<tr><td><code>short topmost_proc(void)</code> <span class="muted">(via
<code>gK-&gt;topmost_proc()</code>)</span></td><td>The focused <i>built-in</i> app's proc, or -1 -
including whenever <b>your</b> window is the focused one. Of little use to a user app.</td></tr>
</tbody>
</table></div>
{CODE_SDKC_REDRAW}

<h2 id="input">Input</h2>
{CODE_SDKC_KEY}
{CODE_SDKC_CLICK}
<h3><code>void GetMouse(Point *p)</code> / <code>Boolean StillDown(void)</code></h3>
<p><code>GetMouse</code> fills <code>p</code> with the pointer position in <b>screen</b> pixels
(<code>p.h</code> horizontal, <code>p.v</code> vertical) - subtract your window origin yourself. On pc64
it also polls the hardware and presents the frame, which is what makes a classic blocking drag loop
track live - but it is not free, so don't spam it. <code>StillDown()</code> reports whether a button is
held, <b>as of the last <code>GetMouse</code> call</b> - poll <code>GetMouse</code> in the loop or it
never changes.</p>
{CODE_SDKC_MOUSE}

<h2 id="tickcount">Time: <code>long TickCount(void)</code></h2>
<p>Read this one carefully: on pc64 <b><code>TickCount()</code> is a call counter, not a clock</b>. It
returns a global counter incremented once per call - by <i>any</i> caller, in any app - and nothing else
advances it. Called exactly once per <code>tick()</code> it approximates a 60&nbsp;Hz frame counter;
called twice, your animation runs at half speed; called while another app is also calling it, your
deltas inflate. The samples therefore pace on <b>their own frame counters</b> (see
<a href="dev-samples.html#timer">TIMER.C</a>), and so should you. There is no wall clock in the classic
SDK; a Python app can read <code>unoauto.uptime()</code> (real milliseconds).</p>

<h2 id="random">Randomness: <code>short Random(void)</code></h2>
<p>A shared linear-congruential generator returning the full signed 16-bit range -32768..32767 (mask
with <code>&amp; 0x7FFF</code> for non-negative). <b>The seed is a compile-time constant and there is no
seeding call</b>: the sequence is identical on every boot, shifted only by however many times anything
else has called it. Mix your own entropy in (LIFE.C stirs the cell coordinates into the seed for
exactly this reason).</p>

<h2 id="memory">Memory</h2>
<h3><code>Ptr NewPtr(long byteCount)</code> / <code>void DisposePtr(Ptr p)</code></h3>
<p><code>NewPtr</code> is malloc: 16-byte aligned, <b>NULL on failure</b> (always check), contents
<b>not zeroed</b>. A non-positive size allocates one byte. The arena is a single 32&nbsp;MB heap shared
with the shell and Studio - there is no per-app quota, so leaks degrade the whole machine until the next
Run (which reloads your module and frees nothing for you: free in <code>closed()</code>).
<code>DisposePtr</code> is free: NULL-safe and double-free-safe.</p>
{CODE_SDKC_MEM}
<p class="muted">The libc slice - <code>memcpy memmove memset memcmp strlen strcpy strncpy strcat strcmp
strncmp</code> - behaves exactly as C requires; sizes are <code>unsigned long</code> (4 bytes,
LLP64).</p>

<h2 id="sound">Sound</h2>
<p>One voice, shared with the shell's own apps: the PC speaker, or the machine's DAC when one exists.
Durations are in shell frames (~60/s).</p>
<div class="tw"><table>
<thead><tr><th>Call</th><th>Effect</th></tr></thead>
<tbody>
<tr><td><code>void music_open_chan(void)</code></td><td>No-op on pc64 (the voice is always ready);
call it once in <code>opened()</code> for portability.</td></tr>
<tr><td><code>void music_note_on(short midi, short durTicks)</code></td><td>One square-wave note at a
MIDI pitch (60 = middle C, 69 = A440; practical range ~24..108 - below ~16 nothing sounds).
<code>durTicks &lt;= 0</code> plays 6 frames. A note interrupts a playing score, which resumes
after.</td></tr>
<tr><td><code>void music_quiet(void)</code> / <code>void music_stop(void)</code> /
<code>void gm_stop(void)</code></td><td>All three do the same thing: stop the score and silence the
voice. <code>music_start()</code> is a no-op.</td></tr>
<tr><td><code>void gm_start(const Note *notes, short count, short owner)</code></td><td>Loop a score
forever. <code>Note</code> is <code>{{midi, dur}}</code> bytes; <code>midi 0</code> is a rest;
<code>owner</code> is ignored on pc64. <b>The array is borrowed, not copied</b> - make it
<code>static const</code>. <b>Not stopped when your window closes</b>: call <code>gm_stop()</code> in
<code>closed()</code>.</td></tr>
</tbody>
</table></div>
{CODE_SDKC_SOUND}

<h2 id="files">Files</h2>
<p>The classic file slice is a small, session-scoped store - fine for a settings file or a saved game,
wrong for anything bigger (a Python app's <code>uno.read/write</code> is the fuller API).</p>
<div class="tw"><table>
<thead><tr><th>Call</th><th>Contract</th></tr></thead>
<tbody>
<tr><td><code>Boolean fat12_mount(void)</code></td><td>Always true on pc64; call once for
portability.</td></tr>
<tr><td><code>void fat12_list(void)</code></td><td>Fills the exported arrays with the root directory of
every volume: <code>gFatCount</code> (capped at 16), <code>gFatNames</code> (12-char names + NUL).
<b><code>gFatSizes</code> is always 0 on pc64</b> - don't display it.</td></tr>
<tr><td><code>long fat12_read(const char *name, unsigned char *buf, long max)</code></td><td>Read from
offset 0, up to <code>max</code> bytes. Returns the byte count or <b>-1</b> for: not found, empty file,
or no free handle. Names are exact, case-sensitive, up to 31 chars. <b>Reads the session store only</b>:
a name listed by <code>fat12_list</code> from a real disk is not readable here.</td></tr>
<tr><td><code>Boolean fat12_write(const char *name, const unsigned char *buf, long len)</code></td>
<td>Creates the file if absent. <b>Writing an existing name APPENDS</b> - there is no truncate and no
delete, so save under a versionless single name only if you write it once per session, or build the
whole content and write once. On success the file is also mirrored to the first writable FAT volume, so
your save is visible in Files and to Python apps. Returns false only when the store is full (24 files /
256 KB per file); a failed mirror is not reported.</td></tr>
</tbody>
</table></div>
{CODE_SDKC_FILES}
{note("Within one session, save + reload round-trips. Across a reboot the classic read path starts "
      "fresh (your file still exists on disk - Files, Notepad and Python see it - but "
      "<code>fat12_read</code> does not). A game that must reload yesterday's save should be a "
      "Python app today.", title="Session-scoped reads")}

<h2 id="display">Display modes</h2>
<p><code>gK-&gt;display_res_count()</code> / <code>display_res_get(idx, &amp;w, &amp;h, &amp;zoom,
&amp;active)</code> / <code>display_res_set(idx)</code> enumerate and change the <i>desktop</i> size
(the output is scaled to the panel; <code>zoom</code> is always 0 on pc64). Call <code>count</code>
first - it rebuilds the list the other two index. <code>set</code> is immediate, relayouts every window,
and <b>bypasses the Control Panel's 15-second revert countdown</b> - a wrong mode stays wrong. Almost no
app should call it.</p>

<h2 id="lang">The language, on one screen</h2>
<div class="tw"><table>
<thead><tr><th>You have</th><th>You don't have</th></tr></thead>
<tbody>
<tr><td>char/short/int/long/long long + unsigned, pointers, multi-dim arrays, struct/union/enum/typedef,
function pointers, the full operator set, if/while/do/for/switch, static globals + locals, brace
initializers with address constants, <code>char s[] = "text"</code>, string-literal concatenation,
<code>sizeof</code>, recursion</td>
<td>floating point (not even the keywords), varargs (so no printf), function-like macros, bitfields,
goto, VLAs, <code>#if</code> arithmetic, <code>&lt;system&gt;</code> includes, more than 8
parameters/arguments, struct pass-by-value above 8 bytes</td></tr>
</tbody>
</table></div>
<p><b>LLP64:</b> <code>long</code> is 4 bytes, <code>long long</code> and pointers are 8. The ABI is
MS x64. <code>#include "FILE.H"</code> searches the file's folder, then <code>SDK\\</code>. <code>char</code>
is signed. Full grammar: <code>DOCS\\LANG.MD</code> on the device.</p>

<h2 id="descriptor">Shipping it: the app descriptor</h2>
<p>What Studio builds runs in the user slot (one at a time, 512&nbsp;KB). To give your app a Start-menu
row, a desktop icon and a durable identity, wrap it with an <b>app descriptor</b> on your PC - the full
walkthrough is in <a href="dev-apps.html#package">Writing apps §8</a>, and the grammar is:</p>
<div class="tw"><table>
<thead><tr><th>Key</th><th>Meaning</th><th>Limit</th></tr></thead>
<tbody>
<tr><td><code>id:</code></td><td>durable identity - geometry, session restore and <code>launch
&lt;id&gt;</code> all key on it</td><td>15 chars, <code>[a-z0-9._-]</code></td></tr>
<tr><td><code>name:</code> / <code>short:</code></td><td>launcher label / desktop-icon label</td>
<td>31 / 15 chars</td></tr>
<tr><td><code>icon:</code></td><td>a named emblem, or <code>file:NAME.QOI</code> (32x32 QOI beside the
module, hard-edged alpha)</td><td>15 chars total - the filename part fits 10</td></tr>
<tr><td><code>cat:</code> / <code>rank:</code></td><td>Start-menu section (system net tools media games
other) and sort key</td><td>rank 0-255</td></tr>
<tr><td><code>flags:</code></td><td><code>singleton hidden game nosession</code></td><td></td></tr>
<tr><td><code>min:</code></td><td>preferred window size <code>WxH</code></td><td>&lt; 8192</td></tr>
</tbody>
</table></div>
<p class="muted">The whole block is capped at 1024 bytes; unknown keys are ignored forever (the
extension point), unknown values fail the build. The user's <code>APPS.CFG</code> can rename, re-file
or hide your app - the module is the authority for what's inside the window, the user for what it's
called.</p>

<p class="kv">Worked examples: <a href="dev-samples.html">the sample programs</a>. The Python side:
<a href="dev-sdk-python.html">Python SDK reference</a>. The wider export table (the kernel exports far
more than UNO.H declares - <code>fb_*</code>, <code>malloc</code>, the filesystem) is discoverable in
<code>pc64_modload.c</code>'s <code>KX()</code> tables, undocumented and unguaranteed: stay inside
UNO.H unless you enjoy archaeology.</p>
""")

CODE_SDKPY_SKELETON = code('''import uno

class MyApp(uno.App):

    def build(self, cv):        # once, when the window opens
        self.w, self.h = cv.width(), cv.height()

    def draw(self, cv):         # every repaint
        cv.clear(uno.rgb(16, 18, 34))
        cv.text(8, 8, "hello", uno.rgb(214, 218, 232))

    def tick(self):             # ~60 times a second
        return False            # nothing changed - let the shell sleep

    def key(self, uni, scan, ctrl):
        return False            # not ours - let the shell have it

app = MyApp()                   # the runtime looks for this exact name
''')

CODE_SDKPY_RGB = code('''RED = uno.rgb(255, 96, 96)      # channels are 0-255, masked & 0xFF
cv.fill_rect(10, 10, 40, 40, RED)''')

CODE_SDKPY_TICKS = code('''import uno, unoauto

def build(self, cv):
    self.frames = 0                     # your own frame counter...
    self.t0 = unoauto.uptime()          # ...and the real clock, in ms

def tick(self):
    self.frames += 1                    # ~60/s: fine for animation pacing
    elapsed_s = (unoauto.uptime() - self.t0) // 1000   # honest seconds''')

CODE_SDKPY_KEYSDOWN = code('''held = uno.keys_down()
if held & 1:  self.y -= 2           # Up is held right now
if held & 4:  self.x += 2           # Right
# 0 on the firmware input path (no key-up events there):
# fall back to timing out your own key() events, like Duum does.''')

CODE_SDKPY_BEEP = code('''uno.beep(69, 30)                    # A440 for half a second
uno.quiet()                         # cut it short''')

CODE_SDKPY_SFX = code('''try:
    ok = uno.sfx_load(0, pcm_bytes, 11025)   # slot, 8-bit PCM, rate
    uno.sfx_play(0, 200, 128)                # slot, volume 0-255, pan 0/128/255
    uno.mus_play(smf_bytes)                  # standard MIDI file; loop=1 repeats
except OSError:                              # no DAC on this machine
    uno.beep(60, 10)                         # the square wave always works''')

CODE_SDKPY_READ = code('''data = uno.read("NOTES.TXT")        # the boot volume; None if missing
big  = uno.read(1, "DATA.BIN")      # an explicit volume index

# stream a large file instead of loading it whole:
hdr = uno.read_at(0, "GAME.WAD", 0, 12)     # vol, name, offset, length

n = uno.size("NOTES.TXT")           # -1 if it does not exist''')

CODE_SDKPY_WRITE = code('''ok = uno.write("SCORES.TXT", text.encode())
if not ok:
    ...                             # read-only volume - tell the user

uno.mkdir("SAVES")                  # one level; the parent must exist
uno.write("SAVES/SLOT1.DAT", blob)''')

CODE_SDKPY_LOG = code('''uno.log(6, 6, "level loaded")   # severity, facility, text - both ints
# severities follow syslog: 3 err, 4 warning, 5 notice, 6 info, 7 debug.
# facilities: 0 kernel, 1 net, 2 storage, 3 browser, 4 ui, 5 security,
# 6 app (yours), 7 remote.  Read it back in the System Log viewer.''')

CODE_SDKPY_PREFS = code('''name = uno.pref_get("player") or "YOU"   # None on first run
uno.pref_set("player", name)             # values are capped at 32 bytes''')

CODE_SDKPY_IDLE = code('''def key(self, uni, scan, ctrl):
    ...change something...
    self.dirty = True
    return True

def tick(self):
    if self.dirty:
        self.dirty = False
        return None                 # repaint this frame
    return False                    # idle - skip the repaint''')

CODE_SDKPY_UNOAUTO = code('''import uno, unoauto

class Bot(uno.App):
    def build(self, cv):
        self.steps = self.script()
        self.until = 0

    def script(self):
        unoauto.key(0x17, 0, 1)     # Ctrl+Esc: open the Start menu...
        yield 30                    # ...which lands on the NEXT frame
        unoauto.key(0, 13)          # Enter
        yield 30
        wins = [r[0] for r in unoauto.probe() if r[1] == 1]
        unoauto.log("open now: %s" % ", ".join(wins))

    def tick(self):
        if self.steps is None:
            return False
        now = unoauto.uptime()
        if now >= self.until:
            try:
                self.until = now + next(self.steps)
            except StopIteration:
                self.steps = None

app = Bot()''')

CODE_SDKPY_UNOSCRIPT = code('''import unoscript as u

u.cap_tier("power")                  # -> 2: needs an explicit grant
if u.request("power"):               # consent sheet, role, or manifest
    u.sys.power(0)                   # clean shutdown

u.fs.write("notes/todo.txt", b"buy milk")   # tier 1: USERS/<uid>/notes/...
print(u.fs.read("notes/todo.txt"))          # b'buy milk'  (4 KB read cap)

for pid, tid, state, name, owner in u.proc.list():   # tier 2
    print(pid, name, "focused" if state & 1 else "")''')


PAGES["dev-sdk-python.html"] = ("Python SDK reference", f"""
<h1>Python SDK reference</h1>
<p class="lede">Every call a Python app can make, with parameters, return values, remarks and a working
snippet for each. The app model itself is on <a href="dev-python.html">Python apps</a>; the machine-readable
stub ships on the device as <code>SDK\\uno.pyi</code>.</p>

<h2 id="model">The application object</h2>
<p>A Python app is one <code>.py</code> file that defines a class and a module-global instance named exactly
<code>app</code>. The runtime (<code>PYRT.UNO</code>) executes your file top to bottom, looks up <code>app</code>,
and binds whichever of the six callbacks you defined. There is no event loop to write and no <code>main()</code>:
the desktop calls you.</p>
{CODE_SDKPY_SKELETON}
{note("The binding happens once, at load. Adding or swapping a method on the instance afterwards has no "
      "effect, and if the name <code>app</code> is missing the run fails with "
      "<i>No `app` object. Define: app = MyApp()</i> in Studio's output pane.", title="Bound at load")}

<h3 id="callbacks">Callbacks</h3>
<div class="tw"><table>
<thead><tr><th>Callback</th><th>When it runs</th><th>Return</th></tr></thead>
<tbody>
<tr><td><code>build(self, cv)</code></td><td>Once, when the window opens. The canvas is already valid, so
<code>cv.width()</code>/<code>cv.height()</code> give your drawable size - read them here, never hardcode
a size (the shell picks your window).</td><td>ignored</td></tr>
<tr><td><code>draw(self, cv)</code></td><td>Every repaint.</td><td>ignored</td></tr>
<tr><td><code>tick(self)</code></td><td>~60 times a second while the app is open.</td><td><code>None</code>
(or nothing) repaints; an explicit falsey value such as <code>False</code> <b>skips the repaint</b> - the
idle idiom below.</td></tr>
<tr><td><code>key(self, uni, scan, ctrl)</code></td><td>A key was pressed while your window is focused.
<code>uni</code> is the character's codepoint (0 if none), <code>scan</code> the special-key code (0 if none),
<code>ctrl</code> nonzero while Ctrl is held. Exactly one of <code>uni</code>/<code>scan</code> is nonzero
per event.</td><td>truthy = you consumed it</td></tr>
<tr><td><code>action(self, id)</code></td><td>A toolkit widget action (only relevant to apps hosted with
widgets).</td><td>truthy = consumed</td></tr>
<tr><td><code>opened(self)</code> / <code>closed(self)</code></td><td>The window was shown / is closing.
Only bound if they exist at load.</td><td>ignored</td></tr>
</tbody>
</table></div>

<h3 id="keys">Key codes</h3>
<div class="tw"><table>
<thead><tr><th>Key</th><th><code>scan</code></th><th><code>uni</code></th></tr></thead>
<tbody>
<tr><td>Up / Down / Right / Left</td><td>1 / 2 / 3 / 4</td><td>0</td></tr>
<tr><td>Delete (forward)</td><td>8</td><td>0</td></tr>
<tr><td>F1..F12</td><td>0x0B..0x16</td><td>0</td></tr>
<tr><td>Esc</td><td>0x17</td><td>0</td></tr>
<tr><td>Enter / Backspace / Tab / Space</td><td>0</td><td>13 / 8 / 9 / 32</td></tr>
<tr><td>Letters, digits, symbols</td><td>0</td><td>the ASCII code, shift-aware</td></tr>
</tbody>
</table></div>
<p class="muted">Keys the shell owns never reach you: Ctrl+Esc, Alt+Tab, Ctrl+W, Ctrl+M, Ctrl+Tab, F2,
Alt+arrows, and Esc while a popup or fullscreen app is up.</p>

<h3 id="idle">The idle idiom</h3>
<p>An app that returns <code>None</code> from <code>tick()</code> repaints sixty times a second whether anything
changed or not. A static app should keep a dirty flag instead:</p>
{CODE_SDKPY_IDLE}

<h2 id="canvas">Canvas</h2>
<p>The drawing surface handed to <code>build</code> and <code>draw</code>. Coordinates are canvas-relative -
(0,&nbsp;0) is the top-left of your content area, below the title bar - and draws are clipped to your window.
All coordinates must be <code>int</code>s: pass <code>int(x)</code> if you animate with floats, or the call
raises <code>TypeError</code>.</p>
<div class="tw"><table>
<thead><tr><th>Method</th><th>Effect</th></tr></thead>
<tbody>
<tr><td><code>cv.width()</code> / <code>cv.height()</code></td><td>Drawable size in pixels. Read it every
draw if you want live resize behaviour.</td></tr>
<tr><td><code>cv.clear(color)</code></td><td>Fill the whole canvas.</td></tr>
<tr><td><code>cv.fill_rect(x, y, w, h, color)</code></td><td>Filled rectangle.</td></tr>
<tr><td><code>cv.rect(x, y, w, h, color)</code></td><td>1-pixel outline.</td></tr>
<tr><td><code>cv.pixel(x, y, color)</code></td><td>One pixel.</td></tr>
<tr><td><code>cv.hline(x, y, w, color)</code> / <code>cv.vline(x, y, h, color)</code></td><td>Horizontal /
vertical line.</td></tr>
<tr><td><code>cv.text(x, y, s, color)</code></td><td>Draw a string, transparent background.</td></tr>
</tbody>
</table></div>
<p class="muted">The remaining Canvas methods (<code>wall_col</code>, <code>wall_span</code>,
<code>mask_span</code>, <code>flat_span</code>, <code>duum_frame</code>, <code>seg_cols</code>) are the
textured-column fast paths built for Duum's renderer; their exact contracts are in
<code>SDK\\uno.pyi</code>.</p>

<h2 id="colours">uno.rgb</h2>
<p><b><code>uno.rgb(r, g, b) -&gt; int</code></b> packs three 0-255 channels into the colour value every
drawing call takes. Build your palette once at module level, not per frame.</p>
{CODE_SDKPY_RGB}

<h2 id="time">Time and held keys</h2>
<p><b><code>uno.ticks() -&gt; int</code></b> - the Toolbox tick counter. Honesty first: this is a
<i>call counter</i> shared with every C app (it advances once per call, from anywhere), so treat it as
frame pacing only - call it exactly once per <code>tick()</code> and use deltas - or skip it and count
your own <code>tick()</code> invocations. For real time, use <b><code>unoauto.uptime()</code></b>:
milliseconds since boot, ungated in every build - the clock the automation samples pace on.</p>
{CODE_SDKPY_TICKS}
<p><b><code>uno.keys_down() -&gt; int</code></b> - the navigation/action keys held <i>right now</i>, as a
bitmask: 1 Up, 2 Down, 4 Right, 8 Left, 16 fire (F or Ctrl), 32 use (Space/E), 64 comma, 128 period.
For movement, polling this beats buffering <code>key()</code> events.</p>
{CODE_SDKPY_KEYSDOWN}

<h2 id="sound">Sound</h2>
<p><b><code>uno.beep(midi, ticks)</code></b> plays a square-wave note at a MIDI pitch (60 = middle C,
69 = A440) for <code>ticks</code> sixtieths of a second; <b><code>uno.quiet()</code></b> stops it. These two
always work, on any machine.</p>
{CODE_SDKPY_BEEP}
<p>The sampled-audio calls need a DAC (HD&nbsp;Audio or AC'97) and <b>raise <code>OSError</code></b> when the
machine has none - always wrap them and fall back to <code>beep</code>:</p>
<div class="tw"><table>
<thead><tr><th>Call</th><th>Effect</th></tr></thead>
<tbody>
<tr><td><code>uno.sfx_load(slot, pcm, rate) -&gt; bool</code></td><td>Load unsigned 8-bit mono PCM into an
effect slot.</td></tr>
<tr><td><code>uno.sfx_play(slot, vol, sep) -&gt; bool</code></td><td>Play a loaded slot: volume 0-255, stereo
position 0 left / 128 centre / 255 right. <code>False</code> means that one play didn't start.</td></tr>
<tr><td><code>uno.mus_play(smf, loop=0) -&gt; bool</code></td><td>Play a standard MIDI file from bytes.</td></tr>
<tr><td><code>uno.mus_stop()</code></td><td>Stop music.</td></tr>
</tbody>
</table></div>
{CODE_SDKPY_SFX}

<h2 id="files">Files</h2>
<p>Volumes are numbered; <b>0 is the boot volume</b> and the one-argument forms default to it. Names are
8.3 on FAT volumes and case-insensitive. There is no <code>listdir</code> and no file objects - the API is
whole-file and offset reads, which is what a small app actually needs.</p>
<div class="tw"><table>
<thead><tr><th>Call</th><th>Returns</th><th>Remarks</th></tr></thead>
<tbody>
<tr><td><code>uno.read(name)</code> / <code>uno.read(vol, name)</code></td><td><code>bytes</code>, or
<code>None</code> if the file does not exist</td><td>Reads the whole file - fine for documents, wrong for a
4&nbsp;MB WAD (stream those with <code>read_at</code>).</td></tr>
<tr><td><code>uno.read_at(vol, name, off, n)</code></td><td><code>bytes</code></td><td>Reads <code>n</code>
bytes at offset <code>off</code>. <code>vol</code> is required here.</td></tr>
<tr><td><code>uno.size(name)</code> / <code>uno.size(vol, name)</code></td><td><code>int</code>, -1 if
missing</td><td>The cheap existence test.</td></tr>
<tr><td><code>uno.write(name, data)</code> / <code>uno.write(vol, name, data)</code></td><td><code>bool</code></td>
<td><code>False</code> on a read-only volume - surface that state to the user, don't swallow it.</td></tr>
<tr><td><code>uno.mkdir(path)</code> / <code>uno.mkdir(vol, path)</code></td><td><code>bool</code></td>
<td>One level at a time; the parent must exist.</td></tr>
</tbody>
</table></div>
{CODE_SDKPY_READ}
{CODE_SDKPY_WRITE}

<h2 id="prefs">Preferences and key bindings</h2>
<p>Small per-app settings without inventing a file format:
<b><code>uno.pref_get(name) -&gt; str|None</code></b> and <b><code>uno.pref_set(name, value) -&gt; bool</code></b>
persist strings (values capped at 32 bytes) in the shell's preference store.
<b><code>uno.bind_name(action)</code></b>, <b><code>uno.bind_set(action, uni, scan)</code></b> and
<b><code>uno.bind_reset()</code></b> read and remap the shared game-action key bindings.</p>
{CODE_SDKPY_PREFS}

<h2 id="syslog">The system log</h2>
<p><b><code>uno.log(sev, fac, text)</code></b> writes a line to the system log (severities follow syslog;
<code>fac</code> is a numeric facility - apps are 6). The viewer app, the log file sink and the remote link all see it -
prefer this to inventing your own trace file. The reading half
(<code>log_read</code>/<code>log_span</code>/<code>log_stat</code>/...) is documented in
<code>SDK\\uno.pyi</code>.</p>
{CODE_SDKPY_LOG}

<h2 id="introspection">Hardware introspection</h2>
<p><b><code>uno.devices() -&gt; str</code></b> and <b><code>uno.pci() -&gt; list</code></b> expose the device
tree (location, IDs, class, claiming driver) - the same data the <code>devices</code> remote verb reports.
Read-only.</p>

<h2 id="limits">Language and library limits</h2>
<p>The runtime is MicroPython at its core feature level, plus the modules above. What that means in
practice:</p>
<div class="tw"><table>
<thead><tr><th>You have</th><th>You don't have</th></tr></thead>
<tbody>
<tr><td>classes, generators, comprehensions, <code>set</code>, <code>bytearray</code>, big ints, floats
(single precision), <code>%</code> and <code>.format()</code> formatting, walrus, <code>async</code>/<code>await</code></td>
<td>f-strings, <code>frozenset</code>, <code>memoryview</code>, <code>__del__</code>, reverse special
methods, <code>NotImplemented</code></td></tr>
<tr><td><code>math</code>, <code>struct</code>, <code>array</code>, <code>collections</code>
(namedtuple), <code>sys</code>, <code>gc</code>, <code>micropython</code></td>
<td><code>time</code>, <code>random</code>, <code>json</code>, <code>os</code>, <code>io</code>,
<code>re</code>, <code>typing</code> - and <code>math.pi</code> (write <code>3.14159265</code>)</td></tr>
<tr><td>one <code>.py</code> file up to 192&nbsp;KB, a 16&nbsp;MB heap, an 8&nbsp;KB
<code>print()</code> buffer shown in Studio</td>
<td>importing a second <code>.py</code> file, threads, and the <code>@micropython.native</code>/<code>viper</code>
decorators (they fault on real hardware - never use them)</td></tr>
</tbody>
</table></div>

<h2 id="unoauto-ref">unoauto: drive the machine</h2>
<p><code>import unoauto</code> is the automation half of <a href="dev-remote.html">unoautomate</a>, available
to any Python app <b>in every build</b>: each call checks the caller's privilege and, when the capability is
absent, returns its inert value (<code>False</code>, <code>[]</code>, <code>None</code>) instead of raising -
so the same script degrades gracefully on a machine that trusts it less. <code>uptime()</code> and
<code>deadline_left()</code> are ungated.</p>
<div class="tw"><table>
<thead><tr><th>Function</th><th>Needs</th><th>Returns / effect</th></tr></thead>
<tbody>
<tr><td><code>available()</code></td><td>observe</td><td><code>True</code> when observation is granted.</td></tr>
<tr><td><code>log(text)</code></td><td>observe</td><td>Emit on the script log channel (streams to the dev PC).</td></tr>
<tr><td><code>probe()</code></td><td>observe</td><td>System snapshot: <code>(name, kind, state, v1, v2)</code>
rows - kind 0 module, 1 window, 2 subsystem.</td></tr>
<tr><td><code>key(scan, uni, ctrl=0)</code></td><td>drive</td><td>Inject a key. <b>Argument order is
scan-first</b> - the mirror image of the <code>key()</code> callback. Lands on the <i>next</i> frame.</td></tr>
<tr><td><code>pointer(x, y, btn)</code></td><td>drive</td><td>Inject a pointer event (btn 1 press, 0 release).</td></tr>
<tr><td><code>apps()</code> / <code>launch(i)</code> / <code>close_top()</code></td><td>drive</td>
<td>Count, open, close apps. <b>Never <code>close_top()</code> your own window</b> from an automation
script.</td></tr>
<tr><td><code>uptime()</code></td><td>-</td><td>Milliseconds since boot. Pace scripts on this.</td></tr>
<tr><td><code>deadline_left()</code></td><td>-</td><td>ms left in the current test budget, -1 if none.</td></tr>
<tr><td><code>poweroff()</code></td><td>system</td><td>Shut down (unattended runs end themselves).</td></tr>
<tr><td><code>remote_active()</code> / <code>remote_send(s)</code> / <code>remote_recv()</code> /
<code>remote_stop()</code></td><td>observe/system</td><td>Talk to the dev PC over the URC link.</td></tr>
</tbody>
</table></div>
<p>The load-bearing pattern - actions land on the <b>next</b> shell frame, so run your steps from a generator
and <code>yield</code> between step and check:</p>
{CODE_SDKPY_UNOAUTO}

<h2 id="unoscript-ref">unoscript: the permission-gated OS surface</h2>
<p><code>import unoscript</code> is the production automation surface: UI, apps, files, processes, memory,
ports and power, each call gated by a capability tier (0 ambient, 1 user, 2 admin, 3 kernel). Denied calls
raise <code>OSError</code>; <code>request()</code> asks for a grant (consent sheet, role, or signed manifest)
and answers <code>False</code> rather than raising. The model, the manifest story and the full catalog are on
<a href="dev-remote.html#unoscript">Remote control &amp; automation</a>.</p>
<div class="tw"><table>
<thead><tr><th>Call</th><th>Tier</th><th>Effect</th></tr></thead>
<tbody>
<tr><td><code>available()</code> / <code>whoami()</code> / <code>secured()</code></td><td>-</td>
<td>Presence, acting uid (0xFFFFFFFF when nobody is signed in), whether the real adjudicator is in.</td></tr>
<tr><td><code>cap_tier(name)</code> / <code>request(name[, scope[, ttl]])</code></td><td>-</td>
<td>Look up a capability's tier; ask for it.</td></tr>
<tr><td><code>ui.click(x, y[, btn])</code> / <code>ui.move(x, y)</code> / <code>ui.key(scan[, uni[, mods]])</code></td>
<td>0</td><td>Synthetic input on the real device path.</td></tr>
<tr><td><code>ui.screen()</code></td><td>0</td><td>The window tree as text, focused window marked.</td></tr>
<tr><td><code>ui.clip_get()</code> / <code>ui.clip_set(s)</code></td><td>0 / 1</td><td>Clipboard.</td></tr>
<tr><td><code>app.count()</code> / <code>app.launch(i)</code> / <code>app.close_top()</code> /
<code>app.message(i, verb)</code></td><td>0 / 1</td><td>App control; <code>verb</code> is
<code>"info"</code>, <code>"focus"</code> or <code>"close"</code>.</td></tr>
<tr><td><code>fs.read(path)</code> / <code>fs.write(path, data)</code></td><td>1 / 2</td>
<td>A bare relative path is <b>your home</b> (<code>USERS\\&lt;uid&gt;\\...</code>, parents auto-created,
reads capped at 4&nbsp;KB); an absolute <code>/label/rest</code> names a volume and is tier 2.
<code>..</code> is rejected.</td></tr>
<tr><td><code>proc.list()</code></td><td>2</td><td><code>(pid, tid, state, name, owner)</code> rows;
state bit 0 = focused.</td></tr>
<tr><td><code>mem.read/write</code>, <code>io.in_/out</code></td><td>2-3</td><td>Raw memory and port I/O;
always audited.</td></tr>
<tr><td><code>sys.power(n)</code></td><td>2</td><td>0 shutdown, 1 reboot.</td></tr>
</tbody>
</table></div>
{CODE_SDKPY_UNOSCRIPT}

<p class="kv">Worked examples: <a href="dev-samples.html">the sample programs</a>. The app model:
<a href="dev-python.html">Python apps</a>. The permission model and unattended grants:
<a href="dev-remote.html">Remote control &amp; automation</a>.</p>
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
<tr><td><code>pc64_fs</code> / <code>pc64_io</code></td><td>Unified file namespace: volume 0 is the RAM disk, volumes 1+ are FAT/FAT32 disks mounted by UnoDOS's own FAT stack (read/write) over the native AHCI/NVMe/SDHCI and USB mass-storage drivers, with firmware Simple File System volumes as read/write extras while attached.</td></tr>
<tr><td><code>blkdev</code> / <code>unostorage</code> / <code>fat</code></td><td>The storage stack: <code>blkdev</code> is raw 512-byte sector transport (native drivers + a firmware fallback); <code>fat</code> mounts + reads/writes FAT16/32 and formats it (<code>uno_fat_mkfs</code>); <code>unostorage</code> authors a GPT + ESP on a raw disk. The installer and the remote channel both wrap these rather than re-implementing them.</td></tr>
<tr><td><code>unosound</code></td><td>Single-voice sequencer; the shared audio path for the games, Music and Tracker (<code>uno_seq_beep</code> / <code>_play</code> / <code>_stop</code>). On pc64 the voice renders into an HD&nbsp;Audio / AC'97 PCM ring when one exists (<code>snd_pcm.c</code>), else the PC speaker.</td></tr>
<tr><td><code>pc64_pci</code></td><td>PCI config scan; locates the e1000 NIC, xHCI controllers and the Intel iGPU.</td></tr>
<tr><td><code>uno_devmgr</code> (unodevices)</td><td>The device manager: enumerates the whole PCI tree once into a registry (location, IDs, class, capabilities, BARs, parent bridge) and reports what is on the machine and which driver, if any, claimed each part. It backs the <code>devices</code> remote verb and the <code>uno.devices()</code>/<code>uno.pci()</code> Python calls. Phase&nbsp;1 is read-only introspection; driver auto-binding is a later phase. See <a href="https://github.com/hmofet/unodos/blob/master/pc64/DEVICES.md" target="_blank" rel="noopener"><code>DEVICES.md</code></a>.</td></tr>
<tr><td><code>net</code> / <code>e1000</code> / <code>e1000e</code> / <code>igb</code> / <code>r8169</code></td><td>Intel (8254x, 82571-4/82574, I217-9, I210/I211/I350) and Realtek (RTL8168/8111/8125) drivers, each publishing a <code>uno_nic_t</code>, plus a from-scratch stack: ARP, IPv4, ICMP, UDP, DHCP, DNS, single-connection TCP.</td></tr>
<tr><td><code>pc64_http</code> / <code>pc64_browser</code> / <code>js</code></td><td>HTTP/1.0 GET with DNS, the immediate-mode HTML/Markdown/CSS renderer, and the JavaScript interpreter.</td></tr>
<tr><td><code>tls</code> / <code>bearssl</code></td><td>Freestanding BearSSL, TLS 1.2, with pinned-key and CA-validated (14 roots) modes; clock from the UEFI RTC.</td></tr>
<tr><td><code>xhci</code> / <code>ax88179</code> / <code>rtl8152</code> / <code>usbmsc</code></td><td>Opt-in (<code>-DUNO_XHCI</code>) polled xHCI host, ASIX and Realtek USB-gigabit drivers (each publishing a <code>uno_nic_t</code>), and USB mass storage (Bulk-Only Transport).</td></tr>
<tr><td><code>iwlwifi</code> / <code>rtwifi</code> / <code>mrvlwifi</code></td><td>Intel (AX201/AX210), Realtek and Marvell Wi-Fi drivers. The Intel driver loads firmware, scans, joins a WPA2 network and holds a DHCP lease on real hardware (a Surface Laptop Go); WPA3 authenticates but its handshake does not complete yet. Realtek and Marvell map the device and load its firmware, but do not associate yet.</td></tr>
<tr><td><code>pc64_font</code></td><td>Optional TrueType engine; registers as the fb text provider with subpixel smoothing, falling back to the built-in bitmap font.</td></tr>
<tr><td><code>unovirt</code> / <code>hv_vmx</code> / <code>hv_svm</code> / <code>unovdev</code></td><td>The hypervisor behind <a href="appliances.html">Appliances</a>: a capability gate that says whether this machine can host a guest and why not, a backend seam with Intel VMX and AMD SVM implementations, second-stage paging into a memory carve taken at detach, a budgeted slice run from the shell's frame loop, and virtio-mmio device models (console, block, net) plus the 8250 and 8259 a Linux kernel expects. Proven on VMX; the SVM side builds but has not yet run a guest. See <a href="https://github.com/hmofet/unodos/blob/master/pc64/UNOVIRT.md" target="_blank" rel="noopener"><code>UNOVIRT.md</code></a>.</td></tr>
</tbody>
</table></div>

<h2 id="uno3d">3D (uno3d)</h2>
<p>A small write-once 3D pipeline with a software rasteriser (Gouraud shading, no textures). Runner3D drives it
directly.</p>
{CODE_UNO3D}

<h2 id="uno-py">uno: the Python app module</h2>
<p>The surface a <a href="dev-python.html">Python app</a> codes against. <code>import uno</code>, subclass
<code>uno.App</code>, and reach the platform through the module and the <code>Canvas</code> passed to your
<code>build</code>/<code>draw</code>. A machine-readable stub ships in <code>SDK\\uno.pyi</code>.</p>
{CODE_PY_UNO_API}
<p class="muted">This is the app-authoring module. The separate <code>unoauto</code> module below is the
system-<em>automation</em> surface (probe, inject input, drive the machine), present in every build and
gated by privilege. The full per-call contract is in the
<a href="dev-sdk-python.html">Python SDK reference</a>.</p>

<h2 id="unoauto">unoauto: automation (Python, on the device)</h2>
<p><code>import unoauto</code> from any Python app to observe and drive the system - the automation half of
<a href="dev-remote.html">unoautomate</a>. The surface ships in <strong>every</strong> build; each call
checks the caller's privilege, and a capability you don't hold makes the call return its inert value
(<code>False</code>, <code>[]</code>, <code>None</code>) rather than raise - so
<code>available()</code> answers <code>False</code> on a machine that grants you nothing. In a debug OS
everything is allowed.</p>
<div class="tw"><table>
<thead><tr><th>Function</th><th>Returns / effect</th></tr></thead>
<tbody>
<tr><td><code>available()</code></td><td>True in a debug OS, False in production.</td></tr>
<tr><td><code>log(text)</code></td><td>Emit a line on the script log channel (streams to the remote link).</td></tr>
<tr><td><code>probe()</code></td><td>System snapshot: a list of (name, kind, state, v1, v2) rows - kind 0 module, 1 window, 2 subsystem.</td></tr>
<tr><td><code>key(scan, uni, ctrl=0)</code></td><td>Inject a keypress, processed on the next frame.</td></tr>
<tr><td><code>pointer(x, y, btn)</code></td><td>Inject a pointer event.</td></tr>
<tr><td><code>apps()</code></td><td>Number of launchable apps.</td></tr>
<tr><td><code>launch(i)</code></td><td>Open app <code>i</code>; True on success. The id form over the wire (<code>launch &lt;id&gt;</code>) is the one to prefer in anything durable.</td></tr>
<tr><td><code>close_top()</code></td><td>Close the top window.</td></tr>
<tr><td><code>uptime()</code></td><td>Milliseconds since boot.</td></tr>
<tr><td><code>deadline_left()</code></td><td>Milliseconds left in the current test budget, or -1 if none is armed.</td></tr>
<tr><td><code>poweroff()</code></td><td>Shut the machine down (for unattended runs).</td></tr>
<tr><td><code>remote_active()</code></td><td>True when the link to the dev PC is up.</td></tr>
<tr><td><code>remote_send(text)</code></td><td>Send a message to the dev PC.</td></tr>
<tr><td><code>remote_recv()</code></td><td>Next inbound message, or None.</td></tr>
<tr><td><code>remote_stop()</code></td><td>Tear the link down.</td></tr>
</tbody>
</table></div>

<h2 id="unoautolink">UnoAutoLink: driving from your PC (Python)</h2>
<p>From <code>pc64/tools/unoauto_remote.py</code>. Construct one, call <code>listen()</code>, and the device dials in
(<code>wait_connected()</code>). Every command method blocks for the reply and raises on a device error or timeout.</p>
<div class="tw"><table>
<thead><tr><th>Method</th><th>Returns / effect</th></tr></thead>
<tbody>
<tr><td><code>listen()</code> / <code>close()</code></td><td>Start / stop the listener.</td></tr>
<tr><td><code>wait_connected(timeout)</code></td><td>Block until the device connects.</td></tr>
<tr><td><code>on_log(cb)</code> / <code>on_message(cb)</code></td><td>Callbacks for streamed logs and messages.</td></tr>
<tr><td><code>on_command(verb, cb)</code></td><td>Handle a command the <em>device</em> sends to your PC.</td></tr>
<tr><td><code>message(text)</code></td><td>Send a free-form message to the device.</td></tr>
<tr><td><code>command(verb, *args, timeout=5)</code></td><td>Run any command; returns its reply lines.</td></tr>
<tr><td><code>probe()</code></td><td>Snapshot as a list of dicts (keys: kind, state, v1, v2, name).</td></tr>
<tr><td><code>vols()</code></td><td>Volumes as a list of dicts (keys: vol, kind, writable, name).</td></tr>
<tr><td><code>launch(id)</code> / <code>close_top()</code> / <code>apps()</code></td><td>App control. <code>launch</code> takes an app <strong>id</strong> (<code>"browser"</code>) or a slot number; prefer the id, since a number is this boot's ordering of whatever is installed. <code>command("apps", "list")</code> lists them.</td></tr>
<tr><td><code>key(scan, uni, ctrl=0)</code> / <code>pointer(x, y, btn=0)</code></td><td>Inject input.</td></tr>
<tr><td><code>eval(src)</code></td><td>Run a line of Python on the device; returns its output.</td></tr>
<tr><td><code>test(suite="")</code></td><td>Run a conformance suite; returns the report.</td></tr>
<tr><td><code>uptime()</code> / <code>poweroff()</code> / <code>reboot()</code></td><td>Read uptime, shut down, or restart.</td></tr>
<tr><td><code>push_file(vol, path, local_path)</code></td><td>Push a file (chunk, finalize, verify); True when verified.</td></tr>
<tr><td><code>bootnext(n)</code></td><td>Set the next UEFI boot entry.</td></tr>
<tr><td><code>disks()</code></td><td>List raw disks (idx / name / sectors / writable / is_boot).</td></tr>
<tr><td><code>arm(disk)</code> / <code>disarm()</code></td><td>Arm a disk for a destructive op (auto-disarms after one; refuses the boot disk).</td></tr>
<tr><td><code>prepdisk(disk, label)</code></td><td>Partition + format a raw disk as a fresh FAT32 ESP (armed; the "prepare disk B" one-shot).</td></tr>
<tr><td><code>mkdir(vol, path)</code></td><td>Create a directory on a volume (parent must exist).</td></tr>
<tr><td><code>install(disk, make_default=False)</code> / <code>install_dir(disk, esp_dir)</code></td><td>Clone the running OS onto a prepared disk in one armed step, or lay down a freshly built ESP tree from your PC instead - the headless install.</td></tr>
<tr><td><code>devices()</code></td><td>The machine's PCI devices as a list of dicts (keys: loc, vendor, device, cls, name, driver, raw) - what hardware is present and what has no driver yet.</td></tr>
<tr><td><code>guard(secs, action="reboot")</code> / <code>pet()</code> / <code>safe()</code></td><td>Arm / keep-alive / stand down the dead-man's switch. <code>with link.guarded(secs): ...</code> arms on entry and stands down on exit, so a wedge inside the block resets the box.</td></tr>
</tbody>
</table></div>
<p class="muted">The C contract underneath (<code>unoauto_log</code>, <code>unoauto_probe</code>,
<code>unoauto_sink_add</code>, <code>unoauto_test_run</code>, the hook taps and <code>unoauto_remote_*</code>) is in
<a href="https://github.com/hmofet/unodos/blob/master/pc64/unoauto.h" target="_blank" rel="noopener"><code>pc64/unoauto.h</code></a>;
the wire protocol is in <a href="https://github.com/hmofet/unodos/blob/master/pc64/REMOTE.md" target="_blank" rel="noopener"><code>REMOTE.md</code></a>.</p>
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
single <code>BOOTX64.EFI</code> into a bootable <strong>ESP</strong> tree in <code>build/esp/</code>. That kernel is
about <strong>3.8&nbsp;MB</strong> and the whole tree about <strong>14&nbsp;MB</strong>: the rest is the
<code>.UNO</code> apps, the four TrueType fonts, the sample media and the SDK beside it. Two local extras grow it
if you have them, and neither is in the repository: the Wi-Fi firmware blobs in <code>fw-blobs/</code> add about
7&nbsp;MB, and a Doom WAD in <code>wads/</code> adds its own size again. <code>UNO_DEBUG=1</code> links a bigger
kernel, about 4.4&nbsp;MB.</p>
{note('The desktop is fully keyboard-driven, so QEMU needs no special input setup. QEMU is also scriptable over QMP (<code>send-key</code> + <code>screendump</code>), which is how the screenshots in this manual are made.', title="Keyboard-first")}

<h2 id="image">Pack a real USB disk image</h2>
<p>QEMU fakes a disk from the <code>build/esp/</code> directory, but real firmware needs a partition table.
<code>tools/mkuefi.py</code> turns the ESP tree into a raw disk image: GPT with one FAT32 EFI System Partition
holding the whole tree. This is the image the flashers embed.</p>

<h2 id="variants">Build variants</h2>
<p>Two environment toggles to <code>build.sh</code> choose what the image is:</p>
<div class="tw"><table>
<thead><tr><th>Toggle</th><th>Effect</th></tr></thead>
<tbody>
<tr><td><code>UNO_DEBUG=1</code></td><td>The debug / test harness: crash reports, the watchdog, the conformance suites, and the whole <a href="dev-remote.html">unoautomate</a> remote channel and on-device automation. Off by default - a production image has none of it.</td></tr>
<tr><td><code>UNO_PYRT=0</code></td><td>Skip <code>PYRT.UNO</code>, the vendored-MicroPython Python runtime, for a smaller image. On by default; without it, <a href="dev-python.html">Python apps</a> will not run.</td></tr>
</tbody>
</table></div>

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
<p>Copy the PNGs you need into <code>docs/assets/img/</code>, then rebuild and commit.</p>
<p>Scenes name apps by <strong>id</strong> - <code>A("uocalc")</code>, never a row number - because the Start-menu
order moves whenever an app is added and a scene that counts keystrokes does not fail when it does: it opens the
next app along and files the picture under the old name. The order comes from
<code>pc64/build/apps_roster.txt</code>, written by a run that opened every app by id over URC and checked the
window that appeared:</p>
<pre><code>UNO_DEBUG=1 ./build.sh &amp;&amp; python3 harness.py unoapps   # measure the order
./build.sh &amp;&amp; python3 docs_shots.py                     # capture the figures</code></pre>
<p>The figures come from a production build, where URC would want a token typed at the console, so the order is
measured on a debug build of the same tree. Before it captures anything, <code>docs_shots.py</code> asks the
live menu how many rows it has and stops the run if that disagrees with the roster, which is what makes borrowing
the order from the other build safe. If the Control Panel's tab order changes, that is still a hand edit.</p>
""")

PAGES["dev-remote.html"] = ("Remote control & automation", f"""
<h1>Remote control &amp; automation (unoautomate)</h1>
<p class="lede">A debug build of UnoDOS pc64 can open a link to the PC you develop from and stream its
logs to you, take commands from you, and exchange messages in either direction - all over the
network, with a simple typed command language or a Python API. This is <strong>unoautomate</strong>:
the OS's remote-control and automation channel. It is how you drive, observe, and update a machine
that is sitting on a bench across the room.</p>

{note('The channel ships in <strong>every</strong> build, and the gate is <strong>privilege, not a compile flag</strong>: a production OS boots with it disarmed, and only a user at the console can arm it (below). A debug OS (<code>UNO_DEBUG=1</code>) arms it from <code>DEBUG.CFG</code> with every verb allowed and no code. Either way it is meant for a <strong>trusted LAN</strong> and speaks in plaintext - the code proves who may drive the box, it does not encrypt the wire - so never expose it to an untrusted network.', kind="warn", title="Armed by a person, LAN only")}

<h2 id="arming">Production: arming and the access code</h2>
<p>On a production boot nothing listens and nothing dials. To hand your PC the keys, open
<strong>Control Panel &rarr; Remote control&hellip;</strong> and tick what you are willing to grant -
the three ticks map to the three powers every command is checked against:</p>
<div class="tw"><table>
<thead><tr><th>Tick</th><th>Grants</th></tr></thead>
<tbody>
<tr><td><b>Watch</b></td><td>see the screen and system state - logs, probe, screenshots</td></tr>
<tr><td><b>Control</b></td><td>move the mouse, type, open apps</td></tr>
<tr><td><b>Full access</b></td><td>disks, files, run code, restart</td></tr>
</tbody>
</table></div>
<p>Arming mints a <strong>6-digit code</strong>, shown on screen and never stored. The first thing a
client sends is <code>auth &lt;code&gt;</code>; until then every verb answers
<code>err auth-required</code>, and <strong>three wrong codes disarm the channel outright</strong> - the
operator can re-arm at the console, an attacker on the wire cannot, which is what the code's length
rests on. A verb outside the granted powers answers <code>err denied</code> naming the missing power.
The remote path never prompts (nobody is at the machine to answer), and signing out disarms - a code
can never outlive the login that made it. Grant the least you need: a monitoring script wants Watch and
nothing else.</p>

<h2 id="enable">Turning it on (debug builds)</h2>
<p>pc64's network stack makes outbound connections only, so the device <strong>dials your PC</strong> rather
than the other way round. You tell it where to dial with one line in the stick's <code>DEBUG.CFG</code>
(the Developer-options file the flasher writes) - your PC's LAN address and a port:</p>
{CODE_REMOTE_CFG}
<p>Boot a debug stick with that key and, once networking is up, it connects to the listener you run on
your PC (below). If the link drops it reconnects on its own.</p>
<p>You do not edit <code>DEBUG.CFG</code> by hand. Flash a debug stick with <strong>Developer options</strong>
turned on in the <a href="getting-started.html#flasher">UnoDOS flasher</a> (that is what selects the debug OS
and writes the file), and set the remote address there. To point an existing debug stick at a different PC,
use the flasher's <strong>Reconfigure</strong> button: it rewrites <code>DEBUG.CFG</code> in place without
erasing the disk, so you are not reflashing the whole image just to change an IP.</p>
<p>If you would rather not hardcode an address at all, set <code>discover</code> instead of <code>remote=</code>:
the device broadcasts on the LAN and any PC running the listener answers with its address, so the box finds
you automatically. And when the machine has <em>no working network</em> - because the NIC is the very thing you
are debugging - the link can ride a serial cable instead; see
<a href="#serial">when the only network is the one you are debugging</a> below.</p>
{note('The link runs on its <strong>own</strong> network connection, so the Browser and AI apps keep working while a link is active - they are no longer mutually exclusive.', title="Runs alongside everything else")}

<h2 id="serial">When the only network is the one you are debugging</h2>
<p>The link normally rides TCP over the LAN - but that assumes a working NIC, and the hardest bring-up cases are
exactly the ones where the machine's <em>only</em> network card is the one that does not work yet. For that, the
same channel runs over a <strong>serial cable</strong> instead: no network required. Every command works
identically; only the wire underneath changes. Arm it in <code>DEBUG.CFG</code> with <code>remote-serial</code>
in place of <code>remote=</code>, and point the tool at the serial port:</p>
<pre><code># on the stick: URC over COM1, or name another port (2f8 = COM2, 3e8 = COM3)
remote-serial
remote-serial=3e8

# on your PC (needs pyserial):
python tools/unoauto_remote.py --serial COM3            # Windows
python tools/unoauto_remote.py --serial /dev/ttyUSB0    # Linux</code></pre>
{note('Debug builds stay attached to firmware, and a firmware serial console can steal bytes from whichever port URC is using. Put URC on a port the firmware is <em>not</em> consoling - <strong>COM3</strong> is the safe default; if unsure, disable serial console redirection in firmware setup. Serial is for interactive control (register debug, driving the desktop); do the multi-megabyte A/B pushes over TCP.', kind="warn", title="Pick a non-console serial port")}

<h2 id="logging">Remote logging</h2>
<p>While the link is up, every line the OS logs - boot, network, storage, UI, script output - streams to
your PC as it happens. Instead of pulling a stick and reading <code>CRASH\\NETLOG.TXT</code> after the fact,
you watch the machine think in real time. Nothing in the OS changes to make this happen; the remote channel
simply subscribes to the same log stream the on-disk logs use.</p>

<h2 id="commands">The command language</h2>
<p>Either end can send the other a command or a free-form message. The commands your PC can run on the
device are short, human-typable lines - you can even reach them with <code>nc</code>:</p>
<div class="tw"><table>
<thead><tr><th>Command</th><th>What it does</th></tr></thead>
<tbody>
<tr><td><code>probe</code></td><td>A snapshot of what the system is doing: subsystems (heap/net/fs/shell), open windows, loaded apps.</td></tr>
<tr><td><code>vols</code></td><td>List storage volumes: index, kind (RAM / native-FAT / firmware), whether it is writable, and its name.</td></tr>
<tr><td><code>key</code> / <code>pointer</code></td><td>Inject a keypress or a pointer event, processed exactly like a human's on the next frame.</td></tr>
<tr><td><code>screen</code></td><td>Grab the desktop as a compressed image - the video feed behind the <a href="#unoremote">UnoRemote</a> remote-desktop client.</td></tr>
<tr><td><code>apps</code> / <code>apps list</code></td><td>How many apps there are, and what they are: one <code>id name</code> row each. The set depends on what is installed, so a script that wants to drive one has to be able to look it up.</td></tr>
<tr><td><code>launch &lt;id&gt;</code> / <code>rescan</code> / <code>close</code></td><td>Open an app <strong>by its id</strong>, pick up apps that have appeared in <code>APPS\\</code> since boot, or close the top window. <code>launch</code> still takes a slot number, but a number is this boot's ordering of whatever happens to be installed - install one app and the same number opens a different one, without failing. Use the id.</td></tr>
<tr><td><code>py &lt;source&gt;</code></td><td>Run a line of Python <em>on the device</em> and get its output back.</td></tr>
<tr><td><code>test &lt;suite&gt;</code></td><td>Run a built-in conformance suite and stream the report.</td></tr>
<tr><td><code>uptime</code> / <code>poweroff</code> / <code>reboot</code></td><td>Read the uptime, or shut down / restart the machine.</td></tr>
<tr><td><code>put</code> / <code>bootnext</code></td><td>Write a file to a volume and pick the next boot device - the A/B update below.</td></tr>
<tr><td><code>guard &lt;secs&gt;</code> / <code>pet</code> / <code>safe</code></td><td>Arm a dead-man's switch before a risky command: if the box stops answering within the timeout, it resets itself and dials back in - the guard below.</td></tr>
<tr><td><code>devices</code></td><td>List the machine's PCI devices and which driver claimed each one (or <code>UNCLAIMED</code>) - what hardware is here and what has no driver yet.</td></tr>
<tr><td><code>hwwdt &lt;status|arm|pet|disarm&gt;</code></td><td>Read or drive the chipset's PCH TCO hardware watchdog - the guard's last-resort backstop for a wedge that has interrupts disabled. <code>status</code> reports whether a usable one was found; the rest drive it directly.</td></tr>
<tr><td><code>iwl</code> / <code>eth</code></td><td>Poke a live network driver's registers - Intel Wi-Fi / Realtek Ethernet - reading and writing registers or retrying its bring-up, with no rebuild.</td></tr>
<tr><td><code>disks</code> / <code>arm</code> / <code>prepdisk</code></td><td>List raw disks, arm one for a destructive op (it refuses the boot disk and echoes the target's size), and partition + format it - preparing a disk to install onto.</td></tr>
<tr><td><code>mkdir</code> / <code>install</code></td><td>Create a directory on a volume, or clone the running OS onto a prepared disk in one armed step - the headless install below.</td></tr>
</tbody>
</table></div>
<p class="muted">These are the everyday commands; the exhaustive list, with every argument and exact reply, is in
<a href="https://github.com/hmofet/unodos/blob/master/pc64/REMOTE.md" target="_blank" rel="noopener"><code>pc64/REMOTE.md</code></a>.</p>

<h2 id="host">The tool on your PC</h2>
<p>One small script, <code>pc64/tools/unoauto_remote.py</code>, is both a listener you talk to interactively and
a Python library you script against. Run it, and it prints the device's logs as they arrive and lets you type
commands back:</p>
{CODE_REMOTE_CLI}
<p>The same thing as a library - drive the machine, read its state, run code on it, and register handlers so
the device can drive <em>your</em> PC in return:</p>
{CODE_REMOTE_PY}
<p>On the device side, an automation script written in Python (see <a href="dev-apps.html">Writing apps</a>) can
talk back over the link with <code>unoauto.remote_send()</code> and <code>unoauto.remote_recv()</code>.</p>

<h2 id="unoremote">Remote desktop (UnoRemote)</h2>
<p>When you want to <em>see and drive</em> the machine rather than type commands at it, <strong>UnoRemote</strong>
is a Windows app that gives you a live remote-desktop view over the same link: the device's screen in a window,
your mouse and keyboard forwarded to it, a log pane, a clickable command bar, a raw-command box and a session
recorder. It is VNC-style - the desktop streams over the <code>screen</code> command and input goes back over
<code>key</code> and <code>pointer</code> - but tuned for UnoDOS's flat desktop: each frame sends only the tiles
that changed since the last one, so the view stays responsive even at full resolution.</p>
<ol>
  <li>Build it once with <code>pc64\\remote\\build-remote.ps1</code> (it produces <code>UnoRemote.exe</code>), run it,
      set the port (default <strong>5099</strong>) and click <strong>Listen</strong>.</li>
  <li>On the device, either point <code>DEBUG.CFG</code> at your PC with <code>remote=&lt;your-ip&gt;:5099</code>, or
      just set <code>discover</code> and let the device find your PC on its own (see the tip below); then boot a debug
      build. The UnoDOS desktop appears in the window.</li>
  <li>Click and type on the view to drive the machine. A <strong>Scale</strong> control (1&times;&ndash;4&times;)
      trades resolution for bandwidth on a busy or high-resolution screen.</li>
  <li>The <strong>command bar</strong> under the log runs the everyday remote actions with a click - list volumes and
      disks, launch or close an app, check uptime, reboot or power off (these ask first) - while the box beside it
      still takes any raw URC command line.</li>
</ol>
<p><strong>Recording.</strong> <strong>Record</strong> saves the session to <code>Videos\\UnoRemote\\</code> - an MP4
if <code>ffmpeg</code> is on your PATH, otherwise a folder of PNG frames. Tick <strong>on device</strong> first to
record <em>on the machine itself</em>: it captures at a steady frame rate independent of the network and UnoRemote
pulls the finished recording when you stop, which is smoother than saving whatever trickled over a slow link.</p>
{note('<strong>No address to type.</strong> With <code>discover</code> set in <code>DEBUG.CFG</code> (instead of a <code>remote=</code> address) the device broadcasts on the local network, and UnoRemote - or the <code>unoauto_remote.py</code> tool - answers with its own address, so the device connects with nothing configured. Both ends must be on the same LAN.', kind="tip", title="Zero-config discovery")}

<p><strong>Which end dials - and the Scan button.</strong> By default the <em>device</em> dials your
PC: you click <strong>Listen</strong> and it connects in. That suits a headless box whose address you
do not know - it calls home when its network is up. You can also flip it: put <code>listen</code> (or
<code>listen=&lt;port&gt;</code>) in the device's <code>DEBUG.CFG</code> and the box becomes a
<strong>server</strong> that waits to be dialed. Then click <strong>Scan&hellip;</strong> in UnoRemote:
it broadcasts on the LAN, lists every box in listen mode by name, and dials the one you pick. That is
the "browse the network and connect to a box" model - you choose the machine, with no address typed on
either end. (Give a box a friendly name with <code>name=&lt;label&gt;</code> so it is easy to spot in
the list.)</p>
{note('The network comes up <strong>at boot</strong> now, not only when an app first needs it: every device brings its NIC up early (skipping any that do not get a lease within a few seconds, so a dead or cableless port cannot hang boot), so a box is reachable - and discoverable - as soon as the desktop appears.', kind="tip", title="Networking is ready at boot")}
{note('UnoRemote rides the same LAN-only URC channel as everything else on this page - it needs the channel armed (the <a href="#arming">Remote control panel</a> on a production OS, <code>DEBUG.CFG</code> on a debug one) and a trusted network.', kind="warn", title="Armed channel, LAN only")}

<h2 id="unoscript">Scripting the OS, with permission (<code>unoscript</code>)</h2>
<p>The <a href="dev-python.html#uno">
<code>uno</code> module</a> is an app's own sandbox: its window, its canvas, its files. <strong><code>unoscript</code></strong>
is the step up from there - a surface for scripting the <em>whole machine</em>. Through <code>import unoscript as u</code>
a script can move the pointer and type, read what is on screen, launch and close apps, see what is running, read and
write the user's files, and - with permission - reach all the way down to memory, ports and power. It is how you
<em>automate the OS</em>, not just write an app inside it. You reach it interactively through the <code>py</code>
command above, or from an on-device automation app.</p>

<p>Every action needs a <strong>capability</strong>, and capabilities are <strong>tiered</strong>. A script begins
with only the ambient tier; anything higher it must ask for with <code>u.request("&lt;cap&gt;")</code>, and the
security subsystem decides - drawing the same consent sheet the login screen uses, honouring a role the signed-in
user holds, or, on a developer machine, an auto-grant policy. A denied action raises <code>PermissionError</code>;
a surface a given build does not carry raises <code>NotImplementedError</code>. This is the same gate the
<a href="getting-started.html">login screen</a> and Accounts manager enforce, so a script can never quietly do more
than the person running it is allowed to.</p>

<div class="tw"><table>
<thead><tr><th>Namespace</th><th>Tier</th><th>What it scripts</th></tr></thead>
<tbody>
<tr><td><code>u.ui</code></td><td>ambient</td><td>Move / click the pointer, press keys, read the on-screen window text, and the shared clipboard.</td></tr>
<tr><td><code>u.app</code></td><td>ambient / user</td><td>Count, launch and close apps; send an app a message (focus, close, ask its state).</td></tr>
<tr><td><code>u.fs</code></td><td>user / admin</td><td>Read and write files. A plain name is the user's own home (<code>USERS/&lt;id&gt;/…</code>); an absolute <code>/volume/path</code> reaches elsewhere and needs the higher tier.</td></tr>
<tr><td><code>u.proc</code></td><td>admin</td><td>List what is running - each open app as a process, with its name and which is focused.</td></tr>
<tr><td><code>u.mem</code> / <code>u.io</code></td><td>admin / kernel</td><td>Read and write raw memory and I/O ports - a kernel-level debugging surface, always audited.</td></tr>
<tr><td><code>u.sys</code></td><td>admin</td><td>Power: shut down or restart the machine.</td></tr>
<tr><td><code>u.hook</code></td><td>admin</td><td>Watch internal events (file writes, module loads) stream past - a debug-build observability tap.</td></tr>
</tbody>
</table></div>

<p>A short session over the <code>py</code> command, escalating as it goes:</p>
<pre><code># ambient - no permission needed: read the screen, drive the pointer
py import unoscript as u; print(u.ui.screen())
py import unoscript as u; u.ui.click(200, 160)

# the user's own files - u.fs.read/write ask for the 'fs.user' capability first
py import unoscript as u; u.request("fs.user"); u.fs.write("notes.txt", b"hello")
py import unoscript as u; u.request("fs.user"); print(u.fs.read("notes.txt"))

# 'what is running' is an admin surface - unescalated it is refused
py import unoscript as u; print(u.proc.list())        # -&gt; PermissionError
py import unoscript as u; u.request("proc.enum"); print(u.proc.list())</code></pre>

{note('The <code>unoscript</code> surface itself is <strong>production</strong> - a trusted, signed automation app can use it on a normal machine, always under the permission gate. Only <code>u.hook</code> is debug-only (a production tap on hot internal events would cost every machine that ships), and the deep <code>u.mem</code>/<code>u.io</code> surfaces are kernel-tier: strongest escalation, every use audited. The full capability list and tiers are in <a href="https://github.com/hmofet/unodos/blob/master/pc64/UNOSCRIPT.md" target="_blank" rel="noopener"><code>UNOSCRIPT.md</code></a>.', title="Production surface, gated by permission")}

<h3 id="automation">Automation apps: caps without prompts (signed manifests)</h3>
<p>Asking for a capability with <code>u.request()</code> works for a script <em>you</em> are driving - the
system can draw a consent sheet and you click Allow. But an <strong>automation app</strong> runs with nobody
watching, so there is no one to answer the prompt. A trusted one instead ships a <strong>signed manifest</strong>:
a small <code>&lt;app&gt;.MFT</code> file next to its <code>.UNO</code> that declares the caps it needs, signed by a
key the machine trusts. At launch UnoDOS verifies the signature and grants exactly those caps for the life of the
app - no prompts - and drops them again the moment it closes.</p>
<pre><code># once: make a signing key and the line that enrolls it on the machine
python tools/uno_manifest.py keygen --key-id acme &gt; acme.line
#   -&gt; prints  "acme &lt;64 hex&gt;"  ; append that line to \\TRUST.MFK on the boot disk

# per app: sign a manifest declaring what it needs, beside the .UNO
python tools/uno_manifest.py sign --name mybot --caps proc.enum,fs.sys \\
    --key-id acme --secret &lt;64 hex&gt; -o MYBOT.MFT</code></pre>
{note('The manifest is a convenience for <em>trusted</em> apps, never a way around the gate: an unsigned or untrusted-key manifest grants nothing (the app just runs with ordinary user authority), a kiosk machine refuses all of it, and every grant is audited. Enrolling a key in <code>TRUST.MFK</code> is what says &ldquo;I trust apps this key signs&rdquo; - guard it like any signing key.', title="Trust is the whole point")}

<h2 id="ab">A/B updates: push a new build over the link</h2>
<p>The headline use: iterate on the OS itself without touching a USB stick. Run <strong>two</strong> sticks -
<strong>A</strong>, the machine you are working on, and <strong>B</strong>, a spare - and push only a freshly
built kernel (<code>EFI\\BOOT\\BOOTX64.EFI</code>, about 4&nbsp;MB) to stick B over the wire, then reboot into
it. A driver change touches only that one file; everything else on the stick is untouched, and stick A stays as
a known-good fallback.</p>
{CODE_REMOTE_PUSH}
<p>The file is streamed in chunks, staged in the device's memory, and written in one step at the end with a size
check - so an interrupted transfer never leaves stick B half-written. Add <code>--bootnext &lt;n&gt;</code> to have
the machine boot the other stick automatically on the next restart, without anyone touching the firmware boot
menu. From the library the same flow is <code>link.push_file(...)</code> then <code>link.reboot()</code>.</p>

{note('<code>put</code> and <code>reboot</code> are an arbitrary file write and a reset. In a production OS they sit behind the <a href="#arming">Full access</a> power; grant it only over your trusted LAN. A single push is capped at 8&nbsp;MB.', kind="warn", title="What these can do")}

<h2 id="install">Installing UnoDOS onto a disk over the link</h2>
<p>The A/B push writes files onto an <em>already-formatted</em> stick. The channel can go further and stand up a
bootable disk from scratch - partition a raw disk, format it, and lay the whole OS down on it - so you can move
UnoDOS off the USB stick and onto an internal drive without ever touching the machine's keyboard. The one-shot
<code>install</code> command does all of it, cloning the running system straight onto the target:</p>
<pre><code>disks                # find the writable, non-boot target (say index 1)
arm 1                # echoes the disk's name and size; refuses the boot disk
install 1            # partition + format, then clone the whole OS onto it
reboot               # writes are flushed to disk first</code></pre>
<p>Because the copy is disk-to-disk on the device, none of the OS actually crosses the network. From the Python
library the same thing is <code>link.install(1)</code>, or <code>link.install_dir(1, "build/esp")</code> to push a
freshly built tree from your PC instead of cloning the running one. The lower-level pieces are there too if you
want them - <code>prepdisk</code> to format, <code>mkdir</code> + <code>put</code> to build the tree by hand.</p>
{note('A <strong>USB stick</strong> installed this way boots on its own (the firmware removable-media fallback finds it). An <strong>internal disk</strong> is made bootable the same way, but writing a permanent firmware boot entry needs the on-device Install app booted to firmware - over the link, an internal disk boots via the firmware fallback or a one-time boot-menu pick. Every destructive step is inert until you <code>arm</code> the specific disk, which auto-disarms after one op and always refuses the disk UnoDOS booted from.', kind="warn", title="Booting the installed disk")}

<h2 id="guard">The guard: recover a wedged box automatically</h2>
<p>Some commands push the device into code that has never run before - driving a network card's bring-up by hand
is the classic case - and when that goes wrong it can <em>wedge</em> the machine: the remote channel stops
answering and normally the only way back is to walk over and power-cycle it. The <strong>guard</strong> turns
that into automatic recovery. Arm it right before the risky command, and if the box cannot answer the channel
within your timeout, it resets itself and dials back in on its own.</p>
<pre><code># arm a 15-second dead-man's switch, then run the risky command
guard 15
&lt;the risky command&gt;
# if the box wedges, ~15s later it resets and reconnects by itself;
# if the command returns fine, stand the guard down:
safe</code></pre>
<p>&quot;Answering the channel&quot; means <em>any</em> command getting through - each one you send pushes the
deadline out, so an ordinary interactive session keeps the guard happy without thinking about it. For a step
that legitimately takes a while, send <code>pet</code> as a keep-alive. From the Python library it is one
context manager that arms on the way in and stands down on the way out:</p>
<pre><code>with link.guarded(15):
    link.command(&quot;iwl&quot;, &quot;rerun&quot;)   # a wedge here resets the box, which reconnects</code></pre>
{note('An armed guard <strong>will</strong> reset a perfectly healthy machine if it simply stops hearing from you (your PC goes to sleep, the network drops) - that is the whole point, so arm it around a specific risky step and <code>safe</code> it afterwards, or use <code>guarded(...)</code>, which does that for you. Debug builds only, like the rest of the channel.', kind="warn", title="It fires on silence, not just crashes")}

<h3 id="hwwdt">The hardware backstop</h3>
<p>The guard normally resets a wedged box from software - a heartbeat in the main loop, a timer interrupt, or the
firmware watchdog. All three need the CPU to still be taking interrupts. The one failure they cannot catch is a
tight loop that has turned interrupts <em>off</em> (or a true bus hang): nothing runs, so nothing resets the machine.
For that last case UnoDOS can enlist separate silicon that does not care what the CPU is doing - the Intel
<strong>PCH TCO hardware watchdog</strong>. When a usable one is present the guard arms it automatically, a little
past the software timeout, so it only ever fires on the wedge software cannot reach. You can also drive it directly
to check it is there:</p>
<pre><code>hwwdt status      # is a usable TCO present on this chipset? which registers?
hwwdt arm 20      # arm it for ~20s; if nothing pets it, the chipset resets the box
hwwdt disarm</code></pre>
{note('The hardware watchdog is only available where UnoDOS knows how to reach that chipset&rsquo;s registers <em>and</em> the firmware has not locked the timer. <code>hwwdt status</code> reports honestly: it says present only when it has proven the watchdog can actually fire. Where it cannot (some laptop firmwares lock it), the software guard still covers every wedge except the interrupts-off one. See <a href="https://github.com/hmofet/unodos/blob/master/pc64/HWWATCHDOG.md" target="_blank" rel="noopener"><code>HWWATCHDOG.md</code></a>.', title="When it is available")}

<h2 id="setup">Setting up a development environment</h2>
<p>Putting the pieces together, a comfortable unoautomate workflow needs a one-time setup and then a tight
loop:</p>
<ol>
  <li><strong>Two sticks.</strong> Flash <strong>two</strong> USB sticks with <em>Developer options</em> on:
      <strong>A</strong>, a known-good build you keep as a fallback, and <strong>B</strong>, the spare you push
      new builds to. Set <code>remote=&lt;your-pc-ip&gt;:5099</code> on both when you flash them (or add it later
      with the flasher's <strong>Reconfigure</strong> button - no reflash).</li>
  <li><strong>The bench machine boots stick B</strong> and, once its network comes up, dials your PC. The
      driver box builds attached (<code>-DUNO_NO_DETACH</code>) so its own USB stick shows up as a writable
      volume you can push to.</li>
  <li><strong>Run the listener on your PC:</strong> <code>python tools/unoauto_remote.py --listen 0.0.0.0:5099</code>.
      Its logs start streaming the moment the device connects.</li>
  <li><strong>Edit, build, push, reboot.</strong> Change a driver, rebuild <code>BOOTX64.EFI</code>, push it to
      stick B, and reboot into it - seconds, no walking to the bench. Stick A is always there if a build will
      not boot.</li>
</ol>
<p>The whole loop scripts cleanly from the Python library, so you can wrap it in one keystroke:</p>
{CODE_REMOTE_LOOP}

<h2 id="usecases">What you can do with it</h2>

<h3 id="uc-driver">Driver development</h3>
<p>This is the case unoautomate was built for. Bringing up a driver for real hardware - a NIC, a Wi-Fi chip,
a storage controller - is a cycle of "change one thing, watch what the hardware does, change it again", and
without a channel each turn means pulling a stick, reflashing it, walking it to the bench, booting, and
reading a log file after the fact. unoautomate collapses that:</p>
<ul>
  <li><strong>Watch the bring-up live.</strong> Every line the driver logs - each register write, each
      firmware handshake, exactly where it stalls - streams to your PC as it happens, instead of being read
      from <code>CRASH\\NETLOG.TXT</code> after a hang.</li>
  <li><strong>Reflash in seconds, not minutes.</strong> The A/B push writes only the ~4&nbsp;MB kernel to the
      spare stick and reboots into it; a driver change touches one file and the round trip is a keystroke.</li>
  <li><strong>Poke the hardware without a rebuild.</strong> Run a line of Python on the device with
      <code>py</code> / <code>link.eval()</code> to read back state between builds, and <code>probe</code> to
      confirm which subsystems actually came up. A debug build is exactly where you want this reach.</li>
  <li><strong>Read and write live registers.</strong> For a network card, <code>iwl</code> (Intel Wi-Fi) and
      <code>eth</code> (Realtek Ethernet) reach straight into the running driver - peek and poke registers,
      retry the bring-up sequence - so you can try a fix in seconds and only rebuild once you know it works.
      Pair it with a <a href="#guard">guard</a> so a bad poke that hangs the card resets the box instead of
      stranding it.</li>
  <li><strong>See what hardware is actually there.</strong> <code>devices</code> dumps the machine's whole
      PCI tree - every function's location, IDs, class and which driver claimed it - so the first question of
      any bring-up, "what is on this box and what has no driver yet?", is answered on-device instead of guessed
      from a spec sheet. The same registry is a Python call away with <code>uno.pci()</code> for a script to
      filter.</li>
</ul>
<p>The Intel Wi-Fi bring-up is the working example: the driver is iterated this way, its firmware-load
sequence streamed back register by register while builds are pushed to the bench machine over the link.</p>

<h3 id="uc-test">Automated conformance testing</h3>
<p>A debug OS carries built-in conformance suites (<code>storage</code>, <code>system</code>,
<code>network</code>, <code>frameworks</code>, <code>apps</code>). Run one from your PC and stream its report,
then assert on a live <code>probe()</code> - a real regression test you can put in CI or run after every push:</p>
{CODE_REMOTE_TEST}

<h3 id="uc-ui">Driving the desktop headlessly</h3>
<p>Because <code>key</code> and <code>pointer</code> are injected exactly as a human's input on the next frame,
you can drive the whole desktop from a script: launch an app, type into it, walk a menu, and check the result -
no monitor or keyboard attached. This is how the screenshots in this manual are produced (see
<a href="dev-build.html#shots">Regenerate the manual screenshots</a>): the harness boots the OS and drives it
entirely over the channel.</p>

<h3 id="uc-triage">Unattended runs and remote triage</h3>
<p>Leave a machine running a soak test on the bench, watch its log stream from your desk, and have it
<code>poweroff</code> itself when it finishes or <code>reboot</code> when it wedges. Wrap a run that might hang in a
<a href="#guard">guard</a> and it recovers on its own - no one has to be in the room to power-cycle it. For an
intermittent bug you no longer reproduce-then-lose-the-evidence: the log is already on your PC when it happens.</p>

<p class="muted">The full wire protocol and every command's exact reply are in
<a href="https://github.com/hmofet/unodos/blob/master/pc64/REMOTE.md" target="_blank" rel="noopener"><code>pc64/REMOTE.md</code></a>.</p>
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
