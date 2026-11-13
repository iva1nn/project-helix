/* Function hook points: every recompiled function checks fn_flags[index] on entry.
 *   FE_TRACE=8005BBF8,800133F4   log calls (arguments, return address, frame)
 *   FE_TRACE=all                 log every call (huge)
 * Native replacements register here too (hook_register). */
#include <stdlib.h>
#include "hw.h"

#define F_TRACE 1
#define F_NATIVE 2

static NativeFn natives[1 << 14];
static int trace_all;

static int find_idx(uint32_t addr, int nth) {
    for (int i = 0; i < recomp_func_count; i++)
        if (fn_addr[i] == addr && nth-- == 0) return i;
    return -1;
}

/* every image that has a function at addr (overlays share addresses) */
int hook_index(uint32_t addr) { return find_idx(addr, 0); }

void hook_register(uint32_t addr, NativeFn fn) {
    for (int n = 0;; n++) {
        int i = find_idx(addr, n);
        if (i < 0) break;
        natives[i] = fn;
        fn_flags[i] |= F_NATIVE;
    }
}

/* only the function of one image (an overlay shares its addresses with the others); returns its index or -1 */
int hook_register_in(const char *image, uint32_t addr, NativeFn fn) {
    for (int n = 0;; n++) {
        int i = find_idx(addr, n);
        if (i < 0) return -1;
        if (strcmp(fn_image[i], image)) continue;
        natives[i] = fn;
        fn_flags[i] |= F_NATIVE;
        return i;
    }
}

void call_original(int idx);

/* debugging aid: libcd stream ring state around StGetNext (FE2 disc 1 addresses) */
static int dbg_ring_hook(void) {
    static int idx = -1;
    if (idx < 0) idx = find_idx(0x800659FC, 0);
    LOG("ring@frame %llu:", (unsigned long long)frame_count);
    for (int k = 0; k < 32; k++) {
        uint32_t h = 0x80094F48 + 32 * k;
        LOG(" %d:%04X/%d/%d/%d", k, mem_r16(h), mem_r16(h + 4), mem_r16(h + 6), mem_r32(h + 8));
    }
    LOG("\n");
    call_original(idx);
    LOG("  StGetNext -> %d addr=%08X\n", R2, mem_r32(0x80082F24));
    return 1;
}

void hooks_init(void) {
    if (getenv("FE_DBG_RING")) hook_register(0x800659FC, dbg_ring_hook);
    const char *t = getenv("FE_TRACE");
    if (!t) return;
    if (!strcmp(t, "all")) {
        trace_all = 1;
        memset(fn_flags, F_TRACE, (size_t)recomp_func_count);
        return;
    }
    while (*t) {
        uint32_t a = (uint32_t)strtoul(t, (char **)&t, 16);
        for (int n = 0;; n++) {
            int i = find_idx(a, n);
            if (i < 0) break;
            fn_flags[i] |= F_TRACE;
        }
        while (*t == ',' || *t == ' ') t++;
    }
}

extern void (*const fn_ptr[])(void);

/* run the recompiled original of a hooked function (from inside its hook). The calls under way are kept: a save
 * state is only taken outside them (a hook's C frame would be lost), and a load that ends them (state.c leaves the
 * frames with a longjmp) switches their hooks back on. */
static struct { int idx; uint8_t f; } co[64];
int hooks_native_depth;
void call_original(int idx) {
    uint8_t f = fn_flags[idx];
    if (hooks_native_depth < 64) { co[hooks_native_depth].idx = idx; co[hooks_native_depth].f = f; }
    hooks_native_depth++;
    fn_flags[idx] = 0;
    fn_ptr[idx]();
    fn_flags[idx] = f;
    hooks_native_depth--;
}

void hooks_state_loaded(void) {
    while (hooks_native_depth > 0) {
        hooks_native_depth--;
        if (hooks_native_depth < 64) fn_flags[co[hooks_native_depth].idx] = co[hooks_native_depth].f;
    }
}

static int64_t trace_from = -1;

int fn_hook(int idx, uint32_t addr) {
    if (trace_from < 0) { const char *f = getenv("FE_TRACE_FROM"); trace_from = f ? atoll(f) : 0; }
    if ((fn_flags[idx] & F_TRACE) && (int64_t)frame_count >= trace_from) {
        LOG("call %s:%08X a0=%08X a1=%08X a2=%08X a3=%08X ra=%08X sp=%08X frame=%llu\n", fn_image[idx], addr,
            R4, R5, R6, R7, R31, R29, (unsigned long long)frame_count);
        const char *mem = getenv("FE_TRACE_MEM"); /* dump N bytes at a0 */
        if (mem) {
            int n = atoi(mem);
            LOG("  [a0]");
            for (int i = 0; i < n; i += 4) LOG(" %08X", mem_r32(R4 + i));
            LOG("\n");
        }
        if (getenv("FE_TRACE_RET") && !(fn_flags[idx] & F_NATIVE)) {
            call_original(idx);
            LOG("  ret %s:%08X v0=%08X v1=%08X\n", fn_image[idx], addr, R2, R3);
            return 1;
        }
    }
    if ((fn_flags[idx] & F_NATIVE) && natives[idx]) return natives[idx]();
    return 0;
}

#ifdef FE_WATCH
uint32_t watch_lo = 0xFFFFFFFF, watch_hi;
void watch_hit(uint32_t a, uint32_t v, int size) {
    LOG("watch: write%d [%08X] = %08X ra=%08X rs_top=%08X frame=%llu cyc=%lld\n", size * 8, a, v, R31,
        rs_top ? rs_stack[rs_top - 1] : 0, (unsigned long long)frame_count, (long long)cpu.cycles);
}
/* reads: the reading function is the target of the jal before the innermost return address (0 = reached by jalr,
 * then ra tells the call site); FE_RWATCH_FROM=FRAME limits the log */
uint32_t rwatch_lo = 0xFFFFFFFF, rwatch_hi;
static uint64_t rwatch_from;
void rwatch_hit(uint32_t a, uint32_t v, int size) {
    if (frame_count < rwatch_from) return;
    uint32_t ra = rs_top ? rs_stack[rs_top - 1] : 0, fn = 0;
    if (ra >= 8 && ((ra - 8) & 0x1FFFFFFF) < 0x200000) {
        uint32_t ins;
        memcpy(&ins, &psx_ram[(ra - 8) & 0x1FFFFC], 4);
        if ((ins >> 26) == 3) fn = ((ra - 8) & 0xF0000000u) | ((ins & 0x3FFFFFFu) << 2);
    }
    LOG("rwatch: read%d [%08X] = %08X fn=%08X ra=%08X depth=%d frame=%llu\n", size * 8, a, v, fn, ra, rs_top,
        (unsigned long long)frame_count);
}
static void watch_init(void) __attribute__((constructor));
static void watch_init(void) {
    const char *w = getenv("FE_WATCH");
    if (w) { watch_lo = (uint32_t)strtoul(w, (char **)&w, 16); if (*w == ',') watch_hi = (uint32_t)strtoul(w + 1, 0, 16); }
    w = getenv("FE_RWATCH");
    if (w) { rwatch_lo = (uint32_t)strtoul(w, (char **)&w, 16); if (*w == ',') rwatch_hi = (uint32_t)strtoul(w + 1, 0, 16); }
    w = getenv("FE_RWATCH_FROM");
    if (w) rwatch_from = strtoull(w, 0, 10);
}
#endif
