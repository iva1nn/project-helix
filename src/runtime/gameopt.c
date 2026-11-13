/* The games' own options, set by the port's options menu (menu.c; NOTES.md "Options menu"). Both games keep their
 * options in one small block of RAM and read them live (a save on the memory card takes the difficulty with it, as
 * on the PlayStation); only the sound (stereo / mono, master volume) has to be pushed to the sound library. The menu
 * writes the block; the sound library calls the original sound screens make at a change are run from a hook on the
 * shell's per-frame function (the game's own thread, between two of its frames).
 *
 *   FE2 0x8007B914: +0 subtitles, +1 difficulty (1 hard), +2 vibration, +3 stereo, +4 master volume 0-127,
 *       +5 auto draw, +6 control type (1 3D), +7 save beacon, +0x2C the 8 actions' buttons (halfwords, the game's
 *       pad bits), defaults at 0x8007B178. Boot sound setup 80028450. Shell frame: GSHELL 800AFE88.
 *   FE1 0x80077804: +0 subtitles, +1 difficulty, +2 vibration, +3 button configuration 0-3, +4 stereo,
 *       +5 master volume 0-127. Boot sound setup 80026830. Shell frame: GSHELL 800AF7EC.
 * Found in the recompiled options screens (FE2 GSHELL 800B35D4, FE1 GSHELL 800B01A4 / 800B0AD4 / 800B0BB8) and
 * checked by RAM dumps after changing each option on the original screens. */
#include <stdlib.h>
#include "hw.h"
#include "gameopt.h"

int overlay_loaded(const char *name);

typedef struct {
    uint32_t base, boot_sound_fn, frame_fn; /* boot_sound_fn: the boot-time sound setup (not used, see frame_hook) */
    int8_t off[GO_N];
} Game;

static const Game FE2 = {0x8007B914u, 0x80028450u, 0x800AFE88u, {0, 1, 2, 3, 4, 5, 7, 6, -1}};
static const Game FE1 = {0x80077804u, 0x80026830u, 0x800AF7ECu, {0, 1, 2, 4, 5, -1, -1, -1, 3}};
static const Game *g;
static int sound_pending, dbg;
int gopt_fe1;

int gopt_game(void) { return g ? (gopt_fe1 ? 1 : 2) : 0; }
int gopt_has(int id) { return g && id >= 0 && id < GO_N && g->off[id] >= 0; }
int gopt_get(int id) { return gopt_has(id) ? mem_r8(g->base + (uint32_t)g->off[id]) : 0; }

void gopt_set(int id, int v) {
    if (!gopt_has(id)) return;
    mem_w8(g->base + (uint32_t)g->off[id], (uint8_t)v);
    if (dbg) LOG("gameopt: %d = %d (frame %llu)\n", id, v, (unsigned long long)frame_count);
    if (id == GO_STEREO) sound_pending |= 1;
    if (id == GO_VOLUME) sound_pending |= 2;
}

/* one call of a game function with a0, from a hook (the caller keeps and restores the registers) */
static void gcall(uint32_t fn, uint32_t a0) {
    R4 = a0;
    call_guest(fn);
}

/* FE2's button layout: action i (0 fire .. 7 evade) -> one of the 8 game pad bits; choosing a button another action
 * has swaps the two, as the game's own screen does (GSHELL 800B4C74) */
int gopt_map_get(int i) { return gopt_fe1 || !g ? 0 : mem_r16(g->base + 0x2Cu + 2u * (uint32_t)i); }
void gopt_map_set(int i, int bit) {
    if (gopt_fe1 || !g) return;
    int old = gopt_map_get(i);
    for (int k = 0; k < 8; k++)
        if (gopt_map_get(k) == bit) mem_w16(g->base + 0x2Cu + 2u * (uint32_t)k, (uint16_t)old);
    mem_w16(g->base + 0x2Cu + 2u * (uint32_t)i, (uint16_t)bit);
}
void gopt_map_reset(void) { /* the game's "reset to default": the table at 0x8007B178, vibration on, classic */
    if (gopt_fe1 || !g) return;
    for (int k = 0; k < 8; k++) mem_w16(g->base + 0x2Cu + 2u * (uint32_t)k, (uint16_t)mem_r16(0x8007B178u + 2u * (uint32_t)k));
    gopt_set(GO_VIBRATION, 1);
    gopt_set(GO_CONTROL_TYPE, 0);
}

/* the shell: the main menu with its Options entry chosen, the main menu at all, the original options screens */
int gopt_main_menu_options(void) {
    if (!g || !overlay_loaded("GSHELL")) return 0;
    if (gopt_fe1) return mem_r32(0x80082D6Cu) == 2 && mem_r8(0x800B4B21u) == 2;
    return mem_r8(0x800BC9DCu) == 8 && mem_r8(0x800BC9DDu) == 8;
}
int gopt_main_menu(void) {
    if (!g || !overlay_loaded("GSHELL")) return 0;
    if (gopt_fe1) return mem_r32(0x80082D6Cu) == 2;
    int n = mem_r8(0x800BC9DCu);
    return n >= 6 && n <= 8 && mem_r8(0x800BC9DDu) == n;
}
/* FE1: modules 3 options, 5 controls, 6 sound (10 load game, 12 the credits). FE2: the shell's node pair
 * 0x800BC9DC (now) / 0x800BC9DD (target): 6-8 the main menu's entries, 5 and 9 transitions, target 0x0C the
 * options list (its cursor 0x800BC9DE), target 0x11 its sub-screens (0x800BC9DE: 0 subtitles .. 4 extras); new and
 * load game use 0x0A, 0x0B, 0x0D, 0x0E, 0x12-0x14 (logged with FE_DBG_OPTIONS). In the extras screen the art
 * gallery's picture is up while the halfword 0x800BC9D8 is not 0xFFFF. */
