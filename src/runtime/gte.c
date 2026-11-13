/* GTE (COP2): the PS1 geometry coprocessor, implemented from the documented hardware behaviour
 * (nocash psx-spx "Geometry Transformation Engine"). */
#include <stdlib.h>
#include <math.h>
#include "hw.h"

static uint32_t d[32]; /* data registers */

/* Precise screen positions (as DuckStation's PGXP): next to the SXY FIFO, each projected vertex keeps its sub-pixel
 * x, y and its view depth. A swc2 of SXY stores them for that RAM word (pgxp_store); the GPU looks the word up when it
 * draws a polygon from it (pgxp_lookup) and gets the vertex without the snapping to whole pixels, with the depth for
 * perspective-correct texturing. A word overwritten since (value differs) falls back to the integers. */
int pgxp_on = 0; /* off by default (NOTES.md "Precise vertices"): ini "precision = 1" */
static float pg_x[3], pg_y[3], pg_z[3];
static uint8_t pg_ok[3];
PgWord *pgxp_mem;          /* allocated on the first precise store; NULL = off */
PgWord pgxp_vcache[4096];
#define pg_mem pgxp_mem

static void pg_push(float x, float y, float z, int ok) {
    for (int i = 0; i < 2; i++) { pg_x[i] = pg_x[i + 1]; pg_y[i] = pg_y[i + 1]; pg_z[i] = pg_z[i + 1]; pg_ok[i] = pg_ok[i + 1]; }
    pg_x[2] = x; pg_y[2] = y; pg_z[2] = z; pg_ok[2] = (uint8_t)ok;
}

void pgxp_store(uint32_t addr, int reg) {
    if (!pgxp_on) return;
    if (!pg_mem) pg_mem = calloc(PSX_RAM_SIZE / 4 + 256, sizeof(PgWord)); /* RAM words, then the scratchpad */
    if (!pg_mem) return;
    int i = reg == 15 ? 2 : reg - 12;
    PgWord *w = &pg_mem[pgxp_idx(addr)];
    w->val = d[12 + i];
    if (pg_ok[i]) { w->x = pg_x[i]; w->y = pg_y[i]; w->z = pg_z[i]; }
    else w->z = 0; /* not precise */
}

/* lwc2 of SXY0-2 / SXYP (after gte_write_data): the register gets the word's precise value, if it has one */
void pgxp_load(uint32_t addr, int reg) {
    if (!pg_mem) return;
    int i = reg == 15 ? 2 : reg - 12;
    const PgWord *w = &pg_mem[pgxp_idx(addr)];
    if (w->val == d[12 + i] && w->z > 0) { pg_x[i] = w->x; pg_y[i] = w->y; pg_z[i] = w->z; pg_ok[i] = 1; }
}

int pgxp_lookup(uint32_t addr, uint32_t val, float *x, float *y, float *z) {
    if (!pg_mem || addr == 0xFFFFFFFFu) return 0;
    const PgWord *w = &pg_mem[pgxp_idx(addr)];
    if (w->val != val || !(w->z > 0)) return 0;
    *x = w->x; *y = w->y; *z = w->z;
    return 1;
}
static uint32_t c[32]; /* control registers */
static uint32_t flag;

#define VX(n) ((int16_t)(d[(n) * 2] & 0xFFFF))
#define VY(n) ((int16_t)(d[(n) * 2] >> 16))
#define VZ(n) ((int16_t)(d[(n) * 2 + 1] & 0xFFFF))
#define IR0 ((int16_t)d[8])
#define IR1 ((int16_t)d[9])
#define IR2 ((int16_t)d[10])
#define IR3 ((int16_t)d[11])
#define SX(n) ((int16_t)(d[12 + (n)] & 0xFFFF))
#define SY(n) ((int16_t)(d[12 + (n)] >> 16))
#define SZ(n) (d[16 + (n)] & 0xFFFF)
#define RGBC d[6]
#define CODE (RGBC >> 24)
#define CR (RGBC & 0xFF)
#define CG ((RGBC >> 8) & 0xFF)
#define CB ((RGBC >> 16) & 0xFF)

