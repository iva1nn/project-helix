/* The remasters' texture packs (redrawn interface, fonts, icons, model textures), as DuckStation loads them:
 * <textures>/<disc serial>/replacements/ (default <data>/<game>/textures, copied by tools/make_data.py from
 * DuckStation's folder, where the remasters' pack builders installed them; F:\fe_port\third_party\fe2_remaster\TEXTURES.md "Replacement names", F:\fe_remaster\docs\TEXTURES.md).
 *
 *  texupload-{P4|P8|STP4|STP8}-{src}-{pal}-{ww}x{h}-0-0-{w}x{h}-P0-{top}.png
 *      src = XXH3-64 of the uploaded block (ww VRAM words x h rows, as the game's LoadImage wrote it),
 *      pal = XXH3-64 of CLUT entries 0..top; a full-colour image (alpha 0 cut-out, 128 semi-transparent texel).
 *  fe2idx/fe2idx-{src}-{0 = 4-bit | 1 = 8-bit}-{w}x{h}.png
 *      the remaster fork's fallback for palettes built at run time (fonts, menu art): a map of palette indices
 *      (red channel), coloured with the live CLUT when no texupload palette matches.
 *
 * Every upload is hashed; blocks that have replacements are remembered with their VRAM rectangle. A textured
 * draw whose texture coordinates lie inside one remembered block samples its replacement instead of VRAM
 * (gpu_hw.c). Images are decoded on a worker thread the first time they are needed; until then the original
 * texture shows (a frame or two). */
#include <stdlib.h>
#include <io.h>
#include <SDL2/SDL.h>
#include "hw.h"
#define XXH_INLINE_ALL
#include "third_party/xxhash.h"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ASSERT(x) ((void)0) /* no assert(): it puts the build path into the exe */
#define STBI_NO_STDIO_WARNINGS
#include "third_party/stb_image.h"

int texrep_on = 1;      /* drawn with the pack (F6, the remaster switch) */
int texrep_enabled = 1; /* the pack is used at all (--no-textures, textures = off) */
static int dbg_tex = -1; /* FE_DBG_TEX: log uploads in the pack and the draws that use them */
char texrep_root[1024]; /* folder holding <serial>/replacements; "" = default */

typedef struct {
    uint64_t src, pal;
    uint8_t kind;   /* 0 texupload, 1 fe2idx */
    uint8_t mode;   /* 0 4-bit, 1 8-bit */
    uint16_t top;   /* highest palette entry hashed */
    uint16_t ww, h; /* block size: VRAM words x rows */
    char *path;
    volatile int state; /* 0 not loaded, 1 queued, 2 decoded, 3 texture, 4 failed */
    unsigned char *pixels;
    int pw, ph;
    unsigned tex;
} Rep;

static Rep *reps;
static int nreps;
static char loaded_dir[1100];

typedef struct { int x, y, w, h; uint64_t hash; int first; unsigned font; } Block; /* first: index of its first Rep;
                                                                                font: a native font rebuild (fonts.c) */
const void *fonts_find(uint64_t hash);
unsigned fonts_texture(const void *atlas, int x, int y);
#define MAXBLOCKS 1024
static Block blocks[MAXBLOCKS];
static int nblocks;

static int cmp_rep(const void *a, const void *b) {
    uint64_t x = ((const Rep *)a)->src, y = ((const Rep *)b)->src;
    return x < y ? -1 : x > y;
}

static int first_rep(uint64_t src) {
    int lo = 0, hi = nreps;
    while (lo < hi) { int mid = (lo + hi) / 2; if (reps[mid].src < src) lo = mid + 1; else hi = mid; }
    return lo < nreps && reps[lo].src == src ? lo : -1;
}

