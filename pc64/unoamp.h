/* UnoAmp - the Winamp 2.x-shaped plugin ABI for the pc64 media player.
 * See docs/PLAYER-WINAMP-PLAN.md.
 *
 * WHY IT LOOKS LIKE WINAMP. The field order and semantics below mirror
 * Winamp 2's In_Module / Out_Module / winampVisModule so that porting a plugin
 * is recompiling its source against this header and deleting its Win32 parts.
 *
 * IT IS NOT BINARY-COMPATIBLE, and cannot be. A stock in_mp3.dll imports
 * kernel32/user32/gdi32/winmm; pc64 has a PE loader but none of those
 * libraries and no window/message system to host a config dialog. Loading
 * stock binaries means writing a Win32 subsystem, which is a bigger project
 * than this player. Two substitutions follow from that, and they are the only
 * deliberate deviations:
 *
 *   HWND hMainWindow / hwndParent  ->  void *host      (a unoui_window *)
 *   HINSTANCE hDllInstance         ->  void *module    (our .UNO handle)
 *
 * Everything else keeps Winamp's shape, including the NUL-separated
 * double-NUL-terminated extension list ("MP3\0MPEG Audio Files\0\0") and the
 * SAAddPCMData / VSAAddPCMData visualisation feed.
 */
#ifndef PC64_UNOAMP_H
#define PC64_UNOAMP_H

/* Bumped on any breaking change, with a dated entry in the plan's changelog.
 * Winamp used per-module version constants (IN_VER/OUT_VER); one number for
 * the whole ABI is simpler and there is no legacy to stay compatible with. */
#define UNOAMP_ABI 1

/* ---- output plugin capabilities -------------------------------------------
 * The core asks the SELECTED output what it can do and refuses formats it
 * cannot carry, instead of running the transport into silence (which is
 * exactly what the pre-UnoAmp player did on a machine with no DAC). */
#define UNOAMP_CAP_PCM     0x0001   /* real sampled audio at a chosen rate    */
#define UNOAMP_CAP_SQUARE  0x0002   /* one square-wave voice; melody only     */
#define UNOAMP_CAP_FILE    0x0004   /* writes to a file, not to hardware      */
#define UNOAMP_CAP_SEEK    0x0008   /* GetOutputTime is meaningful            */

/* ===========================================================================
 * Out_Module - a sink. Winamp's ordering; Open/Write/CanWrite/Flush are the
 * load-bearing four.
 * ======================================================================== */
typedef struct unoamp_out {
    int   version;                  /* UNOAMP_ABI                             */
    const char *description;        /* "HD Audio", "AC'97", "PC speaker"      */
    int   id;                       /* stable id, for remembering a choice    */
    void *host;                     /* unoui_window * (Winamp: hMainWindow)   */
    void *module;                   /* .UNO handle   (Winamp: hDllInstance)   */
    unsigned caps;                  /* UNOAMP_CAP_*                           */

    void (*Config)(void *host);
    void (*About)(void *host);
    void (*Init)(void);
    void (*Quit)(void);

    /* Probe: is this sink usable on THIS machine? 0 = no, and the core moves
     * on to the next in probe order. Not a Winamp field - Winamp had the user
     * pick an output plugin, we autodetect - but it is what turns a list of
     * sinks into "the best available hardware" the plan asked for. */
    int  (*Probe)(void);

    /* Winamp semantics exactly: bufferlenms/prebufferms are hints, the return
     * is 0 on success and <0 on failure. */
    int  (*Open)(int samplerate, int numchannels, int bitspersamp,
                 int bufferlenms, int prebufferms);
    void (*Close)(void);
    int  (*Write)(const char *buf, int len);   /* len BYTES, s16 interleaved  */
    int  (*CanWrite)(void);                    /* bytes writable right now    */
    int  (*IsPlaying)(void);                   /* 1 = buffered data remains   */
    int  (*Pause)(int pause);                  /* returns the PREVIOUS state  */
    void (*SetVolume)(int volume);             /* 0..255, as Winamp           */
    void (*SetPan)(int pan);                   /* -128..128                   */
    void (*Flush)(int time_ms);                /* discard + reset the clock   */
    int  (*GetOutputTime)(void);               /* ms actually heard           */
    int  (*GetWrittenTime)(void);              /* ms written                  */
} unoamp_out;

