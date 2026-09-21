/* ===========================================================================
 * plat_cocoa.m - the desktop shell on macOS, straight on AppKit.
 *
 *   window + blit   one NSWindow whose content view draws our framebuffer as
 *                   an NSBitmapImageRep over the pixels (R,G,B,A in memory is
 *                   exactly AppKit's RGBA layout, so there is no conversion)
 *   input           the view's keyDown: / mouse* / scrollWheel: overrides
 *   services        the window title, native fullscreen (its own Space), an
 *                   application menu with Quit and Enter Full Screen,
 *                   NSOpenPanel / NSSavePanel, the bundle's Resources folder
 *
 * Built with ARC.  The event loop is ours: plat_wait pulls one NSEvent at a
 * time and lets AppKit dispatch it, which lands in the overrides below, which
 * only queue.  Nothing in the apps runs inside an AppKit callback except a
 * file panel, which is modal by nature.
 * ======================================================================== */
#import <Cocoa/Cocoa.h>
#include <string.h>
#include <time.h>

#include "uodesk_plat.h"

static plat_event g_q[256];
static int        g_qh, g_qt;
static NSWindow  *g_win;
static int        g_full;
static unsigned char *g_px;          /* the last frame, RGBA               */
static int        g_pw, g_ph, g_pcap;
static char       g_base[4096];
/* Retina: the view's bounds are POINTS and the frame is PIXELS, backing
 * scale x the bounds, so every glyph lands on a device pixel.  A hidden
 * window (the headless checks) stays at 1. */
static CGFloat    g_bs = 1.0;
static int        g_hidden;
static CGFloat backing(void)
{
    CGFloat s = g_hidden || !g_win ? 1.0 : g_win.backingScaleFactor;
    return s > 0 ? s : 1.0;
}

static void push(const plat_event *e)
{
    int n = (g_qt + 1) % 256;
    if (n != g_qh) { g_q[g_qt] = *e; g_qt = n; }
}

static int mods_of(NSEventModifierFlags f)
{
    int m = 0;
    if (f & NSEventModifierFlagShift)   m |= PM_SHIFT;
    if (f & NSEventModifierFlagControl) m |= PM_CTRL;
    if (f & NSEventModifierFlagOption)  m |= PM_ALT;
    if (f & NSEventModifierFlagCommand) m |= PM_GUI;
    return m;
}

/* virtual key codes (the kVK_* constants of HIToolbox/Events.h) */
static int keycode_to_key(unsigned short k)
{
    switch (k) {
    case 126: return PK_UP;    case 125: return PK_DOWN;
    case 124: return PK_RIGHT; case 123: return PK_LEFT;
    case 115: return PK_HOME;  case 119: return PK_END;
    case 114: return PK_INSERT;                  /* "Help" on old keyboards */
    case 117: return PK_DELETE;                  /* forward delete          */
    case 116: return PK_PGUP;  case 121: return PK_PGDN;
    case 53:  return PK_ESC;   case 36: case 76: return PK_ENTER;
    case 51:  return PK_BACKSPACE; case 48: return PK_TAB;
    case 122: return PK_F1;      case 120: return PK_F1 + 1;  case 99:  return PK_F1 + 2;
    case 118: return PK_F1 + 3;  case 96:  return PK_F1 + 4;  case 97:  return PK_F1 + 5;
    case 98:  return PK_F1 + 6;  case 100: return PK_F1 + 7;  case 101: return PK_F1 + 8;
    case 109: return PK_F1 + 9;  case 103: return PK_F1 + 10; case 111: return PK_F1 + 11;
    default:  return PK_NONE;
    }
}

/* ---- the view: shows the frame, turns events into plat_events ------------ */
@interface UDView : NSView
@end

@implementation UDView {
    CGFloat _wheel;               /* trackpads scroll in fractions of a notch */
}
- (BOOL)isFlipped { return YES; }             /* y grows down, like fb[]  */
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent *)e { (void)e; return YES; }
- (BOOL)isOpaque { return YES; }