static void add_dir(const char *dir, int idx) {
    char pat[1200];
    snprintf(pat, sizeof pat, "%s/*.png", dir);
    struct _finddata_t fd;
    intptr_t h = _findfirst(pat, &fd);
    ftrace(h == -1 ? "dir-missing" : "dir", pat);
    if (h == -1) return;
    do {
        Rep r;
        memset(&r, 0, sizeof r);
        char mode[8];
        unsigned long long src, pal = 0;
        unsigned ww, hh, ox, oy, w, h2, pmin, pmax, m;
        if (!idx && sscanf(fd.name, "texupload-%7[A-Z0-9]-%16llX-%16llX-%ux%u-%u-%u-%ux%u-P%u-%u", mode, &src, &pal, &ww, &hh,
                           &ox, &oy, &w, &h2, &pmin, &pmax) == 11) {
            if (ox || oy || pmin) continue;
            r.kind = 0;
            r.mode = strstr(mode, "8") ? 1 : 0;
            if (!strstr(mode, "4") && !strstr(mode, "8")) continue; /* 16-bit blocks: not in these packs */
            r.top = (uint16_t)pmax;
            r.ww = (uint16_t)ww; r.h = (uint16_t)hh;
        } else if (idx && sscanf(fd.name, "fe2idx-%16llX-%u-%ux%u", &src, &m, &w, &h2) == 4) {
            r.kind = 1;
            r.mode = (uint8_t)m;
            r.ww = (uint16_t)(m ? w / 2 : w / 4); r.h = (uint16_t)h2;
        } else continue;
        r.src = src; r.pal = pal;
        size_t n = strlen(dir) + strlen(fd.name) + 2;
        r.path = malloc(n);
        snprintf(r.path, n, "%s/%s", dir, fd.name);
        if (!(nreps & 255)) reps = realloc(reps, sizeof(Rep) * (size_t)(nreps + 256));
        reps[nreps++] = r;
    } while (_findnext(h, &fd) == 0);
    _findclose(h);
}

/* the pack of the inserted disc (each disc has its own serial and pack) */
static void load_index(void) {
    static const char *fe1[4] = {"SLUS-00920", "SLUS-01056", "SLUS-01057", "SLUS-01058"};
    static const char *fe2[4] = {"SLUS-01266", "SLUS-01275", "SLUS-01276", "SLUS-01277"};
    int is1 = !strcmp(game_serial, "SLUS-00920") || !strncmp(game_serial, "SLUS-0105", 9);
    int is2 = !strncmp(game_serial, "SLUS-0126", 9) || !strncmp(game_serial, "SLUS-0127", 9);
    char root[1024], dir[1100];
    if (texrep_root[0]) snprintf(root, sizeof root, "%s", texrep_root);
    else snprintf(root, sizeof root, "%s/textures", fe_data_game[0] ? fe_data_game : ".");
    snprintf(dir, sizeof dir, "%s/%s/replacements", root,
             is1 ? fe1[disc_current & 3] : is2 ? fe2[disc_current & 3] : game_serial);
    if (!strcmp(dir, loaded_dir)) return;
    snprintf(loaded_dir, sizeof loaded_dir, "%s", dir);
    /* the old disc's images stay as GL textures only while in use; simplest: keep them (a disc has ~100-460) */
    reps = 0; nreps = 0; nblocks = 0;
    add_dir(dir, 0);
    char idx[1200];
    snprintf(idx, sizeof idx, "%s/fe2idx", dir);
    add_dir(idx, 1);
    if (nreps) qsort(reps, (size_t)nreps, sizeof(Rep), cmp_rep);
    LOG("textures: %d replacements in %s\n", nreps, dir);
}

/* ------------------------------------------------------------------ decoding thread */

static SDL_mutex *mu;
static SDL_cond *cv;
static Rep *queue[256];
static int qhead, qtail;

static int decoder(void *arg) {
    (void)arg;
    for (;;) {
        SDL_LockMutex(mu);
        while (qhead == qtail) SDL_CondWait(cv, mu);
        Rep *r = queue[qhead++ & 255];
        SDL_UnlockMutex(mu);
        int w, h, ch;
        unsigned char *px = stbi_load(r->path, &w, &h, &ch, 4);
        r->pixels = px; r->pw = w; r->ph = h;
        SDL_MemoryBarrierRelease();
        r->state = px ? 2 : 4;
    }
    return 0;
}

