/* ===========================================================================
 * UnoDOS/PS2 - Mac Toolbox compatibility shim (the M1 platform layer).
 *
 * `ps2/unodos.c` is the portable C core, copied from `mac/unodos.c` and built
 * against the classic Mac Toolbox. Rather than rewrite it, this shim re-implements
 * the ~40 Toolbox calls it actually uses (audited in HANDOFF SS1) over the
 * software framebuffer `fb.*`:
 *
 *   QuickDraw  - one implicit full-screen GrafPort; pen position + fore/back
 *                colour + text mode/size live here. Rect/oval/line/text map to
 *                fb_* primitives. (M1)
 *   Events     - GetNextEvent/GetMouse/StillDown/TickCount. The host harness and
 *                AUTOTEST drive apps directly, so the queue is input-fed by the
 *                platform main (pad on EE, none on host). (M1)
 *   Memory     - NewPtr/DisposePtr over malloc. (M1)
 *   File Mgr   - FSOpen/Read/Write/Close/Create/Delete + PBGetCatInfo over a
 *                backing store (host file tree / PS2 memory card). (M2)
 *   Sound Mgr  - SndNewChannel/SndDoImmediate square-wave synth. (M3)
 *
 * Colour port only: ps2/unodos.c builds with -DUNO_COLOR=1, so the mono
 * 1-bit QuickDraw paths (FillRect with qd.gray/black patterns) compile out and
 * the palette is the literal UnoDOS 4-colour gamut.
 * ===========================================================================
 */
#ifndef UNO_MAC_COMPAT_H
#define UNO_MAC_COMPAT_H

#include <stddef.h>
#include "fb.h"

/* ---- core types -------------------------------------------------------- */
typedef unsigned char  Boolean;
typedef signed char    SignedByte;
typedef short          OSErr;
typedef char          *Ptr;
typedef Ptr           *Handle;
typedef unsigned long  OSType;
typedef const unsigned char *ConstStr255Param;
typedef unsigned char *StringPtr;

#ifndef true
#define true  1
#define false 0
#endif
#ifndef NULL
#define NULL ((void *)0)
#endif

typedef struct { short v, h; } Point;
typedef struct { short top, left, bottom, right; } Rect;
typedef struct { unsigned short red, green, blue; } RGBColor;
typedef struct { unsigned char pat[8]; } Pattern;

/* GrafPort: we only need thePort identity + a couple of fields the core reads
   (screenBits.bounds). Drawing state proper lives in mac_compat.c globals. */
typedef struct { Rect bounds; } BitMap;
typedef struct GrafPort {
    Rect   portRect;
    BitMap portBits;
} GrafPort;
typedef GrafPort *GrafPtr;
typedef GrafPtr   WindowPtr;

typedef struct {
    GrafPtr thePort;
    BitMap  screenBits;     /* screenBits.bounds = full screen rect */
    Pattern white, black, gray, ltGray, dkGray;
} QDGlobals;

/* ---- events ------------------------------------------------------------ */
typedef struct {
    short what;
    long  message;
    long  when;
    Point where;
    short modifiers;
} EventRecord;

enum {
    nullEvent = 0, mouseDown = 1, mouseUp = 2, keyDown = 3, keyUp = 4,
    autoKey = 5, updateEvt = 6
};
#define everyEvent   (-1)
#define charCodeMask 0x000000FFL
#define keyCodeMask  0x0000FF00L
enum { cmdKey = 0x0100, shiftKey = 0x0200, optionKey = 0x0800 };

/* ---- QuickDraw constants ----------------------------------------------- */
enum { blackColor = 33, whiteColor = 30, redColor = 205, greenColor = 341,
       blueColor = 409, yellowColor = 69 };
/* transfer modes */
enum { srcCopy = 0, srcOr = 1, srcXor = 2, srcBic = 3,
       patCopy = 8, patOr = 9, patXor = 10, patBic = 11 };
/* window proc ids (ignored - UnoDOS owns one full-screen port) */
enum { documentProc = 0, plainDBox = 2, altDBoxProc = 3 };
/* QuickDraw text styles (TextFace) */
enum { normal = 0, bold = 1, italic = 2, underline = 4, outline = 8,
       shadow = 16, condense = 32, extend = 64 };

#define noErr 0

/* ---- File Manager ------------------------------------------------------ */
enum { fsCurPerm = 0, fsRdPerm = 1, fsWrPerm = 2, fsRdWrPerm = 3 };
enum { fsAtMark = 0, fsFromStart = 1, fsFromLEOF = 2, fsFromMark = 3 };
/* File Manager result codes the core compares against */
enum { dirFulErr = -33, dskFulErr = -34, eofErr = -39, fnfErr = -43,
       wPrErr = -44, fLckdErr = -45, dupFNErr = -48, opWrErr = -49,
       paramErr = -50, rfNumErr = -51 };