- (void)drawRect:(NSRect)dirty
{
    unsigned char *planes[1];
    NSBitmapImageRep *rep;
    (void)dirty;
    if (!g_px) { [[NSColor blackColor] setFill]; NSRectFill(self.bounds); return; }
    planes[0] = g_px;
    rep = [[NSBitmapImageRep alloc]
              initWithBitmapDataPlanes:planes pixelsWide:g_pw pixelsHigh:g_ph
              bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES isPlanar:NO
              colorSpaceName:NSDeviceRGBColorSpace bitmapFormat:0
              bytesPerRow:g_pw * 4 bitsPerPixel:32];
    /* the frame is in pixels, the rect in points: at backing scale 2 each
     * framebuffer pixel is one device pixel (a frame left over from before
     * a scale change is stretched, nearest, until the next one arrives) */
    [NSGraphicsContext currentContext].imageInterpolation = NSImageInterpolationNone;
    [rep drawInRect:NSMakeRect(0, 0, g_pw / g_bs, g_ph / g_bs)
           fromRect:NSZeroRect operation:NSCompositingOperationCopy
           fraction:1.0 respectFlipped:YES hints:nil];
}

static void mouse(NSView *v, NSEvent *ev, int type, int button)
{
    NSPoint p = [v convertPoint:ev.locationInWindow fromView:nil];
    plat_event e;
    memset(&e, 0, sizeof e);
    e.type = type; e.x = (int)(p.x * g_bs); e.y = (int)(p.y * g_bs); e.button = button;
    e.mods = mods_of(ev.modifierFlags);
    push(&e);
}
- (void)mouseDown:(NSEvent *)e         { mouse(self, e, PE_MOUSE_DOWN, 0); }
- (void)mouseUp:(NSEvent *)e           { mouse(self, e, PE_MOUSE_UP, 0); }
- (void)rightMouseDown:(NSEvent *)e    { mouse(self, e, PE_MOUSE_DOWN, 1); }
- (void)rightMouseUp:(NSEvent *)e      { mouse(self, e, PE_MOUSE_UP, 1); }
- (void)otherMouseDown:(NSEvent *)e    { mouse(self, e, PE_MOUSE_DOWN, 2); }
- (void)otherMouseUp:(NSEvent *)e      { mouse(self, e, PE_MOUSE_UP, 2); }
- (void)mouseMoved:(NSEvent *)e        { mouse(self, e, PE_MOUSE_MOVE, 0); }
- (void)mouseDragged:(NSEvent *)e      { mouse(self, e, PE_MOUSE_MOVE, 0); }
- (void)rightMouseDragged:(NSEvent *)e { mouse(self, e, PE_MOUSE_MOVE, 0); }
- (void)otherMouseDragged:(NSEvent *)e { mouse(self, e, PE_MOUSE_MOVE, 0); }

- (void)scrollWheel:(NSEvent *)ev
{
    /* scrollingDeltaY is + when content should move DOWN (towards the top
     * of the document) - that is a notch "up" in unoui's sign */
    CGFloat d = ev.scrollingDeltaY;
    if (ev.hasPreciseScrollingDeltas) d /= 16.0;
    _wheel += d;
    while (_wheel >= 1.0 || _wheel <= -1.0) {
        plat_event e;
        int s = _wheel > 0 ? 1 : -1;
        memset(&e, 0, sizeof e);
        e.type = PE_WHEEL; e.wheel = -s; e.mods = mods_of(ev.modifierFlags);
        push(&e);
        _wheel -= s;
    }
}

