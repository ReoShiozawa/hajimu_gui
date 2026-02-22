/**
 * hjp_frame.c — フレームスケジューラ実装
 */
#include "hjp_frame.h"
#include "hjp_platform.h"
#include "hjp_render.h"
#include "hajimu_plugin.h"

#include <string.h>
#include <stdio.h>
#include <math.h>

/* グローバルスケジューラ (シングルウィンドウ想定) */
static HjpFrameScheduler g_sched;
HjpFrameScheduler *hjp_get_global_scheduler(void) { return &g_sched; }

/* ===================================================================
 * 初期化
 * =================================================================*/
void hjp_frame_init(HjpFrameScheduler *sched, int target_fps, bool vsync) {
    memset(sched, 0, sizeof(HjpFrameScheduler));
    sched->target_fps     = target_fps;
    sched->target_frame_ms = (target_fps > 0) ? (1000.0f / (float)target_fps) : 0.0f;
    sched->vsync          = vsync;
    sched->idle_threshold = 10;
    sched->frame_requested = true; /* 初回は必ず描画 */

    if (vsync) {
        hjp_gl_set_swap_interval(1);
    } else {
        hjp_gl_set_swap_interval(0);
    }
}

/* ===================================================================
 * フレーム開始
 * =================================================================*/
uint32_t hjp_frame_begin(HjpFrameScheduler *sched) {
    sched->frame_start_tick = hjp_get_ticks();
    sched->draw_queue_used  = 0;
    sched->frame_requested  = false;
    return sched->frame_start_tick;
}

/* ===================================================================
 * フレーム終了 + FPS 計測 + スリープ
 * =================================================================*/
void hjp_frame_end(HjpFrameScheduler *sched) {
    uint32_t now = hjp_get_ticks();
    float elapsed = (float)(now - sched->frame_start_tick);

    sched->frame_time_ms = elapsed;

    /* フレーム履歴更新 */
    sched->frame_hist[sched->frame_idx % 128] = elapsed;
    sched->frame_idx++;
    sched->total_frames++;

    /* ローリング平均 FPS 計算 (最新 64 フレーム) */
    int samples = (sched->frame_idx < 64) ? sched->frame_idx : 64;
    float sum = 0;
    int base = sched->frame_idx - samples;
    for (int i = base; i < base + samples; i++) {
        sum += sched->frame_hist[i % 128];
    }
    float avg_ms = (samples > 0) ? sum / samples : 16.67f;
    sched->fps = (avg_ms > 0) ? 1000.0f / avg_ms : 0.0f;

    /* ターゲット FPS によるスリープ (VSync OFF 時) */
    if (!sched->vsync && sched->target_fps > 0) {
        float wait = sched->target_frame_ms - elapsed;
        if (wait > 1.0f) {
            hjp_delay((uint32_t)wait);
        }
    }

    /* アイドル管理 */
    if (!sched->frame_requested) {
        sched->idle_frames++;
    } else {
        sched->idle_frames = 0;
    }

    sched->last_tick = hjp_get_ticks();
}

/* ===================================================================
 * VSync 待機
 * =================================================================*/
void hjp_frame_wait_vsync(HjpFrameScheduler *sched) {
    if (sched->vsync) {
        hjp_gl_set_swap_interval(1);
    }
}

/* ===================================================================
 * 再描画要求
 * =================================================================*/
void hjp_frame_request_redraw(HjpFrameScheduler *sched) {
    sched->frame_requested = true;
    sched->idle_frames = 0;
}

/* ===================================================================
 * アイドル判定
 * =================================================================*/
bool hjp_frame_is_idle(const HjpFrameScheduler *sched) {
    return (!sched->frame_requested)
        && (sched->idle_frames >= sched->idle_threshold);
}

/* ===================================================================
 * 描画コール確保
 * =================================================================*/
HjpDrawCall *hjp_frame_draw_alloc(HjpFrameScheduler *sched) {
    if (sched->draw_queue_used >= HJP_DRAW_QUEUE_SIZE) {
        fprintf(stderr, "[hjp_frame] 描画キュー上限 %d に達しました\n",
                HJP_DRAW_QUEUE_SIZE);
        return NULL;
    }
    HjpDrawCall *dc = &sched->draw_queue[sched->draw_queue_used++];
    memset(dc, 0, sizeof(HjpDrawCall));
    return dc;
}

/* ===================================================================
 * キューフラッシュ
 * =================================================================*/
