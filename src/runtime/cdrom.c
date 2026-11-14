/* CD-ROM controller (register level) reading the user's .cue/.bin image. */
#include <stdlib.h>
#include <ctype.h>
#include "port.h"
#include "hw.h"
#include "props.h"

static FILE *bin;
void (*cdrom_seek_hook)(uint32_t file_sector); /* Setloc: the remaster movies prefetch the target */
uint64_t cdrom_last_movie_frame = (uint64_t)-1000; /* last frame an STR video sector was read */
static uint32_t disc_sectors;
static int64_t shell_open_until; /* disc swap: the lid reads as open until then */
static int shell_was_open;

int cdrom_open_image(const char *cue);

/* change the disc: the lid reads as open for about one second, like a real swap */
int cdrom_swap(const char *cue) {
    FILE *old = bin;
    bin = 0;
    if (!cdrom_open_image(cue)) { bin = old; return 0; }
    if (old) fclose(old);
    shell_open_until = cpu.cycles + CPU_HZ;
    shell_was_open = 1;
    sched_at(shell_open_until);
    return 1;
}

int cdrom_open(const char *cue) { return cdrom_open_image(cue); }

int cdrom_open_image(const char *cue) {
    char path[1024];
    snprintf(path, sizeof path, "%s", cue);
    const char *dot = strrchr(cue, '.');
    if (dot && (!_stricmp(dot, ".cue"))) {
        FILE *f = fopen(cue, "rb");
        if (!f) return 0;
        char line[1024], name[512] = {0};
        while (fgets(line, sizeof line, f)) {
            char *q = strstr(line, "FILE \"");
            if (q) {
                q += 6;
                char *e = strchr(q, '"');
                if (e) { *e = 0; snprintf(name, sizeof name, "%s", q); }
                break;
            }
        }
        fclose(f);
        if (!name[0]) return 0;
        const char *slash = strrchr(cue, '/');
        const char *bs = strrchr(cue, '\\');
        if (bs > slash) slash = bs;
        if (slash) snprintf(path, sizeof path, "%.*s/%s", (int)(slash - cue), cue, name);
        else snprintf(path, sizeof path, "%s", name);
    }
    bin = fopen(path, "rb");
    if (!bin) return 0;
    _fseeki64(bin, 0, SEEK_END);
    disc_sectors = (uint32_t)(_ftelli64(bin) / 2352);
    LOG("disc: %s, %u sectors\n", path, disc_sectors);
    return 1;
}

int cdrom_read_sector(uint32_t lba, uint8_t *out) {
    if (!bin || lba >= disc_sectors) { memset(out, 0, 2352); return 0; }
    _fseeki64(bin, (int64_t)lba * 2352, SEEK_SET);
    return fread(out, 1, 2352, bin) == 2352;
}

/* ISO9660 lookup, used by the boot loader */
static int iso_read(uint32_t lba, uint8_t *buf2048) {
    uint8_t s[2352];
    if (!cdrom_read_sector(lba, s)) return 0;
    memcpy(buf2048, s + 24, 2048);
    return 1;
}

int cdrom_find_file(const char *path, uint32_t *out_lba, uint32_t *out_size) {
    uint8_t sec[2048];
    if (!iso_read(16, sec)) return 0;
    uint32_t dlba, dsize;
    memcpy(&dlba, sec + 156 + 2, 4);
    memcpy(&dsize, sec + 156 + 10, 4);
    const char *p = path;
    while (*p == '\\' || *p == '/') p++;
    for (;;) {
        char part[64];
        int n = 0;
        while (*p && *p != '\\' && *p != '/' && n < 63) part[n++] = (char)toupper((unsigned char)*p++);
        part[n] = 0;
        int last = (*p == 0);
        while (*p == '\\' || *p == '/') p++;
        int found = 0;
        for (uint32_t off = 0; off < dsize && !found; off += 2048) {
            if (!iso_read(dlba + off / 2048, sec)) return 0;
            int pos = 0;
            while (pos < 2048) {
                int len = sec[pos];
                if (!len) break;
                int nl = sec[pos + 32];
                char nm[64];
                int k;
                for (k = 0; k < nl && k < 63; k++) nm[k] = (char)toupper(sec[pos + 33 + k]);
                nm[k] = 0;
                char *semi = strchr(nm, ';');
                if (semi) *semi = 0;
                if (!strcmp(nm, part)) {
                    memcpy(&dlba, sec + pos + 2, 4);
                    memcpy(&dsize, sec + pos + 10, 4);
                    found = 1;
                    break;
                }
                pos += len;
            }
        }
        if (!found) return 0;
        if (last) { *out_lba = dlba; *out_size = dsize; return 1; }
    }
}