/* matrix element m (0..2: RT, LLM, LCM), row i, column j */
static int32_t mtx(int m, int i, int j) {
    int base = m * 8;
    int k = i * 3 + j;
    uint32_t w = c[base + k / 2];
    return (k & 1) ? (int16_t)(w >> 16) : (int16_t)(w & 0xFFFF);
}
#define TRX ((int32_t)c[5])
#define TRY ((int32_t)c[6])
#define TRZ ((int32_t)c[7])
#define RBK ((int32_t)c[13])
#define GBK ((int32_t)c[14])
#define BBK ((int32_t)c[15])
#define RFC ((int32_t)c[21])
#define GFC ((int32_t)c[22])
#define BFC ((int32_t)c[23])
#define OFX ((int32_t)c[24])
#define OFY ((int32_t)c[25])
#define H (c[26] & 0xFFFF)
#define DQA ((int16_t)c[27])
#define DQB ((int32_t)c[28])
#define ZSF3 ((int16_t)c[29])
#define ZSF4 ((int16_t)c[30])

static int64_t A(int n, int64_t v) {
    if (v > 0x7FFFFFFFFFFLL) flag |= 1u << (31 - n);
    else if (v < -0x80000000000LL) flag |= 1u << (28 - n);
    return (int64_t)((uint64_t)v << 20) >> 20;
}

static int32_t LmB(int n, int32_t v, int lm) {
    int32_t lo = lm ? 0 : -0x8000;
    if (v < lo) { flag |= 1u << (25 - n); return lo; }
    if (v > 0x7FFF) { flag |= 1u << (25 - n); return 0x7FFF; }
    return v;
}

static void set_mac(int n, int64_t v, int shift) { d[24 + n] = (uint32_t)(int32_t)(v >> shift); }
static void set_ir(int n, int32_t v) { d[8 + n] = (uint32_t)(int32_t)(int16_t)v; }
#define MAC(n) ((int32_t)d[24 + (n)])

static int64_t F(int64_t v) {
    if (v > 0x7FFFFFFFLL) flag |= 1u << 16;
    else if (v < -0x80000000LL) flag |= 1u << 15;
    return v;
}

static uint32_t LmC(int n, int32_t v) {
    if (v < 0) { flag |= 1u << (22 - n); return 0; }
    if (v > 255) { flag |= 1u << (22 - n); return 255; }
    return (uint32_t)v;
}

static uint32_t LmD(int64_t v) {
    if (v < 0) { flag |= 1u << 18; return 0; }
    if (v > 0xFFFF) { flag |= 1u << 18; return 0xFFFF; }
    return (uint32_t)v;
}

static void color_push(void) {
    d[20] = d[21];
    d[21] = d[22];
    d[22] = LmC(1, MAC(1) >> 4) | (LmC(2, MAC(2) >> 4) << 8) | (LmC(3, MAC(3) >> 4) << 16) | (CODE << 24);
}

/* MAC1..3 from 64-bit values, then IR1..3 */
static void mac_ir(int64_t m1, int64_t m2, int64_t m3, int sf, int lm) {
    int sh = sf ? 12 : 0;
    set_mac(1, m1, sh); set_mac(2, m2, sh); set_mac(3, m3, sh);
    set_ir(1, LmB(1, MAC(1), lm));
    set_ir(2, LmB(2, MAC(2), lm));
    set_ir(3, LmB(3, MAC(3), lm));
}

static uint8_t unr_table[0x101];

static uint32_t gte_divide(uint32_t h, uint32_t sz3) {
    if (h >= sz3 * 2) { flag |= 1u << 17; return 0x1FFFF; }
    int z = 0;
    while (z < 16 && !(sz3 & (0x8000u >> z))) z++;
    uint64_t n = (uint64_t)h << z;
    uint32_t dd = sz3 << z;
    uint32_t u = unr_table[(dd - 0x7FC0) >> 7] + 0x101;
    dd = (0x2000080 - (dd * u)) >> 8;
    dd = (0x0000080 + (dd * u)) >> 8;
    uint64_t r = ((n * dd) + 0x8000) >> 16;
    return r > 0x1FFFF ? 0x1FFFF : (uint32_t)r;
}

