/* Fear Effect 2: the remaster's character models (<data>/<game>/models), drawn natively.
 *
 * The game draws a character part by part: 0x800275B4 (one call per part; a1 = the part's record) loads the
 * part's camera-space matrix into the GTE (record +0x14 -> +4 -> MATRIX) and runs the hand-written renderer
 * 0x80024F00, which transforms the part's vertices and fills its GPU packets in the part's prims buffer
 * (record +0x1C + 4 * buffer, double-buffered by the word at 0x80085448). 0x800276FC does the same for the skin
 * part (vertices blended between parts). The port knows each part's exact transform here, so no pose fitting is
 * needed: a part is identified once by its packets (code + UV deltas, the remaster's .fp tables), its matrix is
 * kept per buffer, and when the GPU reaches the first packet of a swapped model the whole host mesh (.hm) is
 * skinned to the parts and drawn by the hardware renderer at that point of the ordering table. The model's own
 * packets are left out of the upscaled picture (the software VRAM keeps them). */
#include <stdlib.h>
#include <math.h>
#include <SDL2/SDL.h>
#include "hw.h"
#include "models.h"
#include "props.h"
#include "mlook.h"

#define MAXPARTS 20
#define MAXMODELS 512 /* FE2: 15 playables + 20 enemies + 14 weapons (swap.txt) + 34 props (swap_props.txt), plus the
                       * scene actors' costume variants (a party crowd: one per archetype; was 128 before 7 Nov) */
#define MAXINST 32
#define MAXMATS 8

typedef struct { uint32_t off; uint16_t len; uint8_t code, nv; int16_t duv[6]; } MFace;
typedef struct {
    int parent, joint[3];
    float jw[3];
    int nverts, nfaces;
    int16_t (*verts)[3];
    MFace *faces;
    uint32_t prims_len, prims_a, prims_b, face_base;
} MPart;
typedef struct {
    char name[32];
    int nparts, total_faces;
    MPart parts[MAXPARTS];
    char hm_file[1400];
    char mats[MAXMATS][16]; /* the RMD's material (TIM) names, "mat" lines: tell twins with one mesh apart */
    int nmats;
    int shadow; /* the blob under every character (SHADOW.RMD): replaced by the character's soft shadow */
    int variant; /* a scene actor's costume variant (ACTOR table "RAIN=RAIN_uw1.hm"): the table's rig with another
                  * mesh, drawn only as a scene actor, never matched to the game's packets */
    HwMesh *mesh;
    MLook *look; /* look fixes (mlook.c): built with the mesh on the loader thread */
    int alt; /* MFIX_ASSET: the model (a variant, same rig) with the improved mesh of swap_look.txt, 0 = none */
    uint32_t keep_parts; /* skin parts the mesh cannot pose (mesh_ready): their original packets stay drawn */
    volatile int mesh_state; /* 0 not asked, 1 reading, 2 bytes ready, 3 created, 4 failed */
    uint8_t *file;
    long file_size;
} MModel;

/* one actor drawn with a model (see record_part for how parts are grouped into actors) */
typedef struct {
    int model;
    uint32_t key;             /* the first record seen (log only) */
    uint32_t rec[MAXPARTS];   /* the part records it was drawn with */
    uint32_t P[2][MAXPARTS];  /* each part's packet buffer per buffer index (0 = not seen) */
    uint64_t stamp[2];      /* frame of the last pose per buffer */
    uint32_t dma_drawn[2];  /* GPU pass in which the mesh was drawn */
    float bones[2][MAXPARTS][12];
    uint8_t has[2][MAXPARTS];
    float rel[MAXPARTS][12];  /* each part relative to its parent, last time both were drawn */
    uint8_t rel_ok[MAXPARTS];
    int8_t rel_to[MAXPARTS];  /* the part rel[] is relative to (the parent, or the reference for a root) */
    float h[2], ofx[2], ofy[2];
    uint64_t link_frame[2]; /* a shadow: the character it lies under (instance id, 0 = none), found once per frame */
    int link[2];
    float steady[MAXPARTS][3]; /* MFIX_STEADY: each part's pivot, de-quantised (view space) */
    uint64_t steady_frame;      /* frame of the last update (0 = none) */
    uint32_t prevcol[2048];     /* MFIX_STALE: the faces' colours as read the last drawn frame */
    uint64_t prevcol_frame;
    uint32_t pose_last;         /* its newest pose in the pose store (60 fps in-betweens: each pose links to the next) */
} Inst;

static MModel models[MAXMODELS];
static int nmodels;
static void load_actors(void);
static void load_cam_tracks(void);
static Inst insts[MAXINST];
static uint32_t last_pkt[MAXINST + 1]; /* per actor id: its last packet in the current GPU pass */
static uint32_t last_serial;
static int ninst;
static uint8_t *owner;          /* per RAM word: instance + 1 whose packets live there */
static char host_dir[1024];
static char game_dir[1024]; /* the data folder's game folder (or a remaster work folder): coll/<X>.txt */
int models_on = 1;
static int dbg;
static int idx_part = -1, idx_skin = -1;
static uint32_t cur_a0; /* a0 of the part call being recorded (the actor object) */
static uint32_t buf_word = 0x80085448u; /* double-buffer index (0/1) */
uint32_t models_dma_serial;
int models_aa = 4;
float models_light_radius = 14.0f; /* the lit colours' blend radius along the surface, model units (LIGHT_RADIUS) */
float models_sheen = 0.16f; /* MFIX_SHEEN band strength (SHEEN) */
/* MFIX_INK: silhouette and inner line width as shares of OUTLINE's, minimum width in screen pixels (INK) */
float models_ink_sil = 1.6f, models_ink_inner = 0.8f, models_ink_min = 1.5f;
float models_normal_radius = 6.0f, models_head_radius = 12.0f; /* shading normals' smoothing (NORMAL_RADIUS body head) */
float models_shade = 0.62f, models_outline_px = 0.3f, models_outline_depth = 8.0f, models_outline_world = 1.5f;
/* soft shadows (swap_props.txt SHADOW_* lines): darkening, blur radius (world units), light tilt away from the
 * camera, fade height (share of the character's height) */
float models_shadow = 1.3f, models_shadow_blur = 18.0f, models_shadow_tilt = 0.7f, models_shadow_fade = 0.8f;
float models_shadow_contact = 0.0f; /* the soft pool under the feet, on the blob's own quad (SHADOW_CONTACT) */
int models_shadow_geom = 0; /* SHADOW_GEOMETRY 1: contact shadows on the room's collision (off: collision walls are often invisible barriers, 4 Nov) */
float models_shadow_inset = 70.0f; /* collision walls stand inside the visible walls by about this (SHADOW_WALL_INSET) */

static const uint8_t *uv_offsets(uint8_t code, int *n) {
    static const uint8_t FT3[3] = {12, 20, 28}, FT4[4] = {12, 20, 28, 36}, GT3[3] = {12, 24, 36}, GT4[4] = {12, 24, 36, 48};
    switch (code & 0xFC) {
    case 0x24: *n = 3; return FT3;
    case 0x2C: *n = 4; return FT4;
    case 0x34: *n = 3; return GT3;
    case 0x3C: *n = 4; return GT4;
    default: *n = 0; return 0;
    }
}

static int packet_matches(uint32_t addr, const MFace *f) {
    if (mem_r8(addr + 7) != f->code) return 0;
    int n;
    const uint8_t *uv = uv_offsets(f->code, &n);
    if (!uv) return 1;
    int u0 = mem_r8(addr + uv[0]), v0 = mem_r8(addr + uv[0] + 1);
    for (int k = 1; k < n; k++)
        if ((int)mem_r8(addr + uv[k]) - u0 != f->duv[2 * (k - 1)] || (int)mem_r8(addr + uv[k] + 1) - v0 != f->duv[2 * (k - 1) + 1])
            return 0;
    return 1;
}

/* every face's packet (at most 24 sampled) looks like the table's */
static int part_matches(uint32_t P, const MPart *p) {
    if (p->nfaces < 1) return 0;
    int step = p->nfaces / 24 + 1;
    for (int i = 0; i < p->nfaces; i += step)
        if (!packet_matches(P + p->faces[i].off, &p->faces[i])) return 0;
    return 1;
}

static int load_table(const char *path, MModel *m) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[512];
    MPart *p = 0;
    int nv = 0, nf = 0;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        if (!strncmp(line, "model ", 6)) {
            sscanf(line + 6, "%31s", m->name);
        } else if (!strncmp(line, "mat ", 4)) {
            if (m->nmats < MAXMATS) sscanf(line + 4, "%15s", m->mats[m->nmats++]);
        } else if (!strncmp(line, "part ", 5)) {
            if (m->nparts >= MAXPARTS) break;
            p = &m->parts[m->nparts++];
            unsigned idx, nverts, nfaces, plen, seam, pool, pa, pb;
            char word[16];
            sscanf(line + 5, "%u %15s %d %d %d %d %u %u %u %u %u %u %u", &idx, word, &p->parent, &p->joint[0], &p->joint[1],
                   &p->joint[2], &nverts, &nfaces, &plen, &seam, &pool, &pa, &pb);
            p->verts = calloc(nverts + 1, sizeof *p->verts);
            p->faces = calloc(nfaces + 1, sizeof *p->faces);
            p->prims_len = plen; p->prims_a = pa; p->prims_b = pb;
            nv = nf = 0;
            (void)nv;
        } else if (line[0] == 'v' && line[1] == ' ' && p) {
            int x, y, z;
            sscanf(line + 2, "%d %d %d", &x, &y, &z);
            p->verts[p->nverts][0] = (int16_t)x; p->verts[p->nverts][1] = (int16_t)y; p->verts[p->nverts][2] = (int16_t)z;
            p->nverts++;
        } else if (line[0] == 'f' && line[1] == ' ' && p) {
            MFace *fc = &p->faces[p->nfaces++];
            unsigned off, len, code, n;
            int d[6] = {0};
            int c = 0;
            sscanf(line + 2, "%u %u %u %u%n", &off, &len, &code, &n, &c);
            const char *s = line + 2 + c;
            for (unsigned k = 0; k < 2 * (n - 1) && k < 6; k++) d[k] = (int)strtol(s, (char **)&s, 10);
            fc->off = off; fc->len = (uint16_t)len; fc->code = (uint8_t)code; fc->nv = (uint8_t)n;
            for (int k = 0; k < 6; k++) fc->duv[k] = (int16_t)d[k];
            (void)nf;
        }
    }
    fclose(f);
    if (!m->nparts) return 0;
    int fb = 0;
    for (int i = 0; i < m->nparts; i++) {
        m->parts[i].face_base = (uint32_t)fb;
        fb += m->parts[i].nfaces;
        float j[3] = {0, 0, 0};
        for (int k = i; k >= 0 && k < m->nparts; k = m->parts[k].parent)
            for (int a = 0; a < 3; a++) j[a] += (float)m->parts[k].joint[a];
        memcpy(m->parts[i].jw, j, sizeof j);
    }
    m->total_faces = fb;
    return 1;
}

/* swap.txt: "<table> <file.hm>" lines, OUTLINE, SHADE; swap_props.txt (the props, tools/props_rework.py): the
 * same lines, read by the port only (the remaster's emulator fork reads swap.txt) */
static void load_swaps(const char *file) {
    char p[1200];
    snprintf(p, sizeof p, "%s/%s", host_dir, file);
    FILE *f = fopen(p, "r");
    if (!f) { LOG("models: no %s\n", p); return; }
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char a[64] = "", b[300] = "";
        if (line[0] == '#' || sscanf(line, "%63s %299s", a, b) < 1) continue;
        if (!strcmp(a, "OUTLINE")) {
            sscanf(line + 7, "%f %f %f", &models_outline_px, &models_outline_depth, &models_outline_world);
            continue;
        }
        if (!strcmp(a, "AA")) { models_aa = atoi(b); continue; }
        if (!strcmp(a, "LIGHT_RADIUS")) { models_light_radius = (float)atof(b); continue; }
        if (!strcmp(a, "SHEEN")) { models_sheen = (float)atof(b); continue; }
        if (!strcmp(a, "INK")) { sscanf(line + 3, "%f %f %f", &models_ink_sil, &models_ink_inner, &models_ink_min); continue; }
        if (!strcmp(a, "NORMAL_RADIUS")) { sscanf(line + 13, "%f %f", &models_normal_radius, &models_head_radius); continue; }
        if (!strcmp(a, "SHADOW_STRENGTH")) { models_shadow = (float)atof(b); continue; }
        if (!strcmp(a, "SHADOW_BLUR")) { models_shadow_blur = (float)atof(b); continue; }
        if (!strcmp(a, "SHADOW_TILT")) { models_shadow_tilt = (float)atof(b); continue; }
        if (!strcmp(a, "SHADOW_FADE")) { models_shadow_fade = (float)atof(b); continue; }
        if (!strcmp(a, "SHADOW_WALL_INSET")) { models_shadow_inset = (float)atof(b); continue; }
        if (!strcmp(a, "SHADOW_GEOMETRY")) { models_shadow_geom = atoi(b); continue; }
        if (!strcmp(a, "SHADOW_CONTACT")) { models_shadow_contact = (float)atof(b); continue; }
        if (!strcmp(a, "SHADE")) {
            if (strstr(line, "smooth")) { float v = 0.45f; sscanf(strstr(line, "smooth") + 6, "%f", &v); models_shade = -v; }
            else sscanf(line + 5, "%f", &models_shade);
            continue;
        }
        int shadow = strstr(b, ".shadow") != 0; /* "SHADOW props/SHADOW.shadow": the blob's table, no mesh */
        if (!b[0] || !(strstr(b, ".hm") || shadow) || nmodels >= MAXMODELS) continue;
        /* the part table <NAME>.fp sits next to the mesh ("weapons/ASSRIFE_d1.hm" -> weapons/ASSRIFE.fp) */
        char dir[300] = "";
        const char *slash = strrchr(b, '/');
        if (slash) snprintf(dir, sizeof dir, "%.*s", (int)(slash - b + 1), b);
        int i;
        for (i = 0; i < nmodels; i++)
            if (!strcmp(models[i].name, a)) break;
        if (i == nmodels) {
            snprintf(p, sizeof p, "%s/%s%s.fp", host_dir, dir, a);
            memset(&models[nmodels], 0, sizeof models[nmodels]);
            if (!load_table(p, &models[nmodels])) { LOG("models: no part table %s\n", p); continue; }
            nmodels++;
        }
        if (shadow) models[i].shadow = 1;
        else snprintf(models[i].hm_file, sizeof models[i].hm_file, "%s/%s", host_dir, b);
    }
    fclose(f);
}

/* swap_look.txt (models folder, optional): "<table> <file.hm>" lines, an improved mesh of a table with the same rig
 * and vertices (tools/model_look.py), drawn instead of swap.txt's while model_fix has MFIX_ASSET (Ctrl+F8 compares).
 * A separate file, so the data folder's swap.txt (hard-linked into the packages) stays as it is. A line whose first
 * word is a .hm file ("RAIN_uw1.hm RAIN_uw2.hm") names the improved mesh of a scene actor's costume variant
 * (actors.txt "RAIN=RAIN_uw1.hm"); load_actors() takes it when it makes that variant. */
static void request_mesh(MModel *m);
static int mesh_ready(MModel *m);
#define MAXLOOKFILES 16
static char look_files[MAXLOOKFILES][2][300];
static int nlook_files;
/* models[i] gets the improved mesh <host_dir>/<b> as its alt */
static void add_look(int i, const char *a, const char *b) {
    if (nmodels >= MAXMODELS) { LOG("models: swap_look.txt: no room for %s\n", b); return; }
    char file[1400];
    snprintf(file, sizeof file, "%s/%s", host_dir, b);
    FILE *hf = fopen(file, "rb");
    if (!hf) { LOG("models: swap_look.txt: no %s\n", file); return; }
    fclose(hf);
    int v = nmodels++;
    models[v] = models[i]; /* parts shared by value, as the scene actors' variants */
    models[v].variant = 1;
    models[v].mesh = 0;
    models[v].look = 0;
    models[v].mesh_state = 0;
    models[v].file = 0;
    models[v].alt = 0;
    snprintf(models[v].hm_file, sizeof models[v].hm_file, "%s", file);
    models[i].alt = v;
    LOG("models: %s improved mesh %s (model_fix %d)\n", a, b, MFIX_ASSET);
}
static void load_look_swaps(void) {
    char p[1200];
    snprintf(p, sizeof p, "%s/swap_look.txt", host_dir);
    FILE *f = fopen(p, "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char a[300] = "", b[300] = "";
        if (line[0] == '#' || sscanf(line, "%299s %299s", a, b) < 2 || !strstr(b, ".hm")) continue;
        if (strstr(a, ".hm")) { /* a costume variant's: kept for load_actors() */
            if (nlook_files < MAXLOOKFILES) {
                snprintf(look_files[nlook_files][0], sizeof look_files[0][0], "%s", a);
                snprintf(look_files[nlook_files][1], sizeof look_files[0][1], "%s", b);
                nlook_files++;
            }
            continue;
        }
        int i;
        for (i = 0; i < nmodels; i++)
            if (!models[i].variant && !strcmp(models[i].name, a)) break;
        if (i == nmodels) { LOG("models: swap_look.txt: no table %s\n", a); continue; }
        add_look(i, a, b);
    }
    fclose(f);
}

/* the mesh to draw for a model: the improved one while MFIX_ASSET is on and it is loaded (asked for here) */
static HwMesh *draw_mesh(MModel *m) {
    if (m->alt && (model_fix & MFIX_ASSET)) {
        MModel *a = &models[m->alt];
        request_mesh(a);
        if (mesh_ready(a)) return a->mesh;
    }
    return m->mesh;
}

/* ------------------------------------------------------------------ identification */

#define CACHE 512
static struct { uint32_t P, hdr, base; int16_t model, part; uint64_t when; } cache[CACHE];
/* Faceless parts (joints with vertices but no polygons: MSER's knees, FE1 soldiers' hips; 18 tables in the two
 * games) have no packets of their own: their buffer pointer is the next part's. Before 4 Nov the cache was keyed by
 * the buffer alone, so the faceless call (the part header differs) evicted the real part's entry and left a
 * "nothing here" entry for 60 frames: that part (MSER's feet) went unidentified and fell back to a rest pose, and
 * the faceless part itself was never posed. Now the cache is keyed by buffer and part header, and a faceless part is
 * identified by its vertices in RAM. FE_MODELS_OLD_IDENT=1: the old way, for comparison. */
static int old_ident = -1;

static uint32_t prims_of(const MPart *p, int buf) { return buf ? p->prims_b : p->prims_a; }

/* Which model and part draws its packets at P (buffer buf), and where that model's RMD lies. Parts shared
 * between costumes (Hana's head in MARA, MAR3 and MAR4) match several tables: the table whose other parts
 * are found at the same RMD base wins. */
static void request_mesh(MModel *m);

/* the part's own vertices in RAM (RMD part header +0x3C -> int16 x, y, z, pad) are the table's: tells mirrored
 * parts apart, whose packets look the same (a soldier's left and right shin) */
static int verts_match(uint32_t mesh, const MPart *p) {
    if (!p->nverts) return 1;
    if ((mesh >> 24) != 0x80) return 0;
    uint32_t v = mem_r32(mesh + 0x3C);
    if ((v >> 24) != 0x80) return 0;
    int n = p->nverts < 6 ? p->nverts : 6;
    for (int i = 0; i < n; i++)
        for (int a = 0; a < 3; a++)
            if ((int16_t)mem_r16(v + 8 * i + 2 * a) != p->verts[i][a]) return 0;
    return 1;
}

/* Twins: tables whose parts have the same packets and the same vertices (FE2: the bio-suit guard BIOF and Rain's
 * BioSuit RAI4 share 13 parts, both on Disc 1; the enemy MECH and Glas's mech GLA2 share 10, both on Disc 4; GHST
 * and DMON share 14; F:\fe_port\third_party\fe2_remaster\MODELS.md "Enemies and NPCs"). The packet layout cannot tell them apart (an
 * enemy's packets are not at the file's offsets), so the RMD itself does: the part header the game passes lies in
 * the loaded RMD, whose file header (u32 1, 1, part count, material count) and part offset table (0x60, 8 bytes
 * per part) stay in RAM. rmd_base_of() walks back from the header to that RMD's start; the twin whose every part
 * has its vertices at that RMD's part headers wins. 0 = not found (the first table found stays the answer). */
static uint32_t rmd_entry(uint32_t A, int r) {
    uint32_t e = mem_r32(A + 0x60 + 8 * r);
    return (e >> 24) == 0x80 ? e : A + e; /* a file offset, or a pointer if the game relocated the table */
}

static int rmd_has(uint32_t A, uint32_t hdr) {
    if (mem_r32(A) != 1 || mem_r32(A + 4) != 1) return 0;
    uint32_t np = mem_r32(A + 8);
    if (np < 1 || np > MAXPARTS || mem_r32(A + 12) > 256) return 0;
    for (uint32_t r = 0; r < np; r++)
        if (rmd_entry(A, (int)r) == hdr) return 1;
    return 0;
}

/* A one-part RMD's header starts 1, 1, 1, <materials>: 4 bytes in it reads 1, 1, <materials>, ... and, the part table
 * entries being (file offset, pointer) pairs, the shifted table's first "entry" is the part's pointer, so the scan
 * down from the part header met that false start first (5 Nov: area A's KCRDYLW, the Fixer Chip, read 2 parts and
 * 0x68 materials there, no twin scored and the first key-card table, BIOKEY1, stayed). Step back while the earlier
 * start holds too, as props.c does. */
static uint32_t rmd_base_of(uint32_t hdr) {
    if ((hdr >> 24) != 0x80) return 0;
    for (uint32_t A = hdr & ~3u; A + 0x20000 > hdr && A >= 0x80000000u; A -= 4) {
        if (!rmd_has(A, hdr)) continue;
        while (!old_ident && rmd_has(A - 4, hdr)) A -= 4;
        return A;
    }
    return 0;
}

/* the RMD's material table (header +0x10: 32 bytes per material, the TIM name first) names the table's TIMs: twins
 * with one mesh and other textures (FE2's key cards KCRDYLW / BIOKEY1 / SECKEY1) differ only there */
