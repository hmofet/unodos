/* ===========================================================================
 * plat_win32.c - the desktop shell on Windows, straight on Win32.
 *
 *   window + blit   RegisterClassW / CreateWindowExW, and SetDIBitsToDevice
 *                   of our framebuffer (a DIB wants B,G,R,X; fb_px is R,G,B,A,
 *                   so each present swaps into a DIB-shaped copy)
 *   input           WM_KEYDOWN / WM_CHAR / WM_*BUTTON* / WM_MOUSEWHEEL
 *   services        SetWindowTextW, a borderless monitor-sized window for
 *                   fullscreen, GetOpenFileNameW / GetSaveFileNameW (the
 *                   Explorer-style common dialog), GetModuleFileNameW
 *
 * The window procedure only QUEUES events; plat_wait hands them to the shell
 * one at a time, so nothing in the apps ever runs inside a message callback
 * except a file dialog, which is modal by nature.
 * ======================================================================== */
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601          /* Windows 7+: GetTickCount64, MAPVK_VK_TO_CHAR */
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "uodesk_plat.h"

static HWND       g_hwnd;
static plat_event g_q[256];
static int        g_qh, g_qt;

static unsigned char *g_dib;          /* the last frame, B,G,R,X             */
static int        g_dw, g_dh, g_dcap;

static int        g_full;
static WINDOWPLACEMENT g_place = { sizeof(WINDOWPLACEMENT) };
static DWORD      g_style;
static WCHAR      g_hi;               /* a UTF-16 high surrogate awaiting its pair */
static char       g_base[MAX_PATH * 3];

static void push(const plat_event *e)
{
    int n = (g_qt + 1) % 256;
    if (n != g_qh) { g_q[g_qt] = *e; g_qt = n; }
}

