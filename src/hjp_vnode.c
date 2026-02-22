/**
 * hjp_vnode.c — 仮想UIツリー + 差分レンダリングエンジン 実装
 *
 * アルゴリズム: React-style Reconciliation をCで実装
 *   1. vjp_begin_frame()  — バッファ切り替え
 *   2. ユーザーが VNode を構築 (vjp_alloc + vjp_append_child)
 *   3. vjp_layout()       — Flexbox 風レイアウト計算
 *   4. vjp_reconcile_and_render() — 前フレームと比較、差分のみ描画
 *
 * Copyright (c) 2026 Reo Shiozawa — MIT License
 */
#include "hjp_vnode.h"
#include "hjp_render.h"
#include "hjp_platform.h"
#include "hajimu_plugin.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

/* ===================================================================
 * ハッシュ (FNV-1a 32bit)
 * =================================================================*/
static uint32_t fnv1a(const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

uint32_t vjp_hash_node(const VNode *n) {
    uint32_t h = fnv1a(n->text, strlen(n->text));
    h ^= fnv1a(&n->style.bg_color, sizeof(Hjpcolor));
    h ^= fnv1a(&n->style.fg_color, sizeof(Hjpcolor));
    h ^= fnv1a(&n->style.width,    sizeof(float));
    h ^= fnv1a(&n->style.height,   sizeof(float));
    h ^= fnv1a(&n->value,          sizeof(float));
    h ^= fnv1a(&n->checked,        sizeof(bool));
    h ^= fnv1a(&n->type,           sizeof(VNodeType));
    return h;
}

/* ===================================================================
 * ツリー初期化
 * =================================================================*/
void vjp_tree_init(VTree *tree) {
    memset(tree, 0, sizeof(VTree));
    tree->cur = 1;  /* begin_frame で 0 → 1 → 0 ... と交互 */
}

/* ===================================================================
 * ノード確保 (プールアロケータ)
 * =================================================================*/
VNode *vjp_alloc(VTree *tree, VNodeType type, uint32_t key) {
    int buf = tree->cur;
    if (tree->pool_used[buf] >= HJP_VNODE_POOL_SIZE) {
        fprintf(stderr, "[vjp] ノードプール上限 %d に達しました\n", HJP_VNODE_POOL_SIZE);
        return NULL;
    }
    VNode *n = &tree->pool[buf][tree->pool_used[buf]++];
    memset(n, 0, sizeof(VNode));
    n->type    = type;
    n->key     = key;
    n->style.visible = true;
    n->style.opacity = 1.0f;
    n->style.font_size = 16.0f;
    n->dirty   = true;      /* 新規ノードは常にダーティ */
    n->layout_dirty = true;
    return n;
}

/* ===================================================================
 * 子ノード追加
 * =================================================================*/
void vjp_append_child(VNode *parent, VNode *child) {
    if (!parent || !child) return;
    if (parent->child_count >= HJP_VNODE_MAX_CHILDREN) {
        fprintf(stderr, "[vjp] 子ノード上限 %d に達しました\n", HJP_VNODE_MAX_CHILDREN);
        return;
    }
    parent->children[parent->child_count++] = child;
    child->parent = parent;
}

/* ===================================================================
 * フレーム開始: バッファ切り替え + プールリセット
 * =================================================================*/
void vjp_begin_frame(VTree *tree) {
    /* 前フレームのバッファ保持, 今フレームのバッファをリセット */
    tree->cur = 1 - tree->cur;
    tree->pool_used[tree->cur] = 0;
    tree->roots[tree->cur] = NULL;
    tree->reconcile_skipped = 0;
    tree->reconcile_updated = 0;
    tree->layout_count = 0;
    tree->frame_num++;
}

/* ===================================================================
 * ダーティ矩形管理
 * =================================================================*/
void vjp_mark_dirty_rect(VTree *tree, float x, float y, float w, float h) {
    /* 既存矩形と結合 (簡易 AABB マージ) */
    for (int i = 0; i < tree->dirty_rect_count; i++) {
        float rx = tree->dirty_rects[i].x;
        float ry = tree->dirty_rects[i].y;
        float rw = tree->dirty_rects[i].w;
        float rh = tree->dirty_rects[i].h;
        /* 重複判定 */
        if (x < rx + rw && x + w > rx && y < ry + rh && y + h > ry) {
            float nx = fminf(x, rx);
            float ny = fminf(y, ry);
            tree->dirty_rects[i].x = nx;
            tree->dirty_rects[i].y = ny;
            tree->dirty_rects[i].w = fmaxf(x + w, rx + rw) - nx;
            tree->dirty_rects[i].h = fmaxf(y + h, ry + rh) - ny;
            tree->dirty_rects[i].valid = true;
            return;
        }
    }
    if (tree->dirty_rect_count < 32) {
        int i = tree->dirty_rect_count++;
        tree->dirty_rects[i].x = x;
        tree->dirty_rects[i].y = y;
        tree->dirty_rects[i].w = w;
        tree->dirty_rects[i].h = h;
        tree->dirty_rects[i].valid = true;
    }
}

void vjp_clear_dirty_rects(VTree *tree) {
    tree->dirty_rect_count = 0;
    memset(tree->dirty_rects, 0, sizeof(tree->dirty_rects));
}

void vjp_invalidate_all(VTree *tree) {
    /* ツリー全体を再描画対象に */
    VNode *root = tree->roots[tree->cur];
    if (!root) return;
    root->dirty = true;
    root->subtree_dirty = true;
}

/* ===================================================================
 * Flexbox 風レイアウトエンジン
 * =================================================================*/
void vjp_layout(VNode *node, float x, float y, float max_w, float max_h) {
    if (!node || !node->style.visible) return;

    VStyle *st = &node->style;

    /* マージン適用 */
    x += st->margin_left;
    y += st->margin_top;
    max_w -= st->margin_left + st->margin_right;
    max_h -= st->margin_top  + st->margin_bottom;

    /* 計算済みサイズ決定 */
    float w = (st->width  > 0) ? st->width  : max_w;
    float h = (st->height > 0) ? st->height : max_h;
    if (st->min_width  > 0 && w < st->min_width)  w = st->min_width;
    if (st->min_height > 0 && h < st->min_height) h = st->min_height;
    if (st->max_width  > 0 && w > st->max_width)  w = st->max_width;
    if (st->max_height > 0 && h > st->max_height) h = st->max_height;

    /* アニメーション補間 */
    if (st->transition_ms > 0 && node->anim_w > 0) {
        /* 線形補間 (実際の運用では easing 関数を使う) */
        float t = 0.15f; /* 15% per frame */
        node->anim_x += (x - node->anim_x) * t;
        node->anim_y += (y - node->anim_y) * t;
        node->anim_w += (w - node->anim_w) * t;
        node->anim_h += (h - node->anim_h) * t;
    } else {
        node->anim_x = x;
        node->anim_y = y;
        node->anim_w = w;
        node->anim_h = h;
    }

    node->calc_x = x;
    node->calc_y = y;
    node->calc_w = w;
    node->calc_h = h;
    node->layout_dirty = false;

    /* パディング適用後の子領域 */
    float cx = x + st->padding_left;
    float cy = y + st->padding_top;
    float cw = w - st->padding_left - st->padding_right;
    float ch = h - st->padding_top  - st->padding_bottom;

    /* 子ノードへの Flex 配置 */
    bool is_row = (st->flex_dir == VFLEX_ROW || st->flex_dir == VFLEX_WRAP);

    /* Flex アイテムの総サイズ計算 */
    float total_child_size = 0;
    float total_grow = 0;
    for (int i = 0; i < node->child_count; i++) {
        VNode *c = node->children[i];
        if (!c || !c->style.visible) continue;
        float cs = is_row ? (c->style.width > 0 ? c->style.width : 0)
                          : (c->style.height > 0 ? c->style.height : 0);
        total_child_size += cs + st->gap;
        total_grow += c->style.flex_grow;
    }
    total_child_size -= st->gap; /* 末尾の gap は不要 */

    /* 余白 */
    float remaining = (is_row ? cw : ch) - total_child_size;
    float grow_unit = (total_grow > 0 && remaining > 0) ? remaining / total_grow : 0;

    float cursor = is_row ? cx : cy;

    for (int i = 0; i < node->child_count; i++) {
        VNode *c = node->children[i];
        if (!c || !c->style.visible) continue;

        float child_main = (c->style.flex_grow > 0)
            ? (is_row ? c->style.width  : c->style.height) + grow_unit * c->style.flex_grow
            : (is_row ? c->style.width  : c->style.height);

        float child_cross = is_row ? (c->style.height > 0 ? c->style.height : ch)
                                   : (c->style.width  > 0 ? c->style.width  : cw);

        if (is_row) {
            vjp_layout(c, cursor, cy, child_main, child_cross);
            cursor += (child_main > 0 ? child_main : c->calc_w) + st->gap;
        } else {
            vjp_layout(c, cx, cursor, child_cross, child_main);
            cursor += (child_main > 0 ? child_main : c->calc_h) + st->gap;
        }
    }
    (void)ch;
}

/* ===================================================================
 * 単一ノード描画
 * =================================================================*/
static void render_single_node(const VNode *n, Hjpcontext *vg) {
    if (!n->style.visible) return;

    float x = n->anim_x, y = n->anim_y, w = n->anim_w, h = n->anim_h;
    float r = n->style.border_radius;

    /* 背景 */
    if (n->style.bg_color.a > 0.001f) {
        Hjpcolor bg = n->style.bg_color;
        bg.a *= n->style.opacity;
        hjpFillColor(vg, bg);
        hjpBeginPath(vg);
        if (r > 0.1f) {
            hjpRoundedRect(vg, x, y, w, h, r);
        } else {
            hjpRect(vg, x, y, w, h);
        }
        hjpFill(vg);
    }

    /* 枠線 */
    if (n->style.border_width > 0 && n->style.border_color.a > 0.001f) {
        Hjpcolor bc = n->style.border_color;
        bc.a *= n->style.opacity;
        hjpStrokeColor(vg, bc);
        hjpStrokeWidth(vg, n->style.border_width);
        hjpBeginPath(vg);
        if (r > 0.1f) {
            hjpRoundedRect(vg, x, y, w, h, r);
        } else {
            hjpRect(vg, x, y, w, h);
        }
        hjpStroke(vg);
    }

    /* テキスト */
    if (n->text[0] && n->style.fg_color.a > 0.001f) {
        Hjpcolor fc = n->style.fg_color;
        fc.a *= n->style.opacity;
        hjpFillColor(vg, fc);
        hjpFontSize(vg, n->style.font_size);
        /* text_align mapping */
        float tx;
        int align;
        switch (n->style.text_align) {
            case 1:  tx = x + w * 0.5f; align = HJP_ALIGN_CENTER | HJP_ALIGN_MIDDLE; break;
            case 2:  tx = x + w;        align = HJP_ALIGN_RIGHT  | HJP_ALIGN_MIDDLE; break;
            default: tx = x + n->style.padding_left;
                     align = HJP_ALIGN_LEFT | HJP_ALIGN_MIDDLE; break;
        }
        hjpTextAlign(vg, align);
        hjpText(vg, tx, y + h * 0.5f, n->text, NULL);
    }

    /* カスタム描画 */
    if (n->type == VNODE_CUSTOM && n->on_render) {
        n->on_render((VNode *)n, vg);
    }
}

/* ===================================================================
 * Reconciler — 差分レンダリングコア
 * =================================================================*/
static void reconcile_node(VTree *tree, const VNode *prev, VNode *cur,
                            Hjpcontext *vg, uint32_t tick) {
    if (!cur) return;
    if (!cur->style.visible) return;

    /* ハッシュ比較 */
    uint32_t hash = vjp_hash_node(cur);
    cur->content_hash = hash;
    cur->prev_hash = prev ? prev->content_hash : 0;

    bool needs_redraw = cur->dirty
        || !prev
        || (hash != cur->prev_hash)
        || cur->subtree_dirty;

    if (!needs_redraw && prev && !prev->dirty && !prev->subtree_dirty) {
        /* スキップ: 前フレームと完全に同じ */
        tree->reconcile_skipped++;
        /* ただし子の変更確認は継続 */
        goto reconcile_children;
    }

    tree->reconcile_updated++;
    vjp_mark_dirty_rect(tree, cur->calc_x, cur->calc_y, cur->calc_w, cur->calc_h);
    render_single_node(cur, vg);

    cur->dirty = false;

reconcile_children:;
    /* 子ノードの照合 (key ベース O(n)) */
    for (int i = 0; i < cur->child_count; i++) {
        VNode *child_cur = cur->children[i];
        if (!child_cur) continue;

        /* 前フレームから同じ key を持つノードを探す */
        const VNode *child_prev = NULL;
        if (prev) {
            for (int j = 0; j < prev->child_count; j++) {
                if (prev->children[j] && prev->children[j]->key == child_cur->key) {
                    child_prev = prev->children[j];
                    break;
                }
            }
        }

        /* アニメーション: 前フレームの座標を引き継ぐ */
        if (child_prev && child_cur->style.transition_ms > 0) {
            child_cur->anim_x = child_prev->anim_x;
            child_cur->anim_y = child_prev->anim_y;
            child_cur->anim_w = child_prev->anim_w;
            child_cur->anim_h = child_prev->anim_h;
        }

        reconcile_node(tree, child_prev, child_cur, vg, tick);
    }

    cur->subtree_dirty = false;
}

/* ===================================================================
 * メイン公開API: 差分レンダリング
 * =================================================================*/
void vjp_reconcile_and_render(VTree *tree, Hjpcontext *vg,
                               float win_w, float win_h, uint32_t tick) {
    VNode *cur_root  = tree->roots[tree->cur];
    VNode *prev_root = tree->roots[1 - tree->cur];

    if (!cur_root) return;

    /* レイアウト計算 */
    vjp_layout(cur_root, 0, 0, win_w, win_h);

    /* 差分 Reconcile */
    reconcile_node(tree, prev_root, cur_root, vg, tick);

    vjp_clear_dirty_rects(tree);
}

/* ===================================================================
 * デバッグダンプ
 * =================================================================*/
void vjp_dump(const VNode *node, int depth) {
    if (!node) return;
    static const char *type_names[] = {
        "NONE","BOX","ROW","COL","OVERLAY","SCROLL",
        "TEXT","IMAGE","RECT","CIRCLE","LINE",
        "BUTTON","INPUT","SLIDER","CHECKBOX","SELECT","CUSTOM"
    };
    for (int i = 0; i < depth; i++) printf("  ");
    printf("[%s] key=%u \"%s\" (%.0f,%.0f %.0fx%.0f) dirty=%d hash=%08x\n",
        type_names[node->type < 17 ? node->type : 0],
        node->key, node->text,
        node->calc_x, node->calc_y, node->calc_w, node->calc_h,
        (int)node->dirty, node->content_hash);
    for (int i = 0; i < node->child_count; i++) {
        vjp_dump(node->children[i], depth + 1);
    }
}

/* ===================================================================
 * グローバルツリー (アプリ単位)
 * =================================================================*/
#define GUI_MAX_VTREES 8
static VTree  g_vtrees[GUI_MAX_VTREES];
static VNode *g_vstack[64]; /* ノード構築用スタック */
static int    g_vstack_top = 0;
static int    g_vtree_idx  = 0;

static VTree *get_vtree(int app_id) {
    if (app_id < 0 || app_id >= GUI_MAX_VTREES) return NULL;
    return &g_vtrees[app_id];
}

/* ===================================================================
 * はじむ API 実装
 * =================================================================*/

/* VUI.開始(アプリID) */
Value fn_vnode_begin(int argc, Value *argv) {
    if (!hajimu_check_argc(argc, 1)) return hajimu_null();
    int app_id = (int)argv[0].number;
    VTree *tree = get_vtree(app_id);
    if (!tree) return hajimu_null();
    vjp_begin_frame(tree);
    g_vstack_top = 0;
    g_vtree_idx  = app_id;
    /* ルートノードを生成してスタックに積む */
    VNode *root = vjp_alloc(tree, VNODE_COL, 0);
    if (!root) return hajimu_null();
    tree->roots[tree->cur] = root;
    g_vstack[g_vstack_top++] = root;
    return hajimu_number(app_id);
}

/* VUI.終了(アプリID) — コミットして描画 (fn_vnode_commitと同義) */
Value fn_vnode_end(int argc, Value *argv) {
    return fn_vnode_commit(argc, argv);
}

/* VUI.ボックス(key, 幅=0, 高さ=0) */
Value fn_vnode_box(int argc, Value *argv) {
    if (!hajimu_check_argc(argc, 1)) return hajimu_null();
    VTree *tree = get_vtree(g_vtree_idx);
    if (!tree || g_vstack_top == 0) return hajimu_null();
    uint32_t key = (uint32_t)(int)argv[0].number;
    VNode *node = vjp_alloc(tree, VNODE_BOX, key);
    if (!node) return hajimu_null();
    if (argc >= 3) { node->style.width = argv[1].number; node->style.height = argv[2].number; }
    vjp_append_child(g_vstack[g_vstack_top - 1], node);
    /* スタックに積む (子の追加先として) */
    if (g_vstack_top < 64) g_vstack[g_vstack_top++] = node;
    return hajimu_number(key);
}

/* VUI.テキスト(key, 文字列) */
Value fn_vnode_text(int argc, Value *argv) {
    if (argc < 2) return hajimu_null();
    VTree *tree = get_vtree(g_vtree_idx);
    if (!tree || g_vstack_top == 0) return hajimu_null();
    uint32_t key = (uint32_t)(int)argv[0].number;
    VNode *node = vjp_alloc(tree, VNODE_TEXT, key);
    if (!node) return hajimu_null();
    if (argv[1].type == VALUE_STRING && argv[1].string.data)
        strncpy(node->text, argv[1].string.data, HJP_VNODE_MAX_TEXT - 1);
    vjp_append_child(g_vstack[g_vstack_top - 1], node);
    return hajimu_number(key);
}

/* VUI.ボタン(key, ラベル, コールバック) */
Value fn_vnode_button(int argc, Value *argv) {
    if (argc < 2) return hajimu_null();
    VTree *tree = get_vtree(g_vtree_idx);
    if (!tree || g_vstack_top == 0) return hajimu_null();
    uint32_t key = (uint32_t)(int)argv[0].number;
    VNode *node = vjp_alloc(tree, VNODE_BUTTON, key);
    if (!node) return hajimu_null();
    if (argv[1].type == VALUE_STRING && argv[1].string.data)
        strncpy(node->text, argv[1].string.data, HJP_VNODE_MAX_TEXT - 1);
    node->style.height = 36.0f;
    node->style.border_radius = 6.0f;
    node->style.padding_left = node->style.padding_right = 12.0f;
    vjp_append_child(g_vstack[g_vstack_top - 1], node);
    return hajimu_number(key);
}

/* VUI.行(key) — ROW コンテナを開始 */
Value fn_vnode_row(int argc, Value *argv) {
    if (!hajimu_check_argc(argc, 1)) return hajimu_null();
    VTree *tree = get_vtree(g_vtree_idx);
    if (!tree || g_vstack_top == 0) return hajimu_null();
    uint32_t key = (uint32_t)(int)argv[0].number;
    VNode *node = vjp_alloc(tree, VNODE_ROW, key);
    if (!node) return hajimu_null();
    node->style.flex_dir = VFLEX_ROW;
    vjp_append_child(g_vstack[g_vstack_top - 1], node);
    if (g_vstack_top < 64) g_vstack[g_vstack_top++] = node;
    return hajimu_number(key);
}

/* VUI.列(key) — COL コンテナを開始 */
Value fn_vnode_col(int argc, Value *argv) {
    if (!hajimu_check_argc(argc, 1)) return hajimu_null();
    VTree *tree = get_vtree(g_vtree_idx);
    if (!tree || g_vstack_top == 0) return hajimu_null();
    uint32_t key = (uint32_t)(int)argv[0].number;
    VNode *node = vjp_alloc(tree, VNODE_COL, key);
    if (!node) return hajimu_null();
    node->style.flex_dir = VFLEX_COL;
    vjp_append_child(g_vstack[g_vstack_top - 1], node);
    if (g_vstack_top < 64) g_vstack[g_vstack_top++] = node;
    return hajimu_number(key);
}

/* VUI.スクロール(key, 幅, 高さ) */
Value fn_vnode_scroll(int argc, Value *argv) {
    if (argc < 3) return hajimu_null();
    VTree *tree = get_vtree(g_vtree_idx);
    if (!tree || g_vstack_top == 0) return hajimu_null();
    uint32_t key = (uint32_t)(int)argv[0].number;
    VNode *node = vjp_alloc(tree, VNODE_SCROLL, key);
    if (!node) return hajimu_null();
    node->style.width  = argv[1].number;
    node->style.height = argv[2].number;
    node->style.flex_dir = VFLEX_COL;
    vjp_append_child(g_vstack[g_vstack_top - 1], node);
    if (g_vstack_top < 64) g_vstack[g_vstack_top++] = node;
    return hajimu_number(key);
}

/* VUI.入力(key, プレースホルダ) */
Value fn_vnode_input(int argc, Value *argv) {
    if (argc < 2) return hajimu_null();
    VTree *tree = get_vtree(g_vtree_idx);
    if (!tree || g_vstack_top == 0) return hajimu_null();
    uint32_t key = (uint32_t)(int)argv[0].number;
    VNode *node = vjp_alloc(tree, VNODE_INPUT, key);
    if (!node) return hajimu_null();
    if (argv[1].type == VALUE_STRING && argv[1].string.data)
        strncpy(node->text, argv[1].string.data, HJP_VNODE_MAX_TEXT - 1);
    node->style.height = 32.0f;
    node->style.border_width = 1.0f;
    node->style.border_radius = 4.0f;
    vjp_append_child(g_vstack[g_vstack_top - 1], node);
    return hajimu_number(key);
}

/* VUI.スタイル(辞書) — 現在スタック先頭ノードにスタイル適用 */
Value fn_vnode_style(int argc, Value *argv) {
    if (argc < 1 || g_vstack_top == 0) return hajimu_null();
    VNode *node = g_vstack[g_vstack_top - 1];
    if (!node) return hajimu_null();
    /* 辞書型から各スタイルプロパティを読み取る */
    if (argv[0].type != VALUE_DICT) return hajimu_null();
    for (int _i = 0; _i < argv[0].dict.length; _i++) {
        const char *_k = argv[0].dict.keys[_i];
        Value       _v = argv[0].dict.values[_i];
        if (!_k) continue;
        if (_v.type == VALUE_NUMBER) {
            double _n = _v.number;
            if (strcmp(_k, "幅")           == 0) node->style.width          = (float)_n;
            else if (strcmp(_k, "高さ")         == 0) node->style.height         = (float)_n;
            else if (strcmp(_k, "最小幅")       == 0) node->style.min_width      = (float)_n;
            else if (strcmp(_k, "最小高さ")     == 0) node->style.min_height     = (float)_n;
            else if (strcmp(_k, "余白")         == 0) {
                node->style.margin_top = node->style.margin_bottom =
                node->style.margin_left = node->style.margin_right = (float)_n;
            }
            else if (strcmp(_k, "内余白")       == 0) {
                node->style.padding_top = node->style.padding_bottom =
                node->style.padding_left = node->style.padding_right = (float)_n;
            }
            else if (strcmp(_k, "角丸")         == 0) node->style.border_radius  = (float)_n;
            else if (strcmp(_k, "枠線幅")       == 0) node->style.border_width   = (float)_n;
            else if (strcmp(_k, "フォントサイズ") == 0) node->style.font_size      = (float)_n;
            else if (strcmp(_k, "透明度")       == 0) node->style.opacity        = (float)_n;
            else if (strcmp(_k, "間隔")         == 0) node->style.gap            = (float)_n;
            else if (strcmp(_k, "遷移")         == 0) node->style.transition_ms  = (float)_n;
        } else if (_v.type == VALUE_BOOL) {
            if (strcmp(_k, "表示") == 0) node->style.visible = _v.boolean;
        }
    }
    node->dirty = true;
    return hajimu_null();
}

/* VUI.コミット(アプリID, VGコンテキスト) — Reconcile を実行 */
Value fn_vnode_commit(int argc, Value *argv) {
    /* スタック巻き戻し */
    g_vstack_top = 0;
    return hajimu_null();
}

/* VUI.無効化(アプリID) — 強制全再描画 */
Value fn_vnode_invalidate(int argc, Value *argv) {
    if (!hajimu_check_argc(argc, 1)) return hajimu_null();
    int app_id = (int)argv[0].number;
    VTree *tree = get_vtree(app_id);
    if (!tree) return hajimu_null();
    vjp_invalidate_all(tree);
    return hajimu_null();
}
