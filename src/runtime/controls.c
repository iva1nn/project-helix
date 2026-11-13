/* Directional ("Parasite Eve style") controls for Fear Effect 1 and 2, on by default: the stick or d-pad means
 * a direction on screen in every view. The player is turned toward it and the game gets plain "forward".
 * A C port of the remaster fork's FE2Controls (fe2_controls.cpp, production version of 31 Oct 2026; how it was
 * measured and tested: F:\fe_port\third_party\fe2_remaster\work\controls\README.md, F:\fe_remaster\docs\FINDINGS.md "Directional
 * controls"). Same rules, same constants:
 *  - the screen direction becomes a floor direction at the player's feet through the game's camera (camera axes
 *    laid on the floor, blended with the exact projection Jacobian where the floor is clearly below the eye);
 *  - the facing (actor +74, 4096 = 360 deg) turns toward it by at most TURN_STEP per frame; "Up" goes to the game
 *    once within WALK_MAX_DIFF (and stays on through a sharp turn once moving); at the vblank the facing is put back
 *    only where the game's update wrote back the facing it had read (never over a facing the game set itself, such
 *    as the ladder mount's: controls_vblank, 5 Nov);
 *  - after a camera cut the old camera is kept until the direction changes by more than HOLD_ANGLE;
 *  - the pad goes unchanged (tank controls) while L1 is held, on a ladder, paused, while the game does not update
 *    the player (puzzles, menus, cutscenes: player_on_pad, added 4 Nov), in a view that does not show the player,
 *    and after NO_RESPONSE_FRAMES of "Up" without movement until the direction turns or is released.
 * Applied once per frame to the pad state after it is recorded, so recordings hold the player's own input. */
#include <math.h>
#include <stdlib.h>
#include "hw.h"

int controls_directional = 1;

typedef struct {
    uint32_t pad_held, player_ptr, game_flags, pause_word, cam_obj_ptr, cam_target, cam_roll;
    int state_from_gp;
} GameAddrs;
static const GameAddrs FE2 = {0x80085590, 0x80082DDC, 0x80083020, 0x800854F0, 0x8008A514, 0x8008A47C, 0x8008A4AC, 1};
static const GameAddrs FE1 = {0x800872D0, 0x80082B2C, 0x80082D60, 0x80087230, 0x8008C49C, 0x8008C404, 0x8008C434, 0};

#define STATE_GP_OFFSET 1888
#define ACTOR_POS 60
#define ACTOR_FACING 74
#define STATE_FLAGS 256
#define STATE_EQUIP 0x8
#define STATE_FIRING 0x4
#define STATE_LADDER 0x4000
#define GAME_PAUSED 0x40
#define STICK_DEADZONE 45
#define STICK_DEADZONE_MOVING 24
#define SQUARE_SEEN_FRAMES 2
#define TURN_STEP 256
#define WALK_MAX_DIFF 683
#define HOLD_ANGLE 45.0
#define NO_RESPONSE_FRAMES 24
#define JACOBIAN_OFF_DEG 3.0
#define JACOBIAN_FULL_DEG 8.0
#define OFF_VIEW_RATIO 4.0
#define PI 3.14159265358979323846

/* PS1 pad bits (pad_buttons is active high here) */
#define BIT_UP (1u << 4)
#define BIT_RIGHT (1u << 5)
#define BIT_DOWN (1u << 6)
#define BIT_LEFT (1u << 7)
#define DIRECTION_BITS (BIT_UP | BIT_RIGHT | BIT_DOWN | BIT_LEFT)
#define BIT_L1 (1u << 10)
#define BIT_SQUARE (1u << 15)

typedef struct { int32_t eye[3], target[3], roll; int valid; } Camera;

static const GameAddrs *game;
static int active, bail, holding, up_prev, prev_valid, square_hidden;
static double bail_deg, hold_deg;
static Camera lock;
static int32_t last_pos[3];
static uint64_t last_move_frame, up_since, square_since, prev_frame;
static int32_t prev_read, prev_wrote;
static int dec_steer, dec_wrote = -1; /* this frame's decision, for the re-apply at the vblank */

