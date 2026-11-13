/* FE_DBG_FX=FILE: a log of the game's textures and textured / semi-transparent drawing for tools/fx_textures.py (the
 * effect texture scraper). Read-only: nothing here changes what the game or the renderer does (FE_STATE_HASH is the
 * same with and without it).
 *
 *   U frame x y w h hash off        every CPU upload to VRAM (w = VRAM words), hash = XXH3-64 as texrep.c hashes it,
 *                                   off = where its pixels are in FILE.bin
 *   C cx cy n hash e0 e1 ...        a CLUT's n entries (16 or 256) the first time this content is drawn with
 *   P frame kind tx ty depth abr semi raw cx cy clh n x0 y0 x1 y1 rgb nb u0 v0 u1 v1 ...
 *                                   per frame, the draws of one (kind, page, depth, blend mode, semi, raw, CLUT place,
 *                                   CLUT content) summed: n draws, their screen box, the first draw's colour, and up to
 *                                   16 distinct texel boxes (nb; more = 99 and their union). kind: t triangle, q quad,
 *                                   s sprite (rectangle), f untextured semi-transparent polygon / rectangle (cx cy clh 0),
 *                                   g untextured opaque polygon / rectangle, l line (semi 1 = semi-transparent)
 *   F frame                         a frame with draws ended
 * FE_DBG_FX_PRIM=FROM:TO limits the P / C / F records to those frames (uploads are always logged). */
#include <stdlib.h>
#include "hw.h"
#define XXH_INLINE_ALL
#include "third_party/xxhash.h"

extern uint16_t vram[1024 * 512];

static FILE *fx_f;
static int fx_state = -1; /* -1 unread, 0 off, 1 on */
static long long fx_from = 0, fx_to = 0x7FFFFFFFFFFFLL;

void fxlog_close(void);
static int fx_open(void) {
    if (fx_state < 0) {
        const char *p = getenv("FE_DBG_FX");
        fx_state = 0;
        if (p && *p && (fx_f = fopen(p, "w"))) { fx_state = 1; atexit(fxlog_close); }
        const char *r = getenv("FE_DBG_FX_PRIM");
        if (r) sscanf(r, "%lld:%lld", &fx_from, &fx_to);
    }
    return fx_state;
}

/* the pixels of each upload hash once (FILE.bin; the U record names the offset, -1 = not kept: seen before, a movie
 * strip of 16 x 160 words, or more than 64 K words) */
static FILE *fx_bin;
static uint64_t *bin_seen;
static int nbin, capbin;
static long long bin_off;

void fxlog_upload(int x, int y, int w, int h) {
    if (fx_open() <= 0 || x + w > 1024 || y + h > 512) return;
    XXH3_state_t st;
    XXH3_64bits_reset(&st);
    for (int r = 0; r < h; r++) XXH3_64bits_update(&st, &vram[(y + r) * 1024 + x], (size_t)w * 2);
    uint64_t hash = XXH3_64bits_digest(&st);
    long long off = -1;
    if (!(w == 16 && h == 160) && w * h <= 65536) {
        int seen = 0;
        for (int i = 0; i < nbin && !seen; i++) seen = bin_seen[i] == hash;
        if (!seen) {
            if (!fx_bin) {
                char p[1100];
                snprintf(p, sizeof p, "%s.bin", getenv("FE_DBG_FX"));
                fx_bin = fopen(p, "wb");
            }
            if (fx_bin) {
                if (nbin == capbin) { capbin = capbin ? capbin * 2 : 1024; bin_seen = realloc(bin_seen, sizeof(uint64_t) * (size_t)capbin); }
                bin_seen[nbin++] = hash;
                for (int r = 0; r < h; r++) fwrite(&vram[(y + r) * 1024 + x], 2, (size_t)w, fx_bin);
                off = bin_off;
                bin_off += (long long)w * h * 2;
            }
        }
    }
    fprintf(fx_f, "U %llu %d %d %d %d %016llX %lld\n", (unsigned long long)frame_count, x, y, w, h, (unsigned long long)hash, off);
}

typedef struct {
    char kind;
    int tx, ty, depth, abr, semi, raw, cx, cy;
    uint64_t clh;
    int n, x0, y0, x1, y1;
    uint32_t rgb;
    int nb, ub[16][4];
} Agg;
#define MAXAGG 2048
static Agg agg[MAXAGG];
static int nagg;
static uint64_t agg_frame = ~0ull;
static uint64_t clut_seen[4096];
static int nclut;

