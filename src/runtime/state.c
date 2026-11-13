/* Save states (NOTES.md "Save states").
 *
 * The game's code runs on the native C call stack (one C function per MIPS function, recomp.py), so a state can only
 * be taken where that stack can be rebuilt. Every recompiled function keeps the MIPS registers in the global cpu
 * struct, so a C frame holds nothing but the place to go on at; the shadow return stack (rs_stack) lists those places
 * for the whole guest call chain. recomp.py gives every function two kinds of resume points: its start (after the
 * hook point) and the return site of each call it makes. A state is therefore taken at the entry of the libetc
 * VSync function (the game's once-per-frame wait, in its main loop: gameplay, menus, movies, loading screens) when
 * the chain is the game's main thread only (no interrupt handler, no BIOS callback, no hook of the port between), and
 * a load rebuilds it from the bottom: VSync is entered at its start; when it returns (its 'jr ra' matches the shadow
 * stack), the caller is entered at the return site, and so on down to the entry point. Tail calls and computed jumps
 * left nothing to do in their C frames (the generated code returns right after them), so they need no frame.
 *
 * A load can come at any vblank: the C stack of the running game is left with a longjmp to state_boot, the state is
 * put in, and the chain is rebuilt from there. A save requested by a key waits for the next VSync entry.
 *
 * File: "FESTATE\0", format version, then sections (state.h). HEAD holds the game, the exe's hash, the generated
 * code's id, the port build, the disc, the frame, the time, the overlay in RAM and the call chain; a load checks
 * them, then reads every section once without changing anything, and only then for real. */
#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdlib.h>
#include <time.h>
#include <direct.h>
#include <sys/stat.h>
#include "hw.h"
#include "state.h"

#ifndef FE_BUILD_ID
#define FE_BUILD_ID "unknown"
#endif
#define FORMAT 1

uint32_t state_resume_ra;
int state_slot = 1;
int64_t state_loaded_frame = -1;

extern int hooks_native_depth;
extern const uint64_t recomp_exe_hash;
int menu_busy(void);
void osd_show(const char *text);
void frontend_state_loaded(const char *path); /* main.c: replay position, recording, audio queue, pacing */
extern int disc_current;
extern char disc_paths[4][1024];

/* ------------------------------------------------------------------ serialisation */

void st_fail(StIO *io, const char *fmt, ...) {
    if (io->err[0]) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(io->err, sizeof io->err, fmt, ap);
    va_end(ap);
}

static void put(StIO *io, const void *p, size_t n) {
    if (io->n + n > io->cap) {
        size_t c = io->cap ? io->cap * 2 : (8u << 20);
        while (c < io->n + n) c *= 2;
        uint8_t *b = realloc(io->buf, c);
        if (!b) { st_fail(io, "out of memory"); return; }
        io->buf = b; io->cap = c;
    }
    memcpy(io->buf + io->n, p, n);
    io->n += n;
}

static int get(StIO *io, void *p, size_t n, size_t limit) {
    if (io->pos + n > limit) return 0;
    if (p) memcpy(p, io->buf + io->pos, n);
    io->pos += n;
    return 1;
}

static uint32_t begin(StIO *io, const char *tag, uint32_t version, uint32_t min_version);
void st_begin(StIO *io, const char *tag, uint32_t version) { begin(io, tag, version, version); }
uint32_t st_begin_compat(StIO *io, const char *tag, uint32_t version, uint32_t min_version) {
    return begin(io, tag, version, min_version);
}

static uint32_t begin(StIO *io, const char *tag, uint32_t version, uint32_t min_version) {
    if (io->err[0]) return version;
    snprintf(io->sec, sizeof io->sec, "%s", tag);
    char t[8] = {0};
    strncpy(t, tag, 8);
    if (!io->load) {
        uint32_t z = 0;
        put(io, t, 8); put(io, &version, 4);
        io->sec_pos = io->n;
        put(io, &z, 4);
        return version;
    }
    char ft[8];
    uint32_t fv, size;
    if (!get(io, ft, 8, io->n) || !get(io, &fv, 4, io->n) || !get(io, &size, 4, io->n)) {
        st_fail(io, "the file ends before section %s", tag);
        return version;
    }
    if (memcmp(ft, t, 8)) { st_fail(io, "section %s expected, %.8s found (a state of another port build)", tag, ft); return version; }
    if (fv > version || fv < min_version) {
        st_fail(io, "section %s has version %u, this build reads %u (a state of another port build)", tag, fv, version);
        return version;
    }
    if (io->pos + size > io->n) { st_fail(io, "section %s is cut off", tag); return version; }
    io->sec_end = io->pos + size;
    return fv;
}

