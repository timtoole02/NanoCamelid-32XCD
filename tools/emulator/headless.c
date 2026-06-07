/*
 * nc-headless — minimal headless libretro frontend for NanoCamelid 32XCD
 * verification. Loads the (locally patched) PicoDrive core, runs N frames
 * deterministically, samples guest memory every frame, dumps regions at
 * exit, and prints a JSON report for the verifier.
 *
 * Memory regions (PicoDrive + local patch, see picodrive-memory-access.patch):
 *   workram=2 (68K work RAM)  vram=3  sdram=16 (32X)  fb=17 (32X DRAM)
 *   prgram=18 (MCD)  wordram=19 (MCD)  sysregs=20 (32X regs incl. COMM0-7)
 *
 * Usage:
 *   nc-headless --core pico.dylib --rom game.32x|disc.cue --frames 600
 *     [--bios-dir DIR]                      (Sega CD BIOS location)
 *     [--sample NAME:REGION:OFF:SIZE]...    (per-frame u8/u16/u32/u64 LE sample)
 *     [--dump REGION:OFF:SIZE:FILE]...      (binary dump after last frame)
 *     [--screenshot FILE.ppm]               (last frame, RGB565/XRGB8888)
 *     [--press FRAME:MASK]...               (joypad 1 buttons held that frame;
 *                                            mask bits = RETRO_DEVICE_ID_JOYPAD_*)
 *     [--quiet]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <dlfcn.h>
#include "libretro.h"

#define MAX_SAMPLES 16
#define MAX_DUMPS   16
#define MAX_PRESSES 4096
#define MAX_FRAMES  (60*60*30) /* 30 min cap: no unbounded runs */

typedef struct { char name[32]; unsigned region; uint32_t off, size; uint64_t *values; } sample_t;
typedef struct { unsigned region; uint32_t off, size; char file[512]; } dump_t;

static struct {
    void *dl;
    void (*set_environment)(retro_environment_t);
    void (*set_video_refresh)(retro_video_refresh_t);
    void (*set_audio_sample)(retro_audio_sample_t);
    void (*set_audio_sample_batch)(retro_audio_sample_batch_t);
    void (*set_input_poll)(retro_input_poll_t);
    void (*set_input_state)(retro_input_state_t);
    void (*init)(void);
    void (*deinit)(void);
    bool (*load_game)(const struct retro_game_info *);
    void (*unload_game)(void);
    void (*run)(void);
    void (*get_system_av_info)(struct retro_system_av_info *);
    void *(*get_memory_data)(unsigned);
    size_t (*get_memory_size)(unsigned);
} core;

static char bios_dir[512] = ".";
static int quiet = 0;
static enum retro_pixel_format pixfmt = RETRO_PIXEL_FORMAT_0RGB1555;

/* last video frame (for --screenshot) */
static const void *last_fb; static unsigned fb_w, fb_h; static size_t fb_pitch;

static void log_printf(enum retro_log_level level, const char *fmt, ...) {
    if (quiet) return;
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "[core:%d] ", level);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

static bool env_cb(unsigned cmd, void *data) {
    switch (cmd) {
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
        ((struct retro_log_callback *)data)->log = log_printf;
        return true;
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
        *(const char **)data = bios_dir;
        return true;
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
        pixfmt = *(enum retro_pixel_format *)data;
        return true;
    case RETRO_ENVIRONMENT_GET_CAN_DUPE:
        *(bool *)data = true;
        return true;
    default:
        return false; /* core options etc. fall back to defaults */
    }
}

static void video_cb(const void *data, unsigned w, unsigned h, size_t pitch) {
    if (data) { last_fb = data; fb_w = w; fb_h = h; fb_pitch = pitch; }
}
static void audio_cb(int16_t l, int16_t r) { (void)l; (void)r; }
static size_t audio_batch_cb(const int16_t *d, size_t frames) { (void)d; return frames; }
/* scripted input: buttons held on joypad 1 for the current frame */
static uint32_t press_frames[MAX_PRESSES];
static uint32_t press_masks[MAX_PRESSES];
static int n_presses;
static uint32_t cur_frame, cur_mask;

static void input_poll_cb(void) {}
static int16_t input_state_cb(unsigned port, unsigned dev, unsigned idx, unsigned id) {
    (void)idx;
    if (port != 0 || dev != RETRO_DEVICE_JOYPAD || id > 15)
        return 0;
    return (cur_mask >> id) & 1;
}

#define SYM(field, name) do { \
    core.field = dlsym(core.dl, name); \
    if (!core.field) { fprintf(stderr, "missing symbol %s\n", name); exit(1); } \
} while (0)

