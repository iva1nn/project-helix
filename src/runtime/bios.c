/* High-level BIOS replacement: the A0/B0/C0 functions the game calls, events, interrupt dispatch,
 * pads and memory card, written in C. No Sony BIOS code or image is used. */
#include <setjmp.h>
#include <stdlib.h>
#include <ctype.h>
#include "hw.h"
#include "state.h"

static inline uint32_t rd32(uint32_t a) { return mem_r32(a); }
static inline void wr32(uint32_t a, uint32_t v) { mem_w32(a, v); }
static char *str(uint32_t a) { return (char *)psx_ptr(a); }

#define A0 R4
#define A1 R5
#define A2 R6
#define A3 R7
#define V0 R2

/* Kernel area: the libraries read the BIOS function tables to patch the exception handler.
 * Give them tables that point at unused kernel RAM, so the patches land somewhere harmless. */
#define KTAB_A0 0x200
#define KTAB_B0 0x874
#define KTAB_C0 0x674

/* ------------------------------------------------------------------ events */

typedef struct { uint32_t cls, spec, mode, func, status; } Event;
#define NEV 32
static Event ev[NEV];
enum { EV_FREE = 0, EV_DISABLED = 0x1000, EV_ENABLED = 0x2000, EV_READY = 0x4000 };

static void deliver_event(uint32_t cls, uint32_t spec) {
    for (int i = 0; i < NEV; i++) {
        Event *e = &ev[i];
        if (e->status != EV_ENABLED || e->cls != cls || e->spec != spec) continue;
        if (e->mode == 0x2000) e->status = EV_READY;
        else if (e->mode == 0x1000 && e->func) {
            Cpu save = cpu;
            call_guest(e->func);
            int64_t cyc = cpu.cycles, nxt = cpu.next_event;
            cpu = save;
            cpu.cycles = cyc; cpu.next_event = nxt;
        }
    }
}

static int ev_index(uint32_t h) {
    int i = (int)(h & 0xFFFF);
    if ((h & 0xFFFF0000u) != 0xF1000000u || i >= NEV) return -1;
    return i;
}

/* ------------------------------------------------------------------ interrupts */

static uint32_t chain[4];      /* SysEnqIntRP queues (head pointers in PS1 RAM) */
static uint32_t hook;          /* HookEntryInt jmp_buf address */
static int clear_pad = 1, clear_rcnt[4] = {1, 1, 1, 1};
static jmp_buf exc_jmp;
static int exc_jmp_valid;

static void call_saving(uint32_t fn) {
    call_guest(fn);
}

void bios_exception(void) {
    static int nexc;
    if (nexc++ < 20) {
        DBG("exception: i_stat=%03X i_mask=%03X hook=%08X\n", i_stat, i_mask, hook);
        if (getenv("FE_DBG_ETC")) {
            uint32_t b = 0x800809DC;
            DBG("  libetc: busy=%04X mask=%04X cb:", mem_r16(b), mem_r16(b + 0x30));
            for (int i = 0; i < 11; i++) DBG(" %08X", rd32(b + 4 + i * 4));
            DBG("\n");
        }
    }
    in_exception = 1;
    Cpu save = cpu;
    cpu.sr = (cpu.sr & ~0x3Fu) | ((cpu.sr << 2) & 0x3Fu);
    cpu.cause = 0x400;
    /* priority chains */
    for (int p = 0; p < 4; p++) {
        for (uint32_t e = chain[p]; e; e = rd32(e)) {
            uint32_t f1 = rd32(e + 8), f2 = rd32(e + 4);
            if (!f1) continue;
            call_saving(f1);
            if (V0 && f2) { A0 = V0; call_saving(f2); }
        }
    }
    /* BIOS's own handlers: root counter and vblank events */
    uint32_t pend = i_stat & i_mask;
    if (pend & 1) {
        deliver_event(0xF2000003, 0x0002);
        if (clear_pad) i_stat &= ~1u;
    }
    for (int n = 0; n < 3; n++)
        if (pend & (1u << (4 + n))) {
            deliver_event(0xF2000000 + n, 0x0002);
            if (clear_rcnt[n]) i_stat &= ~(1u << (4 + n));
        }
    if (hook) {
        int rs_save = rs_top;
        exc_jmp_valid = 1;
        if (!setjmp(exc_jmp)) {
            uint32_t h = hook;
            R31 = rd32(h); R29 = rd32(h + 4); R30 = rd32(h + 8);
            for (int i = 0; i < 8; i++) cpu.r[16 + i] = rd32(h + 12 + i * 4);
            R28 = rd32(h + 44);
            V0 = 1;
            rs_stack[rs_top++] = GUEST_RA;
            dispatch_call(R31); /* longjmp (setjmp resume): a jump, not a call */
        }
        rs_top = rs_save;
        exc_jmp_valid = 0;
    }
    int64_t cyc = cpu.cycles, nxt = cpu.next_event;
    cpu = save;
    cpu.cycles = cyc; cpu.next_event = nxt;
    in_exception = 0;
    sched_at(cpu.cycles); /* another interrupt may be pending */
}

