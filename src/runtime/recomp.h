/* Interface between the generated game code (gen/) and the native runtime. */
#pragma once
#include <stdint.h>
#include <string.h>

typedef struct {
    uint32_t r[32];
    uint32_t hi, lo;
    uint32_t sr, cause, epc;
    int64_t cycles;     /* emulated CPU cycles since boot (1 per instruction) */
    int64_t next_event; /* cycles value at which hw_events() must run */
} Cpu;

extern Cpu cpu;

#define R1 cpu.r[1]
#define R2 cpu.r[2]
#define R3 cpu.r[3]
#define R4 cpu.r[4]
#define R5 cpu.r[5]
#define R6 cpu.r[6]
#define R7 cpu.r[7]
#define R8 cpu.r[8]
#define R9 cpu.r[9]
#define R10 cpu.r[10]
#define R11 cpu.r[11]
#define R12 cpu.r[12]
#define R13 cpu.r[13]
#define R14 cpu.r[14]
#define R15 cpu.r[15]
#define R16 cpu.r[16]
#define R17 cpu.r[17]
#define R18 cpu.r[18]
#define R19 cpu.r[19]
#define R20 cpu.r[20]
#define R21 cpu.r[21]
#define R22 cpu.r[22]
#define R23 cpu.r[23]
#define R24 cpu.r[24]
#define R25 cpu.r[25]
#define R26 cpu.r[26]
#define R27 cpu.r[27]
#define R28 cpu.r[28]
#define R29 cpu.r[29]
#define R30 cpu.r[30]
#define R31 cpu.r[31]

void hw_events(void);
#define CYC(n) (cpu.cycles += (n))
#define CHECK() do { if (cpu.cycles >= cpu.next_event) hw_events(); } while (0)

/* ---- memory ---- */
#define PSX_RAM_SIZE 0x200000
extern uint8_t psx_ram[PSX_RAM_SIZE];
extern uint8_t psx_scratch[0x400];

uint32_t io_read(uint32_t addr, int size);
void io_write(uint32_t addr, uint32_t val, int size);

#define SR_ISC 0x10000u

#ifdef FE_WATCH
/* debug builds: report writes to a RAM range (FE_WATCH=lo,hi physical, hex) */
extern uint32_t watch_lo, watch_hi;
void watch_hit(uint32_t a, uint32_t v, int size);
#define WATCH(a, v, s) do { uint32_t p_ = (a) & 0x1FFFFF; if (p_ >= watch_lo && p_ < watch_hi && ((a) & 0x1FFFFFFF) < 0x800000) watch_hit(a, v, s); } while (0)
/* and reads (FE_RWATCH=lo,hi): which game function reads a word (e.g. the pad state) */
extern uint32_t rwatch_lo, rwatch_hi;
void rwatch_hit(uint32_t a, uint32_t v, int size);
#define RWATCH(a, v, s) do { uint32_t p_ = (a) & 0x1FFFFF; if (p_ >= rwatch_lo && p_ < rwatch_hi && ((a) & 0x1FFFFFFF) < 0x800000) rwatch_hit(a, v, s); } while (0)
#else
#define WATCH(a, v, s) do { } while (0)
#define RWATCH(a, v, s) do { } while (0)
#endif

static inline uint8_t *mem_fast(uint32_t a) {
    uint32_t p = a & 0x1FFFFFFFu;
    if (p < 0x00800000u)
        return &psx_ram[p & (PSX_RAM_SIZE - 1)];
    if ((p & 0xFFFFFC00u) == 0x1F800000u && a < 0xA0000000u)
        return &psx_scratch[p & 0x3FF];
    return 0;
}

/* precise screen positions (gte.c): one entry per RAM / scratchpad word; a word copied by lw / sw keeps its
 * precise value through a small cache keyed by the word's value (the games build their polygons from vertex
 * arrays, so the GTE's stores are rarely the words the GPU reads) */
typedef struct { uint32_t val; float x, y, z; } PgWord;
extern PgWord *pgxp_mem;
extern PgWord pgxp_vcache[4096];
static inline uint32_t pgxp_idx(uint32_t a) {
    return (a & 0x1FFFFC00u) == 0x1F800000u ? (0x200000u >> 2) + ((a & 0x3FCu) >> 2) : (a & 0x1FFFFCu) >> 2;
}
static inline uint32_t pgxp_hash(uint32_t v) { return (v * 2654435761u) >> 20; }

