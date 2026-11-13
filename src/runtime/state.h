/* Save states (state.c; NOTES.md "Save states"): the serialisation helpers every module uses for its own part.
 *
 * A state file is a header and a list of sections; each section is a tag, a version and its size. A module writes and
 * reads its fields with the same calls (ST / st_raw), in the same order: io->load says which way. A load first runs
 * every section "dry" (read and checked, nothing changed), so an incompatible file is refused before anything is
 * touched; a section whose size or version differs from this build's is such a file. */
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct StIO {
    int load;          /* 0 = saving into buf, 1 = loading from buf */
    int dry;           /* loading: read and check only, change nothing */
    uint8_t *buf;
    size_t n, cap;     /* saving: bytes written; loading: buffer size */
    size_t pos;        /* loading: read position */
    size_t sec_pos, sec_end;
    char sec[9];
    char err[300];     /* first error; every call after it does nothing */
} StIO;

void st_begin(StIO *io, const char *tag, uint32_t version);
/* st_begin that also reads a section of an older version (min_version .. version): returns the file's version
 * (saving: version); the module then reads that version's fields */
uint32_t st_begin_compat(StIO *io, const char *tag, uint32_t version, uint32_t min_version);
void st_end(StIO *io);
void st_raw(StIO *io, void *p, size_t n);
#define ST(io, v) st_raw((io), &(v), sizeof(v))
/* a count followed by that many items: saving writes live, loading returns the file's (checked against max) */
int st_count(StIO *io, int live, int max);
void st_fail(StIO *io, const char *fmt, ...);

/* each module's part (in the module, next to its statics) */
void core_state(StIO *io);   /* core.c: CPU, shadow stack, interrupts, scheduler, DMA, timers, RAM */
void bios_state(StIO *io);   /* bios.c: events, interrupt chains, pads, memory card events */
void gte_state(StIO *io);    /* gte.c */
void gpu_state(StIO *io);    /* gpu.c: registers and VRAM */
void cdrom_state(StIO *io);  /* cdrom.c: controller, queue, read position */
void mdec_state(StIO *io);   /* mdec.c */
void spu_state(StIO *io);    /* spu.c: registers, sound RAM, voices, reverb, CD audio */
void sio_state(StIO *io);    /* sio.c: the port and the DualShock */
void mcd_state(StIO *io);    /* mcd.c: open files (the card's contents stay the user's) */
void controls_state(StIO *io); /* controls.c: directional controls (they change the pad state the game gets) */
void game_state(StIO *io);   /* game_fe2.c: the disc swap's wait, the recent movie frames */
void gopt_state(StIO *io);   /* gameopt.c */
void models_state(StIO *io); /* models.c: the room camera's place in RAM, the last character's light (scene actors) */
void texrep_state(StIO *io); /* texrep.c: which VRAM blocks hold pack / font images (rebuilt on load) */

/* host-side state rebuilt after a load (not in the file) */
void hw_state_loaded(void);     /* gpu_hw.c: upscaled VRAM from the native one, 60 fps buffers, a black window */
void models_state_loaded(void); /* models.c: actors, poses, packet owners */
void game_state_loaded(void);   /* game_fe2.c: the 8x picture: both movie buffers stand-ins, decoder to the frame */
void subs_state_loaded(void);   /* subs.c */
void gamefix_state_loaded(void); /* gamefix.c: damage an original game bug left in a saved RAM, put back */
void menu_state_loaded(void);   /* menu.c: the options menu closed, the pad handed back */
void bios_state_loaded(void);   /* bios.c: re-entry guards */
void hooks_state_loaded(void);  /* hooks.c: hooks left switched off by an interrupted call_original */
void spu_state_loaded(void);    /* spu.c: the output queue emptied */
int cdrom_set_image(const char *cue); /* cdrom.c: another disc, without the lid-open delay */

/* state.c */
void state_init(void);                 /* after the hooks are registered (boot_exe) */
void state_boot(uint32_t entry);       /* runs the game: from its entry point, or from --load-state's state */
void state_vblank(void);               /* main.c, at the start of each vblank: pending loads */
void state_action(int which, int arg); /* hotkeys: 0 save, 1 load, 2 next slot, 3 previous slot */
void state_set_load(const char *path); /* --load-state */
void state_add_save_at(int64_t frame, const char *path); /* --save-state-at */
extern int state_slot;
extern int64_t state_loaded_frame;     /* frame of the last load (-1 none): main.c's replay / record */
