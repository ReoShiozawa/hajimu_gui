/**
 * hjp_lifecycle.h — ウィジェットライフサイクル管理 / 参照カウント
 *
 * 機能:
 *   - 参照カウント (retain/release)
 *   - リソース解放コールバック登録
 *   - テクスチャ / 音声 / フォント などのウィジェット紐付け
 *   - ウィジェット破棄時の自動クリーンアップ
 *   - 弱参照サポート (ダングリング防止)
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>

#define HJP_LIFECYCLE_MAX_HANDLES 256
#define HJP_LIFECYCLE_MAX_CLEANUPS 8

/* ===================================================================
 * リソース解放コールバック
 * =================================================================*/
typedef void (*HjpCleanupFn)(uint32_t id, void *user_data);

/* ===================================================================
 * ウィジェットハンドル
 * =================================================================*/
typedef struct HjpWidgetHandle {
    uint32_t      id;
    int           ref_count;
    bool          valid;                          /* false = 解放済み */
    char          debug_name[64];
    HjpCleanupFn  cleanup_fns[HJP_LIFECYCLE_MAX_CLEANUPS];
    void         *cleanup_data[HJP_LIFECYCLE_MAX_CLEANUPS];
    int           cleanup_count;
} HjpWidgetHandle;

/* ===================================================================
 * ライフサイクルマネージャ
 * =================================================================*/
typedef struct HjpLifecycleManager {
    HjpWidgetHandle handles[HJP_LIFECYCLE_MAX_HANDLES];
    int             count;
    uint32_t        next_id;
    int             total_retained;
    int             total_released;
} HjpLifecycleManager;

/* ===================================================================
 * インライン実装 (ヘッダのみ)
 * =================================================================*/
static HjpLifecycleManager g_lifecycle = {0};

static inline uint32_t hjp_widget_create(const char *debug_name) {
    if (g_lifecycle.count >= HJP_LIFECYCLE_MAX_HANDLES) {
        fprintf(stderr, "[lifecycle] ハンドル上限 %d に達しました\n",
                HJP_LIFECYCLE_MAX_HANDLES);
        return 0;
    }
    HjpWidgetHandle *h = &g_lifecycle.handles[g_lifecycle.count++];
    memset(h, 0, sizeof(HjpWidgetHandle));
    h->id         = ++g_lifecycle.next_id;
    h->ref_count  = 1;
    h->valid      = true;
    if (debug_name) {
        strncpy(h->debug_name, debug_name, sizeof(h->debug_name) - 1);
    }
    g_lifecycle.total_retained++;
    return h->id;
}

static inline HjpWidgetHandle *hjp_widget_find(uint32_t id) {
    for (int i = 0; i < g_lifecycle.count; i++) {
        if (g_lifecycle.handles[i].id == id && g_lifecycle.handles[i].valid) {
            return &g_lifecycle.handles[i];
        }
    }
    return NULL;
}

static inline void hjp_widget_retain(uint32_t id) {
    HjpWidgetHandle *h = hjp_widget_find(id);
    if (h) {
        h->ref_count++;
        g_lifecycle.total_retained++;
    }
}

static inline void hjp_widget_release(uint32_t id) {
    HjpWidgetHandle *h = hjp_widget_find(id);
    if (!h) return;
    h->ref_count--;
    g_lifecycle.total_released++;
    if (h->ref_count <= 0) {
        /* クリーンアップコールバックを全て実行 */
        for (int i = 0; i < h->cleanup_count; i++) {
            if (h->cleanup_fns[i]) {
                h->cleanup_fns[i](id, h->cleanup_data[i]);
            }
        }
        h->valid     = false;
        h->ref_count = 0;
    }
}

static inline void hjp_widget_add_cleanup(uint32_t id, HjpCleanupFn fn, void *user_data) {
    HjpWidgetHandle *h = hjp_widget_find(id);
    if (!h || h->cleanup_count >= HJP_LIFECYCLE_MAX_CLEANUPS) return;
    h->cleanup_fns[h->cleanup_count]  = fn;
    h->cleanup_data[h->cleanup_count] = user_data;
    h->cleanup_count++;
}

static inline void hjp_widget_cleanup_all(void) {
    for (int i = 0; i < g_lifecycle.count; i++) {
        HjpWidgetHandle *h = &g_lifecycle.handles[i];
        if (!h->valid) continue;
        for (int j = 0; j < h->cleanup_count; j++) {
            if (h->cleanup_fns[j]) {
                h->cleanup_fns[j](h->id, h->cleanup_data[j]);
            }
        }
        h->valid = false;
    }
    g_lifecycle.count = 0;
}

static inline int hjp_widget_ref_count(uint32_t id) {
    HjpWidgetHandle *h = hjp_widget_find(id);
    return h ? h->ref_count : 0;
}

static inline void hjp_lifecycle_print_stats(void) {
    printf("[lifecycle] ハンドル数=%d 累計retain=%d 累計release=%d\n",
        g_lifecycle.count,
        g_lifecycle.total_retained,
        g_lifecycle.total_released);
}

/* ===================================================================
 * はじむ API (ヘッダ内宣言のみ、登録は hajimu_gui.c)
 * =================================================================*/
struct Value; typedef struct Value Value;

static inline Value fn_ウィジェット作成(int argc, Value *argv);
static inline Value fn_ウィジェット保持(int argc, Value *argv);
static inline Value fn_ウィジェット解放(int argc, Value *argv);
static inline Value fn_ウィジェット参照数(int argc, Value *argv);

/* hajimu_plugin.h に依存するためインライン実装は hajimu_gui.c 側で行う */