/* ------------------------------------------------------------------ controller */

static uint8_t bcd(int v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }
static int unbcd(uint8_t v) { return (v >> 4) * 10 + (v & 15); }

typedef struct { int64_t time; int irq; uint8_t r[16]; int n; int has_sector; uint8_t sector[2352]; } Resp;
#define QN 16
static struct {
    int index;
    uint8_t params[16]; int nparams;
    uint8_t resp[16]; int nresp, rpos;
    uint8_t ien, iflag;
    uint8_t stat, mode;
    uint8_t filt_file, filt_chan;
    uint32_t setloc, pos; int setloc_pending;
    int reading, seeking;
    int64_t next_read;
    Resp q[QN]; int qn;
    uint8_t sector[2352];   /* last sector read */
    uint8_t data[2352]; int dlen, dpos; /* data FIFO */
    int have_sector;
    int busy;
    int64_t retry_time;
    int muted;
} cd;
/* a sector whose INT1 was delivered and that the game has not loaded into the data FIFO yet (BFRD). The read
 * speedup waits for it as for a queued one: a sector read meanwhile reached the game 2000 cycles after the INT1 ack,
 * before its ReqData, replaced the untaken sector and shifted the rest of the file by 2048 bytes (FE1 after the
 * door-gunner cutscene: a TIM header read from pixel data, the VRAM allocator's tree ran over all RAM, NOTES
 * "Save state crash at the door-gunner cutscene"). */
static int sector_untaken;

static void q_push(int64_t t, int irq, const uint8_t *r, int n) {
    if (cd.qn >= QN) { LOG("cdrom: response queue full\n"); return; }
    Resp *x = &cd.q[cd.qn++];
    x->time = t; x->irq = irq; x->n = n; x->has_sector = 0;
    memcpy(x->r, r, n);
    sched_at(t);
}

static void deliver(void) {
    /* deliver the oldest due response if the interrupt flag is free */
    if ((cd.iflag & 7) || !cd.qn) return;
    if (cd.q[0].time > cpu.cycles) { sched_at(cd.q[0].time); return; }
    Resp *x = &cd.q[0];
    memcpy(cd.resp, x->r, x->n);
    cd.nresp = x->n; cd.rpos = 0;
    cd.iflag = (cd.iflag & ~7) | (uint8_t)x->irq;
    if (x->has_sector) { memcpy(cd.sector, x->sector, 2352); cd.have_sector = 1; sector_untaken = 1; }
    memmove(&cd.q[0], &cd.q[1], (size_t)(cd.qn - 1) * sizeof(Resp));
    cd.qn--;
    cd.busy = 0;
    DBG("cdrom INT%d resp %02X (%d bytes) ien=%02X msf=%02X:%02X:%02X\n", cd.iflag & 7, cd.resp[0], cd.nresp, cd.ien,
        cd.sector[12], cd.sector[13], cd.sector[14]);
    if (cd.iflag & cd.ien & 0x1F) irq_raise(IRQ_CDROM);
}

static int64_t sector_cycles(void) { return (cd.mode & 0x80) ? CPU_HZ / 150 : CPU_HZ / 75; }

/* Loading faster than the drive: plain data sectors come cdrom_speedup times faster (DuckStation's read speedup);
 * movie (STR) and XA audio sectors keep the drive's rate, the rooms and the voices play from them in real time. A
 * read that has just delivered one of those stays at that rate. */
int cdrom_speedup = 8;
static int last_streamed; /* the last sector read was STR video or XA audio */

uint8_t cdrom_shell_bits(void);
static void stat_only(int64_t t, int irq) { uint8_t s = cd.stat | cdrom_shell_bits(); q_push(t, irq, &s, 1); }

static void purge_sectors(void) {
    int k = 0;
    for (int i = 0; i < cd.qn; i++)
        if (!cd.q[i].has_sector) cd.q[k++] = cd.q[i];
    cd.qn = k;
}

