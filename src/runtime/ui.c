/* Drawing for the port's own screens (controls menu, short notices): filled rectangles and text in window
 * pixels, over the game picture, with the hardware renderer. Fonts are TrueType files baked into atlases at
 * start (stb_truetype); the menus use the remasters' fonts (SIL Open Font License): Special Elite for Fear
 * Effect 1's typewriter look, Barlow Condensed for Fear Effect 2, found in the data folder (fonts\), next to
 * the exe or in the repository's fonts\, else Windows' Arial Bold. */
#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>
#include <SDL2/SDL_opengl.h>
#include <stdlib.h>
#include <math.h>
#include "hw.h"
#include "ui.h"
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_assert(x) ((void)0) /* no assert(): it puts the build path into the exe */
#include "third_party/stb_truetype.h"

#include "ui_gl.h"
UiGl ui_gl;

#define ATLAS 1024
typedef struct { stbtt_bakedchar ch[96]; GLuint tex; float px; int ok; } Font;
static Font fonts[UI_FONTS];
static GLuint prog, vao, vbo, white;
static GLint u_size;
static int win_w, win_h;

typedef struct { float x, y, u, v; uint8_t r, g, b, a; } UV;
#define MAXQ 8192
static UV verts[MAXQ * 6];
static int nverts;
static GLuint batch_tex;

static const char *vs =
    "#version 330 core\n"
    "layout(location=0) in vec2 a_pos; layout(location=1) in vec2 a_uv; layout(location=2) in vec4 a_col;\n"
    "uniform vec2 u_size; out vec2 v_uv; out vec4 v_col;\n"
    "void main(){ gl_Position = vec4(a_pos.x / u_size.x * 2.0 - 1.0, 1.0 - a_pos.y / u_size.y * 2.0, 0.0, 1.0);\n"
    "  v_uv = a_uv; v_col = a_col; }\n";
static const char *fs =
    "#version 330 core\n"
    "uniform sampler2D u_tex; in vec2 v_uv; in vec4 v_col; out vec4 o;\n"
    "void main(){ o = vec4(v_col.rgb, v_col.a * texture(u_tex, v_uv).r); }\n";

static GLuint compile(GLenum t, const char *src) {
    GLuint s = ui_gl.CreateShader(t);
    ui_gl.ShaderSource(s, 1, &src, 0);
    ui_gl.CompileShader(s);
    return s;
}

static unsigned char *read_file(const char *path, long *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    *size = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *d = malloc((size_t)*size);
    if (d && fread(d, 1, (size_t)*size, f) != (size_t)*size) { free(d); d = 0; }
    fclose(f);
    return d;
}

static int load_font(Font *fn, const char *name, float px) {
    char p[1200];
    char *base = SDL_GetBasePath();
    char d[3][1100];
    snprintf(d[0], sizeof d[0], "%s/fonts", fe_data);            /* the data folder */
    snprintf(d[1], sizeof d[1], "%sfonts", base ? base : "");     /* older packages */
    snprintf(d[2], sizeof d[2], "%s../fonts", base ? base : "");  /* the repository's builds */
    if (base) SDL_free(base);
    unsigned char *ttf = 0;
    long size = 0;
    for (int i = fe_data[0] ? 0 : 1; i < 3 && !ttf; i++) {
        snprintf(p, sizeof p, "%s/%s", d[i], name);
        ttf = read_file(p, &size);
    }
    if (!ttf) {
        snprintf(p, sizeof p, "C:/Windows/Fonts/arialbd.ttf");
        ttf = read_file(p, &size);
        if (!ttf) return 0;
    }
    unsigned char *bmp = calloc(ATLAS * ATLAS, 1);
    int r = stbtt_BakeFontBitmap(ttf, 0, px, bmp, ATLAS, ATLAS, 32, 96, fn->ch);
    free(ttf);
    if (r == 0) { free(bmp); return 0; }
    glGenTextures(1, &fn->tex);
    glBindTexture(GL_TEXTURE_2D, fn->tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, ATLAS, ATLAS, 0, GL_RED, GL_UNSIGNED_BYTE, bmp);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    free(bmp);
    fn->px = px;
    fn->ok = 1;
    LOG("ui: font %s\n", p);
    return 1;
}

static int ready;

int ui_unavailable(void) { return ready < 0; } /* tried and failed (no hardware renderer or no font) */

