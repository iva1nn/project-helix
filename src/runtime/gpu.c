/* GPU: GP0 drawing commands rasterised in software into a 1024x512 16-bit VRAM, GP1 display control.
 * Round 1 renderer; a hardware renderer at higher resolution replaces the rasteriser later. */
#include <stdlib.h>
#include <math.h>
#include "hw.h"

uint16_t vram[1024 * 512];
int (*gpu_upload_hook)(int x, int y, int w, int h);

static struct {
    uint32_t stat;
    int tp_x, tp_y, tp_depth, semi; /* current texpage */
    int tex_disable;
    int tw_mask_x, tw_mask_y, tw_off_x, tw_off_y;
    int da_x1, da_y1, da_x2, da_y2;
    int off_x, off_y;
    int set_mask, check_mask;
    int disp_x, disp_y;
    int h1, h2, v1, v2;
    int dmode; /* GP1(08) */
    int disp_off;
    int dma_dir;
    int dither;
    int draw_to_disp;
    /* command assembly */
    uint32_t buf[64];
    uint32_t bufaddr[64]; /* each word's RAM address (DMA), for the precise vertices */
    int n, need;
    /* VRAM transfers */
    int load_active, lx, ly, lw, lh, lpos;
    int store_active, sx, sy, sw, sh, spos;
    uint32_t read_latch;
    int polyline, pl_count, pl_want_color, pl_x, pl_y;
    uint32_t pl_c, pl_cur_c;
} g;

void gpu_init(void) {
    memset(&g, 0, sizeof g);
    g.disp_off = 1;
    g.da_x2 = 1023; g.da_y2 = 511;
    g.h1 = 0x200; g.h2 = 0x200 + 2560; g.v1 = 0x10; g.v2 = 0x10 + 240;
}

/* ------------------------------------------------------------------ pixel helpers */

static inline uint16_t blend(uint16_t bg, uint16_t fg, int mode) {
    int br = bg & 31, bgc = (bg >> 5) & 31, bb = (bg >> 10) & 31;
    int fr = fg & 31, fgc = (fg >> 5) & 31, fb = (fg >> 10) & 31;
    int r, gg, b;
    switch (mode) {
    case 0: r = (br + fr) >> 1; gg = (bgc + fgc) >> 1; b = (bb + fb) >> 1; break;
    case 1: r = br + fr; gg = bgc + fgc; b = bb + fb; break;
    case 2: r = br - fr; gg = bgc - fgc; b = bb - fb; break;
    default: r = br + (fr >> 2); gg = bgc + (fgc >> 2); b = bb + (fb >> 2); break;
    }
    if (r < 0) r = 0; if (r > 31) r = 31;
    if (gg < 0) gg = 0; if (gg > 31) gg = 31;
    if (b < 0) b = 0; if (b > 31) b = 31;
    return (uint16_t)(r | (gg << 5) | (b << 10));
}

static inline void plot(int x, int y, uint16_t c, int semi, int semi_mode) {
    if (x < g.da_x1 || x > g.da_x2 || y < g.da_y1 || y > g.da_y2) return;
    uint16_t *p = &vram[(y & 511) * 1024 + (x & 1023)];
    if (g.check_mask && (*p & 0x8000)) return;
    uint16_t out = semi ? blend(*p, c, semi_mode) : c;
    out = (out & 0x7FFF) | (c & 0x8000) | (g.set_mask ? 0x8000 : 0);
    *p = out;
}

static inline uint16_t rgb24to15(uint32_t c) {
    return (uint16_t)(((c >> 3) & 31) | (((c >> 11) & 31) << 5) | (((c >> 19) & 31) << 10));
}

typedef struct { int tx, ty, depth, clut_x, clut_y; } TexState;

static inline uint16_t texel(const TexState *t, int u, int v) {
    u &= 0xFF; v &= 0xFF;
    u = (u & ~(g.tw_mask_x * 8)) | ((g.tw_off_x & g.tw_mask_x) * 8);
    v = (v & ~(g.tw_mask_y * 8)) | ((g.tw_off_y & g.tw_mask_y) * 8);
    switch (t->depth) {
    case 0: {
        uint16_t w = vram[((t->ty + v) & 511) * 1024 + ((t->tx + (u >> 2)) & 1023)];
        int idx = (w >> ((u & 3) * 4)) & 0xF;
        return vram[t->clut_y * 1024 + ((t->clut_x + idx) & 1023)];
    }
    case 1: {
        uint16_t w = vram[((t->ty + v) & 511) * 1024 + ((t->tx + (u >> 1)) & 1023)];
        int idx = (w >> ((u & 1) * 8)) & 0xFF;
        return vram[t->clut_y * 1024 + ((t->clut_x + idx) & 1023)];
    }
    default:
        return vram[((t->ty + v) & 511) * 1024 + ((t->tx + u) & 1023)];
    }
}

static inline uint16_t modulate(uint16_t tx, int r, int gg, int b) {
    int tr = tx & 31, tg = (tx >> 5) & 31, tb = (tx >> 10) & 31;
    tr = (tr * r) >> 7; tg = (tg * gg) >> 7; tb = (tb * b) >> 7;
    if (tr > 31) tr = 31; if (tg > 31) tg = 31; if (tb > 31) tb = 31;
    return (uint16_t)(tr | (tg << 5) | (tb << 10) | (tx & 0x8000));
}

/* 4x4 ordered dither (texpage bit 9) for shaded and texture-blended polygons: added to the 8-bit colour
 * before it is cut to 5 bits (software renderer; --no-dither turns it off) */
int opt_dither = 1;
static const int8_t dither4[4][4] = {{-4, 0, -3, 1}, {2, -2, 3, -1}, {-3, 1, -4, 0}, {3, -1, 2, -2}};

static inline int dith5(int c8, int d) {
    c8 += d;
    return c8 < 0 ? 0 : c8 > 255 ? 31 : c8 >> 3;
}

static inline uint16_t modulate_dither(uint16_t tx, int r, int gg, int b, int d) {
    int tr = ((tx & 31) * 8 * r) >> 7, tg = (((tx >> 5) & 31) * 8 * gg) >> 7, tb = (((tx >> 10) & 31) * 8 * b) >> 7;
    return (uint16_t)(dith5(tr, d) | (dith5(tg, d) << 5) | (dith5(tb, d) << 10) | (tx & 0x8000));
}

static void set_texpage(uint32_t v) {
    g.tp_x = (v & 0xF) * 64;
    g.tp_y = ((v >> 4) & 1) * 256;
    g.semi = (v >> 5) & 3;
    g.tp_depth = (v >> 7) & 3;
    if (g.tp_depth == 3) g.tp_depth = 2;
    g.dither = (v >> 9) & 1;
    { static int seen; if (g.dither && !seen++) LOG("gpu: dithering enabled by the game (frame %llu)\n", (unsigned long long)frame_count); }
    g.draw_to_disp = (v >> 10) & 1;
    g.tex_disable = (v >> 11) & 1;
    g.stat = (g.stat & ~0x87FFu) | (v & 0x7FF) | ((v & 0x800) << 4);
}

