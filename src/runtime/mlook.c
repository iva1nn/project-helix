/* Look fixes for the remodels: see mlook.h. Pure CPU; mlook_build runs on the mesh loader thread. */
#include "mlook.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

int model_fix = MFIX_DEFAULT, model_fix_saved = MFIX_DEFAULT;

typedef struct { float pos[3], nrm[3], uv[2]; uint8_t joints[4], weights[4]; uint16_t face, material; } HV;

#define NBR 6
struct MLook {
    uint32_t nverts, nfaces;
    uint16_t *vf;  /* nverts x 4 */
    float *vn;     /* nverts x 3: shading normals, smoothed along the surface (cel bands) */
    uint8_t *vw;   /* nverts x 4 */
    /* per original face: model-space centre and normal, the bones of its most central vertex, neighbours */
    float (*fc)[3], (*fn)[3];
    uint8_t (*fj)[4], (*fw)[4];
    uint16_t (*nb)[NBR];
    float (*nw)[NBR];
    uint8_t *has; /* the face has vertices in the remodel */
};

uint32_t mlook_nverts(const MLook *l) { return l ? l->nverts : 0; }
const uint16_t *mlook_vfaces(const MLook *l) { return l->vf; }
const uint8_t *mlook_vweights(const MLook *l) { return l->vw; }
const float *mlook_vnormals(const MLook *l) { return l->vn; }

void mlook_free(MLook *l) {
    if (!l) return;
    free(l->vf); free(l->vw); free(l->vn); free(l->fc); free(l->fn); free(l->fj); free(l->fw); free(l->nb); free(l->nw); free(l->has);
    free(l);
}

/* binary heap of (dist, vertex) for the bounded Dijkstra */
typedef struct { float d; int32_t v; } HE;
static void hpush(HE *h, int *n, float d, int32_t v) {
    int i = (*n)++;
    while (i > 0) {
        int p = (i - 1) / 2;
        if (h[p].d <= d) break;
        h[i] = h[p];
        i = p;
    }
    h[i].d = d; h[i].v = v;
}
static HE hpop(HE *h, int *n) {
    HE top = h[0], last = h[--(*n)];
    int i = 0;
    for (;;) {
        int c = 2 * i + 1;
        if (c >= *n) break;
        if (c + 1 < *n && h[c + 1].d < h[c].d) c++;
        if (h[c].d >= last.d) break;
        h[i] = h[c];
        i = c;
    }
    h[i] = last;
    return top;
}

