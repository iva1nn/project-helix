/* Prop inventory (debug only, off by default): FE_DBG_PROPS=<log file>. Both games. NOTES.md "Props inventory".
 *
 * Every 3D object the games draw from an RMD goes through the same part call as the characters (FE2 0x800275B4 /
 * skin 0x800276FC, FE1 0x80025EDC / 0x80025FF0; record +0x18 = the RMD part header, +0x1C + 4 * buffer = the part's
 * packet buffer). The RMD stays in RAM as its file image: the part header holds the part's file offsets at +0x18
 * (vertices, faces, ..., packet buffers a / b at +0x2C / +0x30) and the same as pointers at +0x3C, and the file
 * header (u32 1, 1, part count, material count; part table at +0x60) lies before it. So each drawn part is tied to
 * its RMD, and the RMD to its file on the disc by a hash of its vertex bytes (FNV-1a over every part's vertex range,
 * which the game never changes; tools/props_inventory.py hashes the unpacked files the same way).
 *
 * The GPU DMA walk then gives every packet of those buffers its object: per GPU pass the screen box, the polygon count
 * and the texture pages / CLUTs of each object instance. Polygons that no part call filled ("loose" polygons: HUD,
 * effects, anything drawn by other code) are counted per texture page and CLUT, so nothing drawn in 3D goes unseen.
 *
 * FE_PROPS_ROOMS walks the area's rooms with the game's own room change (see rooms_init), and a screen dump is taken
 * a few frames after a new object or view appears (props_shot_due, main.c), for the inventory's crops.
 *
 * Log lines (only when something changed, so a still room writes a few lines):
 *   rmd H base=A parts=N mats=M f=F            an RMD (vertex hash H) seen for the first time at RAM A
 *   o f=F d=D lba=L id=I rmd=H inst=R polys=N box=X0,Y0,X1,Y1 tex=TP:CLUT,.. pose=P rel=Q
 *       one object instance in one GPU pass: inst = its first part record; box in screen pixels; pose = hash of
 *       its parts' camera-space matrices, rel = hash of the parts' poses relative to the first (multi-part motion)
 *   u f=F d=D lba=L tp=TP clut=C polys=N box=..  loose polygons of one texture page / CLUT in one pass
 *   v f=F d=D lba=L                            the room movie jumped to a new place (another view, or a cut)
 *   room f=F d=D room=R                        the room walk changed to room R
 *   shot f=F                                   a screen dump was taken at frame F */
#include <stdlib.h>
#include "hw.h"
#include "props.h"

int props_dbg;
uint32_t movie_shown_lba; /* game_fe2.c: STR sector of the room picture last uploaded (0 = not known) */
uint32_t cdrom_last_movie_lba;
static FILE *pf;
static int idx_part = -1, idx_skin = -1;
static uint32_t buf_word = 0x80085448u;

#define MAXSLOT 255
typedef struct {
    uint32_t A, hash, inst;         /* RMD base, vertex hash, first part record */
    uint64_t called;                /* frame of the last part call */
    uint32_t P[2][32], len[32];     /* packet buffers per buffer index and part */
    uint32_t mat_hash, rel_hash;    /* this frame's pose hashes */
    int16_t r0[9];                  /* the first part's matrix this frame (rel_hash) */
    int32_t t0[3];
    int polys, x0, y0, x1, y1, ntex; /* this GPU pass */
    uint32_t tex[8];
    char last[260];                 /* the last line written */
    uint64_t last_f;
} Slot;
static Slot slots[MAXSLOT];
static int nslots;
static uint8_t *owner; /* per RAM word: slot + 1 whose packets live there */

static struct { uint32_t A, hash; } rmds[512];
static int nrmds;
static struct { uint32_t mesh, A; } base_cache[1024];

typedef struct { uint32_t key; int polys, x0, y0, x1, y1; char last[120]; } Loose;
static Loose loose[64];
static int nloose;
static uint32_t cur_serial;
static int64_t shot_at = -1, last_shot = -1000;
static uint32_t last_lba;

static uint32_t rmd_entry(uint32_t A, int r) {
    uint32_t e = mem_r32(A + 0x60 + 8 * r);
    return (e >> 24) == 0x80 ? e : A + e;
}

static int rmd_has(uint32_t A, uint32_t hdr) {
    if (mem_r32(A) != 1 || mem_r32(A + 4) != 1) return 0;
    uint32_t np = mem_r32(A + 8);
    if (np < 1 || np > 64 || mem_r32(A + 12) > 1024) return 0;
    for (uint32_t r = 0; r < np; r++)
        if (rmd_entry(A, (int)r) == hdr) return 1;
    return 0;
}

