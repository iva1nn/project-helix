/* Hardware renderer (OpenGL 3.3): replays the GPU's drawing at N x resolution for display.
 *
 * The software rasteriser (gpu.c) stays the exact native VRAM: texture source, CPU readbacks and
 * render-to-texture keep PS1 behaviour. Every primitive, fill, copy and CPU upload is mirrored here, in
 * order, into an upscaled copy of VRAM (an RGBA8 texture, 1024*S x 512*S, alpha = mask bit). Textures
 * are sampled from the native VRAM uploaded as an R16UI texture, so CLUT modes are decoded exactly.
 * The display area is shown from the upscaled copy. */
#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>
#include <SDL2/SDL_opengl.h>
#include <stdlib.h>
#include <math.h>
#include "hw.h"
#include "models.h"
#include "mlook.h"
#include "ui_gl.h"

/* ------------------------------------------------------------------ GL loader */

#define GLFUNCS(X) \
    X(PFNGLCREATESHADERPROC, glCreateShader) X(PFNGLSHADERSOURCEPROC, glShaderSource) \
    X(PFNGLCOMPILESHADERPROC, glCompileShader) X(PFNGLGETSHADERIVPROC, glGetShaderiv) \
    X(PFNGLGETSHADERINFOLOGPROC, glGetShaderInfoLog) X(PFNGLCREATEPROGRAMPROC, glCreateProgram) \
    X(PFNGLATTACHSHADERPROC, glAttachShader) X(PFNGLLINKPROGRAMPROC, glLinkProgram) \
    X(PFNGLGETPROGRAMIVPROC, glGetProgramiv) X(PFNGLGETPROGRAMINFOLOGPROC, glGetProgramInfoLog) \
    X(PFNGLUSEPROGRAMPROC, glUseProgram) X(PFNGLGETUNIFORMLOCATIONPROC, glGetUniformLocation) \
    X(PFNGLUNIFORM1IPROC, glUniform1i) X(PFNGLUNIFORM4FPROC, glUniform4f) X(PFNGLUNIFORM2FPROC, glUniform2f) \
    X(PFNGLGENVERTEXARRAYSPROC, glGenVertexArrays) X(PFNGLBINDVERTEXARRAYPROC, glBindVertexArray) \
    X(PFNGLGENBUFFERSPROC, glGenBuffers) X(PFNGLBINDBUFFERPROC, glBindBuffer) X(PFNGLBUFFERDATAPROC, glBufferData) \
    X(PFNGLVERTEXATTRIBPOINTERPROC, glVertexAttribPointer) X(PFNGLVERTEXATTRIBIPOINTERPROC, glVertexAttribIPointer) \
    X(PFNGLENABLEVERTEXATTRIBARRAYPROC, glEnableVertexAttribArray) \
    X(PFNGLGENFRAMEBUFFERSPROC, glGenFramebuffers) X(PFNGLBINDFRAMEBUFFERPROC, glBindFramebuffer) \
    X(PFNGLFRAMEBUFFERTEXTURE2DPROC, glFramebufferTexture2D) \
    X(PFNGLCHECKFRAMEBUFFERSTATUSPROC, glCheckFramebufferStatus) X(PFNGLBLITFRAMEBUFFERPROC, glBlitFramebuffer) \
    X(PFNGLACTIVETEXTUREPROC, glActiveTexture) X(PFNGLBLENDEQUATIONPROC, glBlendEquation) \
    X(PFNGLBLENDFUNCSEPARATEPROC, glBlendFuncSeparate) X(PFNGLBLENDCOLORPROC, glBlendColor_) \
    X(PFNGLUNIFORM4FVPROC, glUniform4fv) X(PFNGLUNIFORM1FPROC, glUniform1f) X(PFNGLUNIFORM3FPROC, glUniform3f) \
    X(PFNGLGENRENDERBUFFERSPROC, glGenRenderbuffers) X(PFNGLBINDRENDERBUFFERPROC, glBindRenderbuffer) \
    X(PFNGLGENERATEMIPMAPPROC, glGenerateMipmap) X(PFNGLRENDERBUFFERSTORAGEPROC, glRenderbufferStorage) X(PFNGLRENDERBUFFERSTORAGEMULTISAMPLEPROC, glRenderbufferStorageMultisample) X(PFNGLFRAMEBUFFERRENDERBUFFERPROC, glFramebufferRenderbuffer)

#define DECL(t, n) static t p_##n;
GLFUNCS(DECL)
#define glCreateShader p_glCreateShader
#define glShaderSource p_glShaderSource
#define glCompileShader p_glCompileShader
#define glGetShaderiv p_glGetShaderiv
#define glGetShaderInfoLog p_glGetShaderInfoLog
#define glCreateProgram p_glCreateProgram
#define glAttachShader p_glAttachShader
#define glLinkProgram p_glLinkProgram
#define glGetProgramiv p_glGetProgramiv
#define glGetProgramInfoLog p_glGetProgramInfoLog
#define glUseProgram p_glUseProgram
#define glGetUniformLocation p_glGetUniformLocation
#define glUniform1i p_glUniform1i
#define glUniform4f p_glUniform4f
#define glUniform2f p_glUniform2f
#define glGenVertexArrays p_glGenVertexArrays
#define glBindVertexArray p_glBindVertexArray
#define glGenBuffers p_glGenBuffers
#define glBindBuffer p_glBindBuffer
#define glBufferData p_glBufferData
#define glVertexAttribPointer p_glVertexAttribPointer
#define glVertexAttribIPointer p_glVertexAttribIPointer
#define glEnableVertexAttribArray p_glEnableVertexAttribArray
#define glGenFramebuffers p_glGenFramebuffers
#define glBindFramebuffer p_glBindFramebuffer
#define glFramebufferTexture2D p_glFramebufferTexture2D
#define glCheckFramebufferStatus p_glCheckFramebufferStatus
#define glBlitFramebuffer p_glBlitFramebuffer
#define glGenerateMipmap p_glGenerateMipmap
#define glActiveTexture p_glActiveTexture
#define glBlendEquation p_glBlendEquation
#define glBlendFuncSeparate p_glBlendFuncSeparate
#define glBlendColorX p_glBlendColor_
#define glUniform4fv p_glUniform4fv
#define glUniform1f p_glUniform1f
#define glUniform3f p_glUniform3f
#define glGenRenderbuffers p_glGenRenderbuffers
#define glBindRenderbuffer p_glBindRenderbuffer
#define glRenderbufferStorage p_glRenderbufferStorage
#define glRenderbufferStorageMultisample p_glRenderbufferStorageMultisample
#define glFramebufferRenderbuffer p_glFramebufferRenderbuffer

static void *getp(const char *n) {
    char b[64];
    snprintf(b, sizeof b, "%s", n);
    size_t l = strlen(b);
    if (l && b[l - 1] == '_') b[l - 1] = 0; /* glBlendColor_ -> glBlendColor (name clash with gl.h) */
    return SDL_GL_GetProcAddress(b);
}

static int load_gl(void) {
#define LOAD(t, n) p_##n = (t)getp(#n); \
    if (!p_##n) { LOG("hw renderer: missing %s\n", #n); return 0; }
    GLFUNCS(LOAD)
    return 1;
}

/* ------------------------------------------------------------------ state */

int hw_active;
int hw_scale = 4;
static SDL_GLContext glctx;
static SDL_Window *glwin;
static GLuint draw_fbo; /* where the drawing goes: fb_fbo, or the in-between frame while it is rebuilt */
static void smooth_touch(int x0, int x1, int y0, int y1);
static int mesh_area_set, mesh_area[6]; /* a replayed mesh: the drawing area it was recorded with */
void hw_gpt_mark(int slot, int begin);
static int gpt_on;
static GLuint fb_tex, fb_fbo, tmp_tex, tmp_fbo, vram_tex, prog_draw, prog_show, prog_up, vao, vbo, vao_empty;
static GLint u_pass, u_setmask, u_show_rect, u_up_rect, u_hi_loc, u_scale_loc, u_rep_loc, u_key_loc, u_mrect_loc, u_mask_loc;
static GLuint hud_mask_tex; /* the bottom HUD strip's box interiors, one texel per PS1 pixel */
static int hi_x0 = 1 << 30, hi_y0 = 1 << 30, hi_x1, hi_y1; /* native bbox of 15-bit texture reads this batch */
static int W, H; /* upscaled VRAM size */

typedef struct {
    float x, y, u, v;
    uint8_t r, g, b, a;
    int16_t tx, ty;       /* texpage base (16-bit units) */
    int16_t clut_x, clut_y;
    int32_t flags;        /* depth | textured<<4 | raw<<5 | semi<<6 | twin<<8 (20 bits) | replacement<<28 (2 bits) */
    float ru, rv;         /* texture pack image coordinates (0..1 over the replaced block) */
    float w;              /* perspective (pgxp; 1 = flat, as the PS1 draws) */
} HV;

#define MAXV (1 << 18)
static HV *verts;
static int nv;
typedef struct { int first, count, blend, pass, set_mask; int sx, sy, sw, sh; GLuint rtex; } DCmd;
#define MAXC 65536
static DCmd *cmds;
static int nc;
static int dirty_y0 = 0, dirty_y1 = 512; /* native VRAM rows to upload before drawing */

/* ------------------------------------------------------------------ shaders */

static const char *vs_draw =
    "#version 330 core\n"
    "layout(location=0) in vec2 a_pos; layout(location=1) in vec2 a_uv; layout(location=2) in vec4 a_col;\n"
    "layout(location=3) in ivec4 a_tex; layout(location=4) in int a_flags; layout(location=5) in vec2 a_ruv;\n"
    "layout(location=6) in float a_w;\n"
    "out vec4 v_col; out vec2 v_uv; flat out ivec4 v_tex; flat out int v_flags; noperspective out vec2 v_ruv;\n"
    "void main(){ gl_Position = vec4((a_pos.x/512.0-1.0) * a_w, (a_pos.y/256.0-1.0) * a_w, 0.0, a_w);\n"
    "  v_col=a_col; v_uv=a_uv; v_tex=a_tex; v_flags=a_flags; v_ruv=a_ruv; }\n";

static const char *fs_draw =
    "#version 330 core\n"
    "uniform usampler2D u_vram; uniform sampler2D u_hi; uniform sampler2D u_rep; uniform int u_scale; uniform int u_pass; uniform int u_setmask;\n"
    "in vec4 v_col; in vec2 v_uv; flat in ivec4 v_tex; flat in int v_flags; noperspective in vec2 v_ruv;\n"
    "out vec4 o;\n"
    "uint fetch(int x,int y){ return texelFetch(u_vram, ivec2(x & 1023, y & 511), 0).r; }\n"
    "void main(){\n"
    "  int depth = v_flags & 3; bool textured = (v_flags & 16) != 0, raw = (v_flags & 32) != 0, semi = (v_flags & 64) != 0;\n"
    "  int tw = (v_flags >> 8) & 0xFFFFF; int mx = tw & 31, my = (tw >> 5) & 31, ox = (tw >> 10) & 31, oy = (tw >> 15) & 31;\n"
    "  int rep = (v_flags >> 28) & 3;\n"
    "  vec3 c; float stp;\n"
    "  if (textured && rep == 1) {\n"
    "    /* texture pack, full colour: alpha 0 cut-out, ~128 semi-transparent texel, 255 opaque */\n"
    "    ivec2 rs = textureSize(u_rep, 0);\n"
    "    vec4 r = (v_flags & (1 << 30)) != 0 ? texelFetch(u_rep, clamp(ivec2(v_ruv * vec2(rs)), ivec2(0), rs - 1), 0)\n"
    "                                        : texture(u_rep, v_ruv);\n"
    "    if (r.a < 0.25) discard;\n"
    "    bool s = r.a < 0.75;\n"
    "    if (u_pass == 1 && s) discard; if (u_pass == 2 && !s) discard;\n"
    "    vec3 t5 = r.rgb * 31.0;\n"
    "    c = raw ? t5 : min(t5 * floor(v_col.rgb * 255.0 + 0.5) / 128.0, vec3(31.0));\n"
    "    o = vec4(c / 31.0, u_setmask != 0 ? 1.0 : (s ? 1.0 : 0.0));\n"
    "    return;\n"
    "  }\n"
    "  if (textured) {\n"
    "    /* u may reach 256 on a movie piece's extended edge (game_fe2.c): never wrap to column 0 */\n"
    "    vec2 uvc = min(v_uv, vec2(255.998));\n"
    "    ivec2 t = ivec2(floor(uvc + 0.0005)) & 255;\n"
    "    t.x = (t.x & ~(mx*8)) | ((ox & mx)*8); t.y = (t.y & ~(my*8)) | ((oy & my)*8);\n"
    "    uint texel;\n"
    "    float gain = 1.0;\n"
    "    if (rep == 2) {\n"
    "      /* texture pack index map: palette index in red, coloured with the live CLUT; alpha 254 = green is a gain */\n"
    "      ivec2 sz = textureSize(u_rep, 0);\n"
    "      vec4 m = texelFetch(u_rep, clamp(ivec2(v_ruv * vec2(sz)), ivec2(0), sz - 1), 0);\n"
    "      int i = int(m.r * 255.0 + 0.5) & (depth == 0 ? 15 : 255);\n"
    "      texel = fetch(v_tex.z + i, v_tex.w);\n"
    "      if (int(m.a * 255.0 + 0.5) == 254) gain = m.g * 255.0 / 128.0;\n"
    "      if (texel == 0u) discard;\n"
    "      bool s = texel >= 0x8000u;\n"
    "      if (u_pass == 1 && s) discard; if (u_pass == 2 && !s) discard;\n"
    "      stp = s ? 1.0 : 0.0;\n"
    "      vec3 t5 = min(vec3(float(texel & 31u), float((texel >> 5) & 31u), float((texel >> 10) & 31u)) * gain, vec3(31.0));\n"
    "      c = raw ? t5 : min(t5 * floor(v_col.rgb * 255.0 + 0.5) / 128.0, vec3(31.0));\n"
    "      o = vec4(c / 31.0, u_setmask != 0 ? 1.0 : stp);\n"
    "      return;\n"
    "    }\n"
    "    if (depth == 0) { uint w = fetch(v_tex.x + (t.x >> 2), v_tex.y + t.y); int i = int((w >> uint((t.x & 3)*4)) & 15u); texel = fetch(v_tex.z + i, v_tex.w); }\n"
    "    else if (depth == 1) { uint w = fetch(v_tex.x + (t.x >> 1), v_tex.y + t.y); int i = int((w >> uint((t.x & 1)*8)) & 255u); texel = fetch(v_tex.z + i, v_tex.w); }\n"
    "    else {\n"
    "      /* 15-bit textures come from the upscaled VRAM copy, with sub-texel position: render-to-texture\n"
    "       * and the remaster's 8x movie frames keep their resolution */\n"
    "      vec2 hp = (vec2(v_tex.x + t.x, v_tex.y + t.y) + fract(uvc + 0.0005)) * float(u_scale);\n"
    "      vec4 h = texelFetch(u_hi, ivec2(int(hp.x) % (1024 * u_scale), int(hp.y) % (512 * u_scale)), 0);\n"
    "      if (h.a < 0.5 && dot(h.rgb, vec3(1.0)) < 0.5 / 31.0) discard;\n"
    "      bool s = h.a >= 0.5;\n"
    "      if (u_pass == 1 && s) discard; if (u_pass == 2 && !s) discard;\n"
    "      stp = s ? 1.0 : 0.0;\n"
    "      vec3 th = h.rgb * 31.0;\n"
    "      c = raw ? th : min(th * floor(v_col.rgb * 255.0 + 0.5) / 128.0, vec3(31.0));\n"
    "      o = vec4(c / 31.0, u_setmask != 0 ? 1.0 : stp);\n"
    "      return;\n"
    "    }\n"
    "    if (texel == 0u) discard;\n"
    "    bool s = texel >= 0x8000u;\n"
    "    if (u_pass == 1 && s) discard; if (u_pass == 2 && !s) discard;\n"
    "    stp = s ? 1.0 : 0.0;\n"
    "    vec3 t5 = vec3(float(texel & 31u), float((texel >> 5) & 31u), float((texel >> 10) & 31u));\n"
    "    c = raw ? t5 : min(floor(t5 * floor(v_col.rgb * 255.0 + 0.5) / 128.0), vec3(31.0));\n"
    "  } else { c = floor(floor(v_col.rgb * 255.0 + 0.5) / 8.0); stp = semi ? 1.0 : 0.0; }\n"
    "  o = vec4(c / 31.0, u_setmask != 0 ? 1.0 : stp);\n"
    "}\n";

/* full-screen quad helpers: draw a rectangle given in native VRAM coordinates */
static const char *vs_quad =
    "#version 330 core\n"
    "uniform vec4 u_rect; out vec2 v_p;\n"
    "void main(){ vec2 c = vec2(gl_VertexID & 1, gl_VertexID >> 1); v_p = u_rect.xy + c * u_rect.zw;\n"
    "  gl_Position = vec4(c * 2.0 - 1.0, 0.0, 1.0); }\n";

/* CPU upload: copy native VRAM pixels (just written) into the upscaled copy */
static const char *fs_up =
    "#version 330 core\n"
    "uniform usampler2D u_vram; in vec2 v_p; out vec4 o;\n"
    "void main(){ uint w = texelFetch(u_vram, ivec2(int(floor(v_p.x)) & 1023, int(floor(v_p.y)) & 511), 0).r;\n"
    "  o = vec4(float(w & 31u)/31.0, float((w>>5)&31u)/31.0, float((w>>10)&31u)/31.0, float(w >> 15)); }\n";

/* display: sample the upscaled copy (v_p in native coordinates) */
static const char *fs_show =
    "#version 330 core\n"
    "uniform sampler2D u_fb; uniform sampler2D u_mask; uniform int u_key; uniform vec4 u_mrect; in vec2 v_p; out vec4 o;\n"
    "void main(){ vec3 c = texture(u_fb, v_p / vec2(1024.0, 512.0)).rgb;\n"
    "  /* HUD over the picture: its black is see-through, except inside the HUD's boxes (u_key 2: the box mask) */\n"
    "  if (u_key != 0 && max(c.r, max(c.g, c.b)) < 0.035 &&\n"
    "      (u_key == 1 || texture(u_mask, (v_p - u_mrect.xy) / u_mrect.zw).r < 0.5)) discard;\n"
    "  o = vec4(c, 1.0); }\n";

static GLuint compile(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, 0);
    glCompileShader(s);
    GLint ok;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[2048]; glGetShaderInfoLog(s, sizeof log, 0, log); LOG("hw renderer: shader: %s\n", log); return 0; }
    return s;
}

static GLuint link(const char *vs, const char *fs) {
    GLuint p = glCreateProgram();
    GLuint a = compile(GL_VERTEX_SHADER, vs), b = compile(GL_FRAGMENT_SHADER, fs);
    if (!a || !b) return 0;
    glAttachShader(p, a);
    glAttachShader(p, b);
    glLinkProgram(p);
    GLint ok;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) { char log[2048]; glGetProgramInfoLog(p, sizeof log, 0, log); LOG("hw renderer: link: %s\n", log); return 0; }
    return p;
}

static GLuint make_fb_tex(GLuint *fbo, int w, int h) {
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1, fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, *fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) LOG("hw renderer: framebuffer incomplete\n");
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    return t;
}

/* window: the SDL window to draw into (NULL = create a hidden one, for headless runs) */
int hw_init(SDL_Window *window, int scale) {
    hw_scale = scale < 1 ? 1 : scale > 8 ? 8 : scale;
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    if (!window) {
        if (SDL_WasInit(SDL_INIT_VIDEO) == 0 && SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) return 0;
        window = SDL_CreateWindow("fe2port", 0, 0, 64, 64, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
        if (!window) return 0;
    }
    glwin = window;
    glctx = SDL_GL_CreateContext(window);
    if (!glctx) { LOG("hw renderer: no OpenGL 3.3 context: %s\n", SDL_GetError()); return 0; }
    SDL_GL_SetSwapInterval(0);
    if (!load_gl()) return 0;
    ui_gl = (UiGl){(void *)p_glCreateShader, (void *)p_glShaderSource, (void *)p_glCompileShader, (void *)p_glCreateProgram,
                   (void *)p_glAttachShader, (void *)p_glLinkProgram, (void *)p_glUseProgram, (void *)p_glGetUniformLocation,
                   (void *)p_glUniform1i, (void *)p_glUniform2f, (void *)p_glGenVertexArrays, (void *)p_glBindVertexArray,
                   (void *)p_glGenBuffers, (void *)p_glBindBuffer, (void *)p_glBufferData, (void *)p_glVertexAttribPointer,
                   (void *)p_glEnableVertexAttribArray, (void *)p_glActiveTexture, (void *)p_glBlendFuncSeparate,
                   (void *)p_glBlendEquation, (void *)p_glBindFramebuffer};
    W = 1024 * hw_scale; H = 512 * hw_scale;
    fb_tex = make_fb_tex(&fb_fbo, W, H);
    draw_fbo = fb_fbo;
    tmp_tex = make_fb_tex(&tmp_fbo, W, H);
    glGenTextures(1, &vram_tex);
    glBindTexture(GL_TEXTURE_2D, vram_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R16UI, 1024, 512, 0, GL_RED_INTEGER, GL_UNSIGNED_SHORT, vram);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    prog_draw = link(vs_draw, fs_draw);
    prog_up = link(vs_quad, fs_up);
    prog_show = link(vs_quad, fs_show);
    if (!prog_draw || !prog_up || !prog_show) return 0;
    u_pass = glGetUniformLocation(prog_draw, "u_pass");
    u_setmask = glGetUniformLocation(prog_draw, "u_setmask");
    u_hi_loc = glGetUniformLocation(prog_draw, "u_hi");
    u_scale_loc = glGetUniformLocation(prog_draw, "u_scale");
    u_up_rect = glGetUniformLocation(prog_up, "u_rect");
    u_show_rect = glGetUniformLocation(prog_show, "u_rect");
    u_key_loc = glGetUniformLocation(prog_show, "u_key");
    u_mrect_loc = glGetUniformLocation(prog_show, "u_mrect");
    u_mask_loc = glGetUniformLocation(prog_show, "u_mask");
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(HV), (void *)offsetof(HV, x));
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(HV), (void *)offsetof(HV, u));
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(HV), (void *)offsetof(HV, r));
    glVertexAttribIPointer(3, 4, GL_SHORT, sizeof(HV), (void *)offsetof(HV, tx));
    glVertexAttribIPointer(4, 1, GL_INT, sizeof(HV), (void *)offsetof(HV, flags));
    glVertexAttribPointer(5, 2, GL_FLOAT, GL_FALSE, sizeof(HV), (void *)offsetof(HV, ru));
    glVertexAttribPointer(6, 1, GL_FLOAT, GL_FALSE, sizeof(HV), (void *)offsetof(HV, w));
    for (int i = 0; i < 7; i++) glEnableVertexAttribArray(i);
    u_rep_loc = glGetUniformLocation(prog_draw, "u_rep");
    glGenVertexArrays(1, &vao_empty);
    verts = malloc(sizeof(HV) * MAXV);
    cmds = malloc(sizeof(DCmd) * MAXC);
    hw_active = 1;
    LOG("hw renderer: OpenGL %s, %dx internal resolution (%dx%d VRAM)\n", (const char *)glGetString(GL_VERSION),
        hw_scale, W, H);
    return 1;
}

/* ------------------------------------------------------------------ recording */

