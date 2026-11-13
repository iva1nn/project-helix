/* MDEC: run-length/IDCT macroblock decoder used for STR movies and the FE room backgrounds.
 * Implemented from the documented algorithm (nocash psx-spx "Macroblock Decoder"). */
#include <stdlib.h>
#include "hw.h"

static const uint8_t zigzag[64] = {
    0, 1, 5, 6, 14, 15, 27, 28, 2, 4, 7, 13, 16, 26, 29, 42, 3, 8, 12, 17, 25, 30, 41, 43, 9, 11, 18, 24, 31, 40, 44, 53,
    10, 19, 23, 32, 39, 45, 52, 54, 20, 22, 33, 38, 46, 51, 55, 60, 21, 34, 37, 47, 50, 56, 59, 61, 35, 36, 48, 49, 57, 58, 62, 63};
static uint8_t zagzig[64];

static struct {
    uint32_t cmd;
    int remaining;          /* parameter words still expected */
    uint32_t *in; int in_n, in_cap;
    uint8_t qt_y[64], qt_c[64];
    int16_t scale[64];
    uint32_t *out; int out_n, out_pos, out_cap;
    int enable_in, enable_out;
    int init;
} m;

void dma_mdec_out_ready(void);

static void out_push(uint32_t w) {
    if (m.out_n >= m.out_cap) {
        m.out_cap = m.out_cap ? m.out_cap * 2 : 65536;
        m.out = realloc(m.out, (size_t)m.out_cap * 4);
    }
    m.out[m.out_n++] = w;
}

static int sx10(int v) { return (v & 0x200) ? v - 0x400 : v; }

/* returns number of halfwords consumed, or -1 at end of data */
static int rl_block(int16_t *blk, const uint16_t *src, int avail, const uint8_t *qt) {
    int p = 0;
    for (int i = 0; i < 64; i++) blk[i] = 0;
    uint16_t n;
    do {
        if (p >= avail) return -1;
        n = src[p++];
    } while (n == 0xFE00);
    int q = (n >> 10) & 0x3F;
    int k = 0;
    int val = sx10(n & 0x3FF) * qt[0];
    for (;;) {
        if (q == 0) val = sx10(n & 0x3FF) * 2;
        if (val < -0x400) val = -0x400;
        if (val > 0x3FF) val = 0x3FF;
        if (q > 0) blk[zagzig[k]] = (int16_t)val;
        else blk[k] = (int16_t)val;
        if (p >= avail) break;
        n = src[p++];
        k += ((n >> 10) & 0x3F) + 1;
        if (k > 63) break;
        val = (sx10(n & 0x3FF) * qt[k] * q + 4) / 8;
    }
    /* IDCT, two passes */
    int64_t tmp[64];
    int64_t s1[64];
    for (int i = 0; i < 64; i++) s1[i] = blk[i];
    int64_t *src2 = s1, *dst = tmp;
    for (int pass = 0; pass < 2; pass++) {
        for (int x = 0; x < 8; x++)
            for (int y = 0; y < 8; y++) {
                int64_t sum = 0;
                for (int z = 0; z < 8; z++) sum += src2[y + z * 8] * (m.scale[x + z * 8] >> 3);
                dst[x + y * 8] = (sum + 0xFFF) >> 13;
            }
        int64_t *t = src2; src2 = dst; dst = t;
    }
    for (int i = 0; i < 64; i++) {
        int64_t v = src2[i];
        if (v < -128) v = -128;
        if (v > 127) v = 127;
        blk[i] = (int16_t)v;
    }
    return p;
}

