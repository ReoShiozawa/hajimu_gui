/**
 * hjp_devtools.c — 開発者ツール オーバーレイ実装
 */
#include "hjp_devtools.h"
#include "hjp_render.h"
#include "hjp_platform.h"
#include "hajimu_plugin.h"

#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <math.h>

static HjpDevTools g_devtools;
HjpDevTools *hjp_get_global_devtools(void) { return &g_devtools; }

/* ===================================================================
 * 初期化
 * =================================================================*/
void hjp_devtools_init(HjpDevTools *dt, float win_w, float win_h) {
    memset(dt, 0, sizeof(HjpDevTools));
    dt->win_w        = win_w;
    dt->win_h        = win_h;
    dt->show_fps     = true;
    dt->show_perf    = true;
    /* パネルはデフォルト右上 */
    dt->panel_w = 320.0f;
    dt->panel_h = 480.0f;
    dt->panel_x = win_w - dt->panel_w - 8.0f;
    dt->panel_y = 8.0f;
    dt->fps_min = 9999.0f;
    dt->fps_max = 0.0f;
}

/* ===================================================================
 * ON/OFF
 * =================================================================*/
void hjp_devtools_toggle(HjpDevTools *dt) {
    dt->visible = !dt->visible;
    dt->panel_visible = dt->visible;
}

void hjp_devtools_set_option(HjpDevTools *dt, const char *option, bool value) {
    if (!option) return;
    if (strcmp(option, "fps")          == 0) dt->show_fps          = value;
    if (strcmp(option, "tree")         == 0) dt->show_widget_tree  = value;
    if (strcmp(option, "dirty")        == 0) dt->show_dirty_rects  = value;
    if (strcmp(option, "layout")       == 0) dt->show_layout_bounds= value;
    if (strcmp(option, "perf")         == 0) dt->show_perf         = value;
}

/* ===================================================================
 * 統計更新
 * =================================================================*/
void hjp_devtools_update(HjpDevTools *dt, float fps, float cpu_ms, int draw_calls,
                          int nodes_rendered, int nodes_skipped) {
    dt->fps_hist[dt->fps_idx % HJP_DT_FPS_HIST] = fps;
    dt->fps_idx++;

    /* min/max/avg */
    float sum = 0;
    float mn = 9999, mx = 0;
    int samples = (dt->fps_idx < HJP_DT_FPS_HIST) ? dt->fps_idx : HJP_DT_FPS_HIST;
    for (int i = 0; i < samples; i++) {
        float v = dt->fps_hist[i];
        sum += v;
        if (v < mn) mn = v;
        if (v > mx) mx = v;
    }
    dt->fps_min       = mn;
    dt->fps_max       = mx;
    dt->fps_avg       = (samples > 0) ? sum / samples : 0;
    dt->cpu_time_ms   = cpu_ms;
    dt->draw_calls    = draw_calls;
    dt->nodes_rendered= nodes_rendered;
    dt->nodes_skipped = nodes_skipped;
}

/* ===================================================================
 * マウス更新
 * =================================================================*/
void hjp_devtools_mouse(HjpDevTools *dt, float mx, float my) {
    dt->inspect_mx = mx;
    dt->inspect_my = my;
}

/* ===================================================================
 * ツリーインスペクト (再帰的にマウス位置とヒットテスト)
 * =================================================================*/
static bool inspect_hit(const VNode *node, float mx, float my, HjpDevTools *dt) {
    if (!node) return false;
    if (mx >= node->calc_x && mx <= node->calc_x + node->calc_w &&
        my >= node->calc_y && my <= node->calc_y + node->calc_h) {
        snprintf(dt->inspect_info, sizeof(dt->inspect_info),
            "key=%u type=%d\n"
            "位置: (%.0f, %.0f)\n"
            "サイズ: %.0fx%.0f\n"
            "テキスト: %s\n"
            "幅=%g 高さ=%g\n"
            "dirty=%d hash=%08x",
            node->key, (int)node->type,
            node->calc_x, node->calc_y,
            node->calc_w, node->calc_h,
            node->text[0] ? node->text : "(テキストなし)",
            node->style.width, node->style.height,
            (int)node->dirty, node->content_hash);
        dt->inspect_key = node->key;
        return true;
    }
    for (int i = node->child_count - 1; i >= 0; i--) {
        if (inspect_hit(node->children[i], mx, my, dt)) return true;
    }
    return false;
}