static int rmd_mats_match(uint32_t A, const MModel *m) {
    if (!m->nmats || (int)mem_r32(A + 12) != m->nmats) return 0;
    uint32_t t = mem_r32(A + 16);
    t = (t >> 24) == 0x80 ? t : A + t;
    for (int k = 0; k < m->nmats; k++)
        for (int c = 0; c < 16; c++) {
            int a = (int)mem_r8(t + 32 * k + c), b = (unsigned char)m->mats[k][c];
            if (a >= 'a' && a <= 'z') a -= 32;
            if (b >= 'a' && b <= 'z') b -= 32;
            if (a != b) return 0;
            if (!a) break;
        }
    return 1;
}

static int rmd_score(uint32_t A, const MModel *m) {
    if ((int)mem_r32(A + 8) != m->nparts) return 0;
    int s = 0;
    for (int r = 0; r < m->nparts; r++)
        if (m->parts[r].nverts && verts_match(rmd_entry(A, r), &m->parts[r])) s += 2;
    return s + rmd_mats_match(A, m);
}

static int identify(uint32_t P, int buf, uint32_t mesh, int skin, int *part, uint32_t *base) {
    if (old_ident < 0) old_ident = getenv("FE_MODELS_OLD_IDENT") && atoi(getenv("FE_MODELS_OLD_IDENT")) != 0;
    unsigned h = (P >> 2) * 2654435761u % CACHE;
    for (int k = 0; k < 8; k++) {
        unsigned i = (h + k) % CACHE;
        if (cache[i].P != P || (!old_ident && cache[i].hdr != mesh)) continue;
        if (cache[i].model >= 0) {
            const MPart *p = &models[cache[i].model].parts[cache[i].part];
            if (!p->nfaces ? verts_match(mesh, p)
                           : packet_matches(P + p->faces[0].off, &p->faces[0]) && (skin || verts_match(mesh, p))) {
                *part = cache[i].part; *base = cache[i].base;
                return cache[i].model;
            }
        } else if (frame_count - cache[i].when < 60) {
            return -1;
        }
        cache[i].P = 0;
        break;
    }
    int found = -1, fpart = -1, best = 0;
    uint32_t fbase = 0;
    int tie_m[8], tie_q[8], nties = 0; /* other tables matching as well as the one found (twins) */
    uint32_t tie_b[8];
    for (int m = 0; m < nmodels; m++)
        for (int q = 0; q < models[m].nparts && !models[m].variant; q++) {
            const MPart *pq = &models[m].parts[q];
            if (skin ? pq->nverts != 0 : (pq->nverts == 0 || !verts_match(mesh, pq))) continue;
            if (!part_matches(P, pq)) continue;
            uint32_t bs = P - prims_of(pq, buf);
            int score = 1; /* among twins (the same part in two costumes' tables) the table found at one RMD base wins */
            for (int r = 0; r < models[m].nparts; r++)
                score += part_matches(bs + prims_of(&models[m].parts[r], buf), &models[m].parts[r]);
            if (score > best) { best = score; found = m; fpart = q; fbase = bs; nties = 0; }
            else if (score == best && m != found && nties < 8) { tie_m[nties] = m; tie_q[nties] = q; tie_b[nties++] = bs; }
        }
    /* a faceless part: no packets to match, its own vertices in RAM name it (ties between tables: the twin rule
     * below, by the RMD in RAM). Every table is searched: until 14 Nov the loop stopped at the first table that
     * matched, so a twin never became a tie and the first table took the part (FE1 Disc 4: Glas is GLAZ, but his
     * faceless thigh roots 8 and 11 went to GLAS, two GLAS actors of one part each drew a second, offset Glas) */
    if (found < 0 && !skin && !old_ident)
        for (int m = 0; m < nmodels; m++)
            for (int q = 0; q < models[m].nparts && !models[m].variant; q++) {
                const MPart *pq = &models[m].parts[q];
                if (pq->nfaces || !pq->nverts || !verts_match(mesh, pq)) continue;
                if (found < 0) { found = m; fpart = q; fbase = 0; best = 1; }
                else if (m != found && nties < 8) { tie_m[nties] = m; tie_q[nties] = q; tie_b[nties++] = 0; }
            }
    if (dbg > 0 && found >= 0) { /* FE_DBG_MODELS: does the twin check find this part's RMD (any part, twin or not)? */
        uint32_t A = rmd_base_of(mesh);
        int nv = 0;
        for (int r = 0; r < models[found].nparts; r++) nv += models[found].parts[r].nverts != 0;
        LOG("models: RMD check %s part %d header %08X: RMD %08X, %d of %d parts' vertices there\n", models[found].name,
            fpart, mesh, A, A ? rmd_score(A, &models[found]) : 0, nv);
    }
    if (nties) {
        uint32_t A = rmd_base_of(mesh);
        int s0 = A ? rmd_score(A, &models[found]) : 0, was = found;
        for (int i = 0; A && i < nties; i++) {
            int s = rmd_score(A, &models[tie_m[i]]);
            if (s > s0) { s0 = s; found = tie_m[i]; fpart = tie_q[i]; fbase = tie_b[i]; }
        }
        static int twin_logs;
        if (twin_logs < 40) {
            twin_logs++;
            LOG("models: twin part at %06X (header %08X): %s, RMD %08X -> %s\n", P, mesh, models[was].name, A,
                models[found].name);
        }
    }
    unsigned slot = h;
    for (int k = 0; k < 8; k++) if (!cache[(h + k) % CACHE].P) { slot = (h + k) % CACHE; break; }
    cache[slot].P = P; cache[slot].hdr = mesh; cache[slot].model = (int16_t)found; cache[slot].part = (int16_t)fpart;
    cache[slot].base = fbase; cache[slot].when = frame_count;
    *part = fpart; *base = fbase;
    return found;
}

/* the actor that used this part record before (records stay with an actor while it exists) */
static Inst *find_by_rec(int model, int part, uint32_t rec) {
    for (int i = 0; i < ninst; i++)
        if (insts[i].model == model && insts[i].rec[part] == rec) return &insts[i];
    return 0;
}

static Inst *new_instance(int model, uint32_t key) {
    Inst *free_i = 0;
    for (int i = 0; i < ninst; i++)
        if (frame_count - insts[i].stamp[0] > 120 && frame_count - insts[i].stamp[1] > 120 && !free_i) free_i = &insts[i];
    if (!free_i) {
        if (ninst >= MAXINST) return 0;
        free_i = &insts[ninst++];
    } else {
        /* reuse: forget the old copy's packet areas */
        int id = (int)(free_i - insts) + 1;
        for (uint32_t a = 0; a < 0x200000 / 4; a++) if (owner[a] == id) owner[a] = 0;
    }
    memset(free_i, 0, sizeof *free_i);
    free_i->model = model; free_i->key = key;
    LOG("models: %s, actor %08X\n", models[model].name, key);
    return free_i;
}

/* the GTE matrix of the part (MATRIX: 3x3 int16 rotation, 3 int32 translation) */
static void read_matrix(uint32_t a, float *r, float *t) {
    for (int i = 0; i < 9; i++) r[i] = (float)(int16_t)mem_r16(a + 2 * i) / 4096.0f;
    for (int i = 0; i < 3; i++) t[i] = (float)(int32_t)mem_r32(a + 20 + 4 * i);
}

/* the current run of parts (record_part) */
static Inst *run_inst;
static int run_model = -1, run_part;
static uint32_t run_buf;
static uint64_t run_frame;

static void record_part(uint32_t rec, int skin) {
    uint32_t buf = mem_r32(buf_word) & 1;
    uint32_t P = mem_r32(rec + 0x1C + 4 * buf) & 0x1FFFFF;
    int part = -1;
    uint32_t base = 0;
    int m = identify(P, (int)buf, mem_r32(rec + 0x18), skin, &part, &base);
    if (dbg > 0) {
        dbg--;
        uint32_t mesh = mem_r32(rec + 0x18), mat = mem_r32(mem_r32(rec + 0x14) + 4);
        float r[9], t[3];
        read_matrix(mat, r, t);
        LOG("models: %s rec %08X buf %u P %06X -> %s part %d  mat %08X r [%.3f %.3f %.3f / %.3f %.3f %.3f / %.3f %.3f %.3f] t %.0f %.0f %.0f  H %u OF %d %d\n",
            skin ? "skin" : "part", rec, buf, P, m >= 0 ? models[m].name : "?", part, mat, r[0], r[1], r[2], r[3], r[4], r[5],
            r[6], r[7], r[8], t[0], t[1], t[2], gte_read_ctrl(26) & 0xFFFF, (int32_t)gte_read_ctrl(24) >> 16,
            (int32_t)gte_read_ctrl(25) >> 16);
        if (m >= 0 && !skin && dbg > 40) {
            LOG("  mesh %08X:", mesh);
            for (int i = 0; i < 24; i++) LOG(" %08X", mem_r32(mesh + 4 * i));
            LOG("\n  table v0 %d %d %d v1 %d %d %d\n", models[m].parts[part].verts[0][0], models[m].parts[part].verts[0][1],
                models[m].parts[part].verts[0][2], models[m].parts[part].verts[1][0], models[m].parts[part].verts[1][1],
                models[m].parts[part].verts[1][2]);
        }
    }
    if (m < 0) return;
    MModel *mm = &models[m];
    MPart *p = &mm->parts[part];
    (void)base;
    if (getenv("FE_DBG_REC")) {
        static int n;
        if (strcmp(mm->name, getenv("FE_DBG_REC")) == 0 && n++ < 80)
            LOG("rec %s part %d rec %08X a0 %08X P %06X buf %u frame %llu\n", mm->name, part, rec, cur_a0, P, buf,
                (unsigned long long)frame_count);
    }
    /* Actors: the game draws an actor's parts as one descending run (13 .. 0), then the skin parts of all
     * actors. A part continues the current run when the model and buffer are the same and its index is lower;
     * otherwise a new run starts, matched to an actor by its part records. (Records are neither consecutive
     * nor keyed by the call's a0, a shared draw list: FE1's second soldier, 2 Nov.) */
    Inst *in = 0;
    if (skin) {
        in = find_by_rec(m, part, rec);
        /* the skin's buffer follows its actor's part buffers in RAM: the actor drawn this frame with a part buffer
         * ending where the skin's starts (a faceless part's buffer, length 0, starts there). Before 4 Nov the skin
         * went to the last run, or to an actor of its own when another model was drawn in between: the actor's skin
         * faces then took stale colours. */
        for (int i = 0; i < ninst && !in && !old_ident; i++) {
            Inst *c = &insts[i];
            if (c->model != m || c->stamp[buf] != frame_count) continue;
            for (int q = 0; q < mm->nparts; q++)
                if (q != part && c->P[buf][q] && c->P[buf][q] + mm->parts[q].prims_len == P) { in = c; break; }
        }
        if (!in && run_inst && run_model == m) in = run_inst;
    } else {
        if (run_inst && run_model == m && run_buf == buf && run_frame == frame_count && part < run_part) in = run_inst;
        else in = find_by_rec(m, part, rec);
        run_part = part;
    }
    if (!in) in = new_instance(m, rec);
    if (!in) return;
    if (!skin) { run_inst = in; run_model = m; run_buf = buf; run_frame = frame_count; }
    in->rec[part] = rec;
    if (!mm->shadow) request_mesh(mm);
    if (in->stamp[buf] != frame_count) { memset(in->has[buf], 0, sizeof in->has[buf]); in->stamp[buf] = frame_count; }
    int id = (int)(in - insts) + 1;
    uint32_t old = in->P[buf][part];
    if (old && old != P) /* the part's buffer moved: release the old area */
        for (uint32_t a = old; a < old + p->prims_len && a < 0x200000; a += 4) if (owner[a >> 2] == id) owner[a >> 2] = 0;
    in->P[buf][part] = P;
    for (uint32_t a = P; a < P + p->prims_len && a < 0x200000; a += 4) owner[a >> 2] = (uint8_t)id;
    if (skin) return;
    float r[9], t[3];
    read_matrix(mem_r32(mem_r32(rec + 0x14) + 4), r, t);
    float *bm = in->bones[buf][part];
    for (int i = 0; i < 3; i++) {
        bm[i * 4 + 0] = r[i * 3 + 0];
        bm[i * 4 + 1] = r[i * 3 + 1];
        bm[i * 4 + 2] = r[i * 3 + 2];
        bm[i * 4 + 3] = t[i] - (r[i * 3] * p->jw[0] + r[i * 3 + 1] * p->jw[1] + r[i * 3 + 2] * p->jw[2]);
    }
    in->has[buf][part] = 1;
    in->h[buf] = (float)(gte_read_ctrl(26) & 0xFFFF);
    in->ofx[buf] = (float)(int32_t)gte_read_ctrl(24) / 65536.0f;
    in->ofy[buf] = (float)(int32_t)gte_read_ctrl(25) / 65536.0f;
}

static int part_hook(void) {
    uint32_t rec = R5;
    cur_a0 = R4;
    call_original(idx_part);
    if (props_dbg) props_part(rec, 0);
    if (models_on) record_part(rec, 0);
    return 1;
}

static int skin_hook(void) {
    uint32_t rec = R5;
    cur_a0 = R4;
    call_original(idx_skin);
    if (props_dbg) props_part(rec, 1);
    if (models_on) record_part(rec, 1);
    return 1;
}

void models_init(const char *remaster_dir) {
    /* FE2: part 0x800275B4, skin part 0x800276FC, buffer word 0x80085448; FE1 (same record layout, no
     * lighting call): 0x80025EDC, 0x80025FF0, 0x80087188 */
    int fe1 = !strcmp(game_serial, "SLUS-00920") || !strncmp(game_serial, "SLUS-0105", 9);
    if (!fe1 && strncmp(game_serial, "SLUS-0126", 9) && strncmp(game_serial, "SLUS-0127", 9)) return;
    /* the collision is also the directional controls' wall guard (coll_wall_guard), with either renderer */
    if (remaster_dir) snprintf(game_dir, sizeof game_dir, "%s", remaster_dir);
    if (!remaster_dir || !hw_active) return;
    uint32_t part_fn = fe1 ? 0x80025EDCu : 0x800275B4u, skin_fn = fe1 ? 0x80025FF0u : 0x800276FCu;
    buf_word = fe1 ? 0x80087188u : 0x80085448u;
    /* the data folder: <game>/models; a remaster work folder (development): models/host */
    snprintf(host_dir, sizeof host_dir, "%s/models/swap.txt", remaster_dir);
    FILE *t = fopen(host_dir, "r");
    if (t) fclose(t);
    snprintf(host_dir, sizeof host_dir, t ? "%s/models" : "%s/models/host", remaster_dir);
    snprintf(game_dir, sizeof game_dir, "%s", remaster_dir);
    load_swaps("swap.txt"); /* the tables named in swap.txt */
    load_swaps("swap_props.txt");
    load_look_swaps();
    load_actors();
    if (getenv("FE_NO_CAMFIX")) models_camfix = 0;
    load_cam_tracks();
    if (!nmodels) { LOG("models: no tables in %s\n", host_dir); return; }
    owner = calloc(0x200000 / 4, 1);
    if (getenv("FE_DBG_MODELS")) dbg = atoi(getenv("FE_DBG_MODELS"));
    if (getenv("FE_NO_MODELS")) models_on = 0;
    if (getenv("FE_MODEL_FIX")) model_fix = (int)strtol(getenv("FE_MODEL_FIX"), 0, 0); /* look fixes, mlook.h bits */
    model_fix_saved = model_fix ? model_fix : MFIX_DEFAULT;
    LOG("models: look fixes %d (model_fix)\n", model_fix);
    idx_part = hook_index(part_fn);
    idx_skin = hook_index(skin_fn);
    hook_register(part_fn, part_hook);
    hook_register(skin_fn, skin_hook);
    int swapped = 0;
    for (int i = 0; i < nmodels; i++) swapped += models[i].hm_file[0] != 0;
    LOG("models: %d tables, %d with a remaster mesh (%s)\n", nmodels, swapped, host_dir);
}

/* ------------------------------------------------------------------ drawing (from the GPU DMA walk) */

/* 3x4 row-major affine helpers */
static void affine_mul(const float *a, const float *b, float *o) {
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 4; c++)
            o[r * 4 + c] = a[r * 4] * b[c] + a[r * 4 + 1] * b[4 + c] + a[r * 4 + 2] * b[8 + c];
        o[r * 4 + 3] += a[r * 4 + 3];
    }
}

static int affine_inverse(const float *m, float *o) {
    float a = m[0], b = m[1], c = m[2], d = m[4], e = m[5], f = m[6], g = m[8], h = m[9], i = m[10];
    float det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    if (fabsf(det) < 1e-9f) return 0;
    float k = 1.0f / det;
    float r[9] = {(e * i - f * h) * k, (c * h - b * i) * k, (b * f - c * e) * k, (f * g - d * i) * k, (a * i - c * g) * k,
                  (c * d - a * f) * k, (d * h - e * g) * k, (b * g - a * h) * k, (a * e - b * d) * k};
    for (int rr = 0; rr < 3; rr++) {
        o[rr * 4] = r[rr * 3]; o[rr * 4 + 1] = r[rr * 3 + 1]; o[rr * 4 + 2] = r[rr * 3 + 2];
        o[rr * 4 + 3] = -(r[rr * 3] * m[3] + r[rr * 3 + 1] * m[7] + r[rr * 3 + 2] * m[11]);
    }
    return 1;
}

/* .hm files are 35-50 MB: read on a thread when the model first appears (the game's own model is drawn
 * meanwhile), the GL objects made on the main thread at the next draw */
static int mesh_reader(void *arg) {
    MModel *m = arg;
    FILE *f = fopen(m->hm_file, "rb");
    uint8_t *d = 0;
    long size = 0;
    if (f) {
        fseek(f, 0, SEEK_END);
        size = ftell(f);
        fseek(f, 0, SEEK_SET);
        d = size > 0 ? malloc((size_t)size) : 0;
        if (d && fread(d, 1, (size_t)size, f) != (size_t)size) { free(d); d = 0; }
        fclose(f);
    }
    m->file = d;
    m->file_size = size;
    if (d && !m->look) {
        uint32_t t0 = SDL_GetTicks();
        m->look = mlook_build(d, size, models_light_radius, models_normal_radius, models_head_radius);
        LOG("models: %s look fixes %s (%u ms on the loader thread)\n", m->hm_file, m->look ? "built" : "FAILED", SDL_GetTicks() - t0);
    }
    SDL_MemoryBarrierRelease();
    m->mesh_state = d ? 2 : 4;
    return 0;
}

static void request_mesh(MModel *m) {
    if (m->mesh_state || !m->hm_file[0]) return;
    m->mesh_state = 1;
    SDL_Thread *t = SDL_CreateThread(mesh_reader, "mesh", m);
    if (t) SDL_DetachThread(t);
    else mesh_reader(m);
}

/* Mesh vertices weighted to a skin part (no vertices of its own, so no matrix from the game) cannot be posed: a skin's
 * polygons join vertices of several parts and take their shape from those parts' poses. FE2's flood gate FLDGATE
 * (area A rooms 13 / 14): posts part 0 and part 1 (faceless) share their rest positions, the game slides part 1 and
 * the skin (part 2) is the gate panel stretched between them; the remodel put the panel on the skin, which follows
 * part 0 at rest, so it collapsed to a line along the post while the game's panel was left out: one edge drawn, the
 * panel gone (since the faceless-part fix every gate is remodelled; before, one of three kept its PS1 panel by chance).
 * Such vertices are folded onto one point (their triangles draw nothing) and the skin's original packets stay drawn
 * over the remodel's other parts. Only FLDGATE has such vertices in the two games' tables (5 Nov). */
static void mesh_unposable(MModel *m) {
    uint8_t *d = m->file;
    if (!d || m->file_size < 28 || memcmp(d, "FE2HM01", 8)) return;
    uint32_t nv, nu;
    memcpy(&nv, d + 8, 4);
    memcpy(&nu, d + 16, 4);
    long o = 28 + 16 * (long)nu;
    if (o + 44 * (long)nv > m->file_size) return;
    int folded = 0;
    for (uint32_t i = 0; i < nv; i++) {
        uint8_t *v = d + o + 44 * (long)i, *j = v + 32, *w = v + 36;
        int skin = 0;
        for (int k = 0; k < 4; k++)
            if (w[k] && j[k] < m->nparts && !m->parts[j[k]].nverts && m->parts[j[k]].nfaces) {
                m->keep_parts |= 1u << j[k];
                skin = 1;
            }
        if (!skin) continue;
        memset(v, 0, 12); /* position: the origin of part 0, weight 1 */
        j[0] = 0; w[0] = 255;
        j[1] = j[2] = j[3] = 0; w[1] = w[2] = w[3] = 0;
        folded++;
    }
    if (folded) LOG("models: %s: %d mesh vertices on skin parts (mask %X) cannot be posed: left out, the game's skin drawn\n",
                    m->name, folded, m->keep_parts);
}

static int mesh_ready(MModel *m) {
    if (m->mesh_state == 2) {
        SDL_MemoryBarrierAcquire();
        uint32_t t0 = SDL_GetTicks();
        if (!getenv("FE_MODELS_SKIN_MESH")) mesh_unposable(m); /* after mlook_build: the look keeps the file's vertices */
        m->mesh = hw_mesh_create(m->file, m->file_size, m->hm_file, m->look);
        m->file = 0;
        m->mesh_state = m->mesh ? 3 : 4;
        LOG("models: %s mesh %s: %s (%u ms on the main thread)\n", m->name, m->hm_file, m->mesh ? "loaded" : "FAILED",
            SDL_GetTicks() - t0);
    }
    return m->mesh_state == 3;
}

/* ------------------------------------------------------------------ sprite actors (SCENE_ACTORS.md "sprite=")
 * A scene actor that turns about one axis (the cog wheels) drawn as pre-rendered pictures of itself, one per angle:
 * a .spr file (text) names an atlas PNG (premultiplied RGBA, the poses in a grid of equal cells, row by row) and gives
 *   SPRITE1
 *   atlas <png, next to the .spr>
 *   cells <cell w> <cell h> <columns>
 *   poses <n> <angle of pose 0> <step>      degrees about the actor's turn axis (phase / osc / spin), pose i at a0 + i step
 *   quad x y z  x y z  x y z  x y z          the picture's corners (cell's top left, top right, bottom right, bottom
 *                                            left) in the model's space at angle 0: the picture lies on that plane
 * At each frame the two poses round the actor's angle are both turned the rest of the way about the axis (their quads
 * rotated in the model, so the teeth line up) and cross-faded: the motion is as smooth as the angle, the lighting
 * changes as the poses' does. Placement, depth (zback), light (lights=) and timing are the actor line's. The atlas is
 * decoded on a thread at the actor's first appearance; if the .spr or its atlas is missing, the actor's mesh is drawn. */