static int mods_now(void)
{
    int m = 0;
    if (GetKeyState(VK_SHIFT)   & 0x8000) m |= PM_SHIFT;
    if (GetKeyState(VK_CONTROL) & 0x8000) m |= PM_CTRL;
    if (GetKeyState(VK_MENU)    & 0x8000) m |= PM_ALT;
    if ((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000) m |= PM_GUI;
    return m;
}

static int vk_to_key(WPARAM vk)
{
    switch (vk) {
    case VK_UP: return PK_UP;       case VK_DOWN: return PK_DOWN;
    case VK_RIGHT: return PK_RIGHT; case VK_LEFT: return PK_LEFT;
    case VK_HOME: return PK_HOME;   case VK_END: return PK_END;
    case VK_INSERT: return PK_INSERT; case VK_DELETE: return PK_DELETE;
    case VK_PRIOR: return PK_PGUP;  case VK_NEXT: return PK_PGDN;
    case VK_ESCAPE: return PK_ESC;  case VK_RETURN: return PK_ENTER;
    case VK_BACK: return PK_BACKSPACE; case VK_TAB: return PK_TAB;
    default: break;
    }
    if (vk >= VK_F1 && vk <= VK_F12) return PK_F1 + (int)(vk - VK_F1);
    return PK_NONE;
}

static void paint(HDC dc)
{
    BITMAPINFO bi;
    RECT r;
    if (!g_dib) return;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = g_dw;
    bi.bmiHeader.biHeight = -g_dh;                   /* top-down rows          */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    GetClientRect(g_hwnd, &r);
    if (r.right == g_dw && r.bottom == g_dh)
        SetDIBitsToDevice(dc, 0, 0, (DWORD)g_dw, (DWORD)g_dh, 0, 0, 0, (UINT)g_dh,
                          g_dib, &bi, DIB_RGB_COLORS);
    else {                    /* a frame from before a resize: scale it once */
        SetStretchBltMode(dc, COLORONCOLOR);
        StretchDIBits(dc, 0, 0, r.right, r.bottom, 0, 0, g_dw, g_dh,
                      g_dib, &bi, DIB_RGB_COLORS, SRCCOPY);
    }
}

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    plat_event e;
    memset(&e, 0, sizeof e);
    switch (msg) {
    case WM_CLOSE:
        e.type = PE_QUIT; push(&e);
        return 0;                              /* the shell decides; not DefWindowProc */
    case WM_SIZE:
        if (wp != SIZE_MINIMIZED) {
            e.type = PE_RESIZE; e.w = LOWORD(lp); e.h = HIWORD(lp); push(&e);
        }
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        paint(dc);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;                              /* every pixel is ours: no flicker */
    case WM_KEYDOWN: case WM_SYSKEYDOWN: {
        int k = vk_to_key(wp), m = mods_now();
        if (msg == WM_SYSKEYDOWN && wp == VK_F4) break;       /* Alt+F4: close */
        if (k != PK_NONE) {
            e.type = PE_KEY; e.key = k; e.mods = m; push(&e);
            return 0;
        }
        /* a character key under Ctrl (not AltGr = Ctrl+Alt): an accelerator.
         * WM_CHAR would only bring a control code, so name the key here. */
        if ((m & PM_CTRL) && !(m & PM_ALT)) {
            UINT c = MapVirtualKeyW((UINT)wp, MAPVK_VK_TO_CHAR) & 0x7FFF;
            if (c >= 'A' && c <= 'Z') c += 32;
            if (c >= 32 && c < 127) {
                e.type = PE_KEY; e.key = PK_CHAR; e.ch = (int)c; e.mods = m; push(&e);
                return 0;
            }
        }
        break;
    }
    case WM_CHAR: case WM_SYSCHAR: {
        WCHAR wc = (WCHAR)wp;
        WCHAR pair[2];
        int n = 1, len;
        if (msg == WM_SYSCHAR) break;          /* Alt+letter: menu territory */
        if (wc >= 0xD800 && wc <= 0xDBFF) { g_hi = wc; return 0; }
        if (wc >= 0xDC00 && wc <= 0xDFFF) {
            if (!g_hi) return 0;
            pair[0] = g_hi; pair[1] = wc; n = 2; g_hi = 0;
        } else pair[0] = wc;
        if (n == 1 && wc < 32) return 0;       /* Enter/Tab/Bksp came as keys */
        {
            int m = mods_now();
            if ((m & PM_CTRL) && !(m & PM_ALT)) return 0;    /* accelerator  */
            len = WideCharToMultiByte(CP_UTF8, 0, pair, n, e.text,
                                      (int)sizeof e.text - 1, 0, 0);
            if (len <= 0) return 0;
            e.text[len] = 0;
            e.type = PE_TEXT; e.mods = m; push(&e);
        }
        return 0;
    }
    case WM_MOUSEMOVE:
        e.type = PE_MOUSE_MOVE; e.x = GET_X_LPARAM(lp); e.y = GET_Y_LPARAM(lp);
        e.mods = mods_now(); push(&e);
        return 0;
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN:
    case WM_LBUTTONUP:   case WM_RBUTTONUP:   case WM_MBUTTONUP: {
        int down = msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MBUTTONDOWN;
        e.type = down ? PE_MOUSE_DOWN : PE_MOUSE_UP;
        e.x = GET_X_LPARAM(lp); e.y = GET_Y_LPARAM(lp);
        e.button = (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP) ? 0
                 : (msg == WM_RBUTTONDOWN || msg == WM_RBUTTONUP) ? 1 : 2;
        e.mods = mods_now();
        if (down) SetCapture(h); else ReleaseCapture();   /* drags leave the window */
        push(&e);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        static int acc;                        /* high-resolution wheels: sum  */
        acc += GET_WHEEL_DELTA_WPARAM(wp);
        while (acc >= WHEEL_DELTA || acc <= -WHEEL_DELTA) {
            int s = acc > 0 ? 1 : -1;
            e.type = PE_WHEEL; e.wheel = -s; e.mods = mods_now(); push(&e);
            acc -= s * WHEEL_DELTA;
        }
        return 0;
    }
    default: break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static WCHAR *wide(const char *s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, 0, 0);
    WCHAR *w = (WCHAR *)malloc((size_t)(n > 0 ? n : 1) * sizeof(WCHAR));
    if (!w) return 0;
    if (n <= 0) { w[0] = 0; return w; }
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}

int plat_init(const char *title, int w, int h, int hidden)
{
    WNDCLASSW wc;
    HINSTANCE inst = GetModuleHandleW(0);
    RECT r = { 0, 0, w, h };
    WCHAR *wt;

    memset(&wc, 0, sizeof wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));   /* app.rc's icon */
    wc.hCursor = LoadCursorW(0, (LPCWSTR)IDC_ARROW);
    wc.lpszClassName = L"UnoOffice";
    if (!RegisterClassW(&wc)) { fprintf(stderr, "RegisterClass failed\n"); return 0; }

    g_style = WS_OVERLAPPEDWINDOW;
    AdjustWindowRect(&r, g_style, FALSE);
    wt = wide(title);
    g_hwnd = CreateWindowExW(0, L"UnoOffice", wt ? wt : L"UnoOffice", g_style,
                             CW_USEDEFAULT, CW_USEDEFAULT,
                             r.right - r.left, r.bottom - r.top, 0, 0, inst, 0);
    free(wt);
    if (!g_hwnd) { fprintf(stderr, "CreateWindow failed\n"); return 0; }
    if (!hidden) { ShowWindow(g_hwnd, SW_SHOWNORMAL); UpdateWindow(g_hwnd); }
    return 1;
}

