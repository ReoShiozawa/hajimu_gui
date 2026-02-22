/**
 * hjp_hotreload.c — ホットリロード実装 (stat() mtime ポーリング)
 */
#include "hjp_hotreload.h"
#include "hjp_platform.h"
#include "hajimu_plugin.h"

#include <sys/stat.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define DEFAULT_POLL_MS 500

/* グローバルホットリローダー */
static HjpHotReloader g_reloader;
HjpHotReloader *hjp_get_global_reloader(void) { return &g_reloader; }

/* ===================================================================
 * 初期化
 * =================================================================*/
void hjp_hotreload_init(HjpHotReloader *hr, uint32_t poll_ms) {
    memset(hr, 0, sizeof(HjpHotReloader));
    hr->poll_interval_ms = (poll_ms > 0) ? poll_ms : DEFAULT_POLL_MS;
    hr->last_poll_tick   = 0;
}

/* ===================================================================
 * ファイル追加
 * =================================================================*/
int hjp_hotreload_watch(HjpHotReloader *hr, const char *path) {
    if (hr->watch_count >= HJP_HOTRELOAD_MAX_WATCHES) {
        fprintf(stderr, "[hotreload] ウォッチ上限 %d に達しました\n",
                HJP_HOTRELOAD_MAX_WATCHES);
        return -1;
    }
    int id = hr->watch_count++;
    HjpWatchedFile *wf = &hr->watches[id];
    strncpy(wf->path, path, sizeof(wf->path) - 1);
    wf->active  = true;
    wf->changed = false;

    /* 現在の mtime を取得 */
    struct stat st;
    if (stat(path, &st) == 0) {
        wf->last_mtime = st.st_mtime;
    } else {
        wf->last_mtime = 0;
        fprintf(stderr, "[hotreload] ファイルが見つかりません: %s\n", path);
    }
    printf("[hotreload] 監視開始: %s (id=%d)\n", path, id);
    return id;
}

/* ===================================================================
 * ウォッチ解除
 * =================================================================*/
void hjp_hotreload_unwatch(HjpHotReloader *hr, int watch_id) {
    if (watch_id < 0 || watch_id >= hr->watch_count) return;
    hr->watches[watch_id].active = false;
}

/* ===================================================================
 * ポーリング実行
 * =================================================================*/
int hjp_hotreload_poll(HjpHotReloader *hr, uint32_t current_tick) {
    /* ポーリング間隔チェック */
    if (current_tick - hr->last_poll_tick < hr->poll_interval_ms) {
        return 0;
    }
    hr->last_poll_tick = current_tick;

    int changed_count = 0;
    for (int i = 0; i < hr->watch_count; i++) {
        HjpWatchedFile *wf = &hr->watches[i];
        if (!wf->active) continue;

        struct stat st;
        if (stat(wf->path, &st) != 0) continue;

        if (st.st_mtime != wf->last_mtime) {
            wf->last_mtime = st.st_mtime;
            wf->changed    = true;
            changed_count++;
            hr->total_reloads++;
            printf("[hotreload] 変更検知: %s\n", wf->path);
            if (hr->on_change) {
                hr->on_change(wf->path, hr->user_data);
            }
        }
    }
    return changed_count;
}

/* ===================================================================
 * 変更確認 / リセット
 * =================================================================*/
bool hjp_hotreload_changed(HjpHotReloader *hr, int watch_id) {
    if (watch_id < 0 || watch_id >= hr->watch_count) return false;
    return hr->watches[watch_id].changed;
}

void hjp_hotreload_reset(HjpHotReloader *hr, int watch_id) {
    if (watch_id < 0 || watch_id >= hr->watch_count) return;
    hr->watches[watch_id].changed = false;
}

void hjp_hotreload_reset_all(HjpHotReloader *hr) {
    for (int i = 0; i < hr->watch_count; i++) {
        hr->watches[i].changed = false;
    }
}

/* ===================================================================
 * UIステート シリアライズ (簡易実装)
 * =================================================================*/
void hjp_hotreload_save_state(const char *state, char *buf, int buf_size) {
    if (!state || !buf || buf_size <= 0) return;
    strncpy(buf, state, buf_size - 1);
    buf[buf_size - 1] = '\0';
}

void hjp_hotreload_restore_state(const char *buf, char *state_out, int state_size) {
    if (!buf || !state_out || state_size <= 0) return;
    strncpy(state_out, buf, state_size - 1);
    state_out[state_size - 1] = '\0';
}

/* ===================================================================
 * はじむ API
 * =================================================================*/

/* ホットリロード開始(ポーリング間隔ms=500) */
Value fn_ホットリロード開始(int argc, Value *argv) {
    uint32_t interval = (argc >= 1) ? (uint32_t)argv[0].number : 500;
    hjp_hotreload_init(&g_reloader, interval);
    return hajimu_null();
}

/* ホットリロード監視(パス文字列) → ウォッチID */
Value fn_ホットリロード監視(int argc, Value *argv) {
    if (argc < 1) return hajimu_number(-1);
    const char *path = (argv[0].type == VALUE_STRING && argv[0].string.data)
        ? argv[0].string.data : "";
    int id = hjp_hotreload_watch(&g_reloader, path);
    return hajimu_number(id);
}

/* ホットリロード確認(ウォッチID) → 真偽値 */
Value fn_ホットリロード確認(int argc, Value *argv) {
    if (argc < 1) return hajimu_bool(false);
    int id = (int)argv[0].number;
    bool changed = hjp_hotreload_changed(&g_reloader, id);
    if (changed) hjp_hotreload_reset(&g_reloader, id);
    return hajimu_bool(changed);
}

/* ホットリロードリセット() */
Value fn_ホットリロードリセット(int argc, Value *argv) {
    (void)argc; (void)argv;
    hjp_hotreload_reset_all(&g_reloader);
    return hajimu_null();
}

/* ホットリロード回数() → 累計リロード数 */
Value fn_ホットリロード回数(int argc, Value *argv) {
    (void)argc; (void)argv;
    return hajimu_number(g_reloader.total_reloads);
}