enum { fsRtDirID = 2 };

/* working-directory param block (files_enter_dir sets the default dir) */
typedef struct {
    short      ioCompletion;
    OSErr      ioResult;
    StringPtr  ioNamePtr;
    short      ioVRefNum;
    short      ioWDIndex;
    long       ioWDProcID;
    short      ioWDVRefNum;
    long       ioWDDirID;
} WDPBRec;
typedef WDPBRec *WDPBPtr;

/* Catalog-info param block. On real Mac OS CInfoPBRec is a union of HFileInfo
   and DirInfo, which the core selects with cpb.hFileInfo.* / cpb.dirInfo.*. We
   only need the fields it actually reads (audited), so both arms are the SAME
   view struct: every field aliases, and PBGetCatInfoSync can fill either arm.
   Layout need not match Mac OS - ps2/unodos.c is the sole consumer. */
typedef struct {
    short      ioCompletion;
    OSErr      ioResult;
    StringPtr  ioNamePtr;
    short      ioVRefNum;
    short      ioFRefNum;
    short      ioFDirIndex;     /* 1-based item index for enumeration          */
    SignedByte ioFlAttrib;      /* bit 4 (0x10) set => directory               */
    SignedByte ioACUser;
    long       ioFlLgLen;       /* file length (bytes)                         */
    long       ioDirID;         /* this item's id                              */
    long       ioDrDirID;       /* directory id (when item is a dir)           */
    long       ioDrParID;       /* parent dir id                               */
} CInfoView;
typedef union { CInfoView hFileInfo; CInfoView dirInfo; } CInfoPBRec;
typedef CInfoPBRec *CInfoPBPtr;

/* generic param block - the core uses pb.ioParam.* (raw .Sony block I/O). */
typedef struct {
    short      ioCompletion;
    OSErr      ioResult;
    StringPtr  ioNamePtr;
    short      ioVRefNum;
    short      ioRefNum;
    Ptr        ioBuffer;
    long       ioReqCount;
    long       ioActCount;
    short      ioPosMode;
    long       ioPosOffset;
} IOParam;
typedef struct { IOParam ioParam; } ParamBlockRec;
typedef ParamBlockRec *ParmBlkPtr;

/* ---- Sound Manager ----------------------------------------------------- */
enum { squareWaveSynth = 1, waveTableSynth = 3, sampledSynth = 5 };
#define noteSynth squareWaveSynth
enum { nullCmd = 0, quietCmd = 3, flushCmd = 4, noteCmd = 40, restCmd = 41,
       freqCmd = 42, ampCmd = 43, timbreCmd = 44 };
typedef struct {
    unsigned short cmd;
    short          param1;
    long           param2;
} SndCommand;
typedef struct SndChannel { int id; } SndChannel, *SndChannelPtr;

/* ===========================================================================
 * Toolbox API the core calls (definitions in mac_compat.c)
 * ======================================================================== */
extern QDGlobals qd;

/* init - all no-ops but InitGraf, which seeds qd */
void InitGraf(void *globalsPtr);
void InitFonts(void);
void InitWindows(void);
void InitMenus(void);
void TEInit(void);
void InitDialogs(void *resumeProc);
void InitCursor(void);
void FlushEvents(short whichMask, short stopMask);

WindowPtr NewWindow(void *storage, const Rect *bounds, ConstStr255Param title,
                    Boolean visible, short proc, WindowPtr behind,
                    Boolean goAway, long refCon);
WindowPtr NewCWindow(void *storage, const Rect *bounds, ConstStr255Param title,
                     Boolean visible, short proc, WindowPtr behind,
                     Boolean goAway, long refCon);
void SetPort(GrafPtr port);

/* rect math */
void SetRect(Rect *r, short left, short top, short right, short bottom);
void OffsetRect(Rect *r, short dh, short dv);
void InsetRect(Rect *r, short dh, short dv);
Boolean PtInRect(Point pt, const Rect *r);

/* colour + pen + text state */
void RGBForeColor(const RGBColor *c);
void RGBBackColor(const RGBColor *c);
void ForeColor(long color);
void BackColor(long color);
void PenNormal(void);
void PenMode(short mode);
void PenSize(short w, short h);
void PenPat(const Pattern *pat);
void TextMode(short mode);
void TextFont(short font);
void TextSize(short size);
void TextFace(short face);

