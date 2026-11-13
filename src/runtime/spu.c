/* SPU: 24 ADPCM voices with ADSR envelopes, noise, pitch modulation, reverb, and XA-ADPCM CD audio.
 * Written from the documented hardware behaviour (nocash psx-spx "Sound Processing Unit").
 * Produces 44100 Hz stereo, one sample per 768 CPU cycles; the front end pulls the samples. */
#include <stdlib.h>
#include "hw.h"
#include "spu_gauss.h"

#define CYCLES_PER_SAMPLE 768
#define RAM_SIZE 0x80000

static uint16_t regs[0x200];
static uint8_t ram[RAM_SIZE];
static uint32_t xfer_addr;
static int64_t last_cycles;
static uint16_t endx_lo, endx_hi;

#define R(off) regs[(off) / 2]

/* ------------------------------------------------------------------ voices */

enum { ENV_OFF, ENV_ATTACK, ENV_DECAY, ENV_SUSTAIN, ENV_RELEASE };

typedef struct {
    uint32_t cur_addr;   /* byte address of the current ADPCM block */
    uint32_t rep_addr;
    uint32_t counter;    /* pitch counter, 12 fractional bits per sample */
    int16_t buf[28];     /* decoded samples of the current block */
    int16_t prev[3];     /* last samples of the previous block (interpolation) */
    int32_t hist0, hist1;
    int env_phase;
    int32_t env_level;
    int32_t env_wait;
    int out;             /* last output before volume (pitch modulation source) */
    int block_loaded;
} Voice;
static Voice v[24];

static const int32_t f0[5] = {0, 60, 115, 98, 122};
static const int32_t f1[5] = {0, 0, -52, -55, -60};

static int irq_enabled(void) { return (R(0x1AA) & 0x40) != 0; }

static void check_irq(uint32_t addr, uint32_t len) {
    if (!irq_enabled()) return;
    uint32_t ia = (uint32_t)R(0x1A4) << 3;
    if (ia >= addr && ia < addr + len) {
        if (!(R(0x1AE) & 0x40)) {
            R(0x1AE) |= 0x40;
            irq_raise(IRQ_SPU);
        }
    }
}

static void decode_block(Voice *vc, int n) {
    const uint8_t *b = &ram[vc->cur_addr & (RAM_SIZE - 1)];
    check_irq(vc->cur_addr & (RAM_SIZE - 1), 16);
    int shift = b[0] & 0x0F, filter = (b[0] >> 4) & 7;
    if (shift > 12) shift = 9;
    if (filter > 4) filter = 4;
    if (b[1] & 0x04) vc->rep_addr = vc->cur_addr;
    for (int i = 0; i < 28; i++) {
        int nib = (b[2 + i / 2] >> ((i & 1) * 4)) & 0xF;
        int32_t s = (int16_t)(nib << 12) >> shift;
        s += (vc->hist0 * f0[filter] + vc->hist1 * f1[filter] + 32) >> 6;
        if (s > 32767) s = 32767;
        if (s < -32768) s = -32768;
        vc->hist1 = vc->hist0;
        vc->hist0 = s;
        vc->buf[i] = (int16_t)s;
    }
    (void)n;
}

static uint64_t kon_count;
static void key_on(int n) {
    Voice *vc = &v[n];
    if (!kon_count++) DBG("SPU: first key on, voice %d addr %05X\n", n, (unsigned)R(n * 16 + 6) << 3);
    vc->cur_addr = (uint32_t)R(n * 16 + 6) << 3;
    vc->rep_addr = (uint32_t)R(n * 16 + 0xE) << 3;
    vc->counter = 0;
    vc->hist0 = vc->hist1 = 0;
    vc->prev[0] = vc->prev[1] = vc->prev[2] = 0;
    vc->env_phase = ENV_ATTACK;
    vc->env_level = 0;
    vc->env_wait = 0;
    vc->block_loaded = 0;
    if (n < 16) endx_lo &= ~(1u << n); else endx_hi &= ~(1u << (n - 16));
}

static void key_off(int n) {
    if (v[n].env_phase != ENV_OFF) { v[n].env_phase = ENV_RELEASE; v[n].env_wait = 0; }
}