void hjp_devtools_inspect_tree(HjpDevTools *dt, const VNode *root) {
    if (!dt->show_widget_tree) return;
    inspect_hit(root, dt->inspect_mx, dt->inspect_my, dt);
}

/* ===================================================================
 * ログ追加
 * =================================================================*/
void hjp_devtools_log(HjpDevTools *dt, const char *fmt, ...) {
    int slot = (dt->log_head + dt->log_count) % HJP_DT_LOG_LINES;
    va_list args;
    va_start(args, fmt);
    vsnprintf(dt->log_lines[slot], HJP_DT_LOG_SIZE, fmt, args);
    va_end(args);
    if (dt->log_count < HJP_DT_LOG_LINES) {
        dt->log_count++;
    } else {
        dt->log_head = (dt->log_head + 1) % HJP_DT_LOG_LINES;
    }
}

/* ===================================================================
 * ヘルパー: 色付き矩形
 * =================================================================*/
static void dt_fill_rect(Hjpcontext *vg, float x, float y, float w, float h,
                          uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    hjpFillColor(vg, hjpRGBA(r, g, b, a));
    hjpBeginPath(vg);
    hjpRect(vg, x, y, w, h);
    hjpFill(vg);
}

static void dt_stroke_rect(Hjpcontext *vg, float x, float y, float w, float h,
                             uint8_t r, uint8_t g, uint8_t b, float sw) {
    hjpStrokeColor(vg, hjpRGBA(r, g, b, 200));
    hjpStrokeWidth(vg, sw);
    hjpBeginPath(vg);
    hjpRect(vg, x, y, w, h);
    hjpStroke(vg);
}

static void dt_text(Hjpcontext *vg, int font, float x, float y,
                     float fs, uint8_t r, uint8_t g, uint8_t b, const char *text) {
    hjpFontFaceId(vg, font);
    hjpFontSize(vg, fs);
    hjpFillColor(vg, hjpRGBA(r, g, b, 255));
    hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_TOP);
    hjpText(vg, x, y, text, NULL);
}

/* ===================================================================
 * メインレンダリング
 * =================================================================*/