/* ------------------------------------------------------------------ triangles */

typedef struct { int x, y; int r, gg, b; int u, v; int prec; float fx, fy, fz; } Vtx; /* prec: fx..fz from pgxp */
uint32_t gpu_word_addr = 0xFFFFFFFFu;
extern int pgxp_on;
int pgxp_lookup(uint32_t addr, uint32_t val, float *x, float *y, float *z);

static void draw_tri(const Vtx *a, const Vtx *b, const Vtx *c, int shaded, int textured, int raw, int semi,
                     const TexState *ts) {
    int64_t area = (int64_t)(b->x - a->x) * (c->y - a->y) - (int64_t)(c->x - a->x) * (b->y - a->y);
    if (area == 0) return;
    if (area < 0) { const Vtx *t = b; b = c; c = t; area = -area; }
    int minx = a->x, maxx = a->x, miny = a->y, maxy = a->y;
    const Vtx *vv[3] = {a, b, c};
    for (int i = 1; i < 3; i++) {
        if (vv[i]->x < minx) minx = vv[i]->x;
        if (vv[i]->x > maxx) maxx = vv[i]->x;
        if (vv[i]->y < miny) miny = vv[i]->y;
        if (vv[i]->y > maxy) maxy = vv[i]->y;
    }
    if (maxx - minx > 1023 || maxy - miny > 511) return;
    if (minx < g.da_x1) minx = g.da_x1;
    if (miny < g.da_y1) miny = g.da_y1;
    if (maxx > g.da_x2 + 1) maxx = g.da_x2 + 1;
    if (maxy > g.da_y2 + 1) maxy = g.da_y2 + 1;
    if (minx >= maxx || miny >= maxy) return;

    /* edge functions e_i(x,y) for edges (b,c), (c,a), (a,b); top-left rule via bias */
    int ex[3], ey[3]; int64_t e0[3]; int bias[3];
    const Vtx *p0[3] = {b, c, a}, *p1[3] = {c, a, b};
    for (int i = 0; i < 3; i++) {
        ex[i] = p0[i]->y - p1[i]->y; /* d/dx */
        ey[i] = p1[i]->x - p0[i]->x; /* d/dy */
        e0[i] = (int64_t)(p1[i]->x - p0[i]->x) * (miny - p0[i]->y) - (int64_t)(p1[i]->y - p0[i]->y) * (minx - p0[i]->x);
        /* top edge: horizontal and going left; left edge: going down (in our winding) */
        int top_left = (ex[i] == 0 && ey[i] < 0) || (ex[i] > 0);
        bias[i] = top_left ? 0 : -1;
    }
    /* attribute gradients (float) */
    double inv = 1.0 / (double)area;
    double ar[3] = {a->r, b->r, c->r}, ag[3] = {a->gg, b->gg, c->gg}, ab[3] = {a->b, b->b, c->b};
    double au[3] = {a->u, b->u, c->u}, av[3] = {a->v, b->v, c->v};
    int sm = g.semi;
    int flat_r = a->r, flat_g = a->gg, flat_b = a->b;
    int dith = opt_dither && g.dither && (shaded || (textured && !raw));
    for (int y = miny; y < maxy; y++) {
        int64_t w0 = e0[0], w1 = e0[1], w2 = e0[2];
        for (int x = minx; x < maxx; x++) {
            if (w0 + bias[0] >= 0 && w1 + bias[1] >= 0 && w2 + bias[2] >= 0) {
                double l0 = w0 * inv, l1 = w1 * inv, l2 = w2 * inv;
                int r = flat_r, gg = flat_g, bb = flat_b;
                if (shaded) {
                    r = (int)(l0 * ar[0] + l1 * ar[1] + l2 * ar[2] + 0.5);
                    gg = (int)(l0 * ag[0] + l1 * ag[1] + l2 * ag[2] + 0.5);
                    bb = (int)(l0 * ab[0] + l1 * ab[1] + l2 * ab[2] + 0.5);
                }
                if (textured) {
                    int u = (int)(l0 * au[0] + l1 * au[1] + l2 * au[2] + 0.001);
                    int v = (int)(l0 * av[0] + l1 * av[1] + l2 * av[2] + 0.001);
                    uint16_t t = texel(ts, u, v);
                    if (t) {
                        uint16_t px = raw ? t : dith ? modulate_dither(t, r, gg, bb, dither4[y & 3][x & 3]) : modulate(t, r, gg, bb);
                        plot(x, y, px, semi && (t & 0x8000), sm);
                    }
                } else if (dith) {
                    int d = dither4[y & 3][x & 3];
                    plot(x, y, (uint16_t)(dith5(r, d) | (dith5(gg, d) << 5) | (dith5(bb, d) << 10)), semi, sm);
                } else {
                    plot(x, y, (uint16_t)((r >> 3) | ((gg >> 3) << 5) | ((bb >> 3) << 10)), semi, sm);
                }
            }
            w0 += ex[0]; w1 += ex[1]; w2 += ex[2];
        }
        e0[0] += ey[0]; e0[1] += ey[1]; e0[2] += ey[2];
    }
}

int gpu_hw_skip; /* the packet being drawn is replaced by a remaster mesh */
int movie_fb_native[2];
/* the screen layout (gpu_hw.c hw_present, NOTES.md "Screen layout: which screens"): per framebuffer (VRAM y 0 / 240,
 * told apart by the draw offset the game sets for it), the last vblank the game drew anything into it, the last vblank
 * it drew the room picture (a movie piece from its movie buffers) into it and the draw area's width then; -1 = not
 * since the start or the last state load */
int64_t fb_draw_frame[2] = {-1, -1}, fb_room_frame[2] = {-1, -1};
int fb_room_w[2];
static void fb_drawn(int x, int y) {
    if (x >= 0 && x < 640 && y >= 0 && y < 480) fb_draw_frame[y >= 200] = (int64_t)frame_count;
}

void gpu_draw_area(int *x1, int *y1, int *x2, int *y2, int *offx, int *offy) {
    *x1 = g.da_x1; *y1 = g.da_y1; *x2 = g.da_x2; *y2 = g.da_y2; *offx = g.off_x; *offy = g.off_y;
}

static int sx11(uint32_t v) { return ((int32_t)(v << 21)) >> 21; }

static HwState hw_state(int textured, int raw, int semi, const TexState *ts) {
    HwState s;
    s.textured = textured != 0; s.raw = raw != 0; s.semi = semi != 0; s.semi_mode = g.semi;
    s.depth = ts ? ts->depth : 0;
    s.tx = ts ? ts->tx : 0; s.ty = ts ? ts->ty : 0;
    s.clut_x = ts ? ts->clut_x : 0; s.clut_y = ts ? ts->clut_y : 0;
    s.twin = g.tw_mask_x | (g.tw_mask_y << 5) | (g.tw_off_x << 10) | (g.tw_off_y << 15);
    s.set_mask = g.set_mask; s.check_mask = g.check_mask;
    s.da_x1 = g.da_x1; s.da_y1 = g.da_y1; s.da_x2 = g.da_x2; s.da_y2 = g.da_y2;
    s.rtex = 0; s.rmode = 0; s.rnearest = 0; s.rox = s.roy = 0; s.rsw = s.rsh = 1;
    s.rstretch_x = s.rstretch_y = 0; s.ru0 = s.ru1 = s.rv0 = s.rv1 = 0;
    return s;
}