/* one envelope step (psx-spx ADSR algorithm; volume sweeps use the same one) */
static void level_step(int32_t *level, int32_t *wait, int exp, int decrease, int shift, int step) {
    if (*wait > 0) { (*wait)--; return; }
    int cycles = 1 << (shift - 11 > 0 ? shift - 11 : 0);
    int32_t st = decrease ? (-8 + step) : (7 - step);
    st <<= (11 - shift > 0 ? 11 - shift : 0);
    if (exp && !decrease && *level > 0x6000) cycles *= 4;
    if (exp && decrease) st = (int32_t)(((int64_t)st * *level) >> 15);
    *wait = cycles - 1;
    *level += st;
    if (*level < 0) *level = 0;
    if (*level > 0x7FFF) *level = 0x7FFF;
}

static void env_step(Voice *vc, int exp, int decrease, int shift, int step) {
    level_step(&vc->env_level, &vc->env_wait, exp, decrease, shift, step);
}

static void envelope(int n) {
    Voice *vc = &v[n];
    uint16_t lo = R(n * 16 + 8), hi = R(n * 16 + 0xA);
    switch (vc->env_phase) {
    case ENV_ATTACK:
        env_step(vc, lo >> 15, 0, (lo >> 10) & 0x1F, (lo >> 8) & 3);
        if (vc->env_level >= 0x7FFF) { vc->env_phase = ENV_DECAY; vc->env_wait = 0; }
        break;
    case ENV_DECAY: {
        int32_t sl = ((lo & 0xF) + 1) * 0x800;
        env_step(vc, 1, 1, (lo >> 4) & 0xF, 0);
        if (vc->env_level <= sl) { vc->env_phase = ENV_SUSTAIN; vc->env_wait = 0; }
        break;
    }
    case ENV_SUSTAIN:
        env_step(vc, hi >> 15, (hi >> 14) & 1, (hi >> 8) & 0x1F, (hi >> 6) & 3);
        break;
    case ENV_RELEASE:
        env_step(vc, (hi >> 5) & 1, 1, hi & 0x1F, 0);
        if (vc->env_level <= 0) vc->env_phase = ENV_OFF;
        break;
    default:
        vc->env_level = 0;
    }
    R(n * 16 + 0xC) = (uint16_t)vc->env_level;
}

/* a volume register (voice L/R, main L/R), stepped once per sample: fixed, or a sweep that moves the
 * level like an envelope from where it is (bit 14 exponential, 13 decrease, 12 negative phase) */
typedef struct { int32_t level, wait; } Sweep;
static Sweep sweeps[25][2];

static int32_t vol(uint16_t r, Sweep *sw) {
    if (!(r & 0x8000)) { sw->level = (int16_t)(r << 1); sw->wait = 0; return sw->level; }
    int32_t lv = sw->level < 0 ? -sw->level : sw->level;
    level_step(&lv, &sw->wait, (r >> 14) & 1, (r >> 13) & 1, (r >> 2) & 0x1F, r & 3);
    sw->level = (r & 0x1000) ? -lv : lv;
    return sw->level;
}

static int32_t noise_level = 1;
static int32_t noise_count;

static void noise_tick(void) {
    uint16_t cnt = R(0x1AA);
    int shift = (cnt >> 10) & 0xF, step = ((cnt >> 8) & 3) + 4;
    noise_count += step;
    if (noise_count < (0x20000 >> shift)) return;
    noise_count &= (0x20000 >> shift) - 1;
    int parity = ((noise_level >> 15) ^ (noise_level >> 12) ^ (noise_level >> 11) ^ (noise_level >> 10) ^ 1) & 1;
    noise_level = ((noise_level << 1) | parity) & 0xFFFF;
}