extern unsigned char *stbi_load(const char *filename, int *x, int *y, int *comp, int req_comp);
extern void stbi_image_free(void *p);
typedef struct Sprite {
    char spr[300], atlas[1400];
    int cw, ch, cols, n;
    float a0, step;
    float quad[4][3];
    volatile int state; /* 0 not asked, 1 reading, 2 pixels ready, 3 texture made, 4 failed */
    unsigned char *px;
    int w, h;
    unsigned tex;
} Sprite;
#define MAXSPRITES 16
static Sprite sprites[MAXSPRITES];
static int nsprites;

static Sprite *sprite_load(const char *dir, const char *file) {
    char p[1400];
    snprintf(p, sizeof p, "%s/%s", dir, file);
    for (int i = 0; i < nsprites; i++) if (!strcmp(sprites[i].spr, p)) return &sprites[i];
    if (nsprites >= MAXSPRITES) return 0;
    FILE *f = fopen(p, "r");
    if (!f) { LOG("actors: sprite %s missing: the mesh instead\n", p); return 0; }
    Sprite s;
    memset(&s, 0, sizeof s);
    snprintf(s.spr, sizeof s.spr, "%s", p);
    char line[512], a[300] = "";
    int got = 0;
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "atlas %299s", a) == 1) got |= 1;
        else if (sscanf(line, "cells %d %d %d", &s.cw, &s.ch, &s.cols) == 3) got |= 2;
        else if (sscanf(line, "poses %d %f %f", &s.n, &s.a0, &s.step) == 3) got |= 4;
        else if (sscanf(line, "quad %f %f %f %f %f %f %f %f %f %f %f %f", &s.quad[0][0], &s.quad[0][1], &s.quad[0][2],
                        &s.quad[1][0], &s.quad[1][1], &s.quad[1][2], &s.quad[2][0], &s.quad[2][1], &s.quad[2][2],
                        &s.quad[3][0], &s.quad[3][1], &s.quad[3][2]) == 12)
            got |= 8;
    }
    fclose(f);
    if (got != 15 || s.n < 1 || s.cw < 1 || s.ch < 1 || s.cols < 1 || (s.n > 1 && s.step == 0.0f)) {
        LOG("actors: sprite %s not valid: the mesh instead\n", p);
        return 0;
    }
    const char *sl = strrchr(p, '/');
    snprintf(s.atlas, sizeof s.atlas, "%.*s/%s", sl ? (int)(sl - p) : 1, sl ? p : ".", a);
    sprites[nsprites] = s;
    LOG("actors: sprite %s: %d poses from %.2f by %.3f degrees, cells %d x %d\n", p, s.n, s.a0, s.step, s.cw, s.ch);
    return &sprites[nsprites++];
}

static int sprite_reader(void *arg) {
    Sprite *s = arg;
    int w = 0, h = 0, c;
    uint32_t t0 = SDL_GetTicks();
    unsigned char *px = stbi_load(s->atlas, &w, &h, &c, 4);
    if (px && (w < s->cw * s->cols || h < s->ch * ((s->n + s->cols - 1) / s->cols))) {
        LOG("actors: sprite atlas %s is %d x %d, smaller than its cells\n", s->atlas, w, h);
        stbi_image_free(px);
        px = 0;
    }
    s->px = px; s->w = w; s->h = h;
    LOG("actors: sprite atlas %s %s (%d x %d, %u ms on the loader thread)\n", s->atlas, px ? "read" : "FAILED", w, h,
        SDL_GetTicks() - t0);
    SDL_MemoryBarrierRelease();
    s->state = px ? 2 : 4;
    return 0;
}

/* 1 = the texture is there; 0 = not yet (asked); -1 = it cannot be had (draw the mesh) */
static int sprite_ready(Sprite *s) {
    if (s->state == 0) {
        s->state = 1;
        SDL_Thread *t = SDL_CreateThread(sprite_reader, "sprite", s);
        if (t) SDL_DetachThread(t);
        else sprite_reader(s);
    }
    if (s->state == 2) {
        SDL_MemoryBarrierAcquire();
        s->tex = hw_tex_rgba(s->w, s->h, s->px, 2);
        stbi_image_free(s->px);
        s->px = 0;
        s->state = s->tex ? 3 : 4;
        if (s->tex) LOG("actors: sprite %s: texture %d x %d (%.1f MB with mipmaps)\n", s->spr, s->w, s->h,
                        (double)s->w * s->h * 4 * 4 / 3 / 1048576.0);
    }
    return s->state == 3 ? 1 : s->state == 4 ? -1 : 0;
}

static float wrap180(float d) {
    d = fmodf(d + 180.0f, 360.0f);
    if (d < 0) d += 360.0f;
    return d - 180.0f;
}

/* B: the actor's root bone without its turn (model -> view, 3 x 4); ang: its angle about axis (unit, model space)
 * through the pivot jw; mul: light per channel; h, ofx, ofy: the GTE projection */
static void sprite_draw(const Sprite *s, const float *B, const float *axis, const float *jw, float ang, const float *mul,
                        float h, float ofx, float ofy, float zback) {
    float span = s->step * (float)(s->n - 1);
    float x = s->n > 1 ? (wrap180(ang - (s->a0 + span * 0.5f)) + span * 0.5f) / s->step : 0.0f;
    if (x < 0) x = 0;
    if (x > (float)(s->n - 1)) x = (float)(s->n - 1);
    int k0 = (int)x, k1 = k0 + 1 < s->n ? k0 + 1 : k0;
    float u = x - (float)k0;
    int nl = k1 != k0 && u > 1e-4f ? 2 : 1;
    float xyz[2][4][3], uv[2][4][2], wt[2] = {nl == 2 ? 1.0f - u : 1.0f, u};
    for (int l = 0; l < nl; l++) {
        int k = l ? k1 : k0;
        float d = wrap180(ang - (s->a0 + s->step * (float)k)) * 0.01745329f, c = cosf(d), sn = sinf(d);
        const float *n = axis;
        for (int i = 0; i < 4; i++) {
            /* the corner turned by d about the axis through the pivot (Rodrigues) */
            float v[3] = {s->quad[i][0] - jw[0], s->quad[i][1] - jw[1], s->quad[i][2] - jw[2]};
            float nd = n[0] * v[0] + n[1] * v[1] + n[2] * v[2];
            float cr[3] = {n[1] * v[2] - n[2] * v[1], n[2] * v[0] - n[0] * v[2], n[0] * v[1] - n[1] * v[0]};
            float r[3], p[3];
            for (int j = 0; j < 3; j++) r[j] = v[j] * c + cr[j] * sn + n[j] * nd * (1.0f - c) + jw[j];
            for (int j = 0; j < 3; j++) p[j] = B[j * 4] * r[0] + B[j * 4 + 1] * r[1] + B[j * 4 + 2] * r[2] + B[j * 4 + 3];
            float z = p[2] > 1.0f ? p[2] : 1.0f;
            xyz[l][i][0] = ofx + h * p[0] / z;
            xyz[l][i][1] = ofy + h * p[1] / z;
            xyz[l][i][2] = p[2];
        }
        float cu = (float)((k % s->cols) * s->cw), cv = (float)((k / s->cols) * s->ch);
        float u0 = cu / (float)s->w, u1 = (cu + (float)s->cw) / (float)s->w;
        float v0 = cv / (float)s->h, v1 = (cv + (float)s->ch) / (float)s->h;
        uv[l][0][0] = u0; uv[l][0][1] = v0; uv[l][1][0] = u1; uv[l][1][1] = v0;
        uv[l][2][0] = u1; uv[l][2][1] = v1; uv[l][3][0] = u0; uv[l][3][1] = v1;
    }
    hw_sprite_draw(s->tex, nl, (const float (*)[4][3])xyz, (const float (*)[4][2])uv, wt, mul, zback);
    if (getenv("FE_DBG_SPRITES")) {
        static int nlog;
        if (nlog++ < 2000)
            LOG("sprite: %s angle %.2f poses %d/%d at %.3f, corner 0 at %.1f %.1f z %.0f\n", s->spr, ang, k0, k1, u, xyz[0][0][0],
                xyz[0][0][1], xyz[0][0][2]);
    }
}

/* every mesh drawn, as drawn (60 fps in-between frames): the game fills a packet buffer one game frame before the GPU
 * list that draws it, so by the in-between vblank the shown frame's buffer already holds the pose after next */
/* a soft shadow as drawn with a pose: its floor (q, n), light, contact quad, fade and blur (0 = none) */
typedef struct {
    int on, recv; /* recv: a contact shadow (R), else the flat one */
    float plane[6], light[3], quad[12], fade, blur;
    ShadowRecv R;
} Shadow;

typedef struct {
    uint32_t seq;
    Inst *in;
    int model, nf;
    uint64_t frame;
    float bones[MAXPARTS][12];
    uint8_t ok[MAXPARTS];
    float h, ofx, ofy;
    Shadow sh;
    int look;      /* a scene actor's own look (actors.txt "ink=0", "shade="): 1 = shade / ink below, 0 = swap.txt's */
    float shade;   /* SHADE for this mesh (as models_shade: > 0 cel bands, < 0 smooth) */
    int noink;     /* no ink outline */
    float zback;   /* a scene actor's depth anchor (actors.txt "zback=") */
    const ActorPaint *paint; float spec; /* its painted look (the actor's, static) */
    const struct Sprite *spr; /* a sprite actor (bones: its pose without the turn), else 0 */
    float spr_ang, spr_mul[3], spr_axis[3], spr_jw[3]; /* its angle about the axis (degrees), light, axis, pivot */
    uint32_t next;            /* the same Inst's next pose (0 = none yet): the in-between's partner, no search */
    unsigned matte_tex;       /* a scene actor under a view's matte (actors.txt MATTE): its texture, 0 = none */
    int skin_dq;              /* a scene actor skinned with dual quaternions (actors.txt "skin=dq") */
    float matte_rect[4];      /* ... and where the movie picture lies in VRAM (native x0, y0, x1, y1) */
    uint32_t colors[2048];
} Pose;
/* The pose store: a ring, each drawn mesh's pose per GPU pass while 60 fps smoothing is on; an in-between frame needs
 * the poses of the frame on display and of the next one. 512 (was 128 before 7 Nov, NOTES "Scene actors: crowd
 * capacity"): about 240 meshes a view instead of 55. FE_CROWD_OLD=1 (or FE_POSE_RING_OLD=1): 128 slots and the old
 * next-pose search. */
#define MAXPOSE 512
#define MAXPOSE_OLD 128
static Pose poses[MAXPOSE];
static uint32_t pose_seq;
static int crowd_old_ = -1;
static int crowd_old(void) { if (crowd_old_ < 0) crowd_old_ = getenv("FE_CROWD_OLD") != 0; return crowd_old_; }
static int pose_old(void) {
    static int d = -1;
    if (d < 0) d = crowd_old() || getenv("FE_POSE_RING_OLD") != 0;
    return d;
}
static uint32_t pose_ring(void) { return pose_old() ? MAXPOSE_OLD : MAXPOSE; }
#define POSE(id) (&poses[(id) % pose_ring()])

static uint32_t save_pose(Inst *in, float (*bones)[12], const uint8_t *ok, const uint32_t *colors, int nf, int buf) {
    Pose *s = POSE(++pose_seq);
    s->seq = pose_seq; s->in = in; s->model = in->model; s->nf = nf; s->frame = frame_count;
    memcpy(s->bones, bones, sizeof s->bones);
    memcpy(s->ok, ok, sizeof s->ok);
    memcpy(s->colors, colors, sizeof(uint32_t) * (size_t)nf);
    s->h = in->h[buf]; s->ofx = in->ofx[buf]; s->ofy = in->ofy[buf];
    s->sh.on = 0;
    s->look = 0;
    s->zback = 0.0f; s->paint = 0; s->spec = 1.0f;
    s->spr = 0;
    s->next = 0;
    s->matte_tex = 0;
    s->skin_dq = 0;
    /* the link: the Inst's previous pose (if still in the store) gets this one as its next */
    if (in->pose_last && in->pose_last != pose_seq) {
        Pose *p = POSE(in->pose_last);
        if (p->seq == in->pose_last && p->in == in) p->next = pose_seq;
    }
    in->pose_last = pose_seq;
    return pose_seq;
}

/* a mesh drawn with its own look (a room decoration painted into the picture: no cartoon ink, smooth shading), the
 * swap.txt look restored after */
static float look_shade_saved, look_ink_saved;
static void look_push(int on, float shade, int noink, float zback, const ActorPaint *paint, float spec) {
    look_shade_saved = models_shade; look_ink_saved = models_outline_px;
    models_zback = zback;
    models_paint = paint && paint->on ? paint : 0;
    models_spec = spec;
    if (!on) return;
    models_shade = shade;
    if (noink) models_outline_px = 0.0f;
}
static void look_pop(void) {
    models_shade = look_shade_saved; models_outline_px = look_ink_saved;
    models_zback = 0.0f; models_paint = 0; models_spec = 1.0f;
    models_matte_tex = 0;
    models_skin_dq = 0;
}

static void bone_apply(const float *b, const float *v, float *o) {
    for (int i = 0; i < 3; i++) o[i] = b[i * 4] * v[0] + b[i * 4 + 1] * v[1] + b[i * 4 + 2] * v[2] + b[i * 4 + 3];
}

static int dbg_shadow;

/* ------------------------------------------------------------------ room camera and collision (contact shadows)
 * Every room view's camera travels in its movie (the remaster's GEOMETRY.md): a block "A05L", 0, then records "ST" /
 * "YD", fov, roll (4096 = 360 deg), eye[3], target[3] (16.16, world units, Y up). The game keeps it in RAM (several
 * copies, all equal for a static view). World -> GTE view: right = up x forward, up' = forward x right, the roll about
 * the view axis, then y * -0.625 (the 512-wide draw of a 320-wide movie; screen y down). Checked per shadow: the blob's
 * centre, taken back to the world, has to lie on a collision floor, else the flat shadow is used. */
static struct { uint32_t at; uint64_t frame, scanned; int ok; char area; float eye[3], V[9], Vi[9]; } cam;

typedef struct { char wall, portal; float n[3], d, c[4][3]; } CFace; /* portal: a zone boundary, not solid */
static CFace *cfaces;
static int ncfaces;
static char coll_area;

static void coll_load(char area) {
    if (area == coll_area) return;
    coll_area = area;
    free(cfaces); cfaces = 0; ncfaces = 0;
    char p[1200];
    FILE *f = 0;
    snprintf(p, sizeof p, "%s/coll/%c.txt", game_dir, area);
    if (!(f = fopen(p, "r"))) {
        snprintf(p, sizeof p, "%s/geometry/coll_port/%c.txt", game_dir, area);
        f = fopen(p, "r");
    }
    if (!f) { LOG("models: no collision for area %c\n", area); return; }
    int cap = 1024;
    cfaces = malloc(sizeof *cfaces * (size_t)cap);
    char line[512];
    while (fgets(line, sizeof line, f)) {
        CFace c;
        char k;
        if (sscanf(line, "%c" " %f %f %f %f" " %f %f %f %f %f %f %f %f %f %f %f %f", &k, &c.n[0], &c.n[1], &c.n[2], &c.d,
                   &c.c[0][0], &c.c[0][1], &c.c[0][2], &c.c[1][0], &c.c[1][1], &c.c[1][2], &c.c[2][0], &c.c[2][1], &c.c[2][2],
                   &c.c[3][0], &c.c[3][1], &c.c[3][2]) != 17) continue;
        c.wall = k == 'w';
        c.portal = c.wall && strstr(line, " portal") != 0; /* tools/make_coll.py: a zone boundary */
        if (ncfaces == cap) { cap *= 2; cfaces = realloc(cfaces, sizeof *cfaces * (size_t)cap); }
        cfaces[ncfaces++] = c;
    }
    fclose(f);
    LOG("models: collision of area %c: %d faces (%s)\n", area, ncfaces, p);
}