static void request(Rep *r) {
    if (r->state) return;
    if (!mu) { mu = SDL_CreateMutex(); cv = SDL_CreateCond(); SDL_DetachThread(SDL_CreateThread(decoder, "textures", 0)); }
    SDL_LockMutex(mu);
    if (qtail - qhead < 256) { queue[qtail++ & 255] = r; r->state = 1; }
    SDL_CondSignal(cv);
    SDL_UnlockMutex(mu);
}

/* wait_ms: how long the draw may wait for a decode in progress (full-screen pictures drawn once, like the
 * loading screen, stay on screen as drawn; small images just show the original for a frame or two) */
static int ready(Rep *r, int wait_ms) {
    if (!r->state) request(r);
    for (uint32_t t0 = SDL_GetTicks(); r->state == 1 && (int)(SDL_GetTicks() - t0) < wait_ms;) SDL_Delay(1);
    if (r->state == 2) {
        SDL_MemoryBarrierAcquire();
        /* full-colour images of 16 pixels per texel or more (the port's own redraws, e.g. the FE2 code panel digits,
         * tools/code_panel_digits.py) are minified at the lower internal scales: mipmapped */
        int texels = r->ww * (r->mode ? 2 : 4);
        r->tex = hw_tex_rgba(r->pw, r->ph, r->pixels, r->kind == 0 ? (r->pw >= 16 * texels ? 2 : 1) : 0);
        stbi_image_free(r->pixels);
        r->pixels = 0;
        r->state = r->tex ? 3 : 4;
    }
    return r->state == 3;
}

/* ------------------------------------------------------------------ uploads and draws */

static void drop_overlapping(int x, int y, int w, int h) {
    for (int i = 0; i < nblocks; i++) {
        Block *b = &blocks[i];
        if (b->x < x + w && x < b->x + b->w && b->y < y + h && y < b->y + b->h) {
            if (dbg_tex > 0) LOG("block %016llX %dx%d at %d,%d dropped by %dx%d at %d,%d (frame %llu)\n", (unsigned long long)b->hash,
                                 b->w, b->h, b->x, b->y, w, h, x, y, (unsigned long long)frame_count);
            blocks[i--] = blocks[--nblocks];
        }
    }
}

/* VRAM changed in this rectangle without an upload (fill, copy): blocks under it no longer hold their image */
void texrep_invalidate(int x, int y, int w, int h) {
    if (nblocks) drop_overlapping(x, y, w, h);
}

/* a VRAM-to-VRAM copy: blocks lying wholly inside the source now also hold their image at the destination;
 * everything else under the destination is gone */
void texrep_copy(int sx, int sy, int dx, int dy, int w, int h) {
    if (dbg_tex > 0 && w * h >= 64) {
        static int n;
        if (n++ < 500) LOG("copy %dx%d from %d,%d to %d,%d (frame %llu)\n", w, h, sx, sy, dx, dy, (unsigned long long)frame_count);
    }
    Block moved[16];
    int nm = 0;
    for (int i = 0; i < nblocks && nm < 16; i++) {
        Block *b = &blocks[i];
        if (b->x >= sx && b->y >= sy && b->x + b->w <= sx + w && b->y + b->h <= sy + h) {
            moved[nm] = *b;
            moved[nm].x += dx - sx;
            moved[nm].y += dy - sy;
            nm++;
        }
    }
    if (nblocks) drop_overlapping(dx, dy, w, h);
    for (int k = 0; k < nm && nblocks < MAXBLOCKS; k++) {
        if (moved[k].x + moved[k].w > 1024 || moved[k].y + moved[k].h > 512) continue;
        if (dbg_tex > 0) LOG("block %016llX %dx%d copied to %d,%d (frame %llu)\n", (unsigned long long)moved[k].hash, moved[k].w,
                             moved[k].h, moved[k].x, moved[k].y, (unsigned long long)frame_count);
        blocks[nblocks++] = moved[k];
    }
}