/* ===========================================================================
 * In_Module - a decoder. Trimmed of the Winamp fields that only mean something
 * under Win32 (InfoBox's HWND, the DSP hooks that belong to the host), but the
 * survivors keep their names, order and units.
 * ======================================================================== */
typedef struct unoamp_in {
    int   version;
    const char *description;
    void *host;
    void *module;
    const char *FileExtensions;     /* "MP3\0MPEG Audio\0WAV\0WAV Audio\0\0"  */
    int   is_seekable;
    unsigned needs_caps;            /* what a sink must advertise to play this */

    void (*Config)(void *host);
    void (*About)(void *host);
    void (*Init)(void);
    void (*Quit)(void);

    /* title may be left empty; length -1 when unknown. */
    void (*GetFileInfo)(const char *file, char *title, int title_cap,
                        int *length_in_ms);
    int  (*IsOurFile)(const char *fn);   /* content sniff; 0 = defer to ext    */
    int  (*Play)(const char *fn);        /* 0 = ok, <0 = error                 */
    void (*Pause)(void);
    void (*UnPause)(void);
    int  (*IsPaused)(void);
    void (*Stop)(void);
    int  (*GetLength)(void);             /* ms, -1 unknown                     */
    int  (*GetOutputTime)(void);         /* ms                                 */
    void (*SetOutputTime)(int time_in_ms);
    void (*SetVolume)(int volume);
    void (*SetPan)(int pan);

    /* Pump: decode up to max_frames into out[] (s16 interleaved at the rate
     * reported by Play). Returns frames produced, 0 at end of stream.
     *
     * Winamp had input plugins run their own thread and push into outMod;
     * pc64's shell is a cooperative frame loop with no threads, so the host
     * pulls instead. Same graph, inverted control - and it is why a slow
     * decode here costs latency rather than stalling the desktop. */
    int  (*Decode)(short *out, int max_frames);

    struct unoamp_out *outMod;      /* set by the host before Play            */
} unoamp_in;

/* ===========================================================================
 * Vis_Module - Winamp's visualiser contract, including the 576-sample buffers
 * every vis plugin ever written expects.
 * ======================================================================== */
#define UNOAMP_VIS_SAMPLES 576

typedef struct unoamp_vis {
    const char *description;
    void *host;
    void *module;
    int   sRate, nCh;
    int   latencyMs, delayMs;
    int   spectrumNch, waveformNch;
    unsigned char spectrumData[2][UNOAMP_VIS_SAMPLES];
    unsigned char waveformData[2][UNOAMP_VIS_SAMPLES];
    void (*Config)(struct unoamp_vis *this_mod);
    int  (*Init)(struct unoamp_vis *this_mod);
    int  (*Render)(struct unoamp_vis *this_mod);
    void (*Quit)(struct unoamp_vis *this_mod);
} unoamp_vis;

/* ===========================================================================
 * Enc_Module - the transcode sink. Winamp 2 had no encoder plugins (they
 * arrived with 5.x), so this is our shape rather than a port of theirs: an
 * encoder is an output that writes a file, which is why transcoding is the
 * same graph with a different sink rather than a separate tool.
 * ======================================================================== */
typedef struct unoamp_enc {
    int   version;
    const char *description;
    const char *extension;          /* "WAV", no dot                          */
    int  (*Open)(int vol, const char *path, int samplerate, int channels,
                 int bitspersamp);
    int  (*Write)(const char *buf, int len);
    void (*Close)(void);
} unoamp_enc;

