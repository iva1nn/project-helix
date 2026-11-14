/* Input: every DualShock button and both sticks, plus the port's own actions (directional controls,
 * remaster on/off, overlay mode, fullscreen), bound to keyboard keys and game-controller buttons through
 * fe2port.ini / fe1port.ini (created next to the exe with the defaults on first run; the controls menu,
 * menu.c, rebinds and saves them). */
#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>
#include <ctype.h>
#include <stdlib.h>
#include "hw.h"
#include "input.h"

uint16_t pad_buttons;              /* PS1 bit order, 1 = pressed */
uint8_t pad_axes[4] = {0x80, 0x80, 0x80, 0x80}; /* right X, right Y, left X, left Y (0..255) */
int pad_analog_toggle;             /* the ANALOG button was pressed this frame */
void (*input_action_hook)(int action); /* an action's key or button went down (main.c) */
int input_blocked;                 /* the menu is open: the game gets no input */
uint16_t input_raw_buttons;        /* the game buttons held, also while blocked (menu.c waits for release) */

static const char *const bind_names[B_COUNT] = {
    "select", "l3", "r3", "start", "up", "right", "down", "left", "l2", "r2", "l1", "r1",
    "triangle", "circle", "cross", "square",
    "lstick_up", "lstick_down", "lstick_left", "lstick_right",
    "rstick_up", "rstick_down", "rstick_left", "rstick_right", "analog",
    "act_controls", "act_remaster", "act_overlay", "act_fullscreen",
    "act_state_save", "act_state_load", "act_state_next", "act_state_prev"};

static const char *const default_keys[B_COUNT] = {
    "Backspace", "Z", "C", "Return", "Up", "Right", "Down", "Left", "1", "3", "Q", "E",
    "W", "D", "S", "A",
    "I", "K", "J", "L",
    "Keypad 8", "Keypad 2", "Keypad 4", "Keypad 6", "F9",
    "F7", "F12", "F10", "F11",
    "F5", "F8", "PageUp", "PageDown"};
/* controller: SDL button names, or lefttrigger/righttrigger; sticks use the analog axes. Select, L3 and R3
 * do nothing in either game (tools/button_test.py), so they carry the port's actions. */
static const char *const default_pad[B_COUNT] = {
    "", "", "", "start", "dpup", "dpright", "dpdown", "dpleft",
    "lefttrigger", "righttrigger", "leftshoulder", "rightshoulder",
    "y", "b", "a", "x",
    "", "", "", "", "", "", "", "", "guide",
    "leftstick", "back, touchpad", "rightstick", "",
    "", "", "", ""};

/* Steam Deck (input_is_steam_deck): no keyboard in Game Mode, the Steam and ... buttons belong to Steam and the
 * trackpads are no "touchpad" button, so the save states go on View combos (View held + a shoulder / trigger) and View
 * alone is the remaster switch; a new settings file also gets the user's PC picture values at 4x (14 Nov 2026). */
static const char *const deck_pad[B_COUNT] = {
    "", "", "", "start", "dpup", "dpright", "dpdown", "dpleft",
    "lefttrigger", "righttrigger", "leftshoulder", "rightshoulder",
    "y", "b", "a", "x",
    "", "", "", "", "", "", "", "", "guide",
    "leftstick", "back", "rightstick", "",
    "back+rightshoulder", "back+leftshoulder", "back+righttrigger", "back+lefttrigger"};

int input_is_steam_deck(void) {
#ifdef _WIN32
    return 0;
#else
    static int deck = -1;
    if (deck < 0) {
        const char *e = getenv("SteamDeck");
        deck = e && !strcmp(e, "1");
        char v[64] = {0}, p[64] = {0};
        FILE *f = fopen("/sys/class/dmi/id/board_vendor", "r");
        if (f) { if (!fgets(v, sizeof v, f)) v[0] = 0; fclose(f); }
        f = fopen("/sys/class/dmi/id/board_name", "r");
        if (f) { if (!fgets(p, sizeof p, f)) p[0] = 0; fclose(f); }
        if (!strncmp(v, "Valve", 5) && (!strncmp(p, "Jupiter", 7) || !strncmp(p, "Galileo", 7))) deck = 1;
        if (deck) LOG("input: Steam Deck: its defaults for a new settings file\n");
    }
    return deck;
#endif
}

