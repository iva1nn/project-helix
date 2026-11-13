/* Entry point: loads the exe from the user's disc image into PS1 RAM, then runs the recompiled code.
 * Front end: SDL2 window, keyboard/controller input, 60 Hz pacing, optional headless PNG dumps. */
#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>
#include <stdlib.h>
#include <direct.h>
#include <io.h>
#include <sys/stat.h>
#include "hw.h"
#include "models.h"
#include "props.h"
#include "input.h"
#include "ui.h"
#include "state.h"
void menu_action(int action);
extern void (*hw_overlay_draw)(int w, int h);

void gte_init(void);
void hooks_init(void);
void subs_init(void);
void gopt_init(void);
void gamefix_init(void); /* gamefix.c: the original games' own bugs */
int audio_volume = 100; /* the port's output volume in percent (ini volume; the options menu's Sound page) */
extern const uint64_t recomp_exe_hash;

int opt_headless, opt_fast, opt_mute;
static int pad_mode_forced;
static const char *cheat_file, *cheat_names[16];
static int ncheat_names;
static int64_t opt_fast_until;
int64_t opt_max_frames;
static int opt_dump_every;
static int64_t opt_dump_from; /* --dump-from FRAME: no dumps before it */
static const char *opt_dump_dir = "out";

static SDL_Window *win;
static SDL_Renderer *ren;
static SDL_Texture *tex;
static const char *opt_renderer;   /* "hw" or "sw"; default hw in a window, sw headless */
static int opt_scale = 8; /* the remasters' movies are 8x: at 4x they were shrunk before being shown */
static const char *opt_remaster; /* a remaster work folder (development); "" = off; unset = the data folder */
static const char *remaster_dir;
static const char *opt_data;     /* --data DIR / ini data = DIR */
char fe_data[1024], fe_data_game[1100];
static int opt_fullscreen = 1, opt_screen_w, opt_screen_h; /* borderless fullscreen by default */
uint32_t *hw_screen_capture(const GpuDisplay *d, int w, int h);
int hw_init(SDL_Window *window, int scale);
void hw_present(const GpuDisplay *d, int vx, int vy, int vw, int vh, int win_w, int win_h);
void hw_present_rgb(const uint32_t *rgb, int w, int h, int vx, int vy, int vw, int vh, int win_w, int win_h);
uint32_t *hw_read_display(const GpuDisplay *d, int *ow, int *oh);
static uint32_t frame_rgb[1024 * 512];
static SDL_AudioDeviceID audio_dev;
static FILE *wav_file;
static uint32_t wav_bytes;

/* ------------------------------------------------------------------ audio */

static void wav_finish(void) {
    if (!wav_file) return;
    uint32_t hdr[11] = {0x46464952, 36 + wav_bytes, 0x45564157, 0x20746D66, 16, 0x00020001, 44100, 44100 * 4,
                        0x00100004, 0x61746164, wav_bytes};
    fseek(wav_file, 0, SEEK_SET);
    fwrite(hdr, 4, 11, wav_file);
    fclose(wav_file);
    wav_file = 0;
}

static int perf_drops;

/* FE_PERF=2: where a slow frame's time went (main thread), logged per frame over 20 ms */
enum { PS_MOVIE, PS_UPLOAD, PS_SEEK, PS_PRESENT, PS_PACE, PS_N };
static double perf_stage[PS_N];
static int perf_detail(void) { static int d = -1; if (d < 0) { const char *e = getenv("FE_PERF"); d = e && atoi(e) >= 2; } return d; }
uint64_t perf_now(void) { return perf_detail() ? SDL_GetPerformanceCounter() : 0; }
void perf_add(int stage, uint64_t t0) {
    if (t0 && stage >= 0 && stage < PS_N)
        perf_stage[stage] += (double)(SDL_GetPerformanceCounter() - t0) * 1000.0 / (double)SDL_GetPerformanceFrequency();
}

/* audio queue in milliseconds (0 without audio) */
static double audio_queue_ms(void) {
    return audio_dev ? SDL_GetQueuedAudioSize(audio_dev) / (44100.0 * 4) * 1000.0 : 0;
}

static void audio_frame(void) {
    static int16_t buf[4096 * 2];
    int n;
    while ((n = spu_take_samples(buf, 4096)) > 0) {
        if (audio_volume < 100) /* the port's output volume, also in the WAV dump */
            for (int i = 0; i < 2 * n; i++) buf[i] = (int16_t)(buf[i] * (audio_volume < 0 ? 0 : audio_volume) / 100);
        if (wav_file) { fwrite(buf, 4, (size_t)n, wav_file); wav_bytes += (uint32_t)n * 4; }
        if (audio_dev) {
            /* the frame limiter keeps the queue near its target; drop only if it ever runs far ahead */
            uint32_t q = SDL_GetQueuedAudioSize(audio_dev);
            if (q < 44100 * 4 / 4) SDL_QueueAudio(audio_dev, buf, (uint32_t)n * 4);
            else perf_drops++;
        }
    }
}
static int fb_w = 320, fb_h = 240;

/* ------------------------------------------------------------------ PNG (stored deflate) */

static uint32_t crc_table[256];
static uint32_t crc32_(const uint8_t *p, size_t n, uint32_t c) {
    if (!crc_table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t v = i;
            for (int k = 0; k < 8; k++) v = (v & 1) ? 0xEDB88320u ^ (v >> 1) : v >> 1;
            crc_table[i] = v;
        }
    c = ~c;
    while (n--) c = crc_table[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return ~c;
}
static void be32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = (uint8_t)v; }
static void chunk(FILE *f, const char *type, const uint8_t *data, uint32_t n) {
    uint8_t h[8]; be32(h, n); memcpy(h + 4, type, 4);
    fwrite(h, 1, 8, f);
    if (n) fwrite(data, 1, n, f);
    uint32_t c = crc32_((const uint8_t *)type, 4, 0);
    c = crc32_(data, n, c);
    uint8_t t[4]; be32(t, c); fwrite(t, 1, 4, f);
}
static void write_png(const char *path, const uint32_t *rgb, int w, int h) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    uint8_t ihdr[13]; be32(ihdr, w); be32(ihdr + 4, h); ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = ihdr[11] = ihdr[12] = 0;
    chunk(f, "IHDR", ihdr, 13);
    size_t raw_n = (size_t)h * (1 + w * 3);
    uint8_t *raw = malloc(raw_n);
    if (!raw) fatal("out of memory (%zu bytes for %s)", raw_n, path);
    for (int y = 0; y < h; y++) {
        uint8_t *r = raw + (size_t)y * (1 + w * 3);
        r[0] = 0;
        for (int x = 0; x < w; x++) {
            uint32_t c = rgb[y * w + x];
            r[1 + x * 3] = (uint8_t)(c >> 16); r[2 + x * 3] = (uint8_t)(c >> 8); r[3 + x * 3] = (uint8_t)c;
        }
    }
    size_t nblk = (raw_n + 65534) / 65535;
    uint8_t *z = malloc(raw_n + nblk * 5 + 6);
    if (!z) fatal("out of memory (%zu bytes for %s)", raw_n + nblk * 5 + 6, path);
    size_t zo = 0;
    z[zo++] = 0x78; z[zo++] = 0x01;
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < raw_n; i++) { a = (a + raw[i]) % 65521; b = (b + a) % 65521; }
    for (size_t off = 0; off < raw_n; off += 65535) {
        size_t n = raw_n - off < 65535 ? raw_n - off : 65535;
        z[zo++] = off + n >= raw_n ? 1 : 0;
        z[zo++] = (uint8_t)n; z[zo++] = (uint8_t)(n >> 8);
        z[zo++] = (uint8_t)~n; z[zo++] = (uint8_t)(~n >> 8);
        memcpy(z + zo, raw + off, n); zo += n;
    }
    be32(z + zo, (b << 16) | a); zo += 4;
    chunk(f, "IDAT", z, (uint32_t)zo);
    chunk(f, "IEND", 0, 0);
    fclose(f);
    free(raw); free(z);
}

