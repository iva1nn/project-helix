/* The games' bitmap fonts rebuilt from TrueType at the internal resolution, natively and the same for both games
 * (NOTES.md "Native fonts"). A port of the remaster's font_rebuild.py (Atlas.place / shape / rebuild): font atlases
 * are known by their upload hash (font_table.h, tools/font_table.py); each glyph is rendered on the native
 * baseline, centred on the native glyph, the game's symbols (arrows, pad buttons, play) drawn as shapes, only over
 * the glyph's own texels and blank ones, then quantised to the atlas's brightness ramp. The result is an index map
 * (palette index in red) that the renderer colours with the live CLUT, like the packs' fe2idx images. */
#include <stdlib.h>
#include <math.h>
#include <SDL2/SDL.h>
#include "hw.h"
#include "third_party/stb_truetype.h"
#include "font_table.h"

#define NATL (int)(sizeof font_atlases / sizeof *font_atlases)
#define NFACE (int)(sizeof font_faces / sizeof *font_faces)
#define OVERHANG 3

static stbtt_fontinfo faces[NFACE];
static unsigned char *face_data[NFACE];
static int face_state[NFACE]; /* 0 not tried, 1 loaded, -1 missing */
static unsigned atlas_tex[NATL];
static int atlas_scale[NATL];

static int game_is_fe1(void) { return !strcmp(game_serial, "SLUS-00920") || !strncmp(game_serial, "SLUS-0105", 9); }

static const FontAtlas *dialogue; /* the dialogue / subtitle font (face 1) last uploaded */

const void *fonts_find(uint64_t hash) {
    int fe1 = game_is_fe1();
    for (int i = 0; i < NATL; i++)
        if (font_atlases[i].hash == hash && font_atlases[i].fe1 == fe1) {
            if (font_atlases[i].face == 1) dialogue = &font_atlases[i];
            return &font_atlases[i];
        }
    return 0;
}

/* the character the dialogue font shows for code c (its slots: FE2 keeps ':' in '|', '/' in '^', '%' in '>') */
int fonts_dialogue_char(int c) {
    if (!dialogue || c < 32 || c >= 128) return c;
    int s = dialogue->slot[c - 32];
    return s > 0 ? s : c;
}