- (void)keyDown:(NSEvent *)ev
{
    plat_event e;
    int m = mods_of(ev.modifierFlags), k = keycode_to_key(ev.keyCode);
    memset(&e, 0, sizeof e);
    e.mods = m;
    if (k != PK_NONE) { e.type = PE_KEY; e.key = k; push(&e); return; }
    if (m & (PM_GUI | PM_CTRL)) {           /* Cmd/Ctrl + key: an accelerator */
        NSString *s = ev.charactersIgnoringModifiers.lowercaseString;
        unichar c = s.length ? [s characterAtIndex:0] : 0;
        if (c >= 32 && c < 127) { e.type = PE_KEY; e.key = PK_CHAR; e.ch = c; push(&e); }
        return;
    }
    {
        NSString *s = ev.characters;
        const char *u = s.UTF8String;
        unichar c = s.length ? [s characterAtIndex:0] : 0;
        /* function keys arrive as private-use characters (0xF700..) */
        if (!u || !*u || c < 32 || c == 127 || (c >= 0xF700 && c <= 0xF8FF)) return;
        strncpy(e.text, u, sizeof e.text - 1);
        e.type = PE_TEXT;
        push(&e);
    }
}
/* Without this, Cmd+letter would beep through performKeyEquivalent: */
- (BOOL)performKeyEquivalent:(NSEvent *)ev
{
    if ((ev.modifierFlags & NSEventModifierFlagCommand) &&
        [[NSApp mainMenu] performKeyEquivalent:ev]) return YES;   /* Cmd+Q ... */
    if (ev.modifierFlags & (NSEventModifierFlagCommand | NSEventModifierFlagControl)) {
        [self keyDown:ev];
        return YES;
    }
    return NO;
}
@end

/* ---- the window's delegate: close, resize, fullscreen --------------------- */
@interface UDDelegate : NSObject <NSWindowDelegate>
@end
@implementation UDDelegate
- (BOOL)windowShouldClose:(NSWindow *)w
{
    plat_event e;
    (void)w;
    memset(&e, 0, sizeof e); e.type = PE_QUIT; push(&e);
    return NO;                              /* the shell decides */
}
- (void)windowDidResize:(NSNotification *)n
{
    plat_event e;
    NSSize s = g_win.contentView.bounds.size;
    (void)n;
    memset(&e, 0, sizeof e);
    e.type = PE_RESIZE; e.w = (int)(s.width * g_bs); e.h = (int)(s.height * g_bs);
    push(&e);
}
/* moved between a Retina and a standard display */
- (void)windowDidChangeBackingProperties:(NSNotification *)n
{
    plat_event e;
    NSSize s = g_win.contentView.bounds.size;
    CGFloat b = backing();
    (void)n;
    if (b == g_bs) return;
    g_bs = b;
    memset(&e, 0, sizeof e);
    e.type = PE_SCALE; push(&e);
    e.type = PE_RESIZE; e.w = (int)(s.width * g_bs); e.h = (int)(s.height * g_bs);
    push(&e);
}
- (void)windowDidEnterFullScreen:(NSNotification *)n { (void)n; g_full = 1; }
- (void)windowDidExitFullScreen:(NSNotification *)n  { (void)n; g_full = 0; }
@end

static UDDelegate *g_delegate;

@interface UDApp : NSObject <NSApplicationDelegate>
@end
@implementation UDApp
- (void)quit:(id)sender
{
    plat_event e;
    (void)sender;
    memset(&e, 0, sizeof e); e.type = PE_QUIT; push(&e);
}
/* A document double-clicked in Finder, dropped on the Dock icon or opened
 * with "Open With" arrives HERE, as an Apple event - never in argv, whether
 * we were already running or were launched to open it. */
- (void)application:(NSApplication *)app openURLs:(NSArray<NSURL *> *)urls
{
    (void)app;
    for (NSURL *u in urls)
        if (u.fileURL) plat_post_open(u.fileSystemRepresentation);
}
/* Logging out, or Quit from the Dock: the same question the close box asks.
 * The shell quits by itself once the document is safe. */
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)s
{
    plat_event e;
    (void)s;
    memset(&e, 0, sizeof e); e.type = PE_QUIT; push(&e);
    return NSTerminateCancel;
}
@end
static UDApp *g_app;