static int32_t voice_sample(int n, int32_t prev_out) {
    Voice *vc = &v[n];
    if (vc->env_phase == ENV_OFF) { vc->out = 0; return 0; }
    if (!vc->block_loaded) { decode_block(vc, n); vc->block_loaded = 1; }
    /* 4-point Gaussian interpolation (the hardware's), weights by bits 4-11 of the pitch counter */
    int idx = (int)(vc->counter >> 12);
    int i = (int)((vc->counter >> 4) & 0xFF);
#define SMP(k) ((k) < 0 ? vc->prev[3 + (k)] : vc->buf[k])
    int32_t s = (gauss[0xFF - i] * SMP(idx - 3) + gauss[0x1FF - i] * SMP(idx - 2) + gauss[0x100 + i] * SMP(idx - 1) +
                 gauss[i] * SMP(idx)) >> 15;
#undef SMP
    /* noise replaces the sample */
    uint32_t non = R(0x194) | ((uint32_t)R(0x196) << 16);
    if (non & (1u << n)) s = (int16_t)noise_level;
    s = (s * vc->env_level) >> 15;
    vc->out = s;
    /* advance pitch counter */
    int32_t step = R(n * 16 + 4);
    uint32_t pmon = R(0x190) | ((uint32_t)R(0x192) << 16);
    if (n > 0 && (pmon & (1u << n))) step = (step * (prev_out + 0x8000)) >> 15;
    if (step > 0x3FFF) step = 0x3FFF;
    if (step < 0) step = 0;
    vc->counter += (uint32_t)step;
    while ((vc->counter >> 12) >= 28) {
        vc->counter -= 28 << 12;
        vc->prev[0] = vc->buf[25];
        vc->prev[1] = vc->buf[26];
        vc->prev[2] = vc->buf[27];
        const uint8_t *b = &ram[vc->cur_addr & (RAM_SIZE - 1)];
        uint8_t flags = b[1];
        if (flags & 0x01) {
            if (n < 16) endx_lo |= 1u << n; else endx_hi |= 1u << (n - 16);
            if (flags & 0x02) {
                vc->cur_addr = vc->rep_addr;
            } else {
                vc->env_phase = ENV_OFF;
                vc->env_level = 0;
                vc->cur_addr += 16;
            }
        } else {
            vc->cur_addr += 16;
        }
        vc->cur_addr &= RAM_SIZE - 1;
        decode_block(vc, n);
    }
    return s;
}

/* ------------------------------------------------------------------ reverb */

static uint32_t rev_addr;
static int rev_phase;
static int32_t rev_out_l, rev_out_r;

static uint32_t rev_index(int32_t off) {
    uint32_t base = (uint32_t)R(0x1A2) << 3;
    if (base >= RAM_SIZE) return 0;
    uint32_t size = RAM_SIZE - base;
    int64_t a = (int64_t)rev_addr - base + off;
    a %= (int64_t)size;
    if (a < 0) a += size;
    return (uint32_t)(base + a) & (RAM_SIZE - 2);
}
static int32_t rv(int32_t off) { int16_t x; memcpy(&x, &ram[rev_index(off)], 2); return x; }
static void wv(int32_t off, int32_t val) {
    if (val > 32767) val = 32767;
    if (val < -32768) val = -32768;
    int16_t x = (int16_t)val;
    memcpy(&ram[rev_index(off)], &x, 2);
}
#define RA(o) ((int32_t)R(o) << 3)
#define RVOL(o) ((int32_t)(int16_t)R(o))
static int32_t mulv(int32_t a, int32_t b) { return (a * b) >> 15; }

