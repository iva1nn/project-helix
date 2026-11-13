/* CPU state, memory map, interrupt controller, scheduler, DMA, timers, dispatcher. */
#include <stdarg.h>
#include <stdlib.h>
#include "hw.h"
#include "models.h"
#include "props.h"
#include "state.h"

Cpu cpu;
uint8_t psx_ram[PSX_RAM_SIZE];
uint8_t psx_scratch[0x400];
uint32_t i_stat, i_mask;
FILE *log_file;
int log_level = 1;

void logf_(int level, const char *fmt, ...) {
    (void)level;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(log_file ? log_file : stderr, fmt, ap);
    va_end(ap);
    if (log_file) fflush(log_file);
}

static void dump_regs(FILE *f) {
    for (int i = 0; i < 32; i++)
        fprintf(f, "r%-2d=%08X%s", i, cpu.r[i], (i & 3) == 3 ? "\n" : "  ");
    fprintf(f, "cycles=%lld frame=%llu sr=%08X\n", (long long)cpu.cycles, (unsigned long long)frame_count, cpu.sr);
}

void fatal(const char *fmt, ...) {
    va_list ap;
    FILE *f = log_file ? log_file : stderr;
    va_start(ap, fmt);
    fprintf(f, "FATAL: ");
    vfprintf(f, fmt, ap);
    va_end(ap);
    fprintf(f, "\n");
    dump_regs(f);
    fprintf(f, "last functions entered (oldest first):");
    for (int i = 0; i < 64; i++) {
        uint16_t k = fn_trail[(fn_trail_pos + i) & 63];
        fprintf(f, "%s%s:%08X", i % 6 ? " " : "\n  ", fn_image[k], fn_addr[k]);
    }
    fprintf(f, "\nshadow stack:");
    for (int i = rs_top - 1; i >= 0 && i >= rs_top - 12; i--) fprintf(f, " %08X", rs_stack[i]);
    fprintf(f, "\n");
    fflush(f);
    fprintf(stderr, "Input of this session: build/last_session.rec (replay: --replay <file> --headless)\n");
    FILE *rf = fopen("out/ram_at_fatal.bin", "wb");
    if (rf) { fwrite(psx_ram, 1, PSX_RAM_SIZE, rf); fclose(rf); }
    if (f != stderr) {
        va_start(ap, fmt);
        fprintf(stderr, "FATAL: ");
        vfprintf(stderr, fmt, ap);
        fprintf(stderr, "\n");
        va_end(ap);
    }
    exit(1);
}

/* ------------------------------------------------------------------ scheduler */

static int64_t ev_hblank;      /* next scanline boundary */
static int scanline;
uint64_t frame_count;

void sched_at(int64_t when) {
    if (when < cpu.next_event) cpu.next_event = when;
}

void irq_raise(int n) {
    i_stat |= 1u << n;
    sched_at(cpu.cycles);
}

int in_exception;

void irq_check(void) {
    if ((i_stat & i_mask) && (cpu.sr & 1) && (cpu.sr & 0x400) && !in_exception)
        bios_exception();
}

void dma2_flush(void);
static int dma2_pending;
static int64_t dma2_due;

void hw_events(void) {
    int64_t now = cpu.cycles;
    cpu.next_event = now + 1000000;
    if (dma2_pending && now >= dma2_due) dma2_flush();
    if (dma2_pending) sched_at(dma2_due);
    int lines = 0;
    while (now >= ev_hblank) {
        ev_hblank += CYCLES_PER_LINE;
        lines++;
        if (++scanline >= LINES_PER_FRAME) scanline = 0;
        if (scanline == 240) { /* vblank start */
            frame_count++;
            if (frame_count % 60 == 0) DBG("frame %llu\n", (unsigned long long)frame_count);
            dma2_flush(); /* the frame's drawing is done by the time it is shown */
            gpu_vblank();
            irq_raise(IRQ_VBLANK);
            bios_vblank();
            pad_vblank();
            game_vblank();
            controls_vblank();
            cheats_vblank();
            { void agent_vblank(void); agent_vblank(); }
            if (!frontend_vblank()) exit(0);
            if (opt_max_frames && (int64_t)frame_count >= opt_max_frames) {
                LOG("frame limit reached\n");
                const char *rp = getenv("FE_RAM_END"); /* where to write the RAM at the frame limit */
                FILE *rf = fopen(rp ? rp : "out/ram_end.bin", "wb");
                if (rf) { fwrite(psx_ram, 1, PSX_RAM_SIZE, rf); fclose(rf); }
                exit(0);
            }
        }
    }
    if (lines) timers_hblank(lines);
    sched_at(ev_hblank);
    timers_update();
    cdrom_update();
    sio_update();
    spu_update();
    bios_update();
    irq_check();
}

