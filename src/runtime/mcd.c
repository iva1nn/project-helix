/* Memory card: a standard 128 KB raw card image (.mcd/.mcr, same layout as DuckStation and ePSXe
 * cards) with the directory/file logic the PS1 BIOS offers on "bu00:". Slot 2 is empty. */
#include <stdlib.h>
#include <ctype.h>
#include "hw.h"

#define CARD_SIZE 0x20000
#define BLOCK 0x2000
#define FRAME 128

static uint8_t card[CARD_SIZE];
static char card_path[1024];
static int card_present;
int mcd_new_card; /* the card was just created/formatted: the BIOS reports "new card" once */

static uint8_t *frame(int n) { return &card[n * FRAME]; }
static uint8_t *dirent(int block) { return frame(block); } /* directory frames 1..15 */

static void fix_xor(uint8_t *f) {
    uint8_t x = 0;
    for (int i = 0; i < 127; i++) x ^= f[i];
    f[127] = x;
}

static void save_card(void) {
    if (!card_path[0]) return;
    FILE *f = fopen(card_path, "wb");
    if (!f) { LOG("memory card: cannot write %s\n", card_path); return; }
    fwrite(card, 1, CARD_SIZE, f);
    fclose(f);
}

static void format_card(void) {
    memset(card, 0, CARD_SIZE);
    uint8_t *h = frame(0);
    h[0] = 'M'; h[1] = 'C';
    fix_xor(h);
    for (int b = 1; b < 16; b++) {
        uint8_t *d = dirent(b);
        d[0] = 0xA0;
        d[8] = 0xFF; d[9] = 0xFF;
        fix_xor(d);
    }
    for (int i = 16; i < 36; i++) {
        uint8_t *f = frame(i);
        f[0] = f[1] = f[2] = f[3] = 0xFF;
        f[8] = f[9] = 0xFF;
        fix_xor(f);
    }
    memcpy(frame(63), frame(0), FRAME);
    mcd_new_card = 1;
}

void mcd_init(const char *path) {
    snprintf(card_path, sizeof card_path, "%s", path);
    card_present = 1;
    FILE *f = fopen(path, "rb");
    if (f) {
        size_t n = fread(card, 1, CARD_SIZE, f);
        fclose(f);
        if (n == CARD_SIZE && card[0] == 'M' && card[1] == 'C') { LOG("memory card: %s\n", path); return; }
        LOG("memory card: %s is not a 128 KB card image, formatting a new one\n", path);
    } else {
        LOG("memory card: creating %s\n", path);
    }
    format_card();
    save_card();
}

int mcd_present(int port) { return card_present && port == 0; }

/* ------------------------------------------------------------------ directory */

static int entry_state(int b) { return dirent(b)[0]; }
static int is_first(int b) { return entry_state(b) == 0x51; }
static int is_free(int b) { int s = entry_state(b); return s == 0xA0 || s == 0xA1 || s == 0xA2 || s == 0xA3 || s == 0; }
static int next_block(int b) { int n = dirent(b)[8] | (dirent(b)[9] << 8); return n == 0xFFFF ? -1 : n + 1; }
static uint32_t file_size(int b) { uint32_t s; memcpy(&s, dirent(b) + 4, 4); return s; }

/* "bu00:NAME" -> port 0, NAME */
static int parse(const char *path, int *port, const char **name) {
    if (!path || strncmp(path, "bu", 2)) return 0;
    *port = (path[2] - '0') * 16 + (path[3] - '0');
    const char *c = strchr(path, ':');
    *name = c ? c + 1 : path + 4;
    return 1;
}

static int find_file(const char *name) {
    for (int b = 1; b < 16; b++)
        if (is_first(b) && !strncmp((const char *)dirent(b) + 10, name, 20)) return b;
    return -1;
}

static int wild_match(const char *pat, const char *s) {
    for (; *pat; pat++, s++) {
        if (*pat == '*') return 1;
        if (!*s) return 0;
        if (*pat != '?' && toupper((unsigned char)*pat) != toupper((unsigned char)*s)) return 0;
    }
    return *s == 0;
}

static int create_file(const char *name, int blocks) {
    if (blocks < 1) blocks = 1;
    int list[15], n = 0;
    for (int b = 1; b < 16 && n < blocks; b++) if (is_free(b)) list[n++] = b;
    if (n < blocks) return -1;
    for (int i = 0; i < n; i++) {
        uint8_t *d = dirent(list[i]);
        memset(d, 0, FRAME);
        d[0] = i == 0 ? 0x51 : (i == n - 1 ? 0x53 : 0x52);
        if (i == 0) {
            uint32_t sz = (uint32_t)blocks * BLOCK;
            memcpy(d + 4, &sz, 4);
            strncpy((char *)d + 10, name, 20);
        }
        uint16_t nx = i + 1 < n ? (uint16_t)(list[i + 1] - 1) : 0xFFFF;
        d[8] = (uint8_t)nx; d[9] = (uint8_t)(nx >> 8);
        fix_xor(d);
        memset(&card[list[i] * BLOCK], 0, BLOCK);
    }
    save_card();
    return list[0];
}

static void delete_file(int b) {
    while (b > 0) {
        int nx = next_block(b);
        uint8_t *d = dirent(b);
        d[0] = (uint8_t)(0xA0 | (d[0] & 0x0F));
        fix_xor(d);
        b = nx;
    }
    save_card();
}