/* ---- host side ------------------------------------------------------------ */

/* Probe every registered output in order and select the first that answers.
 * Returns the chosen sink, or NULL when the machine has no audio at all -
 * which is a legitimate state the UI must show, not an error to hide. */
const unoamp_out *unoamp_select_out(void);
const unoamp_out *unoamp_current_out(void);

/* What the selected sink can do; 0 when there is none. The core gates format
 * offers on this. */
unsigned unoamp_caps(void);

/* Registration, mirroring the unodevices seam: a plugin adds itself, nothing
 * central is edited. Built-ins register at init; loadable .UNO plugins from
 * \PLUGINS\ register when the host loads them. */
void unoamp_out_init(void);      /* register the built-ins + select */
int unoamp_register_out(const unoamp_out *o);
int unoamp_out_count(void);
const unoamp_out *unoamp_out_at(int i);

/* ---- input plugins (phase 2) ---------------------------------------------- */
void unoamp_in_init(void);
int  unoamp_register_in(const unoamp_in *in);
int  unoamp_in_count(void);
const unoamp_in *unoamp_in_at(int i);
const unoamp_in *unoamp_playing(void);
/* Pick a decoder for a name: content sniff first, extension list second. */
const unoamp_in *unoamp_find_in(const char *fn);
/* Open decoder + check the sink can carry it. 1 = playing, 0 = *why says no. */
int  unoamp_play(int vol, const char *fn, const char **why);
void unoamp_stop(void);


/* ---- the player (phase 4, unoamp_app.c) -----------------------------------
 * Playback state that has nothing to do with pixels, so that the skinned
 * window, a keyboard shortcut and a script verb all reach it the same way. */
enum {                              /* CBUTTONS order - see unoamp_ui.c      */
    UNOAMP_T_PREV = 0, UNOAMP_T_PLAY, UNOAMP_T_PAUSE,
    UNOAMP_T_STOP, UNOAMP_T_NEXT, UNOAMP_T_EJECT
};
void unoamp_transport(int which);
void unoamp_next(void);
void unoamp_prev(void);
int  unoamp_play_index(int i);
const char *unoamp_last_error(void);

int  unoamp_pl_add(int vol, const char *path);
void unoamp_pl_remove(int i);
void unoamp_pl_clear(void);
int  unoamp_pl_count(void);
int  unoamp_pl_current(void);
int  unoamp_pl_selected(void);
void unoamp_pl_select(int i);
const char *unoamp_pl_title(int i);
const char *unoamp_pl_path(int i);
int  unoamp_pl_vol(int i);
int  unoamp_pl_len_ms(int i);

void unoamp_set_volume(int v);      /* 0..100                                */
void unoamp_set_balance(int b);     /* -100..100                             */
int  unoamp_volume(void);
int  unoamp_balance(void);

/* One frame: pull the decoder into the sink, advance at end of stream. This
 * IS the playback engine - there are no threads. */
void unoamp_tick(void);
/* First open: load a skin if one is installed and seed the playlist. Both are
 * best-effort - neither can stop the player opening. */
void unoamp_start(void);

/* ---- the skinned windows (phase 4, unoamp_ui.c) --------------------------- */
struct unoui_window;
void unoamp_ui_build(struct unoui_window *win);
struct unoui_window *unoamp_ui_window(void);
void unoamp_ui_set_title(const char *s);
void unoamp_ui_tick(void);
void unoamp_ui_set_shade(int on);
int  unoamp_ui_shaded(void);
int  unoamp_ui_scale(void);
void unoamp_ui_set_scale(int s);
void unoamp_ui_show_eq(int on);
void unoamp_ui_show_pl(int on);
void unoamp_ui_close(void);
void unoamp_ui_set_eq_open(int v);
void unoamp_ui_set_pl_open(int v);
void unoamp_ui_build_eq(struct unoui_window *win);
void unoamp_ui_build_pl(struct unoui_window *win);
struct unoui_window *unoamp_ui_eq_window(void);
struct unoui_window *unoamp_ui_pl_window(void);
void unoamp_ui_dock(void);          /* re-snap EQ + playlist to the main win  */
int  unoamp_eq_enabled(void);
int  unoamp_eq_band(int i);         /* -100..+100, 0 = flat                   */
int  unoamp_eq_preamp(void);
int  unoamp_ui_volume(void);
int  unoamp_ui_balance(void);
int  unoamp_ui_shuffle(void);
int  unoamp_ui_repeat(void);

