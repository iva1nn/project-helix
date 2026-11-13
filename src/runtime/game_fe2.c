/* Fear Effect 2 specifics: automatic disc swap.
 * The game asks for a disc by loading the SWAP overlay (first word 0x0C at 0x800AF0C8) with the
 * wanted disc (0-based) at 0x80082E94; it reads \FEAR.BIN on the inserted disc until it matches
 * (F:\fe_port\third_party\fe2_remaster\work\discswap\README.md). Here the port inserts the wanted image itself. */
#include <stdlib.h>
#include <SDL2/SDL.h>
#include "hw.h"
#include "props.h"

#define SWAP_OVERLAY_ID 0x0000000Cu
#define OVERLAY_BASE 0x800AF0C8u
#define WANTED_DISC 0x80082E94u

int opt_no_autoswap;

/* Fear Effect 1 (SLUS-00920, 01056-01058) has the same disc check (FE1 0x8004CCD8 = FE2 0x8004F034): on a
 * mismatch it sets the module word (0x80082D6C) to 9, the swap screen, with the wanted disc at gp+0x604 */
#define FE1_MODULE 0x80082D6Cu
#define FE1_WANTED_DISC 0x80082BE8u
#define FE1_SWAP_MODULE 9u

static int is_fe1(void) { return !strcmp(game_serial, "SLUS-00920") || !strncmp(game_serial, "SLUS-0105", 9); }
static void standin_vblank(void);
static int wait; /* frames the swap screen has been up with the drive idle (save states keep it) */

void game_vblank(void) {
    static int warned = -1;
    int fe1 = is_fe1();
    standin_vblank();
    if (fe1 && getenv("FE_DBG_MODULE")) { /* module changes (FE1 module word, wanted disc) */
        static uint32_t last = 0xFFFFFFFFu;
        uint32_t m = mem_r32(FE1_MODULE);
        if (m != last) LOG("module %u -> %u (wanted disc %d) frame %llu\n", last, m, (int)(int8_t)mem_r8(FE1_WANTED_DISC),
                           (unsigned long long)frame_count);
        last = m;
    }
    if (!fe1 && strncmp(game_serial, "SLUS-0126", 9) && strncmp(game_serial, "SLUS-0127", 9)) return;
    if (opt_no_autoswap || disc_count < 2) return;
    if (fe1 ? mem_r32(FE1_MODULE) != FE1_SWAP_MODULE : mem_r32(OVERLAY_BASE) != SWAP_OVERLAY_ID) { wait = 0; return; }
    int wanted = (int)(int8_t)mem_r8(fe1 ? FE1_WANTED_DISC : WANTED_DISC);
    if (wanted < 0 || wanted > 3 || wanted == disc_current) { wait = 0; return; }
    /* let the swap screen come up, as on hardware: 30 frames with the drive idle (FE1 loads the screen's
     * pictures from the old disc first; swapping during that read left it reading the new disc at old positions) */
    if (cdrom_reading()) { wait = 0; return; }
    if (++wait < 30) return;
    wait = 0;
    if (!disc_insert(wanted) && warned != wanted) {
        warned = wanted;
        LOG("disc swap: the game wants disc %d but its image was not found; press F1-F4 to insert a disc image by hand\n",
            wanted + 1);
    }
}

/* ------------------------------------------------------------------ remaster movie sets
 * Rooms and cutscenes are STR movies. libcd's StGetNext (0x800659FC) hands the game the next frame;
 * its ring header holds the STR frame number (+8) and the disc position of the frame's first sector
 * (+0x1C, BCD minute/second/frame). The movie player stores the STR frame number at 0x800A4FDC just
 * before decoding and uploads the 320x160 picture as twenty 16x160 slices into two VRAM rectangles
 * per buffer (buffer structs at *0x800A4FB0 / *0x800A4FB4: +0 the 256-wide rect, +16 the 64-wide
 * rect). Each slice of a frame that has an 8x replacement is drawn from the replacement instead.
 * (F:\fe_port\third_party\fe2_remaster\FINDINGS.md "How a frame is chosen"; the frame identity here is exact.) */

/* FE2 addresses; FE1 has the same player 0x28 bytes lower (FE1 0x8003BE94 = FE2 0x8003EBFC reads the frame
 * number) and libcd's StGetNext at 0x80061D3C (tools/match_code.py) */
static uint32_t ST_GET_NEXT = 0x800659FCu, STR_FRAME_NO = 0x800A4FDCu, BUF_STRUCT0 = 0x800A4FB0u, BUF_STRUCT1 = 0x800A4FB4u;

int remaster_on = 1;
static char remaster_work[1024];
static int movies_disc = -1;
static struct { uint32_t frame_no, lba; } recent[32];
static int recent_pos;
static int hd_ok; /* the current picture has a replacement */
static int hd_movie = -1, hd_frame = -2, late;
static int hd_stale;      /* a jump's picture has not arrived: the HD picture on hand is the old room's */
/* room loops (NOTES.md "Room loops"): the first frame of the current in-sequence run, and the crossfade under way */
int movie_loop_fade = 8;
static int run_start = -1, fade_left;
static int buf_state[2]; /* per movie buffer: 0 no 8x movie, 1 its 8x picture, 2 a native stand-in for a late one */
/* a stand-in buffer's frame and where its 20 slices went, to put the 8x picture in once it is decoded */
static struct { int movie, frame; uint64_t since; int16_t x[20], y[20]; uint32_t cols; } standin[2];
int movie_piece_native;  /* the movie piece just checked comes from a stand-in (gpu.c records it per framebuffer) */
static int dbg_movie(void) { static int d = -1; if (d < 0) d = getenv("FE_DBG_MOVIE") != 0; return d; }
static int st_idx = -1;

