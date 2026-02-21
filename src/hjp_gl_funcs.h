/***********************************************************************
 * hjp_gl_funcs.h — OpenGL 3.2 Core 関数ポインタ宣言/ローダー (Windows用)
 *
 * Windows では opengl32.dll が GL 1.1 までしかエクスポートしないため、
 * wglGetProcAddress で動的ロードする必要がある。
 *
 * 使い方: hjp_gl_load_functions() を GL コンテキスト作成後に1回呼ぶ
 *
 * Copyright (c) 2026 Reo Shiozawa — MIT License
 ***********************************************************************/
#ifndef HJP_GL_FUNCS_H
#define HJP_GL_FUNCS_H

#ifdef HJP_GL_LOADER

#include <stddef.h>

/* --- GL 型定義 (Windows の gl.h に不足しているもの) --- */
#ifndef GL_VERSION_2_0
typedef char      GLchar;
#endif
#ifndef GL_VERSION_1_5
typedef ptrdiff_t GLsizeiptr;
typedef ptrdiff_t GLintptr;
#endif

/* --- GL 定数 (Windows の gl.h に不足しているもの) --- */
#ifndef GL_FRAGMENT_SHADER
#define GL_FRAGMENT_SHADER                0x8B30
#define GL_VERTEX_SHADER                  0x8B31
#define GL_COMPILE_STATUS                 0x8B81
#define GL_LINK_STATUS                    0x8B82
#define GL_INFO_LOG_LENGTH                0x8B84
#define GL_ARRAY_BUFFER                   0x8892
#define GL_STATIC_DRAW                    0x88E4
#define GL_STREAM_DRAW                    0x88E0
#define GL_DYNAMIC_DRAW                   0x88E2
#define GL_UNIFORM_BUFFER                 0x8A11
#define GL_ELEMENT_ARRAY_BUFFER           0x8893
#define GL_CLAMP_TO_EDGE                  0x812F
#define GL_GENERATE_MIPMAP                0x8191
#define GL_INCR_WRAP                      0x8507
#define GL_DECR_WRAP                      0x8508
#define GL_FUNC_ADD                       0x8006
#define GL_SRC_ALPHA                      0x0302
#define GL_ONE_MINUS_SRC_ALPHA            0x0303
#define GL_DST_ALPHA                      0x0304
#define GL_ONE_MINUS_DST_ALPHA            0x0305
#define GL_DST_COLOR                      0x0306
#define GL_ONE_MINUS_DST_COLOR            0x0307
#define GL_SRC_ALPHA_SATURATE             0x0308
#endif

#ifndef GL_RED
#define GL_RED                            0x1903
#endif

#ifndef GL_R8
#define GL_R8                             0x8229
#endif