/* ------------------------------------------------------------------ COP0 */

uint32_t cop0_read(int reg) {
    switch (reg) {
    case 12: return cpu.sr;
    case 13: return cpu.cause | ((i_stat & i_mask) ? 0x400 : 0);
    case 14: return cpu.epc;
    case 15: return 2; /* PRId */
    default: return 0;
    }
}

void cop0_write(int reg, uint32_t v) {
    if (reg == 12) {
        cpu.sr = v;
        sched_at(cpu.cycles);
    } else if (reg == 13) {
        cpu.cause = (cpu.cause & ~0x300u) | (v & 0x300u);
    }
}

void cop0_rfe(void) {
    cpu.sr = (cpu.sr & ~0x0Fu) | ((cpu.sr >> 2) & 0x0F);
    sched_at(cpu.cycles);
}

/* ------------------------------------------------------------------ DMA */

static uint32_t dma_madr[7], dma_bcr[7], dma_chcr[7], dpcr = 0x07654321, dicr;
static int dma1_pending;
int mdec_out_words(void);
static void dma_run(int ch);

/* GPU linked-list DMA finishes later, as on the console: the GPU reads the list while it draws, and both games
 * add their last HUD primitives (FE2's item box lines, 2-3k cycles after DrawOTag) to the list already started,
 * which the console still picks up. So the list is walked when its estimated drawing time is over, or earlier
 * when the CPU talks to the GPU again (GP0/GP1, GPUREAD, another GPU DMA), keeping the order. */
int dma2_busy(void) { return dma2_pending; }

void dma2_flush(void) {
    if (!dma2_pending) return;
    dma2_pending = 0;
    dma_run(2);
}

static void dma2_defer(void) {
    uint32_t a = dma_madr[2] & 0x1FFFFC, words = 0;
    for (int guard = 0; guard < 1000000; guard++) { /* words in the list, for the drawing time */
        uint32_t hdr;
        memcpy(&hdr, &psx_ram[a], 4);
        words += hdr >> 24;
        if (hdr & 0x800000) break;
        a = hdr & 0x1FFFFC;
    }
    dma2_pending = 1;
    dma2_due = cpu.cycles + 8 * (int64_t)words; /* short lists finish at once: a longer wait made libgpu queue
                                                    * the next LoadImage until its source RAM was reused (FE1 font) */
    sched_at(dma2_due);
}

void dma_mdec_out_ready(void) {
    if (dma1_pending && (dma_chcr[1] & (1u << 24))) dma_run(1);
}

static void dma_irq(int ch) {
    if (dicr & (1u << (16 + ch))) {
        dicr |= 1u << (24 + ch);
    }
    uint32_t flags = (dicr >> 24) & 0x7F, en = (dicr >> 16) & 0x7F;
    int master = (dicr & 0x8000) || ((dicr & 0x800000) && (flags & en));
    DBG("DMA%d done: dicr=%08X%s\n", ch, dicr, master && !(dicr & 0x80000000u) ? " IRQ" : "");
    if (master) {
        if (!(dicr & 0x80000000u)) irq_raise(IRQ_DMA);
        dicr |= 0x80000000u;
    } else {
        dicr &= ~0x80000000u;
    }
}