static void return_from_exception(void) {
    if (exc_jmp_valid) longjmp(exc_jmp, 1);
    LOG("ReturnFromException outside an exception\n");
}

/* ------------------------------------------------------------------ pads */

static uint32_t pad_buf[2], pad_size[2];
static int pad_started;
static uint32_t pad_old_dest;

void bios_vblank(void) {
    if (pad_started) {
        for (int p = 0; p < 2; p++) {
            if (!pad_buf[p]) continue;
            uint8_t *b = psx_ptr(pad_buf[p]);
            if (!b) continue;
            if (p == 0) {
                uint16_t inv = (uint16_t)~pad_buttons;
                b[0] = 0x00; b[1] = 0x41; b[2] = (uint8_t)inv; b[3] = (uint8_t)(inv >> 8);
            } else {
                b[0] = 0xFF; b[1] = 0x00; b[2] = 0xFF; b[3] = 0xFF;
            }
        }
    }
    if (pad_old_dest) wr32(pad_old_dest, pad_buttons);
}

/* ------------------------------------------------------------------ printf */

static uint32_t arg_n(int n) {
    if (n < 4) return cpu.r[4 + n];
    return rd32(R29 + 4 * n);
}

static void bios_printf(int first_arg) {
    const char *f = str(cpu.r[4 + first_arg - 1]);
    if (!f) return;
    char out[2048];
    int o = 0, an = first_arg;
    while (*f && o < (int)sizeof out - 64) {
        if (*f != '%') { out[o++] = *f++; continue; }
        char spec[32];
        int s = 0;
        spec[s++] = *f++;
        while (*f && strchr("-+ #0123456789.lh", *f) && s < 28) spec[s++] = *f++;
        char cv = *f ? *f++ : 0;
        if (cv == '%') { out[o++] = '%'; continue; }
        /* drop length modifiers, 32-bit everywhere */
        int k = 0;
        for (int i = 0; i < s; i++) if (spec[i] != 'l' && spec[i] != 'h') spec[k++] = spec[i];
        spec[k++] = cv; spec[k] = 0;
        uint32_t a = arg_n(an++);
        if (cv == 's') { const char *p = str(a); o += snprintf(out + o, sizeof out - o, spec, p ? p : "(null)"); }
        else if (cv == 'c') o += snprintf(out + o, sizeof out - o, spec, (int)a);
        else if (cv == 'd' || cv == 'i') o += snprintf(out + o, sizeof out - o, spec, (int32_t)a);
        else o += snprintf(out + o, sizeof out - o, spec, a);
        if (o > (int)sizeof out - 64) break;
    }
    out[o] = 0;
    LOG("[game] %s", out);
    if (o && out[o - 1] != '\n') LOG("\n");
}