/* ------------------------------------------------------------------ display */

static void build_frame(void) {
    GpuDisplay d;
    gpu_display(&d);
    fb_w = d.w; fb_h = d.h;
    if (fb_w > 1024) fb_w = 1024;
    if (fb_h > 512) fb_h = 512;
    for (int y = 0; y < fb_h; y++) {
        for (int x = 0; x < fb_w; x++) {
            uint32_t c;
            if (!d.enabled) c = 0;
            else if (d.depth24) {
                int bx = d.x * 2 + x * 3; /* byte offset in the VRAM row */
                const uint8_t *row = (const uint8_t *)&vram[((d.y + y) & 511) * 1024];
                int b0 = bx & 2047;
                uint8_t r = row[b0], g = row[(b0 + 1) & 2047], bb = row[(b0 + 2) & 2047];
                c = ((uint32_t)r << 16) | ((uint32_t)g << 8) | bb;
            } else {
                uint16_t v = vram[((d.y + y) & 511) * 1024 + ((d.x + x) & 1023)];
                uint32_t r = (v & 31) << 3, g = ((v >> 5) & 31) << 3, b = ((v >> 10) & 31) << 3;
                c = (r << 16) | (g << 8) | b;
            }
            frame_rgb[y * fb_w + x] = c;
        }
    }
}

static void dump_vram_png(const char *path) {
    static uint32_t buf[1024 * 512];
    for (int i = 0; i < 1024 * 512; i++) {
        uint16_t v = vram[i];
        buf[i] = (((v & 31) << 3) << 16) | ((((v >> 5) & 31) << 3) << 8) | (((v >> 10) & 31) << 3);
    }
    write_png(path, buf, 1024, 512);
}

void input_init(void);
void input_open_controllers(void);
void input_event(const SDL_Event *e);
void input_poll(void);
void input_list(void);

SDL_Window *frontend_window(void) { return win; }

static void poll_input(void) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) exit(0);
        input_event(&e);
        if (menu_event(&e)) continue; /* the controls menu is open */
        if (e.type == SDL_KEYDOWN && !e.key.repeat && e.key.keysym.sym >= SDLK_F1 && e.key.keysym.sym <= SDLK_F4)
            disc_insert(e.key.keysym.sym - SDLK_F1); /* manual disc swap */
        /* Alt+Enter: fullscreen (the bindable actions, F7 / F12 / F10 / F11 by default, come through input_poll) */
        if (e.type == SDL_KEYDOWN && !e.key.repeat && e.key.keysym.sym == SDLK_RETURN && (e.key.keysym.mod & KMOD_ALT))
            menu_action(ACT_FULLSCREEN);
        /* single remaster parts, for comparisons */
        if (e.type == SDL_KEYDOWN && !e.key.repeat && e.key.keysym.sym == SDLK_F6 && !(e.key.keysym.mod & KMOD_CTRL))
            texrep_on = !texrep_on;
        /* Ctrl+F6: the scene actors' next look (actors.txt "look=", e.g. the cog wheels' painted styles) */
        if (e.type == SDL_KEYDOWN && !e.key.repeat && e.key.keysym.sym == SDLK_F6 && (e.key.keysym.mod & KMOD_CTRL))
            models_scene_look_cycle();
        /* Shift+F8 (F8 alone loads a save state since 4 Nov) */
        /* Ctrl+F8: the remodels' look fixes (mlook.h) off / on, for comparisons */
        if (e.type == SDL_KEYDOWN && !e.key.repeat && e.key.keysym.sym == SDLK_F8 && (e.key.keysym.mod & KMOD_CTRL)) {
            extern int model_fix, model_fix_saved;
            if (model_fix) { model_fix_saved = model_fix; model_fix = 0; }
            else model_fix = model_fix_saved ? model_fix_saved : 251;
            LOG("remodel look fixes %s (model_fix %d)\n", model_fix ? "on" : "off", model_fix);
        }
        if (e.type == SDL_KEYDOWN && !e.key.repeat && e.key.keysym.sym == SDLK_F8 && (e.key.keysym.mod & KMOD_SHIFT)) {
            models_on = !models_on;
            LOG("remaster character models %s\n", models_on ? "on" : "off");
        }
    }
    input_poll();
}

/* scripted input for headless runs: --press FRAME:MASK:LEN, --stick FRAME:LX:LY:LEN (0..255) */
static struct { int64_t frame; uint16_t mask; int len; } script[64];
static int nscript;
static struct { int64_t frame; int lx, ly, len; } sticks[64];
static int nsticks;
static uint64_t fuzz_seed;
static int64_t fuzz_from;

/* input recording: every windowed session writes the pad state of each frame to last_session.rec
 * (next to the exe); --replay FILE plays it back, so a crash can be reproduced exactly */
static FILE *rec_file, *replay_file;

static void record_or_replay(void) {
    if (replay_file) {
        uint8_t r[6];
        if (fread(r, 1, 6, replay_file) == 6) {
            pad_buttons = (uint16_t)(r[0] | (r[1] << 8));
            memcpy(pad_axes, r + 2, 4);
        } /* past its end: the scripted presses (--press), else nothing */
    }
    if (rec_file) {
        uint8_t r[6] = {(uint8_t)pad_buttons, (uint8_t)(pad_buttons >> 8), pad_axes[0], pad_axes[1], pad_axes[2], pad_axes[3]};
        fwrite(r, 1, 6, rec_file);
        fflush(rec_file);
    }
}

