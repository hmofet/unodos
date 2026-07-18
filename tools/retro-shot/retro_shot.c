/* retro_shot.c - minimal headless libretro frontend for UnoDOS render checks.
 *
 * Loads a libretro core (.so), loads a ROM, runs N frames, and writes the last
 * video frame to a PPM (P6). No audio, no input (the UnoDOS AUTOTEST ROMs drive
 * their own synthetic pad), no display - the core renders to a software
 * framebuffer we capture in the video_refresh callback.
 *
 *   retro_shot <core.so> <rom> <out.ppm> [frames]
 *
 * Handles the three libretro pixel formats: 0RGB1555, XRGB8888, RGB565.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dlfcn.h>
#include "libretro.h"

static unsigned g_fmt = RETRO_PIXEL_FORMAT_0RGB1555; /* libretro default */
static const void *g_fb; static unsigned g_w, g_h; static size_t g_pitch;
static uint8_t *g_last; static unsigned g_lw, g_lh;

static bool env_cb(unsigned cmd, void *data) {
    switch (cmd) {
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
        g_fmt = *(const enum retro_pixel_format *)data; return true;
    case RETRO_ENVIRONMENT_GET_CAN_DUPE:
        *(bool *)data = true; return true;
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
        *(const char **)data = "."; return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
        struct retro_variable *v = (struct retro_variable *)data;
        /* Force hardware-rendered cores (flycast) into their software
           rasterizer + single-threaded, so they fill the libretro framebuffer
           without needing a GL context this headless frontend can't provide. */
        if (v->key && strstr(v->key, "renderer"))              { v->value = "software"; return true; }
        if (v->key && strstr(v->key, "threaded_rendering"))    { v->value = "disabled"; return true; }
        if (v->key && strstr(v->key, "boot_to_bios"))          { v->value = "disabled"; return true; }
        v->value = NULL; return false;
    }
    case RETRO_ENVIRONMENT_SET_VARIABLES:
    case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
    case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
    case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
    case RETRO_ENVIRONMENT_SET_GEOMETRY:
        return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
        *(bool *)data = false; return true;
    default:
        return false;
    }
}

/* capture every frame; keep the last non-dup one */
static void video_cb(const void *data, unsigned w, unsigned h, size_t pitch) {
    if (!data) return;                 /* frame-dup: keep previous */
    g_fb = data; g_w = w; g_h = h; g_pitch = pitch;
    size_t need = (size_t)w * h * 3;
    if (!g_last || g_lw != w || g_lh != h) {
        free(g_last); g_last = malloc(need); g_lw = w; g_lh = h;
    }
    for (unsigned y = 0; y < h; y++) {
        const uint8_t *row = (const uint8_t *)data + (size_t)y * pitch;
        uint8_t *out = g_last + (size_t)y * w * 3;
        for (unsigned x = 0; x < w; x++) {
            uint8_t r, g, b;
            if (g_fmt == RETRO_PIXEL_FORMAT_XRGB8888) {
                uint32_t p = ((const uint32_t *)row)[x];
                r = (p >> 16) & 0xFF; g = (p >> 8) & 0xFF; b = p & 0xFF;
            } else if (g_fmt == RETRO_PIXEL_FORMAT_RGB565) {
                uint16_t p = ((const uint16_t *)row)[x];
                r = ((p >> 11) & 0x1F) * 255 / 31;
                g = ((p >> 5) & 0x3F) * 255 / 63;
                b = (p & 0x1F) * 255 / 31;
            } else { /* 0RGB1555 */
                uint16_t p = ((const uint16_t *)row)[x];
                r = ((p >> 10) & 0x1F) * 255 / 31;
                g = ((p >> 5) & 0x1F) * 255 / 31;
                b = (p & 0x1F) * 255 / 31;
            }
            out[x*3] = r; out[x*3+1] = g; out[x*3+2] = b;
        }
    }
}