static int cam_read(uint32_t a) {
    uint32_t tag = mem_r32(a);
    int fov = (int)mem_r32(a + 4);
    if ((tag != 0x00005453u && tag != 0x00004459u) || fov < 20 || fov > 140) return 0;
    char lab = (char)mem_r8(a - 8);
    /* "A03L" a room's loop view, "A03S" its start (a one-shot shot before the loop: scene actors walk there) */
    if (lab < 'A' || lab > 'Z' || (mem_r8(a - 5) != 'L' && mem_r8(a - 5) != 'S')) return 0;
    float e[3], t[3];
    for (int i = 0; i < 3; i++) {
        e[i] = (float)(int32_t)mem_r32(a + 12 + 4 * i) / 65536.0f;
        t[i] = (float)(int32_t)mem_r32(a + 24 + 4 * i) / 65536.0f;
    }
    float f[3] = {t[0] - e[0], t[1] - e[1], t[2] - e[2]};
    float fl = sqrtf(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    if (fl < 1.0f) return 0;
    for (int i = 0; i < 3; i++) f[i] /= fl;
    float r[3] = {f[2], 0.0f, -f[0]}; /* (0, 1, 0) x forward */
    float rl = sqrtf(r[0] * r[0] + r[2] * r[2]);
    if (rl < 1e-4f) return 0;
    r[0] /= rl; r[2] /= rl;
    float u[3] = {f[1] * r[2] - f[2] * r[1], f[2] * r[0] - f[0] * r[2], f[0] * r[1] - f[1] * r[0]}; /* forward x right */
    float ang = (float)(int)mem_r32(a + 8) * 6.2831853f / 4096.0f, cs = cosf(ang), sn = sinf(ang);
    float R[9];
    for (int i = 0; i < 3; i++) {
        R[i] = cs * r[i] - sn * u[i];
        R[3 + i] = sn * r[i] + cs * u[i];
        R[6 + i] = f[i];
    }
    for (int i = 0; i < 3; i++) { cam.V[i] = R[i]; cam.V[3 + i] = -0.625f * R[3 + i]; cam.V[6 + i] = R[6 + i]; }
    for (int i = 0; i < 3; i++) { /* the inverse: R^T with the y scale undone */
        cam.Vi[i * 3 + 0] = R[i];
        cam.Vi[i * 3 + 1] = -R[3 + i] / 0.625f;
        cam.Vi[i * 3 + 2] = R[6 + i];
    }
    memcpy(cam.eye, e, sizeof e);
    cam.area = lab;
    return 1;
}

/* the room camera of this frame (0 = none: an FMV, a menu) */
static int cam_update(void) {
    if (cam.frame == frame_count) return cam.ok;
    cam.frame = frame_count;
    if (cam.at && cam_read(cam.at)) { cam.ok = 1; coll_load(cam.area); return 1; }
    cam.ok = 0;
    if (cam.scanned + 30 > frame_count && cam.scanned) return 0;
    cam.scanned = frame_count;
    for (uint32_t a = 0x80010000u; a < 0x801FFFC0u; a += 4)
        if (cam_read(a)) { cam.at = a; cam.ok = 1; coll_load(cam.area); return 1; }
    cam.at = 0;
    return 0;
}

/* The directional controls' wall guard (controls.c, NOTES "Directional controls: walking through walls"): the
 * player's centre went from (x0,z0) to (*x,*z) at height y since the last vblank. Where that crosses a wall face of
 * this area's collision (within its length and height), *x / *z go back to the side they came from, 1 unit off the
 * plane, the move along the wall kept (she slides as the game slides her) - but only a wall the game was holding her
 * at: at least GUARD_PRESSED of the hn earlier positions (hx, hz) lie on its front side within GUARD_SKIN units. Not
 * every collision wall is solid all the time (FE2 island, a railing at (5595,-4615) the game lets her walk across):
 * walking through one spends a few frames near it, being pressed against a solid one many. 1 = moved back; 0 = no
 * such crossing, or no room camera / collision known (then nothing is checked). */
#define GUARD_SKIN 60.0f
#define GUARD_PRESSED 10
int coll_wall_guard(float x0, float z0, float *x, float *z, float y, const float *hx, const float *hz, int hn) {
    if (!cam_update() || !ncfaces) return 0;
    int hit = 0;
    for (int i = 0; i < ncfaces; i++) {
        const CFace *c = &cfaces[i];
        if (!c->wall || c->portal) continue; /* a zone boundary is no wall: the game walks through it */
        float nl = sqrtf(c->n[0] * c->n[0] + c->n[2] * c->n[2]);
        if (nl < 0.5f) continue;
        float nx = c->n[0] / nl, nz = c->n[2] / nl, d = c->d / nl;
        float s0 = nx * x0 + nz * z0 - d, s1 = nx * *x + nz * *z - d;
        if (s0 == 0.0f || (s0 > 0.0f) == (s1 > 0.0f)) continue;
        float ylo = c->c[0][1], yhi = c->c[0][1];
        for (int k = 1; k < 4; k++) { if (c->c[k][1] < ylo) ylo = c->c[k][1]; if (c->c[k][1] > yhi) yhi = c->c[k][1]; }
        if (y < ylo - 64.0f || y > yhi) continue;
        float t = s0 / (s0 - s1), px = x0 + t * (*x - x0), pz = z0 + t * (*z - z0);
        float ex = c->c[1][0] - c->c[0][0], ez = c->c[1][2] - c->c[0][2], el2 = ex * ex + ez * ez;
        if (el2 < 1.0f) continue;
        float u = ((px - c->c[0][0]) * ex + (pz - c->c[0][2]) * ez) / el2;
        if (u < 0.0f || u > 1.0f) continue;
        int pressed = 0;
        for (int k = 0; k < hn; k++) {
            float sk = (nx * hx[k] + nz * hz[k] - d) * (s0 > 0.0f ? 1.0f : -1.0f);
            float uk = ((hx[k] - c->c[0][0]) * ex + (hz[k] - c->c[0][2]) * ez) / el2;
            pressed += sk > 0.0f && sk < GUARD_SKIN && uk >= 0.0f && uk <= 1.0f;
        }
        if (pressed < GUARD_PRESSED) continue;
        float back = s1 - (s0 > 0.0f ? 1.0f : -1.0f);
        *x -= back * nx; *z -= back * nz;
        hit = 1;
    }
    return hit;
}

/* ------------------------------------------------------------------ the scene actors' camera: the picture's (NOTES.md
 * "Scene actors: the camera of the picture on screen"). At a cut the game writes the new view's camera into RAM a few
 * frames before the new view's first movie frame is decoded and shown: the old view's picture stays up meanwhile (A65L
 * -> its other view: camera at game frame N, picture at N + 10). An actor is part of a picture, so it keeps the camera
 * its picture's run began with: the RAM camera is taken at the start of every run (game_fe2.c movie_shown_run: a cut,
 * a room change or a load, not a loop restart) and while the run is young (the camera written just after the picture
 * came); a change later in a run waits for the next run (the new view's picture), at most ACTCAM_HOLD vblanks.
 * Presentation only. FE_ACTOR_CAM_OLD=1: the RAM camera every pass (as before); FE_DBG_ACTCAM=1 logs each pass. */
#define ACTCAM_YOUNG 6   /* vblanks after a run's first pass in which a new RAM camera still belongs to it */
#define ACTCAM_HOLD 90   /* vblanks a new RAM camera waits for its picture (then taken: a camera change without a cut) */
static struct { int ok; unsigned run; uint64_t run_at, diff_at; float V[9], Vi[9], eye[3]; } acam;
static float acam_save[21];

static int acam_same(void) {
    for (int i = 0; i < 3; i++) if (fabsf(acam.eye[i] - cam.eye[i]) > 0.5f) return 0;
    for (int i = 0; i < 9; i++) if (fabsf(acam.V[i] - cam.V[i]) > 1e-4f) return 0;
    return 1;
}

static void acam_take(void) {
    memcpy(acam.V, cam.V, sizeof acam.V); memcpy(acam.Vi, cam.Vi, sizeof acam.Vi); memcpy(acam.eye, cam.eye, sizeof acam.eye);
    acam.ok = 1; acam.diff_at = 0;
}

/* after cam_update(): puts the picture's camera into cam (until actor_cam_end) */
static void actor_cam_begin(int mv, int frame) {
    static int old = -1, dbg = -1;
    if (old < 0) { old = getenv("FE_ACTOR_CAM_OLD") != 0; dbg = getenv("FE_DBG_ACTCAM") != 0; }
    memcpy(acam_save, cam.V, sizeof cam.V); memcpy(acam_save + 9, cam.Vi, sizeof cam.Vi); memcpy(acam_save + 18, cam.eye, sizeof cam.eye);
    unsigned run = movie_shown_run();
    const char *why = "same";
    if (!acam.ok || run != acam.run) {
        acam.run = run; acam.run_at = frame_count;
        acam_take(); why = "run";
    } else if (!acam_same()) {
        if (frame_count - acam.run_at <= ACTCAM_YOUNG) { acam_take(); why = "young"; }
        else {
            if (!acam.diff_at) acam.diff_at = frame_count;
            if (frame_count - acam.diff_at > ACTCAM_HOLD) { acam_take(); why = "timeout"; }
            else why = "held";
        }
    } else acam.diff_at = 0;
    if (dbg)
        LOG("actcam: frame %llu %s %d run %u (%s) ram eye %.0f %.0f %.0f picture eye %.0f %.0f %.0f%s\n",
            (unsigned long long)frame_count, movie_name(mv), frame, run, why, cam.eye[0], cam.eye[1], cam.eye[2], acam.eye[0],
            acam.eye[1], acam.eye[2], old ? " (old: ram used)" : "");
    if (old) return;
    memcpy(cam.V, acam.V, sizeof cam.V); memcpy(cam.Vi, acam.Vi, sizeof cam.Vi); memcpy(cam.eye, acam.eye, sizeof cam.eye);
}

static void actor_cam_end(void) {
    memcpy(cam.V, acam_save, sizeof cam.V); memcpy(cam.Vi, acam_save + 9, sizeof cam.Vi); memcpy(cam.eye, acam_save + 18, sizeof cam.eye);
}

static void to_world(const float *v, float *w) {
    for (int i = 0; i < 3; i++) w[i] = cam.Vi[i * 3] * v[0] + cam.Vi[i * 3 + 1] * v[1] + cam.Vi[i * 3 + 2] * v[2] + cam.eye[i];
}

static void to_view(const float *w, float *v) {
    float d[3] = {w[0] - cam.eye[0], w[1] - cam.eye[1], w[2] - cam.eye[2]};
    for (int i = 0; i < 3; i++) v[i] = cam.V[i * 3] * d[0] + cam.V[i * 3 + 1] * d[1] + cam.V[i * 3 + 2] * d[2];
}

/* ------------------------------------------------------------------ moving-camera correction (NOTES.md "60 fps room
 * movies: character stepping"). In a camera move the game projects its characters with the camera of the STR frame
 * on screen (its matrices follow the frame; the RAM camera record is refreshed only every third frame and is not what
 * it projects with, measured 5 Nov), so they move once per 4 vblanks while a 60 fps background glides.
 * cam_tracks.txt in the models folder ("<movie> <STR frame> eye xyz target xyz roll fov", from cameras.json, written
 * by tools/cam_tracks.py) gives the exact camera of every movie frame; a character's view-space bones are taken back
 * to the world with frame f's camera and into the view of the picture on screen (frame f + the picture's fraction).
 * Presentation only. ini [game] movie_cam_fix = 0 / --no-movie-cam-fix / FE_NO_CAMFIX=1: off; FE_DBG_CAMFIX=1 logs the
 * first part's place with both cameras. Not corrected: what the game draws itself (sprites, effects, models without a
 * remodel), and a character's own motion, which the game still updates once per STR frame. */
int models_camfix = 1;
static struct { char movie[32]; int frame; float c[8]; } *ctrack;
static int nctrack;

static void load_cam_tracks(void) {
    char p[1100];
    snprintf(p, sizeof p, "%s/cam_tracks.txt", host_dir);
    FILE *f = fopen(p, "r");
    if (!f) return;
    char line[512];
    int cap = 0;
    while (fgets(line, sizeof line, f)) {
        char mv[32];
        int fr;
        float c[8];
        if (line[0] == '#' || sscanf(line, "%31s %d %f %f %f %f %f %f %f %f", mv, &fr, &c[0], &c[1], &c[2], &c[3], &c[4],
                                     &c[5], &c[6], &c[7]) != 10) continue;
        if (nctrack == cap) { cap = cap ? cap * 2 : 1024; ctrack = realloc(ctrack, sizeof *ctrack * (size_t)cap); }
        snprintf(ctrack[nctrack].movie, sizeof ctrack[nctrack].movie, "%s", mv);
        ctrack[nctrack].frame = fr;
        memcpy(ctrack[nctrack].c, c, sizeof c);
        nctrack++;
    }
    fclose(f);
    LOG("models: %d camera track frames (%s)%s\n", nctrack, p, models_camfix ? "" : ", correction off");
}

/* the camera (V with the GTE's y scale, eye) of movie mv at time t (STR frames); 0 = no track there */
static int track_cam(const char *mv, float t, float *V, float *E) {
    static int last = -1;
    int f0 = (int)floorf(t);
    float u = t - (float)f0;
    const float *a = 0, *b = 0;
    if (last >= 0 && last < nctrack && !strcmp(ctrack[last].movie, mv) && ctrack[last].frame == f0) a = ctrack[last].c;
    for (int i = 0; i < nctrack && !a; i++)
        if (ctrack[i].frame == f0 && !strcmp(ctrack[i].movie, mv)) { a = ctrack[i].c; last = i; }
    if (!a) return 0;
    if (last + 1 < nctrack && ctrack[last + 1].frame == f0 + 1 && !strcmp(ctrack[last + 1].movie, mv)) b = ctrack[last + 1].c;
    if (!b) { b = a; u = 0; }
    float c[8], f[3], r[3], up[3];
    for (int i = 0; i < 8; i++) c[i] = a[i] + (b[i] - a[i]) * u;
    for (int i = 0; i < 3; i++) f[i] = c[3 + i] - c[i];
    float fl = sqrtf(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    r[0] = f[2]; r[1] = 0; r[2] = -f[0];
    float rl = sqrtf(r[0] * r[0] + r[2] * r[2]);
    if (fl < 1.0f || rl < 1e-4f) return 0;
    for (int i = 0; i < 3; i++) { f[i] /= fl; r[i] /= rl; }
    up[0] = f[1] * r[2] - f[2] * r[1]; up[1] = f[2] * r[0] - f[0] * r[2]; up[2] = f[0] * r[1] - f[1] * r[0];
    float ang = c[6] * 6.2831853f / 4096.0f, cs = cosf(ang), sn = sinf(ang);
    for (int i = 0; i < 3; i++) { /* as cam_read */
        V[i] = cs * r[i] - sn * up[i];
        V[3 + i] = -0.625f * (sn * r[i] + cs * up[i]);
        V[6 + i] = f[i];
        E[i] = c[i];
    }
    return 1;
}

/* view-space bones of the RAM camera -> those of the picture on screen (a moving camera with a track); 1 = changed:
 * then cam holds the picture's camera (the shadow is built with it) until camfix_restore(save) */
static void camfix_restore(const float *save) {
    memcpy(cam.V, save, sizeof cam.V);
    memcpy(cam.Vi, save + 9, sizeof cam.Vi);
    memcpy(cam.eye, save + 18, sizeof cam.eye);
}

static int camfix_bones(float bones[][12], int nparts, float *save) {
    if (!models_camfix || !nctrack) return 0;
    cam_update(); /* the RAM camera is not needed (the track has the frame's camera); FE1's "T" views have no record */
    int frame, mv = movie_shown(&frame);
    if (mv < 0) return 0;
    float t = (float)frame + (movie_hfr && movie_rate(mv) > 1 ? movie_shown_frac() : 0.0f);
    /* the camera the game projected with: its matrices follow the movie frame on screen (frame f; the RAM camera record
     * itself is refreshed only every third frame and is not it: frame f + 1 or f - 1 made the positions zigzag) */
    float V[9], E[3], G[9], Gi[9], Eg[3], o0[3] = {0, 0, 1};
    if (!track_cam(movie_name(mv), t, V, E) || !track_cam(movie_name(mv), (float)frame, G, Eg)) return 0;
    for (int i = 0; i < 3; i++) { Gi[i * 3] = G[i]; Gi[i * 3 + 1] = G[3 + i] / (0.625f * 0.625f); Gi[i * 3 + 2] = G[6 + i]; }
    if (nparts > 0) { o0[0] = bones[0][3]; o0[1] = bones[0][7]; o0[2] = bones[0][11]; }
    for (int p = 0; p < nparts; p++) {
        float *b = bones[p], R[9], tw[3], d[3];
        for (int r = 0; r < 3; r++) /* world rotation: Gi * R */
            for (int c = 0; c < 3; c++)
                R[r * 3 + c] = Gi[r * 3] * b[c] + Gi[r * 3 + 1] * b[4 + c] + Gi[r * 3 + 2] * b[8 + c];
        for (int i = 0; i < 3; i++) tw[i] = Gi[i * 3] * b[3] + Gi[i * 3 + 1] * b[7] + Gi[i * 3 + 2] * b[11] + Eg[i];
        for (int i = 0; i < 3; i++) d[i] = tw[i] - E[i];
        for (int r = 0; r < 3; r++) {
            for (int c = 0; c < 3; c++) b[r * 4 + c] = V[r * 3] * R[c] + V[r * 3 + 1] * R[3 + c] + V[r * 3 + 2] * R[6 + c];
            b[r * 4 + 3] = V[r * 3] * d[0] + V[r * 3 + 1] * d[1] + V[r * 3 + 2] * d[2];
        }
    }
    memcpy(save, cam.V, sizeof cam.V);
    memcpy(save + 9, cam.Vi, sizeof cam.Vi);
    memcpy(save + 18, cam.eye, sizeof cam.eye);
    memcpy(cam.V, V, sizeof V);
    for (int i = 0; i < 3; i++) { cam.Vi[i * 3] = V[i]; cam.Vi[i * 3 + 1] = V[3 + i] / (0.625f * 0.625f); cam.Vi[i * 3 + 2] = V[6 + i]; }
    memcpy(cam.eye, E, sizeof E);
    if (getenv("FE_DBG_CAMFIX")) {
        static uint64_t lf;
        if (lf != frame_count) {
            lf = frame_count;
            /* the first part's place on screen (x / z, y / z, x 1000) with the game's camera and with the picture's */
            float nz = bones[0][11] > 1.0f ? bones[0][11] : 1.0f, oz = o0[2] > 1.0f ? o0[2] : 1.0f;
            LOG("camfix: frame %llu %s t %.2f ram eye %.0f %.0f %.0f track eye %.0f %.0f %.0f part0 old %.1f %.1f new %.1f %.1f\n",
                (unsigned long long)frame_count, movie_name(mv), t, save[18], save[19], save[20], E[0], E[1], E[2],
                1000 * o0[0] / oz, 1000 * o0[1] / oz, 1000 * bones[0][3] / nz, 1000 * bones[0][7] / nz);
        }
    }
    return 1;
}

/* how far a world point lies from the nearest collision floor under it (1e9: no floor there) */
static float floor_gap(const float *w) {
    float best = 1e9f;
    for (int i = 0; i < ncfaces; i++) {
        CFace *c = &cfaces[i];
        if (c->wall || fabsf(c->n[1]) < 0.05f) continue;
        float x0 = 1e9f, x1 = -1e9f, z0 = 1e9f, z1 = -1e9f;
        for (int k = 0; k < 4; k++) {
            if (c->c[k][0] < x0) x0 = c->c[k][0];
            if (c->c[k][0] > x1) x1 = c->c[k][0];
            if (c->c[k][2] < z0) z0 = c->c[k][2];
            if (c->c[k][2] > z1) z1 = c->c[k][2];
        }
        if (w[0] < x0 - 60 || w[0] > x1 + 60 || w[2] < z0 - 60 || w[2] > z1 + 60) continue;
        float g = fabsf(c->n[0] * w[0] + c->n[1] * w[1] + c->n[2] * w[2] - c->d);
        if (g < best) best = g;
    }
    return best;
}

/* a shadow instance's floor in view space: centre q, unit normal n (not yet turned up), radius; corners: the quad's
 * first four corners (grid order), when asked */
static int shadow_floor(Inst *s, int buf, float *q, float *n, float *rad, float *corners) {
    MModel *m = &models[s->model];
    MPart *p = &m->parts[0];
    if (!s->has[buf][0] || p->nverts < 3) return 0;
    float pt[8][3];
    int nv = p->nverts < 8 ? p->nverts : 8;
    q[0] = q[1] = q[2] = 0;
    for (int k = 0; k < nv; k++) {
        float v[3] = {p->verts[k][0] + p->jw[0], p->verts[k][1] + p->jw[1], p->verts[k][2] + p->jw[2]};
        bone_apply(s->bones[buf][0], v, pt[k]);
        for (int i = 0; i < 3; i++) q[i] += pt[k][i] / (float)nv;
        if (corners && k < 4) for (int i = 0; i < 3; i++) corners[k * 3 + i] = pt[k][i];
    }
    /* the largest cross product of two corners seen from the first spans the plane (the quad's corners come in
     * grid order: 0 1 / 2 3) */
    float l = 0;
    for (int a = 1; a < nv; a++)
        for (int b = a + 1; b < nv; b++) {
            float e[3] = {pt[a][0] - pt[0][0], pt[a][1] - pt[0][1], pt[a][2] - pt[0][2]};
            float f[3] = {pt[b][0] - pt[0][0], pt[b][1] - pt[0][1], pt[b][2] - pt[0][2]};
            float c[3] = {e[1] * f[2] - e[2] * f[1], e[2] * f[0] - e[0] * f[2], e[0] * f[1] - e[1] * f[0]};
            float cl = sqrtf(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
            if (cl > l) { l = cl; n[0] = c[0]; n[1] = c[1]; n[2] = c[2]; }
        }
    if (l < 1e-6f) return 0;
    for (int i = 0; i < 3; i++) n[i] /= l;
    *rad = 0;
    for (int k = 0; k < nv; k++) {
        float d = sqrtf((pt[k][0] - q[0]) * (pt[k][0] - q[0]) + (pt[k][1] - q[1]) * (pt[k][1] - q[1]) + (pt[k][2] - q[2]) * (pt[k][2] - q[2]));
        if (d > *rad) *rad = d;
    }
    return 1;
}

/* a character's root joint in view space (its first drawn part's joint) */
static int char_root(Inst *c, int buf, float *o) {
    MModel *m = &models[c->model];
    for (int p = 0; p < m->nparts; p++)
        if (c->has[buf][p] && m->parts[p].nverts) { bone_apply(c->bones[buf][p], m->parts[p].jw, o); return 1; }
    return 0;
}

static int is_shadow_caster(Inst *c) {
    MModel *m = &models[c->model];
    return !m->shadow && m->hm_file[0] && m->nparts >= 8 && m->mesh_state == 3; /* characters and enemies */
}

/* the character a shadow lies under: the nearest caster drawn in the same frame whose root stands over the blob */
static int shadow_link(Inst *s, int buf) {
    if (s->link_frame[buf] == s->stamp[buf] + 1) return s->link[buf];
    s->link_frame[buf] = s->stamp[buf] + 1;
    s->link[buf] = 0;
    float q[3], n[3], rad;
    int fl = shadow_floor(s, buf, q, n, &rad, 0);
    if (!fl) return 0;
    float best = 0;
    for (int i = 0; i < ninst; i++) {
        Inst *c = &insts[i];
        if (!is_shadow_caster(c) || c->stamp[buf] + 2 < s->stamp[buf] || c->stamp[buf] > s->stamp[buf] + 2) continue;
        float r[3];
        if (!char_root(c, buf, r)) continue;
        float v[3] = {r[0] - q[0], r[1] - q[1], r[2] - q[2]};
        float up = v[0] * n[0] + v[1] * n[1] + v[2] * n[2];
        float hx = v[0] - n[0] * up, hy = v[1] - n[1] * up, hz = v[2] - n[2] * up;
        float d = sqrtf(hx * hx + hy * hy + hz * hz);
        if (d > 2.5f * rad + 60.0f || fabsf(up) > 2500.0f) continue;
        if (!s->link[buf] || d < best) { best = d; s->link[buf] = i + 1; }
    }
    return s->link[buf];
}

/* the soft shadow of caster c (buffer buf, bones as about to be drawn), if a blob lies under it this frame */
/* the contact shadow's receivers and light (ShadowRecv) for caster c over the blob at view centre q (normal n, up):
 * 0 when this view has no checked camera or collision (an FMV view, a moving camera, a room change) */
static int build_recv(Inst *c, int buf, float bones[][12], const float *q, const float *n, float top, ShadowRecv *R) {
    if (!models_shadow_geom || !cam_update() || !ncfaces) return 0;
    float wb[3];
    to_world(q, wb);
    if (floor_gap(wb) > 60.0f) return 0; /* the camera does not fit this blob: not this view's */
    MModel *m = &models[c->model];
    /* the light, fixed to the camera: from above, tilted to the right and a little away (as the flat shadow's) */
    float Lw[3];
    for (int i = 0; i < 3; i++) Lw[i] = (i == 1 ? -1.0f : 0.0f) + models_shadow_tilt * (cam.Vi[i * 3] + 0.5f * cam.Vi[i * 3 + 2]);
    Lw[1] = -1.0f; /* the tilt stays horizontal */
    float d[3] = {Lw[0], Lw[1], Lw[2]};
    for (int i = 0; i < 3; i++) R->light[i] = cam.V[i * 3] * d[0] + cam.V[i * 3 + 1] * d[1] + cam.V[i * 3 + 2] * d[2];
    float ll = sqrtf(R->light[0] * R->light[0] + R->light[1] * R->light[1] + R->light[2] * R->light[2]);
    for (int i = 0; i < 3; i++) R->light[i] /= ll;
    const float *l = R->light;
    float a[3] = {0, 0, 1};
    if (fabsf(l[2]) > 0.9f) { a[0] = 1; a[2] = 0; }
    float u[3] = {l[1] * a[2] - l[2] * a[1], l[2] * a[0] - l[0] * a[2], l[0] * a[1] - l[1] * a[0]};
    float ul = sqrtf(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
    for (int i = 0; i < 3; i++) u[i] /= ul;
    float v[3] = {l[1] * u[2] - l[2] * u[1], l[2] * u[0] - l[0] * u[2], l[0] * u[1] - l[1] * u[0]};
    memcpy(R->u, u, sizeof u);
    memcpy(R->v, v, sizeof v);
    /* the light map around the character: its joints seen along the light, plus the limbs' reach */
    float root[3];
    if (!char_root(c, buf, root)) return 0;
    memcpy(R->c, root, sizeof root);
    float ext = 0;
    for (int p = 0; p < m->nparts; p++) {
        if (!m->parts[p].nverts) continue;
        float j[3];
        bone_apply(bones[p], m->parts[p].jw, j);
        float dx = fabsf((j[0] - root[0]) * u[0] + (j[1] - root[1]) * u[1] + (j[2] - root[2]) * u[2]);
        float dy = fabsf((j[0] - root[0]) * v[0] + (j[1] - root[1]) * v[1] + (j[2] - root[2]) * v[2]);
        if (dx > ext) ext = dx;
        if (dy > ext) ext = dy;
    }
    R->ext = ext + 220.0f;
    R->dext = 4000.0f;
    R->fade = 1.5f * models_shadow_fade * top; /* the taps already soften: a gentler fade keeps it as dark as the flat one */
    R->soft = 0.4f * models_shadow_blur;
    R->bias = 6.0f;
    R->strength = models_shadow;
    /* the collision near the character (walls moved back to the painted wall, away from the character) */
    float wr[3];
    to_world(root, wr);
    float reach = 900.0f + top * models_shadow_tilt * 2.0f;
    int idx[256], ni = 0;
    float dist[256];
    for (int i = 0; i < ncfaces && ni < 256; i++) {
        CFace *f = &cfaces[i];
        float lo[3] = {1e9f, 1e9f, 1e9f}, hi[3] = {-1e9f, -1e9f, -1e9f};
        for (int k = 0; k < 4; k++)
            for (int x = 0; x < 3; x++) {
                if (f->c[k][x] < lo[x]) lo[x] = f->c[k][x];
                if (f->c[k][x] > hi[x]) hi[x] = f->c[k][x];
            }
        float dd = 0;
        for (int x = 0; x < 3; x++) {
            float e = wb[x] < lo[x] ? lo[x] - wb[x] : wb[x] > hi[x] ? wb[x] - hi[x] : 0.0f;
            dd += e * e;
        }
        if (dd > reach * reach) continue;
        /* collision is not the visible room: many walls are invisible boundaries (a platform's edge, a railing) and
         * some floors are ceilings or upper levels. A wall takes the shadow only when it stands behind the character
         * as the camera sees it (one in front would hide the floor under her, and could not be a visible wall without
         * hiding her too) and she is near it; a floor only when it lies at or below her. */
        if (f->wall) {
            float sc = f->n[0] * cam.eye[0] + f->n[1] * cam.eye[1] + f->n[2] * cam.eye[2] - f->d;
            float sr = f->n[0] * wb[0] + f->n[1] * wb[1] + f->n[2] * wb[2] - f->d;
            if ((sc > 0) != (sr > 0) || fabsf(sr) > 400.0f) continue;
            /* a barrier in open space (the helipad's, 4 Nov, the user's screenshot: the shadow stood up in mid-air and
             * the floor behind it lost its share) has walkable floor on both sides; a room's wall only on one */
            float y0 = 1e9f;
            for (int k = 0; k < 4; k++) if (f->c[k][1] < y0) y0 = f->c[k][1];
            int both = 0;
            for (int t = 1; t <= 3; t++) {
                float m[3] = {0, y0, 0};
                for (int x = 0; x < 3; x += 2) m[x] = f->c[0][x] + (f->c[1][x] - f->c[0][x]) * (float)t / 4.0f;
                float a[3] = {m[0] + 150.0f * f->n[0], y0, m[2] + 150.0f * f->n[2]};
                float b[3] = {m[0] - 150.0f * f->n[0], y0, m[2] - 150.0f * f->n[2]};
                if (floor_gap(a) < 80.0f && floor_gap(b) < 80.0f) both++;
            }
            if (both >= 2) continue;
        } else if (fabsf(f->n[1]) > 0.05f) {
            float yf = (f->d - f->n[0] * wb[0] - f->n[2] * wb[2]) / f->n[1];
            if (yf > wb[1] + 60.0f) continue;
        }
        idx[ni] = i; dist[ni++] = dd;
    }
    for (int i = 1; i < ni; i++) /* nearest first */
        for (int k = i; k > 0 && dist[k] < dist[k - 1]; k--) {
            float t = dist[k]; dist[k] = dist[k - 1]; dist[k - 1] = t;
            int ti = idx[k]; idx[k] = idx[k - 1]; idx[k - 1] = ti;
        }
    R->nq = 0;
    for (int i = 0; i < ni && R->nq < SH_MAXQ; i++) {
        CFace *f = &cfaces[idx[i]];
        float shift[3] = {0, 0, 0};
        if (f->wall) {
            float side = f->n[0] * wr[0] + f->n[1] * wr[1] + f->n[2] * wr[2] - f->d;
            float k = (side >= 0 ? -1.0f : 1.0f) * models_shadow_inset;
            for (int x = 0; x < 3; x++) shift[x] = k * f->n[x];
        }
        for (int k = 0; k < 4; k++) {
            float w[3] = {f->c[k][0] + shift[0], f->c[k][1] + shift[1], f->c[k][2] + shift[2]};
            to_view(w, R->q[R->nq][k]);
        }
        R->nq++;
    }
    /* prop remodels nearby take the shadow too */
    R->np = 0;
    for (int i = 0; i < ninst && R->np < SH_MAXP; i++) {
        Inst *o = &insts[i];
        MModel *om = &models[o->model];
        if (o == c || om->shadow || !om->mesh || om->mesh_state != 3 || om->nparts >= 8 || om->nparts > 20) continue;
        if (o->stamp[buf] + 2 < c->stamp[buf] || !o->has[buf][0]) continue;
        float pr[3];
        bone_apply(o->bones[buf][0], om->parts[0].jw, pr);
        float dx = pr[0] - root[0], dy = pr[1] - root[1], dz = pr[2] - root[2];
        if (dx * dx + dy * dy + dz * dz > reach * reach * 4) continue;
        R->pm[R->np] = om->mesh;
        R->pn[R->np] = om->nparts;
        for (int k = 0; k < om->nparts; k++)
            memcpy(R->pb[R->np] + 12 * k, o->has[buf][k] ? o->bones[buf][k] : o->bones[buf][0], 12 * sizeof(float));
        R->np++;
    }
    if (getenv("FE_DBG_SHADOW") && dbg_shadow == 0)
        for (int i = 0; i < R->nq; i++) {
            CFace *f = &cfaces[idx[i]];
            LOG("models: receiver %d %s world %.0f %.0f %.0f / %.0f %.0f %.0f -> view %.0f %.0f %.0f / %.0f %.0f %.0f (blob world %.0f %.0f %.0f)\n",
                i, f->wall ? "wall" : "floor", f->c[0][0], f->c[0][1], f->c[0][2], f->c[2][0], f->c[2][1], f->c[2][2], R->q[i][0][0],
                R->q[i][0][1], R->q[i][0][2], R->q[i][2][0], R->q[i][2][1], R->q[i][2][2], wb[0], wb[1], wb[2]);
        }
    if (getenv("FE_DBG_SHADOW") && dbg_shadow < 80) {
        dbg_shadow++;
        LOG("models: contact shadow of %s (area %c): %d quads of %d near, %d props, map %.0f, light %.2f %.2f %.2f\n", m->name,
            cam.area, R->nq, ni, R->np, R->ext, l[0], l[1], l[2]);
    }
    return R->nq > 0;
}

static void draw_shadow(Inst *c, int buf, float bones[][12], Shadow *out) {
    out->on = 0;
    out->recv = 0;
    if (models_shadow <= 0.0f) return;
    int id = (int)(c - insts) + 1;
    MModel *m = &models[c->model];
    for (int i = 0; i < ninst; i++) {
        Inst *s = &insts[i];
        if (!models[s->model].shadow || frame_count - s->stamp[buf] > 4 || shadow_link(s, buf) != id) continue;
        float pl[6], rad, quad[12];
        if (models[s->model].parts[0].nverts < 4 || !shadow_floor(s, buf, pl, pl + 3, &rad, quad)) continue;
        float r[3];
        if (!char_root(c, buf, r)) continue;
        float *n = pl + 3;
        if ((r[0] - pl[0]) * n[0] + (r[1] - pl[1]) * n[1] + (r[2] - pl[2]) * n[2] < 0) for (int k = 0; k < 3; k++) n[k] = -n[k];
        /* the light: from above, tilted so the body's shadow falls across the floor to the right and a little back
         * (view +x, +z / 2 along the floor): straight down it hides under the feet, straight back it is foreshortened */
        float g[3] = {1.0f, 0.0f, 0.5f}, gn = g[0] * n[0] + g[1] * n[1] + g[2] * n[2];
        float f[3] = {g[0] - gn * n[0], g[1] - gn * n[1], g[2] - gn * n[2]};
        float fl = sqrtf(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
        float L[3];
        for (int k = 0; k < 3; k++) L[k] = -n[k] + (fl > 1e-4f ? models_shadow_tilt * f[k] / fl : 0.0f);
        /* fade: a share of the character's height over the floor (its highest joint) */
        float top = 1.0f;
        for (int p = 0; p < m->nparts; p++) {
            if (!m->parts[p].nverts) continue;
            float j[3];
            bone_apply(bones[p], m->parts[p].jw, j);
            float u = (j[0] - pl[0]) * n[0] + (j[1] - pl[1]) * n[1] + (j[2] - pl[2]) * n[2];
            if (u > top) top = u;
        }
        float z = pl[2] > 1.0f ? pl[2] : 1.0f;
        if (dbg_shadow < 20 && getenv("FE_DBG_MODELS")) {
            dbg_shadow++;
            LOG("models: shadow of %s frame %llu: floor %.0f %.0f %.0f n %.2f %.2f %.2f r %.0f, height %.0f\n", m->name,
                (unsigned long long)frame_count, pl[0], pl[1], pl[2], n[0], n[1], n[2], rad, top);
        }
        out->on = 1;
        if (build_recv(c, buf, bones, pl, n, top, &out->R)) { /* the contact shadow; else the flat one below */
            out->recv = 1;
            hw_shadow_recv(m->mesh, &bones[0][0], m->nparts, c->h[buf], c->ofx[buf], c->ofy[buf], &out->R);
            return;
        }
        memcpy(out->plane, pl, sizeof pl);
        memcpy(out->light, L, sizeof L);
        memcpy(out->quad, quad, sizeof quad);
        out->fade = models_shadow_fade * top;
        out->blur = models_shadow_blur * c->h[buf] / z;
        hw_shadow_draw(m->mesh, &bones[0][0], m->nparts, c->h[buf], c->ofx[buf], c->ofy[buf], out->plane, out->light,
                       out->fade, out->blur, models_shadow, out->quad, models_shadow_contact);
        return;
    }
}

static uint32_t actor_light = 0x808080; /* scene actors' lit colour: the last character's mean */
static float actor_h, actor_ofx, actor_ofy; /* and the GTE projection it was drawn with */

/* FE_DBG_BONES=<table>:<file>: every drawn pose of that model (game frames: kind 0/1 = the buffer; 60 fps in-betweens:
 * kind 9), each part's GTE view bone (3x4) and whether the game drew it, then the projection (the tremble study) */
static void dbg_bones(const MModel *m, float (*bones)[12], const uint8_t *has, int kind, float h, float ofx, float ofy) {
    static FILE *bf;
    static char bname[32];
    static int binit;
    if (!binit) {
        binit = 1;
        const char *e = getenv("FE_DBG_BONES");
        if (e && strchr(e, ':')) {
            snprintf(bname, sizeof bname, "%.*s", (int)(strchr(e, ':') - e), e);
            bf = fopen(strchr(e, ':') + 1, "w");
        }
    }
    if (!bf || strcmp(m->name, bname)) return;
    fprintf(bf, "f %llu %d %d", (unsigned long long)frame_count, kind, m->nparts);
    for (int p = 0; p < m->nparts; p++) {
        fprintf(bf, "  %d", has ? has[p] : 1);
        for (int k = 0; k < 12; k++) fprintf(bf, " %.5g", bones[p][k]);
    }
    fprintf(bf, "  h %.2f %.2f %.2f\n", h, ofx, ofy);
    fflush(bf);
}

static void draw_instance(Inst *in, int buf) {
    MModel *m = &models[in->model];
    /* parts the game did not draw this frame follow their parent with the last relative pose */
    float bones[MAXPARTS][12];
    uint8_t ok[MAXPARTS];
    memcpy(bones, in->bones[buf], sizeof bones);
    memcpy(ok, in->has[buf], sizeof ok);
    /* the reference for root parts (FE1's enemies have four: torso, both thighs, hips): the first part drawn */
    int ref = -1;
    for (int p = 0; p < m->nparts && ref < 0; p++)
        if (ok[p] && m->parts[p].nverts) ref = p;
    if (ref < 0) return; /* nothing of the model drawn this frame */
    if (getenv("FE_DBG_MODELS")) {
        static int logged;
        char miss[64] = "";
        for (int p = 0; p < m->nparts; p++)
            if (!ok[p] && m->parts[p].nverts && strlen(miss) < 56) sprintf(miss + strlen(miss), " %d", p);
        if (miss[0] && logged++ < 40) LOG("models: %s frame %llu parts not drawn:%s\n", m->name, (unsigned long long)frame_count, miss);
    }
    for (int p = 0; p < m->nparts; p++) {
        int q = m->parts[p].parent >= 0 ? m->parts[p].parent : ref;
        if (ok[p] && ok[q] && q != p) {
            float inv[12];
            if (affine_inverse(bones[q], inv)) {
                affine_mul(inv, bones[p], in->rel[p]);
                in->rel_ok[p] = 1;
                in->rel_to[p] = (int8_t)q;
            }
        }
    }
    /* a part not drawn follows its parent (a root follows the reference) with the last relative pose seen; one
     * never seen takes the rest pose, the same transform (all bones carry the model-space joint). Without this a
     * root not drawn kept an all-zero bone and its vertices shot to the screen centre (FE1 soldiers, 2 Nov). */
    for (int pass = 0; pass < 8; pass++)
        for (int p = 0; p < m->nparts; p++) {
            int q = m->parts[p].parent >= 0 ? m->parts[p].parent : ref;
            if (ok[p] || !ok[q]) continue;
            if (in->rel_ok[p] && in->rel_to[p] == q) affine_mul(bones[q], in->rel[p], bones[p]);
            else memcpy(bones[p], bones[q], sizeof bones[p]);
            ok[p] = 1;
        }
    /* MFIX_JOINTS: the game rounds every part's translation to whole units on its own (truncated matrix products),
     * so a child's pivot sits up to 1 unit off its parent's from frame to frame: the head trembles on the neck, the
     * hands on the arms (measured 5 Nov on Rain's idle: every part's pivot gap 0..1 unit, mean -0.5, no gap beyond).
     * Each part is moved so that its pivot is exactly where its parent puts it (roots: where the reference part puts
     * them); a gap over 2 units is the game's own motion and is kept. Parents first. */
    if (model_fix & MFIX_JOINTS) {
        uint8_t done[MAXPARTS] = {0};
        done[ref] = 1;
        for (int pass = 0; pass < MAXPARTS; pass++) {
            int left = 0;
            for (int p = 0; p < m->nparts; p++) {
                if (done[p]) continue;
                int q = m->parts[p].parent >= 0 ? m->parts[p].parent : ref;
                if (q == p || q < 0 || q >= m->nparts) { done[p] = 1; continue; }
                if (!done[q]) { left++; continue; }
                done[p] = 1;
                if (!m->parts[p].nverts && m->parts[p].nfaces) continue;
                float a[3], b[3];
                bone_apply(bones[p], m->parts[p].jw, a);
                bone_apply(bones[q], m->parts[p].jw, b);
                float g = fabsf(a[0] - b[0]) + fabsf(a[1] - b[1]) + fabsf(a[2] - b[2]);
                if (g > 2.0f) continue;
                for (int r = 0; r < 3; r++) bones[p][r * 4 + 3] += b[r] - a[r];
            }
            if (!left) break;
        }
    }
    /* MFIX_STEADY: every part's translation is whole units (the game rounds each part's matrix product on its own), so
     * while she stands each part hops by a unit now and then, and the idle loop's first key puts the head a unit off
     * for one frame (Rain's gun idle, every 40 frames). Each part's pivot is low-passed while it moves slowly (under
     * 3 units a frame: rounding and breathing), followed at once beyond that (walks, turns, hits). */
    if (model_fix & MFIX_STEADY) {
        int reset = !in->steady_frame || frame_count - in->steady_frame > 6;
        int step = frame_count != in->steady_frame;
        for (int p = 0; p < m->nparts; p++) {
            float g[3];
            bone_apply(bones[p], m->parts[p].jw, g);
            float *f = in->steady[p];
            float dl = fabsf(g[0] - f[0]) + fabsf(g[1] - f[1]) + fabsf(g[2] - f[2]);
            if (reset || dl > 3.0f) memcpy(f, g, sizeof g);
            else if (step) { /* 0.35 for rounding (under a unit off), up to 1 at 2.5 units: a slow walk lags a unit at most */
                float k = 0.35f + 0.65f * fminf(fmaxf((dl - 1.0f) / 1.5f, 0.0f), 1.0f);
                for (int r = 0; r < 3; r++) f[r] += k * (g[r] - f[r]);
            }
            for (int r = 0; r < 3; r++) bones[p][r * 4 + 3] += f[r] - g[r];
        }
        in->steady_frame = frame_count;
    }
    /* FE_DBG_BONES=<table>:<file>: every drawn pose of that model, each part's GTE view bone (3x4), and whether the
     * game drew the part (the head tremble study, 5 Nov) */
    dbg_bones(m, bones, in->has[buf], buf, in->h[buf], in->ofx[buf], in->ofy[buf]);
    /* the lit colour of every original face, from its packet (word 1: code | rgb) */
    static uint32_t colors[2048];
    int nf = m->total_faces < 2048 ? m->total_faces : 2048;
    for (int p = 0; p < m->nparts; p++) {
        uint32_t P = in->P[buf][p];
        if (!P) continue;
        uint32_t sum[3] = {0, 0, 0}, n = 0;
        for (int f = 0; f < m->parts[p].nfaces; f++) {
            uint32_t gi = m->parts[p].face_base + (uint32_t)f;
            if (gi >= (uint32_t)nf) continue;
            uint32_t c = mem_r32(P + m->parts[p].faces[f].off + 4) & 0xFFFFFF;
            colors[gi] = c;
            if (c) { for (int k = 0; k < 3; k++) sum[k] += (c >> (8 * k)) & 0xFF; n++; }
        }
        /* faces never drawn yet (culled back faces) hold no colour: the mean of the part's others */
        uint32_t fill = n ? (sum[0] / n) | ((sum[1] / n) << 8) | ((sum[2] / n) << 16) : 0;
        for (int f = 0; f < m->parts[p].nfaces; f++) {
            uint32_t gi = m->parts[p].face_base + (uint32_t)f;
            if (gi < (uint32_t)nf && !colors[gi]) colors[gi] = fill;
        }
    }
    /* a part whose packets this actor never had (colors[] is shared by all models): the actor's mean colour, not
     * the faces of the model drawn before */
    if (!old_ident) {
        uint32_t s[3] = {0, 0, 0}, n = 0;
        for (int p = 0; p < m->nparts; p++)
            for (int f = 0; in->P[buf][p] && f < m->parts[p].nfaces; f++) {
                uint32_t gi = m->parts[p].face_base + (uint32_t)f;
                if (gi < (uint32_t)nf && colors[gi]) { for (int k = 0; k < 3; k++) s[k] += (colors[gi] >> (8 * k)) & 0xFF; n++; }
            }
        uint32_t mean = n ? (s[0] / n) | ((s[1] / n) << 8) | ((s[2] / n) << 16) : 0x808080;
        for (int p = 0; p < m->nparts; p++)
            for (int f = 0; !in->P[buf][p] && f < m->parts[p].nfaces; f++) {
                uint32_t gi = m->parts[p].face_base + (uint32_t)f;
                if (gi < (uint32_t)nf) colors[gi] = mean;
            }
    }
    if (getenv("FE_DBG_ACTORS") && cam_update()) { /* scene actors: a character's root in world terms (calibration) */
        static uint64_t last[MAXMODELS]; /* per model: every character is logged, not only the first drawn */
        if (frame_count - last[in->model] >= 30) {
            last[in->model] = frame_count;
            const float *b = bones[ref];
            float R[9], M[9], j[3], w[3];
            for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) R[r * 3 + c] = b[r * 4 + c];
            for (int r = 0; r < 3; r++)
                for (int c = 0; c < 3; c++) M[r * 3 + c] = cam.Vi[r * 3] * R[c] + cam.Vi[r * 3 + 1] * R[3 + c] + cam.Vi[r * 3 + 2] * R[6 + c];
            for (int r = 0; r < 3; r++) j[r] = b[r * 4 + 3] + R[r * 3] * m->parts[ref].jw[0] + R[r * 3 + 1] * m->parts[ref].jw[1] + R[r * 3 + 2] * m->parts[ref].jw[2];
            to_world(j, w);
            LOG("actors: %s frame %llu part %d world rot [%.3f %.3f %.3f / %.3f %.3f %.3f / %.3f %.3f %.3f] joint (%.0f %.0f %.0f) jw (%.0f %.0f %.0f) h %.0f of %.1f %.1f area %c\n",
                m->name, (unsigned long long)frame_count, ref, M[0], M[1], M[2], M[3], M[4], M[5], M[6], M[7], M[8], w[0], w[1], w[2],
                m->parts[ref].jw[0], m->parts[ref].jw[1], m->parts[ref].jw[2], in->h[buf], in->ofx[buf], in->ofy[buf], cam.area);
        }
    }
    /* FE_POSE_REC=<table>:<file>: that model's pose as the game animates it, every drawn frame, in world terms (each
     * part's rotation and joint): the source of scene actors' walks and runs (tools/actor_pose.py clip) */
    static FILE *rec_f;
    static char rec_name[32];
    static int rec_init;
    if (!rec_init) {
        rec_init = 1;
        const char *e = getenv("FE_POSE_REC");
        if (e && strchr(e, ':')) {
            snprintf(rec_name, sizeof rec_name, "%.*s", (int)(strchr(e, ':') - e), e);
            rec_f = fopen(strchr(e, ':') + 1, "w");
        }
    }
    if (rec_f && !strcmp(m->name, rec_name) && cam_update()) {
        fprintf(rec_f, "f %llu %d", (unsigned long long)frame_count, m->nparts);
        for (int p = 0; p < m->nparts; p++) {
            const float *b = bones[p];
            float R[9], M[9], j[3], w[3];
            for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) R[r * 3 + c] = b[r * 4 + c];
            for (int r = 0; r < 3; r++)
                for (int c = 0; c < 3; c++) M[r * 3 + c] = cam.Vi[r * 3] * R[c] + cam.Vi[r * 3 + 1] * R[3 + c] + cam.Vi[r * 3 + 2] * R[6 + c];
            for (int r = 0; r < 3; r++) j[r] = b[r * 4 + 3] + R[r * 3] * m->parts[p].jw[0] + R[r * 3 + 1] * m->parts[p].jw[1] + R[r * 3 + 2] * m->parts[p].jw[2];
            to_world(j, w);
            fprintf(rec_f, "  %d", ok[p] ? 1 : 0);
            for (int k = 0; k < 9; k++) fprintf(rec_f, " %.4f", M[k]);
            fprintf(rec_f, " %.1f %.1f %.1f", w[0], w[1], w[2]);
        }
        fprintf(rec_f, "\n");
        fflush(rec_f);
    }
    if (m->nparts >= 10) { /* a character's mean lit colour: the light of scene actors (drawn before the characters) */
        uint32_t s[3] = {0, 0, 0}, n = 0;
        for (int i = 0; i < nf; i++)
            if (colors[i]) { for (int k = 0; k < 3; k++) s[k] += (colors[i] >> (8 * k)) & 0xFF; n++; }
        if (n) actor_light = (s[0] / n) | ((s[1] / n) << 8) | ((s[2] / n) << 16);
        actor_h = in->h[buf]; actor_ofx = in->ofx[buf]; actor_ofy = in->ofy[buf];
    }
    if (model_fix & MFIX_STALE) {
        static uint32_t raw[2048];
        memcpy(raw, colors, sizeof(uint32_t) * (size_t)nf);
        int have = in->prevcol_frame && frame_count != in->prevcol_frame && frame_count - in->prevcol_frame <= 4;
        mlook_fix_colors(m->look, &bones[0][0], m->nparts, colors, nf, have ? in->prevcol : 0);
        if (frame_count != in->prevcol_frame) {
            memcpy(in->prevcol, raw, sizeof(uint32_t) * (size_t)nf);
            in->prevcol_frame = frame_count;
        }
    }
    float camsave[21];
    int camfixed = camfix_bones(bones, m->nparts, camsave); /* a moving camera: the picture's camera */
    uint32_t pid = smooth_on ? save_pose(in, bones, ok, colors, nf, buf) : 0;
    if (pid) smooth_mesh_event(pid); /* gpu_hw.c records from here (its snapshot is taken before the shadow) */
    Shadow sh = {0};
    if (is_shadow_caster(in)) draw_shadow(in, buf, bones, &sh);
    if (pid) POSE(pid)->sh = sh; /* the in-between frames draw it again with the blended pose */
    hw_mesh_draw(draw_mesh(m), &bones[0][0], m->nparts, colors, nf, in->h[buf], in->ofx[buf], in->ofy[buf]);
    if (camfixed) camfix_restore(camsave);
}

/* ------------------------------------------------------------------ in-between poses (60 fps) */

static void mat_quat(const float *m3, float *q) { /* rows of an orthonormal 3x3 -> x, y, z, w */
    float tr = m3[0] + m3[4] + m3[8];
    if (tr > 0) {
        float s = sqrtf(tr + 1.0f) * 2;
        q[3] = 0.25f * s; q[0] = (m3[7] - m3[5]) / s; q[1] = (m3[2] - m3[6]) / s; q[2] = (m3[3] - m3[1]) / s;
    } else if (m3[0] > m3[4] && m3[0] > m3[8]) {
        float s = sqrtf(1.0f + m3[0] - m3[4] - m3[8]) * 2;
        q[3] = (m3[7] - m3[5]) / s; q[0] = 0.25f * s; q[1] = (m3[1] + m3[3]) / s; q[2] = (m3[2] + m3[6]) / s;
    } else if (m3[4] > m3[8]) {
        float s = sqrtf(1.0f + m3[4] - m3[0] - m3[8]) * 2;
        q[3] = (m3[2] - m3[6]) / s; q[0] = (m3[1] + m3[3]) / s; q[1] = 0.25f * s; q[2] = (m3[5] + m3[7]) / s;
    } else {
        float s = sqrtf(1.0f + m3[8] - m3[0] - m3[4]) * 2;
        q[3] = (m3[3] - m3[1]) / s; q[0] = (m3[2] + m3[6]) / s; q[1] = (m3[5] + m3[7]) / s; q[2] = 0.25f * s;
    }
}

static void quat_mat(const float *q, float *m3) {
    float x = q[0], y = q[1], z = q[2], w = q[3];
    m3[0] = 1 - 2 * (y * y + z * z); m3[1] = 2 * (x * y - z * w); m3[2] = 2 * (x * z + y * w);
    m3[3] = 2 * (x * y + z * w); m3[4] = 1 - 2 * (x * x + z * z); m3[5] = 2 * (y * z - x * w);
    m3[6] = 2 * (x * z - y * w); m3[7] = 2 * (y * z + x * w); m3[8] = 1 - 2 * (x * x + y * y);
}

static float det3(const float *m) {
    return m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) + m[2] * (m[3] * m[7] - m[4] * m[6]);
}

/* m = r * s, r a rotation, s symmetric: the y row's 0.625 squash and flip (512-wide mode) and any scale go into s,
 * so r is a true rotation to slerp. Polar decomposition by Newton's iteration r = (r + r^-T) / 2. */
static int polar3(const float *m, float *r, float *s) {
    float a[9];
    memcpy(a, m, sizeof a);
    for (int it = 0; it < 30; it++) {
        float d = det3(a);
        if (fabsf(d) < 1e-9f) return 0;
        float it_[9] = { /* inverse transposed: the cofactors / det */
            (a[4] * a[8] - a[5] * a[7]) / d, (a[5] * a[6] - a[3] * a[8]) / d, (a[3] * a[7] - a[4] * a[6]) / d,
            (a[2] * a[7] - a[1] * a[8]) / d, (a[0] * a[8] - a[2] * a[6]) / d, (a[1] * a[6] - a[0] * a[7]) / d,
            (a[1] * a[5] - a[2] * a[4]) / d, (a[2] * a[3] - a[0] * a[5]) / d, (a[0] * a[4] - a[1] * a[3]) / d};
        float ch = 0;
        for (int i = 0; i < 9; i++) { float n = 0.5f * (a[i] + it_[i]); ch += fabsf(n - a[i]); a[i] = n; }
        if (ch < 1e-6f) break;
    }
    if (det3(a) < 0) for (int i = 0; i < 9; i++) a[i] = -a[i]; /* the flip: r a rotation, s = -(r^T m) */
    memcpy(r, a, sizeof a);
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) s[i * 3 + j] = r[i] * m[j] + r[3 + i] * m[3 + j] + r[6 + i] * m[6 + j];
    return 1;
}

/* a bone between a and b at t: rotation by slerp (the rest of the 3x3 by lerp), the joint's position by lerp; 0 = the
 * two are too far apart to blend (a camera cut, a teleport) */
static int blend_bone(const float *a, const float *b, const float *jw, float t, float *o) {
    float ja[3], jb[3], ma[9], mb[9], ra[9], rb[9], sa[9], sb[9];
    for (int i = 0; i < 3; i++) { /* the joint (where the part pivots) in view space */
        ja[i] = a[i * 4 + 3] + a[i * 4] * jw[0] + a[i * 4 + 1] * jw[1] + a[i * 4 + 2] * jw[2];
        jb[i] = b[i * 4 + 3] + b[i * 4] * jw[0] + b[i * 4 + 1] * jw[1] + b[i * 4 + 2] * jw[2];
    }
    float d2 = (ja[0] - jb[0]) * (ja[0] - jb[0]) + (ja[1] - jb[1]) * (ja[1] - jb[1]) + (ja[2] - jb[2]) * (ja[2] - jb[2]);
    if (d2 > 400.0f * 400.0f) return 0;
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++) { ma[r * 3 + c] = a[r * 4 + c]; mb[r * 3 + c] = b[r * 4 + c]; }
    if ((det3(ma) < 0) != (det3(mb) < 0) || !polar3(ma, ra, sa) || !polar3(mb, rb, sb)) return 0;
    float qa[4], qb[4], q[4];
    mat_quat(ra, qa);
    mat_quat(rb, qb);
    float dot = qa[0] * qb[0] + qa[1] * qb[1] + qa[2] * qb[2] + qa[3] * qb[3];
    if (dot < 0) { for (int i = 0; i < 4; i++) qb[i] = -qb[i]; dot = -dot; }
    if (dot < 0.5f) return 0; /* more than 120 degrees in one game frame */
    if (dot > 0.9995f) for (int i = 0; i < 4; i++) q[i] = qa[i] + (qb[i] - qa[i]) * t;
    else {
        float th = acosf(dot), s0 = sinf((1 - t) * th) / sinf(th), s1 = sinf(t * th) / sinf(th);
        for (int i = 0; i < 4; i++) q[i] = qa[i] * s0 + qb[i] * s1;
    }
    float n = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    for (int i = 0; i < 4; i++) q[i] /= n;
    float r3[9];
    quat_mat(q, r3);
    float s[9];
    for (int i = 0; i < 9; i++) s[i] = sa[i] + (sb[i] - sa[i]) * t;
    for (int r = 0; r < 3; r++) {
        float j = ja[r] + (jb[r] - ja[r]) * t;
        for (int c = 0; c < 3; c++) o[r * 4 + c] = r3[r * 3] * s[c] + r3[r * 3 + 1] * s[3 + c] + r3[r * 3 + 2] * s[6 + c];
        o[r * 4 + 3] = j - (o[r * 4] * jw[0] + o[r * 4 + 1] * jw[1] + o[r * 4 + 2] * jw[2]);
    }
    return 1;
}