static const char *pad_default(int b) { return input_is_steam_deck() ? deck_pad[b] : default_pad[b]; }

static SDL_Scancode keys[B_COUNT][INPUT_MAXB];
static int nkeys[B_COUNT];
/* mod >= 0: a combo, the button mod held while id is pressed ("back+rightshoulder") */
typedef struct { int is_axis; int id; int mod; } PadBind;
static PadBind pbind[B_COUNT][INPUT_MAXB];
static int npbind[B_COUNT];
static int deadzone = 8000; /* stick deadzone (0..32767) */
int input_stick_as_dpad = 0;  /* left stick also presses the d-pad */
int input_analog_on_start = 1; /* DualShock starts in analog mode (left stick works) */
static char ini_path[1024];

static SDL_GameController *ctrl;
static SDL_JoystickID ctrl_id = -1;

static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

static void set_keys(int b, const char *list) {
    char buf[256];
    snprintf(buf, sizeof buf, "%s", list);
    nkeys[b] = 0;
    for (char *t = strtok(buf, ","); t && nkeys[b] < INPUT_MAXB; t = strtok(0, ",")) {
        t = trim(t);
        if (!*t) continue;
        SDL_Scancode sc = SDL_GetScancodeFromName(t);
        if (sc == SDL_SCANCODE_UNKNOWN) LOG("input: unknown key \"%s\" for %s\n", t, bind_names[b]);
        else keys[b][nkeys[b]++] = sc;
    }
}

static void set_pad(int b, const char *list) {
    char buf[256];
    snprintf(buf, sizeof buf, "%s", list);
    npbind[b] = 0;
    for (char *t = strtok(buf, ","); t && npbind[b] < INPUT_MAXB; t = strtok(0, ",")) {
        t = trim(t);
        if (!*t) continue;
        int mod = -1;
        char *plus = strchr(t, '+');
        if (plus) { /* a combo: the first button held, the second pressed */
            *plus = 0;
            mod = SDL_GameControllerGetButtonFromString(trim(t));
            t = trim(plus + 1);
            if (mod == SDL_CONTROLLER_BUTTON_INVALID) { LOG("input: unknown combo button for %s\n", bind_names[b]); continue; }
        }
        SDL_GameControllerButton btn = SDL_GameControllerGetButtonFromString(t);
        if (btn != SDL_CONTROLLER_BUTTON_INVALID) { pbind[b][npbind[b]++] = (PadBind){0, btn, mod}; continue; }
        SDL_GameControllerAxis ax = SDL_GameControllerGetAxisFromString(t);
        if (ax == SDL_CONTROLLER_AXIS_TRIGGERLEFT || ax == SDL_CONTROLLER_AXIS_TRIGGERRIGHT) {
            pbind[b][npbind[b]++] = (PadBind){1, ax, mod};
            continue;
        }
        LOG("input: unknown controller button \"%s\" for %s\n", t, bind_names[b]);
    }
}

static void keys_text(int b, char *out, size_t n) {
    out[0] = 0;
    for (int i = 0; i < nkeys[b]; i++)
        snprintf(out + strlen(out), n - strlen(out), "%s%s", i ? ", " : "", SDL_GetScancodeName(keys[b][i]));
}

static void pad_text(int b, char *out, size_t n) {
    out[0] = 0;
    for (int i = 0; i < npbind[b]; i++) {
        const char *s = pbind[b][i].is_axis ? SDL_GameControllerGetStringForAxis((SDL_GameControllerAxis)pbind[b][i].id)
                                            : SDL_GameControllerGetStringForButton((SDL_GameControllerButton)pbind[b][i].id);
        const char *m = pbind[b][i].mod >= 0 ? SDL_GameControllerGetStringForButton((SDL_GameControllerButton)pbind[b][i].mod) : 0;
        snprintf(out + strlen(out), n - strlen(out), "%s%s%s%s", i ? ", " : "", m ? m : "", m ? "+" : "", s ? s : "?");
    }
}