/* ---- visualisation (phase 5, unoamp_vis.c) --------------------------------
 * The host feeds every decoded block here on its way to the sink; the vis
 * plugins read from the ring. Winamp handed plugins 576 samples, and skins'
 * viscolor.txt palettes assume that width, so that is the window size. */
void unoamp_vis_feed(const short *pcm, int nframes);
int  unoamp_register_vis(unoamp_vis *v);
int  unoamp_vis_count(void);
unoamp_vis *unoamp_vis_at(int i);
void unoamp_vis_select(int i);      /* -1 = none                             */
int  unoamp_vis_selected(void);
void unoamp_vis_init(void);
/* Render the current visualiser into the main window's well. Scale is the
 * integer skin scale; the caller has already cleared the well. */
void unoamp_vis_draw(int x, int y, int w, int h, int scale);


/* ---- DSP plugins (phase 6, unoamp_dsp.c) ----------------------------------
 * Winamp's winampDSPModule, with the two Win32-only fields dropped. It sits
 * between the decoder and the sink on the host's pull.
 *
 * ModifySamples RETURNS the new frame count: a plugin is allowed to resample
 * or time-stretch, so the caller must write back what came out, not what went
 * in. The buffer therefore has headroom - see unoamp_tick. */
typedef struct unoamp_dsp {
    const char *description;
    void *host;
    void *module;
    int   enabled;
    void (*Config)(struct unoamp_dsp *this_mod);
    int  (*Init)(struct unoamp_dsp *this_mod);
    int  (*ModifySamples)(struct unoamp_dsp *this_mod, short *samples,
                          int nframes, int bps, int nch, int srate);
    void (*Quit)(struct unoamp_dsp *this_mod);
} unoamp_dsp;

void unoamp_dsp_init(void);
int  unoamp_register_dsp(unoamp_dsp *d);
int  unoamp_dsp_count(void);
unoamp_dsp *unoamp_dsp_at(int i);
/* Run the chain in place. Returns the frame count after processing. */
int  unoamp_dsp_run(short *samples, int nframes, int nch, int rate);


/* The fs volume a decoder should open paths on. The Winamp Play() contract
 * takes a path and nothing else, so the volume travels beside it - the host
 * sets it before Play, and content-sniffing IsOurFile reads it too. */
void unoamp_in_set_volume_index(int vol);
int  unoamp_probe_volume(void);

/* ---- encoders and transcoding (phase 7, unoamp_enc.c) --------------------- */
void unoamp_enc_init(void);
int  unoamp_register_enc(const unoamp_enc *e);
int  unoamp_enc_count(void);
const unoamp_enc *unoamp_enc_at(int i);
const unoamp_enc *unoamp_find_enc(const char *ext);   /* "WAV", no dot        */
/* Decode -> DSP -> encode, to completion. progress() returns 0 to cancel and
 * gets -1 when the input cannot say how long it is. 1 = written. */
int  unoamp_transcode(int in_vol, const char *in_path,
                      int out_vol, const char *out_path,
                      const char *ext, int apply_dsp,
                      int (*progress)(int pct), const char **why);

/* ---- tracker + chiptune inputs (phase 8, unoamp_mod.c) -------------------- */
void unoamp_mod_init(void);
const char *unoamp_vgm_error(void);   /* why a VGM was refused, or ""         */

#endif /* PC64_UNOAMP_H */
