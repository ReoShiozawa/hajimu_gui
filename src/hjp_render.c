/***********************************************************************
 * hjp_render.c — はじむGUI 2Dベクター描画エンジン実装
 *
 * 自作2Dベクター描画エンジン。
 * OpenGL 3.2 Core Profile ベースの自作2Dベクターレンダラ。
 *
 * 機能:
 *   - パス構築 (moveTo, lineTo, bezierTo, arc, etc.)
 *   - 三角形分割 + ストローク展開
 *   - ステンシルベース塗りつぶし
 *   - グラデーション (線形, 放射, ボックス)
 *   - 画像パターン
 *   - フォントアトラス + テキスト描画
 *   - アンチエイリアス (フリンジ頂点)
 *   - シザークリッピング
 *   - 状態スタック (save/restore)
 *
 * フォント描画: hjp_platform.h の hjp_font_* 経由 (CoreText/FreeType)
 * 画像読み込み: hjp_platform.h の hjp_image_* 経由 (ImageIO/system)
 *
 * Copyright (c) 2026 Reo Shiozawa — MIT License
 ***********************************************************************/

#include "hjp_render.h"
#include "hjp_platform.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <float.h>

/* =====================================================================
 * 内部定数
 * ===================================================================*/
#define HJP_INIT_COMMANDS_SIZE  256
#define HJP_INIT_POINTS_SIZE   128
#define HJP_INIT_PATHS_SIZE    16
#define HJP_INIT_VERTS_SIZE    256
#define HJP_MAX_STATES          64
#define HJP_MAX_FONTS           16
#define HJP_MAX_IMAGES          256
#define HJP_KAPPA90            0.5522847493f
#define HJP_MAX_FONTIMAGE_SIZE 2048
#define HJP_INIT_FONTIMAGE_SIZE 512

/* パスコマンド */
enum {
    HJP_MOVETO = 0,
    HJP_LINETO = 1,
    HJP_BEZIERTO = 2,
    HJP_CLOSE = 3,
    HJP_WINDING = 4,
};

/* ポイントフラグ */
enum {
    HJP_PT_CORNER   = 0x01,
    HJP_PT_LEFT     = 0x02,
    HJP_PT_BEVEL    = 0x04,
    HJP_PR_INNERBEVEL = 0x08,
};

/* GLコールタイプ */
enum {
    HJP_GL_NONE = 0,
    HJP_GL_FILL,
    HJP_GL_CONVEXFILL,
    HJP_GL_STROKE,
    HJP_GL_TRIANGLES,
};

/* シェーダータイプ */
enum {
    HJP_SHADER_FILLGRAD = 0,
    HJP_SHADER_FILLIMG  = 1,
    HJP_SHADER_SIMPLE   = 2,
    HJP_SHADER_IMG      = 3,
};

/* =====================================================================
 * 内部構造体
 * ===================================================================*/

typedef struct {
    float x, y;
    float dx, dy;
    float len;
    float dmx, dmy;
    unsigned char flags;
} HjpPoint;

typedef struct {
    int first, count;
    unsigned char closed;
    int nbevel;
    Hjpvertex *fill;
    int nfill;
    Hjpvertex *stroke;
    int nstroke;
    int winding;
    int convex;
} HjpPath;

typedef struct {
    HjpcompositeOperationState compositeOperation;
    int shapeAntiAlias;
    Hjppaint fill;
    Hjppaint stroke;
    float strokeWidth;
    float miterLimit;
    int lineJoin;
    int lineCap;
    float globalAlpha;
    float xform[6];
    Hjpscissor scissor;
    float fontSize;
    float fontBlur;
    float textLetterSpacing;
    float textLineHeight;
    int textAlign;
    int fontId;
} HjpState;

/* GLシェーダー */
typedef struct {
    GLuint prog, vert, frag;
    GLint loc_viewsize;
    GLint loc_tex;
    GLint loc_frag;
} HjpShader;

/* GLテクスチャ */
typedef struct {
    int id;
    GLuint tex;
    int width, height;
    int type;   /* HJP_TEXTURE_ALPHA or HJP_TEXTURE_RGBA */
    int flags;
} HjpTexture;

/* GLコール */
typedef struct {
    int type;
    int image;
    int pathOffset, pathCount;
    int triangleOffset, triangleCount;
    int uniformOffset;
    GLenum srcRGB, dstRGB, srcAlpha, dstAlpha;
} HjpCall;

/* GLパス */
typedef struct {
    int fillOffset, fillCount;
    int strokeOffset, strokeCount;
} HjpGLPath;

/* フラグメントユニフォーム (UBO) */
typedef struct {
    float scissorMat[12];   /* 3 vec4s */
    float paintMat[12];     /* 3 vec4s */
    Hjpcolor innerCol;
    Hjpcolor outerCol;
    float scissorExt[2];
    float scissorScale[2];
    float extent[2];
    float radius;
    float feather;
    float strokeMult;
    float strokeThr;
    int texType;
    int type;
} HjpFragUniforms;

/* フォントエントリ */
typedef struct {
    char name[64];
    HjpFont handle;   /* hjp_platform フォントハンドル */
} HjpFontEntry;

/* グリフキャッシュ */
typedef struct {
    uint32_t codepoint;
    int fontId;
    float size;
    int x, y, w, h;
    int xoff, yoff;
    float advance;
} HjpGlyph;

#define HJP_MAX_GLYPHS 4096

/* =====================================================================
 * Hjpcontext 本体
 * ===================================================================*/
struct Hjpcontext {
    /* --- 状態スタック --- */
    HjpState states[HJP_MAX_STATES];
    int nstates;

    /* --- パスコマンドバッファ --- */
    float *commands;
    int ccommands, ncommands;

    /* --- パス展開 --- */
    float commandx, commandy;
    HjpPoint *points;
    int npoints, cpoints;
    HjpPath *paths;
    int npaths, cpaths;
    Hjpvertex *verts;
    int nverts, cverts;

    /* --- フレーム --- */
    float devicePxRatio;
    float fringeWidth;
    float tessTol;
    float distTol;
    int drawCallCount, fillTriCount, strokeTriCount, textTriCount;

    /* --- GL レンダー状態 --- */
    HjpShader shader;
    HjpTexture textures[HJP_MAX_IMAGES];
    int ntextures, ctextures, textureId;
    GLuint vertBuf;
    GLuint vertArr;
    GLuint fragBuf;
    int fragSize;
    int flags;
    float view[2];

    /* --- GL コマンドバッファ --- */
    HjpCall *calls;
    int ccalls, ncalls;
    HjpGLPath *glpaths;
    int cglpaths, nglpaths;
    Hjpvertex *glverts;
    int cglverts, nglverts;
    unsigned char *uniforms;
    int cuniforms, nuniforms;

    /* --- キャッシュ state filter --- */
    GLuint boundTexture;
    GLuint dummyTex;  /* macOS: sampler が常に有効なテクスチャを参照するためのダミー (1×1 白) */
    GLuint stencilMask;
    GLenum stencilFunc;
    GLint stencilFuncRef;
    GLuint stencilFuncMask;

    /* --- フォント --- */
    HjpFontEntry fonts[HJP_MAX_FONTS];
    int nfonts;

    /* --- フォントアトラス --- */
    int fontImageIdx;
    int fontImages[4];
    int fontAtlasW, fontAtlasH;
    unsigned char *fontAtlasData;

    /* --- グリフキャッシュ --- */
    HjpGlyph glyphCache[HJP_MAX_GLYPHS];
    int nglyphs;
    int glyphAtlasX, glyphAtlasY, glyphAtlasRowH;
};

/* =====================================================================
 * ユーティリティ (前方宣言)
 * ===================================================================*/
static HjpState *hjp__getState(Hjpcontext *ctx);
static void hjp__setDevicePixelRatio(Hjpcontext *ctx, float ratio);
static int  hjp__maxi(int a, int b) { return a > b ? a : b; }
static int  hjp__mini(int a, int b) { return a < b ? a : b; }
static float hjp__maxf(float a, float b) { return a > b ? a : b; }
static float hjp__minf(float a, float b) { return a < b ? a : b; }
static float hjp__absf(float a) { return a >= 0.0f ? a : -a; }
static float hjp__signf(float a) { return a >= 0.0f ? 1.0f : -1.0f; }
static float hjp__clampf(float a, float lo, float hi) { return a < lo ? lo : (a > hi ? hi : a); }
static float hjp__cross(float dx0, float dy0, float dx1, float dy1) { return dx1*dy0 - dx0*dy1; }

static int hjp__ptEquals(float x1, float y1, float x2, float y2, float tol) {
    float dx = x2-x1, dy = y2-y1;
    return dx*dx + dy*dy < tol*tol;
}
static float hjp__normalize(float *x, float *y) {
    float d = sqrtf((*x)*(*x) + (*y)*(*y));
    if (d > 1e-6f) { float id = 1.0f/d; *x *= id; *y *= id; }
    return d;
}

/* =====================================================================
 * 2x3 行列
 * ===================================================================*/
void hjpTransformIdentity(float *t) {
    t[0]=1; t[1]=0; t[2]=0; t[3]=1; t[4]=0; t[5]=0;
}
void hjpTransformTranslate(float *t, float tx, float ty) {
    t[0]=1; t[1]=0; t[2]=0; t[3]=1; t[4]=tx; t[5]=ty;
}
void hjpTransformScale(float *t, float sx, float sy) {
    t[0]=sx; t[1]=0; t[2]=0; t[3]=sy; t[4]=0; t[5]=0;
}
void hjpTransformRotate(float *t, float a) {
    float cs=cosf(a), sn=sinf(a);
    t[0]=cs; t[1]=sn; t[2]=-sn; t[3]=cs; t[4]=0; t[5]=0;
}
void hjpTransformSkewX(float *t, float a) {
    t[0]=1; t[1]=0; t[2]=tanf(a); t[3]=1; t[4]=0; t[5]=0;
}
void hjpTransformSkewY(float *t, float a) {
    t[0]=1; t[1]=tanf(a); t[2]=0; t[3]=1; t[4]=0; t[5]=0;
}
void hjpTransformMultiply(float *t, const float *s) {
    float t0=t[0]*s[0]+t[1]*s[2], t2=t[2]*s[0]+t[3]*s[2];
    float t4=t[4]*s[0]+t[5]*s[2]+s[4];
    t[1]=t[0]*s[1]+t[1]*s[3]; t[3]=t[2]*s[1]+t[3]*s[3];
    t[5]=t[4]*s[1]+t[5]*s[3]+s[5];
    t[0]=t0; t[2]=t2; t[4]=t4;
}
void hjpTransformPremultiply(float *t, const float *s) {
    float s2[6]; memcpy(s2, s, sizeof(float)*6);
    hjpTransformMultiply(s2, t);
    memcpy(t, s2, sizeof(float)*6);
}
int hjpTransformInverse(float *inv, const float *t) {
    double det = (double)t[0]*t[3] - (double)t[2]*t[1];
    if (hjp__absf((float)det) < 1e-30) { hjpTransformIdentity(inv); return 0; }
    double invdet = 1.0/det;
    inv[0]=(float)(t[3]*invdet); inv[2]=(float)(-t[2]*invdet);
    inv[4]=(float)(((double)t[2]*t[5] - (double)t[3]*t[4])*invdet);
    inv[1]=(float)(-t[1]*invdet); inv[3]=(float)(t[0]*invdet);
    inv[5]=(float)(((double)t[1]*t[4] - (double)t[0]*t[5])*invdet);
    return 1;
}
void hjpTransformPoint(float *dx, float *dy, const float *t, float sx, float sy) {
    *dx = sx*t[0] + sy*t[2] + t[4];
    *dy = sx*t[1] + sy*t[3] + t[5];
}
float hjpDegToRad(float deg) { return deg / 180.0f * HJP_PI; }
float hjpRadToDeg(float rad) { return rad / HJP_PI * 180.0f; }

/* =====================================================================
 * 色ユーティリティ
 * ===================================================================*/
Hjpcolor hjpRGB(unsigned char r, unsigned char g, unsigned char b) {
    return hjpRGBA(r, g, b, 255);
}
Hjpcolor hjpRGBf(float r, float g, float b) {
    return hjpRGBAf(r, g, b, 1.0f);
}
Hjpcolor hjpRGBA(unsigned char r, unsigned char g, unsigned char b, unsigned char a) {
    Hjpcolor c; c.r=r/255.0f; c.g=g/255.0f; c.b=b/255.0f; c.a=a/255.0f; return c;
}
Hjpcolor hjpRGBAf(float r, float g, float b, float a) {
    Hjpcolor c; c.r=r; c.g=g; c.b=b; c.a=a; return c;
}
Hjpcolor hjpLerpRGBA(Hjpcolor c0, Hjpcolor c1, float u) {
    Hjpcolor c; float omu = 1-u;
    c.r = c0.r*omu + c1.r*u; c.g = c0.g*omu + c1.g*u;
    c.b = c0.b*omu + c1.b*u; c.a = c0.a*omu + c1.a*u;
    return c;
}
Hjpcolor hjpTransRGBA(Hjpcolor c, unsigned char a) {
    c.a = a / 255.0f; return c;
}
Hjpcolor hjpTransRGBAf(Hjpcolor c, float a) {
    c.a = a; return c;
}

static float hjp__hue(float h, float m1, float m2) {
    if (h < 0) h += 1;
    if (h > 1) h -= 1;
    if (h < 1.0f/6.0f) return m1 + (m2-m1)*h*6.0f;
    else if (h < 3.0f/6.0f) return m2;
    else if (h < 4.0f/6.0f) return m1 + (m2-m1)*(2.0f/3.0f-h)*6.0f;
    return m1;
}
Hjpcolor hjpHSL(float h, float s, float l) { return hjpHSLA(h,s,l,255); }
Hjpcolor hjpHSLA(float h, float s, float l, unsigned char a) {
    Hjpcolor col;
    h = fmodf(h, 1.0f); if (h<0) h+=1;
    float m2 = l<=0.5f ? l*(1+s) : l+s-l*s;
    float m1 = 2*l - m2;
    col.r = hjp__clampf(hjp__hue(h+1.0f/3.0f, m1, m2), 0, 1);
    col.g = hjp__clampf(hjp__hue(h, m1, m2), 0, 1);
    col.b = hjp__clampf(hjp__hue(h-1.0f/3.0f, m1, m2), 0, 1);
    col.a = a/255.0f;
    return col;
}