static stbtt_fontinfo *face(int k) {
    if (face_state[k]) return face_state[k] > 0 ? &faces[k] : 0;
    face_state[k] = -1;
    char *base = SDL_GetBasePath();
    char d[3][1100], p[1300];
    snprintf(d[0], sizeof d[0], "%s/fonts", fe_data);            /* the data folder */
    snprintf(d[1], sizeof d[1], "%sfonts", base ? base : "");     /* older packages */
    snprintf(d[2], sizeof d[2], "%s../fonts", base ? base : "");  /* the repository's builds (build/, build_fe1/) */
    if (base) SDL_free(base);
    for (int i = fe_data[0] ? 0 : 1; i < 3; i++) {
        snprintf(p, sizeof p, "%s/%s", d[i], font_faces[k]);
        FILE *f = fopen(p, "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        face_data[k] = malloc((size_t)n);
        if (face_data[k] && fread(face_data[k], 1, (size_t)n, f) == (size_t)n &&
            stbtt_InitFont(&faces[k], face_data[k], stbtt_GetFontOffsetForIndex(face_data[k], 0))) {
            fclose(f);
            face_state[k] = 1;
            LOG("fonts: %s\n", p);
            return &faces[k];
        }
        fclose(f);
        free(face_data[k]);
        face_data[k] = 0;
    }
    LOG("fonts: %s not found (fonts\\ next to the exe), the game's own font stays\n", font_faces[k]);
    return 0;
}

/* ------------------------------------------------------------------ the atlas */

typedef struct {
    const FontAtlas *a;
    int W, H, S;                /* native size in texels, scale */
    uint8_t *idx;               /* native indices */
    float cov_of[16];
    int cut;
    float *cov;                 /* native coverage */
} Atlas;

typedef struct { int r, c, code; int left, top, right, bottom; int bx0, by0, bx1, by1; } Cell; /* ink bbox in the cell */

static int bbox(const float *a, int stride, int w, int h, float thr, int *x0, int *y0, int *x1, int *y1) {
    int mx = w, my = h, Mx = -1, My = -1;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            if (a[y * stride + x] > thr) {
                if (x < mx) mx = x;
                if (x > Mx) Mx = x;
                if (y < my) my = y;
                if (y > My) My = y;
            }
    if (Mx < 0) return 0;
    *x0 = mx; *y0 = my; *x1 = Mx + 1; *y1 = My + 1;
    return 1;
}

/* coverage per palette entry (brightness against the brightest; 0x0000 = 0), background entry = the most used
 * of the darkest used entries */
static void ramp(Atlas *at) {
    float lum[16], mx = 1;
    for (int i = 0; i < 16; i++) {
        uint16_t c = at->a->pal[i];
        lum[i] = (float)((c & 31) + ((c >> 5) & 31) + ((c >> 10) & 31));
        if (lum[i] > mx) mx = lum[i];
    }
    for (int i = 0; i < 16; i++) at->cov_of[i] = at->a->pal[i] ? lum[i] / mx : 0.0f;
    int count[16] = {0};
    for (int i = 0; i < at->W * at->H; i++) count[at->idx[i]]++;
    float lo = 2;
    for (int i = 0; i < 16; i++) if (count[i] && at->cov_of[i] < lo) lo = at->cov_of[i];
    at->cut = 0;
    int best = -1;
    for (int i = 0; i < 16; i++) if (count[i] && at->cov_of[i] == lo && count[i] > best) { best = count[i]; at->cut = i; }
}

/* the native glyph in its overhang box: the cell plus ink connected to it in the blank margin of the cells above and
 * below (font_rebuild Atlas.native_glyph); out is (B-T) x (R-L) */
static void native_glyph(const Atlas *at, const Cell *cells, int ncell, int g, float *out, int *T, int *B) {
    const Cell *ce = &cells[g];
    int L = ce->left, R = ce->right;
    *T = ce->top - OVERHANG < 0 ? 0 : ce->top - OVERHANG;
    *B = ce->bottom + OVERHANG > at->H ? at->H : ce->bottom + OVERHANG;
    int w = R - L, h = *B - *T;
    uint8_t keep[64 * 32];
    memset(keep, 0, sizeof keep);
    for (int y = ce->top - *T; y < ce->bottom - *T; y++) for (int x = 0; x < w; x++) keep[y * w + x] = 1;
    int up_limit = 0, down_limit = at->H;
    for (int k = 0; k < ncell; k++) {
        if (cells[k].c != ce->c) continue;
        if (cells[k].r == ce->r - 1) up_limit = cells[k].top + cells[k].by1;
        if (cells[k].r == ce->r + 1) down_limit = cells[k].top + cells[k].by0;
    }
#define INK(y, x) (at->cov[(*T + (y)) * at->W + L + (x)] > 0)
#define KEEP(y, x) keep[(y) * w + (((x) % w + w) % w)]
    for (int y = ce->top - *T - 1; y >= 0; y--) {
        if (*T + y < up_limit) break;
        for (int x = 0; x < w; x++) keep[y * w + x] = INK(y, x) && (KEEP(y + 1, x) || KEEP(y + 1, x - 1) || KEEP(y + 1, x + 1));
    }
    for (int y = ce->bottom - *T; y < h; y++) {
        if (*T + y >= down_limit) break;
        for (int x = 0; x < w; x++) keep[y * w + x] = INK(y, x) && (KEEP(y - 1, x) || KEEP(y - 1, x - 1) || KEEP(y - 1, x + 1));
    }
#undef INK
#undef KEEP
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) out[y * w + x] = keep[y * w + x] ? at->cov[(*T + y) * at->W + L + x] : 0;
}

/* ------------------------------------------------------------------ shapes (font_rebuild.draw_shape) */