/* Effect particles drawn smooth (presentation only, hw renderer; on by default; ini fx_particles = 0 or FE_FX_PARTICLES=0 off).
 * Both games draw blood as sprays of flat untextured triangles of 1-3 PS1 pixels in red, and the ejected shell casings
 * as 2-3 pixel flat quads in brass, bullet hits on metal as pale blue-white shards that fade over three draws (NOTES
 * "Effect textures"): at 8x they show as sharp confetti. With the option the hw renderer draws blood as round droplets
 * and casings as rounded bars at the same place, size and colour (lit centre, darker rim), and puts a soft additive
 * glow under each spark shard (the shard itself stays); the software VRAM gets the game's own triangles, so the game
 * and FE_STATE_HASH are unchanged. */
int fx_particles = -1;
static unsigned fxp_tex[3];
enum { FXP_BLOOD = 1, FXP_BRASS = 2, FXP_SPARK = 3 };

static int fxp_class(uint32_t c) {
    int r = c & 0xFF, gg = (c >> 8) & 0xFF, b = (c >> 16) & 0xFF;
    if (r >= 0x50 && gg * 3 <= r && b * 3 <= r) return FXP_BLOOD;
    if (r >= 0x80 && gg * 10 >= r * 6 && gg * 100 <= r * 95 && b * 2 <= gg) return FXP_BRASS;
    /* the hit shards' three colours: BE BE FF, 5A 5A 78, 1E 1E 28 (pale blue-white fading) */
    if (b >= 0x20 && b >= r && b >= gg && r * 10 >= b * 7 && (r > gg ? r - gg : gg - r) <= 8) return FXP_SPARK;
    return 0;
}

static unsigned fxp_texture(int kind) {
    if (fxp_tex[kind - 1]) return fxp_tex[kind - 1];
    enum { N = 64 };
    static unsigned char px[N * N * 4];
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++) {
            float fx = (x + 0.5f) / N * 2 - 1, fy = (y + 0.5f) / N * 2 - 1, d, lit;
            if (kind == FXP_BLOOD) { /* a droplet: round, lit from the upper left */
                d = sqrtf(fx * fx + fy * fy);
                lit = 1.0f - 0.45f * d + 0.25f * fmaxf(0.f, 0.6f - sqrtf((fx + 0.35f) * (fx + 0.35f) + (fy + 0.35f) * (fy + 0.35f)));
            } else if (kind == FXP_BRASS) { /* a casing: a rounded bar, a bright stripe along it */
                float ax = fabsf(fx) - 0.55f, ay = fabsf(fy);
                d = sqrtf(fmaxf(ax, 0.f) * fmaxf(ax, 0.f) + ay * ay) / 0.38f;
                lit = 1.05f - 1.2f * fabsf(fy + 0.1f);
            } else { /* a spark's glow: a soft round falloff, semi-transparent texels (the draw adds them) */
                d = sqrtf(fx * fx + fy * fy);
                lit = d < 1.f ? (1.f - d) * (1.f - d) : 0.f;
            }
            float a = kind == FXP_SPARK ? (d < 1.f ? 0.5f : 0.f) : d < 0.92f ? 1.f : d < 1.f ? (1.f - d) / 0.08f : 0.f;
            int l = (int)(fminf(fmaxf(lit, 0.f), 1.f) * 255.f + 0.5f);
            unsigned char *o = &px[(y * N + x) * 4];
            o[0] = o[1] = o[2] = (unsigned char)l;
            o[3] = (unsigned char)(a * 255.f + 0.5f);
        }
    fxp_tex[kind - 1] = hw_tex_rgba(N, N, px, 2);
    return fxp_tex[kind - 1];
}

/* a flat untextured opaque polygon of a few pixels in a particle colour: drawn as its smooth shape; 1 = done */
static int fxp_draw(const Vtx *v, int nv, uint32_t col, int mx, int my, int Mx, int My) {
    if (fx_particles < 0) { const char *e = getenv("FE_FX_PARTICLES"); fx_particles = e ? atoi(e) : 1; } /* on by default */
    if (fx_particles <= 0 || !texrep_on || Mx - mx > 24 || My - my > 24) return 0; /* F6 (the remaster switch) too */
    int kind = fxp_class(col);
    if (!kind || kind != FXP_SPARK && (Mx - mx > 4 || My - my > 4)) return 0;
    float cx = 0, cy = 0;
    for (int i = 0; i < nv; i++) { cx += v[i].x; cy += v[i].y; }
    cx = cx / nv + 0.5f; cy = cy / nv + 0.5f;
    float r = (float)((Mx - mx > My - my ? Mx - mx : My - my) + 1) * (kind == FXP_BLOOD ? 0.5f : kind == FXP_BRASS ? 0.6f : 1.1f);
    if (r < 0.8f) r = 0.8f;
    TexState ts = {0, 0, 0, 0, 0};
    HwState st = hw_state(1, 0, kind == FXP_SPARK, &ts);
    if (kind == FXP_SPARK) st.semi_mode = 1; /* additive */
    st.rtex = fxp_texture(kind);
    if (!st.rtex) return 0;
    st.rmode = 1;
    /* the texture's white is the vertex colour x 2 (the shader's modulation: 128 = 1x) */
    int rr = col & 0xFF, gg = (col >> 8) & 0xFF, bb = (col >> 16) & 0xFF;
    int k = kind == FXP_SPARK ? 70 : 128; /* the glow at about half the shard's light */
    uint32_t c2 = (uint32_t)((rr * k / 255) | ((gg * k / 255) << 8) | ((bb * k / 255) << 16));
    hw_quad_xy(cx - r, cy - r, cx + r, cy + r, 0.f, 0.f, 1.f, 1.f, c2, &st);
    return kind != FXP_SPARK; /* a spark's shard is drawn as well, over its glow */
}

static void hw_mirror_tri(const Vtx *a, const Vtx *b, const Vtx *c, const HwState *st) {
    HwVert h[3];
    const Vtx *v[3] = {a, b, c};
    float px[3], py[3], pw[3], zmax = 0;
    int all = 1;
    for (int i = 0; i < 3; i++) {
        h[i].x = v[i]->x; h[i].y = v[i]->y; h[i].u = v[i]->u; h[i].v = v[i]->v;
        h[i].r = (uint8_t)v[i]->r; h[i].g = (uint8_t)v[i]->gg; h[i].b = (uint8_t)v[i]->b;
        px[i] = v[i]->prec ? v[i]->fx : (float)v[i]->x;
        py[i] = v[i]->prec ? v[i]->fy : (float)v[i]->y;
        all &= v[i]->prec;
        if (v[i]->prec && v[i]->fz > zmax) zmax = v[i]->fz;
    }
    /* perspective-correct only when the whole triangle has depths; relative depths are enough */
    for (int i = 0; i < 3; i++) pw[i] = all ? v[i]->fz / zmax : 1.0f;
    hw_tri_ex(&h[0], &h[1], &h[2], st, px, py, pw);
}