/* save states (state.c): after a load the replay goes on at the state's frame, the recording starts over so that
 * --load-state last_session.state --replay last_session.rec repeats the session (the frames before the state are
 * neutral records), the sound queued before the load is dropped and the pacing starts afresh */
static uint64_t pace_next;
static char rec_path[1100];
void frontend_state_loaded(const char *path) {
    int64_t f = (int64_t)frame_count;
    if (replay_file) _fseeki64(replay_file, f * 6, SEEK_SET);
    if (rec_file && rec_path[0]) {
        fflush(rec_file);
        _chsize_s(_fileno(rec_file), 0);
        _fseeki64(rec_file, 0, SEEK_SET);
        uint8_t r[6] = {0, 0, 0x80, 0x80, 0x80, 0x80};
        for (int64_t i = 0; i < f; i++) fwrite(r, 1, 6, rec_file);
        fflush(rec_file);
        char sp[1200];
        snprintf(sp, sizeof sp, "%.*s.state", (int)(strlen(rec_path) - 4), rec_path);
        FILE *in = fopen(path, "rb"), *out = in ? fopen(sp, "wb") : 0;
        if (in && out) { char b[65536]; size_t n; while ((n = fread(b, 1, sizeof b, in)) > 0) fwrite(b, 1, n, out); }
        if (in) fclose(in);
        if (out) fclose(out);
        LOG("state: recording restarted at frame %lld (replay: --load-state %s --replay %s)\n", (long long)f, sp, rec_path);
    }
    if (audio_dev) SDL_ClearQueuedAudio(audio_dev);
    pace_next = 0;
}

extern void (*hw_before_swap)(void);
static void pace(void) {
    if (audio_dev && opt_fast_until && (int64_t)frame_count == opt_fast_until)
        SDL_ClearQueuedAudio(audio_dev); /* end of fast-forward: start real time with an empty queue */
    if (!opt_fast && (int64_t)frame_count >= opt_fast_until) {
        uint64_t next = pace_next;
        uint64_t freq = SDL_GetPerformanceFrequency();
        uint64_t now = SDL_GetPerformanceCounter();
        /* pace at the game's own frame rate, nudged by the audio queue so that sound is never
         * dropped or starved: target 60 ms queued, at most 0.5 % faster or slower */
        double rate = (double)CPU_HZ / (double)CYCLES_PER_FRAME;
        if (audio_dev) {
            double err = (audio_queue_ms() - 60.0) / 60.0;
            if (err > 1) err = 1;
            if (err < -1) err = -1;
            rate *= 1.0 - 0.005 * err;
        }
        uint64_t period = (uint64_t)((double)freq / rate);
        if (!next || now > next + period * 4) next = now;
        next += period;
        pace_next = next;
        uint64_t pt = perf_now();
        for (;;) {
            uint64_t t = SDL_GetPerformanceCounter();
            if (t >= next) break;
            uint64_t left_ms = (next - t) * 1000 / freq;
            if (left_ms > 1) SDL_Delay((uint32_t)(left_ms - 1));
        }
        perf_add(PS_PACE, pt);
        if (getenv("FE_PERF")) {
            static uint64_t last, t0;
            static double worst, qmin = 1e9, qmax;
            static int frames, slow;
            uint64_t shown = SDL_GetPerformanceCounter();
            if (last) { double dt = (double)(shown - last) * 1000.0 / freq; if (dt > worst) worst = dt; if (dt > 18.0) slow++; }
            last = shown;
            double q = audio_queue_ms();
            if (q < qmin) qmin = q;
            if (q > qmax) qmax = q;
            if (!t0) t0 = now;
            if (++frames >= 60) {
                LOG("perf: frame %llu worst %.1f ms (%d over 18 ms), audio queue %.0f-%.0f ms, drops %d\n",
                    (unsigned long long)frame_count, worst, slow, qmin, qmax, perf_drops);
                frames = 0; slow = 0; worst = 0; qmin = 1e9; qmax = 0;
            }
        }
    }
}

void frontend_shot(const char *path) { /* agent.c: the software frame */
    build_frame();
    write_png(path, frame_rgb, fb_w, fb_h);
}