static void write_bindings(FILE *f) {
    fprintf(f, "; Input bindings. Several inputs per action: separate with commas. The controls menu in the\n"
               "; game's options rewrites these two sections.\n"
               "; Keyboard names: SDL key names (e.g. Return, Space, Left Shift, Keypad 8, F9).\n"
               "; Controller names: a b x y back guide start leftstick rightstick leftshoulder rightshoulder\n"
               ";   dpup dpdown dpleft dpright misc1 touchpad paddle1-4, and lefttrigger / righttrigger.\n"
               "; A combo: two names joined by + (back+rightshoulder = hold View / Select, press R1); the first\n"
               ";   button's own action then fires on its release, and only if no combo was used meanwhile.\n"
               "; Positions are by layout (Xbox naming): a = bottom button (Cross on a PlayStation pad).\n"
               "; Both sticks of the controller are always the DualShock sticks; 'analog' toggles the\n"
               "; analog mode like the ANALOG button of a DualShock (the game may also switch it).\n"
               "; act_controls = directional / tank controls, act_remaster = remaster on / off,\n"
               "; act_overlay = screen layout (fill with HUD bands / HUD over the picture / original 4:3),\n"
               "; act_fullscreen = fullscreen / window.\n"
               "; act_state_save / act_state_load = save / load the state in the current slot (states folder next\n"
               ";   to this file), act_state_next / act_state_prev = choose the slot (1-9). The actions do not fire\n"
               ";   while Shift, Ctrl or Alt is held.\n\n[keyboard]\n");
    char t[256];
    for (int b = 0; b < B_COUNT; b++) { keys_text(b, t, sizeof t); fprintf(f, "%s = %s\n", bind_names[b], t); }
    fprintf(f, "\n[controller]\n");
    for (int b = 0; b < B_COUNT; b++) { pad_text(b, t, sizeof t); fprintf(f, "%s = %s\n", bind_names[b], t); }
    fprintf(f, "; the controller's own sticks are the DualShock sticks; stick deadzone 0..32767\n");
    fprintf(f, "deadzone = %d\n", deadzone);
    fprintf(f, "; 1 = start with the ANALOG light on (left stick works), 0 = digital like a powered-on DualShock\n");
    fprintf(f, "analog_on_start = %d\n", input_analog_on_start);
    fprintf(f, "; 1 = the left stick also presses the d-pad (menus, digital mode)\n");
    fprintf(f, "stick_as_dpad = %d\n", input_stick_as_dpad);
}

void write_default_ini(const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) return;
    int deck = input_is_steam_deck();
    fprintf(f, "; Fear Effect native port: settings. Command-line options override them.\n\n[game]\n"
               "; the game's Disc 1 image (.cue); Discs 2-4 are found next to it (same name with (Disc N),\n"
               "; or in sibling folders disc2..disc4). Used when the exe is started without a disc argument.\n"
               "disc = \n"
               "; the data folder (8x movies, character models, texture packs, fonts, ffmpeg), empty = the\n"
               "; folder 'data' next to the exe\n"
               "data = \n"
               "; 8x backgrounds and character models: empty = from the data folder, off = none\n"
               "; (a remaster work folder here is for development)\n"
               "remaster = \n"
               "; internal resolution 1-8, renderer hw (OpenGL) or sw\n"
               "scale = %d\nrenderer = hw\n"
               "; loading speed: data read this many times faster than the CD drive (movies and voices keep its\n"
               "; rate); 1 = the original loading times\n"
               "cd_speed = 8\n"
               "; 60 fps characters: the remodels posed between the game's 30 fps frames on the vblanks between; 0 = off\n"
               "smooth = 1\n"
               "; picture effects (the remaster's shaders; the controls menu's Picture effects page sets them):\n"
               "; bloom (glow of the brightest lights), vignette (darker corners), film grain; 1 = on, 0 = off.\n"
               "; More options: bloom_threshold 0.7, bloom_radius 1.0, bloom_halo 0.5, vignette_start 0.45,\n"
               "; vignette_softness 0.55, grain_rows 640, grain_refresh 24\n"
               "bloom = 1\nbloom_intensity = %s\nvignette = 1\nvignette_strength = %s\n"
               "grain = 1\ngrain_strength = %s\n"
               "; room loops: the end of a room's loop dissolves into its start over this many movie frames\n"
               "; (15 per second); 0 = a hard cut\n"
               "loop_fade = %d\n"
               "; display = fullscreen (borderless) or windowed;\n"
               "; fill = screen layout in rooms: 1 enlarged picture with the HUD in bands, 2 HUD over the\n"
               "; picture, 0 the original 4:3 frame\n"
               "display = fullscreen\nfill = 1\n"
               "; remaster character models (F8)\nmodels = 1\n"
               "; directional controls: the stick / d-pad direction is the direction on screen;\n"
               "; 0 = the original tank controls. L1 held gives tank controls for the quick turn.\n"
               "directional = 1\n"
               "; the texture packs (redrawn interface, F6): the folder holding the SLUS-xxxxx folders,\n"
               "; empty = the data folder's, off = none\n"
               "textures = \n\n",
            deck ? 4 : 8, deck ? "0.50" : "0.80", deck ? "0.50" : "0.30", deck ? "0.50" : "1.00", deck ? 4 : 8);
    for (int b = 0; b < B_COUNT; b++) { set_keys(b, default_keys[b]); set_pad(b, pad_default(b)); }
    write_bindings(f);
    fclose(f);
}