int models_pose_kept(uint32_t id) { return id && POSE(id)->seq == id; }

/* save states: after a load the actors are found again from the game's next frame (their packet buffers, records
 * and poses belong to the RAM before the load); the 60 fps in-betweens start after the next two flips */
/* what the scene actors take from earlier frames: where the room camera lies in RAM (a scan finds the first struct
 * that looks like one, which can be a stale copy) and the last character's light and projection */
#include "state.h"
void models_state(StIO *io) {
    st_begin(io, "MODELS", 1);
    ST(io, cam.at); ST(io, actor_light); ST(io, actor_h); ST(io, actor_ofx); ST(io, actor_ofy);
    st_end(io);
    if (io->load && !io->dry) { cam.frame = 0; cam.scanned = 0; cam.ok = 0; acam.ok = 0; }
}

void models_state_loaded(void) {
    memset(insts, 0, sizeof insts);
    ninst = 0;
    memset(last_pkt, 0, sizeof last_pkt);
    last_serial = 0;
    if (owner) memset(owner, 0, 0x200000 / 4);
    memset(cache, 0, sizeof cache);
    for (int i = 0; i < MAXPOSE; i++) { poses[i].seq = 0; poses[i].in = 0; poses[i].next = 0; }
    run_inst = 0; run_model = -1; run_part = 0; run_buf = 0; run_frame = 0;
}