/* the RMD a part header belongs to: its file header lies before it (as in models.c), cached per header. A file that
 * starts 1, 1, 1, 1 (one part, one material: SHADOW, SAVE, the weapons) also passes 4 bytes in, so the scan steps
 * back while the earlier start holds too. */
static uint32_t rmd_base_of(uint32_t hdr) {
    if ((hdr >> 24) != 0x80) return 0;
    unsigned h = (hdr >> 2) * 2654435761u % 1024;
    if (base_cache[h].mesh == hdr && rmd_has(base_cache[h].A, hdr)) return base_cache[h].A;
    for (uint32_t A = hdr & ~3u; A + 0x20000 > hdr && A >= 0x80000000u; A -= 4) {
        if (!rmd_has(A, hdr)) continue;
        while (rmd_has(A - 4, hdr)) A -= 4;
        base_cache[h].mesh = hdr;
        base_cache[h].A = A;
        return A;
    }
    return 0;
}

static uint32_t fnv(uint32_t h, uint32_t a, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) { h ^= mem_r8(a + i); h *= 16777619u; }
    return h;
}

/* FNV-1a over every part's vertex bytes (file offsets +0x18 .. +0x1C of the part header, relative to the RMD) */
static uint32_t rmd_hash(uint32_t A) {
    uint32_t h = 2166136261u, np = mem_r32(A + 8);
    for (uint32_t r = 0; r < np && r < 64; r++) {
        uint32_t ph = rmd_entry(A, (int)r), v0 = mem_r32(ph + 0x18), v1 = mem_r32(ph + 0x1C);
        if (v1 > v0 && v1 - v0 < 0x10000) h = fnv(h, A + v0, v1 - v0);
    }
    return h;
}

static uint32_t now_lba(void) { return movie_shown_lba ? movie_shown_lba : cdrom_last_movie_lba; }

static int64_t room_f = -1000; /* frame of the room walk's last room change */

/* a dump a few frames on, and at least 30 frames after a room change (the new room's picture is up by then) */
static void want_shot(void) {
    if (shot_at >= 0 || (int64_t)frame_count - last_shot <= 20) return;
    shot_at = (int64_t)frame_count + 4;
    if (shot_at < room_f + 30) shot_at = room_f + 30;
}

