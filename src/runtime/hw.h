/* Native hardware layer: shared declarations. */
#pragma once
#include <stdint.h>
#include <stdio.h>
#include "recomp.h"

#ifndef FE_GAME
#define FE_GAME "fe2" /* build.py sets it per game: names the exe, the settings file, the window */
#endif

#define CPU_HZ 33868800
#define CYCLES_PER_LINE 2153
#define LINES_PER_FRAME 263
#define CYCLES_PER_FRAME (CYCLES_PER_LINE * LINES_PER_FRAME)

enum {
    IRQ_VBLANK = 0, IRQ_GPU = 1, IRQ_CDROM = 2, IRQ_DMA = 3, IRQ_TMR0 = 4, IRQ_TMR1 = 5, IRQ_TMR2 = 6,
    IRQ_PAD = 7, IRQ_SIO = 8, IRQ_SPU = 9, IRQ_LIGHTPEN = 10
};

/* logging */
extern FILE *log_file;
extern int log_level;
void logf_(int level, const char *fmt, ...);
#define LOG(...) logf_(1, __VA_ARGS__)
#define DBG(...) do { if (log_level >= 2) logf_(2, __VA_ARGS__); } while (0)
void fatal(const char *fmt, ...);

/* host pointer into PS1 RAM (NULL if outside RAM/scratchpad) */
static inline void *psx_ptr(uint32_t a) { return mem_fast(a); }

/* interrupts / scheduler */
extern uint32_t i_stat, i_mask;
void irq_raise(int n);
void sched_at(int64_t when); /* make sure hw_events runs no later than this */
void irq_check(void);        /* deliver a pending interrupt now if allowed */
extern int in_exception;

/* DMA */
uint32_t dma_read(uint32_t addr);
void dma_write(uint32_t addr, uint32_t v);

/* timers */
uint32_t timer_read(uint32_t addr);
void timer_write(uint32_t addr, uint32_t v);
void timers_update(void);
void timers_hblank(int lines);

/* GPU */
extern uint16_t vram[1024 * 512];
void gpu_init(void);
void gpu_gp0(uint32_t v);
void gpu_gp1(uint32_t v);
uint32_t gpu_read(void);
uint32_t gpu_stat(void);
void gpu_vblank(void);
typedef struct {
    int x, y, w, h; /* display area in VRAM */
    int depth24;
    int enabled;
    int hres, vres; /* output resolution */
} GpuDisplay;
void gpu_display(GpuDisplay *d);

/* hardware renderer (gpu_hw.c): mirrors the drawing at N x resolution for display */
typedef struct { int x, y, u, v; uint8_t r, g, b; } HwVert;
extern uint32_t gpu_word_addr; /* gpu.c: the RAM address of the GP0 word being fed (DMA), else ~0 */
/* main.c, FE_PERF=2: time a main-thread stage of the frame (0 movie frame, 1 8x upload, 2 decoder seek) */
uint64_t perf_now(void);
void perf_add(int stage, uint64_t t0);
/* 60 fps in-between frames (gpu_hw.c, NOTES.md "60 fps character animation") */
extern int smooth_on;
/* picture effects (gpu_hw.c, NOTES.md "Picture effects"): on / off and the fork's shader options */
extern int fx_bloom, fx_vignette, fx_grain;
extern float fx_bloom_intensity, fx_bloom_threshold, fx_bloom_radius, fx_bloom_halo;
extern float fx_vignette_strength, fx_vignette_start, fx_vignette_softness;
extern float fx_grain_strength, fx_grain_rows, fx_grain_refresh;
void smooth_flip(void);                          /* gpu.c: the display moved to another buffer */
void smooth_mesh_event(uint32_t pose);           /* models.c: a remodel is about to be drawn, its pose kept as id */
void smooth_bg_mark(int tag);                    /* game_fe2.c: a 60 fps movie background is about to be drawn */
int movie_tag_pic(int tag, int dk);              /* game_fe2.c: the picture number of that (FE_DBG_HFR) */
void movie_inbetween_end(void);                /* game_fe2.c: after that replay (a borrowed buffer restored) */
void hw_vram_stash(int n, const int16_t *x, const int16_t *y, int w, int h, int back); /* gpu_hw.c: columns aside / back */
int movie_inbetween(int tag, int dk);            /* game_fe2.c: that background's picture dk vblanks later into its buffer */
int models_pose_kept(uint32_t pose);             /* models.c: the pose is still in its ring */
void models_smooth_draw(uint32_t pose, float t); /* models.c: draw it posed between this frame and the next */
typedef struct {
    int textured, raw, semi, semi_mode, depth;
    int tx, ty, clut_x, clut_y, twin;
    int set_mask, check_mask;
    int da_x1, da_y1, da_x2, da_y2;
    unsigned rtex; int rmode;          /* texture pack image (texrep.c): 1 full colour, 2 index map */
    int rnearest;                      /* sprites: nearest texel (tiles stay seamless), polygons: bilinear */
    int rstretch_x, rstretch_y;        /* the draw magnifies its texels on that axis: sample edge texels at their centres */
    float ru0, ru1, rv0, rv1;          /* its texel range (exclusive ends) */
    float rox, roy, rsw, rsh;          /* the replaced block in texels of the page */
} HwState;
unsigned hw_tex_rgba(int w, int h, const unsigned char *rgba, int linear); /* linear 2: + mipmaps */
unsigned hw_matte_tex(const float *z, int w, int h); /* a scene matte's depths (models.c MATTE lines): R16F, nearest */
void hw_tex_free(unsigned tex);
void hw_bench_frame(void); /* FE_GPU_TIME (main.c, each vblank): GPU timing of the scene actors and the in-betweens */
void hw_gpt_mark(int slot, int begin); /* FE_GPU_TIME: a timed section (0 scene actors, 1 in-between) begins / ends */
extern int texrep_on, texrep_enabled;
extern char texrep_root[1024];
void texrep_upload(int x, int y, int w, int h);
void texrep_invalidate(int x, int y, int w, int h);
void texrep_copy(int sx, int sy, int dx, int dy, int w, int h); /* a VRAM copy carries the blocks inside it */
void texrep_apply(HwState *st, int umin, int vmin, int umax, int vmax);
/* codepanel.c: FE2's code door panel cursor and key-press flash, drawn from the keys' rectangles (1 = drawn) */
int codepanel_quad(int tx, int ty, int depth, int clut_x, int clut_y, int umin, int vmin, int umax, int vmax, int x, int y,
                   int ox, int oy, const HwState *base);
