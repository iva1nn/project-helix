/* FE2 disc 1, area A: the code door panel (five seven-segment digit windows over a keypad 7 8 9 / 4 5 6 / 1 2 3 /
 * <- 0 -> OK; LOCA's loop 0x800B0590; NOTES.md "Code door panel"). Presentation only: the hardware renderer's
 * picture changes, the game, its RAM and the software VRAM do not.
 *
 * The game draws its key cursor as a textured sprite (sprite list 0x800BA1A4, element 6, frame 11 of the 4-bit sheet
 * at VRAM 768,160 / CLUT 560,123): a 16x15-texel cell, u 175..191 v 160..175, stretched over 34x32 lines at
 * (258 + 30 * column, 178 - 20 * row). The box in that cell has no right side (its last column is never sampled, as
 * on the console), is 23 lines tall at a row pitch of 20 while the keys are ~17 lines tall at a pitch of ~19, and
 * its bottom edge lands on lines 200-201: below the 160-line movie picture (lines 40..199), in the bottom HUD strip,
 * which the fill layouts draw elsewhere (layout 1: the bottom band; layout 2: over the picture's lower part), so
 * the box came apart on screen. The key-press flash (element 5, frame 10: a half-transparent red cell, u 159..175
 * v 160..170, 34x22 lines) has the same placement.
 *
 * Here both quads are left out of the hardware picture and drawn again from the key's own rectangle, measured on
 * the remaster's 8x picture of the panel (abga_r1.mp4, 2560x1280 = 5x horizontally, 8x vertically over 512x160):
 * the cursor a closed cyan outline around the key (in the dark gap round it, so the key stays readable), the flash
 * the key's face in half-transparent red. Edges are snapped to the renderer's pixel grid, so every side has the same
 * thickness at any scale. They follow the game's own sprites: drawn when the game draws them (blink, flash), at
 * the key the sprite's position names. Off with the remaster textures (F6) or FE_NO_CODEPANEL. */
#include <math.h>
#include <stdlib.h>
#include "hw.h"

int overlay_loaded(const char *name);
extern int texrep_on;

#define LIST 0x800BA1A4u        /* the panel's sprite list: count byte, then a pointer to 28-byte elements */
#define LOOP_WORD_AT 0x800B06C0u /* addiu a2, a2, 0x102: the cursor's x = 258 + 30 * column */
#define LOOP_WORD 0x24C60102u

/* the keys on the 8x picture: columns x0..x1 (picture pixels / 5 = PS1 pixels), rows y0..y1 (/ 8 + 40 = lines) */
static const float key_x[4][2] = {{1301, 1440}, {1450, 1590}, {1601, 1743}, {1757, 1897}};
static const float key_y[4][2] = {{1088, 1220}, {941, 1073}, {789, 921}, {638, 770}}; /* row 0 = the bottom row */

static int fe2 = -1, off;

static int panel_up(void) {
    if (fe2 < 0) {
        fe2 = !strncmp(game_serial, "SLUS-01266", 10);
        off = getenv("FE_NO_CODEPANEL") != 0;
    }
    if (!fe2 || off || !texrep_on || !overlay_loaded("LOCA") || mem_r32(LOOP_WORD_AT) != LOOP_WORD) return 0;
    return mem_r8(LIST) == 8;
}

static float snap(float v) { return floorf(v * (float)hw_scale + 0.5f) / (float)hw_scale; }

static void rect(float x0, float y0, float x1, float y1, uint8_t r, uint8_t g, uint8_t b, const HwState *st) {
    if (x1 <= x0 || y1 <= y0) return;
    HwVert q[4];
    float px[4] = {x0, x1, x0, x1}, py[4] = {y0, y0, y1, y1};
    for (int i = 0; i < 4; i++) {
        q[i].x = (int)px[i]; q[i].y = (int)py[i]; q[i].u = q[i].v = 0;
        q[i].r = r; q[i].g = g; q[i].b = b;
    }
    float ax[3] = {px[0], px[1], px[2]}, ay[3] = {py[0], py[1], py[2]};
    float bx[3] = {px[1], px[2], px[3]}, by[3] = {py[1], py[2], py[3]};
    hw_tri_ex(&q[0], &q[1], &q[2], st, ax, ay, 0);
    hw_tri_ex(&q[1], &q[2], &q[3], st, bx, by, 0);
}

