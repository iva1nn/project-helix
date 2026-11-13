/* The OpenGL entry points ui.c draws with, filled in by gpu_hw.c's loader (one GL context, one loader). */
#ifndef UI_GL_H
#define UI_GL_H
#include <SDL2/SDL_opengl.h>

typedef struct {
    GLuint (APIENTRY *CreateShader)(GLenum);
    void (APIENTRY *ShaderSource)(GLuint, GLsizei, const GLchar *const *, const GLint *);
    void (APIENTRY *CompileShader)(GLuint);
    GLuint (APIENTRY *CreateProgram)(void);
    void (APIENTRY *AttachShader)(GLuint, GLuint);
    void (APIENTRY *LinkProgram)(GLuint);
    void (APIENTRY *UseProgram)(GLuint);
    GLint (APIENTRY *GetUniformLocation)(GLuint, const GLchar *);
    void (APIENTRY *Uniform1i)(GLint, GLint);
    void (APIENTRY *Uniform2f)(GLint, GLfloat, GLfloat);
    void (APIENTRY *GenVertexArrays)(GLsizei, GLuint *);
    void (APIENTRY *BindVertexArray)(GLuint);
    void (APIENTRY *GenBuffers)(GLsizei, GLuint *);
    void (APIENTRY *BindBuffer)(GLenum, GLuint);
    void (APIENTRY *BufferData)(GLenum, GLsizeiptr, const void *, GLenum);
    void (APIENTRY *VertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void *);
    void (APIENTRY *EnableVertexAttribArray)(GLuint);
    void (APIENTRY *ActiveTexture)(GLenum);
    void (APIENTRY *BlendFuncSeparate)(GLenum, GLenum, GLenum, GLenum);
    void (APIENTRY *BlendEquation)(GLenum);
    void (APIENTRY *BindFramebuffer)(GLenum, GLuint);
} UiGl;
extern UiGl ui_gl;
extern unsigned ui_target_fbo; /* the window (0) or an offscreen capture */
#endif