static unsigned parse_region(const char *s) {
    if (!strcmp(s, "workram")) return RETRO_MEMORY_SYSTEM_RAM;
    if (!strcmp(s, "vram"))    return RETRO_MEMORY_VIDEO_RAM;
    if (!strcmp(s, "sdram"))   return 16;
    if (!strcmp(s, "fb"))      return 17;
    if (!strcmp(s, "prgram"))  return 18;
    if (!strcmp(s, "wordram")) return 19;
    if (!strcmp(s, "sysregs")) return 20;
    return (unsigned)strtoul(s, NULL, 0);
}

/* PicoDrive stores guest memories as u16 words in host (LE) byte order, so
 * guest big-endian byte N lives at host offset N^1. These helpers present
 * memory in guest byte order; multi-byte samples are assembled big-endian
 * (the 68K/SH-2 view). */
static uint8_t guest_byte(const uint8_t *base, uint32_t a) { return base[a ^ 1]; }

static uint64_t read_guest_be(const uint8_t *base, uint32_t off, uint32_t size) {
    uint64_t v = 0;
    for (uint32_t i = 0; i < size; i++) v = (v << 8) | guest_byte(base, off + i);
    return v;
}

static int write_ppm(const char *path) {
    if (!last_fb) return -1;
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fprintf(f, "P6\n%u %u\n255\n", fb_w, fb_h);
    const uint8_t *rows = last_fb;
    for (unsigned y = 0; y < fb_h; y++) {
        for (unsigned x = 0; x < fb_w; x++) {
            uint8_t rgb[3];
            if (pixfmt == RETRO_PIXEL_FORMAT_RGB565) {
                uint16_t px = ((const uint16_t *)(rows + y * fb_pitch))[x];
                rgb[0] = (px >> 11) << 3; rgb[1] = ((px >> 5) & 0x3f) << 2; rgb[2] = (px & 0x1f) << 3;
            } else if (pixfmt == RETRO_PIXEL_FORMAT_XRGB8888) {
                uint32_t px = ((const uint32_t *)(rows + y * fb_pitch))[x];
                rgb[0] = px >> 16; rgb[1] = px >> 8; rgb[2] = px;
            } else { /* 0RGB1555 */
                uint16_t px = ((const uint16_t *)(rows + y * fb_pitch))[x];
                rgb[0] = ((px >> 10) & 0x1f) << 3; rgb[1] = ((px >> 5) & 0x1f) << 3; rgb[2] = (px & 0x1f) << 3;
            }
            fwrite(rgb, 1, 3, f);
        }
    }
    fclose(f);
    return 0;
}