static void reverb(int32_t in_l, int32_t in_r) {
    if (!(R(0x1AA) & 0x80)) { rev_out_l = rev_out_r = 0; return; }
    if (rev_phase ^= 1) return; /* 22050 Hz */
    int32_t vIIR = RVOL(0x1C4), vWALL = RVOL(0x1CE);
    int32_t Lin = mulv(RVOL(0x1FC), in_l), Rin = mulv(RVOL(0x1FE), in_r);
    int32_t mLSAME = RA(0x1D4), mRSAME = RA(0x1D6), dLSAME = RA(0x1E0), dRSAME = RA(0x1E2);
    int32_t mLDIFF = RA(0x1E4), mRDIFF = RA(0x1E6), dLDIFF = RA(0x1F0), dRDIFF = RA(0x1F2);
    wv(mLSAME, mulv(Lin + mulv(rv(dLSAME), vWALL) - rv(mLSAME - 2), vIIR) + rv(mLSAME - 2));
    wv(mRSAME, mulv(Rin + mulv(rv(dRSAME), vWALL) - rv(mRSAME - 2), vIIR) + rv(mRSAME - 2));
    wv(mLDIFF, mulv(Lin + mulv(rv(dRDIFF), vWALL) - rv(mLDIFF - 2), vIIR) + rv(mLDIFF - 2));
    wv(mRDIFF, mulv(Rin + mulv(rv(dLDIFF), vWALL) - rv(mRDIFF - 2), vIIR) + rv(mRDIFF - 2));
    int32_t c1 = RVOL(0x1C6), c2 = RVOL(0x1C8), c3 = RVOL(0x1CA), c4 = RVOL(0x1CC);
    int32_t L = mulv(c1, rv(RA(0x1D8))) + mulv(c2, rv(RA(0x1DC))) + mulv(c3, rv(RA(0x1E8))) + mulv(c4, rv(RA(0x1EC)));
    int32_t Rr = mulv(c1, rv(RA(0x1DA))) + mulv(c2, rv(RA(0x1DE))) + mulv(c3, rv(RA(0x1EA))) + mulv(c4, rv(RA(0x1EE)));
    int32_t dAPF1 = RA(0x1C0), dAPF2 = RA(0x1C2), vAPF1 = RVOL(0x1D0), vAPF2 = RVOL(0x1D2);
    int32_t mLAPF1 = RA(0x1F4), mRAPF1 = RA(0x1F6), mLAPF2 = RA(0x1F8), mRAPF2 = RA(0x1FA);
    L = L - mulv(vAPF1, rv(mLAPF1 - dAPF1)); wv(mLAPF1, L); L = mulv(L, vAPF1) + rv(mLAPF1 - dAPF1);
    Rr = Rr - mulv(vAPF1, rv(mRAPF1 - dAPF1)); wv(mRAPF1, Rr); Rr = mulv(Rr, vAPF1) + rv(mRAPF1 - dAPF1);
    L = L - mulv(vAPF2, rv(mLAPF2 - dAPF2)); wv(mLAPF2, L); L = mulv(L, vAPF2) + rv(mLAPF2 - dAPF2);
    Rr = Rr - mulv(vAPF2, rv(mRAPF2 - dAPF2)); wv(mRAPF2, Rr); Rr = mulv(Rr, vAPF2) + rv(mRAPF2 - dAPF2);
    rev_out_l = mulv(L, RVOL(0x184));
    rev_out_r = mulv(Rr, RVOL(0x186));
    uint32_t base = (uint32_t)R(0x1A2) << 3;
    rev_addr = (rev_addr + 2) & (RAM_SIZE - 2);
    if (rev_addr < base) rev_addr = base;
}

/* ------------------------------------------------------------------ XA-ADPCM (CD audio) */

#define XA_BUF 65536
static int16_t xa_l[XA_BUF], xa_r[XA_BUF];
static uint32_t xa_w, xa_r_pos; /* ring positions in 44100 Hz samples */
static int32_t xa_hist[2][2];
static uint32_t xa_frac;
static int16_t xa_last_l, xa_last_r;

static void xa_push(int16_t l, int16_t r) {
    if (xa_w - xa_r_pos >= XA_BUF - 1) return; /* full: drop */
    xa_l[xa_w % XA_BUF] = l; xa_r[xa_w % XA_BUF] = r; xa_w++;
}

void spu_xa_sector(const uint8_t *s) {
    static const int32_t k0[4] = {0, 60, 115, 98}, k1[4] = {0, 0, -52, -55};
    uint8_t ci = s[19];
    int stereo = (ci & 3) == 1;
    int rate = ((ci >> 2) & 3) ? 18900 : 37800;
    int bits8 = ((ci >> 4) & 3) == 1;
    int16_t out[2][4032];
    int nout[2] = {0, 0};
    const uint8_t *d = s + 24;
    for (int g = 0; g < 18; g++) {
        const uint8_t *grp = d + g * 128;
        int units = bits8 ? 4 : 8;
        for (int u = 0; u < units; u++) {
            uint8_t param = grp[4 + u];
            int shift = param & 0xF, filter = (param >> 4) & 3;
            if (shift > 12) shift = 9;
            int ch = stereo ? (u & 1) : 0;
            for (int i = 0; i < 28; i++) {
                int32_t smp;
                if (bits8) smp = (int16_t)(grp[16 + i * 4 + u] << 8) >> shift;
                else {
                    uint8_t byte = grp[16 + i * 4 + (u >> 1)];
                    int nib = (u & 1) ? byte >> 4 : byte & 0xF;
                    smp = (int16_t)(nib << 12) >> shift;
                }
                smp += (xa_hist[ch][0] * k0[filter] + xa_hist[ch][1] * k1[filter] + 32) >> 6;
                if (smp > 32767) smp = 32767;
                if (smp < -32768) smp = -32768;
                xa_hist[ch][1] = xa_hist[ch][0];
                xa_hist[ch][0] = smp;
                if (nout[ch] < 4032) out[ch][nout[ch]++] = (int16_t)smp;
            }
        }
    }
    int n = stereo ? (nout[0] < nout[1] ? nout[0] : nout[1]) : nout[0];
    /* resample to 44100 Hz (linear) */
    uint32_t step = (uint32_t)(((uint64_t)rate << 16) / 44100);
    for (;;) {
        uint32_t i = xa_frac >> 16;
        if ((int)i >= n) break;
        uint32_t f = xa_frac & 0xFFFF;
        int16_t l1 = out[0][i], r1 = stereo ? out[1][i] : l1;
        int16_t l0 = i ? out[0][i - 1] : xa_last_l, r0 = i ? (stereo ? out[1][i - 1] : out[0][i - 1]) : xa_last_r;
        xa_push((int16_t)(l0 + (((l1 - l0) * (int32_t)f) >> 16)), (int16_t)(r0 + (((r1 - r0) * (int32_t)f) >> 16)));
        xa_frac += step;
    }
    xa_frac -= (uint32_t)n << 16;
    if (n) { xa_last_l = out[0][n - 1]; xa_last_r = stereo ? out[1][n - 1] : out[0][n - 1]; }
}

