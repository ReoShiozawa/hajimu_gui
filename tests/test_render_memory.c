#include "../src/hjp_render.c"
#include <assert.h>

int main(void) {
    Hjpcontext *ctx = calloc(1, sizeof(*ctx));
    assert(ctx != NULL);
    ctx->flags = HJP_ANTIALIAS;
    /* 同じ物理サイズでも、論理サイズ・DPIの違うキャッシュは共有しない。 */
    ctx->nglyphs = 2;
    ctx->glyphCache[0] = (HjpGlyph){.codepoint = 'A', .fontId = 0, .size = 18, .raster_ratio = 2};
    ctx->glyphCache[1] = (HjpGlyph){.codepoint = 'A', .fontId = 0, .size = 36, .raster_ratio = 1};
    ctx->devicePxRatio = 2;
    assert(hjp__findGlyph(ctx, 'A', 0, 18) == &ctx->glyphCache[0]);
    assert(hjp__findGlyph(ctx, 'A', 0, 36) == NULL);
    ctx->devicePxRatio = 1;
    assert(hjp__findGlyph(ctx, 'A', 0, 36) == &ctx->glyphCache[1]);
    ctx->nglyphs = 0;
    int capacity = 0;
    for (int frame = 0; frame < 10000; frame++) {
        hjpBeginFrame(ctx, 640, 480, 1);
        assert(ctx->nverts == 0);
        hjpBeginPath(ctx);
        // 複数パスを用い、小さい初期領域からの拡張も検証する。
        for (int i = 0; i < 128; i++) hjpRect(ctx, (float)i, 0, 20, 20);
        hjp__flattenPaths(ctx);
        hjp__expandFill(ctx, 1, HJP_MITER, 10);
        assert(ctx->npaths == 128);
        for (int i = 0; i < ctx->npaths; i++) {
            HjpPath *p = &ctx->paths[i];
            assert(p->fill >= ctx->verts && p->fill + p->nfill <= ctx->verts + ctx->nverts);
            assert(p->stroke >= ctx->verts && p->stroke + p->nstroke <= ctx->verts + ctx->nverts);
            assert(isfinite(p->fill[0].x));
        }
        if (frame == 0) capacity = ctx->cverts;
        assert(ctx->cverts == capacity);
        hjp__expandStroke(ctx, 2, 1, HJP_BUTT, HJP_MITER, 10);
        for (int i = 0; i < ctx->npaths; i++) {
            HjpPath *p = &ctx->paths[i];
            assert(p->stroke >= ctx->verts && p->stroke + p->nstroke <= ctx->verts + ctx->nverts);
            assert(isfinite(p->stroke[0].x));
        }
        assert(ctx->cverts == capacity);
        hjpCancelFrame(ctx);
        assert(ctx->nverts == 0);
    }
    printf("render memory passed: 10000 frames, 128 paths/frame, capacity %d vertices\n", capacity);
    free(ctx->verts);
    free(ctx->points);
    free(ctx->paths);
    free(ctx->commands);
    free(ctx);
    return 0;
}