static int init(void) {
    if (ready) return ready > 0;
    ready = -1;
    if (!hw_active || !ui_gl.CreateShader) return 0;
    prog = ui_gl.CreateProgram();
    ui_gl.AttachShader(prog, compile(GL_VERTEX_SHADER, vs));
    ui_gl.AttachShader(prog, compile(GL_FRAGMENT_SHADER, fs));
    ui_gl.LinkProgram(prog);
    u_size = ui_gl.GetUniformLocation(prog, "u_size");
    ui_gl.GenVertexArrays(1, &vao);
    ui_gl.BindVertexArray(vao);
    ui_gl.GenBuffers(1, &vbo);
    ui_gl.BindBuffer(GL_ARRAY_BUFFER, vbo);
    ui_gl.VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(UV), (void *)offsetof(UV, x));
    ui_gl.VertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(UV), (void *)offsetof(UV, u));
    ui_gl.VertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(UV), (void *)offsetof(UV, r));
    for (int i = 0; i < 3; i++) ui_gl.EnableVertexAttribArray((GLuint)i);
    uint8_t one = 255;
    glGenTextures(1, &white);
    glBindTexture(GL_TEXTURE_2D, white);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, 1, 1, 0, GL_RED, GL_UNSIGNED_BYTE, &one);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    int fe1 = !strcmp(FE_GAME, "fe1");
    load_font(&fonts[UI_TITLE], fe1 ? "SpecialElite-Regular.ttf" : "BarlowCondensed-Bold.ttf", 96.0f);
    load_font(&fonts[UI_BODY], fe1 ? "SpecialElite-Regular.ttf" : "BarlowCondensed-SemiBold.ttf", 64.0f);
    ready = fonts[UI_BODY].ok ? 1 : -1;
    return ready > 0;
}

static void flush(void) {
    if (!nverts) return;
    ui_gl.BindVertexArray(vao);
    ui_gl.BindBuffer(GL_ARRAY_BUFFER, vbo);
    ui_gl.BufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(sizeof(UV) * (size_t)nverts), verts, GL_STREAM_DRAW);
    ui_gl.ActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, batch_tex);
    glDrawArrays(GL_TRIANGLES, 0, nverts);
    nverts = 0;
}

static void quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, uint32_t rgba, GLuint tex) {
    if (tex != batch_tex) { flush(); batch_tex = tex; }
    if (nverts + 6 > MAXQ * 6) flush();
    uint8_t r = (uint8_t)(rgba >> 24), g = (uint8_t)(rgba >> 16), b = (uint8_t)(rgba >> 8), a = (uint8_t)rgba;
    UV q[4] = {{x0, y0, u0, v0, r, g, b, a}, {x1, y0, u1, v0, r, g, b, a}, {x0, y1, u0, v1, r, g, b, a}, {x1, y1, u1, v1, r, g, b, a}};
    int idx[6] = {0, 1, 2, 1, 3, 2};
    for (int i = 0; i < 6; i++) verts[nverts++] = q[idx[i]];
}

int ui_begin(int w, int h) {
    if (!init()) return 0;
    win_w = w; win_h = h;
    ui_gl.BindFramebuffer(GL_FRAMEBUFFER, ui_target_fbo);
    glViewport(0, 0, w, h);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    ui_gl.BlendEquation(GL_FUNC_ADD);
    ui_gl.BlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE);
    ui_gl.UseProgram(prog);
    ui_gl.Uniform2f(u_size, (float)w, (float)h);
    ui_gl.Uniform1i(ui_gl.GetUniformLocation(prog, "u_tex"), 0);
    batch_tex = white;
    nverts = 0;
    return 1;
}

void ui_end(void) {
    flush();
    glDisable(GL_BLEND);
}

void ui_rect(float x, float y, float w, float h, uint32_t rgba) { quad(x, y, x + w, y + h, 0, 0, 1, 1, rgba, white); }

float ui_text_width(int font, float size, const char *s) {
    Font *fn = fonts[font].ok ? &fonts[font] : &fonts[UI_BODY];
    if (!fn->ok) return 0;
    float x = 0, y = 0, scale = size / fn->px;
    for (; *s; s++) {
        int c = (unsigned char)*s;
        if (c < 32 || c >= 128) c = '?';
        stbtt_aligned_quad q;
        stbtt_GetBakedQuad(fn->ch, ATLAS, ATLAS, c - 32, &x, &y, &q, 1);
    }
    return x * scale;
}

/* text with its top-left corner at x, y (size = line height in pixels); returns the width */
float ui_text(int font, float x, float y, float size, uint32_t rgba, const char *s) {
    Font *fn = fonts[font].ok ? &fonts[font] : &fonts[UI_BODY];
    if (!fn->ok) return 0;
    float scale = size / fn->px;
    float cx = 0, cy = fn->px * 0.78f; /* baseline below the top */
    for (; *s; s++) {
        int c = (unsigned char)*s;
        if (c < 32 || c >= 128) c = '?';
        stbtt_aligned_quad q;
        stbtt_GetBakedQuad(fn->ch, ATLAS, ATLAS, c - 32, &cx, &cy, &q, 1);
        quad(x + q.x0 * scale, y + q.y0 * scale, x + q.x1 * scale, y + q.y1 * scale, q.s0, q.t0, q.s1, q.t1, rgba, fn->tex);
    }
    return cx * scale;
}