MLook *mlook_build(const uint8_t *d, long size, float R, float Rn, float Rh) {
    if (!d || size < 28 || memcmp(d, "FE2HM01", 8)) return 0;
    uint32_t cnt[5];
    memcpy(cnt, d + 8, 20);
    size_t o = 28 + (size_t)cnt[2] * 16;
    uint32_t nv = cnt[0], ni = cnt[1], nf = cnt[4];
    if (o + (size_t)nv * 44 + (size_t)ni * 4 > (size_t)size || !nv || !ni || nf == 0 || nf > 65535) return 0;
    const HV *v = (const HV *)(d + o);
    const uint32_t *idx = (const uint32_t *)(d + o + (size_t)nv * 44);
    MLook *l = calloc(1, sizeof *l);
    l->nverts = nv; l->nfaces = nf;
    l->vf = calloc((size_t)nv * 4, 2); l->vw = calloc((size_t)nv * 4, 1); l->vn = calloc((size_t)nv * 3, sizeof(float));
    if (Rn > R) Rn = R;
    if (Rh > R) Rh = R;
    /* the head: the bone whose vertices sit highest (model space y is up); its normals are smoothed more (Rh) */
    int headbone = -1;
    {
        double sy[256] = {0}, sn[256] = {0};
        for (uint32_t i = 0; i < nv; i++) if (v[i].weights[0] > 200) { sy[v[i].joints[0]] += v[i].pos[1]; sn[v[i].joints[0]] += 1; }
        double best = -1e30;
        for (int b = 0; b < 256; b++) if (sn[b] > 200 && sy[b] / sn[b] > best) { best = sy[b] / sn[b]; headbone = b; }
    }
    l->fc = calloc(nf, sizeof *l->fc); l->fn = calloc(nf, sizeof *l->fn);
    l->fj = calloc(nf, sizeof *l->fj); l->fw = calloc(nf, sizeof *l->fw);
    l->nb = calloc(nf, sizeof *l->nb); l->nw = calloc(nf, sizeof *l->nw);
    l->has = calloc(nf, 1);
    /* weld: vertices at (nearly) the same place are one surface point (UV seams, part shells meeting at a joint) */
    uint32_t hn = 1;
    while (hn < nv * 2) hn <<= 1;
    int32_t *head = malloc(sizeof(int32_t) * hn), *next = malloc(sizeof(int32_t) * nv), *wid = malloc(sizeof(int32_t) * nv);
    for (uint32_t i = 0; i < hn; i++) head[i] = -1;
    int32_t nw = 0;
    int32_t *rep = malloc(sizeof(int32_t) * nv); /* welded id -> first vertex */
    for (uint32_t i = 0; i < nv; i++) {
        int32_t q[3];
        uint32_t k = 2166136261u;
        for (int a = 0; a < 3; a++) { q[a] = (int32_t)lroundf(v[i].pos[a] * 20.0f); k = (k ^ (uint32_t)q[a]) * 16777619u; }
        int32_t j;
        for (j = head[k & (hn - 1)]; j >= 0; j = next[j]) {
            int same = 1;
            for (int a = 0; a < 3; a++) same &= (int32_t)lroundf(v[j].pos[a] * 20.0f) == q[a];
            if (same) break;
        }
        if (j >= 0) wid[i] = wid[j];
        else { wid[i] = nw; rep[nw++] = (int32_t)i; }
        next[i] = head[k & (hn - 1)];
        head[k & (hn - 1)] = (int32_t)i;
    }
    free(head); free(next);
    /* labels of each welded vertex (its duplicates' faces), CSR */
    int32_t *lstart = calloc((size_t)nw + 1, 4), *lab = malloc(sizeof(int32_t) * nv);
    for (uint32_t i = 0; i < nv; i++) lstart[wid[i] + 1]++;
    for (int32_t w = 0; w < nw; w++) lstart[w + 1] += lstart[w];
    {
        int32_t *fill = malloc(sizeof(int32_t) * (size_t)nw);
        memcpy(fill, lstart, sizeof(int32_t) * (size_t)nw);
        for (uint32_t i = 0; i < nv; i++) lab[fill[wid[i]]++] = v[i].face < nf ? v[i].face : 0;
        free(fill);
    }
    /* edges between welded vertices, CSR (both directions, duplicates harmless) */
    int32_t *estart = calloc((size_t)nw + 1, 4), *edge = malloc(sizeof(int32_t) * (size_t)ni * 2);
    float *elen = malloc(sizeof(float) * (size_t)ni * 2);
    for (uint32_t t = 0; t + 2 < ni; t += 3)
        for (int e = 0; e < 3; e++) {
            uint32_t a = idx[t + e], b = idx[t + (e + 1) % 3];
            if (a >= nv || b >= nv) continue;
            estart[wid[a] + 1]++;
            estart[wid[b] + 1]++;
        }
    for (int32_t w = 0; w < nw; w++) estart[w + 1] += estart[w];
    {
        int32_t *fill = malloc(sizeof(int32_t) * (size_t)nw);
        memcpy(fill, estart, sizeof(int32_t) * (size_t)nw);
        for (uint32_t t = 0; t + 2 < ni; t += 3)
            for (int e = 0; e < 3; e++) {
                uint32_t a = idx[t + e], b = idx[t + (e + 1) % 3];
                if (a >= nv || b >= nv) continue;
                float dx = v[a].pos[0] - v[b].pos[0], dy = v[a].pos[1] - v[b].pos[1], dz = v[a].pos[2] - v[b].pos[2];
                float L = sqrtf(dx * dx + dy * dy + dz * dz);
                int32_t A = wid[a], B = wid[b];
                edge[fill[A]] = B; elen[fill[A]++] = L;
                edge[fill[B]] = A; elen[fill[B]++] = L;
            }
        free(fill);
    }
    /* bounded Dijkstra from every welded vertex: the faces met within R, weighted (1 - d/R)^2 */
    float *dist = malloc(sizeof(float) * (size_t)nw);
    int32_t *stamp = malloc(sizeof(int32_t) * (size_t)nw);
    for (int32_t w = 0; w < nw; w++) stamp[w] = -1;
    int hcap = 1 << 16;
    HE *heap = malloc(sizeof(HE) * (size_t)hcap);
    uint16_t (*wf)[4] = calloc((size_t)nw, sizeof *wf);
    uint8_t (*ww)[4] = calloc((size_t)nw, sizeof *ww);
    enum { ML = 48 };
    /* each welded vertex's normal (its duplicates' mean) and whether it belongs to the head */
    float (*wn)[3] = calloc((size_t)nw, sizeof *wn);
    uint8_t *whead = calloc((size_t)nw, 1);
    for (uint32_t i = 0; i < nv; i++) {
        for (int a = 0; a < 3; a++) wn[wid[i]][a] += v[i].nrm[a];
        if (headbone >= 0 && v[i].joints[0] == headbone && v[i].weights[0] > 128) whead[wid[i]] = 1;
    }
    float (*sn)[3] = calloc((size_t)nw, sizeof *sn);
    for (int32_t s = 0; s < nw; s++) {
        int lf[ML], nl = 0;
        float rn = whead[s] ? Rh : Rn;
        float lw[ML];
        int hn2 = 0;
        dist[s] = 0; stamp[s] = s;
        hpush(heap, &hn2, 0, s);
        while (hn2) {
            HE e = hpop(heap, &hn2);
            if (e.d > dist[e.v]) continue;
            float k = 1.0f - e.d / R;
            k = k * k;
            for (int32_t j = lstart[e.v]; j < lstart[e.v + 1]; j++) {
                int f = lab[j], q;
                for (q = 0; q < nl && lf[q] != f; q++) {}
                if (q == nl) { if (nl == ML) continue; lf[nl] = f; lw[nl++] = 0; }
                lw[q] += k;
            }
            if (e.d < rn) { /* the shading normal: neighbours' normals, (1 - d/rn)^2 */
                float kn = 1.0f - e.d / rn;
                kn *= kn;
                for (int a = 0; a < 3; a++) sn[s][a] += kn * wn[e.v][a];
            }
            for (int32_t j = estart[e.v]; j < estart[e.v + 1]; j++) {
                int32_t u = edge[j];
                float nd = e.d + elen[j];
                if (nd >= R) continue;
                if (stamp[u] != s || nd < dist[u]) {
                    stamp[u] = s; dist[u] = nd;
                    if (hn2 < hcap) hpush(heap, &hn2, nd, u);
                }
            }
        }
        /* top 4 */
        float tw[4] = {0, 0, 0, 0};
        int tf[4] = {0, 0, 0, 0};
        for (int q = 0; q < nl; q++)
            for (int r = 0; r < 4; r++)
                if (lw[q] > tw[r]) {
                    for (int t = 3; t > r; t--) { tw[t] = tw[t - 1]; tf[t] = tf[t - 1]; }
                    tw[r] = lw[q]; tf[r] = lf[q];
                    break;
                }
        float sum = tw[0] + tw[1] + tw[2] + tw[3];
        int acc = 0;
        for (int r = 0; r < 4; r++) {
            wf[s][r] = (uint16_t)tf[r];
            int b = sum > 0 ? (int)lroundf(255.0f * tw[r] / sum) : (r == 0 ? 255 : 0);
            if (r == 3) b = 255 - acc;
            if (b < 0) b = 0;
            ww[s][r] = (uint8_t)b;
            acc += b;
        }
        if (sum <= 0) { wf[s][0] = (uint16_t)lab[lstart[s]]; ww[s][0] = 255; ww[s][1] = ww[s][2] = ww[s][3] = 0; }
    }
    for (uint32_t i = 0; i < nv; i++) {
        memcpy(&l->vf[i * 4], wf[wid[i]], 8);
        memcpy(&l->vw[i * 4], ww[wid[i]], 4);
        float *n = sn[wid[i]], nl = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        /* the head's normals are stored at length 2: the shader turns them partly toward the camera (cel faces) */
        float hs = whead[wid[i]] ? 2.0f : 1.0f;
        for (int a = 0; a < 3; a++) l->vn[i * 3 + a] = hs * (nl > 1e-6f ? n[a] / nl : v[i].nrm[a]);
    }
    /* per face: centre, normal (mean of its vertices' normals), neighbours (the blend weights its vertices give
     * other faces) */
    float *cnt_f = calloc(nf, sizeof(float));
    float (*acc_nb)[NBR] = calloc(nf, sizeof *acc_nb);
    for (uint32_t i = 0; i < nv; i++) {
        uint32_t f = v[i].face;
        if (f >= nf) continue;
        for (int a = 0; a < 3; a++) { l->fc[f][a] += v[i].pos[a]; l->fn[f][a] += v[i].nrm[a]; }
        cnt_f[f] += 1;
        const uint16_t *bf = wf[wid[i]];
        const uint8_t *bw = ww[wid[i]];
        for (int r = 0; r < 4; r++) {
            uint16_t g = bf[r];
            if (g == f || !bw[r]) continue;
            int q, free_q = -1, min_q = 0;
            for (q = 0; q < NBR; q++) {
                if (acc_nb[f][q] > 0 && l->nb[f][q] == g) break;
                if (acc_nb[f][q] <= 0 && free_q < 0) free_q = q;
                if (acc_nb[f][q] < acc_nb[f][min_q]) min_q = q;
            }
            if (q == NBR) { q = free_q >= 0 ? free_q : min_q; if (free_q < 0 && acc_nb[f][q] > bw[r]) continue; l->nb[f][q] = g; acc_nb[f][q] = 0; }
            acc_nb[f][q] += bw[r];
        }
    }
    for (uint32_t f = 0; f < nf; f++) {
        if (cnt_f[f] <= 0) continue;
        l->has[f] = 1;
        float nl = 0;
        for (int a = 0; a < 3; a++) { l->fc[f][a] /= cnt_f[f]; nl += l->fn[f][a] * l->fn[f][a]; }
        nl = sqrtf(nl);
        for (int a = 0; a < 3; a++) l->fn[f][a] = nl > 1e-6f ? l->fn[f][a] / nl : 0;
        for (int q = 0; q < NBR; q++) l->nw[f][q] = acc_nb[f][q];
    }
    /* the bones of the vertex nearest each face's centre */
    float *best = malloc(sizeof(float) * nf);
    for (uint32_t f = 0; f < nf; f++) best[f] = 1e30f;
    for (uint32_t i = 0; i < nv; i++) {
        uint32_t f = v[i].face;
        if (f >= nf) continue;
        float dd = 0;
        for (int a = 0; a < 3; a++) dd += (v[i].pos[a] - l->fc[f][a]) * (v[i].pos[a] - l->fc[f][a]);
        if (dd < best[f]) { best[f] = dd; memcpy(l->fj[f], v[i].joints, 4); memcpy(l->fw[f], v[i].weights, 4); }
    }
    free(best); free(cnt_f); free(acc_nb);
    free(wid); free(rep); free(lstart); free(lab); free(estart); free(edge); free(elen); free(dist); free(stamp); free(heap);
    free(wf); free(ww); free(wn); free(whead); free(sn);
    return l;
}