/* the pose an in-between blends towards: the same Inst's next pose of a later frame (at most 8 vblanks on). By the
 * Inst's own chain of poses (save_pose links each to the next), or, FE_POSE_RING_OLD / FE_CROWD_OLD, by a search of
 * the whole store as before 7 Nov (the earliest later frame; the same pose unless the ring wrapped) */
static Pose *pose_next(const Pose *a) {
    Pose *b = 0;
    if (pose_old()) {
        for (uint32_t i = 0; i < MAXPOSE_OLD; i++) {
            Pose *c = &poses[i];
            if (c->seq && c->in == a->in && c->model == a->model && c->frame > a->frame && c->frame - a->frame <= 8 &&
                (!b || c->frame < b->frame))
                b = c;
        }
        return b;
    }
    uint32_t id = a->next;
    for (int guard = 0; id && guard < 64; guard++) {
        Pose *c = POSE(id);
        if (c->seq != id || c->in != a->in) return 0; /* overwritten: no partner (drawn as is) */
        if (c->frame > a->frame) return c->model == a->model && c->frame - a->frame <= 8 ? c : 0;
        id = c->next; /* another pass of the same frame: the next one on */
    }
    return 0;
}

/* gpu_hw.c, rebuilding an in-between frame: pose id as drawn, posed at t towards the same actor's next drawn pose (the
 * game's next frame); without one, or if a part jumped, the pose as drawn */
void models_smooth_draw(uint32_t id, float t) {
    Pose *a = POSE(id);
    if (!id || a->seq != id) return;
    MModel *m = &models[a->model];
    if (!m->mesh && !a->spr) return;
    Pose *b = pose_next(a);
    if (a->spr) { /* a sprite actor: its angle (and place) between this frame's and the next */
        if (b && b->spr != a->spr) b = 0;
        static float B[12];
        int root = 0;
        for (int p = 0; p < m->nparts; p++) if (m->parts[p].parent < 0 || m->parts[p].parent >= m->nparts) { root = p; break; }
        memcpy(B, a->bones[root], sizeof B);
        float h = a->h, ofx = a->ofx, ofy = a->ofy, ang = a->spr_ang;
        if (b && blend_bone(a->bones[root], b->bones[root], m->parts[root].jw, t, B)) {
            h += (b->h - h) * t; ofx += (b->ofx - ofx) * t; ofy += (b->ofy - ofy) * t;
            ang += wrap180(b->spr_ang - ang) * t;
        } else memcpy(B, a->bones[root], sizeof B);
        sprite_draw(a->spr, B, a->spr_axis, a->spr_jw, ang, a->spr_mul, h, ofx, ofy, a->zback);
        return;
    }
    static float out[MAXPARTS][12];
    memcpy(out, a->bones, sizeof out);
    float h = a->h, ofx = a->ofx, ofy = a->ofy;
    int ok = b != 0;
    for (int p = 0; p < m->nparts && ok; p++)
        if (a->ok[p] && b->ok[p]) ok = blend_bone(a->bones[p], b->bones[p], m->parts[p].jw, t, out[p]);
    if (ok) {
        h += (b->h - h) * t; ofx += (b->ofx - ofx) * t; ofy += (b->ofy - ofy) * t;
    } else memcpy(out, a->bones, sizeof out);
    if (getenv("FE_DBG_SMOOTH"))
        LOG("smooth: %s pose of frame %llu -> %llu at %.2f%s\n", m->name, (unsigned long long)a->frame,
            b ? (unsigned long long)b->frame : 0ull, t, ok ? "" : " (not blended)");
    if (a->sh.on && a->sh.recv && models_shadow > 0.0f)
        hw_shadow_recv(m->mesh, &out[0][0], m->nparts, h, ofx, ofy, &a->sh.R);
    else if (a->sh.on && models_shadow > 0.0f)
        hw_shadow_draw(m->mesh, &out[0][0], m->nparts, h, ofx, ofy, a->sh.plane, a->sh.light, a->sh.fade, a->sh.blur,
                       models_shadow, a->sh.quad, models_shadow_contact);
    dbg_bones(m, out, 0, 9, h, ofx, ofy);
    look_push(a->look, a->shade, a->noink, a->zback, a->paint, a->spec);
    models_matte_tex = a->matte_tex;
    models_skin_dq = a->skin_dq;
    memcpy(models_matte_rect, a->matte_rect, sizeof models_matte_rect);
    hw_mesh_draw(draw_mesh(m), &out[0][0], m->nparts, a->colors, a->nf, h, ofx, ofy);
    look_pop();
}

/* ------------------------------------------------------------------ scene actors (NOTES.md "Scene actors")
 * A character painted into a room movie, taken out of the picture (tools/actor_plate.py) and drawn here with its
 * remodel instead: actors.txt in the models folder, "ACTOR <movie> <first> <last> <table> <pose file> <x> <y> <z>
 * <yaw>", while the 8x picture on screen is a frame of that range. The pose follows the movie frame
 * (tools/actor_pose.py); the bones are built as the game's (record_part) from the room camera. */
#define MAXACTORS_OLD 64 /* the fixed list before 7 Nov (FE_CROWD_OLD=1 / FE_ACTORS_OLD=1: lines past it dropped, logged) */
typedef struct {
    char movie[32];
    int first, last, model, nframes, sub;
    float pos[3], yaw;
    float h; /* the room's GTE projection distance (optional: else the last character's, which can be the room before) */
    int shadow; float floor_y; /* optional: the characters' soft shadow on the floor at this height (world y) */
    uint32_t light; /* optional "light=R,G,B" (128 = 1.0): a fixed lit colour, for a room whose light the last drawn
                     * character does not share (A65: Rain in the dark corner of the device); 0 = the characters' */
    float scale[3]; /* optional "scale=S" or "scale=X,Y,Z" (model axes): the remodel's size against the painted figure's
                     * (a painted figure drawn taller or broader than the remodel); default 1 */
    float spin[4];  /* optional "spin=X,Y,Z,DEG" (a room decoration: the machine room's cogwheels, NOTES "Cogwheel room"):
                     * the root turns about that model axis at DEG degrees a second of game time (vblanks / 60), so the
                     * angle is the game's own clock, the same after a save state; 0 = no spin */
    float osc[5];   /* optional "osc=AMP,STROKE,RAMP,DWELL,DELAY": the root turns AMP degrees (the sign: the first stroke's
                     * way) about the spin axis (default the model's z) and back, each stroke STROKE seconds with sinusoidal
                     * eases of RAMP seconds at both ends, held DWELL seconds at each end, the cycle shifted DELAY seconds;
                     * the game's clock, as spin=. A gear train: one line per wheel with the same timing and amplitudes
                     * in the tooth ratio (the wheels' angles stay locked to each other) */
    float phase;    /* optional "phase=DEG": a fixed turn about that axis (a gear's starting tooth position) */
    int turns;      /* spin, osc or phase given */
    float spin_ang; /* this pass's angle (degrees) */
    int look, noink; float shade; /* optional "ink=0" (no outline) and "shade=V" / "shade=smooth:V" (swap.txt's SHADE
                                   * for this mesh): a decoration painted into the room reads as part of the picture */
    ActorPaint paint; /* optional "paint=BLUR,AO,AO_R,GAIN,SAT,CON,GRAIN,DETAIL", "tint=R,G,B,K", "shadow=K,DX,DY,R"
                       * (models.h) */
    float spec;     /* optional "spec=V": the smooth shading's highlight x V (default 1) */
    char lookname[16]; /* optional "look=NAME": drawn only while the scene look (ini scene_look, Ctrl+F6) is NAME; lines
                        * without it always draw */
    float zback;    /* optional "zback=U" (view units): it sorts against the game's models U farther than drawn, at the
                     * surface it is painted on (the cog wheels: the machine face behind them); the picture is unchanged,
                     * only who covers whom */
    uint32_t *lights; int nlights; /* optional "lights=FILE": a lit colour per movie frame from the first ("R G B" lines,
                                    * 128 = 1.0), the room light baked into the movie (A44L's flicker) */
    Sprite *spr;    /* optional "sprite=FILE.spr": drawn as pre-rendered pictures by its angle (sprite_draw), the mesh
                     * only if they are missing; with lights= the pictures are taken as lit by the file's median
                     * colour (lref) and each frame's colour scales them per channel */
    float lref[3];
    float (*q)[MAXPARTS][4]; /* per pose, per part: the rotation from its parent's rest frame (x, y, z, w) */
    float (*root)[3];        /* per pose: the roots' offset (model units) */
    float (*move)[4];        /* per pose ("move" files: a walk): heading added to yaw (degrees), world offset */
    float (*cam)[8];         /* per pose ("cam" files: a moving camera): eye, target, roll (4096 = 360 deg), fov; the
                              * game refreshes its RAM copy only every third movie frame, a frame or two behind */
    int nomatte;             /* optional "matte=0": not hidden by its view's matte (MATTE lines) */
    int skin_dq;             /* optional "skin=dq": dual-quaternion skinning (gpu_hw.c) instead of the linear blend: no
                              * collapse at joints bent far (the party dancers' raised arms); every other mesh as before */
    Inst in;                 /* its entry in the pose store (60 fps in-between frames) */
} Actor;
/* every ACTOR line (NOTES "Scene actors: crowd capacity"): a list grown as actors.txt is read (was a fixed 64 before
 * 7 Nov, the rest dropped silently); never moved after load_actors (the pose store points at each line's Inst) */
static Actor *actors;
static int nactors, cap_actors;

/* ------------------------------------------------------------------ mattes (NOTES "Scene actors: crowd capacity")
 * The room picture has no depth, so nothing painted in it (a bar counter, a railing, a rope post) can cover a live
 * actor. A matte gives a view's foreground pixels a depth: actors.txt "MATTE <movie> <first> <last> <file.matte>",
 * while the shown picture is a frame of that range. The .matte file (text, relative to the models folder):
 *   MATTE1
 *   mask <png> <z>                 the PNG's covered pixels (alpha >= 128 when it has alpha, else luma >= 128) lie at
 *                                  view depth z (GTE view units: distance along the camera's axis)
 *   depth <png> <scale> [<bias>]   a 16-bit (or 8-bit) grey PNG: view z = value * scale + bias where value > 0
 * Every image is the movie's 8x picture (2560 x 1280 for 320 x 160; the same size for all layers); per pixel the
 * nearest layer counts. The mesh shader (gpu_hw.c) drops an actor's pixel whose depth (view z + zback) lies behind the
 * matte's at that point of the picture; the game's own models never see it. Read on a thread at the view's first
 * appearance (its actors wait for it, as for their meshes); at most MATTE_KEEP textures stay on the GPU. */
typedef struct {
    char movie[32];
    int first, last;
    char file[1400];
    volatile int state; /* 0 not asked, 1 reading, 2 depths ready, 3 texture made, 4 failed */
    float *z;
    int w, h;
    unsigned tex;
    uint64_t used; /* the vblank it was last drawn with */
} Matte;
#define MATTE_KEEP 4
#define MATTE_NONE 60000.0f /* no occluder (more than any view depth an actor is drawn at) */
static Matte *mattes;
static int nmattes;
extern unsigned short *stbi_load_16(const char *filename, int *x, int *y, int *comp, int req_comp);

static void matte_layer(Matte *mt, const char *dir, const char *png, int depth, float a, float b) {
    char p[1400];
    snprintf(p, sizeof p, "%s/%s", dir, png);
    int w = 0, h = 0, c = 0;
    unsigned short *px16 = 0;
    unsigned char *px8 = 0;
    if (depth) px16 = stbi_load_16(p, &w, &h, &c, 1);
    else px8 = stbi_load(p, &w, &h, &c, 0);
    if (!px16 && !px8) { LOG("actors: matte %s: layer %s cannot be read\n", mt->file, p); return; }
    if (!mt->z) {
        mt->w = w; mt->h = h;
        mt->z = malloc(sizeof(float) * (size_t)w * (size_t)h);
        if (mt->z) for (size_t i = 0; i < (size_t)w * h; i++) mt->z[i] = MATTE_NONE;
    }
    if (!mt->z || w != mt->w || h != mt->h) {
        LOG("actors: matte %s: layer %s is %d x %d, not %d x %d: left out\n", mt->file, p, w, h, mt->w, mt->h);
    } else {
        size_t n = (size_t)w * h, cov = 0;
        for (size_t i = 0; i < n; i++) {
            float z;
            if (depth) {
                if (!px16[i]) continue;
                z = (float)px16[i] * a + b;
            } else {
                const unsigned char *q = px8 + i * (size_t)c;
                int on = c == 4 ? q[3] >= 128 : c == 2 ? q[1] >= 128 : c == 3 ? (q[0] * 77 + q[1] * 150 + q[2] * 29) >= 128 * 256 : q[0] >= 128;
                if (!on) continue;
                z = a;
            }
            if (z < mt->z[i]) mt->z[i] = z;
            cov++;
        }
        LOG("actors: matte %s: %s %s, %.1f %% of the picture\n", mt->file, depth ? "depth" : "mask", png, 100.0 * (double)cov / (double)n);
    }
    if (px16) stbi_image_free(px16);
    if (px8) stbi_image_free(px8);
}