static int in_ram(uint32_t a) { return (a & 0xFFE00000u) == 0x80000000u && (a & 0x1FFFFFu) < 0x1FFF00u; }

static uint32_t actor_addr(void) {
    uint32_t holder = mem_r32(game->player_ptr);
    if (!in_ram(holder)) return 0;
    uint32_t a = mem_r32(holder);
    return in_ram(a) ? a : 0;
}

/* FE2: gp + 1888 (gp may not be the game's while an interrupt runs: keep the last valid one); FE1: fixed */
static uint32_t last; /* state_addr's last valid one */
static uint32_t state_addr(void) {
    if (!game->state_from_gp) {
        uint32_t st = mem_r32(game->player_ptr);
        return in_ram(st) ? st : 0;
    }
    uint32_t gp = cpu.r[28];
    if (in_ram(gp)) {
        uint32_t st = mem_r32(gp + STATE_GP_OFFSET);
        if (in_ram(st)) last = st;
    }
    return last;
}

static Camera live_camera(void) {
    Camera c;
    memset(&c, 0, sizeof c);
    uint32_t obj = mem_r32(game->cam_obj_ptr);
    if (!in_ram(obj)) return c;
    for (int k = 0; k < 3; k++) {
        c.eye[k] = (int32_t)mem_r32(obj + 60 + 4 * k);
        c.target[k] = (int32_t)mem_r32(game->cam_target + 4 * k);
    }
    c.roll = (int32_t)mem_r32(game->cam_roll);
    c.valid = memcmp(c.eye, c.target, sizeof c.eye) != 0;
    return c;
}

static int same_camera(const Camera *a, const Camera *b) {
    return !memcmp(a->eye, b->eye, sizeof a->eye) && !memcmp(a->target, b->target, sizeof a->target) && a->roll == b->roll;
}

static double angle_diff(double a, double b) { return fabs(fmod(a - b + 540.0, 360.0) - 180.0); }

static int32_t wrap_facing(int32_t d) {
    d %= 4096;
    if (d > 2048) d -= 4096;
    else if (d < -2048) d += 4096;
    return d;
}

/* camera basis: forward f, right r, up' u, rolled rows row0 (screen right) and row1 (screen up) */
static int basis(const Camera *cam, double f[3], double row0[3], double row1[3]) {
    for (int k = 0; k < 3; k++) f[k] = (cam->target[k] - cam->eye[k]) / 65536.0;
    double fl = sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    if (fl < 1e-6) return 0;
    for (int k = 0; k < 3; k++) f[k] /= fl;
    double r[3] = {f[2], 0.0, -f[0]};
    double rl = sqrt(r[0] * r[0] + r[2] * r[2]);
    if (rl < 1e-9) { r[0] = 0.0; r[1] = f[2]; r[2] = -f[1]; rl = sqrt(r[1] * r[1] + r[2] * r[2]); }
    for (int k = 0; k < 3; k++) r[k] /= rl;
    double u[3] = {f[1] * r[2] - f[2] * r[1], f[2] * r[0] - f[0] * r[2], f[0] * r[1] - f[1] * r[0]};
    double a = cam->roll * 2.0 * PI / 4096.0, c = cos(a), s = sin(a);
    for (int k = 0; k < 3; k++) { row0[k] = c * r[k] - s * u[k]; row1[k] = s * r[k] + c * u[k]; }
    return 1;
}

/* the player's feet outside the view or behind the camera (puzzles, close-ups) */
static int player_off_view(const Camera *cam, const int32_t pos[3]) {
    double f[3], row0[3], row1[3], d[3];
    if (!basis(cam, f, row0, row1)) return 0;
    for (int k = 0; k < 3; k++) d[k] = (pos[k] - cam->eye[k]) / 65536.0;
    double X = 0, Y = 0, Z = 0;
    for (int k = 0; k < 3; k++) { X += row0[k] * d[k]; Y += row1[k] * d[k]; Z += f[k] * d[k]; }
    if (Z <= 1.0) return 1;
    return fmax(fabs(X), fabs(Y)) / Z > OFF_VIEW_RATIO;
}