void hw_mark_dirty(int y0, int y1) {
    if (y0 < 0) y0 = 0;
    if (y1 > 512) y1 = 512;
    if (dirty_y1 <= dirty_y0) { dirty_y0 = y0; dirty_y1 = y1; return; }
    if (y0 < dirty_y0) dirty_y0 = y0;
    if (y1 > dirty_y1) dirty_y1 = y1;
}

static void upload_vram(void) {
    if (dirty_y1 <= dirty_y0) return;
    glBindTexture(GL_TEXTURE_2D, vram_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 2);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, dirty_y0, 1024, dirty_y1 - dirty_y0, GL_RED_INTEGER, GL_UNSIGNED_SHORT,
                    &vram[dirty_y0 * 1024]);
    dirty_y0 = dirty_y1 = 0;
}

static void set_blend(int mode) {
    if (!mode) { glDisable(GL_BLEND); return; }
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    switch (mode) {
    case 1: glBlendColorX(0, 0, 0, 0.5f); glBlendFuncSeparate(GL_CONSTANT_ALPHA, GL_CONSTANT_ALPHA, GL_ONE, GL_ZERO); break;
    case 2: glBlendFuncSeparate(GL_ONE, GL_ONE, GL_ONE, GL_ZERO); break;
    case 3: glBlendEquation(GL_FUNC_REVERSE_SUBTRACT); glBlendFuncSeparate(GL_ONE, GL_ONE, GL_ONE, GL_ZERO); break;
    case 4: glBlendColorX(0, 0, 0, 0.25f); glBlendFuncSeparate(GL_CONSTANT_ALPHA, GL_ONE, GL_ONE, GL_ZERO); break;
    }
}

void hw_flush(void) {
    if (!hw_active) return;
    upload_vram();
    if (!nc) return;
    glBindFramebuffer(GL_FRAMEBUFFER, draw_fbo);
    glViewport(0, 0, W, H);
    if (hi_x1 > hi_x0) {
        /* copy the 15-bit texture sources of this batch out of the upscaled VRAM (no feedback loop) */
        int S = hw_scale;
        int x0 = hi_x0 < 0 ? 0 : hi_x0, y0 = hi_y0 < 0 ? 0 : hi_y0;
        int x1 = hi_x1 > 1024 ? 1024 : hi_x1, y1 = hi_y1 > 512 ? 512 : hi_y1;
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fb_fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, tmp_fbo);
        glBlitFramebuffer(x0 * S, y0 * S, x1 * S, y1 * S, x0 * S, y0 * S, x1 * S, y1 * S, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER, draw_fbo);
        hi_x0 = hi_y0 = 1 << 30; hi_x1 = hi_y1 = 0;
    }
    glUseProgram(prog_draw);
    glUniform1i(u_hi_loc, 1);
    glUniform1i(u_rep_loc, 2);
    glUniform1i(u_scale_loc, hw_scale);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, tmp_tex);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, vram_tex);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(sizeof(HV) * (size_t)nv), verts, GL_STREAM_DRAW);
    glEnable(GL_SCISSOR_TEST);
    GLuint bound_rep = (GLuint)-1;
    for (int i = 0; i < nc; i++) {
        DCmd *c = &cmds[i];
        if (c->rtex != bound_rep) { /* a texture pack image (texrep.c) */
            glActiveTexture(GL_TEXTURE2);
            glBindTexture(GL_TEXTURE_2D, c->rtex);
            glActiveTexture(GL_TEXTURE0);
            bound_rep = c->rtex;
        }
        set_blend(c->blend);
        glUniform1i(u_pass, c->pass);
        glUniform1i(u_setmask, c->set_mask);
        glScissor(c->sx * hw_scale, c->sy * hw_scale, c->sw * hw_scale, c->sh * hw_scale);
        glDrawArrays(GL_TRIANGLES, c->first, c->count);
    }
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    nv = nc = 0;
}

static void add_cmd(int first, int count, int blend, int pass, const HwState *st) {
    int sw = st->da_x2 - st->da_x1 + 1, sh = st->da_y2 - st->da_y1 + 1;
    if (sw <= 0 || sh <= 0) return;
    if (nc) {
        DCmd *l = &cmds[nc - 1];
        if (l->first + l->count == first && l->blend == blend && l->pass == pass && l->set_mask == st->set_mask &&
            l->sx == st->da_x1 && l->sy == st->da_y1 && l->sw == sw && l->sh == sh && l->rtex == st->rtex) {
            l->count += count;
            return;
        }
    }
    if (nc >= MAXC) hw_flush();
    cmds[nc++] = (DCmd){first, count, blend, pass, st->set_mask, st->da_x1, st->da_y1, sw, sh, st->rtex};
}

void hw_tri(const HwVert *a, const HwVert *b, const HwVert *c, const HwState *st) { hw_tri_ex(a, b, c, st, 0, 0, 0); }

static void smooth_record_tri(const HwVert *a, const HwVert *b, const HwVert *c, const HwState *st, const float *px,
                              const float *py, const float *pw);

void hw_tri_ex(const HwVert *a, const HwVert *b, const HwVert *c, const HwState *st, const float *px, const float *py,
               const float *pw) {
    if (!hw_active) return;
    smooth_record_tri(a, b, c, st, px, py, pw);
    if (st->check_mask) {
        static int warned;
        if (!warned++) LOG("hw renderer: mask-bit test not implemented (drawn anyway)\n");
    }
    int passes = 1;
    if (st->semi && st->textured) passes = 2;
    if (nv + 6 > MAXV) hw_flush();
    int32_t flags = st->depth | (st->textured << 4) | (st->raw << 5) | (st->semi << 6) | (st->twin << 8) | (st->rmode << 28) | (st->rnearest << 30);
    if (st->textured && st->depth == 2) { /* texture page area read from the upscaled copy */
        if (st->tx < hi_x0) hi_x0 = st->tx;
        if (st->ty < hi_y0) hi_y0 = st->ty;
        if (st->tx + 256 > hi_x1) hi_x1 = st->tx + 256;
        if (st->ty + 256 > hi_y1) hi_y1 = st->ty + 256;
    }
    for (int p = 0; p < passes; p++) {
        int first = nv;
        const HwVert *vv[3] = {a, b, c};
        for (int k = 0; k < 3; k++) {
            HV *o = &verts[nv++];
            o->x = px ? px[k] : (float)vv[k]->x; o->y = py ? py[k] : (float)vv[k]->y;
            o->w = pw ? pw[k] : 1.0f;
            o->u = (float)vv[k]->u; o->v = (float)vv[k]->v;
            o->r = vv[k]->r; o->g = vv[k]->g; o->b = vv[k]->b; o->a = 255;
            o->tx = (int16_t)st->tx; o->ty = (int16_t)st->ty;
            o->clut_x = (int16_t)st->clut_x; o->clut_y = (int16_t)st->clut_y;
            o->flags = flags;
            float uu = (float)vv[k]->u, vq = (float)vv[k]->v;
            if (st->rstretch_x) uu = st->ru0 + 0.5f + (uu - st->ru0) * (st->ru1 - st->ru0 - 1.0f) / (st->ru1 - st->ru0);
            if (st->rstretch_y) vq = st->rv0 + 0.5f + (vq - st->rv0) * (st->rv1 - st->rv0 - 1.0f) / (st->rv1 - st->rv0);
            o->ru = st->rmode ? (uu - st->rox) / st->rsw : 0.0f;
            o->rv = st->rmode ? (vq - st->roy) / st->rsh : 0.0f;
        }
        int blend = 0, pass = 0;
        if (st->semi && !st->textured) blend = st->semi_mode + 1;
        if (passes == 2) { pass = p + 1; blend = p ? st->semi_mode + 1 : 0; }
        add_cmd(first, 3, blend, pass, st);
    }
}

void hw_quad_xy(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, uint32_t col,
                const HwState *st) {
    HwVert p[4];
    float xs[4] = {x0, x1, x0, x1}, ys[4] = {y0, y0, y1, y1}, us[4] = {u0, u1, u0, u1}, vs[4] = {v0, v0, v1, v1};
    for (int i = 0; i < 4; i++) {
        p[i].x = xs[i]; p[i].y = ys[i]; p[i].u = us[i]; p[i].v = vs[i];
        p[i].r = col & 0xFF; p[i].g = (col >> 8) & 0xFF; p[i].b = (col >> 16) & 0xFF;
    }
    hw_tri(&p[0], &p[1], &p[2], st);
    hw_tri(&p[1], &p[2], &p[3], st);
}

void hw_fill(int x, int y, int w, int h, uint16_t c) {
    if (!hw_active) return;
    smooth_touch(x, x + w, y, y + h);
    hw_flush();
    glBindFramebuffer(GL_FRAMEBUFFER, fb_fbo);
    glEnable(GL_SCISSOR_TEST);
    glScissor(x * hw_scale, y * hw_scale, w * hw_scale, h * hw_scale);
    glClearColor((c & 31) / 31.0f, ((c >> 5) & 31) / 31.0f, ((c >> 10) & 31) / 31.0f, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);
}

/* upload: the software VRAM already holds the new pixels */
void hw_upload(int x, int y, int w, int h) {
    if (!hw_active) return;
    smooth_touch(x, x + w, y, y + h);
    hw_mark_dirty(y, y + h);
    if (y + h > 512) hw_mark_dirty(0, (y + h) - 512);
    hw_flush();
    glBindFramebuffer(GL_FRAMEBUFFER, fb_fbo);
    glUseProgram(prog_up);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, vram_tex);
    glBindVertexArray(vao_empty);
    glViewport(x * hw_scale, y * hw_scale, w * hw_scale, h * hw_scale);
    glUniform4f(u_up_rect, (float)x, (float)y, (float)w, (float)h);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glViewport(0, 0, W, H);
}

void hw_copy(int sx, int sy, int dx, int dy, int w, int h) {
    if (!hw_active) return;
    smooth_touch(dx, dx + w, dy, dy + h);
    hw_flush();
    int S = hw_scale;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fb_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, tmp_fbo);
    glBlitFramebuffer(sx * S, sy * S, (sx + w) * S, (sy + h) * S, 0, 0, w * S, h * S, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, tmp_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fb_fbo);
    glBlitFramebuffer(0, 0, w * S, h * S, dx * S, dy * S, (dx + w) * S, (dy + h) * S, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, fb_fbo);
}

/* ------------------------------------------------------------------ display */

/* draw the display area into the window (x0,y0,w0,h0 = target viewport in window pixels) */
/* draw a rectangle of VRAM (native coordinates) into the window; dx,dy = top-left in window pixels */
static void blit_vram(float sx, float sy, float sw, float sh, int dx, int dy, int dw, int dh, int win_h) {
    if (dw <= 0 || dh <= 0) return;
    glViewport(dx, win_h - dy - dh, dw, dh);
    /* screen y goes down, VRAM y goes down too: flip by passing a negative height */
    glUniform4f(u_show_rect, sx, sy + sh, sw, -sh);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void (*hw_before_swap)(void); /* frame limiter: wait for the frame time, then show */
int hw_fill_mode = 1; /* rooms/cutscenes: enlarged picture + HUD bands (the remaster's layout "K") */
static GLuint screen_fbo; /* 0 = the window; else an offscreen capture target */
void (*hw_capture_presented)(int w, int h);
void (*hw_overlay_draw)(int w, int h); /* menu.c, over the picture */
unsigned ui_target_fbo;

/* The inside of the HUD's boxes in a strip of the display (prompt and inventory boxes): a pixel with HUD
 * (brightness over 0.1) above, below, left and right of it in the strip. Their black stays opaque over the
 * picture (the remaster fork's box fill). Taken from the upscaled picture (the redrawn interface's boxes, not
 * the original's), at its resolution, on the GPU: a read back to the CPU waits for the whole frame to be drawn
 * (45 ms a frame at 8x). Such a pixel lies strictly between its row's first and last HUD pixel and its
 * column's: pass 0 finds each row's, pass 1 each column's, pass 2 writes the mask. */
static const char *vs_mask =
    "#version 330 core\n"
    "void main(){ vec2 c = vec2(gl_VertexID & 1, gl_VertexID >> 1); gl_Position = vec4(c * 2.0 - 1.0, 0.0, 1.0); }\n";
static const char *fs_mask =
    "#version 330 core\n"
    "uniform sampler2D u_fb; uniform sampler2D u_rows; uniform sampler2D u_cols; uniform vec4 u_r; uniform int u_mode;\n"
    "out vec4 o;\n"
    "bool hud(int x, int y){ vec3 c = texelFetch(u_fb, ivec2(int(u_r.x) + x, int(u_r.y) + y), 0).rgb;\n"
    "  return any(greaterThan(c, vec3(25.5 / 255.0))); }\n"
    "void main(){ ivec2 p = ivec2(gl_FragCoord.xy); int w = int(u_r.z), h = int(u_r.w); float lo = 1e9, hi = -1.0;\n"
    "  if (u_mode == 0) { for (int x = 0; x < w; x++) if (hud(x, p.y)) { lo = min(lo, float(x)); hi = float(x); } o = vec4(lo, hi, 0, 1); }\n"
    "  else if (u_mode == 1) { for (int y = 0; y < h; y++) if (hud(p.x, y)) { lo = min(lo, float(y)); hi = float(y); } o = vec4(lo, hi, 0, 1); }\n"
    "  else { vec2 r = texelFetch(u_rows, ivec2(0, p.y), 0).rg, c = texelFetch(u_cols, ivec2(p.x, 0), 0).rg;\n"
    "    o = vec4(float(p.x) > r.x && float(p.x) < r.y && float(p.y) > c.x && float(p.y) < c.y ? 1.0 : 0.0); } }\n";

static GLuint mask_target(GLuint *tex, GLuint *fbo, GLenum fmt, GLenum layout, GLenum type, int w, int h) {
    if (!*tex) { glGenTextures(1, tex); glGenFramebuffers(1, fbo); }
    glBindTexture(GL_TEXTURE_2D, *tex);
    glTexImage2D(GL_TEXTURE_2D, 0, fmt, w, h, 0, layout, type, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindFramebuffer(GL_FRAMEBUFFER, *fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, *tex, 0);
    return *fbo;
}

/* pic: the upscaled picture on display. Binds the mask on texture unit 1; the caller's target, viewport and
 * program are restored (the program to prog_show, texture unit 0 to pic) */
static void hud_box_mask(GLuint pic, int x0, int y0, int nw, int nh) {
    static GLuint prog, rows_tex, rows_fbo, cols_tex, cols_fbo, mask_fbo;
    static GLint l_rows, l_cols, l_r, l_mode;
    static int mw, mh;
    int w = nw * hw_scale, h = nh * hw_scale;
    if (w <= 0 || h <= 0) return;
    if (!prog) {
        if (!(prog = link(vs_mask, fs_mask))) return;
        glUseProgram(prog);
        glUniform1i(glGetUniformLocation(prog, "u_fb"), 0);
        l_rows = glGetUniformLocation(prog, "u_rows");
        l_cols = glGetUniformLocation(prog, "u_cols");
        l_r = glGetUniformLocation(prog, "u_r");
        l_mode = glGetUniformLocation(prog, "u_mode");
        glUniform1i(l_rows, 1);
        glUniform1i(l_cols, 2);
    }
    GLint prev_fbo, vp[4];
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev_fbo);
    glGetIntegerv(GL_VIEWPORT, vp);
    GLboolean blend = glIsEnabled(GL_BLEND), scissor = glIsEnabled(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    if (mw != w || mh != h) {
        mask_target(&rows_tex, &rows_fbo, GL_RG32F, GL_RG, GL_FLOAT, 1, h);
        mask_target(&cols_tex, &cols_fbo, GL_RG32F, GL_RG, GL_FLOAT, w, 1);
        mask_target(&hud_mask_tex, &mask_fbo, GL_R8, GL_RED, GL_UNSIGNED_BYTE, w, h);
        mw = w; mh = h;
    }
    glUseProgram(prog);
    glUniform4f(l_r, (float)(x0 * hw_scale), (float)(y0 * hw_scale), (float)w, (float)h);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, pic);
    glBindFramebuffer(GL_FRAMEBUFFER, rows_fbo);
    glViewport(0, 0, 1, h);
    glUniform1i(l_mode, 0);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindFramebuffer(GL_FRAMEBUFFER, cols_fbo);
    glViewport(0, 0, w, 1);
    glUniform1i(l_mode, 1);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, rows_tex);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, cols_tex);
    glBindFramebuffer(GL_FRAMEBUFFER, mask_fbo);
    glViewport(0, 0, w, h);
    glUniform1i(l_mode, 2);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prev_fbo);
    glViewport(vp[0], vp[1], vp[2], vp[3]);
    if (blend) glEnable(GL_BLEND);
    if (scissor) glEnable(GL_SCISSOR_TEST);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, hud_mask_tex);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, pic);
    glUseProgram(prog_show);
    glUniform1i(u_mask_loc, 1);
    glUniform4f(u_mrect_loc, (float)x0, (float)y0, (float)nw, (float)nh); /* native: v_p is in PS1 pixels */
}

/* ------------------------------------------------------------------ 60 fps in-between frames
 * The game draws a frame every second vblank (rooms) and at the flip shows the one drawn a frame before, while the
 * next is already drawn into the other buffer. For the vblank between flips the shown buffer is rebuilt with the
 * remodels posed halfway to the next frame: when a frame draws its first remodel, its buffer's picture is copied (the
 * snapshot: everything behind the remodels), then everything the frame draws after it is recorded (triangles; the
 * remodels as events with their pose as drawn, models.c, and their GPU pass for the depth buffer). The rebuild is the snapshot, the recorded triangles replayed, the remodels drawn with blended
 * poses (models.c). A fill, upload or copy into the buffer after the snapshot makes that frame not rebuildable. */
int smooth_on = 1;
typedef struct { int kind; HwVert v[3]; HwState st; float p[9]; int prec; uint32_t pose, pass; int area[6]; } SEv;
typedef struct { int valid; uint64_t frame; int x0, x1, y0, y1; SEv *ev; int n, cap; int bgtag; } SRec;
static SRec srec[2] = {{.bgtag = -1}, {.bgtag = -1}};
static int srec_cur = -1, smooth_replaying;
static uint32_t smooth_pass; /* while replaying: the recorded mesh's GPU pass, the key of its depth buffer */
static void mesh_depth_reset(GLuint fbo);
static GLuint snap_tex[2], snap_fbo[2], mix_tex, mix_fbo;
static uint64_t flip_frame, flip_frame_prev;

static int load_hold_flips;        /* save states: flips to wait for after a load before the window shows a frame */
static uint32_t load_hold_until;   /* ... at most until then (SDL ticks) */

void smooth_flip(void) {
    if (load_hold_flips > 0) load_hold_flips--;
    srec_cur = -1;
    flip_frame_prev = flip_frame;
    flip_frame = frame_count;
}

static SEv *smooth_add(SRec *r) {
    if (r->n == r->cap) {
        int cap = r->cap ? r->cap * 2 : 4096;
        SEv *e = realloc(r->ev, sizeof(SEv) * (size_t)cap);
        if (!e) { r->valid = 0; return 0; }
        r->ev = e; r->cap = cap;
    }
    return &r->ev[r->n++];
}

static int smooth_dbg(void) { static int d = -1; if (d < 0) d = getenv("FE_DBG_SMOOTH") != 0; return d; }

static void smooth_touch(int x0, int x1, int y0, int y1) {
    if (srec_cur < 0 || smooth_replaying) return;
    if ((x1 - x0) * (y1 - y0) <= 16) return; /* a few pixels (FE2 writes 2 x 1 at the buffer's corner every frame) */
    SRec *r = &srec[srec_cur];
    if (y0 <= r->y1 && y1 > r->y0 && x0 <= r->x1 && x1 > r->x0) {
        if (r->valid && smooth_dbg()) LOG("smooth: frame %llu: buffer at %d,%d touched by %d..%d x %d..%d\n",
                                                     (unsigned long long)frame_count, r->x0, r->y0, x0, x1, y0, y1);
        r->valid = 0;
    }
}

static void smooth_record_tri(const HwVert *a, const HwVert *b, const HwVert *c, const HwState *st, const float *px,
                              const float *py, const float *pw) {
    if (srec_cur < 0 || smooth_replaying) return;
    SRec *r = &srec[srec_cur];
    if (!r->valid || st->da_y1 < r->y0 || st->da_y2 > r->y1 || st->da_x1 < r->x0 || st->da_x2 > r->x1) return;
    SEv *e = smooth_add(r);
    if (!e) return;
    e->kind = 0;
    e->v[0] = *a; e->v[1] = *b; e->v[2] = *c;
    e->st = *st;
    e->prec = px != 0;
    if (px) for (int k = 0; k < 3; k++) { e->p[k] = px[k]; e->p[3 + k] = py[k]; e->p[6 + k] = pw[k]; }
}

/* the snapshot of this frame's buffer, once per frame: at its first remodel, or before its background when that is a
 * 60 fps movie (the background is then replayed with the in-between picture) */
static SRec *smooth_begin(int *a) {
    gpu_draw_area(&a[0], &a[1], &a[2], &a[3], &a[4], &a[5]);
    int ri = a[1] >= 240 ? 1 : 0;
    SRec *r = &srec[ri];
    if (srec_cur != ri || r->frame != frame_count) {
        hw_flush();
        if (!snap_fbo[ri]) snap_tex[ri] = make_fb_tex(&snap_fbo[ri], W, H);
        int S = hw_scale;
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fb_fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, snap_fbo[ri]);
        glBlitFramebuffer(0, a[1] * S, W, (a[3] + 1) * S, 0, a[1] * S, W, (a[3] + 1) * S, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER, draw_fbo);
        r->n = 0; r->valid = 1; r->frame = frame_count; r->x0 = a[0]; r->x1 = a[2]; r->y0 = a[1]; r->y1 = a[3];
        r->bgtag = -1;
        srec_cur = ri;
    }
    return r;
}

/* a 60 fps movie's background is about to be drawn into this frame's buffer (game_fe2.c; tag: which movie picture) */
void smooth_bg_mark(int tag) {
    if (!smooth_on || smooth_replaying || !hw_active) return;
    int a[6];
    SRec *r = smooth_begin(a);
    if (!r->n) r->bgtag = tag; /* only when nothing was recorded before it (it is the frame's background) */
}

void smooth_mesh_event(uint32_t pose) {
    if (!smooth_on || smooth_replaying || !hw_active) return;
    int a[6];
    SRec *r = smooth_begin(a); /* this frame's first remodel in this buffer: the snapshot */
    SEv *e = smooth_add(r);
    if (!e) return;
    e->kind = 1; e->pose = pose; e->pass = models_dma_serial;
    memcpy(e->area, a, sizeof a);
}

static int smooth_build_(const GpuDisplay *d);
/* the in-between frame for the buffer on display, if this vblank is one (1 = mix_tex holds it); FE_DBG_HFR: the 60 fps
 * movie picture each presented frame shows */
static int smooth_build(const GpuDisplay *d) {
    int built = smooth_build_(d);
    static int dbg = -1;
    static uint64_t logged = ~0ull;
    if (dbg < 0) dbg = getenv("FE_DBG_HFR") != 0;
    SRec *r = &srec[d->y >= 240 ? 1 : 0];
    if (dbg && logged != frame_count && r->bgtag >= 0 && r->frame + 8 > frame_count) {
        logged = frame_count;
        int k = (int)(frame_count - flip_frame);
        LOG("hfr: present frame %llu shows picture %d (%s, k %d)\n", (unsigned long long)frame_count,
            movie_tag_pic(r->bgtag, built ? k - 1 : 0), built ? "in-between" : "as drawn", k);
    }
    return built;
}