void plat_shutdown(void)
{
    if (g_hwnd) DestroyWindow(g_hwnd);
    g_hwnd = 0;
    free(g_dib); g_dib = 0;
}

int plat_wait(plat_event *e, int ms)
{
    MSG m;
    if (g_qh == g_qt) {
        if (ms > 0 && !PeekMessageW(&m, 0, 0, 0, PM_NOREMOVE))
            MsgWaitForMultipleObjects(0, 0, FALSE, (DWORD)ms, QS_ALLINPUT);
        while (g_qh == g_qt && PeekMessageW(&m, 0, 0, 0, PM_REMOVE)) {
            if (m.message == WM_QUIT) { plat_event q; memset(&q, 0, sizeof q);
                                        q.type = PE_QUIT; push(&q); break; }
            TranslateMessage(&m);          /* WM_KEYDOWN -> WM_CHAR */
            DispatchMessageW(&m);
        }
    }
    if (g_qh == g_qt) return 0;
    *e = g_q[g_qh];
    g_qh = (g_qh + 1) % 256;
    return 1;
}

void plat_present(const fb_px *px, int w, int h)
{
    HDC dc;
    int need = w * h * 4;
    if (need > g_dcap) {
        free(g_dib);
        g_dib = (unsigned char *)malloc((size_t)need);
        g_dcap = g_dib ? need : 0;
        if (!g_dib) return;
    }
    plat_rgba_to_bgrx(g_dib, px, w * h);
    g_dw = w; g_dh = h;
    dc = GetDC(g_hwnd);
    paint(dc);
    ReleaseDC(g_hwnd, dc);
}

unsigned plat_ticks(void) { return (unsigned)GetTickCount64(); }

/* main()'s argv is in the ANSI code page, where a file called "Resume.doc"
 * with an accent, or anything in Cyrillic, arrives as question marks.  The
 * UTF-16 command line is the real one, so argv is rebuilt from it, as UTF-8,
 * which is what every path in the shell is. */