/* screen direction (sx right, sy down) at the feet -> floor direction (wx, wz) */
static int screen_to_floor(const Camera *cam, const int32_t pos[3], double sx, double sy, double *wx, double *wz) {
    double f[3], row0[3], row1[3];
    if (!basis(cam, f, row0, row1)) return 0;
    double gr[2] = {row0[0], row0[2]};
    double grl = hypot(gr[0], gr[1]);
    if (grl < 1e-6) return 0;
    gr[0] /= grl; gr[1] /= grl;
    double hint[2] = {f[0], f[2]};
    if (hypot(hint[0], hint[1]) < 0.05) { hint[0] = row1[0]; hint[1] = row1[2]; }
    double gu[2] = {-gr[1], gr[0]};
    if (gu[0] * hint[0] + gu[1] * hint[1] < 0.0) { gu[0] = -gu[0]; gu[1] = -gu[1]; }
    double cx = sx * gr[0] - sy * gu[0], cz = sx * gr[1] - sy * gu[1];
    double cl = hypot(cx, cz);
    if (cl < 1e-9) return 0;
    cx /= cl; cz /= cl;
    *wx = cx; *wz = cz;
    double E[3], d[3];
    for (int k = 0; k < 3; k++) { E[k] = cam->eye[k] / 65536.0; d[k] = pos[k] / 65536.0 - E[k]; }
    double depression = atan2(-d[1], hypot(d[0], d[2])) * 180.0 / PI;
    double t = (depression - JACOBIAN_OFF_DEG) / (JACOBIAN_FULL_DEG - JACOBIAN_OFF_DEG);
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    double w = t * t * (3.0 - 2.0 * t);
    if (w <= 0.0) return 1;
    double X = row0[0] * d[0] + row0[1] * d[1] + row0[2] * d[2];
    double Y = row1[0] * d[0] + row1[1] * d[1] + row1[2] * d[2];
    double Z = f[0] * d[0] + f[1] * d[1] + f[2] * d[2];
    if (Z <= 50.0) return 1;
    double ja = row0[0] * Z - X * f[0], jb = row0[2] * Z - X * f[2];
    double jc = -(row1[0] * Z - Y * f[0]), jd = -(row1[2] * Z - Y * f[2]);
    double det = ja * jd - jb * jc;
    if (fabs(det) <= 1e-4 * (ja * ja + jb * jb + jc * jc + jd * jd)) return 1;
    double jx = (jd * sx - jb * sy) / det, jz = (-jc * sx + ja * sy) / det;
    double jl = hypot(jx, jz);
    if (jl < 1e-9) return 1;
    jx /= jl; jz /= jl;
    double bx = w * jx + (1.0 - w) * cx, bz = w * jz + (1.0 - w) * cz;
    if (hypot(bx, bz) > 0.2) { *wx = bx; *wz = bz; }
    return 1;
}

static void reset_steering(void) { active = bail = holding = up_prev = prev_valid = 0; }
static int select_game(void);

/* ------------------------------------------------------------------ is the player on the pad?
 * Both games (the same engine) read the pad for the player in one place: the player's update (FE2 0x80031FE8, FE1
 * 0x8002E6C8; a0 = the player's actor), called once per game frame from the room's object loop (FE2 0x80012664, FE1
 * 0x80012188). It runs the player's animation state machine and state handlers, which turn the d-pad into turning,
 * walking, running, stepping back and aiming. Every other screen that reads the d-pad runs its own loop instead of the
 * room's, and the player is not updated while it is up: FE2's keypad (the A06 number grid, LOCA 0x800B2C94 with the
 * grid reader 0x8004B078) and the other puzzle modules, the pause menu, the title and its load / save screens, the
 * options, game over / continue, full-screen movies and the cutscene close-ups, loading. So the rule: the screen
 * direction is a direction for the player only while the game updates the player, i.e. the update ran in the last
 * UPDATE_LIVE_FRAMES vblanks (the room runs at 30 fps, 15-20 in slow rooms: a gap of 2-4). Otherwise the pad goes to
 * the game as it is, as with the tank controls. Measured over the recorded sessions (NOTES "Directional controls:
 * puzzles and menus"): in normal play the update never paused while a direction was held, so steering is unchanged.
 * A close-up that keeps the room running with the player out of the picture (FE1's bomb vest) is the off-view rule's
 * (player_off_view), as before. */
