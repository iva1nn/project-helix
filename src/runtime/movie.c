/* Replacement movie sets (the remaster's 8x mp4 files): movies.txt + <name>.idx per movie.
 * movies.txt lines: name first_sector last_sector origin file.mp4 [rate] (file sectors of the disc image);
 * <name>.idx: little-endian u32 per STR frame, the last disc sector of that STR frame.
 * rate (optional, default 1): pictures per STR frame in the mp4 (4 = a 60 fps file for a 15 fps STR movie, the
 * in-between pictures interpolated offline; NOTES.md "60 fps room movies"). Decoder frame numbers below are picture
 * numbers: STR frame f, sub-step k = picture f * rate + k (game_fe2.c picks k from game time).
 * Frames are decoded by an ffmpeg process (rgb24 on a pipe) in a background thread that keeps a small
 * cache ahead of the game; a backward or far jump restarts ffmpeg at that frame (-ss is exact), normally by
 * handing the job to a standby ffmpeg started in advance. */
#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>
#include <stdlib.h>
#include <fcntl.h>
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#define FFMPEG_EXE "ffmpeg.exe"
#else /* Linux / SteamOS: posix_spawn with pipes; a HANDLE is a pid (process) or a file descriptor (stdin) */
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
typedef intptr_t HANDLE;
extern char **environ;
#define FFMPEG_EXE "ffmpeg"
#endif
#include "hw.h"

typedef struct {
    char name[64], path[1024];
    uint32_t lba0, lba1;
    uint32_t *idx;
    int n;      /* STR frames (the .idx) */
    int rate;   /* pictures per STR frame */
    int w, h;
} Movie;

static Movie movies[64];
static int nmovies;

/* the decoder's output size: the 8x movies (2560x1280 = 8x of 320x160) as large as the upscaled VRAM holds them,
 * 320 x scale by 160 x scale. At 4x (the Steam Deck) the full-size picture was decoded, converted and piped only to
 * be shrunk to a quarter on upload; ffmpeg scaling it while converting is ~2x faster and loses nothing on screen
 * (14 Nov 2026). FE_MOVIE_FULL=1 keeps the full size. */
static int out_w(void) { extern int hw_scale; return getenv("FE_MOVIE_FULL") || hw_scale >= 8 ? 2560 : 320 * (hw_scale < 1 ? 1 : hw_scale); }
static int out_h(void) { return out_w() / 2; }

/* ffmpeg's threading: frame threads, half the CPU's threads (2-8). Slice threads (8, before 14 Nov 2026) gave the
 * Steam Deck 57-62 pictures a second against a 60 fps movie's 60, and once behind the decoder stayed behind: every
 * other picture of the A01 pull-back late, the background stepping during the camera move. Frame threads: 84-97 a
 * second, caught up after the cut in every run. FE_MOVIE_THREADS / FE_MOVIE_THREAD_TYPE override (tests). */
static const char *dec_threads(void) {
    static char s[64];
    const char *n = getenv("FE_MOVIE_THREADS"), *t = getenv("FE_MOVIE_THREAD_TYPE");
    int c = SDL_GetCPUCount() / 2;
    c = c < 2 ? 2 : c > 8 ? 8 : c;
    if (n) c = atoi(n);
    snprintf(s, sizeof s, "-threads %d -thread_type %s", c, t ? t : "frame");
    return s;
}
static const char *dec_scale(int lead_comma) {
    static char s[2][64];
    if (out_w() >= 2560) return "";
    snprintf(s[lead_comma], sizeof s[0], "%sscale=%d:%d:flags=bilinear", lead_comma ? "," : "-vf ", out_w(), out_h());
    return s[lead_comma];
}

