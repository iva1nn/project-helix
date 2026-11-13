/* The options menu (NOTES.md "Options menu"): the port's own screen in place of both games' options screens.
 * Choosing Options on the game's main menu opens it instead of the game's screen: the press is taken off the pad
 * (the game never leaves its main menu) and the menu is drawn opaque over the whole window. Four categories:
 *   Sound    - the game's stereo / mono and master volume, the port's output volume
 *   Controls - keyboard and gamepad bindings, directional / tank, the game's vibration, FE2's control type and
 *              button layout, FE1's button configuration
 *   Game     - the game's difficulty and subtitles, FE2's auto draw and save beacon; FE1's credits and FE2's art
 *              gallery (reached through the game's own screens under a cover, see "excursion")
 *   Visuals  - remaster, screen layout, 60 fps characters, display, picture effects
 * The game's options are written to its own option block in RAM (gameopt.c), so the memory card saves them as the
 * originals did; the port's are saved to the settings file. Back on the first page returns to the game's main menu.
 * Should the game ever show one of its options screens (a press the menu did not see), the screen is covered, the
 * game is walked back to its main menu and the menu opens.
 * While the menu is open the game gets no input; the pad state recorded for replays is what the game got. */
#include <stdlib.h>
#include <SDL2/SDL.h> /* before hw.h: recomp.h's register names clash with Windows headers */
#include "hw.h"
#include "input.h"
#include "ui.h"
#include "gameopt.h"
#include "state.h"

int menu_open;
static int page, cursor, scroll, capture;    /* capture: waiting for the key / button to bind */
static uint32_t capture_since;
static int closing;                           /* waiting for every button to be released, then hand back */
static char osd_text[128];
static uint32_t osd_until;
static uint16_t prev_raw;
extern int remaster_on, hw_fill_mode, audio_volume;
extern uint16_t input_raw_buttons;
int ui_unavailable(void);

/* excursion: the game's own screens behind the options (FE1 credits, FE2 art gallery), walked to under a cover */
enum { EX_NONE, EX_ENTER, EX_SHOW, EX_RETURN, EX_SETTLE };
static int ex, ex_t, ex_tick, ex_reopen, ex_wait;
static uint16_t ex_btn;

enum { P_ROOT, P_SOUND, P_CONTROLS, P_GAME, P_VISUALS, P_KEYS, P_PAD, P_LAYOUT };

static int game_fe1(void) { return gopt_fe1; }

void menu_gpu_packet(uint32_t a) { (void)a; } /* the old controls-screen detection (core.c still reports packets) */

void osd_show(const char *text) {
    snprintf(osd_text, sizeof osd_text, "%s", text);
    osd_until = SDL_GetTicks() + 2200;
}

/* the menu can be drawn: hardware renderer with a font; headless only for tests (FE_MENU_TEST) */
static int menu_allowed(void) {
    extern void (*hw_overlay_draw)(int w, int h);
    if (!hw_overlay_draw || ui_unavailable() || !gopt_game()) return 0;
    return !opt_headless || getenv("FE_MENU_TEST");
}

/* ------------------------------------------------------------------ binding rows */

static const char *const button_label[B_COUNT] = {
    [B_UP] = "Up  -  walk forward",          [B_DOWN] = "Down  -  walk backwards",
    [B_LEFT] = "Left  -  turn left",          [B_RIGHT] = "Right  -  turn right",
    [B_CROSS] = "Cross  -  fire / quick draw", [B_TRIANGLE] = "Triangle  -  action / select weapon",
    [B_SQUARE] = "Square  -  inventory forward", [B_CIRCLE] = "Circle  -  inventory back",
    [B_R1] = "R1  -  run",                    [B_R2] = "R2  -  crouch",
    [B_L1] = "L1  -  quick turn 180",         [B_L2] = "L2  -  evade",
    [B_START] = "Start  -  pause / skip cinematics",
    [B_SELECT] = "Select  -  not used by the game", [B_L3] = "L3  -  not used by the game",
    [B_R3] = "R3  -  not used by the game",
    [B_LS_UP] = "Left stick up", [B_LS_DOWN] = "Left stick down", [B_LS_LEFT] = "Left stick left",
    [B_LS_RIGHT] = "Left stick right", [B_RS_UP] = "Right stick up", [B_RS_DOWN] = "Right stick down",
    [B_RS_LEFT] = "Right stick left", [B_RS_RIGHT] = "Right stick right", [B_ANALOG] = "Analog button",
    [B_ACT_CONTROLS] = "Remaster: directional / tank controls", [B_ACT_REMASTER] = "Remaster: on / off",
    [B_ACT_OVERLAY] = "Remaster: screen layout", [B_ACT_FULLSCREEN] = "Fullscreen / window",
    [B_ACT_STATE_SAVE] = "Save state (current slot)", [B_ACT_STATE_LOAD] = "Load state (current slot)",
    [B_ACT_STATE_NEXT] = "Save state slot: next", [B_ACT_STATE_PREV] = "Save state slot: previous"};

/* the binding rows of a page, in menu order; -1 = reset to defaults, -2 = back */
static int bind_rows(int pad, int *out) {
    static const int order[] = {B_UP, B_DOWN, B_LEFT, B_RIGHT, B_CROSS, B_TRIANGLE, B_SQUARE, B_CIRCLE, B_R1, B_R2,
                                B_L1, B_L2, B_START, B_SELECT, B_L3, B_R3, B_LS_UP, B_LS_DOWN, B_LS_LEFT, B_LS_RIGHT,
                                B_RS_UP, B_RS_DOWN, B_RS_LEFT, B_RS_RIGHT, B_ANALOG, B_ACT_CONTROLS, B_ACT_REMASTER,
                                B_ACT_OVERLAY, B_ACT_FULLSCREEN, B_ACT_STATE_SAVE, B_ACT_STATE_LOAD, B_ACT_STATE_NEXT,
                                B_ACT_STATE_PREV};
    int n = 0;
    for (size_t i = 0; i < sizeof order / sizeof *order; i++) {
        int b = order[i];
        if (pad && b >= B_LS_UP && b <= B_RS_RIGHT) continue; /* the controller's own sticks are the sticks */
        out[n++] = b;
    }
    out[n++] = -1;
    out[n++] = -2;
    return n;
}

