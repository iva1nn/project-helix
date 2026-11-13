/* The port's own screens: rectangles and text in window pixels (ui.c), the controls menu and notices (menu.c). */
#ifndef UI_H
#define UI_H
#include <stdint.h>
#include <SDL2/SDL.h>

enum { UI_TITLE, UI_BODY, UI_FONTS };

int ui_begin(int w, int h); /* 0 = no UI drawing (no hardware renderer or no font) */
void ui_end(void);
void ui_rect(float x, float y, float w, float h, uint32_t rgba); /* rgba = 0xRRGGBBAA */
float ui_text(int font, float x, float y, float size, uint32_t rgba, const char *s);
float ui_text_width(int font, float size, const char *s);

/* menu.c */
void menu_vblank(void);                      /* opens over the game's controls screen */
int menu_event(const SDL_Event *e);          /* 1 = the menu used the event */
void menu_draw(int w, int h);                /* in the window, after the game picture */
void menu_filter_pad(void);                  /* before the pad state is recorded: blocked input, injected presses */
void osd_show(const char *text);             /* a short notice */
extern int menu_open;

/* subs.c */
int subs_lines(const char **out, int max); /* the game's subtitle lines on screen now, top first */
#endif