/* =====================================================================
 * 合成操作
 * ===================================================================*/
static HjpcompositeOperationState hjp__compositeState(int op) {
    HjpcompositeOperationState s = {0,0,0,0};
    int sf = HJP_ONE, df = HJP_ONE_MINUS_SRC_ALPHA;
    switch (op) {
    case HJP_SOURCE_OVER:       sf=HJP_ONE; df=HJP_ONE_MINUS_SRC_ALPHA; break;
    case HJP_SOURCE_IN:         sf=HJP_DST_ALPHA; df=HJP_ZERO; break;
    case HJP_SOURCE_OUT:        sf=HJP_ONE_MINUS_DST_ALPHA; df=HJP_ZERO; break;
    case HJP_ATOP:              sf=HJP_DST_ALPHA; df=HJP_ONE_MINUS_SRC_ALPHA; break;
    case HJP_DESTINATION_OVER:  sf=HJP_ONE_MINUS_DST_ALPHA; df=HJP_ONE; break;
    case HJP_DESTINATION_IN:    sf=HJP_ZERO; df=HJP_SRC_ALPHA; break;
    case HJP_DESTINATION_OUT:   sf=HJP_ZERO; df=HJP_ONE_MINUS_SRC_ALPHA; break;
    case HJP_DESTINATION_ATOP:  sf=HJP_ONE_MINUS_DST_ALPHA; df=HJP_SRC_ALPHA; break;
    case HJP_LIGHTER:           sf=HJP_ONE; df=HJP_ONE; break;
    case HJP_COPY:              sf=HJP_ONE; df=HJP_ZERO; break;
    case HJP_XOR:               sf=HJP_ONE_MINUS_DST_ALPHA; df=HJP_ONE_MINUS_SRC_ALPHA; break;
    }
    s.srcRGB=sf; s.dstRGB=df; s.srcAlpha=sf; s.dstAlpha=df;
    return s;
}

/* =====================================================================
 * 状態管理
 * ===================================================================*/
static HjpState *hjp__getState(Hjpcontext *ctx) {
    return &ctx->states[ctx->nstates - 1];
}

void hjpSave(Hjpcontext *ctx) {
    if (ctx->nstates >= HJP_MAX_STATES) return;
    if (ctx->nstates > 0)
        memcpy(&ctx->states[ctx->nstates], &ctx->states[ctx->nstates-1], sizeof(HjpState));
    ctx->nstates++;
}
void hjpRestore(Hjpcontext *ctx) {
    if (ctx->nstates <= 1) return;
    ctx->nstates--;
}
void hjpReset(Hjpcontext *ctx) {
    HjpState *s = hjp__getState(ctx);
    memset(s, 0, sizeof(*s));
    s->fill = (Hjppaint){{0}};
    s->fill.innerColor = hjpRGBA(255,255,255,255);
    s->fill.outerColor = hjpRGBA(255,255,255,255);
    s->stroke = (Hjppaint){{0}};
    s->stroke.innerColor = hjpRGBA(0,0,0,255);
    s->stroke.outerColor = hjpRGBA(0,0,0,255);
    s->compositeOperation = hjp__compositeState(HJP_SOURCE_OVER);
    s->shapeAntiAlias = 1;
    s->strokeWidth = 1.0f;
    s->miterLimit = 10.0f;
    s->lineCap = HJP_BUTT;
    s->lineJoin = HJP_MITER;
    s->globalAlpha = 1.0f;
    hjpTransformIdentity(s->xform);
    s->scissor.extent[0] = -1.0f;
    s->scissor.extent[1] = -1.0f;
    s->fontSize = 16.0f;
    s->textLetterSpacing = 0.0f;
    s->textLineHeight = 1.0f;
    s->textAlign = HJP_ALIGN_LEFT | HJP_ALIGN_BASELINE;
    s->fontId = 0;
}

/* =====================================================================
 * レンダースタイル
 * ===================================================================*/
void hjpShapeAntiAlias(Hjpcontext *ctx, int enabled) {
    hjp__getState(ctx)->shapeAntiAlias = enabled;
}
void hjpStrokeColor(Hjpcontext *ctx, Hjpcolor color) {
    HjpState *s = hjp__getState(ctx);
    memset(&s->stroke, 0, sizeof(s->stroke));
    hjpTransformIdentity(s->stroke.xform);
    s->stroke.innerColor = color;
    s->stroke.outerColor = color;
}
void hjpStrokePaint(Hjpcontext *ctx, Hjppaint paint) {
    HjpState *s = hjp__getState(ctx);
    s->stroke = paint;
    hjpTransformMultiply(s->stroke.xform, s->xform);
}
void hjpFillColor(Hjpcontext *ctx, Hjpcolor color) {
    HjpState *s = hjp__getState(ctx);
    memset(&s->fill, 0, sizeof(s->fill));
    hjpTransformIdentity(s->fill.xform);
    s->fill.innerColor = color;
    s->fill.outerColor = color;
}
void hjpFillPaint(Hjpcontext *ctx, Hjppaint paint) {
    HjpState *s = hjp__getState(ctx);
    s->fill = paint;
    hjpTransformMultiply(s->fill.xform, s->xform);
}
void hjpMiterLimit(Hjpcontext *ctx, float limit) { hjp__getState(ctx)->miterLimit = limit; }
void hjpStrokeWidth(Hjpcontext *ctx, float w) { hjp__getState(ctx)->strokeWidth = w; }
void hjpLineCap(Hjpcontext *ctx, int cap) { hjp__getState(ctx)->lineCap = cap; }
void hjpLineJoin(Hjpcontext *ctx, int join) { hjp__getState(ctx)->lineJoin = join; }
void hjpGlobalAlpha(Hjpcontext *ctx, float alpha) { hjp__getState(ctx)->globalAlpha = alpha; }

void hjpGlobalCompositeOperation(Hjpcontext *ctx, int op) {
    hjp__getState(ctx)->compositeOperation = hjp__compositeState(op);
}
void hjpGlobalCompositeBlendFunc(Hjpcontext *ctx, int sf, int df) {
    hjpGlobalCompositeBlendFuncSeparate(ctx, sf, df, sf, df);
}
void hjpGlobalCompositeBlendFuncSeparate(Hjpcontext *ctx, int srcRGB, int dstRGB, int srcAlpha, int dstAlpha) {
    HjpState *s = hjp__getState(ctx);
    s->compositeOperation.srcRGB = srcRGB;
    s->compositeOperation.dstRGB = dstRGB;
    s->compositeOperation.srcAlpha = srcAlpha;
    s->compositeOperation.dstAlpha = dstAlpha;
}

/* =====================================================================
 * 変換
 * ===================================================================*/
void hjpResetTransform(Hjpcontext *ctx) {
    hjpTransformIdentity(hjp__getState(ctx)->xform);
}
void hjpTransform(Hjpcontext *ctx, float a, float b, float c, float d, float e, float f) {
    float t[6] = {a,b,c,d,e,f};
    hjpTransformPremultiply(hjp__getState(ctx)->xform, t);
}
void hjpTranslate(Hjpcontext *ctx, float x, float y) {
    float t[6]; hjpTransformTranslate(t, x, y);
    hjpTransformPremultiply(hjp__getState(ctx)->xform, t);
}
void hjpRotate(Hjpcontext *ctx, float angle) {
    float t[6]; hjpTransformRotate(t, angle);
    hjpTransformPremultiply(hjp__getState(ctx)->xform, t);
}
void hjpSkewX(Hjpcontext *ctx, float angle) {
    float t[6]; hjpTransformSkewX(t, angle);
    hjpTransformPremultiply(hjp__getState(ctx)->xform, t);
}
void hjpSkewY(Hjpcontext *ctx, float angle) {
    float t[6]; hjpTransformSkewY(t, angle);
    hjpTransformPremultiply(hjp__getState(ctx)->xform, t);
}
void hjpScale(Hjpcontext *ctx, float x, float y) {
    float t[6]; hjpTransformScale(t, x, y);
    hjpTransformPremultiply(hjp__getState(ctx)->xform, t);
}
void hjpCurrentTransform(Hjpcontext *ctx, float *xform) {
    memcpy(xform, hjp__getState(ctx)->xform, sizeof(float)*6);
}

/* =====================================================================
 * シザー
 * ===================================================================*/
void hjpScissor(Hjpcontext *ctx, float x, float y, float w, float h) {
    HjpState *s = hjp__getState(ctx);
    w = hjp__maxf(0.0f, w);
    h = hjp__maxf(0.0f, h);
    hjpTransformIdentity(s->scissor.xform);
    s->scissor.xform[4] = x + w*0.5f;
    s->scissor.xform[5] = y + h*0.5f;
    hjpTransformMultiply(s->scissor.xform, s->xform);
    s->scissor.extent[0] = w*0.5f;
    s->scissor.extent[1] = h*0.5f;
}
void hjpIntersectScissor(Hjpcontext *ctx, float x, float y, float w, float h) {
    HjpState *s = hjp__getState(ctx);
    if (s->scissor.extent[0] < 0) { hjpScissor(ctx, x, y, w, h); return; }
    float invxform[6];
    hjpTransformInverse(invxform, s->xform);
    float ex = s->scissor.extent[0], ey = s->scissor.extent[1];
    float tex, tey;
    hjpTransformPoint(&tex, &tey, invxform, s->scissor.xform[4], s->scissor.xform[5]);
    float rx = hjp__maxf(x, tex - ex);
    float ry = hjp__maxf(y, tey - ey);
    float rw = hjp__minf(x+w, tex+ex) - rx;
    float rh = hjp__minf(y+h, tey+ey) - ry;
    hjpScissor(ctx, rx, ry, hjp__maxf(0,rw), hjp__maxf(0,rh));
}
void hjpResetScissor(Hjpcontext *ctx) {
    HjpState *s = hjp__getState(ctx);
    memset(&s->scissor, 0, sizeof(s->scissor));
    s->scissor.extent[0] = -1.0f;
    s->scissor.extent[1] = -1.0f;
}

/* =====================================================================
 * パス構築
 * ===================================================================*/
static void hjp__appendCommands(Hjpcontext *ctx, float *vals, int nvals) {
    HjpState *s = hjp__getState(ctx);
    if (ctx->ncommands + nvals > ctx->ccommands) {
        int ccommands = ctx->ncommands + nvals + ctx->ccommands/2;
        float *c = (float*)realloc(ctx->commands, sizeof(float)*ccommands);
        if (!c) return;
        ctx->commands = c;
        ctx->ccommands = ccommands;
    }
    /* Transform */
    int i = 0;
    while (i < nvals) {
        int cmd = (int)vals[i];
        switch (cmd) {
        case HJP_MOVETO:
            hjpTransformPoint(&vals[i+1], &vals[i+2], s->xform, vals[i+1], vals[i+2]);
            ctx->commandx = vals[i+1]; ctx->commandy = vals[i+2];
            i += 3; break;
        case HJP_LINETO:
            hjpTransformPoint(&vals[i+1], &vals[i+2], s->xform, vals[i+1], vals[i+2]);
            ctx->commandx = vals[i+1]; ctx->commandy = vals[i+2];
            i += 3; break;
        case HJP_BEZIERTO:
            hjpTransformPoint(&vals[i+1], &vals[i+2], s->xform, vals[i+1], vals[i+2]);
            hjpTransformPoint(&vals[i+3], &vals[i+4], s->xform, vals[i+3], vals[i+4]);
            hjpTransformPoint(&vals[i+5], &vals[i+6], s->xform, vals[i+5], vals[i+6]);
            ctx->commandx = vals[i+5]; ctx->commandy = vals[i+6];
            i += 7; break;
        case HJP_CLOSE:   i += 1; break;
        case HJP_WINDING: i += 2; break;
        default: i += 1; break;
        }
    }
    memcpy(ctx->commands + ctx->ncommands, vals, sizeof(float)*nvals);
    ctx->ncommands += nvals;
}

void hjpBeginPath(Hjpcontext *ctx) {
    ctx->ncommands = 0;
    ctx->npoints = 0;
    ctx->npaths = 0;
}
void hjpMoveTo(Hjpcontext *ctx, float x, float y) {
    float vals[] = {HJP_MOVETO, x, y};
    hjp__appendCommands(ctx, vals, 3);
}
void hjpLineTo(Hjpcontext *ctx, float x, float y) {
    float vals[] = {HJP_LINETO, x, y};
    hjp__appendCommands(ctx, vals, 3);
}
void hjpBezierTo(Hjpcontext *ctx, float c1x, float c1y, float c2x, float c2y, float x, float y) {
    float vals[] = {HJP_BEZIERTO, c1x, c1y, c2x, c2y, x, y};
    hjp__appendCommands(ctx, vals, 7);
}
void hjpQuadTo(Hjpcontext *ctx, float cx, float cy, float x, float y) {
    float x0 = ctx->commandx, y0 = ctx->commandy;
    hjpBezierTo(ctx, x0+2.0f/3.0f*(cx-x0), y0+2.0f/3.0f*(cy-y0),
                x+2.0f/3.0f*(cx-x), y+2.0f/3.0f*(cy-y), x, y);
}
void hjpArcTo(Hjpcontext *ctx, float x1, float y1, float x2, float y2, float radius) {
    float x0 = ctx->commandx, y0 = ctx->commandy;
    float dx0 = x0-x1, dy0 = y0-y1, dx1 = x2-x1, dy1 = y2-y1;
    hjp__normalize(&dx0, &dy0);
    hjp__normalize(&dx1, &dy1);
    float a = acosf(dx0*dx1 + dy0*dy1);
    float d = radius / tanf(a/2.0f);
    if (d > 10000.0f) { hjpLineTo(ctx, x1, y1); return; }
    float cross = dx1*dy0 - dx0*dy1;
    float cx, cy, a0, a1;
    int dir;
    if (cross > 0) {
        cx = x1+dx0*d + -dy0*radius;
        cy = y1+dy0*d + dx0*radius;
        a0 = atan2f(dx0, -dy0);
        a1 = atan2f(-dx1, dy1);
        dir = HJP_CW;
    } else {
        cx = x1+dx0*d + dy0*radius;
        cy = y1+dy0*d + -dx0*radius;
        a0 = atan2f(-dx0, dy0);
        a1 = atan2f(dx1, -dy1);
        dir = HJP_CCW;
    }
    hjpArc(ctx, cx, cy, radius, a0, a1, dir);
}
void hjpClosePath(Hjpcontext *ctx) {
    float vals[] = {HJP_CLOSE};
    hjp__appendCommands(ctx, vals, 1);
}
void hjpPathWinding(Hjpcontext *ctx, int dir) {
    float vals[] = {HJP_WINDING, (float)dir};
    hjp__appendCommands(ctx, vals, 2);
}