/* the settings file's controller names, as the pad shows them (PlayStation names, Xbox letters) */
static void pad_pretty(const char *in, char *out, size_t n) {
    static const char *const names[][2] = {
        {"a", "Cross (A)"}, {"b", "Circle (B)"}, {"x", "Square (X)"}, {"y", "Triangle (Y)"},
        {"back", "Select (Back)"}, {"start", "Start"}, {"guide", "Guide"}, {"leftstick", "L3"}, {"rightstick", "R3"},
        {"leftshoulder", "L1"}, {"rightshoulder", "R1"}, {"lefttrigger", "L2"}, {"righttrigger", "R2"},
        {"dpup", "D-pad up"}, {"dpdown", "D-pad down"}, {"dpleft", "D-pad left"}, {"dpright", "D-pad right"},
        {"touchpad", "Touchpad"}, {"misc1", "Share"}};
    size_t len = 0;
    out[0] = 0;
    while (*in) {
        char w[32];
        size_t k = 0;
        while (*in == ' ' || *in == ',') in++;
        while (*in && *in != ',' && k + 1 < sizeof w) w[k++] = *in++;
        w[k] = 0;
        if (!k) break;
        const char *shown = w;
        for (size_t i = 0; i < sizeof names / sizeof *names; i++)
            if (!strcmp(w, names[i][0])) shown = names[i][1];
        len += (size_t)snprintf(out + len, len < n ? n - len : 0, "%s%s", len ? "  /  " : "", shown);
        if (len >= n) break;
    }
}

static const char *layout_name(int m) {
    return m == 1 ? "fill, HUD in bands" : m == 2 ? "fill, HUD over the picture" : "original 4:3";
}

/* ------------------------------------------------------------------ the option rows */

enum {
    X_SOUND, X_CONTROLS, X_GAME, X_VISUALS, X_CLOSE,
    X_STEREO, X_VOLUME, X_PORT_VOLUME,
    X_KEYS, X_PAD, X_DIRECTIONAL, X_CTLTYPE, X_VIBRATION, X_LAYOUT, X_PRESET,
    X_DIFFICULTY, X_SUBTITLES, X_AUTODRAW, X_BEACON, X_EXTRA,
    X_REMASTER, X_FILL, X_SMOOTH, X_DISPLAY, X_BLOOM, X_BLOOM_I, X_VIG, X_VIG_S, X_GRAIN, X_GRAIN_S,
    X_MAP0, X_MAP_RESET = X_MAP0 + 8,
    X_BACK
};

static int page_rows(int p, int *r) {
    int n = 0, fe2 = !game_fe1();
    switch (p) {
    case P_ROOT: r[n++] = X_SOUND; r[n++] = X_CONTROLS; r[n++] = X_GAME; r[n++] = X_VISUALS; r[n++] = X_CLOSE; break;
    case P_SOUND: r[n++] = X_STEREO; r[n++] = X_VOLUME; r[n++] = X_PORT_VOLUME; r[n++] = X_BACK; break;
    case P_CONTROLS:
        r[n++] = X_KEYS; r[n++] = X_PAD; r[n++] = X_DIRECTIONAL;
        if (fe2) { r[n++] = X_CTLTYPE; r[n++] = X_LAYOUT; } else r[n++] = X_PRESET;
        r[n++] = X_VIBRATION; r[n++] = X_BACK;
        break;
    case P_GAME:
        r[n++] = X_DIFFICULTY; r[n++] = X_SUBTITLES;
        if (fe2) { r[n++] = X_AUTODRAW; r[n++] = X_BEACON; }
        r[n++] = X_EXTRA; r[n++] = X_BACK;
        break;
    case P_VISUALS:
        r[n++] = X_REMASTER; r[n++] = X_FILL; r[n++] = X_SMOOTH; r[n++] = X_DISPLAY; r[n++] = X_BLOOM; r[n++] = X_BLOOM_I;
        r[n++] = X_VIG; r[n++] = X_VIG_S; r[n++] = X_GRAIN; r[n++] = X_GRAIN_S; r[n++] = X_BACK;
        break;
    case P_LAYOUT:
        for (int i = 0; i < 8; i++) r[n++] = X_MAP0 + i;
        r[n++] = X_MAP_RESET; r[n++] = X_BACK;
        break;
    }
    return n;
}

static const char *const map_action[8] = {"Fire / quick draw", "Action / select weapon", "Inventory forward",
                                          "Inventory back", "Run", "Crouch", "Quick turn 180", "Evade"};
/* FE2's pad bits (its button word is the PS1 one byte-swapped), in the order Left / Right steps through them */
static const struct { int bit; const char *name; } map_btn[8] = {
    {0x40, "Cross"}, {0x10, "Triangle"}, {0x80, "Square"}, {0x20, "Circle"},
    {0x08, "R1"}, {0x02, "R2"}, {0x04, "L1"}, {0x01, "L2"}};
static const char *const preset_name[4] = {"default", "config 1", "config 2", "config 3"};
/* what FE1's configurations change (its controls screen's diagram); the d-pad turns and walks in all of them */
static const char *const preset_text[4][2] = {
    {"R1 run    R2 crouch    L1 quick turn    L2 evade", "Cross fire    Triangle action    Square inv. forward    Circle inv. back"},
    {"R1 quick turn    R2 evade    L1 run    L2 crouch", "Cross fire    Square action    Triangle inv. forward    Circle inv. back"},
    {"R1 crouch    R2 run    L1 quick turn    L2 evade", "Cross fire    Square action    Triangle inv. forward    Circle inv. back"},
    {"R1 walk forward    R2 run    L1 crouch    L2 evade    Up quick turn", "Cross fire    Triangle action    Square inv. forward    Circle inv. back"}};

static const char *onoff(int v) { return v ? "on" : "off"; }