static void build_menu(NSString *name)
{
    NSMenu *bar = [NSMenu new], *app = [NSMenu new], *view = [NSMenu new];
    NSMenuItem *appItem = [NSMenuItem new], *viewItem = [NSMenuItem new];
    NSMenuItem *q, *fs;
    [bar addItem:appItem];
    [bar addItem:viewItem];
    [app addItemWithTitle:[@"Hide " stringByAppendingString:name]
                   action:@selector(hide:) keyEquivalent:@"h"];
    [app addItem:[NSMenuItem separatorItem]];
    q = [app addItemWithTitle:[@"Quit " stringByAppendingString:name]
                       action:@selector(quit:) keyEquivalent:@"q"];
    q.target = g_app;
    appItem.submenu = app;
    view.title = @"View";
    fs = [view addItemWithTitle:@"Enter Full Screen"
                         action:@selector(toggleFullScreen:) keyEquivalent:@"f"];
    fs.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagControl;
    viewItem.submenu = view;
    NSApp.mainMenu = bar;
}

int  plat_scale(void) { return (int)(g_bs * 100 + 0.5); }
void plat_size(int *w, int *h)
{
    NSSize s = g_win.contentView.bounds.size;
    *w = (int)(s.width * g_bs); *h = (int)(s.height * g_bs);
}

int plat_init(const char *title, int w, int h, int hidden)
{
    g_hidden = hidden;
    @autoreleasepool {
        NSString *t = [NSString stringWithUTF8String:title];
        UDView *v;
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        g_app = [UDApp new];
        NSApp.delegate = g_app;          /* before launch: the open-file event */
        build_menu(t);
        [NSApp finishLaunching];

        g_win = [[NSWindow alloc]
                    initWithContentRect:NSMakeRect(0, 0, w, h)
                    styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                              NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                    backing:NSBackingStoreBuffered defer:NO];
        g_win.releasedWhenClosed = NO;
        g_win.title = t;
        g_win.acceptsMouseMovedEvents = YES;
        g_win.collectionBehavior = NSWindowCollectionBehaviorFullScreenPrimary;
        g_win.contentMinSize = NSMakeSize(480, 320);
        g_delegate = [UDDelegate new];
        g_win.delegate = g_delegate;
        v = [[UDView alloc] initWithFrame:NSMakeRect(0, 0, w, h)];
        g_win.contentView = v;
        [g_win makeFirstResponder:v];
        [g_win center];
        g_bs = backing();
        if (!hidden) {
            [g_win makeKeyAndOrderFront:nil];
            [NSApp activateIgnoringOtherApps:YES];
        }
    }
    return 1;
}

void plat_shutdown(void)
{
    [g_win orderOut:nil];
    free(g_px); g_px = 0;
}

int plat_wait(plat_event *e, int ms)
{
    @autoreleasepool {
        NSDate *until = ms > 0 ? [NSDate dateWithTimeIntervalSinceNow:ms / 1000.0]
                               : [NSDate distantPast];
        while (g_qh == g_qt) {
            NSEvent *ev = [NSApp nextEventMatchingMask:NSEventMaskAny untilDate:until
                                                inMode:NSDefaultRunLoopMode dequeue:YES];
            if (!ev) break;
            [NSApp sendEvent:ev];
            [NSApp updateWindows];
            until = [NSDate distantPast];       /* drain, don't wait again */
        }
    }
    if (g_qh == g_qt) return 0;
    *e = g_q[g_qh];
    g_qh = (g_qh + 1) % 256;
    return 1;
}

void plat_present(const fb_px *px, int w, int h)
{
    int need = w * h * 4;
    if (need > g_pcap) {
        free(g_px);
        g_px = (unsigned char *)malloc((size_t)need);
        g_pcap = g_px ? need : 0;
        if (!g_px) return;
    }
    memcpy(g_px, px, (size_t)need);            /* already RGBA: no conversion */
    g_pw = w; g_ph = h;
    [g_win.contentView setNeedsDisplay:YES];
    [g_win displayIfNeeded];
}

unsigned plat_ticks(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned)(ts.tv_sec * 1000u + (unsigned)(ts.tv_nsec / 1000000));
}

