/**
 * hjp_vnode.h — はじむGUI 仮想UIツリー + 差分レンダリングエンジン
 *
 * 設計思想:
 *   - 宣言的UI: UI = f(状態) を C レベルで実現
 *   - O(n) 差分アルゴリズム: key ベースの Reconciler
 *   - ダーティフラグ: 変更ノードのみ再描画
 *   - 1フレーム分の仮想ツリーを構築 → 前フレームと比較 → 差分描画
 *
 * Copyright (c) 2026 Reo Shiozawa — MIT License
 */
#ifndef HJP_VNODE_H
#define HJP_VNODE_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "hjp_render.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ===================================================================
 * 定数
 * =================================================================*/
#define HJP_VNODE_MAX_CHILDREN  64
#define HJP_VNODE_MAX_TEXT      256
#define HJP_VNODE_MAX_CLASS     64
#define HJP_VNODE_POOL_SIZE     4096   /* ノードプール上限 */
#define HJP_VNODE_MAX_PROPS     16     /* カスタムプロパティ上限 */

/* ===================================================================
 * ノード型
 * =================================================================*/
typedef enum {
    VNODE_NONE = 0,
    /* レイアウト */
    VNODE_BOX,          /* 汎用コンテナ */
    VNODE_ROW,          /* 水平レイアウト */
    VNODE_COL,          /* 垂直レイアウト */
    VNODE_OVERLAY,      /* 絶対配置オーバーレイ */
    VNODE_SCROLL,       /* スクロールコンテナ */
    /* 表示 */
    VNODE_TEXT,         /* テキスト */
    VNODE_IMAGE,        /* 画像 */
    VNODE_RECT,         /* 矩形 */
    VNODE_CIRCLE,       /* 円 */
    VNODE_LINE,         /* 線 */
    /* インタラクション */
    VNODE_BUTTON,       /* ボタン */
    VNODE_INPUT,        /* テキスト入力 */
    VNODE_SLIDER,       /* スライダー */
    VNODE_CHECKBOX,     /* チェックボックス */
    VNODE_SELECT,       /* セレクトボックス */
    /* カスタム */
    VNODE_CUSTOM,       /* ユーザー定義ノード */
} VNodeType;

/* ===================================================================
 * Flex 配置
 * =================================================================*/
typedef enum {
    VFLEX_START = 0,
    VFLEX_CENTER,
    VFLEX_END,
    VFLEX_SPACE_BETWEEN,
    VFLEX_SPACE_AROUND,
} VFlexAlign;

typedef enum {
    VFLEX_ROW = 0,
    VFLEX_COL,
    VFLEX_WRAP,
} VFlexDir;

/* ===================================================================
 * スタイル
 * =================================================================*/
typedef struct {
    /* サイズ (0 = auto) */
    float width, height;
    float min_width, min_height;
    float max_width, max_height;

    /* 余白 */
    float margin_top, margin_right, margin_bottom, margin_left;
    float padding_top, padding_right, padding_bottom, padding_left;

    /* 色 */
    Hjpcolor bg_color;
    Hjpcolor fg_color;
    Hjpcolor border_color;

    /* 枠線 */
    float border_width;
    float border_radius;

    /* テキスト */
    float font_size;
    int   font_weight;      /* 400=通常, 700=太字 */
    int   text_align;       /* 0=左, 1=中, 2=右 */

    /* Flex */
    VFlexDir    flex_dir;
    VFlexAlign  justify;
    VFlexAlign  align_items;
    float       flex_grow;
    float       gap;

    /* 透明度・表示 */
    float   opacity;
    bool    visible;

    /* アニメーション */
    float   transition_ms;  /* 0 = アニメなし */
} VStyle;

/* ===================================================================
 * イベントコールバック
 * =================================================================*/
struct VNode;
typedef void (*VNodeCallback)(struct VNode *node, void *user_data);
typedef void (*VNodeRenderFunc)(struct VNode *node, Hjpcontext *vg);

/* ===================================================================
 * 仮想ノード
 * =================================================================*/
