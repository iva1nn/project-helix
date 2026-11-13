/* Look fixes for the remodels (mlook.c, 5 Nov): the game's lit face colours spread smoothly over the remodel's surface
 * instead of one flat colour per original face, faces the game culled (stale colours) refilled from their neighbours,
 * and the per-part rounding of the game's part translations removed (the head tremble). Presentation only. */
#ifndef MLOOK_H
#define MLOOK_H
#include <stdint.h>

/* model_fix bits (ini [game] model_fix, env FE_MODEL_FIX, Ctrl+F8 toggles all on / off) */
#define MFIX_LIGHT 1   /* lit colours blended over the surface (no flat per-face patches, no saw-tooth edges) */
#define MFIX_STALE 2   /* faces the game did not draw this frame take their neighbours' colour (blinking patches) */
#define MFIX_JOINTS 4  /* each part hangs exactly at its parent's joint (the game rounds every part's translation) */
#define MFIX_STEADY 8  /* the root's translation de-quantised while it rests (whole-body shiver of 1 unit) */
#define MFIX_INK 16    /* outline: silhouette thicker than the inner lines, constant width in screen pixels */
#define MFIX_NORMALS 32 /* cel bands from smoothed shading normals (round band shapes on the face, no facets) */
#define MFIX_SHEEN 64  /* dark fabric catches the light in a thin band at its lit edge (wetsuits read as material) */
#define MFIX_ASSET 128 /* the improved meshes of swap_look.txt (Rain's pilot: RAIN_d5.hm) */
#define MFIX_ALL 255
#define MFIX_DEFAULT (MFIX_ALL & ~MFIX_JOINTS) /* joints: measured worse than STEADY alone (NOTES), kept for tests */
extern int model_fix, model_fix_saved;

typedef struct MLook MLook;
/* hm: the whole .hm file; radius: the blend radius in model units (along the surface). Thread-safe (loader thread). */
/* rn, rh: the shading normals' smoothing radius on the body and on the head */
MLook *mlook_build(const uint8_t *hm, long size, float radius, float rn, float rh);
void mlook_free(MLook *l);
uint32_t mlook_nverts(const MLook *l);
const uint16_t *mlook_vfaces(const MLook *l);  /* nverts x 4 face indices */
const uint8_t *mlook_vweights(const MLook *l); /* nverts x 4 weights (sum 255) */
const float *mlook_vnormals(const MLook *l);   /* nverts x 3 smoothed shading normals */
/* colors[nf]: the lit colour per original face as read from the packets; faces facing away from the camera in this
 * pose (the game culled them: their packets hold an old colour) take the weighted mean of their neighbours */
/* prev: the colours read the frame before (the other packet buffer; 0 = none): a face whose colour jumps between
 * the two buffers holds an old colour in one of them (culled by the game) and is refilled too */
void mlook_fix_colors(const MLook *l, const float *bones, int nbones, uint32_t *colors, int nf, const uint32_t *prev);
#endif