static void dma_run(int ch) {
    uint32_t chcr = dma_chcr[ch];
    uint32_t addr = dma_madr[ch] & 0x1FFFFC;
    DBG("DMA%d madr=%06X bcr=%08X chcr=%08X\n", ch, dma_madr[ch], dma_bcr[ch], chcr);
    int sync = (chcr >> 9) & 3;
    int to_ram = !(chcr & 1);
    int step = (chcr & 2) ? -4 : 4;
    uint32_t words;
    if (sync == 0) {
        words = dma_bcr[ch] & 0xFFFF;
        if (!words) words = 0x10000;
    } else if (sync == 1) {
        words = (dma_bcr[ch] & 0xFFFF) * (dma_bcr[ch] >> 16);
    } else {
        words = 0;
    }
    switch (ch) {
    case 0: /* MDEC in */
        for (uint32_t i = 0; i < words; i++, addr = (addr + step) & 0x1FFFFC) {
            uint32_t w; memcpy(&w, &psx_ram[addr], 4);
            mdec_dma_in(&w, 1);
        }
        break;
    case 1: /* MDEC out: waits until the decoder has the data */
        if (mdec_out_words() < (int)words) { dma1_pending = 1; return; }
        dma1_pending = 0;
        for (uint32_t i = 0; i < words; i++, addr = (addr + step) & 0x1FFFFC) {
            uint32_t w; mdec_dma_out(&w, 1);
            memcpy(&psx_ram[addr], &w, 4);
        }
        break;
    case 2: /* GPU */
        if (sync == 2) {
            /* linked list */
            uint32_t a = addr;
            int guard = 0;
            models_dma_serial++;
            models_dma_begin(addr);
            for (;;) {
                uint32_t hdr; memcpy(&hdr, &psx_ram[a & 0x1FFFFC], 4);
                uint32_t n = hdr >> 24;
                if (n) gpu_hw_skip = models_packet(a); /* a remaster mesh replaces this model's packets */
                if (n) menu_gpu_packet(a);            /* FE2's controls screen is known by its panel */
                if (n && props_dbg) props_packet(a);  /* FE_DBG_PROPS: the prop inventory */
                for (uint32_t i = 0; i < n; i++) {
                    uint32_t w; memcpy(&w, &psx_ram[(a + 4 + i * 4) & 0x1FFFFC], 4);
                    gpu_word_addr = (a + 4 + i * 4) & 0x1FFFFC; /* for the precise vertices (pgxp) */
                    gpu_gp0(w);
                }
                gpu_word_addr = 0xFFFFFFFFu;
                gpu_hw_skip = 0;
                if (hdr & 0x800000) break;
                a = hdr & 0x1FFFFC;
                if (++guard > 1000000) { LOG("GPU DMA: linked list loop\n"); break; }
            }
            dma_madr[ch] = 0x00FFFFFF;
        } else if (to_ram) {
            for (uint32_t i = 0; i < words; i++, addr = (addr + step) & 0x1FFFFC) {
                uint32_t w = gpu_read();
                memcpy(&psx_ram[addr], &w, 4);
            }
        } else {
            for (uint32_t i = 0; i < words; i++, addr = (addr + step) & 0x1FFFFC) {
                uint32_t w; memcpy(&w, &psx_ram[addr], 4);
                gpu_gp0(w);
            }
        }
        break;
    case 3: /* CD-ROM */
        for (uint32_t i = 0; i < words; i++, addr = (addr + step) & 0x1FFFFC) {
            uint32_t w = cdrom_dma_read();
            memcpy(&psx_ram[addr], &w, 4);
        }
        break;
    case 4: /* SPU */
        for (uint32_t i = 0; i < words; i++, addr = (addr + step) & 0x1FFFFC) {
            uint32_t w;
            if (to_ram) { spu_dma_read(&w, 1); memcpy(&psx_ram[addr], &w, 4); }
            else { memcpy(&w, &psx_ram[addr], 4); spu_dma_write(&w, 1); }
        }
        break;
    case 6: /* OTC: reverse-linked empty ordering table */
        for (uint32_t i = 0; i < words; i++) {
            uint32_t w = (i == words - 1) ? 0x00FFFFFF : ((addr - 4) & 0x1FFFFC);
            memcpy(&psx_ram[addr], &w, 4);
            addr = (addr - 4) & 0x1FFFFC;
        }
        break;
    default:
        LOG("DMA channel %d not handled\n", ch);
    }
    dma_chcr[ch] &= ~((1u << 24) | (1u << 28));
    dma_irq(ch);
}

uint32_t dma_read(uint32_t addr) {
    uint32_t r = addr & 0xFF;
    if (r == 0xF0) return dpcr;
    if (r == 0xF4) return dicr;
    int ch = (r - 0x80) >> 4;
    if (ch < 0 || ch > 6) return 0;
    switch (r & 0xF) {
    case 0: return dma_madr[ch];
    case 4: return dma_bcr[ch];
    case 8: return dma_chcr[ch];
    }
    return 0;
}