static float seg_dist(float px, float py, float ax, float ay, float bx, float by) {
    float dx = bx - ax, dy = by - ay, l = dx * dx + dy * dy;
    float t = l > 0 ? ((px - ax) * dx + (py - ay) * dy) / l : 0;
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    float qx = ax + t * dx - px, qy = ay + t * dy - py;
    return sqrtf(qx * qx + qy * qy);
}

/* coverage (h x w) of a symbol filling its box at stroke width stem (4x4 samples per pixel) */
static void draw_shape(int kind, int w, int h, float stem, float *out) {
    float sw = stem;
    if (kind == -4 || kind == -5) { sw *= 0.8f; if (sw > 0.12f * (w < h ? w : h)) sw = 0.12f * (w < h ? w : h); }
    if (sw < 1) sw = 1;
    float m = sw / 2, cx = w / 2.0f, cy = h / 2.0f;
    float seg[8][4];
    int ns = 0;
    if (kind == -2 || kind == -3) { /* arrows: shaft and a 45 degree head */
        float tip = kind == -2 ? m : h - m, tail = kind == -2 ? h - m : m, sgn = kind == -2 ? 1.0f : -1.0f;
        float hw = cx - m < h * 0.45f ? cx - m : h * 0.45f;
        float s0[4] = {cx, tip, cx, tail}, s1[4] = {cx - hw, tip + sgn * hw, cx, tip}, s2[4] = {cx, tip, cx + hw, tip + sgn * hw};
        memcpy(seg[ns++], s0, sizeof s0); memcpy(seg[ns++], s1, sizeof s1); memcpy(seg[ns++], s2, sizeof s2);
    } else if (kind == -4 || kind == -5) { /* pad buttons: a ring with a cross or triangle */
        float r = (w < h ? w : h) / 2.0f - sw;
        if (kind == -4) {
            float q = r * 0.5f;
            float s0[4] = {cx - q, cy - q, cx + q, cy + q}, s1[4] = {cx - q, cy + q, cx + q, cy - q};
            memcpy(seg[ns++], s0, sizeof s0); memcpy(seg[ns++], s1, sizeof s1);
        } else {
            float t = r * 0.62f, px[3], py[3];
            for (int i = 0; i < 3; i++) {
                float an = (float)((-90 + 120 * i) * M_PI / 180);
                px[i] = cx + t * cosf(an); py[i] = cy + 0.08f * r + t * sinf(an);
            }
            for (int i = 0; i < 3; i++) { float s[4] = {px[i], py[i], px[(i + 1) % 3], py[(i + 1) % 3]}; memcpy(seg[ns++], s, sizeof s); }
        }
    }
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int hit = 0;
            for (int sy = 0; sy < 4; sy++)
                for (int sx = 0; sx < 4; sx++) {
                    float px = x + (sx + 0.5f) / 4, py = y + (sy + 0.5f) / 4;
                    int in = 0;
                    for (int k = 0; k < ns && !in; k++) in = seg_dist(px, py, seg[k][0], seg[k][1], seg[k][2], seg[k][3]) <= m;
                    if (!in && (kind == -4 || kind == -5)) { /* the ring: inside the outer ellipse, outside the inner */
                        float a = w / 2.0f - m, b = h / 2.0f - m, ia = a - sw, ib = b - sw;
                        float ex = (px - cx) / a, ey = (py - cy) / b;
                        float fx = ia > 0 ? (px - cx) / ia : 9, fy = ib > 0 ? (py - cy) / ib : 9;
                        in = ex * ex + ey * ey <= 1 && fx * fx + fy * fy >= 1;
                    }
                    if (!in && kind == -6) { /* play: a filled triangle */
                        float ax = m, ay = m, bx = w - m, by = cy, qx = m, qy = h - m;
                        float d1 = (px - bx) * (ay - by) - (ax - bx) * (py - by);
                        float d2 = (px - qx) * (by - qy) - (bx - qx) * (py - qy);
                        float d3 = (px - ax) * (qy - ay) - (qx - ax) * (py - ay);
                        in = !((d1 < 0 || d2 < 0 || d3 < 0) && (d1 > 0 || d2 > 0 || d3 > 0));
                    }
                    hit += in;
                }
            out[y * w + x] = hit / 16.0f;
        }
}