int movies_load(const char *dir) {
    for (int i = 0; i < nmovies; i++) free(movies[i].idx);
    nmovies = 0;
    char p[1200];
    snprintf(p, sizeof p, "%s/movies.txt", dir);
    FILE *f = fopen(p, "r");
    if (!f) { LOG("movies: no movies.txt in %s\n", dir); return 0; }
    char line[512];
    while (fgets(line, sizeof line, f) && nmovies < 64) {
        Movie *m = &movies[nmovies];
        char file[256];
        unsigned a, b, origin;
        int rate = 1;
        if (line[0] == '#' || sscanf(line, "%63s %u %u %u %255s %d", m->name, &a, &b, &origin, file, &rate) < 5) continue;
        m->lba0 = a; m->lba1 = b;
        m->rate = rate >= 1 && rate <= 8 ? rate : 1;
        snprintf(m->path, sizeof m->path, "%s/%s", dir, file);
        snprintf(p, sizeof p, "%s/%s.idx", dir, m->name);
        FILE *fi = fopen(p, "rb");
        if (!fi) continue;
        fseek(fi, 0, SEEK_END);
        long sz = ftell(fi);
        fseek(fi, 0, SEEK_SET);
        m->idx = malloc((size_t)sz);
        m->n = (int)(fread(m->idx, 4, (size_t)sz / 4, fi));
        fclose(fi);
        m->w = out_w(); m->h = out_h(); /* 8x of 320x160, or scaled down to the VRAM's scale (out_w) */
        nmovies++;
    }
    fclose(f);
    for (int i = 0; i < nmovies; i++)
        if (movies[i].rate > 1) LOG("movies: %s has %d pictures per STR frame (%d fps)\n", movies[i].name, movies[i].rate, 15 * movies[i].rate);
    LOG("movies: %d replacement movies in %s\n", nmovies, dir);
    return nmovies;
}

/* movie and frame for a frame whose first sector is lba (file sector) */
int movie_lookup(uint32_t lba, int *frame) {
    for (int i = 0; i < nmovies; i++) {
        Movie *m = &movies[i];
        if (lba < m->lba0 || lba >= m->lba1 || !m->n) continue;
        int lo = 0, hi = m->n; /* lower_bound: first frame whose last sector >= lba */
        while (lo < hi) { int mid = (lo + hi) / 2; if (m->idx[mid] < lba) lo = mid + 1; else hi = mid; }
        if (lo >= m->n) continue;
        *frame = lo;
        return i;
    }
    return -1;
}

/* ------------------------------------------------------------------ decoder thread */

/* Frame cache: least-recently-used, large enough to hold a whole room loop, so a loop restart
 * (the game seeks back to the loop start) plays from memory. FE_MOVIE_CACHE = number of frames. */
#define MAXCACHE 512
static int cache_n = 96;
static struct { int movie, frame; uint8_t *rgb; uint64_t used; } cache[MAXCACHE];
static uint64_t use_clock;
static SDL_mutex *mu;
static SDL_cond *cv;
static SDL_Thread *thr;
static int want_movie = -1, want_frame;
static int dec_movie = -1, dec_next; /* what the pipe will deliver next */
static int dec_first; static uint32_t dec_t0;
static FILE *pipe_f;
static HANDLE pipe_proc; /* the ffmpeg process writing pipe_f */
#define AHEAD 12

/* Standby decoder: an ffmpeg already started and waiting for its job (a concat list) on stdin. Starting
 * ffmpeg.exe costs ~300 ms whenever the last start was more than a few seconds ago (the virus scanner
 * rereads the 200 MB exe), which held the picture at room changes; the standby pays that in advance. */
static SDL_Thread *sb_thr;
static SDL_cond *sb_cv;
static FILE *sb_out;
static HANDLE sb_proc, sb_in;
static int sb_spawning;
static SDL_mutex *spawn_mu; /* one spawn at a time: a child inherits every inheritable handle open at that moment,
                             * and a stray copy of another decoder's pipe end would hide that pipe's end of file */
static char ffmpeg[1100];

/* ffmpeg started directly (no cmd.exe in between) with its stdout on a pipe; ended at once when not needed
 * (a room change must not wait for the old decoder to finish its frame). in: stdin on a pipe too. */
#ifdef _WIN32
static FILE *spawn_decoder(const char *cmdline, HANDLE *proc, HANDLE *in) {
    SECURITY_ATTRIBUTES sa = {sizeof sa, 0, TRUE};
    HANDLE rd, wr, ird = INVALID_HANDLE_VALUE, iwr = 0;
    SDL_LockMutex(spawn_mu);
    if (!CreatePipe(&rd, &wr, &sa, 1 << 22)) { SDL_UnlockMutex(spawn_mu); return 0; }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    if (in) {
        if (!CreatePipe(&ird, &iwr, &sa, 0)) { CloseHandle(rd); CloseHandle(wr); SDL_UnlockMutex(spawn_mu); return 0; }
        SetHandleInformation(iwr, HANDLE_FLAG_INHERIT, 0);
    } else
        ird = CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ, &sa, OPEN_EXISTING, 0, 0);
    HANDLE nul = CreateFileA("NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, 0);
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = ird; si.hStdOutput = wr; si.hStdError = nul;
    char cl[2600];
    snprintf(cl, sizeof cl, "%s", cmdline);
    BOOL ok = CreateProcessA(0, cl, 0, 0, TRUE, CREATE_NO_WINDOW, 0, 0, &si, &pi);
    CloseHandle(wr);
    if (ird != INVALID_HANDLE_VALUE) CloseHandle(ird);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    SDL_UnlockMutex(spawn_mu);
    if (!ok) { CloseHandle(rd); if (iwr) CloseHandle(iwr); return 0; }
    CloseHandle(pi.hThread);
    *proc = pi.hProcess;
    if (in) *in = iwr;
    int fd = _open_osfhandle((intptr_t)rd, _O_RDONLY | _O_BINARY);
    return fd >= 0 ? _fdopen(fd, "rb") : 0;
}