static int bcd(uint8_t v) { return (v >> 4) * 10 + (v & 15); }

static int st_get_next_hook(void) {
    uint32_t hdr_out = R5;
    call_original(st_idx);
    if (R2 == 0) {
        uint32_t hdr = mem_r32(hdr_out);
        uint32_t frame_no = mem_r32(hdr + 8);
        int mm = bcd(mem_r8(hdr + 0x1C)), ss = bcd(mem_r8(hdr + 0x1D)), ff = bcd(mem_r8(hdr + 0x1E));
        int lba = (mm * 60 + ss) * 75 + ff - 150;
        recent[recent_pos & 31].frame_no = frame_no;
        recent[recent_pos & 31].lba = (uint32_t)lba;
        recent_pos++;
    }
    return 1;
}

static void hold_reset(void);
static void load_movies_for_disc(void) {
    if (movies_disc == disc_current || !remaster_work[0]) return;
    movies_disc = disc_current;
    hold_reset(); /* the 60 fps loops' measured holds are per movie of the set */
    char p[1200], dir[2200] = "";
    /* the data folder: movies/disc<N>; a remaster work folder (development): active_stream_<serial of the
     * disc>.txt names the disc's set (FE2's Disc 1 also active_stream.txt), relative = to the work folder */
    snprintf(p, sizeof p, "%s/movies/disc%d/movies.txt", remaster_work, (disc_current & 3) + 1);
    FILE *f = fopen(p, "r");
    if (f) {
        fclose(f);
        snprintf(dir, sizeof dir, "%s/movies/disc%d", remaster_work, (disc_current & 3) + 1);
        movies_load(dir);
        return;
    }
    static const int fe1_serials[4] = {920, 1056, 1057, 1058}, fe2_serials[4] = {1266, 1275, 1276, 1277};
    snprintf(p, sizeof p, "%s/active_stream_SLUS-%05d.txt", remaster_work,
             (is_fe1() ? fe1_serials : fe2_serials)[disc_current & 3]);
    f = fopen(p, "r");
    if (!f && !is_fe1() && disc_current == 0) {
        snprintf(p, sizeof p, "%s/active_stream.txt", remaster_work);
        f = fopen(p, "r");
    }
    if (f) {
        char d[1024] = "";
        if (fgets(d, sizeof d, f)) {
            size_t n = strlen(d);
            while (n && (d[n - 1] == '\n' || d[n - 1] == '\r' || d[n - 1] == ' ')) d[--n] = 0;
        }
        fclose(f);
        int abs = d[0] == '/' || d[0] == '\\' || (d[0] && d[1] == ':');
        if (abs) snprintf(dir, sizeof dir, "%s", d);
        else if (d[0]) snprintf(dir, sizeof dir, "%s/%s", remaster_work, d);
    }
    if (dir[0]) movies_load(dir);
    else LOG("remaster: no movie set for disc %d (%s)\n", disc_current + 1, p);
}

static int rect_col(uint32_t st, int x, int y) {
    if (!st) return -1;
    /* the struct holds pointers to the two RECTs (x, y, w, h as int16) */
    uint32_t ra = mem_r32(st), rb = mem_r32(st + 16);
    if ((ra >> 24) != 0x80 || (rb >> 24) != 0x80) return -1;
    int ax = (int16_t)mem_r16(ra), ay = (int16_t)mem_r16(ra + 2), aw = (int16_t)mem_r16(ra + 4);
    int bx = (int16_t)mem_r16(rb), by = (int16_t)mem_r16(rb + 2), bw = (int16_t)mem_r16(rb + 4);
    if (y == ay && x >= ax && x < ax + aw) return (x - ax) / 16;
    if (y == by && x >= bx && x < bx + bw) return 16 + (x - bx) / 16;
    return -1;
}

/* The game draws each movie frame from its two VRAM pieces (256x160 and 64x160) as two stretched 16-bit
 * textured quads side by side. u is 8-bit, so a quad's u stops at the piece's last texel (255 / 63) and that
 * column is squeezed out; at N x with the 8x movie the joint at x = 256 shows as a break (the remaster fork's
 * fix of 1 Nov 2026, F:\fe_remaster\docs\FINDINGS.md "RESOLVED: vertical seam at the movie's x = 256").
 * Returns 1 when a 16-bit textured polygon over VRAM x0..x1, y0..y1 lies in a movie piece and reaches its
 * last column: the renderer then carries u one texel further on the right-hand vertices. */
static int fe_game;

/* scene actors (models.c): the 8x picture on screen and whether this GPU pass has drawn the room's background yet
 * (both pieces) */