void props_part(uint32_t rec, int skin) {
    if (!pf) return;
    (void)skin;
    uint32_t mesh = mem_r32(rec + 0x18);
    uint32_t A = rmd_base_of(mesh);
    if (!A) return;
    int part = -1, np = (int)mem_r32(A + 8);
    for (int r = 0; r < np && r < 32; r++)
        if (rmd_entry(A, r) == mesh) { part = r; break; }
    if (part < 0) return;
    uint32_t hash = 0;
    for (int i = 0; i < nrmds; i++) if (rmds[i].A == A) { hash = rmds[i].hash; break; }
    if (!hash) {
        hash = rmd_hash(A);
        if (nrmds < 512) { rmds[nrmds].A = A; rmds[nrmds].hash = hash; nrmds++; }
        uint32_t ph = rmd_entry(A, 0), v0 = A + mem_r32(ph + 0x18), nvb = mem_r32(ph + 0x1C) - mem_r32(ph + 0x18);
        fprintf(pf, "rmd %08X base=%08X parts=%d mats=%u f=%llu v=", hash, A, np, mem_r32(A + 12), (unsigned long long)frame_count);
        for (uint32_t i = 0; i < 24 && i < nvb; i++) fprintf(pf, "%02X", mem_r8(v0 + i)); /* part 0's first vertices */
        fprintf(pf, "\n");
    }
    /* the instance: an actor's parts come as one run with the same RMD in a frame (records stay with an actor);
     * a part already met under a record keeps its slot */
    static uint32_t run_A;
    static uint64_t run_f;
    static int run_slot = -1, run_part;
    int s = -1;
    for (int i = 0; i < nslots; i++)
        if (slots[i].A == A && slots[i].hash == hash && slots[i].inst == rec) { s = i; break; }
    if (s < 0 && run_slot >= 0 && run_A == A && run_f == frame_count && part != run_part) s = run_slot;
    if (s < 0) {
        for (int i = 0; i < nslots; i++) /* reuse a slot gone for 10 s */
            if (frame_count - slots[i].called > 600) { s = i; break; }
        if (s < 0) { if (nslots >= MAXSLOT) return; s = nslots++; }
        memset(&slots[s], 0, sizeof slots[s]);
        for (uint32_t a = 0; a < 0x200000 / 4; a++) if (owner[a] == s + 1) owner[a] = 0;
        slots[s].A = A; slots[s].hash = hash; slots[s].inst = rec;
        want_shot();
    }
    run_A = A; run_f = frame_count; run_slot = s; run_part = part;
    Slot *sl = &slots[s];
    if (sl->called != frame_count) { sl->mat_hash = 2166136261u; sl->rel_hash = 2166136261u; }
    sl->called = frame_count;
    uint32_t buf = mem_r32(buf_word) & 1;
    uint32_t P = mem_r32(rec + 0x1C + 4 * buf) & 0x1FFFFC;
    uint32_t len = mem_r32(mesh + 0x30) - mem_r32(mesh + 0x2C);
    if (len > 0x10000) len = 0;
    if (P != sl->P[buf][part] || len != sl->len[part]) {
        uint32_t o = sl->P[buf][part];
        for (uint32_t a = o; a && a < o + sl->len[part] && a < 0x200000; a += 4) if (owner[a >> 2] == s + 1) owner[a >> 2] = 0;
        sl->P[buf][part] = P; sl->len[part] = len;
    }
    /* marked at every call: the buffer filled last belongs to this slot, whatever an older slot left there */
    for (uint32_t a = P; a < P + len && a < 0x200000; a += 4) owner[a >> 2] = (uint8_t)(s + 1);
    /* pose: the part's camera-space matrix (record +0x14 -> +4: 3x3 int16, 3 int32) */
    uint32_t m = mem_r32(mem_r32(rec + 0x14) + 4);
    if ((m >> 24) == 0x80) {
        sl->mat_hash = fnv(sl->mat_hash, m, 32);
        /* relative to the first part drawn this frame: R0^T R (1/64 steps), translation difference (64 units) */
        int16_t *r0 = sl->r0;
        int32_t *t0 = sl->t0;
        if (sl->rel_hash == 2166136261u)
            for (int i = 0; i < 9; i++) r0[i] = (int16_t)mem_r16(m + 2 * i);
        if (sl->rel_hash == 2166136261u)
            for (int i = 0; i < 3; i++) t0[i] = (int32_t)mem_r32(m + 20 + 4 * i);
        int32_t q[12];
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++) {
                int32_t d = 0;
                for (int k = 0; k < 3; k++) d += r0[k * 3 + i] * (int16_t)mem_r16(m + 2 * (k * 3 + j));
                q[i * 3 + j] = d >> 18;
            }
        for (int i = 0; i < 3; i++) q[9 + i] = ((int32_t)mem_r32(m + 20 + 4 * i) - t0[i]) >> 6;
        uint32_t h = sl->rel_hash;
        for (int i = 0; i < 12; i++) for (int b = 0; b < 4; b++) { h ^= (uint8_t)(q[i] >> (8 * b)); h *= 16777619u; }
        sl->rel_hash = h;
    }
}

static void box_add(int *x0, int *y0, int *x1, int *y1, int first, int x, int y) {
    if (first) { *x0 = *x1 = x; *y0 = *y1 = y; return; }
    if (x < *x0) *x0 = x;
    if (x > *x1) *x1 = x;
    if (y < *y0) *y0 = y;
    if (y > *y1) *y1 = y;
}

/* FE_PROPS_ROOMS=FROM[:EVERY[:FIRST[:LAST]]]: from frame FROM on, every EVERY frames (default 150) the game changes to
 * the next room of the area (FIRST .. LAST, default all) with its own room-change call. The area struct (FE2 0x8008A828,
 * FE1 0x8008C740) points at +0x5C to the area's room table (FE2 40-byte, FE1 52-byte records, each with its camera
 * clip); +0x80 holds the current room's index. room_change(area, room) sets a room up and starts its clip, as the game
 * does at doors from its room logic (runs every frame; the call is made at its start) and when the player walks into
 * another room's collision zone (the zone check, called while the player moves: player +36 = the zone's room). While
 * the walk runs the zone check's room change is skipped, so the player, who stays where they were, does not take the
 * game back; each room's props are drawn as in play. FE2 zone check 0x80039778, room change 0x8003CCB0, room logic
 * 0x80033F00; FE1 0x800352A0, 0x8003A36C, 0x8003051C (FE1's zone check disassembled side by side with FE2's). */