void st_end(StIO *io) {
    if (io->err[0]) return;
    if (!io->load) {
        uint32_t size = (uint32_t)(io->n - io->sec_pos - 4);
        memcpy(io->buf + io->sec_pos, &size, 4);
        return;
    }
    if (io->pos != io->sec_end)
        st_fail(io, "section %s has %lld bytes more than this build reads (a state of another port build)", io->sec,
                (long long)io->sec_end - (long long)io->pos);
}

void st_raw(StIO *io, void *p, size_t n) {
    if (io->err[0]) return;
    if (!io->load) { put(io, p, n); return; }
    if (!get(io, io->dry ? 0 : p, n, io->sec_end)) st_fail(io, "section %s is shorter than this build reads (a state of another port build)", io->sec);
}

int st_count(StIO *io, int live, int max) {
    int32_t n = live;
    if (!io->load) { put(io, &n, 4); return live; }
    if (io->err[0]) return 0;
    if (!get(io, &n, 4, io->sec_end)) { st_fail(io, "section %s is cut off", io->sec); return 0; }
    if (n < 0 || n > max) { st_fail(io, "section %s: a count of %d (at most %d)", io->sec, n, max); return 0; }
    return n;
}

/* ------------------------------------------------------------------ resume sites */

static uint32_t vsync_fn;      /* the game's VSync (libetc): FE2 800665CC, FE1 8006290C */
static uint32_t overlay_base;  /* where the overlays load (all share one address) */

static uint32_t ov_ident_now(void) {
    uint32_t w;
    memcpy(&w, &psx_ram[overlay_base & 0x1FFFFC], 4);
    return w;
}

/* the image whose code holds a: the main exe, else the overlay with that first word */
static int image_of(uint32_t a, uint32_t ident) {
    for (int i = 0; i < recomp_image_count; i++) {
        const ImageEntry *im = &recomp_images[i];
        if (a < im->code_lo || a >= im->code_hi) continue;
        if (!im->is_overlay || im->ident == ident) return i;
    }
    return -1;
}

/* the function (fn_ptr index) that can be entered at a: a function start or a call's return site */
static int site_fn(uint32_t a, uint32_t ident) {
    int im = image_of(a, ident);
    if (im < 0) return -1;
    const StateSite *s = state_sites[im];
    int lo = 0, hi = state_site_count[im] - 1;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        if (s[m].addr == a) return s[m].fn;
        if (s[m].addr < a) lo = m + 1; else hi = m - 1;
    }
    return -1;
}

void state_resume_bad(uint32_t a) { fatal("save state: %08X is not a resume point of its function", a); }

static void resume_at(uint32_t a) {
    int fn = site_fn(a, ov_ident_now());
    if (fn < 0) fatal("save state: no resume point at %08X (overlay word %08X)", a, ov_ident_now());
    state_resume_ra = a;
    fn_ptr[fn]();
    state_resume_ra = 0;
}

/* the guest call chain of the state just loaded, from its innermost frame (VSync at its start) outwards */
static void run_chain(void) {
    int n = rs_top;
    resume_at(vsync_fn);
    for (int k = n - 1; k >= 1; k--) {
        rs_top = k;
        resume_at(rs_stack[k]);
    }
    rs_top = 0;
}

static const char *overlay_name(uint32_t ident) {
    for (int i = 0; i < recomp_image_count; i++)
        if (recomp_images[i].is_overlay && recomp_images[i].ident == ident) return recomp_images[i].name;
    return "-";
}

/* ------------------------------------------------------------------ the file */

