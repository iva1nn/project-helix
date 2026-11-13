/* Fixes for bugs of the original games themselves (not of the port): the game's code runs unchanged except where a
 * fix below says so, and only in the case the bug needs.
 *
 * FE2 disc 1, area A, the tile puzzle (room 74 of the LOCA overlay: three rows of five numbered tiles, a lever at the
 * end of each row; NOTES.md "Crash after the keypad: the tile puzzle's count"). The press handler (LOCA 0x800B1CBC)
 * keeps the number of lit tiles at 0x800BA79C and the order they were lit in at 0x800BB510 (five words). It stores
 * the pressed tile at order[count] before it looks at anything (0x800B1DE8), lights the tile only while count < 5
 * (an unsigned compare), and on a lit tile decrements count without a floor (0x800B1F68). A row's lever, pulled
 * while the wrong-answer blink runs, clears only that row's tiles and sets count to 0, so tiles lit in another row
 * stay lit and are no longer counted: pressing them takes count below zero, and every press then stores at
 * order[-1], order[-2], ... The fifth store below the array, order[-5] = 0x800BB4FC, is the next pointer of LOCA's
 * last actor descriptor (0x800BB4CC); when the area later spawns that actor (the fake gun, at the dialogue after the
 * keypad) the spawn loop (0x800126E4) follows the tile number as a pointer and the game calls address 0x0000FFFF.
 * The other end, a press with five tiles lit, stores at order[5] = 0x800BB524 (the combination dial's state).
 * Reproduced with plain pad input from the area's start (NOTES.md); the same input does the same on a PlayStation.
 * The fix: count stays in 0..5 (a lit tile that was no longer counted goes dark without a decrement) and the word
 * after the array keeps its value.
 *
 * States saved after the damage (the user's slot of 4 Nov) are repaired when they are loaded: LOCA's actor
 * descriptors (0x800BB3C8, six of 0x34 bytes) are constant data the game only reads; any word of them that differs
 * from the disc's LOCA.OVR is put back (logged).
 *
 * FE2, all discs, the movie driver's stale DecDCTvlc resume (NOTES.md "Crash after a skipped cutscene: the stale
 * DecDCTvlc resume"). Room backgrounds are STR streams decoded in slices: the driver (0x8005B0D4) starts a frame
 * with DecDCTvlc(bitstream, buffer, table) (0x8005BC20; DecDCTvlcSize 0x157C set at 0x8005A350), sets its "frame in
 * progress" flag (0x800A4FA3) without looking at the result, and at its next call goes on with DecDCTvlc(0, 0, table)
 * up to three times. A frame whose run-length data fits in one slice (the first frame of the stream at sector 344
 * of disc 4, 0x14C0 words) is finished by the first call; DecDCTvlc stores its context (0x8007E260: bit buffer,
 * bitstream and output pointers) only when it pauses, so the resume goes on from the context of the last frame that
 * did pause: an old position in the stream ring (0x80095000..), whose sectors have been replaced since, and an old
 * output pointer. What the old position holds now decides the outcome: mostly an end code soon (nothing seen), but
 * e.g. after START skips the cutscene of area D room 1 (Glas at the door) it is data without one, the resume runs
 * to the slice limit and the next one runs on past the second run-length buffer (0x801A5B1C + 0xB410) over the
 * decoder's table behind it (0x801B0F2C) and the heap block after that (a model, 0x801B113C). From then on every
 * frame decodes to noise and overruns again, and the next unload of the area walks the damaged model's texture list
 * (0x8001C810) decrementing reference counts all over RAM: "call to 00000000 (ra=800758E0)" (libpad's
 * _padGunExec made non-zero), "call to 80FFFF48 (ra=80017008)". The game's own code does this from plain pad
 * input; nothing in the port's timing is needed for it (the same bytes in the ring give the same on a PlayStation).
 * The fix: when a DecDCTvlc call returns "finished", the saved context is set to a finished one (no continuation
 * address, end code, output pointer at the end of the output), so a resume without a paused frame returns
 * "finished" at once and writes nothing; a paused frame resumes as before.
 * FE1 has the same driver (0x800572CC, flag 0x80092F4B) and the same DecDCTvlc (0x80057D30, context 0x8007A1C8,
 * DecDCTvlcSize 0x80057D08) with the same unchecked resume: the same fix there. */
#include <stdlib.h>
#include "hw.h"

int overlay_loaded(const char *name);
int cdrom_find_file(const char *path, uint32_t *out_lba, uint32_t *out_size);
void call_original(int idx);

#define OVL_BASE 0x800AF0C8u
#define TILE_PRESS 0x800B1CBCu
#define TILE_STORE_AT 0x800B1DE8u   /* sw v1, 0(v0): order[count] = tile */
#define TILE_STORE_WORD 0xAC430000u
#define TILE_COUNT 0x800BA79Cu
#define TILE_ORDER 0x800BB510u
#define TILE_N 5
#define DESC_LO 0x800BB3C8u         /* LOCA's actor descriptors (spawn table entries), constant data */
#define DESC_HI 0x800BB500u

#define VLC_FE2 0x8005BC20u          /* libpress DecDCTvlc(bitstream or 0 = go on, buffer, table) */
#define VLC_CTX_FE2 0x8007E260u      /* its context while paused: +8 the last code, +0x10 output pointer, +0x20 end of
                                      * the output, +0x30 where to go on (0 = at the end-code test) */