static int matte_reader(void *arg) {
    Matte *mt = arg;
    uint32_t t0 = SDL_GetTicks();
    FILE *f = fopen(mt->file, "r");
    char line[600], dir[1400];
    snprintf(dir, sizeof dir, "%s", mt->file);
    char *sl = strrchr(dir, '/');
    if (sl) *sl = 0; else snprintf(dir, sizeof dir, ".");
    int ok = f && fgets(line, sizeof line, f) && !strncmp(line, "MATTE1", 6);
    while (ok && fgets(line, sizeof line, f)) {
        char png[600];
        float a, b = 0.0f;
        if (sscanf(line, "mask %599s %f", png, &a) == 2) matte_layer(mt, dir, png, 0, a, 0.0f);
        else if (sscanf(line, "depth %599s %f %f", png, &a, &b) >= 2) matte_layer(mt, dir, png, 1, a, b);
    }
    if (f) fclose(f);
    if (!ok) LOG("actors: matte %s missing or not MATTE1\n", mt->file);
    LOG("actors: matte %s %s (%d x %d, %u ms on the loader thread)\n", mt->file, ok && mt->z ? "read" : "FAILED", mt->w, mt->h,
        SDL_GetTicks() - t0);
    SDL_MemoryBarrierRelease();
    mt->state = ok && mt->z ? 2 : 4;
    return 0;
}

/* 1 = its texture is there; 0 = not yet (asked); -1 = it cannot be had (the actors are drawn without it) */
static int matte_ready(Matte *mt) {
    if (mt->state == 0) {
        mt->state = 1;
        SDL_Thread *t = SDL_CreateThread(matte_reader, "matte", mt);
        if (t) SDL_DetachThread(t);
        else matte_reader(mt);
    }
    if (mt->state == 2) {
        SDL_MemoryBarrierAcquire();
        int held = 0; /* textures on the GPU: drop the longest unused (never one drawn in the last 16 vblanks) */
        for (int i = 0; i < nmattes; i++) held += mattes[i].state == 3;
        while (held >= MATTE_KEEP) {
            Matte *o = 0;
            for (int i = 0; i < nmattes; i++)
                if (mattes[i].state == 3 && mattes[i].used + 16 < frame_count && (!o || mattes[i].used < o->used)) o = &mattes[i];
            if (!o) break;
            hw_tex_free(o->tex);
            o->tex = 0; o->state = 0; held--; /* read again if its view comes back */
            LOG("actors: matte %s let go (unused since vblank %llu)\n", o->file, (unsigned long long)o->used);
        }
        mt->tex = hw_matte_tex(mt->z, mt->w, mt->h);
        free(mt->z);
        mt->z = 0;
        mt->state = mt->tex ? 3 : 4;
        if (mt->tex) LOG("actors: matte %s: texture %d x %d\n", mt->file, mt->w, mt->h);
    }
    return mt->state == 3 ? 1 : mt->state == 4 ? -1 : 0;
}

static int mattes_off(void) { static int d = -1; if (d < 0) d = getenv("FE_MATTE_OFF") != 0; return d; }
static char looks[8][16]; /* the look names actors.txt tags lines with, in order of appearance */
static int nlooks;
char models_scene_look[16];
void models_scene_look_cycle(void) {
    int k = 0;
    while (k < nlooks && strcmp(looks[k], models_scene_look)) k++;
    snprintf(models_scene_look, sizeof models_scene_look, "%s", nlooks ? looks[(k + 1) % nlooks] : "");
    LOG("actors: scene look %s\n", models_scene_look);
}
static uint32_t actor_serial;

static void load_actors(void) {
    char p[1200];
    snprintf(p, sizeof p, "%s/actors.txt", host_dir);
    FILE *f = fopen(p, "r");
    if (!f) return;
    char line[1024];
    int lineno = 0, nlines = 0, dropped = 0;
    int capped = crowd_old() || getenv("FE_ACTORS_OLD") != 0;
    while (fgets(line, sizeof line, f)) {
        lineno++;
        if (!strncmp(line, "MATTE", 5) && (line[5] == ' ' || line[5] == '\t')) {
            Matte mt;
            memset(&mt, 0, sizeof mt);
            char file[600];
            if (sscanf(line, "MATTE %31s %d %d %599s", mt.movie, &mt.first, &mt.last, file) != 4) {
                LOG("actors: actors.txt line %d: MATTE line not understood: dropped\n", lineno);
                continue;
            }
            snprintf(mt.file, sizeof mt.file, "%s/%s", host_dir, file);
            Matte *nm = realloc(mattes, sizeof *mattes * (size_t)(nmattes + 1));
            if (!nm) { LOG("actors: actors.txt line %d: no memory for a matte: dropped\n", lineno); continue; }
            mattes = nm;
            mattes[nmattes++] = mt;
            LOG("actors: matte for %s frames %d-%d: %s\n", mt.movie, mt.first, mt.last, file);
            continue;
        }
        if (strncmp(line, "ACTOR", 5)) continue; /* comments, blank lines */
        nlines++;
        if (capped && nactors >= MAXACTORS_OLD) {
            dropped++;
            LOG("actors: actors.txt line %d dropped: the old list holds %d (FE_CROWD_OLD / FE_ACTORS_OLD)\n", lineno, MAXACTORS_OLD);
            continue;
        }
        Actor a;
        memset(&a, 0, sizeof a);
        char table[64], pose[300];
        int got = sscanf(line, "ACTOR %31s %d %d %63s %299s %f %f %f %f %f %f", a.movie, &a.first, &a.last, table, pose,
                         &a.pos[0], &a.pos[1], &a.pos[2], &a.yaw, &a.h, &a.floor_y);
        if (got < 9) { dropped++; LOG("actors: actors.txt line %d: fewer than 9 fields: dropped\n", lineno); continue; }
        if (strstr(line, " matte=0")) a.nomatte = 1;
        if (strstr(line, " skin=dq")) a.skin_dq = 1;
        a.shadow = got >= 11;
        const char *lt = strstr(line, " light=");
        int lr, lg, lb;
        if (lt && sscanf(lt + 7, "%d,%d,%d", &lr, &lg, &lb) == 3)
            a.light = (uint32_t)(lr & 255) | ((uint32_t)(lg & 255) << 8) | ((uint32_t)(lb & 255) << 16) | 0x1000000u;
        a.scale[0] = a.scale[1] = a.scale[2] = 1.0f;
        const char *sc = strstr(line, " scale=");
        if (sc) {
            int ns = sscanf(sc + 7, "%f,%f,%f", &a.scale[0], &a.scale[1], &a.scale[2]);
            if (ns == 1) a.scale[1] = a.scale[2] = a.scale[0];
            else if (ns != 3) a.scale[0] = a.scale[1] = a.scale[2] = 1.0f;
        }
        const char *sp = strstr(line, " spin=");
        if (sp && sscanf(sp + 6, "%f,%f,%f,%f", &a.spin[0], &a.spin[1], &a.spin[2], &a.spin[3]) == 4) {
            float l = sqrtf(a.spin[0] * a.spin[0] + a.spin[1] * a.spin[1] + a.spin[2] * a.spin[2]);
            if (l > 1e-6f) for (int i = 0; i < 3; i++) a.spin[i] /= l;
            else a.spin[3] = 0.0f;
        } else { a.spin[0] = a.spin[1] = 0.0f; a.spin[2] = 1.0f; a.spin[3] = 0.0f; }
        const char *os = strstr(line, " osc=");
        if (!os || sscanf(os + 5, "%f,%f,%f,%f,%f", &a.osc[0], &a.osc[1], &a.osc[2], &a.osc[3], &a.osc[4]) != 5 ||
            a.osc[1] <= 0.0f)
            memset(a.osc, 0, sizeof a.osc);
        if (a.osc[2] > a.osc[1] * 0.5f) a.osc[2] = a.osc[1] * 0.5f;
        if (a.osc[2] < 0.0f) a.osc[2] = 0.0f;
        if (a.osc[3] < 0.0f) a.osc[3] = 0.0f;
        const char *ph = strstr(line, " phase=");
        a.phase = ph ? (float)atof(ph + 7) : 0.0f;
        a.turns = a.spin[3] != 0.0f || a.osc[0] != 0.0f || a.phase != 0.0f;
        a.shade = models_shade;
        if (strstr(line, " ink=0")) a.look = a.noink = 1;
        const char *zb = strstr(line, " zback=");
        a.zback = zb ? (float)atof(zb + 7) : 0.0f;
        const char *spc = strstr(line, " spec=");
        a.spec = spc ? (float)atof(spc + 6) : 1.0f;
        const char *pt = strstr(line, " paint=");
        if (pt) {
            ActorPaint *q = &a.paint;
            q->gain = q->sat = q->con = 1.0f;
            q->tint[0] = q->tint[1] = q->tint[2] = 1.0f;
            if (sscanf(pt + 7, "%f,%f,%f,%f,%f,%f,%f,%f", &q->blur, &q->ao, &q->ao_r, &q->gain, &q->sat, &q->con, &q->grain,
                       &q->detail) >= 1)
                q->on = 1;
            const char *tn = strstr(line, " tint=");
            if (tn) sscanf(tn + 6, "%f,%f,%f,%f", &q->tint[0], &q->tint[1], &q->tint[2], &q->tint_k);
            const char *sd = strstr(line, " shadow=");
            if (sd) sscanf(sd + 8, "%f,%f,%f,%f", &q->shadow[0], &q->shadow[1], &q->shadow[2], &q->shadow[3]);
        }
        const char *lk = strstr(line, " look=");
        if (lk) {
            sscanf(lk + 6, "%15s", a.lookname);
            int seen = 0;
            for (int k = 0; k < nlooks; k++) seen |= !strcmp(looks[k], a.lookname);
            if (!seen && nlooks < 8) snprintf(looks[nlooks++], sizeof looks[0], "%s", a.lookname);
        }
        const char *sh = strstr(line, " shade=");
        if (sh) {
            float v;
            if (!strncmp(sh + 7, "smooth:", 7) && sscanf(sh + 14, "%f", &v) == 1) { a.shade = -v; a.look = 1; }
            else if (sscanf(sh + 7, "%f", &v) == 1) { a.shade = v; a.look = 1; }
        }
        char lf[300];
        const char *ls = strstr(line, " lights=");
        if (ls && sscanf(ls + 8, "%299s", lf) == 1) {
            snprintf(p, sizeof p, "%s/%s", host_dir, lf);
            FILE *lfp = fopen(p, "r");
            int lr2, lg2, lb2;
            while (lfp && fscanf(lfp, "%d %d %d", &lr2, &lg2, &lb2) == 3) {
                a.lights = realloc(a.lights, (size_t)(a.nlights + 1) * sizeof *a.lights);
                a.lights[a.nlights++] = (uint32_t)(lr2 & 255) | ((uint32_t)(lg2 & 255) << 8) | ((uint32_t)(lb2 & 255) << 16);
            }
            if (lfp) fclose(lfp);
            if (!a.nlights) LOG("actors: lights file %s missing or empty\n", p);
        }
        for (int c = 0; c < 3; c++) { /* the lights' median per channel (a sprite's pictures are lit with it) */
            int v[256], nv = a.nlights < 256 ? a.nlights : 256;
            for (int k = 0; k < nv; k++) v[k] = (int)(a.lights[k] >> (8 * c) & 255);
            for (int i = 1; i < nv; i++) for (int j = i; j > 0 && v[j - 1] > v[j]; j--) { int t = v[j]; v[j] = v[j - 1]; v[j - 1] = t; }
            a.lref[c] = nv ? (float)(nv & 1 ? v[nv / 2] : (v[nv / 2 - 1] + v[nv / 2]) * 0.5f) : 128.0f;
            if (a.lref[c] < 1.0f) a.lref[c] = 1.0f;
        }
        const char *sf = strstr(line, " sprite=");
        if (sf && sscanf(sf + 8, "%299s", lf) == 1) a.spr = sprite_load(host_dir, lf);
        a.model = -1;
        /* "RAIN=RAIN_uw1.hm": a costume variant (tools/actor_variant.py), the table's parts and joints with that mesh */
        char *eq = strchr(table, '=');
        if (eq) *eq++ = 0;
        for (int i = 0; i < nmodels; i++)
            if (!strcmp(models[i].name, table) && models[i].hm_file[0] && !models[i].variant) a.model = i;
        if (a.model < 0) { dropped++; LOG("actors: no remodel for %s (line %d dropped)\n", table, lineno); continue; }
        if (eq && *eq) {
            char file[1400];
            snprintf(file, sizeof file, "%s/%s", host_dir, eq);
            int v = -1;
            for (int i = 0; i < nmodels; i++)
                if (models[i].variant && !strcmp(models[i].hm_file, file)) v = i;
            if (v < 0 && nmodels < MAXMODELS) {
                FILE *hf = fopen(file, "rb");
                if (!hf) { dropped++; LOG("actors: no variant mesh %s (line %d dropped)\n", file, lineno); continue; }
                fclose(hf);
                v = nmodels++;
                models[v] = models[a.model]; /* parts and joints shared by value (the part tables are never freed) */
                models[v].variant = 1;
                models[v].mesh = 0;
                models[v].look = 0;
                models[v].alt = 0; /* a costume of its own: not the improved mesh of its table */
                models[v].mesh_state = 0;
                models[v].file = 0;
                snprintf(models[v].hm_file, sizeof models[v].hm_file, "%s", file);
                LOG("actors: %s variant %s\n", table, eq);
                for (int k = 0; k < nlook_files; k++) /* its own improved mesh (swap_look.txt "<variant>.hm <file>") */
                    if (!strcmp(look_files[k][0], eq)) { add_look(v, eq, look_files[k][1]); break; }
            }
            if (v < 0) { dropped++; LOG("actors: no room for the variant %s (line %d dropped)\n", eq, lineno); continue; }
            a.model = v;
        }
        snprintf(p, sizeof p, "%s/%s", host_dir, pose);
        FILE *pf = fopen(p, "r");
        int np = 0, nfr = 0, mv = 0;
        char hdr[200] = "";
        if (pf && fgets(hdr, sizeof hdr, pf)) sscanf(hdr, "parts %d frames %d", &np, &nfr);
        if (!pf || np != models[a.model].nparts || nfr < 1) {
            dropped++;
            LOG("actors: pose file %s missing or not for %s's %d parts (line %d dropped)\n", p, table, models[a.model].nparts, lineno);
            if (pf) fclose(pf);
            continue;
        }
        /* header options: "sub N" poses per movie frame (a game animation at 30 fps: 2), "move" (a heading and a
         * world offset per pose) */
        a.sub = strstr(hdr, " sub ") ? atoi(strstr(hdr, " sub ") + 5) : 1;
        if (a.sub < 1) a.sub = 1;
        mv = strstr(hdr, " move") != 0;
        int cm = strstr(hdr, " cam") != 0;
        a.q = calloc((size_t)nfr, sizeof *a.q);
        a.root = calloc((size_t)nfr, sizeof *a.root);
        if (mv) a.move = calloc((size_t)nfr, sizeof *a.move);
        if (cm) a.cam = calloc((size_t)nfr, sizeof *a.cam);
        int bad = 0;
        for (int fr = 0; fr < nfr && !bad; fr++) {
            for (int k = 0; k < np && !bad; k++)
                bad = fscanf(pf, "%f %f %f %f", &a.q[fr][k][0], &a.q[fr][k][1], &a.q[fr][k][2], &a.q[fr][k][3]) != 4;
            if (!bad) bad = fscanf(pf, "%f %f %f", &a.root[fr][0], &a.root[fr][1], &a.root[fr][2]) != 3;
            if (!bad && mv) bad = fscanf(pf, "%f %f %f %f", &a.move[fr][0], &a.move[fr][1], &a.move[fr][2], &a.move[fr][3]) != 4;
            for (int k = 0; k < 8 && !bad && cm; k++) bad = fscanf(pf, "%f", &a.cam[fr][k]) != 1;
        }
        fclose(pf);
        if (bad) {
            dropped++;
            LOG("actors: pose file %s cut short (line %d dropped)\n", p, lineno);
            free(a.q); free(a.root); free(a.move); free(a.cam);
            continue;
        }
        a.nframes = nfr;
        a.in.model = a.model;
        if (nactors == cap_actors) {
            int nc = cap_actors ? cap_actors * 2 : 64;
            Actor *na = realloc(actors, sizeof *actors * (size_t)nc);
            if (!na) { dropped++; LOG("actors: no memory for line %d: dropped\n", lineno); free(a.q); free(a.root); free(a.move); free(a.cam); continue; }
            actors = na; cap_actors = nc;
        }
        actors[nactors++] = a;
        LOG("actors: %s in %s frames %d-%d, %d poses, at (%.0f %.0f %.0f) yaw %.1f%s%s%s\n", table, a.movie, a.first, a.last, nfr,
            a.pos[0], a.pos[1], a.pos[2], a.yaw, a.lookname[0] ? ", look " : "", a.lookname, a.spr ? ", sprite" : "");
    }
    fclose(f);
    LOG("actors: %d ACTOR lines, %d in use, %d dropped%s; %d matte lines; %.1f KB for the list, pose store %u x %.1f KB = %.1f MB\n",
        nlines, nactors, dropped, dropped ? " (WARNING: see the lines above)" : "", nmattes, (double)sizeof(Actor) * cap_actors / 1024.0,
        pose_ring(), (double)sizeof(Pose) / 1024.0, (double)sizeof(Pose) * MAXPOSE / 1048576.0);
    /* the scene look: FE_SCENE_LOOK, else the ini's scene_look / --scene-look, else the built-in default: "b" (the cog
     * wheels' sprites, the user's pick of 5 Nov) when actors.txt names it, else "current" (the 5 Nov meshes) when it
     * names that, else the first look it names */
    const char *env = getenv("FE_SCENE_LOOK");
    if (env) snprintf(models_scene_look, sizeof models_scene_look, "%s", env);
    if (!models_scene_look[0] && nlooks) {
        static const char *const pref[] = {"b", "current"};
        int k = nlooks;
        for (int p = 0; p < 2 && k == nlooks; p++)
            for (k = 0; k < nlooks && strcmp(looks[k], pref[p]); k++) {}
        snprintf(models_scene_look, sizeof models_scene_look, "%s", looks[k < nlooks ? k : 0]);
    }
    if (nlooks) LOG("actors: scene look %s (%d named)\n", models_scene_look, nlooks);
}

/* the actor's bones at pose time t (movie frames from its first, looping): forward kinematics in model space from the
 * table's joints, turned and placed in the world, then into the room camera's view */