/* rewrite the binding sections of the settings file, keeping the other sections (before and after them) */
static void add_line(char **buf, size_t *n, size_t *cap, const char *line) {
    size_t l = strlen(line);
    if (*n + l + 2 > *cap) { *cap = (*n + l + 2) * 2; *buf = realloc(*buf, *cap); }
    memcpy(*buf + *n, line, l);
    (*buf)[*n + l] = '\n';
    *n += l + 1;
}

void input_save(void) {
    if (!ini_path[0]) return;
    FILE *f = fopen(ini_path, "r");
    char *head = 0, *tail = 0;
    size_t hn = 0, hcap = 0, tn = 0, tcap = 0;
    if (f) {
        char line[1200];
        int part = 0; /* 0 before the bindings, 1 in them, 2 after them */
        while (fgets(line, sizeof line, f)) {
            char *s = trim(line);
            int ours = !strncmp(s, "[keyboard]", 10) || !strncmp(s, "[controller]", 12);
            /* the bindings' header comment (also the one of earlier versions) */
            if (part == 0 && (!strncmp(s, "; Input bindings", 16) || !strncmp(s, "; Fear Effect native port: input bindings", 41))) part = 1;
            if (ours) part = 1;
            else if (part == 1 && s[0] == '[') part = 2;
            if (part == 0) add_line(&head, &hn, &hcap, s);
            else if (part == 2) add_line(&tail, &tn, &tcap, s);
        }
        fclose(f);
    }
    f = fopen(ini_path, "w");
    if (!f) { free(head); free(tail); return; }
    if (head) {
        while (hn > 1 && head[hn - 1] == '\n' && head[hn - 2] == '\n') hn--; /* one blank line before the bindings */
        fwrite(head, 1, hn, f);
        fputc('\n', f);
    }
    write_bindings(f);
    if (tail) { fputc('\n', f); fwrite(tail, 1, tn, f); }
    fclose(f);
    free(head);
    free(tail);
    LOG("input: bindings saved to %s\n", ini_path);
}

/* set one key of the settings file's [game] section (the controls menu's switches): the key's line is replaced,
 * or added after the section's last setting; the rest of the file stays as it is */