/* ------------------------------------------------------------------ helpers */

static uint32_t rand_seed = 0x24040001;
static int warned[3][256];

static void unknown(uint32_t t, uint32_t fn) {
    int ti = t == 0xA0 ? 0 : t == 0xB0 ? 1 : 2;
    if (!warned[ti][fn & 0xFF]) {
        warned[ti][fn & 0xFF] = 1;
        LOG("BIOS %02X:%02X not implemented (ra=%08X a0=%08X)\n", t, fn, R31, A0);
    }
    V0 = 0;
}

/* memory card: results arrive as events a moment after the call, like the real card interrupt */
int mcd_present(int port);
int mcd_open(const char *path, uint32_t mode);
int mcd_is_fd(int fd);
int mcd_fd_async(int fd);
int mcd_rw(int fd, uint32_t buf, uint32_t len, int write);
int mcd_lseek(int fd, int32_t off, int whence);
void mcd_close(int fd);
uint32_t mcd_find(const char *pat, uint32_t de, int first);
int mcd_erase(const char *path);
int mcd_format(const char *path);
int mcd_sector(int port, int sector, uint32_t buf, int write);
extern int mcd_errno;

#define CARD_IOE 0x0004
#define CARD_ERROR 0x8000
#define CARD_TIMEOUT 0x0100
static struct { uint32_t cls, spec; int64_t when; } card_q[16];
static int card_qn;

static void card_event_cls(uint32_t cls, uint32_t spec) {
    if (card_qn < 16) {
        card_q[card_qn].cls = cls; card_q[card_qn].spec = spec;
        card_q[card_qn].when = cpu.cycles + 20000;
        sched_at(card_q[card_qn].when);
        card_qn++;
    }
}
static void card_event(uint32_t spec) { card_event_cls(0xF4000001, spec); }   /* SwCARD */
static void card_hw_event(uint32_t spec) { card_event_cls(0xF0000011, spec); } /* HwCARD */

static int busy; /* bios_update is running (a card event's handler may come back here) */

void bios_update(void) {
    if (busy) return;
    busy = 1;
    while (card_qn && cpu.cycles >= card_q[0].when) {
        uint32_t cls = card_q[0].cls, spec = card_q[0].spec;
        memmove(&card_q[0], &card_q[1], (size_t)--card_qn * sizeof card_q[0]);
        LOG("card event %08X %04X\n", cls, spec);
        deliver_event(cls, spec);
    }
    if (card_qn) sched_at(card_q[0].when);
    busy = 0;
}

/* ------------------------------------------------------------------ tables */