/* matrix * vector + translation (m: 0 RT, 1 LLM, 2 LCM; t: translation vector or NULL) */
static void mvmva_core(int m, int32_t vx, int32_t vy, int32_t vz, const int32_t *t, int sf, int lm, int fc_bug) {
    int64_t out[3];
    for (int i = 0; i < 3; i++) {
        int64_t acc = t ? ((int64_t)t[i] << 12) : 0;
        if (fc_bug) {
            /* far-colour translation: the first product is computed (flags) and thrown away */
            int64_t junk = A(i + 1, acc + (int64_t)mtx(m, i, 0) * vx);
            set_ir(i + 1, LmB(i + 1, (int32_t)(junk >> (sf ? 12 : 0)), 0));
            acc = 0;
            acc = A(i + 1, acc + (int64_t)mtx(m, i, 1) * vy);
            acc = A(i + 1, acc + (int64_t)mtx(m, i, 2) * vz);
        } else {
            acc = A(i + 1, acc + (int64_t)mtx(m, i, 0) * vx);
            acc = A(i + 1, acc + (int64_t)mtx(m, i, 1) * vy);
            acc = A(i + 1, acc + (int64_t)mtx(m, i, 2) * vz);
        }
        out[i] = acc;
    }
    mac_ir(out[0], out[1], out[2], sf, lm);
}

static void rtp(int n, int sf, int last) {
    int32_t tr[3] = {TRX, TRY, TRZ};
    int64_t m[3];
    int32_t vx = VX(n), vy = VY(n), vz = VZ(n);
    for (int i = 0; i < 3; i++) {
        int64_t acc = (int64_t)tr[i] << 12;
        acc = A(i + 1, acc + (int64_t)mtx(0, i, 0) * vx);
        acc = A(i + 1, acc + (int64_t)mtx(0, i, 1) * vy);
        acc = A(i + 1, acc + (int64_t)mtx(0, i, 2) * vz);
        m[i] = acc;
    }
    int sh = sf ? 12 : 0;
    set_mac(1, m[0], sh); set_mac(2, m[1], sh); set_mac(3, m[2], sh);
    set_ir(1, LmB(1, MAC(1), 0));
    set_ir(2, LmB(2, MAC(2), 0));
    /* IR3 quirk: saturated from MAC3, flag from MAC3 >> 12 when sf=0 */
    {
        int32_t v = MAC(3);
        int32_t s = v < -0x8000 ? -0x8000 : (v > 0x7FFF ? 0x7FFF : v);
        int32_t fv = (int32_t)(m[2] >> 12);
        if (fv < -0x8000 || fv > 0x7FFF) flag |= 1u << 22;
        set_ir(3, s);
    }
    d[16] = d[17]; d[17] = d[18]; d[18] = d[19];
    d[19] = LmD(m[2] >> 12);
    uint32_t q = gte_divide(H, SZ(3));
    int64_t sx = F((int64_t)IR1 * q + OFX);
    int64_t sy = F((int64_t)IR2 * q + OFY);
    int32_t x = (int32_t)(sx >> 16), y = (int32_t)(sy >> 16);
    if (x < -0x400) { x = -0x400; flag |= 1u << 14; } else if (x > 0x3FF) { x = 0x3FF; flag |= 1u << 14; }
    if (y < -0x400) { y = -0x400; flag |= 1u << 13; } else if (y > 0x3FF) { y = 0x3FF; flag |= 1u << 13; }
    d[12] = d[13]; d[13] = d[14];
    d[14] = ((uint32_t)y << 16) | ((uint32_t)x & 0xFFFF);
    {   /* the same projection without the rounding (pgxp) */
        double vx = sf ? m[0] / 4096.0 : (double)m[0], vy = sf ? m[1] / 4096.0 : (double)m[1], vz = m[2] / 4096.0;
        int ok = vz > H / 2.0 && vz < 65535.0; /* clamped positions fail the 1-pixel check below */
        double qf = ok ? H * 65536.0 / vz : 0;
        float fx = (float)((vx * qf + (int32_t)OFX) / 65536.0), fy = (float)((vy * qf + (int32_t)OFY) / 65536.0);
        ok = ok && fabsf(fx - (float)x) < 1.0f && fabsf(fy - (float)y) < 1.0f;
        pg_push(fx, fy, (float)vz, ok);
    }
    if (last) {
        int64_t mac0 = F((int64_t)DQA * q + DQB);
        d[24] = (uint32_t)(int32_t)mac0;
        int32_t ir0 = (int32_t)(mac0 >> 12);
        if (ir0 < 0) { ir0 = 0; flag |= 1u << 12; } else if (ir0 > 0x1000) { ir0 = 0x1000; flag |= 1u << 12; }
        d[8] = (uint32_t)ir0;
    }
}