void settings_set(const char *key, const char *value) {
    if (!ini_path[0]) return;
    FILE *f = fopen(ini_path, "r");
    char *buf = 0;
    size_t n = 0, cap = 0, game_end = 0; /* game_end: just after [game]'s header or its last setting */
    int in_game = 0, done = 0, have_game = 0;
    char set[300];
    snprintf(set, sizeof set, "%s = %s", key, value);
    if (f) {
        char line[1200];
        size_t kl = strlen(key);
        while (fgets(line, sizeof line, f)) {
            size_t l = strlen(line);
            while (l && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = 0;
            const char *s = line;
            while (*s == ' ' || *s == '\t') s++;
            if (*s == '[') in_game = !strncmp(s, "[game]", 6);
            if (in_game && !done && *s != '[' && !strncmp(s, key, kl) && (s[kl] == ' ' || s[kl] == '\t' || s[kl] == '=')) {
                add_line(&buf, &n, &cap, set);
                done = 1;
            } else add_line(&buf, &n, &cap, line);
            if (in_game && (*s == '[' || (*s && *s != ';' && *s != '#'))) { game_end = n; have_game = 1; }
        }
        fclose(f);
    }
    f = fopen(ini_path, "w");
    if (!f) { free(buf); return; }
    if (!have_game) fprintf(f, "[game]\n%s\n\n", set);
    if (buf && !done && have_game) { /* a new setting: after [game]'s last one */
        fwrite(buf, 1, game_end, f);
        fprintf(f, "%s\n", set);
        fwrite(buf + game_end, 1, n - game_end, f);
    } else if (buf) fwrite(buf, 1, n, f);
    fclose(f);
    free(buf);
}

void input_init(void) {
    for (int b = 0; b < B_COUNT; b++) { set_keys(b, default_keys[b]); set_pad(b, pad_default(b)); }
    char *base = SDL_GetBasePath();
    snprintf(ini_path, sizeof ini_path, "%s" FE_GAME "port.ini", base ? base : "");
    if (base) SDL_free(base);
    FILE *f = fopen(ini_path, "r");
    if (!f) { write_default_ini(ini_path); LOG("input: wrote default bindings to %s\n", ini_path); return; }
    char line[512];
    int section = 0; /* 1 keyboard, 2 controller */
    int seen_actions = 0;
    while (fgets(line, sizeof line, f)) {
        char *s = trim(line);
        if (!*s || *s == ';' || *s == '#') continue;
        if (*s == '[') { section = !strncmp(s, "[keyboard]", 10) ? 1 : !strncmp(s, "[controller]", 12) ? 2 : 0; continue; }
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = 0;
        char *name = trim(s), *val = trim(eq + 1);
        if (section == 2 && !strcmp(name, "deadzone")) { deadzone = atoi(val); continue; }
        if (section == 2 && !strcmp(name, "analog_on_start")) { input_analog_on_start = atoi(val); continue; }
        if (section == 2 && !strcmp(name, "stick_as_dpad")) { input_stick_as_dpad = atoi(val); continue; }
        if (!strncmp(name, "act_", 4)) seen_actions = 1;
        for (int b = 0; b < B_COUNT; b++)
            if (!strcmp(name, bind_names[b])) {
                if (section == 1) set_keys(b, val);
                else if (section == 2) set_pad(b, val);
            }
    }
    fclose(f);
    if (!seen_actions) {
        /* a settings file from before the actions: Select, L3 and R3 move to the actions, as in a new file */
        set_pad(B_SELECT, pad_default(B_SELECT)); set_pad(B_L3, pad_default(B_L3)); set_pad(B_R3, pad_default(B_R3));
        input_save();
    }
    LOG("input: bindings from %s\n", ini_path);
}

/* ------------------------------------------------------------------ the menu's view of the bindings */

const char *input_bind_id(int b) { return bind_names[b]; }

void input_bind_text(int b, int pad, char *out, size_t n) {
    if (pad) pad_text(b, out, n);
    else keys_text(b, out, n);
}

/* one input per binding from the menu; the same input is taken off every other binding */
void input_bind_key(int b, SDL_Scancode sc) {
    for (int o = 0; o < B_COUNT; o++)
        for (int i = 0; i < nkeys[o]; i++)
            if (keys[o][i] == sc) { memmove(&keys[o][i], &keys[o][i + 1], sizeof(SDL_Scancode) * (size_t)(nkeys[o] - i - 1)); nkeys[o]--; i--; }
    keys[b][0] = sc;
    nkeys[b] = 1;
}

void input_bind_pad(int b, int is_axis, int id) {
    for (int o = 0; o < B_COUNT; o++)
        for (int i = 0; i < npbind[o]; i++)
            if (pbind[o][i].is_axis == is_axis && pbind[o][i].id == id && pbind[o][i].mod < 0) {
                memmove(&pbind[o][i], &pbind[o][i + 1], sizeof(PadBind) * (size_t)(npbind[o] - i - 1));
                npbind[o]--; i--;
            }
    pbind[b][0] = (PadBind){is_axis, id, -1};
    npbind[b] = 1;
}

void input_bind_defaults(int pad) {
    for (int b = 0; b < B_COUNT; b++) {
        if (pad) set_pad(b, pad_default(b));
        else set_keys(b, default_keys[b]);
    }
}

int input_has_controller(void) { return ctrl != 0; }

/* ------------------------------------------------------------------ devices and polling */

static void open_controller(int index) {
    if (ctrl) return;
    ctrl = SDL_GameControllerOpen(index);
    if (ctrl) {
        ctrl_id = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(ctrl));
        LOG("input: controller \"%s\"\n", SDL_GameControllerName(ctrl));
    }
}