static void actor_bones(Actor *a, float t, float bones[][12], float *Vout, float *Eout, float *org) {
    MModel *m = &models[a->model];
    int n = a->nframes;
    t *= (float)a->sub;
    float ft = fmodf(t, (float)n);
    if (ft < 0) ft += (float)n;
    if (a->move) ft = t < 0 ? 0 : t > (float)(n - 1) ? (float)(n - 1) : t; /* a walk does not loop */
    int k0 = (int)ft % n, k1 = a->move ? (k0 + 1 < n ? k0 + 1 : k0) : (k0 + 1) % n;
    float u = ft - (float)(int)ft;
    float L[MAXPARTS][9], Rm[MAXPARTS][9], Jm[MAXPARTS][3], root[3];
    uint8_t done[MAXPARTS] = {0};
    for (int i = 0; i < 3; i++) root[i] = a->root[k0][i] + (a->root[k1][i] - a->root[k0][i]) * u;
    for (int p = 0; p < m->nparts; p++) {
        const float *qa = a->q[k0][p], *qb = a->q[k1][p];
        float q[4], d = qa[0] * qb[0] + qa[1] * qb[1] + qa[2] * qb[2] + qa[3] * qb[3], s = d < 0 ? -1.0f : 1.0f;
        for (int i = 0; i < 4; i++) q[i] = qa[i] + (s * qb[i] - qa[i]) * u; /* nlerp: neighbouring poses are close */
        float l = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
        if (l < 1e-6f) { q[0] = q[1] = q[2] = 0; q[3] = 1; l = 1; }
        for (int i = 0; i < 4; i++) q[i] /= l;
        quat_mat(q, L[p]);
        int pp = m->parts[p].parent;
        if (a->turns && (pp < 0 || pp >= m->nparts)) { /* a decoration's turn about its axis, before the pose */
            float h = a->spin_ang * 0.00872665f, s = sinf(h), sq[4] = {a->spin[0] * s, a->spin[1] * s, a->spin[2] * s, cosf(h)};
            float S[9], T[9];
            quat_mat(sq, S);
            for (int r = 0; r < 3; r++)
                for (int c = 0; c < 3; c++) T[r * 3 + c] = L[p][r * 3] * S[c] + L[p][r * 3 + 1] * S[3 + c] + L[p][r * 3 + 2] * S[6 + c];
            memcpy(L[p], T, sizeof T);
        }
    }
    for (int pass = 0; pass < MAXPARTS; pass++)
        for (int p = 0; p < m->nparts; p++) {
            if (done[p]) continue;
            int q = m->parts[p].parent;
            if (q < 0 || q >= m->nparts) {
                memcpy(Rm[p], L[p], sizeof Rm[p]);
                for (int i = 0; i < 3; i++) Jm[p][i] = m->parts[p].jw[i] + root[i];
            } else if (done[q]) {
                float d[3];
                for (int i = 0; i < 3; i++) d[i] = m->parts[p].jw[i] - m->parts[q].jw[i];
                for (int r = 0; r < 3; r++) {
                    for (int c = 0; c < 3; c++)
                        Rm[p][r * 3 + c] = Rm[q][r * 3] * L[p][c] + Rm[q][r * 3 + 1] * L[p][3 + c] + Rm[q][r * 3 + 2] * L[p][6 + c];
                    Jm[p][r] = Jm[q][r] + Rm[q][r * 3] * d[0] + Rm[q][r * 3 + 1] * d[1] + Rm[q][r * 3 + 2] * d[2];
                }
            } else continue;
            done[p] = 1;
        }
    /* model -> world: a turn about the vertical (Y up, as the game's characters: their root's world rotation is a
     * pure yaw, measured on Hana) */
    float yaw = a->yaw, off[3] = {0, 0, 0};
    if (a->move) { /* a walk: the heading and the world offset of this pose (headings are continuous, no wrap) */
        yaw += a->move[k0][0] + (a->move[k1][0] - a->move[k0][0]) * u;
        for (int i = 0; i < 3; i++) off[i] = a->move[k0][1 + i] + (a->move[k1][1 + i] - a->move[k0][1 + i]) * u;
    }
    float cy = cosf(yaw * 0.01745329f), sy = sinf(yaw * 0.01745329f);
    float W[9] = {cy, 0, sy, 0, 1, 0, -sy, 0, cy};
    for (int r = 0; r < 3; r++) /* the actor's size: the model scaled about its origin before the turn (normals are
                                 * normalised in the shader) */
        for (int c = 0; c < 3; c++) W[r * 3 + c] *= a->scale[c];
    /* the view: the room camera from RAM, or the pose's own (a moving camera, exact for the movie frame) */
    float Vv[9], E[3];
    memcpy(Vv, cam.V, sizeof Vv);
    memcpy(E, cam.eye, sizeof E);
    if (a->cam) {
        float c[8], f[3], r[3], up[3];
        for (int i = 0; i < 8; i++) c[i] = a->cam[k0][i] + (a->cam[k1][i] - a->cam[k0][i]) * u;
        for (int i = 0; i < 3; i++) f[i] = c[3 + i] - c[i];
        float fl = sqrtf(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
        r[0] = f[2]; r[1] = 0; r[2] = -f[0];
        float rl = sqrtf(r[0] * r[0] + r[2] * r[2]);
        if (fl > 1.0f && rl > 1e-4f) { /* as cam_read */
            for (int i = 0; i < 3; i++) { f[i] /= fl; r[i] /= rl; }
            up[0] = f[1] * r[2] - f[2] * r[1]; up[1] = f[2] * r[0] - f[0] * r[2]; up[2] = f[0] * r[1] - f[1] * r[0];
            float ang = c[6] * 6.2831853f / 4096.0f, cs = cosf(ang), sn = sinf(ang);
            for (int i = 0; i < 3; i++) {
                Vv[i] = cs * r[i] - sn * up[i];
                Vv[3 + i] = -0.625f * (sn * r[i] + cs * up[i]);
                Vv[6 + i] = f[i];
                E[i] = c[i];
            }
        }
    }
    for (int p = 0; p < m->nparts; p++) {
        float Rw[9], R[9], jwld[3], jv[3];
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 3; c++) Rw[r * 3 + c] = W[r * 3] * Rm[p][c] + W[r * 3 + 1] * Rm[p][3 + c] + W[r * 3 + 2] * Rm[p][6 + c];
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 3; c++) R[r * 3 + c] = Vv[r * 3] * Rw[c] + Vv[r * 3 + 1] * Rw[3 + c] + Vv[r * 3 + 2] * Rw[6 + c];
        for (int r = 0; r < 3; r++) jwld[r] = W[r * 3] * Jm[p][0] + W[r * 3 + 1] * Jm[p][1] + W[r * 3 + 2] * Jm[p][2] + a->pos[r] + off[r] - E[r];
        for (int r = 0; r < 3; r++) jv[r] = Vv[r * 3] * jwld[0] + Vv[r * 3 + 1] * jwld[1] + Vv[r * 3 + 2] * jwld[2];
        const float *j = m->parts[p].jw;
        for (int r = 0; r < 3; r++) {
            for (int c = 0; c < 3; c++) bones[p][r * 4 + c] = R[r * 3 + c];
            bones[p][r * 4 + 3] = jv[r] - (R[r * 3] * j[0] + R[r * 3 + 1] * j[1] + R[r * 3 + 2] * j[2]);
        }
    }
    memcpy(Vout, Vv, sizeof Vv);
    memcpy(Eout, E, sizeof E);
    for (int i = 0; i < 3; i++) org[i] = a->pos[i] + off[i];
}

/* a scene actor's soft shadow, as draw_shadow's flat one: the floor at the actor's floor height under its origin, a
 * quad round the feet, the light tilted the same way, the fade from its height over the floor */
#define ACTOR_SHADOW_R 110.0f /* half the quad (world units): about the game's blob under a character */
static void actor_shadow(Actor *a, MModel *m, float bones[][12], const float *V, const float *E, const float *org, float h,
                         float ofx, float ofy, Shadow *out) {
    memset(out, 0, sizeof *out);
    if (models_shadow <= 0.0f) return;
    float c[3] = {org[0], a->floor_y, org[2]}, pl[6], quad[12];
    #define TOVIEW(w, o) do { float d_[3] = {(w)[0] - E[0], (w)[1] - E[1], (w)[2] - E[2]}; \
        for (int r_ = 0; r_ < 3; r_++) (o)[r_] = V[r_ * 3] * d_[0] + V[r_ * 3 + 1] * d_[1] + V[r_ * 3 + 2] * d_[2]; } while (0)
    TOVIEW(c, pl);
    for (int k = 0; k < 4; k++) {
        float w[3] = {c[0] + ((k & 1) ? ACTOR_SHADOW_R : -ACTOR_SHADOW_R), c[1], c[2] + ((k & 2) ? ACTOR_SHADOW_R : -ACTOR_SHADOW_R)};
        TOVIEW(w, quad + 3 * k);
    }
    #undef TOVIEW
    float *n = pl + 3;
    for (int r = 0; r < 3; r++) n[r] = V[r * 3 + 1]; /* world up in view space */
    float nl = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    for (int r = 0; r < 3; r++) n[r] /= nl;
    float g[3] = {1.0f, 0.0f, 0.5f}, gn = g[0] * n[0] + g[1] * n[1] + g[2] * n[2];
    float f[3] = {g[0] - gn * n[0], g[1] - gn * n[1], g[2] - gn * n[2]};
    float fl = sqrtf(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    float top = 1.0f;
    for (int p = 0; p < m->nparts; p++) {
        if (!m->parts[p].nverts) continue;
        float j[3];
        bone_apply(bones[p], m->parts[p].jw, j);
        float u = (j[0] - pl[0]) * n[0] + (j[1] - pl[1]) * n[1] + (j[2] - pl[2]) * n[2];
        if (u > top) top = u;
    }
    out->on = 1;
    memcpy(out->plane, pl, sizeof pl);
    for (int k = 0; k < 3; k++) out->light[k] = -n[k] + (fl > 1e-4f ? models_shadow_tilt * f[k] / fl : 0.0f);
    memcpy(out->quad, quad, sizeof quad);
    out->fade = models_shadow_fade * top;
    out->blur = models_shadow_blur * h / (pl[2] > 1.0f ? pl[2] : 1.0f);
    hw_shadow_draw(m->mesh, &bones[0][0], m->nparts, h, ofx, ofy, out->plane, out->light, out->fade, out->blur, models_shadow,
                   out->quad, models_shadow_contact);
}

/* a stroke's eased progress 0..1 at x seconds of a T-second stroke with sinusoidal ramps of r seconds */
static double osc_stroke(double x, double T, double r) {
    if (x <= 0.0) return 0.0;
    if (x >= T) return 1.0;
    if (x > T * 0.5) return 1.0 - osc_stroke(T - x, T, r);
    double v = 1.0 / (T - r); /* the cruise speed that covers the stroke */
    if (r > 0.0 && x < r) return v * (x * 0.5 - r / (2.0 * 3.14159265358979) * sin(3.14159265358979 * x / r));
    return v * (r * 0.5 + (x - r));
}

/* a decoration's angle about its axis (degrees) at t seconds of the game's clock: phase + spin + oscillation */
static float actor_turn(const Actor *a, double t) {
    double ang = (double)a->phase + fmod((double)a->spin[3] * t, 360.0);
    if (a->osc[0] != 0.0f) {
        double T = a->osc[1], r = a->osc[2], w = a->osc[3], P = 2.0 * (T + w);
        double x = fmod(t - (double)a->osc[4], P);
        if (x < 0.0) x += P;
        double f = x < T ? osc_stroke(x, T, r) : x < T + w ? 1.0 : x < 2.0 * T + w ? 1.0 - osc_stroke(x - T - w, T, r) : 0.0;
        ang += (double)a->osc[0] * f;
    }
    return (float)fmod(ang, 360.0);
}

/* a sprite actor (actors.txt "sprite="): its root's place without the turn, the turn as an angle for the pictures */
static void actor_sprite(Actor *a, float t, int frame) {
    MModel *m = &models[a->model];
    static float bones[MAXPARTS][12];
    float Vv[9], E[3], org[3], ang = a->turns ? a->spin_ang : 0.0f;
    a->spin_ang = 0.0f;
    actor_bones(a, t, bones, Vv, E, org);
    a->spin_ang = ang;
    int root = 0;
    for (int p = 0; p < m->nparts; p++) if (m->parts[p].parent < 0 || m->parts[p].parent >= m->nparts) { root = p; break; }
    float mul[3] = {1.0f, 1.0f, 1.0f};
    if (a->nlights) {
        int k = frame - a->first;
        uint32_t lc = a->lights[k < 0 ? 0 : k >= a->nlights ? a->nlights - 1 : k];
        for (int c = 0; c < 3; c++) mul[c] = (float)(lc >> (8 * c) & 255) / a->lref[c];
    }
    float h = a->h > 0 ? a->h : actor_h, ofx = actor_ofx, ofy = actor_ofy;
    if (h < 1.0f) {
        h = (float)(gte_read_ctrl(26) & 0xFFFF);
        ofx = (float)(int32_t)gte_read_ctrl(24) / 65536.0f;
        ofy = (float)(int32_t)gte_read_ctrl(25) / 65536.0f;
    }
    a->in.h[0] = h; a->in.ofx[0] = ofx; a->in.ofy[0] = ofy;
    if (smooth_on) {
        uint8_t ok[MAXPARTS];
        memset(ok, 1, sizeof ok);
        static const uint32_t none[1] = {0};
        uint32_t pid = save_pose(&a->in, bones, ok, none, 0, 0);
        Pose *ps = POSE(pid);
        ps->spr = a->spr; ps->spr_ang = ang; ps->zback = a->zback;
        memcpy(ps->spr_mul, mul, sizeof mul);
        memcpy(ps->spr_axis, a->spin, sizeof ps->spr_axis);
        memcpy(ps->spr_jw, m->parts[root].jw, sizeof ps->spr_jw);
        smooth_mesh_event(pid);
    }
    sprite_draw(a->spr, bones[root], a->spin, m->parts[root].jw, ang, mul, h, ofx, ofy, a->zback);
}

/* the ACTOR lines of one movie, in file order (built when the shown movie changes: a crowd's 1000 lines are not
 * compared by name every pass) */
static int *by_movie, nby_movie, by_movie_mv = -1;
static uint64_t skin_from; /* FE_DBG_SKIN_FROM, FE_DBG_SKIN_ACTOR (the line's index in the list) */
static int skin_actor;
static void actors_of_movie(int mv, const char *name) {
    if (mv == by_movie_mv) return;
    by_movie_mv = mv;
    nby_movie = 0;
    if (!by_movie) by_movie = malloc(sizeof(int) * (size_t)(nactors ? nactors : 1));
    if (!by_movie) return;
    for (int i = 0; i < nactors; i++)
        if (!strcmp(actors[i].movie, name)) by_movie[nby_movie++] = i;
}

/* drawn once per GPU pass, right after the room's background, before the characters (the pass's depth buffer is
 * shared with them) */
static void actors_draw(void) {
    int frame, mv = movie_shown(&frame);
    if (mv < 0 || !cam_update()) return;
    const char *name = movie_name(mv);
    actor_cam_begin(mv, frame); /* every pass of a run (the actors' camera of the picture), drawn or waiting */
    /* the view's matte (MATTE lines): where the picture lies in VRAM this pass, and its texture */
    unsigned mtex = 0;
    float mrect[4] = {0, 0, 0, 0};
    for (int k = 0; k < nmattes && !mattes_off(); k++) {
        Matte *mt = &mattes[k];
        if (strcmp(mt->movie, name) || frame < mt->first || frame > mt->last) continue;
        int r = matte_ready(mt);
        if (r == 0) { actor_cam_end(); return; } /* being read: the view's actors wait for it (a frame or two at first) */
        if (r > 0 && movie_bg_rect(mrect)) { mtex = mt->tex; mt->used = frame_count; }
        else if (r > 0) {
            static uint64_t warned;
            if (frame_count - warned > 600) { warned = frame_count; LOG("actors: matte %s: the picture's place is not known: drawn without\n", mt->file); }
        }
        if (getenv("FE_DBG_MATTE")) {
            static int n;
            if (n++ < 200) LOG("matte: %s frame %d vblank %llu tex %u picture at %.2f %.2f - %.2f %.2f\n", name, frame,
                               (unsigned long long)frame_count, mtex, mrect[0], mrect[1], mrect[2], mrect[3]);
        }
        break;
    }
    hw_gpt_mark(0, 1); /* FE_GPU_TIME */
    int all = crowd_old() || getenv("FE_ACTORS_OLD") != 0;
    if (!all) actors_of_movie(mv, name);
    int n = all ? nactors : nby_movie;
    for (int j = 0; j < n; j++) {
        int i = all ? j : by_movie[j];
        Actor *a = &actors[i];
        if (strcmp(a->movie, name) || frame < a->first || frame > a->last) continue;
        if (a->lookname[0] && strcmp(a->lookname, models_scene_look)) continue; /* another look's line */
        MModel *m = &models[a->model];
        /* the time since the background's frame began (game_fe2.c: 4 vblanks per movie frame; a 60 fps movie: the
         * time of its in-between picture, so she stays locked to it) */
        float t = (float)(frame - a->first) + movie_shown_frac();
        if (a->turns) a->spin_ang = actor_turn(a, (double)frame_count / 60.0);
        int sr = a->spr ? sprite_ready(a->spr) : -1;
        if (sr == 0) continue; /* its pictures are being read (a frame or two at the room's first appearance) */
        if (sr > 0) { actor_sprite(a, t, frame); continue; }
        request_mesh(m);
        if (!mesh_ready(m)) continue;
        static float bones[MAXPARTS][12];
        static uint32_t cols[2048];
        uint8_t ok[MAXPARTS];
        float Vv[9], E[3], org[3];
        actor_bones(a, t, bones, Vv, E, org);
        memset(ok, 1, sizeof ok);
        int nf = m->total_faces < 2048 ? m->total_faces : 2048;
        uint32_t lc = a->light ? (a->light & 0xFFFFFF) : actor_light;
        if (a->nlights) { int k = frame - a->first; lc = a->lights[k < 0 ? 0 : k >= a->nlights ? a->nlights - 1 : k]; }
        for (int k = 0; k < nf; k++) cols[k] = lc;
        float h = a->h > 0 ? a->h : actor_h, ofx = actor_ofx, ofy = actor_ofy;
        if (h < 1.0f) {
            h = (float)(gte_read_ctrl(26) & 0xFFFF);
            ofx = (float)(int32_t)gte_read_ctrl(24) / 65536.0f;
            ofy = (float)(int32_t)gte_read_ctrl(25) / 65536.0f;
        }
        a->in.h[0] = h; a->in.ofx[0] = ofx; a->in.ofy[0] = ofy;
        unsigned atex = a->nomatte ? 0 : mtex;
        uint32_t pid = smooth_on ? save_pose(&a->in, bones, ok, cols, nf, 0) : 0;
        if (pid) {
            Pose *ps = POSE(pid);
            ps->look = a->look; ps->shade = a->shade; ps->noink = a->noink; ps->zback = a->zback;
            ps->paint = &a->paint; ps->spec = a->spec;
            ps->matte_tex = atex;
            ps->skin_dq = a->skin_dq;
            memcpy(ps->matte_rect, mrect, sizeof mrect);
            smooth_mesh_event(pid);
        }
        if (a->shadow) {
            Shadow sh;
            actor_shadow(a, m, bones, Vv, E, org, h, ofx, ofy, &sh);
            if (pid) POSE(pid)->sh = sh; /* the in-between frames draw it again with the blended pose */
        }
        look_push(a->look, a->shade, a->noink, a->zback, &a->paint, a->spec);
        models_matte_tex = atex;
        models_skin_dq = a->skin_dq;
        { /* FE_DBG_SKIN=FILE: the first drawn actor's vertices as the GPU skins them (tools/skin_ref.py check) */
            static int done = -1;
            static const char *fn;
            if (done < 0) {
                fn = getenv("FE_DBG_SKIN");
                const char *fr = getenv("FE_DBG_SKIN_FROM"), *ac = getenv("FE_DBG_SKIN_ACTOR");
                done = fn ? 0 : 1;
                skin_from = fr ? strtoull(fr, 0, 10) : 0;
                skin_actor = ac ? atoi(ac) : -1;
            }
            if (!done && frame_count >= skin_from && (skin_actor < 0 || skin_actor == i)) {
                done = 1;
                snprintf(models_skin_capture, sizeof models_skin_capture, "%s", fn);
                char p2[1300];
                snprintf(p2, sizeof p2, "%s.txt", fn);
                FILE *tf = fopen(p2, "w");
                if (tf) {
                    fprintf(tf, "mesh %s\nactor %d\nframe %llu\nmovie_frame %d\nskin %s\nnparts %d\n", m->hm_file, i,
                            (unsigned long long)frame_count, frame, a->skin_dq ? "dq" : "lbs", m->nparts);
                    fclose(tf);
                }
            }
        }
        memcpy(models_matte_rect, mrect, sizeof mrect);
        hw_mesh_draw(draw_mesh(m), &bones[0][0], m->nparts, cols, nf, h, ofx, ofy);
        look_pop();
        if (getenv("FE_DBG_ACTCAM")) { /* where its origin lands (native pixels): tools/research/actor_cut_jump.py */
            float d[3] = {org[0] - E[0], org[1] - E[1], org[2] - E[2]}, v[3];
            for (int r = 0; r < 3; r++) v[r] = Vv[r * 3] * d[0] + Vv[r * 3 + 1] * d[1] + Vv[r * 3 + 2] * d[2];
            float z = v[2] > 1.0f ? v[2] : 1.0f;
            LOG("actcam: actor %d %s %s %d frame %llu at %.2f %.2f\n", i, m->name, name, frame, (unsigned long long)frame_count,
                ofx + h * v[0] / z, ofy + h * v[1] / z);
        }
        if (getenv("FE_DBG_ACTORS")) {
            static int n;
            if (n++ < 400) LOG("actors: %s at movie frame %d (t %.2f) game frame %llu, light %06X, h %.0f, camera eye %.0f %.0f %.0f\n",
                               m->name, frame, t, (unsigned long long)frame_count, lc, h, cam.eye[0], cam.eye[1], cam.eye[2]);
        }
    }
    hw_gpt_mark(0, 0);
    actor_cam_end();
}

/* the GPU DMA is about to send the packet at RAM address a: 1 = leave it out of the upscaled picture */
int models_packet(uint32_t a) {
    if (nactors && models_on && actor_serial != models_dma_serial && movie_bg_drawn()) {
        actor_serial = models_dma_serial;
        actors_draw();
    }
    if (!owner || !models_on) return 0;
    a &= 0x1FFFFC;
    int id = owner[a >> 2];
    if (!id) return 0;
    Inst *in = &insts[id - 1];
    MModel *m = &models[in->model];
    if (m->shadow) { /* the blob: left out when its character draws a soft shadow (in draw_instance) */
        if (models_shadow <= 0.0f) return 0;
        for (int b = 0; b < 2; b++) {
            uint32_t P = in->P[b][0];
            if (P && a >= P && a < P + m->parts[0].prims_len)
                return frame_count - in->stamp[b] <= 4 && shadow_link(in, b) != 0;
        }
        return 0;
    }
    if (!m->hm_file[0]) return 0;
    /* which buffer: the packet lies in a part's area of buffer 0 or 1 */
    int buf = -1, part = -1;
    for (int p = 0; p < m->nparts && buf < 0; p++)
        for (int b = 0; b < 2; b++) {
            uint32_t P = in->P[b][p];
            if (P && a >= P && a < P + m->parts[p].prims_len) { buf = b; part = p; break; }
        }
    if (buf < 0 || frame_count - in->stamp[buf] > 4) return 0;
    if (!mesh_ready(m)) return 0;
    /* draw at the actor's nearest packet (the last one in the ordering table): what the game sorts between
     * its parts (a pickup like the fuse in front of her back) goes under it, what lies in front of all of
     * them still covers it. Without a pre-walk of this pass: at the first packet. */
    int at = last_serial == models_dma_serial && last_pkt[id] ? a == last_pkt[id] : 1;
    if (at && in->dma_drawn[buf] != models_dma_serial) {
        in->dma_drawn[buf] = models_dma_serial;
        draw_instance(in, buf);
    }
    return !(m->keep_parts >> part & 1); /* a skin the mesh cannot pose: the game's polygons (mesh_unposable) */
}

/* a GPU linked-list DMA starts at addr: find each actor's last packet in it */
void models_dma_begin(uint32_t addr) {
    if (!owner || !models_on || !ninst) return;
    memset(last_pkt, 0, sizeof last_pkt);
    last_serial = models_dma_serial;
    uint32_t a = addr & 0x1FFFFC;
    for (int guard = 0; guard < 1000000; guard++) {
        uint32_t hdr = mem_r32(0x80000000u | a);
        if (hdr >> 24) {
            int id = owner[a >> 2];
            if (id) last_pkt[id] = a;
        }
        if (hdr & 0x800000) break;
        a = hdr & 0x1FFFFC;
    }
}