int main(int argc, char **argv) {
    const char *core_path = NULL, *rom_path = NULL, *shot_path = NULL;
    long frames = 600;
    sample_t samples[MAX_SAMPLES]; int n_samples = 0;
    dump_t dumps[MAX_DUMPS]; int n_dumps = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--core") && i + 1 < argc) core_path = argv[++i];
        else if (!strcmp(argv[i], "--rom") && i + 1 < argc) rom_path = argv[++i];
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atol(argv[++i]);
        else if (!strcmp(argv[i], "--bios-dir") && i + 1 < argc)
            snprintf(bios_dir, sizeof bios_dir, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--screenshot") && i + 1 < argc) shot_path = argv[++i];
        else if (!strcmp(argv[i], "--press") && i + 1 < argc && n_presses < MAX_PRESSES) {
            if (sscanf(argv[++i], "%u:%u", &press_frames[n_presses],
                       &press_masks[n_presses]) != 2)
                { fprintf(stderr, "bad --press %s\n", argv[i]); return 2; }
            n_presses++;
        }
        else if (!strcmp(argv[i], "--quiet")) quiet = 1;
        else if (!strcmp(argv[i], "--sample") && i + 1 < argc && n_samples < MAX_SAMPLES) {
            char region[32];
            sample_t *s = &samples[n_samples];
            if (sscanf(argv[++i], "%31[^:]:%31[^:]:%i:%i", s->name, region, &s->off, &s->size) != 4
                || s->size > 8) { fprintf(stderr, "bad --sample %s\n", argv[i]); return 2; }
            s->region = parse_region(region);
            n_samples++;
        }
        else if (!strcmp(argv[i], "--dump") && i + 1 < argc && n_dumps < MAX_DUMPS) {
            char region[32];
            dump_t *d = &dumps[n_dumps];
            if (sscanf(argv[++i], "%31[^:]:%i:%i:%511s", region, &d->off, &d->size, d->file) != 4)
                { fprintf(stderr, "bad --dump %s\n", argv[i]); return 2; }
            d->region = parse_region(region);
            n_dumps++;
        }
        else { fprintf(stderr, "unknown arg: %s\n", argv[i]); return 2; }
    }
    if (!core_path || !rom_path) {
        fprintf(stderr, "usage: nc-headless --core CORE --rom ROM [--frames N] "
                        "[--bios-dir DIR] [--sample NAME:REGION:OFF:SIZE]... "
                        "[--dump REGION:OFF:SIZE:FILE]... [--screenshot F.ppm] [--quiet]\n");
        return 2;
    }
    if (frames < 1 || frames > MAX_FRAMES) { fprintf(stderr, "frames out of range\n"); return 2; }

    core.dl = dlopen(core_path, RTLD_NOW | RTLD_LOCAL);
    if (!core.dl) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }
    SYM(set_environment, "retro_set_environment");
    SYM(set_video_refresh, "retro_set_video_refresh");
    SYM(set_audio_sample, "retro_set_audio_sample");
    SYM(set_audio_sample_batch, "retro_set_audio_sample_batch");
    SYM(set_input_poll, "retro_set_input_poll");
    SYM(set_input_state, "retro_set_input_state");
    SYM(init, "retro_init");
    SYM(deinit, "retro_deinit");
    SYM(load_game, "retro_load_game");
    SYM(unload_game, "retro_unload_game");
    SYM(run, "retro_run");
    SYM(get_system_av_info, "retro_get_system_av_info");
    SYM(get_memory_data, "retro_get_memory_data");
    SYM(get_memory_size, "retro_get_memory_size");

    core.set_environment(env_cb);
    core.set_video_refresh(video_cb);
    core.set_audio_sample(audio_cb);
    core.set_audio_sample_batch(audio_batch_cb);
    core.set_input_poll(input_poll_cb);
    core.set_input_state(input_state_cb);
    core.init();

    struct retro_game_info info = { .path = rom_path, .data = NULL, .size = 0, .meta = NULL };
    if (!core.load_game(&info)) { fprintf(stderr, "load_game failed: %s\n", rom_path); return 1; }

    for (int i = 0; i < n_samples; i++) {
        samples[i].values = calloc(frames, sizeof(uint64_t));
        if (!samples[i].values) { fprintf(stderr, "oom\n"); return 1; }
    }

    for (long f = 0; f < frames; f++) {
        cur_frame = (uint32_t)f;
        cur_mask = 0;
        for (int i = 0; i < n_presses; i++)
            if (press_frames[i] == cur_frame)
                cur_mask |= press_masks[i];
        core.run();
        for (int i = 0; i < n_samples; i++) {
            const uint8_t *base = core.get_memory_data(samples[i].region);
            size_t sz = core.get_memory_size(samples[i].region);
            samples[i].values[f] = (base && samples[i].off + samples[i].size <= sz)
                ? read_guest_be(base, samples[i].off, samples[i].size) : (uint64_t)-1;
        }
    }

    /* dumps */
    int dump_fail = 0;
    for (int i = 0; i < n_dumps; i++) {
        const uint8_t *base = core.get_memory_data(dumps[i].region);
        size_t sz = core.get_memory_size(dumps[i].region);
        if (!base || dumps[i].off + dumps[i].size > sz) {
            fprintf(stderr, "dump %d: region %u unavailable (size %zu)\n", i, dumps[i].region, sz);
            dump_fail = 1; continue;
        }
        FILE *out = fopen(dumps[i].file, "wb");
        if (!out) { fprintf(stderr, "dump %d: open failed: %s\n", i, dumps[i].file); dump_fail = 1; continue; }
        for (uint32_t b = 0; b < dumps[i].size; b++)
            fputc(guest_byte(base, dumps[i].off + b), out);
        if (ferror(out)) { fprintf(stderr, "dump %d: write failed: %s\n", i, dumps[i].file); dump_fail = 1; }
        fclose(out);
    }
    if (shot_path && write_ppm(shot_path) != 0)
        fprintf(stderr, "screenshot failed\n");

    /* JSON report */
    printf("{\n  \"frames\": %ld,\n  \"pixel_format\": %d,\n  \"samples\": {", frames, pixfmt);
    for (int i = 0; i < n_samples; i++) {
        printf("%s\n    \"%s\": [", i ? "," : "", samples[i].name);
        for (long f = 0; f < frames; f++)
            printf("%s%llu", f ? "," : "", (unsigned long long)samples[i].values[f]);
        printf("]");
    }
    printf("\n  },\n  \"regions\": {");
    static const struct { const char *n; unsigned r; } regs[] = {
        {"workram", RETRO_MEMORY_SYSTEM_RAM}, {"vram", RETRO_MEMORY_VIDEO_RAM},
        {"sdram", 16}, {"fb", 17}, {"prgram", 18}, {"wordram", 19}, {"sysregs", 20}
    };
    for (unsigned i = 0; i < sizeof regs / sizeof regs[0]; i++)
        printf("%s\n    \"%s\": %zu", i ? "," : "", regs[i].n, core.get_memory_size(regs[i].r));
    printf("\n  }\n}\n");

    core.unload_game();
    core.deinit();
    return dump_fail;
}