/* label and value of a row; returns 0 when the value is inactive (drawn dimmed) */
static int row_text(int id, char *l, size_t ln, char *v, size_t vn) {
    v[0] = 0;
    int live = 1;
    switch (id) {
    case X_SOUND: snprintf(l, ln, "Sound"); break;
    case X_CONTROLS: snprintf(l, ln, "Controls"); break;
    case X_GAME: snprintf(l, ln, "Game"); break;
    case X_VISUALS: snprintf(l, ln, "Visuals"); break;
    case X_CLOSE: snprintf(l, ln, "Back to the main menu"); break;
    case X_BACK: snprintf(l, ln, "Back"); break;
    case X_STEREO: snprintf(l, ln, "Output"); snprintf(v, vn, "%s", gopt_get(GO_STEREO) ? "stereo" : "mono"); break;
    case X_VOLUME: snprintf(l, ln, "Master volume"); snprintf(v, vn, "%d", gopt_get(GO_VOLUME) * 100 / 127); break;
    case X_PORT_VOLUME: snprintf(l, ln, "Port output volume"); snprintf(v, vn, "%d", audio_volume); break;
    case X_KEYS: snprintf(l, ln, "Keyboard bindings"); break;
    case X_PAD: snprintf(l, ln, "Gamepad bindings"); break;
    case X_DIRECTIONAL: snprintf(l, ln, "Movement"); snprintf(v, vn, "%s", controls_directional ? "directional (camera)" : "tank (original)"); break;
    case X_CTLTYPE: snprintf(l, ln, "Control type"); snprintf(v, vn, "%s", gopt_get(GO_CONTROL_TYPE) ? "3D" : "classic"); break;
    case X_VIBRATION: snprintf(l, ln, "Vibration"); snprintf(v, vn, "%s", onoff(gopt_get(GO_VIBRATION))); break;
    case X_LAYOUT: snprintf(l, ln, "Button layout"); break;
    case X_PRESET: snprintf(l, ln, "Button configuration"); snprintf(v, vn, "%s", preset_name[gopt_get(GO_PRESET) & 3]); break;
    case X_DIFFICULTY: snprintf(l, ln, "Difficulty"); snprintf(v, vn, "%s", gopt_get(GO_DIFFICULTY) ? "hard" : "normal"); break;
    case X_SUBTITLES: snprintf(l, ln, "Subtitles"); snprintf(v, vn, "%s", onoff(gopt_get(GO_SUBTITLES))); break;
    case X_AUTODRAW: snprintf(l, ln, "Auto draw"); snprintf(v, vn, "%s", onoff(gopt_get(GO_AUTO_DRAW))); break;
    case X_BEACON: snprintf(l, ln, "Save beacon"); snprintf(v, vn, "%s", onoff(gopt_get(GO_SAVE_BEACON))); break;
    case X_EXTRA: snprintf(l, ln, "%s", game_fe1() ? "Credits" : "Extras: art gallery"); break;
    case X_REMASTER: snprintf(l, ln, "Remaster"); snprintf(v, vn, "%s", onoff(remaster_on)); break;
    case X_FILL: snprintf(l, ln, "Screen layout"); snprintf(v, vn, "%s", layout_name(hw_fill_mode)); break;
    case X_SMOOTH: snprintf(l, ln, "Characters at 60 fps"); snprintf(v, vn, "%s", onoff(smooth_on)); break;
    case X_DISPLAY: {
        extern SDL_Window *frontend_window(void);
        SDL_Window *w = frontend_window();
        snprintf(l, ln, "Display");
        snprintf(v, vn, "%s", w && (SDL_GetWindowFlags(w) & SDL_WINDOW_FULLSCREEN_DESKTOP) ? "fullscreen" : "window");
        break;
    }
    case X_BLOOM: snprintf(l, ln, "Bloom"); snprintf(v, vn, "%s", onoff(fx_bloom)); break;
    case X_BLOOM_I: snprintf(l, ln, "Bloom intensity"); snprintf(v, vn, "%.2f", fx_bloom_intensity); live = fx_bloom; break;
    case X_VIG: snprintf(l, ln, "Vignette"); snprintf(v, vn, "%s", onoff(fx_vignette)); break;
    case X_VIG_S: snprintf(l, ln, "Vignette strength"); snprintf(v, vn, "%.2f", fx_vignette_strength); live = fx_vignette; break;
    case X_GRAIN: snprintf(l, ln, "Film grain"); snprintf(v, vn, "%s", onoff(fx_grain)); break;
    case X_GRAIN_S: snprintf(l, ln, "Grain strength"); snprintf(v, vn, "%.2f", fx_grain_strength); live = fx_grain; break;
    case X_MAP_RESET: snprintf(l, ln, "Reset to default"); break;
    default:
        if (id >= X_MAP0 && id < X_MAP0 + 8) {
            int b = gopt_map_get(id - X_MAP0);
            snprintf(l, ln, "%s", map_action[id - X_MAP0]);
            snprintf(v, vn, "?");
            for (int k = 0; k < 8; k++) if (map_btn[k].bit == b) snprintf(v, vn, "%s", map_btn[k].name);
        } else l[0] = 0;
    }
    return live;
}

static float fx_step(float x, int d, float step, float hi) {
    int k = (int)(x / step + 0.5f) + d;
    float r = (float)k * step;
    return r < 0 ? 0 : r > hi ? hi : r;
}

static void save_float(const char *key, float v) { char t[32]; snprintf(t, sizeof t, "%.2f", v); settings_set(key, t); }
static void save_int(const char *key, int v) { char t[32]; snprintf(t, sizeof t, "%d", v); settings_set(key, t); }

void menu_action(int action);
static void start_excursion(void);