void mlook_fix_colors(const MLook *l, const float *bones, int nbones, uint32_t *col, int nf, const uint32_t *prev) {
    if (!l || nbones < 1) return;
    if ((uint32_t)nf > l->nfaces) nf = (int)l->nfaces;
    float r0 = sqrtf(bones[0] * bones[0] + bones[1] * bones[1] + bones[2] * bones[2]);
    float r1 = sqrtf(bones[4] * bones[4] + bones[5] * bones[5] + bones[6] * bones[6]);
    float ys = (r0 > 0.01f && r1 > 0.01f) ? r1 / r0 : 0.625f;
    static uint8_t stale[65536];
    for (int f = 0; f < nf; f++) {
        stale[f] = 0;
        if (!l->has[f]) continue;
        float p[3] = {0, 0, 0}, n[3] = {0, 0, 0}, ws = 0;
        for (int k = 0; k < 4; k++) {
            float w = l->fw[f][k] / 255.0f;
            int j = l->fj[f][k];
            if (w <= 0 || j >= nbones) continue;
            const float *b = bones + j * 12;
            for (int r = 0; r < 3; r++) {
                p[r] += w * (b[r * 4] * l->fc[f][0] + b[r * 4 + 1] * l->fc[f][1] + b[r * 4 + 2] * l->fc[f][2] + b[r * 4 + 3]);
                n[r] += w * (b[r * 4] * l->fn[f][0] + b[r * 4 + 1] * l->fn[f][1] + b[r * 4 + 2] * l->fn[f][2]);
            }
            ws += w;
        }
        if (ws <= 0) continue;
        /* unsquash y (the GTE view of the 512-wide mode) for a true angle */
        p[1] /= ys; n[1] /= ys;
        float pl = sqrtf(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]), nl = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (pl < 1e-3f || nl < 1e-6f) continue;
        float c = (p[0] * n[0] + p[1] * n[1] + p[2] * n[2]) / (pl * nl);
        stale[f] = c > -0.2f; /* facing away or edge-on: the game culled it (or nearly), its packet is old */
    }
    /* the game's culling is on the low-poly face, whose angle differs from the remodel's smooth surface: a face can be
     * culled while the remodel shows it facing. Such a face's colour differs between the two packet buffers (each holds
     * the colour of the last frame it was drawn in that buffer) where a drawn face's light changes smoothly: a jump of
     * more than 20 levels from the last frame marks it as well (both frames: the blink goes) */
    if (prev)
        for (int f = 0; f < nf; f++) {
            if (stale[f] || !prev[f] || !col[f]) continue;
            int d = 0;
            for (int k = 0; k < 3; k++) {
                int a = (int)((col[f] >> (8 * k)) & 0xFF) - (int)((prev[f] >> (8 * k)) & 0xFF);
                if (a < 0) a = -a;
                if (a > d) d = a;
            }
            if (d > 20) stale[f] = 1;
        }
    /* two passes: a stale face takes the weighted mean of its fresh neighbours (then of refilled ones) */
    for (int pass = 0; pass < 2; pass++)
        for (int f = 0; f < nf; f++) {
            if (!stale[f]) continue;
            float s[3] = {0, 0, 0}, ws = 0;
            for (int q = 0; q < NBR; q++) {
                int g = l->nb[f][q];
                float w = l->nw[f][q];
                if (w <= 0 || g >= nf || stale[g] == 1) continue;
                for (int k = 0; k < 3; k++) s[k] += w * (float)((col[g] >> (8 * k)) & 0xFF);
                ws += w;
            }
            if (ws <= 0) continue;
            col[f] = (uint32_t)lroundf(s[0] / ws) | ((uint32_t)lroundf(s[1] / ws) << 8) | ((uint32_t)lroundf(s[2] / ws) << 16);
            stale[f] = 2; /* refilled */
        }
}