int frontend_vblank(void) {
    state_vblank(); /* a load requested at the last vblank (leaves through a longjmp) */
    if (opt_headless) {
        uint16_t b = 0;
        for (int i = 0; i < nscript; i++)
            if ((int64_t)frame_count >= script[i].frame && (int64_t)frame_count < script[i].frame + script[i].len)
                b |= script[i].mask;
        pad_buttons = b;
        memset(pad_axes, 0x80, 4);
        for (int i = 0; i < nsticks; i++)
            if ((int64_t)frame_count >= sticks[i].frame && (int64_t)frame_count < sticks[i].frame + sticks[i].len) {
                pad_axes[2] = (uint8_t)sticks[i].lx;
                pad_axes[3] = (uint8_t)sticks[i].ly;
            }
        if (fuzz_seed && (int64_t)frame_count >= fuzz_from) {
            /* soak testing: deterministic random play (walking, running, actions, inventory, pause) */
            static int64_t next_change;
            static uint16_t fb;
            static uint8_t flx = 0x80, fly = 0x80;
            if ((int64_t)frame_count >= next_change) {
                fuzz_seed = fuzz_seed * 6364136223846793005ULL + 1442695040888963407ULL;
                uint32_t r = (uint32_t)(fuzz_seed >> 33);
                static const uint16_t acts[] = {0x4000, 0x8000, 0x2000, 0x1000, 0x0400, 0x0800, 0x0100, 0x0200,
                                                0x0010, 0x0040, 0x0020, 0x0080, 0x0810, 0x0410, 0x0008, 0x0001};
                fb = 0; flx = fly = 0x80;
                switch (r % 6) {
                case 0: case 1: flx = (uint8_t)(r >> 8); fly = (uint8_t)(r >> 16); break; /* stick */
                case 2: fb = acts[8 + ((r >> 4) % 6)]; break;                             /* d-pad / run */
                case 3: case 4: fb = acts[(r >> 4) % 8]; break;                           /* buttons */
                default: fb = (r >> 4) % 10 == 0 ? acts[14 + ((r >> 8) & 1)] : 0; break; /* start/select or idle */
                }
                next_change = (int64_t)frame_count + 10 + (int64_t)((r >> 20) % 80);
            }
            pad_buttons |= fb;
            if (flx != 0x80 || fly != 0x80) { pad_axes[2] = flx; pad_axes[3] = fly; }
        }
        {
            extern int agent_on; extern uint16_t agent_buttons; extern uint8_t agent_lx, agent_ly;
            if (agent_on) {
                pad_buttons |= agent_buttons;
                if (agent_lx != 0x80 || agent_ly != 0x80) { pad_axes[2] = agent_lx; pad_axes[3] = agent_ly; }
            }
        }
        menu_vblank();
        menu_filter_pad();
        record_or_replay();
        controls_frame();
        audio_frame();
        if ((opt_dump_every && (int64_t)frame_count >= opt_dump_from && frame_count % (uint64_t)opt_dump_every == 0) ||
            props_shot_due()) {
            char p[512];
            GpuDisplay dd;
            gpu_display(&dd);
            snprintf(p, sizeof p, "%s/frame_%06llu.png", opt_dump_dir, (unsigned long long)frame_count);
            if (hw_active && !dd.depth24) {
                int w, h;
                if (!getenv("FE_DUMP_NO_FRAME")) { /* with --dump-screen: the screen pictures only */
                    uint32_t *img = hw_read_display(&dd, &w, &h);
                    write_png(p, img, w, h);
                    free(img);
                }
                if (opt_screen_w) {
                    uint32_t *shot = hw_screen_capture(&dd, opt_screen_w, opt_screen_h);
                    snprintf(p, sizeof p, "%s/screen_%06llu.png", opt_dump_dir, (unsigned long long)frame_count);
                    write_png(p, shot, opt_screen_w, opt_screen_h);
                    free(shot);
                }
                if (getenv("FE_DUMP_SW")) {
                    build_frame();
                    snprintf(p, sizeof p, "%s/sw_%06llu.png", opt_dump_dir, (unsigned long long)frame_count);
                    write_png(p, frame_rgb, fb_w, fb_h);
                }
            } else {
                build_frame();
                write_png(p, frame_rgb, fb_w, fb_h);
            }
            if (!getenv("FE_DUMP_NO_VRAM")) { /* per-frame dumps of long ranges: the VRAM pictures are 1.5 MB each */
                snprintf(p, sizeof p, "%s/vram_%06llu.png", opt_dump_dir, (unsigned long long)frame_count);
                dump_vram_png(p);
            }
        }
        if (hw_active) hw_bench_frame(); /* FE_GPU_TIME only */
        return 1;
    }
    menu_vblank();
    poll_input();
    menu_filter_pad();
    record_or_replay();
    controls_frame();
    audio_frame();
    if (hw_active) {
        GpuDisplay dd;
        gpu_display(&dd);
        int ww, wh;
        SDL_GL_GetDrawableSize(win, &ww, &wh);
        int vw = ww, vh = ww * 3 / 4; /* 4:3 picture, letterboxed */
        if (vh > wh) { vh = wh; vw = wh * 4 / 3; }
        int vx = (ww - vw) / 2, vy = (wh - vh) / 2;
        if (dd.depth24) {
            build_frame();
            hw_present_rgb(frame_rgb, fb_w, fb_h, vx, vy, vw, vh, ww, wh);
        } else {
            uint64_t pt = perf_now();
            hw_present(&dd, vx, vy, vw, vh, ww, wh);
            perf_add(PS_PRESENT, pt);
        }
        hw_bench_frame(); /* FE_GPU_TIME only */
        if (perf_detail()) {
            static uint64_t last;
            uint64_t t = SDL_GetPerformanceCounter();
            double dt = last ? (double)(t - last) * 1000.0 / (double)SDL_GetPerformanceFrequency() : 0, q = audio_queue_ms();
            last = t;
            if (dt > 20.0 || (audio_dev && q < 15.0)) {
                double present = perf_stage[PS_PRESENT] - perf_stage[PS_PACE];
                double emu = dt - perf_stage[PS_MOVIE] - perf_stage[PS_UPLOAD] - perf_stage[PS_SEEK] - perf_stage[PS_PRESENT];
                LOG("perf: frame %llu took %.1f ms: emulation %.1f, movie frame %.1f, 8x upload %.1f, seek %.1f, present %.1f, "
                    "limiter %.1f; audio queue %.0f ms\n", (unsigned long long)frame_count, dt, emu, perf_stage[PS_MOVIE],
                    perf_stage[PS_UPLOAD], perf_stage[PS_SEEK], present, perf_stage[PS_PACE], q);
            }
            memset(perf_stage, 0, sizeof perf_stage);
        }
    } else {
    build_frame();
    if (!tex || 1) {
        static int tw, th;
        if (tw != fb_w || th != fb_h) {
            if (tex) SDL_DestroyTexture(tex);
            tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, fb_w, fb_h);
            tw = fb_w; th = fb_h;
        }
    }
    SDL_UpdateTexture(tex, 0, frame_rgb, fb_w * 4);
    SDL_RenderClear(ren);
    SDL_RenderCopy(ren, tex, 0, 0);
    pace();
    SDL_RenderPresent(ren);
    }
    return 1;
}

/* ------------------------------------------------------------------ disc set */

char disc_paths[4][1024];
int disc_count, disc_current;
char game_serial[16];

/* the given .cue is disc N: the others are '(Disc N)' renamed, in the same folder or in a sibling
 * folder named discN (third_party/fe2_remaster/work layout) */
static void find_disc_set(const char *given) {
    char dir[1024], name[512];
    const char *slash = strrchr(given, '/'), *bs = strrchr(given, '\\');
    if (bs > slash) slash = bs;
    snprintf(dir, sizeof dir, "%.*s", slash ? (int)(slash - given) : 0, given);
    snprintf(name, sizeof name, "%s", slash ? slash + 1 : given);
    char *tag = strstr(name, "(Disc ");
    disc_count = 1;
    snprintf(disc_paths[0], sizeof disc_paths[0], "%s", given);
    disc_current = 0;
    if (!tag || tag[6] < '1' || tag[6] > '4') return;
    int me = tag[6] - '1';
    char parent[1024];
    const char *ps = strrchr(dir, '/'), *pb = strrchr(dir, '\\');
    if (pb > ps) ps = pb;
    snprintf(parent, sizeof parent, "%.*s", ps ? (int)(ps - dir) : 0, dir);
    for (int n = 0; n < 4; n++) {
        tag[6] = (char)('1' + n);
        char cand[1024];
        disc_paths[n][0] = 0;
        snprintf(cand, sizeof cand, "%s/%s", dir, name);
        FILE *f = fopen(cand, "rb");
        if (!f) { snprintf(cand, sizeof cand, "%s/disc%d/%s", parent, n + 1, name); f = fopen(cand, "rb"); }
        if (f) { fclose(f); snprintf(disc_paths[n], sizeof disc_paths[n], "%s", cand); }
    }
    snprintf(disc_paths[me], sizeof disc_paths[me], "%s", given);
    disc_current = me;
    disc_count = 4;
    for (int n = 0; n < 4; n++) LOG("disc %d: %s\n", n + 1, disc_paths[n][0] ? disc_paths[n] : "(not found)");
}