void hjp_frame_flush(HjpFrameScheduler *sched, void *vg) {
    Hjpcontext *ctx = (Hjpcontext *)vg;
    sched->draw_calls_last = sched->draw_queue_used;

    for (int i = 0; i < sched->draw_queue_used; i++) {
        HjpDrawCall *dc = &sched->draw_queue[i];
        switch (dc->type) {
            case HJP_DRAW_RECT: {
                Hjpcolor c = hjpRGBA(
                    (dc->color >> 24) & 0xFF,
                    (dc->color >> 16) & 0xFF,
                    (dc->color >>  8) & 0xFF,
                    (dc->color      ) & 0xFF);
                hjpFillColor(ctx, c);
                hjpBeginPath(ctx);
                hjpRect(ctx, dc->x, dc->y, dc->w, dc->h);
                hjpFill(ctx);
                break;
            }
            case HJP_DRAW_ROUND_RECT: {
                Hjpcolor c = hjpRGBA(
                    (dc->color >> 24) & 0xFF,
                    (dc->color >> 16) & 0xFF,
                    (dc->color >>  8) & 0xFF,
                    (dc->color      ) & 0xFF);
                hjpFillColor(ctx, c);
                hjpBeginPath(ctx);
                hjpRoundedRect(ctx, dc->x, dc->y, dc->w, dc->h, dc->radius);
                hjpFill(ctx);
                break;
            }
            case HJP_DRAW_TEXT: {
                Hjpcolor c = hjpRGBA(
                    (dc->color >> 24) & 0xFF,
                    (dc->color >> 16) & 0xFF,
                    (dc->color >>  8) & 0xFF,
                    (dc->color      ) & 0xFF);
                hjpFillColor(ctx, c);
                hjpFontSize(ctx, dc->radius); /* radius フィールドをフォントサイズとして流用 */
                hjpTextAlign(ctx, dc->align);
                hjpText(ctx, dc->x, dc->y, dc->text, NULL);
                break;
            }
            case HJP_DRAW_CIRCLE: {
                Hjpcolor c = hjpRGBA(
                    (dc->color >> 24) & 0xFF,
                    (dc->color >> 16) & 0xFF,
                    (dc->color >>  8) & 0xFF,
                    (dc->color      ) & 0xFF);
                hjpFillColor(ctx, c);
                hjpBeginPath(ctx);
                hjpCircle(ctx, dc->x, dc->y, dc->radius);
                hjpFill(ctx);
                break;
            }
            case HJP_DRAW_LINE: {
                Hjpcolor c = hjpRGBA(
                    (dc->color >> 24) & 0xFF,
                    (dc->color >> 16) & 0xFF,
                    (dc->color >>  8) & 0xFF,
                    (dc->color      ) & 0xFF);
                hjpStrokeColor(ctx, c);
                hjpStrokeWidth(ctx, dc->stroke_width);
                hjpBeginPath(ctx);
                hjpMoveTo(ctx, dc->x, dc->y);
                hjpLineTo(ctx, dc->w, dc->h);
                hjpStroke(ctx);
                break;
            }
            default: break;
        }
    }
    sched->draw_queue_used = 0;
}

/* ===================================================================
 * デバッグ表示
 * =================================================================*/
void hjp_frame_debug_print(const HjpFrameScheduler *sched) {
    printf("[hjp_frame] FPS=%.1f フレーム時間=%.2fms 描画コール=%d アイドル=%d\n",
        sched->fps, sched->frame_time_ms,
        sched->draw_calls_last, sched->idle_frames);
}

/* ===================================================================
 * はじむ API
 * =================================================================*/
Value fn_フレーム設定(int argc, Value *argv) {
    if (argc < 1) return hajimu_null();
    int fps = (int)argv[0].number;
    bool vsync = (argc >= 2) ? argv[1].boolean : true;
    hjp_frame_init(&g_sched, fps, vsync);
    return hajimu_null();
}

Value fn_フレームレート取得(int argc, Value *argv) {
    (void)argc; (void)argv;
    return hajimu_number(g_sched.fps);
}

Value fn_フレーム時間(int argc, Value *argv) {
    (void)argc; (void)argv;
    return hajimu_number(g_sched.frame_time_ms);
}

Value fn_フレーム要求(int argc, Value *argv) {
    (void)argc; (void)argv;
    hjp_frame_request_redraw(&g_sched);
    return hajimu_null();
}

Value fn_描画コール数(int argc, Value *argv) {
    (void)argc; (void)argv;
    return hajimu_number(g_sched.draw_calls_last);
}