static int fe2_gallery(void) {
    return mem_r8(0x800BC9DDu) == 0x11 && mem_r8(0x800BC9DEu) == 4 && mem_r16(0x800BC9D8u) != 0xFFFFu;
}
int gopt_original_screen(void) {
    if (!g || !overlay_loaded("GSHELL")) return 0;
    if (gopt_fe1) { uint32_t m = mem_r32(0x80082D6Cu); return m == 3 || m == 5 || m == 6; }
    int t = mem_r8(0x800BC9DDu);
    return (t == 0x0C || t == 0x11) && !fe2_gallery();
}
/* the content the options lead to: FE1 the credits (module 12), FE2 the extras' art gallery */
int gopt_extra_running(void) {
    if (!g) return 0;
    if (gopt_fe1) return mem_r32(0x80082D6Cu) == 12;
    return overlay_loaded("GSHELL") && fe2_gallery();
}
int gopt_cursor(void) { /* the options list's cursor row: FE1 shared with its main menu, FE2 0x800BC9DE */
    return gopt_fe1 ? mem_r8(0x800B4B21u) : mem_r8(0x800BC9DEu);
}

/* FE_DBG_OPTIONS: the shell's state at every change (menu.c calls this every frame): FE1 module | cursor << 16,
 * FE2 node | target << 8 | cursor << 16 | art gallery up << 24, FFFFFF while the shell is not loaded (FE2) */
void gopt_debug_frame(void) {
    if (!dbg || !g) return;
    static uint32_t was = ~0u;
    int sh = overlay_loaded("GSHELL");
    uint32_t now = gopt_fe1 ? mem_r32(0x80082D6Cu) | (uint32_t)(sh ? mem_r8(0x800B4B21u) : 0xFF) << 16
                 : !sh ? 0xFFFFFFu
                       : (uint32_t)mem_r8(0x800BC9DCu) | (uint32_t)mem_r8(0x800BC9DDu) << 8 | (uint32_t)mem_r8(0x800BC9DEu) << 16 |
                             (uint32_t)fe2_gallery() << 24;
    if (now != was) LOG("gameopt: shell state %06X (frame %llu)\n", now, (unsigned long long)frame_count);
    was = now;
}

static int frame_hook(void) {
    if (!sound_pending || !overlay_loaded("GSHELL")) return 0;
    int what = sound_pending;
    sound_pending = 0;
    Cpu keep = cpu;
    /* the calls the original sound screens make at a change (FE2 GSHELL 800B3894 / 800B3974, FE1 GSHELL 800B0BB8 /
     * 800B0AD4 without its slider drawing); not the boot function, which sets the music's own level to the master
     * volume (it fades in from there) and so left the menu music near silent */
    int st = gopt_get(GO_STEREO), vol = gopt_get(GO_VOLUME);
    if (!gopt_fe1) {
        if (what & 1) { gcall(0x80020CB0u, (uint32_t)st); gcall(st == 1 ? 0x80073CBCu : 0x80073CACu, 0); } /* CdMix, SsSetStereo / Mono */
        if (what & 2) {
            mem_w16(0x80082D38u, (uint16_t)vol);
            gcall(0x80020C98u, (uint32_t)(int32_t)(int8_t)vol); /* master volume */
            gcall(0x8001F710u, 0x7F);                         /* the music at its full level under it */
            gcall(0x8001FF10u, mem_r32(0x80082D3Cu));
        }
    } else {
        if (what & 1) { gcall(0x8001FB5Cu, (uint32_t)st); gcall(st ? 0x8006FB2Cu : 0x8006FB1Cu, 0); }
        if (what & 2) {
            mem_w16(0x80082AB0u, (uint16_t)vol);
            gcall(0x8001FB44u, (uint32_t)(int32_t)(int8_t)vol);
            gcall(0x8001EC5Cu, (uint32_t)vol);
            gcall(0x8001F1E4u, mem_r32(0x80082AB4u));
        }
    }
    memcpy(cpu.r, keep.r, sizeof cpu.r);
    cpu.hi = keep.hi;
    cpu.lo = keep.lo;
    if (dbg) LOG("gameopt: sound applied (stereo %d, volume %d)\n", st, vol);
    return 0; /* then the shell's frame as usual */
}

void gopt_init(void) {
    if (!strcmp(game_serial, "SLUS-00920") || !strncmp(game_serial, "SLUS-0105", 9)) { g = &FE1; gopt_fe1 = 1; }
    else if (!strncmp(game_serial, "SLUS-0126", 9) || !strncmp(game_serial, "SLUS-0127", 9)) g = &FE2;
    else return;
    dbg = getenv("FE_DBG_OPTIONS") != 0;
    if (hook_index(g->frame_fn) < 0) { LOG("gameopt: no shell frame function\n"); return; }
    hook_register(g->frame_fn, frame_hook);
}

#include "state.h"
void gopt_state(StIO *io) {
    st_begin(io, "GOPT", 1);
    ST(io, sound_pending);
    st_end(io);
}