#define VLC_FE1 0x80057D30u          /* FE1: the same routine and the same driver (0x800572CC, flag 0x80092F4B, */
#define VLC_CTX_FE1 0x8007A1C8u      /* DecDCTvlcSize 0x80057D08) */
#define VLC_ENTRY_WORD 0x23BDFFFCu   /* addi sp, sp, -4 */

static int fe2, tile_idx = -1, vlc_idx = -1, dbg;
static uint32_t VLC, VLC_CTX;

static int vlc_hook(void) {
    if (mem_r32(VLC) != VLC_ENTRY_WORD) return 0;
    if (dbg && !R4 && !mem_r32(VLC_CTX + 0x30) && mem_r32(VLC_CTX + 8) == 0x3FF &&
        mem_r32(VLC_CTX + 0x10) == mem_r32(VLC_CTX + 0x20))
        LOG("gamefix: DecDCTvlc: a resume with no paused frame, nothing to do (frame %llu)\n",
            (unsigned long long)frame_count);
    call_original(vlc_idx);
    if (R2 == 0) { /* finished: a resume now has nothing to go on with */
        mem_w32(VLC_CTX + 0x30, 0);
        mem_w32(VLC_CTX + 8, 0x3FF);
        mem_w32(VLC_CTX + 0x10, mem_r32(VLC_CTX + 0x20));
    }
    return 1;
}

static int is_tile_puzzle(void) {
    return fe2 && overlay_loaded("LOCA") && mem_r32(TILE_STORE_AT) == TILE_STORE_WORD;
}

static int tile_press_hook(void) {
    if (!is_tile_puzzle()) return 0;
    int32_t n = (int32_t)mem_r32(TILE_COUNT);
    if (n < 0 || n > TILE_N) { /* a state saved after the bug: start from a count the array holds */
        LOG("gamefix: tile puzzle count %d set to %d (frame %llu)\n", n, n < 0 ? 0 : TILE_N, (unsigned long long)frame_count);
        n = n < 0 ? 0 : TILE_N;
        mem_w32(TILE_COUNT, (uint32_t)n);
    }
    uint32_t past = TILE_ORDER + 4u * TILE_N, keep = mem_r32(past);
    call_original(tile_idx);
    int32_t after = (int32_t)mem_r32(TILE_COUNT);
    if (n == TILE_N && mem_r32(past) != keep) {
        mem_w32(past, keep);
        if (dbg) LOG("gamefix: tile puzzle: the store past the order array undone (frame %llu)\n", (unsigned long long)frame_count);
    }
    if (after < 0) {
        mem_w32(TILE_COUNT, 0);
        if (dbg) LOG("gamefix: tile puzzle: a lit tile no longer counted went dark, count stays 0 (frame %llu)\n",
                     (unsigned long long)frame_count);
    }
    return 1;
}

/* after a save state is loaded: the damage the bug left in LOCA's actor descriptors, put back from the disc */
void gamefix_state_loaded(void) {
    if (!fe2 || !overlay_loaded("LOCA")) return;
    uint32_t lba, size;
    if (!cdrom_find_file("OVERLAY/LOCA.OVR", &lba, &size) || DESC_HI - OVL_BASE > size) return;
    uint32_t off = DESC_LO - OVL_BASE, end = DESC_HI - OVL_BASE;
    uint8_t buf[2 * 2048], s[2352];
    uint32_t first = off / 2048, n = 0;
    for (uint32_t k = 0; k < 2; k++) {
        if (!cdrom_read_sector(lba + first + k, s)) return;
        memcpy(buf + 2048 * k, s + 24, 2048);
    }
    if (end - first * 2048 > sizeof buf) return;
    for (uint32_t o = off; o < end; o += 4) {
        uint32_t want;
        memcpy(&want, buf + (o - first * 2048), 4);
        uint32_t a = OVL_BASE + o, have = mem_r32(a);
        if (have == want) continue;
        LOG("gamefix: LOCA actor descriptor word %08X was %08X, the disc has %08X: put back\n", a, have, want);
        mem_w32(a, want);
        n++;
    }
    if (n) LOG("gamefix: %u words of LOCA's actor descriptors repaired (damage of the tile puzzle bug)\n", n);
}

void gamefix_init(void) {
    fe2 = !strncmp(game_serial, "SLUS-0126", 9) || !strncmp(game_serial, "SLUS-0127", 9);
    int fe1 = !strcmp(game_serial, "SLUS-00920") || !strncmp(game_serial, "SLUS-0105", 9);
    dbg = getenv("FE_DBG_GAMEFIX") != 0;
    if (getenv("FE_NO_GAMEFIX")) { fe2 = 0; return; }
    if (fe1 || fe2) { /* both games: the stale DecDCTvlc resume */
        VLC = fe1 ? VLC_FE1 : VLC_FE2;
        VLC_CTX = fe1 ? VLC_CTX_FE1 : VLC_CTX_FE2;
        vlc_idx = hook_register_in("main", VLC, vlc_hook);
        if (vlc_idx < 0) LOG("gamefix: no function at %08X\n", VLC);
    }
    if (!fe2) return;
    tile_idx = hook_register_in("LOCA", TILE_PRESS, tile_press_hook);
    if (tile_idx < 0) LOG("gamefix: no LOCA function at %08X\n", TILE_PRESS);
}