static uint32_t bg_serial;
static int bg_mask, bg_buf = -1;
static int buf_movie[2] = {-1, -1}, buf_frame[2]; /* the 8x picture each movie buffer holds */
static uint32_t buf_lba[2]; /* the disc sector of the movie frame each buffer holds (0 = none; save states) */
/* the run each buffer's picture belongs to: a new run at every jump that is not the loop's restart (a cut, a room
 * change, a state load); models.c ties the scene actors' camera to it (NOTES "Scene actors: the camera of the picture") */
static unsigned run_id = 1, buf_run[2], hd_run;
int movie_bg_drawn(void) { return bg_serial == models_dma_serial && bg_mask == 3; }

/* ------------------------------------------------------------------ 60 fps room movies (NOTES.md "60 fps room movies")
 * A movie with rate R > 1 (movies.txt) has R pictures per STR frame. The game uploads STR frame f once (picture f * R)
 * and draws it on two game frames, 2 vblanks apart; the 60 fps in-between frames (gpu_hw.c) replay each between them.
 * Each draw and each in-between shows picture f * R + step, step = the vblanks since a pass first drew frame f (x R / 4,
 * at most R - 1): the picture goes into the movie buffer's upscaled VRAM just before the background reads it (the
 * game's pass: at its first movie piece; an in-between: before its replay). The scene actors take the same time. */
int movie_hfr = 1;
static int buf_pic[2] = {-1, -1};          /* the picture each movie buffer holds in the upscaled VRAM (-1 unknown) */
static int16_t buf_x[2][20], buf_y[2][20]; /* where its 20 slices went */
static uint32_t buf_cols[2], buf_known[2]; /* slices of the current frame / ever seen (the rects do not move) */
static int buf_faded[2];                   /* its picture got the loop crossfade: no sub-steps (they are not faded) */
static int shown_m = -1, shown_f;          /* the background frame of the newest pass, and the vblank of its first pass */
static uint64_t shown_at;
static int hfr_dbg(void) { static int d = -1; if (d < 0) d = getenv("FE_DBG_HFR") != 0; return d; }

static int pic_step(int vblanks, int R) {
    int k = vblanks * R / 4;
    return k < 0 ? 0 : k > R - 1 ? R - 1 : k;
}

/* The hold before a loop's restart (NOTES "60 fps loops: the hold before the restart"). The game shows a loop's last
 * STR frame for 8 vblanks, not 4, before the loop's first (every 60 fps loop measured: the CD seek back delays the
 * start's next frame), so its last picture stayed up for 5 vblanks: the water froze, then went on from picture 0. The
 * game seeks the CD back to the loop start when it uploads that last frame, 6 vblanks before its first pass, so it is
 * known in time: its R pictures are spread over the hold (vblanks x R / hold: 2 vblanks each for 8) and the restart
 * lands on picture 0 as before, no picture shown again at the seam or skipped. The hold is the movie's last measured
 * one (the last frame's first pass to the start's), 8 before the first restart. A 1-picture movie, a crossfaded loop
 * and every other frame keep the 4-vblank step. FE_LOOP_HOLD_OLD=1: as before. */
static int hold_m = -1, hold_f = -1; /* the loop's last frame, once the game has sought back to the loop's start */
static uint64_t hold_from;           /* the vblank of that frame's first pass (0 = not drawn yet) */
static uint8_t hold_len[64];         /* per movie: the hold last measured, in vblanks (0 = none yet: 8) */
static int hold_old(void) { static int d = -1; if (d < 0) d = getenv("FE_LOOP_HOLD_OLD") != 0; return d; }

/* the picture of STR frame f of movie m (rate R) shown vblanks after the frame's first pass: f * R + this */
static void hold_reset(void) { hold_m = hold_f = -1; hold_from = 0; memset(hold_len, 0, sizeof hold_len); }
static int frame_step(int m, int f, int vblanks, int R) {
    if (R > 1 && m == hold_m && f == hold_f) {
        int H = hold_len[m & 63] ? hold_len[m & 63] : 8;
        int k = vblanks * R / H;
        return k < 0 ? 0 : k > R - 1 ? R - 1 : k;
    }
    return pic_step(vblanks, R);
}

/* picture pic of movie m into movie buffer s (its 20 slices at 8x), through upload slot 1. borrow: the buffer holds a
 * later frame (or part of one) by now; its content is put aside first and back after the replay (movie_inbetween_end) */
static int borrowed = -1;
static int put_picture(int s, int m, int pic, const char *why, int borrow) {
    if (!borrow && buf_pic[s] == pic) return 1;
    if ((borrow ? buf_known[s] : buf_cols[s]) != 0xFFFFFu) return 0;
    int mw, mh;
    uint64_t pt = perf_now();
    const uint8_t *rgb = movie_peek(m, pic, &mw, &mh);
    if (!rgb && opt_fast) rgb = movie_frame(m, pic, 1000, &mw, &mh); /* headless --fast waits, as the uploads do */
    perf_add(0, pt);
    if (!rgb) {
        if (hfr_dbg()) LOG("hfr: frame %llu %s buffer %d picture %d not decoded: holds %d\n", (unsigned long long)frame_count, why, s,
                           pic, buf_pic[s]);
        return 0;
    }
    pt = perf_now();
    if (borrow) { hw_vram_stash(20, buf_x[s], buf_y[s], 16, 160, 0); borrowed = s; }
    hw_hd_slot(1);
    hw_hd_upload(rgb, mw, mh);
    movie_release();
    for (int c = 0; c < 20; c++) hw_hd_blit(320, 160, c * 16, c * 16 + 16, buf_x[s][c], buf_y[s][c]);
    hw_hd_slot(0);
    perf_add(1, pt);
    if (hfr_dbg()) LOG("hfr: frame %llu %s buffer %d picture %d%s\n", (unsigned long long)frame_count, why, s, pic, borrow ? " (borrowed)" : "");
    if (!borrow) buf_pic[s] = pic;
    return 1;
}