static void command(uint8_t cmd) {
    int64_t now = cpu.cycles;
    if (cmd == 0x06 || cmd == 0x1B || cmd == 0x08 || cmd == 0x09 || cmd == 0x0A || cmd == 0x15 || cmd == 0x16 || cmd == 0x1C) {
        purge_sectors();
        sector_untaken = 0; /* the reads that follow do not wait for a sector the game has dropped */
    }
    int64_t t1 = now + 25000;
    uint8_t r[16];
    DBG("cdrom cmd %02X params %d [%02X %02X %02X]\n", cmd, cd.nparams, cd.params[0], cd.params[1], cd.params[2]);
    cd.busy = 1;
    switch (cmd) {
    case 0x01: stat_only(t1, 3); break;
    case 0x02:
        cd.setloc = (uint32_t)((unbcd(cd.params[0]) * 60 + unbcd(cd.params[1])) * 75 + unbcd(cd.params[2]));
        cd.setloc_pending = 1;
        if (cdrom_seek_hook && cd.setloc >= 150) cdrom_seek_hook(cd.setloc - 150); /* file sector */
        { void agent_cd_seek(uint32_t sector); if (cd.setloc >= 150) agent_cd_seek(cd.setloc - 150); }
        stat_only(t1, 3);
        break;
    case 0x03: /* Play (CD audio): not supported, behave like a seek */
        cd.stat = 0x82; stat_only(t1, 3); break;
    case 0x06: case 0x1B: /* ReadN / ReadS */
        if (cpu.cycles < shell_open_until) { /* lid open: error */
            r[0] = (uint8_t)(cd.stat | 0x11); r[1] = 0x08; q_push(t1, 5, r, 2);
            break;
        }
        if (cd.setloc_pending) { cd.pos = cd.setloc; cd.setloc_pending = 0; }
        cd.stat = 0x02;
        stat_only(t1, 3);
        cd.stat = 0x22;
        cd.reading = 1;
        cd.next_read = now + sector_cycles() + 30000;
        sched_at(cd.next_read);
        break;
    case 0x07: cd.stat = 0x02; stat_only(t1, 3); stat_only(t1 + 50000, 2); break;
    case 0x08: cd.reading = 0; stat_only(t1, 3); cd.stat = 0x00; stat_only(t1 + 100000, 2); break;
    case 0x09: /* Pause */
        stat_only(t1, 3);
        cd.reading = 0;
        cd.stat = 0x02;
        stat_only(t1 + ((cd.mode & 0x80) ? 70000 : 140000), 2);
        break;
    case 0x0A: /* Init */
        cd.mode = 0; cd.reading = 0;
        cd.stat = 0x02;
        stat_only(t1, 3);
        stat_only(t1 + 80000, 2);
        break;
    case 0x0B: cd.muted = 1; stat_only(t1, 3); break;
    case 0x0C: cd.muted = 0; stat_only(t1, 3); break;
    case 0x0D: cd.filt_file = cd.params[0]; cd.filt_chan = cd.params[1]; stat_only(t1, 3); break;
    case 0x0E: cd.mode = cd.params[0]; stat_only(t1, 3); break;
    case 0x0F: r[0] = cd.stat; r[1] = cd.mode; r[2] = 0; r[3] = cd.filt_file; r[4] = cd.filt_chan; q_push(t1, 3, r, 5); break;
    case 0x10: memcpy(r, cd.sector + 12, 8); q_push(t1, 3, r, 8); break;
    case 0x11: {
        uint32_t p = cd.pos ? cd.pos - 1 : 0;
        uint32_t rel = p >= 150 ? p - 150 : 0;
        r[0] = 1; r[1] = 1;
        r[2] = bcd(rel / 75 / 60); r[3] = bcd(rel / 75 % 60); r[4] = bcd(rel % 75);
        r[5] = bcd(p / 75 / 60); r[6] = bcd(p / 75 % 60); r[7] = bcd(p % 75);
        q_push(t1, 3, r, 8);
        break;
    }
    case 0x12: stat_only(t1, 3); stat_only(t1 + 50000, 2); break;
    case 0x13: r[0] = cd.stat; r[1] = 1; r[2] = 1; q_push(t1, 3, r, 3); break;
    case 0x14: {
        uint32_t end = disc_sectors + 150;
        r[0] = cd.stat;
        if (cd.params[0] == 0) { r[1] = bcd(end / 75 / 60); r[2] = bcd(end / 75 % 60); }
        else { r[1] = 0; r[2] = 2; }
        q_push(t1, 3, r, 3);
        break;
    }
    case 0x15: case 0x16: /* SeekL / SeekP */
        if (cd.setloc_pending) { cd.pos = cd.setloc; cd.setloc_pending = 0; }
        cd.reading = 0;
        cd.stat = 0x42;
        stat_only(t1, 3);
        cd.stat = 0x02;
        stat_only(t1 + 60000, 2);
        break;
    case 0x19:
        if (cd.params[0] == 0x20) { r[0] = 0x94; r[1] = 0x09; r[2] = 0x19; r[3] = 0xC0; q_push(t1, 3, r, 4); }
        else stat_only(t1, 3);
        break;
    case 0x1A: {
        stat_only(t1, 3);
        uint8_t id[8] = {0x02, 0x00, 0x20, 0x00, 'S', 'C', 'E', 'A'};
        q_push(t1 + 30000, 2, id, 8);
        break;
    }
    case 0x1C: cd.mode = 0; cd.reading = 0; cd.stat = 0x02; stat_only(t1, 3); break;
    case 0x1E: stat_only(t1, 3); stat_only(t1 + 200000, 2); break;
    default:
        LOG("cdrom: unknown command %02X\n", cmd);
        r[0] = cd.stat | 1; r[1] = 0x40; q_push(t1, 5, r, 2);
        break;
    }
    cd.nparams = 0;
}