/* depth-cue interpolation towards the far colour; m = MAC values before the final shift */
static void interp(int64_t m1, int64_t m2, int64_t m3, int sf, int lm) {
    int sh = sf ? 12 : 0;
    int64_t fc[3] = {RFC, GFC, BFC}, mm[3] = {m1, m2, m3};
    for (int i = 0; i < 3; i++) {
        int64_t t = A(i + 1, (fc[i] << 12) - mm[i]) >> sh;
        set_ir(i + 1, LmB(i + 1, (int32_t)t, 0));
    }
    int32_t ir[3] = {IR1, IR2, IR3};
    for (int i = 0; i < 3; i++) mm[i] = A(i + 1, (int64_t)ir[i] * IR0 + mm[i]);
    mac_ir(mm[0], mm[1], mm[2], sf, lm);
    color_push();
}

static void light_color(int n, int sf, int lm, int mode) {
    /* mode: 0 NCS, 1 NCC, 2 NCD */
    mvmva_core(1, VX(n), VY(n), VZ(n), 0, sf, lm, 0);
    int32_t bk[3] = {RBK, GBK, BBK};
    mvmva_core(2, IR1, IR2, IR3, bk, sf, lm, 0);
    if (mode == 0) { color_push(); return; }
    int64_t m1 = ((int64_t)CR * IR1) << 4, m2 = ((int64_t)CG * IR2) << 4, m3 = ((int64_t)CB * IR3) << 4;
    if (mode == 1) { mac_ir(A(1, m1), A(2, m2), A(3, m3), sf, lm); color_push(); return; }
    interp(m1, m2, m3, sf, lm);
}