void hjpArc(Hjpcontext *ctx, float cx, float cy, float r, float a0, float a1, int dir) {
    float da = a1 - a0;
    int move = ctx->ncommands > 0 ? 0 : 1;
    if (dir == HJP_CW) {
        if (hjp__absf(da) >= HJP_PI*2) da = HJP_PI*2;
        else while (da < 0.0f) da += HJP_PI*2;
    } else {
        if (hjp__absf(da) >= HJP_PI*2) da = -HJP_PI*2;
        else while (da > 0.0f) da -= HJP_PI*2;
    }
    int ndivs = hjp__maxi(1, hjp__mini((int)(hjp__absf(da) / (HJP_PI*0.5f) + 0.5f), 5));
    float hda = (da / (float)ndivs) / 2.0f;
    float kappa = hjp__absf(4.0f/3.0f * (1.0f-cosf(hda)) / sinf(hda));
    if (dir == HJP_CCW) kappa = -kappa;

    float px = 0, py = 0, ptx = 0, pty = 0;
    for (int i = 0; i <= ndivs; i++) {
        float a = a0 + da * ((float)i / (float)ndivs);
        float dx = cosf(a), dy = sinf(a);
        float x = cx + dx*r, y = cy + dy*r;
        float tx = -dy*r*kappa, ty = dx*r*kappa;
        if (i == 0) {
            if (move) hjpMoveTo(ctx, x, y);
            else hjpLineTo(ctx, x, y);
        } else {
            hjpBezierTo(ctx, px+ptx, py+pty, x-tx, y-ty, x, y);
        }
        px = x; py = y; ptx = tx; pty = ty;
    }
}

void hjpRect(Hjpcontext *ctx, float x, float y, float w, float h) {
    hjpMoveTo(ctx, x, y);
    hjpLineTo(ctx, x+w, y);
    hjpLineTo(ctx, x+w, y+h);
    hjpLineTo(ctx, x, y+h);
    hjpClosePath(ctx);
}

void hjpRoundedRect(Hjpcontext *ctx, float x, float y, float w, float h, float r) {
    hjpRoundedRectVarying(ctx, x, y, w, h, r, r, r, r);
}

void hjpRoundedRectVarying(Hjpcontext *ctx, float x, float y, float w, float h,
                           float rtl, float rtr, float rbr, float rbl) {
    if (rtl < 0.1f && rtr < 0.1f && rbr < 0.1f && rbl < 0.1f) {
        hjpRect(ctx, x, y, w, h); return;
    }
    float halfw = hjp__absf(w)*0.5f, halfh = hjp__absf(h)*0.5f;
    float rxBL=hjp__minf(rbl,halfw)*hjp__signf(w), ryBL=hjp__minf(rbl,halfh)*hjp__signf(h);
    float rxBR=hjp__minf(rbr,halfw)*hjp__signf(w), ryBR=hjp__minf(rbr,halfh)*hjp__signf(h);
    float rxTR=hjp__minf(rtr,halfw)*hjp__signf(w), ryTR=hjp__minf(rtr,halfh)*hjp__signf(h);
    float rxTL=hjp__minf(rtl,halfw)*hjp__signf(w), ryTL=hjp__minf(rtl,halfh)*hjp__signf(h);
    hjpMoveTo(ctx, x, y+ryTL);
    hjpLineTo(ctx, x, y+h-ryBL);
    hjpBezierTo(ctx, x, y+h-ryBL*(1-HJP_KAPPA90), x+rxBL*(1-HJP_KAPPA90), y+h, x+rxBL, y+h);
    hjpLineTo(ctx, x+w-rxBR, y+h);
    hjpBezierTo(ctx, x+w-rxBR*(1-HJP_KAPPA90), y+h, x+w, y+h-ryBR*(1-HJP_KAPPA90), x+w, y+h-ryBR);
    hjpLineTo(ctx, x+w, y+ryTR);
    hjpBezierTo(ctx, x+w, y+ryTR*(1-HJP_KAPPA90), x+w-rxTR*(1-HJP_KAPPA90), y, x+w-rxTR, y);
    hjpLineTo(ctx, x+rxTL, y);
    hjpBezierTo(ctx, x+rxTL*(1-HJP_KAPPA90), y, x, y+ryTL*(1-HJP_KAPPA90), x, y+ryTL);
    hjpClosePath(ctx);
}

void hjpEllipse(Hjpcontext *ctx, float cx, float cy, float rx, float ry) {
    hjpMoveTo(ctx, cx-rx, cy);
    hjpBezierTo(ctx, cx-rx, cy+ry*HJP_KAPPA90, cx-rx*HJP_KAPPA90, cy+ry, cx, cy+ry);
    hjpBezierTo(ctx, cx+rx*HJP_KAPPA90, cy+ry, cx+rx, cy+ry*HJP_KAPPA90, cx+rx, cy);
    hjpBezierTo(ctx, cx+rx, cy-ry*HJP_KAPPA90, cx+rx*HJP_KAPPA90, cy-ry, cx, cy-ry);
    hjpBezierTo(ctx, cx-rx*HJP_KAPPA90, cy-ry, cx-rx, cy-ry*HJP_KAPPA90, cx-rx, cy);
    hjpClosePath(ctx);
}

void hjpCircle(Hjpcontext *ctx, float cx, float cy, float r) {
    hjpEllipse(ctx, cx, cy, r, r);
}

/* =====================================================================
 * ペイント
 * ===================================================================*/
Hjppaint hjpLinearGradient(Hjpcontext *ctx, float sx, float sy, float ex, float ey,
                           Hjpcolor icol, Hjpcolor ocol) {
    (void)ctx;
    Hjppaint p; memset(&p, 0, sizeof(p));
    float dx = ex-sx, dy = ey-sy;
    float d = sqrtf(dx*dx + dy*dy);
    if (d > 0.0001f) { dx/=d; dy/=d; } else { dx=0; dy=1; }
    p.xform[0] = dy; p.xform[1] = -dx;
    p.xform[2] = dx; p.xform[3] = dy;
    p.xform[4] = sx - dx*0.5f; p.xform[5] = sy - dy*0.5f;
    p.extent[0] = d*0.5f; p.extent[1] = d*0.5f + d*0.5f;
    p.radius = 0;
    p.feather = hjp__maxf(1.0f, d);
    p.innerColor = icol;
    p.outerColor = ocol;
    return p;
}

Hjppaint hjpRadialGradient(Hjpcontext *ctx, float cx, float cy, float inr, float outr,
                           Hjpcolor icol, Hjpcolor ocol) {
    (void)ctx;
    Hjppaint p; memset(&p, 0, sizeof(p));
    float r = (inr+outr)*0.5f;
    float f = outr-inr;
    hjpTransformIdentity(p.xform);
    p.xform[4] = cx; p.xform[5] = cy;
    p.extent[0] = r; p.extent[1] = r;
    p.radius = r;
    p.feather = hjp__maxf(1.0f, f);
    p.innerColor = icol;
    p.outerColor = ocol;
    return p;
}

Hjppaint hjpBoxGradient(Hjpcontext *ctx, float x, float y, float w, float h,
                        float r, float f, Hjpcolor icol, Hjpcolor ocol) {
    (void)ctx;
    Hjppaint p; memset(&p, 0, sizeof(p));
    hjpTransformIdentity(p.xform);
    p.xform[4] = x+w*0.5f; p.xform[5] = y+h*0.5f;
    p.extent[0] = w*0.5f; p.extent[1] = h*0.5f;
    p.radius = r;
    p.feather = hjp__maxf(1.0f, f);
    p.innerColor = icol;
    p.outerColor = ocol;
    return p;
}

Hjppaint hjpImagePattern(Hjpcontext *ctx, float ox, float oy, float ex, float ey,
                         float angle, int image, float alpha) {
    (void)ctx;
    Hjppaint p; memset(&p, 0, sizeof(p));
    hjpTransformRotate(p.xform, angle);
    p.xform[4] = ox; p.xform[5] = oy;
    p.extent[0] = ex; p.extent[1] = ey;
    p.image = image;
    p.innerColor = hjpRGBAf(1,1,1,alpha);
    p.outerColor = hjpRGBAf(1,1,1,alpha);
    return p;
}

/* =====================================================================
 * テクスチャ管理 (GL)
 * ===================================================================*/
static HjpTexture *hjp__allocTexture(Hjpcontext *ctx) {
    HjpTexture *tex = NULL;
    for (int i = 0; i < ctx->ntextures; i++) {
        if (ctx->textures[i].id == 0) { tex = &ctx->textures[i]; break; }
    }
    if (!tex) {
        if (ctx->ntextures >= HJP_MAX_IMAGES) return NULL;
        tex = &ctx->textures[ctx->ntextures++];
    }
    memset(tex, 0, sizeof(*tex));
    tex->id = ++ctx->textureId;
    return tex;
}
static HjpTexture *hjp__findTexture(Hjpcontext *ctx, int id) {
    for (int i = 0; i < ctx->ntextures; i++)
        if (ctx->textures[i].id == id) return &ctx->textures[i];
    return NULL;
}
static int hjp__deleteTexture(Hjpcontext *ctx, int id) {
    HjpTexture *tex = hjp__findTexture(ctx, id);
    if (!tex) return 0;
    if (tex->tex && !(tex->flags & HJP_IMAGE_NODELETE))
        glDeleteTextures(1, &tex->tex);
    memset(tex, 0, sizeof(*tex));
    return 1;
}

static int hjp__renderCreateTexture(Hjpcontext *ctx, int type, int w, int h,
                                     int imageFlags, const unsigned char *data) {
    HjpTexture *tex = hjp__allocTexture(ctx);
    if (!tex) return 0;
    tex->width = w; tex->height = h;
    tex->type = type; tex->flags = imageFlags;
    glGenTextures(1, &tex->tex);
    glBindTexture(GL_TEXTURE_2D, tex->tex);
    ctx->boundTexture = tex->tex;
    /* macOS: NULL データで glTexImage2D すると "unloadable" としてマークされる。
     * そのあとデータをアップロードしても解除されないため、NULL の場合はゼロ初期化バッファを使う。 */
    unsigned char *zero_buf = NULL;
    if (!data) {
        size_t bufsize = (size_t)w * (size_t)h * ((type == HJP_TEXTURE_RGBA) ? 4 : 1);
        zero_buf = (unsigned char*)calloc(1, bufsize);
        data = zero_buf;
    }
    if (type == HJP_TEXTURE_RGBA) {
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
    } else {
        /* macOS Core Profile: sized format GL_R8 を使う (GL_RED unsized は非対応) */
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, w, h, 0, GL_RED, GL_UNSIGNED_BYTE, data);
    }
    if (zero_buf) { free(zero_buf); zero_buf = NULL; data = NULL; }
    if (imageFlags & HJP_IMAGE_GENERATE_MIPMAPS) {
        if (imageFlags & HJP_IMAGE_NEAREST) {
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
        } else {
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        }
    } else {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                        (imageFlags & HJP_IMAGE_NEAREST) ? GL_NEAREST : GL_LINEAR);
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
                    (imageFlags & HJP_IMAGE_NEAREST) ? GL_NEAREST : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,
                    (imageFlags & HJP_IMAGE_REPEATX) ? GL_REPEAT : GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,
                    (imageFlags & HJP_IMAGE_REPEATY) ? GL_REPEAT : GL_CLAMP_TO_EDGE);
    if (imageFlags & HJP_IMAGE_GENERATE_MIPMAPS)
        glGenerateMipmap(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, 0);
    ctx->boundTexture = 0;
    return tex->id;
}

/* =====================================================================
 * 画像 API
 * ===================================================================*/
