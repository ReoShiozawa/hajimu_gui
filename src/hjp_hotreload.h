/**
 * hjp_hotreload.h — ホットリロード (ファイル変更検知)
 *
 * 方式: stat() mtime ポーリング (クロスプラットフォーム対応)
 * macOS kqueue / Linux inotify は不要 — stat() のみで実装
 *
 * 用途:
 *   - .jp スクリプトファイルの変更を検知して自動リロード
 *   - テーマ CSS / 設定 JSON の即時反映
 *   - UIコンポーネント定義の変更でツリー再構築
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <time.h>

#define HJP_HOTRELOAD_MAX_WATCHES 32

/* ===================================================================
 * ウォッチエントリ
 * =================================================================*/
typedef struct HjpWatchedFile {
    char   path[1024];
    time_t last_mtime;
    bool   changed;
    bool   active;
} HjpWatchedFile;

/* 変更コールバック型 */
typedef void (*HjpReloadCallback)(const char *path, void *user_data);

/* ===================================================================
 * ホットリローダー本体
 * =================================================================*/
typedef struct HjpHotReloader {
    HjpWatchedFile   watches[HJP_HOTRELOAD_MAX_WATCHES];
    int              watch_count;
    HjpReloadCallback on_change;
    void             *user_data;
    uint32_t         poll_interval_ms; /* ポーリング間隔 (デフォルト 500ms) */
    uint32_t         last_poll_tick;
    int              total_reloads;    /* 累計リロード回数 */
} HjpHotReloader;

/* ===================================================================
 * C API
 * =================================================================*/
#ifdef __cplusplus
extern "C" {
#endif

/**
 * 初期化
 * @param poll_ms  ポーリング間隔 ms (0 → デフォルト 500ms)
 */
void hjp_hotreload_init(HjpHotReloader *hr, uint32_t poll_ms);

/**
 * ファイルを監視対象に追加
 * @return ウォッチ ID (0 以上), 失敗時 -1
 */
int hjp_hotreload_watch(HjpHotReloader *hr, const char *path);

/**
 * ウォッチ解除
 */
void hjp_hotreload_unwatch(HjpHotReloader *hr, int watch_id);

/**
 * ポーリング実行 (メインループから定期的に呼ぶ)
 * 変更があればコールバック呼び出し
 * @return 変更されたファイル数
 */
int hjp_hotreload_poll(HjpHotReloader *hr, uint32_t current_tick);

/**
 * 特定ファイルが変更されたか確認
 */
bool hjp_hotreload_changed(HjpHotReloader *hr, int watch_id);

/**
 * 変更フラグをリセット
 */
void hjp_hotreload_reset(HjpHotReloader *hr, int watch_id);

/**
 * 全ファイルの変更フラグをリセット
 */
void hjp_hotreload_reset_all(HjpHotReloader *hr);

/**
 * UIステート スナップショット (文字列シリアライズ)
 * バッファに key=value 形式で保存
 */
void hjp_hotreload_save_state(const char *state, char *buf, int buf_size);

/**
 * UIステート 復元
 */
void hjp_hotreload_restore_state(const char *buf, char *state_out, int state_size);

/* ===================================================================
 * はじむ API
 * =================================================================*/
struct Value; typedef struct Value Value;
Value fn_ホットリロード開始(int argc, Value *argv);
Value fn_ホットリロード監視(int argc, Value *argv);
Value fn_ホットリロード確認(int argc, Value *argv);
Value fn_ホットリロードリセット(int argc, Value *argv);
Value fn_ホットリロード回数(int argc, Value *argv);

#ifdef __cplusplus
}
#endif