/* ------------------------------------------------------------------ output */

#define OUT_BUF 32768
static int16_t outbuf[OUT_BUF * 2];
static uint32_t out_w, out_r;

int spu_take_samples(int16_t *dst, int max_frames) {
    int n = 0;
    while (out_r != out_w && n < max_frames) {
        dst[n * 2] = outbuf[(out_r % OUT_BUF) * 2];
        dst[n * 2 + 1] = outbuf[(out_r % OUT_BUF) * 2 + 1];
        out_r++; n++;
    }
    return n;
}

static int32_t clamp16(int32_t x) { return x > 32767 ? 32767 : x < -32768 ? -32768 : x; }

static void mix_sample(void) {
    uint16_t cnt = R(0x1AA);
    int32_t L = 0, Rr = 0, revL = 0, revR = 0;
    uint32_t eon = R(0x198) | ((uint32_t)R(0x19A) << 16);
    noise_tick();
    int32_t prev = 0;
    for (int n = 0; n < 24; n++) {
        envelope(n);
        int32_t s = voice_sample(n, prev);
        prev = s;
        int32_t l = (s * vol(R(n * 16), &sweeps[n][0])) >> 15, r = (s * vol(R(n * 16 + 2), &sweeps[n][1])) >> 15;
        L += l; Rr += r;
        if (eon & (1u << n)) { revL += l; revR += r; }
    }
    /* CD audio (XA) */
    int32_t cdl = 0, cdr = 0;
    if (xa_r_pos != xa_w) {
        cdl = xa_l[xa_r_pos % XA_BUF]; cdr = xa_r[xa_r_pos % XA_BUF]; xa_r_pos++;
    }
    if (cnt & 0x0001) {
        cdl = (cdl * (int16_t)R(0x1B0)) >> 15;
        cdr = (cdr * (int16_t)R(0x1B2)) >> 15;
        L += cdl; Rr += cdr;
        if (cnt & 0x0004) { revL += cdl; revR += cdr; }
    }
    reverb(clamp16(revL), clamp16(revR));
    if (!(cnt & 0x4000)) { L = 0; Rr = 0; } /* muted */
    L = (L * vol(R(0x180), &sweeps[24][0])) >> 15;
    Rr = (Rr * vol(R(0x182), &sweeps[24][1])) >> 15;
    L += rev_out_l; Rr += rev_out_r;
    if (!(cnt & 0x8000)) { L = 0; Rr = 0; } /* SPU disabled */
    outbuf[(out_w % OUT_BUF) * 2] = (int16_t)clamp16(L);
    outbuf[(out_w % OUT_BUF) * 2 + 1] = (int16_t)clamp16(Rr);
    out_w++;
    if (out_w - out_r > OUT_BUF) out_r = out_w - OUT_BUF;
}

void spu_update(void) {
    while (cpu.cycles - last_cycles >= CYCLES_PER_SAMPLE) {
        last_cycles += CYCLES_PER_SAMPLE;
        mix_sample();
    }
}

/* ------------------------------------------------------------------ registers */