static void dirty_rows(int y0, int y1) {
    if (!hw_active) return;
    if (y0 < g.da_y1) y0 = g.da_y1;
    if (y1 > g.da_y2 + 1) y1 = g.da_y2 + 1;
    if (y1 > y0) hw_mark_dirty(y0, y1);
}

/* The action / inventory box frame (both games, the remaster fork's FE2FixHudBoxUV / FE1FixHudBoxUV): drawn from
 * stretched pieces of MINVENT.TIM (4-bit). The right cap's first column is empty (a 1-pixel gap in the frame, also on
 * the console, where the picture shows through in fill mode) and the middle pieces end on the left cap's bar (seams
 * once the texture is upscaled): the cap starts one texel later, the middle ends one earlier. FE2: page 512,0, rows
 * 80-99, middle u 192-216, cap u 239-247. FE1: the same art elsewhere, found by its place in the 64-texel sheet
 * (middle base+0..24, cap base+47..55, 18-20 rows high). */
static void hud_box_uv_fix(Vtx *v, const TexState *ts) {
    if (ts->depth != 0) return;
    int fe1 = !strcmp(game_serial, "SLUS-00920") || !strncmp(game_serial, "SLUS-0105", 9);
    int umin = 255, umax = 0, vmin = 255, vmax = 0;
    for (int i = 0; i < 4; i++) {
        if (v[i].u < umin) umin = v[i].u;
        if (v[i].u > umax) umax = v[i].u;
        if (v[i].v < vmin) vmin = v[i].v;
        if (v[i].v > vmax) vmax = v[i].v;
    }
    int from, to;
    if (fe1) {
        if (vmax - vmin < 18 || vmax - vmin > 20) return;
        if ((umin & 63) == 47 && umax - umin == 8) { from = umin; to = umin + 1; }
        else if ((umin & 63) == 0 && umax - umin == 24) { from = umax; to = umax - 1; }
        else return;
    } else {
        if (ts->tx != 512 || ts->ty != 0 || vmin < 80 || vmax > 99) return;
        if (umin == 239 && umax == 247) { from = 239; to = 240; }
        else if (umin == 192 && umax == 216) { from = 216; to = 215; }
        else return;
    }
    for (int i = 0; i < 4; i++)
        if (v[i].u == from) v[i].u = to;
}

static void cmd_polygon(void) {
    uint32_t c0 = g.buf[0];
    int op = c0 >> 24;
    int shaded = op & 0x10, quad = op & 0x08, textured = op & 0x04, semi = op & 0x02, raw = op & 0x01;
    int nv = quad ? 4 : 3;
    Vtx v[4];
    int k = 0;
    TexState ts = {g.tp_x, g.tp_y, g.tp_depth, 0, 0};
    for (int i = 0; i < nv; i++) {
        uint32_t col = (i == 0 || !shaded) ? c0 : g.buf[k++];
        if (i == 0) k = 1;
        uint32_t xy = g.buf[k++];
        v[i].x = sx11(xy) + g.off_x;
        v[i].y = sx11(xy >> 16) + g.off_y;
        v[i].prec = 0;
        if (pgxp_on && pgxp_lookup(g.bufaddr[k - 1], xy, &v[i].fx, &v[i].fy, &v[i].fz)) {
            v[i].fx += (float)g.off_x;
            v[i].fy += (float)g.off_y;
            v[i].prec = 1;
        }
        v[i].r = col & 0xFF; v[i].gg = (col >> 8) & 0xFF; v[i].b = (col >> 16) & 0xFF;
        if (textured) {
            uint32_t uv = g.buf[k++];
            v[i].u = uv & 0xFF; v[i].v = (uv >> 8) & 0xFF;
            if (i == 0) { ts.clut_x = ((uv >> 16) & 0x3F) * 16; ts.clut_y = (uv >> 22) & 0x1FF; }
            if (i == 1) {
                uint32_t tp = uv >> 16;
                ts.tx = (tp & 0xF) * 64; ts.ty = ((tp >> 4) & 1) * 256;
                ts.depth = (tp >> 7) & 3; if (ts.depth == 3) ts.depth = 2;
                g.semi = (tp >> 5) & 3;
                g.stat = (g.stat & ~0x81FFu) | (tp & 0x1FF);
                g.tp_x = ts.tx; g.tp_y = ts.ty; g.tp_depth = ts.depth;
            }
        } else {
            v[i].u = v[i].v = 0;
        }
    }
    if (!shaded) for (int i = 1; i < nv; i++) { v[i].r = v[0].r; v[i].gg = v[0].gg; v[i].b = v[0].b; }
    if (textured && quad) hud_box_uv_fix(v, &ts);
    if (textured && raw) { /* raw texture: no modulation */ }
    draw_tri(&v[0], &v[1], &v[2], shaded, textured, raw, semi, &ts);
    if (quad) draw_tri(&v[1], &v[2], &v[3], shaded, textured, raw, semi, &ts);
    if (hw_active && !gpu_hw_skip) {
        int mx = v[0].x, Mx = v[0].x, my = v[0].y, My = v[0].y;
        for (int i = 1; i < nv; i++) {
            if (v[i].x < mx) mx = v[i].x;
            if (v[i].x > Mx) Mx = v[i].x;
            if (v[i].y < my) my = v[i].y;
            if (v[i].y > My) My = v[i].y;
        }
        if (Mx - mx <= 1023 && My - my <= 511) { /* the GPU skips oversized polygons */
            if (!textured && !semi && !shaded && fxp_draw(v, nv, c0 & 0xFFFFFF, mx, my, Mx, My)) {
                dirty_rows(my, My + 1);
                return;
            }
            HwState st = hw_state(textured, raw, semi, &ts);
            if (textured) {
                int umin = 255, vmin = 255, umax = 0, vmax = 0;
                for (int i = 0; i < nv; i++) {
                    if (v[i].u < umin) umin = v[i].u;
                    if (v[i].u > umax) umax = v[i].u;
                    if (v[i].v < vmin) vmin = v[i].v;
                    if (v[i].v > vmax) vmax = v[i].v;
                }
                /* FE2's code door panel: its key cursor and key-press flash drawn from the keys' rectangles */
                if (quad && codepanel_quad(ts.tx, ts.ty, ts.depth, ts.clut_x, ts.clut_y, umin, vmin, umax, vmax, mx - g.off_x,
                                           my - g.off_y, g.off_x, g.off_y, &st)) {
                    dirty_rows(my, My + 1);
                    return;
                }
                texrep_apply(&st, umin, vmin, umax > umin ? umax - 1 : umax, vmax > vmin ? vmax - 1 : vmax);
                /* a short strip stretched over a long box (the HUD frames): the redrawn strip's edge columns
                 * would show as seams where the copies meet */
                if (st.rmode) {
                    st.ru0 = (float)umin; st.ru1 = (float)umax; st.rv0 = (float)vmin; st.rv1 = (float)vmax;
                    st.rstretch_x = umax - umin >= 2 && (Mx - mx) > (umax - umin) * 3 / 2;
                    st.rstretch_y = vmax - vmin >= 2 && (My - my) > (vmax - vmin) * 3 / 2;
                }
                /* a movie frame piece reaching its last VRAM column: u stops at that texel (8-bit), which squeezes
                 * the column out and breaks the picture at x = 256 when upscaled; carry the right-hand vertices
                 * one texel on (the software VRAM above is drawn as the PS1 does) */
                movie_piece_native = -1;
                if (ts.depth == 2 && umax > umin &&
                    movie_piece_last_column(ts.tx + umin, ts.tx + umax, ts.ty + vmin, ts.ty + vmax))
                    for (int i = 0; i < nv; i++)
                        if (v[i].u == umax) v[i].u = umax + 1;
                if (movie_piece_native >= 0) { /* scene mattes: where the picture lies (presentation only) */
                    int xs[4], ys[4], us[4], vs[4];
                    for (int i = 0; i < nv; i++) { xs[i] = v[i].x; ys[i] = v[i].y; us[i] = v[i].u; vs[i] = v[i].v; }
                    movie_piece_vertices(nv, xs, ys, us, vs, ts.tx, ts.ty);
                }
                /* which framebuffer (top or bottom half of VRAM) now shows a stand-in background: the window does
                 * not present it (hw_present looks at the framebuffer on display, drawn a frame earlier) */
                if (movie_piece_native >= 0) {
                    movie_fb_native[my >= 256] = movie_piece_native;
                    if (g.off_x >= 0 && g.off_x < 640 && g.off_y >= 0 && g.off_y < 480) {
                        fb_room_frame[g.off_y >= 200] = (int64_t)frame_count;
                        fb_room_w[g.off_y >= 200] = g.da_x2 - g.da_x1 + 1;
                    }
                }
            }
            hw_mirror_tri(&v[0], &v[1], &v[2], &st);
            if (quad) hw_mirror_tri(&v[1], &v[2], &v[3], &st);
            dirty_rows(my, My + 1);
        }
    }
}

