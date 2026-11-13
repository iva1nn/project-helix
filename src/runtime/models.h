/* Remaster character models drawn natively (models.c) by the hardware renderer (gpu_hw.c). */
#ifndef MODELS_H
#define MODELS_H
#include <stdint.h>

typedef struct HwMesh HwMesh;
struct MLook;
/* takes the .hm file's bytes; look: the look fixes' per-vertex light blend (mlook.c), may be 0 */
HwMesh *hw_mesh_create(uint8_t *file, long size, const char *path, const struct MLook *look);
/* the remodel flattened onto a floor plane (q, n) along light, with a soft pool on the blob's quad (4 corners,
 * view space), blurred, darkening the picture (gpu_hw.c) */
void hw_shadow_draw(HwMesh *m, const float *bones, int nbones, float h, float ofx, float ofy, const float *plane,
                    const float *light, float fade, float blur, float strength, const float *quad, float contact);
/* contact shadows: what the character's shadow falls on, in GTE view space (models.c builds it from the room's
 * collision and camera): floor and wall quads near the character, and nearby prop remodels with their bones; the light
 * is directional (light = its direction); the light map spans c +- ext along u, v and dext along the light */
#define SH_MAXQ 48
#define SH_MAXP 3
typedef struct {
    float light[3], c[3], u[3], v[3], ext, dext, fade, soft, bias, strength;
    int nq;
    float q[SH_MAXQ][4][3];
    int np, pn[SH_MAXP];
    HwMesh *pm[SH_MAXP];
    float pb[SH_MAXP][20 * 12];
} ShadowRecv;
void hw_shadow_recv(HwMesh *caster, const float *bones, int nbones, float h, float ofx, float ofy, const ShadowRecv *r);
/* bones: nbones x 3x4 row-major (part-local model space -> GTE view space); colors: lit packet colour per face */
void hw_mesh_draw(HwMesh *m, const float *bones, int nbones, const uint32_t *colors, int ncolors, float h, float ofx,
                  float ofy);
/* a sprite actor's picture (models.c, actors.txt "sprite="): nl (1-2) layers, each a quad (corners: GTE screen x, y and
 * view z; texture coordinates), weights wt (summing to 1), colour x mul (per channel), depth at view z + zback */
void hw_sprite_draw(unsigned tex, int nl, const float (*xyz)[4][3], const float (*uv)[4][2], const float *wt,
                    const float *mul, float zback);

void models_init(const char *remaster_dir);
int models_packet(uint32_t addr);
void models_dma_begin(uint32_t addr);
extern uint32_t models_dma_serial;
extern int models_on;
extern int models_aa;
extern float models_light_radius, models_normal_radius, models_head_radius, models_sheen;
extern float models_ink_sil, models_ink_inner, models_ink_min;
extern float models_shade, models_outline_px, models_outline_depth, models_outline_world;
/* a scene actor's depth anchor (actors.txt "zback=U"): its triangles sort in the pass's depth buffer U view units
 * farther than they are drawn (the surface it is painted on); 0 for every other mesh */
extern float models_zback;
/* a scene actor's painted look (actors.txt "paint=", "tint=", "spec="; NOTES "Cogwheel room: draw order and look"):
 * gpu_hw.c lays the actor's anti-aliased picture over the room through a filter instead of as it is: a blur of the
 * colour and the edge (the plates' softness), a darkening near its own silhouette and holes (the painted recesses), a
 * grade (gain, saturation, contrast about a pivot), a tint toward the room's colour round it and a still grain */
typedef struct {
    int on;
    float blur;            /* gaussian sigma, 8x pixels (colour and coverage together) */
    float ao, ao_r;        /* darkening (0..1) where less than all of a ring of radius ao_r (8x pixels) is covered */
    float gain, sat, con;  /* grade: x gain, saturation about the luma, contrast about 0.18 */
    float tint[3], tint_k; /* mix toward the luma times this colour (0..1 each), amount */
    float grain;           /* amplitude of a still per-pixel noise (levels / 255) */
    float detail;          /* 0 the blurred picture (default), 1 the sharp one, > 1 its detail boosted */
    float shadow[4];       /* a soft drop shadow on the room: strength, offset x, y, softness (8x pixels) */
} ActorPaint;
extern const ActorPaint *models_paint; /* while a scene actor with a painted look is drawn, else 0 */
/* while a scene actor under its view's matte is drawn (actors.txt MATTE): the matte's depth texture and the movie
 * picture's place in VRAM (native x0, y0, x1, y1); the mesh shader drops the actor's pixels behind it; else 0 */
extern unsigned models_matte_tex;
extern float models_matte_rect[4];
/* while a scene actor with "skin=dq" is drawn: dual-quaternion skinning (gpu_hw.c), else the linear blend as always */
extern int models_skin_dq;
/* FE_DBG_SKIN: the file the next mesh draw writes its GPU-skinned vertices to (transform feedback), then "" */
extern char models_skin_capture[1024];
extern float models_spec;              /* the smooth shading's highlight, x (1 = as before; actors.txt "spec=") */
extern char models_scene_look[16];     /* which tagged ACTOR lines draw (actors.txt "look=NAME"; ini scene_look) */
void models_scene_look_cycle(void);    /* Ctrl+F6: the next look named in actors.txt */
#endif