static inline uint32_t mem_r32(uint32_t a) {
    uint8_t *p = mem_fast(a);
    if (p) {
        uint32_t v; memcpy(&v, p, 4);
        RWATCH(a, v, 4);
        if (pgxp_mem) {
            const PgWord *w = &pgxp_mem[pgxp_idx(a)];
            if (w->val == v && w->z > 0) pgxp_vcache[pgxp_hash(v)] = *w;
        }
        return v;
    }
    return io_read(a, 4);
}
static inline uint32_t mem_r16(uint32_t a) {
    uint8_t *p = mem_fast(a);
    if (p) { uint16_t v; memcpy(&v, p, 2); RWATCH(a, v, 2); return v; }
    return io_read(a, 2) & 0xFFFF;
}
static inline uint32_t mem_r8(uint32_t a) {
    uint8_t *p = mem_fast(a);
    if (p) { RWATCH(a, *p, 1); return *p; }
    return io_read(a, 1) & 0xFF;
}
static inline void mem_w32(uint32_t a, uint32_t v) {
    if (cpu.sr & SR_ISC) return;
    WATCH(a, v, 4);
    uint8_t *p = mem_fast(a);
    if (p) {
        memcpy(p, &v, 4);
        if (pgxp_mem) {
            PgWord *w = &pgxp_mem[pgxp_idx(a)];
            const PgWord *c = &pgxp_vcache[pgxp_hash(v)];
            if (c->val == v && c->z > 0) *w = *c;
            else if (w->z > 0) w->z = 0;
        }
        return;
    }
    io_write(a, v, 4);
}
static inline void mem_w16(uint32_t a, uint32_t v) {
    if (cpu.sr & SR_ISC) return;
    WATCH(a, v, 2);
    uint8_t *p = mem_fast(a);
    if (p) { uint16_t h = (uint16_t)v; memcpy(p, &h, 2); return; }
    io_write(a, v & 0xFFFF, 2);
}
static inline void mem_w8(uint32_t a, uint32_t v) {
    if (cpu.sr & SR_ISC) return;
    WATCH(a, v, 1);
    uint8_t *p = mem_fast(a);
    if (p) { *p = (uint8_t)v; return; }
    io_write(a, v & 0xFF, 1);
}
/* unaligned word loads/stores (little endian) */
static inline uint32_t mem_lwl(uint32_t a, uint32_t old) {
    uint32_t w = mem_r32(a & ~3u), s = a & 3;
    return (old & (0x00FFFFFFu >> (s * 8))) | (w << (24 - s * 8));
}
static inline uint32_t mem_lwr(uint32_t a, uint32_t old) {
    uint32_t w = mem_r32(a & ~3u), s = a & 3;
    return (old & ~(0xFFFFFFFFu >> (s * 8))) | (w >> (s * 8));
}
static inline void mem_swl(uint32_t a, uint32_t v) {
    uint32_t w = mem_r32(a & ~3u), s = a & 3;
    mem_w32(a & ~3u, (w & (0xFFFFFF00u << (s * 8))) | (v >> (24 - s * 8)));
}
static inline void mem_swr(uint32_t a, uint32_t v) {
    uint32_t w = mem_r32(a & ~3u), s = a & 3;
    mem_w32(a & ~3u, (w & (0x00FFFFFFu >> (24 - s * 8))) | (v << (s * 8)));
}

