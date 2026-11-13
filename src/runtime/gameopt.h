/* gameopt.c: the games' own options in RAM (the port's options menu sets them). */
#ifndef GAMEOPT_H
#define GAMEOPT_H
enum { GO_SUBTITLES, GO_DIFFICULTY, GO_VIBRATION, GO_STEREO, GO_VOLUME, GO_AUTO_DRAW, GO_SAVE_BEACON, GO_CONTROL_TYPE,
       GO_PRESET, GO_N };
extern int gopt_fe1;
void gopt_init(void);
int gopt_game(void); /* 0 none, 1 FE1, 2 FE2 */
int gopt_has(int id);
int gopt_get(int id);
void gopt_set(int id, int v);
int gopt_map_get(int action);
void gopt_map_set(int action, int bit);
void gopt_map_reset(void);
int gopt_main_menu_options(void);
int gopt_main_menu(void);
int gopt_original_screen(void);
int gopt_extra_running(void);
int gopt_cursor(void);
void gopt_debug_frame(void);
#endif