/* drawing */
void MoveTo(short h, short v);
void LineTo(short h, short v);
void PaintRect(const Rect *r);
void FrameRect(const Rect *r);
void FillRect(const Rect *r, const Pattern *pat);
void EraseRect(const Rect *r);
void InvertRect(const Rect *r);
void PaintOval(const Rect *r);
void FrameOval(const Rect *r);
void FrameRoundRect(const Rect *r, short ovalW, short ovalH);
void FrameArc(const Rect *r, short startAngle, short arcAngle);
void DrawText(const void *textBuf, short firstByte, short byteCount);
short TextWidth(const void *textBuf, short firstByte, short byteCount);
void GlobalToLocal(Point *pt);

/* events / time / input */
long TickCount(void);
Boolean GetNextEvent(short eventMask, EventRecord *theEvent);
void GetMouse(Point *mouseLoc);
Boolean StillDown(void);
short Random(void);

/* memory */
Ptr NewPtr(long byteCount);
void DisposePtr(Ptr p);

/* File Manager (M2) */
OSErr FSOpen(const void *fileName, short vRefNum, short *refNum);
OSErr FSClose(short refNum);
OSErr FSRead(short refNum, long *count, void *buffPtr);
OSErr FSWrite(short refNum, long *count, const void *buffPtr);
OSErr Create(const void *fileName, short vRefNum, OSType creator, OSType fileType);
OSErr FSDelete(const void *fileName, short vRefNum);
OSErr FlushVol(const void *volName, short vRefNum);
OSErr PBGetCatInfoSync(CInfoPBPtr pb);
OSErr PBHSetVolSync(void *pb);
OSErr PBReadSync(ParmBlkPtr pb);
OSErr PBWriteSync(ParmBlkPtr pb);

/* Sound Manager (M3) */
OSErr SndNewChannel(SndChannelPtr *chan, short synth, long init, void *userRoutine);
OSErr SndDisposeChannel(SndChannelPtr chan, Boolean quietNow);
OSErr SndDoImmediate(SndChannelPtr chan, const SndCommand *cmd);

/* ---- host/EE present + input hooks (platform main provides these) ------- */
/* The platform feeds events into the queue via these; the core never sees them. */
void uno_post_event(short what, long message, Point where, short modifiers);
void uno_set_mouse(short h, short v, Boolean down);
/* present the framebuffer (host: PPM; EE: GS upload) - platform-defined. */
void uno_host_present(void);    /* host shim (host_desktop.c) */
void uno_ee_init(void);         /* EE platform (ee_platform.c) */
void uno_ee_poll(void);
void uno_ee_present(void);
void uno_usb_init(void);        /* USB keyboard + mouse (ee_usb.c) */
void uno_usb_poll(void);
int  uno_usb_cursor(short *x, short *y);  /* 1 = pointer visible */
void uno_dc_init(void);         /* Dreamcast platform (dc_main.c) */
void uno_dc_poll(void);         /* maple controller + keyboard + mouse */
void uno_dc_present(void);      /* fb -> RGB565 framebuffer each vblank */
void uno_pc64_init(void);       /* modern-PC UEFI platform (uefi_main.c) */
void uno_pc64_poll(void);       /* UEFI SimpleTextIn keyboard + pointer */
void uno_pc64_present(void);    /* fb -> GOP framebuffer each frame */
void uno_pc64_lowres(int on);   /* low-res render mode (full-screen 3D) */
int  uno_pc64_next_key(int *scan, int *uni, int *ctrl);  /* raw key stream (unoui) */
void uno_pc64_mouse(int *x, int *y, int *btn);           /* fb-space mouse + button */
void uno_pc64_delay_ms(int ms);                          /* firmware Stall (idle pacing) */
void uno_pc64_res_set(int idx);      /* Settings resolution (shell reuse) */
int  uno_pc64_res_count(void);
void uno_pc64_res_get(int idx, short *w, short *h, short *zoom, Boolean *active);
int  uno_pc64_res_count(void);  /* display resolutions (Settings app) */
void uno_pc64_res_get(int idx, short *w, short *h, short *zoom,
                      Boolean *active);
void uno_pc64_res_set(int idx);

/* PC-speaker single voice (uefi_main.c) - the UnoSound backend */
void uno_pc64_snd_note(int midi);
void uno_pc64_snd_quiet(void);

/* power, wall-clock (firmware RTC) + startup chime (uefi_main.c) */
void uno_pc64_shutdown(void);
void uno_pc64_restart(void);
int  uno_pc64_time(int *y, int *mo, int *d, int *h, int *mi, int *s);
int  uno_pc64_set_time(int y, int mo, int d, int h, int mi, int s);
void uno_pc64_chime(void);

#endif /* UNO_MAC_COMPAT_H */