#define UPDATE_LIVE_FRAMES 8   /* vblanks since the game last updated the player */
static const uint32_t UPDATE_FN[2] = {0x80031FE8u, 0x8002E6C8u}; /* FE2, FE1 */
static uint64_t upd_frame; /* frame_count + 1 of the game's last update of the player (0 never) */
static int dbg_controls = -1, old_rule, old_reapply;
enum { ON_PAD, NOT_RUN };
static const char *why_text[] = {"on the pad", "player not updated (another loop)"};

static int update_hook(void) {
    if (game) {
        uint32_t st = mem_r32(game->player_ptr);
        if (in_ram(st) && R4 == mem_r32(st)) upd_frame = frame_count + 1;
        if (dbg_controls > 1) LOG("controls: update a0 %08X actor %08X frame %llu\n", R4, in_ram(st) ? mem_r32(st) : 0,
                                  (unsigned long long)frame_count);
    }
    return 0; /* an observer: the game's code runs as it is */
}

static int player_on_pad(uint64_t frame) {
    return upd_frame && frame + 1 - upd_frame <= UPDATE_LIVE_FRAMES ? ON_PAD : NOT_RUN;
}

/* agent.c: vblanks since the game last updated the player (-1 never), the actor and its position */
int controls_player_info(uint32_t *actor, int32_t pos[3]) {
    *actor = 0; pos[0] = pos[1] = pos[2] = 0;
    if (!game) return -1;
    uint32_t st = mem_r32(game->player_ptr);
    if (in_ram(st)) {
        uint32_t a = mem_r32(st);
        if (in_ram(a)) { *actor = a; for (int i = 0; i < 3; i++) pos[i] = (int32_t)mem_r32(a + 60 + 4 * (uint32_t)i); }
    }
    return upd_frame ? (int)(frame_count + 1 - upd_frame) : -1;
}

void controls_init(void) {
    if (dbg_controls < 0) {
        dbg_controls = getenv("FE_DBG_CONTROLS") ? atoi(getenv("FE_DBG_CONTROLS")) : 0;
        old_rule = getenv("FE_CONTROLS_OLD") != 0; /* comparisons: the rule before 4 Nov (logged, not applied) */
        old_reapply = getenv("FE_CONTROLS_OLD_REAPPLY") != 0; /* comparisons: the facing re-apply before 5 Nov (night) */
    }
    if (!select_game()) return;
    hook_register(UPDATE_FN[game == &FE1], update_hook);
}

