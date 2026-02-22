/**
 * hjp_frame.h — フレームスケジューラ / VSync / 描画コールバッチング
 *
 * 機能:
 *   - VSync 同期 (hjp_gl_set_swap_interval)
 *   - ターゲット FPS 管理 (60 / 30 / 無制限)
 *   - フレーム時間計測とローリング平均 FPS
 *   - 描画コールバッチ収集 (Draw Call キュー)
 *   - アダプティブフレームスロットリング (CPU 節約)
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

/* ===================================================================
 * 描画コールタイプ
 * =================================================================*/
typedef enum {
    HJP_DRAW_NONE = 0,
    HJP_DRAW_RECT,
    HJP_DRAW_ROUND_RECT,
    HJP_DRAW_TEXT,
    HJP_DRAW_IMAGE,
    HJP_DRAW_CIRCLE,
    HJP_DRAW_LINE,
    HJP_DRAW_CLIP_PUSH,
    HJP_DRAW_CLIP_POP,
} HjpDrawCallType;

/* バッチ単位の描画コール */
typedef struct HjpDrawCall {
    HjpDrawCallType type;
    float     x, y, w, h;
    float     radius;
    uint32_t  color;          /* RGBA packed */
    float     stroke_width;
    int       align;
    char      text[128];
    int       image_id;
    float     opacity;
} HjpDrawCall;

#define HJP_DRAW_QUEUE_SIZE 4096

/* ===================================================================
 * フレームスケジューラ本体
 * =================================================================*/
typedef struct HjpFrameScheduler {
    /* VSync / FPS 設定 */
    bool     vsync;
    int      target_fps;        /* 0 = 無制限 */
    float    target_frame_ms;   /* 1000.0 / target_fps */

    /* タイミング */
    uint32_t frame_start_tick;  /* 現フレーム開始 tick (ms) */
    uint32_t last_tick;
    float    frame_time_ms;     /* 最後のフレーム所要時間 */
    float    cpu_time_ms;       /* CPU 側処理時間 */

    /* FPS 計測 */
    float    fps;               /* ローリング平均 */
    float    frame_hist[128];   /* フレーム時間履歴 */
    int      frame_idx;
    uint32_t total_frames;

    /* 描画コールキュー */
    HjpDrawCall draw_queue[HJP_DRAW_QUEUE_SIZE];
    int          draw_queue_used;
    int          draw_calls_last; /* 前フレームのコール数 */

    /* アダプティブスロットリング */
    bool     frame_requested;   /* 再描画要求フラグ */
    int      idle_frames;       /* 連続アイドルフレーム数 */
    int      idle_threshold;    /* この値以上でスリープ (デフォルト 10) */

} HjpFrameScheduler;

/* ===================================================================
 * C API
 * =================================================================*/
#ifdef __cplusplus
extern "C" {
#endif

/**
 * スケジューラ初期化
 * @param target_fps  目標FPS (0=無制限, 60 推奨)
 * @param vsync       VSync 有効化
 */
void hjp_frame_init(HjpFrameScheduler *sched, int target_fps, bool vsync);

/**
 * フレーム開始 — タイムスタンプ記録
 * @return 現在フレームの開始 tick
 */
uint32_t hjp_frame_begin(HjpFrameScheduler *sched);

/**
 * フレーム終了 — FPS 計測, スリープ調整
 * @param sched スケジューラ
 */
void hjp_frame_end(HjpFrameScheduler *sched);

/**
 * VSync 待機 (hjp_gl_set_swap_interval によるドライバレベル同期)
 */
void hjp_frame_wait_vsync(HjpFrameScheduler *sched);

/**
 * 再描画要求フラグを立てる
 */
void hjp_frame_request_redraw(HjpFrameScheduler *sched);

/**
 * アイドル判定: アイドル中なら true (描画スキップ可)
 */
bool hjp_frame_is_idle(const HjpFrameScheduler *sched);

/**
 * 描画コールをキューに追加
 */
HjpDrawCall *hjp_frame_draw_alloc(HjpFrameScheduler *sched);

/**
 * キューをフラッシュ (実際の hjp_render 呼び出し)
 */
void hjp_frame_flush(HjpFrameScheduler *sched, void *vg);

/**
 * デバッグ用サマリ表示
 */
void hjp_frame_debug_print(const HjpFrameScheduler *sched);

/* ===================================================================
 * はじむ API (fn_* ラッパー) — hajimu_gui.c から登録
 * =================================================================*/
struct Value; typedef struct Value Value;
Value fn_フレーム設定(int argc, Value *argv);
Value fn_フレームレート取得(int argc, Value *argv);
Value fn_フレーム時間(int argc, Value *argv);
Value fn_フレーム要求(int argc, Value *argv);
Value fn_描画コール数(int argc, Value *argv);

#ifdef __cplusplus
}
#endif
