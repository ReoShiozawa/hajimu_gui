/***********************************************************************
 * hjp_render.h — はじむGUI 自作2Dベクター描画エンジン
 *
 * 自作2Dベクター描画エンジン。
 * OpenGL 3.2 Core Profile ベースの自作2Dベクターレンダラ。
 *
 * hajimu_gui.c の全描画呼び出しに対応。
 *
 * Copyright (c) 2026 Reo Shiozawa — MIT License
 ***********************************************************************/
#ifndef HJP_RENDER_H
#define HJP_RENDER_H

#ifdef __cplusplus
extern "C" {
#endif

/* =====================================================================
 *  OpenGL ヘッダー (プラットフォーム別)
 * ===================================================================*/
#ifdef __APPLE__
  #ifndef GL_SILENCE_DEPRECATION
    #define GL_SILENCE_DEPRECATION
  #endif
  #include <OpenGL/gl3.h>
#elif defined(_WIN32)
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
  #include <GL/gl.h>
  /* Windows GL 1.1 しかリンクできないので拡張関数を動的ロード */
  #define HJP_GL_LOADER
  #include "hjp_gl_funcs.h"
#elif defined(__linux__)
  #define GL_GLEXT_PROTOTYPES
  #include <GL/gl.h>
  #include <GL/glext.h>
#else
  #include <GL/gl.h>
#endif

#include <math.h>

/* =====================================================================
 * 定数
 * ===================================================================*/

#ifndef HJP_PI
#define HJP_PI 3.14159265358979323846264338327f
#endif

/* =====================================================================
 * 型定義
 * ===================================================================*/

typedef struct Hjpcontext Hjpcontext;

struct Hjpcolor {
    union {
        float rgba[4];
        struct { float r, g, b, a; };
    };
};
typedef struct Hjpcolor Hjpcolor;

struct Hjppaint {
    float xform[6];
    float extent[2];
    float radius;
    float feather;
    Hjpcolor innerColor;
    Hjpcolor outerColor;
    int image;
};
typedef struct Hjppaint Hjppaint;

struct Hjpscissor {
    float xform[6];
    float extent[2];
};
typedef struct Hjpscissor Hjpscissor;

struct Hjpvertex {
    float x, y, u, v;
};
typedef struct Hjpvertex Hjpvertex;

struct Hjppath {
    int first;
    int count;
    unsigned char closed;
    int nbevel;
    Hjpvertex *fill;
    int nfill;
    Hjpvertex *stroke;
    int nstroke;
    int winding;
    int convex;
};
typedef struct Hjppath Hjppath;

struct HjpglyphPosition {
    const char *str;
    float x;
    float minx, maxx;
};
typedef struct HjpglyphPosition HjpglyphPosition;

struct HjptextRow {
    const char *start;
    const char *end;
    const char *next;
    float width;
    float minx, maxx;
};
typedef struct HjptextRow HjptextRow;

struct HjpcompositeOperationState {
    int srcRGB;
    int dstRGB;
    int srcAlpha;
    int dstAlpha;
};
typedef struct HjpcompositeOperationState HjpcompositeOperationState;

/* =====================================================================
 * 列挙型
 * ===================================================================*/

enum Hjpwinding {
    HJP_CCW = 1,
    HJP_CW  = 2,
};

enum Hjpsolidity {
    HJP_SOLID = 1,
    HJP_HOLE  = 2,
};

enum HjplineCap {
    HJP_BUTT   = 0,
    HJP_ROUND  = 1,
    HJP_SQUARE = 2,
    HJP_BEVEL  = 3,
    HJP_MITER  = 4,
};

enum Hjpalign {
    HJP_ALIGN_LEFT     = 1 << 0,
    HJP_ALIGN_CENTER   = 1 << 1,
    HJP_ALIGN_RIGHT    = 1 << 2,
    HJP_ALIGN_TOP      = 1 << 3,
    HJP_ALIGN_MIDDLE   = 1 << 4,
    HJP_ALIGN_BOTTOM   = 1 << 5,
    HJP_ALIGN_BASELINE = 1 << 6,
};

enum HjpblendFactor {
    HJP_ZERO                = 1 << 0,
    HJP_ONE                 = 1 << 1,
    HJP_SRC_COLOR           = 1 << 2,
    HJP_ONE_MINUS_SRC_COLOR = 1 << 3,
    HJP_DST_COLOR           = 1 << 4,
    HJP_ONE_MINUS_DST_COLOR = 1 << 5,
    HJP_SRC_ALPHA           = 1 << 6,
    HJP_ONE_MINUS_SRC_ALPHA = 1 << 7,
    HJP_DST_ALPHA           = 1 << 8,
    HJP_ONE_MINUS_DST_ALPHA = 1 << 9,
    HJP_SRC_ALPHA_SATURATE  = 1 << 10,
};

enum HjpcompositeOperation {
    HJP_SOURCE_OVER,
    HJP_SOURCE_IN,
    HJP_SOURCE_OUT,
    HJP_ATOP,
    HJP_DESTINATION_OVER,
    HJP_DESTINATION_IN,
    HJP_DESTINATION_OUT,
    HJP_DESTINATION_ATOP,
    HJP_LIGHTER,
    HJP_COPY,
    HJP_XOR,
};

enum HjpimageFlags {
    HJP_IMAGE_GENERATE_MIPMAPS = 1 << 0,
    HJP_IMAGE_REPEATX          = 1 << 1,
    HJP_IMAGE_REPEATY          = 1 << 2,
    HJP_IMAGE_FLIPY            = 1 << 3,
    HJP_IMAGE_PREMULTIPLIED    = 1 << 4,
    HJP_IMAGE_NEAREST          = 1 << 5,
};

enum HjpcreateFlags {
    HJP_ANTIALIAS       = 1 << 0,
    HJP_STENCIL_STROKES = 1 << 1,
    HJP_DEBUG           = 1 << 2,
};

enum Hjptexture {
    HJP_TEXTURE_ALPHA = 0x01,
    HJP_TEXTURE_RGBA  = 0x02,
};

enum HjpimageFlagsGL {
    HJP_IMAGE_NODELETE = 1 << 16,
};

/* =====================================================================
 * GL3 コンテキスト作成/破棄
 * ===================================================================*/

Hjpcontext *hjpCreateGL3(int flags);
void        hjpDeleteGL3(Hjpcontext *ctx);

/* =====================================================================
 * フレーム
 * ===================================================================*/

void hjpBeginFrame(Hjpcontext *ctx, float windowWidth, float windowHeight, float devicePixelRatio);
void hjpCancelFrame(Hjpcontext *ctx);
void hjpEndFrame(Hjpcontext *ctx);

/* =====================================================================
 * 合成操作
 * ===================================================================*/

void hjpGlobalCompositeOperation(Hjpcontext *ctx, int op);
void hjpGlobalCompositeBlendFunc(Hjpcontext *ctx, int sfactor, int dfactor);
void hjpGlobalCompositeBlendFuncSeparate(Hjpcontext *ctx, int srcRGB, int dstRGB, int srcAlpha, int dstAlpha);

/* =====================================================================
 * 色ユーティリティ
 * ===================================================================*/

Hjpcolor hjpRGB(unsigned char r, unsigned char g, unsigned char b);
Hjpcolor hjpRGBf(float r, float g, float b);
Hjpcolor hjpRGBA(unsigned char r, unsigned char g, unsigned char b, unsigned char a);
Hjpcolor hjpRGBAf(float r, float g, float b, float a);
Hjpcolor hjpLerpRGBA(Hjpcolor c0, Hjpcolor c1, float u);
Hjpcolor hjpTransRGBA(Hjpcolor c, unsigned char a);
Hjpcolor hjpTransRGBAf(Hjpcolor c, float a);
Hjpcolor hjpHSL(float h, float s, float l);
Hjpcolor hjpHSLA(float h, float s, float l, unsigned char a);

/* =====================================================================
 * 状態管理
 * ===================================================================*/

void hjpSave(Hjpcontext *ctx);
void hjpRestore(Hjpcontext *ctx);
void hjpReset(Hjpcontext *ctx);

/* =====================================================================
 * レンダースタイル
 * ===================================================================*/

void hjpShapeAntiAlias(Hjpcontext *ctx, int enabled);
void hjpStrokeColor(Hjpcontext *ctx, Hjpcolor color);
void hjpStrokePaint(Hjpcontext *ctx, Hjppaint paint);
void hjpFillColor(Hjpcontext *ctx, Hjpcolor color);
void hjpFillPaint(Hjpcontext *ctx, Hjppaint paint);
void hjpMiterLimit(Hjpcontext *ctx, float limit);
void hjpStrokeWidth(Hjpcontext *ctx, float size);
void hjpLineCap(Hjpcontext *ctx, int cap);
void hjpLineJoin(Hjpcontext *ctx, int join);
void hjpGlobalAlpha(Hjpcontext *ctx, float alpha);

/* =====================================================================
 * 変換
 * ===================================================================*/

void hjpResetTransform(Hjpcontext *ctx);
void hjpTransform(Hjpcontext *ctx, float a, float b, float c, float d, float e, float f);
void hjpTranslate(Hjpcontext *ctx, float x, float y);
void hjpRotate(Hjpcontext *ctx, float angle);
void hjpSkewX(Hjpcontext *ctx, float angle);
void hjpSkewY(Hjpcontext *ctx, float angle);
void hjpScale(Hjpcontext *ctx, float x, float y);
void hjpCurrentTransform(Hjpcontext *ctx, float *xform);

/* 2x3行列ユーティリティ */
void  hjpTransformIdentity(float *dst);
void  hjpTransformTranslate(float *dst, float tx, float ty);
void  hjpTransformScale(float *dst, float sx, float sy);
void  hjpTransformRotate(float *dst, float a);
void  hjpTransformSkewX(float *dst, float a);
void  hjpTransformSkewY(float *dst, float a);
void  hjpTransformMultiply(float *dst, const float *src);
void  hjpTransformPremultiply(float *dst, const float *src);
int   hjpTransformInverse(float *dst, const float *src);
void  hjpTransformPoint(float *dstx, float *dsty, const float *xform, float srcx, float srcy);
float hjpDegToRad(float deg);
float hjpRadToDeg(float rad);

/* =====================================================================
 * 画像
 * ===================================================================*/

int  hjpCreateImage(Hjpcontext *ctx, const char *filename, int imageFlags);
int  hjpCreateImageMem(Hjpcontext *ctx, int imageFlags, unsigned char *data, int ndata);
int  hjpCreateImageRGBA(Hjpcontext *ctx, int w, int h, int imageFlags, const unsigned char *data);
void hjpUpdateImage(Hjpcontext *ctx, int image, const unsigned char *data);
void hjpImageSize(Hjpcontext *ctx, int image, int *w, int *h);
void hjpDeleteImage(Hjpcontext *ctx, int image);

/* =====================================================================
 * ペイント
 * ===================================================================*/

Hjppaint hjpLinearGradient(Hjpcontext *ctx, float sx, float sy, float ex, float ey, Hjpcolor icol, Hjpcolor ocol);
Hjppaint hjpBoxGradient(Hjpcontext *ctx, float x, float y, float w, float h, float r, float f, Hjpcolor icol, Hjpcolor ocol);
Hjppaint hjpRadialGradient(Hjpcontext *ctx, float cx, float cy, float inr, float outr, Hjpcolor icol, Hjpcolor ocol);
Hjppaint hjpImagePattern(Hjpcontext *ctx, float ox, float oy, float ex, float ey, float angle, int image, float alpha);

/* =====================================================================
 * シザー
 * ===================================================================*/

void hjpScissor(Hjpcontext *ctx, float x, float y, float w, float h);
void hjpIntersectScissor(Hjpcontext *ctx, float x, float y, float w, float h);
void hjpResetScissor(Hjpcontext *ctx);

/* =====================================================================
 * パス
 * ===================================================================*/

void hjpBeginPath(Hjpcontext *ctx);
void hjpMoveTo(Hjpcontext *ctx, float x, float y);
void hjpLineTo(Hjpcontext *ctx, float x, float y);
void hjpBezierTo(Hjpcontext *ctx, float c1x, float c1y, float c2x, float c2y, float x, float y);
void hjpQuadTo(Hjpcontext *ctx, float cx, float cy, float x, float y);
void hjpArcTo(Hjpcontext *ctx, float x1, float y1, float x2, float y2, float radius);
void hjpClosePath(Hjpcontext *ctx);
void hjpPathWinding(Hjpcontext *ctx, int dir);
void hjpArc(Hjpcontext *ctx, float cx, float cy, float r, float a0, float a1, int dir);
void hjpRect(Hjpcontext *ctx, float x, float y, float w, float h);
void hjpRoundedRect(Hjpcontext *ctx, float x, float y, float w, float h, float r);
void hjpRoundedRectVarying(Hjpcontext *ctx, float x, float y, float w, float h,
                           float radTopLeft, float radTopRight, float radBottomRight, float radBottomLeft);
void hjpEllipse(Hjpcontext *ctx, float cx, float cy, float rx, float ry);
void hjpCircle(Hjpcontext *ctx, float cx, float cy, float r);
void hjpFill(Hjpcontext *ctx);
void hjpStroke(Hjpcontext *ctx);

/* =====================================================================
 * テキスト
 * ===================================================================*/

int   hjpCreateFont(Hjpcontext *ctx, const char *name, const char *filename);
int   hjpCreateFontAtIndex(Hjpcontext *ctx, const char *name, const char *filename, int fontIndex);
int   hjpCreateFontMem(Hjpcontext *ctx, const char *name, unsigned char *data, int ndata, int freeData);
int   hjpCreateFontMemAtIndex(Hjpcontext *ctx, const char *name, unsigned char *data, int ndata, int freeData, int fontIndex);
int   hjpFindFont(Hjpcontext *ctx, const char *name);
int   hjpAddFallbackFontId(Hjpcontext *ctx, int baseFont, int fallbackFont);
int   hjpAddFallbackFont(Hjpcontext *ctx, const char *baseFont, const char *fallbackFont);
void  hjpResetFallbackFontsId(Hjpcontext *ctx, int baseFont);
void  hjpResetFallbackFonts(Hjpcontext *ctx, const char *baseFont);
void  hjpFontSize(Hjpcontext *ctx, float size);
void  hjpFontBlur(Hjpcontext *ctx, float blur);
void  hjpTextLetterSpacing(Hjpcontext *ctx, float spacing);
void  hjpTextLineHeight(Hjpcontext *ctx, float lineHeight);
void  hjpTextAlign(Hjpcontext *ctx, int align);
void  hjpFontFaceId(Hjpcontext *ctx, int font);
void  hjpFontFace(Hjpcontext *ctx, const char *font);
float hjpText(Hjpcontext *ctx, float x, float y, const char *string, const char *end);
void  hjpTextBox(Hjpcontext *ctx, float x, float y, float breakRowWidth, const char *string, const char *end);
float hjpTextBounds(Hjpcontext *ctx, float x, float y, const char *string, const char *end, float *bounds);
void  hjpTextBoxBounds(Hjpcontext *ctx, float x, float y, float breakRowWidth, const char *string, const char *end, float *bounds);
int   hjpTextGlyphPositions(Hjpcontext *ctx, float x, float y, const char *string, const char *end, HjpglyphPosition *positions, int maxPositions);
void  hjpTextMetrics(Hjpcontext *ctx, float *ascender, float *descender, float *lineh);
int   hjpTextBreakLines(Hjpcontext *ctx, const char *string, const char *end, float breakRowWidth, HjptextRow *rows, int maxRows);

/* =====================================================================
 * 内部レンダーAPI (互換用)
 * ===================================================================*/

struct Hjpparams {
    void *userPtr;
    int edgeAntiAlias;
    int (*renderCreate)(void *uptr);
    int (*renderCreateTexture)(void *uptr, int type, int w, int h, int imageFlags, const unsigned char *data);
    int (*renderDeleteTexture)(void *uptr, int image);
    int (*renderUpdateTexture)(void *uptr, int image, int x, int y, int w, int h, const unsigned char *data);
    int (*renderGetTextureSize)(void *uptr, int image, int *w, int *h);
    void (*renderViewport)(void *uptr, float width, float height, float devicePixelRatio);
    void (*renderCancel)(void *uptr);
    void (*renderFlush)(void *uptr);
    void (*renderFill)(void *uptr, Hjppaint *paint, HjpcompositeOperationState compositeOperation, Hjpscissor *scissor, float fringe, const float *bounds, const Hjppath *paths, int npaths);
    void (*renderStroke)(void *uptr, Hjppaint *paint, HjpcompositeOperationState compositeOperation, Hjpscissor *scissor, float fringe, float strokeWidth, const Hjppath *paths, int npaths);
    void (*renderTriangles)(void *uptr, Hjppaint *paint, HjpcompositeOperationState compositeOperation, Hjpscissor *scissor, const Hjpvertex *verts, int nverts, float fringe);
    void (*renderDelete)(void *uptr);
};
typedef struct Hjpparams Hjpparams;

Hjpcontext *hjpCreateInternal(Hjpparams *params);
void        hjpDeleteInternal(Hjpcontext *ctx);
Hjpparams  *hjpInternalParams(Hjpcontext *ctx);
void        hjpDebugDumpPathCache(Hjpcontext *ctx);

#define HJP_NOTUSED(v) for (;;) { (void)(1 ? (void)0 : ((void)(v))); break; }

#ifdef __cplusplus
}
#endif

#endif /* HJP_RENDER_H */