static void a0(uint32_t fn) {
    switch (fn) {
    case 0x13: { /* setjmp */
        uint32_t b = A0;
        wr32(b, R31); wr32(b + 4, R29); wr32(b + 8, R30);
        for (int i = 0; i < 8; i++) wr32(b + 12 + i * 4, cpu.r[16 + i]);
        wr32(b + 44, R28);
        V0 = 0;
        break;
    }
    case 0x15: { char *d = str(A0), *s = str(A1); if (d && s) strcat(d, s); V0 = A0; break; }
    case 0x17: { char *a = str(A0), *b = str(A1); V0 = (uint32_t)(a && b ? strcmp(a, b) : 0); break; }
    case 0x18: { char *a = str(A0), *b = str(A1); V0 = (uint32_t)(a && b ? strncmp(a, b, A2) : 0); break; }
    case 0x19: { char *d = str(A0), *s = str(A1); if (d && s) strcpy(d, s); V0 = A0; break; }
    case 0x1A: { char *d = str(A0), *s = str(A1); if (d && s) strncpy(d, s, A2); V0 = A0; break; }
    case 0x1B: { char *s = str(A0); V0 = s ? (uint32_t)strlen(s) : 0; break; }
    case 0x25: V0 = (uint32_t)toupper((int)(A0 & 0xFF)); break;
    case 0x26: V0 = (uint32_t)tolower((int)(A0 & 0xFF)); break;
    case 0x27: for (uint32_t i = 0; i < A2; i++) mem_w8(A1 + i, mem_r8(A0 + i)); V0 = A1; break;
    case 0x28: for (uint32_t i = 0; i < A1; i++) mem_w8(A0 + i, 0); break;
    case 0x2A: for (uint32_t i = 0; i < A2; i++) mem_w8(A0 + i, mem_r8(A1 + i)); V0 = A0; break;
    case 0x2B: for (uint32_t i = 0; i < A2; i++) mem_w8(A0 + i, A1); V0 = A0; break;
    case 0x2C: { uint8_t *d = psx_ptr(A0), *s = psx_ptr(A1); if (d && s) memmove(d, s, A2); V0 = A0; break; }
    case 0x2D: { uint8_t *a = psx_ptr(A0), *b = psx_ptr(A1); V0 = (uint32_t)(a && b ? memcmp(a, b, A2) : 0); break; }
    case 0x2E: { V0 = 0; for (uint32_t i = 0; i < A2; i++) if (mem_r8(A0 + i) == (A1 & 0xFF)) { V0 = A0 + i; break; } break; }
    case 0x2F: rand_seed = rand_seed * 0x41C64E6D + 0x3039; V0 = (rand_seed >> 16) & 0x7FFF; break;
    case 0x30: rand_seed = A0; break;
    case 0x39: LOG("InitHeap(%08X, %X)\n", A0, A1); break;
    case 0x3F: bios_printf(1); break;
    case 0x44: break; /* FlushCache */
    case 0x49: { void dma2_flush(void); dma2_flush(); gpu_gp0(A0); break; } /* GPU_cw */
    case 0x70: break; /* _bu_init */
    case 0x72: break; /* CdRemove */
    case 0x9F: break; /* SetMemSize */
    case 0xAB: /* _card_info(port) */
    case 0xAC: /* _card_load(port) */
        LOG("_card_%s(%02X)\n", fn == 0xAB ? "info" : "load", A0);
        card_event(mcd_present((int)A0) ? CARD_IOE : CARD_TIMEOUT);
        V0 = 1;
        break;
    default: unknown(0xA0, fn);
    }
}