/* ---- ALU helpers ---- */
static inline void op_mult(uint32_t a, uint32_t b) {
    int64_t p = (int64_t)(int32_t)a * (int64_t)(int32_t)b;
    cpu.lo = (uint32_t)p; cpu.hi = (uint32_t)((uint64_t)p >> 32);
}
static inline void op_multu(uint32_t a, uint32_t b) {
    uint64_t p = (uint64_t)a * (uint64_t)b;
    cpu.lo = (uint32_t)p; cpu.hi = (uint32_t)(p >> 32);
}
static inline void op_div(uint32_t a, uint32_t b) {
    int32_t n = (int32_t)a, d = (int32_t)b;
    if (d == 0) { cpu.hi = a; cpu.lo = n >= 0 ? 0xFFFFFFFFu : 1u; }
    else if (a == 0x80000000u && d == -1) { cpu.hi = 0; cpu.lo = 0x80000000u; }
    else { cpu.lo = (uint32_t)(n / d); cpu.hi = (uint32_t)(n % d); }
}
static inline void op_divu(uint32_t a, uint32_t b) {
    if (b == 0) { cpu.hi = a; cpu.lo = 0xFFFFFFFFu; }
    else { cpu.lo = a / b; cpu.hi = a % b; }
}

/* ---- coprocessors ---- */
uint32_t cop0_read(int reg);
void cop0_write(int reg, uint32_t v);
void cop0_rfe(void);
uint32_t gte_read_data(int reg);
void pgxp_store(uint32_t addr, int reg); /* gte.c: a screen position stored to RAM keeps its precise value */
void pgxp_load(uint32_t addr, int reg);  /* gte.c: ... and gets it back when loaded into the GTE */
uint32_t gte_read_ctrl(int reg);
void gte_write_data(int reg, uint32_t v);
void gte_write_ctrl(int reg, uint32_t v);
void gte_command(uint32_t cmd);

/* ---- control flow / system ---- */
void dispatch_call(uint32_t addr);

/* Shadow return stack: 'jr ra' is a C return only when ra is the address the caller expects.
 * Otherwise the code is jumping somewhere computed (hand-written asm that saved ra as data), and
 * the dispatcher continues there. */
#define RS_MAX 65536
extern uint32_t rs_stack[RS_MAX];
extern int rs_top;
#define CALL(pc8, call) do { R31 = (pc8); rs_stack[rs_top++] = (pc8); call; rs_top--; } while (0)
#define RET() do { if (R31 == rs_stack[rs_top - 1]) return; dispatch_call(R31); return; } while (0)
#define JUMP_REG(t) do { if ((t) == rs_stack[rs_top - 1]) return; dispatch_call(t); } while (0)
/* call guest code from the runtime (BIOS callbacks, interrupt handlers) */
#define GUEST_RA 0xBFC0FFF0u
void call_guest(uint32_t addr);
void bios_syscall(uint32_t pc);
void bios_break(uint32_t pc, uint32_t code);
void bad_insn(uint32_t pc, uint32_t word);

typedef struct { uint32_t start, end; void (*fn)(void); } FuncEntry;
typedef struct {
    const char *name;
    uint32_t base, code_lo, code_hi;
    int is_overlay;
    uint32_t ident; /* first word of the overlay file, identifies which one is in RAM */
    const FuncEntry *funcs;
    int count;
} ImageEntry;
extern const ImageEntry recomp_images[];
extern const int recomp_image_count;

/* ---- save states (state.c, NOTES.md "Save states"): the guest call chain is rebuilt by entering each frame at its
 * function start (after the hook point) or at the return site of the call it was in. Every generated function checks
 * state_resume_ra on entry; the driver sets it just before calling the function. */
extern uint32_t state_resume_ra;
void state_resume_bad(uint32_t addr);
typedef struct { uint32_t addr; int fn; } StateSite;
extern const StateSite *const state_sites[]; /* per image (recomp_images order), sorted by address */
extern const int state_site_count[];
extern const uint64_t recomp_gen_id;
extern void (*const fn_ptr[])(void);

/* ---- per-function hook points (tracing now; native replacements of game functions later) ---- */
extern uint8_t fn_flags[];
extern const uint32_t fn_addr[];
extern const char *const fn_image[];
extern const int recomp_func_count;
int fn_hook(int idx, uint32_t addr); /* returns 1 when a native replacement ran instead */
extern uint16_t fn_trail[64]; /* last functions entered (for crash reports) */
extern uint32_t fn_trail_pos;
#define HOOK_POINT(idx, addr) do { fn_trail[fn_trail_pos++ & 63] = (uint16_t)(idx);     if (fn_flags[idx] && fn_hook(idx, addr)) return; } while (0)