static void end_decoder(FILE *f, HANDLE proc) {
    if (proc) { TerminateProcess(proc, 0); CloseHandle(proc); }
    if (f) fclose(f);
}

static int in_write(HANDLE in, const char *s) {
    DWORD wrote;
    return WriteFile(in, s, (DWORD)strlen(s), &wrote, 0) != 0;
}
static void in_close(HANDLE in) { CloseHandle(in); }
#else
/* the command line as the Windows path builds it: words, "quoted words" (quotes removed), nothing else escaped */
static int split_args(char *s, char **argv, int max) {
    int n = 0;
    while (*s && n < max - 1) {
        while (*s == ' ') s++;
        if (!*s) break;
        if (*s == '"') {
            argv[n++] = ++s;
            while (*s && *s != '"') s++;
        } else {
            argv[n++] = s;
            while (*s && *s != ' ') s++;
        }
        if (*s) *s++ = 0;
    }
    argv[n] = 0;
    return n;
}

static FILE *spawn_decoder(const char *cmdline, HANDLE *proc, HANDLE *in) {
    static int sigpipe_off;
    if (!sigpipe_off) { signal(SIGPIPE, SIG_IGN); sigpipe_off = 1; } /* a killed decoder's stdin must not end us */
    char cl[2600], *argv[64];
    snprintf(cl, sizeof cl, "%s", cmdline);
    if (!split_args(cl, argv, 64)) return 0;
    int out[2], inp[2] = {-1, -1};
    SDL_LockMutex(spawn_mu);
    if (pipe(out)) { SDL_UnlockMutex(spawn_mu); return 0; }
    if (in && pipe(inp)) { close(out[0]); close(out[1]); SDL_UnlockMutex(spawn_mu); return 0; }
    fcntl(out[0], F_SETFD, FD_CLOEXEC);
    if (in) fcntl(inp[1], F_SETFD, FD_CLOEXEC);
#ifdef F_SETPIPE_SZ
    fcntl(out[0], F_SETPIPE_SZ, 1 << 20); /* a few frames in flight, as the Windows pipe's 4 MB buffer */
#endif
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    if (in) posix_spawn_file_actions_adddup2(&fa, inp[0], 0);
    else posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&fa, out[1], 1);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    pid_t pid;
    int err = posix_spawnp(&pid, argv[0], &fa, 0, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    close(out[1]);
    if (in) close(inp[0]);
    SDL_UnlockMutex(spawn_mu);
    if (err) { close(out[0]); if (in) close(inp[1]); return 0; }
    *proc = pid;
    if (in) *in = inp[1];
    return fdopen(out[0], "rb");
}

static void end_decoder(FILE *f, HANDLE proc) {
    if (proc) { kill((pid_t)proc, SIGKILL); waitpid((pid_t)proc, 0, 0); }
    if (f) fclose(f);
}

static int in_write(HANDLE in, const char *s) {
    size_t n = strlen(s);
    while (n) {
        ssize_t w = write((int)in, s, n);
        if (w <= 0) return 0;
        s += w; n -= (size_t)w;
    }
    return 1;
}
static void in_close(HANDLE in) { close((int)in); }
#endif

/* frames decoded ahead of the wanted one: 12 STR frames' worth, at most half the cache (a 60 fps movie: 48) */
static int ahead_of(int m) {
    int a = AHEAD * movies[m].rate;
    return a > cache_n / 2 ? cache_n / 2 : a;
}

static int cached(int m, int f) {
    for (int i = 0; i < cache_n; i++) if (cache[i].movie == m && cache[i].frame == f && cache[i].rgb) return i;
    return -1;
}