static int poly_words(int op) {
    int shaded = op & 0x10, quad = op & 0x08, textured = op & 0x04;
    int nv = quad ? 4 : 3;
    return 1 + nv * (1 + (textured ? 1 : 0)) + (shaded ? nv - 1 : 0);
}

/* ------------------------------------------------------------------ lines */

static void draw_line(int x0, int y0, uint32_t c0, int x1, int y1, uint32_t c1, int shaded, int semi) {
    int dx = abs(x1 - x0), dy = abs(y1 - y0);
    if (dx > 1023 || dy > 511) return;
    if (hw_active) {
        /* one native pixel wide quad along the line, ends included */
        HwState st = hw_state(0, 0, semi, 0);
        float ax = (float)x0, ay = (float)y0, bx = (float)x1, by = (float)y1;
        HwVert q[4];
        float ox0, oy0, ox1, oy1;
        if (dx >= dy) { if (bx < ax) { float t = ax; ax = bx; bx = t; t = ay; ay = by; by = t; uint32_t c = c0; c0 = c1; c1 = c; }
                        ox0 = ax; ox1 = bx + 1; oy0 = ay; oy1 = by; }
        else { if (by < ay) { float t = ax; ax = bx; bx = t; t = ay; ay = by; by = t; uint32_t c = c0; c0 = c1; c1 = c; }
               ox0 = ax; ox1 = bx; oy0 = ay; oy1 = by + 1; }
        int horiz = dx >= dy;
        float px[4] = {ox0, ox1, ox0, ox1}, py[4];
        if (horiz) { py[0] = oy0; py[1] = oy1; py[2] = oy0 + 1; py[3] = oy1 + 1; }
        else { px[0] = ox0; px[1] = ox0 + 1; px[2] = ox1; px[3] = ox1 + 1; py[0] = oy0; py[1] = oy0; py[2] = oy1; py[3] = oy1; }
        uint32_t cs[4] = {c0, shaded ? c1 : c0, horiz ? c0 : (shaded ? c1 : c0), shaded ? c1 : c0};
        if (horiz) { cs[1] = shaded ? c1 : c0; cs[2] = c0; }
        else { cs[1] = c0; cs[2] = shaded ? c1 : c0; }
        for (int i = 0; i < 4; i++) {
            q[i].x = (int)px[i]; q[i].y = (int)py[i]; q[i].u = q[i].v = 0;
            q[i].r = cs[i] & 0xFF; q[i].g = (cs[i] >> 8) & 0xFF; q[i].b = (cs[i] >> 16) & 0xFF;
        }
        hw_tri(&q[0], &q[1], &q[2], &st);
        hw_tri(&q[1], &q[2], &q[3], &st);
        dirty_rows(y0 < y1 ? y0 : y1, (y0 > y1 ? y0 : y1) + 1);
    }
    int n = dx > dy ? dx : dy;
    int r0 = c0 & 0xFF, g0 = (c0 >> 8) & 0xFF, b0 = (c0 >> 16) & 0xFF;
    int r1 = c1 & 0xFF, g1 = (c1 >> 8) & 0xFF, b1 = (c1 >> 16) & 0xFF;
    for (int i = 0; i <= n; i++) {
        int x = n ? x0 + (x1 - x0) * i / n : x0;
        int y = n ? y0 + (y1 - y0) * i / n : y0;
        int r = r0, gg = g0, b = b0;
        if (shaded && n) { r = r0 + (r1 - r0) * i / n; gg = g0 + (g1 - g0) * i / n; b = b0 + (b1 - b0) * i / n; }
        plot(x, y, (uint16_t)((r >> 3) | ((gg >> 3) << 5) | ((b >> 3) << 10)), semi, g.semi);
    }
}

/* ------------------------------------------------------------------ rectangles */

