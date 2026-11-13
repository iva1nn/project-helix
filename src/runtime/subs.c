/* The games' subtitles drawn with the port's own text (NOTES.md "Native subtitles"). Both games draw the movie
 * subtitles (cutscenes and rooms) once per game frame with their text routine and font 1, one or two centred lines
 * under the picture. A native hook on that routine takes the calls from the subtitle sites: the line is kept for
 * menu.c (drawn like the port's notices, without their box), the routine runs unchanged (same cycles, replays stay
 * in step) and the glyph quads it wrote (POLY_FT4, 40 bytes, one run per call) are collapsed onto their first corner,
 * so nothing of the original shows. Other callers run untouched. The code is the same for both games; the addresses
 * are per game (the exe is the same on all four discs of each). */
#include <stdlib.h>
#include <SDL2/SDL.h>
#include "hw.h"
#include "ui.h"

typedef struct {
    uint32_t text_fn;  /* the text routine: (font, string, y) FE2 centred; (font, string, x, y) FE1 */
    uint32_t site[2];  /* return addresses of its two subtitle calls (first, second line) */
    int fe1;
} SubsGame;

/* FE2: 8005A8D0 (after the flag test at 8005A9F8, byte 0x8007B914); FE1: 80056AAC (flag 0x80077804) */
static const SubsGame FE2 = {0x8004AD48u, {0x8005A9D0u, 0x8005A9E8u}, 0};
static const SubsGame FE1 = {0x80048C60u, {0x80056BC8u, 0x80056BECu}, 1};
static const SubsGame *game;
static int text_idx, dbg;

#define MAXL 4
typedef struct { char s[128]; int y; } Line;
static Line line[MAXL];
static int nline;
static uint64_t line_frame = ~0ull; /* the game frame that drew them */

extern int remaster_on;
extern void (*hw_overlay_draw)(int w, int h);
int fonts_dialogue_char(int c);
int ui_unavailable(void);

/* where the text routine writes its next glyph: FE2 the frame's glyph array at its count, FE1 a pointer */
static uint32_t next_glyph(void) {
    if (game->fe1) return mem_r32(R28 + 0x428u);
    uint32_t base = mem_r32(0x80082E48u + 4u * mem_r32(0x80085448u));
    return base + 40u * (uint32_t)(int32_t)(int16_t)mem_r16(R28 + 0x7C6u);
}

static void keep(uint32_t s, int y) {
    if (frame_count != line_frame) { nline = 0; line_frame = frame_count; }
    if (nline == MAXL) return;
    Line *l = &line[nline];
    int n = 0;
    for (int c; n < (int)sizeof l->s - 1 && (c = mem_r8(s + (uint32_t)n)) != 0; n++) l->s[n] = (char)fonts_dialogue_char(c);
    while (n && l->s[n - 1] == ' ') n--;
    l->s[n] = 0;
    l->y = y;
    if (!n) return;
    nline++;
    if (dbg) LOG("subs: frame %llu y %d \"%s\"\n", (unsigned long long)frame_count, y, l->s);
}

void agent_text(uint32_t s, int font, int x, int y);

static int text_hook(void) {
    agent_text(R5, (int)R4, game->fe1 ? (int)(int16_t)R6 : -1, (int)(int16_t)(game->fe1 ? R7 : R6)); /* playtest: every text drawn */
    if (R31 != game->site[0] && R31 != game->site[1]) return 0; /* not a subtitle: the routine as it is */
    if (!remaster_on || !hw_overlay_draw || ui_unavailable()) return 0; /* the game's own text */
    keep(R5, (int)(int16_t)(game->fe1 ? R7 : R6));
    uint32_t a = next_glyph();
    call_original(text_idx);
    uint32_t b = next_glyph();
    for (uint32_t p = a; p < b && b - a <= 40u * 256u; p += 40u) {
        uint32_t xy = mem_r32(p + 8u);
        mem_w32(p + 0x10u, xy); mem_w32(p + 0x18u, xy); mem_w32(p + 0x20u, xy);
    }
    return 1;
}

void subs_init(void) {
    if (!strcmp(game_serial, "SLUS-00920") || !strncmp(game_serial, "SLUS-0105", 9)) game = &FE1;
    else if (!strncmp(game_serial, "SLUS-0126", 9) || !strncmp(game_serial, "SLUS-0127", 9)) game = &FE2;
    else return;
    dbg = getenv("FE_DBG_SUBS") != 0;
    text_idx = hook_index(game->text_fn);
    if (text_idx < 0) { game = 0; return; }
    hook_register(game->text_fn, text_hook);
}

void subs_state_loaded(void) { nline = 0; line_frame = ~0ull; } /* save states: the lines come again with the game's next */

/* the subtitle lines on screen now, top first (those of the last game frame that drew any, for 6 vblanks) */
int subs_lines(const char **out, int max) {
    if (!game || nline == 0 || frame_count - line_frame >= 6) return 0;
    int idx[MAXL], n = 0;
    for (int i = 0; i < nline; i++) {
        int k = n++;
        while (k && line[idx[k - 1]].y > line[i].y) { idx[k] = idx[k - 1]; k--; }
        idx[k] = i;
    }
    if (n > max) n = max;
    for (int i = 0; i < n; i++) out[i] = line[idx[i]].s;
    return n;
}