static void decode(void) {
    int depth = (m.cmd >> 27) & 3;
    int is_signed = (m.cmd >> 26) & 1;
    int bit15 = (m.cmd >> 25) & 1;
    const uint16_t *src = (const uint16_t *)m.in;
    int avail = m.in_n * 2;
    int p = 0;
    int16_t cr[64], cb[64], y[4][64];
    while (p < avail) {
        if (depth >= 2) {
            int r;
            if ((r = rl_block(cr, src + p, avail - p, m.qt_c)) < 0) break; p += r;
            if ((r = rl_block(cb, src + p, avail - p, m.qt_c)) < 0) break; p += r;
            int ok = 1;
            for (int b = 0; b < 4 && ok; b++) {
                if ((r = rl_block(y[b], src + p, avail - p, m.qt_y)) < 0) ok = 0; else p += r;
            }
            if (!ok) break;
            /* colour conversion into a 16x16 macroblock */
            uint32_t px[256];
            for (int b = 0; b < 4; b++) {
                int xx = (b & 1) * 8, yy = (b >> 1) * 8;
                for (int j = 0; j < 8; j++)
                    for (int i = 0; i < 8; i++) {
                        int ci = ((i + xx) >> 1) + ((j + yy) >> 1) * 8;
                        double R = cr[ci], B = cb[ci];
                        double G = -0.3437 * B - 0.7143 * R;
                        R = 1.402 * R; B = 1.772 * B;
                        int Y = y[b][i + j * 8];
                        int ir = (int)(Y + R), ig = (int)(Y + G), ib = (int)(Y + B);
                        if (ir < -128) ir = -128; if (ir > 127) ir = 127;
                        if (ig < -128) ig = -128; if (ig > 127) ig = 127;
                        if (ib < -128) ib = -128; if (ib > 127) ib = 127;
                        uint32_t rgb = (uint32_t)(ir & 0xFF) | ((uint32_t)(ig & 0xFF) << 8) | ((uint32_t)(ib & 0xFF) << 16);
                        if (!is_signed) rgb ^= 0x808080;
                        px[(i + xx) + (j + yy) * 16] = rgb;
                    }
            }
            if (depth == 3) {
                for (int i = 0; i < 256; i += 2) {
                    uint32_t a = px[i], b2 = px[i + 1];
                    uint32_t ha = ((a >> 3) & 31) | (((a >> 11) & 31) << 5) | (((a >> 19) & 31) << 10) | (bit15 << 15);
                    uint32_t hb = ((b2 >> 3) & 31) | (((b2 >> 11) & 31) << 5) | (((b2 >> 19) & 31) << 10) | (bit15 << 15);
                    out_push(ha | (hb << 16));
                }
            } else {
                uint8_t bytes[768];
                for (int i = 0; i < 256; i++) {
                    bytes[i * 3] = (uint8_t)px[i];
                    bytes[i * 3 + 1] = (uint8_t)(px[i] >> 8);
                    bytes[i * 3 + 2] = (uint8_t)(px[i] >> 16);
                }
                for (int i = 0; i < 768; i += 4) {
                    uint32_t w; memcpy(&w, bytes + i, 4);
                    out_push(w);
                }
            }
        } else {
            int r;
            if ((r = rl_block(y[0], src + p, avail - p, m.qt_y)) < 0) break;
            p += r;
            uint8_t g8[64];
            for (int i = 0; i < 64; i++) {
                int v = y[0][i];
                if (!is_signed) v ^= 0x80;
                g8[i] = (uint8_t)v;
            }
            if (depth == 1) {
                for (int i = 0; i < 64; i += 4) {
                    uint32_t w; memcpy(&w, g8 + i, 4);
                    out_push(w);
                }
            } else {
                for (int i = 0; i < 64; i += 8) {
                    uint32_t w = 0;
                    for (int k = 0; k < 8; k++) w |= (uint32_t)(g8[i + k] >> 4) << (4 * k);
                    out_push(w);
                }
            }
        }
    }
}

static void in_push(uint32_t w) {
    if (m.in_n >= m.in_cap) {
        m.in_cap = m.in_cap ? m.in_cap * 2 : 65536;
        m.in = realloc(m.in, (size_t)m.in_cap * 4);
    }
    m.in[m.in_n++] = w;
}

static void finish_command(void) {
    int op = m.cmd >> 29;
    if (op == 1) {
        /* a new decode: drop output already read */
        if (m.out_pos >= m.out_n) { m.out_n = m.out_pos = 0; }
        int before = m.out_n;
        if (getenv("FE_MDEC_DUMP")) {
            static int nd;
            char p[64];
            snprintf(p, sizeof p, "out/mdec_in_%03d.bin", nd++);
            FILE *f = fopen(p, "wb");
            if (f) { fwrite(&m.cmd, 4, 1, f); fwrite(m.in, 4, (size_t)m.in_n, f); fclose(f); }
        }
        decode();
        DBG("MDEC decoded %d words from %d\n", m.out_n - before, m.in_n);
        dma_mdec_out_ready();
    } else if (op == 2) {
        const uint8_t *b = (const uint8_t *)m.in;
        memcpy(m.qt_y, b, 64);
        if (m.cmd & 1) memcpy(m.qt_c, b + 64, 64);
    } else if (op == 3) {
        memcpy(m.scale, m.in, 128);
    }
    m.in_n = 0;
}