extern int hw_active, hw_scale;
extern int gpu_hw_skip;
int movie_piece_last_column(int x0, int x1, int y0, int y1); /* game_fe2.c */
/* game_fe2.c: the movie piece just checked was drawn with these vertices (VRAM x, y; texture u, v; after the column
 * fix): where the picture lies in VRAM (mattes) */
void movie_piece_vertices(int nv, const int *x, const int *y, const int *u, const int *v, int tx, int ty);
int movie_bg_rect(float *r); /* game_fe2.c: this pass's movie picture in VRAM (native x0, y0, x1, y1); 0 = not known */
extern int controls_directional; /* camera-relative controls (controls.c), on by default */
void controls_frame(void);
void controls_init(void); /* the observer on the player's animation state machine (boot_exe) */
void controls_vblank(void);
void gpu_draw_area(int *x1, int *y1, int *x2, int *y2, int *offx, int *offy);
void hw_tri(const HwVert *a, const HwVert *b, const HwVert *c, const HwState *st);
void hw_tri_ex(const HwVert *a, const HwVert *b, const HwVert *c, const HwState *st, const float *px,
               const float *py, const float *pw); /* precise positions and perspective w (pgxp) */
void hw_quad_xy(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, uint32_t col,
                const HwState *st);
void hw_fill(int x, int y, int w, int h, uint16_t c);
void hw_upload(int x, int y, int w, int h);
void hw_copy(int sx, int sy, int dx, int dy, int w, int h);
void hw_mark_dirty(int y0, int y1);
void hw_flush(void);
void hw_hd_upload(const uint8_t *rgb, int w, int h);
void hw_hd_blit(int pw, int ph, int px0, int px1, int dx, int dy);
void hw_hd_slot(int slot);       /* upload slot of the two calls above: 0 the game's uploads, 1 the 60 fps sub-steps */
int hw_hd_fade_begin(void);      /* room loop crossfade: freeze the uploaded picture (1 = done) */
void hw_hd_fade_apply(float w);  /* blend the frozen picture over the just uploaded one, weight w */
extern int movie_loop_fade;      /* game_fe2.c, ini loop_fade: crossfade length in movie frames (0 = off) */
void movie_keep(int m, int f0, int n); /* movie.c: keep cached frames f0..f0+n-1 of movie m from eviction */
int movie_shown(int *frame);     /* game_fe2.c: movie (index) and frame of the 8x picture on screen, -1 none */
int movie_bg_drawn(void);        /* game_fe2.c: this GPU pass has drawn the room background's two pieces */
unsigned movie_shown_run(void);  /* game_fe2.c: the run (cut / room change / load; not a loop restart) of that picture */
float movie_shown_frac(void);    /* game_fe2.c: time since that frame began, in movie frames (0..1; scene actors) */
extern int models_camfix;        /* models.c, ini movie_cam_fix: characters drawn with the movie picture's camera (cam_tracks.txt) */
extern int movie_hfr;           /* game_fe2.c, ini movie_60fps: 60 fps movies show their in-between pictures */
int movie_rate(int m);           /* movie.c: pictures per STR frame (movies.txt's 6th field, default 1) */
extern int (*gpu_upload_hook)(int x, int y, int w, int h); /* returns 1 if it drew a replacement */