void movie_inbetween_end(void) {
    if (borrowed < 0) return;
    hw_vram_stash(20, buf_x[borrowed], buf_y[borrowed], 16, 160, 1);
    borrowed = -1;
}

/* a pass is about to draw its background from movie buffer s (its first movie piece) */
static void pass_background(int s) {
    if (buf_state[s] != 1 || buf_movie[s] < 0) return;
    int m = buf_movie[s], f = buf_frame[s];
    if (m != shown_m || f != shown_f) {
        if (hold_m >= 0 && hold_from) {
            /* the loop's start after its held last frame: the hold as measured, for the next restart; the mapping stays
             * for the held frame's last in-betweens and is dropped two frames on */
            if (shown_m == hold_m && shown_f == hold_f && m == hold_m && f < hold_f) {
                uint64_t h = frame_count - hold_from;
                if (h >= 4 && h <= 31) hold_len[m & 63] = (uint8_t)h;
                if (dbg_movie() || hfr_dbg())
                    LOG("movies: loop hold %s frame %d up %llu vblanks (frame %llu)\n", movie_name(m), hold_f,
                        (unsigned long long)h, (unsigned long long)frame_count);
            } else if (m != hold_m || (f != hold_f && f > run_start + 1)) hold_m = hold_f = -1;
        }
        shown_m = m; shown_f = f; shown_at = frame_count;
        if (m == hold_m && f == hold_f) hold_from = frame_count;
    }
    int R = movie_rate(m);
    if (R <= 1 || !movie_hfr || buf_faded[s]) return;
    int sub = (int)(frame_count - shown_at);
    put_picture(s, m, f * R + frame_step(m, f, sub, R), "pass", 0);
    smooth_bg_mark(m | f << 6 | s << 22 | (sub > 31 ? 31 : sub) << 23);
}

/* the in-between frame dk vblanks after a pass that drew tag's background (gpu_hw.c smooth_build); 0 = no in-between
 * (the drawn frame stays up). The game decodes the next frame but one into the buffer as soon as the next frame is
 * uploaded (A01: 2 vblanks after the pass), so the buffer is usually borrowed. */
int movie_tag_pic(int tag, int dk) {
    int m = tag & 63, f = (tag >> 6) & 0xFFFF, sub = (tag >> 23) & 31, R = movie_rate(m);
    return f * R + frame_step(m, f, sub + dk, R);
}

int movie_inbetween(int tag, int dk) {
    int m = tag & 63, f = (tag >> 6) & 0xFFFF, s = (tag >> 22) & 1, sub = (tag >> 23) & 31;
    if (!movie_hfr) return 0;
    int R = movie_rate(m), pic = f * R + frame_step(m, f, sub + dk, R);
    int same = buf_state[s] == 1 && buf_movie[s] == m && buf_frame[s] == f;
    if (same && buf_faded[s]) return 1; /* a loop crossfade: the drawn (faded) picture, no sub-steps */
    /* not decoded: the drawn picture stays in place (a hold), or, borrowed, the in-between is not built */
    return put_picture(s, m, pic, "in-between", !same) || same;
}

/* the scene actors' time inside the shown movie frame (models.c): the background's own picture for a 60 fps movie */
float movie_shown_frac(void) {
    int s = bg_buf;
    if (bg_serial != models_dma_serial || s < 0 || buf_movie[s] != shown_m || buf_frame[s] != shown_f) return 0;
    int sub = (int)(frame_count - shown_at), R = movie_rate(shown_m);
    if (R > 1 && movie_hfr && !buf_faded[s]) return (float)frame_step(shown_m, shown_f, sub, R) / (float)R;
    float x = (float)sub / 4.0f; /* 4 vblanks per movie frame (15 fps) */
    return x < 0.999f ? x : 0.999f;
}
/* the 8x picture drawn as this pass's background (the buffer it came from: the game alternates two, so the newest
 * upload can be a frame ahead of the one on screen; a moving camera makes that visible) */
int movie_shown(int *frame) {
    if (!remaster_on) return -1;
    if (bg_serial == models_dma_serial && bg_buf >= 0 && buf_movie[bg_buf] >= 0 && buf_state[bg_buf] == 1) {
        *frame = buf_frame[bg_buf];
        return buf_movie[bg_buf];
    }
    if (!hd_ok || hd_movie < 0) return -1;
    *frame = hd_frame;
    return hd_movie;
}

/* the run of the picture movie_shown() names (a new number after every cut, room change or load; not at a loop's
 * restart) */
unsigned movie_shown_run(void) {
    if (bg_serial == models_dma_serial && bg_buf >= 0 && buf_movie[bg_buf] >= 0 && buf_state[bg_buf] == 1) return buf_run[bg_buf];
    return hd_run;
}