static void start_command(uint32_t w) {
    DBG("MDEC command %08X\n", w);
    m.cmd = w;
    int op = w >> 29;
    m.in_n = 0;
    if (op == 1) m.remaining = w & 0xFFFF;
    else if (op == 2) m.remaining = (w & 1) ? 32 : 16;
    else if (op == 3) m.remaining = 32;
    else m.remaining = 0;
    if (m.remaining == 0) finish_command();
}

void mdec_dma_in(const uint32_t *src, int words) {
    for (int i = 0; i < words; i++) {
        if (m.remaining > 0) {
            in_push(src[i]);
            if (--m.remaining == 0) finish_command();
        } else {
            start_command(src[i]);
        }
    }
}

void mdec_dma_out(uint32_t *dst, int words) {
    for (int i = 0; i < words; i++) {
        dst[i] = m.out_pos < m.out_n ? m.out[m.out_pos++] : 0;
    }
    if (m.out_pos >= m.out_n) m.out_n = m.out_pos = 0;
}

int mdec_out_words(void) { return m.out_n - m.out_pos; }

uint32_t mdec_read(uint32_t addr) {
    if ((addr & 0xF) == 0) {
        uint32_t w = 0;
        mdec_dma_out(&w, 1);
        return w;
    }
    uint32_t s = 0;
    if (m.out_pos >= m.out_n) s |= 1u << 31;
    if (m.remaining > 0 && m.enable_in) s |= 1u << 28;
    if (m.out_pos < m.out_n && m.enable_out) s |= 1u << 27;
    s |= ((m.cmd >> 25) & 0xF) << 23;
    s |= (uint32_t)((m.remaining - 1) & 0xFFFF);
    return s;
}

void mdec_write(uint32_t addr, uint32_t v) {
    if (!m.init) {
        for (int i = 0; i < 64; i++) zagzig[zigzag[i]] = (uint8_t)i;
        m.init = 1;
    }
    if ((addr & 0xF) == 0) {
        mdec_dma_in(&v, 1);
        return;
    }
    if (v & 0x80000000u) {
        m.remaining = 0; m.in_n = 0; m.out_n = m.out_pos = 0; m.cmd = 0;
    }
    m.enable_in = (v >> 30) & 1;
    m.enable_out = (v >> 29) & 1;
}

/* ------------------------------------------------------------------ save states (state.c) */
#include "state.h"

void mdec_state(StIO *io) {
    int live = io->load && !io->dry;
    st_begin(io, "MDEC", 1);
    ST(io, m.cmd); ST(io, m.remaining); ST(io, m.qt_y); ST(io, m.qt_c); ST(io, m.scale);
    ST(io, m.enable_in); ST(io, m.enable_out); ST(io, m.init);
    int nin = st_count(io, m.in_n, 1 << 24);
    if (live && nin > m.in_cap) { m.in_cap = nin; m.in = realloc(m.in, (size_t)m.in_cap * 4); }
    for (int i = 0; i < nin; i++) {
        uint32_t w = io->load ? 0 : m.in[i];
        ST(io, w);
        if (live) m.in[i] = w;
    }
    if (live) m.in_n = nin;
    int nout = st_count(io, m.out_n - m.out_pos, 1 << 24); /* the words not read yet */
    if (live && nout > m.out_cap) { m.out_cap = nout; m.out = realloc(m.out, (size_t)m.out_cap * 4); }
    for (int i = 0; i < nout; i++) {
        uint32_t w = io->load ? 0 : m.out[m.out_pos + i];
        ST(io, w);
        if (live) m.out[i] = w;
    }
    if (live) { m.out_n = nout; m.out_pos = 0; }
    st_end(io);
    if (live) for (int i = 0; i < 64; i++) zagzig[zigzag[i]] = (uint8_t)i;
}