/* returns 1 when the pad's directions are replaced (send_up: report "Up") */
static int decide(uint16_t buttons, uint8_t lx, uint8_t ly, uint64_t frame, int *send_up) {
    *send_up = 0;
    dec_steer = 0;
    int square = (buttons & BIT_SQUARE) != 0;
    if (!square) { square_since = frame; square_hidden = 0; }
    double sx = (double)lx - 128.0, sy = (double)ly - 128.0;
    int dz = (active && frame - last_move_frame <= 4) ? STICK_DEADZONE_MOVING : STICK_DEADZONE;
    if (hypot(sx, sy) < dz) {
        sx = ((buttons & BIT_RIGHT) ? 1.0 : 0.0) - ((buttons & BIT_LEFT) ? 1.0 : 0.0);
        sy = ((buttons & BIT_DOWN) ? 1.0 : 0.0) - ((buttons & BIT_UP) ? 1.0 : 0.0);
    }
    if (sx == 0.0 && sy == 0.0) { reset_steering(); return 0; }
    double in_deg = fmod(atan2(sx, -sy) * 180.0 / PI + 360.0, 360.0);
    int starting = !active;
    active = 1;
    if (bail) {
        if (angle_diff(in_deg, bail_deg) <= HOLD_ANGLE) return 0;
        bail = 0; up_prev = 0; last_move_frame = frame;
    }
    if (buttons & BIT_L1) { prev_valid = 0; return 0; } /* L1: quick turn, the pad as it is */
    uint32_t actor = actor_addr(), state = state_addr();
    if (!actor || !state) return 0;
    uint32_t state_flags = mem_r32(state + STATE_FLAGS);
    if (state_flags & STATE_LADDER) return 0;
    if ((mem_r32(game->game_flags) & GAME_PAUSED) || mem_r32(game->pause_word) != 0) return 0;
    int why = player_on_pad(frame);
    if (dbg_controls > 0) LOG("controls: frame %llu pad %04X %s\n", (unsigned long long)frame, buttons, why_text[why]);
    if (why && !old_rule) { prev_valid = 0; return 0; } /* a puzzle, menu, map, cutscene...: the pad as it is */
    Camera live = live_camera();
    if (!live.valid) return 0;
    int32_t pos[3];
    for (int k = 0; k < 3; k++) pos[k] = (int32_t)mem_r32(actor + ACTOR_POS + 4 * k);
    if (player_off_view(&live, pos)) {
        if (dbg_controls > 0) LOG("controls: frame %llu off view\n", (unsigned long long)frame);
        prev_valid = 0;
        return 0;
    }
    if (starting || !lock.valid) { lock = live; holding = 0; }
    else if (!same_camera(&live, &lock)) {
        if (!holding) { holding = 1; hold_deg = in_deg; }
        if (angle_diff(in_deg, hold_deg) > HOLD_ANGLE) { lock = live; holding = 0; }
    } else holding = 0;
    if (memcmp(pos, last_pos, sizeof pos)) { memcpy(last_pos, pos, sizeof pos); last_move_frame = frame; }
    double wx, wz;
    if (!screen_to_floor(&lock, pos, sx, sy, &wx, &wz)) return 0;
    int32_t want = (int32_t)lround(atan2(-wx, -wz) * 4096.0 / (2.0 * PI)) & 4095;
    int32_t read = (int16_t)mem_r16(actor + ACTOR_FACING) & 4095;
    int32_t before = read;
    /* the game's update sometimes writes back the facing it read before our write: carry on from ours */
    if (prev_valid && frame - prev_frame == 1 && prev_wrote != prev_read && read == prev_read) before = prev_wrote;
    int32_t diff = wrap_facing(want - before);
    int32_t step = diff < -TURN_STEP ? -TURN_STEP : diff > TURN_STEP ? TURN_STEP : diff;
    int32_t wrote = (before + step) & 4095;
    if (wrote != read) mem_w16(actor + ACTOR_FACING, (uint16_t)wrote);
    prev_valid = 1; prev_frame = frame; prev_read = read; prev_wrote = wrote;
    dec_steer = 1;
    dec_wrote = wrote;
    int up = abs(diff - step) < WALK_MAX_DIFF;
    if (up_prev && frame - last_move_frame <= 3) up = 1; /* once moving, a sharp turn is a tight arc */
    /* standing with Square held, FE1 ignores "Up" until Square is released: report it released */
    if (up && square && frame - square_since >= SQUARE_SEEN_FRAMES && frame - last_move_frame > 2) square_hidden = 1;
    if (up && (!up_prev || (state_flags & (STATE_FIRING | STATE_EQUIP)))) up_since = frame;
    up_prev = up;
    if (up && frame - up_since > NO_RESPONSE_FRAMES && frame - last_move_frame > NO_RESPONSE_FRAMES) {
        bail = 1; bail_deg = in_deg; dec_steer = 0; /* a menu, a cutscene or a wall: the pad back until release */
        return 0;
    }
    *send_up = up;
    return 1;
}

static int select_game(void) {
    if (!strcmp(game_serial, "SLUS-00920") || !strncmp(game_serial, "SLUS-0105", 9)) game = &FE1;
    else if (!strncmp(game_serial, "SLUS-0126", 9) || !strncmp(game_serial, "SLUS-0127", 9)) game = &FE2;
    else game = 0;
    return game != 0;
}