static struct { uint32_t area, rec, zone, change, logic; } RG[2] = {
    {0x8008A828u, 40, 0x80039778u, 0x8003CCB0u, 0x80033F00u},  /* FE2 */
    {0x8008C740u, 52, 0x800352A0u, 0x8003A36Cu, 0x8003051Cu}}; /* FE1 */
static int rg, idx_zone = -1, idx_logic = -1;
static int64_t rooms_from = -1, rooms_every = 150, rooms_next;
static int room_i, rooms_n, room_last = 9999;

static int rooms_count(void) {
    /* the room table ends where the next of the area's tables starts (pointers at area +0x10 .. +0x70) */
    uint32_t rt = mem_r32(RG[rg].area + 0x5C), end = rt + RG[rg].rec * 256;
    for (uint32_t o = 0x10; o <= 0x70; o += 4) {
        uint32_t p = mem_r32(RG[rg].area + o);
        if (p > rt && p < end) end = p;
    }
    return (int)((end - rt) / RG[rg].rec);
}

static void rooms_end(const char *why) {
    fprintf(pf, "rooms: %s at frame %llu, next room %d\n", why, (unsigned long long)frame_count, room_i);
    fclose(pf);
    exit(0);
}

static int logic_hook(void) {
    static int64_t first = -1;
    static uint32_t table;
    if (rooms_from >= 0 && first < 0) { /* the area runs: let its start settle before the walk */
        first = (int64_t)frame_count;
        if (rooms_next < first + 200) rooms_next = first + 200;
    }
    if (table && mem_r32(RG[rg].area + 0x5C) != table) rooms_end("left the area"); /* a room was a way out */
    if (rooms_from >= 0 && (int64_t)frame_count >= rooms_next && (rooms_n == 0 || (room_i < rooms_n && room_i <= room_last))) {
        if (!rooms_n) {
            rooms_n = rooms_count();
            table = mem_r32(RG[rg].area + 0x5C);
            fprintf(pf, "rooms: %d in the area (table %08X)\n", rooms_n, table);
        }
        if (room_i < rooms_n) {
            uint32_t save[32], hi = cpu.hi, lo = cpu.lo;
            memcpy(save, cpu.r, sizeof save);
            R4 = RG[rg].area;
            R5 = (uint32_t)room_i;
            fprintf(pf, "room f=%llu d=%d room=%d\n", (unsigned long long)frame_count, disc_current + 1, room_i);
            room_i++; /* first: some rooms run frames inside the call (a load), where rooms_watch may end the run */
            room_f = (int64_t)frame_count;
            rooms_next = (int64_t)frame_count + rooms_every;
            call_guest(RG[rg].change);
            memcpy(cpu.r, save, sizeof save); /* the registers of the game's own call, as if nothing came before it */
            cpu.hi = hi; cpu.lo = lo;
            rooms_next = (int64_t)frame_count + rooms_every;
        }
    }
    call_original(idx_logic);
    return 1;
}

static int zone_hook(void) {
    if (rooms_from < 0 || (int64_t)frame_count < rooms_from) { call_original(idx_zone); return 1; }
    R2 = mem_r32(mem_r32(R4) + 32) & 0xFFFFFE; /* what the check returns, without its room change */
    return 1;
}

/* the walk is over (its last room shown for EVERY frames) or stuck (a room that leaves the area: a load, a cutscene
 * waiting for the pad; the room logic stops running): the run ends, tools/props_inventory.py goes on in a new one */
static void rooms_watch(void) {
    if (rooms_from < 0 || !rooms_n) return;
    int done = room_i >= rooms_n || room_i > room_last;
    if ((int64_t)frame_count < rooms_next + (done ? 0 : 600)) return;
    rooms_end(done ? "done" : "stuck");
}

static void rooms_init(int fe1) {
    const char *e = getenv("FE_PROPS_ROOMS");
    if (!e) return;
    rg = fe1;
    rooms_from = atoll(e);
    const char *c = strchr(e, ':');
    if (c) { rooms_every = atoll(++c); c = strchr(c, ':'); }
    if (c) { room_i = atoi(++c); c = strchr(c, ':'); }
    if (c) room_last = atoi(++c);
    rooms_next = rooms_from;
    idx_zone = hook_index(RG[rg].zone);
    idx_logic = hook_index(RG[rg].logic);
    hook_register(RG[rg].zone, zone_hook);
    hook_register(RG[rg].logic, logic_hook);
    fprintf(pf, "rooms: walk from frame %lld every %lld, rooms %d..%d\n", (long long)rooms_from, (long long)rooms_every,
            room_i, room_last);
}