static int smooth_build_(const GpuDisplay *d) {
    if (!smooth_on || !flip_frame_prev) return 0;
    /* the window is presented at the start of vblank, before the game's handler flips: a buffer flipped in vblank F
     * is first presented at F + 1 (k = 1, as drawn), the in-betweens are F + 2 .. F + period */
    int period = (int)(flip_frame - flip_frame_prev), k = (int)(frame_count - flip_frame);
    if (period < 2 || period > 4 || k < 2 || k > period) return 0;
    int ri = d->y >= 240 ? 1 : 0;
    SRec *r = &srec[ri];
    static int dbg = -1;
    if (dbg < 0) dbg = getenv("FE_DBG_SMOOTH") != 0;
    if (dbg)
        LOG("smooth: frame %llu flips %llu/%llu shows y %d: rec frame %llu valid %d events %d\n", (unsigned long long)frame_count,
            (unsigned long long)flip_frame_prev, (unsigned long long)flip_frame, d->y, (unsigned long long)r->frame, r->valid, r->n);
    /* the frame on display was drawn before the last flip, within one period of the one before */
    if (!r->valid || !r->n || r->frame >= flip_frame || r->frame + 2 * (uint64_t)period < flip_frame) return 0;
    static uint64_t built_frame;
    static int built_ri = -1;
    if (built_frame == frame_count && built_ri == ri) return 1;
    for (int i = 0; i < r->n; i++)
        if (r->ev[i].kind == 1 && !models_pose_kept(r->ev[i].pose)) return 0;
    float t = (float)(k - 1) / (float)period;
    hw_flush();
    hw_gpt_mark(1, 1);
    /* a 60 fps movie background: its in-between picture into the movie buffer the replayed background reads */
    if (r->bgtag >= 0 && !movie_inbetween(r->bgtag, k - 1)) { hw_gpt_mark(1, 0); return 0; }
    if (!mix_fbo) mix_tex = make_fb_tex(&mix_fbo, W, H);
    int S = hw_scale;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, snap_fbo[ri]);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, mix_fbo);
    glBlitFramebuffer(0, r->y0 * S, W, (r->y1 + 1) * S, 0, r->y0 * S, W, (r->y1 + 1) * S, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    mesh_depth_reset(mix_fbo);
    draw_fbo = mix_fbo;
    smooth_replaying = 1;
    for (int i = 0; i < r->n; i++) {
        SEv *e = &r->ev[i];
        if (e->kind == 0) hw_tri_ex(&e->v[0], &e->v[1], &e->v[2], &e->st, e->prec ? e->p : 0, e->prec ? e->p + 3 : 0,
                                    e->prec ? e->p + 6 : 0);
        else {
            hw_flush();
            memcpy(mesh_area, e->area, sizeof mesh_area);
            mesh_area_set = 1;
            smooth_pass = 0x80000000u | e->pass;
            models_smooth_draw(e->pose, t);
            mesh_area_set = 0;
        }
    }
    hw_flush();
    if (r->bgtag >= 0) movie_inbetween_end(); /* a borrowed movie buffer gets its own content back */
    smooth_replaying = 0;
    mesh_depth_reset(0);
    draw_fbo = fb_fbo;
    glBindFramebuffer(GL_FRAMEBUFFER, fb_fbo);
    built_frame = frame_count;
    built_ri = ri;
    hw_gpt_mark(1, 0);
    return 1;
}

/* save states (state.c): the upscaled VRAM is rebuilt from the restored native VRAM (textures exactly; the
 * framebuffers at native sharpness until the game draws them again, one or two frames; the movie pictures until the
 * next 8x frame, while game_fe2.c holds the window), the 60 fps recording starts over, and the window goes black
 * once so that no picture of before the load stays up while the window is held */
static uint32_t native_since; /* hw_present: since when the window is held on a stand-in */
void hw_state_loaded(void) {
    if (!hw_active) return;
    native_since = 0;
    /* the framebuffer on display was drawn before the save: shown now it would be the native picture (its 8x
     * detail is not in the state); the window stays black until the game has drawn and flipped twice (0.4 s at most) */
    load_hold_flips = 2;
    load_hold_until = 0;
    hw_flush();
    nv = nc = 0;
    for (int i = 0; i < 2; i++) { srec[i].valid = 0; srec[i].n = 0; srec[i].frame = 0; srec[i].bgtag = -1; }
    srec_cur = -1; smooth_replaying = 0; flip_frame = flip_frame_prev = 0;
    draw_fbo = fb_fbo;
    mesh_area_set = 0;
    dirty_y0 = 0; dirty_y1 = 512;
    hw_upload(0, 0, 1024, 512);
    hw_flush();
    if (glwin && !screen_fbo) {
        int w, h;
        SDL_GL_GetDrawableSize(glwin, &w, &h);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, w, h);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        SDL_GL_SwapWindow(glwin);
        glClear(GL_COLOR_BUFFER_BIT);
        glBindFramebuffer(GL_FRAMEBUFFER, fb_fbo);
    }
}

/* ------------------------------------------------------------------ picture effects
 * The remaster fork's post-processing shaders (fe-bloom.fx, fe-vignette.glsl, fe-film-grain.glsl), same maths and
 * defaults, in the fork's order: bloom, vignette, grain (NOTES.md "Picture effects"). The picture is drawn into
 * fx_tex (window-sized); fx_apply draws it into the window with the effects, over the picture's rectangle only.
 * The HUD strips and the menu are drawn after, so they stay crisp. */
int fx_bloom = 1, fx_vignette = 1, fx_grain = 1;
float fx_bloom_intensity = 0.8f, fx_bloom_threshold = 0.7f, fx_bloom_radius = 1.0f, fx_bloom_halo = 0.5f;
float fx_vignette_strength = 0.3f, fx_vignette_start = 0.45f, fx_vignette_softness = 0.55f;
float fx_grain_strength = 1.0f, fx_grain_rows = 640.0f, fx_grain_refresh = 24.0f;

static GLuint fx_tex, fx_fbo, fx_q1, fx_q1_fbo, fx_q2, fx_q2_fbo, fx_e1, fx_e1_fbo, fx_e2, fx_e2_fbo;
static GLuint prog_fx_pre, prog_fx_blur, prog_fx_down, prog_fx_final;
static int fx_w, fx_h, fx_failed;

static const char *vs_fx =
    "#version 330 core\n"
    "out vec2 v_uv;\n"
    "void main(){ vec2 c = vec2(gl_VertexID & 1, gl_VertexID >> 1); v_uv = c; gl_Position = vec4(c * 2.0 - 1.0, 0.0, 1.0); }\n";

/* full size -> 1/4: four bilinear taps one full pixel off centre, the soft bright pass per tap */
static const char *fs_fx_pre =
    "#version 330 core\n"
    "uniform sampler2D u_src; uniform vec2 u_px; uniform float u_thr; in vec2 v_uv; out vec4 o;\n"
    "vec3 bright(vec3 c){ float b = max(c.r, max(c.g, c.b)); float w = clamp((b - u_thr) / max(1.0 - u_thr, 0.001), 0.0, 1.0);\n"
    "  return c * (w * w); }\n"
    "void main(){ vec3 s = bright(texture(u_src, v_uv + u_px * vec2(-1.0, -1.0)).rgb) + bright(texture(u_src, v_uv + u_px * vec2(1.0, -1.0)).rgb)\n"
    "  + bright(texture(u_src, v_uv + u_px * vec2(-1.0, 1.0)).rgb) + bright(texture(u_src, v_uv + u_px * vec2(1.0, 1.0)).rgb);\n"
    "  o = vec4(s * 0.25, 1.0); }\n";

/* 13-tap gaussian as 7 bilinear taps; u_step = one texel along the axis times the radius */
static const char *fs_fx_blur =
    "#version 330 core\n"
    "uniform sampler2D u_src; uniform vec2 u_step; in vec2 v_uv; out vec4 o;\n"
    "void main(){ vec3 c = texture(u_src, v_uv).rgb * 0.1964825502;\n"
    "  c += (texture(u_src, v_uv + u_step * 1.4117647059).rgb + texture(u_src, v_uv - u_step * 1.4117647059).rgb) * 0.2969069647;\n"
    "  c += (texture(u_src, v_uv + u_step * 3.2941176471).rgb + texture(u_src, v_uv - u_step * 3.2941176471).rgb) * 0.0944703979;\n"
    "  c += (texture(u_src, v_uv + u_step * 5.1764705882).rgb + texture(u_src, v_uv - u_step * 5.1764705882).rgb) * 0.0103813624;\n"
    "  o = vec4(c, 1.0); }\n";

/* 1/4 -> 1/8: a 2x2 box (four bilinear taps half a 1/4 texel off centre) */
static const char *fs_fx_down =
    "#version 330 core\n"
    "uniform sampler2D u_src; uniform vec2 u_px; in vec2 v_uv; out vec4 o;\n"
    "void main(){ vec3 s = texture(u_src, v_uv + u_px * vec2(-1.0, -1.0)).rgb + texture(u_src, v_uv + u_px * vec2(1.0, -1.0)).rgb\n"
    "  + texture(u_src, v_uv + u_px * vec2(-1.0, 1.0)).rgb + texture(u_src, v_uv + u_px * vec2(1.0, 1.0)).rgb;\n"
    "  o = vec4(s * 0.25, 1.0); }\n";

/* bloom's screen blend, the vignette, the grain; u_rect = the picture in window pixels (GL: y from the bottom) */
static const char *fs_fx_final =
    "#version 330 core\n"
    "uniform sampler2D u_base, u_q, u_e; uniform vec4 u_rect; uniform vec2 u_bloom; uniform vec3 u_vig; uniform vec3 u_grain;\n"
    "in vec2 v_uv; out vec4 o;\n"
    "float hash13(vec3 p3){ p3 = fract(p3 * 0.1031); p3 += dot(p3, p3.zyx + 31.32); return fract((p3.x + p3.y) * p3.z); }\n"
    "float cell_noise(vec2 c, float f){ float s = hash13(vec3(c, f)) + hash13(vec3(c, f + 17.0)) + hash13(vec3(c, f + 41.0))\n"
    "  + hash13(vec3(c, f + 73.0)); return (s - 2.0) * 1.7320508; }\n"
    "void main(){ vec4 c = texture(u_base, v_uv);\n"
    "  vec2 fc = gl_FragCoord.xy, p = (fc - u_rect.xy) / u_rect.zw;\n"
    "  if (p.x < 0.0 || p.y < 0.0 || p.x >= 1.0 || p.y >= 1.0) { o = vec4(c.rgb, 1.0); return; }\n"
    "  if (u_bloom.x > 0.0) { vec3 g = mix(texture(u_q, v_uv).rgb, texture(u_e, v_uv).rgb, u_bloom.y) * u_bloom.x;\n"
    "    c.rgb = 1.0 - (1.0 - c.rgb) * (1.0 - clamp(g, 0.0, 1.0)); }\n"
    "  if (u_vig.x > 0.0) { float d = length((p - 0.5) * 2.0) * 0.70710678;\n"
    "    c.rgb *= 1.0 - u_vig.x * smoothstep(u_vig.y, u_vig.y + u_vig.z, d); }\n"
    "  if (u_grain.x > 0.0) { float cell = max(u_rect.w / u_grain.y, 1.0); vec2 q = (fc - u_rect.xy) / cell, i = floor(q), f = q - i;\n"
    "    float n = mix(mix(cell_noise(i, u_grain.z), cell_noise(i + vec2(1.0, 0.0), u_grain.z), f.x),\n"
    "                  mix(cell_noise(i + vec2(0.0, 1.0), u_grain.z), cell_noise(i + vec2(1.0, 1.0), u_grain.z), f.x), f.y) * 1.5;\n"
    "    c.rgb = clamp(c.rgb + n * (3.1 / 255.0) * u_grain.x, 0.0, 1.0); }\n"
    "  o = vec4(c.rgb, 1.0); }\n";

static GLuint fx_make(GLuint *fbo, int w, int h, GLenum fmt) {
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, fmt, w, h, 0, GL_RGBA, fmt == GL_RGBA8 ? GL_UNSIGNED_BYTE : GL_FLOAT, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if (!*fbo) glGenFramebuffers(1, fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, *fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) fx_failed = 1;
    return t;
}

/* any effect on, and the buffers for a win_w x win_h window ready: the picture goes into fx_fbo */
static int fx_ready(int win_w, int win_h) {
    int bloom = fx_bloom && fx_bloom_intensity > 0, vig = fx_vignette && fx_vignette_strength > 0;
    int grain = fx_grain && fx_grain_strength > 0;
    if (fx_failed || !(bloom || vig || grain) || win_w < 16 || win_h < 16) return 0;
    if (!prog_fx_final) {
        prog_fx_pre = link(vs_fx, fs_fx_pre);
        prog_fx_blur = link(vs_fx, fs_fx_blur);
        prog_fx_down = link(vs_fx, fs_fx_down);
        prog_fx_final = link(vs_fx, fs_fx_final);
        if (!prog_fx_pre || !prog_fx_blur || !prog_fx_down || !prog_fx_final) {
            LOG("hw renderer: picture effects off (shader)\n");
            fx_failed = 1;
            return 0;
        }
    }
    if (fx_w != win_w || fx_h != win_h) {
        GLuint *t[] = {&fx_tex, &fx_q1, &fx_q2, &fx_e1, &fx_e2};
        for (int i = 0; i < 5; i++)
            if (*t[i]) glDeleteTextures(1, t[i]);
        fx_tex = fx_make(&fx_fbo, win_w, win_h, GL_RGBA8);
        int qw = win_w / 4 > 1 ? win_w / 4 : 1, qh = win_h / 4 > 1 ? win_h / 4 : 1;
        int ew = win_w / 8 > 1 ? win_w / 8 : 1, eh = win_h / 8 > 1 ? win_h / 8 : 1;
        fx_q1 = fx_make(&fx_q1_fbo, qw, qh, GL_RGBA16F);
        fx_q2 = fx_make(&fx_q2_fbo, qw, qh, GL_RGBA16F);
        fx_e1 = fx_make(&fx_e1_fbo, ew, eh, GL_RGBA16F);
        fx_e2 = fx_make(&fx_e2_fbo, ew, eh, GL_RGBA16F);
        fx_w = win_w; fx_h = win_h;
        if (fx_failed) { LOG("hw renderer: picture effects off (framebuffer)\n"); return 0; }
    }
    return 1;
}

/* bind a pass: its target, program and source (u_src); the caller sets the rest and draws */
static void fx_pass(GLuint prog, GLuint fbo, int w, int h, GLuint src) {
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, w, h);
    glUseProgram(prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, src);
    glUniform1i(glGetUniformLocation(prog, "u_src"), 0);
}

/* fx_tex (the picture) -> screen_fbo with the effects; px..ph = the picture's rectangle (window pixels, y from the
 * top). Leaves screen_fbo bound with the whole window as the viewport. */
static void fx_apply(int px, int py, int pw, int ph, int win_w, int win_h) {
    int w = fx_w, h = fx_h, qw = w / 4 > 1 ? w / 4 : 1, qh = h / 4 > 1 ? h / 4 : 1;
    int ew = w / 8 > 1 ? w / 8 : 1, eh = h / 8 > 1 ? h / 8 : 1;
    int bloom = fx_bloom && fx_bloom_intensity > 0;
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glBindVertexArray(vao_empty);
    if (bloom) {
        float r = fx_bloom_radius;
        GLint l;
        fx_pass(prog_fx_pre, fx_q1_fbo, qw, qh, fx_tex);
        glUniform2f(glGetUniformLocation(prog_fx_pre, "u_px"), 1.0f / (float)w, 1.0f / (float)h);
        glUniform1f(glGetUniformLocation(prog_fx_pre, "u_thr"), fx_bloom_threshold);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        l = glGetUniformLocation(prog_fx_blur, "u_step");
        fx_pass(prog_fx_blur, fx_q2_fbo, qw, qh, fx_q1);
        glUniform2f(l, r / (float)qw, 0); glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        fx_pass(prog_fx_blur, fx_q1_fbo, qw, qh, fx_q2);
        glUniform2f(l, 0, r / (float)qh); glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        fx_pass(prog_fx_down, fx_e1_fbo, ew, eh, fx_q1);
        glUniform2f(glGetUniformLocation(prog_fx_down, "u_px"), 0.5f / (float)qw, 0.5f / (float)qh);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        fx_pass(prog_fx_blur, fx_e2_fbo, ew, eh, fx_e1);
        glUniform2f(l, r / (float)ew, 0); glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        fx_pass(prog_fx_blur, fx_e1_fbo, ew, eh, fx_e2);
        glUniform2f(l, 0, r / (float)eh); glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, screen_fbo);
    glViewport(0, 0, win_w, win_h);
    glUseProgram(prog_fx_final);
    glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, fx_e1);
    glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, fx_q1);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, fx_tex);
    glUniform1i(glGetUniformLocation(prog_fx_final, "u_base"), 0);
    glUniform1i(glGetUniformLocation(prog_fx_final, "u_q"), 1);
    glUniform1i(glGetUniformLocation(prog_fx_final, "u_e"), 2);
    glUniform4f(glGetUniformLocation(prog_fx_final, "u_rect"), (float)px, (float)(win_h - py - ph), (float)pw, (float)ph);
    glUniform2f(glGetUniformLocation(prog_fx_final, "u_bloom"), bloom ? fx_bloom_intensity : 0.0f, fx_bloom_halo);
    GLint l = glGetUniformLocation(prog_fx_final, "u_vig");
    glUniform3f(l, fx_vignette ? fx_vignette_strength : 0.0f, fx_vignette_start, fx_vignette_softness);
    /* a new pattern REFRESH times a second (NTSC vblanks), wrapped to 0..1023 */
    double t = (double)frame_count / 59.94;
    float pat = (float)fmod(floor(t * (double)fx_grain_refresh), 1024.0);
    l = glGetUniformLocation(prog_fx_final, "u_grain");
    glUniform3f(l, fx_grain ? fx_grain_strength : 0.0f, fx_grain_rows > 1 ? fx_grain_rows : 1.0f, pat);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

/* vx..vh: the 4:3 viewport for the full frame (window pixels, y from the top) */
void hw_present(const GpuDisplay *d, int vx, int vy, int vw, int vh, int win_w, int win_h) {
    hw_flush();
    int mixed = smooth_build(d); /* a vblank between flips: the remodels posed halfway to the next frame */
    int fx = fx_ready(win_w, win_h); /* the picture into fx_fbo, the effects drawn from it into the window */
    glBindFramebuffer(GL_FRAMEBUFFER, fx ? fx_fbo : screen_fbo);
    glViewport(0, 0, win_w, win_h);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    if (fx) { glBindFramebuffer(GL_FRAMEBUFFER, screen_fbo); glClear(GL_COLOR_BUFFER_BIT); glBindFramebuffer(GL_FRAMEBUFFER, fx_fbo); }
    GLuint pic_tex = mixed ? mix_tex : fb_tex;
    glUseProgram(prog_show);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, pic_tex);
    glBindVertexArray(vao_empty);
    /* FE2 rooms: the picture is 160 lines (display rows 40..200 of 240), the HUD lives in the strips above
     * (fear meter, rows 16..40) and below (prompts and inventory, rows 200..222).
     * Which screens get the layout (NOTES.md "Screen layout: which screens"): a 240-line display that shows a movie
     * picture, i.e. an STR movie streams (any video sector in the last 45 frames) OR the framebuffer on display holds
     * the room picture: the game's latest drawing into it (that vblank) drew the room from its movie buffers, at the
     * width on display. The second half keeps the layout over the frozen room while the stream stops (pause, both
     * games); screens drawn without the room picture (title, menus, NOW LOADING, card and game-over screens) or in
     * another display mode stay 4:3.
     * FE_LAYOUT_OLD=1: the stream alone, as before 5 Nov. */
    static int layout_old = -1;
    if (layout_old < 0) layout_old = getenv("FE_LAYOUT_OLD") != 0;
    int movie = frame_count - cdrom_last_movie_frame < 45;
    int fb = d->y >= 200;
    int room_pic = !layout_old && fb_room_frame[fb] >= 0 && fb_room_frame[fb] == fb_draw_frame[fb] && fb_room_w[fb] == d->w;
    int layout = (movie || room_pic) && d->h == 240 && (hw_fill_mode == 1 || hw_fill_mode == 2) ? hw_fill_mode : 0;
    {
        static int dbg = -1;
        if (dbg < 0) dbg = getenv("FE_DBG_LAYOUT") != 0;
        if (dbg) {
            static char last[200];
            char s[200];
            int x1, y1, x2, y2, ox, oy;
            gpu_draw_area(&x1, &y1, &x2, &y2, &ox, &oy);
            snprintf(s, sizeof s, "display %d,%d %dx%d draw area %d,%d-%d,%d movie stream %s, room picture in this buffer %s, layout %d",
                     d->x, d->y, d->w, d->h, x1, y1, x2, y2, movie ? "on" : "off",
                     room_pic ? "yes" : "no", layout);
            if (strcmp(s, last)) {
                LOG("layout: frame %llu %s (last movie sector at frame %lld)\n", (unsigned long long)frame_count, s,
                    (long long)cdrom_last_movie_frame);
                strcpy(last, s);
            }
        }
    }
    float line = (float)win_h / 224.0f;                       /* one PS1 line at the original size */
    float pxw = layout ? line * (4.0f / 3.0f) * 224.0f / (float)d->w : 0; /* one PS1 pixel, true shape */
    float aspect = layout ? (float)d->w * pxw / (160.0f * line) : 0;
    int px = vx, py = vy, pw = vw, ph = vh, band = 0; /* the picture's rectangle in the window */
    if (layout == 1) {
        band = (int)(24.0f * line + 0.5f);
        int avail = win_h - 2 * band;
        pw = win_w; ph = (int)((float)pw / aspect);
        if (ph > avail) { ph = avail; pw = (int)((float)ph * aspect); }
        px = (win_w - pw) / 2; py = band + (avail - ph) / 2;
    } else if (layout == 2) {
        /* HUD over the picture (the remaster fork's letterbox off, DrawFE2HudStrips): the room fills the window
         * (true shape); the 32 lines above and below it (the HUD strips) are drawn over its top and bottom edges
         * at the picture's own scale and width, their black left out, except inside the bottom strip's boxes */
        pw = win_w; ph = (int)((float)pw / aspect);
        if (ph > win_h) { ph = win_h; pw = (int)((float)ph * aspect); }
        px = (win_w - pw) / 2; py = (win_h - ph) / 2;
    }
    if (layout) blit_vram((float)d->x, (float)d->y + 40, (float)d->w, 160, px, py, pw, ph, win_h);
    else blit_vram((float)d->x, (float)d->y, (float)d->w, (float)d->h, vx, vy, vw, vh, win_h);
    if (fx) { /* the effects over the picture, then the HUD over them */
        fx_apply(px, py, pw, ph, win_w, win_h);
        glUseProgram(prog_show);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, pic_tex);
        glBindVertexArray(vao_empty);
    }
    if (layout == 1) {
        int hw4 = (int)((float)d->w * pxw);
        int hx = (win_w - hw4) / 2;
        blit_vram((float)d->x, (float)d->y + 16, (float)d->w, 24, hx, 0, hw4, band, win_h);
        blit_vram((float)d->x, (float)d->y + 200, (float)d->w, 22, hx, win_h - band, hw4, (int)(22.0f * line + 0.5f), win_h);
    } else if (layout == 2) {
        /* half a line short on the picture side: the filtered edge would show the picture's first/last row */
        int sh = (int)(31.5f * (float)ph / 160.0f + 0.5f);
        glUniform1i(u_key_loc, 1); /* the top strip (fear meter, crosshair): see-through */
        blit_vram((float)d->x, (float)d->y + 8, (float)d->w, 31.5f, px, py, pw, sh, win_h);
        hud_box_mask(pic_tex, d->x, d->y + 200, d->w, 32);
        glUniform1i(u_key_loc, 2); /* the bottom strip: the boxes keep their black inside */
        blit_vram((float)d->x, (float)d->y + 200.5f, (float)d->w, 31.5f, px, py + ph - sh, pw, sh, win_h);
        glUniform1i(u_key_loc, 0);
    }
    if (hw_overlay_draw) { ui_target_fbo = screen_fbo; hw_overlay_draw(win_w, win_h); } /* the port's menu, notices */
    if (!screen_fbo) {
        if (hw_before_swap) hw_before_swap();
        /* a frame drawn from a native stand-in for a late 8x movie picture (a room change) is not shown: the
         * window keeps the last complete frame, for at most 0.7 s */
        /* the framebuffer on display (drawn a frame earlier, shown on several vblanks) */
        if (load_hold_flips > 0) { /* right after a state load (0.4 s from the first present: the load itself may wait for
                                    * the 8x movie frames) */
            if (!load_hold_until) load_hold_until = SDL_GetTicks() + 400;
            if ((int32_t)(SDL_GetTicks() - load_hold_until) < 0) return;
        }
        load_hold_flips = 0;
        int native = movie_fb_native[d->y >= 200] && frame_count - cdrom_last_movie_frame < 45;
        if (!native) native_since = 0;
        else {
            uint32_t now = SDL_GetTicks();
            if (!native_since) native_since = now;
            static int dbg = -1;
            if (dbg < 0) dbg = getenv("FE_DBG_MOVIE") != 0;
            int hold = now - native_since < 700;
            if (dbg) LOG("present: frame %llu drawn from a native stand-in: %s\n", (unsigned long long)frame_count,
                         hold ? "held" : "shown (0.7 s passed)");
            if (hold) return;
        }
        if (hw_capture_presented) hw_capture_presented(win_w, win_h); /* FE_CAPTURE: debugging what the window shows */
        SDL_GL_SwapWindow(glwin);
    }
}