/* once per frame, after the pad state is set (and recorded) */
void controls_frame(void) {
    if (!controls_directional || !select_game()) { reset_steering(); square_hidden = 0; dec_steer = 0; return; }
    int send_up;
    int override = decide(pad_buttons, pad_axes[2], pad_axes[3], frame_count, &send_up);
    if (dbg_controls > 0 && override)
        LOG("controls: frame %llu steer facing %d up %d\n", (unsigned long long)frame_count, dec_wrote, send_up);
    if (square_hidden) pad_buttons &= (uint16_t)~BIT_SQUARE;
    if (!override) return;
    pad_buttons &= (uint16_t)~DIRECTION_BITS;
    if (send_up) pad_buttons |= BIT_UP;
    pad_axes[2] = pad_axes[3] = 0x80; /* the stick reads centred: the game walks on "Up" */
}

/* FE_DBG_PLAYER=1: the player's position, facing, state flags and the pad per vblank (read only; 2 adds raw words) */
static int dbg_player = -1;
static void log_player(void) {
    if (dbg_player < 0) dbg_player = getenv("FE_DBG_PLAYER") ? atoi(getenv("FE_DBG_PLAYER")) : 0;
    if (!dbg_player || !game) return;
    uint32_t actor = actor_addr(), st = state_addr();
    if (!actor) return;
    LOG("player: frame %llu actor %08X pos %d %d %d facing %d state %08X flags %08X pad %04X steer %d wrote %d",
        (unsigned long long)frame_count, actor, (int32_t)mem_r32(actor + ACTOR_POS), (int32_t)mem_r32(actor + ACTOR_POS + 4),
        (int32_t)mem_r32(actor + ACTOR_POS + 8), (int16_t)mem_r16(actor + ACTOR_FACING), st,
        st ? mem_r32(st + STATE_FLAGS) : 0, pad_buttons, dec_steer, dec_wrote);
    if (dbg_player > 1) { /* raw words: actor +0..+0x100 */
        LOG(" a:");
        for (int k = 0; k < 64; k++) LOG(" %08X", mem_r32(actor + 4 * k));
        if (st) { LOG(" s:"); for (int k = 0; k < 80; k++) LOG(" %08X", mem_r32(st + 4 * k)); }
    }
    LOG("\n");
}

/* The wall guard (NOTES "Directional controls: walking through walls", 12 Nov): the steering writes the facing up to
 * TURN_STEP a frame, and with the player pressed against a wall of the game's collision those turns let the game's
 * move carry her centre into the wall and, after some frames of it, past the wall's plane, where nothing pushes her back
 * any more (FE2 tower hall room 46: Rain ran on into the void; FE2 island: Hana 24 units into a wall). The game's own
 * turning (tank controls, the same spot) never did. So while the controls steer, a move that takes the centre across a
 * wall face of the collision she has been pressed against (models.c coll_wall_guard) is put back on the side it came
 * from. A jump longer than
 * GUARD_MAX_MOVE (a room change, a scripted move) is left alone. */