static void flush_pass(void) {
    static int view_shot;
    uint32_t lba = now_lba();
    if (lba < last_lba || lba > last_lba + 64) { /* a jump in the room movie: another view, or a loop restart */
        static uint32_t targets[16];
        static int nt;
        int k;
        for (k = 0; k < 16 && (targets[k] > lba + 16 || targets[k] + 16 < lba); k++) {}
        if (k == 16) { /* not one of the last places jumped to: a new view */
            targets[nt++ & 15] = lba;
            fprintf(pf, "v f=%llu d=%d lba=%u\n", (unsigned long long)frame_count, disc_current + 1, lba);
            view_shot = 1;
        }
    }
    last_lba = lba;
    rooms_watch();
    /* Polygons outside every part buffer with an object's texture page and CLUT, overlapping its box, are that
     * object's: the renderer subdivides near polygons into a pool of its own (FE2 around 0x105000). */
    for (int i = 0; i < nloose; i++) {
        Loose *l = &loose[i];
        if (!l->polys) continue;
        for (int s = 0; s < nslots; s++) {
            Slot *sl = &slots[s];
            int k;
            for (k = 0; k < sl->ntex && sl->tex[k] != l->key; k++) {}
            if (!sl->polys || k == sl->ntex || l->x1 < sl->x0 - 8 || l->x0 > sl->x1 + 8 || l->y1 < sl->y0 - 8 || l->y0 > sl->y1 + 8)
                continue;
            box_add(&sl->x0, &sl->y0, &sl->x1, &sl->y1, 0, l->x0, l->y0);
            box_add(&sl->x0, &sl->y0, &sl->x1, &sl->y1, 0, l->x1, l->y1);
            sl->polys += l->polys;
            l->polys = 0;
            break;
        }
    }
    for (int i = 0; i < nslots; i++) {
        Slot *sl = &slots[i];
        if (!sl->polys) continue;
        if (view_shot) { want_shot(); view_shot = 0; }
        char tex[128] = "";
        for (int k = 0; k < sl->ntex; k++)
            sprintf(tex + strlen(tex), "%s%04X:%04X", k ? "," : "", sl->tex[k] >> 16, sl->tex[k] & 0xFFFF);
        char line[260];
        snprintf(line, sizeof line, "rmd=%08X inst=%08X polys=%d box=%d,%d,%d,%d tex=%s pose=%08X rel=%08X", sl->hash,
                 sl->inst, sl->polys, sl->x0, sl->y0, sl->x1, sl->y1, tex[0] ? tex : "-", sl->mat_hash, sl->rel_hash);
        if (strcmp(line, sl->last) || frame_count - sl->last_f >= 300) {
            fprintf(pf, "o f=%llu d=%d lba=%u id=%d %s\n", (unsigned long long)frame_count, disc_current + 1, lba, i, line);
            strcpy(sl->last, line);
            sl->last_f = frame_count;
        }
        sl->polys = 0; sl->ntex = 0;
    }
    for (int i = 0; i < nloose; i++) {
        Loose *l = &loose[i];
        if (!l->polys) continue;
        char line[120];
        snprintf(line, sizeof line, "tp=%04X clut=%04X polys=%d box=%d,%d,%d,%d", l->key >> 16, l->key & 0xFFFF, l->polys,
                 l->x0, l->y0, l->x1, l->y1);
        if (strcmp(line, l->last)) {
            fprintf(pf, "u f=%llu d=%d lba=%u %s\n", (unsigned long long)frame_count, disc_current + 1, lba, line);
            strcpy(l->last, line);
        }
        l->polys = 0;
    }
    fflush(pf);
}