/* frame dumps (headless): a failed allocation or a GL out-of-memory ends the run with a FATAL line instead of
   a native crash (many 8x runs at once can exhaust the GPU and system memory) */
static void *dump_alloc(size_t n) {
    void *p = malloc(n);
    if (!p) fatal("out of memory (%zu bytes for a frame read-back)", n);
    return p;
}
static void gl_oom_check(const char *what) {
    int oom = 0;
    for (GLenum e; (e = glGetError()) != GL_NO_ERROR;) oom |= e == GL_OUT_OF_MEMORY;
    if (oom) fatal("GPU out of memory (%s)", what);
}

/* headless: compose the window picture at w x h offscreen and read it back (top-down RGB) */
uint32_t *hw_screen_capture(const GpuDisplay *d, int w, int h) {
    static GLuint tex, fbo;
    static int cw, ch;
    if (!fbo || cw != w || ch != h) {
        if (!fbo) { glGenTextures(1, &tex); glGenFramebuffers(1, &fbo); }
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
        cw = w; ch = h;
    }
    int vw = w, vh = w * 3 / 4;
    if (vh > h) { vh = h; vw = h * 4 / 3; }
    screen_fbo = fbo;
    hw_present(d, (w - vw) / 2, (h - vh) / 2, vw, vh, w, h);
    screen_fbo = 0;
    uint8_t *px = dump_alloc((size_t)w * h * 4);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    gl_oom_check("screen capture");
    uint32_t *out = dump_alloc((size_t)w * h * 4);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const uint8_t *p = &px[((size_t)(h - 1 - y) * w + x) * 4];
            out[(size_t)y * w + x] = ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
        }
    free(px);
    return out;
}

/* read the upscaled display area back (headless frame dumps); returns a malloc'd RGB buffer */
uint32_t *hw_read_display(const GpuDisplay *d, int *ow, int *oh) {
    hw_flush();
    int w = d->w * hw_scale, h = d->h * hw_scale;
    uint8_t *px = dump_alloc((size_t)w * h * 4);
    glBindFramebuffer(GL_FRAMEBUFFER, fb_fbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(d->x * hw_scale, d->y * hw_scale, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    gl_oom_check("display read-back");
    uint32_t *out = dump_alloc((size_t)w * h * 4);
    for (int i = 0; i < w * h; i++)
        out[i] = ((uint32_t)px[i * 4] << 16) | ((uint32_t)px[i * 4 + 1] << 8) | px[i * 4 + 2];
    free(px);
    *ow = w; *oh = h;
    return out;
}

/* show a software-built RGB picture (24-bit display mode) */
void hw_present_rgb(const uint32_t *rgb, int w, int h, int vx, int vy, int vw, int vh, int win_w, int win_h) {
    static GLuint t, fbo;
    static int tw, th;
    hw_flush();
    if (!t || tw != w || th != h) {
        if (!t) { glGenTextures(1, &t); glGenFramebuffers(1, &fbo); }
        glBindTexture(GL_TEXTURE_2D, t);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_BGRA, GL_UNSIGNED_BYTE, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t, 0);
        tw = w; th = h;
    }
    glBindTexture(GL_TEXTURE_2D, t);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_BGRA, GL_UNSIGNED_BYTE, rgb);
    int fx = fx_ready(win_w, win_h); /* the picture effects, as in hw_present */
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glViewport(0, 0, win_w, win_h);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    if (fx) { glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fx_fbo); glClear(GL_COLOR_BUFFER_BIT); }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    /* rows of rgb go top-down: flip while blitting */
    glBlitFramebuffer(0, 0, w, h, vx, vy + vh, vx + vw, vy, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    if (fx) fx_apply(vx, win_h - vy - vh, vw, vh, win_w, win_h); /* the blit's y is from the bottom */
    if (hw_before_swap) hw_before_swap();
    SDL_GL_SwapWindow(glwin);
}

/* ------------------------------------------------------------------ replacement pictures */

/* two upload slots: 0 the game's uploads (a frame's 20 slices may be uploaded over several vblanks, the crossfade works
 * on it), 1 the 60 fps movies' sub-step pictures put into a movie buffer between those (game_fe2.c) */
static GLuint hd_texs[2], hd_fbos[2];
static int hd_ws[2], hd_hs[2], hd_cur;
#define hd_tex hd_texs[hd_cur]
#define hd_fbo hd_fbos[hd_cur]
#define hd_w hd_ws[hd_cur]
#define hd_h hd_hs[hd_cur]
void hw_hd_slot(int slot) { hd_cur = slot & 1; }

/* upload a top-down BGRA picture (e.g. an 8x movie frame) */
void hw_hd_upload(const uint8_t *rgb, int w, int h) {
    if (!hw_active) return;
    if (!hd_tex || hd_w != w || hd_h != h) {
        if (!hd_tex) { glGenTextures(1, &hd_tex); glGenFramebuffers(1, &hd_fbo); }
        glBindTexture(GL_TEXTURE_2D, hd_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glBindFramebuffer(GL_FRAMEBUFFER, hd_fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, hd_tex, 0);
        hd_w = w; hd_h = h;
    }
    glBindTexture(GL_TEXTURE_2D, hd_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, rgb);
}

/* copy part of the uploaded picture into the upscaled VRAM. The picture covers pw x ph native pixels;
 * columns px0..px1 (native) go to VRAM (dx, dy) with height ph. Alpha 1 = PS1 bit 15 set. */
void hw_hd_blit(int pw, int ph, int px0, int px1, int dx, int dy) {
    if (!hw_active || !hd_tex) return;
    if (nc) hw_flush(); /* pending draws first; the native VRAM upload can wait for the next draw batch */
    float sx = (float)hd_w / (float)pw, sy = (float)hd_h / (float)ph;
    int S = hw_scale;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, hd_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fb_fbo);
    glBlitFramebuffer((int)(px0 * sx), 0, (int)(px1 * sx), (int)(ph * sy), dx * S, dy * S, (dx + px1 - px0) * S,
                      (dy + ph) * S, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glBindFramebuffer(GL_FRAMEBUFFER, fb_fbo);
}

/* n columns of the upscaled VRAM (w x h native each, at x[i], y[i]) copied aside (back = 0) or back (back = 1): a 60 fps
 * movie's in-between frame borrows a movie buffer the game is already filling with a later frame (game_fe2.c) */
void hw_vram_stash(int n, const int16_t *x, const int16_t *y, int w, int h, int back) {
    static GLuint tex, fbo;
    static int tw, th;
    if (!hw_active || n <= 0) return;
    int S = hw_scale;
    if (!fbo || tw != n * w * S || th != h * S) {
        if (!fbo) { glGenTextures(1, &tex); glGenFramebuffers(1, &fbo); }
        tw = n * w * S; th = h * S;
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, tw, th, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    }
    if (nc) hw_flush();
    glBindFramebuffer(GL_READ_FRAMEBUFFER, back ? fbo : fb_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, back ? fb_fbo : fbo);
    for (int i = 0; i < n; i++) {
        int vx = x[i] * S, vy = y[i] * S, sx = i * w * S;
        if (back) glBlitFramebuffer(sx, 0, sx + w * S, h * S, vx, vy, vx + w * S, vy + h * S, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        else glBlitFramebuffer(vx, vy, vx + w * S, vy + h * S, sx, 0, sx + w * S, h * S, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, fb_fbo);
}

/* Room loop crossfade (NOTES.md "Room loops"): the last 8x picture frozen at a loop restart, blended over the
 * next pictures of the loop as they are uploaded (before the column copies into VRAM) */
static GLuint fade_tex, fade_fbo, prog_fade;
static int fade_w, fade_h;
static const char *fs_fade =
    "#version 330 core\n"
    "uniform sampler2D u_src; in vec2 v_uv; out vec4 o;\n"
    "void main(){ o = vec4(texture(u_src, v_uv).rgb, 1.0); }\n";

int hw_hd_fade_begin(void) {
    if (!hw_active || !hd_tex) return 0;
    if (!prog_fade && !(prog_fade = link(vs_fx, fs_fade))) return 0;
    if (!fade_tex || fade_w != hd_w || fade_h != hd_h) {
        if (!fade_tex) { glGenTextures(1, &fade_tex); glGenFramebuffers(1, &fade_fbo); }
        glBindTexture(GL_TEXTURE_2D, fade_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, hd_w, hd_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER, fade_fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fade_tex, 0);
        fade_w = hd_w; fade_h = hd_h;
    }
    if (nc) hw_flush();
    glBindFramebuffer(GL_READ_FRAMEBUFFER, hd_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fade_fbo);
    glBlitFramebuffer(0, 0, hd_w, hd_h, 0, 0, hd_w, hd_h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, fb_fbo);
    return 1;
}

/* the frozen picture over the just uploaded one, weight w (0..1) */
void hw_hd_fade_apply(float w) {
    if (!hw_active || !hd_tex || !fade_tex || fade_w != hd_w || fade_h != hd_h || w <= 0) return;
    if (nc) hw_flush();
    glBindFramebuffer(GL_FRAMEBUFFER, hd_fbo);
    glViewport(0, 0, hd_w, hd_h);
    glDisable(GL_SCISSOR_TEST);
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendColorX(0, 0, 0, w);
    glBlendFuncSeparate(GL_CONSTANT_ALPHA, GL_ONE_MINUS_CONSTANT_ALPHA, GL_ZERO, GL_ONE);
    glUseProgram(prog_fade);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, fade_tex);
    glUniform1i(glGetUniformLocation(prog_fade, "u_src"), 0);
    glBindVertexArray(vao_empty);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisable(GL_BLEND);
    glViewport(0, 0, W, H);
    glBindFramebuffer(GL_FRAMEBUFFER, fb_fbo);
}


/* ------------------------------------------------------------------ remaster character meshes
 * A .hm file (FE2HM01, scripts\model_pack.py in the remaster): skinned vertices in model space (4 parts and
 * weights each), triangles grouped by material, one RGBA texture per material. Drawn into the upscaled VRAM
 * at the point of the ordering table where the game's own model would be, with a depth buffer shared by the
 * meshes of one GPU pass (a character and her weapon), shaded by the game's lit packet colour of the original
 * face each vertex came from, cel bands and an ink outline as in the remaster (swap.txt SHADE / OUTLINE). */

#define MESH_MAXBONES 20

typedef struct { float pos[3], nrm[3], uv[2]; uint8_t joints[4], weights[4]; uint16_t face, material; } HmVert;
typedef struct { uint32_t first, count; GLuint tex; } HmMat;
struct HwMesh {
    GLuint vao, vbo, ink_vbo, ibo, light_vbo, nrm_vbo;
    int has_look; /* per-vertex light blend (mlook.c) uploaded */
    int nmat;
    HmMat *mats;
    uint32_t nverts, nidx, nfaces;
    /* the anti-aliasing work's box (hw_mesh_draw): per bone the box round the model-space positions of the vertices it
     * moves (weight > 0); a vertex with no weight at all makes the box unknown (the whole area, as before 7 Nov) */
    float jbox[MESH_MAXBONES][6];
    uint32_t jmask;
    int jbox_bad;
};

static GLuint prog_mesh, mesh_depth_rb, mesh_colors_tex;
float models_matte_rect[4]; /* models.h */
unsigned models_matte_tex;
static GLint um_matte, um_mt, um_mt2;
static int aa_box_old(void) { /* FE_AA_FULL=1 / FE_CROWD_OLD=1: each mesh's AA work over the whole drawing area */
    static int d = -1;
    if (d < 0) d = getenv("FE_AA_FULL") != 0 || getenv("FE_CROWD_OLD") != 0;
    return d;
}
/* ---- FE_GPU_TIME=1: GPU time of the scene actors (each game pass) and of the 60 fps in-between rebuilds, read with
 * timestamp queries after a glFinish at every vblank (a measuring mode: it serialises CPU and GPU); headless it also
 * composes the window picture every vblank (hw_bench_present), as a window does, so the in-betweens are built */
typedef void (APIENTRY *PFN_GENQ)(GLsizei, GLuint *);
typedef void (APIENTRY *PFN_QCNT)(GLuint, GLenum);
typedef void (APIENTRY *PFN_GETQ64)(GLuint, GLenum, GLuint64 *);
static PFN_GENQ q_gen;
static PFN_QCNT q_cnt;
static PFN_GETQ64 q_get;
static int gpt_on = -1;
#define GPT_MAX 64
static GLuint gpt_q[GPT_MAX * 2];
static int gpt_slot[GPT_MAX], gpt_n, gpt_open = -1;
static int gpt_meshes, gpt_meshes_mid;
static double gpt_sum[2], gpt_max[2], gpt_wall_sum, gpt_wall_max, gpt_mesh_sum;
static int gpt_frames, gpt_cnt[2];
static uint64_t gpt_t0, gpt_from;
static int gpt(void) {
    if (gpt_on < 0) {
        gpt_on = getenv("FE_GPU_TIME") != 0;
        if (gpt_on) {
            q_gen = (PFN_GENQ)SDL_GL_GetProcAddress("glGenQueries");
            q_cnt = (PFN_QCNT)SDL_GL_GetProcAddress("glQueryCounter");
            q_get = (PFN_GETQ64)SDL_GL_GetProcAddress("glGetQueryObjectui64v");
            if (!q_gen || !q_cnt || !q_get) { LOG("gputime: no timer queries\n"); gpt_on = 0; }
            else q_gen(GPT_MAX * 2, gpt_q);
        }
    }
    return gpt_on && hw_active;
}
void hw_gpt_mark(int slot, int begin) {
    if (!gpt()) return;
    if (begin) {
        if (gpt_open >= 0 || gpt_n >= GPT_MAX) return;
        gpt_open = gpt_n;
        gpt_slot[gpt_n] = slot;
        q_cnt(gpt_q[gpt_n * 2], GL_TIMESTAMP);
    } else if (gpt_open >= 0 && gpt_slot[gpt_open] == slot) {
        q_cnt(gpt_q[gpt_open * 2 + 1], GL_TIMESTAMP);
        gpt_n++;
        gpt_open = -1;
    }
}
/* anti-aliasing: the meshes go into a multisampled target over the drawing area, which is resolved and laid
 * over the upscaled VRAM with its coverage as alpha */
static GLuint aa_fbo, aa_color_rb, aa_depth_rb, aa_res_fbo, aa_res_tex, prog_comp;
static GLint uc_rect, uc_size;
static int aa_w, aa_h, aa_samples;
static uint32_t aa_depth_serial = 0xFFFFFFFFu;
static const char *fs_comp =
    "#version 330 core\n"
    "uniform sampler2D u_t; uniform vec2 u_size; in vec2 v_p; out vec4 o;\n"
    "void main(){ o = texture(u_t, v_p / u_size); }\n";

static int aa_target(int w, int h, int samples) {
    if (aa_fbo && w <= aa_w && h <= aa_h && samples == aa_samples) return 1;
    if (w < aa_w) w = aa_w;
    if (h < aa_h) h = aa_h;
    if (!aa_fbo) {
        glGenFramebuffers(1, &aa_fbo); glGenRenderbuffers(1, &aa_color_rb); glGenRenderbuffers(1, &aa_depth_rb);
        glGenFramebuffers(1, &aa_res_fbo); glGenTextures(1, &aa_res_tex);
        prog_comp = link(vs_quad, fs_comp);
        uc_rect = glGetUniformLocation(prog_comp, "u_rect");
        uc_size = glGetUniformLocation(prog_comp, "u_size");
    }
    glBindRenderbuffer(GL_RENDERBUFFER, aa_color_rb);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_RGBA8, w, h);
    glBindRenderbuffer(GL_RENDERBUFFER, aa_depth_rb);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_DEPTH24_STENCIL8, w, h);
    glBindFramebuffer(GL_FRAMEBUFFER, aa_fbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, aa_color_rb);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, aa_depth_rb);
    int ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glBindTexture(GL_TEXTURE_2D, aa_res_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, aa_res_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, aa_res_tex, 0);
    ok = ok && glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE && prog_comp;
    aa_w = w; aa_h = h; aa_samples = samples;
    aa_depth_serial = 0xFFFFFFFFu;
    if (!ok) { LOG("models: anti-aliasing target failed, drawing without\n"); models_aa = 0; }
    return ok;
}
float models_zback; /* models.h */
const ActorPaint *models_paint;
float models_spec = 1.0f;
/* a scene actor's painted look (models.h ActorPaint): the composite of its resolved anti-aliased picture through a
 * blur (colour premultiplied by coverage, so the edge softens with it), a darkening near its silhouette and holes (a
 * ring of 12 coverage taps), a grade, a tint toward the room colour and a still grain */
static GLuint prog_paint;
static GLint up_rect, up_size, up_p0, up_p1, up_p2, up_p3, up_p4;
static const char *fs_paint =
    "#version 330 core\n"
    "uniform sampler2D u_t; uniform vec2 u_size; uniform vec4 u_p0; uniform vec4 u_p1; uniform vec4 u_p2; uniform vec4 u_p3; uniform vec4 u_p4;\n"
    "in vec2 v_p; out vec4 o;\n"
    "/* p0: blur sigma (pixels of this target), ao, ao radius (pixels), grain; p1: gain, sat, con; p2: tint rgb, k;\n"
    " * p3.x: pixels per 8x pixel (the grain cell) */\n"
    "vec4 at(vec2 p){ return texelFetch(u_t, ivec2(clamp(p, vec2(0.0), u_size - 1.0)), 0); }\n"
    "float hash(vec2 p){ return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453); }\n"
    "void main(){\n"
    "  vec2 p = floor(v_p);\n"
    "  float s = u_p0.x; vec4 c = vec4(0.0); float ws = 0.0;\n"
    "  int R = int(min(ceil(s * 2.5), 8.0));\n"
    "  float near = 0.0; /* nothing of the mesh within the kernel: out early (most of the area) */\n"
    "  for (int y = -1; y <= 1; y++) for (int x = -1; x <= 1; x++) near += at(p + vec2(x, y) * float(max(R, 1))).a;\n"
    "  /* u_p4: a soft drop shadow on the room behind (strength, offset x, y, softness radius; pixels of this target) */\n"
    "  float sh = 0.0;\n"
    "  if (u_p4.x > 0.0) { vec2 q = p - u_p4.yz; sh = at(q).a;\n"
    "    for (int k = 0; k < 12; k++) { float a = 6.2831853 * float(k) / 12.0; sh += at(q + u_p4.w * vec2(cos(a), sin(a))).a; }\n"
    "    sh = u_p4.x * sh / 13.0; near += sh; }\n"
    "  if (near < 0.002) discard;\n"
    "  if (s < 0.05) { c = at(p); ws = 1.0; }\n"
    "  else for (int y = -R; y <= R; y++) for (int x = -R; x <= R; x++) {\n"
    "    float w = exp(-float(x * x + y * y) / (2.0 * s * s)); c += w * at(p + vec2(x, y)); ws += w; }\n"
    "  c /= ws;\n"
    "  /* p1.w, detail: 0 the blurred picture, 1 the sharp one, above 1 the sharp one's detail boosted (an unsharp mask:\n"
    "   * the HYPIR plates' crisp local contrast); the coverage never sharper than the mesh's own */\n"
    "  vec4 c0 = at(p);\n"
    "  vec3 dd = c0.rgb - c.rgb; /* the boost above 1 on the luma only: per channel it threw coloured speckles */\n"
    "  c = vec4(c.rgb + clamp(u_p1.w, 0.0, 1.0) * dd + max(u_p1.w - 1.0, 0.0) * vec3(dot(dd, vec3(0.299, 0.587, 0.114))),\n"
    "           c.a + clamp(u_p1.w, 0.0, 1.0) * (c0.a - c.a));\n"
    "  c.rgb = clamp(c.rgb, vec3(0.0), vec3(c.a));\n"
    "  if (c.a < 0.002 && sh < 0.002) discard;\n"
    "  vec3 col = c.rgb / max(c.a, 1e-4);\n"
    "  if (u_p0.y > 0.0) { /* the painted recesses: dark where the ring round the pixel is not all covered */\n"
    "    float cov = 0.0;\n"
    "    for (int k = 0; k < 12; k++) { float a = 6.2831853 * float(k) / 12.0;\n"
    "      cov += at(p + u_p0.z * vec2(cos(a), sin(a))).a + at(p + 0.5 * u_p0.z * vec2(cos(a + 0.26), sin(a + 0.26))).a; }\n"
    "    cov /= 24.0; col *= 1.0 - u_p0.y * (1.0 - cov); }\n"
    "  col *= u_p1.x;\n"
    "  float l = dot(col, vec3(0.299, 0.587, 0.114));\n"
    "  col = mix(vec3(l), col, u_p1.y);\n"
    "  col = max((col - 0.18) * u_p1.z + 0.18, 0.0);\n"
    "  l = dot(col, vec3(0.299, 0.587, 0.114)); float tl = max(dot(u_p2.rgb, vec3(0.299, 0.587, 0.114)), 1e-3);\n"
    "  col = mix(col, l * u_p2.rgb / tl, u_p2.w);\n"
    "  col += u_p0.w * (hash(floor(p / u_p3.x)) - 0.5);\n"
    "  o = vec4(clamp(col, 0.0, 1.0) * c.a, c.a + (1.0 - c.a) * sh); /* the shadow: black under the wheel */\n"
    "}\n";