static int decoder_main(void *arg) {
    (void)arg;
    SDL_LockMutex(mu);
    for (;;) {
        while (want_movie < 0) SDL_CondWait(cv, mu);
        int m = want_movie, target = want_frame;
        int npics = movies[m].n * movies[m].rate, ahead = ahead_of(m);
        /* first frame from the target on that is not cached yet */
        int need = target;
        while (need < target + ahead && need < npics && cached(m, need) >= 0) need++;
        if (need >= target + ahead || need >= npics) { SDL_CondWaitTimeout(cv, mu, 20); continue; }
        if (dec_movie != m || !pipe_f || dec_next > need || dec_next < need - 20) {
            FILE *old = pipe_f;
            HANDLE oldp = pipe_proc;
            pipe_f = 0; pipe_proc = 0;
            /* half a picture before it: -ss / inpoint T start at the first picture whose time is >= T, and picture
             * need's time is exactly need / fps (an earlier + 1 ms started one picture late: every picture after a
             * decoder start was the next STR frame's; FE_MOVIE_SEEK_OLD=1 for comparison) */
            double fps = 15.0 * movies[m].rate;
            double ss = getenv("FE_MOVIE_SEEK_OLD") ? need / fps + 0.001 : need > 0 ? (need - 0.5) / fps : 0.0;
            while (!sb_out && sb_spawning) SDL_CondWait(cv, mu); /* sooner than a start of its own */
            FILE *nf = sb_out;
            HANDLE np = sb_proc, nin = sb_in;
            sb_out = 0; sb_proc = sb_in = 0;
            if (nf) SDL_CondSignal(sb_cv);
            char cmd[2600];
            if (nf) { /* concat list; the path inside '' is taken literally, only ' needs escaping. Linux: "file:" in
                       * front, else the list's own URL (pipe:0) is put before the absolute path ("pipe:/home/..."); a
                       * Windows path's drive letter already reads as an absolute URL */
#ifdef _WIN32
                int n = snprintf(cmd, sizeof cmd, "ffconcat version 1.0\nfile '");
#else
                int n = snprintf(cmd, sizeof cmd, "ffconcat version 1.0\nfile 'file:");
#endif
                for (const char *p = movies[m].path; *p && n < (int)sizeof cmd - 64; p++)
                    n += *p == '\'' ? snprintf(cmd + n, sizeof cmd - n, "'\\''") : (cmd[n] = *p == '\\' ? '/' : *p, 1);
                snprintf(cmd + n, sizeof cmd - n, "'\ninpoint %.4f\n", ss);
            } else
                snprintf(cmd, sizeof cmd, "\"%s\" -v error -nostdin %s -ss %.4f -i \"%s\" %s -f rawvideo -pix_fmt bgra -",
                         ffmpeg, dec_threads(), ss, movies[m].path, dec_scale(0));
            ftrace("exec", ffmpeg);
            ftrace("ffmpeg-input", movies[m].path);
            /* unlocked: starting a process takes long enough (100-300 ms seen) to stall the game's frame, which
             * waits on this lock for its picture (room changes: picture and sound froze) */
            uint32_t t0 = SDL_GetTicks();
            SDL_UnlockMutex(mu);
            if (old) end_decoder(old, oldp);
            if (nf) {
                if (!in_write(nin, cmd)) { end_decoder(nf, np); nf = 0; }
                in_close(nin);
            }
            int warm = nf != 0;
            if (!nf) nf = spawn_decoder(cmd, &np, 0);
            SDL_LockMutex(mu);
            if (getenv("FE_DBG_MOVIE"))
                LOG("movies: decoder %s at %s frame %d in %u ms\n", warm ? "standby" : "started", movies[m].name, need,
                    SDL_GetTicks() - t0);
            pipe_f = nf; pipe_proc = np;
            dec_movie = m; dec_next = need;
            dec_first = need; dec_t0 = t0;
            if (!pipe_f) { LOG("movies: cannot start ffmpeg\n"); want_movie = -1; continue; }
        }
        int f = dec_next;
        size_t bytes = (size_t)movies[m].w * movies[m].h * 4;
        /* slot: an empty one, else the least recently used one outside the wanted window */
        int slot = -1;
        for (int i = 0; i < cache_n; i++)
            if (!cache[i].rgb) { slot = i; break; }
        if (slot < 0)
            for (int i = 0; i < cache_n; i++) {
                /* also the 3 STR frames before the wanted one: the game uploads up to 2 frames ahead of the one on screen,
                 * and a 60 fps movie's in-between pictures of that one are read only when shown (the oldest in LRU terms) */
                if (cache[i].movie == m && cache[i].frame >= target - 3 * movies[m].rate && cache[i].frame < target + ahead) continue;
                if (slot < 0 || cache[i].used < cache[slot].used) slot = i;
            }
        if (slot < 0) { SDL_CondWaitTimeout(cv, mu, 20); continue; }
        uint8_t *buf = cache[slot].rgb ? cache[slot].rgb : malloc(bytes);
        cache[slot].rgb = 0;
        cache[slot].movie = -1;
        FILE *pf = pipe_f;
        SDL_UnlockMutex(mu);
        uint64_t t_read = SDL_GetPerformanceCounter();
        size_t got = fread(buf, 1, bytes, pf);
        uint64_t t_lock = SDL_GetPerformanceCounter();
        SDL_LockMutex(mu);
        { /* FE_PERF: the decoder's rate (a 60 fps movie needs 60 pictures a second), time waiting for ffmpeg / the lock */
            static int on = -1, n;
            static double rd, lk;
            static uint32_t t0;
            if (on < 0) on = getenv("FE_PERF") != 0;
            if (on) {
                double fq = (double)SDL_GetPerformanceFrequency(), now = (double)SDL_GetPerformanceCounter();
                rd += ((double)t_lock - (double)t_read) * 1000.0 / fq;
                lk += (now - (double)t_lock) * 1000.0 / fq;
                if (!t0) t0 = SDL_GetTicks();
                if (++n >= 60) {
                    uint32_t dt = SDL_GetTicks() - t0;
                    LOG("perf: decoder %d pictures in %u ms (%.0f/s), %s at %d, wanted %d; reading %.0f ms, lock %.0f ms\n", n, dt,
                        n * 1000.0 / (dt ? dt : 1), movies[m].name, f, target, rd, lk);
                    n = 0; rd = lk = 0; t0 = SDL_GetTicks();
                }
            }
        }
        cache[slot].rgb = buf;
        if (got != bytes) { /* end of file or the pipe was replaced */
            if (pipe_f == pf) {
                HANDLE pp = pipe_proc;
                pipe_f = 0; pipe_proc = 0;
                SDL_UnlockMutex(mu); end_decoder(pf, pp); SDL_LockMutex(mu);
            }
            dec_movie = -1;
            if (want_movie == m && want_frame == target) want_movie = -1;
            continue;
        }
        if (pipe_f != pf || dec_movie != m) continue; /* restarted meanwhile: data belongs to the old stream */
        cache[slot].movie = m; cache[slot].frame = f; cache[slot].used = ++use_clock;
        if (getenv("FE_DBG_MOVIE") && f == dec_first) LOG("movies: first frame %d %u ms after the start\n", f, SDL_GetTicks() - dec_t0);
        dec_next = f + 1;
        SDL_CondBroadcast(cv);
    }
    return 0;
}