typedef struct {
    char game[8];
    uint64_t exe_hash, gen_id;
    char build[64];
    int32_t disc;
    char serial[16];
    uint64_t frame;
    int64_t saved_at;  /* time() */
    uint32_t ov_ident, pc;
    char where[64];
    int32_t depth;
    uint32_t chain[256];
} Head;

static void head_io(StIO *io, Head *h) {
    st_begin(io, "HEAD", 1);
    ST(io, h->game); ST(io, h->exe_hash); ST(io, h->gen_id); ST(io, h->build); ST(io, h->disc); ST(io, h->serial);
    ST(io, h->frame); ST(io, h->saved_at); ST(io, h->ov_ident); ST(io, h->pc); ST(io, h->where);
    h->depth = st_count(io, h->depth, 256);
    for (int i = 0; i < h->depth; i++) ST(io, h->chain[i]);
    st_end(io);
}

static void pad_state(StIO *io) {
    st_begin(io, "PAD", 1);
    ST(io, pad_buttons); ST(io, pad_axes); ST(io, pad_analog_toggle);
    st_end(io);
}

static void modules_io(StIO *io) {
    core_state(io);
    bios_state(io);
    gte_state(io);
    gpu_state(io);
    cdrom_state(io);
    mdec_state(io);
    spu_state(io);
    sio_state(io);
    mcd_state(io);
    controls_state(io);
    game_state(io);
    gopt_state(io);
    pad_state(io);
    models_state(io);
    texrep_state(io);
}

static int write_file(const char *path, const void *p, size_t n) {
    char tmp[1200];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return 0;
    int ok = fwrite(p, 1, n, f) == n;
    ok &= fclose(f) == 0;
    if (!ok) { remove(tmp); return 0; }
    remove(path);
    return rename(tmp, path) == 0;
}

/* save the state as it is now (at a VSync entry in the game's main thread, or right after a load) */
static int save_to(const char *path) {
    uint32_t t0 = SDL_GetTicks();
    StIO io;
    memset(&io, 0, sizeof io);
    put(&io, "FESTATE", 8);
    uint32_t fmt = FORMAT;
    put(&io, &fmt, 4);
    Head h;
    memset(&h, 0, sizeof h);
    snprintf(h.game, sizeof h.game, "%s", FE_GAME);
    h.exe_hash = recomp_exe_hash; h.gen_id = recomp_gen_id;
    snprintf(h.build, sizeof h.build, "%s", FE_BUILD_ID);
    h.disc = disc_current;
    snprintf(h.serial, sizeof h.serial, "%s", game_serial);
    h.frame = frame_count;
    h.saved_at = (int64_t)time(0);
    h.ov_ident = ov_ident_now();
    h.pc = vsync_fn;
    snprintf(h.where, sizeof h.where, "%s disc %d, %s, frame %llu", strcmp(FE_GAME, "fe1") ? "FE2" : "FE1", disc_current + 1,
             overlay_name(h.ov_ident), (unsigned long long)frame_count);
    h.depth = rs_top > 256 ? 256 : rs_top;
    memcpy(h.chain, rs_stack, sizeof(uint32_t) * (size_t)h.depth);
    head_io(&io, &h);
    modules_io(&io);
    int ok = !io.err[0] && write_file(path, io.buf, io.n);
    if (ok) LOG("state: saved %s (%s, chain depth %d, %.1f MB, %u ms)\n", path, h.where, h.depth, io.n / 1048576.0,
                SDL_GetTicks() - t0);
    else LOG("state: could not save %s%s%s\n", path, io.err[0] ? ": " : "", io.err);
    free(io.buf);
    return ok;
}