static GLint um_bones, um_proj, um_off, um_ink, um_shade, um_atlas, um_colors, um_fix, um_fix2;
static int model_dbg = -1; /* FE_MODEL_DBG */
static uint32_t mesh_depth_serial = 0xFFFFFFFFu;

static const char *vs_mesh =
    "#version 330 core\n"
    "layout(location=0) in vec3 a_pos; layout(location=1) in vec3 a_nrm; layout(location=2) in vec2 a_uv;\n"
    "layout(location=3) in uvec4 a_j; layout(location=4) in vec4 a_w; layout(location=5) in uint a_face;\n"
    "layout(location=6) in vec3 a_ink; layout(location=7) in uvec4 a_lf; layout(location=8) in vec4 a_lw;\n"
    "layout(location=9) in vec3 a_sn;\n"
    "uniform vec4 u_bones[60]; uniform vec4 u_proj; uniform vec4 u_off; uniform vec4 u_ink; uniform vec4 u_fix;\n"
    "uniform sampler2D u_colors; uniform vec4 u_fix2;\n"
    "out vec2 v_uv; out vec3 v_nrm; flat out uint v_face; out vec3 v_light; out float v_z;\n"
    "/* dual-quaternion skinning (a scene actor's \"skin=dq\"; tools/skin_ref.py is the same maths): per bone its rotation\n"
    " * quaternion and dual part (u_dq[2j], [2j+1]) of the bone without the 512-wide mode's y squash and the actor's\n"
    " * uniform size; u_dqp: x the squash (signed), y the size, z on */\n"
    "uniform vec4 u_dq[40]; uniform vec4 u_dqp;\n"
    "vec3 qrot(vec4 q, vec3 v){ return v + 2.0 * cross(q.xyz, cross(q.xyz, v) + q.w * v); }\n"
    "void main(){\n"
    "  /* u_fix.x: the game's lit colours blended over the surface (mlook.c) instead of one per original face */\n"
    "  v_light = vec3(0.0);\n"
    "  if (u_fix.x > 0.0) for (int i = 0; i < 4; i++) v_light += a_lw[i] * texelFetch(u_colors, ivec2(int(a_lf[i]), 0), 0).rgb;\n"
    "  vec3 p = vec3(0.0), n = vec3(0.0), k = vec3(0.0); float ws = 0.0;\n"
    "  if (u_dqp.z > 0.0) {\n"
    "    vec4 qr = vec4(0.0), qd = vec4(0.0), q0 = vec4(0.0); bool f0 = true;\n"
    "    for (int i = 0; i < 4; i++) {\n"
    "      float w = a_w[i]; if (w <= 0.0) continue;\n"
    "      int j = int(a_j[i]) * 2; vec4 r = u_dq[j], d = u_dq[j + 1];\n"
    "      if (f0) { q0 = r; f0 = false; } else if (dot(r, q0) < 0.0) { r = -r; d = -d; } /* the shortest way */\n"
    "      qr += w * r; qd += w * d;\n"
    "    }\n"
    "    float ql = max(length(qr), 1e-8); qr /= ql; qd /= ql;\n"
    "    vec3 tq = 2.0 * (qr.w * qd.xyz - qd.w * qr.xyz + cross(qr.xyz, qd.xyz));\n"
    "    vec3 an = u_fix.y > 0.0 ? a_sn : a_nrm;\n"
    "    p = qrot(qr, a_pos * u_dqp.y) + tq; n = qrot(qr, an); k = qrot(qr, a_ink);\n"
    "    p.y *= u_dqp.x; n.y *= u_dqp.x; k.y *= u_dqp.x; ws = 1.0;\n"
    "  } else\n"
    "  for (int i = 0; i < 4; i++) {\n"
    "    float w = a_w[i]; if (w <= 0.0) continue;\n"
    "    int j = int(a_j[i]) * 3; vec4 r0 = u_bones[j], r1 = u_bones[j + 1], r2 = u_bones[j + 2];\n"
    "    p += w * vec3(dot(r0.xyz, a_pos) + r0.w, dot(r1.xyz, a_pos) + r1.w, dot(r2.xyz, a_pos) + r2.w);\n"
    "    vec3 an = u_fix.y > 0.0 ? a_sn : a_nrm;\n"
    "    n += w * vec3(dot(r0.xyz, an), dot(r1.xyz, an), dot(r2.xyz, an));\n"
    "    k += w * vec3(dot(r0.xyz, a_ink), dot(r1.xyz, a_ink), dot(r2.xyz, a_ink));\n"
    "    ws += w;\n"
    "  }\n"
    "  if (ws > 0.0) p /= ws;\n"
    "  /* view space of the GTE: y is squashed by the 512-wide mode (u_proj.w); normals unsquashed for the light */\n"
    "  v_nrm = vec3(n.x, n.y / u_proj.w, n.z);\n"
    "  /* MFIX_NORMALS: the head's shading normals (stored at length 2) lean toward the camera, as cel-shaded faces do:\n"
    "   * the shadow band keeps to the far side of the face instead of crossing the cheek and jaw */\n"
    "  if (u_fix.y > 0.0) {\n"
    "    float hf = clamp(length(v_nrm) / max(ws, 1e-4) - 1.0, 0.0, 1.0);\n"
    "    v_nrm = normalize(mix(normalize(v_nrm), normalize(-vec3(p.x, p.y / u_proj.w, p.z)), 0.45 * hf));\n"
    "  }\n"
    "  if (u_ink.x > 0.0) {\n"
    "    vec3 kn = vec3(k.x, k.y / u_proj.w, k.z); float kl = length(kn); kn = kl > 1e-6 ? kn / kl : vec3(0.0);\n"
    "    float z0 = max(p.z, 1.0);\n"
    "    float wpx = u_ink.x; if (u_ink.z > 0.0) wpx = min(wpx, u_ink.z * u_proj.x / z0);\n"
    "    wpx = max(wpx, u_fix2.x); /* MFIX_INK: never thinner than about one and a half screen pixels */\n"
    "    p += vec3(kn.x, kn.y * u_proj.w, kn.z) * wpx * z0 / u_proj.x;\n"
    "    float zb = max(p.z, 1.0); float back = (zb + u_ink.y) / zb;\n"
    "    p.xy *= back; p.z += u_ink.y;\n"
    "  }\n"
    "  float z = max(p.z, 1.0);\n"
    "  float vx = u_proj.y + u_proj.x * p.x / z + u_off.x, vy = u_proj.z + u_proj.x * p.y / z + u_off.y;\n"
    "  float d = clamp((z + u_off.z) / 65536.0, 0.0, 1.0) * 2.0 - 1.0; /* u_off.z: a scene actor's depth anchor */\n"
    "  v_z = z + u_off.z; /* the depth it sorts at: against a scene matte (fs) */\n"
    "  gl_Position = vec4((vx / 512.0 - 1.0) * z, (vy / 256.0 - 1.0) * z, d * z, z);\n"
    "  v_uv = a_uv; v_face = a_face;\n"
    "}\n";

static const char *fs_mesh =
    "#version 330 core\n"
    "uniform sampler2D u_atlas; uniform sampler2D u_colors; uniform vec4 u_ink; uniform float u_shade;\n"
    "uniform vec4 u_fix; uniform vec4 u_fix2;\n"
    "uniform sampler2D u_matte; uniform vec4 u_mt; uniform vec4 u_mt2;\n"
    "in vec2 v_uv; in vec3 v_nrm; flat in uint v_face; in vec3 v_light; in float v_z; out vec4 o;\n"
    "void main(){\n"
    "  /* a scene matte (models.c MATTE): the picture's foreground at this pixel lies nearer than the actor: not drawn */\n"
    "  if (u_mt2.x > 0.0) { vec2 mu = gl_FragCoord.xy * u_mt.xy + u_mt.zw;\n"
    "    if (mu.x >= 0.0 && mu.y >= 0.0 && mu.x < 1.0 && mu.y < 1.0 && v_z > texture(u_matte, mu).r) discard; }\n"
    "  vec4 tex = u_ink.x > 0.0 ? vec4(22.0, 18.0, 26.0, 255.0) / 255.0 : texture(u_atlas, v_uv);\n"
    "  if (tex.a < 0.5) discard;\n"
    "  vec3 light = u_fix.x > 0.0 ? v_light : texelFetch(u_colors, ivec2(int(v_face), 0), 0).rgb;\n"
    "  vec3 col = tex.rgb * light * 2.0;\n"
    "  vec3 l = normalize(vec3(-0.45, -0.65, -0.6));\n"
    "  if (u_shade < 0.0) {\n"
    "    vec3 n = normalize(v_nrm); float d = dot(n, l); float amb = -u_shade;\n"
    "    float k = amb + (1.0 - amb) * clamp(d * 0.6 + 0.4, 0.0, 1.0) * 1.12;\n"
    "    vec3 h = normalize(l + vec3(0.0, 0.0, -1.0));\n"
    "    float spec = pow(clamp(dot(n, h), 0.0, 1.0), 28.0) * 0.18 * u_fix2.z; /* z: a scene actor's spec= (1) */\n"
    "    col = col * k + spec * light * 2.0;\n"
    "  } else if (u_shade > 0.0) {\n"
    "    vec3 n = normalize(v_nrm); float d = dot(n, l); float aa = max(fwidth(d), 0.0001);\n"
    "    float lit = smoothstep(0.30 - aa, 0.30 + aa, d), mid = smoothstep(-0.20 - aa, -0.20 + aa, d);\n"
    "    float k = mix(u_shade, 0.92, mid); k = mix(k, 1.10, lit); col *= k;\n"
    "  }\n"
    "  /* MFIX_SHEEN (u_fix2.y): dark fabric (the wetsuits, gloves) catches the light in a thin hard band at its lit edge,\n"
    "   * as cel-shaded leather and rubber do, instead of reading as one flat dark grey */\n"
    "  if (u_fix2.y > 0.0 && u_ink.x <= 0.0) {\n"
    "    vec3 n = normalize(v_nrm); float rim = 1.0 - clamp(-n.z, 0.0, 1.0); float aa = max(fwidth(rim), 0.0001);\n"
    "    float band = smoothstep(0.55 - aa, 0.55 + aa, rim) * smoothstep(-0.05, 0.15, dot(n, l));\n"
    "    float lum = dot(tex.rgb, vec3(0.3, 0.55, 0.15)); float dark = 1.0 - smoothstep(0.10, 0.30, lum);\n"
    "    col += u_fix2.y * band * dark * light * 2.0 * vec3(0.80, 0.86, 1.0);\n"
    "  }\n"
    "  o = vec4(min(col, vec3(1.0)), u_ink.w);\n"
    "  /* FE_MODEL_DBG: 1 texture only, 2 the game's light only, 3 the cel factor only (diagnosis) */\n"
    "  if (u_fix.z > 0.5 && u_ink.x <= 0.0) {\n"
    "    vec3 n = normalize(v_nrm); float d = dot(n, l); float aa = max(fwidth(d), 0.0001);\n"
    "    float lit = smoothstep(0.30 - aa, 0.30 + aa, d), mid = smoothstep(-0.20 - aa, -0.20 + aa, d);\n"
    "    float k = mix(u_shade, 0.92, mid); k = mix(k, 1.10, lit);\n"
    "    o.rgb = u_fix.z < 1.5 ? tex.rgb : u_fix.z < 2.5 ? min(light * 2.0, vec3(1.0)) : vec3(k * 0.8);\n"
    "  }\n"
    "}\n";

typedef void (APIENTRY *PFN_TFV)(GLuint, GLsizei, const char *const *, GLenum);
typedef void (APIENTRY *PFN_TFB)(GLenum);
typedef void (APIENTRY *PFN_TFE)(void);
typedef void (APIENTRY *PFN_BBB)(GLenum, GLuint, GLuint);
typedef void (APIENTRY *PFN_GBS)(GLenum, GLintptr, GLsizeiptr, void *);
static PFN_TFV tf_varyings;
static PFN_TFB tf_begin;
static PFN_TFE tf_end;
static PFN_BBB tf_base;
static PFN_GBS tf_get;
typedef void (APIENTRY *PFN_DBF)(GLsizei, const GLuint *);
static PFN_DBF tf_del;
static GLint um_dq, um_dqp;
int models_skin_dq;              /* models.h */
char models_skin_capture[1024];  /* models.h */
static GLuint link_tf(const char *vs, const char *fs) {
    GLuint p = glCreateProgram();
    GLuint a = compile(GL_VERTEX_SHADER, vs), b = compile(GL_FRAGMENT_SHADER, fs);
    if (!a || !b) return 0;
    glAttachShader(p, a);
    glAttachShader(p, b);
    static const char *const names[] = {"gl_Position", "v_z"};
    tf_varyings(p, 2, names, 0x8C8C /* GL_INTERLEAVED_ATTRIBS */);
    glLinkProgram(p);
    GLint ok;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) { char log[2048]; glGetProgramInfoLog(p, sizeof log, 0, log); LOG("hw renderer: link (skin capture): %s\n", log); return 0; }
    LOG("models: FE_DBG_SKIN: the mesh program captures its vertices\n");
    return p;
}

/* the bones as dual quaternions (skin=dq): each 3x4 bone is D (s R | t) with D = diag(1, k, 1) the 512-wide mode's y
 * squash (k signed: the view's y flip) and s the actor's uniform size, the same for every bone; R as a quaternion, the
 * translation D^-1 t as its dual part. 0 = the bones are not of that form (a non-uniform scale=): the linear blend. */
static int dq_bones(const float *bones, int nbones, uint32_t used, float *dq, float *kk, float *ss) {
    const float *B = bones;
    float r0 = sqrtf(B[0] * B[0] + B[1] * B[1] + B[2] * B[2]), r1 = sqrtf(B[4] * B[4] + B[5] * B[5] + B[6] * B[6]);
    float det = B[0] * (B[5] * B[10] - B[6] * B[9]) - B[1] * (B[4] * B[10] - B[6] * B[8]) + B[2] * (B[4] * B[9] - B[5] * B[8]);
    if (r0 < 1e-6f || r1 < 1e-6f) return 0;
    float s = r0, k = (det < 0 ? -1.0f : 1.0f) * r1 / r0;
    memset(dq, 0, sizeof(float) * 8 * MESH_MAXBONES);
    for (int j = 0; j < nbones && j < MESH_MAXBONES; j++) {
        const float *b = bones + 12 * j;
        float R[9];
        for (int c = 0; c < 3; c++) { R[c] = b[c] / s; R[3 + c] = b[4 + c] / (k * s); R[6 + c] = b[8 + c] / s; }
        int bad = 0;
        for (int x = 0; x < 3 && !bad; x++)
            for (int y = 0; y < 3; y++) {
                float d = R[x * 3] * R[y * 3] + R[x * 3 + 1] * R[y * 3 + 1] + R[x * 3 + 2] * R[y * 3 + 2] - (x == y ? 1.0f : 0.0f);
                if (fabsf(d) > 5e-3f) { bad = 1; break; }
            }
        float *q = dq + 8 * j;
        if (bad) { if (used >> j & 1) return 0; q[3] = 1.0f; continue; }
        float tr = R[0] + R[4] + R[8]; /* rows of R -> x, y, z, w (as models.c mat_quat) */
        if (tr > 0) { float t = sqrtf(tr + 1.0f) * 2; q[3] = 0.25f * t; q[0] = (R[7] - R[5]) / t; q[1] = (R[2] - R[6]) / t; q[2] = (R[3] - R[1]) / t; }
        else if (R[0] > R[4] && R[0] > R[8]) { float t = sqrtf(1.0f + R[0] - R[4] - R[8]) * 2; q[3] = (R[7] - R[5]) / t; q[0] = 0.25f * t; q[1] = (R[1] + R[3]) / t; q[2] = (R[2] + R[6]) / t; }
        else if (R[4] > R[8]) { float t = sqrtf(1.0f + R[4] - R[0] - R[8]) * 2; q[3] = (R[2] - R[6]) / t; q[0] = (R[1] + R[3]) / t; q[1] = 0.25f * t; q[2] = (R[5] + R[7]) / t; }
        else { float t = sqrtf(1.0f + R[8] - R[0] - R[4]) * 2; q[3] = (R[3] - R[1]) / t; q[0] = (R[2] + R[6]) / t; q[1] = (R[5] + R[7]) / t; q[2] = 0.25f * t; }
        float ql = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
        for (int i = 0; i < 4; i++) q[i] /= ql;
        float t3[3] = {b[3], b[7] / k, b[11]};
        /* dual = 1/2 (0, t) q */
        q[4] = 0.5f * (t3[0] * q[3] + t3[1] * q[2] - t3[2] * q[1]);
        q[5] = 0.5f * (-t3[0] * q[2] + t3[1] * q[3] + t3[2] * q[0]);
        q[6] = 0.5f * (t3[0] * q[1] - t3[1] * q[0] + t3[2] * q[3]);
        q[7] = -0.5f * (t3[0] * q[0] + t3[1] * q[1] + t3[2] * q[2]);
    }
    *kk = k; *ss = s;
    return 1;
}

/* FE_DBG_SKIN: this draw's vertices as the vertex shader puts them (gl_Position, v_z) with its inputs, to FILE.bin */
static void skin_capture(HwMesh *m, const float *bones, int nbones, const float *dq, const float *dqp, float h, float ofx,
                         float ofy, int offx, int offy, float ys) {
    GLuint buf;
    size_t n = m->nverts;
    glGenBuffers(1, &buf);
    glBindBuffer(0x8C8E /* GL_TRANSFORM_FEEDBACK_BUFFER */, buf);
    glBufferData(0x8C8E, (GLsizeiptr)(n * 20), 0, 0x88E9 /* GL_STREAM_READ */);
    tf_base(0x8C8E, 0, buf);
    glEnable(0x8C89 /* GL_RASTERIZER_DISCARD */);
    tf_begin(GL_POINTS);
    glDrawArrays(GL_POINTS, 0, (GLsizei)n);
    tf_end();
    glDisable(0x8C89);
    float *out = malloc(n * 20);
    if (out) tf_get(0x8C8E, 0, (GLsizeiptr)(n * 20), out);
    tf_base(0x8C8E, 0, 0);
    if (tf_del) tf_del(1, &buf);
    char p[1100];
    snprintf(p, sizeof p, "%s.bin", models_skin_capture);
    FILE *f = out ? fopen(p, "wb") : 0;
    if (f) {
        uint32_t hd[4] = {0x4E494B53u /* SKIN */, (uint32_t)n, (uint32_t)nbones, dqp[2] > 0 ? 1u : 0u};
        float pr[12] = {h, ofx, ofy, ys, (float)offx, (float)offy, models_zback, 0, dqp[0], dqp[1], dqp[2], dqp[3]};
        float ub[MESH_MAXBONES * 12];
        memset(ub, 0, sizeof ub);
        memcpy(ub, bones, sizeof(float) * 12 * (size_t)(nbones < MESH_MAXBONES ? nbones : MESH_MAXBONES));
        fwrite(hd, 4, 4, f); fwrite(pr, 4, 12, f); fwrite(ub, 4, MESH_MAXBONES * 12, f); fwrite(dq, 4, MESH_MAXBONES * 8, f);
        fwrite(out, 20, n, f);
        fclose(f);
        LOG("models: FE_DBG_SKIN: %u vertices (%s) to %s\n", (unsigned)n, dqp[2] > 0 ? "dual quaternions" : "linear blend", p);
    }
    free(out);
    models_skin_capture[0] = 0;
}

static int mesh_init(void) {
    static int state;
    if (state) return state > 0;
    state = -1;
    if (getenv("FE_DBG_SKIN")) { /* the skinning check (tools/skin_ref.py): the vertices captured by transform feedback */
        tf_varyings = (PFN_TFV)SDL_GL_GetProcAddress("glTransformFeedbackVaryings");
        tf_begin = (PFN_TFB)SDL_GL_GetProcAddress("glBeginTransformFeedback");
        tf_end = (PFN_TFE)SDL_GL_GetProcAddress("glEndTransformFeedback");
        tf_base = (PFN_BBB)SDL_GL_GetProcAddress("glBindBufferBase");
        tf_get = (PFN_GBS)SDL_GL_GetProcAddress("glGetBufferSubData");
        tf_del = (PFN_DBF)SDL_GL_GetProcAddress("glDeleteBuffers");
    }
    prog_mesh = tf_varyings && tf_begin && tf_end && tf_base && tf_get ? link_tf(vs_mesh, fs_mesh) : link(vs_mesh, fs_mesh);
    if (!prog_mesh) return 0;
    um_bones = glGetUniformLocation(prog_mesh, "u_bones");
    um_proj = glGetUniformLocation(prog_mesh, "u_proj");
    um_off = glGetUniformLocation(prog_mesh, "u_off");
    um_ink = glGetUniformLocation(prog_mesh, "u_ink");
    um_shade = glGetUniformLocation(prog_mesh, "u_shade");
    um_atlas = glGetUniformLocation(prog_mesh, "u_atlas");
    um_colors = glGetUniformLocation(prog_mesh, "u_colors");
    um_fix = glGetUniformLocation(prog_mesh, "u_fix");
    um_fix2 = glGetUniformLocation(prog_mesh, "u_fix2");
    um_matte = glGetUniformLocation(prog_mesh, "u_matte");
    um_mt = glGetUniformLocation(prog_mesh, "u_mt");
    um_mt2 = glGetUniformLocation(prog_mesh, "u_mt2");
    um_dq = glGetUniformLocation(prog_mesh, "u_dq");
    um_dqp = glGetUniformLocation(prog_mesh, "u_dqp");
    glGenRenderbuffers(1, &mesh_depth_rb);
    glBindRenderbuffer(GL_RENDERBUFFER, mesh_depth_rb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, W, H);
    glBindFramebuffer(GL_FRAMEBUFFER, fb_fbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, mesh_depth_rb);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) { LOG("models: depth buffer failed\n"); return 0; }
    glGenTextures(1, &mesh_colors_tex);
    glBindTexture(GL_TEXTURE_2D, mesh_colors_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 2048, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    state = 1;
    return 1;
}

/* the next mesh clears its depth buffer; fbo (if any) gets the plain path's depth buffer (the in-between target) */
static void mesh_depth_reset(GLuint fbo) {
    aa_depth_serial = mesh_depth_serial = 0xFFFFFFFFu;
    static GLuint attached;
    if (fbo && mesh_depth_rb && attached != fbo) {
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, mesh_depth_rb);
        glBindFramebuffer(GL_FRAMEBUFFER, draw_fbo);
        attached = fbo;
    }
}