void input_open_controllers(void) {
    for (int i = 0; i < SDL_NumJoysticks(); i++)
        if (SDL_IsGameController(i)) { open_controller(i); break; }
}

void input_event(const SDL_Event *e) {
    if (e->type == SDL_CONTROLLERDEVICEADDED) open_controller(e->cdevice.which);
    if (e->type == SDL_CONTROLLERDEVICEREMOVED && ctrl && e->cdevice.which == ctrl_id) {
        SDL_GameControllerClose(ctrl);
        ctrl = 0; ctrl_id = -1;
        LOG("input: controller removed\n");
        input_open_controllers();
    }
}

static uint8_t axis_to_u8(int v) {
    if (v > -deadzone && v < deadzone) return 0x80;
    int r = (v + 32768) >> 8;
    return (uint8_t)(r < 0 ? 0 : r > 255 ? 255 : r);
}

static int pad_input_down(PadBind p) {
    int d = p.is_axis ? SDL_GameControllerGetAxis(ctrl, (SDL_GameControllerAxis)p.id) > 12000
                      : SDL_GameControllerGetButton(ctrl, (SDL_GameControllerButton)p.id);
    return d && (p.mod < 0 || SDL_GameControllerGetButton(ctrl, (SDL_GameControllerButton)p.mod));
}

/* a controller input bound to one of the port's actions does not also reach the game; a combo's second input only
 * while the combo's first button is held (R1 stays the game's R1 without View) */
static int used_by_action(PadBind p) {
    for (int b = B_ACT_FIRST; b < B_COUNT; b++)
        for (int i = 0; i < npbind[b]; i++) {
            PadBind q = pbind[b][i];
            if (q.is_axis != p.is_axis || q.id != p.id) continue;
            if (q.mod < 0 || SDL_GameControllerGetButton(ctrl, (SDL_GameControllerButton)q.mod)) return 1;
        }
    return 0;
}

/* buttons that start a combo: an action bound to one alone fires on its release, if no combo was used while it was
 * held (View alone = remaster on / off, View + R1 = save state) */
static int combo_first(int btn) {
    for (int b = B_ACT_FIRST; b < B_COUNT; b++)
        for (int i = 0; i < npbind[b]; i++)
            if (pbind[b][i].mod == btn) return 1;
    return 0;
}