void dma_write(uint32_t addr, uint32_t v) {
    uint32_t r = addr & 0xFF;
    if (r == 0xF0) { dpcr = v; return; }
    if (r == 0xF4) {
        uint32_t keep_flags = (dicr >> 24) & 0x7F & ~((v >> 24) & 0x7F);
        dicr = (v & 0x00FF803F) | (keep_flags << 24);
        uint32_t flags = (dicr >> 24) & 0x7F, en = (dicr >> 16) & 0x7F;
        if ((dicr & 0x8000) || ((dicr & 0x800000) && (flags & en))) dicr |= 0x80000000u;
        DBG("DICR write %08X -> %08X\n", v, dicr);
        return;
    }
    int ch = (r - 0x80) >> 4;
    if (ch < 0 || ch > 6) return;
    switch (r & 0xF) {
    case 0: dma_madr[ch] = v & 0xFFFFFF; break;
    case 4: dma_bcr[ch] = v; break;
    case 8:
        if (ch == 6) v = (v & 0x51000000) | 2;
        if (ch == 2) dma2_flush(); /* the list before still draws first */
        dma_chcr[ch] = v;
        if (v & (1u << 24)) {
            int sync = (v >> 9) & 3;
            if (ch == 2 && sync == 2) dma2_defer();
            else if (sync != 0 || (v & (1u << 28)) || ch == 6)
                dma_run(ch);
        }
        break;
    }
}

/* ------------------------------------------------------------------ timers */

typedef struct {
    uint32_t count, mode, target;
    int64_t last;  /* cycles at last update (sysclk sources) */
    int64_t frac;
} Timer;
static Timer tmr[3];

static int timer_uses_hblank(int n) { return n == 1 && (tmr[1].mode & 0x100); }

static void timer_add(int n, uint32_t ticks) {
    Timer *t = &tmr[n];
    if (!ticks) return;
    uint32_t c = t->count + ticks;
    int irq = 0;
    if (t->count < t->target && c >= t->target && t->target) {
        t->mode |= 0x800;
        if (t->mode & 0x10) irq = 1;
        if (t->mode & 0x8) c = t->target ? (c - t->target) % t->target : 0;
    }
    if (c >= 0x10000) {
        t->mode |= 0x1000;
        if (t->mode & 0x20) irq = 1;
        c &= 0xFFFF;
    }
    t->count = c;
    if (irq) {
        t->mode &= ~0x400u; /* bit10 = 0 means IRQ requested */
        irq_raise(IRQ_TMR0 + n);
        if (!(t->mode & 0x40)) {} /* one-shot: we keep firing; harmless for libs that re-arm */
        if (t->mode & 0x80) t->mode ^= 0x400; else t->mode |= 0x400;
    }
}

void timers_update(void) {
    int64_t now = cpu.cycles;
    for (int n = 0; n < 3; n++) {
        Timer *t = &tmr[n];
        int64_t el = now - t->last;
        t->last = now;
        if (timer_uses_hblank(n)) continue;
        int div = 1;
        if (n == 2 && (t->mode & 0x200)) div = 8;
        t->frac += el;
        uint32_t ticks = (uint32_t)(t->frac / div);
        t->frac -= (int64_t)ticks * div;
        timer_add(n, ticks);
        /* schedule the next IRQ crossing for sysclk timers with IRQ enabled */
        if (t->mode & 0x30) {
            uint32_t to = (t->mode & 0x10) && t->count < t->target ? t->target - t->count : 0x10000 - t->count;
            sched_at(now + (int64_t)to * div);
        }
    }
}

void timers_hblank(int lines) {
    if (timer_uses_hblank(1)) timer_add(1, lines);
}

uint32_t timer_read(uint32_t addr) {
    int n = (addr >> 4) & 3;
    if (n > 2) return 0;
    timers_update();
    switch (addr & 0xF) {
    case 0: return tmr[n].count & 0xFFFF;
    case 4: { uint32_t m = tmr[n].mode; tmr[n].mode &= ~0x1800u; return m; }
    case 8: return tmr[n].target;
    }
    return 0;
}

void timer_write(uint32_t addr, uint32_t v) {
    int n = (addr >> 4) & 3;
    if (n > 2) return;
    timers_update();
    switch (addr & 0xF) {
    case 0: tmr[n].count = v & 0xFFFF; break;
    case 4: tmr[n].mode = (v & 0x3FF) | 0x400; tmr[n].count = 0; tmr[n].frac = 0; break;
    case 8: tmr[n].target = v & 0xFFFF; break;
    }
    sched_at(cpu.cycles);
}