/* Contents/Resources inside a bundle; the executable's folder outside one */
const char *plat_base_path(void)
{
    @autoreleasepool {
        NSString *p = [NSBundle mainBundle].resourcePath;
        if (!p) return 0;
        snprintf(g_base, sizeof g_base, "%s/", p.fileSystemRepresentation);
    }
    return g_base;
}

void plat_set_title(const char *utf8)
{
    @autoreleasepool { g_win.title = [NSString stringWithUTF8String:utf8]; }
}

void plat_set_fullscreen(int on)
{
    if (!!on != g_full) [g_win toggleFullScreen:nil];
}

/* the dot in the close button: unsaved changes */
void plat_set_modified(int on) { g_win.documentEdited = on ? YES : NO; }

void plat_args(int *argc, char ***argv) { (void)argc; (void)argv; }   /* UTF-8 already */

/* ---- the clipboard: the general pasteboard, as plain text ------------------ */
int plat_clip_set(const char *utf8)
{
    @autoreleasepool {
        NSPasteboard *pb = [NSPasteboard generalPasteboard];
        NSString *s = [NSString stringWithUTF8String:utf8 ? utf8 : ""];
        if (!s) return 0;
        [pb clearContents];
        return [pb setString:s forType:NSPasteboardTypeString] ? 1 : 0;
    }
}

long plat_clip_get(char *buf, long cap)
{
    @autoreleasepool {
        NSString *s = [[NSPasteboard generalPasteboard] stringForType:NSPasteboardTypeString];
        const char *u;
        long n;
        if (cap > 0) buf[0] = 0;
        if (!s) return 0;
        /* a Mac line end is LF already; CR LF from a Windows app is not */
        s = [s stringByReplacingOccurrencesOfString:@"\r\n" withString:@"\n"];
        u = s.UTF8String;
        if (!u) return 0;
        n = (long)strlen(u);
        if (cap > 0) {
            long m = n < cap - 1 ? n : cap - 1;
            memcpy(buf, u, (size_t)m);
            buf[m] = 0;
        }
        return n;
    }
}
int plat_is_fullscreen(void) { return g_full; }

/* ---- NSOpenPanel / NSSavePanel -------------------------------------------- */
int plat_file_dialog(int save, const char *const *types, int ntypes,
                     char *path, int cap, int *type)
{
    @autoreleasepool {
        NSMutableArray<NSString *> *exts = [NSMutableArray array];
        NSSavePanel *panel;
        NSURL *url;
        int t, i;
        for (t = 0; t < ntypes; t++) {
            char pat[8][16];
            int n = plat_type_patterns(types[t], pat, 8);
            for (i = 0; i < n; i++)
                if (pat[i][0] == '*' && pat[i][1] == '.' && pat[i][2] != '*')
                    [exts addObject:[NSString stringWithUTF8String:pat[i] + 2]];
        }
        if (save) {
            panel = [NSSavePanel savePanel];
            panel.canCreateDirectories = YES;
            /* the first type's extension is the default; any of ours is fine */
            panel.allowsOtherFileTypes = YES;
        } else {
            NSOpenPanel *op = [NSOpenPanel openPanel];
            op.canChooseFiles = YES;
            op.canChooseDirectories = NO;
            op.allowsMultipleSelection = NO;
            panel = op;
        }
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        if (exts.count) panel.allowedFileTypes = exts;   /* macOS 11 target */
#pragma clang diagnostic pop
        panel.title = save ? @"Save As" : @"Open";
        if ([panel runModal] != NSModalResponseOK) { [g_win makeKeyWindow]; return 0; }
        [g_win makeKeyWindow];
        url = save ? panel.URL : ((NSOpenPanel *)panel).URLs.firstObject;
        if (!url || !url.fileURL) return 0;
        snprintf(path, (size_t)cap, "%s", url.fileSystemRepresentation);
        if (type) *type = plat_type_of_path(path, types, ntypes);
        return 1;
    }
}