/* where this pass's movie picture lies in VRAM (scene mattes, models.c): from the 256-wide piece's vertices, the VRAM
 * x of the picture's column 0 and 320 and y of its rows 0 and 160 */
static int piece_r = -1, piece_rx, piece_ry;
static float bg_rect[4];
static uint32_t bg_rect_serial;
static int bg_rect_ok;
void movie_piece_vertices(int nv, const int *x, const int *y, const int *u, const int *v, int tx, int ty) {
    if (piece_r != 0) return;
    int a = 0, b = 0, c = 0, d = 0; /* the vertices of the least and greatest column, row */
    for (int i = 1; i < nv; i++) {
        if (u[i] < u[a]) a = i;
        if (u[i] > u[b]) b = i;
        if (v[i] < v[c]) c = i;
        if (v[i] > v[d]) d = i;
    }
    if (u[b] == u[a] || v[d] == v[c]) return;
    float ca = (float)(tx + u[a] - piece_rx), cb = (float)(tx + u[b] - piece_rx);
    float ra = (float)(ty + v[c] - piece_ry), rb = (float)(ty + v[d] - piece_ry);
    float sx = (float)(x[b] - x[a]) / (cb - ca), sy = (float)(y[d] - y[c]) / (rb - ra);
    bg_rect[0] = (float)x[a] - ca * sx; bg_rect[2] = bg_rect[0] + 320.0f * sx;
    bg_rect[1] = (float)y[c] - ra * sy; bg_rect[3] = bg_rect[1] + 160.0f * sy;
    bg_rect_serial = models_dma_serial;
    bg_rect_ok = 1;
}
int movie_bg_rect(float *r) {
    if (!bg_rect_ok || bg_rect_serial != models_dma_serial) return 0;
    memcpy(r, bg_rect, sizeof bg_rect);
    return 1;
}

int movie_piece_last_column(int x0, int x1, int y0, int y1) {
    static int dbg = -1;
    static int off = -1;
    if (dbg < 0) { dbg = getenv("FE_DBG_PIECES") != 0; off = getenv("FE_NO_PIECE_FIX") != 0; }
    piece_r = -1;
    if (!fe_game || off) return 0;
    for (int s = 0; s < 2; s++) {
        uint32_t st = mem_r32(s ? BUF_STRUCT1 : BUF_STRUCT0);
        if ((st >> 24) != 0x80) continue;
        for (int r = 0; r < 2; r++) {
            uint32_t ra = mem_r32(st + 16 * (uint32_t)r);
            if ((ra >> 24) != 0x80) continue;
            int rx = (int16_t)mem_r16(ra), ry = (int16_t)mem_r16(ra + 2), rw = (int16_t)mem_r16(ra + 4),
                rh = (int16_t)mem_r16(ra + 6);
            if (x0 < rx || x1 >= rx + rw || y0 < ry || y1 >= ry + rh) continue;
            if (dbg) {
                static uint32_t seen[16];
                static int ns;
                uint32_t k = (uint32_t)(s << 30 | r << 29 | x0 << 15 | x1);
                int dup = 0;
                for (int i = 0; i < ns; i++) dup |= seen[i] == k;
                if (!dup && ns < 16) {
                    seen[ns++] = k;
                    LOG("movie piece: buffer %d %s rect (%d,%d %dx%d), VRAM x %d..%d y %d..%d%s\n", s, r ? "64" : "256", rx, ry,
                        rw, rh, x0, x1, y0, y1, x1 == rx + rw - 1 ? ", reaches the last column" : "");
                }
            }
            movie_piece_native = buf_state[s] == 2;
            piece_r = r; piece_rx = rx; piece_ry = ry;
            if (bg_serial != models_dma_serial) { bg_serial = models_dma_serial; bg_mask = 0; pass_background(s); }
            bg_mask |= 1 << r;
            bg_buf = s;
            if (dbg_movie()) {
                static uint64_t lastf;
                static int lasts = -1;
                if (lastf != frame_count || lasts != s)
                    LOG("draw: frame %llu background from buffer %d (%s), piece %s\n", (unsigned long long)frame_count, s,
                        buf_state[s] == 1 ? "8x" : buf_state[s] == 2 ? "stand-in" : "no 8x movie", r ? "64" : "256");
                lastf = frame_count; lasts = s;
            }
            return x1 == rx + rw - 1;
        }
    }
    return 0;
}