#define GUARD_MAX_MOVE 200
#define GUARD_HIST 16
int coll_wall_guard(float x0, float z0, float *x, float *z, float y, const float *hx, const float *hz, int hn);
static int32_t guard_pos[3];
static uint32_t guard_actor;
static int guard_valid;
static float guard_hx[GUARD_HIST], guard_hz[GUARD_HIST]; /* the last positions, for "pressed against it" */
static int guard_hn, guard_hi;
static void wall_guard(uint32_t actor) {
    int32_t p[3];
    for (int k = 0; k < 3; k++) p[k] = (int32_t)mem_r32(actor + ACTOR_POS + 4 * k);
    if (guard_valid && actor == guard_actor) {
        float x0 = guard_pos[0] / 65536.0f, z0 = guard_pos[2] / 65536.0f, x = p[0] / 65536.0f, z = p[2] / 65536.0f;
        if (hypotf(x - x0, z - z0) < GUARD_MAX_MOVE && coll_wall_guard(x0, z0, &x, &z, p[1] / 65536.0f, guard_hx, guard_hz, guard_hn)) {
            if (dbg_controls > 0) LOG("controls: frame %llu wall guard: %.0f,%.0f -> %.0f,%.0f put back to %.0f,%.0f\n",
                                      (unsigned long long)frame_count, x0, z0, p[0] / 65536.0f, p[2] / 65536.0f, x, z);
            p[0] = (int32_t)lroundf(x * 65536.0f); p[2] = (int32_t)lroundf(z * 65536.0f);
            mem_w32(actor + ACTOR_POS, (uint32_t)p[0]);
            mem_w32(actor + ACTOR_POS + 8, (uint32_t)p[2]);
        }
    }
    if (!guard_valid || actor != guard_actor) guard_hn = guard_hi = 0;
    memcpy(guard_pos, p, sizeof p); guard_actor = actor; guard_valid = 1;
    guard_hx[guard_hi] = p[0] / 65536.0f; guard_hz[guard_hi] = p[2] / 65536.0f;
    guard_hi = (guard_hi + 1) % GUARD_HIST;
    if (guard_hn < GUARD_HIST) guard_hn++;
}

/* at the vblank: the game's update undid this frame's facing write: put it back before the next update */
void controls_vblank(void) {
    log_player();
    if (!controls_directional || !game) { guard_valid = 0; return; }
    uint32_t actor = actor_addr();
    if (!actor) { guard_valid = 0; return; }
    if (dec_steer) wall_guard(actor);
    else guard_valid = 0;
    if (!dec_steer || dec_wrote < 0) return;
    int32_t now = (int16_t)mem_r16(actor + ACTOR_FACING) & 4095;
    if (now == dec_wrote) return;
    /* Put ours back only over the game's write-back of the facing it read before our write (the undo). Any other value
     * is the game's own: it turned the player itself this frame (the ladder mount snaps her to face the rungs; 5 Nov,
     * FE2 slot 1: the re-apply turned her round on the mount frame, she climbed down facing the camera and at the
     * bottom sank through the floor for good), a turn of its own or the state changed under us. */
    uint32_t st = state_addr();
    int keep = now != prev_read || (st && (mem_r32(st + STATE_FLAGS) & STATE_LADDER));
    if (dbg_controls > 0) LOG("controls: frame %llu vblank facing %d ours %d read %d -> %s\n", (unsigned long long)frame_count,
                              now, dec_wrote, prev_read, keep ? "the game's" : "ours again");
    if (old_reapply) keep = 0; /* FE_CONTROLS_OLD_REAPPLY=1: the re-apply before 5 Nov, night (comparisons) */
    if (!keep) mem_w16(actor + ACTOR_FACING, (uint16_t)dec_wrote);
}

/* ------------------------------------------------------------------ save states (state.c): this changes the pad
 * state the game gets and writes the facing, so its memory is part of the game's state */
#include "state.h"

void controls_state(StIO *io) {
    /* version 2 adds the player's last update (player_on_pad); a version 1 state (older builds) loads with "not seen":
     * steering starts with the game's next update of the player (in a room, one or two vblanks after the load) */
    uint32_t v = st_begin_compat(io, "CTRL", 2, 1);
    ST(io, active); ST(io, bail); ST(io, holding); ST(io, up_prev); ST(io, prev_valid); ST(io, square_hidden);
    ST(io, bail_deg); ST(io, hold_deg); ST(io, lock); ST(io, last_pos);
    ST(io, last_move_frame); ST(io, up_since); ST(io, square_since); ST(io, prev_frame);
    ST(io, prev_read); ST(io, prev_wrote); ST(io, dec_steer); ST(io, dec_wrote); ST(io, last);
    if (v >= 2) ST(io, upd_frame);
    else if (io->load && !io->dry) upd_frame = 0;
    if (io->load && !io->dry) guard_valid = 0; /* the guard starts again from the loaded position */
    st_end(io);
}