int disc_insert(int index) {
    if (index < 0 || index >= 4 || !disc_paths[index][0]) return 0;
    if (!cdrom_swap(disc_paths[index])) return 0;
    disc_current = index;
    LOG("disc swap: disc %d inserted\n", index + 1);
    return 1;
}

/* ------------------------------------------------------------------ boot */

static uint64_t fnv64(const uint8_t *p, size_t n) {
    uint64_t h = 1469598103934665603ULL;
    while (n--) { h ^= *p++; h *= 1099511628211ULL; }
    return h;
}

static int boot_exe(void) {
    uint32_t lba, size;
    char exe_path[128] = "SLUS_012.66";
    if (cdrom_find_file("SYSTEM.CNF", &lba, &size)) {
        uint8_t s[2352];
        cdrom_read_sector(lba, s);
        char cnf[2049]; memcpy(cnf, s + 24, 2048); cnf[2048] = 0;
        char *b = strstr(cnf, "BOOT");
        if (b) {
            b = strchr(b, ':');
            if (b) {
                b++;
                while (*b == '\\') b++;
                int n = 0;
                while (*b && *b != ';' && *b != '\r' && *b != '\n' && n < 120) exe_path[n++] = *b++;
                exe_path[n] = 0;
                /* serial for cheats: SLUS_012.66 -> SLUS-01266 */
                int k = 0;
                for (const char *c = exe_path; *c && k < 15; c++) {
                    if (*c == '_') game_serial[k++] = '-';
                    else if (*c != '.') game_serial[k++] = *c;
                }
                game_serial[k] = 0;
            }
        }
    }
    if (!cdrom_find_file(exe_path, &lba, &size)) { fprintf(stderr, "exe %s not found on disc\n", exe_path); return 0; }
    uint8_t *exe = malloc(size + 2048);
    for (uint32_t i = 0; i * 2048 < size; i++) {
        uint8_t s[2352];
        cdrom_read_sector(lba + i, s);
        memcpy(exe + i * 2048, s + 24, 2048);
    }
    if (memcmp(exe, "PS-X EXE", 8)) { fprintf(stderr, "not a PS-X EXE\n"); return 0; }
    uint32_t hdr[16];
    memcpy(hdr, exe + 0x10, sizeof hdr);
    uint32_t pc = hdr[0], gp = hdr[1], taddr = hdr[2], tsize = hdr[3];
    uint32_t baddr = hdr[6], bsize = hdr[7], saddr = hdr[8], ssize = hdr[9];
    uint64_t h = fnv64(exe + 0x800, tsize);
    if (h != recomp_exe_hash) {
        fprintf(stderr, "this disc's exe (%s) is not the one the code was recompiled from\n", exe_path);
        return 0;
    }
    memcpy(&psx_ram[taddr & 0x1FFFFF], exe + 0x800, tsize);
    if (bsize) memset(&psx_ram[baddr & 0x1FFFFF], 0, bsize);
    LOG("boot: %s pc=%08X text %08X+%X bss %08X+%X\n", exe_path, pc, taddr, tsize, baddr, bsize);
    free(exe);
    memset(&cpu, 0, sizeof cpu);
    cpu.r[28] = gp;
    cpu.r[29] = cpu.r[30] = saddr ? saddr + ssize : 0x801FFFF0;
    cpu.sr = 0x40000401;
    cpu.next_event = 0;
    if (!opt_remaster && fe_data_game[0]) remaster_dir = fe_data_game; /* movies/discN, models/ */
    game_init(remaster_dir);
    props_init();
    models_init(remaster_dir);
    subs_init();
    gopt_init();
    controls_init();
    gamefix_init();
    { void agent_init(void); agent_init(); } /* playtest: FE2's HUD text */
    state_init();
    LOG("entering %08X\n", pc);
    state_boot(pc); /* the game from its entry point, or from --load-state's state */
    LOG("exe returned\n");
    return 1;
}

/* the data folder: --data / ini data (relative to the exe), else data/ next to the exe, else ../data (the
 * repository's build/ and build_fe1/); NOTES.md "Self-contained data folder" */
static int is_dir(const char *p) {
    struct stat st;
    return p[0] && !stat(p, &st) && (st.st_mode & S_IFDIR);
}
static void find_data(int from_ini) {
    char *base = SDL_GetBasePath();
    const char *b = base ? base : "";
    char c[2][1100];
    int n = 0;
    if (opt_data && opt_data[0]) {
        int abs = opt_data[0] == '/' || opt_data[0] == '\\' || opt_data[1] == ':';
        snprintf(c[n++], sizeof c[0], "%s%s", abs || !from_ini ? "" : b, opt_data);
    } else {
        snprintf(c[n++], sizeof c[0], "%sdata", b);
        snprintf(c[n++], sizeof c[0], "%s../data", b);
    }
    if (base) SDL_free(base);
    for (int i = 0; i < n; i++)
        if (is_dir(c[i])) {
            /* absolute: a relative --data (resolved against the current directory) would mean something else to
             * the processes started from another directory (the movie decoder) */
#ifdef _WIN32
            if (!_fullpath(fe_data, c[i], sizeof fe_data))
                snprintf(fe_data, sizeof fe_data, "%s", c[i]);
#else
            char *r = realpath(c[i], NULL);
            snprintf(fe_data, sizeof fe_data, "%s", r ? r : c[i]);
            free(r);
#endif
            snprintf(fe_data_game, sizeof fe_data_game, "%s/" FE_GAME, fe_data);
            break;
        }
    if (fe_data[0]) LOG("data: %s\n", fe_data);
    else LOG("data: no data folder (%s)\n", c[0]);
}

/* no disc given: the first '(Disc 1)' .cue in discs/<game> next to the data folder (F:/fe_port/discs/fe2) */
static const char *data_disc(void) {
    static char p[1300];
    if (!fe_data[0]) return 0;
    char pat[1200];
    snprintf(pat, sizeof pat, "%s/../discs/" FE_GAME "/*(Disc 1)*.cue", fe_data);
    struct _finddata_t fd;
    intptr_t h = _findfirst(pat, &fd);
    if (h == -1) return 0;
    snprintf(p, sizeof p, "%s/../discs/" FE_GAME "/%s", fe_data, fd.name);
    _findclose(h);
    return p;
}