static void cmd_rect(void) {
    uint32_t c0 = g.buf[0];
    int op = c0 >> 24;
    int textured = op & 0x04, semi = op & 0x02, raw = op & 0x01;
    int size = (op >> 3) & 3;
    int k = 1;
    uint32_t xy = g.buf[k++];
    int x = sx11(xy) + g.off_x, y = sx11(xy >> 16) + g.off_y;
    int u0 = 0, v0 = 0;
    TexState ts = {g.tp_x, g.tp_y, g.tp_depth, 0, 0};
    if (textured) {
        uint32_t uv = g.buf[k++];
        u0 = uv & 0xFF; v0 = (uv >> 8) & 0xFF;
        ts.clut_x = ((uv >> 16) & 0x3F) * 16; ts.clut_y = (uv >> 22) & 0x1FF;
    }
    int w, h;
    if (size == 0) { uint32_t wh = g.buf[k++]; w = wh & 0x3FF; h = (wh >> 16) & 0x1FF; }
    else if (size == 1) w = h = 1;
    else if (size == 2) w = h = 8;
    else w = h = 16;
    int r = c0 & 0xFF, gg = (c0 >> 8) & 0xFF, b = (c0 >> 16) & 0xFF;
    uint16_t flat = (uint16_t)((r >> 3) | ((gg >> 3) << 5) | ((b >> 3) << 10));
    if (hw_active && w > 0 && h > 0) {
        HwState st = hw_state(textured, raw, semi, textured ? &ts : 0);
        if (textured) { texrep_apply(&st, u0, v0, u0 + w - 1, v0 + h - 1); st.rnearest = 1; }
        hw_quad_xy((float)x, (float)y, (float)(x + w), (float)(y + h), (float)u0, (float)v0, (float)(u0 + w),
                   (float)(v0 + h), c0 & 0xFFFFFF, &st);
        dirty_rows(y, y + h);
    }
    for (int j = 0; j < h; j++) {
        int py = y + j;
        if (py < g.da_y1 || py > g.da_y2) continue;
        for (int i = 0; i < w; i++) {
            int px = x + i;
            if (px < g.da_x1 || px > g.da_x2) continue;
            if (textured) {
                uint16_t t = texel(&ts, u0 + i, v0 + j);
                if (!t) continue;
                plot(px, py, raw ? t : modulate(t, r, gg, b), semi && (t & 0x8000), g.semi);
            } else {
                plot(px, py, flat, semi, g.semi);
            }
        }
    }
}

static int rect_words(int op) {
    int textured = op & 0x04, size = (op >> 3) & 3;
    return 2 + (textured ? 1 : 0) + (size == 0 ? 1 : 0);
}

/* ------------------------------------------------------------------ VRAM transfers */

static void vram_put(uint16_t v) {
    int x = (g.lx + g.lpos % g.lw) & 1023;
    int y = (g.ly + g.lpos / g.lw) & 511;
    uint16_t *p = &vram[y * 1024 + x];
    if (!(g.check_mask && (*p & 0x8000)))
        *p = v | (g.set_mask ? 0x8000 : 0);
    if (++g.lpos >= g.lw * g.lh) {
        g.load_active = 0;
        texrep_upload(g.lx, g.ly, g.lw, g.lh); /* also without the hw renderer: save states keep the blocks */
        { void fxlog_upload(int x, int y, int w, int h); fxlog_upload(g.lx, g.ly, g.lw, g.lh); } /* FE_DBG_FX (read only) */
        if (hw_active) {
            if (gpu_upload_hook && gpu_upload_hook(g.lx, g.ly, g.lw, g.lh)) hw_mark_dirty(g.ly, g.ly + g.lh);
            else hw_upload(g.lx, g.ly, g.lw, g.lh);
        }
    }
}

static uint16_t vram_get(void) {
    int x = (g.sx + g.spos % g.sw) & 1023;
    int y = (g.sy + g.spos / g.sw) & 511;
    uint16_t v = vram[y * 1024 + x];
    if (++g.spos >= g.sw * g.sh) g.store_active = 0;
    return v;
}

uint32_t gpu_read(void) {
    if (g.store_active) {
        uint32_t lo = vram_get();
        uint32_t hi = g.store_active ? vram_get() : 0;
        g.read_latch = lo | (hi << 16);
    }
    return g.read_latch;
}

static void cmd_fill(void) {
    uint32_t c0 = g.buf[0];
    int x = g.buf[1] & 0x3F0, y = (g.buf[1] >> 16) & 0x1FF;
    int w = ((g.buf[2] & 0x3FF) + 15) & ~15, h = (g.buf[2] >> 16) & 0x1FF;
    uint16_t c = rgb24to15(c0);
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            vram[((y + j) & 511) * 1024 + ((x + i) & 1023)] = c;
    if (hw_active) { hw_fill(x, y, w, h, c); hw_mark_dirty(y, y + h); }
    texrep_invalidate(x, y, w, h);
}

static void cmd_copy(void) {
    int sx = g.buf[1] & 0x3FF, sy = (g.buf[1] >> 16) & 0x1FF;
    int dx = g.buf[2] & 0x3FF, dy = (g.buf[2] >> 16) & 0x1FF;
    int w = g.buf[3] & 0x3FF, h = (g.buf[3] >> 16) & 0x1FF;
    if (!w) w = 0x400;
    if (!h) h = 0x200;
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            uint16_t v = vram[((sy + j) & 511) * 1024 + ((sx + i) & 1023)];
            uint16_t *p = &vram[((dy + j) & 511) * 1024 + ((dx + i) & 1023)];
            if (g.check_mask && (*p & 0x8000)) continue;
            *p = v | (g.set_mask ? 0x8000 : 0);
        }
    if (hw_active) { hw_copy(sx, sy, dx, dy, w, h); hw_mark_dirty(dy, dy + h); }
    texrep_copy(sx, sy, dx, dy, w, h);
}

/* ------------------------------------------------------------------ GP0 */

static int cmd_len(uint32_t w) {
    int op = w >> 24;
    if (op >= 0x20 && op < 0x40) return poly_words(op);
    if (op >= 0x40 && op < 0x60) return (op & 0x08) ? -1 : ((op & 0x10) ? 4 : 3);
    if (op >= 0x60 && op < 0x80) return rect_words(op);
    if (op == 0x02) return 3;
    if (op >= 0x80 && op < 0xA0) return 4;
    if (op >= 0xA0 && op < 0xC0) return 3;
    if (op >= 0xC0 && op < 0xE0) return 3;
    return 1;
}

/* FE_DBG_PRIM=FROM:TO: log every drawing command of those frames (op, raw words, draw offset, texture page) */
static void prim_log(int op) {
    static long long from = -2, to;
    if (from == -2) {
        const char *e = getenv("FE_DBG_PRIM");
        from = -1;
        if (e && sscanf(e, "%lld:%lld", &from, &to) != 2) from = -1;
    }
    if (from < 0 || (long long)frame_count < from || (long long)frame_count > to || op < 0x20 || op >= 0x80) return;
    char s[256];
    int n = snprintf(s, sizeof s, "prim f%llu op %02X off %d,%d tp %d,%d d%d semi %d:", (unsigned long long)frame_count, op, g.off_x,
                     g.off_y, g.tp_x, g.tp_y, g.tp_depth, g.semi);
    for (int i = 0; i < g.need && n < (int)sizeof s - 12; i++) n += snprintf(s + n, sizeof s - (size_t)n, " %08X", g.buf[i]);
    LOG("%s\n", s);
}