/* a CPU upload finished (the software VRAM holds it): remember it if the pack has its block */
void texrep_upload(int x, int y, int w, int h) {
    /* tracked also while switched off, so switching on needs no reload; without the hw renderer only the blocks'
     * places and hashes (a save state made by a software-renderer run is complete for a hardware-renderer load) */
    if (!texrep_enabled) return;
    if (dbg_tex < 0) dbg_tex = getenv("FE_DBG_TEX") != 0;
    load_index();
    if (nblocks) drop_overlapping(x, y, w, h);
    if (x + w > 1024 || y + h > 512) return;
    XXH3_state_t st;
    XXH3_64bits_reset(&st);
    for (int r = 0; r < h; r++) XXH3_64bits_update(&st, &vram[(y + r) * 1024 + x], (size_t)w * 2);
    uint64_t hash = XXH3_64bits_digest(&st);
    /* the games' fonts: rebuilt natively from TrueType, before (and without) any texture pack */
    const void *fa = fonts_find(hash);
    if (fa && nblocks < MAXBLOCKS) {
        if (!hw_active) { blocks[nblocks++] = (Block){x, y, w, h, hash, -1, 0}; return; }
        unsigned tex = fonts_texture(fa, x, y);
        if (tex) { blocks[nblocks++] = (Block){x, y, w, h, hash, -1, tex}; return; }
    }
    if (!nreps) return;
    int f = first_rep(hash);
    if (dbg_tex > 0 && w * h >= 64 && !(w == 16 && h == 160)) {
        static int n;
        int dma2_busy(void);
        if (n++ < 2000) LOG("upload %dx%d at %d,%d hash %016llX%s frame %llu list %d\n", w, h, x, y, (unsigned long long)hash,
                              f >= 0 ? " (in the pack)" : "", (unsigned long long)frame_count, dma2_busy());
    }
    if (f >= 0 && nblocks >= MAXBLOCKS) {
        static int warned;
        if (!warned++) LOG("textures: block table full (%d) at frame %llu\n", nblocks, (unsigned long long)frame_count);
    }
    if (f < 0 || nblocks >= MAXBLOCKS) return;
    blocks[nblocks++] = (Block){x, y, w, h, hash, f, 0};
    if (!hw_active) return;
    /* a large picture (menu, loading screen): decode its images now, the first draw follows within a few
     * frames (small blocks may have an image per palette row: decoded when drawn) */
    if (w * h >= 4096)
        for (int k = f; k < nreps && reps[k].src == hash; k++)
        if (reps[k].ww == w && reps[k].h == h) request(&reps[k]);
}