/* d: the whole .hm file (read on a loader thread); freed here */
HwMesh *hw_mesh_create(uint8_t *d, long size, const char *path, const struct MLook *look) {
    if (!d) return 0;
    if (!hw_active || !mesh_init()) { free(d); return 0; }
    HwMesh *m = 0;
    if (size < 28 || memcmp(d, "FE2HM01", 8)) goto bad;
    uint32_t cnt[5];
    memcpy(cnt, d + 8, 20);
    size_t o = 28 + (size_t)cnt[2] * 16;
    size_t vbytes = (size_t)cnt[0] * sizeof(HmVert);
    if (sizeof(HmVert) != 44 || o + vbytes + (size_t)cnt[1] * 4 > (size_t)size) goto bad;
    m = calloc(1, sizeof *m);
    m->nverts = cnt[0]; m->nidx = cnt[1]; m->nfaces = cnt[4];
    HmVert *v = (HmVert *)(d + o);
    for (uint32_t i = 0; i < m->nverts; i++)
        for (int k = 0; k < 4; k++) if (v[i].joints[k] >= MESH_MAXBONES) v[i].weights[k] = 0;
    for (uint32_t i = 0; i < m->nverts; i++) { /* each bone's box (the AA work's box in hw_mesh_draw) */
        int any = 0;
        for (int k = 0; k < 4; k++) {
            if (!v[i].weights[k]) continue;
            int j = v[i].joints[k];
            float *b = m->jbox[j];
            if (!(m->jmask >> j & 1)) { for (int a = 0; a < 3; a++) b[a] = b[3 + a] = v[i].pos[a]; m->jmask |= 1u << j; }
            for (int a = 0; a < 3; a++) {
                if (v[i].pos[a] < b[a]) b[a] = v[i].pos[a];
                if (v[i].pos[a] > b[3 + a]) b[3 + a] = v[i].pos[a];
            }
            any = 1;
        }
        if (!any) m->jbox_bad = 1;
    }
    /* outline normals: averaged over every vertex at the same position, so the shell stays closed at seams */
    float *ink = calloc((size_t)m->nverts * 3, sizeof(float));
    {
        uint32_t hn = 1;
        while (hn < m->nverts * 2) hn <<= 1;
        int32_t *head = malloc(sizeof(int32_t) * hn), *next = malloc(sizeof(int32_t) * m->nverts);
        for (uint32_t i = 0; i < hn; i++) head[i] = -1;
        uint32_t *key = malloc(sizeof(uint32_t) * m->nverts);
        for (uint32_t i = 0; i < m->nverts; i++) {
            uint32_t k = 2166136261u;
            for (int a = 0; a < 3; a++) k = (k ^ (uint32_t)lroundf(v[i].pos[a] * 1000.0f)) * 16777619u;
            key[i] = k;
            next[i] = head[k & (hn - 1)];
            head[k & (hn - 1)] = (int32_t)i;
        }
        for (uint32_t i = 0; i < m->nverts; i++) {
            float s[3] = {0, 0, 0};
            for (int32_t j = head[key[i] & (hn - 1)]; j >= 0; j = next[j])
                if (key[j] == key[i] && !memcmp(v[j].pos, v[i].pos, 12))
                    for (int a = 0; a < 3; a++) s[a] += v[j].nrm[a];
            float l = sqrtf(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
            for (int a = 0; a < 3; a++) ink[i * 3 + a] = l > 1e-6f ? s[a] / l : v[i].nrm[a];
        }
        free(head); free(next); free(key);
    }
    glGenVertexArrays(1, &m->vao);
    glBindVertexArray(m->vao);
    glGenBuffers(1, &m->vbo);
    glBindBuffer(GL_ARRAY_BUFFER, m->vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)vbytes, v, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 44, (void *)0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 44, (void *)12);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 44, (void *)24);
    glVertexAttribIPointer(3, 4, GL_UNSIGNED_BYTE, 44, (void *)32);
    glVertexAttribPointer(4, 4, GL_UNSIGNED_BYTE, GL_TRUE, 44, (void *)36);
    glVertexAttribIPointer(5, 1, GL_UNSIGNED_SHORT, 44, (void *)40);
    glGenBuffers(1, &m->ink_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, m->ink_vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(m->nverts * 12), ink, GL_STATIC_DRAW);
    glVertexAttribPointer(6, 3, GL_FLOAT, GL_FALSE, 12, (void *)0);
    for (int i = 0; i < 7; i++) glEnableVertexAttribArray(i);
    free(ink);
    if (look && mlook_nverts(look) == m->nverts) { /* faces (4 x u16) and weights (4 x u8) per vertex, one buffer */
        size_t n = m->nverts;
        uint8_t *lb = malloc(n * 12);
        const uint16_t *lf = mlook_vfaces(look);
        const uint8_t *lw = mlook_vweights(look);
        for (size_t i = 0; i < n; i++) { memcpy(lb + i * 12, lf + i * 4, 8); memcpy(lb + i * 12 + 8, lw + i * 4, 4); }
        glGenBuffers(1, &m->light_vbo);
        glBindBuffer(GL_ARRAY_BUFFER, m->light_vbo);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(n * 12), lb, GL_STATIC_DRAW);
        glVertexAttribIPointer(7, 4, GL_UNSIGNED_SHORT, 12, (void *)0);
        glVertexAttribPointer(8, 4, GL_UNSIGNED_BYTE, GL_TRUE, 12, (void *)8);
        glEnableVertexAttribArray(7);
        glEnableVertexAttribArray(8);
        free(lb);
        glGenBuffers(1, &m->nrm_vbo);
        glBindBuffer(GL_ARRAY_BUFFER, m->nrm_vbo);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(n * 12), mlook_vnormals(look), GL_STATIC_DRAW);
        glVertexAttribPointer(9, 3, GL_FLOAT, GL_FALSE, 12, (void *)0);
        glEnableVertexAttribArray(9);
        m->has_look = 1;
    }
    o += vbytes;
    glGenBuffers(1, &m->ibo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m->ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)(m->nidx * 4), d + o, GL_STATIC_DRAW);
    o += (size_t)m->nidx * 4;
    glBindVertexArray(vao);
    m->nmat = (int)cnt[3];
    m->mats = calloc((size_t)m->nmat + 1, sizeof *m->mats);
    for (int i = 0; i < m->nmat; i++) {
        uint32_t h[4];
        if (o + 16 > (size_t)size) goto bad;
        memcpy(h, d + o, 16);
        o += 16;
        size_t bytes = (size_t)h[2] * h[3] * 4;
        if (o + bytes > (size_t)size || h[0] + h[1] > m->nidx) goto bad;
        m->mats[i].first = h[0]; m->mats[i].count = h[1];
        glGenTextures(1, &m->mats[i].tex);
        glBindTexture(GL_TEXTURE_2D, m->mats[i].tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)h[2], (GLsizei)h[3], 0, GL_RGBA, GL_UNSIGNED_BYTE, d + o);
        /* filtered with mipmaps: the remodels' textures are drawn smaller than their size (the face, the cloth's
         * drawn edges); nearest sampling, as in the fork (played at 8x there), left them blocky and shimmering at 4x */
        glGenerateMipmap(GL_TEXTURE_2D);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameterf(GL_TEXTURE_2D, 0x84FE /* GL_TEXTURE_MAX_ANISOTROPY */, 16.0f); /* the driver caps it */
        while (glGetError() != GL_NO_ERROR) {} /* no anisotropic filtering: fine */
        o += bytes;
    }
    free(d);
    LOG("models: %s: %u vertices, %u triangles, %d materials\n", path, m->nverts, m->nidx / 3, m->nmat);
    return m;
bad:
    LOG("models: %s: not a valid .hm file\n", path);
    free(d);
    free(m);
    return 0;
}

/* the mesh's box on screen (VRAM native pixels, x0 y0 x1 y1): its bones' boxes projected (the skinned vertices are
 * blends of their bones' moves, so they lie in the hull of those corners), grown by the ink outline's widest reach;
 * 0 = not known (a corner behind the camera, a vertex without weight, a bone not given) */
static int mesh_box(const HwMesh *m, const float *bones, int nbones, float h, float ofx, float ofy, int offx, int offy,
                    float *box) {
    if (m->jbox_bad || !m->jmask) return 0;
    float bx0 = 1e30f, by0 = 1e30f, bx1 = -1e30f, by1 = -1e30f;
    for (int j = 0; j < MESH_MAXBONES; j++) {
        if (!(m->jmask >> j & 1)) continue;
        if (j >= nbones) return 0;
        const float *B = bones + 12 * j, *b = m->jbox[j];
        for (int c = 0; c < 8; c++) {
            float v[3] = {b[(c & 1) ? 3 : 0], b[(c & 2) ? 4 : 1], b[(c & 4) ? 5 : 2]}, p[3];
            for (int r = 0; r < 3; r++) p[r] = B[r * 4] * v[0] + B[r * 4 + 1] * v[1] + B[r * 4 + 2] * v[2] + B[r * 4 + 3];
            if (!(p[2] >= 2.0f)) return 0;
            float sx = ofx + h * p[0] / p[2] + (float)offx, sy = ofy + h * p[1] / p[2] + (float)offy;
            if (sx < bx0) bx0 = sx;
            if (sx > bx1) bx1 = sx;
            if (sy < by0) by0 = sy;
            if (sy > by1) by1 = sy;
        }
    }
    /* the ink hulls move a vertex out by up to wpx native pixels (and toward the camera by as much in depth, which
     * moves it out by another x / h of that): the widest of the inner, silhouette and minimum widths, doubled */
    float k = models_ink_inner > models_ink_sil ? models_ink_inner : models_ink_sil;
    if (k < 1.0f) k = 1.0f;
    float w = models_outline_px * k, wm = models_ink_min / (float)hw_scale * 1.4f;
    if (w < wm) w = wm;
    float pad = 2.0f * w * (1.0f + 1024.0f / h) + 2.0f;
    box[0] = bx0 - pad; box[1] = by0 - pad; box[2] = bx1 + pad; box[3] = by1 + pad;
    return 1;
}

void hw_mesh_draw(HwMesh *m, const float *bones, int nbones, const uint32_t *colors, int ncolors, float h, float ofx,
                  float ofy) {
    if (!hw_active || !m || h < 1.0f) return;
    hw_flush();
    uint32_t pass = smooth_replaying ? smooth_pass : models_dma_serial;
    int x1, y1, x2, y2, offx, offy;
    if (mesh_area_set) { x1 = mesh_area[0]; y1 = mesh_area[1]; x2 = mesh_area[2]; y2 = mesh_area[3]; offx = mesh_area[4]; offy = mesh_area[5]; }
    else gpu_draw_area(&x1, &y1, &x2, &y2, &offx, &offy);
    if (x2 < x1 || y2 < y1) return;
    int S = hw_scale, aw = (x2 - x1 + 1) * S, ah = (y2 - y1 + 1) * S;
    int aa = models_aa > 1 && aa_target(aw, ah, models_aa);
    if (gpt_on > 0) { if (smooth_replaying) gpt_meshes_mid++; else gpt_meshes++; }
    /* the AA work (clear, resolve, composite) over the mesh's box only, not the whole area (7 Nov, NOTES "Scene actors:
     * crowd capacity"): outside it the mesh has no coverage, so the composite left those pixels as they were anyway.
     * A painted look reads round its pixels: the whole area. */
    int bx = 0, by = 0, bw = aw, bh = ah;
    float mb[4];
    /* skin=dq: the bones as dual quaternions (the linear blend if they are not rigid but for the squash and one size) */
    static float dq[MESH_MAXBONES * 8];
    float dqp[4] = {0, 0, 0, 0};
    if (models_skin_dq && !getenv("FE_SKIN_DQ_OFF")) {
        if (dq_bones(bones, nbones, m->jmask, dq, &dqp[0], &dqp[1])) dqp[2] = 1.0f;
        else {
            static int warned;
            if (warned++ < 5) LOG("models: skin=dq: bones not rigid (a non-uniform scale=?): the linear blend\n");
        }
    }
    if (aa && !models_paint && !aa_box_old() && mesh_box(m, bones, nbones, h, ofx, ofy, offx, offy, mb)) {
        if (dqp[2] > 0.0f) { /* a blend of rotations bulges out of the bones' hull by at most half its size */
            float gx = 0.5f * (mb[2] - mb[0]), gy = 0.5f * (mb[3] - mb[1]), g = gx > gy ? gx : gy;
            mb[0] -= g; mb[1] -= g; mb[2] += g; mb[3] += g;
        }
        int X0 = (int)floorf((mb[0] - (float)x1) * (float)S), Y0 = (int)floorf((mb[1] - (float)y1) * (float)S);
        int X1 = (int)ceilf((mb[2] - (float)x1) * (float)S), Y1 = (int)ceilf((mb[3] - (float)y1) * (float)S);
        if (X0 < 0) X0 = 0;
        if (Y0 < 0) Y0 = 0;
        if (X1 > aw) X1 = aw;
        if (Y1 > ah) Y1 = ah;
        bx = X0; by = Y0; bw = X1 - X0; bh = Y1 - Y0;
        if (bw <= 0 || bh <= 0) return; /* wholly outside the area: nothing of it would be drawn */
    }
    if (aa) {
        glBindFramebuffer(GL_FRAMEBUFFER, aa_fbo);
        glViewport(-x1 * S, -y1 * S, W, H); /* same VRAM mapping as the upscaled copy, shifted to the area */
        glEnable(GL_SCISSOR_TEST);
        glScissor(0, 0, aw, ah);
        if (aa_depth_serial != pass) {
            aa_depth_serial = pass;
            glClearDepth(1.0);
            glClearStencil(0);
            glStencilMask(0xFF);
            glClear(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        }
        glScissor(bx, by, bw, bh);
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);
    } else {
        glBindFramebuffer(GL_FRAMEBUFFER, draw_fbo);
        glViewport(0, 0, W, H);
        glEnable(GL_SCISSOR_TEST);
        glScissor(x1 * S, y1 * S, aw, ah);
        if (mesh_depth_serial != pass) { /* one depth buffer per GPU pass */
            mesh_depth_serial = pass;
            glDisable(GL_SCISSOR_TEST);
            glClearDepth(1.0);
            glClearStencil(0);
            glStencilMask(0xFF);
            glClear(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
            glEnable(GL_SCISSOR_TEST);
        }
    }
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glUseProgram(prog_mesh);
    if (model_dbg < 0) model_dbg = getenv("FE_MODEL_DBG") ? atoi(getenv("FE_MODEL_DBG")) : 0;
    float ub[MESH_MAXBONES * 12];
    memset(ub, 0, sizeof ub);
    if (nbones > MESH_MAXBONES) nbones = MESH_MAXBONES;
    memcpy(ub, bones, sizeof(float) * 12 * (size_t)nbones);
    glUniform4fv(um_bones, MESH_MAXBONES * 3, ub);
    if (dqp[2] > 0.0f) glUniform4fv(um_dq, MESH_MAXBONES * 2, dq);
    glUniform4f(um_dqp, dqp[0], dqp[1], dqp[2], 0);
    /* the y squash of the GTE view (512-wide mode): row 1 against row 0 of the root part */
    float r0 = sqrtf(bones[0] * bones[0] + bones[1] * bones[1] + bones[2] * bones[2]);
    float r1 = sqrtf(bones[4] * bones[4] + bones[5] * bones[5] + bones[6] * bones[6]);
    float ys = (r0 > 0.01f && r1 > 0.01f) ? r1 / r0 : 0.625f;
    glUniform4f(um_proj, h, ofx, ofy, ys);
    glUniform4f(um_off, (float)offx, (float)offy, models_zback, 0);
    glUniform1f(um_shade, models_shade);
    glUniform4f(um_fix, (m->has_look && (model_fix & MFIX_LIGHT)) ? 1.0f : 0.0f,
                (m->has_look && (model_fix & MFIX_NORMALS)) ? 1.0f : 0.0f, (float)model_dbg, 0);
    int ink2 = (model_fix & MFIX_INK) && models_outline_px > 0.0f;
    glUniform4f(um_fix2, 0, (model_fix & MFIX_SHEEN) ? models_sheen : 0.0f, models_spec, 0);
    glUniform1i(um_atlas, 0);
    glUniform1i(um_colors, 1);
    glUniform1i(um_matte, 2);
    float mr[4] = {models_matte_rect[0], models_matte_rect[1], models_matte_rect[2], models_matte_rect[3]};
    if (models_matte_tex && mr[2] > mr[0] && mr[3] > mr[1]) {
        /* window pixel (gl_FragCoord) -> VRAM: / S, + the area's corner when drawn into the AA target -> the picture's
         * 0..1 (its top row first, as the PNG's) */
        float ox = aa ? (float)x1 : 0.0f, oy = aa ? (float)y1 : 0.0f, iw = 1.0f / (mr[2] - mr[0]), ih = 1.0f / (mr[3] - mr[1]);
        glUniform4f(um_mt, iw / (float)S, ih / (float)S, (ox - mr[0]) * iw, (oy - mr[1]) * ih);
        glUniform4f(um_mt2, 1, 0, 0, 0);
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, models_matte_tex);
    } else glUniform4f(um_mt2, 0, 0, 0, 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, mesh_colors_tex);
    if (ncolors > 2048) ncolors = 2048;
    if (ncolors > 0) glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, ncolors, 1, GL_RGBA, GL_UNSIGNED_BYTE, colors);
    glActiveTexture(GL_TEXTURE0);
    glBindVertexArray(m->vao);
    float alpha = aa ? 1.0f : 0.0f; /* coverage into the AA target; the mask bit (0) into VRAM */
    glUniform4f(um_ink, 0, 0, 0, alpha);
    if (models_skin_capture[0] && tf_begin) skin_capture(m, bones, nbones, dq, dqp, h, ofx, ofy, offx, offy, ys);
    if (ink2) { /* the bodies mark the stencil: the silhouette pass below draws only outside them */
        glEnable(GL_STENCIL_TEST);
        glStencilMask(0xFF);
        glStencilFunc(GL_ALWAYS, 1, 0xFF);
        glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
    }
    for (int i = 0; i < m->nmat; i++) {
        if (!m->mats[i].count) continue;
        glBindTexture(GL_TEXTURE_2D, m->mats[i].tex);
        glDrawElements(GL_TRIANGLES, (GLsizei)m->mats[i].count, GL_UNSIGNED_INT, (void *)(size_t)(m->mats[i].first * 4));
    }
    if (ink2) {
        /* MFIX_INK: two hulls. Inner lines (an arm over the body, folds): thin, as the old single hull. The
         * silhouette against the room: thicker, drawn only where no body is (stencil 0). Both at least ~1.5 screen
         * pixels wide (u_fix2.x, native pixels), so a far character keeps a line, and thinner with distance above it. */
        float minpx = models_ink_min / (float)S;
        glStencilMask(0x00);
        glDisable(GL_STENCIL_TEST);
        glUniform4f(um_fix2, minpx, 0, 0, 0);
        glUniform4f(um_ink, models_outline_px * models_ink_inner, models_outline_depth, models_outline_world * models_ink_inner, alpha);
        for (int i = 0; i < m->nmat; i++)
            if (m->mats[i].count)
                glDrawElements(GL_TRIANGLES, (GLsizei)m->mats[i].count, GL_UNSIGNED_INT, (void *)(size_t)(m->mats[i].first * 4));
        glEnable(GL_STENCIL_TEST);
        glStencilFunc(GL_EQUAL, 0, 0xFF);
        glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
        glUniform4f(um_fix2, minpx * 1.4f, 0, 0, 0);
        glUniform4f(um_ink, models_outline_px * models_ink_sil, models_outline_depth, models_outline_world * models_ink_sil, alpha);
        for (int i = 0; i < m->nmat; i++)
            if (m->mats[i].count)
                glDrawElements(GL_TRIANGLES, (GLsizei)m->mats[i].count, GL_UNSIGNED_INT, (void *)(size_t)(m->mats[i].first * 4));
        glDisable(GL_STENCIL_TEST);
        glStencilMask(0xFF);
    } else if (models_outline_px > 0.0f) {
        glUniform4f(um_ink, models_outline_px, models_outline_depth, models_outline_world, alpha);
        for (int i = 0; i < m->nmat; i++)
            if (m->mats[i].count)
                glDrawElements(GL_TRIANGLES, (GLsizei)m->mats[i].count, GL_UNSIGNED_INT, (void *)(size_t)(m->mats[i].first * 4));
    }
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);
    if (aa) {
        /* resolve, then lay the picture over VRAM: premultiplied by coverage; VRAM alpha (mask bit) kept */
        glBindFramebuffer(GL_READ_FRAMEBUFFER, aa_fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, aa_res_fbo);
        glBlitFramebuffer(bx, by, bx + bw, by + bh, bx, by, bx + bw, by + bh, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER, draw_fbo);
        glViewport(x1 * S + bx, y1 * S + by, bw, bh);
        const ActorPaint *pt = models_paint;
        if (pt && !prog_paint) {
            prog_paint = link(vs_quad, fs_paint);
            up_rect = glGetUniformLocation(prog_paint, "u_rect"); up_size = glGetUniformLocation(prog_paint, "u_size");
            up_p0 = glGetUniformLocation(prog_paint, "u_p0"); up_p1 = glGetUniformLocation(prog_paint, "u_p1");
            up_p2 = glGetUniformLocation(prog_paint, "u_p2"); up_p3 = glGetUniformLocation(prog_paint, "u_p3");
            up_p4 = glGetUniformLocation(prog_paint, "u_p4");
            if (!prog_paint) { LOG("models: painted look shader failed, drawn plain\n"); prog_paint = (GLuint)-1; }
        }
        if (pt && prog_paint != (GLuint)-1) { /* a scene actor's painted look (models.h ActorPaint) */
            float k8 = (float)S / 8.0f; /* the look's sizes are 8x pixels */
            glUseProgram(prog_paint);
            glUniform4f(up_rect, 0, 0, (float)aw, (float)ah);
            glUniform2f(up_size, (float)aa_w, (float)aa_h);
            glUniform4f(up_p0, pt->blur * k8, pt->ao, pt->ao_r * k8, pt->grain / 255.0f);
            glUniform4f(up_p1, pt->gain, pt->sat, pt->con, pt->detail);
            glUniform4f(up_p2, pt->tint[0], pt->tint[1], pt->tint[2], pt->tint_k);
            glUniform4f(up_p3, k8 > 1.0f ? k8 : 1.0f, 0, 0, 0);
            glUniform4f(up_p4, pt->shadow[0], pt->shadow[1] * k8, pt->shadow[2] * k8, pt->shadow[3] * k8);
        } else {
            glUseProgram(prog_comp);
            glUniform4f(uc_rect, (float)bx, (float)by, (float)bw, (float)bh);
            glUniform2f(uc_size, (float)aa_w, (float)aa_h);
        }
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, aa_res_tex);
        glEnable(GL_BLEND);
        glBlendEquation(GL_FUNC_ADD);
        glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
        glBindVertexArray(vao_empty);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glDisable(GL_BLEND);
        glViewport(0, 0, W, H);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, draw_fbo);
    glBindVertexArray(vao);
}

/* ------------------------------------------------------------------ sprite actors (models.c, actors.txt "sprite=")
 * A scene actor drawn as a pre-rendered picture of itself instead of its mesh: up to two layers (the neighbouring
 * poses of its picture set, each turned to the actor's exact angle), each a quad with its corners in screen terms
 * (x, y as the GTE projects them, view z) and its texture corners; the picture is premultiplied RGBA. As the meshes:
 * into the anti-aliasing target over the drawing area, depth-tested against the pass's shared depth buffer, then laid
 * over the upscaled VRAM; its depth (view z + zback) is written where the heavier layer's coverage is 1/2 or more,
 * so the game's models drawn after it sort against it like against a mesh. */
static GLuint prog_spr, spr_vao, spr_vbo;
static GLint us_tex, us_mul, us_cut;
static const char *vs_spr =
    "#version 330 core\n"
    "layout(location=0) in vec3 a_p; layout(location=1) in vec2 a_uv; noperspective out vec2 v_uv;\n"
    "void main(){ v_uv = a_uv; gl_Position = vec4(a_p.x / 512.0 - 1.0, a_p.y / 256.0 - 1.0, a_p.z, 1.0); }\n";