void input_poll(void) {
    const uint8_t *k = SDL_GetKeyboardState(0);
    int on[B_COUNT] = {0};
    /* the port's actions on the keyboard: not with Shift / Ctrl / Alt held (Shift+F8, Alt+Enter are fixed keys) */
    int mods = (SDL_GetModState() & (KMOD_SHIFT | KMOD_CTRL | KMOD_ALT)) != 0;
    /* combo first buttons: held / released this poll, and whether a combo was used during the hold */
    static int held_prev[SDL_CONTROLLER_BUTTON_MAX], combo_used[SDL_CONTROLLER_BUTTON_MAX];
    int clean_release[SDL_CONTROLLER_BUTTON_MAX] = {0};
    if (ctrl)
        for (int m = 0; m < SDL_CONTROLLER_BUTTON_MAX; m++) {
            if (!combo_first(m)) { held_prev[m] = 0; continue; }
            int held = SDL_GameControllerGetButton(ctrl, (SDL_GameControllerButton)m);
            if (held && !held_prev[m]) combo_used[m] = 0;
            for (int b = B_ACT_FIRST; b < B_COUNT; b++)
                for (int i = 0; i < npbind[b]; i++)
                    if (pbind[b][i].mod == m && pad_input_down(pbind[b][i])) combo_used[m] = 1;
            clean_release[m] = !held && held_prev[m] && !combo_used[m];
            held_prev[m] = held;
        }
    for (int b = 0; b < B_COUNT; b++) {
        if (!(b >= B_ACT_FIRST && mods))
            for (int i = 0; i < nkeys[b]; i++) if (k[keys[b][i]]) on[b] = 1;
        if (ctrl)
            for (int i = 0; i < npbind[b]; i++) {
                PadBind p = pbind[b][i];
                int down = b >= B_ACT_FIRST && p.mod < 0 && !p.is_axis && combo_first(p.id) ? clean_release[p.id]
                                                                                            : pad_input_down(p);
                if (down && (b >= B_ACT_FIRST || !used_by_action(p))) on[b] = 1;
            }
    }
    /* the port's actions: on the press */
    static int prev_act[B_COUNT];
    for (int b = B_ACT_FIRST; b < B_COUNT; b++) {
        if (on[b] && !prev_act[b] && input_action_hook && !input_blocked) input_action_hook(b - B_ACT_FIRST);
        prev_act[b] = on[b];
    }
    uint16_t bt = 0;
    for (int b = 0; b < 16; b++) if (on[b]) bt |= (uint16_t)(1u << b);
    pad_buttons = bt;
    input_raw_buttons = bt;
    /* sticks: controller axes, keyboard keys push to the edge */
    uint8_t lx = 0x80, ly = 0x80, rx = 0x80, ry = 0x80;
    if (ctrl) {
        lx = axis_to_u8(SDL_GameControllerGetAxis(ctrl, SDL_CONTROLLER_AXIS_LEFTX));
        ly = axis_to_u8(SDL_GameControllerGetAxis(ctrl, SDL_CONTROLLER_AXIS_LEFTY));
        rx = axis_to_u8(SDL_GameControllerGetAxis(ctrl, SDL_CONTROLLER_AXIS_RIGHTX));
        ry = axis_to_u8(SDL_GameControllerGetAxis(ctrl, SDL_CONTROLLER_AXIS_RIGHTY));
    }
    if (on[B_LS_LEFT]) lx = 0x00;
    if (on[B_LS_RIGHT]) lx = 0xFF;
    if (on[B_LS_UP]) ly = 0x00;
    if (on[B_LS_DOWN]) ly = 0xFF;
    if (on[B_RS_LEFT]) rx = 0x00;
    if (on[B_RS_RIGHT]) rx = 0xFF;
    if (on[B_RS_UP]) ry = 0x00;
    if (on[B_RS_DOWN]) ry = 0xFF;
    pad_axes[0] = rx; pad_axes[1] = ry; pad_axes[2] = lx; pad_axes[3] = ly;
    if (input_stick_as_dpad) { /* the left stick also presses the d-pad (PS1 bits: up 4, right 5, down 6, left 7) */
        if (ly < 0x40) pad_buttons |= 0x10;
        if (lx > 0xC0) pad_buttons |= 0x20;
        if (ly > 0xC0) pad_buttons |= 0x40;
        if (lx < 0x40) pad_buttons |= 0x80;
    }
    static int prev_analog;
    pad_analog_toggle = on[B_ANALOG] && !prev_analog;
    prev_analog = on[B_ANALOG];
    if (input_blocked) { /* the menu is open */
        pad_buttons = 0;
        memset(pad_axes, 0x80, 4);
        pad_analog_toggle = 0;
    }
}

/* vibration from the game (DualShock motors) */
void input_rumble(int small_on, int large) {
    static int was_on;
    if (!ctrl) return;
    int now_on = small_on || large;
    if (!now_on && !was_on) return; /* refresh while active (each call lasts 200 ms) */
    was_on = now_on;
    SDL_GameControllerRumble(ctrl, (uint16_t)(large * 257), small_on ? 0xFFFF : 0, 200);
}

/* list controllers SDL sees (diagnostics: --list-controllers) */
void input_list(void) {
    printf("%d joystick(s)\n", SDL_NumJoysticks());
    for (int i = 0; i < SDL_NumJoysticks(); i++)
        printf("  %d: %s%s\n", i, SDL_JoystickNameForIndex(i), SDL_IsGameController(i) ? " (game controller)" : "");
}