int hjpCreateImage(Hjpcontext *ctx, const char *filename, int imageFlags) {
    (void)ctx; (void)filename; (void)imageFlags;
    /* ファイル読み込み → メモリ → hjpCreateImageMem */
    FILE *f = fopen(filename, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *data = (unsigned char*)malloc(sz);
    if (!data) { fclose(f); return 0; }
    if ((long)fread(data, 1, sz, f) != sz) { free(data); fclose(f); return 0; }
    fclose(f);
    int img = hjpCreateImageMem(ctx, imageFlags, data, (int)sz);
    free(data);
    return img;
}

int hjpCreateImageMem(Hjpcontext *ctx, int imageFlags, unsigned char *data, int ndata) {
    int w, h;
    unsigned char *pixels = hjp_image_load_mem(data, ndata, &w, &h);
    if (!pixels) return 0;
    int img = hjpCreateImageRGBA(ctx, w, h, imageFlags | HJP_IMAGE_PREMULTIPLIED, pixels);
    hjp_image_free(pixels);
    return img;
}

int hjpCreateImageRGBA(Hjpcontext *ctx, int w, int h, int imageFlags, const unsigned char *data) {
    return hjp__renderCreateTexture(ctx, HJP_TEXTURE_RGBA, w, h, imageFlags, data);
}

void hjpUpdateImage(Hjpcontext *ctx, int image, const unsigned char *data) {
    HjpTexture *tex = hjp__findTexture(ctx, image);
    if (!tex) return;
    glBindTexture(GL_TEXTURE_2D, tex->tex);
    ctx->boundTexture = tex->tex;
    if (tex->type == HJP_TEXTURE_RGBA)
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, tex->width, tex->height, GL_RGBA, GL_UNSIGNED_BYTE, data);
    else
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, tex->width, tex->height, GL_RED, GL_UNSIGNED_BYTE, data);
    glBindTexture(GL_TEXTURE_2D, 0);
    ctx->boundTexture = 0;
}

void hjpImageSize(Hjpcontext *ctx, int image, int *w, int *h) {
    HjpTexture *tex = hjp__findTexture(ctx, image);
    if (tex) { if (w) *w = tex->width; if (h) *h = tex->height; }
    else { if (w) *w = 0; if (h) *h = 0; }
}

void hjpDeleteImage(Hjpcontext *ctx, int image) {
    hjp__deleteTexture(ctx, image);
}

/* =====================================================================
 * パス展開 (三角形分割)
 * ===================================================================*/
static HjpPoint *hjp__addPoint(Hjpcontext *ctx, float x, float y, int flags) {
    if (ctx->npoints > 0) {
        HjpPoint *pt = &ctx->points[ctx->npoints-1];
        if (hjp__ptEquals(pt->x, pt->y, x, y, ctx->distTol)) {
            pt->flags |= flags;
            return pt;
        }
    }
    if (ctx->npoints+1 > ctx->cpoints) {
        int cp = ctx->cpoints + hjp__maxi(ctx->cpoints/2,8);
        HjpPoint *p = (HjpPoint*)realloc(ctx->points, sizeof(HjpPoint)*cp);
        if (!p) return NULL;
        ctx->points = p; ctx->cpoints = cp;
    }
    HjpPoint *pt = &ctx->points[ctx->npoints];
    memset(pt, 0, sizeof(*pt));
    pt->x = x; pt->y = y;
    pt->flags = (unsigned char)flags;
    ctx->npoints++;
    return pt;
}

static void hjp__addPath(Hjpcontext *ctx) {
    if (ctx->npaths + 1 > ctx->cpaths) {
        int cp = ctx->cpaths + hjp__maxi(ctx->cpaths/2, 8);
        HjpPath *p = (HjpPath*)realloc(ctx->paths, sizeof(HjpPath)*cp);
        if (!p) return;
        ctx->paths = p; ctx->cpaths = cp;
    }
    HjpPath *path = &ctx->paths[ctx->npaths];
    memset(path, 0, sizeof(*path));
    path->first = ctx->npoints;
    path->winding = HJP_CCW;
    ctx->npaths++;
}

static HjpPath *hjp__lastPath(Hjpcontext *ctx) {
    return ctx->npaths > 0 ? &ctx->paths[ctx->npaths-1] : NULL;
}

static void hjp__closePath(Hjpcontext *ctx) {
    HjpPath *p = hjp__lastPath(ctx);
    if (p) p->closed = 1;
}

static void hjp__pathWinding(Hjpcontext *ctx, int winding) {
    HjpPath *p = hjp__lastPath(ctx);
    if (p) p->winding = winding;
}

static void hjp__tesselateBezier(Hjpcontext *ctx,
    float x1, float y1, float x2, float y2,
    float x3, float y3, float x4, float y4, int level, int type) {
    if (level > 10) return;
    float x12=(x1+x2)*0.5f, y12=(y1+y2)*0.5f;
    float x23=(x2+x3)*0.5f, y23=(y2+y3)*0.5f;
    float x34=(x3+x4)*0.5f, y34=(y3+y4)*0.5f;
    float x123=(x12+x23)*0.5f, y123=(y12+y23)*0.5f;
    float x234=(x23+x34)*0.5f, y234=(y23+y34)*0.5f;
    float x1234=(x123+x234)*0.5f, y1234=(y123+y234)*0.5f;
    float dx = x4-x1, dy = y4-y1;
    float d2 = hjp__absf(((x2-x4)*dy - (y2-y4)*dx));
    float d3 = hjp__absf(((x3-x4)*dy - (y3-y4)*dx));
    if ((d2+d3)*(d2+d3) < ctx->tessTol * (dx*dx + dy*dy)) {
        hjp__addPoint(ctx, x4, y4, type);
        return;
    }
    hjp__tesselateBezier(ctx, x1,y1, x12,y12, x123,y123, x1234,y1234, level+1, 0);
    hjp__tesselateBezier(ctx, x1234,y1234, x234,y234, x34,y34, x4,y4, level+1, type);
}

/* パスのフラット化: コマンドバッファ→ポイント列 */
static void hjp__flattenPaths(Hjpcontext *ctx) {
    ctx->npoints = 0;
    ctx->npaths = 0;

    float *cmd = ctx->commands;
    int i = 0;
    while (i < ctx->ncommands) {
        int c = (int)cmd[i];
        switch (c) {
        case HJP_MOVETO:
            hjp__addPath(ctx);
            hjp__addPoint(ctx, cmd[i+1], cmd[i+2], HJP_PT_CORNER);
            i += 3; break;
        case HJP_LINETO:
            hjp__addPoint(ctx, cmd[i+1], cmd[i+2], HJP_PT_CORNER);
            i += 3; break;
        case HJP_BEZIERTO: {
            HjpPoint *last = ctx->npoints > 0 ? &ctx->points[ctx->npoints-1] : NULL;
            float lx = last ? last->x : 0, ly = last ? last->y : 0;
            hjp__tesselateBezier(ctx, lx,ly, cmd[i+1],cmd[i+2],
                                cmd[i+3],cmd[i+4], cmd[i+5],cmd[i+6], 0, HJP_PT_CORNER);
            i += 7; break;
        }
        case HJP_CLOSE:
            hjp__closePath(ctx);
            i += 1; break;
        case HJP_WINDING:
            hjp__pathWinding(ctx, (int)cmd[i+1]);
            i += 2; break;
        default: i++; break;
        }
    }
    /* 各パスのcount計算 + 方向・長さ計算 */
    for (int j = 0; j < ctx->npaths; j++) {
        HjpPath *path = &ctx->paths[j];
        HjpPoint *pts = ctx->points + path->first;
        int npts = (j+1 < ctx->npaths) ? ctx->paths[j+1].first - path->first
                                        : ctx->npoints - path->first;
        path->count = npts;
        /* Direction vectors */
        HjpPoint *p0 = &pts[npts-1], *p1;
        for (int k = 0; k < npts; k++) {
            p1 = &pts[k];
            p0->dx = p1->x - p0->x;
            p0->dy = p1->y - p0->y;
            p0->len = hjp__normalize(&p0->dx, &p0->dy);
            p0 = p1;
        }
    }
}

/* 頂点バッファ確保 */
static Hjpvertex *hjp__allocVerts(Hjpcontext *ctx, int n) {
    if (ctx->nverts + n > ctx->cverts) {
        int cv = hjp__maxi(ctx->nverts + n, ctx->cverts + ctx->cverts/2);
        Hjpvertex *v = (Hjpvertex*)realloc(ctx->verts, sizeof(Hjpvertex)*cv);
        if (!v) return NULL;
        ctx->verts = v; ctx->cverts = cv;
    }
    Hjpvertex *ret = ctx->verts + ctx->nverts;
    ctx->nverts += n;
    return ret;
}

static void hjp__vset(Hjpvertex *v, float x, float y, float u, float vv) {
    v->x = x; v->y = y; v->u = u; v->v = vv;
}

/* パス展開: フィル用頂点生成 (fan triangulation for convex) */
static void hjp__expandFill(Hjpcontext *ctx, float w, int lineJoin, float miterLimit) {
    (void)lineJoin; (void)miterLimit;
    int aa = (ctx->flags & HJP_ANTIALIAS) ? 1 : 0;
    float fringe = ctx->fringeWidth;
    /* Calculate: 凸性判定 */
    for (int i = 0; i < ctx->npaths; i++) {
        HjpPath *path = &ctx->paths[i];
        HjpPoint *pts = ctx->points + path->first;
        int npts = path->count;
        /* Convex check */
        float area = 0;
        for (int j = 0; j < npts; j++) {
            int k = (j+1) % npts;
            area += pts[j].x * pts[k].y - pts[k].x * pts[j].y;
        }
        path->convex = (path->winding == HJP_CCW) ? (area < 0 ? 0 : 1) : (area > 0 ? 0 : 1);
        /* Allocate fill verts (fan triangulation) */
        int nfill = npts;
        int nstroke_verts = aa ? (npts + 1) * 2 : 0;
        path->fill = hjp__allocVerts(ctx, nfill + nstroke_verts);
        if (!path->fill) continue;
        path->nfill = nfill;
        /* Fill vertices */
        for (int j = 0; j < npts; j++) {
            hjp__vset(&path->fill[j], pts[j].x, pts[j].y, 0.5f, 1.0f);
        }
        /* AA fringe */
        if (aa) {
            path->stroke = path->fill + nfill;
            path->nstroke = nstroke_verts;
            float lw = w + fringe;
            for (int j = 0, k = npts-1; j < npts; k = j++) {
                float dlx = pts[j].y - pts[k].y;
                float dly = -(pts[j].x - pts[k].x);
                hjp__normalize(&dlx, &dly);
                hjp__vset(&path->stroke[j*2+0], pts[j].x + dlx*lw, pts[j].y + dly*lw, 0.0f, 0.0f);
                hjp__vset(&path->stroke[j*2+1], pts[j].x, pts[j].y, 0.5f, 1.0f);
            }
            /* Close */
            hjp__vset(&path->stroke[npts*2+0], path->stroke[0].x, path->stroke[0].y, 0.0f, 0.0f);
            hjp__vset(&path->stroke[npts*2+1], path->stroke[1].x, path->stroke[1].y, 0.5f, 1.0f);
        } else {
            path->stroke = NULL;
            path->nstroke = 0;
        }
    }
}

/* パス展開: ストローク用頂点生成 (triangle strip along path) */
static void hjp__expandStroke(Hjpcontext *ctx, float w, float fringe, int lineCap, int lineJoin, float miterLimit) {
    (void)miterLimit;
    int aa = (ctx->flags & HJP_ANTIALIAS) ? 1 : 0;
    float lw = w * 0.5f;
    if (aa) lw += fringe * 0.5f;

    for (int i = 0; i < ctx->npaths; i++) {
        HjpPath *path = &ctx->paths[i];
        HjpPoint *pts = ctx->points + path->first;
        int npts = path->count;
        if (npts < 2) continue;
        int closed = path->closed;
        int loop = closed;
        int s = closed ? 0 : 1;
        int e = closed ? npts : npts - 1;
        /* Cap + body + cap */
        int nverts = (e - s) * 2 + (closed ? 2 : 4);
        path->stroke = hjp__allocVerts(ctx, nverts);
        if (!path->stroke) continue;
        Hjpvertex *dst = path->stroke;
        int ndst = 0;

        /* Start cap */
        if (!closed) {
            float dx = pts[1].x - pts[0].x;
            float dy = pts[1].y - pts[0].y;
            hjp__normalize(&dx, &dy);
            float dlx = dy, dly = -dx;
            if (lineCap == HJP_BUTT) {
                hjp__vset(&dst[ndst++], pts[0].x + dlx*lw, pts[0].y + dly*lw, 0.0f, aa?0.0f:0.5f);
                hjp__vset(&dst[ndst++], pts[0].x - dlx*lw, pts[0].y - dly*lw, 1.0f, aa?0.0f:0.5f);
            } else if (lineCap == HJP_SQUARE) {
                hjp__vset(&dst[ndst++], pts[0].x + dlx*lw - dx*lw, pts[0].y + dly*lw - dy*lw, 0.0f, 0.5f);
                hjp__vset(&dst[ndst++], pts[0].x - dlx*lw - dx*lw, pts[0].y - dly*lw - dy*lw, 1.0f, 0.5f);
            } else { /* HJP_ROUND - simplified */
                hjp__vset(&dst[ndst++], pts[0].x + dlx*lw, pts[0].y + dly*lw, 0.0f, 0.5f);
                hjp__vset(&dst[ndst++], pts[0].x - dlx*lw, pts[0].y - dly*lw, 1.0f, 0.5f);
            }
        }

        /* Body segments */
        for (int j = s; j < e; j++) {
            int j0 = loop ? ((j - 1 + npts) % npts) : (j > 0 ? j-1 : j);
            int j1 = j;
            (void)j0;
            float dx, dy;
            if (j + 1 < npts) { dx = pts[j+1].x - pts[j].x; dy = pts[j+1].y - pts[j].y; }
            else if (loop) { dx = pts[0].x - pts[j].x; dy = pts[0].y - pts[j].y; }
            else { dx = pts[j].x - pts[j-1].x; dy = pts[j].y - pts[j-1].y; }
            hjp__normalize(&dx, &dy);
            float dlx = dy, dly = -dx;
            (void)lineJoin;
            hjp__vset(&dst[ndst++], pts[j1].x + dlx*lw, pts[j1].y + dly*lw, 0.0f, 0.5f);
            hjp__vset(&dst[ndst++], pts[j1].x - dlx*lw, pts[j1].y - dly*lw, 1.0f, 0.5f);
        }

        /* End cap */
        if (!closed) {
            int last = npts - 1;
            float dx = pts[last].x - pts[last-1].x;
            float dy = pts[last].y - pts[last-1].y;
            hjp__normalize(&dx, &dy);
            float dlx = dy, dly = -dx;
            if (lineCap == HJP_BUTT) {
                hjp__vset(&dst[ndst++], pts[last].x + dlx*lw, pts[last].y + dly*lw, 0.0f, aa?0.0f:0.5f);
                hjp__vset(&dst[ndst++], pts[last].x - dlx*lw, pts[last].y - dly*lw, 1.0f, aa?0.0f:0.5f);
            } else if (lineCap == HJP_SQUARE) {
                hjp__vset(&dst[ndst++], pts[last].x + dlx*lw + dx*lw, pts[last].y + dly*lw + dy*lw, 0.0f, 0.5f);
                hjp__vset(&dst[ndst++], pts[last].x - dlx*lw + dx*lw, pts[last].y - dly*lw + dy*lw, 1.0f, 0.5f);
            } else {
                hjp__vset(&dst[ndst++], pts[last].x + dlx*lw, pts[last].y + dly*lw, 0.0f, 0.5f);
                hjp__vset(&dst[ndst++], pts[last].x - dlx*lw, pts[last].y - dly*lw, 1.0f, 0.5f);
            }
        } else {
            /* Close loop */
            hjp__vset(&dst[ndst++], dst[0].x, dst[0].y, 0.0f, 0.5f);
            hjp__vset(&dst[ndst++], dst[1].x, dst[1].y, 1.0f, 0.5f);
        }

        path->nstroke = ndst;
        path->fill = NULL;
        path->nfill = 0;
    }
}