static void b0(uint32_t fn) {
    switch (fn) {
    case 0x07: deliver_event(A0, A1); break;
    case 0x08: { /* OpenEvent */
        V0 = 0xFFFFFFFF;
        for (int i = 0; i < NEV; i++)
            if (ev[i].status == EV_FREE) {
                ev[i].cls = A0; ev[i].spec = A1; ev[i].mode = A2; ev[i].func = A3; ev[i].status = EV_DISABLED;
                V0 = 0xF1000000u | (uint32_t)i;
                break;
            }
        DBG("OpenEvent(%08X,%04X,%04X,%08X) -> %08X\n", A0, A1, A2, A3, V0);
        break;
    }
    case 0x09: { int i = ev_index(A0); if (i >= 0) ev[i].status = EV_FREE; V0 = 1; break; }
    case 0x0A: { /* WaitEvent */
        int i = ev_index(A0);
        V0 = 0;
        if (i < 0 || ev[i].status == EV_DISABLED || ev[i].status == EV_FREE) break;
        int64_t start = cpu.cycles;
        while (ev[i].status != EV_READY) {
            cpu.cycles += 200;
            CHECK();
            if (cpu.cycles - start > (int64_t)CPU_HZ * 30) { LOG("WaitEvent(%08X): 30 s timeout\n", A0); break; }
        }
        if (ev[i].status == EV_READY) ev[i].status = EV_ENABLED;
        V0 = 1;
        break;
    }
    case 0x0B: { int i = ev_index(A0); V0 = 0; if (i >= 0 && ev[i].status == EV_READY) { ev[i].status = EV_ENABLED; V0 = 1; } break; }
    case 0x0C: { int i = ev_index(A0); if (i >= 0 && ev[i].status != EV_FREE) ev[i].status = EV_ENABLED; V0 = 1; break; }
    case 0x0D: { int i = ev_index(A0); if (i >= 0 && ev[i].status != EV_FREE) ev[i].status = EV_DISABLED; V0 = 1; break; }
    case 0x12:
        pad_buf[0] = A0; pad_size[0] = A1; pad_buf[1] = A2; pad_size[1] = A3; V0 = 2;
        LOG("InitPAD(%08X,%d,%08X,%d)\n", A0, A1, A2, A3);
        break;
    case 0x13: pad_started = 1; V0 = 1; LOG("StartPAD\n"); break;
    case 0x14: pad_started = 0; V0 = 1; break;
    case 0x15: pad_old_dest = A1; V0 = 2; break;
    case 0x17: return_from_exception(); break;
    case 0x18: hook = 0; break;
    case 0x19: hook = A0; DBG("HookEntryInt(%08X)\n", A0); break;
    case 0x32: { /* open */
        int fd = mcd_open(str(A0), A1);
        LOG("open(\"%s\", %X) -> %d\n", str(A0) ? str(A0) : "?", A1, fd);
        V0 = fd < 0 ? 0xFFFFFFFF : (uint32_t)fd;
        break;
    }
    case 0x33: V0 = (uint32_t)mcd_lseek((int)A0, (int32_t)A1, (int)A2); break;
    case 0x34: case 0x35: { /* read / write */
        int fd = (int)A0;
        int n = mcd_rw(fd, A1, A2, fn == 0x35);
        LOG("%s(%d, %08X, %u) -> %d\n", fn == 0x35 ? "write" : "read", fd, A1, A2, n);
        V0 = (uint32_t)n;
        if (mcd_fd_async(fd)) {
            /* asynchronous: 0 = request accepted, the result arrives as a SwCARD event */
            card_event(n >= 0 ? CARD_IOE : CARD_ERROR);
            V0 = n >= 0 ? 0 : 0xFFFFFFFF;
        }
        break;
    }
    case 0x36: mcd_close((int)A0); V0 = A0; break;
    case 0x54: case 0x55: V0 = (uint32_t)mcd_errno; break; /* _get_errno / _get_error */
    case 0x3F: { const char *s = str(A0); LOG("[game] %s\n", s ? s : ""); break; }
    case 0x41: V0 = (uint32_t)mcd_format(str(A0)); card_hw_event(CARD_IOE); break;
    case 0x42: V0 = mcd_find(str(A0), A1, 1); card_hw_event(CARD_IOE); LOG("firstfile(\"%s\") -> %08X\n", str(A0) ? str(A0) : "?", V0); break;
    case 0x43: V0 = mcd_find(0, A0, 0); card_hw_event(CARD_IOE); break;
    case 0x45: V0 = (uint32_t)mcd_erase(str(A0)); card_hw_event(CARD_IOE); LOG("erase(\"%s\") -> %d\n", str(A0) ? str(A0) : "?", V0); break;
    case 0x4A: case 0x4B: case 0x4C: V0 = 1; break;
    case 0x4E: case 0x4F: { /* _card_write / _card_read (port, sector, buf) */
        int ok = mcd_sector((int)A0, (int)A1, A2, fn == 0x4E);
        LOG("_card_%s(%02X, %d)\n", fn == 0x4E ? "write" : "read", A0, A1);
        card_hw_event(ok ? CARD_IOE : CARD_TIMEOUT);
        V0 = 1;
        break;
    }
    case 0x50: V0 = 1; break; /* _new_card */
    case 0x56: V0 = KTAB_C0; break;
    case 0x57: V0 = KTAB_B0; break;
    case 0x5B: clear_pad = (int)A0; DBG("ChangeClearPad(%d)\n", A0); break;
    default: unknown(0xB0, fn);
    }
}