/* [game] section of <game>port.ini next to the exe (written with the defaults on first run) */
static char set_disc[1024], set_remaster[1024], set_data[1024];
static int data_from_ini;
static void load_settings(void) {
    extern void write_default_ini(const char *path);
    char *base = SDL_GetBasePath();
    char path[1100];
    snprintf(path, sizeof path, "%s" FE_GAME "port.ini", base ? base : "");
    if (base) SDL_free(base);
    FILE *f = fopen(path, "r");
    if (!f) { write_default_ini(path); f = fopen(path, "r"); }
    if (!f) return;
    char line[1200];
    int in_game = 0;
    while (fgets(line, sizeof line, f)) {
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (*s == '[') { in_game = !strncmp(s, "[game]", 6); continue; }
        if (!in_game || *s == ';' || *s == '#') continue;
        char *eq = strchr(s, '=');
        if (!eq) continue;
        char *k = s, *v = eq + 1;
        char *ke = eq;
        while (ke > k && (ke[-1] == ' ' || ke[-1] == '\t')) ke--;
        *ke = 0;
        while (*v == ' ' || *v == '\t') v++;
        size_t n = strlen(v);
        while (n && (v[n - 1] == '\n' || v[n - 1] == '\r' || v[n - 1] == ' ')) v[--n] = 0;
        if (n >= 2 && v[0] == '"' && v[n - 1] == '"') { v[n - 1] = 0; v++; }
        if (!strcmp(k, "disc")) snprintf(set_disc, sizeof set_disc, "%s", v);
        else if (!strcmp(k, "data") && v[0]) { snprintf(set_data, sizeof set_data, "%s", v); opt_data = set_data; data_from_ini = 1; }
        else if (!strcmp(k, "remaster") && v[0]) {
            snprintf(set_remaster, sizeof set_remaster, "%s", strcmp(v, "off") ? v : "");
            opt_remaster = set_remaster;
        }
        else if (!strcmp(k, "scale") && v[0]) opt_scale = atoi(v);
        else if (!strcmp(k, "renderer") && v[0]) { static char r[8]; snprintf(r, sizeof r, "%s", v); opt_renderer = r; }
        /* display = fullscreen | windowed ("fullscreen = 0" of the first settings files was only the old default) */
        else if (!strcmp(k, "display")) opt_fullscreen = strcmp(v, "windowed") != 0;
        else if (!strcmp(k, "fill")) { extern int hw_fill_mode; hw_fill_mode = atoi(v); }
        else if (!strcmp(k, "models")) models_on = atoi(v);
        else if (!strcmp(k, "smooth")) smooth_on = atoi(v);
        else if (!strcmp(k, "model_fix")) { extern int model_fix; model_fix = (int)strtol(v, 0, 0); }
        else if (!strcmp(k, "volume")) audio_volume = atoi(v);
        else if (!strcmp(k, "bloom")) fx_bloom = atoi(v);
        else if (!strcmp(k, "bloom_intensity")) fx_bloom_intensity = (float)atof(v);
        else if (!strcmp(k, "bloom_threshold")) fx_bloom_threshold = (float)atof(v);
        else if (!strcmp(k, "bloom_radius")) fx_bloom_radius = (float)atof(v);
        else if (!strcmp(k, "bloom_halo")) fx_bloom_halo = (float)atof(v);
        else if (!strcmp(k, "vignette")) fx_vignette = atoi(v);
        else if (!strcmp(k, "vignette_strength")) fx_vignette_strength = (float)atof(v);
        else if (!strcmp(k, "vignette_start")) fx_vignette_start = (float)atof(v);
        else if (!strcmp(k, "vignette_softness")) fx_vignette_softness = (float)atof(v);
        else if (!strcmp(k, "grain")) fx_grain = atoi(v);
        else if (!strcmp(k, "grain_strength")) fx_grain_strength = (float)atof(v);
        else if (!strcmp(k, "grain_rows")) fx_grain_rows = (float)atof(v);
        else if (!strcmp(k, "grain_refresh")) fx_grain_refresh = (float)atof(v);
        else if (!strcmp(k, "loop_fade")) movie_loop_fade = atoi(v);
        else if (!strcmp(k, "scene_look")) snprintf(models_scene_look, sizeof models_scene_look, "%s", v);
        else if (!strcmp(k, "movie_60fps")) movie_hfr = atoi(v);
        else if (!strcmp(k, "movie_cam_fix")) models_camfix = atoi(v);
        else if (!strcmp(k, "cd_speed")) { extern int cdrom_speedup; cdrom_speedup = atoi(v); }
        else if (!strcmp(k, "precision")) { extern int pgxp_on; pgxp_on = atoi(v); }
        else if (!strcmp(k, "directional")) controls_directional = atoi(v);
        else if (!strcmp(k, "fx_particles")) { extern int fx_particles; fx_particles = atoi(v); }
        else if (!strcmp(k, "textures")) { if (!strcmp(v, "off")) texrep_enabled = texrep_on = 0; else snprintf(texrep_root, sizeof texrep_root, "%s", v); }
    }
    fclose(f);
}

/* FE_CAPTURE=FROM:TO (frames) [FE_CAPTURE_DIR]: every frame the window presents in that range, as PNG */
extern void (*hw_capture_presented)(int w, int h);
uint32_t *hw_read_back(int w, int h);
static void capture_presented(int w, int h) {
    static int64_t a = -2, b;
    if (a == -2) {
        const char *e = getenv("FE_CAPTURE");
        a = -1;
        if (e) sscanf(e, "%lld:%lld", (long long *)&a, (long long *)&b);
    }
    static uint32_t *imgs[256];
    static uint64_t frames[256];
    static int n, written;
    if (a < 0 || written || (int64_t)frame_count < a) return;
    if ((int64_t)frame_count <= b && n < 256) { /* kept in memory: writing now would change the timing */
        frames[n] = frame_count;
        imgs[n++] = hw_read_back(w, h);
        return;
    }
    written = 1;
    const char *dir = getenv("FE_CAPTURE_DIR");
    for (int i = 0; i < n; i++) {
        char p[600];
        snprintf(p, sizeof p, "%s/present_%06llu.png", dir ? dir : ".", (unsigned long long)frames[i]);
        write_png(p, imgs[i], w, h);
        free(imgs[i]);
    }
}