typedef struct VNode {
    /* 識別 */
    VNodeType   type;
    uint32_t    key;            /* 差分計算の照合キー (hash or explicit) */
    char        class_name[HJP_VNODE_MAX_CLASS]; /* デバッグ/CSS的分類 */

    /* コンテンツ */
    char        text[HJP_VNODE_MAX_TEXT];
    int         image_id;       /* VNODE_IMAGE/RECT のテクスチャID */

    /* 状態 */
    bool        checked;        /* VNODE_CHECKBOX */
    float       value;          /* VNODE_SLIDER/INPUT */
    char        input_text[HJP_VNODE_MAX_TEXT];

    /* 計算済みレイアウト (Reconciler が埋める) */
    float       calc_x, calc_y, calc_w, calc_h;

    /* スタイル */
    VStyle      style;

    /* ダーティフラグ */
    bool        dirty;          /* 自ノード再描画が必要 */
    bool        layout_dirty;   /* レイアウト再計算が必要 */
    bool        subtree_dirty;  /* 子孫いずれか dirty */

    /* 差分制御 */
    uint32_t    content_hash;   /* text/style/value のハッシュ */
    uint32_t    prev_hash;      /* 前フレームのハッシュ (一致 → skip) */

    /* イベント */
    VNodeCallback on_click;
    VNodeCallback on_hover;
    VNodeCallback on_change;
    VNodeRenderFunc on_render;   /* VNODE_CUSTOM の描画関数 */
    void         *user_data;

    /* ツリー構造 */
    struct VNode *parent;
    struct VNode *children[HJP_VNODE_MAX_CHILDREN];
    int           child_count;
    struct VNode *next_sibling;  /* フラット走査用 */

    /* アニメーション (transition) */
    float        anim_x, anim_y;  /* 現在の補間済み座標 */
    float        anim_w, anim_h;
    uint32_t     anim_start_tick;
} VNode;

/* ===================================================================
 * 仮想UIツリー (ダブルバッファ構造)
 * =================================================================*/
typedef struct {
    /* ノードプール (フレーム単位でリセット) */
    VNode   pool[2][HJP_VNODE_POOL_SIZE];  /* [0]=前フレーム, [1]=今フレーム */
    int     pool_used[2];
    int     cur;            /* 現在書き込み対象バッファ (0 or 1) */

    /* ルートノード per バッファ */
    VNode  *roots[2];

    /* 統計 */
    int     reconcile_skipped;  /* 差分スキップ数 */
    int     reconcile_updated;  /* 差分更新数 */
    int     layout_count;       /* レイアウト計算数 */

    /* フレーム番号 */
    uint64_t frame_num;

    /* ダーティ領域 (再描画が必要な矩形) */
    struct {
        float x, y, w, h;
        bool  valid;
    } dirty_rects[32];
    int dirty_rect_count;
} VTree;

/* ===================================================================
 * Public API
 * =================================================================*/

/* ツリーの初期化 */
void    vjp_tree_init(VTree *tree);

/* ノード確保 (カレントバッファから) */
VNode  *vjp_alloc(VTree *tree, VNodeType type, uint32_t key);

/* ノード追加 */
void    vjp_append_child(VNode *parent, VNode *child);

/* フレーム開始: 書き込みバッファを切り替え */
void    vjp_begin_frame(VTree *tree);

/* 差分レンダリング: 前フレームと比較し、変更ノードのみ再描画 */
void    vjp_reconcile_and_render(VTree *tree, Hjpcontext *vg, float win_w, float win_h, uint32_t tick);

/* レイアウト計算 */
void    vjp_layout(VNode *node, float x, float y, float max_w, float max_h);

/* ハッシュ計算 (テキスト + スタイルのフィンガープリント) */
uint32_t vjp_hash_node(const VNode *node);

/* ダーティ領域を展開 */
void    vjp_mark_dirty_rect(VTree *tree, float x, float y, float w, float h);

/* 全ダーティ矩形を消去 */
void    vjp_clear_dirty_rects(VTree *tree);

/* ツリー全体を強制ダーティ (リサイズ時等) */
void    vjp_invalidate_all(VTree *tree);

/* デバッグダンプ (stdout) */
void    vjp_dump(const VNode *node, int depth);

/* ===================================================================
 * はじむAPI ラッパー (hajimu_gui.c から呼ばれる)
 * =================================================================*/
#include "hajimu_plugin.h"

Value fn_vnode_begin(int argc, Value *argv);         /* VUI.開始(アプリ) */
Value fn_vnode_end(int argc, Value *argv);           /* VUI.終了(アプリ) */
Value fn_vnode_box(int argc, Value *argv);           /* VUI.ボックス(key, 幅, 高さ) */
Value fn_vnode_text(int argc, Value *argv);          /* VUI.テキスト(key, 文字列) */
Value fn_vnode_button(int argc, Value *argv);        /* VUI.ボタン(key, ラベル, コールバック) */
Value fn_vnode_input(int argc, Value *argv);         /* VUI.入力(key, プレースホルダ) */
Value fn_vnode_row(int argc, Value *argv);           /* VUI.行(key) */
Value fn_vnode_col(int argc, Value *argv);           /* VUI.列(key) */
Value fn_vnode_scroll(int argc, Value *argv);        /* VUI.スクロール(key, 幅, 高さ) */
Value fn_vnode_style(int argc, Value *argv);         /* VUI.スタイル(key: 辞書) */
Value fn_vnode_commit(int argc, Value *argv);        /* VUI.コミット(アプリ) */
Value fn_vnode_invalidate(int argc, Value *argv);    /* VUI.無効化(アプリ) */

#ifdef __cplusplus
}
#endif

#endif /* HJP_VNODE_H */