static void audio_cb(const int16_t *d, size_t f) { (void)d; (void)f; }
static size_t audio_batch_cb(const int16_t *d, size_t f) { (void)d; return f; }
static void input_poll_cb(void) {}
static int16_t input_state_cb(unsigned p, unsigned d, unsigned i, unsigned id) {
    (void)p; (void)d; (void)i; (void)id; return 0;
}

typedef void (*fn_set_env)(retro_environment_t);
typedef void (*fn_set_video)(retro_video_refresh_t);
typedef void (*fn_set_audio)(retro_audio_sample_t);
typedef void (*fn_set_audio_batch)(retro_audio_sample_batch_t);
typedef void (*fn_set_poll)(retro_input_poll_t);
typedef void (*fn_set_state)(retro_input_state_t);
typedef void (*fn_void)(void);
typedef bool (*fn_load)(const struct retro_game_info *);
typedef void (*fn_av)(struct retro_system_av_info *);

#define SYM(h,name,type) type name = (type)dlsym(h, #name); \
    if (!name) { fprintf(stderr, "missing %s\n", #name); return 2; }

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: %s core.so rom out.ppm [frames]\n", argv[0]); return 1; }
    const char *core = argv[1], *rom = argv[2], *out = argv[3];
    int frames = argc > 4 ? atoi(argv[4]) : 300;

    void *h = dlopen(core, RTLD_NOW | RTLD_LOCAL);
    if (!h) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 2; }

    SYM(h, retro_set_environment, fn_set_env);
    SYM(h, retro_set_video_refresh, fn_set_video);
    SYM(h, retro_set_audio_sample, fn_set_audio);
    SYM(h, retro_set_audio_sample_batch, fn_set_audio_batch);
    SYM(h, retro_set_input_poll, fn_set_poll);
    SYM(h, retro_set_input_state, fn_set_state);
    SYM(h, retro_init, fn_void);
    SYM(h, retro_load_game, fn_load);
    SYM(h, retro_get_system_av_info, fn_av);
    SYM(h, retro_run, fn_void);
    SYM(h, retro_deinit, fn_void);

    retro_set_environment(env_cb);
    retro_set_video_refresh(video_cb);
    retro_set_audio_sample(audio_cb);
    retro_set_audio_sample_batch(audio_batch_cb);
    retro_set_input_poll(input_poll_cb);
    retro_set_input_state(input_state_cb);
    retro_init();

    FILE *rf = fopen(rom, "rb");
    if (!rf) { fprintf(stderr, "open rom: %s\n", rom); return 3; }
    fseek(rf, 0, SEEK_END); long sz = ftell(rf); fseek(rf, 0, SEEK_SET);
    void *buf = malloc(sz);
    if (fread(buf, 1, sz, rf) != (size_t)sz) { fprintf(stderr, "read rom\n"); return 3; }
    fclose(rf);

    struct retro_game_info gi; memset(&gi, 0, sizeof gi);
    gi.path = rom; gi.data = buf; gi.size = sz;
    if (!retro_load_game(&gi)) { fprintf(stderr, "load_game failed\n"); return 4; }

    struct retro_system_av_info av; memset(&av, 0, sizeof av);
    retro_get_system_av_info(&av);
    fprintf(stderr, "geom base=%ux%u max=%ux%u fmt=%u\n",
            av.geometry.base_width, av.geometry.base_height,
            av.geometry.max_width, av.geometry.max_height, g_fmt);

    for (int i = 0; i < frames; i++) retro_run();

    if (!g_last) { fprintf(stderr, "no frame captured\n"); return 5; }
    FILE *of = fopen(out, "wb");
    fprintf(of, "P6\n%u %u\n255\n", g_lw, g_lh);
    fwrite(g_last, 1, (size_t)g_lw * g_lh * 3, of);
    fclose(of);
    fprintf(stderr, "wrote %s (%ux%u) after %d frames\n", out, g_lw, g_lh, frames);
    retro_deinit();
    return 0;
}