static const char *fs_spr =
    "#version 330 core\n"
    "uniform sampler2D u_t; uniform vec4 u_mul; uniform float u_cut; noperspective in vec2 v_uv; out vec4 o;\n"
    "/* u_mul: the light per channel (1 = as rendered), w: the layer's weight; u_cut > 0: the depth pass */\n"
    "void main(){ vec4 c = texture(u_t, v_uv);\n"
    "  if (u_cut > 0.0) { if (c.a < u_cut) discard; o = vec4(0.0); return; }\n"
    "  o = vec4(min(c.rgb * u_mul.rgb, vec3(c.a)), c.a) * u_mul.w; }\n";

void hw_sprite_draw(unsigned tex, int nl, const float (*xyz)[4][3], const float (*uv)[4][2], const float *wt,
                    const float *mul, float zback) {
    if (!hw_active || !tex || nl < 1 || !mesh_init()) return;
    hw_flush();
    if (!prog_spr) {
        prog_spr = link(vs_spr, fs_spr);
        if (!prog_spr) { LOG("models: sprite shader failed\n"); prog_spr = (GLuint)-1; }
        else {
            us_tex = glGetUniformLocation(prog_spr, "u_t"); us_mul = glGetUniformLocation(prog_spr, "u_mul");
            us_cut = glGetUniformLocation(prog_spr, "u_cut");
            glGenVertexArrays(1, &spr_vao);
            glGenBuffers(1, &spr_vbo);
            glBindVertexArray(spr_vao);
            glBindBuffer(GL_ARRAY_BUFFER, spr_vbo);
            glBufferData(GL_ARRAY_BUFFER, sizeof(float) * 5 * 8, 0, GL_DYNAMIC_DRAW);
            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 20, (void *)0);
            glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 20, (void *)12);
            glEnableVertexAttribArray(0);
            glEnableVertexAttribArray(1);
            glBindVertexArray(vao);
        }
    }
    if (prog_spr == (GLuint)-1) return;
    if (nl > 2) nl = 2;
    uint32_t pass = smooth_replaying ? smooth_pass : models_dma_serial;
    int x1, y1, x2, y2, offx, offy;
    if (mesh_area_set) { x1 = mesh_area[0]; y1 = mesh_area[1]; x2 = mesh_area[2]; y2 = mesh_area[3]; offx = mesh_area[4]; offy = mesh_area[5]; }
    else gpu_draw_area(&x1, &y1, &x2, &y2, &offx, &offy);
    if (x2 < x1 || y2 < y1) return;
    float vb[8][5];
    for (int l = 0; l < nl; l++)
        for (int i = 0; i < 4; i++) {
            float z = xyz[l][i][2];
            vb[l * 4 + i][0] = xyz[l][i][0] + (float)offx;
            vb[l * 4 + i][1] = xyz[l][i][1] + (float)offy;
            vb[l * 4 + i][2] = (z > 1.0f ? (z + zback) / 65536.0f > 1.0f ? 1.0f : (z + zback) / 65536.0f : 0.0f) * 2.0f - 1.0f;
            vb[l * 4 + i][3] = uv[l][i][0];
            vb[l * 4 + i][4] = uv[l][i][1];
        }
    int S = hw_scale, aw = (x2 - x1 + 1) * S, ah = (y2 - y1 + 1) * S;
    int aa = models_aa > 1 && aa_target(aw, ah, models_aa);
    if (aa) {
        glBindFramebuffer(GL_FRAMEBUFFER, aa_fbo);
        glViewport(-x1 * S, -y1 * S, W, H);
        glEnable(GL_SCISSOR_TEST);
        glScissor(0, 0, aw, ah);
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);
        if (aa_depth_serial != pass) {
            aa_depth_serial = pass;
            glClearDepth(1.0); glClearStencil(0); glStencilMask(0xFF);
            glClear(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        }
    } else {
        glBindFramebuffer(GL_FRAMEBUFFER, draw_fbo);
        glViewport(0, 0, W, H);
        glEnable(GL_SCISSOR_TEST);
        glScissor(x1 * S, y1 * S, aw, ah);
        if (mesh_depth_serial != pass) {
            mesh_depth_serial = pass;
            glDisable(GL_SCISSOR_TEST);
            glClearDepth(1.0); glClearStencil(0); glStencilMask(0xFF);
            glClear(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
            glEnable(GL_SCISSOR_TEST);
        }
    }
    glUseProgram(prog_spr);
    glUniform1i(us_tex, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glBindVertexArray(spr_vao);
    glBindBuffer(GL_ARRAY_BUFFER, spr_vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(sizeof(float) * 5 * 4 * (size_t)nl), vb, GL_STREAM_DRAW);
    int heavy = nl > 1 && wt[1] > wt[0] ? 1 : 0;
    /* the colour: tested against what the pass drew before (no depth written yet); the layers add up in the cleared
     * target (their weights sum to 1); without anti-aliasing only the heavier layer, laid over VRAM directly */
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_FALSE);
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    if (aa) glBlendFunc(GL_ONE, GL_ONE);
    else glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
    glUniform1f(us_cut, 0.0f);
    for (int l = 0; l < nl; l++) {
        if (!aa && l != heavy) continue;
        if (wt[l] <= 0.0f) continue;
        glUniform4f(us_mul, mul[0], mul[1], mul[2], aa ? wt[l] : 1.0f);
        glDrawArrays(GL_TRIANGLE_FAN, l * 4, 4);
    }
    glDisable(GL_BLEND);
    /* the depth, where it covers */
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glDepthMask(GL_TRUE);
    glUniform1f(us_cut, 0.5f);
    glDrawArrays(GL_TRIANGLE_FAN, heavy * 4, 4);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);
    if (aa) { /* resolve, lay over VRAM premultiplied (as the meshes) */
        glBindFramebuffer(GL_READ_FRAMEBUFFER, aa_fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, aa_res_fbo);
        glBlitFramebuffer(0, 0, aw, ah, 0, 0, aw, ah, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER, draw_fbo);
        glViewport(x1 * S, y1 * S, aw, ah);
        glUseProgram(prog_comp);
        glUniform4f(uc_rect, 0, 0, (float)aw, (float)ah);
        glUniform2f(uc_size, (float)aa_w, (float)aa_h);
        glBindTexture(GL_TEXTURE_2D, aa_res_tex);
        glEnable(GL_BLEND);
        glBlendEquation(GL_FUNC_ADD);
        glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
        glBindVertexArray(vao_empty);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glDisable(GL_BLEND);
        glViewport(0, 0, W, H);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, draw_fbo);
    glBindVertexArray(vao);
}

/* ------------------------------------------------------------------ soft character shadows
 * In place of the PS1 blob (SHADOW.RMD, one quad under every character) the remodel is drawn flattened onto the
 * blob's floor plane along the light, into a half-resolution mask: each point weighs exp(-height / fade), so feet
 * on the floor are dark and raised arms faint. The mask is blurred (two passes) and darkens the picture over the
 * draw area: picture *= 1 - strength * mask. models.c draws it just before the character's mesh. */
static GLuint sh_tex[2], sh_fbo[2], prog_sh, prog_sh_blur, prog_sh_comp, prog_sh_pool;
static GLint us_bones, us_proj, us_off, us_q, us_n, us_l, us_fade, ub_src, ub_dir, uq_rect, uq_size, uq_k;
static int sh_w, sh_h, sh_state;

static const char *vs_shadow =
    "#version 330 core\n"
    "layout(location=0) in vec3 a_pos; layout(location=3) in uvec4 a_j; layout(location=4) in vec4 a_w;\n"
    "uniform vec4 u_bones[60]; uniform vec4 u_proj; uniform vec4 u_off; uniform vec3 u_q; uniform vec3 u_n;\n"
    "uniform vec3 u_l; uniform float u_fade; out float v_k;\n"
    "void main(){\n"
    "  vec3 p = vec3(0.0); float ws = 0.0;\n"
    "  for (int i = 0; i < 4; i++) {\n"
    "    float w = a_w[i]; if (w <= 0.0) continue;\n"
    "    int j = int(a_j[i]) * 3; vec4 r0 = u_bones[j], r1 = u_bones[j + 1], r2 = u_bones[j + 2];\n"
    "    p += w * vec3(dot(r0.xyz, a_pos) + r0.w, dot(r1.xyz, a_pos) + r1.w, dot(r2.xyz, a_pos) + r2.w);\n"
    "    ws += w;\n"
    "  }\n"
    "  if (ws > 0.0) p /= ws;\n"
    "  float d = dot(p - u_q, u_n);\n"
    "  p -= u_l * (d / dot(u_l, u_n));\n"
    "  v_k = exp(-max(d, 0.0) / u_fade);\n"
    "  float z = max(p.z, 1.0);\n"
    "  float vx = u_proj.y + u_proj.x * p.x / z + u_off.x, vy = u_proj.z + u_proj.x * p.y / z + u_off.y;\n"
    "  gl_Position = vec4((vx / 512.0 - 1.0) * z, (vy / 256.0 - 1.0) * z, 0.0, z);\n"
    "}\n";
static const char *fs_shadow =
    "#version 330 core\n"
    "in float v_k; out vec4 o;\n"
    "void main(){ o = vec4(v_k, 0.0, 0.0, 1.0); }\n";
/* the contact pool: the blob's quad (corners in grid order = a triangle strip), dark in the middle, soft to the rim */
static const char *vs_shadow_pool =
    "#version 330 core\n"
    "uniform vec3 u_c[4]; uniform vec4 u_proj; uniform vec4 u_off; out vec2 v_uv;\n"
    "void main(){ vec3 p = u_c[gl_VertexID]; v_uv = vec2(gl_VertexID & 1, gl_VertexID >> 1) * 2.0 - 1.0;\n"
    "  float z = max(p.z, 1.0);\n"
    "  float vx = u_proj.y + u_proj.x * p.x / z + u_off.x, vy = u_proj.z + u_proj.x * p.y / z + u_off.y;\n"
    "  gl_Position = vec4((vx / 512.0 - 1.0) * z, (vy / 256.0 - 1.0) * z, 0.0, z); }\n";
static const char *fs_shadow_pool =
    "#version 330 core\n"
    "uniform float u_k; in vec2 v_uv; out vec4 o;\n"
    "void main(){ float r = length(v_uv); o = vec4(u_k * (1.0 - smoothstep(0.4, 1.0, r)), 0.0, 0.0, 1.0); }\n";
static const char *fs_shadow_blur = /* 9 taps, Gaussian, step = u_dir (texels) */
    "#version 330 core\n"
    "uniform sampler2D u_src; uniform vec2 u_dir; in vec2 v_p; out vec4 o;\n"
    "void main(){ vec2 px = 1.0 / vec2(textureSize(u_src, 0)); vec2 uv = v_p * px;\n"
    "  float w[5] = float[](0.2270, 0.1946, 0.1216, 0.0541, 0.0162); float s = texture(u_src, uv).r * w[0];\n"
    "  for (int i = 1; i < 5; i++) s += (texture(u_src, uv + u_dir * px * float(i)).r + texture(u_src, uv - u_dir * px * float(i)).r) * w[i];\n"
    "  o = vec4(s, 0.0, 0.0, 1.0); }\n";
static const char *fs_shadow_comp =
    "#version 330 core\n"
    "uniform sampler2D u_t; uniform vec2 u_size; uniform float u_k; in vec2 v_p; out vec4 o;\n"
    "void main(){ o = vec4(0.0, 0.0, 0.0, min(u_k * texture(u_t, v_p / u_size).r, 0.9)); }\n";

static int shadow_init(int w, int h) {
    if (sh_state < 0) return 0;
    if (!sh_state) {
        sh_state = -1;
        prog_sh = link(vs_shadow, fs_shadow);
        prog_sh_blur = link(vs_quad, fs_shadow_blur);
        prog_sh_comp = link(vs_quad, fs_shadow_comp);
        prog_sh_pool = link(vs_shadow_pool, fs_shadow_pool);
        if (!prog_sh || !prog_sh_blur || !prog_sh_comp || !prog_sh_pool) { LOG("models: shadow shaders failed\n"); return 0; }
        us_bones = glGetUniformLocation(prog_sh, "u_bones"); us_proj = glGetUniformLocation(prog_sh, "u_proj");
        us_off = glGetUniformLocation(prog_sh, "u_off"); us_q = glGetUniformLocation(prog_sh, "u_q");
        us_n = glGetUniformLocation(prog_sh, "u_n"); us_l = glGetUniformLocation(prog_sh, "u_l");
        us_fade = glGetUniformLocation(prog_sh, "u_fade");
        ub_src = glGetUniformLocation(prog_sh_blur, "u_src"); ub_dir = glGetUniformLocation(prog_sh_blur, "u_dir");
        uq_rect = glGetUniformLocation(prog_sh_blur, "u_rect");
        uq_size = glGetUniformLocation(prog_sh_comp, "u_size"); uq_k = glGetUniformLocation(prog_sh_comp, "u_k");
        sh_state = 1;
    }
    if (w > sh_w || h > sh_h) { /* grown in place: the framebuffers keep their texture */
        sh_w = w > sh_w ? w : sh_w; sh_h = h > sh_h ? h : sh_h;
        for (int i = 0; i < 2; i++) {
            if (!sh_tex[i]) { sh_tex[i] = make_fb_tex(&sh_fbo[i], sh_w, sh_h); continue; }
            glBindTexture(GL_TEXTURE_2D, sh_tex[i]);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, sh_w, sh_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
        }
    }
    return 1;
}

void hw_shadow_draw(HwMesh *m, const float *bones, int nbones, float h, float ofx, float ofy, const float *plane,
                    const float *light, float fade, float blur, float strength, const float *quad, float contact) {
    if (!hw_active || !m || h < 1.0f) return;
    hw_flush();
    int x1, y1, x2, y2, offx, offy;
    if (mesh_area_set) { x1 = mesh_area[0]; y1 = mesh_area[1]; x2 = mesh_area[2]; y2 = mesh_area[3]; offx = mesh_area[4]; offy = mesh_area[5]; }
    else gpu_draw_area(&x1, &y1, &x2, &y2, &offx, &offy);
    if (x2 < x1 || y2 < y1) return;
    int S = hw_scale, aw = (x2 - x1 + 1) * S, ah = (y2 - y1 + 1) * S;
    int sw = (aw + 1) / 2, shh = (ah + 1) / 2; /* the mask at half the picture's resolution */
    if (!shadow_init(sw, shh)) return;
    /* 1: the flattened mesh into mask 0, the same VRAM mapping as the mesh (shifted to the area, halved) */
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0, 0, 0, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, sh_fbo[1]);
    glClear(GL_COLOR_BUFFER_BIT); /* the blur reads a little past the area */
    glBindFramebuffer(GL_FRAMEBUFFER, sh_fbo[0]);
    glViewport(-x1 * S / 2, -y1 * S / 2, W / 2, H / 2);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendEquation(GL_MAX);
    glBlendFunc(GL_ONE, GL_ONE);
    glUseProgram(prog_sh);
    float ub[MESH_MAXBONES * 12];
    memset(ub, 0, sizeof ub);
    if (nbones > MESH_MAXBONES) nbones = MESH_MAXBONES;
    memcpy(ub, bones, sizeof(float) * 12 * (size_t)nbones);
    glUniform4fv(us_bones, MESH_MAXBONES * 3, ub);
    glUniform4f(us_proj, h, ofx, ofy, 1.0f);
    glUniform4f(us_off, (float)offx, (float)offy, 0, 0);
    glUniform3f(us_q, plane[0], plane[1], plane[2]);
    glUniform3f(us_n, plane[3], plane[4], plane[5]);
    glUniform3f(us_l, light[0], light[1], light[2]);
    glUniform1f(us_fade, fade > 1.0f ? fade : 1.0f);
    glBindVertexArray(m->vao);
    for (int i = 0; i < m->nmat; i++)
        if (m->mats[i].count)
            glDrawElements(GL_TRIANGLES, (GLsizei)m->mats[i].count, GL_UNSIGNED_INT, (void *)(size_t)(m->mats[i].first * 4));
    if (quad && contact > 0.0f) {
        glUseProgram(prog_sh_pool);
        for (int k = 0; k < 4; k++) {
            char nm[8] = "u_c[0]";
            nm[4] = (char)('0' + k);
            glUniform3f(glGetUniformLocation(prog_sh_pool, nm), quad[k * 3], quad[k * 3 + 1], quad[k * 3 + 2]);
        }
        glUniform4f(glGetUniformLocation(prog_sh_pool, "u_proj"), h, ofx, ofy, 1.0f);
        glUniform4f(glGetUniformLocation(prog_sh_pool, "u_off"), (float)offx, (float)offy, 0, 0);
        glUniform1f(glGetUniformLocation(prog_sh_pool, "u_k"), contact);
        glBindVertexArray(vao_empty);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }
    glBlendEquation(GL_FUNC_ADD);
    glDisable(GL_BLEND);
    static int dbg_mask = -1;
    if (dbg_mask < 0) dbg_mask = getenv("FE_DBG_SHADOW") ? 20 : 0;
    if (dbg_mask > 0) { /* FE_DBG_SHADOW: how much of the mask the flattened mesh covers */
        dbg_mask--;
        unsigned char *px = malloc((size_t)sw * shh * 4);
        glReadPixels(0, 0, sw, shh, GL_RGBA, GL_UNSIGNED_BYTE, px);
        int cover = 0, mx = 0, bx0 = sw, by0 = shh, bx1 = -1, by1 = -1;
        for (int y = 0; y < shh; y++)
            for (int x = 0; x < sw; x++) {
                int v = px[(y * sw + x) * 4];
                if (v > mx) mx = v;
                if (v > 8) { cover++; if (x < bx0) bx0 = x; if (x > bx1) bx1 = x; if (y < by0) by0 = y; if (y > by1) by1 = y; }
            }
        free(px);
        LOG("shadow mask %dx%d (area %d,%d-%d,%d off %d,%d S %d): %d texels, max %d, box %d,%d-%d,%d; floor %.0f %.0f %.0f n %.2f %.2f %.2f L %.2f %.2f %.2f fade %.0f blur %.1f\n",
            sw, shh, x1, y1, x2, y2, offx, offy, S, cover, mx, bx0, by0, bx1, by1, plane[0], plane[1], plane[2], plane[3],
            plane[4], plane[5], light[0], light[1], light[2], fade, blur);
    }
    /* 2: blur 0 -> 1 across, 1 -> 0 down; the step spreads the 9 taps over the blur radius (mask texels) */
    /* the blur lies on the floor: seen at a grazing angle it is foreshortened, so the step down the screen shrinks by
     * the angle between the floor and the line of sight (else the thin shadow of a low camera blurs away) */
    float step = blur * S / 2.0f / 4.0f, ql = sqrtf(plane[0] * plane[0] + plane[1] * plane[1] + plane[2] * plane[2]);
    float fore = ql > 1e-3f ? fabsf(plane[0] * plane[3] + plane[1] * plane[4] + plane[2] * plane[5]) / ql : 1.0f;
    if (fore < 0.15f) fore = 0.15f;
    if (step < 0.5f) step = 0.5f;
    float step_y = step * fore < 0.35f ? 0.35f : step * fore;
    glUseProgram(prog_sh_blur);
    glUniform1i(ub_src, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindVertexArray(vao_empty);
    for (int pass = 0; pass < 2; pass++) {
        glBindFramebuffer(GL_FRAMEBUFFER, sh_fbo[1 - pass]);
        glViewport(0, 0, sw, shh);
        glBindTexture(GL_TEXTURE_2D, sh_tex[pass]);
        glUniform4f(uq_rect, 0, 0, (float)sw, (float)shh);
        glUniform2f(ub_dir, pass ? 0.0f : step, pass ? step_y : 0.0f);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }
    if (dbg_mask > 0) {
        unsigned char *px = malloc((size_t)sw * shh * 4);
        glReadPixels(0, 0, sw, shh, GL_RGBA, GL_UNSIGNED_BYTE, px);
        int mx = 0;
        for (int i = 0; i < sw * shh; i++) if (px[i * 4] > mx) mx = px[i * 4];
        free(px);
        LOG("shadow mask after blur: max %d (step %.2f x %.2f)\n", mx, step, step_y);
    }
    /* 3: darken the picture over the draw area (VRAM alpha, the mask bit, kept) */
    glBindFramebuffer(GL_FRAMEBUFFER, draw_fbo);
    glViewport(x1 * S, y1 * S, aw, ah);
    glUseProgram(prog_sh_comp);
    glUniform4f(glGetUniformLocation(prog_sh_comp, "u_rect"), 0, 0, (float)sw, (float)shh);
    glUniform2f(uq_size, (float)sh_w, (float)sh_h);
    glUniform1f(uq_k, strength);
    glBindTexture(GL_TEXTURE_2D, sh_tex[0]);
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_ZERO, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisable(GL_BLEND);
    glViewport(0, 0, W, H);
    glBindVertexArray(vao);
}

/* ------------------------------------------------------------------ contact shadows (shadow map on the room's geometry)
 * 1. receivers: the room's collision quads near the character and nearby prop remodels, drawn through the game's
 *    projection into a half-resolution buffer of view-space positions (nearest wins);
 * 2. the character drawn from the light (orthographic along it) into a depth map;
 * 3. per receiver texel: the share of 16 taps around it in the light map where the character lies between it and the
 *    light, each weighted exp(-gap / fade) (limbs far above the surface cast fainter), into the mask;
 * then the same blur and darkening as the flat shadow. Texels with no receiver stay unshadowed: a shadow stops at a
 * wall's edge, runs up it, lies on stairs and slopes as the collision has them, and on a gate or tank remodel. */
static GLuint rc_tex, rc_fbo, rc_depth, lm_tex, lm_fbo, lm_depth, rc_vao, rc_vbo, prog_rc_quad, prog_rc_mesh, prog_lm, prog_rc_mask;
static int rc_w, rc_h, rc_state;
#define LM_SIZE 512

static const char *vs_rc_quad =
    "#version 330 core\n"
    "layout(location=0) in vec3 a_p; uniform vec4 u_proj; uniform vec4 u_off; out vec3 v_p;\n"
    "void main(){ vec3 p = a_p; v_p = p; float z = max(p.z, 1.0);\n"
    "  float vx = u_proj.y + u_proj.x * p.x / z + u_off.x, vy = u_proj.z + u_proj.x * p.y / z + u_off.y;\n"
    "  float d = clamp(z / 65536.0, 0.0, 1.0) * 2.0 - 1.0;\n"
    "  gl_Position = vec4((vx / 512.0 - 1.0) * z, (vy / 256.0 - 1.0) * z, d * z, z); }\n";
#define SKIN_GLSL \
    "layout(location=0) in vec3 a_pos; layout(location=3) in uvec4 a_j; layout(location=4) in vec4 a_w;\n" \
    "uniform vec4 u_bones[60];\n" \
    "vec3 skin(){ vec3 p = vec3(0.0); float ws = 0.0;\n" \
    "  for (int i = 0; i < 4; i++) { float w = a_w[i]; if (w <= 0.0) continue;\n" \
    "    int j = int(a_j[i]) * 3; vec4 r0 = u_bones[j], r1 = u_bones[j + 1], r2 = u_bones[j + 2];\n" \
    "    p += w * vec3(dot(r0.xyz, a_pos) + r0.w, dot(r1.xyz, a_pos) + r1.w, dot(r2.xyz, a_pos) + r2.w); ws += w; }\n" \
    "  return ws > 0.0 ? p / ws : p; }\n"