/* FE_DBG_FX (fxlog.c): every textured or semi-transparent drawing command, decoded from its words (read only) */
int fxlog_on(void);
void fxlog_draw(char kind, int tx, int ty, int depth, int abr, int semi, int raw, int cx, int cy, int u0, int v0, int u1, int v1,
                int x0, int y0, int x1, int y1, uint32_t rgb);
static void fx_prim(int op) {
    static int on = -1;
    if (on < 0) on = fxlog_on();
    if (!on) return;
    if (op >= 0x20 && op < 0x40) {
        int shaded = op & 0x10, quad = op & 0x08, textured = op & 0x04, semi = op & 0x02, raw = op & 0x01;
        (void)0; /* untextured opaque polygons too (kind g): blood or sparks may be flat / Gouraud polygons */
        int nv = quad ? 4 : 3, k = 1;
        int x0 = 99999, y0 = 99999, x1 = -99999, y1 = -99999, u0 = 255, v0 = 255, u1 = 0, v1 = 0;
        int tx = g.tp_x, ty = g.tp_y, depth = g.tp_depth, abr = g.semi, cx = 0, cy = 0;
        for (int i = 0; i < nv; i++) {
            if (i > 0 && shaded) k++;
            uint32_t xy = g.buf[k++];
            int x = sx11(xy) + g.off_x, y = sx11(xy >> 16) + g.off_y;
            if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0) y0 = y; if (y > y1) y1 = y;
            if (textured) {
                uint32_t uv = g.buf[k++];
                int u = uv & 0xFF, v = (uv >> 8) & 0xFF;
                if (u < u0) u0 = u; if (u > u1) u1 = u; if (v < v0) v0 = v; if (v > v1) v1 = v;
                if (i == 0) { cx = ((uv >> 16) & 0x3F) * 16; cy = (uv >> 22) & 0x1FF; }
                if (i == 1) {
                    uint32_t tp = uv >> 16;
                    tx = (tp & 0xF) * 64; ty = ((tp >> 4) & 1) * 256;
                    depth = (tp >> 7) & 3; if (depth == 3) depth = 2;
                    abr = (tp >> 5) & 3;
                }
            }
        }
        if (!textured) { u0 = v0 = u1 = v1 = 0; }
        fxlog_draw(textured ? (quad ? 'q' : 't') : semi ? 'f' : 'g', textured ? tx : 0, textured ? ty : 0, textured ? depth : 0, abr, semi ? 1 : 0,
                   raw ? 1 : 0, cx, cy, u0, v0, u1, v1, x0, y0, x1, y1, g.buf[0] & 0xFFFFFF);
    } else if (op >= 0x40 && op < 0x60) { /* lines (kind l; a polyline: one record per vertex, op 7F below) */
        int shaded = op & 0x10, semi = op & 0x02;
        uint32_t xy0 = g.buf[1], xy1 = shaded ? g.buf[3] : g.buf[2];
        int x0 = sx11(xy0) + g.off_x, y0 = sx11(xy0 >> 16) + g.off_y, x1 = sx11(xy1) + g.off_x, y1 = sx11(xy1 >> 16) + g.off_y;
        fxlog_draw('l', 0, 0, 0, g.semi, semi ? 1 : 0, 0, 0, 0, 0, 0, 0, 0, x0 < x1 ? x0 : x1, y0 < y1 ? y0 : y1, x0 > x1 ? x0 : x1,
                   y0 > y1 ? y0 : y1, g.buf[0] & 0xFFFFFF);
    } else if (op == 0x7F) {
        int x = sx11(g.buf[1]) + g.off_x, y = sx11(g.buf[1] >> 16) + g.off_y;
        fxlog_draw('l', 0, 0, 0, g.semi, (g.buf[0] >> 25) & 1, 0, 0, 0, 0, 0, 0, 0, x, y, x, y, g.buf[0] & 0xFFFFFF);
    } else if (op >= 0x60 && op < 0x80) {
        int textured = op & 0x04, semi = op & 0x02, raw = op & 0x01, size = (op >> 3) & 3;
        (void)0;
        int k = 1;
        uint32_t xy = g.buf[k++];
        int x = sx11(xy) + g.off_x, y = sx11(xy >> 16) + g.off_y, u = 0, v = 0, cx = 0, cy = 0, w, h;
        if (textured) {
            uint32_t uv = g.buf[k++];
            u = uv & 0xFF; v = (uv >> 8) & 0xFF;
            cx = ((uv >> 16) & 0x3F) * 16; cy = (uv >> 22) & 0x1FF;
        }
        if (size == 0) { uint32_t wh = g.buf[k++]; w = wh & 0x3FF; h = (wh >> 16) & 0x1FF; }
        else if (size == 1) w = h = 1;
        else if (size == 2) w = h = 8;
        else w = h = 16;
        if (w <= 0 || h <= 0) return;
        fxlog_draw(textured ? 's' : semi ? 'f' : 'g', textured ? g.tp_x : 0, textured ? g.tp_y : 0, textured ? g.tp_depth : 0, g.semi, semi ? 1 : 0,
                   raw ? 1 : 0, cx, cy, u, v, u + w - 1, v + h - 1, x, y, x + w - 1, y + h - 1, g.buf[0] & 0xFFFFFF);
    }
}