/* byte offset in the card of file position pos */
static int file_offset(int first, uint32_t pos) {
    int b = first;
    for (uint32_t k = pos / BLOCK; k && b > 0; k--) b = next_block(b);
    if (b <= 0) return -1;
    return b * BLOCK + (int)(pos % BLOCK);
}

/* ------------------------------------------------------------------ BIOS file interface */

typedef struct { int used, first; uint32_t pos, size; int mode; } Fd;
static Fd fds[16];
int mcd_errno;

int mcd_open(const char *path, uint32_t mode) {
    int port; const char *name;
    if (!parse(path, &port, &name)) return -2; /* not a card path */
    if (!mcd_present(port)) { mcd_errno = 6; return -1; } /* ENXIO */
    int b = find_file(name);
    if (mode & 0x200) { /* create: block count in bits 16-31 */
        if (b > 0) { mcd_errno = 17; return -1; } /* EEXIST */
        b = create_file(name, (int)(mode >> 16));
        if (b < 0) { mcd_errno = 28; return -1; } /* ENOSPC */
    }
    if (b < 0) { mcd_errno = 2; return -1; } /* ENOENT */
    for (int i = 2; i < 16; i++)
        if (!fds[i].used) {
            fds[i] = (Fd){1, b, 0, file_size(b), (int)mode};
            mcd_errno = 0;
            return i;
        }
    mcd_errno = 24;
    return -1;
}

int mcd_is_fd(int fd) { return fd >= 2 && fd < 16 && fds[fd].used; }
int mcd_fd_async(int fd) { return mcd_is_fd(fd) && (fds[fd].mode & 0x8000); }

int mcd_rw(int fd, uint32_t buf, uint32_t len, int write) {
    if (!mcd_is_fd(fd)) return -1;
    Fd *f = &fds[fd];
    uint32_t done = 0;
    while (done < len && f->pos < f->size) {
        int off = file_offset(f->first, f->pos);
        if (off < 0) break;
        if (write) card[off] = (uint8_t)mem_r8(buf + done);
        else mem_w8(buf + done, card[off]);
        f->pos++; done++;
    }
    if (write) save_card();
    return (int)done;
}

int mcd_lseek(int fd, int32_t off, int whence) {
    if (!mcd_is_fd(fd)) return -1;
    Fd *f = &fds[fd];
    int64_t p = whence == 0 ? off : whence == 1 ? (int64_t)f->pos + off : (int64_t)f->size + off;
    if (p < 0) p = 0;
    f->pos = (uint32_t)p;
    return (int)f->pos;
}

void mcd_close(int fd) { if (mcd_is_fd(fd)) fds[fd].used = 0; }

/* firstfile/nextfile: fills a 40-byte DIRENTRY in PS1 RAM */
static char find_pat[32];
static int find_next_block;

static uint32_t fill_dirent(uint32_t de, int b) {
    for (int i = 0; i < 20; i++) mem_w8(de + i, dirent(b)[10 + i]);
    mem_w32(de + 20, 0x50);           /* attr */
    mem_w32(de + 24, file_size(b));   /* size */
    mem_w32(de + 28, 0);              /* next */
    mem_w32(de + 32, (uint32_t)b);    /* head */
    mem_w32(de + 36, 0);
    return de;
}

uint32_t mcd_find(const char *pat_path, uint32_t de, int first) {
    if (first) {
        int port; const char *name;
        if (!parse(pat_path, &port, &name) || !mcd_present(port)) return 0;
        snprintf(find_pat, sizeof find_pat, "%s", *name ? name : "*");
        find_next_block = 1;
    }
    for (; find_next_block < 16; find_next_block++) {
        int b = find_next_block;
        if (is_first(b) && wild_match(find_pat, (const char *)dirent(b) + 10)) {
            find_next_block++;
            return fill_dirent(de, b);
        }
    }
    return 0;
}

int mcd_erase(const char *path) {
    int port; const char *name;
    if (!parse(path, &port, &name) || !mcd_present(port)) return 0;
    int b = find_file(name);
    if (b < 0) return 0;
    delete_file(b);
    return 1;
}

int mcd_format(const char *path) {
    int port; const char *name;
    if (!parse(path, &port, &name) || !mcd_present(port)) return 0;
    format_card();
    save_card();
    return 1;
}

/* raw sector access (_card_read/_card_write) */
int mcd_sector(int port, int sector, uint32_t buf, int write) {
    if (!mcd_present(port) || sector < 0 || sector >= CARD_SIZE / FRAME) return 0;
    for (int i = 0; i < FRAME; i++) {
        if (write) card[sector * FRAME + i] = (uint8_t)mem_r8(buf + i);
        else mem_w8(buf + i, card[sector * FRAME + i]);
    }
    if (write) save_card();
    return 1;
}

/* ------------------------------------------------------------------ save states (state.c)
 * The card's contents are not rolled back by a load: saves on it are the player's progress (a state from before a
 * save must not delete it). Only the open files and the directory search are part of the state. */
#include "state.h"

void mcd_state(StIO *io) {
    st_begin(io, "MCD", 1);
    ST(io, fds); ST(io, mcd_errno); ST(io, find_pat); ST(io, find_next_block); ST(io, mcd_new_card);
    uint64_t h = 1469598103934665603ULL;
    for (int i = 0; i < CARD_SIZE; i++) { h ^= card[i]; h *= 1099511628211ULL; }
    uint64_t had = h;
    ST(io, had);
    st_end(io);
    if (io->load && !io->dry && had != h) LOG("state: the memory card differs from the one at the save (kept as it is)\n");
}