/* Left (d -1), Right (d 1) or Cross (d 0) on a row that holds a value */
static void row_change(int id, int d) {
    int step = d ? d : 1;
    switch (id) {
    case X_STEREO: gopt_set(GO_STEREO, !gopt_get(GO_STEREO)); break;
    case X_VOLUME: {
        int v = gopt_get(GO_VOLUME) + (d ? 4 * d : 0);
        if (!d) v = gopt_get(GO_VOLUME) >= 127 ? 0 : gopt_get(GO_VOLUME) + 32 > 127 ? 127 : gopt_get(GO_VOLUME) + 32;
        gopt_set(GO_VOLUME, v < 0 ? 0 : v > 127 ? 127 : v);
        break;
    }
    case X_PORT_VOLUME:
        audio_volume += 5 * step;
        if (!d && audio_volume > 100) audio_volume = 0;
        audio_volume = audio_volume < 0 ? 0 : audio_volume > 100 ? 100 : audio_volume;
        save_int("volume", audio_volume);
        break;
    case X_DIRECTIONAL: menu_action(ACT_CONTROLS); save_int("directional", controls_directional); break;
    case X_CTLTYPE: gopt_set(GO_CONTROL_TYPE, !gopt_get(GO_CONTROL_TYPE)); break;
    case X_VIBRATION: gopt_set(GO_VIBRATION, !gopt_get(GO_VIBRATION)); break;
    case X_PRESET: gopt_set(GO_PRESET, (gopt_get(GO_PRESET) + step + 4) & 3); break;
    case X_DIFFICULTY: gopt_set(GO_DIFFICULTY, !gopt_get(GO_DIFFICULTY)); break;
    case X_SUBTITLES: gopt_set(GO_SUBTITLES, !gopt_get(GO_SUBTITLES)); break;
    case X_AUTODRAW: gopt_set(GO_AUTO_DRAW, !gopt_get(GO_AUTO_DRAW)); break;
    case X_BEACON: gopt_set(GO_SAVE_BEACON, !gopt_get(GO_SAVE_BEACON)); break;
    case X_REMASTER: menu_action(ACT_REMASTER); break;
    case X_FILL:
        hw_fill_mode = d < 0 ? (hw_fill_mode + 2) % 3 : (hw_fill_mode + 1) % 3;
        save_int("fill", hw_fill_mode);
        break;
    case X_SMOOTH: smooth_on = !smooth_on; save_int("smooth", smooth_on); break;
    case X_DISPLAY: {
        extern SDL_Window *frontend_window(void);
        menu_action(ACT_FULLSCREEN);
        SDL_Window *w = frontend_window();
        if (w) settings_set("display", SDL_GetWindowFlags(w) & SDL_WINDOW_FULLSCREEN_DESKTOP ? "fullscreen" : "windowed");
        break;
    }
    case X_BLOOM: fx_bloom = !fx_bloom; save_int("bloom", fx_bloom); break;
    case X_BLOOM_I: fx_bloom_intensity = fx_step(fx_bloom_intensity, step, 0.1f, 2.0f); save_float("bloom_intensity", fx_bloom_intensity); break;
    case X_VIG: fx_vignette = !fx_vignette; save_int("vignette", fx_vignette); break;
    case X_VIG_S: fx_vignette_strength = fx_step(fx_vignette_strength, step, 0.05f, 1.0f); save_float("vignette_strength", fx_vignette_strength); break;
    case X_GRAIN: fx_grain = !fx_grain; save_int("grain", fx_grain); break;
    case X_GRAIN_S: fx_grain_strength = fx_step(fx_grain_strength, step, 0.1f, 3.0f); save_float("grain_strength", fx_grain_strength); break;
    case X_MAP_RESET: gopt_map_reset(); osd_show("Button layout, vibration and control type reset"); break;
    default:
        if (id >= X_MAP0 && id < X_MAP0 + 8) {
            int b = gopt_map_get(id - X_MAP0), k = 0;
            while (k < 8 && map_btn[k].bit != b) k++;
            gopt_map_set(id - X_MAP0, map_btn[((k < 8 ? k : 0) + step + 8) % 8].bit);
        }
    }
}

/* ------------------------------------------------------------------ actions (also from their keys/buttons) */