static int movie_upload_hook(int x, int y, int w, int h) {
    static int dbg;
    {
        static int64_t a = -2, b;
        if (a == -2) { const char *e = getenv("FE_DBG_UPLOADS"); a = e ? atoll(e) : -1; b = a + 40; }
        if (a >= 0 && (int64_t)frame_count >= a && (int64_t)frame_count <= b && !(w == 16 && h == 160))
            LOG("upload %dx%d at %d,%d frame %llu\n", w, h, x, y, (unsigned long long)frame_count);
    }
    if (getenv("FE_DBG_MOVIE") && dbg < 40 && w >= 16) {
        dbg++;
        uint32_t s0 = mem_r32(BUF_STRUCT0), s1 = mem_r32(BUF_STRUCT1);
        LOG("upload %dx%d at %d,%d  structs %08X %08X\n", w, h, x, y, s0, s1);
        static int scanned;
        if (w == 16 && h == 160 && !scanned++) /* where are the buffer structs? words in the exe's data pointing at a struct whose +0 points at a 256x160 rect */
            for (uint32_t a = 0x80080000u; a < 0x800B0000u; a += 4) {
                uint32_t st = mem_r32(a);
                if ((st >> 24) != 0x80 || (st & 3)) continue;
                uint32_t ra = mem_r32(st & 0x801FFFFCu);
                if ((ra >> 24) != 0x80 || (ra & 1)) continue;
                if (mem_r16(ra + 4) == 256 && mem_r16(ra + 6) == 160)
                    LOG("  candidate struct pointer at %08X -> %08X -> rect %d,%d\n", a, st, mem_r16(ra), mem_r16(ra + 2));
            }
    }
    if (!remaster_on || w != 16 || h != 160) return 0;
    int buf = 0, col = rect_col(mem_r32(BUF_STRUCT0), x, y);
    if (col < 0) { buf = 1; col = rect_col(mem_r32(BUF_STRUCT1), x, y); }
    if (col < 0) return 0;
    if (col == 0) {
        hd_ok = 0;
        buf_state[buf] = 0;
        buf_lba[buf] = 0;
        load_movies_for_disc();
        uint32_t frame_no = mem_r32(STR_FRAME_NO);
        static int dbg2;
        if (getenv("FE_DBG_MOVIE") && dbg2++ < 30) {
            LOG("slice 0: frame word %u; ring:", frame_no);
            for (int k = 1; k <= 6; k++) LOG(" %u@%u", recent[(recent_pos - k) & 31].frame_no, recent[(recent_pos - k) & 31].lba);
            LOG("\n");
        }
        for (int k = 1; k <= 32; k++) {
            int i = (recent_pos - k) & 31;
            if (recent[i].frame_no != frame_no || !recent[i].lba) continue;
            int frame, mw, mh;
            movie_shown_lba = recent[i].lba; /* props.c (FE_DBG_PROPS) */
            int m = movie_lookup(recent[i].lba, &frame);
            buf_lba[buf] = recent[i].lba;
            if (m < 0) break;
            /* A jump (a room change or a cut; the decoder was sent there at the CD seek) whose picture is not
             * decoded yet: the native frame goes into this buffer, marked as a stand-in; the window does not
             * show frames drawn from a stand-in (hw_present, at most 0.7 s; the game and its sound run on), so
             * the new room appears together with the models drawn over it. In-sequence frames that are late keep
             * the previous 8x picture of the same stretch. Headless --fast runs wait instead. */
            int jump = m != hd_movie || frame < hd_frame || frame > hd_frame + 8;
            int R = movie_rate(m); /* a 60 fps movie: STR frame f is picture f * R, its first sub-step */
            uint64_t pt = perf_now();
            const uint8_t *rgb = movie_frame(m, frame * R, opt_fast ? 1000 : jump ? 8 : 3, &mw, &mh);
            perf_add(0, pt);
            if (dbg_movie() && (jump || hd_stale))
                LOG("movies: %s %s frame %d (frame %llu, buffer %d): %s\n", jump ? "jump to" : "late in", movie_name(m), frame,
                    (unsigned long long)frame_count, buf, rgb ? "8x picture" : "native stand-in");
            /* a loop restart: back inside the same movie to a frame of the run on screen (a room change lands
             * outside it); the last picture dissolves into the loop's start */
            /* (2 frames of slack: the first pass may start a frame after the loop start, 121 for A01's 120) */
            int restart = jump && m == hd_movie && !hd_stale && run_start >= 0 && frame >= run_start - 2 && frame < hd_frame;
            if (jump) { fade_left = 0; run_start = frame; }
            if (jump && !restart) run_id++;
            /* the loop end (frame_step): a cut, a room change or a jump forward drops it (a jump back in the same movie
             * is the restart it was set for, also the first one after a state load, which the rule above does not
             * count); an in-sequence upload after the seek back is the last frame (the seek may come just before it) */
            if (hold_m >= 0 && (m != hold_m || (jump && frame >= hold_f))) hold_m = hold_f = -1;
            else if (hold_m == m && !jump && frame > hold_f && !hold_from) hold_f = frame;
            hd_run = run_id;
            if (rgb) {
                pt = perf_now();
                /* a 60 fps loop (rate > 1, shown with its in-betweens) is not crossfaded: a faded picture has no
                 * sub-steps, so the fade would drop it to 15 fps for loop_fade frames; its file's last in-betweens lead
                 * into the loop's first frame instead (tools/movie_60fps.py --loop-end; NOTES "60 fps room movies") */
                int hfr_loop = movie_hfr && R > 1;
                if (restart && movie_loop_fade > 0 && !hfr_loop && hw_hd_fade_begin()) fade_left = movie_loop_fade;
                if (restart && dbg_movie())
                    LOG("movies: loop restart %s frame %d -> %d (frame %llu)%s\n", movie_name(m), hd_frame, frame,
                        (unsigned long long)frame_count, fade_left ? ", crossfade" : "");
                hw_hd_upload(rgb, mw, mh);
                movie_release();
                buf_faded[buf] = fade_left > 0;
                if (fade_left > 0) {
                    hw_hd_fade_apply((float)fade_left / (float)(movie_loop_fade + 1));
                    fade_left--;
                }
                buf_pic[buf] = frame * R;
                if (!jump) movie_keep(m, run_start * R, R > 1 ? 24 : 12); /* a 60 fps loop: its first 6 STR frames */
                perf_add(1, pt);
                hd_ok = 1;
                hd_movie = m;
                hd_frame = frame;
                hd_stale = 0;
            } else if (jump || hd_stale) {
                hd_ok = 0;
                hd_movie = m;
                hd_frame = frame;
                hd_stale = 1;
            } else {
                hd_ok = 1;
                hd_frame = frame;
                late++;
            }
            if (!rgb) buf_pic[buf] = -1; /* the previous picture or a stand-in */
            buf_cols[buf] = 0;
            buf_state[buf] = hd_ok ? 1 : 2;
            buf_movie[buf] = hd_movie; buf_frame[buf] = hd_frame; buf_run[buf] = run_id;
            if (!hd_ok) { standin[buf].movie = m; standin[buf].frame = frame; standin[buf].since = frame_count; standin[buf].cols = 0; }
            break;
        }
    }
    if (buf_state[buf] == 2) { standin[buf].x[col] = (int16_t)x; standin[buf].y[col] = (int16_t)y; standin[buf].cols |= 1u << col; }
    buf_x[buf][col] = (int16_t)x; buf_y[buf][col] = (int16_t)y; buf_cols[buf] |= 1u << col; buf_known[buf] |= 1u << col;
    if (!hd_ok) return 0;
    hw_hd_blit(320, 160, col * 16, col * 16 + 16, x, y);
    return 1;
}