/* read and check a state file; apply = put it in (only when every check passed). Returns 1 if usable / done. */
static int load_from(const char *path, int apply, char *err, size_t errn) {
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(err, errn, "no state file %s", path); return 0; }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    StIO io;
    memset(&io, 0, sizeof io);
    io.load = 1;
    io.buf = size > 12 ? malloc((size_t)size) : 0;
    io.n = io.buf && fread(io.buf, 1, (size_t)size, f) == (size_t)size ? (size_t)size : 0;
    fclose(f);
    int ok = 0;
    if (io.n < 12 || memcmp(io.buf, "FESTATE", 8)) { snprintf(err, errn, "%s is not a save state", path); goto done; }
    uint32_t fmt;
    memcpy(&fmt, io.buf + 8, 4);
    if (fmt != FORMAT) { snprintf(err, errn, "state format %u, this build reads format %u", fmt, FORMAT); goto done; }
    Head h;
    memset(&h, 0, sizeof h);
    io.pos = 12;
    head_io(&io, &h);
    if (io.err[0]) { snprintf(err, errn, "%s", io.err); goto done; }
    if (strcmp(h.game, FE_GAME)) { snprintf(err, errn, "the state is from %s, this is %s", h.game, FE_GAME); goto done; }
    if (h.exe_hash != recomp_exe_hash) { snprintf(err, errn, "the state was made from another game exe"); goto done; }
    if (h.disc < 0 || h.disc > 3 || (h.disc != disc_current && !disc_paths[h.disc][0])) {
        snprintf(err, errn, "the state is on disc %d, whose image was not found", h.disc + 1);
        goto done;
    }
    if (h.pc != vsync_fn || h.depth < 2 || h.chain[0] != GUEST_RA) { snprintf(err, errn, "the state's call chain is not one this build can rebuild"); goto done; }
    for (int i = 1; i < h.depth; i++)
        if (site_fn(h.chain[i], h.ov_ident) < 0) {
            snprintf(err, errn, "the state's call chain (%08X, overlay %s) cannot be rebuilt: the game code was recompiled "
                     "differently (state of build %s)", h.chain[i], overlay_name(h.ov_ident), h.build);
            goto done;
        }
    if (h.gen_id != recomp_gen_id) LOG("state: %s was made by another recompile (build %s); its call chain fits\n", path, h.build);
    size_t body = io.pos;
    io.dry = 1;
    modules_io(&io);
    if (!io.err[0] && io.pos != io.n) st_fail(&io, "%lld bytes after the last section", (long long)(io.n - io.pos));
    if (io.err[0]) { snprintf(err, errn, "%s", io.err); goto done; }
    ok = 1;
    if (!apply) goto done;
    /* every check passed: put it in */
    if (h.disc != disc_current) {
        if (!cdrom_set_image(disc_paths[h.disc])) fatal("save state: cannot open disc %d (%s)", h.disc + 1, disc_paths[h.disc]);
        disc_current = h.disc;
        LOG("state: disc %d inserted\n", h.disc + 1);
    }
    io.dry = 0;
    io.pos = body;
    modules_io(&io);
    if (io.err[0]) fatal("save state: %s (after the checks)", io.err);
    if (h.serial[0]) snprintf(game_serial, sizeof game_serial, "%s", h.serial); /* the set's serial the game booted from */
    state_resume_ra = 0;
    hooks_state_loaded();
    bios_state_loaded();
    spu_state_loaded();
    hw_state_loaded();
    game_state_loaded();
    models_state_loaded();
    subs_state_loaded();
    gamefix_state_loaded();
    menu_state_loaded();
    state_loaded_frame = (int64_t)frame_count;
    frontend_state_loaded(path);
    LOG("state: loaded %s (%s, saved by build %s)\n", path, h.where, h.build);
done:
    free(io.buf);
    return ok;
}

/* ------------------------------------------------------------------ requests */

static jmp_buf top;
static int top_valid;
static char boot_load[1024];      /* --load-state */
static char want_load[1024];      /* a load at the next vblank */
static int want_save;             /* a key: save into slot want_save_slot at the next state point */
static int want_save_slot;
static int64_t want_save_since;
static struct { int64_t frame; char path[1024]; int done; } save_at[16]; /* --save-state-at */
static int nsave_at;
static char last_refusal[160];

static void slot_path(int slot, char *out, size_t n) {
    char *base = SDL_GetBasePath();
    char dir[1024];
    snprintf(dir, sizeof dir, "%sstates", base ? base : "");
    if (base) SDL_free(base);
    _mkdir(dir);
    snprintf(out, n, "%s/%s_slot%d.state", dir, FE_GAME, slot);
}