void gte_command(uint32_t cmd) {
    int sf = (cmd >> 19) & 1;
    int lm = (cmd >> 10) & 1;
    int sh = sf ? 12 : 0;
    flag = 0;
    switch (cmd & 0x3F) {
    case 0x01: rtp(0, sf, 1); break;
    case 0x30: rtp(0, sf, 0); rtp(1, sf, 0); rtp(2, sf, 1); break;
    case 0x06: {
        int64_t v = (int64_t)SX(0) * SY(1) + (int64_t)SX(1) * SY(2) + (int64_t)SX(2) * SY(0)
                  - (int64_t)SX(0) * SY(2) - (int64_t)SX(1) * SY(0) - (int64_t)SX(2) * SY(1);
        d[24] = (uint32_t)(int32_t)F(v);
        break;
    }
    case 0x0C: { /* OP */
        int32_t d1 = mtx(0, 0, 0), d2 = mtx(0, 1, 1), d3 = mtx(0, 2, 2);
        mac_ir(A(1, (int64_t)IR3 * d2 - (int64_t)IR2 * d3), A(2, (int64_t)IR1 * d3 - (int64_t)IR3 * d1),
               A(3, (int64_t)IR2 * d1 - (int64_t)IR1 * d2), sf, lm);
        break;
    }
    case 0x10: /* DPCS */
        interp((int64_t)CR << 16, (int64_t)CG << 16, (int64_t)CB << 16, sf, lm);
        break;
    case 0x2A: /* DPCT */
        for (int i = 0; i < 3; i++) {
            uint32_t c0 = d[20];
            interp((int64_t)(c0 & 0xFF) << 16, (int64_t)((c0 >> 8) & 0xFF) << 16, (int64_t)((c0 >> 16) & 0xFF) << 16, sf, lm);
        }
        break;
    case 0x11: /* INTPL */
        interp((int64_t)IR1 << 12, (int64_t)IR2 << 12, (int64_t)IR3 << 12, sf, lm);
        break;
    case 0x12: { /* MVMVA */
        int mx = (cmd >> 17) & 3, v = (cmd >> 15) & 3, cv = (cmd >> 13) & 3;
        int32_t vx, vy, vz;
        if (v == 3) { vx = IR1; vy = IR2; vz = IR3; } else { vx = VX(v); vy = VY(v); vz = VZ(v); }
        int32_t tr[3];
        const int32_t *t = 0;
        if (cv == 0) { tr[0] = TRX; tr[1] = TRY; tr[2] = TRZ; t = tr; }
        else if (cv == 1) { tr[0] = RBK; tr[1] = GBK; tr[2] = BBK; t = tr; }
        else if (cv == 2) { tr[0] = RFC; tr[1] = GFC; tr[2] = BFC; t = tr; }
        if (mx == 3) {
            /* garbage matrix: approximate with zeros except documented row values */
            int64_t r1 = -(int64_t)(CR << 4) * vx + (int64_t)(CR << 4) * vy + (int64_t)IR0 * vz;
            (void)r1;
            mac_ir(0, 0, 0, sf, lm);
        } else {
            mvmva_core(mx, vx, vy, vz, t, sf, lm, cv == 2);
        }
        break;
    }
    case 0x13: light_color(0, sf, lm, 2); break;                                  /* NCDS */
    case 0x16: for (int i = 0; i < 3; i++) light_color(i, sf, lm, 2); break;      /* NCDT */
    case 0x1B: light_color(0, sf, lm, 1); break;                                  /* NCCS */
    case 0x3F: for (int i = 0; i < 3; i++) light_color(i, sf, lm, 1); break;      /* NCCT */
    case 0x1E: light_color(0, sf, lm, 0); break;                                  /* NCS */
    case 0x20: for (int i = 0; i < 3; i++) light_color(i, sf, lm, 0); break;      /* NCT */
    case 0x14: { /* CDP */
        int32_t bk[3] = {RBK, GBK, BBK};
        mvmva_core(2, IR1, IR2, IR3, bk, sf, lm, 0);
        interp(((int64_t)CR * IR1) << 4, ((int64_t)CG * IR2) << 4, ((int64_t)CB * IR3) << 4, sf, lm);
        break;
    }
    case 0x1C: { /* CC */
        int32_t bk[3] = {RBK, GBK, BBK};
        mvmva_core(2, IR1, IR2, IR3, bk, sf, lm, 0);
        mac_ir(A(1, ((int64_t)CR * IR1) << 4), A(2, ((int64_t)CG * IR2) << 4), A(3, ((int64_t)CB * IR3) << 4), sf, lm);
        color_push();
        break;
    }
    case 0x29: /* DCPL */
        interp(((int64_t)CR * IR1) << 4, ((int64_t)CG * IR2) << 4, ((int64_t)CB * IR3) << 4, sf, lm);
        break;
    case 0x28: /* SQR */
        mac_ir((int64_t)IR1 * IR1, (int64_t)IR2 * IR2, (int64_t)IR3 * IR3, sf, lm);
        break;
    case 0x2D: { /* AVSZ3 */
        int64_t v = F((int64_t)ZSF3 * (int64_t)(SZ(1) + SZ(2) + SZ(3)));
        d[24] = (uint32_t)(int32_t)v;
        d[7] = LmD(v >> 12);
        break;
    }
    case 0x2E: { /* AVSZ4 */
        int64_t v = F((int64_t)ZSF4 * (int64_t)(SZ(0) + SZ(1) + SZ(2) + SZ(3)));
        d[24] = (uint32_t)(int32_t)v;
        d[7] = LmD(v >> 12);
        break;
    }
    case 0x3D: /* GPF */
        mac_ir(A(1, (int64_t)IR1 * IR0), A(2, (int64_t)IR2 * IR0), A(3, (int64_t)IR3 * IR0), sf, lm);
        color_push();
        break;
    case 0x3E: /* GPL */
        mac_ir(A(1, ((int64_t)MAC(1) << sh) + (int64_t)IR1 * IR0), A(2, ((int64_t)MAC(2) << sh) + (int64_t)IR2 * IR0),
               A(3, ((int64_t)MAC(3) << sh) + (int64_t)IR3 * IR0), sf, lm);
        color_push();
        break;
    default:
        LOG("GTE: unknown command %07X\n", cmd);
    }
    if (flag & 0x7F87E000u) flag |= 0x80000000u;
    c[31] = flag;
}