void hjp_devtools_render(HjpDevTools *dt, void *vg_ptr, int font_id) {
    if (!dt->visible) return;
    Hjpcontext *vg = (Hjpcontext *)vg_ptr;

    float px = dt->panel_x;
    float py = dt->panel_y;
    float pw = dt->panel_w;

    /* パネル背景 */
    dt_fill_rect(vg, px, py, pw, dt->panel_h, 18, 18, 22, 220);
    dt_stroke_rect(vg, px, py, pw, dt->panel_h, 80, 80, 120, 1.0f);

    /* タイトル */
    dt_text(vg, font_id, px + 8, py + 6, 13.0f, 150, 200, 255,
            "■ DevTools [F12 で閉じる]");

    float cy = py + 28;

    /* FPS グラフ */
    if (dt->show_fps) {
        char buf[64];
        snprintf(buf, sizeof(buf), "FPS: %.1f  (min=%.0f max=%.0f avg=%.0f)",
            dt->fps_hist[(dt->fps_idx - 1 + HJP_DT_FPS_HIST) % HJP_DT_FPS_HIST],
            dt->fps_min, dt->fps_max, dt->fps_avg);
        dt_text(vg, font_id, px + 8, cy, 12.0f, 100, 255, 100, buf);
        cy += 16;

        /* グラフ描画 */
        float gx = px + 8, gy = cy, gw = pw - 16, gh = 48;
        dt_fill_rect(vg, gx, gy, gw, gh, 10, 10, 15, 200);
        /* 60fps 基準線 */
        float ref_y = gy + gh - (60.0f / 120.0f) * gh;
        hjpStrokeColor(vg, hjpRGBA(80, 80, 80, 180));
        hjpStrokeWidth(vg, 1.0f);
        hjpBeginPath(vg);
        hjpMoveTo(vg, gx, ref_y);
        hjpLineTo(vg, gx + gw, ref_y);
        hjpStroke(vg);
        /* FPS 折れ線 */
        hjpBeginPath(vg);
        for (int i = 0; i < HJP_DT_FPS_HIST; i++) {
            int idx = (dt->fps_idx - HJP_DT_FPS_HIST + i + HJP_DT_FPS_HIST) % HJP_DT_FPS_HIST;
            float v = dt->fps_hist[idx];
            float nx = gx + (float)i / HJP_DT_FPS_HIST * gw;
            float ny = gy + gh - (v / 120.0f) * gh;
            if (ny < gy) ny = gy;
            if (i == 0) hjpMoveTo(vg, nx, ny);
            else        hjpLineTo(vg, nx, ny);
        }
        hjpStrokeColor(vg, hjpRGBA(100, 220, 100, 255));
        hjpStrokeWidth(vg, 1.5f);
        hjpStroke(vg);
        cy += gh + 6;
    }

    /* パフォーマンス情報 */
    if (dt->show_perf) {
        char buf[128];
        snprintf(buf, sizeof(buf), "CPU: %.2fms  描画コール: %d",
            dt->cpu_time_ms, dt->draw_calls);
        dt_text(vg, font_id, px + 8, cy, 11.5f, 200, 200, 100, buf);
        cy += 15;
        snprintf(buf, sizeof(buf), "描画ノード: %d  スキップ: %d",
            dt->nodes_rendered, dt->nodes_skipped);
        dt_text(vg, font_id, px + 8, cy, 11.5f, 200, 200, 100, buf);
        cy += 18;
    }

    /* ウィジェットインスペクタ */
    if (dt->show_widget_tree && dt->inspect_info[0]) {
        dt_text(vg, font_id, px + 8, cy, 12.0f, 160, 200, 255, "【インスペクタ】");
        cy += 16;
        /* 複数行出力 */
        const char *p = dt->inspect_info;
        char line[128];
        int li = 0;
        while (*p && cy < py + dt->panel_h - 20) {
            if (*p == '\n' || li >= 127) {
                line[li] = '\0';
                dt_text(vg, font_id, px + 8, cy, 11.0f, 200, 200, 200, line);
                cy += 14;
                li = 0;
                if (*p == '\n') p++;
            } else {
                line[li++] = *p++;
            }
        }
        if (li > 0) {
            line[li] = '\0';
            dt_text(vg, font_id, px + 8, cy, 11.0f, 200, 200, 200, line);
            cy += 14;
        }

        /* 選択ノードをハイライト (赤枠) */
        if (dt->inspect_key != 0) {
            hjpStrokeColor(vg, hjpRGBA(255, 80, 80, 200));
            hjpStrokeWidth(vg, 2.0f);
            /* 実際の座標は VTree から取得する必要があるため、
               inspect_hit() が既にセットした calc_* を使う想定 */
        }
    }

    /* ログコンソール */
    if (dt->log_count > 0) {
        cy = fmaxf(cy, py + dt->panel_h - (dt->log_count * 14) - 20);
        dt_fill_rect(vg, px + 4, cy - 2, pw - 8, dt->log_count * 14 + 4, 10, 10, 20, 200);
        for (int i = 0; i < dt->log_count; i++) {
            int idx = (dt->log_head + i) % HJP_DT_LOG_LINES;
            dt_text(vg, font_id, px + 8, cy, 10.5f, 180, 180, 180, dt->log_lines[idx]);
            cy += 14;
        }
    }
}

/* ===================================================================
 * はじむ API
 * =================================================================*/
Value fn_DevTools開始(int argc, Value *argv) {
    float w = (argc >= 1) ? argv[0].number : 1280;
    float h = (argc >= 2) ? argv[1].number : 720;
    hjp_devtools_init(&g_devtools, w, h);
    return hajimu_null();
}

Value fn_DevTools表示(int argc, Value *argv) {
    (void)argc; (void)argv;
    g_devtools.visible       = true;
    g_devtools.panel_visible = true;
    return hajimu_null();
}

Value fn_DevTools非表示(int argc, Value *argv) {
    (void)argc; (void)argv;
    g_devtools.visible       = false;
    g_devtools.panel_visible = false;
    return hajimu_null();
}

Value fn_DevToolsログ(int argc, Value *argv) {
    if (argc < 1) return hajimu_null();
    const char *msg = (argv[0].type == VALUE_STRING && argv[0].string.data)
        ? argv[0].string.data : "";
    hjp_devtools_log(&g_devtools, "%s", msg);
    return hajimu_null();
}

Value fn_DevToolsオプション(int argc, Value *argv) {
    if (argc < 2) return hajimu_null();
    const char *opt = (argv[0].type == VALUE_STRING && argv[0].string.data)
        ? argv[0].string.data : "";
    bool val = argv[1].boolean;
    hjp_devtools_set_option(&g_devtools, opt, val);
    return hajimu_null();
}