/* ------------------------------------------------------------------ rebuild */

static unsigned char *rebuild(const FontAtlas *fa, const uint8_t *idx, int S, int *ow, int *oh) {
    Atlas at = {fa, fa->ww * 4, fa->h, S, (uint8_t *)idx, {0}, 0, 0};
    int W = at.W, H = at.H, SW = W * S, SH = H * S;
    stbtt_fontinfo *font = face(fa->face);
    if (!font) return 0;
    ramp(&at);
    at.cov = malloc(sizeof(float) * (size_t)(W * H));
    for (int i = 0; i < W * H; i++) at.cov[i] = at.cov_of[idx[i]];
    /* the cells with ink (font_rebuild Atlas.__init__; grid and baselines from the table) */
    Cell *cells = malloc(sizeof(Cell) * 16 * (size_t)fa->nrows);
    int ncell = 0;
    for (int r = 0; r < fa->nrows; r++)
        for (int c = 0; c < 16; c++) {
            Cell ce = {r, c, 32 + (r - fa->first) * 16 + c, fa->ox + c * fa->pw, fa->oy + r * fa->ph, 0, 0, 0, 0, 0, 0};
            ce.right = ce.left + fa->pw > W ? W : ce.left + fa->pw;
            ce.bottom = ce.top + fa->ph > H ? H : ce.top + fa->ph;
            if (ce.left >= W || ce.top >= H) continue;
            if (!bbox(&at.cov[ce.top * W + ce.left], W, ce.right - ce.left, ce.bottom - ce.top, 0.25f, &ce.bx0, &ce.by0, &ce.bx1, &ce.by1))
                continue;
            if (ce.code > 255) continue;
            cells[ncell++] = ce;
        }
    /* each native ink texel belongs to one glyph: its cell first, then overhang ink connected to it */
    int *owner = malloc(sizeof(int) * (size_t)(W * H));
    for (int i = 0; i < W * H; i++) owner[i] = -1;
    for (int g = 0; g < ncell; g++)
        for (int y = cells[g].top; y < cells[g].bottom; y++)
            for (int x = cells[g].left; x < cells[g].right; x++)
                if (at.cov[y * W + x] > 0) owner[y * W + x] = g;
    float *ng = malloc(sizeof(float) * 64 * 32);
    for (int g = 0; g < ncell; g++) {
        int T, B;
        native_glyph(&at, cells, ncell, g, ng, &T, &B);
        int w = cells[g].right - cells[g].left;
        for (int y = T; y < B; y++)
            for (int x = cells[g].left; x < cells[g].right; x++)
                if (ng[(y - T) * w + x - cells[g].left] > 0 && owner[y * W + x] == -1) owner[y * W + x] = g;
    }
    /* the canvas: the original's coverage (nearest), glyph by glyph replaced by the rendered one */
    float *canvas = malloc(sizeof(float) * (size_t)(SW * SH));
    uint8_t *rep = calloc((size_t)(SW * SH), 1);
    for (int y = 0; y < SH; y++) for (int x = 0; x < SW; x++) canvas[y * SW + x] = at.cov_of[idx[(y / S) * W + x / S]];
    float peak = 0;
    for (int i = 0; i < 16; i++) if (at.cov_of[i] > peak) peak = at.cov_of[i];
    float px_size = fa->size8 * (float)S / 8.0f;
    float scale = stbtt_ScaleForMappingEmToPixels(font, px_size);
    /* stem width at this size (the middle row of "I"), for the drawn symbols */
    float stem = (float)S;
    {
        int gw, gh, xo, yo;
        unsigned char *bm = stbtt_GetCodepointBitmap(font, scale * fa->stretch, scale, 'I', &gw, &gh, &xo, &yo);
        if (bm) { int n = 0; for (int x = 0; x < gw; x++) n += bm[(gh / 2) * gw + x] > 127; if (n) stem = (float)n; stbtt_FreeBitmap(bm, 0); }
    }
    /* the game's glyph quads cover all but the cell's last row and column (FE2 FONTSM: uv 0..8 of a 10 px cell), so
     * a rendered glyph reaching lower (the tail of Q, which read as O) is squashed to fit */
    int rendered = 0, kept = 0;
    float *hi = malloc(sizeof(float) * (size_t)(64 * 32 * S * S));
    for (int g = 0; g < ncell; g++) {
        const Cell *ce = &cells[g];
        int code = ce->code, what = code < 128 ? fa->slot[code - 32] : -1;
        if (code >= 128) { kept++; continue; }
        int cp = what == 0 ? code : what;
        if (what == -1 || (cp > 0 && !stbtt_FindGlyphIndex(font, cp)) || !(fa->base[ce->r] >= 0)) { kept++; continue; }
        int T, B;
        native_glyph(&at, cells, ncell, g, ng, &T, &B);
        int L = ce->left, R = ce->right, bw = (R - L) * S, bh = (B - T) * S;
        memset(hi, 0, sizeof(float) * (size_t)(bw * bh));
        int ok = 0;
        if (cp < 0) { /* a symbol over the native glyph's ink box */
            int x0, y0, x1, y1;
            if (bbox(ng, R - L, R - L, B - T, 0.25f, &x0, &y0, &x1, &y1)) {
                int sw2 = (x1 - x0) * S, sh2 = (y1 - y0) * S;
                float *sh = malloc(sizeof(float) * (size_t)(sw2 * sh2));
                draw_shape(cp, sw2, sh2, stem, sh);
                for (int y = 0; y < sh2; y++) for (int x = 0; x < sw2; x++) hi[(y0 * S + y) * bw + x0 * S + x] = sh[y * sw2 + x];
                free(sh);
                ok = 1;
            }
        } else { /* the face's glyph on the native baseline, centred on the native glyph */
            int gw, gh, xo, yo;
            unsigned char *bm = stbtt_GetCodepointBitmap(font, scale * fa->stretch, scale, cp, &gw, &gh, &xo, &yo);
            if (bm && gw > 0 && gh > 0) {
                int gx = (int)lroundf((ce->bx0 + ce->bx1) / 2.0f * S - gw / 2.0f);
                int line = (fa->base[ce->r] - T) * S;
                int gy = line + yo; /* yo: the bitmap's top relative to the baseline */
                int limit = (ce->top + fa->ph - 1 - T) * S, dh = gh; /* the last row of the cell is not drawn */
                if (gy + gh > limit && limit - gy > gh / 2) dh = limit - gy; /* squash on its top */
                for (int y = 0; y < dh; y++) {
                    float sy = (y + 0.5f) * gh / dh - 0.5f; /* linear resample of the glyph's rows */
                    int y0 = (int)floorf(sy), y1 = y0 + 1;
                    float fy = sy - y0;
                    if (y0 < 0) { y0 = 0; fy = 0; }
                    if (y1 > gh - 1) y1 = gh - 1;
                    for (int x = 0; x < gw; x++) {
                        int X = gx + x, Y = gy + y;
                        float v = (bm[y0 * gw + x] * (1 - fy) + bm[y1 * gw + x] * fy) / 255.0f;
                        if (X >= 0 && X < bw && Y >= 0 && Y < bh) hi[Y * bw + X] = v;
                    }
                }
                ok = 1;
            }
            if (bm) stbtt_FreeBitmap(bm, 0);
        }
        if (!ok) { kept++; continue; }
        /* repaint only this glyph's own texels and blank ones */
        for (int y = 0; y < bh; y++)
            for (int x = 0; x < bw; x++) {
                int nx = L + x / S, ny = T + y / S, o = owner[ny * W + nx];
                if (o != g && o != -1) continue;
                int X = L * S + x, Y = T * S + y;
                if (!rep[Y * SW + X]) canvas[Y * SW + X] = 0;
                float v = hi[y * bw + x] * peak;
                if (v > canvas[Y * SW + X]) canvas[Y * SW + X] = v;
                rep[Y * SW + X] = 1;
            }
        rendered++;
    }
    /* back to ramp entries: per brightness level the entry the native glyphs use most */
    int count[16] = {0};
    for (int i = 0; i < W * H; i++) count[idx[i]]++;
    int used[16], nused = 0;
    for (int i = 0; i < 16; i++) {
        if (!count[i] || i == at.cut) continue;
        int dup = -1;
        for (int k = 0; k < nused; k++) if (at.cov_of[used[k]] == at.cov_of[i]) dup = k;
        if (dup < 0) used[nused++] = i;
        else if (count[i] > count[used[dup]]) used[dup] = i;
    }
    float lmin = 2;
    for (int k = 0; k < nused; k++) if (at.cov_of[used[k]] < lmin) lmin = at.cov_of[used[k]];
    unsigned char *out = malloc((size_t)(SW * SH) * 4);
    for (int i = 0; i < SW * SH; i++) {
        int e;
        if (!rep[i]) e = idx[((i / SW) / S) * W + (i % SW) / S];
        else if (!nused || canvas[i] < lmin * 0.5f) e = at.cut;
        else {
            e = used[0];
            float bd = 9;
            for (int k = 0; k < nused; k++) { float d = fabsf(canvas[i] - at.cov_of[used[k]]); if (d < bd) { bd = d; e = used[k]; } }
        }
        out[i * 4] = (unsigned char)e; out[i * 4 + 1] = 0; out[i * 4 + 2] = 0; out[i * 4 + 3] = 255;
    }
    LOG("fonts: atlas %016llX (%s) rebuilt at %dx: %d glyphs rendered, %d kept\n", (unsigned long long)fa->hash, font_faces[fa->face], S,
        rendered, kept);
    free(hi); free(ng); free(canvas); free(rep); free(owner); free(cells); free(at.cov);
    *ow = SW; *oh = SH;
    return out;
}