static uint32_t irgb(void) {
    uint32_t r = 0;
    for (int i = 0; i < 3; i++) {
        int32_t v = (int16_t)d[9 + i] >> 7;
        if (v < 0) v = 0;
        if (v > 0x1F) v = 0x1F;
        r |= (uint32_t)v << (5 * i);
    }
    return r;
}

uint32_t gte_read_data(int reg) {
    switch (reg) {
    case 1: case 3: case 5: case 8: case 9: case 10: case 11:
        return (uint32_t)(int32_t)(int16_t)d[reg];
    case 7: case 16: case 17: case 18: case 19:
        return d[reg] & 0xFFFF;
    case 15: return d[14];
    case 28: case 29: return irgb();
    case 31: {
        uint32_t v = d[30];
        int n = 0;
        uint32_t top = v >> 31;
        while (n < 32 && ((v >> (31 - n)) & 1) == top) n++;
        return (uint32_t)n;
    }
    default: return d[reg];
    }
}

void gte_write_data(int reg, uint32_t v) {
    switch (reg) {
    case 15: d[12] = d[13]; d[13] = d[14]; d[14] = v; pg_push(0, 0, 0, 0); break;
    case 12: case 13: case 14: d[reg] = v; pg_ok[reg - 12] = 0; break;
    case 28:
        d[28] = v & 0x7FFF;
        d[9] = (v & 0x1F) << 7;
        d[10] = ((v >> 5) & 0x1F) << 7;
        d[11] = ((v >> 10) & 0x1F) << 7;
        break;
    case 29: case 31: break;
    default: d[reg] = v;
    }
}

uint32_t gte_read_ctrl(int reg) {
    switch (reg) {
    case 4: case 12: case 20: case 26: case 27: case 29: case 30:
        return (uint32_t)(int32_t)(int16_t)c[reg];
    case 31: return c[31];
    default: return c[reg];
    }
}

void gte_write_ctrl(int reg, uint32_t v) {
    if (reg == 31) {
        v &= 0x7FFFF000u;
        if (v & 0x7F87E000u) v |= 0x80000000u;
    }
    c[reg] = v;
}

void gte_init(void) {
    for (int i = 0; i < 0x101; i++) {
        int v = (0x40000 / (i + 0x100) + 1) / 2 - 0x101;
        unr_table[i] = (uint8_t)(v < 0 ? 0 : v);
    }
}

/* ------------------------------------------------------------------ save states (state.c) */
#include "state.h"

void gte_state(StIO *io) {
    st_begin(io, "GTE", 1);
    ST(io, d); ST(io, c); ST(io, flag);
    st_end(io);
    if (io->load && !io->dry) { /* the precise vertices (off by default) start over */
        memset(pg_ok, 0, sizeof pg_ok);
        if (pg_mem) memset(pg_mem, 0, (PSX_RAM_SIZE / 4 + 256) * sizeof(PgWord));
        memset(pgxp_vcache, 0, sizeof pgxp_vcache);
    }
}
