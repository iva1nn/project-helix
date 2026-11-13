/* Prop inventory (debug only): FE_DBG_PROPS=<log file>, see props.c and NOTES.md "Props inventory". */
#pragma once
#include <stdint.h>

extern int props_dbg;
extern uint32_t models_dma_serial;       /* models.c: GPU linked-list pass counter */
extern uint32_t movie_shown_lba;         /* game_fe2.c: STR sector of the room picture last uploaded */
extern uint32_t cdrom_last_movie_lba;    /* cdrom.c: last STR video sector read */
void props_init(void);                   /* before models_init: hooks the part calls when the models do not */
void props_part(uint32_t rec, int skin); /* a part call has filled its packets (models.c forwards its hooks) */
void props_packet(uint32_t a);           /* the GPU DMA walk reaches the packet at a */
int props_shot_due(void);                /* main.c: dump the screen this frame */