/* a textured quad of the panel sheet about to be drawn by the hardware renderer: 1 = drawn here instead.
 * x, y: its top-left corner without the drawing offset; ox, oy: the drawing offset; base: the state the quad would
 * be drawn with (drawing area, mask) */
int codepanel_quad(int tx, int ty, int depth, int clut_x, int clut_y, int umin, int vmin, int umax, int vmax, int x, int y,
                   int ox, int oy, const HwState *base) {
    if (tx != 768 || ty != 0 || depth != 0 || clut_x != 560 || clut_y != 123 || vmin != 160) return 0;
    int cursor = umin == 175 && umax == 191 && vmax == 175, flash = umin == 159 && umax == 175 && vmax == 170;
    if (!cursor && !flash) return 0;
    if (!panel_up()) return 0;
    /* the panel's own sprite (element 6 the cursor, frame 11; element 5 the flash, frame 10) at this very place: other
     * screens of area A keep the same sheet resident (LOCA is loaded in all of them) */
    uint32_t el = mem_r32(LIST + 4) + 28u * (cursor ? 6u : 5u);
    if ((el & 0xFF000000u) != 0x80000000u || mem_r8(el) != (cursor ? 11u : 10u) || (int16_t)mem_r16(el + 8) != x ||
        (int16_t)mem_r16(el + 10) != y)
        return 0;
    int col = x - 258, row = 178 - y;
    if (col < 0 || row < 0 || col % 30 || row % 20 || col / 30 > 3 || row / 20 > 3) return 0;
    col /= 30; row /= 20;
    if (row > 0 && col > 2) return 0; /* no key there */
    float kx0 = key_x[col][0] / 5.0f + (float)ox, kx1 = key_x[col][1] / 5.0f + (float)ox;
    float ky0 = key_y[row][0] / 8.0f + 40.0f + (float)oy, ky1 = key_y[row][1] / 8.0f + 40.0f + (float)oy;
    HwState st = *base;
    st.textured = 0; st.raw = 0; st.depth = 0; st.rtex = 0; st.rmode = 0; st.rnearest = 0;
    st.rstretch_x = st.rstretch_y = 0;
    if (flash) { /* the game's red, half over the key face (blend 0: average) */
        st.semi = 1; st.semi_mode = 0;
        rect(snap(kx0), snap(ky0), snap(kx1), snap(ky1), 152, 24, 24, &st);
        return 1;
    }
    /* the outline: 1.5 lines wide, a quarter on the key's edge, the rest in the gap around it */
    st.semi = 0;
    float in = 0.25f, outw = 1.25f;
    float ox0 = snap(kx0 - outw), ox1 = snap(kx1 + outw), oy0 = snap(ky0 - outw), oy1 = snap(ky1 + outw);
    float ix0 = snap(kx0 + in), ix1 = snap(kx1 - in), iy0 = snap(ky0 + in), iy1 = snap(ky1 - in);
    uint8_t r = 128, g = 240, b = 240; /* CLUT 560,123 entry 4 (0x7BD0), the cursor's own cyan */
    rect(ox0, oy0, ox1, iy0, r, g, b, &st); /* top, full width */
    rect(ox0, iy1, ox1, oy1, r, g, b, &st); /* bottom, full width */
    rect(ox0, iy0, ix0, iy1, r, g, b, &st); /* left */
    rect(ix1, iy0, ox1, iy1, r, g, b, &st); /* right */
    return 1;
}