static void fx_flush(void) {
    for (int i = 0; i < nagg; i++) {
        Agg *a = &agg[i];
        fprintf(fx_f, "P %llu %c %d %d %d %d %d %d %d %d %016llX %d %d %d %d %d %06X %d", (unsigned long long)agg_frame, a->kind, a->tx,
                a->ty, a->depth, a->abr, a->semi, a->raw, a->cx, a->cy, (unsigned long long)a->clh, a->n, a->x0, a->y0, a->x1, a->y1,
                a->rgb, a->nb);
        int nb = a->nb > 16 ? 1 : a->nb;
        for (int k = 0; k < nb; k++) fprintf(fx_f, " %d %d %d %d", a->ub[k][0], a->ub[k][1], a->ub[k][2], a->ub[k][3]);
        fputc('\n', fx_f);
    }
    if (nagg) fprintf(fx_f, "F %llu\n", (unsigned long long)agg_frame);
    nagg = 0;
}

/* one drawing command: kind, page, blend, CLUT, texel box, screen box */
void fxlog_draw(char kind, int tx, int ty, int depth, int abr, int semi, int raw, int cx, int cy, int u0, int v0, int u1, int v1,
                int x0, int y0, int x1, int y1, uint32_t rgb) {
    if (fx_open() <= 0 || (long long)frame_count < fx_from || (long long)frame_count > fx_to) return;
    if (frame_count != agg_frame) { fx_flush(); agg_frame = frame_count; }
    /* untextured: the colour in 8 levels per channel stands in for the CLUT (red blood apart from grey dust) */
    uint64_t clh = (kind == 'f' || kind == 'g' || kind == 'l') ? (uint64_t)(((rgb >> 5) & 7) | (((rgb >> 13) & 7) << 4) | (((rgb >> 21) & 7) << 8)) : 0;
    if (kind != 'f' && kind != 'g' && kind != 'l') {
        int n = depth == 0 ? 16 : depth == 1 ? 256 : 0;
        if (n && cx + n <= 1024) {
            clh = XXH3_64bits(&vram[cy * 1024 + cx], (size_t)n * 2);
            uint64_t key = clh ^ ((uint64_t)cx << 40) ^ ((uint64_t)cy << 52);
            int seen = 0;
            for (int i = 0; i < nclut && !seen; i++) seen = clut_seen[i] == key;
            if (!seen) {
                if (nclut < 4096) clut_seen[nclut++] = key;
                fprintf(fx_f, "C %d %d %d %016llX", cx, cy, n, (unsigned long long)clh);
                for (int i = 0; i < n; i++) fprintf(fx_f, " %04X", vram[cy * 1024 + cx + i]);
                fputc('\n', fx_f);
            }
        }
    }
    Agg *a = 0;
    for (int i = 0; i < nagg && !a; i++) {
        Agg *b = &agg[i];
        if (b->kind == kind && b->tx == tx && b->ty == ty && b->depth == depth && b->abr == abr && b->semi == semi && b->raw == raw &&
            b->cx == cx && b->cy == cy && b->clh == clh) a = b;
    }
    if (!a) {
        if (nagg >= MAXAGG) return;
        a = &agg[nagg++];
        memset(a, 0, sizeof *a);
        a->kind = kind; a->tx = tx; a->ty = ty; a->depth = depth; a->abr = abr; a->semi = semi; a->raw = raw;
        a->cx = cx; a->cy = cy; a->clh = clh; a->rgb = rgb;
        a->x0 = x0; a->y0 = y0; a->x1 = x1; a->y1 = y1;
    }
    a->n++;
    if (x0 < a->x0) a->x0 = x0;
    if (y0 < a->y0) a->y0 = y0;
    if (x1 > a->x1) a->x1 = x1;
    if (y1 > a->y1) a->y1 = y1;
    if (kind == 'f' || kind == 'g' || kind == 'l') return;
    int found = 0;
    for (int k = 0; k < a->nb && k < 16 && !found; k++)
        found = a->ub[k][0] == u0 && a->ub[k][1] == v0 && a->ub[k][2] == u1 && a->ub[k][3] == v1;
    if (found) return;
    if (a->nb < 16) {
        a->ub[a->nb][0] = u0; a->ub[a->nb][1] = v0; a->ub[a->nb][2] = u1; a->ub[a->nb][3] = v1;
        a->nb++;
    } else { /* more than 16: the union in slot 0 */
        if (a->nb == 16) a->nb = 99;
        if (u0 < a->ub[0][0]) a->ub[0][0] = u0;
        if (v0 < a->ub[0][1]) a->ub[0][1] = v0;
        if (u1 > a->ub[0][2]) a->ub[0][2] = u1;
        if (v1 > a->ub[0][3]) a->ub[0][3] = v1;
    }
}

int fxlog_on(void) { return fx_open() > 0; }

void fxlog_close(void) {
    if (fx_state > 0) { fx_flush(); fflush(fx_f); if (fx_bin) fflush(fx_bin); }
}