static void c0(uint32_t fn) {
    switch (fn) {
    case 0x02: { /* SysEnqIntRP(prio, struct) */
        int p = (int)(A0 & 3);
        wr32(A1, chain[p]);
        chain[p] = A1;
        V0 = 0;
        DBG("SysEnqIntRP(%d, %08X) f1=%08X f2=%08X\n", p, A1, rd32(A1 + 8), rd32(A1 + 4));
        break;
    }
    case 0x03: { /* SysDeqIntRP */
        int p = (int)(A0 & 3);
        uint32_t *link = &chain[p];
        uint32_t prev = 0;
        for (uint32_t e = chain[p]; e; prev = e, e = rd32(e)) {
            if (e == A1) {
                if (prev) wr32(prev, rd32(e)); else *link = rd32(e);
                break;
            }
        }
        V0 = 0;
        break;
    }
    case 0x0A: DBG("ChangeClearRCnt(%d,%d)\n", A0, A1); if (A0 < 4) { V0 = (uint32_t)clear_rcnt[A0]; clear_rcnt[A0] = (int)A1; } break;
    default: unknown(0xC0, fn);
    }
}

void bios_call(uint32_t table, uint32_t fn) {
    static uint8_t seen[3][256];
    int t = table == 0xA0 ? 0 : table == 0xB0 ? 1 : 2;
    if (!seen[t][fn & 0xFF]) { seen[t][fn & 0xFF] = 1; DBG("BIOS first call %02X:%02X ra=%08X a0=%08X a1=%08X\n", table, fn, R31, A0, A1); }
    if (table == 0xA0) a0(fn);
    else if (table == 0xB0) b0(fn);
    else c0(fn);
}

void bios_syscall(uint32_t pc) {
    (void)pc;
    switch (A0) {
    case 0: break;
    case 1: V0 = (cpu.sr & 0x401) == 0x401; cpu.sr &= ~0x401u; break;
    case 2: cpu.sr |= 0x401; sched_at(cpu.cycles); break;
    default: LOG("syscall %u\n", A0);
    }
}


void bios_init(void) {
    memset(ev, 0, sizeof ev);
    for (int i = 0; i < 0xC0; i++) wr32(KTAB_A0 + 4 * i, 0x4000 + i * 0x40);
    for (int i = 0; i < 0x80; i++) wr32(KTAB_B0 + 4 * i, 0x8000 + i * 0x40);
    for (int i = 0; i < 0x20; i++) wr32(KTAB_C0 + 4 * i, 0xC000 + i * 0x80);
    /* device control blocks (0x50 bytes each): libmcrd searches them for "bu" before firstfile() */
    static const char *const devs[] = {"cdrom", "bu"};
    wr32(0x150, 0xE000);
    wr32(0x154, 2 * 0x50);
    for (int i = 0; i < 2; i++) {
        uint32_t name = 0xE200 + i * 0x10;
        for (int k = 0; devs[i][k] || (k && !devs[i][k - 1] ? 0 : 0); k++) mem_w8(name + k, (uint8_t)devs[i][k]);
        mem_w8(name + (uint32_t)strlen(devs[i]), 0);
        wr32(0xE000 + i * 0x50, name);
    }
}

/* ------------------------------------------------------------------ save states (state.c) */

void bios_state(StIO *io) {
    st_begin(io, "BIOS", 1);
    ST(io, ev); ST(io, chain); ST(io, hook); ST(io, clear_pad); ST(io, clear_rcnt);
    ST(io, pad_buf); ST(io, pad_size); ST(io, pad_started); ST(io, pad_old_dest);
    ST(io, rand_seed);
    ST(io, card_q); ST(io, card_qn);
    st_end(io);
}

/* a load can come while an exception or a card event was being delivered: those C frames are gone */
void bios_state_loaded(void) {
    exc_jmp_valid = 0;
    busy = 0;
}