/* ------------------------------------------------------------------ I/O dispatch */

static uint32_t memctrl[16];
static uint32_t ram_size_reg = 0x00000B88;

uint32_t io_read(uint32_t addr, int size) {
    uint32_t p = addr & 0x1FFFFFFF;
    if (p >= 0x1F801000 && p < 0x1F802000) {
        uint32_t r = p & 0xFFF;
        if (r < 0x40) return memctrl[r >> 2];
        if (r >= 0x40 && r < 0x50) return sio_read(p, size);
        if (r >= 0x50 && r < 0x60) return 0; /* SIO1 (serial cable) */
        if (r == 0x60) return ram_size_reg;
        if (r == 0x70) return i_stat;
        if (r == 0x74) return i_mask;
        if (r >= 0x80 && r < 0x100) return dma_read(p & ~3u) >> (8 * (p & 3));
        if (r >= 0x100 && r < 0x130) return timer_read(p);
        if (r >= 0x800 && r < 0x804) {
            if (size == 1) return cdrom_read(p);
            uint32_t v = 0;
            /* wider reads of the data register take several bytes from the FIFO */
            for (int i = 0; i < size; i++) v |= (uint32_t)cdrom_read(r == 0x802 ? p : p + i) << (8 * i);
            return v;
        }
        if (r == 0x810) { dma2_flush(); return gpu_read(); }
        if (r == 0x814) return gpu_stat();
        if (r == 0x820 || r == 0x824) return mdec_read(p);
        if (r >= 0xC00) {
            if (size == 4) return spu_read(p) | ((uint32_t)spu_read(p + 2) << 16);
            return spu_read(p);
        }
        DBG("io_read %08X size %d\n", addr, size);
        return 0;
    }
    if (p >= 0x1F000000 && p < 0x1F800000) return 0xFFFFFFFF; /* expansion 1: nothing there */
    if (p >= 0x1FC00000 && p < 0x1FC80000) return 0;           /* BIOS ROM: not present (HLE) */
    if (addr == 0xFFFE0130) return 0;
    DBG("unmapped read %08X\n", addr);
    return 0;
}

void io_write(uint32_t addr, uint32_t v, int size) {
    uint32_t p = addr & 0x1FFFFFFF;
    if (p >= 0x1F801000 && p < 0x1F802000) {
        uint32_t r = p & 0xFFF;
        if (r < 0x40) { memctrl[r >> 2] = v; return; }
        if (r >= 0x40 && r < 0x50) { sio_write(p, v, size); return; }
        if (r >= 0x50 && r < 0x60) return;
        if (r == 0x60) { ram_size_reg = v; return; }
        if (r == 0x70) { i_stat &= v; return; }
        if (r == 0x74) { i_mask = v & 0x7FF; DBG("I_MASK=%03X\n", i_mask); sched_at(cpu.cycles); return; }
        if (r >= 0x80 && r < 0x100) {
            if (size < 4) {
                /* byte/halfword writes (libcd sets DICR enable bits with a byte store to DICR+2) */
                uint32_t a4 = p & ~3u, sh = 8 * (p & 3);
                uint32_t mask = (size == 1 ? 0xFFu : 0xFFFFu) << sh;
                uint32_t cur = dma_read(a4);
                if ((a4 & 0xFF) == 0xF4) cur &= 0x00FFFFFFu; /* keep flags: only an explicit write acks them */
                v = (cur & ~mask) | ((v << sh) & mask);
                dma_write(a4, v);
            } else {
                dma_write(p, v);
            }
            return;
        }
        if (r >= 0x100 && r < 0x130) { timer_write(p, v); return; }
        if (r >= 0x800 && r < 0x804) {
            for (int i = 0; i < size; i++) cdrom_write(p + i, (uint8_t)(v >> (8 * i)));
            return;
        }
        if (r == 0x810) { dma2_flush(); gpu_gp0(v); return; }
        if (r == 0x814) { dma2_flush(); gpu_gp1(v); return; }
        if (r == 0x820 || r == 0x824) { mdec_write(p, v); return; }
        if (r >= 0xC00) {
            if (size == 4) { spu_write(p, v & 0xFFFF); spu_write(p + 2, v >> 16); }
            else spu_write(p, v & 0xFFFF);
            return;
        }
        DBG("io_write %08X = %08X size %d\n", addr, v, size);
        return;
    }
    if (p >= 0x1F802000 && p < 0x1F804000) return; /* expansion 2 (debug LEDs, POST) */
    if (addr == 0xFFFE0130) return;
    DBG("unmapped write %08X = %08X\n", addr, v);
}