/* agent.c: a save at the next state point / a load at the next vblank, counted when done */
int state_agent_saves, state_agent_loads;
void state_request_save(const char *path) {
    int i;
    for (i = 0; i < nsave_at; i++) if (save_at[i].done) break;
    if (i == nsave_at) { if (nsave_at >= 16) return; nsave_at++; }
    save_at[i].frame = (int64_t)frame_count; save_at[i].done = 0;
    snprintf(save_at[i].path, sizeof save_at[i].path, "%s", path);
}
void state_request_load(const char *path) { snprintf(want_load, sizeof want_load, "%s", path); }

void state_set_load(const char *path) { snprintf(boot_load, sizeof boot_load, "%s", path); }

void state_add_save_at(int64_t frame, const char *path) {
    if (nsave_at >= 16) return;
    save_at[nsave_at].frame = frame;
    snprintf(save_at[nsave_at].path, sizeof save_at[nsave_at].path, "%s", path);
    nsave_at++;
}

static void notice(const char *fmt, ...) {
    char t[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(t, sizeof t, fmt, ap);
    va_end(ap);
    LOG("state: %s (frame %llu)\n", t, (unsigned long long)frame_count);
    osd_show(t);
}

static void slot_notice(void) {
    char p[1100];
    slot_path(state_slot, p, sizeof p);
    FILE *f = fopen(p, "rb");
    if (!f) { notice("State slot %d (empty)", state_slot); return; }
    fclose(f);
    char err[300];
    struct _stat64 st;
    char when[32] = "";
    if (!_stat64(p, &st)) strftime(when, sizeof when, "%d %b %H:%M", localtime(&st.st_mtime));
    if (load_from(p, 0, err, sizeof err)) notice("State slot %d (%s)", state_slot, when);
    else notice("State slot %d (%s, cannot be loaded)", state_slot, when);
}

void state_action(int which, int arg) {
    (void)arg;
    switch (which) {
    case 0:
        if (menu_busy()) { notice("Close the menu to save a state"); break; }
        want_save = 1; want_save_slot = state_slot; want_save_since = (int64_t)frame_count;
        break;
    case 1: {
        char p[1100];
        slot_path(state_slot, p, sizeof p);
        snprintf(want_load, sizeof want_load, "%s", p);
        break;
    }
    case 2: state_slot = state_slot % 9 + 1; slot_notice(); break;
    case 3: state_slot = (state_slot + 7) % 9 + 1; slot_notice(); break;
    }
}

/* can the stack be rebuilt from here (the entry of VSync)? */
static int context_ok(char *why, size_t n) {
    if (in_exception) { snprintf(why, n, "in an interrupt"); return 0; }
    if (hooks_native_depth) { snprintf(why, n, "inside a hook of the port"); return 0; }
    if (rs_top < 2 || rs_top > 256 || rs_stack[0] != GUEST_RA) { snprintf(why, n, "call chain depth %d", rs_top); return 0; }
    if (R31 != rs_stack[rs_top - 1]) { snprintf(why, n, "VSync not entered by a call"); return 0; }
    uint32_t id = ov_ident_now();
    for (int i = 1; i < rs_top; i++) {
        if (rs_stack[i] == GUEST_RA) { snprintf(why, n, "inside a callback of the BIOS"); return 0; }
        if (site_fn(rs_stack[i], id) < 0) { snprintf(why, n, "no resume point at %08X", rs_stack[i]); return 0; }
    }
    return 1;
}

static void take_saves(int at_load) {
    for (int i = 0; i < nsave_at; i++)
        if (!save_at[i].done && (int64_t)frame_count >= save_at[i].frame) {
            save_at[i].done = 1;
            if (save_to(save_at[i].path) && ++state_agent_saves)
                LOG("state: --save-state-at %lld: saved at frame %llu%s\n", (long long)save_at[i].frame,
                    (unsigned long long)frame_count, at_load ? " (right after the load)" : "");
        }
    if (want_save && !at_load) {
        char p[1100];
        slot_path(want_save_slot, p, sizeof p);
        want_save = 0;
        if (save_to(p)) notice("State %d saved", want_save_slot);
        else notice("State %d: could not be saved", want_save_slot);
    }
}

/* the entry of VSync (hook point, the game's main thread or not) */
static int vsync_hook(void) {
    int pending = want_save;
    for (int i = 0; i < nsave_at; i++) pending |= !save_at[i].done && (int64_t)frame_count >= save_at[i].frame;
    if (!pending) return 0;
    char why[160];
    if (!context_ok(why, sizeof why)) {
        if (strcmp(why, last_refusal)) {
            LOG("state: no state point at frame %llu: %s\n", (unsigned long long)frame_count, why);
            snprintf(last_refusal, sizeof last_refusal, "%s", why);
        }
        return 0;
    }
    last_refusal[0] = 0;
    take_saves(0);
    return 0;
}

void state_vblank(void) {
    static int64_t load_at = -2;
    static char load_at_path[1024];
    if (load_at == -2) { /* tests: FE_STATE_LOAD_AT=FRAME:FILE loads at that vblank of a running game, as the key does */
        const char *e = getenv("FE_STATE_LOAD_AT");
        char *c = 0;
        load_at = e ? strtoll(e, &c, 10) : -1;
        if (e && c && *c == ':') snprintf(load_at_path, sizeof load_at_path, "%s", c + 1);
        else load_at = -1;
    }
    if (load_at >= 0 && (int64_t)frame_count == load_at) {
        load_at = -1;
        snprintf(want_load, sizeof want_load, "%s", load_at_path);
    }
    if (want_save && (int64_t)frame_count - want_save_since > 300) {
        want_save = 0;
        notice("No state point here: the state was not saved");
    }
    if (want_load[0]) {
        char p[1100], err[300];
        snprintf(p, sizeof p, "%s", want_load);
        want_load[0] = 0;
        if (!top_valid) return;
        if (!load_from(p, 0, err, sizeof err)) {
            FILE *f = fopen(p, "rb");
            if (!f) notice("State slot %d is empty", state_slot);
            else { fclose(f); notice("State cannot be loaded: %s", err); }
            return;
        }
        load_from(p, 1, err, sizeof err);
        state_agent_loads++;
        notice("State %d loaded", state_slot);
        longjmp(top, 1); /* leave the running game's C frames; state_boot rebuilds the chain */
    }
    const char *e = getenv("FE_STATE_HASH"); /* determinism checks: hashes of RAM, VRAM and sound RAM per vblank */
    if (e && (int64_t)frame_count >= atoll(e)) {
        uint64_t h = 1469598103934665603ULL, hv = h;
        for (int i = 0; i < PSX_RAM_SIZE; i += 8) { uint64_t w; memcpy(&w, psx_ram + i, 8); h = (h ^ w) * 1099511628211ULL; }
        for (int i = 0; i < 1024 * 512; i += 4) { uint64_t w; memcpy(&w, vram + i, 8); hv = (hv ^ w) * 1099511628211ULL; }
        LOG("hash %llu ram %016llX vram %016llX cyc %lld\n", (unsigned long long)frame_count, (unsigned long long)h,
            (unsigned long long)hv, (long long)cpu.cycles);
    }
}

void state_init(void) {
    vsync_fn = strcmp(FE_GAME, "fe1") ? 0x800665CCu : 0x8006290Cu;
    for (int i = 0; i < recomp_image_count; i++)
        if (recomp_images[i].is_overlay) { overlay_base = recomp_images[i].base; break; }
    if (hook_index(vsync_fn) < 0) { LOG("state: no VSync function at %08X: save states off\n", vsync_fn); vsync_fn = 0; return; }
    hook_register(vsync_fn, vsync_hook);
}

void state_boot(uint32_t entry) {
    if (setjmp(top)) { /* a load at a vblank: the state is in */
        take_saves(1);
        run_chain();
        return;
    }
    top_valid = vsync_fn != 0;
    if (boot_load[0]) {
        char err[300];
        if (!vsync_fn || !load_from(boot_load, 1, err, sizeof err)) {
            if (!vsync_fn) snprintf(err, sizeof err, "save states are off");
            LOG("state: --load-state %s: %s\n", boot_load, err);
            fprintf(stderr, "FATAL: --load-state %s: %s\n", boot_load, err);
            if (!opt_headless) SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, FE_GAME "port", err, 0);
            exit(1);
        }
        take_saves(1);
        run_chain();
        return;
    }
    call_guest(entry);
}