/* --- 関数ポインタ型 & グローバル変数 --- */
#define HJP_GL_FUNC(ret, name, args) \
    typedef ret (APIENTRY *PFN_##name) args; \
    extern PFN_##name name;

/* Shader */
HJP_GL_FUNC(GLuint, glCreateShader,    (GLenum type))
HJP_GL_FUNC(void,   glDeleteShader,    (GLuint shader))
HJP_GL_FUNC(void,   glShaderSource,    (GLuint shader, GLsizei count, const GLchar *const*string, const GLint *length))
HJP_GL_FUNC(void,   glCompileShader,   (GLuint shader))
HJP_GL_FUNC(void,   glGetShaderiv,     (GLuint shader, GLenum pname, GLint *params))
HJP_GL_FUNC(void,   glGetShaderInfoLog,(GLuint shader, GLsizei bufSize, GLsizei *length, GLchar *infoLog))

/* Program */
HJP_GL_FUNC(GLuint, glCreateProgram,   (void))
HJP_GL_FUNC(void,   glDeleteProgram,   (GLuint program))
HJP_GL_FUNC(void,   glAttachShader,    (GLuint program, GLuint shader))
HJP_GL_FUNC(void,   glLinkProgram,     (GLuint program))
HJP_GL_FUNC(void,   glGetProgramiv,    (GLuint program, GLenum pname, GLint *params))
HJP_GL_FUNC(void,   glGetProgramInfoLog,(GLuint program, GLsizei bufSize, GLsizei *length, GLchar *infoLog))
HJP_GL_FUNC(void,   glUseProgram,      (GLuint program))
HJP_GL_FUNC(GLint,  glGetUniformLocation,(GLuint program, const GLchar *name))
HJP_GL_FUNC(GLuint, glGetUniformBlockIndex,(GLuint program, const GLchar *uniformBlockName))
HJP_GL_FUNC(void,   glUniformBlockBinding,(GLuint program, GLuint uniformBlockIndex, GLuint uniformBlockBinding))
HJP_GL_FUNC(void,   glBindAttribLocation,(GLuint program, GLuint index, const GLchar *name))

/* Uniform */
HJP_GL_FUNC(void,   glUniform1i,       (GLint location, GLint v0))
HJP_GL_FUNC(void,   glUniform2f,       (GLint location, GLfloat v0, GLfloat v1))

/* Buffer */
HJP_GL_FUNC(void,   glGenBuffers,      (GLsizei n, GLuint *buffers))
HJP_GL_FUNC(void,   glDeleteBuffers,   (GLsizei n, const GLuint *buffers))
HJP_GL_FUNC(void,   glBindBuffer,      (GLenum target, GLuint buffer))
HJP_GL_FUNC(void,   glBufferData,      (GLenum target, GLsizeiptr size, const void *data, GLenum usage))
HJP_GL_FUNC(void,   glBindBufferRange, (GLenum target, GLuint index, GLuint buffer, GLintptr offset, GLsizeiptr size))

/* VAO */
HJP_GL_FUNC(void,   glGenVertexArrays, (GLsizei n, GLuint *arrays))
HJP_GL_FUNC(void,   glDeleteVertexArrays,(GLsizei n, const GLuint *arrays))
HJP_GL_FUNC(void,   glBindVertexArray, (GLuint arr))
HJP_GL_FUNC(void,   glEnableVertexAttribArray,(GLuint index))
HJP_GL_FUNC(void,   glVertexAttribPointer,(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void *pointer))

/* Texture */
HJP_GL_FUNC(void,   glGenerateMipmap,  (GLenum target))
HJP_GL_FUNC(void,   glBlendFuncSeparate,(GLenum sfactorRGB, GLenum dfactorRGB, GLenum sfactorAlpha, GLenum dfactorAlpha))
HJP_GL_FUNC(void,   glStencilOpSeparate,(GLenum face, GLenum sfail, GLenum dpfail, GLenum dppass))

/* =====================================================================
 * ローダー実装 (hjp_gl_funcs.c の代わりにヘッダーinline)
 * HJP_GL_LOADER_IMPL を1つのCファイルで定義してからインクルード
 * ===================================================================*/

#ifdef HJP_GL_LOADER_IMPL

#undef HJP_GL_FUNC
#define HJP_GL_FUNC(ret, name, args) PFN_##name name = NULL;

/* Shader */
HJP_GL_FUNC(GLuint, glCreateShader,    (GLenum type))
HJP_GL_FUNC(void,   glDeleteShader,    (GLuint shader))
HJP_GL_FUNC(void,   glShaderSource,    (GLuint shader, GLsizei count, const GLchar *const*string, const GLint *length))
HJP_GL_FUNC(void,   glCompileShader,   (GLuint shader))
HJP_GL_FUNC(void,   glGetShaderiv,     (GLuint shader, GLenum pname, GLint *params))
HJP_GL_FUNC(void,   glGetShaderInfoLog,(GLuint shader, GLsizei bufSize, GLsizei *length, GLchar *infoLog))
HJP_GL_FUNC(GLuint, glCreateProgram,   (void))
HJP_GL_FUNC(void,   glDeleteProgram,   (GLuint program))
HJP_GL_FUNC(void,   glAttachShader,    (GLuint program, GLuint shader))
HJP_GL_FUNC(void,   glLinkProgram,     (GLuint program))
HJP_GL_FUNC(void,   glGetProgramiv,    (GLuint program, GLenum pname, GLint *params))
HJP_GL_FUNC(void,   glGetProgramInfoLog,(GLuint program, GLsizei bufSize, GLsizei *length, GLchar *infoLog))
HJP_GL_FUNC(void,   glUseProgram,      (GLuint program))
HJP_GL_FUNC(GLint,  glGetUniformLocation,(GLuint program, const GLchar *name))
HJP_GL_FUNC(GLuint, glGetUniformBlockIndex,(GLuint program, const GLchar *uniformBlockName))
HJP_GL_FUNC(void,   glUniformBlockBinding,(GLuint program, GLuint uniformBlockIndex, GLuint uniformBlockBinding))
HJP_GL_FUNC(void,   glBindAttribLocation,(GLuint program, GLuint index, const GLchar *name))
HJP_GL_FUNC(void,   glUniform1i,       (GLint location, GLint v0))
HJP_GL_FUNC(void,   glUniform2f,       (GLint location, GLfloat v0, GLfloat v1))
HJP_GL_FUNC(void,   glGenBuffers,      (GLsizei n, GLuint *buffers))
HJP_GL_FUNC(void,   glDeleteBuffers,   (GLsizei n, const GLuint *buffers))
HJP_GL_FUNC(void,   glBindBuffer,      (GLenum target, GLuint buffer))
HJP_GL_FUNC(void,   glBufferData,      (GLenum target, GLsizeiptr size, const void *data, GLenum usage))
HJP_GL_FUNC(void,   glBindBufferRange, (GLenum target, GLuint index, GLuint buffer, GLintptr offset, GLsizeiptr size))
HJP_GL_FUNC(void,   glGenVertexArrays, (GLsizei n, GLuint *arrays))
HJP_GL_FUNC(void,   glDeleteVertexArrays,(GLsizei n, const GLuint *arrays))
HJP_GL_FUNC(void,   glBindVertexArray, (GLuint arr))
HJP_GL_FUNC(void,   glEnableVertexAttribArray,(GLuint index))
HJP_GL_FUNC(void,   glVertexAttribPointer,(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void *pointer))
HJP_GL_FUNC(void,   glGenerateMipmap,  (GLenum target))
HJP_GL_FUNC(void,   glBlendFuncSeparate,(GLenum sfactorRGB, GLenum dfactorRGB, GLenum sfactorAlpha, GLenum dfactorAlpha))
HJP_GL_FUNC(void,   glStencilOpSeparate,(GLenum face, GLenum sfail, GLenum dpfail, GLenum dppass))

static void hjp_gl_load_functions(void) {
#undef HJP_GL_FUNC
#define HJP_GL_FUNC(ret, name, args) \
    name = (PFN_##name)wglGetProcAddress(#name);

    HJP_GL_FUNC(GLuint, glCreateShader,    (GLenum type))
    HJP_GL_FUNC(void,   glDeleteShader,    (GLuint shader))
    HJP_GL_FUNC(void,   glShaderSource,    (GLuint shader, GLsizei count, const GLchar *const*string, const GLint *length))
    HJP_GL_FUNC(void,   glCompileShader,   (GLuint shader))
    HJP_GL_FUNC(void,   glGetShaderiv,     (GLuint shader, GLenum pname, GLint *params))
    HJP_GL_FUNC(void,   glGetShaderInfoLog,(GLuint shader, GLsizei bufSize, GLsizei *length, GLchar *infoLog))
    HJP_GL_FUNC(GLuint, glCreateProgram,   (void))
    HJP_GL_FUNC(void,   glDeleteProgram,   (GLuint program))
    HJP_GL_FUNC(void,   glAttachShader,    (GLuint program, GLuint shader))
    HJP_GL_FUNC(void,   glLinkProgram,     (GLuint program))
    HJP_GL_FUNC(void,   glGetProgramiv,    (GLuint program, GLenum pname, GLint *params))
    HJP_GL_FUNC(void,   glGetProgramInfoLog,(GLuint program, GLsizei bufSize, GLsizei *length, GLchar *infoLog))
    HJP_GL_FUNC(void,   glUseProgram,      (GLuint program))
    HJP_GL_FUNC(GLint,  glGetUniformLocation,(GLuint program, const GLchar *name))
    HJP_GL_FUNC(GLuint, glGetUniformBlockIndex,(GLuint program, const GLchar *uniformBlockName))
    HJP_GL_FUNC(void,   glUniformBlockBinding,(GLuint program, GLuint uniformBlockIndex, GLuint uniformBlockBinding))
    HJP_GL_FUNC(void,   glBindAttribLocation,(GLuint program, GLuint index, const GLchar *name))
    HJP_GL_FUNC(void,   glUniform1i,       (GLint location, GLint v0))
    HJP_GL_FUNC(void,   glUniform2f,       (GLint location, GLfloat v0, GLfloat v1))
    HJP_GL_FUNC(void,   glGenBuffers,      (GLsizei n, GLuint *buffers))
    HJP_GL_FUNC(void,   glDeleteBuffers,   (GLsizei n, const GLuint *buffers))
    HJP_GL_FUNC(void,   glBindBuffer,      (GLenum target, GLuint buffer))
    HJP_GL_FUNC(void,   glBufferData,      (GLenum target, GLsizeiptr size, const void *data, GLenum usage))
    HJP_GL_FUNC(void,   glBindBufferRange, (GLenum target, GLuint index, GLuint buffer, GLintptr offset, GLsizeiptr size))
    HJP_GL_FUNC(void,   glGenVertexArrays, (GLsizei n, GLuint *arrays))
    HJP_GL_FUNC(void,   glDeleteVertexArrays,(GLsizei n, const GLuint *arrays))
    HJP_GL_FUNC(void,   glBindVertexArray, (GLuint arr))
    HJP_GL_FUNC(void,   glEnableVertexAttribArray,(GLuint index))
    HJP_GL_FUNC(void,   glVertexAttribPointer,(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void *pointer))
    HJP_GL_FUNC(void,   glGenerateMipmap,  (GLenum target))
    HJP_GL_FUNC(void,   glBlendFuncSeparate,(GLenum sfactorRGB, GLenum dfactorRGB, GLenum sfactorAlpha, GLenum dfactorAlpha))
    HJP_GL_FUNC(void,   glStencilOpSeparate,(GLenum face, GLenum sfail, GLenum dpfail, GLenum dppass))
}

#endif /* HJP_GL_LOADER_IMPL */
#endif /* HJP_GL_LOADER */
#endif /* HJP_GL_FUNCS_H */