int main(int argc, char **argv) {
    const char *disc = 0, *logp = 0, *memcard = 0;
    int headless_arg = 0;
    for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "--headless")) headless_arg = 1;
    if (!headless_arg) load_settings(); /* tests stay independent of the user's settings */
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--headless")) opt_headless = 1;
        else if (!strcmp(argv[i], "--fast")) opt_fast = 1;
        else if (!strcmp(argv[i], "--agent")) { extern int agent_on; agent_on = 1; }
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) opt_max_frames = atoll(argv[++i]);
        else if (!strcmp(argv[i], "--dump-every") && i + 1 < argc) opt_dump_every = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--dump-from") && i + 1 < argc) opt_dump_from = atoll(argv[++i]);
        else if (!strcmp(argv[i], "--dump-dir") && i + 1 < argc) opt_dump_dir = argv[++i];
        else if (!strcmp(argv[i], "--wav") && i + 1 < argc) {
            wav_file = fopen(argv[++i], "wb");
            if (wav_file) { uint8_t z[44] = {0}; fwrite(z, 1, 44, wav_file); atexit(wav_finish); }
        }
        else if (!strcmp(argv[i], "--mute")) opt_mute = 1;
        else if (!strcmp(argv[i], "--renderer") && i + 1 < argc) opt_renderer = argv[++i];
        else if (!strcmp(argv[i], "--remaster") && i + 1 < argc) opt_remaster = argv[++i];
        else if (!strcmp(argv[i], "--data") && i + 1 < argc) { opt_data = argv[++i]; data_from_ini = 0; }
        else if (!strcmp(argv[i], "--no-remaster")) opt_remaster = "";
        else if (!strcmp(argv[i], "--no-models")) models_on = 0;
        else if (!strcmp(argv[i], "--no-smooth")) smooth_on = 0; /* remodels at the game's rate */
        else if (!strcmp(argv[i], "--no-fx")) fx_bloom = fx_vignette = fx_grain = 0; /* no picture effects */
        else if (!strcmp(argv[i], "--loop-fade") && i + 1 < argc) movie_loop_fade = atoi(argv[++i]); /* 0 = hard cut */
        else if (!strcmp(argv[i], "--scene-look") && i + 1 < argc) /* the scene actors' look (actors.txt "look=") */
            snprintf(models_scene_look, sizeof models_scene_look, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--no-movie-60fps")) movie_hfr = 0; /* 60 fps movies at the STR rate */
        else if (!strcmp(argv[i], "--no-movie-cam-fix")) models_camfix = 0; /* characters with the game's own camera */
        else if (!strcmp(argv[i], "--cd-speed") && i + 1 < argc) { extern int cdrom_speedup; cdrom_speedup = atoi(argv[++i]); }
        else if (!strcmp(argv[i], "--no-pgxp")) { extern int pgxp_on; pgxp_on = 0; } /* integer vertices, as the PS1 */
        else if (!strcmp(argv[i], "--pgxp")) { extern int pgxp_on; pgxp_on = 1; }    /* precise vertices (experimental) */
        else if (!strcmp(argv[i], "--no-textures")) texrep_enabled = texrep_on = 0;
        else if (!strcmp(argv[i], "--textures") && i + 1 < argc) snprintf(texrep_root, sizeof texrep_root, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--tank-controls")) controls_directional = 0;
        else if (!strcmp(argv[i], "--no-dither")) { extern int opt_dither; opt_dither = 0; }
        else if (!strcmp(argv[i], "--no-fill")) { extern int hw_fill_mode; hw_fill_mode = 0; }
        else if (!strcmp(argv[i], "--fill") && i + 1 < argc) { extern int hw_fill_mode; hw_fill_mode = atoi(argv[++i]); }
        else if (!strcmp(argv[i], "--fullscreen")) opt_fullscreen = 1;
        else if (!strcmp(argv[i], "--windowed")) opt_fullscreen = 0;
        else if (!strcmp(argv[i], "--dump-screen") && i + 1 < argc) sscanf(argv[++i], "%dx%d", &opt_screen_w, &opt_screen_h);
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc) opt_scale = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--no-autoswap")) { extern int opt_no_autoswap; opt_no_autoswap = 1; }
        else if (!strcmp(argv[i], "--cheats") && i + 1 < argc) cheat_file = argv[++i];
        else if (!strcmp(argv[i], "--cheat") && i + 1 < argc && ncheat_names < 16) cheat_names[ncheat_names++] = argv[++i];
        else if (!strcmp(argv[i], "--fast-until") && i + 1 < argc) opt_fast_until = atoll(argv[++i]);
        else if (!strcmp(argv[i], "--fuzz") && i + 1 < argc) {
            unsigned long long sd = 1; long long from = 0;
            sscanf(argv[++i], "%llu:%lld", &sd, &from);
            fuzz_seed = sd ? sd : 1; fuzz_from = from;
        }
        else if (!strcmp(argv[i], "--analog")) { pad_set_analog(1); pad_mode_forced = 1; }
        else if (!strcmp(argv[i], "--digital")) { pad_set_analog(0); pad_mode_forced = 1; }
        else if (!strcmp(argv[i], "--stick") && i + 1 < argc && nsticks < 64) {
            long long f; int lx, ly, len = 60;
            if (sscanf(argv[++i], "%lld:%d:%d:%d", &f, &lx, &ly, &len) >= 3) {
                sticks[nsticks].frame = f; sticks[nsticks].lx = lx; sticks[nsticks].ly = ly; sticks[nsticks].len = len; nsticks++;
            }
        }
        else if (!strcmp(argv[i], "--list-controllers")) {
            SDL_SetMainReady();
            SDL_Init(SDL_INIT_GAMECONTROLLER);
            SDL_Delay(1500); /* bluetooth controllers can take a moment */
            SDL_PumpEvents();
            input_list();
            return 0;
        }
        else if (!strcmp(argv[i], "--input-test")) {
            /* small window; prints the PS1 buttons and stick values the port sees (close window to quit) */
            static const char *const names[16] = {"SELECT", "L3", "R3", "START", "UP", "RIGHT", "DOWN", "LEFT",
                                                  "L2", "R2", "L1", "R1", "TRIANGLE", "CIRCLE", "CROSS", "SQUARE"};
            SDL_SetMainReady();
            SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER);
            SDL_Window *w = SDL_CreateWindow("fe2port input test (keep this window focused)", SDL_WINDOWPOS_CENTERED,
                                             SDL_WINDOWPOS_CENTERED, 480, 120, 0);
            input_init();
            input_open_controllers();
            char last[512] = "";
            for (;;) {
                SDL_Event e;
                while (SDL_PollEvent(&e)) {
                    if (e.type == SDL_QUIT) return 0;
                    input_event(&e);
                }
                input_poll();
                char line[512];
                int n = snprintf(line, sizeof line, "LX=%3d LY=%3d RX=%3d RY=%3d ", pad_axes[2], pad_axes[3], pad_axes[0], pad_axes[1]);
                for (int b = 0; b < 16; b++)
                    if (pad_buttons & (1u << b)) n += snprintf(line + n, sizeof line - (size_t)n, " %s", names[b]);
                if (pad_analog_toggle) snprintf(line + n, sizeof line - (size_t)n, " ANALOG");
                if (strcmp(line, last)) { printf("%s\n", line); fflush(stdout); snprintf(last, sizeof last, "%s", line); }
                SDL_Delay(16);
            }
            (void)w;
        }
        else if (!strcmp(argv[i], "--memcard") && i + 1 < argc) memcard = argv[++i];
        else if (!strcmp(argv[i], "--log") && i + 1 < argc) logp = argv[++i];
        else if (!strcmp(argv[i], "-v")) log_level = 2;
        else if (!strcmp(argv[i], "--replay") && i + 1 < argc) replay_file = fopen(argv[++i], "rb");
        else if (!strcmp(argv[i], "--load-state") && i + 1 < argc) state_set_load(argv[++i]);
        else if (!strcmp(argv[i], "--save-state-at") && i + 1 < argc) {
            long long f; char *colon;
            const char *a = argv[++i];
            f = strtoll(a, &colon, 10);
            if (*colon == ':' && colon[1]) state_add_save_at(f, colon + 1);
            else fprintf(stderr, "--save-state-at FRAME:FILE\n");
        }
        else if (!strcmp(argv[i], "--record") && i + 1 < argc) { snprintf(rec_path, sizeof rec_path, "%s", argv[i + 1]); rec_file = fopen(argv[++i], "wb"); }
        else if (!strcmp(argv[i], "--press") && i + 1 < argc && nscript < 64) {
            long long f; unsigned m; int len = 6;
            if (sscanf(argv[++i], "%lld:%x:%d", &f, &m, &len) >= 2) {
                script[nscript].frame = f; script[nscript].mask = (uint16_t)m; script[nscript].len = len; nscript++;
            }
        }
        else disc = argv[i];
    }
    if (logp) log_file = fopen(logp, "w");
    find_data(data_from_ini);
    if (!disc && set_disc[0]) disc = set_disc;
    if (!disc) disc = data_disc();
    if (!disc) {
        const char *msg = "No disc image given.\n\nStart it with the Disc 1 .cue file as argument (or drop the .cue on the exe),\n"
                          "or set 'disc = <path to Disc 1 .cue>' in the [game] section of " FE_GAME "port.ini next to the exe,\n"
                          "or put the disc images into discs\\" FE_GAME " next to the data folder.";
        fprintf(stderr, "usage: " FE_GAME "port <disc.cue> [options]\n\n%s\n", msg);
        if (!opt_headless) SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, FE_GAME "port", msg, 0);
        return 1;
    }
    if (!opt_headless && !rec_file && !replay_file) {
        char *base = SDL_GetBasePath();
        char p[1024];
        snprintf(p, sizeof p, "%slast_session.rec", base ? base : "");
        snprintf(rec_path, sizeof rec_path, "%s", p);
        rec_file = fopen(p, "wb");
        if (base) SDL_free(base);
    }
    if (!cdrom_open(disc)) {
        char m[1200];
        snprintf(m, sizeof m, "Cannot open the disc image %s", disc);
        fprintf(stderr, "%s\n", m);
        if (!opt_headless) SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, FE_GAME "port", m, 0);
        return 1;
    }
    find_disc_set(disc);
    if (!opt_headless) {
        SDL_SetMainReady();
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_AUDIO) != 0) { fprintf(stderr, "SDL: %s\n", SDL_GetError()); return 1; }
        if (!opt_mute) {
            SDL_AudioSpec want = {0}, have;
            want.freq = 44100; want.format = AUDIO_S16LSB; want.channels = 2; want.samples = 1024;
            audio_dev = SDL_OpenAudioDevice(0, 0, &want, &have, 0);
            if (audio_dev) SDL_PauseAudioDevice(audio_dev, 0);
            else LOG("audio: %s\n", SDL_GetError());
        }
        int want_hw = !opt_renderer || !strcmp(opt_renderer, "hw");
        win = SDL_CreateWindow(strcmp(FE_GAME, "fe1") ? "Fear Effect 2 (native port)" : "Fear Effect (native port)", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 960, 720,
                               SDL_WINDOW_RESIZABLE | (want_hw ? SDL_WINDOW_OPENGL : 0));
        if (want_hw && !hw_init(win, opt_scale)) {
            LOG("hw renderer unavailable, using the software renderer\n");
            SDL_DestroyWindow(win);
            win = SDL_CreateWindow(strcmp(FE_GAME, "fe1") ? "Fear Effect 2 (native port)" : "Fear Effect (native port)", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 960,
                                   720, SDL_WINDOW_RESIZABLE);
        }
        if (opt_fullscreen) { SDL_SetWindowFullscreen(win, SDL_WINDOW_FULLSCREEN_DESKTOP); SDL_ShowCursor(SDL_DISABLE); }
        if (hw_active) hw_before_swap = pace;
        if (hw_active) hw_overlay_draw = menu_draw;
        input_action_hook = menu_action;
        if (hw_active && getenv("FE_CAPTURE")) hw_capture_presented = capture_presented;
        if (!hw_active) {
            ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
            SDL_RenderSetLogicalSize(ren, 960, 720);
        }
        input_open_controllers();
    }
    {
        /* memory card 1: a 128 KB card image, by default build/memcards/card1.mcd (shared by all discs) */
        char p[1024];
        if (!memcard) {
            char *base = SDL_GetBasePath();
            snprintf(p, sizeof p, "%smemcards", base ? base : "");
            _mkdir(p);
            snprintf(p, sizeof p, "%smemcards/card1.mcd", base ? base : "");
            if (base) SDL_free(base);
            memcard = p;
        }
        mcd_init(memcard);
    }
    if (cheat_file) {
        cheats_load(cheat_file);
        for (int i = 0; i < ncheat_names; i++) cheats_enable(cheat_names[i]);
    }
    if (opt_headless && opt_renderer && !strcmp(opt_renderer, "hw")) {
        SDL_SetMainReady();
        if (!hw_init(0, opt_scale)) LOG("hw renderer unavailable (headless)\n");
        else hw_overlay_draw = menu_draw; /* notices, subtitles; the controls menu only with FE_MENU_TEST (menu_vblank) */
    }
    {
        /* the 8x movie sets and models: --remaster DIR (development); without it the data folder's
         * (boot_exe) */
        if (opt_remaster && opt_remaster[0]) remaster_dir = opt_remaster;
    }
    input_init();
    {
        extern int input_analog_on_start;
        if (!pad_mode_forced) pad_set_analog(input_analog_on_start);
    }
    gte_init();
    gpu_init();
    cdrom_init();
    bios_init();
    hooks_init();
    mdec_write(0x1F801824, 0x80000000u);
    return boot_exe() ? 0 : 1;
}