void gpu_gp0(uint32_t w) {
    if (g.load_active) {
        vram_put(w & 0xFFFF);
        if (g.load_active) vram_put(w >> 16);
        return;
    }
    if (g.polyline) {
        /* polyline: vertices until the 0x5xxx5xxx terminator */
        int shaded = (g.buf[0] >> 24) & 0x10;
        if ((w & 0xF000F000) == 0x50005000 && g.pl_count >= 2) { g.polyline = 0; g.n = 0; return; }
        if (shaded && g.pl_want_color) { g.pl_cur_c = w & 0xFFFFFF; g.pl_want_color = 0; return; }
        int x = sx11(w) + g.off_x, y = sx11(w >> 16) + g.off_y;
        g.buf[1] = w; g.need = 2;
        prim_log(0x7F); /* logged as op 7F: one polyline vertex (word 0 = the polyline's command) */
        fx_prim(0x7F);
        if (g.pl_count > 0)
            draw_line(g.pl_x, g.pl_y, g.pl_c, x, y, g.pl_cur_c, shaded, (g.buf[0] >> 24) & 2);
        g.pl_x = x; g.pl_y = y; g.pl_c = g.pl_cur_c; g.pl_count++;
        g.pl_want_color = 1;
        return;
    }
    if (g.n == 0) {
        int op = w >> 24;
        if (op >= 0xE1 && op <= 0xE6) {
            switch (op) {
            case 0xE1: set_texpage(w); break;
            case 0xE2:
                g.tw_mask_x = w & 0x1F; g.tw_mask_y = (w >> 5) & 0x1F;
                g.tw_off_x = (w >> 10) & 0x1F; g.tw_off_y = (w >> 15) & 0x1F;
                break;
            case 0xE3: g.da_x1 = w & 0x3FF; g.da_y1 = (w >> 10) & 0x1FF; break;
            case 0xE4: g.da_x2 = w & 0x3FF; g.da_y2 = (w >> 10) & 0x1FF; break;
            case 0xE5: g.off_x = sx11(w); g.off_y = sx11(w >> 11); break;
            case 0xE6:
                g.set_mask = w & 1; g.check_mask = (w >> 1) & 1;
                g.stat = (g.stat & ~0x1800u) | ((w & 3) << 11);
                break;
            }
            return;
        }
        if (op == 0 || op == 1 || (op >= 0x03 && op < 0x20) || op >= 0xE0) return;
        g.need = cmd_len(w);
        if (g.need < 0) {
            g.polyline = 1; g.buf[0] = w; g.n = 0;
            g.pl_count = 0; g.pl_cur_c = w & 0xFFFFFF; g.pl_want_color = 0;
            return;
        }
    }
    g.bufaddr[g.n] = gpu_word_addr;
    g.buf[g.n++] = w;
    if (g.n < g.need) return;
    g.n = 0;
    int op = g.buf[0] >> 24;
    prim_log(op);
    fx_prim(op);
    if (op >= 0x20 && op < 0x80) fb_drawn(g.off_x, g.off_y);
    else if (op == 0x02) fb_drawn(g.buf[1] & 0x3F0, (g.buf[1] >> 16) & 0x1FF);
    if (op >= 0x20 && op < 0x40) cmd_polygon();
    else if (op >= 0x40 && op < 0x60) {
        int shaded = op & 0x10;
        uint32_t c0 = g.buf[0], xy0 = g.buf[1];
        uint32_t c1 = shaded ? g.buf[2] : c0, xy1 = shaded ? g.buf[3] : g.buf[2];
        draw_line(sx11(xy0) + g.off_x, sx11(xy0 >> 16) + g.off_y, c0, sx11(xy1) + g.off_x, sx11(xy1 >> 16) + g.off_y, c1, shaded, op & 2);
    } else if (op >= 0x60 && op < 0x80) cmd_rect();
    else if (op == 0x02) cmd_fill();
    else if (op >= 0x80 && op < 0xA0) cmd_copy();
    else if (op >= 0xA0 && op < 0xC0) {
        g.lx = g.buf[1] & 0x3FF; g.ly = (g.buf[1] >> 16) & 0x1FF;
        g.lw = g.buf[2] & 0x3FF; g.lh = (g.buf[2] >> 16) & 0x1FF;
        if (!g.lw) g.lw = 0x400;
        if (!g.lh) g.lh = 0x200;
        g.lpos = 0; g.load_active = 1;
    } else if (op >= 0xC0 && op < 0xE0) {
        g.sx = g.buf[1] & 0x3FF; g.sy = (g.buf[1] >> 16) & 0x1FF;
        g.sw = g.buf[2] & 0x3FF; g.sh = (g.buf[2] >> 16) & 0x1FF;
        if (!g.sw) g.sw = 0x400;
        if (!g.sh) g.sh = 0x200;
        g.spos = 0; g.store_active = 1;
    }
}

/* ------------------------------------------------------------------ GP1 */

void gpu_gp1(uint32_t w) {
    int op = (w >> 24) & 0x3F;
    switch (op) {
    case 0x00: {
        uint16_t keep[1]; (void)keep;
        gpu_init();
        g.stat = 0x14802000;
        break;
    }
    case 0x01: g.n = 0; g.load_active = 0; g.polyline = 0; break;
    case 0x02: g.stat &= ~(1u << 24); break;
    case 0x03: g.disp_off = w & 1; g.stat = (g.stat & ~(1u << 23)) | ((w & 1) << 23); break;
    case 0x04: g.dma_dir = w & 3; g.stat = (g.stat & ~0x60000000u) | ((w & 3) << 29); break;
    case 0x05: {
        int ny = (w >> 10) & 0x1FF, nx = w & 0x3FE;
        if (ny != g.disp_y || nx != g.disp_x) smooth_flip(); /* a new frame on screen (gpu_hw.c, 60 fps) */
        g.disp_x = nx; g.disp_y = ny;
        break;
    }
    case 0x06: g.h1 = w & 0xFFF; g.h2 = (w >> 12) & 0xFFF; break;
    case 0x07: g.v1 = w & 0x3FF; g.v2 = (w >> 10) & 0x3FF; break;
    case 0x08:
        g.dmode = w & 0xFF;
        g.stat = (g.stat & ~0x7F4000u) | ((w & 0x3F) << 17) | ((w & 0x40) << 10) | ((w & 0x80) << 7);
        break;
    case 0x10:
        switch (w & 7) {
        case 2: g.read_latch = (uint32_t)(g.tw_mask_x | (g.tw_mask_y << 5) | (g.tw_off_x << 10) | (g.tw_off_y << 15)); break;
        case 3: g.read_latch = (uint32_t)(g.da_x1 | (g.da_y1 << 10)); break;
        case 4: g.read_latch = (uint32_t)(g.da_x2 | (g.da_y2 << 10)); break;
        case 5: g.read_latch = (uint32_t)((g.off_x & 0x7FF) | ((g.off_y & 0x7FF) << 11)); break;
        case 7: g.read_latch = 2; break;
        }
        break;
    default: break;
    }
}

uint32_t gpu_stat(void) {
    uint32_t s = g.stat & ~((1u << 25) | (1u << 26) | (1u << 27) | (1u << 28) | (1u << 31));
    s |= (1u << 26) | (1u << 28);
    if (g.store_active) s |= 1u << 27;
    switch (g.dma_dir) {
    case 1: case 2: s |= 1u << 25; break;
    case 3: if (g.store_active) s |= 1u << 25; break;
    }
    if (frame_count & 1) s |= 1u << 31;
    if (g.dmode & 0x20) s &= ~(1u << 31) | ((frame_count & 1) << 31);
    return s;
}

void gpu_vblank(void) {}

void gpu_display(GpuDisplay *dsp) {
    static const int hres_tab[8] = {256, 368, 320, 368, 512, 368, 640, 368};
    int hr = (g.dmode & 0x40) ? 368 : hres_tab[(g.dmode & 3) * 2];
    int interlace = (g.dmode & 0x24) == 0x24;
    int lines = g.v2 - g.v1;
    if (lines <= 0 || lines > 288) lines = 240;
    int vr = interlace ? lines * 2 : lines;
    dsp->x = g.disp_x; dsp->y = g.disp_y;
    dsp->w = hr; dsp->h = vr;
    dsp->hres = hr; dsp->vres = vr;
    dsp->depth24 = (g.dmode >> 4) & 1;
    dsp->enabled = !g.disp_off;
}

/* ------------------------------------------------------------------ save states (state.c) */
#include "state.h"

void gpu_state(StIO *io) {
    st_begin(io, "GPU", 1);
    ST(io, g);
    st_raw(io, vram, sizeof vram);
    st_end(io); /* movie_fb_native is the presenter's (game_fe2.c sets it after a load) */
}