void plat_args(int *argc, char ***argv)
{
    int n = 0, i;
    LPWSTR *w = CommandLineToArgvW(GetCommandLineW(), &n);
    char **out;
    if (!w || n <= 0) return;
    out = (char **)calloc((size_t)n + 1, sizeof *out);
    if (!out) { LocalFree(w); return; }
    for (i = 0; i < n; i++) {
        int len = WideCharToMultiByte(CP_UTF8, 0, w[i], -1, 0, 0, 0, 0);
        out[i] = (char *)malloc(len > 0 ? (size_t)len : 1);
        if (!out[i]) { LocalFree(w); return; }
        if (len <= 0) out[i][0] = 0;
        else WideCharToMultiByte(CP_UTF8, 0, w[i], -1, out[i], len, 0, 0);
    }
    LocalFree(w);
    *argc = n;
    *argv = out;
}

void plat_set_modified(int on) { (void)on; }

/* ---- the clipboard: CF_UNICODETEXT, with Windows' CR LF line ends ---------- */
int plat_clip_set(const char *utf8)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, 0, 0), i, k = 0, nl = 0;
    WCHAR *w, *d;
    HGLOBAL h;
    if (n <= 0) return 0;
    w = (WCHAR *)malloc((size_t)n * sizeof(WCHAR));
    if (!w) return 0;
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w, n);
    for (i = 0; w[i]; i++) if (w[i] == L'\n') nl++;
    h = GlobalAlloc(GMEM_MOVEABLE, (size_t)(n + nl) * sizeof(WCHAR));
    if (!h) { free(w); return 0; }
    d = (WCHAR *)GlobalLock(h);
    for (i = 0; w[i]; i++) {
        if (w[i] == L'\n' && (i == 0 || w[i - 1] != L'\r')) d[k++] = L'\r';
        d[k++] = w[i];
    }
    d[k] = 0;
    GlobalUnlock(h);
    free(w);
    if (!OpenClipboard(g_hwnd)) { GlobalFree(h); return 0; }
    EmptyClipboard();
    if (!SetClipboardData(CF_UNICODETEXT, h)) { CloseClipboard(); GlobalFree(h); return 0; }
    CloseClipboard();                       /* the clipboard owns h now */
    return 1;
}

long plat_clip_get(char *buf, long cap)
{
    HANDLE h;
    const WCHAR *w;
    WCHAR *lf;
    long n = 0;
    int i, k = 0;
    if (cap > 0) buf[0] = 0;
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT) || !OpenClipboard(g_hwnd)) return 0;
    h = GetClipboardData(CF_UNICODETEXT);
    w = h ? (const WCHAR *)GlobalLock(h) : 0;
    if (w) {
        for (i = 0; w[i]; i++) ;
        lf = (WCHAR *)malloc(((size_t)i + 1) * sizeof(WCHAR));
        if (lf) {
            for (i = 0; w[i]; i++)                   /* CR LF -> LF */
                if (!(w[i] == L'\r' && w[i + 1] == L'\n')) lf[k++] = w[i];
            lf[k] = 0;
            n = WideCharToMultiByte(CP_UTF8, 0, lf, -1, 0, 0, 0, 0) - 1;
            if (n > 0 && cap > 0) {
                if (n < cap) WideCharToMultiByte(CP_UTF8, 0, lf, -1, buf, (int)cap, 0, 0);
                else {                               /* too small: a prefix */
                    char *all = (char *)malloc((size_t)n + 1);
                    if (all) {
                        WideCharToMultiByte(CP_UTF8, 0, lf, -1, all, (int)n + 1, 0, 0);
                        memcpy(buf, all, (size_t)cap - 1);
                        buf[cap - 1] = 0;
                        free(all);
                    }
                }
            }
            free(lf);
        }
        GlobalUnlock(h);
    }
    CloseClipboard();
    return n > 0 ? n : 0;
}

const char *plat_base_path(void)
{
    WCHAR w[MAX_PATH];
    DWORD n = GetModuleFileNameW(0, w, MAX_PATH);
    int i;
    if (!n || n >= MAX_PATH) return 0;
    for (i = (int)n; i > 0 && w[i - 1] != L'\\' && w[i - 1] != L'/'; i--) ;
    w[i] = 0;                                   /* keep the trailing '\'     */
    if (WideCharToMultiByte(CP_UTF8, 0, w, -1, g_base, (int)sizeof g_base, 0, 0) <= 0)
        return 0;
    return g_base;
}