static const char *vs_rc_mesh =
    "#version 330 core\n" SKIN_GLSL
    "uniform vec4 u_proj; uniform vec4 u_off; out vec3 v_p;\n"
    "void main(){ vec3 p = skin(); v_p = p; float z = max(p.z, 1.0);\n"
    "  float vx = u_proj.y + u_proj.x * p.x / z + u_off.x, vy = u_proj.z + u_proj.x * p.y / z + u_off.y;\n"
    "  float d = clamp(z / 65536.0, 0.0, 1.0) * 2.0 - 1.0;\n"
    "  gl_Position = vec4((vx / 512.0 - 1.0) * z, (vy / 256.0 - 1.0) * z, d * z, z); }\n";
static const char *fs_rc =
    "#version 330 core\n"
    "in vec3 v_p; out vec4 o;\n"
    "void main(){ o = vec4(v_p, 1.0); }\n";
static const char *vs_lm =
    "#version 330 core\n" SKIN_GLSL
    "uniform vec3 u_c; uniform vec3 u_u; uniform vec3 u_v; uniform vec3 u_l; uniform vec2 u_ext; out float v_d;\n"
    "void main(){ vec3 q = skin() - u_c; v_d = dot(q, u_l);\n"
    "  gl_Position = vec4(dot(q, u_u) / u_ext.x, dot(q, u_v) / u_ext.x, clamp(v_d / u_ext.y, -1.0, 1.0), 1.0); }\n";
static const char *fs_lm =
    "#version 330 core\n"
    "in float v_d; out vec4 o;\n"
    "void main(){ o = vec4(v_d, 0.0, 0.0, 1.0); }\n";
static const char *fs_rc_mask =
    "#version 330 core\n"
    "uniform sampler2D u_rc; uniform sampler2D u_lm; uniform vec3 u_c; uniform vec3 u_u; uniform vec3 u_v; uniform vec3 u_l;\n"
    "uniform vec2 u_ext; uniform float u_fade; uniform float u_soft; uniform float u_bias; in vec2 v_p; out vec4 o;\n"
    "const vec2 tap[16] = vec2[](vec2(-0.94,-0.40), vec2(0.95,-0.77), vec2(-0.09,-0.93), vec2(0.34,0.29), vec2(-0.92,0.46),\n"
    "  vec2(-0.82,0.15), vec2(0.97,0.76), vec2(0.44,-0.97), vec2(0.54,-0.47), vec2(-0.26,-0.42), vec2(-0.27,0.97),\n"
    "  vec2(0.79,0.19), vec2(-0.12,0.65), vec2(0.15,0.05), vec2(-0.55,-0.79), vec2(0.06,-0.16));\n"
    "void main(){ vec4 r = texelFetch(u_rc, ivec2(v_p), 0);\n"
    "  if (r.w < 0.5) { o = vec4(0.0); return; }\n"
    "  vec3 q = r.xyz - u_c; float dr = dot(q, u_l);\n"
    "  vec2 uv = vec2(dot(q, u_u), dot(q, u_v)) / u_ext.x * 0.5 + 0.5;\n"
    "  if (uv.x < 0.0 || uv.y < 0.0 || uv.x > 1.0 || uv.y > 1.0) { o = vec4(0.0); return; }\n"
    "  float s = 0.0;\n"
    "  for (int i = 0; i < 16; i++) { float d = texture(u_lm, uv + tap[i] * u_soft).r;\n"
    "    if (d < dr - u_bias) s += exp(-(dr - d) / u_fade); }\n"
    "  o = vec4(s / 16.0, 0.0, 0.0, 1.0); }\n";

static GLuint float_target(GLuint *tex, GLuint *fbo, GLuint *depth, GLenum fmt, GLenum layout, int w, int h) {
    if (!*tex) { glGenTextures(1, tex); glGenFramebuffers(1, fbo); glGenRenderbuffers(1, depth); }
    glBindTexture(GL_TEXTURE_2D, *tex);
    glTexImage2D(GL_TEXTURE_2D, 0, fmt, w, h, 0, layout, GL_FLOAT, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, fmt == GL_R32F ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, fmt == GL_R32F ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindRenderbuffer(GL_RENDERBUFFER, *depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h);
    glBindFramebuffer(GL_FRAMEBUFFER, *fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, *tex, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, *depth);
    return glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
}

static int recv_init(int w, int h) {
    if (rc_state < 0) return 0;
    if (!rc_state) {
        rc_state = -1;
        prog_rc_quad = link(vs_rc_quad, fs_rc);
        prog_rc_mesh = link(vs_rc_mesh, fs_rc);
        prog_lm = link(vs_lm, fs_lm);
        prog_rc_mask = link(vs_quad, fs_rc_mask);
        if (!prog_rc_quad || !prog_rc_mesh || !prog_lm || !prog_rc_mask) { LOG("models: contact shadow shaders failed\n"); return 0; }
        if (!float_target(&lm_tex, &lm_fbo, &lm_depth, GL_R32F, GL_RED, LM_SIZE, LM_SIZE)) {
            LOG("models: contact shadow light map failed\n"); return 0;
        }
        glGenVertexArrays(1, &rc_vao);
        glGenBuffers(1, &rc_vbo);
        glBindVertexArray(rc_vao);
        glBindBuffer(GL_ARRAY_BUFFER, rc_vbo);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 12, (void *)0);
        glBindVertexArray(vao);
        rc_state = 1;
    }
    if (w > rc_w || h > rc_h) {
        rc_w = w > rc_w ? w : rc_w; rc_h = h > rc_h ? h : rc_h;
        if (!float_target(&rc_tex, &rc_fbo, &rc_depth, GL_RGBA32F, GL_RGBA, rc_w, rc_h)) {
            LOG("models: contact shadow receiver buffer failed\n"); rc_state = -1; return 0;
        }
    }
    return 1;
}

static void mesh_uniform_bones(GLuint prog, const float *bones, int nbones) {
    float ub[MESH_MAXBONES * 12];
    memset(ub, 0, sizeof ub);
    if (nbones > MESH_MAXBONES) nbones = MESH_MAXBONES;
    memcpy(ub, bones, sizeof(float) * 12 * (size_t)nbones);
    glUniform4fv(glGetUniformLocation(prog, "u_bones"), MESH_MAXBONES * 3, ub);
}

static void mesh_draw_all(HwMesh *m) {
    glBindVertexArray(m->vao);
    for (int i = 0; i < m->nmat; i++)
        if (m->mats[i].count)
            glDrawElements(GL_TRIANGLES, (GLsizei)m->mats[i].count, GL_UNSIGNED_INT, (void *)(size_t)(m->mats[i].first * 4));
}

void hw_shadow_recv(HwMesh *caster, const float *bones, int nbones, float h, float ofx, float ofy, const ShadowRecv *r) {
    if (!hw_active || !caster || h < 1.0f || (!r->nq && !r->np)) return;
    hw_flush();
    int x1, y1, x2, y2, offx, offy;
    if (mesh_area_set) { x1 = mesh_area[0]; y1 = mesh_area[1]; x2 = mesh_area[2]; y2 = mesh_area[3]; offx = mesh_area[4]; offy = mesh_area[5]; }
    else gpu_draw_area(&x1, &y1, &x2, &y2, &offx, &offy);
    if (x2 < x1 || y2 < y1) return;
    int S = hw_scale, aw = (x2 - x1 + 1) * S, ah = (y2 - y1 + 1) * S;
    int sw = (aw + 1) / 2, shh = (ah + 1) / 2;
    if (!shadow_init(sw, shh) || !recv_init(sw, shh)) return;
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    /* 1: receivers */
    glBindFramebuffer(GL_FRAMEBUFFER, rc_fbo);
    glViewport(-x1 * S / 2, -y1 * S / 2, W / 2, H / 2);
    glClearColor(0, 0, 0, 0);
    glClearDepth(1.0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_TRUE);
    if (r->nq) {
        float tri[SH_MAXQ * 6 * 3];
        static const int ord[6] = {0, 1, 2, 0, 2, 3};
        for (int i = 0; i < r->nq; i++)
            for (int k = 0; k < 6; k++) memcpy(&tri[(i * 6 + k) * 3], r->q[i][ord[k]], 12);
        glUseProgram(prog_rc_quad);
        glUniform4f(glGetUniformLocation(prog_rc_quad, "u_proj"), h, ofx, ofy, 1.0f);
        glUniform4f(glGetUniformLocation(prog_rc_quad, "u_off"), (float)offx, (float)offy, 0, 0);
        glBindVertexArray(rc_vao);
        glBindBuffer(GL_ARRAY_BUFFER, rc_vbo);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(sizeof(float) * 18 * (size_t)r->nq), tri, GL_STREAM_DRAW);
        glDrawArrays(GL_TRIANGLES, 0, 6 * r->nq);
    }
    if (r->np) {
        glUseProgram(prog_rc_mesh);
        glUniform4f(glGetUniformLocation(prog_rc_mesh, "u_proj"), h, ofx, ofy, 1.0f);
        glUniform4f(glGetUniformLocation(prog_rc_mesh, "u_off"), (float)offx, (float)offy, 0, 0);
        for (int i = 0; i < r->np; i++) {
            mesh_uniform_bones(prog_rc_mesh, r->pb[i], r->pn[i]);
            mesh_draw_all(r->pm[i]);
        }
    }
    /* 2: the character from the light */
    glBindFramebuffer(GL_FRAMEBUFFER, lm_fbo);
    glViewport(0, 0, LM_SIZE, LM_SIZE);
    glClearColor(1e9f, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glUseProgram(prog_lm);
    mesh_uniform_bones(prog_lm, bones, nbones);
    glUniform3f(glGetUniformLocation(prog_lm, "u_c"), r->c[0], r->c[1], r->c[2]);
    glUniform3f(glGetUniformLocation(prog_lm, "u_u"), r->u[0], r->u[1], r->u[2]);
    glUniform3f(glGetUniformLocation(prog_lm, "u_v"), r->v[0], r->v[1], r->v[2]);
    glUniform3f(glGetUniformLocation(prog_lm, "u_l"), r->light[0], r->light[1], r->light[2]);
    glUniform2f(glGetUniformLocation(prog_lm, "u_ext"), r->ext, r->dext);
    mesh_draw_all(caster);
    glDisable(GL_DEPTH_TEST);
    glClearColor(0, 0, 0, 0);
    /* 3: the mask */
    glBindFramebuffer(GL_FRAMEBUFFER, sh_fbo[1]);
    glViewport(0, 0, sh_w, sh_h);
    glClear(GL_COLOR_BUFFER_BIT);
    glBindFramebuffer(GL_FRAMEBUFFER, sh_fbo[0]);
    glClear(GL_COLOR_BUFFER_BIT);
    glViewport(0, 0, sw, shh);
    glUseProgram(prog_rc_mask);
    glUniform4f(glGetUniformLocation(prog_rc_mask, "u_rect"), 0, 0, (float)sw, (float)shh);
    glUniform1i(glGetUniformLocation(prog_rc_mask, "u_rc"), 0);
    glUniform1i(glGetUniformLocation(prog_rc_mask, "u_lm"), 1);
    glUniform3f(glGetUniformLocation(prog_rc_mask, "u_c"), r->c[0], r->c[1], r->c[2]);
    glUniform3f(glGetUniformLocation(prog_rc_mask, "u_u"), r->u[0], r->u[1], r->u[2]);
    glUniform3f(glGetUniformLocation(prog_rc_mask, "u_v"), r->v[0], r->v[1], r->v[2]);
    glUniform3f(glGetUniformLocation(prog_rc_mask, "u_l"), r->light[0], r->light[1], r->light[2]);
    glUniform2f(glGetUniformLocation(prog_rc_mask, "u_ext"), r->ext, r->dext);
    glUniform1f(glGetUniformLocation(prog_rc_mask, "u_fade"), r->fade > 1.0f ? r->fade : 1.0f);
    glUniform1f(glGetUniformLocation(prog_rc_mask, "u_soft"), r->soft / (2.0f * r->ext)); /* world units -> map uv */
    glUniform1f(glGetUniformLocation(prog_rc_mask, "u_bias"), r->bias);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, lm_tex);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, rc_tex);
    glBindVertexArray(vao_empty);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    static int dbg_rc = -1;
    if (dbg_rc < 0) dbg_rc = getenv("FE_DBG_SHADOW") ? 12 : 0;
    if (dbg_rc > 0) { /* FE_DBG_SHADOW: what each step left */
        dbg_rc--;
        float *px = malloc(sizeof(float) * 4 * (size_t)(sw * shh > LM_SIZE * LM_SIZE ? sw * shh : LM_SIZE * LM_SIZE));
        float *rcb = malloc(sizeof(float) * 4 * (size_t)(sw * shh)), *lmb = malloc(sizeof(float) * LM_SIZE * LM_SIZE);
        glBindFramebuffer(GL_FRAMEBUFFER, rc_fbo);
        glReadPixels(0, 0, sw, shh, GL_RGBA, GL_FLOAT, rcb);
        glBindFramebuffer(GL_FRAMEBUFFER, lm_fbo);
        glReadPixels(0, 0, LM_SIZE, LM_SIZE, GL_RED, GL_FLOAT, lmb);
        int inmap = 0, occ = 0, over = 0;
        float drmin = 1e9f, drmax = -1e9f;
        for (int i = 0; i < sw * shh; i++) { /* the mask's test, on the CPU */
            if (rcb[i * 4 + 3] < 0.5f) continue;
            float q[3] = {rcb[i * 4] - r->c[0], rcb[i * 4 + 1] - r->c[1], rcb[i * 4 + 2] - r->c[2]};
            float dr = q[0] * r->light[0] + q[1] * r->light[1] + q[2] * r->light[2];
            float ux = (q[0] * r->u[0] + q[1] * r->u[1] + q[2] * r->u[2]) / r->ext * 0.5f + 0.5f;
            float uy = (q[0] * r->v[0] + q[1] * r->v[1] + q[2] * r->v[2]) / r->ext * 0.5f + 0.5f;
            if (ux < 0 || uy < 0 || ux >= 1 || uy >= 1) continue;
            inmap++;
            float d = lmb[(int)(uy * LM_SIZE) * LM_SIZE + (int)(ux * LM_SIZE)];
            if (d < 1e8f) { over++; if (dr < drmin) drmin = dr; if (dr > drmax) drmax = dr; if (d < dr - r->bias) occ++; }
        }
        LOG("contact shadow cpu: %d receiver texels in the map, %d under the character (dr %.0f..%.0f), %d occluded\n", inmap,
            over, drmin, drmax, occ);
        { /* the texels at the root's screen position and below it */
            float sx = ofx + h * r->c[0] / r->c[2] + offx - x1, sy = ofy + h * r->c[1] / r->c[2] + offy - y1;
            for (int dy = 0; dy <= 120; dy += 20) {
                int tx = (int)(sx * S / 2), ty = (int)((sy + dy) * S / 2);
                if (tx < 0 || ty < 0 || tx >= sw || ty >= shh) continue;
                float *t = rcb + 4 * (ty * sw + tx);
                float q[3] = {t[0] - r->c[0], t[1] - r->c[1], t[2] - r->c[2]};
                LOG("contact shadow cpu: texel %d,%d w %.0f = %.0f %.0f %.0f; q.u %.0f q.v %.0f q.l %.0f (ext %.0f)\n", tx, ty, t[3], t[0],
                    t[1], t[2], q[0] * r->u[0] + q[1] * r->u[1] + q[2] * r->u[2], q[0] * r->v[0] + q[1] * r->v[1] + q[2] * r->v[2],
                    q[0] * r->light[0] + q[1] * r->light[1] + q[2] * r->light[2], r->ext);
            }
        }
        free(rcb); free(lmb);
        int nrc = 0, nmask = 0, nlm = 0;
        float mx = 0, lmin = 1e9f, zmin = 1e9f, zmax = -1e9f;
        glBindFramebuffer(GL_FRAMEBUFFER, rc_fbo);
        glReadPixels(0, 0, sw, shh, GL_RGBA, GL_FLOAT, px);
        for (int i = 0; i < sw * shh; i++)
            if (px[i * 4 + 3] > 0.5f) { nrc++; if (px[i * 4 + 2] < zmin) zmin = px[i * 4 + 2]; if (px[i * 4 + 2] > zmax) zmax = px[i * 4 + 2]; }
        glBindFramebuffer(GL_FRAMEBUFFER, lm_fbo);
        glReadPixels(0, 0, LM_SIZE, LM_SIZE, GL_RED, GL_FLOAT, px);
        for (int i = 0; i < LM_SIZE * LM_SIZE; i++) if (px[i] < 1e8f) { nlm++; if (px[i] < lmin) lmin = px[i]; }
        glBindFramebuffer(GL_FRAMEBUFFER, sh_fbo[0]);
        glReadPixels(0, 0, sw, shh, GL_RGBA, GL_FLOAT, px);
        for (int i = 0; i < sw * shh; i++) if (px[i * 4] > 0.01f) { nmask++; if (px[i * 4] > mx) mx = px[i * 4]; }
        free(px);
        LOG("contact shadow: %dx%d, receivers %d texels (z %.0f..%.0f), light map %d texels (nearest %.0f), mask %d texels max %.2f\n",
            sw, shh, nrc, zmin, zmax, nlm, lmin, nmask, mx);
        glBindFramebuffer(GL_FRAMEBUFFER, sh_fbo[0]);
    }
    /* 4: a light blur (the taps already soften it), then darken the picture */
    glUseProgram(prog_sh_blur);
    glUniform1i(ub_src, 0);
    for (int pass = 0; pass < 2; pass++) {
        glBindFramebuffer(GL_FRAMEBUFFER, sh_fbo[1 - pass]);
        glViewport(0, 0, sw, shh);
        glBindTexture(GL_TEXTURE_2D, sh_tex[pass]);
        glUniform4f(uq_rect, 0, 0, (float)sw, (float)shh);
        glUniform2f(ub_dir, pass ? 0.0f : 0.6f, pass ? 0.6f : 0.0f);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, draw_fbo);
    glViewport(x1 * S, y1 * S, aw, ah);
    glUseProgram(prog_sh_comp);
    glUniform4f(glGetUniformLocation(prog_sh_comp, "u_rect"), 0, 0, (float)sw, (float)shh);
    glUniform2f(uq_size, (float)sh_w, (float)sh_h);
    glUniform1f(uq_k, r->strength);
    glBindTexture(GL_TEXTURE_2D, sh_tex[0]);
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_ZERO, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisable(GL_BLEND);
    glViewport(0, 0, W, H);
    glBindVertexArray(vao);
}

/* a texture pack image (RGBA, top row first) as a GL texture; linear filtering for full-colour images */
unsigned hw_tex_rgba(int w, int h, const unsigned char *rgba, int linear) {
    if (!hw_active) return 0;
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    /* linear 2: mipmapped too (images of 16 pixels per texel or more: minified at the lower internal scales) */
    if (linear == 2) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 4);
        glGenerateMipmap(GL_TEXTURE_2D);
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, linear == 2 ? GL_LINEAR_MIPMAP_LINEAR : linear ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return t;
}

/* a scene matte (models.c MATTE lines): one view depth per picture pixel, R16F, sampled nearest */
unsigned hw_matte_tex(const float *z, int w, int h) {
    if (!hw_active || !z) return 0;
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R16F, w, h, 0, GL_RED, GL_FLOAT, z);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if (glGetError() != GL_NO_ERROR) { glDeleteTextures(1, &t); return 0; }
    return t;
}

void hw_tex_free(unsigned tex) {
    GLuint t = tex;
    if (hw_active && t) glDeleteTextures(1, &t);
}

/* FE_GPU_TIME (headless): the window picture composed offscreen every vblank (the in-betweens are built there) */
static void hw_bench_present(void) {
    static GLuint tex, fbo;
    const int w = 1920, h = 1080;
    GpuDisplay d;
    gpu_display(&d);
    if (d.depth24) return;
    if (!fbo) {
        glGenTextures(1, &tex); glGenFramebuffers(1, &fbo);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    }
    screen_fbo = fbo;
    hw_present(&d, (w - h * 4 / 3) / 2, 0, h * 4 / 3, h, w, h);
    screen_fbo = 0;
}

/* each vblank (main.c): finish the GPU, read this vblank's timestamps; every 240 vblanks the means and worsts */
void hw_bench_frame(void) {
    if (!gpt()) return;
    extern int opt_headless;
    if (opt_headless) hw_bench_present();
    hw_flush();
    if (gpt_open >= 0) hw_gpt_mark(gpt_slot[gpt_open], 0);
    glFinish();
    double s[2] = {0, 0};
    for (int i = 0; i < gpt_n; i++) {
        GLuint64 a = 0, b = 0;
        q_get(gpt_q[i * 2], GL_QUERY_RESULT, &a);
        q_get(gpt_q[i * 2 + 1], GL_QUERY_RESULT, &b);
        s[gpt_slot[i] & 1] += (double)(b - a) / 1e6;
    }
    uint64_t now = SDL_GetPerformanceCounter();
    double wall = gpt_t0 ? (double)(now - gpt_t0) * 1000.0 / (double)SDL_GetPerformanceFrequency() : 0.0;
    gpt_t0 = now;
    if (!gpt_from) gpt_from = frame_count;
    for (int k = 0; k < 2; k++) {
        gpt_sum[k] += s[k];
        if (s[k] > gpt_max[k]) gpt_max[k] = s[k];
        for (int i = 0; i < gpt_n; i++) gpt_cnt[k] += (gpt_slot[i] & 1) == k;
    }
    gpt_wall_sum += wall;
    if (wall > gpt_wall_max) gpt_wall_max = wall;
    gpt_mesh_sum += gpt_meshes + gpt_meshes_mid;
    gpt_n = 0; gpt_open = -1;
    gpt_meshes = gpt_meshes_mid = 0;
    if (++gpt_frames >= 240) {
        double f = (double)gpt_frames;
        LOG("gputime: vblanks %llu-%llu: scene actors %.3f ms a vblank (worst %.3f, %d passes), in-betweens %.3f ms a vblank "
            "(worst %.3f, %d built), meshes %.1f a vblank, wall %.3f ms a vblank (worst %.3f; with glFinish)\n",
            (unsigned long long)gpt_from, (unsigned long long)frame_count, gpt_sum[0] / f, gpt_max[0], gpt_cnt[0],
            gpt_sum[1] / f, gpt_max[1], gpt_cnt[1], gpt_mesh_sum / f, gpt_wall_sum / f, gpt_wall_max);
        gpt_frames = 0; gpt_from = 0;
        gpt_sum[0] = gpt_sum[1] = gpt_max[0] = gpt_max[1] = gpt_wall_sum = gpt_wall_max = gpt_mesh_sum = 0;
        gpt_cnt[0] = gpt_cnt[1] = 0;
    }
}

/* the window's back buffer as top-down RGB (FE_CAPTURE) */
uint32_t *hw_read_back(int w, int h) {
    uint8_t *px = malloc((size_t)w * h * 4);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glReadBuffer(GL_BACK);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    uint32_t *out = malloc((size_t)w * h * 4);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const uint8_t *p = &px[((size_t)(h - 1 - y) * w + x) * 4];
            out[(size_t)y * w + x] = ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
        }
    free(px);
    return out;
}