uint16_t spu_read(uint32_t addr) {
    int r = (addr - 0x1F801C00) >> 1 & 0x1FF;
    switch (r) {
    case 0x1AE / 2: {
        uint16_t c = R(0x1AA);
        uint16_t s = (c & 0x3F) | (R(0x1AE) & 0x40);
        int mode = (c >> 4) & 3;
        if (mode == 2) s |= 0x80 | 0x100;
        else if (mode == 3) s |= 0x80 | 0x200;
        return s;
    }
    case 0x1A6 / 2: return (uint16_t)(xfer_addr >> 3);
    case 0x19C / 2: return endx_lo;
    case 0x19E / 2: return endx_hi;
    case 0x1B8 / 2: return R(0x180);
    case 0x1BA / 2: return R(0x182);
    default:
        if (r < 0x180 / 2 && (r & 7) == 6) return (uint16_t)v[r >> 3].env_level;
        if (r < 0x180 / 2 && (r & 7) == 7) return (uint16_t)(v[r >> 3].rep_addr >> 3);
        return regs[r];
    }
}

void spu_write(uint32_t addr, uint16_t val) {
    int r = (addr - 0x1F801C00) >> 1 & 0x1FF;
    spu_update();
    switch (r * 2) {
    case 0x188: regs[r] = val; for (int n = 0; n < 16; n++) if (val & (1u << n)) key_on(n); return;
    case 0x18A: regs[r] = val; for (int n = 0; n < 8; n++) if (val & (1u << n)) key_on(16 + n); return;
    case 0x18C: regs[r] = val; for (int n = 0; n < 16; n++) if (val & (1u << n)) key_off(n); return;
    case 0x18E: regs[r] = val; for (int n = 0; n < 8; n++) if (val & (1u << n)) key_off(16 + n); return;
    case 0x19C: case 0x19E: return; /* ENDX is read-only */
    case 0x1A2: regs[r] = val; rev_addr = (uint32_t)val << 3; return;
    case 0x1A6: regs[r] = val; xfer_addr = (uint32_t)val << 3; return;
    case 0x1A8:
        check_irq(xfer_addr, 2);
        ram[xfer_addr & (RAM_SIZE - 1)] = (uint8_t)val;
        ram[(xfer_addr + 1) & (RAM_SIZE - 1)] = (uint8_t)(val >> 8);
        xfer_addr = (xfer_addr + 2) & (RAM_SIZE - 1);
        return;
    case 0x1AA:
        regs[r] = val;
        if (!(val & 0x40)) R(0x1AE) &= ~0x40; /* IRQ acknowledged by disabling it */
        return;
    case 0x1AE: return;
    }
    regs[r] = val;
    if (r < 0x180 / 2 && (r & 7) == 7) v[r >> 3].rep_addr = (uint32_t)val << 3;
    if (r < 0x180 / 2 && (r & 7) == 6) v[r >> 3].env_level = val;
}

void spu_dma_write(const uint32_t *src, int words) {
    check_irq(xfer_addr, (uint32_t)words * 4);
    for (int i = 0; i < words; i++) {
        memcpy(&ram[xfer_addr & (RAM_SIZE - 4)], &src[i], 4);
        xfer_addr = (xfer_addr + 4) & (RAM_SIZE - 1);
    }
}

void spu_dma_read(uint32_t *dst, int words) {
    for (int i = 0; i < words; i++) {
        memcpy(&dst[i], &ram[xfer_addr & (RAM_SIZE - 4)], 4);
        xfer_addr = (xfer_addr + 4) & (RAM_SIZE - 1);
    }
}

/* ------------------------------------------------------------------ save states (state.c) */
#include "state.h"

void spu_state(StIO *io) {
    st_begin(io, "SPU", 1);
    ST(io, regs); st_raw(io, ram, sizeof ram);
    ST(io, xfer_addr); ST(io, last_cycles); ST(io, endx_lo); ST(io, endx_hi);
    ST(io, v); ST(io, kon_count); ST(io, sweeps); ST(io, noise_level); ST(io, noise_count);
    ST(io, rev_addr); ST(io, rev_phase); ST(io, rev_out_l); ST(io, rev_out_r);
    ST(io, xa_l); ST(io, xa_r); ST(io, xa_w); ST(io, xa_r_pos); ST(io, xa_hist); ST(io, xa_frac);
    ST(io, xa_last_l); ST(io, xa_last_r);
    st_end(io);
}

/* the samples made before the load are not played */
void spu_state_loaded(void) { out_r = out_w; }
