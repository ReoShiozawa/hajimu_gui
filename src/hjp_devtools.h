/**
 * hjp_devtools.h — 開発者ツール オーバーレイ
 *
 * 機能:
 *   - F12 でオーバーレイ ON/OFF
 *   - FPS グラフ (128フレーム履歴)
 *   - ウィジェットツリーインスペクタ (マウスホバーで強調)
 *   - ダーティ矩形の可視化 (再描画領域を赤枠で表示)
 *   - レイアウト境界線の表示
 *   - パフォーマンスプロファイラ (CPU / GPU 時間)
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "hjp_vnode.h"

#define HJP_DT_FPS_HIST  128
#define HJP_DT_LOG_LINES 32
#define HJP_DT_LOG_SIZE  256

/* ===================================================================
 * DevTools 本体
 * =================================================================*/
typedef struct HjpDevTools {
    /* 可視化オプション */
    bool visible;
    bool show_fps;
    bool show_widget_tree;
    bool show_dirty_rects;
    bool show_layout_bounds;
    bool show_perf;

    /* FPS グラフ */
    float fps_hist[HJP_DT_FPS_HIST];
    int   fps_idx;
    float fps_min, fps_max, fps_avg;

    /* インスペクタ: 選択ノード */
    uint32_t inspect_key;
    float    inspect_mx, inspect_my; /* マウス座標 */
    char     inspect_info[512];

    /* ログコンソール */
    char log_lines[HJP_DT_LOG_LINES][HJP_DT_LOG_SIZE];
    int  log_count;
    int  log_head;

    /* パフォーマンス */
    float cpu_time_ms;
    float gpu_time_ms;
    int   draw_calls;
    int   nodes_rendered;
    int   nodes_skipped;

    /* ウィンドウサイズ (描画位置計算用) */
    float win_w, win_h;

    /* パネル位置/サイズ */
    float panel_x, panel_y, panel_w, panel_h;
    bool  panel_visible;

} HjpDevTools;

/* ===================================================================
 * C API
 * =================================================================*/
#ifdef __cplusplus
extern "C" {
#endif

void hjp_devtools_init(HjpDevTools *dt, float win_w, float win_h);

/** F12 相当: オーバーレイ表示切り替え */
void hjp_devtools_toggle(HjpDevTools *dt);

/** 設定変更 */
void hjp_devtools_set_option(HjpDevTools *dt, const char *option, bool value);

/** フレーム統計更新 */
void hjp_devtools_update(HjpDevTools *dt, float fps, float cpu_ms, int draw_calls,
                          int nodes_rendered, int nodes_skipped);

/** マウス位置更新 (インスペクタ用) */
void hjp_devtools_mouse(HjpDevTools *dt, float mx, float my);

/** VNodeツリーからインスペクト情報を収集 */
void hjp_devtools_inspect_tree(HjpDevTools *dt, const VNode *root);

/** オーバーレイ描画 (メインフレーム描画後に呼ぶ) */
void hjp_devtools_render(HjpDevTools *dt, void *vg, int font_id);

/** ログ追加 */
void hjp_devtools_log(HjpDevTools *dt, const char *fmt, ...);

/* はじむ API */
struct Value; typedef struct Value Value;
Value fn_DevTools開始(int argc, Value *argv);
Value fn_DevTools表示(int argc, Value *argv);
Value fn_DevTools非表示(int argc, Value *argv);
Value fn_DevToolsログ(int argc, Value *argv);
Value fn_DevToolsオプション(int argc, Value *argv);

#ifdef __cplusplus
}
#endif