static void read_sector_now(void) {
    uint32_t lba = cd.pos >= 150 ? cd.pos - 150 : 0;
    uint8_t s[2352];
    cdrom_read_sector(lba, s);
    cd.pos++;
    uint8_t submode = s[18];
    int is_audio = (s[15] == 2) && (submode & 0x04) && (submode & 0x20);
    last_streamed = is_audio || (s[24] == 0x60 && s[25] == 0x01 && s[26] == 0x01 && s[27] == 0x80);
    if (s[24] == 0x60 && s[25] == 0x01 && s[26] == 0x01 && s[27] == 0x80) {
        if (getenv("FE_DBG_CD") && frame_count - cdrom_last_movie_frame > 30)
            LOG("cdrom: movie stream at sector %u (frame %llu)\n", lba, (unsigned long long)frame_count);
        cdrom_last_movie_frame = frame_count;
        cdrom_last_movie_lba = lba; /* props.c */
    }
    if ((cd.mode & 0x40) && is_audio) {
        /* XA-ADPCM sector: goes to the sound chip when the filter matches; never to the CPU */
        if (!(cd.mode & 0x08) || (s[16] == cd.filt_file && s[17] == cd.filt_chan))
            if (!cd.muted) spu_xa_sector(s);
        return;
    }
    if ((cd.mode & 0x08) && (cd.mode & 0x40) && s[15] == 2 && (submode & 0x40)) {
        /* real-time sector with XA filter on: only matching channel delivered */
        if (s[16] != cd.filt_file || s[17] != cd.filt_chan) return;
    }
    int pending = 0;
    for (int i = 0; i < cd.qn; i++) pending += cd.q[i].has_sector;
    if (pending >= 8) { DBG("cdrom: sector %u dropped (CPU too slow)\n", lba); return; }
    uint8_t st = cd.stat;
    q_push(cpu.cycles, 1, &st, 1);
    cd.q[cd.qn - 1].has_sector = 1;
    memcpy(cd.q[cd.qn - 1].sector, s, 2352);
}

void cdrom_update(void) {
    int fast_wait = 0;
    if (cd.reading && cpu.cycles >= cd.next_read && !last_streamed && cdrom_speedup > 1) {
        /* faster than the drive: the next data sector waits until the game has taken the last one (a real drive
         * at that rate would overrun the game's buffer, and a dropped sector is a corrupt load) */
        for (int i = 0; i < cd.qn; i++) fast_wait |= cd.q[i].has_sector;
        fast_wait |= sector_untaken;
        if (fast_wait) cd.next_read = cpu.cycles + 2000;
    }
    if (cd.reading && cpu.cycles >= cd.next_read && !fast_wait) {
        read_sector_now();
        int64_t step = sector_cycles();
        if (!last_streamed && cdrom_speedup > 1) step /= cdrom_speedup;
        cd.next_read += step;
        if (cd.next_read < cpu.cycles) cd.next_read = cpu.cycles + step;
    }
    if (cd.reading) sched_at(cd.next_read);
    deliver();
}