void plat_set_title(const char *utf8)
{
    WCHAR *w = wide(utf8);
    if (w) { SetWindowTextW(g_hwnd, w); free(w); }
}

/* The Raymond Chen fullscreen: drop the frame, cover the monitor, and put
 * the saved placement back afterwards. */
void plat_set_fullscreen(int on)
{
    if (on == g_full || !g_hwnd) return;
    if (on) {
        MONITORINFO mi = { sizeof mi };
        if (!GetWindowPlacement(g_hwnd, &g_place) ||
            !GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTOPRIMARY), &mi))
            return;
        SetWindowLongW(g_hwnd, GWL_STYLE, (LONG)(g_style & ~WS_OVERLAPPEDWINDOW) | WS_POPUP | WS_VISIBLE);
        SetWindowPos(g_hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    } else {
        SetWindowLongW(g_hwnd, GWL_STYLE, (LONG)g_style | WS_VISIBLE);
        SetWindowPlacement(g_hwnd, &g_place);
        SetWindowPos(g_hwnd, 0, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                     SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
    g_full = on;
}
int plat_is_fullscreen(void) { return g_full; }

/* ---- the common file dialog ---------------------------------------------- */
int plat_file_dialog(int save, const char *const *types, int ntypes,
                     char *path, int cap, int *type)
{
    OPENFILENAMEW ofn;
    WCHAR file[MAX_PATH * 4];
    WCHAR filter[2100];                 /* 2040 of types + the All Files tail */
    WCHAR defext[16];
    int k = 0, t, ok;

    /* "Word Document (*.doc)\0*.doc\0...\0All Files (*.*)\0*.*\0\0" */
    defext[0] = 0;
    for (t = 0; t < ntypes && k < 1900; t++) {
        char pats[8][16], spec[160];
        int n = plat_type_patterns(types[t], pats, 8), i, s = 0;
        spec[0] = 0;
        for (i = 0; i < n && s < 140; i++)
            s += snprintf(spec + s, sizeof spec - (size_t)s, "%s%s", i ? ";" : "", pats[i]);
        if (!n) snprintf(spec, sizeof spec, "*.*");
        k += MultiByteToWideChar(CP_UTF8, 0, types[t], -1, filter + k, 2040 - k);
        k += MultiByteToWideChar(CP_UTF8, 0, spec, -1, filter + k, 2040 - k);
        if (t == 0 && n && pats[0][0] == '*' && pats[0][1] == '.')
            MultiByteToWideChar(CP_UTF8, 0, pats[0] + 2, -1, defext, 16);
    }
    memcpy(filter + k, L"All Files (*.*)\0*.*\0\0", 22 * sizeof(WCHAR));

    file[0] = 0;
    memset(&ofn, 0, sizeof ofn);
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = g_hwnd;
    ofn.lpstrFilter = filter;
    ofn.nFilterIndex = 1;
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH * 4;
    ofn.lpstrDefExt = defext[0] ? defext : 0;
    ofn.Flags = OFN_EXPLORER | OFN_HIDEREADONLY | OFN_NOCHANGEDIR |
                (save ? OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST : OFN_FILEMUSTEXIST);
    ok = save ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn);
    if (!ok) return CommDlgExtendedError() ? -1 : 0;   /* error vs Cancel   */
    if (WideCharToMultiByte(CP_UTF8, 0, file, -1, path, cap, 0, 0) <= 0) return 0;
    /* the filter the user left selected, or (on "All Files") the extension */
    if (type) {
        int fi = (int)ofn.nFilterIndex - 1;
        *type = (fi >= 0 && fi < ntypes) ? fi : plat_type_of_path(path, types, ntypes);
    }
    return 1;
}