/* hooks.c */
typedef int (*NativeFn)(void);
void hook_register(uint32_t addr, NativeFn fn);
int hook_index(uint32_t addr);
int hook_register_in(const char *image, uint32_t addr, NativeFn fn); /* one image's function only; its index */
void menu_gpu_packet(uint32_t a); /* menu.c: a GPU display-list packet at a is drawn */
void call_original(int idx);

/* movie.c: the remaster's replacement movie sets */
int movies_load(const char *dir);
int movie_lookup(uint32_t lba, int *frame);
const uint8_t *movie_frame(int m, int frame, int wait_ms, int *w, int *h);
void movie_release(void);
const uint8_t *movie_peek(int m, int frame, int *w, int *h); /* the same, cached frames only; movie_release() */
extern int movie_piece_native;  /* game_fe2.c: the movie piece just checked is a native stand-in (-1 none) */
extern int64_t fb_draw_frame[2], fb_room_frame[2]; /* gpu.c: per framebuffer, the last vblank anything / the room picture was drawn into it */
extern int fb_room_w[2]; /* gpu.c: the draw area's width when the room picture was drawn */
extern int movie_fb_native[2]; /* gpu.c: per framebuffer (VRAM top / bottom half): its background is a stand-in */
void movie_prefetch(int m, int frame);
extern void (*cdrom_seek_hook)(uint32_t file_sector);
const char *movie_name(int m);
void game_init(const char *remaster_dir);

/* CD-ROM */
int cdrom_open(const char *cue_path);
int cdrom_swap(const char *cue_path);
int cdrom_reading(void);
extern uint64_t cdrom_last_movie_frame;

/* disc set (discs 1..4 found next to the one given) and the inserted disc, 0-based */
extern char disc_paths[4][1024];
extern int disc_count, disc_current;
extern char game_serial[16]; /* e.g. SLUS-01266, from SYSTEM.CNF */
/* the port's data folder (NOTES.md "Self-contained data folder"): fe_data = the root (fonts, ffmpeg.exe),
 * fe_data_game = <root>/<FE_GAME> (movies, models, textures); "" when none was found */
extern char fe_data[1024], fe_data_game[1100];
/* FE_TRACE_FILES: every path the port opens is appended to that file (filetrace.c) */
void ftrace(const char *what, const char *path);
int disc_insert(int index);

/* per-game behaviour (game_fe2.c) and cheats (cheats.c) */
void game_vblank(void);
void cheats_vblank(void);
int cheats_load(const char *path);
void cheats_enable(const char *name_part);
void cdrom_init(void);
uint8_t cdrom_read(uint32_t addr);
void cdrom_write(uint32_t addr, uint8_t v);
void cdrom_update(void);
uint32_t cdrom_dma_read(void);
int cdrom_read_sector(uint32_t lba, uint8_t *out2352);
int cdrom_find_file(const char *path, uint32_t *lba, uint32_t *size);

/* MDEC */
uint32_t mdec_read(uint32_t addr);
void mdec_write(uint32_t addr, uint32_t v);
void mdec_dma_in(const uint32_t *src, int words);
void mdec_dma_out(uint32_t *dst, int words);

/* SPU */
uint16_t spu_read(uint32_t addr);
void spu_write(uint32_t addr, uint16_t v);
void spu_dma_write(const uint32_t *src, int words);
void spu_dma_read(uint32_t *dst, int words);
void spu_xa_sector(const uint8_t *sector2352);
void spu_update(void);
int spu_take_samples(int16_t *dst, int max_frames); /* stereo frames */

/* BIOS (HLE) */
void bios_init(void);
void bios_call(uint32_t table, uint32_t fn);
void bios_exception(void);
void bios_vblank(void);
void bios_update(void);
void mcd_init(const char *path);

/* SIO0 (pads) */
uint32_t sio_read(uint32_t addr, int size);
void sio_write(uint32_t addr, uint32_t v, int size);
void sio_update(void);
void pad_vblank(void);
void pad_set_analog(int on);

/* pad */
extern uint16_t pad_buttons;  /* active-high PS1 button mask */
extern uint8_t pad_axes[4];    /* right X, right Y, left X, left Y; 0x80 = centre */
extern int pad_analog_toggle;  /* ANALOG button pressed this frame */
void input_rumble(int small_on, int large);

/* front end */
int frontend_vblank(void); /* present a frame, poll input; returns 0 to quit */
extern int opt_headless, opt_fast;
extern int64_t opt_max_frames;
extern uint64_t frame_count;
