/* Input bindings shared by input.c (polling, settings file) and menu.c (the controls menu). */
#ifndef INPUT_H
#define INPUT_H
#include <stddef.h>
#include <SDL2/SDL.h>

enum {
    B_SELECT, B_L3, B_R3, B_START, B_UP, B_RIGHT, B_DOWN, B_LEFT, B_L2, B_R2, B_L1, B_R1,
    B_TRIANGLE, B_CIRCLE, B_CROSS, B_SQUARE, /* 0..15 = PS1 button bits */
    B_LS_UP, B_LS_DOWN, B_LS_LEFT, B_LS_RIGHT, B_RS_UP, B_RS_DOWN, B_RS_LEFT, B_RS_RIGHT, B_ANALOG,
    B_ACT_CONTROLS, B_ACT_REMASTER, B_ACT_OVERLAY, B_ACT_FULLSCREEN, /* the port's own actions */
    B_ACT_STATE_SAVE, B_ACT_STATE_LOAD, B_ACT_STATE_NEXT, B_ACT_STATE_PREV, /* save states (state.c) */
    B_COUNT
};
#define B_ACT_FIRST B_ACT_CONTROLS
enum { ACT_CONTROLS, ACT_REMASTER, ACT_OVERLAY, ACT_FULLSCREEN, ACT_STATE_SAVE, ACT_STATE_LOAD, ACT_STATE_NEXT, ACT_STATE_PREV };
#define INPUT_MAXB 4

extern void (*input_action_hook)(int action);
extern int input_blocked;

const char *input_bind_id(int b);
void input_bind_text(int b, int pad, char *out, size_t n);
void input_bind_key(int b, SDL_Scancode sc);
void input_bind_pad(int b, int is_axis, int id);
void input_bind_defaults(int pad);
void input_save(void);
void settings_set(const char *key, const char *value); /* one key of the settings file's [game] section */
int input_has_controller(void);
#endif