/* =====================================================================
 * GL シェーダー
 * ===================================================================*/
static const char *hjp__vertShaderSrc =
    "#version 150 core\n"
    "#define NANOVG_GL3 1\n"
    "#define USE_UNIFORMBUFFER 1\n"
    "uniform vec2 viewSize;\n"
    "in vec2 vertex;\n"
    "in vec2 tcoord;\n"
    "out vec2 ftcoord;\n"
    "out vec2 fpos;\n"
    "void main(void) {\n"
    "   ftcoord = tcoord;\n"
    "   fpos = vertex;\n"
    "   gl_Position = vec4(2.0*vertex.x/viewSize.x - 1.0, 1.0 - 2.0*vertex.y/viewSize.y, 0, 1);\n"
    "}\n";

static const char *hjp__fragShaderSrc =
    "#version 150 core\n"
    "#define NANOVG_GL3 1\n"
    "#define USE_UNIFORMBUFFER 1\n"
    "#define EDGE_AA 1\n"
    "layout(std140) uniform frag {\n"
    "   mat3 scissorMat;\n"
    "   mat3 paintMat;\n"
    "   vec4 innerCol;\n"
    "   vec4 outerCol;\n"
    "   vec2 scissorExt;\n"
    "   vec2 scissorScale;\n"
    "   vec2 extent;\n"
    "   float radius;\n"
    "   float feather;\n"
    "   float strokeMult;\n"
    "   float strokeThr;\n"
    "   int texType;\n"
    "   int type;\n"
    "};\n"
    "uniform sampler2D tex;\n"
    "in vec2 ftcoord;\n"
    "in vec2 fpos;\n"
    "out vec4 outColor;\n"
    "float sdroundrect(vec2 pt, vec2 ext, float rad) {\n"
    "   vec2 ext2 = ext - vec2(rad,rad);\n"
    "   vec2 d = abs(pt) - ext2;\n"
    "   return min(max(d.x,d.y),0.0) + length(max(d,0.0)) - rad;\n"
    "}\n"
    "float scissorMask(vec2 p) {\n"
    "   vec2 sc = (abs((scissorMat * vec3(p,1.0)).xy) - scissorExt);\n"
    "   sc = vec2(0.5,0.5) - sc * scissorScale;\n"
    "   return clamp(sc.x,0.0,1.0) * clamp(sc.y,0.0,1.0);\n"
    "}\n"
    "float strokeMask() {\n"
    "   return min(1.0, (1.0-abs(ftcoord.x*2.0-1.0))*strokeMult) * min(1.0, ftcoord.y);\n"
    "}\n"
    "void main(void) {\n"
    "   vec4 result;\n"
    "   float scissor = scissorMask(fpos);\n"
    "   float strokeAlpha = strokeMask();\n"
    "   if (strokeAlpha < strokeThr) discard;\n"
    "   if (type == 0) {\n"
    "       vec2 pt = (paintMat * vec3(fpos,1.0)).xy;\n"
    "       float d = clamp((sdroundrect(pt, extent, radius) + feather*0.5) / feather, 0.0, 1.0);\n"
    "       vec4 color = mix(innerCol,outerCol,d);\n"
    "       color *= strokeAlpha * scissor;\n"
    "       result = color;\n"
    "   } else if (type == 1) {\n"
    "       vec2 pt = (paintMat * vec3(fpos,1.0)).xy / extent;\n"
    "       vec4 color = texture(tex, pt);\n"
    "       if (texType == 1) color = vec4(color.xyz*color.w,color.w);\n"
    "       if (texType == 2) color = vec4(color.x);\n"
    "       color *= innerCol;\n"
    "       color *= strokeAlpha * scissor;\n"
    "       result = color;\n"
    "   } else if (type == 2) {\n"
    "       result = vec4(1,1,1,1);\n"
    "   } else if (type == 3) {\n"
    "       vec4 color = texture(tex, ftcoord);\n"
    "       if (texType == 1) color = vec4(color.xyz*color.w,color.w);\n"
    "       if (texType == 2) color = vec4(color.x);\n"
    "       color *= scissor;\n"
    "       result = color * innerCol;\n"
    "   }\n"
    "   outColor = result;\n"
    "}\n";

static int hjp__createShader(HjpShader *shader) {
    memset(shader, 0, sizeof(*shader));
    shader->prog = glCreateProgram();
    shader->vert = glCreateShader(GL_VERTEX_SHADER);
    shader->frag = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(shader->vert, 1, &hjp__vertShaderSrc, NULL);
    glCompileShader(shader->vert);
    GLint status = 0;
    glGetShaderiv(shader->vert, GL_COMPILE_STATUS, &status);
    if (status != GL_TRUE) {
        char log[512]; glGetShaderInfoLog(shader->vert, 512, NULL, log);
        fprintf(stderr, "[hjp_render] vertex shader error: %s\n", log);
        return 0;
    }
    glShaderSource(shader->frag, 1, &hjp__fragShaderSrc, NULL);
    glCompileShader(shader->frag);
    glGetShaderiv(shader->frag, GL_COMPILE_STATUS, &status);
    if (status != GL_TRUE) {
        char log[512]; glGetShaderInfoLog(shader->frag, 512, NULL, log);
        fprintf(stderr, "[hjp_render] fragment shader error: %s\n", log);
        return 0;
    }
    glAttachShader(shader->prog, shader->vert);
    glAttachShader(shader->prog, shader->frag);
    glBindAttribLocation(shader->prog, 0, "vertex");
    glBindAttribLocation(shader->prog, 1, "tcoord");
    glLinkProgram(shader->prog);
    glGetProgramiv(shader->prog, GL_LINK_STATUS, &status);
    if (status != GL_TRUE) {
        char log[512]; glGetProgramInfoLog(shader->prog, 512, NULL, log);
        fprintf(stderr, "[hjp_render] link error: %s\n", log);
        return 0;
    }
    shader->loc_viewsize = glGetUniformLocation(shader->prog, "viewSize");
    shader->loc_tex = glGetUniformLocation(shader->prog, "tex");
    shader->loc_frag = glGetUniformBlockIndex(shader->prog, "frag");
    return 1;
}

static void hjp__deleteShader(HjpShader *shader) {
    if (shader->prog) glDeleteProgram(shader->prog);
    if (shader->vert) glDeleteShader(shader->vert);
    if (shader->frag) glDeleteShader(shader->frag);
    memset(shader, 0, sizeof(*shader));
}

/* =====================================================================
 * GLレンダーバッファ管理
 * ===================================================================*/
static HjpCall *hjp__allocCall(Hjpcontext *ctx) {
    if (ctx->ncalls + 1 > ctx->ccalls) {
        int cc = hjp__maxi(ctx->ncalls+1, ctx->ccalls+128);
        HjpCall *c = (HjpCall*)realloc(ctx->calls, sizeof(HjpCall)*cc);
        if (!c) return NULL;
        ctx->calls = c; ctx->ccalls = cc;
    }
    HjpCall *call = &ctx->calls[ctx->ncalls++];
    memset(call, 0, sizeof(*call));
    return call;
}

static int hjp__allocGLPaths(Hjpcontext *ctx, int n) {
    if (ctx->nglpaths + n > ctx->cglpaths) {
        int cp = hjp__maxi(ctx->nglpaths + n, ctx->cglpaths + 128);
        HjpGLPath *p = (HjpGLPath*)realloc(ctx->glpaths, sizeof(HjpGLPath)*cp);
        if (!p) return -1;
        ctx->glpaths = p; ctx->cglpaths = cp;
    }
    int ret = ctx->nglpaths;
    ctx->nglpaths += n;
    return ret;
}

static int hjp__allocGLVerts(Hjpcontext *ctx, int n) {
    if (ctx->nglverts + n > ctx->cglverts) {
        int cv = hjp__maxi(ctx->nglverts + n, ctx->cglverts + 4096);
        Hjpvertex *v = (Hjpvertex*)realloc(ctx->glverts, sizeof(Hjpvertex)*cv);
        if (!v) return -1;
        ctx->glverts = v; ctx->cglverts = cv;
    }
    int ret = ctx->nglverts;
    ctx->nglverts += n;
    return ret;
}

static int hjp__allocFragUniforms(Hjpcontext *ctx, int n) {
    int structSize = ctx->fragSize;
    if (ctx->nuniforms + n > ctx->cuniforms) {
        int cu = hjp__maxi(ctx->nuniforms + n, ctx->cuniforms + 128);
        unsigned char *u = (unsigned char*)realloc(ctx->uniforms, structSize * cu);
        if (!u) return -1;
        ctx->uniforms = u; ctx->cuniforms = cu;
    }
    int ret = ctx->nuniforms;
    ctx->nuniforms += n;
    return ret;
}

static HjpFragUniforms *hjp__fragUniformPtr(Hjpcontext *ctx, int i) {
    return (HjpFragUniforms*)(ctx->uniforms + i * ctx->fragSize);
}

/* =====================================================================
 * ユニフォーム設定
 * ===================================================================*/
static void hjp__xformToMat3x3(float *m3, const float *t) {
    m3[0]=t[0]; m3[1]=t[1]; m3[2]=0;
    m3[3]=0;    /* padding */
    m3[4]=t[2]; m3[5]=t[3]; m3[6]=0;
    m3[7]=0;
    m3[8]=t[4]; m3[9]=t[5]; m3[10]=1;
    m3[11]=0;
}

static GLenum hjp__blendFactor(int factor) {
    if (factor == HJP_ZERO) return GL_ZERO;
    if (factor == HJP_ONE) return GL_ONE;
    if (factor == HJP_SRC_COLOR) return GL_SRC_COLOR;
    if (factor == HJP_ONE_MINUS_SRC_COLOR) return GL_ONE_MINUS_SRC_COLOR;
    if (factor == HJP_DST_COLOR) return GL_DST_COLOR;
    if (factor == HJP_ONE_MINUS_DST_COLOR) return GL_ONE_MINUS_DST_COLOR;
    if (factor == HJP_SRC_ALPHA) return GL_SRC_ALPHA;
    if (factor == HJP_ONE_MINUS_SRC_ALPHA) return GL_ONE_MINUS_SRC_ALPHA;
    if (factor == HJP_DST_ALPHA) return GL_DST_ALPHA;
    if (factor == HJP_ONE_MINUS_DST_ALPHA) return GL_ONE_MINUS_DST_ALPHA;
    if (factor == HJP_SRC_ALPHA_SATURATE) return GL_SRC_ALPHA_SATURATE;
    return GL_INVALID_ENUM;
}

static int hjp__convertPaint(Hjpcontext *ctx, HjpFragUniforms *frag, Hjppaint *paint,
                              Hjpscissor *scissor, float width, float fringe, float strokeThr) {
    (void)ctx;
    memset(frag, 0, sizeof(*frag));
    frag->innerCol = paint->innerColor;
    frag->outerCol = paint->outerColor;
    /* Premultiply alpha */
    frag->innerCol.r *= frag->innerCol.a;
    frag->innerCol.g *= frag->innerCol.a;
    frag->innerCol.b *= frag->innerCol.a;
    frag->outerCol.r *= frag->outerCol.a;
    frag->outerCol.g *= frag->outerCol.a;
    frag->outerCol.b *= frag->outerCol.a;

    if (scissor->extent[0] < -0.5f || scissor->extent[1] < -0.5f) {
        /* No scissor */
        frag->scissorExt[0] = 1.0f; frag->scissorExt[1] = 1.0f;
        frag->scissorScale[0] = 1.0f; frag->scissorScale[1] = 1.0f;
        memset(frag->scissorMat, 0, sizeof(frag->scissorMat));
    } else {
        float invxform[6];
        hjpTransformInverse(invxform, scissor->xform);
        hjp__xformToMat3x3(frag->scissorMat, invxform);
        frag->scissorExt[0] = scissor->extent[0];
        frag->scissorExt[1] = scissor->extent[1];
        frag->scissorScale[0] = sqrtf(scissor->xform[0]*scissor->xform[0]+scissor->xform[2]*scissor->xform[2]) / fringe;
        frag->scissorScale[1] = sqrtf(scissor->xform[1]*scissor->xform[1]+scissor->xform[3]*scissor->xform[3]) / fringe;
    }
    frag->extent[0] = paint->extent[0];
    frag->extent[1] = paint->extent[1];
    frag->strokeMult = (width*0.5f + fringe*0.5f) / fringe;
    frag->strokeThr = strokeThr;
    if (paint->image != 0) {
        HjpTexture *tex = hjp__findTexture(ctx, paint->image);
        if (!tex) return 0;
        if ((tex->flags & HJP_IMAGE_FLIPY) != 0) {
            float m1[6], m2[6]; hjpTransformTranslate(m1, 0, frag->extent[1]*0.5f);
            hjpTransformMultiply(m1, paint->xform);
            hjpTransformScale(m2, 1.0f, -1.0f);
            hjpTransformMultiply(m2, m1);
            hjpTransformTranslate(m1, 0, -frag->extent[1]*0.5f);
            hjpTransformMultiply(m1, m2);
            hjpTransformInverse(m2, m1);
            hjp__xformToMat3x3(frag->paintMat, m2);
        } else {
            float invxform[6]; hjpTransformInverse(invxform, paint->xform);
            hjp__xformToMat3x3(frag->paintMat, invxform);
        }
        frag->type = HJP_SHADER_FILLIMG;
        if (tex->type == HJP_TEXTURE_RGBA)
            frag->texType = (tex->flags & HJP_IMAGE_PREMULTIPLIED) ? 0 : 1;
        else
            frag->texType = 2;
    } else {
        float invxform[6]; hjpTransformInverse(invxform, paint->xform);
        hjp__xformToMat3x3(frag->paintMat, invxform);
        frag->radius = paint->radius;
        frag->feather = paint->feather;
        frag->type = HJP_SHADER_FILLGRAD;
    }
    return 1;
}