/* keeps one standby decoder ready; its frames are those of "-ss inpoint": -copyts keeps the file's times
 * (minus the inpoint), and the frames before the inpoint that concat passes on (back to the keyframe) are dropped */
static int standby_main(void *arg) {
    (void)arg;
    char cmd[2600];
    snprintf(cmd, sizeof cmd, "\"%s\" -v error -nostdin %s -protocol_whitelist file,pipe -copyts -f concat -safe 0 "
             "-i pipe:0 -vf select=gte(t\\,0)%s -f rawvideo -pix_fmt bgra -", ffmpeg, dec_threads(), dec_scale(1));
    SDL_LockMutex(mu);
    for (;;) {
        while (sb_out) SDL_CondWait(sb_cv, mu);
        sb_spawning = 1;
        SDL_UnlockMutex(mu);
        HANDLE p = 0, in = 0;
        uint32_t t0 = SDL_GetTicks();
        FILE *f = spawn_decoder(cmd, &p, &in);
        SDL_LockMutex(mu);
        sb_spawning = 0;
        SDL_CondBroadcast(cv);
        if (!f) { LOG("movies: cannot start a standby ffmpeg\n"); break; }
        if (getenv("FE_DBG_MOVIE")) LOG("movies: standby ready in %u ms\n", SDL_GetTicks() - t0);
        sb_out = f; sb_proc = p; sb_in = in;
    }
    SDL_UnlockMutex(mu);
    return 0;
}