/* A stand-in stays in its buffer while the game redraws the room from it (a still room: until the movie
 * loops), so the window held the old room for the whole 0.7 s although the 8x frame came a frame later. */
static void standin_vblank(void) {
    for (int s = 0; s < 2; s++) {
        if (buf_state[s] != 2 || standin[s].cols != 0xFFFFFu || frame_count - standin[s].since > 45) continue;
        int mw, mh;
        int R = movie_rate(standin[s].movie);
        const uint8_t *rgb = movie_peek(standin[s].movie, standin[s].frame * R, &mw, &mh);
        if (!rgb) continue;
        hw_hd_slot(1); /* not the game's upload slot: a frame's slices may still be on their way into the other buffer */
        hw_hd_upload(rgb, mw, mh);
        movie_release();
        for (int c = 0; c < 20; c++) hw_hd_blit(320, 160, c * 16, c * 16 + 16, standin[s].x[c], standin[s].y[c]);
        hw_hd_slot(0);
        buf_state[s] = 1;
        buf_pic[s] = standin[s].frame * R;
        buf_faded[s] = 0;
        if (hd_movie == standin[s].movie && hd_frame == standin[s].frame) { hd_ok = 1; hd_stale = 0; }
        if (dbg_movie())
            LOG("movies: %s frame %d put into stand-in buffer %d (frame %llu)\n", movie_name(standin[s].movie),
                standin[s].frame, s, (unsigned long long)frame_count);
    }
}

/* the game seeks the CD: if the target is in a movie with an 8x replacement, start decoding it now */
static void movie_seek_hook(uint32_t sector) {
    if (!remaster_on) return;
    load_movies_for_disc();
    int frame, m = movie_lookup(sector, &frame);
    if (m < 0) return;
    if (dbg_movie()) LOG("movies: seek to %s frame %d (frame %llu)\n", movie_name(m), frame, (unsigned long long)frame_count);
    /* back to the start of the loop on screen in a 60 fps movie (movie_upload_hook's restart rule, or the movie's first
     * frames: after a state load run_start can be another room's): the newest frame uploaded is the loop's last, held
     * before the restart (frame_step) */
    if (!hold_old() && movie_hfr && movie_rate(m) > 1 && m == hd_movie && !hd_stale && frame < hd_frame &&
        ((run_start >= 0 && frame >= run_start - 2 && frame <= run_start + 2) || frame <= 2)) {
        hold_m = m; hold_f = hd_frame;
        hold_from = shown_m == m && shown_f == hd_frame ? shown_at : 0;
        if (hfr_dbg()) LOG("hfr: loop end %s frame %d, hold %d vblanks (frame %llu)\n", movie_name(m), hold_f,
                           hold_len[m & 63] ? hold_len[m & 63] : 8, (unsigned long long)frame_count);
    }
    uint64_t pt = perf_now();
    movie_prefetch(m, frame * movie_rate(m));
    perf_add(2, pt);
}

void game_init(const char *remaster_dir) {
    int fe1 = is_fe1();
    if (!fe1 && strncmp(game_serial, "SLUS-0126", 9) && strncmp(game_serial, "SLUS-0127", 9)) return;
    if (fe1) { ST_GET_NEXT = 0x80061D3Cu; STR_FRAME_NO = 0x800A4FB4u; BUF_STRUCT0 = 0x80092F58u; BUF_STRUCT1 = 0x80092F5Cu; }
    fe_game = 1; /* the movie piece rule applies with or without the remaster */
    if (!remaster_dir || !hw_active) return;
    snprintf(remaster_work, sizeof remaster_work, "%s", remaster_dir);
    st_idx = hook_index(ST_GET_NEXT);
    hook_register(ST_GET_NEXT, st_get_next_hook);
    gpu_upload_hook = movie_upload_hook;
    cdrom_seek_hook = movie_seek_hook;
    LOG("remaster: movie sets from %s\n", remaster_work);
}