uint8_t cdrom_read(uint32_t addr) {
    switch (addr & 3) {
    case 0: {
        uint8_t s = (uint8_t)cd.index;
        if (cd.nparams == 0) s |= 0x08;
        if (cd.nparams < 16) s |= 0x10;
        if (cd.rpos < cd.nresp) s |= 0x20;
        if (cd.dpos < cd.dlen) s |= 0x40;
        if (cd.busy) s |= 0x80;
        return s;
    }
    case 1: {
        if (cd.rpos < cd.nresp) return cd.resp[cd.rpos++];
        return 0;
    }
    case 2:
        if (cd.dpos < cd.dlen) return cd.data[cd.dpos++];
        return 0;
    case 3:
        if (cd.index & 1) return cd.iflag | 0xE0;
        return cd.ien | 0xE0;
    }
    return 0;
}

static void load_data_fifo(void) {
    if (!cd.have_sector) { cd.dlen = cd.dpos = 0; return; }
    sector_untaken = 0;
    if (cd.mode & 0x20) { memcpy(cd.data, cd.sector + 12, 0x924); cd.dlen = 0x924; }
    else { memcpy(cd.data, cd.sector + 24, 0x800); cd.dlen = 0x800; }
    cd.dpos = 0;
}

void cdrom_write(uint32_t addr, uint8_t v) {
    int r = addr & 3;
    DBG("cdrom write %08X reg%d.%d = %02X\n", addr, r, cd.index, v);
    if (r == 0) { cd.index = v & 3; return; }
    switch (cd.index * 4 + r) {
    case 1: command(v); break;
    case 2: if (cd.nparams < 16) cd.params[cd.nparams++] = v; break;
    case 3: /* request register: BFRD loads the FIFO if it is empty; clearing it empties the FIFO */
        if (v & 0x80) { if (cd.dpos >= cd.dlen) load_data_fifo(); }
        else { cd.dlen = cd.dpos = 0; }
        break;
    case 6: cd.ien = v & 0x1F; break;
    case 7:
        cd.iflag &= ~(v & 0x1F);
        if (v & 0x40) cd.nparams = 0;
        if (!(cd.iflag & 7) && cd.qn) {
            if (cd.q[0].time < cpu.cycles + 2000) cd.q[0].time = cpu.cycles + 2000;
            sched_at(cd.q[0].time);
        }
        break;
    default: break; /* volume registers */
    }
}

uint32_t cdrom_dma_read(void) {
    uint32_t w = 0;
    for (int i = 0; i < 4; i++) {
        uint8_t b = cd.dpos < cd.dlen ? cd.data[cd.dpos++] : 0;
        w |= (uint32_t)b << (8 * i);
    }
    return w;
}

void cdrom_init(void) {
    memset(&cd, 0, sizeof cd);
    sector_untaken = 0;
    cd.stat = 0x02;
    cd.ien = 0x1F; /* left like this by the real BIOS; libcd never sets it */
}

/* drive status bit 4 (shell open) while a swap is in progress; reported once more after closing */
uint8_t cdrom_shell_bits(void) {
    if (cpu.cycles < shell_open_until) return 0x10;
    /* lid closed again: the drive spins up by itself (FE1's swap screen waits for the motor bit) */
    if (shell_was_open) { shell_was_open = 0; cd.stat |= 0x02; return 0x10; }
    return 0;
}

/* the drive is reading sectors (automatic disc swap waits until it is idle) */
int cdrom_reading(void) { return cd.reading; }

/* ------------------------------------------------------------------ save states (state.c) */
#include "state.h"

void cdrom_state(StIO *io) {
    uint32_t v = st_begin_compat(io, "CDROM", 2, 1);
    ST(io, cd);
    ST(io, shell_open_until); ST(io, shell_was_open); ST(io, last_streamed);
    ST(io, cdrom_last_movie_frame); ST(io, cdrom_last_movie_lba);
    if (v >= 2) ST(io, sector_untaken);
    else if (io->load && !io->dry) sector_untaken = 0;
    st_end(io);
}

/* a state on another disc: that image, without the lid-open delay of a swap (the state has the drive's status) */
int cdrom_set_image(const char *cue) {
    FILE *old = bin;
    bin = 0;
    if (!cdrom_open_image(cue)) { bin = old; return 0; }
    if (old) fclose(old);
    return 1;
}