static void start_decoder(void) {
    if (mu) return;
    const char *e = getenv("FE_MOVIE_CACHE");
    if (e) { cache_n = atoi(e); if (cache_n < 16) cache_n = 16; if (cache_n > MAXCACHE) cache_n = MAXCACHE; }
    /* ffmpeg.exe in the data folder, else next to the exe (older packages), else the one on PATH */
    char *base = SDL_GetBasePath();
    FILE *t = 0;
    if (fe_data[0]) {
        snprintf(ffmpeg, sizeof ffmpeg, "%s/" FFMPEG_EXE, fe_data);
        t = fopen(ffmpeg, "rb");
    }
    if (!t) {
        snprintf(ffmpeg, sizeof ffmpeg, "%s" FFMPEG_EXE, base ? base : "");
        t = fopen(ffmpeg, "rb");
    }
    if (base) SDL_free(base);
    if (t) fclose(t);
    else snprintf(ffmpeg, sizeof ffmpeg, "ffmpeg");
    LOG("movies: decoder %s, pictures %dx%d, %s\n", ffmpeg, out_w(), out_h(), dec_threads());
    mu = SDL_CreateMutex(); cv = SDL_CreateCond(); sb_cv = SDL_CreateCond(); spawn_mu = SDL_CreateMutex();
    thr = SDL_CreateThread(decoder_main, "movies", 0);
    if (!getenv("FE_MOVIE_NO_STANDBY")) sb_thr = SDL_CreateThread(standby_main, "movie-standby", 0);
}

/* the game seeks the CD to this frame (a room change, a cut): start decoding there now, several frames
 * before the game shows it */
void movie_prefetch(int m, int frame) {
    if (m < 0 || m >= nmovies) return;
    start_decoder();
    SDL_LockMutex(mu);
    want_movie = m; want_frame = frame;
    SDL_CondBroadcast(cv);
    SDL_UnlockMutex(mu);
}

/* BGRA frame (movies[m].w x h x 4) or NULL if it is not ready within wait_ms.
 * A non-NULL result stays valid until movie_release() (the decoder waits). */
const uint8_t *movie_frame(int m, int frame, int wait_ms, int *w, int *h) {
    start_decoder();
    SDL_LockMutex(mu);
    want_movie = m; want_frame = frame;
    SDL_CondBroadcast(cv);
    int i;
    uint32_t t0 = SDL_GetTicks();
    while ((i = cached(m, frame)) < 0 && (int)(SDL_GetTicks() - t0) < wait_ms) SDL_CondWaitTimeout(cv, mu, 1);
    const uint8_t *r = i >= 0 ? cache[i].rgb : 0;
    if (r) cache[i].used = ++use_clock;
    *w = movies[m].w; *h = movies[m].h;
    if (!r) SDL_UnlockMutex(mu);
    return r;
}

void movie_release(void) { SDL_UnlockMutex(mu); }

/* movie_frame without waiting and without moving the decoder's target */
const uint8_t *movie_peek(int m, int frame, int *w, int *h) {
    if (!mu || m < 0 || m >= nmovies) return 0;
    SDL_LockMutex(mu);
    int i = cached(m, frame);
    if (i < 0 && getenv("FE_DBG_HFR")) { /* what the cache holds of this movie */
        int lo = 1 << 30, hi = -1, n = 0, other = 0;
        for (int k = 0; k < cache_n; k++)
            if (cache[k].rgb && cache[k].movie == m) { n++; if (cache[k].frame < lo) lo = cache[k].frame; if (cache[k].frame > hi) hi = cache[k].frame; }
            else if (cache[k].rgb) other++;
        LOG("movies: peek %s %d missing; cache: %d of it (%d-%d), %d other, wanted %d, decoder at %d\n", movies[m].name, frame, n, lo, hi,
            other, want_frame, dec_next);
    }
    if (i < 0) { SDL_UnlockMutex(mu); return 0; }
    cache[i].used = ++use_clock;
    *w = movies[m].w; *h = movies[m].h;
    return cache[i].rgb;
}

/* a room loop's head (NOTES.md "Room loops"): its cached frames count as just used, so the loop restarts from memory */
void movie_keep(int m, int f0, int n) {
    if (!mu || m < 0 || m >= nmovies) return;
    SDL_LockMutex(mu);
    for (int i = 0; i < cache_n; i++)
        if (cache[i].rgb && cache[i].movie == m && cache[i].frame >= f0 && cache[i].frame < f0 + n) cache[i].used = ++use_clock;
    SDL_UnlockMutex(mu);
}

const char *movie_name(int m) { return m >= 0 && m < nmovies ? movies[m].name : "?"; }
int movie_rate(int m) { return m >= 0 && m < nmovies ? movies[m].rate : 1; }