/* =====================================================================
 * GL描画フラッシュ
 * ===================================================================*/
static void hjp__setUniforms(Hjpcontext *ctx, int uniformOffset, int image) {
    glBindBufferRange(GL_UNIFORM_BUFFER, 0, ctx->fragBuf,
                      uniformOffset * ctx->fragSize, sizeof(HjpFragUniforms));
    if (image != 0) {
        HjpTexture *tex = hjp__findTexture(ctx, image);
        if (tex) {
            glBindTexture(GL_TEXTURE_2D, tex->tex);
            ctx->boundTexture = tex->tex;
        }
    } else {
        /* macOS: texture 0 (非バインド) は sampler2D の警告を引き起こすため
         * ダミーテクスチャをバインドする */
        glBindTexture(GL_TEXTURE_2D, ctx->dummyTex);
        ctx->boundTexture = ctx->dummyTex;
    }
}

static void hjp__renderFlush(Hjpcontext *ctx) {
    if (ctx->ncalls == 0) return;

    /* Upload vertex data */
    glBindBuffer(GL_ARRAY_BUFFER, ctx->vertBuf);
    glBufferData(GL_ARRAY_BUFFER, ctx->nglverts * sizeof(Hjpvertex), ctx->glverts, GL_STREAM_DRAW);

    /* Upload uniform data */
    glBindBuffer(GL_UNIFORM_BUFFER, ctx->fragBuf);
    glBufferData(GL_UNIFORM_BUFFER, ctx->nuniforms * ctx->fragSize, ctx->uniforms, GL_STREAM_DRAW);

    /* Setup state */
    glUseProgram(ctx->shader.prog);
    glUniform2f(ctx->shader.loc_viewsize, ctx->view[0], ctx->view[1]);
    glUniform1i(ctx->shader.loc_tex, 0);
    glUniformBlockBinding(ctx->shader.prog, ctx->shader.loc_frag, 0);

    glBindVertexArray(ctx->vertArr);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glFrontFace(GL_CCW);
    glDisable(GL_DEPTH_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glStencilMask(0xffffffff);
    glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
    glStencilFunc(GL_ALWAYS, 0, 0xffffffff);
    ctx->boundTexture = 0;
    ctx->stencilMask = 0xffffffff;
    ctx->stencilFunc = GL_ALWAYS;
    ctx->stencilFuncRef = 0;
    ctx->stencilFuncMask = 0xffffffff;

    /* Process calls */
    for (int i = 0; i < ctx->ncalls; i++) {
        HjpCall *call = &ctx->calls[i];
        glBlendFuncSeparate(call->srcRGB, call->dstRGB, call->srcAlpha, call->dstAlpha);

        if (call->type == HJP_GL_CONVEXFILL) {
            hjp__setUniforms(ctx, call->uniformOffset, call->image);
            for (int j = 0; j < call->pathCount; j++) {
                HjpGLPath *gp = &ctx->glpaths[call->pathOffset + j];
                glDrawArrays(GL_TRIANGLE_FAN, gp->fillOffset, gp->fillCount);
                if (gp->strokeCount > 0)
                    glDrawArrays(GL_TRIANGLE_STRIP, gp->strokeOffset, gp->strokeCount);
            }
        } else if (call->type == HJP_GL_FILL) {
            /* Stencil fill */
            glEnable(GL_STENCIL_TEST);
            glStencilMask(0xff);
            glStencilFunc(GL_ALWAYS, 0, 0xff);
            glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);

            hjp__setUniforms(ctx, call->uniformOffset + 1, 0);
            glStencilOpSeparate(GL_FRONT, GL_KEEP, GL_KEEP, GL_INCR_WRAP);
            glStencilOpSeparate(GL_BACK, GL_KEEP, GL_KEEP, GL_DECR_WRAP);
            glDisable(GL_CULL_FACE);
            for (int j = 0; j < call->pathCount; j++) {
                HjpGLPath *gp = &ctx->glpaths[call->pathOffset + j];
                glDrawArrays(GL_TRIANGLE_FAN, gp->fillOffset, gp->fillCount);
            }
            glEnable(GL_CULL_FACE);

            /* Cover */
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            hjp__setUniforms(ctx, call->uniformOffset, call->image);
            glStencilFunc(GL_NOTEQUAL, 0, 0xff);
            glStencilOp(GL_ZERO, GL_ZERO, GL_ZERO);
            for (int j = 0; j < call->pathCount; j++) {
                HjpGLPath *gp = &ctx->glpaths[call->pathOffset + j];
                if (gp->strokeCount > 0)
                    glDrawArrays(GL_TRIANGLE_STRIP, gp->strokeOffset, gp->strokeCount);
            }
            /* Reset stencil */
            glStencilFunc(GL_ALWAYS, 0, 0xff);
            glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
            glDisable(GL_STENCIL_TEST);

        } else if (call->type == HJP_GL_STROKE) {
            hjp__setUniforms(ctx, call->uniformOffset, call->image);
            for (int j = 0; j < call->pathCount; j++) {
                HjpGLPath *gp = &ctx->glpaths[call->pathOffset + j];
                glDrawArrays(GL_TRIANGLE_STRIP, gp->strokeOffset, gp->strokeCount);
            }
        } else if (call->type == HJP_GL_TRIANGLES) {
            hjp__setUniforms(ctx, call->uniformOffset, call->image);
            glDrawArrays(GL_TRIANGLES, call->triangleOffset, call->triangleCount);
        }
    }

    /* Reset GL state */
    glDisable(GL_CULL_FACE);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glUseProgram(0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindVertexArray(0);
}

/* =====================================================================
 * Fill / Stroke
 * ===================================================================*/
void hjpFill(Hjpcontext *ctx) {
    HjpState *state = hjp__getState(ctx);
    Hjppaint fillPaint = state->fill;
    hjp__flattenPaths(ctx);
    hjp__expandFill(ctx, ctx->fringeWidth, state->lineJoin, state->miterLimit);

    fillPaint.innerColor.a *= state->globalAlpha;
    fillPaint.outerColor.a *= state->globalAlpha;

    HjpCall *call = hjp__allocCall(ctx);
    if (!call) return;

    int maxverts = 0;
    for (int i = 0; i < ctx->npaths; i++)
        maxverts += ctx->paths[i].nfill + ctx->paths[i].nstroke;

    int pathOffset = hjp__allocGLPaths(ctx, ctx->npaths);
    int vertOffset = hjp__allocGLVerts(ctx, maxverts);
    int uniformOffset = hjp__allocFragUniforms(ctx, 2);
    if (pathOffset < 0 || vertOffset < 0 || uniformOffset < 0) { ctx->ncalls--; return; }

    call->pathOffset = pathOffset;
    call->pathCount = ctx->npaths;
    call->image = fillPaint.image;
    call->uniformOffset = uniformOffset;

    /* Determine convex vs complex fill */
    int convex = (ctx->npaths == 1 && ctx->paths[0].convex);
    call->type = convex ? HJP_GL_CONVEXFILL : HJP_GL_FILL;

    /* Blend */
    call->srcRGB = hjp__blendFactor(state->compositeOperation.srcRGB);
    call->dstRGB = hjp__blendFactor(state->compositeOperation.dstRGB);
    call->srcAlpha = hjp__blendFactor(state->compositeOperation.srcAlpha);
    call->dstAlpha = hjp__blendFactor(state->compositeOperation.dstAlpha);

    /* Copy verts */
    int off = vertOffset;
    for (int i = 0; i < ctx->npaths; i++) {
        HjpPath *path = &ctx->paths[i];
        HjpGLPath *gp = &ctx->glpaths[pathOffset + i];
        memset(gp, 0, sizeof(*gp));
        if (path->nfill > 0) {
            gp->fillOffset = off;
            gp->fillCount = path->nfill;
            memcpy(&ctx->glverts[off], path->fill, sizeof(Hjpvertex)*path->nfill);
            off += path->nfill;
        }
        if (path->nstroke > 0) {
            gp->strokeOffset = off;
            gp->strokeCount = path->nstroke;
            memcpy(&ctx->glverts[off], path->stroke, sizeof(Hjpvertex)*path->nstroke);
            off += path->nstroke;
        }
    }

    /* Uniforms */
    HjpFragUniforms *frag = hjp__fragUniformPtr(ctx, uniformOffset);
    hjp__convertPaint(ctx, frag, &fillPaint, &state->scissor, ctx->fringeWidth, ctx->fringeWidth, -1.0f);

    /* Simple uniform for stencil fill */
    HjpFragUniforms *frag2 = hjp__fragUniformPtr(ctx, uniformOffset + 1);
    memset(frag2, 0, sizeof(*frag2));
    frag2->strokeThr = -1.0f;
    frag2->type = HJP_SHADER_SIMPLE;
}

void hjpStroke(Hjpcontext *ctx) {
    HjpState *state = hjp__getState(ctx);
    float scale = 0.5f * (sqrtf(state->xform[0]*state->xform[0]+state->xform[2]*state->xform[2]) +
                          sqrtf(state->xform[1]*state->xform[1]+state->xform[3]*state->xform[3]));
    float strokeWidth = hjp__clampf(state->strokeWidth * scale, 0.0f, 200.0f);
    Hjppaint strokePaint = state->stroke;

    if (strokeWidth < ctx->fringeWidth) {
        float alpha = hjp__clampf(strokeWidth / ctx->fringeWidth, 0.0f, 1.0f);
        strokePaint.innerColor.a *= alpha*alpha;
        strokePaint.outerColor.a *= alpha*alpha;
        strokeWidth = ctx->fringeWidth;
    }
    strokePaint.innerColor.a *= state->globalAlpha;
    strokePaint.outerColor.a *= state->globalAlpha;

    hjp__flattenPaths(ctx);
    hjp__expandStroke(ctx, strokeWidth, ctx->fringeWidth, state->lineCap, state->lineJoin, state->miterLimit);

    HjpCall *call = hjp__allocCall(ctx);
    if (!call) return;
    call->type = HJP_GL_STROKE;

    int maxverts = 0;
    for (int i = 0; i < ctx->npaths; i++)
        maxverts += ctx->paths[i].nstroke;

    int pathOffset = hjp__allocGLPaths(ctx, ctx->npaths);
    int vertOffset = hjp__allocGLVerts(ctx, maxverts);
    int uniformOffset = hjp__allocFragUniforms(ctx, 1);
    if (pathOffset < 0 || vertOffset < 0 || uniformOffset < 0) { ctx->ncalls--; return; }

    call->pathOffset = pathOffset;
    call->pathCount = ctx->npaths;
    call->image = strokePaint.image;
    call->uniformOffset = uniformOffset;
    call->srcRGB = hjp__blendFactor(state->compositeOperation.srcRGB);
    call->dstRGB = hjp__blendFactor(state->compositeOperation.dstRGB);
    call->srcAlpha = hjp__blendFactor(state->compositeOperation.srcAlpha);
    call->dstAlpha = hjp__blendFactor(state->compositeOperation.dstAlpha);

    int off = vertOffset;
    for (int i = 0; i < ctx->npaths; i++) {
        HjpPath *path = &ctx->paths[i];
        HjpGLPath *gp = &ctx->glpaths[pathOffset + i];
        memset(gp, 0, sizeof(*gp));
        if (path->nstroke) {
            gp->strokeOffset = off;
            gp->strokeCount = path->nstroke;
            memcpy(&ctx->glverts[off], path->stroke, sizeof(Hjpvertex)*path->nstroke);
            off += path->nstroke;
        }
    }

    HjpFragUniforms *frag = hjp__fragUniformPtr(ctx, uniformOffset);
    hjp__convertPaint(ctx, frag, &strokePaint, &state->scissor, strokeWidth, ctx->fringeWidth, -1.0f);
}

/* =====================================================================
 * フォントアトラス + テキスト描画
 * ===================================================================*/
static int hjp__fontImageCreate(Hjpcontext *ctx, int w, int h) {
    /* フォントアトラスはRGBAで作成（GL_R8のmacOSドライバ問題回避） */
    return hjp__renderCreateTexture(ctx, HJP_TEXTURE_RGBA, w, h, 0, NULL);
}

static void hjp__fontAtlasInit(Hjpcontext *ctx) {
    ctx->fontAtlasW = HJP_INIT_FONTIMAGE_SIZE;
    ctx->fontAtlasH = HJP_INIT_FONTIMAGE_SIZE;
    /* RGBAフォーマット: 4 bytes/pixel */
    ctx->fontAtlasData = (unsigned char*)calloc(4, ctx->fontAtlasW * ctx->fontAtlasH);
    ctx->fontImageIdx = 0;
    ctx->fontImages[0] = hjp__fontImageCreate(ctx, ctx->fontAtlasW, ctx->fontAtlasH);
    ctx->glyphAtlasX = 1;
    ctx->glyphAtlasY = 1;
    ctx->glyphAtlasRowH = 0;
}

static uint32_t hjp__decodeUTF8(const char **s) {
    const unsigned char *p = (const unsigned char *)*s;
    uint32_t cp;
    if (p[0] < 0x80) { cp = p[0]; *s += 1; }
    else if ((p[0] & 0xE0) == 0xC0) { cp = ((p[0]&0x1F)<<6)|(p[1]&0x3F); *s += 2; }
    else if ((p[0] & 0xF0) == 0xE0) { cp = ((p[0]&0x0F)<<12)|((p[1]&0x3F)<<6)|(p[2]&0x3F); *s += 3; }
    else if ((p[0] & 0xF8) == 0xF0) { cp = ((p[0]&0x07)<<18)|((p[1]&0x3F)<<12)|((p[2]&0x3F)<<6)|(p[3]&0x3F); *s += 4; }
    else { cp = 0xFFFD; *s += 1; }
    return cp;
}

static HjpGlyph *hjp__findGlyph(Hjpcontext *ctx, uint32_t cp, int fontId, float size) {
    for (int i = 0; i < ctx->nglyphs; i++) {
        HjpGlyph *g = &ctx->glyphCache[i];
        if (g->codepoint == cp && g->fontId == fontId && hjp__absf(g->size - size) < 0.5f)
            return g;
    }
    return NULL;
}

static HjpGlyph *hjp__renderGlyph(Hjpcontext *ctx, uint32_t cp, int fontId, float size) {
    HjpGlyph *existing = hjp__findGlyph(ctx, cp, fontId, size);
    if (existing) return existing;
    if (fontId < 0 || fontId >= ctx->nfonts || !ctx->fonts[fontId].handle) return NULL;

    unsigned char *bitmap = NULL;
    int gw, gh, gxoff, gyoff;
    float advance;
    int ok = hjp_font_get_glyph(ctx->fonts[fontId].handle, size, cp,
                                 &bitmap, &gw, &gh, &gxoff, &gyoff, &advance);
    if (!ok || !bitmap) { if (bitmap) free(bitmap); return NULL; }

    if (ctx->glyphAtlasX + gw + 1 >= ctx->fontAtlasW) {
        ctx->glyphAtlasX = 1;
        ctx->glyphAtlasY += ctx->glyphAtlasRowH + 1;
        ctx->glyphAtlasRowH = 0;
    }
    if (ctx->glyphAtlasY + gh + 1 >= ctx->fontAtlasH) {
        /* Grow atlas */
        int newH = ctx->fontAtlasH * 2;
        if (newH > HJP_MAX_FONTIMAGE_SIZE) newH = HJP_MAX_FONTIMAGE_SIZE;
        if (ctx->glyphAtlasY + gh + 1 >= newH) {
            /* Still too small — evict cache */
            ctx->nglyphs = 0;
            ctx->glyphAtlasX = 1;
            ctx->glyphAtlasY = 1;
            ctx->glyphAtlasRowH = 0;
            memset(ctx->fontAtlasData, 0, ctx->fontAtlasW * ctx->fontAtlasH * 4);
        } else {
            unsigned char *newData = (unsigned char*)calloc(4, ctx->fontAtlasW * newH);
            if (newData) {
                memcpy(newData, ctx->fontAtlasData, ctx->fontAtlasW * ctx->fontAtlasH * 4);
                free(ctx->fontAtlasData);
                ctx->fontAtlasData = newData;
                ctx->fontAtlasH = newH;
            }
        }
        /* Recreate atlas texture */
        if (ctx->fontImages[0]) hjpDeleteImage(ctx, ctx->fontImages[0]);
        ctx->fontImages[0] = hjp__fontImageCreate(ctx, ctx->fontAtlasW, ctx->fontAtlasH);
    }

    /* Blit glyph into atlas data (RGBA: 4 bytes/pixel, all channels = glyph alpha) */
    int ax = ctx->glyphAtlasX, ay = ctx->glyphAtlasY;
    for (int row = 0; row < gh; row++) {
        if (ay+row < ctx->fontAtlasH) {
            unsigned char *dst = ctx->fontAtlasData + ((ay+row)*ctx->fontAtlasW + ax) * 4;
            const unsigned char *src = bitmap + row*gw;
            for (int col = 0; col < gw; col++) {
                unsigned char a = src[col];
                dst[col*4+0] = a;
                dst[col*4+1] = a;
                dst[col*4+2] = a;
                dst[col*4+3] = a;
            }
        }
    }
    free(bitmap);

    if (gh > ctx->glyphAtlasRowH) ctx->glyphAtlasRowH = gh;
    ctx->glyphAtlasX += gw + 1;

    /* Update atlas texture */
    HjpTexture *tex = hjp__findTexture(ctx, ctx->fontImages[0]);
    if (tex) {
        glBindTexture(GL_TEXTURE_2D, tex->tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, ctx->fontAtlasW, ctx->fontAtlasH,
                        GL_RGBA, GL_UNSIGNED_BYTE, ctx->fontAtlasData);
        glBindTexture(GL_TEXTURE_2D, 0);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    /* Add to cache */
    HjpGlyph *g;
    if (ctx->nglyphs < HJP_MAX_GLYPHS) {
        g = &ctx->glyphCache[ctx->nglyphs++];
    } else {
        g = &ctx->glyphCache[ctx->nglyphs - 1]; /* Overwrite last */
    }
    g->codepoint = cp; g->fontId = fontId; g->size = size;
    g->x = ax; g->y = ay; g->w = gw; g->h = gh;
    g->xoff = gxoff; g->yoff = gyoff;
    g->advance = advance;
    return g;
}

/* =====================================================================
 * テキスト API
 * ===================================================================*/
int hjpCreateFont(Hjpcontext *ctx, const char *name, const char *filename) {
    if (ctx->nfonts >= HJP_MAX_FONTS) return -1;
    HjpFont h = hjp_font_create_from_file(filename);
    if (!h) return -1;
    int idx = ctx->nfonts++;
    strncpy(ctx->fonts[idx].name, name, 63);
    ctx->fonts[idx].handle = h;
    return idx;
}
int hjpCreateFontAtIndex(Hjpcontext *ctx, const char *name, const char *filename, int fontIndex) {
    (void)fontIndex;
    return hjpCreateFont(ctx, name, filename);
}
int hjpCreateFontMem(Hjpcontext *ctx, const char *name, unsigned char *data, int ndata, int freeData) {
    if (ctx->nfonts >= HJP_MAX_FONTS) return -1;
    HjpFont h = hjp_font_create_from_mem(data, ndata);
    if (freeData) free(data);
    if (!h) return -1;
    int idx = ctx->nfonts++;
    strncpy(ctx->fonts[idx].name, name, 63);
    ctx->fonts[idx].handle = h;
    return idx;
}
int hjpCreateFontMemAtIndex(Hjpcontext *ctx, const char *name, unsigned char *data, int ndata, int freeData, int fontIndex) {
    (void)fontIndex;
    return hjpCreateFontMem(ctx, name, data, ndata, freeData);
}
int hjpFindFont(Hjpcontext *ctx, const char *name) {
    for (int i = 0; i < ctx->nfonts; i++)
        if (strcmp(ctx->fonts[i].name, name) == 0) return i;
    return -1;
}
int hjpAddFallbackFontId(Hjpcontext *ctx, int base, int fb) { (void)ctx; (void)base; (void)fb; return 1; }
int hjpAddFallbackFont(Hjpcontext *ctx, const char *b, const char *f) { (void)ctx; (void)b; (void)f; return 1; }
void hjpResetFallbackFontsId(Hjpcontext *ctx, int base) { (void)ctx; (void)base; }
void hjpResetFallbackFonts(Hjpcontext *ctx, const char *base) { (void)ctx; (void)base; }

void hjpFontSize(Hjpcontext *ctx, float size) { hjp__getState(ctx)->fontSize = size; }
void hjpFontBlur(Hjpcontext *ctx, float blur) { hjp__getState(ctx)->fontBlur = blur; }
void hjpTextLetterSpacing(Hjpcontext *ctx, float spacing) { hjp__getState(ctx)->textLetterSpacing = spacing; }
void hjpTextLineHeight(Hjpcontext *ctx, float lineHeight) { hjp__getState(ctx)->textLineHeight = lineHeight; }
void hjpTextAlign(Hjpcontext *ctx, int align) { hjp__getState(ctx)->textAlign = align; }
void hjpFontFaceId(Hjpcontext *ctx, int font) { hjp__getState(ctx)->fontId = font; }
void hjpFontFace(Hjpcontext *ctx, const char *font) {
    int id = hjpFindFont(ctx, font);
    if (id >= 0) hjp__getState(ctx)->fontId = id;
}

void hjpTextMetrics(Hjpcontext *ctx, float *ascender, float *descender, float *lineh) {
    HjpState *s = hjp__getState(ctx);
    if (s->fontId >= 0 && s->fontId < ctx->nfonts && ctx->fonts[s->fontId].handle) {
        float asc, desc, gap;
        hjp_font_metrics(ctx->fonts[s->fontId].handle, s->fontSize, &asc, &desc, &gap);
        if (ascender) *ascender = asc;
        if (descender) *descender = desc;
        if (lineh) *lineh = asc - desc + gap;
    } else {
        if (ascender) *ascender = s->fontSize * 0.8f;
        if (descender) *descender = -s->fontSize * 0.2f;
        if (lineh) *lineh = s->fontSize * 1.2f;
    }
}

float hjpTextBounds(Hjpcontext *ctx, float x, float y, const char *str, const char *end, float *bounds) {
    HjpState *s = hjp__getState(ctx);
    if (!str) return x;
    float width = 0;
    if (s->fontId >= 0 && s->fontId < ctx->nfonts && ctx->fonts[s->fontId].handle) {
        width = hjp_font_text_width(ctx->fonts[s->fontId].handle, s->fontSize, str, end);
    }
    /* Alignment */
    float asc = s->fontSize * 0.8f, desc = -s->fontSize * 0.2f;
    if (s->fontId >= 0 && s->fontId < ctx->nfonts && ctx->fonts[s->fontId].handle) {
        float gap;
        hjp_font_metrics(ctx->fonts[s->fontId].handle, s->fontSize, &asc, &desc, &gap);
    }
    float tx = x;
    if (s->textAlign & HJP_ALIGN_CENTER) tx -= width * 0.5f;
    else if (s->textAlign & HJP_ALIGN_RIGHT) tx -= width;
    float ty = y;
    if (s->textAlign & HJP_ALIGN_TOP) ty += asc;
    else if (s->textAlign & HJP_ALIGN_MIDDLE) ty += (asc + desc) * 0.5f;
    else if (s->textAlign & HJP_ALIGN_BOTTOM) ty += desc;

    if (bounds) {
        bounds[0] = tx; bounds[1] = ty - asc;
        bounds[2] = tx + width; bounds[3] = ty - desc;
    }
    return x + width;
}

float hjpText(Hjpcontext *ctx, float x, float y, const char *str, const char *end) {
    HjpState *s = hjp__getState(ctx);
    if (!str || !str[0]) return x;
    if (s->fontId < 0 || s->fontId >= ctx->nfonts) return x;
    float fontSize = s->fontSize;

    /* Calculate alignment offset */
    float totalWidth = 0;
    if (s->fontId >= 0 && s->fontId < ctx->nfonts && ctx->fonts[s->fontId].handle)
        totalWidth = hjp_font_text_width(ctx->fonts[s->fontId].handle, fontSize, str, end);
    float startX = x;
    if (s->textAlign & HJP_ALIGN_CENTER) startX -= totalWidth * 0.5f;
    else if (s->textAlign & HJP_ALIGN_RIGHT) startX -= totalWidth;

    /* Vertical alignment */
    float asc = fontSize * 0.8f, desc = -fontSize * 0.2f;
    if (s->fontId >= 0 && s->fontId < ctx->nfonts && ctx->fonts[s->fontId].handle) {
        float gap;
        hjp_font_metrics(ctx->fonts[s->fontId].handle, fontSize, &asc, &desc, &gap);
    }
    float baselineY = y;
    if (s->textAlign & HJP_ALIGN_TOP) baselineY += asc;
    else if (s->textAlign & HJP_ALIGN_MIDDLE) baselineY += (asc + desc) * 0.5f;
    else if (s->textAlign & HJP_ALIGN_BOTTOM) baselineY += desc;

    /* Render glyphs as textured triangles */
    const char *p = str;
    float cx = startX;
    int nquads = 0;
    /* Pre-count */
    const char *tp = str;
    while (*tp && (!end || tp < end)) { hjp__decodeUTF8(&tp); nquads++; }
    if (nquads == 0) return x + totalWidth;

    /* Allocate call for textured triangles */
    HjpCall *call = hjp__allocCall(ctx);
    if (!call) return x + totalWidth;
    call->type = HJP_GL_TRIANGLES;
    int vertOff = hjp__allocGLVerts(ctx, nquads * 6);
    int uniOff = hjp__allocFragUniforms(ctx, 1);
    if (vertOff < 0 || uniOff < 0) { ctx->ncalls--; return x + totalWidth; }
    call->triangleOffset = vertOff;
    call->image = ctx->fontImages[0];
    call->uniformOffset = uniOff;
    call->srcRGB = hjp__blendFactor(s->compositeOperation.srcRGB);
    call->dstRGB = hjp__blendFactor(s->compositeOperation.dstRGB);
    call->srcAlpha = hjp__blendFactor(s->compositeOperation.srcAlpha);
    call->dstAlpha = hjp__blendFactor(s->compositeOperation.dstAlpha);

    /* Setup uniform for text rendering */
    HjpFragUniforms *frag = hjp__fragUniformPtr(ctx, uniOff);
    memset(frag, 0, sizeof(*frag));
    frag->type = HJP_SHADER_IMG;
    frag->texType = 2; /* alpha texture */
    Hjpcolor fillCol = s->fill.innerColor;
    fillCol.r *= fillCol.a * s->globalAlpha;
    fillCol.g *= fillCol.a * s->globalAlpha;
    fillCol.b *= fillCol.a * s->globalAlpha;
    fillCol.a *= s->globalAlpha;
    frag->innerCol = fillCol;
    /* Scissor */
    if (s->scissor.extent[0] >= 0) {
        float invxform[6];
        hjpTransformInverse(invxform, s->scissor.xform);
        hjp__xformToMat3x3(frag->scissorMat, invxform);
        frag->scissorExt[0] = s->scissor.extent[0];
        frag->scissorExt[1] = s->scissor.extent[1];
        frag->scissorScale[0] = sqrtf(s->scissor.xform[0]*s->scissor.xform[0]+s->scissor.xform[2]*s->scissor.xform[2]) / ctx->fringeWidth;
        frag->scissorScale[1] = sqrtf(s->scissor.xform[1]*s->scissor.xform[1]+s->scissor.xform[3]*s->scissor.xform[3]) / ctx->fringeWidth;
    } else {
        frag->scissorExt[0] = 1.0f; frag->scissorExt[1] = 1.0f;
        frag->scissorScale[0] = 1.0f; frag->scissorScale[1] = 1.0f;
    }
    frag->strokeMult = 1.0f;
    frag->strokeThr = -1.0f;

    float iw = 1.0f / (float)ctx->fontAtlasW;
    float ih = 1.0f / (float)ctx->fontAtlasH;
    int triCount = 0;

    p = str;
    while (*p && (!end || p < end)) {
        uint32_t cp = hjp__decodeUTF8(&p);
        HjpGlyph *g = hjp__renderGlyph(ctx, cp, s->fontId, fontSize);
        if (!g) { cx += fontSize * 0.5f; continue; }

        float gx = cx + g->xoff;
        float gy = baselineY + g->yoff;
        float gw = (float)g->w;
        float gh = (float)g->h;
        float u0 = g->x * iw;
        float u1 = (g->x + g->w) * iw;
        /* CoreGraphics ビットマップは行0=下 (Y-up) なので V 座標を反転 */
        float v0 = (g->y + g->h) * ih;   /* アトラス下端 → 画面上端 */
        float v1 = g->y * ih;             /* アトラス上端 → 画面下端 */

        /* Two triangles = one quad (NanoVG CCW winding order) */
        Hjpvertex *v = &ctx->glverts[vertOff + triCount];
        hjp__vset(&v[0], gx,    gy,    u0, v0);       /* TL */
        hjp__vset(&v[1], gx+gw, gy+gh, u1, v1);       /* BR */
        hjp__vset(&v[2], gx+gw, gy,    u1, v0);       /* TR */
        hjp__vset(&v[3], gx,    gy,    u0, v0);       /* TL */
        hjp__vset(&v[4], gx,    gy+gh, u0, v1);       /* BL */
        hjp__vset(&v[5], gx+gw, gy+gh, u1, v1);       /* BR */
        triCount += 6;

        cx += g->advance + s->textLetterSpacing;
    }

    call->triangleCount = triCount;
    return x + totalWidth;
}

void hjpTextBox(Hjpcontext *ctx, float x, float y, float breakRowWidth, const char *str, const char *end) {
    HjpState *s = hjp__getState(ctx);
    float lineh;
    hjpTextMetrics(ctx, NULL, NULL, &lineh);
    lineh *= s->textLineHeight;

    int oldAlign = s->textAlign;
    int hAlign = s->textAlign & (HJP_ALIGN_LEFT | HJP_ALIGN_CENTER | HJP_ALIGN_RIGHT);
    int vAlign = s->textAlign & (HJP_ALIGN_TOP | HJP_ALIGN_MIDDLE | HJP_ALIGN_BOTTOM | HJP_ALIGN_BASELINE);
    s->textAlign = HJP_ALIGN_LEFT | vAlign;

    HjptextRow rows[4];
    int nrows;
    while ((nrows = hjpTextBreakLines(ctx, str, end, breakRowWidth, rows, 4)) > 0) {
        for (int i = 0; i < nrows; i++) {
            float rx = x;
            if (hAlign & HJP_ALIGN_CENTER) rx = x + breakRowWidth*0.5f - rows[i].width*0.5f;
            else if (hAlign & HJP_ALIGN_RIGHT) rx = x + breakRowWidth - rows[i].width;
            hjpText(ctx, rx, y, rows[i].start, rows[i].end);
            y += lineh;
        }
        str = rows[nrows-1].next;
    }
    s->textAlign = oldAlign;
}

void hjpTextBoxBounds(Hjpcontext *ctx, float x, float y, float breakRowWidth, const char *str, const char *end, float *bounds) {
    float lineh;
    hjpTextMetrics(ctx, NULL, NULL, &lineh);
    lineh *= hjp__getState(ctx)->textLineHeight;
    float minx = x, miny = y, maxx = x, maxy = y;
    HjptextRow rows[4];
    int nrows;
    while ((nrows = hjpTextBreakLines(ctx, str, end, breakRowWidth, rows, 4)) > 0) {
        for (int i = 0; i < nrows; i++) {
            if (rows[i].width > maxx - minx) maxx = minx + rows[i].width;
            maxy = y + lineh;
            y += lineh;
        }
        str = rows[nrows-1].next;
    }
    if (bounds) { bounds[0]=minx; bounds[1]=miny; bounds[2]=maxx; bounds[3]=maxy; }
}

int hjpTextGlyphPositions(Hjpcontext *ctx, float x, float y, const char *str, const char *end,
                          HjpglyphPosition *positions, int maxPositions) {
    (void)y;
    HjpState *s = hjp__getState(ctx);
    if (!str || maxPositions <= 0) return 0;
    const char *p = str;
    float cx = x;
    int count = 0;
    while (*p && (!end || p < end) && count < maxPositions) {
        const char *prev = p;
        uint32_t cp = hjp__decodeUTF8(&p);
        HjpGlyph *g = hjp__renderGlyph(ctx, cp, s->fontId, s->fontSize);
        float adv = g ? g->advance : s->fontSize * 0.5f;
        positions[count].str = prev;
        positions[count].x = cx;
        positions[count].minx = cx;
        positions[count].maxx = cx + adv;
        count++;
        cx += adv + s->textLetterSpacing;
    }
    return count;
}

int hjpTextBreakLines(Hjpcontext *ctx, const char *str, const char *end, float breakRowWidth,
                      HjptextRow *rows, int maxRows) {
    HjpState *s = hjp__getState(ctx);
    if (!str || maxRows <= 0) return 0;
    int nrows = 0;
    const char *lineStart = str;
    const char *p = str;
    float lineWidth = 0;
    while (*p && (!end || p < end) && nrows < maxRows) {
        const char *prev = p;
        uint32_t cp = hjp__decodeUTF8(&p);
        if (cp == '\n' || cp == '\r') {
            rows[nrows].start = lineStart;
            rows[nrows].end = prev;
            rows[nrows].next = p;
            rows[nrows].width = lineWidth;
            rows[nrows].minx = 0;
            rows[nrows].maxx = lineWidth;
            nrows++;
            lineStart = p;
            lineWidth = 0;
            if (cp == '\r' && *p == '\n') { p++; lineStart = p; }
            continue;
        }
        HjpGlyph *g = hjp__renderGlyph(ctx, cp, s->fontId, s->fontSize);
        float adv = g ? g->advance + s->textLetterSpacing : s->fontSize * 0.5f;
        if (lineWidth + adv > breakRowWidth && lineWidth > 0) {
            /* Word wrap: break at previous position */
            rows[nrows].start = lineStart;
            rows[nrows].end = prev;
            rows[nrows].next = prev;
            rows[nrows].width = lineWidth;
            rows[nrows].minx = 0;
            rows[nrows].maxx = lineWidth;
            nrows++;
            lineStart = prev;
            lineWidth = adv;
        } else {
            lineWidth += adv;
        }
    }
    /* Last line */
    if (lineStart < p && nrows < maxRows) {
        rows[nrows].start = lineStart;
        rows[nrows].end = p;
        rows[nrows].next = p;
        rows[nrows].width = lineWidth;
        rows[nrows].minx = 0;
        rows[nrows].maxx = lineWidth;
        nrows++;
    }
    return nrows;
}

/* =====================================================================
 * フレーム管理
 * ===================================================================*/
void hjpBeginFrame(Hjpcontext *ctx, float windowWidth, float windowHeight, float devicePixelRatio) {
    ctx->nstates = 0;
    hjpSave(ctx);
    hjpReset(ctx);
    hjp__setDevicePixelRatio(ctx, devicePixelRatio);
    ctx->view[0] = windowWidth;
    ctx->view[1] = windowHeight;
    /* Reset render buffers */
    ctx->ncalls = 0;
    ctx->nglpaths = 0;
    ctx->nglverts = 0;
    ctx->nuniforms = 0;
}

void hjpCancelFrame(Hjpcontext *ctx) {
    ctx->ncalls = 0;
    ctx->nglpaths = 0;
    ctx->nglverts = 0;
    ctx->nuniforms = 0;
}

void hjpEndFrame(Hjpcontext *ctx) {
    hjp__renderFlush(ctx);
}

static void hjp__setDevicePixelRatio(Hjpcontext *ctx, float ratio) {
    ctx->tessTol = 0.25f / ratio;
    ctx->distTol = 0.01f / ratio;
    ctx->fringeWidth = 1.0f / ratio;
    ctx->devicePxRatio = ratio;
}

/* =====================================================================
 * コンテキスト作成/破棄
 * ===================================================================*/
Hjpcontext *hjpCreateGL3(int flags) {
    Hjpcontext *ctx = (Hjpcontext*)calloc(1, sizeof(Hjpcontext));
    if (!ctx) return NULL;
    ctx->flags = flags;

    /* Compute UBO alignment */
    GLint align = 16;
    glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &align);
    ctx->fragSize = sizeof(HjpFragUniforms) + align - sizeof(HjpFragUniforms) % align;

    /* Create GL resources */
    if (!hjp__createShader(&ctx->shader)) { free(ctx); return NULL; }

    glGenVertexArrays(1, &ctx->vertArr);
    glGenBuffers(1, &ctx->vertBuf);
    glGenBuffers(1, &ctx->fragBuf);

    /* Setup VAO */
    glBindVertexArray(ctx->vertArr);
    glBindBuffer(GL_ARRAY_BUFFER, ctx->vertBuf);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Hjpvertex), (void*)0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Hjpvertex), (void*)(2*sizeof(float)));
    glBindVertexArray(0);

    /* Init allocs */
    ctx->commands = (float*)malloc(sizeof(float)*HJP_INIT_COMMANDS_SIZE);
    ctx->ccommands = HJP_INIT_COMMANDS_SIZE;
    ctx->points = (HjpPoint*)malloc(sizeof(HjpPoint)*HJP_INIT_POINTS_SIZE);
    ctx->cpoints = HJP_INIT_POINTS_SIZE;
    ctx->paths = (HjpPath*)malloc(sizeof(HjpPath)*HJP_INIT_PATHS_SIZE);
    ctx->cpaths = HJP_INIT_PATHS_SIZE;
    ctx->verts = (Hjpvertex*)malloc(sizeof(Hjpvertex)*HJP_INIT_VERTS_SIZE);
    ctx->cverts = HJP_INIT_VERTS_SIZE;

    /* Font atlas init */
    hjp__fontAtlasInit(ctx);

    /* macOS Core Profile: sampler2D には常に有効なテクスチャが必要。
     * image==0 のとき glBindTexture(0) するとバリデーション警告が出るため
     * 1×1 白 RGBA テクスチャをダミーとして常時バインドする。 */
    {
        static const unsigned char white[4] = {255,255,255,255};
        glGenTextures(1, &ctx->dummyTex);
        glBindTexture(GL_TEXTURE_2D, ctx->dummyTex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    hjp__setDevicePixelRatio(ctx, 1.0f);

    return ctx;
}

void hjpDeleteGL3(Hjpcontext *ctx) {
    if (!ctx) return;
    hjp__deleteShader(&ctx->shader);
    if (ctx->vertBuf) glDeleteBuffers(1, &ctx->vertBuf);
    if (ctx->vertArr) glDeleteVertexArrays(1, &ctx->vertArr);
    if (ctx->fragBuf) glDeleteBuffers(1, &ctx->fragBuf);
    if (ctx->dummyTex) glDeleteTextures(1, &ctx->dummyTex);
    /* Delete textures */
    for (int i = 0; i < ctx->ntextures; i++)
        if (ctx->textures[i].id && ctx->textures[i].tex)
            glDeleteTextures(1, &ctx->textures[i].tex);
    /* Destroy fonts */
    for (int i = 0; i < ctx->nfonts; i++)
        if (ctx->fonts[i].handle) hjp_font_destroy(ctx->fonts[i].handle);
    free(ctx->commands);
    free(ctx->points);
    free(ctx->paths);
    free(ctx->verts);
    free(ctx->calls);
    free(ctx->glpaths);
    free(ctx->glverts);
    free(ctx->uniforms);
    free(ctx->fontAtlasData);
    free(ctx);
}

/* Stub internal API for compatibility */
Hjpcontext *hjpCreateInternal(Hjpparams *params) { (void)params; return NULL; }
void hjpDeleteInternal(Hjpcontext *ctx) { (void)ctx; }
Hjpparams *hjpInternalParams(Hjpcontext *ctx) { (void)ctx; return NULL; }
void hjpDebugDumpPathCache(Hjpcontext *ctx) { (void)ctx; }