void menu_action(int action) {
    extern int models_on, texrep_on;
    switch (action) {
    case ACT_CONTROLS:
        controls_directional = !controls_directional;
        osd_show(controls_directional ? "Directional controls: the stick moves the way it points on screen"
                                      : "Tank controls (original)");
        break;
    case ACT_REMASTER:
        remaster_on = !remaster_on;
        models_on = remaster_on;
        texrep_on = remaster_on;
        osd_show(remaster_on ? "Remaster on: 8x pictures, characters and interface" : "Remaster off: original graphics");
        break;
    case ACT_OVERLAY: {
        hw_fill_mode = hw_fill_mode == 1 ? 2 : hw_fill_mode == 2 ? 0 : 1;
        char t[96];
        snprintf(t, sizeof t, "Screen layout: %s", layout_name(hw_fill_mode));
        osd_show(t);
        break;
    }
    case ACT_FULLSCREEN: {
        extern SDL_Window *frontend_window(void);
        SDL_Window *w = frontend_window();
        if (!w) break;
        int fs = SDL_GetWindowFlags(w) & SDL_WINDOW_FULLSCREEN_DESKTOP;
        SDL_SetWindowFullscreen(w, fs ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
        SDL_ShowCursor(fs ? SDL_ENABLE : SDL_DISABLE);
        break;
    }
    case ACT_STATE_SAVE: state_action(0, 0); break;
    case ACT_STATE_LOAD: state_action(1, 0); break;
    case ACT_STATE_NEXT: state_action(2, 0); break;
    case ACT_STATE_PREV: state_action(3, 0); break;
    }
}

int menu_event(const SDL_Event *e);

/* ------------------------------------------------------------------ open / close */

static void open_menu(int at_page, int at_cursor) {
    LOG("options menu: open (frame %llu)\n", (unsigned long long)frame_count);
    menu_open = 1;
    input_blocked = 1;
    page = at_page; cursor = at_cursor; scroll = 0; capture = 0;
}

/* leave: wait for every button to be released (a held Cross would act on the game's main menu), then hand back */
static void close_menu(void) {
    LOG("options menu: close (frame %llu)\n", (unsigned long long)frame_count);
    menu_open = 0;
    capture = 0;
    closing = 1;
}

/* save states: a load ends any walk through the game's screens and closes the menu; the pad goes straight back */
void menu_state_loaded(void) {
    if (menu_open) LOG("options menu: closed by a state load\n");
    menu_open = 0; capture = 0; closing = 0;
    ex = EX_NONE; ex_t = ex_tick = ex_reopen = ex_wait = 0;
    input_blocked = 0;
    prev_raw = 0xFFFF; /* a button held through the load is not a new press */
}
int menu_busy(void) { return menu_open || closing || ex != EX_NONE; }

/* headless tests: FE_MENU_TEST=1 lets the menu open; FE_MENU_KEYS="frame:scancode,..." types keys into it */
static void test_keys(void) {
    const char *k = getenv("FE_MENU_KEYS");
    while (k && *k) {
        long long f;
        int sc;
        if (sscanf(k, "%lld:%d", &f, &sc) == 2 && (long long)frame_count == f) {
            SDL_Event e;
            memset(&e, 0, sizeof e);
            e.type = SDL_KEYDOWN;
            e.key.keysym.scancode = (SDL_Scancode)sc;
            LOG("menu test: key %d at frame %lld (open %d page %d cursor %d)\n", sc, f, menu_open, page, cursor);
            menu_event(&e);
        }
        k = strchr(k, ',');
        if (k) k++;
    }
}

void menu_vblank(void) {
    if (opt_headless && getenv("FE_MENU_TEST")) test_keys();
}

/* ------------------------------------------------------------------ the excursion (credits, art gallery) */

static void start_excursion(void) {
    LOG("options menu: to the game's %s (frame %llu)\n", game_fe1() ? "credits" : "art gallery", (unsigned long long)frame_count);
    close_menu();
    ex = EX_ENTER; ex_t = 0; ex_tick = 0; ex_reopen = 1; ex_wait = 0;
}

static void ex_go(int state) {
    LOG("options menu: excursion %d -> %d after %d frames (frame %llu)\n", ex, state, ex_t, (unsigned long long)frame_count);
    ex = state; ex_t = 0; ex_tick = 0;
}

/* which button walks the game one step further: towards the target (enter) or back to its main menu (return) */
static uint16_t ex_step(int enter) {
    if (game_fe1()) {
        uint32_t m = mem_r32(0x80082D6Cu);
        if (!enter) return 0x1000;                                          /* Triangle: back */
        if (m == 2) return 0x4000;                                          /* main menu: Cross on Options */
        if (m == 3) return gopt_cursor() == 4 ? 0x4000 : 0x0040;            /* options: Down to Credits, Cross */
        return 0x1000;                                                      /* sound / controls: back */
    }
    int n = mem_r8(0x800BC9DCu), t = mem_r8(0x800BC9DDu), sub = mem_r8(0x800BC9DEu);
    if (!enter) {
        if (t == 0x11 && sub == 3) return mem_r8(0x800BC9E8u) == 11 ? 0x4000 : 0x0010; /* controls: EXIT row, Cross */
        return 0x1000;                                                      /* Triangle: back */
    }
    if (n == 8 && t == 8) return 0x4000;                                    /* main menu: Cross on OPTIONS */
    if (t == 0x0C) return sub == 4 ? 0x4000 : 0x0040;                       /* options list: Down to extras, Cross */
    if (t == 0x11) return sub == 4 ? 0x4000 : 0x1000;                       /* extras: Cross on ART GALLERY */
    return 0;
}

/* the displayed picture (VRAM display area) is black */
static int display_black(void) {
    GpuDisplay d;
    gpu_display(&d);
    if (!d.enabled) return 1;
    int w = d.depth24 ? d.w * 3 / 2 : d.w;
    for (int y = d.y; y < d.y + d.h && y < 512; y++)
        for (int x = d.x; x < d.x + w && x < 1024; x++)
            if (vram[y * 1024 + x] & 0x7FFF) return 0;
    return 1;
}

static void excursion_frame(void) {
    ex_t++;
    int press = 0;
    switch (ex) {
    case EX_ENTER:
        if (gopt_extra_running()) {
            /* FE1 switches to the credits module while its options screen still shrinks away (~190 frames, then
             * black): the cover stays until the displayed picture has been black once (or 400 frames) */
            if (!game_fe1() || display_black() || ++ex_wait >= 400) { ex_go(EX_SHOW); input_blocked = 0; return; }
            break;
        }
        if (!game_fe1() && mem_r8(0x800BC9DCu) == 0x11 && mem_r8(0x800BC9DDu) == 0x11 && mem_r8(0x800BC9DEu) == 4 &&
            mem_r32(0x800BCA78u) == 0) { /* the extras screen's own flag: the gallery is not unlocked yet */
            osd_show("The art gallery opens once the game is finished");
            ex_go(EX_RETURN);
            break;
        }
        if (ex_t > 1200) { osd_show(game_fe1() ? "The credits did not start" : "The art gallery did not open"); ex_go(EX_RETURN); break; }
        press = 1;
        break;
    case EX_SHOW:
        if (!gopt_extra_running()) { input_blocked = 1; ex_go(EX_RETURN); }
        return;
    case EX_RETURN:
        if (gopt_main_menu()) { ex_go(EX_SETTLE); break; }
        if (ex_t > 3000) { LOG("options menu: the game did not return to its main menu\n"); ex = EX_NONE; input_blocked = 0; return; }
        press = 2;
        break;
    case EX_SETTLE: /* the main menu's fade-in under the cover, then the menu again */
        if (!gopt_main_menu()) { ex_go(EX_RETURN); break; }
        if (ex_t >= 40) {
            ex = EX_NONE;
            if (ex_reopen) {
                int r[32], n = page_rows(P_GAME, r), c = 0;
                while (c < n && r[c] != X_EXTRA) c++;
                open_menu(ex_reopen == 1 ? P_GAME : P_ROOT, ex_reopen == 1 ? c : 0);
            } else input_blocked = 0;
            return;
        }
        break;
    }
    pad_buttons = 0;
    memset(pad_axes, 0x80, 4);
    if (press) { /* held 6 frames every 16: the game's screens ignore the pad for a while after they open */
        if (ex_tick % 16 == 0) ex_btn = ex_step(press == 1);
        if (ex_tick % 16 >= 1 && ex_tick % 16 < 7) pad_buttons = ex_btn;
        ex_tick++;
    }
}

/* the pad state the game gets this frame (called after polling, before recording) */
void menu_filter_pad(void) {
    gopt_debug_frame();
    uint16_t raw = (uint16_t)(pad_buttons | input_raw_buttons);
    uint16_t pressed = (uint16_t)(raw & ~prev_raw);
    prev_raw = raw;
    if (menu_open) { pad_buttons = 0; memset(pad_axes, 0x80, 4); return; }
    if (closing) {
        pad_buttons = 0; memset(pad_axes, 0x80, 4);
        if (!raw) { closing = 0; if (!ex) input_blocked = 0; }
        return;
    }
    if (ex) { excursion_frame(); return; }
    if (!menu_allowed()) return;
    if (gopt_main_menu_options() && (pressed & 0x4008)) { /* Cross / Start on Options: the port's menu instead */
        open_menu(P_ROOT, 0);
        pad_buttons = 0; memset(pad_axes, 0x80, 4);
        return;
    }
    if (gopt_original_screen()) { /* the game got into its options after all: cover, walk back, open the menu */
        LOG("options menu: the game's options screen is up: back to its main menu (frame %llu)\n", (unsigned long long)frame_count);
        input_blocked = 1;
        ex = EX_RETURN; ex_t = 0; ex_tick = 0; ex_reopen = 2;
        excursion_frame();
    }
}

/* ------------------------------------------------------------------ input */

static int rows_now(int *r) {
    if (page == P_KEYS || page == P_PAD) return bind_rows(page == P_PAD, r);
    return page_rows(page, r);
}

static void go_page(int p, int at) { page = p; cursor = at; scroll = 0; }

/* the row of a sub page's parent that leads to it */
static void go_back(void) {
    int r[64], n, c = 0, want;
    int parent = page == P_KEYS || page == P_PAD || page == P_LAYOUT ? P_CONTROLS : P_ROOT;
    want = page == P_SOUND ? X_SOUND : page == P_CONTROLS ? X_CONTROLS : page == P_GAME ? X_GAME : page == P_VISUALS ? X_VISUALS
         : page == P_KEYS ? X_KEYS : page == P_PAD ? X_PAD : X_LAYOUT;
    n = page_rows(parent, r);
    while (c < n && r[c] != want) c++;
    go_page(parent, c < n ? c : 0);
}

int menu_event(const SDL_Event *e) {
    if (!menu_open) return 0;
    int rv[B_COUNT + 2 > 64 ? B_COUNT + 2 : 64], n = rows_now(rv);
    if (capture) {
        if (page == P_KEYS && e->type == SDL_KEYDOWN && !e->key.repeat) {
            if (e->key.keysym.scancode != SDL_SCANCODE_ESCAPE) { input_bind_key(capture - 1, e->key.keysym.scancode); input_save(); }
            capture = 0;
            return 1;
        }
        if (page == P_PAD && e->type == SDL_CONTROLLERBUTTONDOWN) {
            input_bind_pad(capture - 1, 0, e->cbutton.button);
            input_save();
            capture = 0;
            return 1;
        }
        if (page == P_PAD && e->type == SDL_CONTROLLERAXISMOTION && e->caxis.value > 20000 &&
            (e->caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT || e->caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT)) {
            input_bind_pad(capture - 1, 1, e->caxis.axis);
            input_save();
            capture = 0;
            return 1;
        }
        if (page == P_PAD && e->type == SDL_KEYDOWN && e->key.keysym.scancode == SDL_SCANCODE_ESCAPE) { capture = 0; return 1; }
        return e->type == SDL_KEYDOWN || e->type == SDL_CONTROLLERBUTTONDOWN;
    }
    int up = 0, down = 0, ok = 0, back = 0, left = 0, right = 0;
    if (e->type == SDL_KEYDOWN) {
        SDL_Scancode s = e->key.keysym.scancode;
        up = s == SDL_SCANCODE_UP; down = s == SDL_SCANCODE_DOWN;
        left = s == SDL_SCANCODE_LEFT; right = s == SDL_SCANCODE_RIGHT;
        ok = !e->key.repeat && (s == SDL_SCANCODE_RETURN || s == SDL_SCANCODE_KP_ENTER || s == SDL_SCANCODE_SPACE);
        back = !e->key.repeat && (s == SDL_SCANCODE_ESCAPE || s == SDL_SCANCODE_BACKSPACE);
    } else if (e->type == SDL_CONTROLLERBUTTONDOWN) {
        int b = e->cbutton.button;
        up = b == SDL_CONTROLLER_BUTTON_DPAD_UP; down = b == SDL_CONTROLLER_BUTTON_DPAD_DOWN;
        left = b == SDL_CONTROLLER_BUTTON_DPAD_LEFT; right = b == SDL_CONTROLLER_BUTTON_DPAD_RIGHT;
        ok = b == SDL_CONTROLLER_BUTTON_A;                                  /* Cross */
        back = b == SDL_CONTROLLER_BUTTON_Y || b == SDL_CONTROLLER_BUTTON_B; /* Triangle / Circle */
    } else if (e->type == SDL_CONTROLLERAXISMOTION && (e->caxis.axis == SDL_CONTROLLER_AXIS_LEFTY || e->caxis.axis == SDL_CONTROLLER_AXIS_LEFTX)) {
        static int was[2];
        int ax = e->caxis.axis == SDL_CONTROLLER_AXIS_LEFTX;
        int now = e->caxis.value < -20000 ? -1 : e->caxis.value > 20000 ? 1 : 0;
        if (now && now != was[ax]) { if (ax) { left = now < 0; right = now > 0; } else { up = now < 0; down = now > 0; } }
        was[ax] = now;
    } else {
        return e->type == SDL_KEYDOWN || e->type == SDL_KEYUP;
    }
    if (up) cursor = (cursor + n - 1) % n;
    if (down) cursor = (cursor + 1) % n;
    if (page == P_KEYS || page == P_PAD) {
        if (back) { go_back(); return 1; }
        if (ok) {
            int r = rv[cursor];
            if (r == -2) go_back();
            else if (r == -1) { input_bind_defaults(page == P_PAD); input_save(); osd_show("Defaults restored"); }
            else { capture = r + 1; capture_since = SDL_GetTicks(); }
        }
        return 1;
    }
    int id = rv[cursor];
    if (back) {
        if (page == P_ROOT) close_menu();
        else go_back();
        return 1;
    }
    if (left || right) { row_change(id, left ? -1 : 1); return 1; }
    if (!ok) return 1;
    switch (id) {
    case X_SOUND: go_page(P_SOUND, 0); break;
    case X_CONTROLS: go_page(P_CONTROLS, 0); break;
    case X_GAME: go_page(P_GAME, 0); break;
    case X_VISUALS: go_page(P_VISUALS, 0); break;
    case X_CLOSE: close_menu(); break;
    case X_BACK: go_back(); break;
    case X_KEYS: go_page(P_KEYS, 0); break;
    case X_PAD: go_page(P_PAD, 0); break;
    case X_LAYOUT: go_page(P_LAYOUT, 0); break;
    case X_EXTRA: start_excursion(); break;
    default: row_change(id, 0); break;
    }
    return 1;
}

/* ------------------------------------------------------------------ drawing */

typedef struct { uint32_t title, text, dim, sel, sel_bg, line, panel, value, cover; } Style;

static Style style(void) {
    if (game_fe1()) /* FE1: red typewriter on dark blue, cream for the chosen row (its options screens) */
        return (Style){0xE04A2AFF, 0xC8452CFF, 0x8E3B2AFF, 0xF2E6B4FF, 0x00000000, 0x9A2A1EFF, 0x0A0A24FF, 0xE9DDA8FF, 0x05050FFF};
    /* FE2: white and grey condensed capitals in green frames (its options screens) */
    return (Style){0xFFFFFFFF, 0x9A9A9AFF, 0x5E5E5EFF, 0xFFFFFFFF, 0x3C5A1A80, 0x6E9A2AFF, 0x000000FF, 0xBFE07AFF, 0x000000FF};
}

static void frame(float x, float y, float w, float h, uint32_t c, float t) {
    ui_rect(x, y, w, t, c); ui_rect(x, y + h - t, w, t, c); ui_rect(x, y, t, h, c); ui_rect(x + w - t, y, t, h, c);
}

static const char *page_title(void) {
    switch (page) {
    case P_SOUND: return "sound";
    case P_CONTROLS: return "controls";
    case P_GAME: return "game";
    case P_VISUALS: return "visuals";
    case P_KEYS: return "controls  -  keyboard";
    case P_PAD: return "controls  -  gamepad";
    case P_LAYOUT: return "controls  -  button layout";
    }
    return "options";
}

static void upper(char *d, const char *s, size_t n) {
    size_t i = 0;
    for (; s[i] && i + 1 < n; i++) d[i] = (char)(s[i] >= 'a' && s[i] <= 'z' ? s[i] - 32 : s[i]);
    d[i] = 0;
}

static void draw_menu(int w, int h, Style st, float s) {
    float pw = 1240 * s, ph = 900 * s, px = ((float)w - pw) / 2, py = ((float)h - ph) / 2;
    ui_rect(px, py, pw, ph, st.panel);
    frame(px, py, pw, ph, st.line, 3 * s);
    char t[96];
    if (game_fe1()) ui_text(UI_TITLE, px + 60 * s, py + 30 * s, 84 * s, st.title, page_title());
    else {
        upper(t, page == P_ROOT ? "options" : "options", sizeof t);
        ui_text(UI_TITLE, px + 60 * s, py + 30 * s, 84 * s, st.title, t);
        if (page != P_ROOT) { upper(t, page_title(), sizeof t); ui_text(UI_BODY, px + 330 * s, py + 52 * s, 50 * s, st.value, t); }
    }
    float y0 = py + 150 * s, lh = 50 * s, size = 44 * s;
    int vis = page == P_PAD && !input_has_controller() ? 12 : 13;
    char label[160], val[200];
    if (page == P_KEYS || page == P_PAD) {
        int r[B_COUNT + 2], n = bind_rows(page == P_PAD, r);
        if (cursor < scroll) scroll = cursor;
        if (cursor >= scroll + vis) scroll = cursor - vis + 1;
        for (int i = scroll; i < n && i < scroll + vis; i++) {
            float y = y0 + (float)(i - scroll) * lh;
            int sel = i == cursor;
            if (sel) ui_rect(px + 40 * s, y - 4 * s, pw - 80 * s, lh, st.sel_bg);
            uint32_t c = sel ? st.sel : st.text;
            if (r[i] == -1) { ui_text(UI_BODY, px + 70 * s, y, size, c, "Reset to defaults"); continue; }
            if (r[i] == -2) { ui_text(UI_BODY, px + 70 * s, y, size, c, "Back"); continue; }
            int b = r[i];
            ui_text(UI_BODY, px + 70 * s, y, size, b >= B_ACT_FIRST ? (sel ? st.sel : st.value) : c, button_label[b]);
            if (capture == b + 1) snprintf(val, sizeof val, page == P_KEYS ? "press a key  (Esc cancels)" : "press a button");
            else {
                input_bind_text(b, page == P_PAD, val, sizeof val);
                if (page == P_PAD) { char raw[200]; snprintf(raw, sizeof raw, "%s", val); pad_pretty(raw, val, sizeof val); }
                if (!val[0]) snprintf(val, sizeof val, "-");
            }
            float vw = ui_text_width(UI_BODY, size, val);
            ui_text(UI_BODY, px + pw - 70 * s - vw, y, size, capture == b + 1 ? st.title : (sel ? st.sel : st.dim), val);
        }
        if (n > vis) { /* scroll bar */
            float bh = ph - 260 * s, sh = bh * (float)vis / (float)n, sy = y0 + bh * (float)scroll / (float)n;
            ui_rect(px + pw - 30 * s, y0, 6 * s, bh, st.dim);
            ui_rect(px + pw - 32 * s, sy, 10 * s, sh, st.line);
        }
    } else {
        int r[32], n = page_rows(page, r);
        float gap = n > 9 ? 1.1f : 1.25f;
        for (int i = 0; i < n; i++) {
            float y = y0 + (float)i * lh * gap;
            int sel = i == cursor, live = row_text(r[i], label, sizeof label, val, sizeof val);
            if (sel) ui_rect(px + 40 * s, y - 4 * s, pw - 80 * s, lh, st.sel_bg);
            ui_text(UI_BODY, px + 70 * s, y, size, sel ? st.sel : st.text, label);
            if (r[i] == X_VOLUME || r[i] == X_PORT_VOLUME) { /* a bar beside the number */
                float frac = r[i] == X_VOLUME ? (float)gopt_get(GO_VOLUME) / 127.0f : (float)audio_volume / 100.0f;
                float bw = 300 * s, bx = px + pw - 70 * s - 110 * s - bw, by = y + lh * 0.38f;
                ui_rect(bx, by, bw, 6 * s, st.dim);
                ui_rect(bx, by - 3 * s, bw * frac, 12 * s, sel ? st.sel : st.value);
            }
            if (!val[0]) continue;
            float vw = ui_text_width(UI_BODY, size, val);
            ui_text(UI_BODY, px + pw - 70 * s - vw, y, size, sel && live ? st.sel : live ? st.value : st.dim, val);
        }
        if (page == P_CONTROLS && r[cursor] == X_PRESET) { /* what the configuration does */
            int p = gopt_get(GO_PRESET) & 3;
            for (int k = 0; k < 2; k++) {
                float ts = 32 * s, tw = ui_text_width(UI_BODY, ts, preset_text[p][k]);
                if (tw > pw - 140 * s) ts *= (pw - 140 * s) / tw;
                ui_text(UI_BODY, px + 70 * s, py + ph - 200 * s + (float)k * 44 * s, ts, st.value, preset_text[p][k]);
            }
        }
    }
    const char *hint = page == P_KEYS || page == P_PAD ? "Cross / Enter  change        Triangle / Esc  back"
                       : page == P_ROOT ? "Cross / Enter  select        Triangle / Esc  back to the main menu"
                                        : "Left / Right  change        Cross / Enter  select        Triangle / Esc  back";
    float hs = 36 * s, hw = ui_text_width(UI_BODY, hs, hint);
    if (hw > pw - 120 * s) hs *= (pw - 120 * s) / hw; /* FE1's typewriter font is wide */
    ui_text(UI_BODY, px + 60 * s, py + ph - 80 * s, hs, st.dim, hint);
    if (page == P_PAD && !input_has_controller()) { /* bottom right, above the hints (FE1's titles are long) */
        const char *nc = "No controller connected";
        ui_text(UI_BODY, px + pw - 70 * s - ui_text_width(UI_BODY, 36 * s, nc), py + ph - 130 * s, 36 * s, st.title, nc);
    }
}

void menu_draw(int w, int h) {
    int osd = osd_text[0] && (int32_t)(osd_until - SDL_GetTicks()) > 0;
    /* opaque over the whole window: the menu, the way back to the game's main menu (closing), the walk through the
     * game's screens to its credits / art gallery and back, and any frame of the game's own options screens */
    int cover = (ex && ex != EX_SHOW) || (menu_allowed() && gopt_original_screen());
    int show = menu_open || closing || cover;
    const char *sub[4];
    int nsub = show ? 0 : subs_lines(sub, 4);
    if (!show && !osd && !nsub) return;
    if (!ui_begin(w, h)) return;
    if (capture && page == P_PAD && SDL_GetTicks() - capture_since > 6000) capture = 0; /* nothing pressed: cancel */
    Style st = style();
    float s = (float)h / 1080.0f; /* layout at 1080p, scaled */
    if (show) {
        ui_rect(0, 0, (float)w, (float)h, st.cover);
        if (menu_open || closing) draw_menu(w, h, st, s);
        else { /* the walk through the game's screens: the panel without rows */
            float pw = 1240 * s, ph = 900 * s, px = ((float)w - pw) / 2, py = ((float)h - ph) / 2;
            ui_rect(px, py, pw, ph, st.panel);
            frame(px, py, pw, ph, st.line, 3 * s);
            ui_text(game_fe1() ? UI_TITLE : UI_TITLE, px + 60 * s, py + 30 * s, 84 * s, st.title, game_fe1() ? "options" : "OPTIONS");
            const char *t = ex == EX_ENTER ? (game_fe1() ? "credits ..." : "ART GALLERY ...") : "...";
            ui_text(UI_BODY, px + 70 * s, py + 150 * s, 44 * s, st.text, t);
        }
    }
    if (nsub) { /* the game's subtitles: the notices' face and colour, no box, a soft shadow; in the bottom band */
        float size = 44 * s, lh = 50 * s;
        for (int i = 0; i < nsub; i++) {
            float sz = size, tw = ui_text_width(UI_BODY, sz, sub[i]);
            if (tw > (float)w * 0.94f) { sz *= (float)w * 0.94f / tw; tw = (float)w * 0.94f; }
            float x = ((float)w - tw) / 2, y = (float)h - 12 * s - (float)(nsub - i) * lh + (size - sz) / 2;
            static const float sh[5][3] = {{-1.5f, -1.5f, 0x50}, {1.5f, -1.5f, 0x50}, {-1.5f, 1.5f, 0x50}, {1.5f, 1.5f, 0x50}, {2.5f, 3, 0x70}};
            for (int k = 0; k < 5; k++) ui_text(UI_BODY, x + sh[k][0] * s, y + sh[k][1] * s, sz, (uint32_t)sh[k][2], sub[i]);
            ui_text(UI_BODY, x, y, sz, st.value, sub[i]);
        }
    }
    if (osd) {
        float size = 40 * s, tw = ui_text_width(UI_BODY, size, osd_text);
        float x = ((float)w - tw) / 2, y = show ? (float)h / 2 + 470 * s : (float)h - 140 * s; /* under the panel */
        ui_rect(x - 30 * s, y - 14 * s, tw + 60 * s, size + 28 * s, 0x000000C0);
        ui_text(UI_BODY, x, y, size, st.value, osd_text);
    }
    ui_end();
}