/* a textured draw with texture coordinates umin..umax, vmin..vmax (texels, inclusive) */
void texrep_apply(HwState *st, int umin, int vmin, int umax, int vmax) {
    static long long dbg_from = -1; /* FE_DBG_TEX_FROM=frame: log every textured draw from then on */
    if (dbg_from < 0) { const char *e = getenv("FE_DBG_TEX_FROM"); dbg_from = e ? atoll(e) : 0; }
    if (dbg_from && (long long)frame_count >= dbg_from && (long long)frame_count < dbg_from + 3)
        LOG("draw frame %llu page %d,%d depth %d clut %d,%d uv %d..%d x %d..%d (%d blocks)\n", (unsigned long long)frame_count,
            st->tx, st->ty, st->depth, st->clut_x, st->clut_y, umin, umax, vmin, vmax, nblocks);
    if (!texrep_on || !nblocks || st->depth > 1) return; /* fonts too follow the remaster switch */
    int ppw = st->depth ? 2 : 4; /* texels per VRAM word */
    for (int i = 0; i < nblocks; i++) {
        Block *b = &blocks[i];
        int bx = (b->x - st->tx) * ppw, by = b->y - st->ty;
        if (dbg_tex > 0 && b->x >= st->tx && b->x < st->tx + 64 && b->y >= st->ty && b->y < st->ty + 256) {
            static int n2;
            if (n2++ < 60)
                LOG("draw on page %d,%d depth %d uv %d..%d x %d..%d, block %016llX at texels %d,%d %dx%d\n", st->tx, st->ty,
                    st->depth, umin, umax, vmin, vmax, (unsigned long long)b->hash, bx, by, b->w * ppw, b->h);
        }
        if (bx > umin || by > vmin || umax >= bx + b->w * ppw || vmax >= by + b->h) continue;
        if (b->font) { /* a native font rebuild: an index map, 4-bit */
            if (st->depth != 0) return;
            st->rtex = b->font;
            st->rmode = 2;
            st->rox = (float)bx; st->roy = (float)by;
            st->rsw = (float)(b->w * ppw); st->rsh = (float)b->h;
            return;
        }
        /* the block holds the texels: a full-colour image for the live palette, else the index map */
        Rep *idx = 0, *hit = 0;
        uint64_t palhash = 0;
        int haveh = -1;
        for (int k = b->first; k < nreps && reps[k].src == b->hash && !hit; k++) {
            Rep *r = &reps[k];
            if (r->mode != st->depth || r->ww != b->w || r->h != b->h) continue;
            if (r->kind == 1) { idx = r; continue; }
            if (haveh != r->top) {
                int n = r->top + 1;
                if (st->clut_x + n > 1024) continue;
                palhash = XXH3_64bits(&vram[st->clut_y * 1024 + st->clut_x], (size_t)n * 2);
                haveh = r->top;
            }
            if (palhash == r->pal) hit = r;
        }
        Rep *use = hit ? hit : idx;
        if (dbg_tex > 0) {
            static uint64_t seen[512];
            static int ns;
            uint64_t key = b->hash ^ ((uint64_t)(hit ? 1 : idx ? 2 : 3) << 62) ^ ((uint64_t)st->clut_y << 40) ^ ((uint64_t)st->clut_x << 30);
            int dup = 0;
            for (int s2 = 0; s2 < ns; s2++) if (seen[s2] == key) dup = 1;
            if (!dup && ns < 512) seen[ns++] = key;
            if (!dup)
                LOG("tex frame %llu: block %016llX %dx%d at %d,%d mode %d clut %d,%d -> %s\n", (unsigned long long)frame_count,
                    (unsigned long long)b->hash, b->w, b->h, b->x, b->y, st->depth, st->clut_x, st->clut_y,
                    hit ? "full colour" : idx ? "index map" : "no palette match");
        }
        int big = b->w * b->h >= 4096; /* VRAM words: menu and loading screen pictures */
        if (!use || !ready(use, opt_fast ? 3000 : big ? 1000 : 0)) return;
        st->rtex = use->tex;
        st->rmode = hit ? 1 : 2;
        st->rox = (float)bx; st->roy = (float)by;
        st->rsw = (float)(b->w * ppw); st->rsh = (float)b->h;
        return;
    }
}

/* ------------------------------------------------------------------ save states (state.c)
 * The remembered blocks: where they lie and the hash of their upload (the VRAM there may have been drawn over since,
 * so it is not hashed again); on a load each finds its font rebuild or pack images by that hash, as at the upload. */
#include "state.h"

void texrep_state(StIO *io) {
    st_begin(io, "TEXREP", 2);
    int n = st_count(io, nblocks, MAXBLOCKS);
    static struct { int16_t x, y, w, h; uint64_t hash; } r[MAXBLOCKS];
    for (int i = 0; i < n; i++) {
        if (!io->load) { r[i].x = (int16_t)blocks[i].x; r[i].y = (int16_t)blocks[i].y; r[i].w = (int16_t)blocks[i].w; r[i].h = (int16_t)blocks[i].h; r[i].hash = blocks[i].hash; }
        ST(io, r[i]);
    }
    st_end(io);
    if (!io->load || io->dry || io->err[0]) return;
    nblocks = 0;
    if (!texrep_enabled) return;
    load_index();
    for (int i = 0; i < n && nblocks < MAXBLOCKS; i++) {
        if (!hw_active) { blocks[nblocks++] = (Block){r[i].x, r[i].y, r[i].w, r[i].h, r[i].hash, -1, 0}; continue; }
        const void *fa = fonts_find(r[i].hash);
        unsigned tex = fa ? fonts_texture(fa, r[i].x, r[i].y) : 0;
        int f = tex ? -1 : nreps ? first_rep(r[i].hash) : -1;
        if (!tex && f < 0) continue;
        blocks[nblocks++] = (Block){r[i].x, r[i].y, r[i].w, r[i].h, r[i].hash, f, tex};
    }
}