/* ------------------------------------------------------------------ save states (state.c) */
#include "state.h"

void game_state(StIO *io) {
    st_begin(io, "GAME", 2);
    ST(io, wait);
    ST(io, recent); ST(io, recent_pos); /* the movie frames the game took last: the next upload's frame */
    ST(io, buf_lba);                    /* the movie frame in each movie buffer */
    ST(io, run_start);                  /* the room loop's first frame: its next restart dissolves as before */
    st_end(io);
}

/* After a load the upscaled VRAM holds the native pictures (rebuilt from the native VRAM). Each movie buffer gets its
 * 8x frame again: the decoder is sent there and the frame is waited for (at most 1.5 s, the window is black meanwhile),
 * then copied into the buffer's 20 slices as a fresh upload would be. A buffer whose frame does not come stays a
 * stand-in: the window holds until the game's next 8x frame, as at a room change (standin_vblank fills it later). */
void game_state_loaded(void) {
    hd_ok = 0; hd_movie = -1; hd_frame = -2; hd_stale = 0; late = 0;
    fade_left = 0; /* run_start comes from the state */
    memset(standin, 0, sizeof standin);
    bg_serial = 0; bg_mask = 0; bg_buf = -1;
    buf_movie[0] = buf_movie[1] = -1; buf_frame[0] = buf_frame[1] = 0;
    buf_state[0] = buf_state[1] = 0;
    movie_fb_native[0] = movie_fb_native[1] = 0;
    fb_draw_frame[0] = fb_draw_frame[1] = fb_room_frame[0] = fb_room_frame[1] = -1; /* the screen layout: the game's next pass */
    /* 60 fps room movies: presentation only, not in the state. The picture in each buffer is set below; the sub-step
     * clock starts at the next pass (the window is held for two flips after a load anyway) */
    for (int s = 0; s < 2; s++) { buf_pic[s] = -1; buf_cols[s] = buf_known[s] = 0; buf_faded[s] = 0; }
    shown_m = -1; shown_f = 0; shown_at = 0; borrowed = -1;
    hold_m = hold_f = -1; hold_from = 0; /* hold_len stays (measured on this movie set; a new set resets it) */
    hd_run = ++run_id; /* a run of its own (models.c takes the camera in RAM now) */
    if (!remaster_work[0] || !remaster_on) return;
    load_movies_for_disc();
    int newest = -1;
    for (int s = 0; s < 2; s++) {
        int frame, m = buf_lba[s] ? movie_lookup(buf_lba[s], &frame) : -1;
        uint32_t st = mem_r32(s ? BUF_STRUCT1 : BUF_STRUCT0);
        uint32_t ra = (st >> 24) == 0x80 ? mem_r32(st) : 0, rb = (st >> 24) == 0x80 ? mem_r32(st + 16) : 0;
        if (m < 0 || (ra >> 24) != 0x80 || (rb >> 24) != 0x80) continue;
        standin[s].movie = m; standin[s].frame = frame; standin[s].since = frame_count; standin[s].cols = 0xFFFFFu;
        for (int c = 0; c < 20; c++) {
            uint32_t r = c < 16 ? ra : rb;
            standin[s].x[c] = (int16_t)((int16_t)mem_r16(r) + 16 * (c < 16 ? c : c - 16));
            standin[s].y[c] = (int16_t)mem_r16(r + 2);
            buf_x[s][c] = standin[s].x[c]; buf_y[s][c] = standin[s].y[c]; /* the 60 fps sub-steps' slices */
        }
        buf_cols[s] = buf_known[s] = 0xFFFFFu;
        buf_state[s] = 2;
        buf_movie[s] = m; buf_frame[s] = frame; buf_run[s] = run_id;
        if (newest < 0 || buf_lba[s] > buf_lba[newest]) newest = s;
    }
    if (newest < 0) return;
    hd_movie = buf_movie[newest]; hd_frame = buf_frame[newest];
    for (int k = 0; k < 2; k++) { /* the newest buffer first: the decoder runs on from it */
        int s = k ? !newest : newest;
        if (buf_state[s] != 2) continue;
        int mw, mh, R = movie_rate(standin[s].movie); /* a 60 fps movie: STR frame f is picture f * R */
        const uint8_t *rgb = movie_frame(standin[s].movie, standin[s].frame * R, 1500, &mw, &mh);
        if (dbg_movie()) LOG("movies: state loaded: buffer %d %s frame %d %s\n", s, movie_name(standin[s].movie),
                             standin[s].frame, rgb ? "8x picture" : "not decoded in time: stand-in");
        if (!rgb) { movie_fb_native[0] = movie_fb_native[1] = 1; continue; }
        hw_hd_slot(0);
        hw_hd_upload(rgb, mw, mh);
        movie_release();
        for (int c = 0; c < 20; c++) hw_hd_blit(320, 160, c * 16, c * 16 + 16, standin[s].x[c], standin[s].y[c]);
        buf_state[s] = 1;
        buf_pic[s] = standin[s].frame * R;
    }
    hd_ok = buf_state[newest] == 1;
    if (hd_ok) movie_prefetch(hd_movie, hd_frame * movie_rate(hd_movie)); /* and on from the newest */
}