/* ------------------------------------------------------------------ dispatcher */

static const FuncEntry *find_in(const ImageEntry *im, uint32_t a, int *mid) {
    int lo = 0, hi = im->count - 1;
    *mid = 0;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        if (im->funcs[m].start == a) return &im->funcs[m];
        if (im->funcs[m].start < a) lo = m + 1; else hi = m - 1;
    }
    if (hi >= 0 && a < im->funcs[hi].end) *mid = 1;
    return 0;
}

static const ImageEntry *overlay_now(uint32_t a) {
    for (int i = 0; i < recomp_image_count; i++) {
        const ImageEntry *im = &recomp_images[i];
        if (!im->is_overlay || a < im->code_lo || a >= im->code_hi) continue;
        uint32_t w; memcpy(&w, &psx_ram[im->base & 0x1FFFFC], 4);
        if (w == im->ident) return im;
    }
    return 0;
}

/* the overlay of that name is the one in RAM now (its first word at its base) */
int overlay_loaded(const char *name) {
    for (int i = 0; i < recomp_image_count; i++) {
        const ImageEntry *im = &recomp_images[i];
        if (!im->is_overlay || strcmp(im->name, name)) continue;
        uint32_t w; memcpy(&w, &psx_ram[im->base & 0x1FFFFC], 4);
        if (w == im->ident) return 1;
    }
    return 0;
}

void dispatch_call(uint32_t a) {
    uint32_t p = a & 0x1FFFFFFF;
    if (p == 0xA0 || p == 0xB0 || p == 0xC0) {
        bios_call(p, cpu.r[9]);
        return;
    }
    uint32_t k = 0x80000000u | p;
    for (int i = 0; i < recomp_image_count; i++) {
        const ImageEntry *im = &recomp_images[i];
        if (im->is_overlay) continue;
        if (k >= im->code_lo && k < im->code_hi) {
            int mid;
            const FuncEntry *f = find_in(im, k, &mid);
            if (f) { f->fn(); return; }
            fatal("call to %08X: inside a function of %s but not a known entry (add it to extra_seeds)", a, im->name);
        }
    }
    const ImageEntry *ov = overlay_now(k);
    if (ov) {
        int mid;
        const FuncEntry *f = find_in(ov, k, &mid);
        if (f) { f->fn(); return; }
        fatal("call to %08X: not a known entry of overlay %s", a, ov->name);
    }
    fatal("call to %08X: no recompiled code there (ra=%08X)", a, cpu.r[31]);
}

uint32_t rs_stack[RS_MAX];
uint16_t fn_trail[64];
uint32_t fn_trail_pos;
int rs_top;

void call_guest(uint32_t addr) {
    CALL(GUEST_RA, dispatch_call(addr));
}

void bad_insn(uint32_t pc, uint32_t word) {
    fatal("invalid instruction %08X at %08X", word, pc);
}

/* ------------------------------------------------------------------ save states (state.c) */

void core_state(StIO *io) {
    st_begin(io, "CPU", 1);
    ST(io, cpu);
    int n = st_count(io, rs_top, RS_MAX);
    for (int i = 0; i < n; i++) ST(io, rs_stack[i]);
    if (io->load && !io->dry) rs_top = n;
    ST(io, i_stat); ST(io, i_mask); ST(io, in_exception);
    ST(io, ev_hblank); ST(io, scanline); ST(io, frame_count);
    ST(io, dma_madr); ST(io, dma_bcr); ST(io, dma_chcr); ST(io, dpcr); ST(io, dicr);
    ST(io, dma1_pending); ST(io, dma2_pending); ST(io, dma2_due);
    ST(io, tmr);
    ST(io, memctrl); ST(io, ram_size_reg);
    st_end(io);
    st_begin(io, "RAM", 1);
    st_raw(io, psx_ram, sizeof psx_ram);
    st_raw(io, psx_scratch, sizeof psx_scratch);
    st_end(io);
}

void bios_break(uint32_t pc, uint32_t code) {
    LOG("break %X at %08X (ra=%08X)\n", code, pc, cpu.r[31]);
    /* divide-by-zero checks emitted by the compiler use break 7; ignore like hardware with no handler */
}