/* a GPU display-list packet at a: polygons only (GP0 0x20-0x3F) */
void props_packet(uint32_t a) {
    if (!pf) return;
    if (models_dma_serial != cur_serial) { /* a new pass: write out the last one */
        if (cur_serial) flush_pass();
        cur_serial = models_dma_serial;
    }
    a &= 0x1FFFFC;
    uint32_t w0 = mem_r32(0x80000000u | (a + 4));
    uint8_t code = (uint8_t)(w0 >> 24);
    if (code < 0x20 || code > 0x3F) return;
    int nv = (code & 8) ? 4 : 3, tex = code & 4, gour = code & 0x10;
    int dx1, dy1, dx2, dy2, offx, offy;
    gpu_draw_area(&dx1, &dy1, &dx2, &dy2, &offx, &offy);
    uint32_t p = a + 8, clut = 0, tp = 0;
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    for (int i = 0; i < nv; i++) {
        if (gour && i) p += 4;
        uint32_t xy = mem_r32(0x80000000u | p);
        p += 4;
        int x = (int)((int32_t)(xy << 21) >> 21) + offx - dx1, y = (int)((int32_t)((xy >> 16) << 21) >> 21) + offy - dy1;
        box_add(&x0, &y0, &x1, &y1, i == 0, x, y);
        if (tex) {
            uint32_t uv = mem_r32(0x80000000u | p);
            p += 4;
            if (i == 0) clut = uv >> 16;
            if (i == 1) tp = uv >> 16;
        }
    }
    if (tex && ((tp >> 7) & 3) == 2) return; /* 16-bit texture: the room movie's pieces */
    if (x1 - x0 > 1023 || y1 - y0 > 511) return; /* the GPU drops it (behind the camera, too close) */
    /* clipped to the drawing area as the GPU does; a polygon wholly outside is not seen */
    int w = dx2 - dx1, h = dy2 - dy1;
    if (x1 < 0 || y1 < 0 || x0 > w || y0 > h) return;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > w) x1 = w;
    if (y1 > h) y1 = h;
    if (!tex) tp = 0xFFFF;
    uint32_t key = (tp & 0x1FF) << 16 | (tex ? clut : 0); /* page x / y and depth, not the blend bits */
    int id = owner[a >> 2];
    if (id && frame_count - slots[id - 1].called <= 4) {
        Slot *sl = &slots[id - 1];
        box_add(&sl->x0, &sl->y0, &sl->x1, &sl->y1, sl->polys == 0, x0, y0);
        box_add(&sl->x0, &sl->y0, &sl->x1, &sl->y1, 0, x1, y1);
        sl->polys++;
        int k;
        for (k = 0; k < sl->ntex && sl->tex[k] != key; k++) {}
        if (k == sl->ntex && k < 8) sl->tex[sl->ntex++] = key;
        return;
    }
    int k;
    for (k = 0; k < nloose && loose[k].key != key; k++) {}
    if (k == nloose) { if (nloose >= 64) return; loose[nloose].key = key; nloose++; }
    Loose *l = &loose[k];
    box_add(&l->x0, &l->y0, &l->x1, &l->y1, l->polys == 0, x0, y0);
    box_add(&l->x0, &l->y0, &l->x1, &l->y1, 0, x1, y1);
    l->polys++;
}

/* main.c: dump the screen now (a new object or view appeared a few frames ago) */
int props_shot_due(void) {
    if (!pf || shot_at < 0 || (int64_t)frame_count < shot_at) return 0;
    shot_at = -1;
    last_shot = (int64_t)frame_count;
    fprintf(pf, "shot f=%llu\n", (unsigned long long)frame_count);
    return 1;
}

static int part_hook(void) { uint32_t rec = R5; call_original(idx_part); props_part(rec, 0); return 1; }
static int skin_hook(void) { uint32_t rec = R5; call_original(idx_skin); props_part(rec, 1); return 1; }

void props_init(void) {
    const char *e = getenv("FE_DBG_PROPS");
    if (!e || !e[0]) return;
    int fe1 = !strcmp(game_serial, "SLUS-00920") || !strncmp(game_serial, "SLUS-0105", 9);
    if (!fe1 && strncmp(game_serial, "SLUS-0126", 9) && strncmp(game_serial, "SLUS-0127", 9)) return;
    pf = fopen(e, "w");
    if (!pf) return;
    owner = calloc(0x200000 / 4, 1);
    props_dbg = 1;
    uint32_t part_fn = fe1 ? 0x80025EDCu : 0x800275B4u, skin_fn = fe1 ? 0x80025FF0u : 0x800276FCu;
    buf_word = fe1 ? 0x80087188u : 0x80085448u;
    idx_part = hook_index(part_fn);
    idx_skin = hook_index(skin_fn);
    hook_register(part_fn, part_hook); /* models_init registers its own hooks over these and forwards */
    hook_register(skin_fn, skin_hook);
    fprintf(pf, "props: %s %s\n", fe1 ? "fe1" : "fe2", game_serial);
    rooms_init(fe1);
}