/* the index-map texture for an atlas just uploaded at x, y (VRAM words); 0 = none */
unsigned fonts_texture(const void *fav, int x, int y) {
    const FontAtlas *fa = fav;
    int i = (int)(fa - font_atlases);
    int S = hw_scale < 2 ? 2 : hw_scale > 8 ? 8 : hw_scale;
    if (atlas_tex[i] && atlas_scale[i] == S) return atlas_tex[i];
    int W = fa->ww * 4, H = fa->h;
    uint8_t *idx = malloc((size_t)(W * H));
    for (int r = 0; r < H; r++)
        for (int c = 0; c < fa->ww; c++) {
            uint16_t v = vram[((y + r) & 511) * 1024 + ((x + c) & 1023)];
            for (int k = 0; k < 4; k++) idx[r * W + c * 4 + k] = (v >> (4 * k)) & 15;
        }
    int ow, oh;
    unsigned char *px = rebuild(fa, idx, S, &ow, &oh);
    free(idx);
    if (!px) return 0;
    if (getenv("FE_DUMP_FONTS")) { /* the index map as a grey PGM, for checks */
        char p[64];
        snprintf(p, sizeof p, "out/font_%016llX.pgm", (unsigned long long)fa->hash);
        FILE *f = fopen(p, "wb");
        if (f) {
            fprintf(f, "P5 %d %d 255\n", ow, oh);
            for (int k = 0; k < ow * oh; k++) {
                uint16_t c = fa->pal[px[k * 4]];
                fputc(((c & 31) + ((c >> 5) & 31) + ((c >> 10) & 31)) * 255 / 93, f);
            }
            fclose(f);
        }
    }
    atlas_tex[i] = hw_tex_rgba(ow, oh, px, 0);
    atlas_scale[i] = S;
    free(px);
    return atlas_tex[i];
}
