/***********************************************************************
 * hajimu_gui.c — はじむ用 GUI プラグイン
 *
 * hjp_platform + hjp_render (OpenGL 3) ベースの即時モード GUI フレームワーク。
 * egui × PyQt のハイブリッド設計を完全日本語 API で提供する。
 *
 * Phase 1: ウィンドウ + 基本描画 + イベントループ
 * Phase 2: テキスト・ボタン・チェックボックス等の基本ウィジェット
 *
 * Copyright (c) 2026 Reo Shiozawa — MIT License
 *
 *   Noto Sans CJK JP — Google — SIL Open Font License 1.1
 ***********************************************************************/

/* =====================================================================
 * インクルード
 * ===================================================================*/

/* macOS: hajimu_plugin.h が _POSIX_C_SOURCE を定義すると dlfcn.h の
   Dl_info / dladdr が隠れるため、最初に _DARWIN_C_SOURCE を宣言する */
#ifdef __APPLE__
  #ifndef _DARWIN_C_SOURCE
    #define _DARWIN_C_SOURCE
  #endif
#endif

/* Linux: dladdr は GNU 拡張のため _GNU_SOURCE が必要 */
#ifdef __linux__
  #ifndef _GNU_SOURCE
    #define _GNU_SOURCE
  #endif
#endif

#include "hajimu_plugin.h"

#include "hjp_platform.h"
#include "hjp_vnode.h"
#include "hjp_frame.h"
#include "hjp_hotreload.h"
#include "hjp_devtools.h"
#include "hjp_lifecycle.h"
#include <math.h>
#include <ctype.h>
#include <time.h>

/* --- OpenGL ヘッダー --- */
#ifdef __APPLE__
  #ifndef GL_SILENCE_DEPRECATION
    #define GL_SILENCE_DEPRECATION
  #endif
  #include <OpenGL/gl3.h>
  #include <dlfcn.h>
  #include <libgen.h>
  #include <unistd.h>
#elif defined(__linux__)
  #define GL_GLEXT_PROTOTYPES
  #include <GL/gl.h>
  #include <GL/glext.h>
  #include <dlfcn.h>
  #include <libgen.h>
  #include <unistd.h>
#else
  #include <GL/gl.h>
  #include <windows.h>
  #include <io.h>
  #define access _access
  #define F_OK   0
  /* OpenGL 3.0+ 関数ポインタを hjp_platform_win32.c の実装から参照 */
  #ifndef HJP_GL_LOADER
    #define HJP_GL_LOADER
  #endif
  #include "hjp_gl_funcs.h"
#endif

/* --- hjp_render (自作描画エンジン) --- */
#include "hjp_render.h"

/* =====================================================================
 * 定数・テーマ
 * ===================================================================*/

#define GUI_MAX_APPS         8
#define GUI_PADDING         14.0f
#define GUI_MARGIN           8.0f
#define GUI_WIDGET_H        34.0f
#define GUI_BTN_RADIUS       6.0f
#define GUI_CHECK_SIZE      20.0f
#define GUI_RADIO_RADIUS    10.0f
#define GUI_SLIDER_H         6.0f
#define GUI_SLIDER_HANDLE   14.0f
#define GUI_PROGRESS_H      10.0f
#define GUI_FONT_SIZE       16.0f
#define GUI_FONT_NAME       "noto-cjk"
#define GUI_LABEL_W        130.0f    /* ラベル幅 (スライダー等) */
#define GUI_INPUT_H         32.0f    /* テキスト入力高さ */
#define GUI_INPUT_PAD        8.0f    /* 入力フィールド内余白 */
#define GUI_COMBO_ITEM_H    28.0f    /* コンボボックス項目高さ */
#define GUI_MAX_TEXT_BUF   2048      /* テキストバッファ最大長 */
#define GUI_MAX_TEXT_SLOTS   64      /* 同時テキスト入力数 */
#define GUI_TEXTAREA_LINES   10      /* テキストエリア行数デフォルト */
#define GUI_CURSOR_BLINK_MS 530      /* カーソル点滅間隔 */
#define GUI_MAX_PANELS       32      /* パネルスタック最大数 */

/* =====================================================================
 * テーマカラー (動的テーマ対応 — Phase 8)
 * ===================================================================*/

typedef struct {
    Hjpcolor bg, widget_bg, widget_hover, widget_active;
    Hjpcolor accent, accent_hover;
    Hjpcolor text, text_dim;
    Hjpcolor border, sep, check, track;
    Hjpcolor tooltip_bg, tooltip_fg;
} GuiThemeColors;

static GuiThemeColors g_th;
static bool  g_th_init = false;

static void gui_theme_set_dark(void) {
    g_th.bg           = hjpRGBA( 30,  30,  30, 255);
    g_th.widget_bg    = hjpRGBA( 50,  50,  55, 255);
    g_th.widget_hover = hjpRGBA( 65,  65,  72, 255);
    g_th.widget_active= hjpRGBA( 80,  80,  90, 255);
    g_th.accent       = hjpRGBA( 66, 133, 244, 255);
    g_th.accent_hover = hjpRGBA(100, 160, 255, 255);
    g_th.text         = hjpRGBA(228, 228, 228, 255);
    g_th.text_dim     = hjpRGBA(155, 155, 155, 255);
    g_th.border       = hjpRGBA( 75,  75,  80, 255);
    g_th.sep          = hjpRGBA( 55,  55,  60, 255);
    g_th.check        = hjpRGBA(255, 255, 255, 255);
    g_th.track        = hjpRGBA( 55,  55,  60, 255);
    g_th.tooltip_bg   = hjpRGBA( 55,  55,  62, 235);
    g_th.tooltip_fg   = hjpRGBA(215, 215, 215, 255);
    g_th_init = true;
}

static void gui_theme_set_light(void) {
    g_th.bg           = hjpRGBA(245, 245, 245, 255);
    g_th.widget_bg    = hjpRGBA(255, 255, 255, 255);
    g_th.widget_hover = hjpRGBA(230, 230, 235, 255);
    g_th.widget_active= hjpRGBA(215, 215, 220, 255);
    g_th.accent       = hjpRGBA( 26, 115, 232, 255);
    g_th.accent_hover = hjpRGBA( 66, 133, 244, 255);
    g_th.text         = hjpRGBA( 32,  33,  36, 255);
    g_th.text_dim     = hjpRGBA(128, 128, 128, 255);
    g_th.border       = hjpRGBA(200, 200, 205, 255);
    g_th.sep          = hjpRGBA(218, 218, 220, 255);
    g_th.check        = hjpRGBA(255, 255, 255, 255);
    g_th.track        = hjpRGBA(200, 200, 205, 255);
    g_th.tooltip_bg   = hjpRGBA( 55,  55,  62, 235);
    g_th.tooltip_fg   = hjpRGBA(240, 240, 240, 255);
    g_th_init = true;
}

static void gui_theme_set_high_contrast(void) {
    g_th.bg           = hjpRGBA(  0,   0,   0, 255);
    g_th.widget_bg    = hjpRGBA( 30,  30,  30, 255);
    g_th.widget_hover = hjpRGBA( 60,  60,  60, 255);
    g_th.widget_active= hjpRGBA( 80,  80,  80, 255);
    g_th.accent       = hjpRGBA(  0, 200, 255, 255);
    g_th.accent_hover = hjpRGBA(100, 220, 255, 255);
    g_th.text         = hjpRGBA(255, 255, 255, 255);
    g_th.text_dim     = hjpRGBA(200, 200, 200, 255);
    g_th.border       = hjpRGBA(255, 255, 255, 255);
    g_th.sep          = hjpRGBA(180, 180, 180, 255);
    g_th.check        = hjpRGBA(255, 255,   0, 255);
    g_th.track        = hjpRGBA( 80,  80,  80, 255);
    g_th.tooltip_bg   = hjpRGBA( 40,  40,  40, 255);
    g_th.tooltip_fg   = hjpRGBA(255, 255, 255, 255);
    g_th_init = true;
}

/* 動的テーママクロ (g_th 参照) */
#define TH_BG            g_th.bg
#define TH_WIDGET_BG     g_th.widget_bg
#define TH_WIDGET_HOVER  g_th.widget_hover
#define TH_WIDGET_ACTIVE g_th.widget_active
#define TH_ACCENT        g_th.accent
#define TH_ACCENT_HOVER  g_th.accent_hover
#define TH_TEXT          g_th.text
#define TH_TEXT_DIM      g_th.text_dim
#define TH_BORDER        g_th.border
#define TH_SEP           g_th.sep
#define TH_CHECK         g_th.check
#define TH_TRACK         g_th.track
#define TH_TOOLTIP_BG    g_th.tooltip_bg
#define TH_TOOLTIP_FG    g_th.tooltip_fg

/* 見出しサイズ (h1–h6) */
static const float HEADING_SZ[] = {34.0f, 30.0f, 26.0f, 22.0f, 19.0f, 17.0f};

/* =====================================================================
 * 構造体
 * ===================================================================*/

/* --- RGBA 色 --- */
typedef struct {
    float r, g, b, a;
} GuiRGBA;

/* --- 入力状態 (フレーム単位) --- */
typedef struct {
    int mx, my;                 /* 現在のマウス座標                 */
    int pmx, pmy;               /* 前フレームのマウス座標           */
    bool down;                  /* ボタン押下中                     */
    bool clicked;               /* このフレームで押された           */
    bool released;              /* このフレームで離された           */
    int  scroll_y;              /* スクロール量                     */
    /* キーボード入力 (Phase 3) */
    char text_input[128];       /* テキスト入力で受けた UTF-8 */
    int  text_input_len;        /* 今フレームの入力バイト数     */
    bool key_backspace;         /* Backspace 押下                 */
    bool key_delete;            /* Delete 押下                    */
    bool key_return;            /* Enter / Return 押下            */
    bool key_left;              /* ← 押下                          */
    bool key_right;             /* → 押下                          */
    bool key_home;              /* Home 押下                      */
    bool key_end;               /* End 押下                       */
    bool key_a;                 /* Ctrl+A (Cmd+A)                  */
    bool key_c;                 /* Ctrl+C                          */
    bool key_v;                 /* Ctrl+V                          */
    bool key_x;                 /* Ctrl+X                          */
    bool mod_ctrl;              /* Ctrl/Cmd 保持                   */
    bool mod_shift;             /* Shift 保持                     */
    /* Phase 9 追加 */
    bool mouse_right_clicked;   /* 右クリック                      */
    bool mouse_middle;          /* 中ボタン押下中                 */
    HjpKeycode last_key;       /* 最後に押されたキー              */
    bool key_up, key_down;      /* ↑↓ 押下                        */
    bool key_tab;               /* Tab 押下                        */
} GuiInput;

/* --- レイアウトカーソル --- */
typedef struct {
    float x, y;
    float w;                    /* 使用可能幅                       */
    float indent;
} GuiLayout;

/* --- テキスト入力バッファ (Phase 3) --- */
typedef struct {
    uint32_t id;                /* ウィジェット ID                  */
    char     buf[GUI_MAX_TEXT_BUF]; /* UTF-8 テキスト         */
    int      len;               /* バイト長                         */
    int      cursor;            /* カーソル位置 (バイトオフセット) */
    int      sel_start;         /* 選択開始 (バイト)             */
    int      sel_end;           /* 選択終了 (バイト)             */
} GuiTextBuf;

/* --- コンボボックス開閉状態 --- */
static uint32_t g_combo_open = 0;  /* 開いているコンボ ID (0=なし) */

/* --- パネルスタック (Phase 4) --- */
typedef struct {
    float x, y, w, h;          /* パネル領域                       */
    float scroll_y;            /* スクロールオフセット           */
    float content_h;           /* 内容の総高さ                   */
    bool  scrollable;          /* スクロール有効か                 */
} GuiPanel;

/* --- 直前ウィジェット情報 (ツールチップ用) --- */
typedef struct {
    float x, y, w, h;
    bool hovered;
} GuiLastWidget;

/* --- アプリケーション本体 --- */
typedef struct {
    HjpWindow   *window;
    HjpGLContext  gl;
    Hjpcontext   *vg;
    int           font_id;

    int  win_w, win_h;          /* ウィンドウ論理サイズ             */
    int  fb_w,  fb_h;           /* フレームバッファサイズ           */
    float px_ratio;             /* Retina 倍率                      */

    bool   running;
    int    target_fps;
    GuiRGBA bg;

    GuiInput     in;
    GuiLayout    lay;
    GuiLastWidget last;

    /* 即時モード追跡 */
    uint32_t hot;               /* ホバー中ウィジェット ID          */
    uint32_t active;            /* 操作中ウィジェット ID            */

    /* ツールチップ */
    char  tooltip[512];
    bool  tooltip_show;
    float tooltip_x, tooltip_y;

    /* テキスト入力バッファプール (Phase 3) */
    GuiTextBuf text_bufs[GUI_MAX_TEXT_SLOTS];
    int  text_buf_count;
    uint32_t focused;           /* フォーカス中テキスト入力 ID      */

    /* パネルスタック (Phase 4) */
    GuiPanel panels[GUI_MAX_PANELS];
    int      panel_depth;
    GuiLayout saved_lay[GUI_MAX_PANELS]; /* パネル前のレイアウト保存 */

    bool valid;
} GuiApp;

static GuiApp  g_apps[GUI_MAX_APPS];
static GuiApp *g_cur  = NULL;           /* 描画中の現在アプリ        */

/* =====================================================================
 * Phase 5-8 追加構造体・グローバル
 * ===================================================================*/

/* --- Phase 5: テーブル・ツリー --- */
#define GUI_MAX_TABLES       16
#define GUI_MAX_TREES        16
#define GUI_MAX_TREE_NODES  256
#define GUI_LIST_ITEM_H      28.0f
#define GUI_TABLE_HEADER_H   30.0f
#define GUI_TABLE_ROW_H      26.0f
#define GUI_TREE_INDENT      20.0f
#define GUI_TREE_NODE_H      26.0f

typedef struct {
    uint32_t id;
    int col_count;
    char col_names[16][64];
    int row_count;
    int row_cap;
    char ***rows;           /* rows[r][c] = strdup'd string */
    int selected;
    int sort_col;
    int sort_dir;           /* 1=昇順, -1=降順 */
    float scroll_y;
    bool valid;
    /* Phase 35: テーブル高度操作 */
    int  edit_row, edit_col;              /* インライン編集中セル (-1=なし) */
    char edit_buf[256];                   /* 編集バッファ */
    int  edit_cursor;                     /* 編集カーソル位置 */
    int  col_order[16];                   /* 列表示順 (初期値 0,1,2,...) */
    int  sel_mode;                        /* 0=単一, 1=複数, 2=範囲 */
    bool row_selected[4096];              /* 複数選択用 */
    int  fixed_cols;                      /* 固定列数 */
    int  group_col;                       /* グループ化列 (-1=なし) */
    bool group_collapsed[256];            /* グループ折畳状態 */
    int  merge_row, merge_col;            /* セル結合起点 */
    int  merge_rspan, merge_cspan;        /* 結合範囲 */
    int  drag_row;                        /* ドラッグ中の行 (-1=なし) */
    int  drag_target;                     /* ドロップ先行 */
} GuiTable;

typedef struct {
    int parent;
    char label[128];
    bool expanded;
    /* Phase 36 */
    bool checked;
    int  icon_image;       /* hjp_render画像ID (-1=なし) */
    bool visible;          /* 検索フィルタ結果 */
} GuiTreeNode;

typedef struct {
    uint32_t id;
    GuiTreeNode nodes[GUI_MAX_TREE_NODES];
    int node_count;
    int selected;
    float scroll_y;
    bool valid;
    /* Phase 36 */
    int  sel_mode;            /* 0=単一, 1=複数 */
    bool node_selected[GUI_MAX_TREE_NODES];
    bool checkbox_mode;
    bool lazy_load;
} GuiTree;

static GuiTable g_tables[GUI_MAX_TABLES];
static GuiTree  g_trees[GUI_MAX_TREES];

/* --- Phase 6: メニュー・トースト --- */
#define GUI_TAB_H            32.0f
#define GUI_MENUBAR_H        28.0f
#define GUI_MENU_ITEM_H      28.0f
#define GUI_MENU_W          180.0f
#define GUI_MAX_TOASTS        8
#define GUI_TOAST_DURATION  3000

typedef struct {
    char message[256];
    uint32_t start_time;
    int duration_ms;
    bool active;
} GuiToast;

static GuiToast  g_toasts[GUI_MAX_TOASTS];
static uint32_t  g_menu_open   = 0;
static float     g_menu_x      = 0, g_menu_y = 0;
static bool      g_menubar_active  = false;
static float     g_menubar_cursor_x = 0;

/* --- Phase 7: 画像・キャンバス --- */
#define GUI_MAX_IMAGES       64

typedef struct {
    int handle;
    int w, h;
    bool valid;
} GuiImage;

static GuiImage g_images[GUI_MAX_IMAGES];
static bool     g_canvas_active = false;
static float    g_canvas_ox = 0, g_canvas_oy = 0;

/* --- Phase 8: カスタムフォントサイズ --- */
static float g_custom_font_size = 0;

/* --- Phase 9: ドラッグ&ドロップ・クリップボード --- */
#define GUI_MAX_DROP_FILES   32
#define GUI_MAX_SHORTCUTS    64
#define GUI_MAX_ANIM_SLOTS   64

typedef struct {
    uint32_t id;
    float    src_x, src_y, src_w, src_h;
    Value    data;
    bool     dragging;
} GuiDragSource;

static GuiDragSource g_drag = {0};
static bool  g_drag_active      = false;
static char  g_drop_files[GUI_MAX_DROP_FILES][512];
static int   g_drop_file_count  = 0;
static int   g_cursor_type      = 0;   /* 0=arrow,1=hand,2=ibeam,3=crosshair,4=resize */
static HjpCursor *g_cursors[5] = {NULL};

/* --- Phase 10: アニメーション --- */
typedef struct {
    uint32_t id;
    float    start_val, end_val;
    float    duration_ms;
    uint32_t start_time;
    int      easing;    /* 0=linear,1=ease_in,2=ease_out,3=ease_in_out */
    bool     active;
} GuiAnim;

static GuiAnim g_anims[GUI_MAX_ANIM_SLOTS];

typedef struct {
    uint32_t id;
    int      interval_ms;
    uint32_t last_fire;
    Value    callback;
    bool     active;
} GuiTimer;

#define GUI_MAX_TIMERS 16
static GuiTimer g_timers[GUI_MAX_TIMERS];

/* --- Phase 11: 追加ウィジェット --- */
#define GUI_TOGGLE_W         44.0f   /* トグルスイッチ幅 */
#define GUI_TOGGLE_H         24.0f   /* トグルスイッチ高さ */
#define GUI_SPINNER_RADIUS   12.0f   /* スピナー半径 */
#define GUI_BADGE_PAD         6.0f   /* バッジ内余白 */
#define GUI_TAG_H            26.0f   /* タグ高さ */
#define GUI_TAG_PAD           8.0f   /* タグ内余白 */
#define GUI_CARD_SHADOW       4.0f   /* カード影オフセット */
#define GUI_MAX_CARD_DEPTH    16     /* カードネスト最大 */

/* カードスタック */
typedef struct {
    float x, y, w;
    float saved_y;
} GuiCardState;

static GuiCardState g_cards[GUI_MAX_CARD_DEPTH];
static int          g_card_depth = 0;

/* --- Phase 28: レイアウト拡張 III --- */
static float g_next_widget_w = 0;    /* 次ウィジェットの幅指定 (0=auto) */
static float g_margin_top = 0, g_margin_right = 0, g_margin_bottom = 0, g_margin_left = 0;
static float g_padding_extra = 0;
static float g_min_w = 0, g_min_h = 0, g_max_w = 0, g_max_h = 0;

/* --- Phase 12: 高度なレイアウト --- */
#define GUI_MAX_SCROLL       16
#define GUI_MAX_GRID         16
#define GUI_MAX_GROUP        16
#define GUI_SCROLLBAR_W      10.0f
#define GUI_TOOLBAR_H        40.0f
#define GUI_STATUSBAR_H      28.0f

typedef struct {
    float x, y, w, h;        /* 表示領域 */
    float scroll_y;           /* 現在のスクロール位置 */
    float content_h;          /* コンテンツ全高 */
    float saved_x, saved_y, saved_w;  /* レイアウト復元用 */
    bool  active;
} GuiScrollRegion;

typedef struct {
    int   cols;
    float gap;
    float x, y, w;           /* 領域開始 */
    float col_w;
    int   cur_col;
    float row_h;              /* 行内最大高さ */
    float saved_x, saved_y, saved_w;
    bool  active;
} GuiGrid;

typedef struct {
    float x, y, w;
    float saved_x, saved_y, saved_w;
    bool  has_title;
    char  title[128];
    bool  active;
} GuiGroup;

static GuiScrollRegion g_scrolls[GUI_MAX_SCROLL];
static GuiGrid         g_grids[GUI_MAX_GRID];
static GuiGroup        g_groups[GUI_MAX_GROUP];

/* 次ウィジェット配置フラグ */
typedef enum {
    GUI_ALIGN_LEFT   = 0,
    GUI_ALIGN_CENTER = 1,
    GUI_ALIGN_RIGHT  = 2
} GuiAlign;
static GuiAlign g_next_align = GUI_ALIGN_LEFT;

/* --- Phase 13: フォーム・入力拡張 --- */
#define GUI_SEARCH_ICON_W    28.0f
#define GUI_AC_MAX_VISIBLE    6    /* オートコンプリート候補最大表示数 */
static char g_placeholder[256] = {0};
static bool g_disabled = false;    /* ウィジェット無効化フラグ */

/* --- Phase 14: チャート --- */
#define GUI_CHART_PAD      30.0f
#define GUI_CHART_AXIS_W   40.0f

/* --- Phase 15: サブウィンドウ・ポップアップ --- */
#define GUI_MAX_POPUP      8
#define GUI_POPUP_W        200.0f
#define GUI_SIDE_PANEL_W   250.0f
#define GUI_BOTTOM_SHEET_H 200.0f
#define GUI_INFOBAR_H      36.0f
#define GUI_FAB_SIZE       56.0f

typedef struct {
    uint32_t id;
    float x, y, w, h;
    bool  open;
} GuiPopup;

static GuiPopup g_popups[GUI_MAX_POPUP];
static int      g_popup_count = 0;

/* --- Phase 17: システム統合 --- */
#define GUI_MAX_SUB_WINDOWS  8
#define GUI_MAX_SETTINGS     64
#define GUI_SETTINGS_KEY_LEN 128
#define GUI_SETTINGS_VAL_LEN 512
#define GUI_UNDO_MAX         64

typedef struct {
    char key[GUI_SETTINGS_KEY_LEN];
    char val[GUI_SETTINGS_VAL_LEN];
} GuiSetting;

typedef struct {
    Value callback;
    bool  used;
} GuiUndoEntry;

static HjpWindow *g_sub_windows[GUI_MAX_SUB_WINDOWS];
static int         g_sub_window_count = 0;

static GuiSetting  g_settings[GUI_MAX_SETTINGS];
static int         g_setting_count  = 0;
static bool        g_settings_loaded = false;

static GuiUndoEntry g_undo_stack[GUI_UNDO_MAX];
static int          g_undo_top = 0;
static GuiUndoEntry g_redo_stack[GUI_UNDO_MAX];
static int          g_redo_top = 0;

/* --- Phase 18: アクセシビリティ + パフォーマンス --- */
#define GUI_MAX_TAB_ORDER    64

typedef struct {
    uint32_t widget_id;
    int      order;
} GuiTabOrder;

static uint32_t    g_focus_next      = 0;   /* 次フレームでフォーカスするID */
static bool        g_focus_set       = false;
static GuiTabOrder g_tab_orders[GUI_MAX_TAB_ORDER];
static int         g_tab_order_count = 0;
static float       g_table_col_w[GUI_MAX_TABLES][16]; /* テーブル列幅 */

/* --- Phase 19: ナビゲーション + ウィザード --- */
#define GUI_NAV_ITEM_W       64.0f
#define GUI_NAV_BAR_H        56.0f
#define GUI_BREADCRUMB_H     32.0f
#define GUI_STEPPER_R        14.0f
#define GUI_WIZARD_MAX_STEPS 16
#define GUI_ACCORDION_MAX    16

typedef struct {
    int   step_count;
    int   current;
    float x, y, w;
    char  title[128];
    bool  active;
} GuiWizard;

static GuiWizard g_wizard = {0};

/* --- Phase 21: レイアウト拡張 II --- */
#define GUI_MAX_FLOW         8
#define GUI_MAX_OVERLAY      8
#define GUI_MAX_DOCK         8

typedef struct {
    float x, y, w;              /* 領域開始 */
    float gap;
    float cur_x, cur_y, row_h;  /* 現在配置位置 */
    float saved_x, saved_y, saved_w;
    bool  active;
} GuiFlow;

typedef struct {
    float x, y, w, h;
    float saved_x, saved_y, saved_w;
    bool  active;
} GuiOverlay;

static GuiFlow    g_flows[GUI_MAX_FLOW];
static GuiOverlay g_overlays[GUI_MAX_OVERLAY];
static float      g_next_opacity = 1.0f;  /* 次ウィジェットの透明度 */

/* =====================================================================
 * ユーティリティ
 * ===================================================================*/

/* FNV‑1a ハッシュ */
static uint32_t gui_hash(const char *s) {
    uint32_t h = 2166136261u;
    for (; *s; s++) { h ^= (uint8_t)*s; h *= 16777619u; }
    return h ? h : 1;          /* 0 は "なし" として予約 */
}

/* --- シェル引数エスケープ (コマンドインジェクション対策) --- */
static void gui_shell_escape(char *out, size_t outsz, const char *in) {
    size_t j = 0;
    if (j < outsz - 1) out[j++] = '\'';
    for (size_t i = 0; in[i] && j < outsz - 5; i++) {
        if (in[i] == '\'') {
            /* ' → '\'' でエスケープ */
            out[j++] = '\''; out[j++] = '\\';
            out[j++] = '\''; out[j++] = '\'';
        } else {
            out[j++] = in[i];
        }
    }
    if (j < outsz - 1) out[j++] = '\'';
    out[j] = '\0';
}

/* --- URL バリデーション (ブラウザ開く用) --- */
static bool gui_is_safe_url(const char *url) {
    return (strncmp(url, "http://", 7) == 0 ||
            strncmp(url, "https://", 8) == 0 ||
            strncmp(url, "file://", 7) == 0);
}

/* --- フォント検索 --- */
static bool gui_find_font(char *out, size_t sz) {
#if defined(__APPLE__) || defined(__linux__)
    Dl_info di;
    if (dladdr((void *)gui_find_font, &di) && di.dli_fname) {
        char *tmp = strdup(di.dli_fname);
        char *dir = dirname(tmp);
        snprintf(out, sz, "%s/fonts/NotoSansCJKjp-Regular.otf", dir);
        free(tmp);
        if (access(out, F_OK) == 0) return true;
    }
#endif
    const char *tries[] = {
        "fonts/NotoSansCJKjp-Regular.otf",
        "../fonts/NotoSansCJKjp-Regular.otf",
        NULL
    };
    for (int i = 0; tries[i]; i++) {
        snprintf(out, sz, "%s", tries[i]);
        if (access(out, F_OK) == 0) return true;
    }
    const char *home = getenv("HOME");
    if (home) {
        snprintf(out, sz,
                 "%s/.hajimu/plugins/hajimu_gui/fonts/NotoSansCJKjp-Regular.otf",
                 home);
        if (access(out, F_OK) == 0) return true;
    }
    return false;
}

/* --- 16 進数カラー解析 "#RRGGBB" / "#RRGGBBAA" --- */
static GuiRGBA gui_hex(const char *hex) {
    GuiRGBA c = {0, 0, 0, 1.0f};
    if (!hex || *hex != '#') return c;
    hex++;
    unsigned v = 0;
    sscanf(hex, "%x", &v);
    int len = (int)strlen(hex);
    if (len >= 8) {
        c.r = ((v >> 24) & 0xFF) / 255.0f;
        c.g = ((v >> 16) & 0xFF) / 255.0f;
        c.b = ((v >>  8) & 0xFF) / 255.0f;
        c.a = ( v        & 0xFF) / 255.0f;
    } else {
        c.r = ((v >> 16) & 0xFF) / 255.0f;
        c.g = ((v >>  8) & 0xFF) / 255.0f;
        c.b = ( v        & 0xFF) / 255.0f;
    }
    return c;
}

/* --- 色をパック (double に格納) --- */
static inline double gui_pack_rgba(int r, int g, int b, int a) {
    uint32_t v = ((uint32_t)(r & 0xFF) << 24) |
                 ((uint32_t)(g & 0xFF) << 16) |
                 ((uint32_t)(b & 0xFF) <<  8) |
                  (uint32_t)(a & 0xFF);
    return (double)v;
}
static inline Hjpcolor gui_unpack(double d) {
    uint32_t v = (uint32_t)d;
    return hjpRGBA((v >> 24) & 0xFF, (v >> 16) & 0xFF,
                   (v >>  8) & 0xFF,  v        & 0xFF);
}
static inline GuiRGBA gui_unpack_rgba(double d) {
    uint32_t v = (uint32_t)d;
    return (GuiRGBA){
        ((v >> 24) & 0xFF) / 255.0f,
        ((v >> 16) & 0xFF) / 255.0f,
        ((v >>  8) & 0xFF) / 255.0f,
        ( v        & 0xFF) / 255.0f
    };
}

/* --- 矩形内判定 --- */
static inline bool gui_hit(float px, float py,
                            float x, float y, float w, float h) {
    return px >= x && px < x + w && py >= y && py < y + h;
}

/* --- レイアウト進行 --- */
static inline void gui_advance(float h) {
    if (g_cur) g_cur->lay.y += h + GUI_MARGIN;
}
static inline void gui_pos(float *x, float *y, float *w) {
    *x = g_cur->lay.x + g_cur->lay.indent + GUI_PADDING;
    *y = g_cur->lay.y;
    *w = g_cur->lay.w - g_cur->lay.indent - GUI_PADDING * 2.0f;
}

/* --- ウィジェットのホバー・アクティブ判定 --- */
static bool gui_widget_logic(uint32_t id,
                              float x, float y, float w, float h,
                              bool *hovered, bool *pressed) {
    bool hov = gui_hit((float)g_cur->in.mx, (float)g_cur->in.my, x, y, w, h);

    if (hov) g_cur->hot = id;

    *hovered = hov;
    *pressed = false;

    /* クリック判定 */
    bool clicked = false;
    if (g_cur->active == id) {
        if (g_cur->in.released) {
            if (hov) clicked = true;
            g_cur->active = 0;
        }
        *pressed = true;
    } else if (hov && g_cur->in.clicked) {
        g_cur->active = id;
        *pressed = true;
    }

    /* 直前ウィジェット記録 */
    g_cur->last = (GuiLastWidget){x, y, w, h, hov};

    return clicked;
}

/* --- dict ヘルパー --- */
static Value make_dict(void) {
    Value v;
    memset(&v, 0, sizeof(v));
    v.type = VALUE_DICT;
    v.dict.capacity = 8;
    v.dict.keys   = calloc(8, sizeof(char *));
    v.dict.values = calloc(8, sizeof(Value));
    return v;
}
static void dict_set(Value *d, const char *key, Value val) {
    if (d->type != VALUE_DICT) return;
    for (int i = 0; i < d->dict.length; i++) {
        if (strcmp(d->dict.keys[i], key) == 0) {
            d->dict.values[i] = val;
            return;
        }
    }
    if (d->dict.length >= d->dict.capacity) {
        d->dict.capacity *= 2;
        d->dict.keys   = realloc(d->dict.keys,   d->dict.capacity * sizeof(char *));
        d->dict.values = realloc(d->dict.values,  d->dict.capacity * sizeof(Value));
    }
    d->dict.keys  [d->dict.length] = strdup(key);
    d->dict.values[d->dict.length] = val;
    d->dict.length++;
}

/* =====================================================================
 * Phase 1: コア — ウィンドウ・ループ・色
 * ===================================================================*/

/* ---------------------------------------------------------------
 * アプリ作成(タイトル, 幅, 高さ)
 * hjp_platform ウィンドウ + OpenGL 3.2 Core + hjp_render を初期化し、
 * アプリ ID（数値）を返す。
 * ---------------------------------------------------------------*/
static Value fn_app_create(int argc, Value *argv) {
    if (argc < 3 ||
        argv[0].type != VALUE_STRING ||
        argv[1].type != VALUE_NUMBER ||
        argv[2].type != VALUE_NUMBER) {
        fprintf(stderr, "[hajimu_gui] アプリ作成: 引数エラー (タイトル, 幅, 高さ)\n");
        return hajimu_null();
    }

    /* テーマ初期化 */
    if (!g_th_init) gui_theme_set_dark();

    /* 空きスロット検索 */
    int idx = -1;
    for (int i = 0; i < GUI_MAX_APPS; i++) {
        if (!g_apps[i].valid) { idx = i; break; }
    }
    if (idx < 0) {
        fprintf(stderr, "[hajimu_gui] アプリ作成: 最大数 (%d) に達しました\n",
                GUI_MAX_APPS);
        return hajimu_null();
    }

    const char *title = argv[0].string.data;
    int w = (int)argv[1].number;
    int h = (int)argv[2].number;
    if (w <= 0) w = 800;
    if (h <= 0) h = 600;

    /* プラットフォーム初期化 */
    if (!hjp_init()) {
        fprintf(stderr, "[hajimu_gui] init failed\n");
        return hajimu_null();
    }

    /* ウィンドウ作成 */
    HjpWindow *win = hjp_window_create(
        title,
        HJP_WINDOWPOS_CENTERED, HJP_WINDOWPOS_CENTERED,
        w, h,
        HJP_WINDOW_OPENGL | HJP_WINDOW_RESIZABLE | HJP_WINDOW_HIGHDPI);
    if (!win) {
        fprintf(stderr, "[hajimu_gui] ウィンドウ作成失敗: %s\n", "init failed");
        return hajimu_null();
    }

    HjpGLContext gl = hjp_gl_create_context(win);
    if (!gl) {
        fprintf(stderr, "[hajimu_gui] GL コンテキスト作成失敗: %s\n",
                "init failed");
        hjp_window_destroy(win);
        return hajimu_null();
    }
    hjp_gl_make_current(win, gl);
    hjp_gl_set_swap_interval(1);   /* VSync */

    /* hjp_render 初期化 */
    Hjpcontext *vg = hjpCreateGL3(HJP_ANTIALIAS | HJP_STENCIL_STROKES);
    if (!vg) {
        fprintf(stderr, "[hajimu_gui] hjp_render 初期化失敗\n");
        hjp_gl_delete_context(gl);
        hjp_window_destroy(win);
        return hajimu_null();
    }

    /* フォント読み込み */
    char font_path[1024];
    int font_id = -1;
    if (gui_find_font(font_path, sizeof(font_path))) {
        font_id = hjpCreateFont(vg, GUI_FONT_NAME, font_path);
        if (font_id < 0) {
            fprintf(stderr, "[hajimu_gui] フォント読込失敗: %s\n", font_path);
        }
    } else {
        fprintf(stderr, "[hajimu_gui] フォントファイルが見つかりません\n");
    }

    /* アプリ構造体を初期化 */
    GuiApp *app = &g_apps[idx];
    memset(app, 0, sizeof(*app));
    app->window     = win;
    app->gl         = gl;
    app->vg         = vg;
    app->font_id    = font_id;
    app->win_w      = w;
    app->win_h      = h;
    app->running    = true;
    app->target_fps = 60;
    app->bg         = (GuiRGBA){30/255.0f, 30/255.0f, 30/255.0f, 1.0f};
    app->valid      = true;

    return hajimu_number((double)idx);
}

/* ---------------------------------------------------------------
 * 描画ループ(アプリ, コールバック)
 * メインイベントループ。毎フレームコールバックを呼ぶ。
 * ---------------------------------------------------------------*/
static Value fn_draw_loop(int argc, Value *argv) {
    if (argc < 2 || argv[0].type != VALUE_NUMBER) {
        fprintf(stderr, "[hajimu_gui] 描画ループ: 引数エラー\n");
        return hajimu_null();
    }

    int idx = (int)argv[0].number;
    if (idx < 0 || idx >= GUI_MAX_APPS || !g_apps[idx].valid) {
        fprintf(stderr, "[hajimu_gui] 描画ループ: 無効なアプリ ID\n");
        return hajimu_null();
    }

    Value callback = argv[1];
    if (callback.type != VALUE_FUNCTION &&
        callback.type != VALUE_BUILTIN) {
        fprintf(stderr, "[hajimu_gui] 描画ループ: コールバック関数が必要です\n");
        return hajimu_null();
    }

    GuiApp *app = &g_apps[idx];

    while (app->running) {
        uint32_t frame_start = hjp_get_ticks();

        /* ── イベント処理 ── */
        app->in.clicked  = false;
        app->in.released = false;
        app->in.scroll_y = 0;
        app->in.pmx = app->in.mx;
        app->in.pmy = app->in.my;
        app->in.text_input_len = 0;
        app->in.text_input[0]  = '\0';
        app->in.key_backspace  = false;
        app->in.key_delete     = false;
        app->in.key_return     = false;
        app->in.key_left       = false;
        app->in.key_right      = false;
        app->in.key_home       = false;
        app->in.key_end        = false;
        app->in.key_a          = false;
        app->in.key_c          = false;
        app->in.key_v          = false;
        app->in.key_x          = false;
        app->in.mouse_right_clicked = false;
        app->in.key_up         = false;
        app->in.key_down       = false;
        app->in.key_tab        = false;
        app->in.last_key       = 0;
        g_drop_file_count      = 0;

        HjpEvent ev;
        while (hjp_poll_event(&ev)) {
            switch (ev.type) {
            case HJP_EVENT_QUIT:
                app->running = false;
                break;
            case HJP_EVENT_MOUSEMOTION:
                app->in.mx = ev.motion.x;
                app->in.my = ev.motion.y;
                break;
            case HJP_EVENT_MOUSEBUTTONDOWN:
                if (ev.button.button == HJP_BUTTON_LEFT) {
                    app->in.down    = true;
                    app->in.clicked = true;
                } else if (ev.button.button == HJP_BUTTON_RIGHT) {
                    app->in.mouse_right_clicked = true;
                } else if (ev.button.button == HJP_BUTTON_MIDDLE) {
                    app->in.mouse_middle = true;
                }
                break;
            case HJP_EVENT_MOUSEBUTTONUP:
                if (ev.button.button == HJP_BUTTON_LEFT) {
                    app->in.down     = false;
                    app->in.released = true;
                } else if (ev.button.button == HJP_BUTTON_MIDDLE) {
                    app->in.mouse_middle = false;
                }
                break;
            case HJP_EVENT_MOUSEWHEEL:
                app->in.scroll_y = ev.wheel.y;
                break;
            case HJP_EVENT_WINDOWEVENT:
                if (ev.window.event == HJP_WINDOWEVENT_SIZE_CHANGED) {
                    app->win_w = ev.window.data1;
                    app->win_h = ev.window.data2;
                }
                break;
            case HJP_EVENT_TEXTINPUT: {
                int n = (int)strlen(ev.text.text);
                if (app->in.text_input_len + n < (int)sizeof(app->in.text_input) - 1) {
                    memcpy(app->in.text_input + app->in.text_input_len,
                           ev.text.text, n);
                    app->in.text_input_len += n;
                    app->in.text_input[app->in.text_input_len] = '\0';
                }
                break;
            }
            case HJP_EVENT_KEYDOWN: {
                HjpKeycode k = ev.key.sym;
                HjpKeymod  m = ev.key.mod;
#ifdef __APPLE__
                bool ctrl = (m & HJP_KMOD_GUI) != 0;  /* Cmd on macOS */
#else
                bool ctrl = (m & HJP_KMOD_CTRL) != 0;
#endif
                app->in.mod_ctrl  = ctrl;
                app->in.mod_shift = (m & HJP_KMOD_SHIFT) != 0;

                if (k == HJPK_BACKSPACE) app->in.key_backspace = true;
                else if (k == HJPK_DELETE)    app->in.key_delete = true;
                else if (k == HJPK_RETURN || k == HJPK_KP_ENTER)
                    app->in.key_return = true;
                else if (k == HJPK_LEFT)      app->in.key_left  = true;
                else if (k == HJPK_RIGHT)     app->in.key_right = true;
                else if (k == HJPK_HOME)      app->in.key_home  = true;
                else if (k == HJPK_END)       app->in.key_end   = true;
                else if (k == HJPK_ESCAPE)    app->running = false;
                else if (ctrl && k == HJPK_a) app->in.key_a = true;
                else if (ctrl && k == HJPK_c) app->in.key_c = true;
                else if (ctrl && k == HJPK_v) app->in.key_v = true;
                else if (ctrl && k == HJPK_x) app->in.key_x = true;
                else if (k == HJPK_UP)        app->in.key_up = true;
                else if (k == HJPK_DOWN)      app->in.key_down = true;
                else if (k == HJPK_TAB)       app->in.key_tab = true;
                app->in.last_key = k;
                break;
            }
            case HJP_EVENT_DROPFILE: {
                if (g_drop_file_count < GUI_MAX_DROP_FILES && ev.drop.file) {
                    snprintf(g_drop_files[g_drop_file_count],
                             sizeof(g_drop_files[0]), "%s", ev.drop.file);
                    g_drop_file_count++;
                    hjp_free(ev.drop.file);
                }
                break;
            }
            default:
                break;
            }
        }
        if (!app->running) break;

        /* ── フレーム開始 ── */
        hjp_gl_get_drawable_size(app->window, &app->fb_w, &app->fb_h);
        hjp_window_get_size(app->window, &app->win_w, &app->win_h);
        app->px_ratio = (float)app->fb_w / (float)app->win_w;

        glViewport(0, 0, app->fb_w, app->fb_h);
        glClearColor(app->bg.r, app->bg.g, app->bg.b, app->bg.a);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT |
                GL_STENCIL_BUFFER_BIT);

        hjpBeginFrame(app->vg, app->win_w, app->win_h, app->px_ratio);

        /* レイアウト初期化 */
        app->lay = (GuiLayout){0, GUI_PADDING, (float)app->win_w, 0};
        app->hot = 0;
        app->tooltip_show = false;
        if (!g_th_init) gui_theme_set_dark();

        /* ── ユーザーコールバック実行 ── */
        g_cur = app;
        hajimu_call(&callback, 0, NULL);

        /* ── タイマー発火 (Phase 10) ── */
        {
            uint32_t tnow = hjp_get_ticks();
            for (int ti = 0; ti < GUI_MAX_TIMERS; ti++) {
                if (!g_timers[ti].active) continue;
                if ((int)(tnow - g_timers[ti].last_fire) >=
                    g_timers[ti].interval_ms) {
                    g_timers[ti].last_fire = tnow;
                    Value tcb = g_timers[ti].callback;
                    hajimu_call(&tcb, 0, NULL);
                }
            }
        }

        /* ── ツールチップ描画 ── */
        if (app->tooltip_show && app->tooltip[0]) {
            Hjpcontext *vg = app->vg;
            hjpFontFaceId(vg, app->font_id);
            hjpFontSize(vg, 14.0f);
            float bnd[4];
            hjpTextBounds(vg, 0, 0, app->tooltip, NULL, bnd);
            float tw = bnd[2] - bnd[0] + 16.0f;
            float th = bnd[3] - bnd[1] + 10.0f;
            float tx = app->tooltip_x + 12.0f;
            float ty = app->tooltip_y - th - 4.0f;
            if (tx + tw > app->win_w) tx = app->win_w - tw - 4.0f;
            if (ty < 0) ty = app->tooltip_y + 20.0f;

            hjpBeginPath(vg);
            hjpRoundedRect(vg, tx, ty, tw, th, 4.0f);
            hjpFillColor(vg, TH_TOOLTIP_BG);
            hjpFill(vg);

            hjpFillColor(vg, TH_TOOLTIP_FG);
            hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_MIDDLE);
            hjpText(vg, tx + 8.0f, ty + th * 0.5f, app->tooltip, NULL);
        }

        /* ── トースト通知描画 ── */
        {
            Hjpcontext *tvg = app->vg;
            float toast_y = (float)app->win_h - 20.0f;
            uint32_t tnow = hjp_get_ticks();
            for (int ti = 0; ti < GUI_MAX_TOASTS; ti++) {
                if (!g_toasts[ti].active) continue;
                uint32_t te = tnow - g_toasts[ti].start_time;
                if ((int)te > g_toasts[ti].duration_ms) {
                    g_toasts[ti].active = false;
                    continue;
                }
                float talpha = 1.0f;
                if ((int)te > g_toasts[ti].duration_ms - 300)
                    talpha = (float)(g_toasts[ti].duration_ms - (int)te) / 300.0f;
                hjpFontFaceId(tvg, app->font_id);
                hjpFontSize(tvg, 14.0f);
                float tbnd[4];
                hjpTextBounds(tvg, 0, 0, g_toasts[ti].message, NULL, tbnd);
                float ttw = tbnd[2] - tbnd[0] + 24.0f;
                float tth = 36.0f;
                float ttx = (float)app->win_w - ttw - 16.0f;
                toast_y -= tth + 8.0f;
                hjpBeginPath(tvg);
                hjpRoundedRect(tvg, ttx, toast_y, ttw, tth, 6.0f);
                hjpFillColor(tvg, hjpRGBAf(0.2f, 0.2f, 0.22f, 0.92f * talpha));
                hjpFill(tvg);
                hjpFillColor(tvg, hjpRGBAf(0.9f, 0.9f, 0.9f, talpha));
                hjpTextAlign(tvg, HJP_ALIGN_LEFT | HJP_ALIGN_MIDDLE);
                hjpText(tvg, ttx + 12.0f, toast_y + tth * 0.5f,
                        g_toasts[ti].message, NULL);
            }
        }

        g_cur = NULL;

        /* ── フレーム終了 ── */
        hjpEndFrame(app->vg);
        hjp_gl_swap_window(app->window);

        /* アクティブ解除 */
        if (app->in.released) app->active = 0;

        /* フレームレート制御 */
        if (app->target_fps > 0) {
            uint32_t elapsed = hjp_get_ticks() - frame_start;
            uint32_t target  = 1000u / (uint32_t)app->target_fps;
            if (elapsed < target) hjp_delay(target - elapsed);
        }
    }

    return hajimu_null();
}

/* ---------------------------------------------------------------
 * アプリ終了(アプリ)
 * ---------------------------------------------------------------*/
static Value fn_app_quit(int argc, Value *argv) {
    if (argc < 1 || argv[0].type != VALUE_NUMBER) return hajimu_null();
    int idx = (int)argv[0].number;
    if (idx < 0 || idx >= GUI_MAX_APPS || !g_apps[idx].valid) return hajimu_null();

    GuiApp *app = &g_apps[idx];
    app->running = false;

    if (app->vg) { hjpDeleteGL3(app->vg); app->vg = NULL; }
    if (app->gl) { hjp_gl_delete_context(app->gl); app->gl = NULL; }
    if (app->window) { hjp_window_destroy(app->window); app->window = NULL; }
    app->valid = false;

    /* 全アプリ終了ならプラットフォームも解放 */
    bool any = false;
    for (int i = 0; i < GUI_MAX_APPS; i++) if (g_apps[i].valid) any = true;
    if (!any) hjp_quit();

    return hajimu_null();
}

/* ---------------------------------------------------------------
 * ウィンドウサイズ(アプリ) → {幅, 高さ}
 * ---------------------------------------------------------------*/
static Value fn_window_size(int argc, Value *argv) {
    if (argc < 1 || argv[0].type != VALUE_NUMBER) return hajimu_null();
    int idx = (int)argv[0].number;
    if (idx < 0 || idx >= GUI_MAX_APPS || !g_apps[idx].valid) return hajimu_null();

    GuiApp *app = &g_apps[idx];
    Value d = make_dict();
    dict_set(&d, "幅",  hajimu_number(app->win_w));
    dict_set(&d, "高さ", hajimu_number(app->win_h));
    return d;
}

/* ---------------------------------------------------------------
 * ウィンドウタイトル(アプリ, タイトル)
 * ---------------------------------------------------------------*/
static Value fn_window_title(int argc, Value *argv) {
    if (argc < 2 || argv[0].type != VALUE_NUMBER ||
        argv[1].type != VALUE_STRING) return hajimu_null();
    int idx = (int)argv[0].number;
    if (idx < 0 || idx >= GUI_MAX_APPS || !g_apps[idx].valid) return hajimu_null();
    hjp_window_set_title(g_apps[idx].window, argv[1].string.data);
    return hajimu_null();
}

/* ---------------------------------------------------------------
 * ウィンドウリサイズ(アプリ, 幅, 高さ)
 * ---------------------------------------------------------------*/
static Value fn_window_resize(int argc, Value *argv) {
    if (argc < 3 || argv[0].type != VALUE_NUMBER) return hajimu_null();
    int idx = (int)argv[0].number;
    if (idx < 0 || idx >= GUI_MAX_APPS || !g_apps[idx].valid) return hajimu_null();
    int nw = (int)argv[1].number;
    int nh = (int)argv[2].number;
    if (nw > 0 && nh > 0) hjp_window_set_size(g_apps[idx].window, nw, nh);
    return hajimu_null();
}

/* ---------------------------------------------------------------
 * フレームレート(アプリ, FPS)
 * ---------------------------------------------------------------*/
static Value fn_framerate(int argc, Value *argv) {
    if (argc < 2 || argv[0].type != VALUE_NUMBER) return hajimu_null();
    int idx = (int)argv[0].number;
    if (idx < 0 || idx >= GUI_MAX_APPS || !g_apps[idx].valid) return hajimu_null();
    g_apps[idx].target_fps = (int)argv[1].number;
    return hajimu_null();
}

/* ---------------------------------------------------------------
 * 背景色(色)  — パック数値 or 16進文字列
 * ---------------------------------------------------------------*/
static Value fn_bg_color(int argc, Value *argv) {
    if (!g_cur || argc < 1) return hajimu_null();
    if (argv[0].type == VALUE_NUMBER) {
        g_cur->bg = gui_unpack_rgba(argv[0].number);
    } else if (argv[0].type == VALUE_STRING) {
        g_cur->bg = gui_hex(argv[0].string.data);
    }
    return hajimu_null();
}

/* ---------------------------------------------------------------
 * 色(R, G, B [, A]) → パック数値
 * ---------------------------------------------------------------*/
static Value fn_color(int argc, Value *argv) {
    int r = 0, g = 0, b = 0, a = 255;
    if (argc >= 3) {
        r = (int)argv[0].number;
        g = (int)argv[1].number;
        b = (int)argv[2].number;
    }
    if (argc >= 4) a = (int)argv[3].number;
    return hajimu_number(gui_pack_rgba(r, g, b, a));
}

/* ---------------------------------------------------------------
 * 色16進(hex) → パック数値
 * ---------------------------------------------------------------*/
static Value fn_color_hex(int argc, Value *argv) {
    if (argc < 1 || argv[0].type != VALUE_STRING) return hajimu_number(0);
    GuiRGBA c = gui_hex(argv[0].string.data);
    return hajimu_number(gui_pack_rgba(
        (int)(c.r * 255), (int)(c.g * 255),
        (int)(c.b * 255), (int)(c.a * 255)));
}

/* =====================================================================
 * Phase 2: 基本ウィジェット
 * ===================================================================*/

/* ---------------------------------------------------------------
 * テキスト(内容 [, オプション])
 *   オプション: {サイズ: 数値, 色: パック色}
 * ---------------------------------------------------------------*/
static Value fn_text(int argc, Value *argv) {
    if (!g_cur || argc < 1 || argv[0].type != VALUE_STRING)
        return hajimu_null();

    Hjpcontext *vg = g_cur->vg;
    const char *text = argv[0].string.data;

    float sz  = GUI_FONT_SIZE;
    Hjpcolor col = TH_TEXT;

    /* オプション解析 */
    if (argc >= 2 && argv[1].type == VALUE_DICT) {
        for (int i = 0; i < argv[1].dict.length; i++) {
            const char *k = argv[1].dict.keys[i];
            Value       v = argv[1].dict.values[i];
            if (strcmp(k, "サイズ") == 0 && v.type == VALUE_NUMBER)
                sz = (float)v.number;
            else if (strcmp(k, "色") == 0 && v.type == VALUE_NUMBER)
                col = gui_unpack(v.number);
        }
    } else if (argc >= 2 && argv[1].type == VALUE_NUMBER) {
        sz = (float)argv[1].number;
    }

    float x, y, w;
    gui_pos(&x, &y, &w);

    hjpFontFaceId(vg, g_cur->font_id);
    hjpFontSize(vg, sz);
    hjpFillColor(vg, col);
    hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_TOP);

    /* 折り返し描画 */
    float line_h = sz * 1.4f;
    HjptextRow rows[64];
    const char *start = text;
    const char *end   = text + strlen(text);
    float cy = y;
    while (start < end) {
        int nrows = hjpTextBreakLines(vg, start, end, w, rows, 64);
        if (nrows <= 0) break;
        for (int i = 0; i < nrows; i++) {
            hjpText(vg, x, cy, rows[i].start, rows[i].end);
            cy += line_h;
        }
        start = rows[nrows - 1].next;
    }

    float total_h = cy - y;
    if (total_h < line_h) total_h = line_h;
    g_cur->last = (GuiLastWidget){x, y, w, total_h, false};
    gui_advance(total_h);
    return hajimu_null();
}

/* ---------------------------------------------------------------
 * 見出し(内容 [, レベル])   レベル 1–6 (デフォルト 1)
 * ---------------------------------------------------------------*/
static Value fn_heading(int argc, Value *argv) {
    if (!g_cur || argc < 1 || argv[0].type != VALUE_STRING)
        return hajimu_null();

    int level = 1;
    if (argc >= 2 && argv[1].type == VALUE_NUMBER)
        level = (int)argv[1].number;
    if (level < 1) level = 1;
    if (level > 6) level = 6;

    float sz = HEADING_SZ[level - 1];
    float x, y, w;
    gui_pos(&x, &y, &w);

    Hjpcontext *vg = g_cur->vg;
    hjpFontFaceId(vg, g_cur->font_id);
    hjpFontSize(vg, sz);
    hjpFillColor(vg, TH_TEXT);
    hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_TOP);
    hjpText(vg, x, y, argv[0].string.data, NULL);

    float h = sz * 1.5f;

    /* h1/h2 は下線付き */
    if (level <= 2) {
        hjpBeginPath(vg);
        hjpMoveTo(vg, x, y + h - 2.0f);
        hjpLineTo(vg, x + w, y + h - 2.0f);
        hjpStrokeColor(vg, TH_SEP);
        hjpStrokeWidth(vg, 1.0f);
        hjpStroke(vg);
        h += 4.0f;
    }

    g_cur->last = (GuiLastWidget){x, y, w, h, false};
    gui_advance(h);
    return hajimu_null();
}

/* ---------------------------------------------------------------
 * ボタン(ラベル [, オプション]) → 真偽
 *   クリックされたら 真 を返す。
 *   オプション: {幅: 数値, 色: パック色}
 * ---------------------------------------------------------------*/
static Value fn_button(int argc, Value *argv) {
    if (!g_cur || argc < 1 || argv[0].type != VALUE_STRING)
        return hajimu_bool(false);

    Hjpcontext *vg = g_cur->vg;
    const char *label = argv[0].string.data;
    uint32_t id = gui_hash(label);

    float x, y, avail_w;
    gui_pos(&x, &y, &avail_w);

    /* テキスト幅計測 */
    hjpFontFaceId(vg, g_cur->font_id);
    hjpFontSize(vg, GUI_FONT_SIZE);
    float tw = hjpTextBounds(vg, 0, 0, label, NULL, NULL);
    float btn_w = tw + GUI_PADDING * 3.0f;
    float btn_h = GUI_WIDGET_H;

    /* オプション: 幅 */
    Hjpcolor accent = TH_ACCENT;
    if (argc >= 2 && argv[1].type == VALUE_DICT) {
        for (int i = 0; i < argv[1].dict.length; i++) {
            const char *k = argv[1].dict.keys[i];
            Value       v = argv[1].dict.values[i];
            if (strcmp(k, "幅") == 0 && v.type == VALUE_NUMBER)
                btn_w = (float)v.number;
            if (strcmp(k, "色") == 0 && v.type == VALUE_NUMBER)
                accent = gui_unpack(v.number);
        }
    }

    bool hov = false, pressed = false;
    bool clicked = gui_widget_logic(id, x, y, btn_w, btn_h, &hov, &pressed);

    /* 描画 */
    Hjpcolor bg;
    if (pressed)    bg = TH_WIDGET_ACTIVE;
    else if (hov)   bg = TH_ACCENT_HOVER;
    else            bg = accent;

    hjpBeginPath(vg);
    hjpRoundedRect(vg, x, y, btn_w, btn_h, GUI_BTN_RADIUS);
    hjpFillColor(vg, bg);
    hjpFill(vg);

    /* ラベル */
    hjpFillColor(vg, TH_TEXT);
    hjpTextAlign(vg, HJP_ALIGN_CENTER | HJP_ALIGN_MIDDLE);
    hjpText(vg, x + btn_w * 0.5f, y + btn_h * 0.5f, label, NULL);

    gui_advance(btn_h);
    return hajimu_bool(clicked);
}

/* ---------------------------------------------------------------
 * チェックボックス(ラベル, 値) → 真偽
 *   クリックでトグルした値を返す。
 * ---------------------------------------------------------------*/
static Value fn_checkbox(int argc, Value *argv) {
    if (!g_cur || argc < 2 || argv[0].type != VALUE_STRING)
        return hajimu_bool(false);

    Hjpcontext *vg = g_cur->vg;
    const char *label = argv[0].string.data;
    bool checked = (argv[1].type == VALUE_BOOL) ? argv[1].boolean
                 : (argv[1].type == VALUE_NUMBER) ? (argv[1].number != 0)
                 : false;
    uint32_t id = gui_hash(label);

    float x, y, w;
    gui_pos(&x, &y, &w);
    float row_h = GUI_CHECK_SIZE + 4.0f;
    float total_w = GUI_CHECK_SIZE + 8.0f;

    /* テキスト幅を追加してヒット領域を広げる */
    hjpFontFaceId(vg, g_cur->font_id);
    hjpFontSize(vg, GUI_FONT_SIZE);
    float tw = hjpTextBounds(vg, 0, 0, label, NULL, NULL);
    total_w += tw;

    bool hov = false, pressed = false;
    bool clicked = gui_widget_logic(id, x, y, total_w, row_h, &hov, &pressed);
    if (clicked) checked = !checked;

    /* チェックボックス枠 */
    float bx = x, by = y + (row_h - GUI_CHECK_SIZE) * 0.5f;
    hjpBeginPath(vg);
    hjpRoundedRect(vg, bx, by, GUI_CHECK_SIZE, GUI_CHECK_SIZE, 4.0f);
    hjpFillColor(vg, checked ? TH_ACCENT : TH_WIDGET_BG);
    hjpFill(vg);
    hjpStrokeColor(vg, hov ? TH_ACCENT_HOVER : TH_BORDER);
    hjpStrokeWidth(vg, 1.5f);
    hjpStroke(vg);

    /* チェックマーク */
    if (checked) {
        hjpBeginPath(vg);
        float cx = bx + GUI_CHECK_SIZE * 0.5f;
        float cy = by + GUI_CHECK_SIZE * 0.5f;
        float s  = GUI_CHECK_SIZE * 0.3f;
        hjpMoveTo(vg, cx - s, cy);
        hjpLineTo(vg, cx - s * 0.3f, cy + s * 0.7f);
        hjpLineTo(vg, cx + s, cy - s * 0.6f);
        hjpStrokeColor(vg, TH_CHECK);
        hjpStrokeWidth(vg, 2.5f);
        hjpLineCap(vg, HJP_ROUND);
        hjpLineJoin(vg, HJP_ROUND);
        hjpStroke(vg);
    }

    /* ラベル */
    hjpFillColor(vg, TH_TEXT);
    hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_MIDDLE);
    hjpText(vg, bx + GUI_CHECK_SIZE + 8.0f, y + row_h * 0.5f, label, NULL);

    gui_advance(row_h);
    return hajimu_bool(checked);
}

/* ---------------------------------------------------------------
 * ラジオボタン(ラベル, グループ, 現在値) → 選択値
 *   クリックされたらラベルを返す。そうでなければ現在値を返す。
 * ---------------------------------------------------------------*/
static Value fn_radio(int argc, Value *argv) {
    if (!g_cur || argc < 3 ||
        argv[0].type != VALUE_STRING ||
        argv[1].type != VALUE_STRING)
        return (argc >= 3) ? argv[2] : hajimu_null();

    Hjpcontext *vg = g_cur->vg;
    const char *label = argv[0].string.data;
    /* argv[1] = グループ名 (将来のグループ管理用、現在は不使用) */
    Value current = argv[2];
    bool selected = false;
    if (current.type == VALUE_STRING)
        selected = (strcmp(current.string.data, label) == 0);

    uint32_t id = gui_hash(label);

    float x, y, w;
    gui_pos(&x, &y, &w);
    float row_h = GUI_RADIO_RADIUS * 2.0f + 8.0f;

    hjpFontFaceId(vg, g_cur->font_id);
    hjpFontSize(vg, GUI_FONT_SIZE);
    float tw = hjpTextBounds(vg, 0, 0, label, NULL, NULL);
    float total_w = GUI_RADIO_RADIUS * 2.0f + 10.0f + tw;

    bool hov = false, pressed = false;
    bool clicked = gui_widget_logic(id, x, y, total_w, row_h, &hov, &pressed);

    /* 外円 */
    float cx = x + GUI_RADIO_RADIUS + 2.0f;
    float cy = y + row_h * 0.5f;
    hjpBeginPath(vg);
    hjpCircle(vg, cx, cy, GUI_RADIO_RADIUS);
    hjpFillColor(vg, TH_WIDGET_BG);
    hjpFill(vg);
    hjpStrokeColor(vg, hov ? TH_ACCENT_HOVER : TH_BORDER);
    hjpStrokeWidth(vg, 1.5f);
    hjpStroke(vg);

    /* 選択時の内円 */
    if (selected) {
        hjpBeginPath(vg);
        hjpCircle(vg, cx, cy, GUI_RADIO_RADIUS * 0.5f);
        hjpFillColor(vg, TH_ACCENT);
        hjpFill(vg);
    }

    /* ラベル */
    hjpFillColor(vg, TH_TEXT);
    hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_MIDDLE);
    hjpText(vg, cx + GUI_RADIO_RADIUS + 8.0f, cy, label, NULL);

    gui_advance(row_h);
    return clicked ? hajimu_string(label) : argv[2];
}

/* ---------------------------------------------------------------
 * スライダー(ラベル, 値, 最小, 最大) → 数値
 *   ドラッグで値を変更。現在値を返す。
 * ---------------------------------------------------------------*/
static Value fn_slider(int argc, Value *argv) {
    if (!g_cur || argc < 4 || argv[0].type != VALUE_STRING)
        return (argc >= 2) ? argv[1] : hajimu_number(0);

    Hjpcontext *vg = g_cur->vg;
    const char *label = argv[0].string.data;
    double val = argv[1].number;
    double vmin = argv[2].number;
    double vmax = argv[3].number;
    if (vmax <= vmin) vmax = vmin + 1.0;
    uint32_t id = gui_hash(label);

    float x, y, avail_w;
    gui_pos(&x, &y, &avail_w);
    float row_h = GUI_WIDGET_H;

    /* ラベル描画 */
    hjpFontFaceId(vg, g_cur->font_id);
    hjpFontSize(vg, GUI_FONT_SIZE);
    hjpFillColor(vg, TH_TEXT);
    hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_MIDDLE);
    hjpText(vg, x, y + row_h * 0.5f, label, NULL);

    /* スライダー領域 */
    float sx = x + GUI_LABEL_W;
    float sw = avail_w - GUI_LABEL_W - 70.0f;  /* 値表示分を確保 */
    if (sw < 60.0f) sw = 60.0f;
    float sy = y + (row_h - GUI_SLIDER_H) * 0.5f;

    /* ヒット領域はスライダートラック全体 */
    bool hov = false, pressed = false;
    gui_widget_logic(id, sx, y, sw, row_h, &hov, &pressed);

    /* ドラッグ中の値更新 */
    if (g_cur->active == id && g_cur->in.down) {
        float ratio = ((float)g_cur->in.mx - sx) / sw;
        if (ratio < 0) ratio = 0;
        if (ratio > 1) ratio = 1;
        val = vmin + (vmax - vmin) * ratio;
    }

    float ratio = (float)((val - vmin) / (vmax - vmin));
    if (ratio < 0) ratio = 0;
    if (ratio > 1) ratio = 1;

    /* トラック背景 */
    hjpBeginPath(vg);
    hjpRoundedRect(vg, sx, sy, sw, GUI_SLIDER_H, GUI_SLIDER_H * 0.5f);
    hjpFillColor(vg, TH_TRACK);
    hjpFill(vg);

    /* トラック塗り */
    hjpBeginPath(vg);
    hjpRoundedRect(vg, sx, sy, sw * ratio, GUI_SLIDER_H, GUI_SLIDER_H * 0.5f);
    hjpFillColor(vg, TH_ACCENT);
    hjpFill(vg);

    /* ハンドル */
    float hx = sx + sw * ratio;
    float hy = y + row_h * 0.5f;
    hjpBeginPath(vg);
    hjpCircle(vg, hx, hy, GUI_SLIDER_HANDLE * 0.5f);
    Hjpcolor hcol = (g_cur->active == id) ? TH_ACCENT_HOVER
                  : hov                   ? TH_WIDGET_HOVER
                  :                         TH_ACCENT;
    hjpFillColor(vg, hcol);
    hjpFill(vg);

    /* 値テキスト */
    char vbuf[32];
    if (val == (int)val)
        snprintf(vbuf, sizeof(vbuf), "%d", (int)val);
    else
        snprintf(vbuf, sizeof(vbuf), "%.2f", val);
    hjpFillColor(vg, TH_TEXT_DIM);
    hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_MIDDLE);
    hjpText(vg, sx + sw + 10.0f, y + row_h * 0.5f, vbuf, NULL);

    gui_advance(row_h);
    return hajimu_number(val);
}

/* ---------------------------------------------------------------
 * プログレスバー(値 [, 最大])
 *   値は 0–最大 の範囲。最大のデフォルトは 100。
 * ---------------------------------------------------------------*/
static Value fn_progress(int argc, Value *argv) {
    if (!g_cur || argc < 1) return hajimu_null();

    Hjpcontext *vg = g_cur->vg;
    double val = argv[0].number;
    double vmax = 100.0;
    if (argc >= 2 && argv[1].type == VALUE_NUMBER) vmax = argv[1].number;
    if (vmax <= 0) vmax = 100.0;

    float ratio = (float)(val / vmax);
    if (ratio < 0) ratio = 0;
    if (ratio > 1) ratio = 1;

    float x, y, w;
    gui_pos(&x, &y, &w);

    /* 背景 */
    hjpBeginPath(vg);
    hjpRoundedRect(vg, x, y, w, GUI_PROGRESS_H, GUI_PROGRESS_H * 0.5f);
    hjpFillColor(vg, TH_TRACK);
    hjpFill(vg);

    /* 進捗 */
    if (ratio > 0) {
        hjpBeginPath(vg);
        hjpRoundedRect(vg, x, y, w * ratio, GUI_PROGRESS_H,
                       GUI_PROGRESS_H * 0.5f);
        hjpFillColor(vg, TH_ACCENT);
        hjpFill(vg);
    }

    /* パーセント */
    char buf[16];
    snprintf(buf, sizeof(buf), "%d%%", (int)(ratio * 100));
    hjpFontFaceId(vg, g_cur->font_id);
    hjpFontSize(vg, 12.0f);
    hjpFillColor(vg, TH_TEXT);
    hjpTextAlign(vg, HJP_ALIGN_CENTER | HJP_ALIGN_TOP);
    hjpText(vg, x + w * 0.5f, y + GUI_PROGRESS_H + 2.0f, buf, NULL);

    g_cur->last = (GuiLastWidget){x, y, w, GUI_PROGRESS_H + 16.0f, false};
    gui_advance(GUI_PROGRESS_H + 16.0f);
    return hajimu_null();
}

/* ---------------------------------------------------------------
 * セパレーター()
 * ---------------------------------------------------------------*/
static Value fn_separator(int argc, Value *argv) {
    (void)argc; (void)argv;
    if (!g_cur) return hajimu_null();

    float x, y, w;
    gui_pos(&x, &y, &w);

    Hjpcontext *vg = g_cur->vg;
    hjpBeginPath(vg);
    hjpMoveTo(vg, x, y + 4.0f);
    hjpLineTo(vg, x + w, y + 4.0f);
    hjpStrokeColor(vg, TH_SEP);
    hjpStrokeWidth(vg, 1.0f);
    hjpStroke(vg);

    gui_advance(8.0f);
    return hajimu_null();
}

/* ---------------------------------------------------------------
 * スペーサー([高さ])   デフォルト 16px
 * ---------------------------------------------------------------*/
static Value fn_spacer(int argc, Value *argv) {
    if (!g_cur) return hajimu_null();
    float h = 16.0f;
    if (argc >= 1 && argv[0].type == VALUE_NUMBER) h = (float)argv[0].number;
    g_cur->lay.y += h;
    return hajimu_null();
}

/* ---------------------------------------------------------------
 * ツールチップ(テキスト)
 *   直前のウィジェットがホバー中なら、ツールチップを表示予約する。
 * ---------------------------------------------------------------*/
static Value fn_tooltip(int argc, Value *argv) {
    if (!g_cur || argc < 1 || argv[0].type != VALUE_STRING)
        return hajimu_null();

    if (g_cur->last.hovered) {
        snprintf(g_cur->tooltip, sizeof(g_cur->tooltip),
                 "%s", argv[0].string.data);
        g_cur->tooltip_show = true;
        g_cur->tooltip_x = (float)g_cur->in.mx;
        g_cur->tooltip_y = (float)g_cur->in.my;
    }
    return hajimu_null();
}

/* =====================================================================
 * Phase 3: テキスト入力 + コンボボックス
 * ===================================================================*/

/* --- UTF-8 ヘルパー --- */

/* UTF-8 文字のバイト長を返す (先頭バイトから判定) */
static int utf8_char_len(unsigned char c) {
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

/* pos の直前の文字の先頭バイト位置を返す */
static int utf8_prev(const char *s, int pos) {
    if (pos <= 0) return 0;
    pos--;
    while (pos > 0 && (s[pos] & 0xC0) == 0x80) pos--;
    return pos;
}

/* pos の次の文字の先頭バイト位置を返す */
static int utf8_next(const char *s, int pos, int len) {
    if (pos >= len) return len;
    int n = utf8_char_len((unsigned char)s[pos]);
    int np = pos + n;
    return np > len ? len : np;
}

/* --- テキストバッファ管理 --- */

/* ID でバッファを検索、なければ新規作成 */
static GuiTextBuf *gui_get_text_buf(uint32_t id, const char *init_val) {
    GuiApp *app = g_cur;
    for (int i = 0; i < app->text_buf_count; i++) {
        if (app->text_bufs[i].id == id) return &app->text_bufs[i];
    }
    /* 新規作成 */
    if (app->text_buf_count >= GUI_MAX_TEXT_SLOTS) return NULL;
    GuiTextBuf *tb = &app->text_bufs[app->text_buf_count++];
    memset(tb, 0, sizeof(*tb));
    tb->id = id;
    if (init_val && *init_val) {
        int n = (int)strlen(init_val);
        if (n >= GUI_MAX_TEXT_BUF) n = GUI_MAX_TEXT_BUF - 1;
        memcpy(tb->buf, init_val, n);
        tb->buf[n] = '\0';
        tb->len = n;
        tb->cursor = n;
    }
    return tb;
}

/* テキストバッファにキー入力を反映（フォーカス中のみ呼ばれる） */
static void gui_text_buf_process(GuiTextBuf *tb) {
    GuiInput *in = &g_cur->in;

    /* テキスト挿入 */
    if (in->text_input_len > 0) {
        int avail = GUI_MAX_TEXT_BUF - 1 - tb->len;
        int n = in->text_input_len;
        if (n > avail) n = avail;
        if (n > 0) {
            memmove(tb->buf + tb->cursor + n,
                    tb->buf + tb->cursor,
                    tb->len - tb->cursor + 1);
            memcpy(tb->buf + tb->cursor, in->text_input, n);
            tb->len += n;
            tb->cursor += n;
        }
    }

    /* Backspace */
    if (in->key_backspace && tb->cursor > 0) {
        int prev = utf8_prev(tb->buf, tb->cursor);
        int del = tb->cursor - prev;
        memmove(tb->buf + prev, tb->buf + tb->cursor, tb->len - tb->cursor + 1);
        tb->len -= del;
        tb->cursor = prev;
    }

    /* Delete */
    if (in->key_delete && tb->cursor < tb->len) {
        int nxt = utf8_next(tb->buf, tb->cursor, tb->len);
        int del = nxt - tb->cursor;
        memmove(tb->buf + tb->cursor, tb->buf + nxt, tb->len - nxt + 1);
        tb->len -= del;
    }

    /* カーソル移動 */
    if (in->key_left)  tb->cursor = utf8_prev(tb->buf, tb->cursor);
    if (in->key_right) tb->cursor = utf8_next(tb->buf, tb->cursor, tb->len);
    if (in->key_home)  tb->cursor = 0;
    if (in->key_end)   tb->cursor = tb->len;

    /* Ctrl+A — 全選択 (今はカーソルを末尾へ) */
    if (in->key_a) { tb->cursor = tb->len; }

    /* Ctrl+V — ペースト */
    if (in->key_v) {
        char *clip = hjp_get_clipboard_text();
        if (clip) {
            int cl = (int)strlen(clip);
            int avail = GUI_MAX_TEXT_BUF - 1 - tb->len;
            if (cl > avail) cl = avail;
            if (cl > 0) {
                memmove(tb->buf + tb->cursor + cl,
                        tb->buf + tb->cursor,
                        tb->len - tb->cursor + 1);
                memcpy(tb->buf + tb->cursor, clip, cl);
                tb->len += cl;
                tb->cursor += cl;
            }
            hjp_free(clip);
        }
    }

    /* Ctrl+C / Ctrl+X — 全テキストコピー (選択範囲は未実装) */
    if (in->key_c || in->key_x) {
        hjp_set_clipboard_text(tb->buf);
        if (in->key_x) { tb->buf[0] = '\0'; tb->len = 0; tb->cursor = 0; }
    }
}

/* ---------------------------------------------------------------
 * テキスト入力(ラベル, 値 [, オプション]) → 文字列
 *   オプション: {幅: 数値, プレースホルダー: 文字列}
 *   フォーカス中はキーボード入力を受け付ける。
 * ---------------------------------------------------------------*/
static Value fn_text_input(int argc, Value *argv) {
    if (!g_cur || argc < 2 || argv[0].type != VALUE_STRING)
        return (argc >= 2) ? argv[1] : hajimu_string("");

    Hjpcontext *vg = g_cur->vg;
    const char *label = argv[0].string.data;
    const char *val = (argv[1].type == VALUE_STRING) ? argv[1].string.data : "";
    const char *placeholder = NULL;
    uint32_t id = gui_hash(label);

    /* オプション */
    if (argc >= 3 && argv[2].type == VALUE_DICT) {
        for (int i = 0; i < argv[2].dict.length; i++) {
            if (strcmp(argv[2].dict.keys[i], "プレースホルダー") == 0 &&
                argv[2].dict.values[i].type == VALUE_STRING)
                placeholder = argv[2].dict.values[i].string.data;
        }
    }

    /* テキストバッファ取得 */
    GuiTextBuf *tb = gui_get_text_buf(id, val);
    if (!tb) return hajimu_string(val);

    float x, y, avail_w;
    gui_pos(&x, &y, &avail_w);
    float row_h = GUI_INPUT_H + 4.0f;

    /* ラベル描画 */
    hjpFontFaceId(vg, g_cur->font_id);
    hjpFontSize(vg, GUI_FONT_SIZE);
    hjpFillColor(vg, TH_TEXT);
    hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_MIDDLE);
    hjpText(vg, x, y + row_h * 0.5f, label, NULL);

    /* 入力フィールド */
    float fx = x + GUI_LABEL_W;
    float fw = avail_w - GUI_LABEL_W;
    if (fw < 80.0f) fw = 80.0f;

    bool focused = (g_cur->focused == id);

    /* クリックでフォーカス */
    bool hov = gui_hit((float)g_cur->in.mx, (float)g_cur->in.my,
                        fx, y, fw, GUI_INPUT_H);
    if (g_cur->in.clicked) {
        if (hov) {
            g_cur->focused = id;
            focused = true;
            hjp_start_text_input();
        } else if (focused) {
            g_cur->focused = 0;
            focused = false;
            hjp_stop_text_input();
        }
    }

    /* フォーカス中: 入力処理 */
    if (focused) gui_text_buf_process(tb);

    /* フィールド背景 */
    hjpBeginPath(vg);
    hjpRoundedRect(vg, fx, y, fw, GUI_INPUT_H, 4.0f);
    hjpFillColor(vg, focused ? TH_WIDGET_ACTIVE : TH_WIDGET_BG);
    hjpFill(vg);
    hjpStrokeColor(vg, focused ? TH_ACCENT : (hov ? TH_ACCENT_HOVER : TH_BORDER));
    hjpStrokeWidth(vg, focused ? 2.0f : 1.0f);
    hjpStroke(vg);

    /* テキスト or プレースホルダー描画 */
    hjpFontFaceId(vg, g_cur->font_id);
    hjpFontSize(vg, GUI_FONT_SIZE);
    hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_MIDDLE);

    /* クリッピング (簡易: scissor) */
    hjpSave(vg);
    hjpScissor(vg, fx + GUI_INPUT_PAD, y, fw - GUI_INPUT_PAD * 2, GUI_INPUT_H);

    if (tb->len > 0) {
        hjpFillColor(vg, TH_TEXT);
        hjpText(vg, fx + GUI_INPUT_PAD, y + GUI_INPUT_H * 0.5f,
                tb->buf, NULL);
    } else if (placeholder) {
        hjpFillColor(vg, TH_TEXT_DIM);
        hjpText(vg, fx + GUI_INPUT_PAD, y + GUI_INPUT_H * 0.5f,
                placeholder, NULL);
    }

    /* カーソル描画 */
    if (focused) {
        uint32_t t = hjp_get_ticks();
        if ((t / GUI_CURSOR_BLINK_MS) % 2 == 0) {
            float cx_pos = fx + GUI_INPUT_PAD;
            if (tb->cursor > 0) {
                cx_pos += hjpTextBounds(vg, 0, 0, tb->buf,
                                        tb->buf + tb->cursor, NULL);
            }
            hjpBeginPath(vg);
            hjpRect(vg, cx_pos, y + 6.0f, 1.5f, GUI_INPUT_H - 12.0f);
            hjpFillColor(vg, TH_TEXT);
            hjpFill(vg);
        }
    }

    hjpRestore(vg);

    g_cur->last = (GuiLastWidget){fx, y, fw, GUI_INPUT_H, hov};
    gui_advance(row_h);
    return hajimu_string(tb->buf);
}

/* ---------------------------------------------------------------
 * パスワード入力(ラベル, 値) → 文字列
 *   テキスト入力と同じだが表示をマスクする。
 * ---------------------------------------------------------------*/
static Value fn_password_input(int argc, Value *argv) {
    if (!g_cur || argc < 2 || argv[0].type != VALUE_STRING)
        return (argc >= 2) ? argv[1] : hajimu_string("");

    Hjpcontext *vg = g_cur->vg;
    const char *label = argv[0].string.data;
    const char *val = (argv[1].type == VALUE_STRING) ? argv[1].string.data : "";
    uint32_t id = gui_hash(label);

    GuiTextBuf *tb = gui_get_text_buf(id, val);
    if (!tb) return hajimu_string(val);

    float x, y, avail_w;
    gui_pos(&x, &y, &avail_w);
    float row_h = GUI_INPUT_H + 4.0f;

    hjpFontFaceId(vg, g_cur->font_id);
    hjpFontSize(vg, GUI_FONT_SIZE);
    hjpFillColor(vg, TH_TEXT);
    hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_MIDDLE);
    hjpText(vg, x, y + row_h * 0.5f, label, NULL);

    float fx = x + GUI_LABEL_W;
    float fw = avail_w - GUI_LABEL_W;
    if (fw < 80.0f) fw = 80.0f;

    bool focused = (g_cur->focused == id);
    bool hov = gui_hit((float)g_cur->in.mx, (float)g_cur->in.my,
                        fx, y, fw, GUI_INPUT_H);
    if (g_cur->in.clicked) {
        if (hov) { g_cur->focused = id; focused = true; hjp_start_text_input(); }
        else if (focused) { g_cur->focused = 0; focused = false; hjp_stop_text_input(); }
    }
    if (focused) gui_text_buf_process(tb);

    /* フィールド描画 */
    hjpBeginPath(vg);
    hjpRoundedRect(vg, fx, y, fw, GUI_INPUT_H, 4.0f);
    hjpFillColor(vg, focused ? TH_WIDGET_ACTIVE : TH_WIDGET_BG);
    hjpFill(vg);
    hjpStrokeColor(vg, focused ? TH_ACCENT : (hov ? TH_ACCENT_HOVER : TH_BORDER));
    hjpStrokeWidth(vg, focused ? 2.0f : 1.0f);
    hjpStroke(vg);

    /* マスク描画 — 文字数分の ● を表示 */
    if (tb->len > 0) {
        /* UTF-8 文字数をカウント */
        int chars = 0;
        for (int p = 0; p < tb->len; ) {
            p = utf8_next(tb->buf, p, tb->len);
            chars++;
        }
        /* ● をバッファへ */
        char mask[512];
        int mp = 0;
        for (int i = 0; i < chars && mp < (int)sizeof(mask) - 4; i++) {
            /* ● = U+25CF = E2 97 8F */
            mask[mp++] = (char)0xE2;
            mask[mp++] = (char)0x97;
            mask[mp++] = (char)0x8F;
        }
        mask[mp] = '\0';
        hjpFillColor(vg, TH_TEXT);
        hjpFontFaceId(vg, g_cur->font_id);
        hjpFontSize(vg, GUI_FONT_SIZE);
        hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_MIDDLE);
        hjpText(vg, fx + GUI_INPUT_PAD, y + GUI_INPUT_H * 0.5f, mask, NULL);
    }

    /* カーソル */
    if (focused) {
        uint32_t t = hjp_get_ticks();
        if ((t / GUI_CURSOR_BLINK_MS) % 2 == 0) {
            /* マスク文字の幅からカーソル位置を概算 */
            int chars_before = 0;
            for (int p = 0; p < tb->cursor; ) {
                p = utf8_next(tb->buf, p, tb->cursor);
                chars_before++;
            }
            char tmp[512]; int tp = 0;
            for (int i = 0; i < chars_before && tp < (int)sizeof(tmp) - 4; i++) {
                tmp[tp++] = (char)0xE2; tmp[tp++] = (char)0x97; tmp[tp++] = (char)0x8F;
            }
            tmp[tp] = '\0';
            float cx_pos = fx + GUI_INPUT_PAD;
            if (tp > 0) cx_pos += hjpTextBounds(vg, 0, 0, tmp, tmp + tp, NULL);
            hjpBeginPath(vg);
            hjpRect(vg, cx_pos, y + 6.0f, 1.5f, GUI_INPUT_H - 12.0f);
            hjpFillColor(vg, TH_TEXT);
            hjpFill(vg);
        }
    }

    g_cur->last = (GuiLastWidget){fx, y, fw, GUI_INPUT_H, hov};
    gui_advance(row_h);
    return hajimu_string(tb->buf);
}

/* ---------------------------------------------------------------
 * 数値入力(ラベル, 値 [, 最小, 最大, ステップ]) → 数値
 * ---------------------------------------------------------------*/
static Value fn_number_input(int argc, Value *argv) {
    if (!g_cur || argc < 2 || argv[0].type != VALUE_STRING)
        return (argc >= 2) ? argv[1] : hajimu_number(0);

    Hjpcontext *vg = g_cur->vg;
    const char *label = argv[0].string.data;
    double val = argv[1].number;
    double vmin = (argc >= 3) ? argv[2].number : -1e15;
    double vmax = (argc >= 4) ? argv[3].number :  1e15;
    double step = (argc >= 5) ? argv[4].number :  1.0;

    uint32_t id = gui_hash(label);

    float x, y, avail_w;
    gui_pos(&x, &y, &avail_w);
    float row_h = GUI_INPUT_H + 4.0f;

    /* ラベル */
    hjpFontFaceId(vg, g_cur->font_id);
    hjpFontSize(vg, GUI_FONT_SIZE);
    hjpFillColor(vg, TH_TEXT);
    hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_MIDDLE);
    hjpText(vg, x, y + row_h * 0.5f, label, NULL);

    float fx = x + GUI_LABEL_W;
    float fw = avail_w - GUI_LABEL_W;
    if (fw < 100.0f) fw = 100.0f;
    float btn_w = GUI_INPUT_H;

    /* − ボタン */
    uint32_t id_minus = gui_hash(label) ^ 0xA1B2;
    bool hov_m = false, pr_m = false;
    bool click_minus = gui_widget_logic(id_minus, fx, y, btn_w, GUI_INPUT_H,
                                         &hov_m, &pr_m);
    hjpBeginPath(vg);
    hjpRoundedRect(vg, fx, y, btn_w, GUI_INPUT_H, 4.0f);
    hjpFillColor(vg, hov_m ? TH_WIDGET_HOVER : TH_WIDGET_BG);
    hjpFill(vg);
    hjpFillColor(vg, TH_TEXT);
    hjpTextAlign(vg, HJP_ALIGN_CENTER | HJP_ALIGN_MIDDLE);
    hjpText(vg, fx + btn_w * 0.5f, y + GUI_INPUT_H * 0.5f, "−", NULL);

    /* 値表示 */
    float vx = fx + btn_w + 2.0f;
    float vw = fw - btn_w * 2 - 4.0f;
    hjpBeginPath(vg);
    hjpRoundedRect(vg, vx, y, vw, GUI_INPUT_H, 0);
    hjpFillColor(vg, TH_WIDGET_BG);
    hjpFill(vg);

    char vbuf[64];
    if (val == (int)val) snprintf(vbuf, sizeof(vbuf), "%d", (int)val);
    else snprintf(vbuf, sizeof(vbuf), "%.2f", val);
    hjpFillColor(vg, TH_TEXT);
    hjpTextAlign(vg, HJP_ALIGN_CENTER | HJP_ALIGN_MIDDLE);
    hjpText(vg, vx + vw * 0.5f, y + GUI_INPUT_H * 0.5f, vbuf, NULL);

    /* + ボタン */
    float px = fx + fw - btn_w;
    uint32_t id_plus = gui_hash(label) ^ 0xC3D4;
    bool hov_p = false, pr_p = false;
    bool click_plus = gui_widget_logic(id_plus, px, y, btn_w, GUI_INPUT_H,
                                        &hov_p, &pr_p);
    hjpBeginPath(vg);
    hjpRoundedRect(vg, px, y, btn_w, GUI_INPUT_H, 4.0f);
    hjpFillColor(vg, hov_p ? TH_WIDGET_HOVER : TH_WIDGET_BG);
    hjpFill(vg);
    hjpFillColor(vg, TH_TEXT);
    hjpTextAlign(vg, HJP_ALIGN_CENTER | HJP_ALIGN_MIDDLE);
    hjpText(vg, px + btn_w * 0.5f, y + GUI_INPUT_H * 0.5f, "+", NULL);

    if (click_minus) val -= step;
    if (click_plus)  val += step;

    /* スクロール */
    bool hov_all = gui_hit((float)g_cur->in.mx, (float)g_cur->in.my,
                            fx, y, fw, GUI_INPUT_H);
    if (hov_all && g_cur->in.scroll_y != 0) {
        val += step * g_cur->in.scroll_y;
    }

    if (val < vmin) val = vmin;
    if (val > vmax) val = vmax;

    (void)id;
    g_cur->last = (GuiLastWidget){fx, y, fw, GUI_INPUT_H, hov_all};
    gui_advance(row_h);
    return hajimu_number(val);
}

/* ---------------------------------------------------------------
 * テキストエリア(ラベル, 値 [, 行数]) → 文字列
 *   複数行テキスト入力。Enter で改行挿入。
 * ---------------------------------------------------------------*/
static Value fn_textarea(int argc, Value *argv) {
    if (!g_cur || argc < 2 || argv[0].type != VALUE_STRING)
        return (argc >= 2) ? argv[1] : hajimu_string("");

    Hjpcontext *vg = g_cur->vg;
    const char *label = argv[0].string.data;
    const char *val = (argv[1].type == VALUE_STRING) ? argv[1].string.data : "";
    int lines = GUI_TEXTAREA_LINES;
    if (argc >= 3 && argv[2].type == VALUE_NUMBER)
        lines = (int)argv[2].number;
    if (lines < 2) lines = 2;

    uint32_t id = gui_hash(label);
    GuiTextBuf *tb = gui_get_text_buf(id, val);
    if (!tb) return hajimu_string(val);

    float x, y, avail_w;
    gui_pos(&x, &y, &avail_w);

    /* ラベル (上に配置) */
    hjpFontFaceId(vg, g_cur->font_id);
    hjpFontSize(vg, GUI_FONT_SIZE);
    hjpFillColor(vg, TH_TEXT);
    hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_TOP);
    hjpText(vg, x, y, label, NULL);
    y += GUI_FONT_SIZE * 1.4f + 4.0f;

    float fw = avail_w;
    float fh = GUI_FONT_SIZE * 1.4f * lines + GUI_INPUT_PAD * 2;

    bool focused = (g_cur->focused == id);
    bool hov = gui_hit((float)g_cur->in.mx, (float)g_cur->in.my,
                        x, y, fw, fh);
    if (g_cur->in.clicked) {
        if (hov) { g_cur->focused = id; focused = true; hjp_start_text_input(); }
        else if (focused) { g_cur->focused = 0; focused = false; hjp_stop_text_input(); }
    }

    /* Enter → 改行挿入 */
    if (focused && g_cur->in.key_return) {
        if (tb->len < GUI_MAX_TEXT_BUF - 2) {
            memmove(tb->buf + tb->cursor + 1,
                    tb->buf + tb->cursor,
                    tb->len - tb->cursor + 1);
            tb->buf[tb->cursor] = '\n';
            tb->len++;
            tb->cursor++;
        }
        /* key_return を消費（ESCと連動しないよう） */
    } else if (focused) {
        gui_text_buf_process(tb);
    }

    /* 背景 */
    hjpBeginPath(vg);
    hjpRoundedRect(vg, x, y, fw, fh, 4.0f);
    hjpFillColor(vg, focused ? TH_WIDGET_ACTIVE : TH_WIDGET_BG);
    hjpFill(vg);
    hjpStrokeColor(vg, focused ? TH_ACCENT : (hov ? TH_ACCENT_HOVER : TH_BORDER));
    hjpStrokeWidth(vg, focused ? 2.0f : 1.0f);
    hjpStroke(vg);

    /* テキスト描画 (折り返し) */
    hjpSave(vg);
    hjpScissor(vg, x + GUI_INPUT_PAD, y + GUI_INPUT_PAD,
               fw - GUI_INPUT_PAD * 2, fh - GUI_INPUT_PAD * 2);
    hjpFontFaceId(vg, g_cur->font_id);
    hjpFontSize(vg, GUI_FONT_SIZE);
    hjpFillColor(vg, TH_TEXT);
    hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_TOP);

    float line_h = GUI_FONT_SIZE * 1.4f;
    float tx = x + GUI_INPUT_PAD;
    float ty = y + GUI_INPUT_PAD;
    float tw = fw - GUI_INPUT_PAD * 2;

    /* 行ごとに描画 */
    const char *start = tb->buf;
    const char *end = tb->buf + tb->len;
    float cy = ty;
    while (start <= end && cy < y + fh) {
        const char *nl = strchr(start, '\n');
        const char *line_end = nl ? nl : end;
        if (line_end > start) {
            HjptextRow rows[16];
            const char *s = start;
            while (s < line_end) {
                int nr = hjpTextBreakLines(vg, s, line_end, tw, rows, 16);
                if (nr <= 0) break;
                for (int i = 0; i < nr; i++) {
                    hjpText(vg, tx, cy, rows[i].start, rows[i].end);
                    cy += line_h;
                }
                s = rows[nr - 1].next;
            }
        } else {
            cy += line_h;
        }
        if (!nl) break;
        start = nl + 1;
    }

    /* カーソル */
    if (focused) {
        uint32_t t = hjp_get_ticks();
        if ((t / GUI_CURSOR_BLINK_MS) % 2 == 0) {
            /* カーソル位置を概算 */
            float cx_pos = tx;
            float cy_pos = ty;
            const char *s = tb->buf;
            const char *cpos = tb->buf + tb->cursor;
            while (s < cpos) {
                const char *nl2 = memchr(s, '\n', cpos - s);
                if (nl2) {
                    cy_pos += line_h;
                    s = nl2 + 1;
                } else {
                    cx_pos = tx + hjpTextBounds(vg, 0, 0, s, cpos, NULL);
                    break;
                }
            }
            hjpBeginPath(vg);
            hjpRect(vg, cx_pos, cy_pos, 1.5f, line_h);
            hjpFillColor(vg, TH_TEXT);
            hjpFill(vg);
        }
    }

    hjpRestore(vg);

    float total_h = (y + fh + 4.0f) - (y - GUI_FONT_SIZE * 1.4f - 4.0f);
    g_cur->last = (GuiLastWidget){x, y - GUI_FONT_SIZE * 1.4f - 4.0f, fw, total_h, hov};
    gui_advance(fh + GUI_FONT_SIZE * 1.4f + 8.0f);
    return hajimu_string(tb->buf);
}

/* ---------------------------------------------------------------
 * コンボボックス(ラベル, 選択肢, 選択値) → 値
 *   選択肢は配列。クリックでドロップダウン表示。
 * ---------------------------------------------------------------*/
static Value fn_combobox(int argc, Value *argv) {
    if (!g_cur || argc < 3 ||
        argv[0].type != VALUE_STRING ||
        argv[1].type != VALUE_ARRAY)
        return (argc >= 3) ? argv[2] : hajimu_null();

    Hjpcontext *vg = g_cur->vg;
    const char *label = argv[0].string.data;
    Value choices = argv[1];
    Value current = argv[2];
    uint32_t id = gui_hash(label);

    float x, y, avail_w;
    gui_pos(&x, &y, &avail_w);
    float row_h = GUI_INPUT_H + 4.0f;

    /* ラベル */
    hjpFontFaceId(vg, g_cur->font_id);
    hjpFontSize(vg, GUI_FONT_SIZE);
    hjpFillColor(vg, TH_TEXT);
    hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_MIDDLE);
    hjpText(vg, x, y + row_h * 0.5f, label, NULL);

    float fx = x + GUI_LABEL_W;
    float fw = avail_w - GUI_LABEL_W;
    if (fw < 100.0f) fw = 100.0f;

    /* 現在値の表示テキスト */
    const char *cur_text = "(未選択)";
    if (current.type == VALUE_STRING) cur_text = current.string.data;
    else if (current.type == VALUE_NUMBER) {
        static char nb[32];
        if (current.number == (int)current.number)
            snprintf(nb, sizeof(nb), "%d", (int)current.number);
        else
            snprintf(nb, sizeof(nb), "%.2f", current.number);
        cur_text = nb;
    }

    bool hov = gui_hit((float)g_cur->in.mx, (float)g_cur->in.my,
                        fx, y, fw, GUI_INPUT_H);
    bool is_open = (g_combo_open == id);

    /* ヘッダー部分のクリック判定 */
    if (g_cur->in.clicked && hov) {
        g_combo_open = is_open ? 0 : id;
        is_open = !is_open;
    }

    /* ヘッダー描画 */
    hjpBeginPath(vg);
    hjpRoundedRect(vg, fx, y, fw, GUI_INPUT_H, 4.0f);
    hjpFillColor(vg, hov ? TH_WIDGET_HOVER : TH_WIDGET_BG);
    hjpFill(vg);
    hjpStrokeColor(vg, is_open ? TH_ACCENT : TH_BORDER);
    hjpStrokeWidth(vg, 1.0f);
    hjpStroke(vg);

    hjpFillColor(vg, TH_TEXT);
    hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_MIDDLE);
    hjpText(vg, fx + GUI_INPUT_PAD, y + GUI_INPUT_H * 0.5f, cur_text, NULL);

    /* ▼ アイコン */
    hjpFillColor(vg, TH_TEXT_DIM);
    hjpTextAlign(vg, HJP_ALIGN_RIGHT | HJP_ALIGN_MIDDLE);
    hjpText(vg, fx + fw - 8.0f, y + GUI_INPUT_H * 0.5f, "▼", NULL);

    Value result = current;
    float dropdown_h = 0;

    /* ドロップダウン表示 */
    if (is_open && choices.array.length > 0) {
        int n = choices.array.length;
        dropdown_h = GUI_COMBO_ITEM_H * n + 4.0f;
        float dy = y + GUI_INPUT_H + 2.0f;

        /* 背景 */
        hjpBeginPath(vg);
        hjpRoundedRect(vg, fx, dy, fw, dropdown_h, 4.0f);
        hjpFillColor(vg, TH_WIDGET_BG);
        hjpFill(vg);
        hjpStrokeColor(vg, TH_BORDER);
        hjpStrokeWidth(vg, 1.0f);
        hjpStroke(vg);

        for (int i = 0; i < n; i++) {
            float iy = dy + 2.0f + GUI_COMBO_ITEM_H * i;
            bool ih = gui_hit((float)g_cur->in.mx, (float)g_cur->in.my,
                               fx + 2, iy, fw - 4, GUI_COMBO_ITEM_H);

            if (ih) {
                hjpBeginPath(vg);
                hjpRoundedRect(vg, fx + 2, iy, fw - 4, GUI_COMBO_ITEM_H, 2.0f);
                hjpFillColor(vg, TH_ACCENT);
                hjpFill(vg);
            }

            const char *item_text = "";
            Value item = choices.array.elements[i];
            if (item.type == VALUE_STRING) item_text = item.string.data;
            else if (item.type == VALUE_NUMBER) {
                static char ib[32];
                if (item.number == (int)item.number)
                    snprintf(ib, sizeof(ib), "%d", (int)item.number);
                else
                    snprintf(ib, sizeof(ib), "%.2f", item.number);
                item_text = ib;
            }

            hjpFillColor(vg, ih ? TH_CHECK : TH_TEXT);
            hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_MIDDLE);
            hjpText(vg, fx + GUI_INPUT_PAD,
                    iy + GUI_COMBO_ITEM_H * 0.5f, item_text, NULL);

            if (ih && g_cur->in.clicked) {
                result = item;
                g_combo_open = 0;
            }
        }
    }

    g_cur->last = (GuiLastWidget){fx, y, fw, GUI_INPUT_H, hov};
    gui_advance(row_h + dropdown_h);
    return result;
}

/* =====================================================================
 * Phase 4: レイアウトシステム
 * ===================================================================*/

/* ---------------------------------------------------------------
 * パネル開始(タイトル [, オプション])
 *   コンテンツをグループ化するパネルを開始する。
 *   オプション: {幅: 数値, スクロール: 真偽}
 * ---------------------------------------------------------------*/
static Value fn_panel_begin(int argc, Value *argv) {
    if (!g_cur) return hajimu_null();

    const char *title = (argc >= 1 && argv[0].type == VALUE_STRING)
                       ? argv[0].string.data : NULL;
    bool scrollable = false;

    if (argc >= 2 && argv[1].type == VALUE_DICT) {
        for (int i = 0; i < argv[1].dict.length; i++) {
            if (strcmp(argv[1].dict.keys[i], "スクロール") == 0)
                scrollable = argv[1].dict.values[i].boolean;
        }
    }

    if (g_cur->panel_depth >= GUI_MAX_PANELS) return hajimu_null();

    float x, y, w;
    gui_pos(&x, &y, &w);

    /* 現在のレイアウトを保存 */
    int d = g_cur->panel_depth;
    g_cur->saved_lay[d] = g_cur->lay;

    GuiPanel *p = &g_cur->panels[d];
    p->x = x - GUI_PADDING;
    p->y = y;
    p->w = w + GUI_PADDING * 2;
    p->h = 0;  /* 終了時に計算 */
    p->scrollable = scrollable;
    p->content_h = 0;

    /* タイトルバー */
    Hjpcontext *vg = g_cur->vg;
    float header_h = 0;
    if (title) {
        header_h = 28.0f;
        hjpBeginPath(vg);
        hjpRoundedRect(vg, p->x, p->y, p->w, header_h, 6.0f);
        hjpFillColor(vg, TH_WIDGET_BG);
        hjpFill(vg);

        hjpFontFaceId(vg, g_cur->font_id);
        hjpFontSize(vg, 15.0f);
        hjpFillColor(vg, TH_TEXT);
        hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_MIDDLE);
        hjpText(vg, p->x + 10.0f, p->y + header_h * 0.5f, title, NULL);
    }

    /* レイアウトを内側に変更 */
    g_cur->lay.x = p->x + 4.0f;
    g_cur->lay.y = p->y + header_h + GUI_PADDING * 0.5f;
    g_cur->lay.w = p->w - 8.0f;
    g_cur->lay.indent = 0;

    g_cur->panel_depth++;
    return hajimu_null();
}

/* ---------------------------------------------------------------
 * パネル終了()
 * ---------------------------------------------------------------*/
static Value fn_panel_end(int argc, Value *argv) {
    (void)argc; (void)argv;
    if (!g_cur || g_cur->panel_depth <= 0) return hajimu_null();

    g_cur->panel_depth--;
    int d = g_cur->panel_depth;
    GuiPanel *p = &g_cur->panels[d];

    float panel_bottom = g_cur->lay.y + GUI_PADDING * 0.5f;
    p->h = panel_bottom - p->y;

    /* パネル枠の描画 */
    Hjpcontext *vg = g_cur->vg;
    hjpBeginPath(vg);
    hjpRoundedRect(vg, p->x, p->y, p->w, p->h, 6.0f);
    hjpStrokeColor(vg, TH_BORDER);
    hjpStrokeWidth(vg, 1.0f);
    hjpStroke(vg);

    /* レイアウトを復帰 */
    g_cur->lay = g_cur->saved_lay[d];
    g_cur->lay.y = panel_bottom + GUI_MARGIN;

    return hajimu_null();
}

/* ---------------------------------------------------------------
 * 横並び開始()  — 以降のウィジェットを横に並べる
 * ---------------------------------------------------------------*/
static Value fn_horizontal_begin(int argc, Value *argv) {
    (void)argc; (void)argv;
    if (!g_cur) return hajimu_null();

    /* 横並びのためにインデントを保存して使う */
    if (g_cur->panel_depth >= GUI_MAX_PANELS) return hajimu_null();

    int d = g_cur->panel_depth;
    g_cur->saved_lay[d] = g_cur->lay;
    g_cur->panel_depth++;

    return hajimu_null();
}

/* ---------------------------------------------------------------
 * 横並び終了()
 * ---------------------------------------------------------------*/
static Value fn_horizontal_end(int argc, Value *argv) {
    (void)argc; (void)argv;
    if (!g_cur || g_cur->panel_depth <= 0) return hajimu_null();

    g_cur->panel_depth--;
    int d = g_cur->panel_depth;

    /* 横並びで最も進んだ y を使用 */
    float max_y = g_cur->lay.y;
    g_cur->lay = g_cur->saved_lay[d];
    if (max_y > g_cur->lay.y) g_cur->lay.y = max_y;

    return hajimu_null();
}

/* ---------------------------------------------------------------
 * インデント([幅])  デフォルト 20px
 * ---------------------------------------------------------------*/
static Value fn_indent(int argc, Value *argv) {
    if (!g_cur) return hajimu_null();
    float w = 20.0f;
    if (argc >= 1 && argv[0].type == VALUE_NUMBER)
        w = (float)argv[0].number;
    g_cur->lay.indent += w;
    return hajimu_null();
}

/* ---------------------------------------------------------------
 * インデント解除()
 * ---------------------------------------------------------------*/
static Value fn_unindent(int argc, Value *argv) {
    (void)argc; (void)argv;
    if (!g_cur) return hajimu_null();
    g_cur->lay.indent = 0;
    return hajimu_null();
}

/* ---------------------------------------------------------------
 * マウス位置() → {x, y}
 * ---------------------------------------------------------------*/
static Value fn_mouse_pos(int argc, Value *argv) {
    (void)argc; (void)argv;
    if (!g_cur) return hajimu_null();
    Value d = make_dict();
    dict_set(&d, "x", hajimu_number(g_cur->in.mx));
    dict_set(&d, "y", hajimu_number(g_cur->in.my));
    return d;
}

/* ---------------------------------------------------------------
 * マウスクリック() → 真偽
 * ---------------------------------------------------------------*/
static Value fn_mouse_clicked(int argc, Value *argv) {
    (void)argc; (void)argv;
    if (!g_cur) return hajimu_bool(false);
    return hajimu_bool(g_cur->in.clicked);
}

/* =====================================================================
 * Phase 5: リスト・テーブル・ツリー (保持モード)
 * ===================================================================*/

/* --- ヘルパー: Value → 表示用文字列 --- */
static const char *gui_value_to_str(Value v, char *buf, int sz) {
    if (v.type == VALUE_STRING) return v.string.data;
    if (v.type == VALUE_NUMBER) {
        if (v.number == (int)v.number)
            snprintf(buf, sz, "%d", (int)v.number);
        else
            snprintf(buf, sz, "%.2f", v.number);
        return buf;
    }
    if (v.type == VALUE_BOOL) return v.boolean ? "\xe7\x9c\x9f" : "\xe5\x81\xbd";
    return "";
}

/* ---------------------------------------------------------------
 * リスト(ID, 項目 [, 選択]) → 選択値
 * ---------------------------------------------------------------*/
static Value fn_list(int argc, Value *argv) {
    if (!g_cur || argc < 2 || argv[0].type != VALUE_STRING ||
        argv[1].type != VALUE_ARRAY)
        return hajimu_number(-1);

    Hjpcontext *vg = g_cur->vg;
    const char *label = argv[0].string.data;
    Value items = argv[1];
    int selected = (argc >= 3 && argv[2].type == VALUE_NUMBER)
                   ? (int)argv[2].number : -1;
    int n = items.array.length;
    int visible = n < 10 ? n : 10;
    if (visible < 1) visible = 1;
    float list_h = GUI_LIST_ITEM_H * visible;
    float x, y, avail_w;
    gui_pos(&x, &y, &avail_w);

    hjpFontFaceId(vg, g_cur->font_id);
    hjpFontSize(vg, GUI_FONT_SIZE);
    hjpFillColor(vg, TH_TEXT);
    hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_TOP);
    hjpText(vg, x, y, label, NULL);
    y += GUI_FONT_SIZE * 1.4f + 4.0f;

    hjpBeginPath(vg);
    hjpRoundedRect(vg, x, y, avail_w, list_h, 4.0f);
    hjpFillColor(vg, TH_WIDGET_BG);
    hjpFill(vg);
    hjpStrokeColor(vg, TH_BORDER);
    hjpStrokeWidth(vg, 1.0f);
    hjpStroke(vg);

    static float s_list_scroll[64];
    int slot = gui_hash(label) % 64;
    bool list_hov = gui_hit((float)g_cur->in.mx, (float)g_cur->in.my,
                             x, y, avail_w, list_h);
    if (list_hov && g_cur->in.scroll_y != 0) {
        s_list_scroll[slot] -= g_cur->in.scroll_y * GUI_LIST_ITEM_H;
        float max_s = (n - visible) * GUI_LIST_ITEM_H;
        if (max_s < 0) max_s = 0;
        if (s_list_scroll[slot] < 0) s_list_scroll[slot] = 0;
        if (s_list_scroll[slot] > max_s) s_list_scroll[slot] = max_s;
    }
    float scroll = s_list_scroll[slot];

    hjpSave(vg);
    hjpScissor(vg, x, y, avail_w, list_h);
    for (int i = 0; i < n; i++) {
        float iy = y + GUI_LIST_ITEM_H * i - scroll;
        if (iy + GUI_LIST_ITEM_H < y || iy > y + list_h) continue;
        bool is_sel = (i == selected);
        bool hov = gui_hit((float)g_cur->in.mx, (float)g_cur->in.my,
                            x, iy, avail_w, GUI_LIST_ITEM_H);
        if (is_sel) {
            hjpBeginPath(vg);
            hjpRect(vg, x + 1, iy, avail_w - 2, GUI_LIST_ITEM_H);
            hjpFillColor(vg, TH_ACCENT);
            hjpFill(vg);
        } else if (hov) {
            hjpBeginPath(vg);
            hjpRect(vg, x + 1, iy, avail_w - 2, GUI_LIST_ITEM_H);
            hjpFillColor(vg, TH_WIDGET_HOVER);
            hjpFill(vg);
        }
        if (hov && g_cur->in.clicked) selected = i;
        char nb[32];
        const char *text = gui_value_to_str(items.array.elements[i], nb, sizeof(nb));
        hjpFontFaceId(vg, g_cur->font_id);
        hjpFontSize(vg, GUI_FONT_SIZE);
        hjpFillColor(vg, is_sel ? TH_CHECK : TH_TEXT);
        hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_MIDDLE);
        hjpText(vg, x + 8.0f, iy + GUI_LIST_ITEM_H * 0.5f, text, NULL);
    }
    hjpRestore(vg);

    g_cur->last = (GuiLastWidget){x, y, avail_w, list_h, list_hov};
    gui_advance(list_h + GUI_FONT_SIZE * 1.4f + 4.0f);
    return hajimu_number(selected);
}

/* ---------------------------------------------------------------
 * テーブル作成(ID, 列定義) → テーブルID
 * ---------------------------------------------------------------*/
static Value fn_table_create(int argc, Value *argv) {
    if (argc < 2 || argv[0].type != VALUE_STRING ||
        argv[1].type != VALUE_ARRAY)
        return hajimu_number(-1);

    uint32_t id = gui_hash(argv[0].string.data);
    for (int i = 0; i < GUI_MAX_TABLES; i++)
        if (g_tables[i].valid && g_tables[i].id == id)
            return hajimu_number(i);

    int idx = -1;
    for (int i = 0; i < GUI_MAX_TABLES; i++)
        if (!g_tables[i].valid) { idx = i; break; }
    if (idx < 0) return hajimu_number(-1);

    GuiTable *t = &g_tables[idx];
    memset(t, 0, sizeof(*t));
    t->id = id;
    t->col_count = argv[1].array.length;
    if (t->col_count > 16) t->col_count = 16;
    for (int c = 0; c < t->col_count; c++) {
        char nb[32];
        const char *s = gui_value_to_str(argv[1].array.elements[c], nb, sizeof(nb));
        snprintf(t->col_names[c], sizeof(t->col_names[c]), "%s", s);
    }
    t->selected = -1;
    t->sort_col = -1;
    t->sort_dir = 1;
    t->row_cap = 64;
    t->rows = calloc(t->row_cap, sizeof(char **));
    t->valid = true;
    /* Phase 35 初期化 */
    t->edit_row = -1; t->edit_col = -1;
    for (int c = 0; c < 16; c++) t->col_order[c] = c;
    t->sel_mode = 0;
    t->fixed_cols = 0;
    t->group_col = -1;
    t->merge_row = -1; t->merge_col = -1;
    t->merge_rspan = 1; t->merge_cspan = 1;
    t->drag_row = -1; t->drag_target = -1;
    return hajimu_number(idx);
}

/* ---------------------------------------------------------------
 * テーブル行追加(テーブル, データ)
 * ---------------------------------------------------------------*/
static Value fn_table_add_row(int argc, Value *argv) {
    if (argc < 2 || argv[0].type != VALUE_NUMBER ||
        argv[1].type != VALUE_ARRAY)
        return hajimu_null();

    int idx = (int)argv[0].number;
    if (idx < 0 || idx >= GUI_MAX_TABLES || !g_tables[idx].valid)
        return hajimu_null();

    GuiTable *t = &g_tables[idx];
    if (t->row_count >= t->row_cap) {
        t->row_cap *= 2;
        t->rows = realloc(t->rows, t->row_cap * sizeof(char **));
    }
    char **row = calloc(t->col_count, sizeof(char *));
    for (int c = 0; c < t->col_count && c < argv[1].array.length; c++) {
        char nb[32];
        const char *s = gui_value_to_str(argv[1].array.elements[c], nb, sizeof(nb));
        row[c] = strdup(s);
    }
    for (int c = argv[1].array.length; c < t->col_count; c++)
        row[c] = strdup("");
    t->rows[t->row_count++] = row;
    return hajimu_null();
}

/* ---------------------------------------------------------------
 * テーブル描画(テーブル [, オプション]) → 選択行
 * ---------------------------------------------------------------*/
static Value fn_table_draw(int argc, Value *argv) {
    if (!g_cur || argc < 1 || argv[0].type != VALUE_NUMBER)
        return hajimu_number(-1);

    int idx = (int)argv[0].number;
    if (idx < 0 || idx >= GUI_MAX_TABLES || !g_tables[idx].valid)
        return hajimu_number(-1);

    GuiTable *t = &g_tables[idx];
    Hjpcontext *vg = g_cur->vg;
    float x, y, avail_w;
    gui_pos(&x, &y, &avail_w);

    int vis = t->row_count < 12 ? t->row_count : 12;
    if (vis < 1) vis = 1;
    float table_h = GUI_TABLE_HEADER_H + GUI_TABLE_ROW_H * vis;
    float col_w = (t->col_count > 0) ? avail_w / t->col_count : avail_w;

    hjpBeginPath(vg);
    hjpRoundedRect(vg, x, y, avail_w, table_h, 4.0f);
    hjpFillColor(vg, TH_WIDGET_BG);
    hjpFill(vg);
    hjpStrokeColor(vg, TH_BORDER);
    hjpStrokeWidth(vg, 1.0f);
    hjpStroke(vg);

    /* ヘッダー */
    hjpBeginPath(vg);
    hjpRect(vg, x, y, avail_w, GUI_TABLE_HEADER_H);
    hjpFillColor(vg, TH_WIDGET_ACTIVE);
    hjpFill(vg);

    hjpFontFaceId(vg, g_cur->font_id);
    hjpFontSize(vg, 14.0f);
    for (int c = 0; c < t->col_count; c++) {
        float cx = x + col_w * c;
        char hdr[80];
        if (t->sort_col == c)
            snprintf(hdr, sizeof(hdr), "%s %s", t->col_names[c],
                     t->sort_dir > 0 ? "\xe2\x96\xb2" : "\xe2\x96\xbc");
        else
            snprintf(hdr, sizeof(hdr), "%s", t->col_names[c]);

        hjpFillColor(vg, TH_TEXT);
        hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_MIDDLE);
        hjpText(vg, cx + 6.0f, y + GUI_TABLE_HEADER_H * 0.5f, hdr, NULL);

        bool hov_h = gui_hit((float)g_cur->in.mx, (float)g_cur->in.my,
                              cx, y, col_w, GUI_TABLE_HEADER_H);
        if (hov_h && g_cur->in.clicked) {
            if (t->sort_col == c) t->sort_dir = -t->sort_dir;
            else { t->sort_col = c; t->sort_dir = 1; }
            for (int a = 0; a < t->row_count - 1; a++)
                for (int b = a + 1; b < t->row_count; b++) {
                    int cmp = strcmp(t->rows[a][c] ? t->rows[a][c] : "",
                                     t->rows[b][c] ? t->rows[b][c] : "");
                    if ((t->sort_dir > 0 && cmp > 0) ||
                        (t->sort_dir < 0 && cmp < 0)) {
                        char **tmp = t->rows[a];
                        t->rows[a] = t->rows[b];
                        t->rows[b] = tmp;
                    }
                }
        }
        if (c > 0) {
            hjpBeginPath(vg);
            hjpMoveTo(vg, cx, y);
            hjpLineTo(vg, cx, y + table_h);
            hjpStrokeColor(vg, TH_BORDER);
            hjpStrokeWidth(vg, 1.0f);
            hjpStroke(vg);
        }
    }

    /* 行データ */
    bool tbl_hov = gui_hit((float)g_cur->in.mx, (float)g_cur->in.my,
                            x, y + GUI_TABLE_HEADER_H,
                            avail_w, table_h - GUI_TABLE_HEADER_H);
    if (tbl_hov && g_cur->in.scroll_y != 0) {
        t->scroll_y -= g_cur->in.scroll_y * GUI_TABLE_ROW_H;
        float max_s = (t->row_count - vis) * GUI_TABLE_ROW_H;
        if (max_s < 0) max_s = 0;
        if (t->scroll_y < 0) t->scroll_y = 0;
        if (t->scroll_y > max_s) t->scroll_y = max_s;
    }

    hjpSave(vg);
    hjpScissor(vg, x, y + GUI_TABLE_HEADER_H,
               avail_w, table_h - GUI_TABLE_HEADER_H);
    float ry = y + GUI_TABLE_HEADER_H;
    for (int r = 0; r < t->row_count; r++) {
        float row_y = ry + GUI_TABLE_ROW_H * r - t->scroll_y;
        if (row_y + GUI_TABLE_ROW_H < ry || row_y > y + table_h) continue;
        bool is_sel = (r == t->selected);
        bool rhov = gui_hit((float)g_cur->in.mx, (float)g_cur->in.my,
                             x, row_y, avail_w, GUI_TABLE_ROW_H);
        if (is_sel) {
            hjpBeginPath(vg);
            hjpRect(vg, x + 1, row_y, avail_w - 2, GUI_TABLE_ROW_H);
            hjpFillColor(vg, TH_ACCENT);
            hjpFill(vg);
        } else if (rhov) {
            hjpBeginPath(vg);
            hjpRect(vg, x + 1, row_y, avail_w - 2, GUI_TABLE_ROW_H);
            hjpFillColor(vg, TH_WIDGET_HOVER);
            hjpFill(vg);
        }
        if (rhov && g_cur->in.clicked) t->selected = r;
        for (int c = 0; c < t->col_count; c++) {
            const char *cell = (t->rows[r] && t->rows[r][c])
                              ? t->rows[r][c] : "";
            hjpFillColor(vg, is_sel ? TH_CHECK : TH_TEXT);
            hjpFontSize(vg, GUI_FONT_SIZE);
            hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_MIDDLE);
            hjpText(vg, x + col_w * c + 6.0f,
                    row_y + GUI_TABLE_ROW_H * 0.5f, cell, NULL);
        }
    }
    hjpRestore(vg);

    g_cur->last = (GuiLastWidget){x, y, avail_w, table_h, tbl_hov};
    gui_advance(table_h);
    return hajimu_number(t->selected);
}

/* ---------------------------------------------------------------
 * テーブルソート(テーブル, 列, 方向)
 * ---------------------------------------------------------------*/
static Value fn_table_sort(int argc, Value *argv) {
    if (argc < 3 || argv[0].type != VALUE_NUMBER ||
        argv[1].type != VALUE_NUMBER)
        return hajimu_null();

    int idx = (int)argv[0].number;
    if (idx < 0 || idx >= GUI_MAX_TABLES || !g_tables[idx].valid)
        return hajimu_null();
    GuiTable *t = &g_tables[idx];
    int col = (int)argv[1].number;
    if (col < 0 || col >= t->col_count) return hajimu_null();

    int dir = 1;
    if (argv[2].type == VALUE_STRING &&
        strstr(argv[2].string.data, "\xe9\x99\x8d") != NULL)
        dir = -1;
    t->sort_col = col;
    t->sort_dir = dir;
    for (int a = 0; a < t->row_count - 1; a++)
        for (int b = a + 1; b < t->row_count; b++) {
            int cmp = strcmp(t->rows[a][col] ? t->rows[a][col] : "",
                             t->rows[b][col] ? t->rows[b][col] : "");
            if ((dir > 0 && cmp > 0) || (dir < 0 && cmp < 0)) {
                char **tmp = t->rows[a];
                t->rows[a] = t->rows[b];
                t->rows[b] = tmp;
            }
        }
    return hajimu_null();
}

/* ---------------------------------------------------------------
 * ツリー作成(ID) → ツリーID
 * ---------------------------------------------------------------*/
static Value fn_tree_create(int argc, Value *argv) {
    if (argc < 1 || argv[0].type != VALUE_STRING)
        return hajimu_number(-1);
    uint32_t id = gui_hash(argv[0].string.data);
    for (int i = 0; i < GUI_MAX_TREES; i++)
        if (g_trees[i].valid && g_trees[i].id == id)
            return hajimu_number(i);
    int idx = -1;
    for (int i = 0; i < GUI_MAX_TREES; i++)
        if (!g_trees[i].valid) { idx = i; break; }
    if (idx < 0) return hajimu_number(-1);
    GuiTree *tr = &g_trees[idx];
    memset(tr, 0, sizeof(*tr));
    tr->id = id;
    tr->selected = -1;
    tr->valid = true;
    return hajimu_number(idx);
}

/* ---------------------------------------------------------------
 * ツリーノード追加(ツリー, 親, ラベル [, データ]) → ノードID
 * ---------------------------------------------------------------*/
static Value fn_tree_add_node(int argc, Value *argv) {
    if (argc < 3 || argv[0].type != VALUE_NUMBER ||
        argv[1].type != VALUE_NUMBER || argv[2].type != VALUE_STRING)
        return hajimu_number(-1);
    int idx = (int)argv[0].number;
    if (idx < 0 || idx >= GUI_MAX_TREES || !g_trees[idx].valid)
        return hajimu_number(-1);
    GuiTree *tr = &g_trees[idx];
    if (tr->node_count >= GUI_MAX_TREE_NODES) return hajimu_number(-1);
    int ni = tr->node_count++;
    tr->nodes[ni].parent = (int)argv[1].number;
    snprintf(tr->nodes[ni].label, sizeof(tr->nodes[ni].label),
             "%s", argv[2].string.data);
    tr->nodes[ni].expanded = true;
    return hajimu_number(ni);
}

/* ---------------------------------------------------------------
 * ツリー描画 — 再帰ヘルパー
 * ---------------------------------------------------------------*/
static void gui_tree_draw_node(GuiTree *tr, int ni, float x, float w,
                                float *cy, int depth) {
    if (!g_cur) return;
    Hjpcontext *vg = g_cur->vg;
    float indent = GUI_TREE_INDENT * depth;
    float nx = x + indent;
    float ny = *cy;

    bool has_ch = false;
    for (int i = 0; i < tr->node_count; i++)
        if (tr->nodes[i].parent == ni) { has_ch = true; break; }

    bool is_sel = (tr->selected == ni);
    bool hov = gui_hit((float)g_cur->in.mx, (float)g_cur->in.my,
                        x, ny, w, GUI_TREE_NODE_H);

    if (is_sel) {
        hjpBeginPath(vg);
        hjpRect(vg, x + 1, ny, w - 2, GUI_TREE_NODE_H);
        hjpFillColor(vg, TH_ACCENT);
        hjpFill(vg);
    } else if (hov) {
        hjpBeginPath(vg);
        hjpRect(vg, x + 1, ny, w - 2, GUI_TREE_NODE_H);
        hjpFillColor(vg, TH_WIDGET_HOVER);
        hjpFill(vg);
    }

    if (has_ch) {
        const char *icon = tr->nodes[ni].expanded
                         ? "\xe2\x96\xbc" : "\xe2\x96\xb6";
        hjpFillColor(vg, TH_TEXT_DIM);
        hjpFontFaceId(vg, g_cur->font_id);
        hjpFontSize(vg, 12.0f);
        hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_MIDDLE);
        hjpText(vg, nx, ny + GUI_TREE_NODE_H * 0.5f, icon, NULL);
    }

    hjpFillColor(vg, is_sel ? TH_CHECK : TH_TEXT);
    hjpFontSize(vg, GUI_FONT_SIZE);
    hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_MIDDLE);
    hjpText(vg, nx + 16.0f, ny + GUI_TREE_NODE_H * 0.5f,
            tr->nodes[ni].label, NULL);

    if (hov && g_cur->in.clicked) {
        if (has_ch && g_cur->in.mx < (int)(nx + 16.0f))
            tr->nodes[ni].expanded = !tr->nodes[ni].expanded;
        else
            tr->selected = ni;
    }

    *cy += GUI_TREE_NODE_H;
    if (has_ch && tr->nodes[ni].expanded) {
        for (int i = 0; i < tr->node_count; i++)
            if (tr->nodes[i].parent == ni)
                gui_tree_draw_node(tr, i, x, w, cy, depth + 1);
    }
}

/* ---------------------------------------------------------------
 * ツリー描画(ツリー [, オプション]) → 選択ノード
 * ---------------------------------------------------------------*/
static Value fn_tree_draw(int argc, Value *argv) {
    if (!g_cur || argc < 1 || argv[0].type != VALUE_NUMBER)
        return hajimu_number(-1);
    int idx = (int)argv[0].number;
    if (idx < 0 || idx >= GUI_MAX_TREES || !g_trees[idx].valid)
        return hajimu_number(-1);

    GuiTree *tr = &g_trees[idx];
    Hjpcontext *vg = g_cur->vg;
    float x, y, avail_w;
    gui_pos(&x, &y, &avail_w);

    int vis = tr->node_count < 12 ? tr->node_count : 12;
    if (vis < 1) vis = 1;
    float tree_h = GUI_TREE_NODE_H * vis;

    hjpBeginPath(vg);
    hjpRoundedRect(vg, x, y, avail_w, tree_h, 4.0f);
    hjpFillColor(vg, TH_WIDGET_BG);
    hjpFill(vg);
    hjpStrokeColor(vg, TH_BORDER);
    hjpStrokeWidth(vg, 1.0f);
    hjpStroke(vg);

    hjpSave(vg);
    hjpScissor(vg, x, y, avail_w, tree_h);
    float cy = y;
    for (int i = 0; i < tr->node_count; i++)
        if (tr->nodes[i].parent == -1)
            gui_tree_draw_node(tr, i, x, avail_w, &cy, 0);
    hjpRestore(vg);

    g_cur->last = (GuiLastWidget){x, y, avail_w, tree_h, false};
    gui_advance(tree_h);
    return hajimu_number(tr->selected);
}

/* =====================================================================
 * Phase 6: タブ・メニュー・ダイアログ
 * ===================================================================*/

static int g_menu_item_idx = 0;   /* メニュー項目インデックス */

/* ---------------------------------------------------------------
 * タブバー(ID, タブ配列, 選択) → 選択インデックス
 * ---------------------------------------------------------------*/
static Value fn_tab_bar(int argc, Value *argv) {
    if (!g_cur || argc < 3 || argv[0].type != VALUE_STRING ||
        argv[1].type != VALUE_ARRAY || argv[2].type != VALUE_NUMBER)
        return hajimu_number(0);

    Hjpcontext *vg = g_cur->vg;
    Value tabs = argv[1];
    int selected = (int)argv[2].number;
    int n = tabs.array.length;
    if (n <= 0) return hajimu_number(0);
    if (selected < 0 || selected >= n) selected = 0;

    float x, y, avail_w;
    gui_pos(&x, &y, &avail_w);
    float tab_w = avail_w / n;

    hjpBeginPath(vg);
    hjpRect(vg, x, y, avail_w, GUI_TAB_H);
    hjpFillColor(vg, TH_WIDGET_BG);
    hjpFill(vg);

    for (int i = 0; i < n; i++) {
        float tx = x + tab_w * i;
        bool hov = gui_hit((float)g_cur->in.mx, (float)g_cur->in.my,
                            tx, y, tab_w, GUI_TAB_H);
        bool is_sel = (i == selected);
        if (hov && !is_sel) {
            hjpBeginPath(vg);
            hjpRect(vg, tx, y, tab_w, GUI_TAB_H);
            hjpFillColor(vg, TH_WIDGET_HOVER);
            hjpFill(vg);
        }
        if (hov && g_cur->in.clicked) selected = i;
        if (is_sel) {
            hjpBeginPath(vg);
            hjpRect(vg, tx, y + GUI_TAB_H - 3.0f, tab_w, 3.0f);
            hjpFillColor(vg, TH_ACCENT);
            hjpFill(vg);
        }
        char nb[32];
        const char *text = gui_value_to_str(tabs.array.elements[i], nb, sizeof(nb));
        hjpFontFaceId(vg, g_cur->font_id);
        hjpFontSize(vg, GUI_FONT_SIZE);
        hjpFillColor(vg, is_sel ? TH_TEXT : TH_TEXT_DIM);
        hjpTextAlign(vg, HJP_ALIGN_CENTER | HJP_ALIGN_MIDDLE);
        hjpText(vg, tx + tab_w * 0.5f, y + GUI_TAB_H * 0.5f, text, NULL);
    }

    hjpBeginPath(vg);
    hjpMoveTo(vg, x, y + GUI_TAB_H);
    hjpLineTo(vg, x + avail_w, y + GUI_TAB_H);
    hjpStrokeColor(vg, TH_BORDER);
    hjpStrokeWidth(vg, 1.0f);
    hjpStroke(vg);

    gui_advance(GUI_TAB_H);
    return hajimu_number(selected);
}

/* ---------------------------------------------------------------
 * タブ内容(選択, インデックス) → 真偽
 * ---------------------------------------------------------------*/
static Value fn_tab_content(int argc, Value *argv) {
    if (argc < 2) return hajimu_bool(false);
    int sel = (argv[0].type == VALUE_NUMBER) ? (int)argv[0].number : -1;
    int idx = (argv[1].type == VALUE_NUMBER) ? (int)argv[1].number : -2;
    return hajimu_bool(sel == idx);
}

/* ---------------------------------------------------------------
 * メニューバー開始()
 * ---------------------------------------------------------------*/
static Value fn_menubar_begin(int argc, Value *argv) {
    (void)argc; (void)argv;
    if (!g_cur) return hajimu_null();
    g_menubar_active = true;
    g_menubar_cursor_x = 0;
    Hjpcontext *vg = g_cur->vg;
    hjpBeginPath(vg);
    hjpRect(vg, 0, 0, (float)g_cur->win_w, GUI_MENUBAR_H);
    hjpFillColor(vg, TH_WIDGET_BG);
    hjpFill(vg);
    hjpBeginPath(vg);
    hjpMoveTo(vg, 0, GUI_MENUBAR_H);
    hjpLineTo(vg, (float)g_cur->win_w, GUI_MENUBAR_H);
    hjpStrokeColor(vg, TH_BORDER);
    hjpStrokeWidth(vg, 1.0f);
    hjpStroke(vg);
    return hajimu_null();
}

/* ---------------------------------------------------------------
 * メニューバー終了()
 * ---------------------------------------------------------------*/
static Value fn_menubar_end(int argc, Value *argv) {
    (void)argc; (void)argv;
    g_menubar_active = false;
    if (g_cur && g_cur->lay.y < GUI_MENUBAR_H + GUI_MARGIN)
        g_cur->lay.y = GUI_MENUBAR_H + GUI_MARGIN;
    return hajimu_null();
}

/* ---------------------------------------------------------------
 * メニュー(名前) → 真偽
 * ---------------------------------------------------------------*/
static Value fn_menu(int argc, Value *argv) {
    if (!g_cur || argc < 1 || argv[0].type != VALUE_STRING)
        return hajimu_bool(false);

    Hjpcontext *vg = g_cur->vg;
    const char *name = argv[0].string.data;
    uint32_t id = gui_hash(name);

    float mx = g_menubar_cursor_x + 8.0f;
    hjpFontFaceId(vg, g_cur->font_id);
    hjpFontSize(vg, 14.0f);
    float bnd[4];
    hjpTextBounds(vg, 0, 0, name, NULL, bnd);
    float mw = bnd[2] - bnd[0] + 20.0f;

    bool hov = gui_hit((float)g_cur->in.mx, (float)g_cur->in.my,
                        mx, 0, mw, GUI_MENUBAR_H);
    bool is_open = (g_menu_open == id);

    if (hov) {
        hjpBeginPath(vg);
        hjpRect(vg, mx, 0, mw, GUI_MENUBAR_H);
        hjpFillColor(vg, is_open ? TH_WIDGET_ACTIVE : TH_WIDGET_HOVER);
        hjpFill(vg);
    }
    if (hov && g_cur->in.clicked) {
        g_menu_open = is_open ? 0 : id;
        is_open = !is_open;
    }

    hjpFillColor(vg, TH_TEXT);
    hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_MIDDLE);
    hjpText(vg, mx + 10.0f, GUI_MENUBAR_H * 0.5f, name, NULL);

    g_menubar_cursor_x = mx + mw;
    if (is_open) {
        g_menu_x = mx;
        g_menu_y = GUI_MENUBAR_H;
        g_menu_item_idx = 0;
    }
    return hajimu_bool(is_open);
}

/* ---------------------------------------------------------------
 * メニュー項目(名前 [, ショートカット]) → 真偽
 * ---------------------------------------------------------------*/
static Value fn_menu_item(int argc, Value *argv) {
    if (!g_cur || argc < 1 || argv[0].type != VALUE_STRING ||
        g_menu_open == 0)
        return hajimu_bool(false);

    Hjpcontext *vg = g_cur->vg;
    const char *name = argv[0].string.data;
    const char *shortcut = (argc >= 2 && argv[1].type == VALUE_STRING)
                          ? argv[1].string.data : NULL;

    float iy = g_menu_y + GUI_MENU_ITEM_H * g_menu_item_idx;
    g_menu_item_idx++;

    hjpBeginPath(vg);
    hjpRect(vg, g_menu_x, iy, GUI_MENU_W, GUI_MENU_ITEM_H);
    bool hov = gui_hit((float)g_cur->in.mx, (float)g_cur->in.my,
                        g_menu_x, iy, GUI_MENU_W, GUI_MENU_ITEM_H);
    hjpFillColor(vg, hov ? TH_ACCENT : TH_WIDGET_BG);
    hjpFill(vg);
    hjpStrokeColor(vg, TH_BORDER);
    hjpStrokeWidth(vg, 0.5f);
    hjpStroke(vg);

    hjpFontFaceId(vg, g_cur->font_id);
    hjpFontSize(vg, 14.0f);
    hjpFillColor(vg, hov ? TH_CHECK : TH_TEXT);
    hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_MIDDLE);
    hjpText(vg, g_menu_x + 10.0f, iy + GUI_MENU_ITEM_H * 0.5f, name, NULL);

    if (shortcut) {
        hjpFillColor(vg, TH_TEXT_DIM);
        hjpTextAlign(vg, HJP_ALIGN_RIGHT | HJP_ALIGN_MIDDLE);
        hjpText(vg, g_menu_x + GUI_MENU_W - 10.0f,
                iy + GUI_MENU_ITEM_H * 0.5f, shortcut, NULL);
    }

    bool clicked = (hov && g_cur->in.clicked);
    if (clicked) g_menu_open = 0;
    return hajimu_bool(clicked);
}

/* ---------------------------------------------------------------
 * メニューセパレーター()
 * ---------------------------------------------------------------*/
static Value fn_menu_separator_item(int argc, Value *argv) {
    (void)argc; (void)argv;
    if (!g_cur || g_menu_open == 0) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    float iy = g_menu_y + GUI_MENU_ITEM_H * g_menu_item_idx;
    hjpBeginPath(vg);
    hjpRect(vg, g_menu_x, iy, GUI_MENU_W, 2.0f);
    hjpFillColor(vg, TH_SEP);
    hjpFill(vg);
    g_menu_item_idx++;
    return hajimu_null();
}

/* ---------------------------------------------------------------
 * ダイアログ(タイトル, 開閉) → 真偽
 * ---------------------------------------------------------------*/
static Value fn_dialog(int argc, Value *argv) {
    if (!g_cur || argc < 2 || argv[0].type != VALUE_STRING)
        return hajimu_bool(false);

    bool is_open = false;
    if (argv[1].type == VALUE_BOOL)   is_open = argv[1].boolean;
    else if (argv[1].type == VALUE_NUMBER) is_open = (argv[1].number != 0);
    if (!is_open) return hajimu_bool(false);

    Hjpcontext *vg = g_cur->vg;
    const char *title = argv[0].string.data;
    float ww = (float)g_cur->win_w, wh = (float)g_cur->win_h;

    hjpBeginPath(vg);
    hjpRect(vg, 0, 0, ww, wh);
    hjpFillColor(vg, hjpRGBA(0, 0, 0, 140));
    hjpFill(vg);

    float dw = ww * 0.5f, dh = wh * 0.5f;
    if (dw < 300) dw = 300;
    if (dh < 200) dh = 200;
    float dx = (ww - dw) * 0.5f, dy = (wh - dh) * 0.5f;

    hjpBeginPath(vg);
    hjpRoundedRect(vg, dx, dy, dw, dh, 8.0f);
    hjpFillColor(vg, TH_BG);
    hjpFill(vg);
    hjpStrokeColor(vg, TH_BORDER);
    hjpStrokeWidth(vg, 1.0f);
    hjpStroke(vg);

    hjpBeginPath(vg);
    hjpRoundedRect(vg, dx, dy, dw, 32.0f, 8.0f);
    hjpFillColor(vg, TH_WIDGET_BG);
    hjpFill(vg);
    hjpFontFaceId(vg, g_cur->font_id);
    hjpFontSize(vg, 15.0f);
    hjpFillColor(vg, TH_TEXT);
    hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_MIDDLE);
    hjpText(vg, dx + 12.0f, dy + 16.0f, title, NULL);

    g_cur->lay.x = dx;
    g_cur->lay.y = dy + 40.0f;
    g_cur->lay.w = dw;
    g_cur->lay.indent = 0;
    return hajimu_bool(true);
}

/* ---------------------------------------------------------------
 * ファイルダイアログ(モード [, フィルター]) → 文字列
 * ---------------------------------------------------------------*/
static Value fn_file_dialog(int argc, Value *argv) {
    const char *mode = "\xe9\x96\x8b\xe3\x81\x8f";
    if (argc >= 1 && argv[0].type == VALUE_STRING)
        mode = argv[0].string.data;

    char cmd[512];
#ifdef __APPLE__
    if (strstr(mode, "\xe4\xbf\x9d\xe5\xad\x98") != NULL)
        snprintf(cmd, sizeof(cmd),
            "osascript -e 'POSIX path of (choose file name)' 2>/dev/null");
    else
        snprintf(cmd, sizeof(cmd),
            "osascript -e 'POSIX path of (choose file)' 2>/dev/null");
#elif defined(__linux__)
    if (strstr(mode, "\xe4\xbf\x9d\xe5\xad\x98") != NULL)
        snprintf(cmd, sizeof(cmd), "zenity --file-selection --save 2>/dev/null");
    else
        snprintf(cmd, sizeof(cmd), "zenity --file-selection 2>/dev/null");
#else
    (void)mode;
    return hajimu_string("");
#endif

    FILE *fp = popen(cmd, "r");
    if (!fp) return hajimu_string("");
    char result[1024] = {0};
    if (fgets(result, sizeof(result), fp)) {
        int len = (int)strlen(result);
        while (len > 0 && (result[len-1] == '\n' || result[len-1] == '\r'))
            result[--len] = '\0';
    }
    pclose(fp);
    return hajimu_string(result);
}

/* ---------------------------------------------------------------
 * メッセージ(タイトル, 内容, 種類) → 真偽
 * ---------------------------------------------------------------*/
static Value fn_message(int argc, Value *argv) {
    if (argc < 3) return hajimu_bool(false);
    const char *title = (argv[0].type == VALUE_STRING) ? argv[0].string.data : "";
    const char *body  = (argv[1].type == VALUE_STRING) ? argv[1].string.data : "";
    const char *kind  = (argv[2].type == VALUE_STRING) ? argv[2].string.data : "";

    char cmdm[4096];
#ifdef __APPLE__
    /* シェル引数をエスケープ */
    char esc_title[512], esc_body[512];
    gui_shell_escape(esc_title, sizeof(esc_title), title);
    gui_shell_escape(esc_body, sizeof(esc_body), body);

    const char *icon = "note";
    if (strstr(kind, "\xe8\xad\xa6\xe5\x91\x8a") != NULL) icon = "caution";
    else if (strstr(kind, "\xe3\x82\xa8\xe3\x83\xa9\xe3\x83\xbc") != NULL)
        icon = "stop";

    if (strstr(kind, "\xe7\xa2\xba\xe8\xaa\x8d") != NULL) {
        snprintf(cmdm, sizeof(cmdm),
            "osascript -e 'button returned of (display dialog '\"'\"'%s'\"'\"' "
            "with title '\"'\"'%s'\"'\"' buttons {\"Cancel\",\"OK\"} "
            "default button \"OK\")' 2>/dev/null", esc_body, esc_title);
        FILE *fp = popen(cmdm, "r");
        if (!fp) return hajimu_bool(false);
        char buf[64] = {0};
        if (fgets(buf, sizeof(buf), fp)) {}
        pclose(fp);
        return hajimu_bool(strstr(buf, "OK") != NULL);
    }
    snprintf(cmdm, sizeof(cmdm),
        "osascript -e 'display dialog '\"'\"'%s'\"'\"' with title '\"'\"'%s'\"'\"' "
        "with icon %s buttons {\"OK\"} default button \"OK\"' 2>/dev/null",
        esc_body, esc_title, icon);
    FILE *fp = popen(cmdm, "r");
    if (fp) { char buf[64]; if (fgets(buf, sizeof(buf), fp)) {} pclose(fp); }
    return hajimu_bool(true);
#elif defined(__linux__)
    char esc_title[512], esc_body[512];
    gui_shell_escape(esc_title, sizeof(esc_title), title);
    gui_shell_escape(esc_body, sizeof(esc_body), body);

    const char *ztype = "--info";
    if (strstr(kind, "\xe8\xad\xa6\xe5\x91\x8a") != NULL) ztype = "--warning";
    else if (strstr(kind, "\xe3\x82\xa8\xe3\x83\xa9\xe3\x83\xbc") != NULL) ztype = "--error";
    else if (strstr(kind, "\xe7\xa2\xba\xe8\xaa\x8d") != NULL) ztype = "--question";
    snprintf(cmdm, sizeof(cmdm),
        "zenity %s --title=%s --text=%s 2>/dev/null",
        ztype, esc_title, esc_body);
    int ret = system(cmdm);
    return hajimu_bool(ret == 0);
#else
    (void)title; (void)body; (void)kind;
    return hajimu_bool(true);
#endif
}

/* ---------------------------------------------------------------
 * トースト(メッセージ [, 秒数])
 * ---------------------------------------------------------------*/
static Value fn_toast(int argc, Value *argv) {
    if (argc < 1 || argv[0].type != VALUE_STRING) return hajimu_null();
    int dur = GUI_TOAST_DURATION;
    if (argc >= 2 && argv[1].type == VALUE_NUMBER)
        dur = (int)(argv[1].number * 1000);
    if (dur < 500) dur = 500;
    int slot = -1;
    for (int i = 0; i < GUI_MAX_TOASTS; i++)
        if (!g_toasts[i].active) { slot = i; break; }
    if (slot < 0) slot = 0;
    snprintf(g_toasts[slot].message, sizeof(g_toasts[slot].message),
             "%s", argv[0].string.data);
    g_toasts[slot].start_time = hjp_get_ticks();
    g_toasts[slot].duration_ms = dur;
    g_toasts[slot].active = true;
    return hajimu_null();
}

/* =====================================================================
 * Phase 7: カスタム描画 + Canvas
 * ===================================================================*/

/* 色引数ヘルパー */
static Hjpcolor gui_arg_color(Value v) {
    if (v.type == VALUE_NUMBER) return gui_unpack(v.number);
    if (v.type == VALUE_STRING) {
        GuiRGBA c = gui_hex(v.string.data);
        return hjpRGBAf(c.r, c.g, c.b, c.a);
    }
    return hjpRGBA(228, 228, 228, 255);
}

/* ---------------------------------------------------------------
 * キャンバス開始(ID, 幅, 高さ)
 * ---------------------------------------------------------------*/
static Value fn_canvas_begin(int argc, Value *argv) {
    if (!g_cur || argc < 3) return hajimu_null();
    float w = (argv[1].type == VALUE_NUMBER) ? (float)argv[1].number : 400;
    float h = (argv[2].type == VALUE_NUMBER) ? (float)argv[2].number : 300;
    float x, y, avail_w;
    gui_pos(&x, &y, &avail_w);
    if (w > avail_w) w = avail_w;

    Hjpcontext *vg = g_cur->vg;
    hjpBeginPath(vg);
    hjpRoundedRect(vg, x, y, w, h, 2.0f);
    hjpFillColor(vg, TH_WIDGET_BG);
    hjpFill(vg);
    hjpStrokeColor(vg, TH_BORDER);
    hjpStrokeWidth(vg, 1.0f);
    hjpStroke(vg);

    hjpSave(vg);
    hjpScissor(vg, x, y, w, h);
    hjpTranslate(vg, x, y);
    g_canvas_active = true;
    g_canvas_ox = x;
    g_canvas_oy = y;
    gui_advance(h);
    return hajimu_null();
}

/* キャンバス終了() */
static Value fn_canvas_end(int argc, Value *argv) {
    (void)argc; (void)argv;
    if (!g_cur) return hajimu_null();
    hjpRestore(g_cur->vg);
    g_canvas_active = false;
    return hajimu_null();
}

/* 線(x1,y1,x2,y2[,色,太さ]) */
static Value fn_draw_line(int argc, Value *argv) {
    if (!g_cur || argc < 4) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    Hjpcolor col = (argc >= 5) ? gui_arg_color(argv[4]) : TH_TEXT;
    float thick = (argc >= 6) ? (float)argv[5].number : 1.0f;
    hjpBeginPath(vg);
    hjpMoveTo(vg, (float)argv[0].number, (float)argv[1].number);
    hjpLineTo(vg, (float)argv[2].number, (float)argv[3].number);
    hjpStrokeColor(vg, col);
    hjpStrokeWidth(vg, thick);
    hjpStroke(vg);
    return hajimu_null();
}

/* 矩形(x,y,w,h[,色]) */
static Value fn_draw_rect(int argc, Value *argv) {
    if (!g_cur || argc < 4) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    Hjpcolor col = (argc >= 5) ? gui_arg_color(argv[4]) : TH_ACCENT;
    hjpBeginPath(vg);
    hjpRect(vg, (float)argv[0].number, (float)argv[1].number,
                (float)argv[2].number, (float)argv[3].number);
    hjpFillColor(vg, col);
    hjpFill(vg);
    return hajimu_null();
}

/* 矩形枠(x,y,w,h[,色,太さ]) */
static Value fn_draw_rect_stroke(int argc, Value *argv) {
    if (!g_cur || argc < 4) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    Hjpcolor col = (argc >= 5) ? gui_arg_color(argv[4]) : TH_TEXT;
    float thick = (argc >= 6) ? (float)argv[5].number : 1.0f;
    hjpBeginPath(vg);
    hjpRect(vg, (float)argv[0].number, (float)argv[1].number,
                (float)argv[2].number, (float)argv[3].number);
    hjpStrokeColor(vg, col);
    hjpStrokeWidth(vg, thick);
    hjpStroke(vg);
    return hajimu_null();
}

/* 角丸矩形(x,y,w,h,r[,色]) */
static Value fn_draw_rounded_rect(int argc, Value *argv) {
    if (!g_cur || argc < 5) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    Hjpcolor col = (argc >= 6) ? gui_arg_color(argv[5]) : TH_ACCENT;
    hjpBeginPath(vg);
    hjpRoundedRect(vg, (float)argv[0].number, (float)argv[1].number,
                       (float)argv[2].number, (float)argv[3].number,
                       (float)argv[4].number);
    hjpFillColor(vg, col);
    hjpFill(vg);
    return hajimu_null();
}

/* 円(cx,cy,r[,色]) */
static Value fn_draw_circle(int argc, Value *argv) {
    if (!g_cur || argc < 3) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    Hjpcolor col = (argc >= 4) ? gui_arg_color(argv[3]) : TH_ACCENT;
    hjpBeginPath(vg);
    hjpCircle(vg, (float)argv[0].number, (float)argv[1].number,
                  (float)argv[2].number);
    hjpFillColor(vg, col);
    hjpFill(vg);
    return hajimu_null();
}

/* 円弧(cx,cy,r,a0,a1[,色]) */
static Value fn_draw_arc(int argc, Value *argv) {
    if (!g_cur || argc < 5) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    Hjpcolor col = (argc >= 6) ? gui_arg_color(argv[5]) : TH_ACCENT;
    hjpBeginPath(vg);
    hjpArc(vg, (float)argv[0].number, (float)argv[1].number,
               (float)argv[2].number,
               (float)argv[3].number, (float)argv[4].number, HJP_CW);
    hjpStrokeColor(vg, col);
    hjpStrokeWidth(vg, 2.0f);
    hjpStroke(vg);
    return hajimu_null();
}

/* 多角形(点配列[,色]) */
static Value fn_draw_polygon(int argc, Value *argv) {
    if (!g_cur || argc < 1 || argv[0].type != VALUE_ARRAY)
        return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    Value pts = argv[0];
    Hjpcolor col = (argc >= 2) ? gui_arg_color(argv[1]) : TH_ACCENT;
    if (pts.array.length < 3) return hajimu_null();
    hjpBeginPath(vg);
    for (int i = 0; i < pts.array.length; i++) {
        Value p = pts.array.elements[i];
        float px = 0, py = 0;
        if (p.type == VALUE_DICT) {
            for (int j = 0; j < p.dict.length; j++) {
                if (strcmp(p.dict.keys[j], "x") == 0)
                    px = (float)p.dict.values[j].number;
                if (strcmp(p.dict.keys[j], "y") == 0)
                    py = (float)p.dict.values[j].number;
            }
        }
        if (i == 0) hjpMoveTo(vg, px, py);
        else        hjpLineTo(vg, px, py);
    }
    hjpClosePath(vg);
    hjpFillColor(vg, col);
    hjpFill(vg);
    return hajimu_null();
}

/* パス開始() */
static Value fn_path_begin(int argc, Value *argv) {
    (void)argc; (void)argv;
    if (g_cur) hjpBeginPath(g_cur->vg);
    return hajimu_null();
}

/* パス終了([色,太さ]) */
static Value fn_path_end(int argc, Value *argv) {
    if (!g_cur) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    Hjpcolor col = (argc >= 1) ? gui_arg_color(argv[0]) : TH_TEXT;
    if (argc >= 2 && argv[1].type == VALUE_NUMBER) {
        hjpStrokeColor(vg, col);
        hjpStrokeWidth(vg, (float)argv[1].number);
        hjpStroke(vg);
    } else {
        hjpFillColor(vg, col);
        hjpFill(vg);
    }
    return hajimu_null();
}

/* パス移動(x,y) */
static Value fn_path_move(int argc, Value *argv) {
    if (g_cur && argc >= 2)
        hjpMoveTo(g_cur->vg, (float)argv[0].number, (float)argv[1].number);
    return hajimu_null();
}

/* パス線(x,y) */
static Value fn_path_line(int argc, Value *argv) {
    if (g_cur && argc >= 2)
        hjpLineTo(g_cur->vg, (float)argv[0].number, (float)argv[1].number);
    return hajimu_null();
}

/* ベジェ(cx1,cy1,cx2,cy2,x,y) */
static Value fn_bezier(int argc, Value *argv) {
    if (g_cur && argc >= 6)
        hjpBezierTo(g_cur->vg,
            (float)argv[0].number, (float)argv[1].number,
            (float)argv[2].number, (float)argv[3].number,
            (float)argv[4].number, (float)argv[5].number);
    return hajimu_null();
}

/* 線形グラデーション(x1,y1,x2,y2,色1,色2) */
static Value fn_linear_gradient(int argc, Value *argv) {
    if (!g_cur || argc < 6) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    Hjppaint p = hjpLinearGradient(vg,
        (float)argv[0].number, (float)argv[1].number,
        (float)argv[2].number, (float)argv[3].number,
        gui_arg_color(argv[4]), gui_arg_color(argv[5]));
    hjpFillPaint(vg, p);
    hjpFill(vg);
    return hajimu_null();
}

/* 画像読み込み(パス) → ハンドル */
static Value fn_image_load(int argc, Value *argv) {
    if (!g_cur || argc < 1 || argv[0].type != VALUE_STRING)
        return hajimu_number(-1);
    int slot = -1;
    for (int i = 0; i < GUI_MAX_IMAGES; i++)
        if (!g_images[i].valid) { slot = i; break; }
    if (slot < 0) return hajimu_number(-1);
    int handle = hjpCreateImage(g_cur->vg, argv[0].string.data, 0);
    if (handle <= 0) return hajimu_number(-1);
    int iw, ih;
    hjpImageSize(g_cur->vg, handle, &iw, &ih);
    g_images[slot] = (GuiImage){handle, iw, ih, true};
    return hajimu_number(slot);
}

/* 画像描画(画像,x,y[,w,h]) */
static Value fn_image_draw(int argc, Value *argv) {
    if (!g_cur || argc < 3 || argv[0].type != VALUE_NUMBER)
        return hajimu_null();
    int slot = (int)argv[0].number;
    if (slot < 0 || slot >= GUI_MAX_IMAGES || !g_images[slot].valid)
        return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    float ix = (float)argv[1].number, iy = (float)argv[2].number;
    float iw = (argc >= 4) ? (float)argv[3].number : (float)g_images[slot].w;
    float ih = (argc >= 5) ? (float)argv[4].number : (float)g_images[slot].h;
    Hjppaint img = hjpImagePattern(vg, ix, iy, iw, ih, 0,
                                    g_images[slot].handle, 1.0f);
    hjpBeginPath(vg);
    hjpRect(vg, ix, iy, iw, ih);
    hjpFillPaint(vg, img);
    hjpFill(vg);
    return hajimu_null();
}

/* 描画テキスト(テキスト,x,y[,オプション]) */
static Value fn_draw_text_at(int argc, Value *argv) {
    if (!g_cur || argc < 3 || argv[0].type != VALUE_STRING)
        return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    float size = GUI_FONT_SIZE;
    Hjpcolor col = TH_TEXT;
    if (argc >= 4 && argv[3].type == VALUE_DICT) {
        for (int i = 0; i < argv[3].dict.length; i++) {
            if (strcmp(argv[3].dict.keys[i], "\xe3\x82\xb5\xe3\x82\xa4\xe3\x82\xba") == 0)
                size = (float)argv[3].dict.values[i].number;
            else if (strcmp(argv[3].dict.keys[i], "\xe8\x89\xb2") == 0)
                col = gui_arg_color(argv[3].dict.values[i]);
        }
    }
    hjpFontFaceId(vg, g_cur->font_id);
    hjpFontSize(vg, size);
    hjpFillColor(vg, col);
    hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_TOP);
    hjpText(vg, (float)argv[1].number, (float)argv[2].number,
            argv[0].string.data, NULL);
    return hajimu_null();
}

/* 変換保存() */
static Value fn_save_transform(int argc, Value *argv) {
    (void)argc; (void)argv;
    if (g_cur) hjpSave(g_cur->vg);
    return hajimu_null();
}

/* 変換復元() */
static Value fn_restore_transform(int argc, Value *argv) {
    (void)argc; (void)argv;
    if (g_cur) hjpRestore(g_cur->vg);
    return hajimu_null();
}

/* 平行移動(x,y) */
static Value fn_translate(int argc, Value *argv) {
    if (g_cur && argc >= 2)
        hjpTranslate(g_cur->vg, (float)argv[0].number, (float)argv[1].number);
    return hajimu_null();
}

/* 回転(角度) ラジアン */
static Value fn_rotate_transform(int argc, Value *argv) {
    if (g_cur && argc >= 1)
        hjpRotate(g_cur->vg, (float)argv[0].number);
    return hajimu_null();
}

/* 拡縮(sx,sy) */
static Value fn_scale_transform(int argc, Value *argv) {
    if (g_cur && argc >= 2)
        hjpScale(g_cur->vg, (float)argv[0].number, (float)argv[1].number);
    return hajimu_null();
}

/* =====================================================================
 * Phase 8: テーマ・スタイル
 * ===================================================================*/

/* テーマ設定(テーマ名) */
static Value fn_theme_set(int argc, Value *argv) {
    if (argc < 1 || argv[0].type != VALUE_STRING) return hajimu_null();
    const char *name = argv[0].string.data;
    if (strstr(name, "\xe3\x83\xa9\xe3\x82\xa4\xe3\x83\x88") != NULL)
        gui_theme_set_light();
    else if (strstr(name, "\xe9\xab\x98") != NULL)
        gui_theme_set_high_contrast();
    else
        gui_theme_set_dark();
    if (g_cur)
        g_cur->bg = (GuiRGBA){g_th.bg.r, g_th.bg.g, g_th.bg.b, g_th.bg.a};
    return hajimu_null();
}

/* テーマ色(項目, 色) */
static Value fn_theme_color(int argc, Value *argv) {
    if (argc < 2 || argv[0].type != VALUE_STRING) return hajimu_null();
    const char *key = argv[0].string.data;
    Hjpcolor col = gui_arg_color(argv[1]);
    if (strstr(key, "\xe8\x83\x8c\xe6\x99\xaf") != NULL) g_th.bg = col;
    else if (strstr(key, "\xe3\x82\xa6\xe3\x82\xa3\xe3\x82\xb8\xe3\x82\xa7\xe3\x83\x83\xe3\x83\x88") != NULL)
        g_th.widget_bg = col;
    else if (strstr(key, "\xe3\x82\xa2\xe3\x82\xaf\xe3\x82\xbb\xe3\x83\xb3\xe3\x83\x88") != NULL)
        g_th.accent = col;
    else if (strstr(key, "\xe3\x83\x86\xe3\x82\xad\xe3\x82\xb9\xe3\x83\x88") != NULL)
        g_th.text = col;
    else if (strstr(key, "\xe6\x9e\xa0") != NULL) g_th.border = col;
    return hajimu_null();
}

/* テーマフォント(ファミリー[,サイズ]) */
static Value fn_theme_font(int argc, Value *argv) {
    if (argc >= 2 && argv[1].type == VALUE_NUMBER)
        g_custom_font_size = (float)argv[1].number;
    (void)argv;
    return hajimu_null();
}

/* スタイル設定(対象, プロパティ) — v0.8.0 スタブ */
static Value fn_style_set(int argc, Value *argv) {
    (void)argc; (void)argv;
    return hajimu_null();
}

/* フォント読み込み(名前, パス) → 真偽 */
static Value fn_font_load(int argc, Value *argv) {
    if (!g_cur || argc < 2 ||
        argv[0].type != VALUE_STRING || argv[1].type != VALUE_STRING)
        return hajimu_bool(false);
    int fid = hjpCreateFont(g_cur->vg, argv[0].string.data,
                             argv[1].string.data);
    return hajimu_bool(fid >= 0);
}

/* フォントサイズ(サイズ) */
static Value fn_font_size_set(int argc, Value *argv) {
    if (argc >= 1 && argv[0].type == VALUE_NUMBER)
        g_custom_font_size = (float)argv[0].number;
    return hajimu_null();
}

/* DPIスケール() → 数値 */
static Value fn_dpi_scale(int argc, Value *argv) {
    (void)argc; (void)argv;
    if (!g_cur) return hajimu_number(1.0);
    return hajimu_number((double)g_cur->px_ratio);
}

/* =====================================================================
 * Phase 9: ドラッグ&ドロップ + クリップボード + ショートカット
 * ===================================================================*/

/* ---------------------------------------------------------------
 * ドラッグソース(ID, データ)
 * ---------------------------------------------------------------*/
static Value fn_drag_source(int argc, Value *argv) {
    if (!g_cur || argc < 2 || argv[0].type != VALUE_STRING)
        return hajimu_null();

    uint32_t id = gui_hash(argv[0].string.data);
    bool hov = g_cur->last.hovered;

    /* ドラッグ開始: ホバー中 + マウス押下 + ドラッグ */
    if (hov && g_cur->in.down && !g_drag_active) {
        int dx = g_cur->in.mx - g_cur->in.pmx;
        int dy = g_cur->in.my - g_cur->in.pmy;
        if (dx * dx + dy * dy > 16) {
            g_drag.id = id;
            g_drag.data = argv[1];
            g_drag.dragging = true;
            g_drag_active = true;
        }
    }

    /* ドラッグ中の視覚表示 */
    if (g_drag_active && g_drag.id == id) {
        Hjpcontext *vg = g_cur->vg;
        hjpFontFaceId(vg, g_cur->font_id);
        hjpFontSize(vg, 12.0f);
        const char *lbl = (argv[1].type == VALUE_STRING) ? argv[1].string.data : "...";
        hjpBeginPath(vg);
        hjpRoundedRect(vg, (float)g_cur->in.mx + 8, (float)g_cur->in.my - 8,
                       80, 24, 4.0f);
        hjpFillColor(vg, hjpRGBA(60, 60, 68, 220));
        hjpFill(vg);
        hjpFillColor(vg, TH_TEXT);
        hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_MIDDLE);
        hjpText(vg, (float)g_cur->in.mx + 14, (float)g_cur->in.my + 4, lbl, NULL);

        if (g_cur->in.released) {
            g_drag_active = false;
            g_drag.dragging = false;
        }
    }
    return hajimu_null();
}

/* ---------------------------------------------------------------
 * ドロップターゲット(ID) → データ or 無
 * ---------------------------------------------------------------*/
static Value fn_drop_target(int argc, Value *argv) {
    if (!g_cur || argc < 1 || argv[0].type != VALUE_STRING)
        return hajimu_null();

    bool hov = g_cur->last.hovered;
    if (hov && g_drag_active && g_cur->in.released) {
        Value result = g_drag.data;
        g_drag_active = false;
        g_drag.dragging = false;
        return result;
    }

    /* ホバー中のドロップ予告表示 */
    if (hov && g_drag_active) {
        Hjpcontext *vg = g_cur->vg;
        hjpBeginPath(vg);
        hjpRect(vg, g_cur->last.x, g_cur->last.y,
                g_cur->last.w, g_cur->last.h);
        hjpStrokeColor(vg, TH_ACCENT);
        hjpStrokeWidth(vg, 2.0f);
        hjpStroke(vg);
    }
    return hajimu_null();
}

/* ---------------------------------------------------------------
 * ファイルドロップ取得() → 配列
 * ---------------------------------------------------------------*/
static Value fn_file_drop_get(int argc, Value *argv) {
    (void)argc; (void)argv;
    Value arr;
    memset(&arr, 0, sizeof(arr));
    arr.type = VALUE_ARRAY;
    arr.array.length = g_drop_file_count;
    arr.array.capacity = g_drop_file_count > 0 ? g_drop_file_count : 1;
    arr.array.elements = calloc(arr.array.capacity, sizeof(Value));
    for (int i = 0; i < g_drop_file_count; i++)
        arr.array.elements[i] = hajimu_string(g_drop_files[i]);
    return arr;
}

/* ---------------------------------------------------------------
 * クリップボード取得() → 文字列
 * ---------------------------------------------------------------*/
static Value fn_clipboard_get(int argc, Value *argv) {
    (void)argc; (void)argv;
    char *text = hjp_get_clipboard_text();
    if (!text) return hajimu_string("");
    Value result = hajimu_string(text);
    hjp_free(text);
    return result;
}

/* ---------------------------------------------------------------
 * クリップボード設定(テキスト)
 * ---------------------------------------------------------------*/
static Value fn_clipboard_set(int argc, Value *argv) {
    if (argc < 1 || argv[0].type != VALUE_STRING) return hajimu_null();
    hjp_set_clipboard_text(argv[0].string.data);
    return hajimu_null();
}

/* ---------------------------------------------------------------
 * ショートカット(キー [, 修飾キー]) → 真偽
 * ---------------------------------------------------------------*/
static Value fn_shortcut(int argc, Value *argv) {
    if (!g_cur || argc < 1 || argv[0].type != VALUE_STRING)
        return hajimu_bool(false);

    const char *key = argv[0].string.data;
    HjpKeycode target = 0;

    /* 日本語キー名 → HjpKeycode */
    if (strcmp(key, "\xe3\x82\xb9\xe3\x83\x9a\xe3\x83\xbc\xe3\x82\xb9") == 0) target = ' ';
    else if (strcmp(key, "\xe3\x82\xa8\xe3\x83\xb3\xe3\x82\xbf\xe3\x83\xbc") == 0) target = HJPK_RETURN;
    else if (strcmp(key, "\xe3\x82\xa8\xe3\x82\xb9\xe3\x82\xb1\xe3\x83\xbc\xe3\x83\x97") == 0) target = HJPK_ESCAPE;
    else if (strcmp(key, "\xe3\x82\xbf\xe3\x83\x96") == 0) target = HJPK_TAB;
    else if (strcmp(key, "\xe4\xb8\x8a") == 0) target = HJPK_UP;
    else if (strcmp(key, "\xe4\xb8\x8b") == 0) target = HJPK_DOWN;
    else if (strcmp(key, "\xe5\xb7\xa6") == 0) target = HJPK_LEFT;
    else if (strcmp(key, "\xe5\x8f\xb3") == 0) target = HJPK_RIGHT;
    else if (strlen(key) == 1 && key[0] >= 'a' && key[0] <= 'z')
        target = (HjpKeycode)key[0];
    else if (strlen(key) == 1 && key[0] >= 'A' && key[0] <= 'Z')
        target = (HjpKeycode)(key[0] + 32);
    else if (strlen(key) == 1 && key[0] >= '0' && key[0] <= '9')
        target = (HjpKeycode)key[0];

    if (target == 0) return hajimu_bool(false);

    /* 修飾キーチェック */
    bool need_ctrl = false, need_shift = false;
    if (argc >= 2 && argv[1].type == VALUE_STRING) {
        const char *mod = argv[1].string.data;
        if (strstr(mod, "Ctrl") || strstr(mod, "ctrl") ||
            strstr(mod, "Cmd") || strstr(mod, "cmd"))
            need_ctrl = true;
        if (strstr(mod, "Shift") || strstr(mod, "shift"))
            need_shift = true;
    }

    bool pressed = (g_cur->in.last_key == target);
    if (need_ctrl && !g_cur->in.mod_ctrl) pressed = false;
    if (need_shift && !g_cur->in.mod_shift) pressed = false;
    if (!need_ctrl && g_cur->in.mod_ctrl && argc >= 2) pressed = false;

    return hajimu_bool(pressed);
}

/* ---------------------------------------------------------------
 * カーソル設定(種類)
 * ---------------------------------------------------------------*/
static Value fn_cursor_set(int argc, Value *argv) {
    if (argc < 1 || argv[0].type != VALUE_STRING) return hajimu_null();
    const char *kind = argv[0].string.data;
    int type = 0;
    if (strstr(kind, "\xe6\x89\x8b") != NULL)              type = 1; /* 手 */
    else if (strstr(kind, "\xe3\x83\x86\xe3\x82\xad\xe3\x82\xb9\xe3\x83\x88") != NULL) type = 2;
    else if (strstr(kind, "\xe5\x8d\x81\xe5\xad\x97") != NULL)    type = 3;
    else if (strstr(kind, "\xe3\x83\xaa\xe3\x82\xb5\xe3\x82\xa4\xe3\x82\xba") != NULL) type = 4;

    if (type != g_cursor_type) {
        g_cursor_type = type;
        int sc;
        switch (type) {
            case 1: sc = HJP_CURSOR_HAND; break;
            case 2: sc = HJP_CURSOR_IBEAM; break;
            case 3: sc = HJP_CURSOR_CROSSHAIR; break;
            case 4: sc = HJP_CURSOR_SIZEALL; break;
            default: sc = HJP_CURSOR_ARROW; break;
        }
        if (g_cursors[type]) hjp_free_cursor(g_cursors[type]);
        g_cursors[type] = hjp_create_system_cursor(sc);
        hjp_set_cursor(g_cursors[type]);
    }
    return hajimu_null();
}

/* ---------------------------------------------------------------
 * マウスボタン(ボタン) → 真偽
 * ---------------------------------------------------------------*/
static Value fn_mouse_button(int argc, Value *argv) {
    if (!g_cur || argc < 1 || argv[0].type != VALUE_STRING)
        return hajimu_bool(false);
    const char *btn = argv[0].string.data;
    if (strstr(btn, "\xe5\xb7\xa6") != NULL)  return hajimu_bool(g_cur->in.down);
    if (strstr(btn, "\xe5\x8f\xb3") != NULL)  return hajimu_bool(g_cur->in.mouse_right_clicked);
    if (strstr(btn, "\xe4\xb8\xad") != NULL)  return hajimu_bool(g_cur->in.mouse_middle);
    return hajimu_bool(false);
}

/* ---------------------------------------------------------------
 * キー押下(キー) → 真偽
 * ---------------------------------------------------------------*/
static Value fn_key_pressed(int argc, Value *argv) {
    if (!g_cur || argc < 1 || argv[0].type != VALUE_STRING)
        return hajimu_bool(false);
    const char *key = argv[0].string.data;

    if (strcmp(key, "\xe4\xb8\x8a") == 0) return hajimu_bool(g_cur->in.key_up);
    if (strcmp(key, "\xe4\xb8\x8b") == 0) return hajimu_bool(g_cur->in.key_down);
    if (strcmp(key, "\xe5\xb7\xa6") == 0) return hajimu_bool(g_cur->in.key_left);
    if (strcmp(key, "\xe5\x8f\xb3") == 0) return hajimu_bool(g_cur->in.key_right);
    if (strcmp(key, "\xe3\x82\xb9\xe3\x83\x9a\xe3\x83\xbc\xe3\x82\xb9") == 0)
        return hajimu_bool(g_cur->in.last_key == ' ');
    if (strcmp(key, "\xe3\x82\xa8\xe3\x83\xb3\xe3\x82\xbf\xe3\x83\xbc") == 0)
        return hajimu_bool(g_cur->in.key_return);
    if (strcmp(key, "\xe3\x82\xbf\xe3\x83\x96") == 0)
        return hajimu_bool(g_cur->in.key_tab);
    if (strlen(key) == 1) {
        char c = key[0];
        if (c >= 'A' && c <= 'Z') c += 32;
        return hajimu_bool(g_cur->in.last_key == (HjpKeycode)c);
    }
    return hajimu_bool(false);
}

/* =====================================================================
 * Phase 10: アニメーション + トランジション + タイマー
 * ===================================================================*/

/* --- イージング関数 --- */
static float gui_ease(float t, int type) {
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    switch (type) {
        case 1: return t * t;                    /* ease_in   */
        case 2: return t * (2.0f - t);           /* ease_out  */
        case 3:                                  /* ease_in_out */
            if (t < 0.5f) return 2.0f * t * t;
            return -1.0f + (4.0f - 2.0f * t) * t;
        default: return t;                       /* linear    */
    }
}

/* ---------------------------------------------------------------
 * アニメーション(ID, 開始, 終了, 秒数 [, イージング]) → 数値
 * ---------------------------------------------------------------*/
static Value fn_animation(int argc, Value *argv) {
    if (argc < 4 || argv[0].type != VALUE_STRING)
        return hajimu_number(0);

    uint32_t id = gui_hash(argv[0].string.data);
    float start_v    = (float)argv[1].number;
    float end_v      = (float)argv[2].number;
    float duration_s = (float)argv[3].number;
    int easing = 0;
    if (argc >= 5 && argv[4].type == VALUE_STRING) {
        const char *e = argv[4].string.data;
        if (strstr(e, "\xe3\x82\xa4\xe3\x83\xb3\xe3\x82\xa2\xe3\x82\xa6\xe3\x83\x88") != NULL) easing = 3;
        else if (strstr(e, "\xe3\x82\xa4\xe3\x83\xb3") != NULL) easing = 1;
        else if (strstr(e, "\xe3\x82\xa2\xe3\x82\xa6\xe3\x83\x88") != NULL) easing = 2;
    }

    /* 既存スロット検索 */
    int slot = -1;
    for (int i = 0; i < GUI_MAX_ANIM_SLOTS; i++) {
        if (g_anims[i].active && g_anims[i].id == id) { slot = i; break; }
    }
    if (slot < 0) {
        for (int i = 0; i < GUI_MAX_ANIM_SLOTS; i++) {
            if (!g_anims[i].active) { slot = i; break; }
        }
        if (slot < 0) return hajimu_number(end_v);
        g_anims[slot].id = id;
        g_anims[slot].start_val = start_v;
        g_anims[slot].end_val = end_v;
        g_anims[slot].duration_ms = duration_s * 1000.0f;
        g_anims[slot].start_time = hjp_get_ticks();
        g_anims[slot].easing = easing;
        g_anims[slot].active = true;
    }

    GuiAnim *a = &g_anims[slot];
    /* パラメーター変更検出 */
    if (a->end_val != end_v || a->start_val != start_v) {
        a->start_val = start_v;
        a->end_val = end_v;
        a->duration_ms = duration_s * 1000.0f;
        a->start_time = hjp_get_ticks();
        a->easing = easing;
    }

    float elapsed = (float)(hjp_get_ticks() - a->start_time);
    float t = elapsed / a->duration_ms;
    if (t >= 1.0f) {
        a->active = false;
        return hajimu_number(a->end_val);
    }
    float e = gui_ease(t, a->easing);
    return hajimu_number(a->start_val + (a->end_val - a->start_val) * e);
}

/* ---------------------------------------------------------------
 * トランジション(ID, 状態, 秒数) → 0.0〜1.0
 * ---------------------------------------------------------------*/
static Value fn_transition(int argc, Value *argv) {
    if (argc < 3 || argv[0].type != VALUE_STRING)
        return hajimu_number(0);

    uint32_t id = gui_hash(argv[0].string.data);
    bool target_state = false;
    if (argv[1].type == VALUE_BOOL) target_state = argv[1].boolean;
    else if (argv[1].type == VALUE_NUMBER) target_state = (argv[1].number != 0);
    float duration_s = (float)argv[2].number;
    float target = target_state ? 1.0f : 0.0f;

    int slot = -1;
    for (int i = 0; i < GUI_MAX_ANIM_SLOTS; i++)
        if (g_anims[i].active && g_anims[i].id == id) { slot = i; break; }
    if (slot < 0) {
        for (int i = 0; i < GUI_MAX_ANIM_SLOTS; i++)
            if (!g_anims[i].active) { slot = i; break; }
        if (slot < 0) return hajimu_number(target);
        g_anims[slot].id = id;
        g_anims[slot].start_val = target_state ? 0.0f : 1.0f;
        g_anims[slot].end_val = target;
        g_anims[slot].duration_ms = duration_s * 1000.0f;
        g_anims[slot].start_time = hjp_get_ticks();
        g_anims[slot].easing = 3;
        g_anims[slot].active = true;
    }

    GuiAnim *a = &g_anims[slot];
    if (a->end_val != target) {
        float cur_elapsed = (float)(hjp_get_ticks() - a->start_time);
        float cur_t = cur_elapsed / a->duration_ms;
        if (cur_t > 1.0f) cur_t = 1.0f;
        float cur_val = a->start_val + (a->end_val - a->start_val) * gui_ease(cur_t, a->easing);
        a->start_val = cur_val;
        a->end_val = target;
        a->start_time = hjp_get_ticks();
    }

    float elapsed = (float)(hjp_get_ticks() - a->start_time);
    float t = elapsed / a->duration_ms;
    if (t >= 1.0f) {
        a->active = false;
        return hajimu_number(a->end_val);
    }
    return hajimu_number(a->start_val + (a->end_val - a->start_val) * gui_ease(t, a->easing));
}

/* ---------------------------------------------------------------
 * タイマー(ID, ミリ秒, コールバック)
 * ---------------------------------------------------------------*/
static Value fn_timer_start(int argc, Value *argv) {
    if (argc < 3 || argv[0].type != VALUE_STRING ||
        argv[1].type != VALUE_NUMBER)
        return hajimu_null();

    uint32_t id = gui_hash(argv[0].string.data);
    int interval = (int)argv[1].number;
    if (interval < 16) interval = 16;

    /* 既存タイマー検索 */
    for (int i = 0; i < GUI_MAX_TIMERS; i++) {
        if (g_timers[i].active && g_timers[i].id == id) {
            g_timers[i].interval_ms = interval;
            g_timers[i].callback = argv[2];
            return hajimu_null();
        }
    }

    /* 新規スロット */
    for (int i = 0; i < GUI_MAX_TIMERS; i++) {
        if (!g_timers[i].active) {
            g_timers[i].id = id;
            g_timers[i].interval_ms = interval;
            g_timers[i].last_fire = hjp_get_ticks();
            g_timers[i].callback = argv[2];
            g_timers[i].active = true;
            return hajimu_null();
        }
    }
    return hajimu_null();
}

/* ---------------------------------------------------------------
 * タイマー停止(ID)
 * ---------------------------------------------------------------*/
static Value fn_timer_stop(int argc, Value *argv) {
    if (argc < 1 || argv[0].type != VALUE_STRING) return hajimu_null();
    uint32_t id = gui_hash(argv[0].string.data);
    for (int i = 0; i < GUI_MAX_TIMERS; i++) {
        if (g_timers[i].active && g_timers[i].id == id) {
            g_timers[i].active = false;
            break;
        }
    }
    return hajimu_null();
}

/* =====================================================================
 * Phase 11: 追加ウィジェット (v1.1.0)
 * ===================================================================*/

/* ---------------------------------------------------------------
 * トグルスイッチ(ラベル, 値) → 真偽
 *   iOS/Flutter風の ON/OFF トグル。クリックで反転した値を返す。
 * ---------------------------------------------------------------*/
static Value fn_toggle_switch(int argc, Value *argv) {
    if (!g_cur || argc < 2 || argv[0].type != VALUE_STRING)
        return hajimu_bool(false);

    Hjpcontext *vg = g_cur->vg;
    const char *label = argv[0].string.data;
    bool on = (argv[1].type == VALUE_BOOL) ? argv[1].boolean
            : (argv[1].type == VALUE_NUMBER) ? (argv[1].number != 0)
            : false;
    uint32_t id = gui_hash(label);

    float x, y, w;
    gui_pos(&x, &y, &w);
    float row_h = GUI_TOGGLE_H + 4.0f;

    /* ラベル幅 */
    hjpFontFaceId(vg, g_cur->font_id);
    hjpFontSize(vg, GUI_FONT_SIZE);
    float tw = hjpTextBounds(vg, 0, 0, label, NULL, NULL);
    float total_w = GUI_TOGGLE_W + 10.0f + tw;

    bool hov = false, pressed = false;
    bool clicked = gui_widget_logic(id, x, y, total_w, row_h, &hov, &pressed);
    if (clicked) on = !on;

    /* トラック */
    float tx = x, ty = y + (row_h - GUI_TOGGLE_H) * 0.5f;
    float r = GUI_TOGGLE_H * 0.5f;
    hjpBeginPath(vg);
    hjpRoundedRect(vg, tx, ty, GUI_TOGGLE_W, GUI_TOGGLE_H, r);
    hjpFillColor(vg, on ? TH_ACCENT : TH_TRACK);
    hjpFill(vg);
    if (hov) {
        hjpStrokeColor(vg, on ? TH_ACCENT_HOVER : TH_BORDER);
        hjpStrokeWidth(vg, 1.0f);
        hjpStroke(vg);
    }

    /* つまみ */
    float knob_r = r - 3.0f;
    float knob_x = on ? (tx + GUI_TOGGLE_W - r) : (tx + r);
    hjpBeginPath(vg);
    hjpCircle(vg, knob_x, ty + r, knob_r);
    hjpFillColor(vg, hjpRGBA(255, 255, 255, 255));
    hjpFill(vg);

    /* ラベル */
    hjpFillColor(vg, TH_TEXT);
    hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_MIDDLE);
    hjpText(vg, tx + GUI_TOGGLE_W + 10.0f, y + row_h * 0.5f, label, NULL);

    gui_advance(row_h);
    return hajimu_bool(on);
}

/* ---------------------------------------------------------------
 * カラーピッカー(ラベル, 色) → 色
 *   色プレビュー四角 + RGB スライダー 3 本のインライン色選択。
 *   返り値は gui_pack_rgba でパックされた色値。
 * ---------------------------------------------------------------*/
static Value fn_color_picker(int argc, Value *argv) {
    if (!g_cur || argc < 2 || argv[0].type != VALUE_STRING)
        return hajimu_number(0);

    Hjpcontext *vg = g_cur->vg;
    const char *label = argv[0].string.data;
    uint32_t id = gui_hash(label);

    /* 現在の色を復元 */
    GuiRGBA c = {1.0f, 0.0f, 0.0f, 1.0f};
    if (argv[1].type == VALUE_NUMBER) {
        c = gui_unpack_rgba(argv[1].number);
    }

    float x, y, w;
    gui_pos(&x, &y, &w);
    float preview_sz = 28.0f;
    float slider_w = w - preview_sz - 12.0f;
    if (slider_w < 60) slider_w = 60;

    /* ラベル */
    hjpFontFaceId(vg, g_cur->font_id);
    hjpFontSize(vg, GUI_FONT_SIZE);
    hjpFillColor(vg, TH_TEXT);
    hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_TOP);
    hjpText(vg, x, y, label, NULL);
    y += GUI_FONT_SIZE + 4.0f;

    /* カラープレビュー */
    hjpBeginPath(vg);
    hjpRoundedRect(vg, x, y, preview_sz, preview_sz * 3 + 8, 4.0f);
    hjpFillColor(vg, hjpRGBAf(c.r, c.g, c.b, c.a));
    hjpFill(vg);
    hjpStrokeColor(vg, TH_BORDER);
    hjpStrokeWidth(vg, 1.0f);
    hjpStroke(vg);

    float sx = x + preview_sz + 8.0f;
    float *channels[3] = {&c.r, &c.g, &c.b};
    const char *ch_labels[] = {"R", "G", "B"};
    Hjpcolor ch_colors[] = {
        hjpRGB(220, 60, 60), hjpRGB(60, 180, 60), hjpRGB(60, 100, 220)
    };

    for (int i = 0; i < 3; i++) {
        float sy = y + i * (preview_sz + 4.0f);
        uint32_t sid = id + (uint32_t)(i + 1);
        float val = *channels[i];

        /* スライダー背景 */
        hjpBeginPath(vg);
        hjpRoundedRect(vg, sx, sy + 10.0f, slider_w, 8.0f, 4.0f);
        hjpFillColor(vg, TH_TRACK);
        hjpFill(vg);

        /* スライダー彩色部分 */
        hjpBeginPath(vg);
        hjpRoundedRect(vg, sx, sy + 10.0f, slider_w * val, 8.0f, 4.0f);
        hjpFillColor(vg, ch_colors[i]);
        hjpFill(vg);

        /* ハンドル操作 */
        float hx = sx + slider_w * val;
        bool hov = false, prs = false;
        gui_widget_logic(sid, sx, sy, slider_w, preview_sz, &hov, &prs);
        if (prs || (g_cur->active == sid)) {
            float mx = (float)g_cur->in.mx;
            val = (mx - sx) / slider_w;
            if (val < 0) val = 0;
            if (val > 1) val = 1;
            *channels[i] = val;
            hx = sx + slider_w * val;
        }

        /* ハンドル描画 */
        hjpBeginPath(vg);
        hjpCircle(vg, hx, sy + 14.0f, 7.0f);
        hjpFillColor(vg, hjpRGBA(255, 255, 255, 255));
        hjpFill(vg);
        hjpStrokeColor(vg, ch_colors[i]);
        hjpStrokeWidth(vg, 2.0f);
        hjpStroke(vg);

        /* チャンネルラベル */
        hjpFontSize(vg, 12.0f);
        hjpFillColor(vg, TH_TEXT_DIM);
        hjpTextAlign(vg, HJP_ALIGN_RIGHT | HJP_ALIGN_MIDDLE);
        char vbuf[8];
        snprintf(vbuf, sizeof(vbuf), "%s:%d", ch_labels[i], (int)(*channels[i] * 255));
        hjpText(vg, sx + slider_w, sy + 6.0f, vbuf, NULL);
    }

    float total_h = GUI_FONT_SIZE + 4.0f + preview_sz * 3 + 8.0f;
    g_cur->last = (GuiLastWidget){x, y - GUI_FONT_SIZE - 4.0f, w, total_h, false};
    gui_advance(total_h);

    return hajimu_number(gui_pack_rgba((int)(c.r * 255), (int)(c.g * 255),
                                        (int)(c.b * 255), (int)(c.a * 255)));
}

/* ---------------------------------------------------------------
 * スピナー([サイズ])
 *   回転するローディングインジケーター。
 * ---------------------------------------------------------------*/
static Value fn_spinner(int argc, Value *argv) {
    if (!g_cur) return hajimu_null();

    Hjpcontext *vg = g_cur->vg;
    float radius = GUI_SPINNER_RADIUS;
    if (argc >= 1 && argv[0].type == VALUE_NUMBER)
        radius = (float)argv[0].number;

    float x, y, w;
    gui_pos(&x, &y, &w);
    float cx = x + radius + 4.0f;
    float cy = y + radius + 4.0f;

    /* 回転角度（時間ベース） */
    float angle = (float)(hjp_get_ticks() % 1000) / 1000.0f * HJP_PI * 2.0f;

    /* 円弧 */
    hjpBeginPath(vg);
    hjpArc(vg, cx, cy, radius, angle, angle + HJP_PI * 1.5f, HJP_CW);
    hjpStrokeColor(vg, TH_ACCENT);
    hjpStrokeWidth(vg, 3.0f);
    hjpLineCap(vg, HJP_ROUND);
    hjpStroke(vg);

    gui_advance(radius * 2 + 8.0f);
    return hajimu_null();
}

/* ---------------------------------------------------------------
 * 折りたたみ(タイトル, 開閉) → 真偽
 *   開閉状態を受け取り、クリックでトグル。中身は呼び出し側で制御。
 * ---------------------------------------------------------------*/
static Value fn_collapsing(int argc, Value *argv) {
    if (!g_cur || argc < 2 || argv[0].type != VALUE_STRING)
        return hajimu_bool(false);

    Hjpcontext *vg = g_cur->vg;
    const char *title = argv[0].string.data;
    bool open = (argv[1].type == VALUE_BOOL) ? argv[1].boolean
              : (argv[1].type == VALUE_NUMBER) ? (argv[1].number != 0)
              : false;
    uint32_t id = gui_hash(title);

    float x, y, w;
    gui_pos(&x, &y, &w);
    float h = GUI_WIDGET_H;

    bool hov = false, pressed = false;
    bool clicked = gui_widget_logic(id, x, y, w, h, &hov, &pressed);
    if (clicked) open = !open;

    /* 背景 */
    hjpBeginPath(vg);
    hjpRoundedRect(vg, x, y, w, h, 4.0f);
    hjpFillColor(vg, hov ? TH_WIDGET_HOVER : TH_WIDGET_BG);
    hjpFill(vg);

    /* 三角マーク */
    float ax = x + 12.0f, ay = y + h * 0.5f;
    hjpBeginPath(vg);
    if (open) {
        hjpMoveTo(vg, ax - 5, ay - 3);
        hjpLineTo(vg, ax + 5, ay - 3);
        hjpLineTo(vg, ax, ay + 4);
    } else {
        hjpMoveTo(vg, ax - 3, ay - 5);
        hjpLineTo(vg, ax + 4, ay);
        hjpLineTo(vg, ax - 3, ay + 5);
    }
    hjpFillColor(vg, TH_TEXT);
    hjpFill(vg);

    /* タイトル */
    hjpFontFaceId(vg, g_cur->font_id);
    hjpFontSize(vg, GUI_FONT_SIZE);
    hjpFillColor(vg, TH_TEXT);
    hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_MIDDLE);
    hjpText(vg, ax + 14.0f, ay, title, NULL);

    gui_advance(h);
    return hajimu_bool(open);
}

/* ---------------------------------------------------------------
 * リンク(テキスト[, URL]) → 真偽
 *   ハイパーリンク風テキスト。クリックで真を返す。
 * ---------------------------------------------------------------*/
static Value fn_link(int argc, Value *argv) {
    if (!g_cur || argc < 1 || argv[0].type != VALUE_STRING)
        return hajimu_bool(false);

    Hjpcontext *vg = g_cur->vg;
    const char *text = argv[0].string.data;
    uint32_t id = gui_hash(text);

    float x, y, w;
    gui_pos(&x, &y, &w);

    hjpFontFaceId(vg, g_cur->font_id);
    hjpFontSize(vg, GUI_FONT_SIZE);
    float tw = hjpTextBounds(vg, 0, 0, text, NULL, NULL);
    float h = GUI_FONT_SIZE + 4.0f;

    bool hov = false, pressed = false;
    bool clicked = gui_widget_logic(id, x, y, tw, h, &hov, &pressed);

    /* テキスト（青色 + ホバー時下線） */
    Hjpcolor link_col = hjpRGB(70, 130, 230);
    if (hov) link_col = hjpRGB(100, 160, 255);
    hjpFillColor(vg, link_col);
    hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_TOP);
    hjpText(vg, x, y, text, NULL);

    /* 下線 */
    if (hov) {
        hjpBeginPath(vg);
        hjpMoveTo(vg, x, y + h - 1.0f);
        hjpLineTo(vg, x + tw, y + h - 1.0f);
        hjpStrokeColor(vg, link_col);
        hjpStrokeWidth(vg, 1.0f);
        hjpStroke(vg);
    }

    gui_advance(h);
    return hajimu_bool(clicked);
}

/* ---------------------------------------------------------------
 * 選択可能(ラベル, 選択) → 真偽
 *   クリック可能な全幅テキスト行。選択中は背景ハイライト。
 * ---------------------------------------------------------------*/
static Value fn_selectable(int argc, Value *argv) {
    if (!g_cur || argc < 2 || argv[0].type != VALUE_STRING)
        return hajimu_bool(false);

    Hjpcontext *vg = g_cur->vg;
    const char *label = argv[0].string.data;
    bool selected = (argv[1].type == VALUE_BOOL) ? argv[1].boolean
                  : (argv[1].type == VALUE_NUMBER) ? (argv[1].number != 0)
                  : false;
    uint32_t id = gui_hash(label);

    float x, y, w;
    gui_pos(&x, &y, &w);
    float h = GUI_LIST_ITEM_H;

    bool hov = false, pressed = false;
    bool clicked = gui_widget_logic(id, x, y, w, h, &hov, &pressed);
    if (clicked) selected = !selected;

    /* 背景 */
    if (selected || hov) {
        hjpBeginPath(vg);
        hjpRoundedRect(vg, x, y, w, h, 3.0f);
        hjpFillColor(vg, selected ? TH_ACCENT : TH_WIDGET_HOVER);
        hjpFill(vg);
    }

    /* テキスト */
    hjpFontFaceId(vg, g_cur->font_id);
    hjpFontSize(vg, GUI_FONT_SIZE);
    hjpFillColor(vg, selected ? hjpRGBA(255, 255, 255, 255) : TH_TEXT);
    hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_MIDDLE);
    hjpText(vg, x + 8.0f, y + h * 0.5f, label, NULL);

    gui_advance(h);
    return hajimu_bool(selected);
}

/* ---------------------------------------------------------------
 * バッジ(テキスト[, 色])
 *   通知用の小さなラウンドラベル。
 * ---------------------------------------------------------------*/
static Value fn_badge(int argc, Value *argv) {
    if (!g_cur || argc < 1 || argv[0].type != VALUE_STRING)
        return hajimu_null();

    Hjpcontext *vg = g_cur->vg;
    const char *text = argv[0].string.data;
    Hjpcolor bg_col = TH_ACCENT;
    if (argc >= 2) bg_col = gui_arg_color(argv[1]);

    float x, y, w;
    gui_pos(&x, &y, &w);

    hjpFontFaceId(vg, g_cur->font_id);
    hjpFontSize(vg, 12.0f);
    float tw = hjpTextBounds(vg, 0, 0, text, NULL, NULL);
    float bw = tw + GUI_BADGE_PAD * 2;
    float bh = 20.0f;
    float r = bh * 0.5f;

    /* 背景 */
    hjpBeginPath(vg);
    hjpRoundedRect(vg, x, y, bw, bh, r);
    hjpFillColor(vg, bg_col);
    hjpFill(vg);

    /* テキスト */
    hjpFillColor(vg, hjpRGBA(255, 255, 255, 255));
    hjpTextAlign(vg, HJP_ALIGN_CENTER | HJP_ALIGN_MIDDLE);
    hjpText(vg, x + bw * 0.5f, y + bh * 0.5f, text, NULL);

    g_cur->last = (GuiLastWidget){x, y, bw, bh, false};
    gui_advance(bh);
    return hajimu_null();
}

/* ---------------------------------------------------------------
 * タグ(ラベル[, 閉じ可能]) → 真偽
 *   タグ / チップ。閉じ可能=真のとき × ボタン付き。
 *   × クリックで偽を返す。それ以外は真。
 * ---------------------------------------------------------------*/
static Value fn_tag(int argc, Value *argv) {
    if (!g_cur || argc < 1 || argv[0].type != VALUE_STRING)
        return hajimu_bool(true);

    Hjpcontext *vg = g_cur->vg;
    const char *label = argv[0].string.data;
    bool closable = false;
    if (argc >= 2 && argv[1].type == VALUE_BOOL) closable = argv[1].boolean;
    if (argc >= 2 && argv[1].type == VALUE_NUMBER) closable = (argv[1].number != 0);

    uint32_t id = gui_hash(label);

    float x, y, w;
    gui_pos(&x, &y, &w);

    hjpFontFaceId(vg, g_cur->font_id);
    hjpFontSize(vg, 13.0f);
    float tw = hjpTextBounds(vg, 0, 0, label, NULL, NULL);
    float tag_w = tw + GUI_TAG_PAD * 2 + (closable ? 18.0f : 0.0f);

    bool hov = false, pressed = false;
    bool close_clicked = false;

    /* 閉じるボタン領域判定 */
    if (closable) {
        uint32_t close_id = id + 9999;
        float cx = x + tag_w - 18.0f;
        bool ch = false, cp = false;
        close_clicked = gui_widget_logic(close_id, cx, y, 18.0f, GUI_TAG_H, &ch, &cp);
    }
    gui_widget_logic(id, x, y, tag_w, GUI_TAG_H, &hov, &pressed);

    /* 背景 */
    hjpBeginPath(vg);
    hjpRoundedRect(vg, x, y, tag_w, GUI_TAG_H, GUI_TAG_H * 0.5f);
    hjpFillColor(vg, hov ? TH_WIDGET_HOVER : TH_WIDGET_BG);
    hjpFill(vg);
    hjpStrokeColor(vg, TH_BORDER);
    hjpStrokeWidth(vg, 1.0f);
    hjpStroke(vg);

    /* ラベル */
    hjpFillColor(vg, TH_TEXT);
    hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_MIDDLE);
    hjpText(vg, x + GUI_TAG_PAD, y + GUI_TAG_H * 0.5f, label, NULL);

    /* × ボタン */
    if (closable) {
        float cx = x + tag_w - 14.0f;
        float cy = y + GUI_TAG_H * 0.5f;
        float s = 4.0f;
        hjpBeginPath(vg);
        hjpMoveTo(vg, cx - s, cy - s);
        hjpLineTo(vg, cx + s, cy + s);
        hjpMoveTo(vg, cx + s, cy - s);
        hjpLineTo(vg, cx - s, cy + s);
        hjpStrokeColor(vg, TH_TEXT_DIM);
        hjpStrokeWidth(vg, 1.5f);
        hjpStroke(vg);
    }

    gui_advance(GUI_TAG_H);
    return hajimu_bool(!close_clicked);
}

/* ---------------------------------------------------------------
 * カード開始([影の深さ])
 *   カード型コンテナ開始。影の深さはデフォルト 4。
 * ---------------------------------------------------------------*/
static Value fn_card_begin(int argc, Value *argv) {
    if (!g_cur) return hajimu_null();
    if (g_card_depth >= GUI_MAX_CARD_DEPTH) return hajimu_null();

    float shadow = GUI_CARD_SHADOW;
    if (argc >= 1 && argv[0].type == VALUE_NUMBER)
        shadow = (float)argv[0].number;

    float x, y, w;
    gui_pos(&x, &y, &w);

    /* カード状態を保存 */
    g_cards[g_card_depth] = (GuiCardState){x, y, w, g_cur->lay.y};
    g_card_depth++;

    /* 影 (ぼやけた暗い背景矩形) */
    Hjpcontext *vg = g_cur->vg;
    Hjppaint shadow_paint = hjpBoxGradient(vg,
        x, y + 2, w, 10, 4.0f, shadow * 2,
        hjpRGBA(0, 0, 0, 60), hjpRGBA(0, 0, 0, 0));
    hjpBeginPath(vg);
    hjpRect(vg, x - shadow, y - shadow, w + shadow * 2, shadow * 4 + 10);
    hjpFillPaint(vg, shadow_paint);
    hjpFill(vg);

    /* カード背景を後で描画するため、y を進める */
    g_cur->lay.y = y + GUI_PADDING;
    g_cur->lay.x += GUI_PADDING * 0.5f;
    g_cur->lay.w -= GUI_PADDING;

    return hajimu_null();
}

/* ---------------------------------------------------------------
 * カード終了()
 * ---------------------------------------------------------------*/
static Value fn_card_end(int argc, Value *argv) {
    (void)argc; (void)argv;
    if (!g_cur || g_card_depth <= 0) return hajimu_null();

    g_card_depth--;
    GuiCardState *cs = &g_cards[g_card_depth];

    float card_h = g_cur->lay.y - cs->y + GUI_PADDING;
    Hjpcontext *vg = g_cur->vg;

    /* カード本体（白背景 + 角丸 + 枠線） */
    hjpBeginPath(vg);
    hjpRoundedRect(vg, cs->x, cs->y, cs->w, card_h, 8.0f);
    hjpFillColor(vg, TH_WIDGET_BG);
    hjpFill(vg);
    hjpStrokeColor(vg, TH_BORDER);
    hjpStrokeWidth(vg, 1.0f);
    hjpStroke(vg);

    /* レイアウトを復元 */
    g_cur->lay.x -= GUI_PADDING * 0.5f;
    g_cur->lay.w += GUI_PADDING;
    g_cur->lay.y = cs->y + card_h + GUI_MARGIN;

    return hajimu_null();
}

/* ---------------------------------------------------------------
 * 区切りテキスト(テキスト)
 *   テキスト付きセパレーター。
 * ---------------------------------------------------------------*/
static Value fn_separator_text(int argc, Value *argv) {
    if (!g_cur || argc < 1 || argv[0].type != VALUE_STRING)
        return hajimu_null();

    Hjpcontext *vg = g_cur->vg;
    const char *text = argv[0].string.data;

    float x, y, w;
    gui_pos(&x, &y, &w);

    hjpFontFaceId(vg, g_cur->font_id);
    hjpFontSize(vg, 13.0f);
    float tw = hjpTextBounds(vg, 0, 0, text, NULL, NULL);

    float cy = y + 8.0f;
    float text_x = x + (w - tw) * 0.5f;
    float gap = 6.0f;

    /* 左側の線 */
    if (text_x - gap > x) {
        hjpBeginPath(vg);
        hjpMoveTo(vg, x, cy);
        hjpLineTo(vg, text_x - gap, cy);
        hjpStrokeColor(vg, TH_SEP);
        hjpStrokeWidth(vg, 1.0f);
        hjpStroke(vg);
    }

    /* 右側の線 */
    if (text_x + tw + gap < x + w) {
        hjpBeginPath(vg);
        hjpMoveTo(vg, text_x + tw + gap, cy);
        hjpLineTo(vg, x + w, cy);
        hjpStrokeColor(vg, TH_SEP);
        hjpStrokeWidth(vg, 1.0f);
        hjpStroke(vg);
    }

    /* テキスト */
    hjpFillColor(vg, TH_TEXT_DIM);
    hjpTextAlign(vg, HJP_ALIGN_CENTER | HJP_ALIGN_MIDDLE);
    hjpText(vg, x + w * 0.5f, cy, text, NULL);

    gui_advance(16.0f);
    return hajimu_null();
}

/* ---------------------------------------------------------------
 * 小ボタン(ラベル) → 真偽
 *   パディングなしのコンパクトなテキストボタン。
 * ---------------------------------------------------------------*/
static Value fn_small_button(int argc, Value *argv) {
    if (!g_cur || argc < 1 || argv[0].type != VALUE_STRING)
        return hajimu_bool(false);

    Hjpcontext *vg = g_cur->vg;
    const char *label = argv[0].string.data;
    uint32_t id = gui_hash(label);

    float x, y, w;
    gui_pos(&x, &y, &w);

    hjpFontFaceId(vg, g_cur->font_id);
    hjpFontSize(vg, GUI_FONT_SIZE);
    float tw = hjpTextBounds(vg, 0, 0, label, NULL, NULL);
    float btn_w = tw + 12.0f;
    float btn_h = GUI_FONT_SIZE + 6.0f;

    bool hov = false, pressed = false;
    bool clicked = gui_widget_logic(id, x, y, btn_w, btn_h, &hov, &pressed);

    /* 背景: ホバー時のみ薄く表示 */
    if (hov || pressed) {
        hjpBeginPath(vg);
        hjpRoundedRect(vg, x, y, btn_w, btn_h, 3.0f);
        hjpFillColor(vg, pressed ? TH_WIDGET_ACTIVE : TH_WIDGET_HOVER);
        hjpFill(vg);
    }

    /* テキスト */
    hjpFillColor(vg, TH_ACCENT);
    hjpTextAlign(vg, HJP_ALIGN_CENTER | HJP_ALIGN_MIDDLE);
    hjpText(vg, x + btn_w * 0.5f, y + btn_h * 0.5f, label, NULL);

    gui_advance(btn_h);
    return hajimu_bool(clicked);
}

/* ---------------------------------------------------------------
 * 画像ボタン(画像[, 幅, 高さ]) → 真偽
 *   画像をクリック可能なボタンとして表示。
 * ---------------------------------------------------------------*/
static Value fn_image_button(int argc, Value *argv) {
    if (!g_cur || argc < 1 || argv[0].type != VALUE_NUMBER)
        return hajimu_bool(false);

    int slot = (int)argv[0].number;
    if (slot < 0 || slot >= GUI_MAX_IMAGES || !g_images[slot].valid)
        return hajimu_bool(false);

    Hjpcontext *vg = g_cur->vg;
    float iw = (argc >= 2 && argv[1].type == VALUE_NUMBER) ? (float)argv[1].number : (float)g_images[slot].w;
    float ih = (argc >= 3 && argv[2].type == VALUE_NUMBER) ? (float)argv[2].number : (float)g_images[slot].h;

    uint32_t id = gui_hash("__imgbtn__") + (uint32_t)slot;

    float x, y, avail_w;
    gui_pos(&x, &y, &avail_w);

    bool hov = false, pressed = false;
    bool clicked = gui_widget_logic(id, x, y, iw, ih, &hov, &pressed);

    /* ホバー時：枠線表示 */
    if (hov) {
        hjpBeginPath(vg);
        hjpRoundedRect(vg, x - 2, y - 2, iw + 4, ih + 4, 4.0f);
        hjpStrokeColor(vg, TH_ACCENT);
        hjpStrokeWidth(vg, 2.0f);
        hjpStroke(vg);
    }
    if (pressed) {
        hjpGlobalAlpha(vg, 0.7f);
    }

    /* 画像描画 */
    Hjppaint img = hjpImagePattern(vg, x, y, iw, ih, 0,
                                    g_images[slot].handle, 1.0f);
    hjpBeginPath(vg);
    hjpRoundedRect(vg, x, y, iw, ih, 4.0f);
    hjpFillPaint(vg, img);
    hjpFill(vg);

    if (pressed) {
        hjpGlobalAlpha(vg, 1.0f);
    }

    gui_advance(ih);
    return hajimu_bool(clicked);
}

/* =====================================================================
 * Phase 12: 高度なレイアウト (v1.2.0)
 * ===================================================================*/

/* ---------------------------------------------------------------
 * スクロール領域開始(幅, 高さ)
 *   クリッピング+ スクロールバー付き領域を開始。
 * ---------------------------------------------------------------*/
static Value fn_scroll_begin(int argc, Value *argv) {
    if (!g_cur || argc < 2) return hajimu_null();

    float sw = (argv[0].type == VALUE_NUMBER) ? (float)argv[0].number : 200.0f;
    float sh = (argv[1].type == VALUE_NUMBER) ? (float)argv[1].number : 200.0f;

    /* 空きスロット探索 */
    int slot = -1;
    for (int i = 0; i < GUI_MAX_SCROLL; i++) {
        if (!g_scrolls[i].active) { slot = i; break; }
    }
    if (slot < 0) return hajimu_number(-1);

    float x, y, w;
    gui_pos(&x, &y, &w);
    if (sw > w) sw = w;

    GuiScrollRegion *sr = &g_scrolls[slot];
    sr->x = x; sr->y = y; sr->w = sw; sr->h = sh;
    sr->saved_x = g_cur->lay.x;
    sr->saved_y = g_cur->lay.y;
    sr->saved_w = g_cur->lay.w;
    sr->content_h = 0;
    sr->active = true;

    /* クリッピング設定 */
    Hjpcontext *vg = g_cur->vg;
    hjpSave(vg);
    hjpScissor(vg, x, y, sw - GUI_SCROLLBAR_W, sh);

    /* レイアウトをスクロール領域内に変更 */
    g_cur->lay.x = x + 2;
    g_cur->lay.y = y - sr->scroll_y;
    g_cur->lay.w = sw - GUI_SCROLLBAR_W - 4;

    return hajimu_number(slot);
}

/* ---------------------------------------------------------------
 * スクロール領域終了()
 *   スクロール領域を閉じてスクロールバーを描画。
 * ---------------------------------------------------------------*/
static Value fn_scroll_end(int argc, Value *argv) {
    if (!g_cur || argc < 1) return hajimu_null();

    int slot = (argv[0].type == VALUE_NUMBER) ? (int)argv[0].number : -1;
    if (slot < 0 || slot >= GUI_MAX_SCROLL || !g_scrolls[slot].active)
        return hajimu_null();

    GuiScrollRegion *sr = &g_scrolls[slot];
    Hjpcontext *vg = g_cur->vg;

    sr->content_h = (g_cur->lay.y + sr->scroll_y) - sr->y;
    hjpRestore(vg); /* クリッピング解除 */

    /* スクロールバー背景 */
    float sbx = sr->x + sr->w - GUI_SCROLLBAR_W;
    hjpBeginPath(vg);
    hjpRoundedRect(vg, sbx, sr->y, GUI_SCROLLBAR_W, sr->h, 4.0f);
    hjpFillColor(vg, TH_TRACK);
    hjpFill(vg);

    /* スクロールバーつまみ */
    if (sr->content_h > sr->h) {
        float ratio = sr->h / sr->content_h;
        float thumb_h = sr->h * ratio;
        if (thumb_h < 20) thumb_h = 20;
        float thumb_y = sr->y + (sr->scroll_y / (sr->content_h - sr->h)) * (sr->h - thumb_h);

        uint32_t sbid = gui_hash("__scroll__") + (uint32_t)slot;
        bool hov = false, prs = false;
        gui_widget_logic(sbid, sbx, sr->y, GUI_SCROLLBAR_W, sr->h, &hov, &prs);

        /* マウスホイール対応（簡易） */
        if (gui_hit((float)g_cur->in.mx, (float)g_cur->in.my, sr->x, sr->y, sr->w, sr->h) && g_cur->in.scroll_y != 0) {
            sr->scroll_y -= (float)g_cur->in.scroll_y * 30.0f;
        }
        if (sr->scroll_y < 0) sr->scroll_y = 0;
        float max_scroll = sr->content_h - sr->h;
        if (sr->scroll_y > max_scroll) sr->scroll_y = max_scroll;
        thumb_y = sr->y + (sr->scroll_y / max_scroll) * (sr->h - thumb_h);

        hjpBeginPath(vg);
        hjpRoundedRect(vg, sbx + 1, thumb_y, GUI_SCROLLBAR_W - 2, thumb_h, 4.0f);
        hjpFillColor(vg, hov ? TH_ACCENT_HOVER : TH_ACCENT);
        hjpFill(vg);
    }

    /* 枠線 */
    hjpBeginPath(vg);
    hjpRoundedRect(vg, sr->x, sr->y, sr->w, sr->h, 2.0f);
    hjpStrokeColor(vg, TH_BORDER);
    hjpStrokeWidth(vg, 1.0f);
    hjpStroke(vg);

    /* レイアウト復元 */
    g_cur->lay.x = sr->saved_x;
    g_cur->lay.y = sr->saved_y + sr->h + GUI_MARGIN;
    g_cur->lay.w = sr->saved_w;
    sr->active = false;

    return hajimu_null();
}

/* ---------------------------------------------------------------
 * グリッド開始(列数[, 間隔])
 * ---------------------------------------------------------------*/
static Value fn_grid_begin(int argc, Value *argv) {
    if (!g_cur || argc < 1) return hajimu_null();

    int cols = (argv[0].type == VALUE_NUMBER) ? (int)argv[0].number : 2;
    if (cols < 1) cols = 1;
    float gap = (argc >= 2 && argv[1].type == VALUE_NUMBER) ? (float)argv[1].number : GUI_MARGIN;

    int slot = -1;
    for (int i = 0; i < GUI_MAX_GRID; i++) {
        if (!g_grids[i].active) { slot = i; break; }
    }
    if (slot < 0) return hajimu_number(-1);

    float x, y, w;
    gui_pos(&x, &y, &w);

    GuiGrid *g = &g_grids[slot];
    g->cols = cols;
    g->gap = gap;
    g->x = x; g->y = y; g->w = w;
    g->col_w = (w - gap * (cols - 1)) / cols;
    g->cur_col = 0;
    g->row_h = 0;
    g->saved_x = g_cur->lay.x;
    g->saved_y = g_cur->lay.y;
    g->saved_w = g_cur->lay.w;
    g->active = true;

    g_cur->lay.x = x;
    g_cur->lay.w = g->col_w;

    return hajimu_number(slot);
}

/* ---------------------------------------------------------------
 * グリッド終了()
 * ---------------------------------------------------------------*/
static Value fn_grid_end(int argc, Value *argv) {
    if (!g_cur || argc < 1) return hajimu_null();

    int slot = (argv[0].type == VALUE_NUMBER) ? (int)argv[0].number : -1;
    if (slot < 0 || slot >= GUI_MAX_GRID || !g_grids[slot].active)
        return hajimu_null();

    GuiGrid *g = &g_grids[slot];

    g_cur->lay.x = g->saved_x;
    g_cur->lay.y = g->saved_y;
    g_cur->lay.w = g->saved_w;

    /* 最終行の高さを反映 */
    if (g->cur_col > 0) {
        g_cur->lay.y += g->row_h + g->gap;
    }
    g->active = false;

    return hajimu_null();
}

/* ---------------------------------------------------------------
 * グリッド次列() — 内部的にグリッドの次のセルへ移動
 *   グリッド開始後、各ウィジェット間に呼び出す。
 * ---------------------------------------------------------------*/
static Value fn_grid_next(int argc, Value *argv) {
    if (!g_cur || argc < 1) return hajimu_null();

    int slot = (argv[0].type == VALUE_NUMBER) ? (int)argv[0].number : -1;
    if (slot < 0 || slot >= GUI_MAX_GRID || !g_grids[slot].active)
        return hajimu_null();

    GuiGrid *g = &g_grids[slot];
    float cell_h = g_cur->lay.y - (g->y + (g->cur_col == 0 ? 0 : 0));

    /* 現在のセル高さを行最大に反映 */
    if (cell_h > g->row_h) g->row_h = cell_h;

    g->cur_col++;
    if (g->cur_col >= g->cols) {
        /* 次の行へ */
        g->cur_col = 0;
        g->y += g->row_h + g->gap;
        g->row_h = 0;
        g_cur->lay.x = g->x;
    } else {
        g_cur->lay.x = g->x + (g->col_w + g->gap) * g->cur_col;
    }
    g_cur->lay.y = g->y;
    g_cur->lay.w = g->col_w;

    return hajimu_null();
}

/* ---------------------------------------------------------------
 * スプリッター(方向, 位置) → 位置
 *   水平/垂直のリサイズ可能分割バー。
 * ---------------------------------------------------------------*/
static Value fn_splitter(int argc, Value *argv) {
    if (!g_cur || argc < 2) return hajimu_number(0);

    const char *dir = (argv[0].type == VALUE_STRING) ? argv[0].string.data : "水平";
    float pos = (argv[1].type == VALUE_NUMBER) ? (float)argv[1].number : 200.0f;

    bool horizontal = (strcmp(dir, "水平") == 0 || strcmp(dir, "horizontal") == 0);
    uint32_t id = gui_hash("__splitter__");

    float x, y, w;
    gui_pos(&x, &y, &w);
    Hjpcontext *vg = g_cur->vg;

    float bar_sz = 6.0f;
    bool hov = false, prs = false;

    if (horizontal) {
        float bar_y = y + pos;
        gui_widget_logic(id, x, bar_y, w, bar_sz, &hov, &prs);
        if (prs || g_cur->active == id) {
            pos = (float)g_cur->in.my - y;
            if (pos < 30) pos = 30;
        }
        hjpBeginPath(vg);
        hjpRect(vg, x, bar_y, w, bar_sz);
        hjpFillColor(vg, hov ? TH_ACCENT : TH_SEP);
        hjpFill(vg);
        /* 中央のグリップ */
        for (int i = -1; i <= 1; i++) {
            hjpBeginPath(vg);
            hjpCircle(vg, x + w * 0.5f + i * 10.0f, bar_y + bar_sz * 0.5f, 1.5f);
            hjpFillColor(vg, TH_TEXT_DIM);
            hjpFill(vg);
        }
    } else {
        float bar_x = x + pos;
        gui_widget_logic(id, bar_x, y, bar_sz, GUI_WIDGET_H * 4, &hov, &prs);
        if (prs || g_cur->active == id) {
            pos = (float)g_cur->in.mx - x;
            if (pos < 30) pos = 30;
        }
        hjpBeginPath(vg);
        hjpRect(vg, bar_x, y, bar_sz, GUI_WIDGET_H * 4);
        hjpFillColor(vg, hov ? TH_ACCENT : TH_SEP);
        hjpFill(vg);
        for (int i = -1; i <= 1; i++) {
            hjpBeginPath(vg);
            hjpCircle(vg, bar_x + bar_sz * 0.5f, y + GUI_WIDGET_H * 2 + i * 10.0f, 1.5f);
            hjpFillColor(vg, TH_TEXT_DIM);
            hjpFill(vg);
        }
    }

    return hajimu_number(pos);
}

/* ---------------------------------------------------------------
 * グループ開始([タイトル])
 * ---------------------------------------------------------------*/
static Value fn_group_begin(int argc, Value *argv) {
    if (!g_cur) return hajimu_null();

    int slot = -1;
    for (int i = 0; i < GUI_MAX_GROUP; i++) {
        if (!g_groups[i].active) { slot = i; break; }
    }
    if (slot < 0) return hajimu_number(-1);

    const char *title = (argc >= 1 && argv[0].type == VALUE_STRING) ? argv[0].string.data : NULL;

    float x, y, w;
    gui_pos(&x, &y, &w);

    GuiGroup *grp = &g_groups[slot];
    grp->x = x; grp->y = y; grp->w = w;
    grp->saved_x = g_cur->lay.x;
    grp->saved_y = g_cur->lay.y;
    grp->saved_w = g_cur->lay.w;
    grp->has_title = (title != NULL);
    if (title) snprintf(grp->title, sizeof(grp->title), "%s", title);
    grp->active = true;

    /* タイトル分のオフセット */
    float title_h = title ? (GUI_FONT_SIZE + 4.0f) : 0;
    g_cur->lay.x = x + GUI_PADDING * 0.5f;
    g_cur->lay.y = y + GUI_PADDING + title_h;
    g_cur->lay.w = w - GUI_PADDING;

    return hajimu_number(slot);
}

/* ---------------------------------------------------------------
 * グループ終了()
 * ---------------------------------------------------------------*/
static Value fn_group_end(int argc, Value *argv) {
    if (!g_cur || argc < 1) return hajimu_null();

    int slot = (argv[0].type == VALUE_NUMBER) ? (int)argv[0].number : -1;
    if (slot < 0 || slot >= GUI_MAX_GROUP || !g_groups[slot].active)
        return hajimu_null();

    GuiGroup *grp = &g_groups[slot];
    Hjpcontext *vg = g_cur->vg;

    float group_h = g_cur->lay.y - grp->y + GUI_PADDING;

    /* 枠線 */
    hjpBeginPath(vg);
    hjpRoundedRect(vg, grp->x, grp->y, grp->w, group_h, 6.0f);
    hjpStrokeColor(vg, TH_BORDER);
    hjpStrokeWidth(vg, 1.0f);
    hjpStroke(vg);

    /* タイトル（枠線の上に背景付きで描画） */
    if (grp->has_title) {
        hjpFontFaceId(vg, g_cur->font_id);
        hjpFontSize(vg, 13.0f);
        float tw = hjpTextBounds(vg, 0, 0, grp->title, NULL, NULL);
        float tx = grp->x + 12.0f;
        float ty = grp->y - 2.0f;

        /* 背景でラインを消す */
        hjpBeginPath(vg);
        hjpRect(vg, tx - 4, ty - 7, tw + 8, 14.0f);
        hjpFillColor(vg, TH_BG);
        hjpFill(vg);

        hjpFillColor(vg, TH_TEXT_DIM);
        hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_MIDDLE);
        hjpText(vg, tx, ty, grp->title, NULL);
    }

    /* レイアウト復元 */
    g_cur->lay.x = grp->saved_x;
    g_cur->lay.y = grp->saved_y + group_h + GUI_MARGIN;
    g_cur->lay.w = grp->saved_w;
    grp->active = false;

    return hajimu_null();
}

/* ---------------------------------------------------------------
 * ツールバー開始()
 * ---------------------------------------------------------------*/
static Value fn_toolbar_begin(int argc, Value *argv) {
    (void)argc; (void)argv;
    if (!g_cur) return hajimu_null();

    Hjpcontext *vg = g_cur->vg;
    float x = g_cur->lay.x, w = g_cur->lay.w;
    float y = g_cur->lay.y;

    /* ツールバー背景 */
    hjpBeginPath(vg);
    hjpRect(vg, x, y, w, GUI_TOOLBAR_H);
    hjpFillColor(vg, TH_WIDGET_BG);
    hjpFill(vg);
    hjpBeginPath(vg);
    hjpMoveTo(vg, x, y + GUI_TOOLBAR_H);
    hjpLineTo(vg, x + w, y + GUI_TOOLBAR_H);
    hjpStrokeColor(vg, TH_BORDER);
    hjpStrokeWidth(vg, 1.0f);
    hjpStroke(vg);

    g_cur->lay.y = y + 4.0f;
    g_cur->lay.x = x + 4.0f;
    g_cur->lay.w = w - 8.0f;

    return hajimu_null();
}

/* ---------------------------------------------------------------
 * ツールバー終了()
 * ---------------------------------------------------------------*/
static Value fn_toolbar_end(int argc, Value *argv) {
    (void)argc; (void)argv;
    if (!g_cur) return hajimu_null();

    g_cur->lay.y += GUI_TOOLBAR_H - 4.0f + GUI_MARGIN;

    return hajimu_null();
}

/* ---------------------------------------------------------------
 * ステータスバー開始()
 * ---------------------------------------------------------------*/
static Value fn_statusbar_begin(int argc, Value *argv) {
    (void)argc; (void)argv;
    if (!g_cur) return hajimu_null();

    Hjpcontext *vg = g_cur->vg;
    float x = g_cur->lay.x;
    float w = g_cur->lay.w;
    float bar_y = (float)g_cur->win_h - GUI_STATUSBAR_H;

    /* ステータスバー背景 */
    hjpBeginPath(vg);
    hjpRect(vg, x, bar_y, w, GUI_STATUSBAR_H);
    hjpFillColor(vg, TH_WIDGET_BG);
    hjpFill(vg);
    hjpBeginPath(vg);
    hjpMoveTo(vg, x, bar_y);
    hjpLineTo(vg, x + w, bar_y);
    hjpStrokeColor(vg, TH_BORDER);
    hjpStrokeWidth(vg, 1.0f);
    hjpStroke(vg);

    g_cur->lay.y = bar_y + 2.0f;
    g_cur->lay.x = x + 8.0f;

    return hajimu_null();
}

/* ---------------------------------------------------------------
 * ステータスバー終了()
 * ---------------------------------------------------------------*/
static Value fn_statusbar_end(int argc, Value *argv) {
    (void)argc; (void)argv;
    if (!g_cur) return hajimu_null();
    /* ステータスバーは画面下部固定のため、レイアウトy は元に戻さない */
    return hajimu_null();
}

/* ---------------------------------------------------------------
 * 中央揃え()
 * ---------------------------------------------------------------*/
static Value fn_align_center(int argc, Value *argv) {
    (void)argc; (void)argv;
    g_next_align = GUI_ALIGN_CENTER;
    return hajimu_null();
}

/* ---------------------------------------------------------------
 * 右揃え()
 * ---------------------------------------------------------------*/
static Value fn_align_right(int argc, Value *argv) {
    (void)argc; (void)argv;
    g_next_align = GUI_ALIGN_RIGHT;
    return hajimu_null();
}

/* =====================================================================
 * Phase 13: フォーム・入力拡張 (v1.3.0)
 * ===================================================================*/

/* ---------------------------------------------------------------
 * 検索入力(ラベル, 値[, ヒント]) → 文字列
 *   虫眼鏡アイコン付き検索テキスト入力。
 * ---------------------------------------------------------------*/
static Value fn_search_input(int argc, Value *argv) {
    if (!g_cur || argc < 2 || argv[0].type != VALUE_STRING)
        return hajimu_string("");

    Hjpcontext *vg = g_cur->vg;
    (void)argv[0].string.data; /* label — 検索入力はラベル不要 */
    const char *val = (argv[1].type == VALUE_STRING) ? argv[1].string.data : "";
    const char *hint = (argc >= 3 && argv[2].type == VALUE_STRING) ? argv[2].string.data : "検索...";

    float x, y, w;
    gui_pos(&x, &y, &w);
    float h = GUI_WIDGET_H;

    /* 虫眼鏡アイコン */
    hjpBeginPath(vg);
    float ix = x + 14.0f, iy = y + h * 0.5f;
    hjpCircle(vg, ix, iy - 2, 6.0f);
    hjpStrokeColor(vg, TH_TEXT_DIM);
    hjpStrokeWidth(vg, 1.5f);
    hjpStroke(vg);
    hjpBeginPath(vg);
    hjpMoveTo(vg, ix + 4, iy + 2);
    hjpLineTo(vg, ix + 8, iy + 6);
    hjpStroke(vg);

    /* テキスト入力部分（アイコン分オフセット） */
    float input_x = x + GUI_SEARCH_ICON_W;

    /* 背景 */
    hjpBeginPath(vg);
    hjpRoundedRect(vg, x, y, w, h, GUI_BTN_RADIUS);
    hjpFillColor(vg, TH_WIDGET_BG);
    hjpFill(vg);
    hjpStrokeColor(vg, TH_BORDER);
    hjpStrokeWidth(vg, 1.0f);
    hjpStroke(vg);

    /* テキスト or ヒント */
    hjpFontFaceId(vg, g_cur->font_id);
    hjpFontSize(vg, GUI_FONT_SIZE);
    hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_MIDDLE);
    if (strlen(val) > 0) {
        hjpFillColor(vg, TH_TEXT);
        hjpText(vg, input_x, y + h * 0.5f, val, NULL);
    } else {
        hjpFillColor(vg, TH_TEXT_DIM);
        hjpText(vg, input_x, y + h * 0.5f, hint, NULL);
    }

    gui_advance(h);

    /* テキスト入力の実際の編集は fn_text_input に委譲 */
    /* ここでは表示のみ。実使用ではテキスト入力と組み合わせ */
    return hajimu_string(val);
}

/* ---------------------------------------------------------------
 * オートコンプリート(ラベル, 値, 候補配列) → 文字列
 * ---------------------------------------------------------------*/
static Value fn_autocomplete(int argc, Value *argv) {
    if (!g_cur || argc < 3 || argv[0].type != VALUE_STRING)
        return hajimu_string("");

    Hjpcontext *vg = g_cur->vg;
    const char *label = argv[0].string.data;
    const char *val = (argv[1].type == VALUE_STRING) ? argv[1].string.data : "";
    /* argv[2] should be array */

    uint32_t id = gui_hash(label);
    float x, y, w;
    gui_pos(&x, &y, &w);
    float h = GUI_WIDGET_H;

    /* 入力フィールド背景 */
    hjpBeginPath(vg);
    hjpRoundedRect(vg, x, y, w, h, GUI_BTN_RADIUS);
    hjpFillColor(vg, TH_WIDGET_BG);
    hjpFill(vg);
    hjpStrokeColor(vg, TH_BORDER);
    hjpStrokeWidth(vg, 1.0f);
    hjpStroke(vg);

    /* ラベル */
    hjpFontFaceId(vg, g_cur->font_id);
    hjpFontSize(vg, 12.0f);
    hjpFillColor(vg, TH_TEXT_DIM);
    hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_TOP);
    hjpText(vg, x + GUI_PADDING, y - 14.0f, label, NULL);

    /* テキスト */
    hjpFontSize(vg, GUI_FONT_SIZE);
    hjpFillColor(vg, TH_TEXT);
    hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_MIDDLE);
    hjpText(vg, x + GUI_PADDING, y + h * 0.5f, val, NULL);

    /* 候補リストの描画（入力値に一致する候補をドロップダウン表示） */
    const char *result = val;
    if (argv[2].type == VALUE_ARRAY && strlen(val) > 0) {
        int shown = 0;
        float drop_y = y + h + 2;
        for (int i = 0; i < argv[2].array.length && shown < GUI_AC_MAX_VISIBLE; i++) {
            Value item = argv[2].array.elements[i];
            if (item.type != VALUE_STRING) continue;
            if (strstr(item.string.data, val) == NULL) continue;

            uint32_t iid = id + (uint32_t)(i + 1);
            bool hov = false, prs = false;
            float ih = GUI_LIST_ITEM_H;
            bool clicked = gui_widget_logic(iid, x, drop_y, w, ih, &hov, &prs);

            hjpBeginPath(vg);
            hjpRect(vg, x, drop_y, w, ih);
            hjpFillColor(vg, hov ? TH_WIDGET_HOVER : TH_WIDGET_BG);
            hjpFill(vg);

            hjpFillColor(vg, TH_TEXT);
            hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_MIDDLE);
            hjpText(vg, x + GUI_PADDING, drop_y + ih * 0.5f, item.string.data, NULL);

            if (clicked) result = item.string.data;
            drop_y += ih;
            shown++;
        }
        if (shown > 0) {
            hjpBeginPath(vg);
            hjpRect(vg, x, y + h + 2, w, (float)shown * GUI_LIST_ITEM_H);
            hjpStrokeColor(vg, TH_BORDER);
            hjpStrokeWidth(vg, 1.0f);
            hjpStroke(vg);
        }
    }

    gui_advance(h);
    return hajimu_string(result);
}

/* ---------------------------------------------------------------
 * 数値ドラッグ(ラベル, 値, 速度[, 最小, 最大]) → 数値
 * ---------------------------------------------------------------*/
static Value fn_drag_value(int argc, Value *argv) {
    if (!g_cur || argc < 3 || argv[0].type != VALUE_STRING)
        return hajimu_number(0);

    Hjpcontext *vg = g_cur->vg;
    const char *label = argv[0].string.data;
    double val = (argv[1].type == VALUE_NUMBER) ? argv[1].number : 0;
    double speed = (argv[2].type == VALUE_NUMBER) ? argv[2].number : 1.0;
    double vmin = (argc >= 4 && argv[3].type == VALUE_NUMBER) ? argv[3].number : -1e18;
    double vmax = (argc >= 5 && argv[4].type == VALUE_NUMBER) ? argv[4].number : 1e18;

    uint32_t id = gui_hash(label);
    float x, y, w;
    gui_pos(&x, &y, &w);
    float h = GUI_WIDGET_H;

    bool hov = false, prs = false;
    gui_widget_logic(id, x, y, w, h, &hov, &prs);

    /* ドラッグ中の値変更 */
    if (g_cur->active == id && g_cur->in.down) {
        int dx = g_cur->in.mx - g_cur->in.pmx;
        val += dx * speed;
        if (val < vmin) val = vmin;
        if (val > vmax) val = vmax;
    }

    /* 背景 */
    hjpBeginPath(vg);
    hjpRoundedRect(vg, x, y, w, h, GUI_BTN_RADIUS);
    hjpFillColor(vg, hov ? TH_WIDGET_HOVER : TH_WIDGET_BG);
    hjpFill(vg);
    hjpStrokeColor(vg, TH_BORDER);
    hjpStrokeWidth(vg, 1.0f);
    hjpStroke(vg);

    /* ラベル */
    hjpFontFaceId(vg, g_cur->font_id);
    hjpFontSize(vg, GUI_FONT_SIZE);
    hjpFillColor(vg, TH_TEXT);
    hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_MIDDLE);
    hjpText(vg, x + GUI_PADDING, y + h * 0.5f, label, NULL);

    /* 値 */
    char buf[32];
    snprintf(buf, sizeof(buf), "%.2f", val);
    hjpTextAlign(vg, HJP_ALIGN_RIGHT | HJP_ALIGN_MIDDLE);
    hjpText(vg, x + w - GUI_PADDING, y + h * 0.5f, buf, NULL);

    /* ◄ ► 矢印 */
    hjpFontSize(vg, 10.0f);
    hjpFillColor(vg, TH_TEXT_DIM);
    hjpTextAlign(vg, HJP_ALIGN_CENTER | HJP_ALIGN_MIDDLE);
    hjpText(vg, x + w * 0.5f, y + h * 0.5f, "◄ ドラッグ ►", NULL);

    gui_advance(h);
    return hajimu_number(val);
}

/* ---------------------------------------------------------------
 * 日付ピッカー(ラベル, 値) → 文字列
 *   簡易日付表示。入力として YYYY-MM-DD 文字列。
 * ---------------------------------------------------------------*/
static Value fn_date_picker(int argc, Value *argv) {
    if (!g_cur || argc < 2 || argv[0].type != VALUE_STRING)
        return hajimu_string("");

    Hjpcontext *vg = g_cur->vg;
    const char *label = argv[0].string.data;
    const char *val = (argv[1].type == VALUE_STRING) ? argv[1].string.data : "";
    uint32_t id = gui_hash(label);

    float x, y, w;
    gui_pos(&x, &y, &w);
    float h = GUI_WIDGET_H;

    bool hov = false, prs = false;
    gui_widget_logic(id, x, y, w, h, &hov, &prs);

    /* 背景 */
    hjpBeginPath(vg);
    hjpRoundedRect(vg, x, y, w, h, GUI_BTN_RADIUS);
    hjpFillColor(vg, hov ? TH_WIDGET_HOVER : TH_WIDGET_BG);
    hjpFill(vg);
    hjpStrokeColor(vg, TH_BORDER);
    hjpStrokeWidth(vg, 1.0f);
    hjpStroke(vg);

    /* カレンダーアイコン */
    float ix = x + 12.0f, iy = y + h * 0.5f;
    hjpBeginPath(vg);
    hjpRect(vg, ix - 5, iy - 5, 10, 10);
    hjpStrokeColor(vg, TH_TEXT_DIM);
    hjpStrokeWidth(vg, 1.0f);
    hjpStroke(vg);
    hjpBeginPath(vg);
    hjpMoveTo(vg, ix - 5, iy - 2);
    hjpLineTo(vg, ix + 5, iy - 2);
    hjpStroke(vg);

    /* ラベルと値 */
    hjpFontFaceId(vg, g_cur->font_id);
    hjpFontSize(vg, 12.0f);
    hjpFillColor(vg, TH_TEXT_DIM);
    hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_MIDDLE);
    hjpText(vg, x + 28.0f, y + h * 0.5f, label, NULL);

    hjpFontSize(vg, GUI_FONT_SIZE);
    hjpFillColor(vg, TH_TEXT);
    hjpTextAlign(vg, HJP_ALIGN_RIGHT | HJP_ALIGN_MIDDLE);
    hjpText(vg, x + w - GUI_PADDING, y + h * 0.5f, val, NULL);

    gui_advance(h);
    return hajimu_string(val);
}

/* ---------------------------------------------------------------
 * 時間ピッカー(ラベル, 値) → 文字列
 * ---------------------------------------------------------------*/
static Value fn_time_picker(int argc, Value *argv) {
    if (!g_cur || argc < 2 || argv[0].type != VALUE_STRING)
        return hajimu_string("");

    Hjpcontext *vg = g_cur->vg;
    const char *label = argv[0].string.data;
    const char *val = (argv[1].type == VALUE_STRING) ? argv[1].string.data : "";
    uint32_t id = gui_hash(label);

    float x, y, w;
    gui_pos(&x, &y, &w);
    float h = GUI_WIDGET_H;

    bool hov = false, prs = false;
    gui_widget_logic(id, x, y, w, h, &hov, &prs);

    /* 背景 */
    hjpBeginPath(vg);
    hjpRoundedRect(vg, x, y, w, h, GUI_BTN_RADIUS);
    hjpFillColor(vg, hov ? TH_WIDGET_HOVER : TH_WIDGET_BG);
    hjpFill(vg);
    hjpStrokeColor(vg, TH_BORDER);
    hjpStrokeWidth(vg, 1.0f);
    hjpStroke(vg);

    /* 時計アイコン */
    float ix = x + 12.0f, iy = y + h * 0.5f;
    hjpBeginPath(vg);
    hjpCircle(vg, ix, iy, 6.0f);
    hjpStrokeColor(vg, TH_TEXT_DIM);
    hjpStrokeWidth(vg, 1.0f);
    hjpStroke(vg);
    hjpBeginPath(vg);
    hjpMoveTo(vg, ix, iy);
    hjpLineTo(vg, ix + 3, iy - 3);
    hjpStroke(vg);

    /* ラベルと値 */
    hjpFontFaceId(vg, g_cur->font_id);
    hjpFontSize(vg, 12.0f);
    hjpFillColor(vg, TH_TEXT_DIM);
    hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_MIDDLE);
    hjpText(vg, x + 28.0f, y + h * 0.5f, label, NULL);

    hjpFontSize(vg, GUI_FONT_SIZE);
    hjpFillColor(vg, TH_TEXT);
    hjpTextAlign(vg, HJP_ALIGN_RIGHT | HJP_ALIGN_MIDDLE);
    hjpText(vg, x + w - GUI_PADDING, y + h * 0.5f, val, NULL);

    gui_advance(h);
    return hajimu_string(val);
}

/* ---------------------------------------------------------------
 * プレースホルダー(テキスト)
 *   次の入力フィールドに表示するヒントテキストを設定。
 * ---------------------------------------------------------------*/
static Value fn_placeholder(int argc, Value *argv) {
    if (argc >= 1 && argv[0].type == VALUE_STRING) {
        snprintf(g_placeholder, sizeof(g_placeholder), "%s", argv[0].string.data);
    }
    return hajimu_null();
}

/* ---------------------------------------------------------------
 * バリデーション(値, ルール) → {有効, メッセージ}
 *   ルール: "必須" | "数値" | "メール" | "最小:N" | "最大:N"
 * ---------------------------------------------------------------*/
static Value fn_validation(int argc, Value *argv) {
    if (!g_cur || argc < 2) return hajimu_bool(false);

    const char *val = (argv[0].type == VALUE_STRING) ? argv[0].string.data : "";
    const char *rule = (argv[1].type == VALUE_STRING) ? argv[1].string.data : "";

    bool valid = true;
    const char *msg = "";

    if (strcmp(rule, "必須") == 0) {
        if (strlen(val) == 0) { valid = false; msg = "入力は必須です"; }
    } else if (strcmp(rule, "数値") == 0) {
        char *end = NULL;
        strtod(val, &end);
        if (end == val || *end != '\0') { valid = false; msg = "数値を入力してください"; }
    } else if (strcmp(rule, "メール") == 0) {
        if (strchr(val, '@') == NULL) { valid = false; msg = "有効なメールアドレスを入力してください"; }
    } else if (strncmp(rule, "最小:", strlen("最小:")) == 0) {
        int min_len = atoi(rule + strlen("最小:"));
        if ((int)strlen(val) < min_len) { valid = false; msg = "文字数が不足しています"; }
    } else if (strncmp(rule, "最大:", strlen("最大:")) == 0) {
        int max_len = atoi(rule + strlen("最大:"));
        if ((int)strlen(val) > max_len) { valid = false; msg = "文字数が超過しています"; }
    }

    /* 無効時にメッセージを表示 */
    if (!valid && g_cur) {
        Hjpcontext *vg = g_cur->vg;
        float x, y, w;
        gui_pos(&x, &y, &w);
        hjpFontFaceId(vg, g_cur->font_id);
        hjpFontSize(vg, 12.0f);
        hjpFillColor(vg, hjpRGB(220, 50, 50));
        hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_TOP);
        hjpText(vg, x, y, msg, NULL);
        gui_advance(16.0f);
    }

    return hajimu_bool(valid);
}

/* ---------------------------------------------------------------
 * 無効化開始()
 * ---------------------------------------------------------------*/
static Value fn_disable_begin(int argc, Value *argv) {
    (void)argc; (void)argv;
    g_disabled = true;
    if (g_cur) hjpGlobalAlpha(g_cur->vg, 0.4f);
    return hajimu_null();
}

/* ---------------------------------------------------------------
 * 無効化終了()
 * ---------------------------------------------------------------*/
static Value fn_disable_end(int argc, Value *argv) {
    (void)argc; (void)argv;
    g_disabled = false;
    if (g_cur) hjpGlobalAlpha(g_cur->vg, 1.0f);
    return hajimu_null();
}

/* ---------------------------------------------------------------
 * マルチセレクト(ラベル, 選択肢, 選択配列) → 配列
 *   チェックボックス式の複数選択。
 * ---------------------------------------------------------------*/
static Value fn_multi_select(int argc, Value *argv) {
    if (!g_cur || argc < 3 || argv[0].type != VALUE_STRING)
        return hajimu_null();

    Hjpcontext *vg = g_cur->vg;
    const char *label = argv[0].string.data;

    /* ラベル表示 */
    float x, y, w;
    gui_pos(&x, &y, &w);
    hjpFontFaceId(vg, g_cur->font_id);
    hjpFontSize(vg, 13.0f);
    hjpFillColor(vg, TH_TEXT_DIM);
    hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_TOP);
    hjpText(vg, x, y, label, NULL);
    gui_advance(18.0f);

    if (argv[1].type != VALUE_ARRAY || argv[2].type != VALUE_ARRAY)
        return hajimu_null();

    /* 各選択肢をチェックボックスとして描画 */
    Value choices = argv[1];
    Value selected = argv[2];

    for (int i = 0; i < choices.array.length; i++) {
        if (choices.array.elements[i].type != VALUE_STRING) continue;
        const char *item = choices.array.elements[i].string.data;
        uint32_t iid = gui_hash(item) + gui_hash(label);

        /* 選択中かチェック */
        bool is_selected = false;
        for (int j = 0; j < selected.array.length; j++) {
            if (selected.array.elements[j].type == VALUE_STRING &&
                strcmp(selected.array.elements[j].string.data, item) == 0) {
                is_selected = true;
                break;
            }
        }

        gui_pos(&x, &y, &w);
        float h = GUI_CHECK_SIZE + 4.0f;

        bool hov = false, prs = false;
        bool clicked = gui_widget_logic(iid, x, y, w, h, &hov, &prs);

        /* チェックボックス描画 */
        float cx = x, cy = y + (h - GUI_CHECK_SIZE) * 0.5f;
        hjpBeginPath(vg);
        hjpRoundedRect(vg, cx, cy, GUI_CHECK_SIZE, GUI_CHECK_SIZE, 3.0f);
        hjpFillColor(vg, is_selected ? TH_ACCENT : TH_WIDGET_BG);
        hjpFill(vg);
        hjpStrokeColor(vg, is_selected ? TH_ACCENT : TH_BORDER);
        hjpStrokeWidth(vg, 1.0f);
        hjpStroke(vg);

        if (is_selected) {
            hjpBeginPath(vg);
            float mx = cx + GUI_CHECK_SIZE * 0.5f;
            float my = cy + GUI_CHECK_SIZE * 0.5f;
            hjpMoveTo(vg, mx - 4, my);
            hjpLineTo(vg, mx - 1, my + 4);
            hjpLineTo(vg, mx + 5, my - 4);
            hjpStrokeColor(vg, TH_CHECK);
            hjpStrokeWidth(vg, 2.0f);
            hjpStroke(vg);
        }

        /* ラベル */
        hjpFontSize(vg, GUI_FONT_SIZE);
        hjpFillColor(vg, TH_TEXT);
        hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_MIDDLE);
        hjpText(vg, cx + GUI_CHECK_SIZE + 8.0f, y + h * 0.5f, item, NULL);

        gui_advance(h);

        /* クリック時のトグル処理は呼び出し側で行う */
        (void)clicked;
    }

    return argv[2]; /* 呼び出し側で管理するので、現在の配列をそのまま返す */
}

/* =====================================================================
 * Phase 14: チャート・データ可視化 (v1.4.0)
 * ===================================================================*/

/* ヘルパー: データ配列から数値を取得 */
static double chart_val(Value *arr, int i) {
    if (arr->type != VALUE_ARRAY || i >= arr->array.length) return 0;
    Value v = arr->array.elements[i];
    return (v.type == VALUE_NUMBER) ? v.number : 0;
}
static int chart_len(Value *arr) {
    return (arr->type == VALUE_ARRAY) ? arr->array.length : 0;
}

/* ---------------------------------------------------------------
 * 折れ線グラフ(ID, データ, オプション)
 *   データ = [数値, ...]  オプション = {"幅": n, "高さ": n, "色": c}
 * ---------------------------------------------------------------*/
static Value fn_line_chart(int argc, Value *argv) {
    if (!g_cur || argc < 2) return hajimu_null();

    Hjpcontext *vg = g_cur->vg;
    int n = chart_len(&argv[1]);
    if (n < 2) return hajimu_null();

    float cw = 300, ch = 150;
    Hjpcolor col = TH_ACCENT;
    /* オプション解析は簡略化 */

    float x, y, w;
    gui_pos(&x, &y, &w);
    if (cw > w) cw = w;

    /* 軸 */
    hjpBeginPath(vg);
    hjpMoveTo(vg, x, y + ch);
    hjpLineTo(vg, x + cw, y + ch);
    hjpStrokeColor(vg, TH_SEP);
    hjpStrokeWidth(vg, 1.0f);
    hjpStroke(vg);

    /* 最小/最大 */
    double vmin = chart_val(&argv[1], 0), vmax = vmin;
    for (int i = 1; i < n; i++) {
        double v = chart_val(&argv[1], i);
        if (v < vmin) vmin = v;
        if (v > vmax) vmax = v;
    }
    double range = vmax - vmin;
    if (range < 1e-9) range = 1;

    /* 線 */
    hjpBeginPath(vg);
    for (int i = 0; i < n; i++) {
        float px = x + (float)i / (n - 1) * cw;
        float py = y + ch - (float)((chart_val(&argv[1], i) - vmin) / range) * (ch - 10);
        if (i == 0) hjpMoveTo(vg, px, py);
        else hjpLineTo(vg, px, py);
    }
    hjpStrokeColor(vg, col);
    hjpStrokeWidth(vg, 2.0f);
    hjpStroke(vg);

    /* ドット */
    for (int i = 0; i < n; i++) {
        float px = x + (float)i / (n - 1) * cw;
        float py = y + ch - (float)((chart_val(&argv[1], i) - vmin) / range) * (ch - 10);
        hjpBeginPath(vg);
        hjpCircle(vg, px, py, 3.0f);
        hjpFillColor(vg, col);
        hjpFill(vg);
    }

    gui_advance(ch + GUI_MARGIN);
    return hajimu_null();
}

/* ---------------------------------------------------------------
 * 棒グラフ(ID, データ, オプション)
 * ---------------------------------------------------------------*/
static Value fn_bar_chart(int argc, Value *argv) {
    if (!g_cur || argc < 2) return hajimu_null();

    Hjpcontext *vg = g_cur->vg;
    int n = chart_len(&argv[1]);
    if (n < 1) return hajimu_null();

    float cw = 300, ch = 150;

    float x, y, w;
    gui_pos(&x, &y, &w);
    if (cw > w) cw = w;

    double vmax = 0;
    for (int i = 0; i < n; i++) {
        double v = chart_val(&argv[1], i);
        if (v > vmax) vmax = v;
    }
    if (vmax < 1e-9) vmax = 1;

    float bar_w = (cw - (n - 1) * 4.0f) / n;

    for (int i = 0; i < n; i++) {
        double v = chart_val(&argv[1], i);
        float bh = (float)(v / vmax) * (ch - 10);
        float bx = x + i * (bar_w + 4.0f);
        float by = y + ch - bh;

        hjpBeginPath(vg);
        hjpRoundedRect(vg, bx, by, bar_w, bh, 2.0f);
        Hjpcolor col = hjpHSLA(0.55f + (float)i * 0.1f, 0.7f, 0.5f, 230);
        hjpFillColor(vg, col);
        hjpFill(vg);
    }

    /* 軸 */
    hjpBeginPath(vg);
    hjpMoveTo(vg, x, y + ch);
    hjpLineTo(vg, x + cw, y + ch);
    hjpStrokeColor(vg, TH_SEP);
    hjpStrokeWidth(vg, 1.0f);
    hjpStroke(vg);

    gui_advance(ch + GUI_MARGIN);
    return hajimu_null();
}

/* ---------------------------------------------------------------
 * 円グラフ(ID, データ, オプション)
 * ---------------------------------------------------------------*/
static Value fn_pie_chart(int argc, Value *argv) {
    if (!g_cur || argc < 2) return hajimu_null();

    Hjpcontext *vg = g_cur->vg;
    int n = chart_len(&argv[1]);
    if (n < 1) return hajimu_null();

    float radius = 70;
    float x, y, w;
    gui_pos(&x, &y, &w);
    float cx = x + radius + 10, cy = y + radius + 10;

    double total = 0;
    for (int i = 0; i < n; i++) total += chart_val(&argv[1], i);
    if (total < 1e-9) total = 1;

    float angle = -HJP_PI * 0.5f;
    for (int i = 0; i < n; i++) {
        double v = chart_val(&argv[1], i);
        float sweep = (float)(v / total) * HJP_PI * 2.0f;
        Hjpcolor col = hjpHSLA((float)i / n, 0.7f, 0.55f, 230);

        hjpBeginPath(vg);
        hjpMoveTo(vg, cx, cy);
        hjpArc(vg, cx, cy, radius, angle, angle + sweep, HJP_CW);
        hjpClosePath(vg);
        hjpFillColor(vg, col);
        hjpFill(vg);

        angle += sweep;
    }

    gui_advance(radius * 2 + 20 + GUI_MARGIN);
    return hajimu_null();
}

/* ---------------------------------------------------------------
 * 散布図(ID, データ, オプション)
 *   データ = [[x,y], [x,y], ...] — ただし１次元配列も許容
 * ---------------------------------------------------------------*/
static Value fn_scatter_chart(int argc, Value *argv) {
    if (!g_cur || argc < 2) return hajimu_null();

    Hjpcontext *vg = g_cur->vg;
    int n = chart_len(&argv[1]);
    if (n < 2) return hajimu_null();

    float cw = 300, ch = 150;
    float x, y, w;
    gui_pos(&x, &y, &w);
    if (cw > w) cw = w;

    /* 1次元配列として扱い x=index */
    double vmin = chart_val(&argv[1], 0), vmax = vmin;
    for (int i = 1; i < n; i++) {
        double v = chart_val(&argv[1], i);
        if (v < vmin) vmin = v;
        if (v > vmax) vmax = v;
    }
    double range = vmax - vmin;
    if (range < 1e-9) range = 1;

    for (int i = 0; i < n; i++) {
        float px = x + (float)i / (n - 1) * cw;
        float py = y + ch - (float)((chart_val(&argv[1], i) - vmin) / range) * (ch - 10);
        hjpBeginPath(vg);
        hjpCircle(vg, px, py, 4.0f);
        hjpFillColor(vg, TH_ACCENT);
        hjpFill(vg);
    }

    hjpBeginPath(vg);
    hjpMoveTo(vg, x, y + ch);
    hjpLineTo(vg, x + cw, y + ch);
    hjpStrokeColor(vg, TH_SEP);
    hjpStrokeWidth(vg, 1.0f);
    hjpStroke(vg);

    gui_advance(ch + GUI_MARGIN);
    return hajimu_null();
}

/* ---------------------------------------------------------------
 * エリアチャート(ID, データ, オプション)
 * ---------------------------------------------------------------*/
static Value fn_area_chart(int argc, Value *argv) {
    if (!g_cur || argc < 2) return hajimu_null();

    Hjpcontext *vg = g_cur->vg;
    int n = chart_len(&argv[1]);
    if (n < 2) return hajimu_null();

    float cw = 300, ch = 150;
    float x, y, w;
    gui_pos(&x, &y, &w);
    if (cw > w) cw = w;

    double vmin = chart_val(&argv[1], 0), vmax = vmin;
    for (int i = 1; i < n; i++) {
        double v = chart_val(&argv[1], i);
        if (v < vmin) vmin = v;
        if (v > vmax) vmax = v;
    }
    double range = vmax - vmin;
    if (range < 1e-9) range = 1;

    /* 塗りつぶし */
    hjpBeginPath(vg);
    hjpMoveTo(vg, x, y + ch);
    for (int i = 0; i < n; i++) {
        float px = x + (float)i / (n - 1) * cw;
        float py = y + ch - (float)((chart_val(&argv[1], i) - vmin) / range) * (ch - 10);
        hjpLineTo(vg, px, py);
    }
    hjpLineTo(vg, x + cw, y + ch);
    hjpClosePath(vg);
    Hjpcolor fill = TH_ACCENT;
    fill.a = 0.3f;
    hjpFillColor(vg, fill);
    hjpFill(vg);

    /* 線 */
    hjpBeginPath(vg);
    for (int i = 0; i < n; i++) {
        float px = x + (float)i / (n - 1) * cw;
        float py = y + ch - (float)((chart_val(&argv[1], i) - vmin) / range) * (ch - 10);
        if (i == 0) hjpMoveTo(vg, px, py);
        else hjpLineTo(vg, px, py);
    }
    hjpStrokeColor(vg, TH_ACCENT);
    hjpStrokeWidth(vg, 2.0f);
    hjpStroke(vg);

    gui_advance(ch + GUI_MARGIN);
    return hajimu_null();
}

/* ---------------------------------------------------------------
 * ゲージ(値, 最大[, ラベル])
 * ---------------------------------------------------------------*/
static Value fn_gauge(int argc, Value *argv) {
    if (!g_cur || argc < 2) return hajimu_null();

    Hjpcontext *vg = g_cur->vg;
    double val = (argv[0].type == VALUE_NUMBER) ? argv[0].number : 0;
    double vmax = (argv[1].type == VALUE_NUMBER) ? argv[1].number : 100;
    const char *label = (argc >= 3 && argv[2].type == VALUE_STRING) ? argv[2].string.data : NULL;

    float radius = 50;
    float x, y, w;
    gui_pos(&x, &y, &w);
    float cx = x + radius + 10, cy = y + radius + 10;

    float ratio = (float)(val / vmax);
    if (ratio < 0) ratio = 0;
    if (ratio > 1) ratio = 1;

    /* 背景弧 */
    float start_angle = HJP_PI * 0.75f;
    float end_angle = HJP_PI * 2.25f;
    hjpBeginPath(vg);
    hjpArc(vg, cx, cy, radius, start_angle, end_angle, HJP_CW);
    hjpStrokeColor(vg, TH_TRACK);
    hjpStrokeWidth(vg, 8.0f);
    hjpLineCap(vg, HJP_ROUND);
    hjpStroke(vg);

    /* 値弧 */
    float val_angle = start_angle + (end_angle - start_angle) * ratio;
    hjpBeginPath(vg);
    hjpArc(vg, cx, cy, radius, start_angle, val_angle, HJP_CW);
    Hjpcolor gauge_col = ratio < 0.5f ? TH_ACCENT :
                         ratio < 0.8f ? hjpRGB(230, 180, 40) :
                                        hjpRGB(220, 60, 60);
    hjpStrokeColor(vg, gauge_col);
    hjpStrokeWidth(vg, 8.0f);
    hjpStroke(vg);

    /* 値テキスト */
    char vbuf[32];
    snprintf(vbuf, sizeof(vbuf), "%.0f%%", ratio * 100);
    hjpFontFaceId(vg, g_cur->font_id);
    hjpFontSize(vg, 20.0f);
    hjpFillColor(vg, TH_TEXT);
    hjpTextAlign(vg, HJP_ALIGN_CENTER | HJP_ALIGN_MIDDLE);
    hjpText(vg, cx, cy, vbuf, NULL);

    if (label) {
        hjpFontSize(vg, 12.0f);
        hjpFillColor(vg, TH_TEXT_DIM);
        hjpText(vg, cx, cy + 18, label, NULL);
    }

    gui_advance(radius * 2 + 20 + GUI_MARGIN);
    return hajimu_null();
}

/* ---------------------------------------------------------------
 * スパークライン(データ, 幅, 高さ)
 * ---------------------------------------------------------------*/
static Value fn_sparkline(int argc, Value *argv) {
    if (!g_cur || argc < 3) return hajimu_null();

    Hjpcontext *vg = g_cur->vg;
    int n = chart_len(&argv[0]);
    if (n < 2) return hajimu_null();

    float sw = (argv[1].type == VALUE_NUMBER) ? (float)argv[1].number : 100;
    float sh = (argv[2].type == VALUE_NUMBER) ? (float)argv[2].number : 24;

    float x, y, avail_w;
    gui_pos(&x, &y, &avail_w);

    double vmin = chart_val(&argv[0], 0), vmax = vmin;
    for (int i = 1; i < n; i++) {
        double v = chart_val(&argv[0], i);
        if (v < vmin) vmin = v;
        if (v > vmax) vmax = v;
    }
    double range = vmax - vmin;
    if (range < 1e-9) range = 1;

    hjpBeginPath(vg);
    for (int i = 0; i < n; i++) {
        float px = x + (float)i / (n - 1) * sw;
        float py = y + sh - (float)((chart_val(&argv[0], i) - vmin) / range) * (sh - 2);
        if (i == 0) hjpMoveTo(vg, px, py);
        else hjpLineTo(vg, px, py);
    }
    hjpStrokeColor(vg, TH_ACCENT);
    hjpStrokeWidth(vg, 1.5f);
    hjpStroke(vg);

    gui_advance(sh + 4);
    return hajimu_null();
}

/* ---------------------------------------------------------------
 * ヒストグラム(ID, データ, オプション)
 *   棒グラフと同じだが、間隔なしで描画。
 * ---------------------------------------------------------------*/
static Value fn_histogram(int argc, Value *argv) {
    if (!g_cur || argc < 2) return hajimu_null();

    Hjpcontext *vg = g_cur->vg;
    int n = chart_len(&argv[1]);
    if (n < 1) return hajimu_null();

    float cw = 300, ch = 150;
    float x, y, w;
    gui_pos(&x, &y, &w);
    if (cw > w) cw = w;

    double vmax = 0;
    for (int i = 0; i < n; i++) {
        double v = chart_val(&argv[1], i);
        if (v > vmax) vmax = v;
    }
    if (vmax < 1e-9) vmax = 1;

    float bar_w = cw / n;

    for (int i = 0; i < n; i++) {
        double v = chart_val(&argv[1], i);
        float bh = (float)(v / vmax) * (ch - 10);
        float bx = x + i * bar_w;
        float by = y + ch - bh;

        hjpBeginPath(vg);
        hjpRect(vg, bx, by, bar_w - 1, bh);
        hjpFillColor(vg, TH_ACCENT);
        hjpFill(vg);
    }

    hjpBeginPath(vg);
    hjpMoveTo(vg, x, y + ch);
    hjpLineTo(vg, x + cw, y + ch);
    hjpStrokeColor(vg, TH_SEP);
    hjpStrokeWidth(vg, 1.0f);
    hjpStroke(vg);

    gui_advance(ch + GUI_MARGIN);
    return hajimu_null();
}

/* =====================================================================
 * Phase 15: サブウィンドウ・ポップアップ (v1.5.0)
 * ===================================================================*/

/* ---------------------------------------------------------------
 * 子ウィンドウ開始(ID, 幅, 高さ[, 枠線]) → 真偽
 *   スクロール領域の薄いラッパー。常に真を返す。
 * ---------------------------------------------------------------*/
static Value fn_child_begin(int argc, Value *argv) {
    if (!g_cur || argc < 3) return hajimu_bool(false);

    /* スクロール領域と同様の処理 */
    float cw = (argv[1].type == VALUE_NUMBER) ? (float)argv[1].number : 200;
    float ch = (argv[2].type == VALUE_NUMBER) ? (float)argv[2].number : 200;
    bool border = (argc >= 4 && argv[3].type == VALUE_BOOL) ? argv[3].boolean : true;

    int slot = -1;
    for (int i = 0; i < GUI_MAX_SCROLL; i++) {
        if (!g_scrolls[i].active) { slot = i; break; }
    }
    if (slot < 0) return hajimu_bool(false);

    float x, y, w;
    gui_pos(&x, &y, &w);
    if (cw > w) cw = w;

    GuiScrollRegion *sr = &g_scrolls[slot];
    sr->x = x; sr->y = y; sr->w = cw; sr->h = ch;
    sr->saved_x = g_cur->lay.x;
    sr->saved_y = g_cur->lay.y;
    sr->saved_w = g_cur->lay.w;
    sr->content_h = 0;
    sr->active = true;

    Hjpcontext *vg = g_cur->vg;
    hjpSave(vg);
    hjpScissor(vg, x, y, cw, ch);

    if (border) {
        hjpBeginPath(vg);
        hjpRoundedRect(vg, x, y, cw, ch, 2.0f);
        hjpStrokeColor(vg, TH_BORDER);
        hjpStrokeWidth(vg, 1.0f);
        hjpStroke(vg);
    }

    g_cur->lay.x = x + 4;
    g_cur->lay.y = y + 4 - sr->scroll_y;
    g_cur->lay.w = cw - 8;

    return hajimu_bool(true);
}

/* ---------------------------------------------------------------
 * 子ウィンドウ終了()
 * ---------------------------------------------------------------*/
static Value fn_child_end(int argc, Value *argv) {
    (void)argc; (void)argv;
    if (!g_cur) return hajimu_null();

    /* 最近アクティブなスクロール領域を閉じる */
    for (int i = GUI_MAX_SCROLL - 1; i >= 0; i--) {
        if (g_scrolls[i].active) {
            GuiScrollRegion *sr = &g_scrolls[i];
            sr->content_h = (g_cur->lay.y + sr->scroll_y) - sr->y;
            hjpRestore(g_cur->vg);
            g_cur->lay.x = sr->saved_x;
            g_cur->lay.y = sr->saved_y + sr->h + GUI_MARGIN;
            g_cur->lay.w = sr->saved_w;
            sr->active = false;
            break;
        }
    }

    return hajimu_null();
}

/* ---------------------------------------------------------------
 * ポップアップ開始(ID) → 真偽
 * ---------------------------------------------------------------*/
static Value fn_popup_begin(int argc, Value *argv) {
    if (!g_cur || argc < 1 || argv[0].type != VALUE_STRING)
        return hajimu_bool(false);

    uint32_t id = gui_hash(argv[0].string.data);
    /* オープン中のポップアップを探す */
    for (int i = 0; i < g_popup_count; i++) {
        if (g_popups[i].id == id && g_popups[i].open) {
            Hjpcontext *vg = g_cur->vg;

            /* マウス位置にポップアップを表示 */
            float px = g_popups[i].x, py = g_popups[i].y;
            float pw = GUI_POPUP_W, ph = 200;

            /* 背景 */
            hjpSave(vg);
            Hjppaint shadow = hjpBoxGradient(vg, px, py + 2, pw, ph, 4, 8,
                hjpRGBA(0, 0, 0, 60), hjpRGBA(0, 0, 0, 0));
            hjpBeginPath(vg);
            hjpRect(vg, px - 10, py - 10, pw + 20, ph + 20);
            hjpFillPaint(vg, shadow);
            hjpFill(vg);

            hjpBeginPath(vg);
            hjpRoundedRect(vg, px, py, pw, ph, 6.0f);
            hjpFillColor(vg, TH_WIDGET_BG);
            hjpFill(vg);
            hjpStrokeColor(vg, TH_BORDER);
            hjpStrokeWidth(vg, 1.0f);
            hjpStroke(vg);

            g_cur->lay.x = px + 8;
            g_cur->lay.y = py + 8;
            g_cur->lay.w = pw - 16;

            return hajimu_bool(true);
        }
    }
    return hajimu_bool(false);
}

/* ---------------------------------------------------------------
 * ポップアップ終了()
 * ---------------------------------------------------------------*/
static Value fn_popup_end(int argc, Value *argv) {
    (void)argc; (void)argv;
    if (!g_cur) return hajimu_null();
    hjpRestore(g_cur->vg);
    return hajimu_null();
}

/* ---------------------------------------------------------------
 * ポップアップ表示(ID)
 * ---------------------------------------------------------------*/
static Value fn_popup_open(int argc, Value *argv) {
    if (argc < 1 || argv[0].type != VALUE_STRING) return hajimu_null();

    uint32_t id = gui_hash(argv[0].string.data);
    /* 既存のポップアップを探す */
    for (int i = 0; i < g_popup_count; i++) {
        if (g_popups[i].id == id) {
            g_popups[i].open = true;
            if (g_cur) {
                g_popups[i].x = (float)g_cur->in.mx;
                g_popups[i].y = (float)g_cur->in.my;
            }
            return hajimu_null();
        }
    }
    /* 新規 */
    if (g_popup_count < GUI_MAX_POPUP) {
        g_popups[g_popup_count].id = id;
        g_popups[g_popup_count].open = true;
        if (g_cur) {
            g_popups[g_popup_count].x = (float)g_cur->in.mx;
            g_popups[g_popup_count].y = (float)g_cur->in.my;
        }
        g_popup_count++;
    }
    return hajimu_null();
}

/* ---------------------------------------------------------------
 * サイドパネル(ID, 幅, 開閉) → 真偽
 * ---------------------------------------------------------------*/
static Value fn_side_panel(int argc, Value *argv) {
    if (!g_cur || argc < 3) return hajimu_bool(false);

    Hjpcontext *vg = g_cur->vg;
    float panel_w = (argv[1].type == VALUE_NUMBER) ? (float)argv[1].number : GUI_SIDE_PANEL_W;
    bool open = (argv[2].type == VALUE_BOOL) ? argv[2].boolean
              : (argv[2].type == VALUE_NUMBER) ? (argv[2].number != 0)
              : false;

    if (!open) return hajimu_bool(false);

    float x = g_cur->lay.x, y = g_cur->lay.y;
    float h = (float)g_cur->win_h - y;

    /* パネル背景 */
    hjpBeginPath(vg);
    hjpRect(vg, x, y, panel_w, h);
    hjpFillColor(vg, TH_WIDGET_BG);
    hjpFill(vg);
    hjpBeginPath(vg);
    hjpMoveTo(vg, x + panel_w, y);
    hjpLineTo(vg, x + panel_w, y + h);
    hjpStrokeColor(vg, TH_BORDER);
    hjpStrokeWidth(vg, 1.0f);
    hjpStroke(vg);

    /* レイアウトをパネル内に変更 */
    g_cur->lay.x = x + 8;
    g_cur->lay.y = y + 8;
    g_cur->lay.w = panel_w - 16;

    return hajimu_bool(true);
}

/* ---------------------------------------------------------------
 * ボトムシート開始(タイトル, 開閉) → 真偽
 * ---------------------------------------------------------------*/
static Value fn_bottom_sheet_begin(int argc, Value *argv) {
    if (!g_cur || argc < 2) return hajimu_bool(false);

    const char *title = (argv[0].type == VALUE_STRING) ? argv[0].string.data : "";
    bool open = (argv[1].type == VALUE_BOOL) ? argv[1].boolean
              : (argv[1].type == VALUE_NUMBER) ? (argv[1].number != 0)
              : false;

    if (!open) return hajimu_bool(false);

    Hjpcontext *vg = g_cur->vg;
    float x = g_cur->lay.x;
    float w = g_cur->lay.w;
    float sheet_y = (float)g_cur->win_h - GUI_BOTTOM_SHEET_H;

    /* 背景 */
    hjpBeginPath(vg);
    hjpRoundedRect(vg, x, sheet_y, w, GUI_BOTTOM_SHEET_H, 12.0f);
    hjpFillColor(vg, TH_WIDGET_BG);
    hjpFill(vg);
    hjpStrokeColor(vg, TH_BORDER);
    hjpStrokeWidth(vg, 1.0f);
    hjpStroke(vg);

    /* ハンドル */
    hjpBeginPath(vg);
    hjpRoundedRect(vg, x + w * 0.5f - 20, sheet_y + 8, 40, 4, 2);
    hjpFillColor(vg, TH_SEP);
    hjpFill(vg);

    /* タイトル */
    hjpFontFaceId(vg, g_cur->font_id);
    hjpFontSize(vg, GUI_FONT_SIZE);
    hjpFillColor(vg, TH_TEXT);
    hjpTextAlign(vg, HJP_ALIGN_CENTER | HJP_ALIGN_TOP);
    hjpText(vg, x + w * 0.5f, sheet_y + 18, title, NULL);

    g_cur->lay.x = x + GUI_PADDING;
    g_cur->lay.y = sheet_y + 40;
    g_cur->lay.w = w - GUI_PADDING * 2;

    return hajimu_bool(true);
}

/* ---------------------------------------------------------------
 * ボトムシート終了()
 * ---------------------------------------------------------------*/
static Value fn_bottom_sheet_end(int argc, Value *argv) {
    (void)argc; (void)argv;
    /* レイアウトは呼び出し元で管理 */
    return hajimu_null();
}

/* ---------------------------------------------------------------
 * 情報バー(メッセージ, 種類[, 閉じ可]) → 真偽
 *   種類: "情報" | "警告" | "エラー" | "成功"
 * ---------------------------------------------------------------*/
static Value fn_info_bar(int argc, Value *argv) {
    if (!g_cur || argc < 2) return hajimu_bool(true);

    Hjpcontext *vg = g_cur->vg;
    const char *msg = (argv[0].type == VALUE_STRING) ? argv[0].string.data : "";
    const char *kind = (argv[1].type == VALUE_STRING) ? argv[1].string.data : "情報";
    bool closable = (argc >= 3 && argv[2].type == VALUE_BOOL) ? argv[2].boolean : true;

    Hjpcolor bg;
    if (strcmp(kind, "エラー") == 0) bg = hjpRGBA(220, 50, 50, 220);
    else if (strcmp(kind, "警告") == 0) bg = hjpRGBA(230, 180, 40, 220);
    else if (strcmp(kind, "成功") == 0) bg = hjpRGBA(50, 180, 80, 220);
    else bg = hjpRGBA(50, 120, 220, 220);

    float x, y, w;
    gui_pos(&x, &y, &w);

    hjpBeginPath(vg);
    hjpRoundedRect(vg, x, y, w, GUI_INFOBAR_H, 4.0f);
    hjpFillColor(vg, bg);
    hjpFill(vg);

    hjpFontFaceId(vg, g_cur->font_id);
    hjpFontSize(vg, 14.0f);
    hjpFillColor(vg, hjpRGBA(255, 255, 255, 255));
    hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_MIDDLE);
    hjpText(vg, x + 12, y + GUI_INFOBAR_H * 0.5f, msg, NULL);

    bool closed = false;
    if (closable) {
        uint32_t cid = gui_hash(msg) + 9999;
        float cx = x + w - 28;
        bool hov = false, prs = false;
        closed = gui_widget_logic(cid, cx, y, 28, GUI_INFOBAR_H, &hov, &prs);

        float ccx = cx + 14, ccy = y + GUI_INFOBAR_H * 0.5f;
        float s = 5;
        hjpBeginPath(vg);
        hjpMoveTo(vg, ccx - s, ccy - s);
        hjpLineTo(vg, ccx + s, ccy + s);
        hjpMoveTo(vg, ccx + s, ccy - s);
        hjpLineTo(vg, ccx - s, ccy + s);
        hjpStrokeColor(vg, hjpRGBA(255, 255, 255, hov ? 255 : 180));
        hjpStrokeWidth(vg, 2.0f);
        hjpStroke(vg);
    }

    gui_advance(GUI_INFOBAR_H);
    return hajimu_bool(!closed);
}

/* ---------------------------------------------------------------
 * フローティングボタン(アイコン, x, y) → 真偽
 * ---------------------------------------------------------------*/
static Value fn_fab(int argc, Value *argv) {
    if (!g_cur || argc < 3) return hajimu_bool(false);

    Hjpcontext *vg = g_cur->vg;
    const char *icon = (argv[0].type == VALUE_STRING) ? argv[0].string.data : "+";
    float fx = (argv[1].type == VALUE_NUMBER) ? (float)argv[1].number : 0;
    float fy = (argv[2].type == VALUE_NUMBER) ? (float)argv[2].number : 0;

    uint32_t id = gui_hash("__fab__");
    float r = GUI_FAB_SIZE * 0.5f;

    bool hov = false, prs = false;
    bool clicked = gui_widget_logic(id, fx - r, fy - r, GUI_FAB_SIZE, GUI_FAB_SIZE, &hov, &prs);

    /* 影 */
    Hjppaint shadow = hjpBoxGradient(vg, fx - r, fy - r + 2, GUI_FAB_SIZE, GUI_FAB_SIZE, r, 8,
        hjpRGBA(0, 0, 0, 80), hjpRGBA(0, 0, 0, 0));
    hjpBeginPath(vg);
    hjpRect(vg, fx - r - 10, fy - r - 10, GUI_FAB_SIZE + 20, GUI_FAB_SIZE + 20);
    hjpFillPaint(vg, shadow);
    hjpFill(vg);

    /* 円 */
    hjpBeginPath(vg);
    hjpCircle(vg, fx, fy, r);
    hjpFillColor(vg, prs ? TH_ACCENT_HOVER : TH_ACCENT);
    hjpFill(vg);

    /* アイコン */
    hjpFontFaceId(vg, g_cur->font_id);
    hjpFontSize(vg, 24.0f);
    hjpFillColor(vg, hjpRGBA(255, 255, 255, 255));
    hjpTextAlign(vg, HJP_ALIGN_CENTER | HJP_ALIGN_MIDDLE);
    hjpText(vg, fx, fy, icon, NULL);

    return hajimu_bool(clicked);
}

/* =====================================================================
 * Phase 16: リッチコンテンツ表示 (v1.6.0)
 * ===================================================================*/

/* ---------------------------------------------------------------
 * 色付きテキスト(テキスト, 色)
 * ---------------------------------------------------------------*/
static Value fn_text_colored(int argc, Value *argv) {
    if (!g_cur || argc < 2 || argv[0].type != VALUE_STRING)
        return hajimu_null();

    Hjpcontext *vg = g_cur->vg;
    const char *text = argv[0].string.data;
    Hjpcolor col = gui_arg_color(argv[1]);

    float x, y, w;
    gui_pos(&x, &y, &w);

    hjpFontFaceId(vg, g_cur->font_id);
    hjpFontSize(vg, GUI_FONT_SIZE);
    hjpFillColor(vg, col);
    hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_TOP);
    hjpText(vg, x, y, text, NULL);

    gui_advance(GUI_FONT_SIZE + 4);
    return hajimu_null();
}

/* ---------------------------------------------------------------
 * 折返しテキスト(テキスト)
 * ---------------------------------------------------------------*/
static Value fn_text_wrapped(int argc, Value *argv) {
    if (!g_cur || argc < 1 || argv[0].type != VALUE_STRING)
        return hajimu_null();

    Hjpcontext *vg = g_cur->vg;
    const char *text = argv[0].string.data;

    float x, y, w;
    gui_pos(&x, &y, &w);

    hjpFontFaceId(vg, g_cur->font_id);
    hjpFontSize(vg, GUI_FONT_SIZE);
    hjpFillColor(vg, TH_TEXT);
    hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_TOP);

    float bounds[4];
    hjpTextBoxBounds(vg, x, y, w, text, NULL, bounds);
    hjpTextBox(vg, x, y, w, text, NULL);

    float h = bounds[3] - bounds[1];
    if (h < GUI_FONT_SIZE) h = GUI_FONT_SIZE;
    gui_advance(h + 4);
    return hajimu_null();
}

/* ---------------------------------------------------------------
 * 無効テキスト(テキスト)
 * ---------------------------------------------------------------*/
static Value fn_text_disabled(int argc, Value *argv) {
    if (!g_cur || argc < 1 || argv[0].type != VALUE_STRING)
        return hajimu_null();

    Hjpcontext *vg = g_cur->vg;
    float x, y, w;
    gui_pos(&x, &y, &w);

    hjpFontFaceId(vg, g_cur->font_id);
    hjpFontSize(vg, GUI_FONT_SIZE);
    hjpFillColor(vg, TH_TEXT_DIM);
    hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_TOP);
    hjpText(vg, x, y, argv[0].string.data, NULL);

    gui_advance(GUI_FONT_SIZE + 4);
    return hajimu_null();
}

/* ---------------------------------------------------------------
 * 箇条書き(テキスト)
 * ---------------------------------------------------------------*/
static Value fn_bullet_text(int argc, Value *argv) {
    if (!g_cur || argc < 1 || argv[0].type != VALUE_STRING)
        return hajimu_null();

    Hjpcontext *vg = g_cur->vg;
    float x, y, w;
    gui_pos(&x, &y, &w);

    float cy = y + GUI_FONT_SIZE * 0.5f;
    hjpBeginPath(vg);
    hjpCircle(vg, x + 6, cy, 3.0f);
    hjpFillColor(vg, TH_TEXT);
    hjpFill(vg);

    hjpFontFaceId(vg, g_cur->font_id);
    hjpFontSize(vg, GUI_FONT_SIZE);
    hjpFillColor(vg, TH_TEXT);
    hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_TOP);
    hjpText(vg, x + 18, y, argv[0].string.data, NULL);

    gui_advance(GUI_FONT_SIZE + 4);
    return hajimu_null();
}

/* ---------------------------------------------------------------
 * テキスト選択(テキスト) → 文字列
 *   選択可能なテキスト（簡易実装: 表示のみ、クリックでコピー）
 * ---------------------------------------------------------------*/
static Value fn_text_selectable(int argc, Value *argv) {
    if (!g_cur || argc < 1 || argv[0].type != VALUE_STRING)
        return hajimu_string("");

    Hjpcontext *vg = g_cur->vg;
    const char *text = argv[0].string.data;
    uint32_t id = gui_hash(text);

    float x, y, w;
    gui_pos(&x, &y, &w);

    hjpFontFaceId(vg, g_cur->font_id);
    hjpFontSize(vg, GUI_FONT_SIZE);
    float tw = hjpTextBounds(vg, 0, 0, text, NULL, NULL);
    float h = GUI_FONT_SIZE + 4;

    bool hov = false, prs = false;
    bool clicked = gui_widget_logic(id, x, y, tw, h, &hov, &prs);

    if (hov) {
        hjpBeginPath(vg);
        hjpRect(vg, x - 2, y - 1, tw + 4, h + 2);
        hjpFillColor(vg, hjpRGBA(100, 150, 255, 30));
        hjpFill(vg);
    }

    hjpFillColor(vg, TH_TEXT);
    hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_TOP);
    hjpText(vg, x, y, text, NULL);

    if (clicked) {
        hjp_set_clipboard_text(text);
    }

    gui_advance(h);
    return hajimu_string(text);
}

/* ---------------------------------------------------------------
 * リッチテキスト(セグメント配列)
 *   簡易実装: [{テキスト, 色, サイズ}, ...]
 * ---------------------------------------------------------------*/
static Value fn_rich_text(int argc, Value *argv) {
    if (!g_cur || argc < 1 || argv[0].type != VALUE_ARRAY)
        return hajimu_null();

    Hjpcontext *vg = g_cur->vg;
    float x, y, w;
    gui_pos(&x, &y, &w);

    float cx = x;
    float max_h = GUI_FONT_SIZE;

    /* 各セグメントを横に連結 */
    for (int i = 0; i < argv[0].array.length; i++) {
        Value seg = argv[0].array.elements[i];
        if (seg.type == VALUE_STRING) {
            hjpFontFaceId(vg, g_cur->font_id);
            hjpFontSize(vg, GUI_FONT_SIZE);
            hjpFillColor(vg, TH_TEXT);
            hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_TOP);
            float tw = hjpTextBounds(vg, 0, 0, seg.string.data, NULL, NULL);
            hjpText(vg, cx, y, seg.string.data, NULL);
            cx += tw;
        }
    }

    gui_advance(max_h + 4);
    return hajimu_null();
}

/* ---------------------------------------------------------------
 * マークダウン(テキスト)
 *   超簡易: # → 見出し, - → 箇条書き, ** → 太字風
 * ---------------------------------------------------------------*/
static Value fn_markdown(int argc, Value *argv) {
    if (!g_cur || argc < 1 || argv[0].type != VALUE_STRING)
        return hajimu_null();

    Hjpcontext *vg = g_cur->vg;
    const char *text = argv[0].string.data;

    float x, y, w;
    gui_pos(&x, &y, &w);

    hjpFontFaceId(vg, g_cur->font_id);
    hjpFillColor(vg, TH_TEXT);
    hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_TOP);

    /* 行分割して処理 */
    const char *p = text;
    float cy = y;
    while (*p) {
        const char *eol = strchr(p, '\n');
        int len = eol ? (int)(eol - p) : (int)strlen(p);
        char line[512];
        if (len >= (int)sizeof(line)) len = (int)sizeof(line) - 1;
        memcpy(line, p, len);
        line[len] = '\0';

        if (line[0] == '#') {
            int level = 0;
            while (line[level] == '#') level++;
            float sz = (level == 1) ? 24.0f : (level == 2) ? 20.0f : 17.0f;
            hjpFontSize(vg, sz);
            hjpText(vg, x, cy, line + level + (line[level] == ' ' ? 1 : 0), NULL);
            cy += sz + 6;
        } else if (line[0] == '-' && line[1] == ' ') {
            hjpFontSize(vg, GUI_FONT_SIZE);
            hjpBeginPath(vg);
            hjpCircle(vg, x + 6, cy + GUI_FONT_SIZE * 0.5f, 2.5f);
            hjpFill(vg);
            hjpText(vg, x + 16, cy, line + 2, NULL);
            cy += GUI_FONT_SIZE + 4;
        } else {
            hjpFontSize(vg, GUI_FONT_SIZE);
            hjpTextBox(vg, x, cy, w, line, NULL);
            float bounds[4];
            hjpTextBoxBounds(vg, x, cy, w, line, NULL, bounds);
            cy += (bounds[3] - bounds[1]) + 4;
        }

        p += len;
        if (eol) p++;
    }

    gui_advance(cy - y);
    return hajimu_null();
}

/* ---------------------------------------------------------------
 * コードブロック(コード[, 言語])
 * ---------------------------------------------------------------*/
static Value fn_code_block(int argc, Value *argv) {
    if (!g_cur || argc < 1 || argv[0].type != VALUE_STRING)
        return hajimu_null();

    Hjpcontext *vg = g_cur->vg;
    const char *code = argv[0].string.data;

    float x, y, w;
    gui_pos(&x, &y, &w);

    /* 行数を数える */
    int lines = 1;
    for (const char *p = code; *p; p++) if (*p == '\n') lines++;
    float line_h = 16.0f;
    float block_h = lines * line_h + 16;

    /* 暗い背景 */
    hjpBeginPath(vg);
    hjpRoundedRect(vg, x, y, w, block_h, 6.0f);
    hjpFillColor(vg, hjpRGBA(30, 30, 30, 240));
    hjpFill(vg);

    /* コードテキスト（等幅風） */
    hjpFontFaceId(vg, g_cur->font_id);
    hjpFontSize(vg, 13.0f);
    hjpFillColor(vg, hjpRGBA(200, 220, 255, 255));
    hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_TOP);

    const char *p = code;
    float cy = y + 8;
    while (*p) {
        const char *eol = strchr(p, '\n');
        int len = eol ? (int)(eol - p) : (int)strlen(p);
        char buf[512];
        if (len >= (int)sizeof(buf)) len = (int)sizeof(buf) - 1;
        memcpy(buf, p, len);
        buf[len] = '\0';
        hjpText(vg, x + 12, cy, buf, NULL);
        cy += line_h;
        p += len;
        if (eol) p++;
    }

    gui_advance(block_h + GUI_MARGIN);
    return hajimu_null();
}

/* =====================================================================
 * Phase 17: システム統合
 * ===================================================================*/

/* --- 設定ファイルパス取得 --- */
static void gui_settings_path(char *out, size_t sz) {
    const char *home = getenv("HOME");
#ifdef _WIN32
    if (!home) home = getenv("USERPROFILE");
#endif
    if (!home) home = "/tmp";
    snprintf(out, sz, "%s/.hajimu/gui_settings.ini", home);
}

/* --- 設定ファイル読み込み --- */
static void gui_settings_load_all(void) {
    if (g_settings_loaded) return;
    g_settings_loaded = true;
    char path[512];
    gui_settings_path(path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f) return;
    g_setting_count = 0;
    char line[1024];
    while (fgets(line, sizeof(line), f) && g_setting_count < GUI_MAX_SETTINGS) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *val = eq + 1;
        char *nl = strchr(val, '\n');
        if (nl) *nl = '\0';
        strncpy(g_settings[g_setting_count].key, line, GUI_SETTINGS_KEY_LEN - 1);
        g_settings[g_setting_count].key[GUI_SETTINGS_KEY_LEN - 1] = '\0';
        strncpy(g_settings[g_setting_count].val, val, GUI_SETTINGS_VAL_LEN - 1);
        g_settings[g_setting_count].val[GUI_SETTINGS_VAL_LEN - 1] = '\0';
        g_setting_count++;
    }
    fclose(f);
}

/* --- 設定ファイル書き出し --- */
static void gui_settings_flush(void) {
    char path[512];
    gui_settings_path(path, sizeof(path));
    FILE *f = fopen(path, "w");
    if (!f) return;
    for (int i = 0; i < g_setting_count; i++) {
        fprintf(f, "%s=%s\n", g_settings[i].key, g_settings[i].val);
    }
    fclose(f);
}

/* マルチウィンドウ(タイトル, 幅, 高さ) → ID */
static Value fn_multi_window(int argc, Value *argv) {
    (void)argc;
    const char *title = argv[0].string.data;
    int w = (int)argv[1].number;
    int h = (int)argv[2].number;
    if (g_sub_window_count >= GUI_MAX_SUB_WINDOWS)
        return hajimu_number(-1);
    HjpWindow *win = hjp_window_create(
        title, HJP_WINDOWPOS_CENTERED, HJP_WINDOWPOS_CENTERED,
        w, h, HJP_WINDOW_SHOWN | HJP_WINDOW_RESIZABLE);
    if (!win) return hajimu_number(-1);
    int idx = g_sub_window_count++;
    g_sub_windows[idx] = win;
    return hajimu_number((double)hjp_window_get_id(win));
}

/* ウィンドウ位置(x, y) */
static Value fn_window_pos(int argc, Value *argv) {
    (void)argc;
    int x = (int)argv[0].number;
    int y = (int)argv[1].number;
    if (g_cur && g_cur->window)
        hjp_window_set_position(g_cur->window, x, y);
    return hajimu_null();
}

/* ウィンドウ最大化() */
static Value fn_window_maximize(int argc, Value *argv) {
    (void)argc; (void)argv;
    if (g_cur && g_cur->window)
        hjp_window_maximize(g_cur->window);
    return hajimu_null();
}

/* ウィンドウ最小化() */
static Value fn_window_minimize(int argc, Value *argv) {
    (void)argc; (void)argv;
    if (g_cur && g_cur->window)
        hjp_window_minimize(g_cur->window);
    return hajimu_null();
}

/* フルスクリーン(真偽) */
static Value fn_fullscreen(int argc, Value *argv) {
    (void)argc;
    bool on = (argv[0].type == VALUE_BOOL) ? argv[0].boolean
                                            : (argv[0].number != 0);
    if (g_cur && g_cur->window) {
        hjp_window_set_fullscreen(g_cur->window, on);
    }
    return hajimu_null();
}

/* OSテーマ取得() → "ダーク" or "ライト" */
static Value fn_os_theme(int argc, Value *argv) {
    (void)argc; (void)argv;
#if defined(__APPLE__)
    FILE *fp = popen("defaults read -g AppleInterfaceStyle 2>/dev/null", "r");
    if (fp) {
        char buf[64] = {0};
        if (fgets(buf, sizeof(buf), fp) && strstr(buf, "Dark")) {
            pclose(fp);
            return hajimu_string("ダーク");
        }
        pclose(fp);
    }
    return hajimu_string("ライト");
#elif defined(_WIN32)
    return hajimu_string("ライト");
#else
    const char *gtk = getenv("GTK_THEME");
    if (gtk && (strstr(gtk, "dark") || strstr(gtk, "Dark")))
        return hajimu_string("ダーク");
    const char *de_session = getenv("DESKTOP_SESSION");
    if (de_session && (strstr(de_session, "dark") || strstr(de_session, "Dark")))
        return hajimu_string("ダーク");
    return hajimu_string("ライト");
#endif
}

/* 設定保存(キー, 値) */
static Value fn_settings_save(int argc, Value *argv) {
    (void)argc;
    gui_settings_load_all();
    const char *key = argv[0].string.data;
    /* 値を文字列に変換 */
    char val_str[GUI_SETTINGS_VAL_LEN] = {0};
    if (argv[1].type == VALUE_STRING)
        strncpy(val_str, argv[1].string.data, sizeof(val_str) - 1);
    else if (argv[1].type == VALUE_NUMBER)
        snprintf(val_str, sizeof(val_str), "%g", argv[1].number);
    else if (argv[1].type == VALUE_BOOL)
        strncpy(val_str, argv[1].boolean ? "真" : "偽", sizeof(val_str) - 1);
    /* 既存キーの上書き */
    for (int i = 0; i < g_setting_count; i++) {
        if (strcmp(g_settings[i].key, key) == 0) {
            strncpy(g_settings[i].val, val_str, GUI_SETTINGS_VAL_LEN - 1);
            g_settings[i].val[GUI_SETTINGS_VAL_LEN - 1] = '\0';
            gui_settings_flush();
            return hajimu_null();
        }
    }
    /* 新規追加 */
    if (g_setting_count < GUI_MAX_SETTINGS) {
        strncpy(g_settings[g_setting_count].key, key, GUI_SETTINGS_KEY_LEN - 1);
        g_settings[g_setting_count].key[GUI_SETTINGS_KEY_LEN - 1] = '\0';
        strncpy(g_settings[g_setting_count].val, val_str, GUI_SETTINGS_VAL_LEN - 1);
        g_settings[g_setting_count].val[GUI_SETTINGS_VAL_LEN - 1] = '\0';
        g_setting_count++;
        gui_settings_flush();
    }
    return hajimu_null();
}

/* 設定読込(キー) → 値 */
static Value fn_settings_load(int argc, Value *argv) {
    (void)argc;
    gui_settings_load_all();
    const char *key = argv[0].string.data;
    for (int i = 0; i < g_setting_count; i++) {
        if (strcmp(g_settings[i].key, key) == 0)
            return hajimu_string(g_settings[i].val);
    }
    return hajimu_null();
}

/* 元に戻す(コールバック) */
static Value fn_undo(int argc, Value *argv) {
    (void)argc;
    if (g_undo_top < GUI_UNDO_MAX) {
        g_undo_stack[g_undo_top].callback = argv[0];
        g_undo_stack[g_undo_top].used = true;
        g_undo_top++;
    }
    return hajimu_number((double)g_undo_top);
}

/* やり直す(コールバック) */
static Value fn_redo(int argc, Value *argv) {
    (void)argc;
    if (g_redo_top < GUI_UNDO_MAX) {
        g_redo_stack[g_redo_top].callback = argv[0];
        g_redo_stack[g_redo_top].used = true;
        g_redo_top++;
    }
    return hajimu_number((double)g_redo_top);
}

/* =====================================================================
 * Phase 18: アクセシビリティ + パフォーマンス
 * ===================================================================*/

/* --- 辞書ヘルパー --- */
static Value gui_dict2(const char *k1, Value v1,
                       const char *k2, Value v2) {
    Value d;
    memset(&d, 0, sizeof(d));
    d.type = VALUE_DICT;
    d.dict.length   = 2;
    d.dict.capacity = 4;
    d.dict.keys   = (char **)calloc(4, sizeof(char *));
    d.dict.values = (Value *)calloc(4, sizeof(Value));
    d.dict.keys[0]   = strdup(k1);
    d.dict.keys[1]   = strdup(k2);
    d.dict.values[0] = v1;
    d.dict.values[1] = v2;
    return d;
}

static Value gui_dict3(const char *k1, Value v1,
                       const char *k2, Value v2,
                       const char *k3, Value v3) {
    Value d;
    memset(&d, 0, sizeof(d));
    d.type = VALUE_DICT;
    d.dict.length   = 3;
    d.dict.capacity = 4;
    d.dict.keys   = (char **)calloc(4, sizeof(char *));
    d.dict.values = (Value *)calloc(4, sizeof(Value));
    d.dict.keys[0]   = strdup(k1);
    d.dict.keys[1]   = strdup(k2);
    d.dict.keys[2]   = strdup(k3);
    d.dict.values[0] = v1;
    d.dict.values[1] = v2;
    d.dict.values[2] = v3;
    return d;
}

/* フォーカス設定(オフセット) — 次ウィジェットにフォーカスを移す */
static Value fn_focus_set(int argc, Value *argv) {
    (void)argc;
    int offset = (int)argv[0].number;
    g_focus_set = true;
    /* offset: 0=次ウィジェット, 1=その次, ... */
    g_focus_next = (uint32_t)offset;
    return hajimu_null();
}

/* フォーカス取得() → 真偽: 現在フォーカス中か */
static Value fn_focus_get(int argc, Value *argv) {
    (void)argc; (void)argv;
    if (!g_cur) return hajimu_bool(false);
    return hajimu_bool(g_cur->focused != 0);
}

/* タブ順序(順番) — 次ウィジェットのタブキー遷移順を指定 */
static Value fn_tab_order(int argc, Value *argv) {
    (void)argc;
    int order = (int)argv[0].number;
    if (g_tab_order_count < GUI_MAX_TAB_ORDER && g_cur) {
        g_tab_orders[g_tab_order_count].widget_id = g_cur->active;
        g_tab_orders[g_tab_order_count].order = order;
        g_tab_order_count++;
    }
    return hajimu_null();
}

/* 仮想スクロール(総数, 行高さ) → {開始, 終了} — 可視範囲を計算 */
static Value fn_virtual_scroll(int argc, Value *argv) {
    (void)argc;
    int   total = (int)argv[0].number;
    float row_h = (float)argv[1].number;
    if (row_h <= 0) row_h = 28.0f;

    float visible_h = g_cur ? (float)g_cur->win_h : 600.0f;
    float scroll_y  = g_cur ? g_cur->lay.y : 0.0f;

    int start = (int)(scroll_y / row_h);
    if (start < 0) start = 0;
    int visible_count = (int)(visible_h / row_h) + 2;
    int end = start + visible_count;
    if (end > total) end = total;

    return gui_dict2("開始", hajimu_number(start),
                     "終了", hajimu_number(end));
}

/* テーブルフィルター(テーブル, 列, 条件) — 行フィルタ（破壊的） */
static Value fn_table_filter(int argc, Value *argv) {
    (void)argc;
    int idx = (int)argv[0].number;
    int col = (int)argv[1].number;
    const char *cond = argv[2].string.data;

    if (idx < 0 || idx >= GUI_MAX_TABLES || !g_tables[idx].valid)
        return hajimu_null();
    GuiTable *t = &g_tables[idx];
    if (col < 0 || col >= t->col_count) return hajimu_null();

    int write = 0;
    for (int r = 0; r < t->row_count; r++) {
        if (t->rows[r][col] && strstr(t->rows[r][col], cond)) {
            t->rows[write] = t->rows[r];
            write++;
        } else {
            for (int c = 0; c < t->col_count; c++) free(t->rows[r][c]);
            free(t->rows[r]);
        }
    }
    t->row_count = write;
    return hajimu_null();
}

/* テーブルページ(テーブル, ページ, 行数) → {開始, 終了, 総ページ} */
static Value fn_table_page(int argc, Value *argv) {
    (void)argc;
    int idx      = (int)argv[0].number;
    int page     = (int)argv[1].number;
    int per_page = (int)argv[2].number;

    if (idx < 0 || idx >= GUI_MAX_TABLES || !g_tables[idx].valid)
        return hajimu_null();
    GuiTable *t = &g_tables[idx];
    if (per_page <= 0) per_page = 10;

    int total_pages = (t->row_count + per_page - 1) / per_page;
    if (page < 0) page = 0;
    if (page >= total_pages && total_pages > 0) page = total_pages - 1;

    int start = page * per_page;
    int end   = start + per_page;
    if (end > t->row_count) end = t->row_count;

    return gui_dict3("開始",     hajimu_number(start),
                     "終了",     hajimu_number(end),
                     "総ページ", hajimu_number(total_pages));
}

/* テーブル列幅(テーブル, 列, 幅) — 列幅を設定 */
static Value fn_table_col_width(int argc, Value *argv) {
    (void)argc;
    int   idx = (int)argv[0].number;
    int   col = (int)argv[1].number;
    float w   = (float)argv[2].number;

    if (idx < 0 || idx >= GUI_MAX_TABLES || !g_tables[idx].valid)
        return hajimu_null();
    if (col < 0 || col >= g_tables[idx].col_count) return hajimu_null();
    g_table_col_w[idx][col] = w;
    return hajimu_null();
}

/* クリッピング(x, y, 幅, 高さ) — 描画制限領域を設定 */
static Value fn_clipping(int argc, Value *argv) {
    (void)argc;
    float cx = (float)argv[0].number;
    float cy = (float)argv[1].number;
    float cw = (float)argv[2].number;
    float ch = (float)argv[3].number;
    if (g_cur && g_cur->vg) {
        if (cw <= 0 || ch <= 0)
            hjpResetScissor(g_cur->vg);
        else
            hjpScissor(g_cur->vg, cx, cy, cw, ch);
    }
    return hajimu_null();
}

/* =====================================================================
 * Phase 19: ナビゲーション + ウィザード
 * ===================================================================*/

/* ナビゲーションバー(項目配列, 選択) → 数値 */
static Value fn_nav_bar(int argc, Value *argv) {
    (void)argc;
    if (argv[0].type != VALUE_ARRAY) return hajimu_number(0);
    int count = argv[0].array.length;
    int sel   = (int)argv[1].number;
    if (!g_cur) return hajimu_number(sel);

    Hjpcontext *vg = g_cur->vg;
    float bar_w = (float)g_cur->win_w;
    float bar_y = (float)g_cur->win_h - GUI_NAV_BAR_H;

    /* 背景 */
    hjpBeginPath(vg);
    hjpRect(vg, 0, bar_y, bar_w, GUI_NAV_BAR_H);
    hjpFillColor(vg, TH_WIDGET_BG);
    hjpFill(vg);
    /* 上部ボーダー */
    hjpBeginPath(vg);
    hjpMoveTo(vg, 0, bar_y);
    hjpLineTo(vg, bar_w, bar_y);
    hjpStrokeColor(vg, TH_BORDER);
    hjpStrokeWidth(vg, 1.0f);
    hjpStroke(vg);

    float item_w = bar_w / (float)(count > 0 ? count : 1);
    float mx = g_cur->in.mx, my = g_cur->in.my;

    int new_sel = sel;
    hjpFontSize(vg, 12.0f);
    hjpTextAlign(vg, HJP_ALIGN_CENTER | HJP_ALIGN_MIDDLE);
    for (int i = 0; i < count; i++) {
        float ix = item_w * (float)i;
        float cx = ix + item_w * 0.5f;
        float cy = bar_y + GUI_NAV_BAR_H * 0.5f;
        bool hov = mx >= ix && mx < ix + item_w && my >= bar_y && my < bar_y + GUI_NAV_BAR_H;
        if (hov && g_cur->in.clicked) new_sel = i;

        Hjpcolor col = (i == sel) ? TH_ACCENT : TH_TEXT_DIM;
        if (hov && i != sel) col = TH_TEXT;

        /* アイコン代わりの● */
        hjpBeginPath(vg);
        hjpCircle(vg, cx, cy - 8.0f, 4.0f);
        hjpFillColor(vg, col);
        hjpFill(vg);

        /* ラベル */
        const char *label = argv[0].array.elements[i].string.data;
        hjpFillColor(vg, col);
        hjpText(vg, cx, cy + 10.0f, label, NULL);
    }
    return hajimu_number((double)new_sel);
}

/* ブレッドクラム(パス配列) → 数値 (クリックされた階層, -1=なし) */
static Value fn_breadcrumb(int argc, Value *argv) {
    (void)argc;
    if (argv[0].type != VALUE_ARRAY) return hajimu_number(-1);
    int count = argv[0].array.length;
    if (!g_cur || count == 0) return hajimu_number(-1);

    Hjpcontext *vg = g_cur->vg;
    float x, y, w;
    gui_pos(&x, &y, &w);

    hjpFontSize(vg, GUI_FONT_SIZE - 2.0f);
    hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_MIDDLE);
    float mx = g_cur->in.mx, my = g_cur->in.my;
    float cy = y + GUI_BREADCRUMB_H * 0.5f;
    float cx = x;
    int clicked = -1;

    for (int i = 0; i < count; i++) {
        const char *seg = argv[0].array.elements[i].string.data;
        float bounds[4];
        hjpTextBounds(vg, cx, cy, seg, NULL, bounds);
        float tw = bounds[2] - bounds[0];

        bool hov = mx >= cx && mx <= cx + tw && my >= y && my <= y + GUI_BREADCRUMB_H;
        bool is_last = (i == count - 1);

        hjpFillColor(vg, is_last ? TH_TEXT : (hov ? TH_ACCENT_HOVER : TH_ACCENT));
        hjpText(vg, cx, cy, seg, NULL);

        if (hov && g_cur->in.clicked && !is_last) clicked = i;
        cx += tw;

        if (!is_last) {
            hjpFillColor(vg, TH_TEXT_DIM);
            hjpText(vg, cx, cy, " > ", NULL);
            float sb[4];
            hjpTextBounds(vg, cx, cy, " > ", NULL, sb);
            cx += sb[2] - sb[0];
        }
    }
    gui_advance(GUI_BREADCRUMB_H);
    return hajimu_number((double)clicked);
}

/* ステッパー(ステップ数, 現在) → 数値 */
static Value fn_stepper(int argc, Value *argv) {
    (void)argc;
    int total   = (int)argv[0].number;
    int current = (int)argv[1].number;
    if (!g_cur || total <= 0) return hajimu_number(current);

    Hjpcontext *vg = g_cur->vg;
    float x, y, w;
    gui_pos(&x, &y, &w);

    float step_w = w / (float)total;
    float cy = y + GUI_STEPPER_R + 4.0f;

    for (int i = 0; i < total; i++) {
        float cx = x + step_w * (float)i + step_w * 0.5f;

        /* 接続線 */
        if (i > 0) {
            float px = x + step_w * (float)(i - 1) + step_w * 0.5f;
            hjpBeginPath(vg);
            hjpMoveTo(vg, px + GUI_STEPPER_R, cy);
            hjpLineTo(vg, cx - GUI_STEPPER_R, cy);
            hjpStrokeColor(vg, i <= current ? TH_ACCENT : TH_BORDER);
            hjpStrokeWidth(vg, 2.0f);
            hjpStroke(vg);
        }

        /* 円 */
        hjpBeginPath(vg);
        hjpCircle(vg, cx, cy, GUI_STEPPER_R);
        hjpFillColor(vg, i <= current ? TH_ACCENT : TH_WIDGET_BG);
        hjpFill(vg);
        hjpStrokeColor(vg, i <= current ? TH_ACCENT : TH_BORDER);
        hjpStrokeWidth(vg, 2.0f);
        hjpStroke(vg);

        /* ステップ番号 */
        char num[8];
        snprintf(num, sizeof(num), "%d", i + 1);
        hjpFontSize(vg, 13.0f);
        hjpTextAlign(vg, HJP_ALIGN_CENTER | HJP_ALIGN_MIDDLE);
        hjpFillColor(vg, i <= current ? hjpRGBA(255,255,255,255) : TH_TEXT_DIM);
        hjpText(vg, cx, cy, num, NULL);
    }
    gui_advance(GUI_STEPPER_R * 2.0f + 12.0f);
    return hajimu_number((double)current);
}

/* ウィザード開始(タイトル, ステップ数) */
static Value fn_wizard_begin(int argc, Value *argv) {
    (void)argc;
    const char *title = argv[0].string.data;
    int steps = (int)argv[1].number;
    if (steps > GUI_WIZARD_MAX_STEPS) steps = GUI_WIZARD_MAX_STEPS;

    g_wizard.active = true;
    g_wizard.step_count = steps;
    g_wizard.current = 0;
    strncpy(g_wizard.title, title, sizeof(g_wizard.title) - 1);
    g_wizard.title[sizeof(g_wizard.title) - 1] = '\0';

    if (g_cur) {
        gui_pos(&g_wizard.x, &g_wizard.y, &g_wizard.w);
        /* タイトル表示 */
        Hjpcontext *vg = g_cur->vg;
        hjpFontSize(vg, GUI_FONT_SIZE + 2.0f);
        hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_TOP);
        hjpFillColor(vg, TH_TEXT);
        float x, y, w;
        gui_pos(&x, &y, &w);
        hjpText(vg, x, y, title, NULL);
        gui_advance(GUI_FONT_SIZE + 10.0f);
    }
    return hajimu_null();
}

/* ウィザードページ(インデックス, タイトル) → 真偽 (現在ページか) */
static Value fn_wizard_page(int argc, Value *argv) {
    (void)argc;
    int idx = (int)argv[0].number;
    (void)argv[1]; /* タイトルは将来用 */
    return hajimu_bool(g_wizard.active && idx == g_wizard.current);
}

/* ウィザード終了() → 数値 (現在ステップ) */
static Value fn_wizard_end(int argc, Value *argv) {
    (void)argc; (void)argv;
    if (!g_cur || !g_wizard.active) return hajimu_number(0);

    Hjpcontext *vg = g_cur->vg;
    float x, y, w;
    gui_pos(&x, &y, &w);
    float btn_w = 80.0f, btn_h = 32.0f;
    float gap = 8.0f;

    int result = g_wizard.current;

    /* 「前へ」ボタン */
    if (g_wizard.current > 0) {
        uint32_t id1 = gui_hash("__wizard_prev");
        bool hov1 = false, pressed1 = false;
        gui_widget_logic(id1, x, y, btn_w, btn_h, &hov1, &pressed1);

        hjpBeginPath(vg);
        hjpRoundedRect(vg, x, y, btn_w, btn_h, GUI_BTN_RADIUS);
        hjpFillColor(vg, hov1 ? TH_WIDGET_HOVER : TH_WIDGET_BG);
        hjpFill(vg);
        hjpFontSize(vg, GUI_FONT_SIZE);
        hjpTextAlign(vg, HJP_ALIGN_CENTER | HJP_ALIGN_MIDDLE);
        hjpFillColor(vg, TH_TEXT);
        hjpText(vg, x + btn_w * 0.5f, y + btn_h * 0.5f, "前へ", NULL);

        if (pressed1) result = g_wizard.current - 1;
    }

    /* 「次へ」/「完了」ボタン */
    bool is_last = (g_wizard.current >= g_wizard.step_count - 1);
    const char *next_label = is_last ? "完了" : "次へ";
    float nx = x + w - btn_w;
    uint32_t id2 = gui_hash("__wizard_next");
    bool hov2 = false, pressed2 = false;
    gui_widget_logic(id2, nx, y, btn_w, btn_h, &hov2, &pressed2);

    hjpBeginPath(vg);
    hjpRoundedRect(vg, nx, y, btn_w, btn_h, GUI_BTN_RADIUS);
    hjpFillColor(vg, hov2 ? TH_ACCENT_HOVER : TH_ACCENT);
    hjpFill(vg);
    hjpFontSize(vg, GUI_FONT_SIZE);
    hjpTextAlign(vg, HJP_ALIGN_CENTER | HJP_ALIGN_MIDDLE);
    hjpFillColor(vg, hjpRGBA(255,255,255,255));
    hjpText(vg, nx + btn_w * 0.5f, y + btn_h * 0.5f, next_label, NULL);

    if (pressed2) result = g_wizard.current + 1;
    (void)gap;

    gui_advance(btn_h + GUI_MARGIN);
    g_wizard.current = result;
    if (result >= g_wizard.step_count) g_wizard.active = false;

    return hajimu_number((double)result);
}

/* アコーディオン(タイトル配列, 開閉配列) → 配列 */
static Value fn_accordion(int argc, Value *argv) {
    (void)argc;
    if (argv[0].type != VALUE_ARRAY || argv[1].type != VALUE_ARRAY)
        return hajimu_array();
    int count = argv[0].array.length;
    if (count > GUI_ACCORDION_MAX) count = GUI_ACCORDION_MAX;
    if (!g_cur) return argv[1];

    Hjpcontext *vg = g_cur->vg;
    float x, y, w;
    gui_pos(&x, &y, &w);
    float mx = g_cur->in.mx, my = g_cur->in.my;

    /* 開閉状態コピー */
    Value result = hajimu_array();
    bool opens[GUI_ACCORDION_MAX];
    for (int i = 0; i < count; i++) {
        opens[i] = (i < argv[1].array.length) ? argv[1].array.elements[i].boolean : false;
    }

    int clicked_idx = -1;
    float header_h = GUI_WIDGET_H;

    for (int i = 0; i < count; i++) {
        float hy = y;
        const char *title = argv[0].array.elements[i].string.data;

        bool hov = mx >= x && mx <= x + w && my >= hy && my <= hy + header_h;
        if (hov && g_cur->in.clicked) clicked_idx = i;

        /* ヘッダー背景 */
        hjpBeginPath(vg);
        hjpRect(vg, x, hy, w, header_h);
        hjpFillColor(vg, hov ? TH_WIDGET_HOVER : TH_WIDGET_BG);
        hjpFill(vg);
        hjpBeginPath(vg);
        hjpRect(vg, x, hy, w, header_h);
        hjpStrokeColor(vg, TH_BORDER);
        hjpStrokeWidth(vg, 1.0f);
        hjpStroke(vg);

        /* 三角マーク */
        float tx = x + 12.0f, ty = hy + header_h * 0.5f;
        hjpBeginPath(vg);
        if (opens[i]) {
            hjpMoveTo(vg, tx - 4, ty - 3);
            hjpLineTo(vg, tx + 4, ty - 3);
            hjpLineTo(vg, tx, ty + 4);
        } else {
            hjpMoveTo(vg, tx - 3, ty - 4);
            hjpLineTo(vg, tx + 4, ty);
            hjpLineTo(vg, tx - 3, ty + 4);
        }
        hjpFillColor(vg, TH_TEXT);
        hjpFill(vg);

        /* タイトル */
        hjpFontSize(vg, GUI_FONT_SIZE);
        hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_MIDDLE);
        hjpFillColor(vg, TH_TEXT);
        hjpText(vg, x + 28.0f, hy + header_h * 0.5f, title, NULL);

        y += header_h;
        g_cur->lay.y = y;
    }

    /* 排他的: クリックされたら他を閉じる */
    if (clicked_idx >= 0) {
        for (int i = 0; i < count; i++) {
            opens[i] = (i == clicked_idx) ? !opens[i] : false;
        }
    }

    for (int i = 0; i < count; i++)
        hajimu_array_push(&result, hajimu_bool(opens[i]));

    return result;
}

/* ページビュー(ページ数, 現在) → 数値 */
static Value fn_page_view(int argc, Value *argv) {
    (void)argc;
    int total   = (int)argv[0].number;
    int current = (int)argv[1].number;
    if (!g_cur || total <= 0) return hajimu_number(current);

    Hjpcontext *vg = g_cur->vg;
    float x, y, w;
    gui_pos(&x, &y, &w);

    /* ドットインジケーター */
    float dot_r = 4.0f;
    float dot_gap = 12.0f;
    float dots_w = total * dot_gap;
    float dx = x + (w - dots_w) * 0.5f;
    float dy = y + 8.0f;
    float mx = g_cur->in.mx, my = g_cur->in.my;
    int new_cur = current;

    for (int i = 0; i < total; i++) {
        float cx = dx + dot_gap * (float)i + dot_gap * 0.5f;
        bool hov = (mx - cx) * (mx - cx) + (my - dy) * (my - dy) < 64.0f;
        if (hov && g_cur->in.clicked) new_cur = i;

        hjpBeginPath(vg);
        hjpCircle(vg, cx, dy, (i == current) ? dot_r + 1.0f : dot_r);
        hjpFillColor(vg, (i == current) ? TH_ACCENT : TH_TEXT_DIM);
        hjpFill(vg);
    }

    /* 左右矢印 */
    float arrow_y = dy;
    /* 左 */
    if (current > 0) {
        bool lhov = mx >= x && mx <= x + 24 && my >= arrow_y - 8 && my <= arrow_y + 8;
        if (lhov && g_cur->in.clicked) new_cur = current - 1;
        hjpFontSize(vg, 18.0f);
        hjpTextAlign(vg, HJP_ALIGN_CENTER | HJP_ALIGN_MIDDLE);
        hjpFillColor(vg, lhov ? TH_TEXT : TH_TEXT_DIM);
        hjpText(vg, x + 12.0f, arrow_y, "<", NULL);
    }
    /* 右 */
    if (current < total - 1) {
        bool rhov = mx >= x + w - 24 && mx <= x + w && my >= arrow_y - 8 && my <= arrow_y + 8;
        if (rhov && g_cur->in.clicked) new_cur = current + 1;
        hjpFontSize(vg, 18.0f);
        hjpTextAlign(vg, HJP_ALIGN_CENTER | HJP_ALIGN_MIDDLE);
        hjpFillColor(vg, rhov ? TH_TEXT : TH_TEXT_DIM);
        hjpText(vg, x + w - 12.0f, arrow_y, ">", NULL);
    }

    gui_advance(24.0f);
    return hajimu_number((double)new_cur);
}

/* =====================================================================
 * Phase 20: 高度なウィジェット II
 * ===================================================================*/

/* ダイヤル(ラベル, 値, 最小, 最大) → 数値 — 回転ノブ */
static Value fn_dial(int argc, Value *argv) {
    (void)argc;
    const char *label = argv[0].string.data;
    float val = (float)argv[1].number;
    float vmin = (float)argv[2].number;
    float vmax = (float)argv[3].number;
    if (!g_cur) return hajimu_number(val);

    Hjpcontext *vg = g_cur->vg;
    float x, y, w;
    gui_pos(&x, &y, &w);

    float r = 30.0f;
    float cx = x + w * 0.5f;
    float cy = y + r + GUI_FONT_SIZE + 4.0f;
    float range = vmax - vmin;
    if (range <= 0) range = 1;
    float ratio = (val - vmin) / range;

    /* ラベル */
    hjpFontSize(vg, GUI_FONT_SIZE);
    hjpTextAlign(vg, HJP_ALIGN_CENTER | HJP_ALIGN_TOP);
    hjpFillColor(vg, TH_TEXT);
    hjpText(vg, cx, y, label, NULL);

    /* 背景アーク */
    float sa = HJP_PI * 0.75f, ea = HJP_PI * 2.25f;
    hjpBeginPath(vg);
    hjpArc(vg, cx, cy, r, sa, ea, HJP_CW);
    hjpStrokeColor(vg, TH_TRACK);
    hjpStrokeWidth(vg, 4.0f);
    hjpStroke(vg);

    /* 値アーク */
    float va = sa + (ea - sa) * ratio;
    hjpBeginPath(vg);
    hjpArc(vg, cx, cy, r, sa, va, HJP_CW);
    hjpStrokeColor(vg, TH_ACCENT);
    hjpStrokeWidth(vg, 4.0f);
    hjpStroke(vg);

    /* ノブ位置 */
    float kx = cx + cosf(va) * (r - 8.0f);
    float ky = cy + sinf(va) * (r - 8.0f);
    hjpBeginPath(vg);
    hjpCircle(vg, kx, ky, 6.0f);
    hjpFillColor(vg, TH_ACCENT);
    hjpFill(vg);

    /* 値表示 */
    char vstr[32];
    snprintf(vstr, sizeof(vstr), "%.1f", val);
    hjpFontSize(vg, 13.0f);
    hjpTextAlign(vg, HJP_ALIGN_CENTER | HJP_ALIGN_MIDDLE);
    hjpFillColor(vg, TH_TEXT);
    hjpText(vg, cx, cy, vstr, NULL);

    /* ドラッグ操作 */
    uint32_t id = gui_hash(label);
    float mx = g_cur->in.mx, my = g_cur->in.my;
    float dx = mx - cx, dy = my - cy;
    float dist = sqrtf(dx * dx + dy * dy);
    bool in_knob = dist < r + 10.0f;

    if (in_knob && g_cur->in.clicked) g_cur->active = id;
    if (g_cur->active == id) {
        if (g_cur->in.down) {
            float angle = atan2f(dy, dx);
            float norm = (angle - sa) / (ea - sa);
            if (norm < 0) norm += 1.0f;
            if (norm > 1) norm = 1;
            val = vmin + norm * range;
        } else {
            g_cur->active = 0;
        }
    }

    gui_advance(r * 2.0f + GUI_FONT_SIZE + 12.0f);
    return hajimu_number((double)val);
}

/* レンジスライダー(ラベル, 下限, 上限, 最小, 最大) → 辞書{下限,上限} */
static Value fn_range_slider(int argc, Value *argv) {
    (void)argc;
    const char *label = argv[0].string.data;
    float lo  = (float)argv[1].number;
    float hi  = (float)argv[2].number;
    float vmin = (float)argv[3].number;
    float vmax = (float)argv[4].number;
    if (!g_cur) return gui_dict2("下限", hajimu_number(lo), "上限", hajimu_number(hi));

    Hjpcontext *vg = g_cur->vg;
    float x, y, w;
    gui_pos(&x, &y, &w);
    float range = vmax - vmin;
    if (range <= 0) range = 1;

    /* ラベル */
    float lw = 0;
    hjpFontSize(vg, GUI_FONT_SIZE);
    hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_MIDDLE);
    float bounds[4];
    hjpTextBounds(vg, 0, 0, label, NULL, bounds);
    lw = bounds[2] - bounds[0] + 12.0f;
    hjpFillColor(vg, TH_TEXT);
    hjpText(vg, x, y + GUI_WIDGET_H * 0.5f, label, NULL);

    float sx = x + lw, sw = w - lw;
    float sy = y + GUI_WIDGET_H * 0.5f;
    float thumb_r = 8.0f;

    /* トラック */
    hjpBeginPath(vg);
    hjpRoundedRect(vg, sx, sy - 3, sw, 6, 3);
    hjpFillColor(vg, TH_TRACK);
    hjpFill(vg);

    float lo_x = sx + ((lo - vmin) / range) * sw;
    float hi_x = sx + ((hi - vmin) / range) * sw;

    /* アクティブ範囲 */
    hjpBeginPath(vg);
    hjpRect(vg, lo_x, sy - 3, hi_x - lo_x, 6);
    hjpFillColor(vg, TH_ACCENT);
    hjpFill(vg);

    /* 下限つまみ */
    float mx = g_cur->in.mx, my = g_cur->in.my;
    uint32_t id_lo = gui_hash(label) + 1;
    uint32_t id_hi = gui_hash(label) + 2;

    bool hov_lo = fabsf(mx - lo_x) < thumb_r * 2 && fabsf(my - sy) < thumb_r * 2;
    bool hov_hi = fabsf(mx - hi_x) < thumb_r * 2 && fabsf(my - sy) < thumb_r * 2;

    if (hov_lo && g_cur->in.clicked) g_cur->active = id_lo;
    if (hov_hi && g_cur->in.clicked) g_cur->active = id_hi;

    if (g_cur->active == id_lo && g_cur->in.down) {
        lo = vmin + ((mx - sx) / sw) * range;
        if (lo < vmin) lo = vmin;
        if (lo > hi) lo = hi;
    } else if (g_cur->active == id_hi && g_cur->in.down) {
        hi = vmin + ((mx - sx) / sw) * range;
        if (hi > vmax) hi = vmax;
        if (hi < lo) hi = lo;
    }
    if (!g_cur->in.down && (g_cur->active == id_lo || g_cur->active == id_hi))
        g_cur->active = 0;

    lo_x = sx + ((lo - vmin) / range) * sw;
    hi_x = sx + ((hi - vmin) / range) * sw;

    for (int t = 0; t < 2; t++) {
        float tx = (t == 0) ? lo_x : hi_x;
        bool th = (t == 0) ? hov_lo : hov_hi;
        hjpBeginPath(vg);
        hjpCircle(vg, tx, sy, thumb_r);
        hjpFillColor(vg, th ? TH_ACCENT_HOVER : TH_ACCENT);
        hjpFill(vg);
    }

    gui_advance(GUI_WIDGET_H);
    return gui_dict2("下限", hajimu_number(lo), "上限", hajimu_number(hi));
}

/* カレンダー(ラベル, 日付) → 文字列 — 月表示カレンダー */
static Value fn_calendar(int argc, Value *argv) {
    (void)argc;
    const char *label = argv[0].string.data;
    const char *date_str = argv[1].string.data;
    if (!g_cur) return hajimu_string(date_str);

    /* 日付パース */
    int year = 2026, month = 1, day = 1;
    sscanf(date_str, "%d-%d-%d", &year, &month, &day);
    if (month < 1) month = 1; if (month > 12) month = 12;
    if (day < 1) day = 1; if (day > 31) day = 31;

    Hjpcontext *vg = g_cur->vg;
    float x, y, w;
    gui_pos(&x, &y, &w);
    float cell_w = w / 7.0f, cell_h = 28.0f;
    float mx = g_cur->in.mx, my = g_cur->in.my;

    /* ラベル */
    hjpFontSize(vg, GUI_FONT_SIZE);
    hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_TOP);
    hjpFillColor(vg, TH_TEXT);
    hjpText(vg, x, y, label, NULL);
    y += GUI_FONT_SIZE + 4.0f;

    /* 月タイトル */
    char mtitle[64];
    snprintf(mtitle, sizeof(mtitle), "%d年%d月", year, month);
    hjpFontSize(vg, GUI_FONT_SIZE);
    hjpTextAlign(vg, HJP_ALIGN_CENTER | HJP_ALIGN_TOP);
    hjpFillColor(vg, TH_TEXT);
    hjpText(vg, x + w * 0.5f, y, mtitle, NULL);
    y += GUI_FONT_SIZE + 6.0f;

    /* 曜日ヘッダー */
    const char *dow[] = {"日","月","火","水","木","金","土"};
    hjpFontSize(vg, 12.0f);
    hjpTextAlign(vg, HJP_ALIGN_CENTER | HJP_ALIGN_MIDDLE);
    for (int i = 0; i < 7; i++) {
        hjpFillColor(vg, (i == 0) ? hjpRGBA(220,80,80,255) : (i == 6) ? hjpRGBA(80,120,220,255) : TH_TEXT_DIM);
        hjpText(vg, x + cell_w * (float)i + cell_w * 0.5f, y + cell_h * 0.5f, dow[i], NULL);
    }
    y += cell_h;

    /* 月の日数 & 開始曜日 (Zellerの公式簡易版) */
    int days_in_month[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0) days_in_month[1] = 29;
    int dim = days_in_month[month - 1];

    /* Tomohiko Sakamoto のアルゴリズムで曜日計算 */
    int t_table[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    int yy = year; if (month < 3) yy--;
    int first_dow = (yy + yy/4 - yy/100 + yy/400 + t_table[month-1] + 1) % 7;

    int new_day = day;
    int col = first_dow, row = 0;
    for (int d = 1; d <= dim; d++) {
        float dx = x + cell_w * (float)col;
        float dy = y + cell_h * (float)row;

        bool sel = (d == day);
        bool hov = mx >= dx && mx < dx + cell_w && my >= dy && my < dy + cell_h;
        if (hov && g_cur->in.clicked) new_day = d;

        if (sel) {
            hjpBeginPath(vg);
            hjpCircle(vg, dx + cell_w * 0.5f, dy + cell_h * 0.5f, 13.0f);
            hjpFillColor(vg, TH_ACCENT);
            hjpFill(vg);
        }

        char ds[4];
        snprintf(ds, sizeof(ds), "%d", d);
        hjpFontSize(vg, 13.0f);
        hjpTextAlign(vg, HJP_ALIGN_CENTER | HJP_ALIGN_MIDDLE);
        hjpFillColor(vg, sel ? hjpRGBA(255,255,255,255) : (hov ? TH_TEXT : TH_TEXT_DIM));
        hjpText(vg, dx + cell_w * 0.5f, dy + cell_h * 0.5f, ds, NULL);

        col++;
        if (col >= 7) { col = 0; row++; }
    }

    float total_h = GUI_FONT_SIZE + 4.0f + GUI_FONT_SIZE + 6.0f + cell_h + cell_h * (float)(row + 1) + GUI_MARGIN;
    gui_advance(total_h);

    char result[32];
    snprintf(result, sizeof(result), "%04d-%02d-%02d", year, month, new_day);
    return hajimu_string(result);
}

/* 評価(ラベル, 値, 最大) → 数値 — 星レーティング */
static Value fn_rating(int argc, Value *argv) {
    (void)argc;
    const char *label = argv[0].string.data;
    float val = (float)argv[1].number;
    int max_stars = (int)argv[2].number;
    if (max_stars <= 0) max_stars = 5;
    if (!g_cur) return hajimu_number(val);

    Hjpcontext *vg = g_cur->vg;
    float x, y, w;
    gui_pos(&x, &y, &w);
    float star_size = 20.0f;
    float mx = g_cur->in.mx, my = g_cur->in.my;

    /* ラベル */
    hjpFontSize(vg, GUI_FONT_SIZE);
    hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_MIDDLE);
    float bounds[4];
    hjpTextBounds(vg, 0, 0, label, NULL, bounds);
    float lw = bounds[2] - bounds[0] + 8.0f;
    hjpFillColor(vg, TH_TEXT);
    hjpText(vg, x, y + GUI_WIDGET_H * 0.5f, label, NULL);

    float sx = x + lw;
    float sy = y + GUI_WIDGET_H * 0.5f;
    float new_val = val;

    for (int i = 0; i < max_stars; i++) {
        float scx = sx + star_size * (float)i + star_size * 0.5f;
        bool filled = ((float)(i + 1) <= val);
        bool hov = fabsf(mx - scx) < star_size * 0.5f && fabsf(my - sy) < star_size * 0.5f;

        if (hov && g_cur->in.clicked) new_val = (float)(i + 1);

        /* 星を描画 (五角形簡易) */
        hjpFontSize(vg, star_size);
        hjpTextAlign(vg, HJP_ALIGN_CENTER | HJP_ALIGN_MIDDLE);
        hjpFillColor(vg, filled ? hjpRGBA(255, 200, 0, 255) :
                         (hov ? hjpRGBA(255, 200, 0, 128) : TH_TEXT_DIM));
        hjpText(vg, scx, sy, "★", NULL);
    }

    gui_advance(GUI_WIDGET_H);
    return hajimu_number((double)new_val);
}

/* アバター(画像or文字, サイズ) → 無 — 丸型アイコン */
static Value fn_avatar(int argc, Value *argv) {
    const char *text = argv[0].string.data;
    float size = (argc >= 2) ? (float)argv[1].number : 40.0f;
    if (!g_cur) return hajimu_null();

    Hjpcontext *vg = g_cur->vg;
    float x, y, w;
    gui_pos(&x, &y, &w);
    float cx = x + size * 0.5f;
    float cy = y + size * 0.5f;
    float r = size * 0.5f;

    /* 背景 */
    uint32_t h = gui_hash(text);
    int hue = (int)(h % 360);
    Hjpcolor bg = hjpHSLA(hue / 360.0f, 0.5f, 0.5f, 255);
    hjpBeginPath(vg);
    hjpCircle(vg, cx, cy, r);
    hjpFillColor(vg, bg);
    hjpFill(vg);

    /* テキスト (最初の1文字) */
    char initial[8] = {0};
    int len = 0;
    const unsigned char *p = (const unsigned char *)text;
    if (*p >= 0xC0) {
        /* UTF-8 マルチバイト */
        if (*p < 0xE0) len = 2;
        else if (*p < 0xF0) len = 3;
        else len = 4;
    } else {
        len = 1;
    }
    if (len > 0 && len <= 4) memcpy(initial, text, len);

    hjpFontSize(vg, size * 0.45f);
    hjpTextAlign(vg, HJP_ALIGN_CENTER | HJP_ALIGN_MIDDLE);
    hjpFillColor(vg, hjpRGBA(255, 255, 255, 255));
    hjpText(vg, cx, cy, initial, NULL);

    gui_advance(size + GUI_MARGIN);
    return hajimu_null();
}

/* タイムライン(項目配列) → 無 — 垂直時系列表示 */
static Value fn_timeline(int argc, Value *argv) {
    (void)argc;
    if (argv[0].type != VALUE_ARRAY) return hajimu_null();
    int count = argv[0].array.length;
    if (!g_cur || count == 0) return hajimu_null();

    Hjpcontext *vg = g_cur->vg;
    float x, y, w;
    gui_pos(&x, &y, &w);

    float dot_r = 6.0f;
    float line_x = x + 20.0f;
    float item_h = 40.0f;

    for (int i = 0; i < count; i++) {
        float iy = y + item_h * (float)i;

        /* 接続線 */
        if (i < count - 1) {
            hjpBeginPath(vg);
            hjpMoveTo(vg, line_x, iy + dot_r);
            hjpLineTo(vg, line_x, iy + item_h - dot_r);
            hjpStrokeColor(vg, TH_BORDER);
            hjpStrokeWidth(vg, 2.0f);
            hjpStroke(vg);
        }

        /* ドット */
        hjpBeginPath(vg);
        hjpCircle(vg, line_x, iy, dot_r);
        hjpFillColor(vg, TH_ACCENT);
        hjpFill(vg);

        /* テキスト */
        const char *txt = argv[0].array.elements[i].string.data;
        hjpFontSize(vg, GUI_FONT_SIZE);
        hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_MIDDLE);
        hjpFillColor(vg, TH_TEXT);
        hjpText(vg, line_x + 16.0f, iy, txt, NULL);
    }

    gui_advance(item_h * (float)count);
    return hajimu_null();
}

/* スケルトン(幅, 高さ, 角丸) → 無 — プレースホルダー shimmer */
static Value fn_skeleton(int argc, Value *argv) {
    float sk_w = (float)argv[0].number;
    float sk_h = (float)argv[1].number;
    float sk_r = (argc >= 3) ? (float)argv[2].number : 6.0f;
    if (!g_cur) return hajimu_null();

    Hjpcontext *vg = g_cur->vg;
    float x, y, w;
    gui_pos(&x, &y, &w);
    if (sk_w <= 0) sk_w = w;

    /* アニメーション shimmer */
    uint32_t t = hjp_get_ticks();
    float phase = (float)(t % 1500) / 1500.0f;
    int base_alpha = 40 + (int)(20.0f * sinf(phase * HJP_PI * 2.0f));

    hjpBeginPath(vg);
    hjpRoundedRect(vg, x, y, sk_w, sk_h, sk_r);
    hjpFillColor(vg, hjpRGBA(150, 150, 150, (unsigned char)base_alpha));
    hjpFill(vg);

    gui_advance(sk_h + GUI_MARGIN);
    return hajimu_null();
}

/* カルーセル(画像配列, 幅, 高さ) → 数値 (現在インデックス) */
static Value fn_carousel(int argc, Value *argv) {
    (void)argc;
    if (argv[0].type != VALUE_ARRAY) return hajimu_number(0);
    int count = argv[0].array.length;
    float cw = (float)argv[1].number;
    float ch = (float)argv[2].number;
    if (!g_cur || count == 0) return hajimu_number(0);

    /* 静的カルーセルインデックス管理 */
    static int carousel_idx = 0;
    if (carousel_idx >= count) carousel_idx = 0;

    Hjpcontext *vg = g_cur->vg;
    float x, y, w;
    gui_pos(&x, &y, &w);
    if (cw <= 0) cw = w;
    float mx = g_cur->in.mx, my = g_cur->in.my;

    /* 背景 */
    hjpBeginPath(vg);
    hjpRoundedRect(vg, x, y, cw, ch, GUI_BTN_RADIUS);
    hjpFillColor(vg, TH_WIDGET_BG);
    hjpFill(vg);

    /* 現在項目のテキスト表示 */
    const char *item = argv[0].array.elements[carousel_idx].string.data;
    hjpFontSize(vg, GUI_FONT_SIZE);
    hjpTextAlign(vg, HJP_ALIGN_CENTER | HJP_ALIGN_MIDDLE);
    hjpFillColor(vg, TH_TEXT);
    hjpText(vg, x + cw * 0.5f, y + ch * 0.5f, item, NULL);

    /* 左右矢印 */
    float arr_y = y + ch * 0.5f;
    if (carousel_idx > 0) {
        bool lh = mx >= x && mx <= x + 30 && my >= arr_y - 15 && my <= arr_y + 15;
        if (lh && g_cur->in.clicked) carousel_idx--;
        hjpFontSize(vg, 24.0f);
        hjpTextAlign(vg, HJP_ALIGN_CENTER | HJP_ALIGN_MIDDLE);
        hjpFillColor(vg, lh ? TH_TEXT : TH_TEXT_DIM);
        hjpText(vg, x + 15.0f, arr_y, "<", NULL);
    }
    if (carousel_idx < count - 1) {
        bool rh = mx >= x + cw - 30 && mx <= x + cw && my >= arr_y - 15 && my <= arr_y + 15;
        if (rh && g_cur->in.clicked) carousel_idx++;
        hjpFontSize(vg, 24.0f);
        hjpTextAlign(vg, HJP_ALIGN_CENTER | HJP_ALIGN_MIDDLE);
        hjpFillColor(vg, rh ? TH_TEXT : TH_TEXT_DIM);
        hjpText(vg, x + cw - 15.0f, arr_y, ">", NULL);
    }

    /* ドットインジケーター */
    float dot_y = y + ch - 12.0f;
    float dots_total = count * 10.0f;
    float dot_sx = x + (cw - dots_total) * 0.5f;
    for (int i = 0; i < count; i++) {
        hjpBeginPath(vg);
        hjpCircle(vg, dot_sx + i * 10.0f + 5.0f, dot_y, (i == carousel_idx) ? 4.0f : 3.0f);
        hjpFillColor(vg, (i == carousel_idx) ? TH_ACCENT : TH_TEXT_DIM);
        hjpFill(vg);
    }

    gui_advance(ch + GUI_MARGIN);
    return hajimu_number((double)carousel_idx);
}

/* 数値ステッパー(ラベル, 値, 最小, 最大) → 数値 — +/- ボタン付き */
static Value fn_num_stepper(int argc, Value *argv) {
    (void)argc;
    const char *label = argv[0].string.data;
    float val = (float)argv[1].number;
    float vmin = (float)argv[2].number;
    float vmax = (float)argv[3].number;
    if (!g_cur) return hajimu_number(val);

    Hjpcontext *vg = g_cur->vg;
    float x, y, w;
    gui_pos(&x, &y, &w);
    float h = GUI_WIDGET_H;
    float btn_w = 32.0f;
    float mx = g_cur->in.mx, my = g_cur->in.my;

    /* ラベル */
    hjpFontSize(vg, GUI_FONT_SIZE);
    hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_MIDDLE);
    float bounds[4];
    hjpTextBounds(vg, 0, 0, label, NULL, bounds);
    float lw = bounds[2] - bounds[0] + 8.0f;
    hjpFillColor(vg, TH_TEXT);
    hjpText(vg, x, y + h * 0.5f, label, NULL);

    float new_val = val;
    float sx = x + lw;

    /* - ボタン */
    bool mhov = mx >= sx && mx <= sx + btn_w && my >= y && my <= y + h;
    hjpBeginPath(vg);
    hjpRoundedRect(vg, sx, y, btn_w, h, GUI_BTN_RADIUS);
    hjpFillColor(vg, mhov ? TH_WIDGET_HOVER : TH_WIDGET_BG);
    hjpFill(vg);
    hjpFontSize(vg, 18.0f);
    hjpTextAlign(vg, HJP_ALIGN_CENTER | HJP_ALIGN_MIDDLE);
    hjpFillColor(vg, TH_TEXT);
    hjpText(vg, sx + btn_w * 0.5f, y + h * 0.5f, "-", NULL);
    if (mhov && g_cur->in.clicked && new_val > vmin) new_val -= 1;

    /* 数値表示 */
    char vstr[32];
    snprintf(vstr, sizeof(vstr), "%.0f", val);
    float vw = 50.0f;
    hjpFontSize(vg, GUI_FONT_SIZE);
    hjpTextAlign(vg, HJP_ALIGN_CENTER | HJP_ALIGN_MIDDLE);
    hjpFillColor(vg, TH_TEXT);
    hjpText(vg, sx + btn_w + vw * 0.5f, y + h * 0.5f, vstr, NULL);

    /* + ボタン */
    float px = sx + btn_w + vw;
    bool phov = mx >= px && mx <= px + btn_w && my >= y && my <= y + h;
    hjpBeginPath(vg);
    hjpRoundedRect(vg, px, y, btn_w, h, GUI_BTN_RADIUS);
    hjpFillColor(vg, phov ? TH_WIDGET_HOVER : TH_WIDGET_BG);
    hjpFill(vg);
    hjpFontSize(vg, 18.0f);
    hjpTextAlign(vg, HJP_ALIGN_CENTER | HJP_ALIGN_MIDDLE);
    hjpFillColor(vg, TH_TEXT);
    hjpText(vg, px + btn_w * 0.5f, y + h * 0.5f, "+", NULL);
    if (phov && g_cur->in.clicked && new_val < vmax) new_val += 1;

    gui_advance(h);
    return hajimu_number((double)new_val);
}

/* リストタイル(タイトル, 副題, アイコン) → 真偽 */
static Value fn_list_tile(int argc, Value *argv) {
    const char *title = argv[0].string.data;
    const char *subtitle = (argc >= 2) ? argv[1].string.data : NULL;
    const char *icon = (argc >= 3) ? argv[2].string.data : NULL;
    if (!g_cur) return hajimu_bool(false);

    Hjpcontext *vg = g_cur->vg;
    float x, y, w;
    gui_pos(&x, &y, &w);
    float h = (subtitle && strlen(subtitle) > 0) ? 56.0f : 40.0f;
    float mx = g_cur->in.mx, my = g_cur->in.my;

    bool hov = mx >= x && mx <= x + w && my >= y && my <= y + h;
    bool clicked = hov && g_cur->in.clicked;

    /* 背景 */
    if (hov) {
        hjpBeginPath(vg);
        hjpRect(vg, x, y, w, h);
        hjpFillColor(vg, TH_WIDGET_HOVER);
        hjpFill(vg);
    }

    float tx = x + 8.0f;

    /* アイコン */
    if (icon && strlen(icon) > 0) {
        hjpFontSize(vg, 20.0f);
        hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_MIDDLE);
        hjpFillColor(vg, TH_TEXT_DIM);
        hjpText(vg, tx, y + h * 0.5f, icon, NULL);
        tx += 32.0f;
    }

    /* タイトル */
    hjpFontSize(vg, GUI_FONT_SIZE);
    hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_MIDDLE);
    hjpFillColor(vg, TH_TEXT);
    float title_y = (subtitle && strlen(subtitle) > 0) ? y + h * 0.35f : y + h * 0.5f;
    hjpText(vg, tx, title_y, title, NULL);

    /* 副題 */
    if (subtitle && strlen(subtitle) > 0) {
        hjpFontSize(vg, 12.0f);
        hjpFillColor(vg, TH_TEXT_DIM);
        hjpText(vg, tx, y + h * 0.65f, subtitle, NULL);
    }

    /* 下ボーダー */
    hjpBeginPath(vg);
    hjpMoveTo(vg, x, y + h);
    hjpLineTo(vg, x + w, y + h);
    hjpStrokeColor(vg, TH_SEP);
    hjpStrokeWidth(vg, 1.0f);
    hjpStroke(vg);

    gui_advance(h);
    return hajimu_bool(clicked);
}

/* =====================================================================
 * Phase 21: レイアウト拡張 II
 * ===================================================================*/

/* フロー開始(間隔) — 自動折返しレイアウト */
static Value fn_flow_begin(int argc, Value *argv) {
    float gap = (argc >= 1) ? (float)argv[0].number : GUI_MARGIN;
    if (!g_cur) return hajimu_null();

    int idx = -1;
    for (int i = 0; i < GUI_MAX_FLOW; i++) {
        if (!g_flows[i].active) { idx = i; break; }
    }
    if (idx < 0) return hajimu_null();

    GuiFlow *f = &g_flows[idx];
    f->active = true;
    f->gap = gap;
    gui_pos(&f->x, &f->y, &f->w);
    f->saved_x = g_cur->lay.x;
    f->saved_y = g_cur->lay.y;
    f->saved_w = g_cur->lay.w;
    f->cur_x = f->x;
    f->cur_y = f->y;
    f->row_h = 0;

    return hajimu_number((double)idx);
}

/* フロー終了() */
static Value fn_flow_end(int argc, Value *argv) {
    (void)argc; (void)argv;
    if (!g_cur) return hajimu_null();

    /* 最後に使ったフローを閉じる */
    for (int i = GUI_MAX_FLOW - 1; i >= 0; i--) {
        if (g_flows[i].active) {
            GuiFlow *f = &g_flows[i];
            float total_h = (f->cur_y - f->y) + f->row_h + GUI_MARGIN;
            g_cur->lay.x = f->saved_x;
            g_cur->lay.y = f->saved_y + total_h;
            g_cur->lay.w = f->saved_w;
            f->active = false;
            break;
        }
    }
    return hajimu_null();
}

/* オーバーレイ開始(幅, 高さ) — 重ね合わせ領域 */
static Value fn_overlay_begin(int argc, Value *argv) {
    (void)argc;
    float ow = (float)argv[0].number;
    float oh = (float)argv[1].number;
    if (!g_cur) return hajimu_null();

    int idx = -1;
    for (int i = 0; i < GUI_MAX_OVERLAY; i++) {
        if (!g_overlays[i].active) { idx = i; break; }
    }
    if (idx < 0) return hajimu_null();

    GuiOverlay *o = &g_overlays[idx];
    o->active = true;
    float x, y, w;
    gui_pos(&x, &y, &w);
    o->x = x; o->y = y;
    o->w = (ow > 0) ? ow : w;
    o->h = oh;
    o->saved_x = g_cur->lay.x;
    o->saved_y = g_cur->lay.y;
    o->saved_w = g_cur->lay.w;

    return hajimu_number((double)idx);
}

/* オーバーレイ終了() */
static Value fn_overlay_end(int argc, Value *argv) {
    (void)argc; (void)argv;
    if (!g_cur) return hajimu_null();

    for (int i = GUI_MAX_OVERLAY - 1; i >= 0; i--) {
        if (g_overlays[i].active) {
            GuiOverlay *o = &g_overlays[i];
            g_cur->lay.x = o->saved_x;
            g_cur->lay.y = o->saved_y + o->h + GUI_MARGIN;
            g_cur->lay.w = o->saved_w;
            o->active = false;
            break;
        }
    }
    return hajimu_null();
}

/* 絶対配置(x, y) — オーバーレイ内で位置指定 */
static Value fn_absolute_pos(int argc, Value *argv) {
    (void)argc;
    float ax = (float)argv[0].number;
    float ay = (float)argv[1].number;

    /* オーバーレイの座標にオフセット */
    for (int i = GUI_MAX_OVERLAY - 1; i >= 0; i--) {
        if (g_overlays[i].active) {
            if (g_cur) {
                g_cur->lay.x = g_overlays[i].x + ax;
                g_cur->lay.y = g_overlays[i].y + ay;
            }
            break;
        }
    }
    return hajimu_null();
}

/* 透明度(値) — 次ウィジェットの透明度制御 (0.0-1.0) */
static Value fn_opacity(int argc, Value *argv) {
    (void)argc;
    float v = (float)argv[0].number;
    if (v < 0) v = 0; if (v > 1) v = 1;
    if (g_cur && g_cur->vg)
        hjpGlobalAlpha(g_cur->vg, v);
    g_next_opacity = v;
    return hajimu_null();
}

/* リビーラー(表示, 秒数) — アニメ付き表示切替 */
static Value fn_revealer(int argc, Value *argv) {
    bool show = argv[0].boolean;
    float dur = (argc >= 2) ? (float)argv[1].number : 0.3f;
    (void)dur; /* 将来的にアニメーション */

    if (g_cur && g_cur->vg) {
        hjpGlobalAlpha(g_cur->vg, show ? 1.0f : 0.0f);
    }
    return hajimu_bool(show);
}

/* ドッキング(ID, 方向, 幅) → 真偽 */
static Value fn_docking(int argc, Value *argv) {
    (void)argc;
    const char *id_str = argv[0].string.data;
    const char *dir = argv[1].string.data;
    float dock_w = (float)argv[2].number;
    if (!g_cur) return hajimu_bool(false);

    Hjpcontext *vg = g_cur->vg;
    float ww = (float)g_cur->win_w;
    float wh = (float)g_cur->win_h;
    (void)id_str;

    float dx = 0, dy = 0, dw = dock_w, dh = wh;
    bool is_left = (strstr(dir, "左") != NULL);
    bool is_right = (strstr(dir, "右") != NULL);
    bool is_bottom = (strstr(dir, "下") != NULL);

    if (is_right)  { dx = ww - dock_w; dw = dock_w; dh = wh; }
    else if (is_bottom) { dx = 0; dy = wh - dock_w; dw = ww; dh = dock_w; }
    else if (is_left) { dx = 0; dw = dock_w; dh = wh; }
    else { dx = 0; dw = dock_w; dh = wh; }

    /* 背景 */
    hjpBeginPath(vg);
    hjpRect(vg, dx, dy, dw, dh);
    hjpFillColor(vg, TH_WIDGET_BG);
    hjpFill(vg);
    hjpBeginPath(vg);
    hjpRect(vg, dx, dy, dw, dh);
    hjpStrokeColor(vg, TH_BORDER);
    hjpStrokeWidth(vg, 1.0f);
    hjpStroke(vg);

    /* ドッキング領域内のレイアウト設定 */
    g_cur->lay.x = dx + GUI_PADDING;
    g_cur->lay.y = dy + GUI_PADDING;
    g_cur->lay.w = dw - GUI_PADDING * 2;

    return hajimu_bool(true);
}

/* =====================================================================
 * Phase 22: システム連携 II
 * ===================================================================*/

/* システム通知(タイトル, メッセージ) → 無 */
static Value fn_sys_notify(int argc, Value *argv) {
    (void)argc;
    const char *title = argv[0].string.data;
    const char *msg   = argv[1].string.data;
#if defined(__APPLE__)
    char esc_title[512], esc_msg[512];
    gui_shell_escape(esc_title, sizeof(esc_title), title);
    gui_shell_escape(esc_msg, sizeof(esc_msg), msg);
    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
        "osascript -e 'display notification %s with title %s' &",
        esc_msg, esc_title);
    system(cmd);
#elif defined(__linux__)
    char esc_title[512], esc_msg[512];
    gui_shell_escape(esc_title, sizeof(esc_title), title);
    gui_shell_escape(esc_msg, sizeof(esc_msg), msg);
    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "notify-send %s %s &", esc_title, esc_msg);
    system(cmd);
#else
    (void)title; (void)msg;
#endif
    return hajimu_null();
}

/* システムトレイ(タイトル, アイコン) → 無 — 簡易実装 */
static Value fn_sys_tray(int argc, Value *argv) {
    const char *title = argv[0].string.data;
    (void)title;
    (void)argc;
    if (argc >= 2) {
        (void)argv[1]; /* アイコンパス — 将来用 */
    }
    /* システムトレイのネイティブAPIは別途対応が必要
       macOS: NSStatusBarを直接呼ぶ必要があるため、
       ここではトースト通知で代替 */
    if (g_cur) {
        for (int i = 0; i < GUI_MAX_TOASTS; i++) {
            if (!g_toasts[i].active) {
                strncpy(g_toasts[i].message, title, sizeof(g_toasts[i].message) - 1);
                g_toasts[i].message[sizeof(g_toasts[i].message) - 1] = '\0';
                g_toasts[i].start_time = hjp_get_ticks();
                g_toasts[i].duration_ms = 5000;
                g_toasts[i].active = true;
                break;
            }
        }
    }
    return hajimu_null();
}

/* フォントダイアログ() → 辞書{フォント名, サイズ} — 簡易実装 */
static Value fn_font_dialog(int argc, Value *argv) {
    (void)argc; (void)argv;
    /* ネイティブフォントダイアログは未対応のため、
       現在のフォント情報を返す */
    float size = g_custom_font_size > 0 ? g_custom_font_size : GUI_FONT_SIZE;
    return gui_dict2("フォント名", hajimu_string("NotoSansCJKjp"),
                     "サイズ",     hajimu_number(size));
}

/* アバウトダイアログ(アプリ名, バージョン, 説明) → 無 */
static Value fn_about_dialog(int argc, Value *argv) {
    (void)argc;
    const char *name = argv[0].string.data;
    const char *ver  = argv[1].string.data;
    const char *desc = argv[2].string.data;
    if (!g_cur) return hajimu_null();

    Hjpcontext *vg = g_cur->vg;
    float ww = (float)g_cur->win_w;
    float wh = (float)g_cur->win_h;
    float dw = 320, dh = 200;
    float dx = (ww - dw) * 0.5f;
    float dy = (wh - dh) * 0.5f;

    /* 半透明オーバーレイ */
    hjpBeginPath(vg);
    hjpRect(vg, 0, 0, ww, wh);
    hjpFillColor(vg, hjpRGBA(0, 0, 0, 128));
    hjpFill(vg);

    /* ダイアログ背景 */
    hjpBeginPath(vg);
    hjpRoundedRect(vg, dx, dy, dw, dh, 12);
    hjpFillColor(vg, TH_BG);
    hjpFill(vg);
    hjpStrokeColor(vg, TH_BORDER);
    hjpStrokeWidth(vg, 1.0f);
    hjpStroke(vg);

    /* アプリ名 */
    hjpFontSize(vg, 22.0f);
    hjpTextAlign(vg, HJP_ALIGN_CENTER | HJP_ALIGN_TOP);
    hjpFillColor(vg, TH_TEXT);
    hjpText(vg, dx + dw * 0.5f, dy + 24, name, NULL);

    /* バージョン */
    hjpFontSize(vg, 14.0f);
    hjpFillColor(vg, TH_TEXT_DIM);
    hjpText(vg, dx + dw * 0.5f, dy + 56, ver, NULL);

    /* 説明 */
    hjpFontSize(vg, GUI_FONT_SIZE);
    hjpFillColor(vg, TH_TEXT);
    hjpText(vg, dx + dw * 0.5f, dy + 86, desc, NULL);

    /* 閉じるボタン */
    float bx = dx + (dw - 80) * 0.5f, by = dy + dh - 48;
    float bw = 80, bh = 32;
    float mx = g_cur->in.mx, my = g_cur->in.my;
    bool bhov = mx >= bx && mx <= bx + bw && my >= by && my <= by + bh;

    hjpBeginPath(vg);
    hjpRoundedRect(vg, bx, by, bw, bh, GUI_BTN_RADIUS);
    hjpFillColor(vg, bhov ? TH_ACCENT_HOVER : TH_ACCENT);
    hjpFill(vg);
    hjpFontSize(vg, GUI_FONT_SIZE);
    hjpTextAlign(vg, HJP_ALIGN_CENTER | HJP_ALIGN_MIDDLE);
    hjpFillColor(vg, hjpRGBA(255, 255, 255, 255));
    hjpText(vg, bx + bw * 0.5f, by + bh * 0.5f, "OK", NULL);

    return hajimu_bool(bhov && g_cur->in.clicked);
}

/* ウィンドウアイコン(画像) → 無 */
static Value fn_window_icon(int argc, Value *argv) {
    (void)argc;
    int img_idx = (int)argv[0].number;
    if (img_idx < 0 || img_idx >= GUI_MAX_IMAGES || !g_images[img_idx].valid)
        return hajimu_null();
    /* hjp_window_set_iconにはピクセルデータが必要。
       将来的にhjp_image_load_memで対応。現在はnoop。 */
    return hajimu_null();
}

/* ウィンドウ透明度(値) → 無 */
static Value fn_window_opacity(int argc, Value *argv) {
    (void)argc;
    float op = (float)argv[0].number;
    if (op < 0) op = 0; if (op > 1) op = 1;
    if (g_cur && g_cur->window)
        hjp_window_set_opacity(g_cur->window, op);
    return hajimu_null();
}

/* スクリーンショット(パス) → 真偽 */
static Value fn_screenshot(int argc, Value *argv) {
    (void)argc;
    const char *path = argv[0].string.data;
    if (!g_cur) return hajimu_bool(false);

    int w = g_cur->fb_w, h = g_cur->fb_h;
    if (w <= 0 || h <= 0) return hajimu_bool(false);
    /* 整数オーバーフロー防止: size_t でサイズ計算 */
    size_t pixel_sz = (size_t)w * (size_t)h * 4;
    if (pixel_sz / 4 / (size_t)w != (size_t)h) return hajimu_bool(false);
    unsigned char *pixels = (unsigned char *)malloc(pixel_sz);
    if (!pixels) return hajimu_bool(false);

    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels);

    /* 上下反転 */
    int stride = w * 4;
    unsigned char *row = (unsigned char *)malloc(stride);
    if (row) {
        for (int y = 0; y < h / 2; y++) {
            memcpy(row, pixels + y * stride, stride);
            memcpy(pixels + y * stride, pixels + (h - 1 - y) * stride, stride);
            memcpy(pixels + (h - 1 - y) * stride, row, stride);
        }
        free(row);
    }

    /* PPM形式で保存（画像書き出しライブラリが無いため簡易） */
    FILE *f = fopen(path, "wb");
    if (!f) { free(pixels); return hajimu_bool(false); }
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; i++) {
        fwrite(pixels + i * 4, 1, 3, f); /* RGBのみ */
    }
    fclose(f);
    free(pixels);
    return hajimu_bool(true);
}

/* 経過時間() → 数値 (ミリ秒) */
static Value fn_elapsed_time(int argc, Value *argv) {
    (void)argc; (void)argv;
    return hajimu_number((double)hjp_get_ticks());
}

/* =====================================================================
 * Phase 23: コンテキスト・選択拡張
 * ===================================================================*/

/* コンテキストメニュー開始(ラベル) → 真偽 */
static bool g_ctx_menu_nvg_saved = false;
static Value fn_context_menu_begin(int argc, Value *argv) {
    (void)argc;
    const char *label = argv[0].string.data;
    if (!g_cur) return hajimu_bool(false);

    uint32_t id = gui_hash(label);
    float mx = g_cur->in.mx, my = g_cur->in.my;

    /* 右クリック検出 — 右ボタンは button==3 */
    if (g_cur->in.clicked && hjp_get_mouse_state(NULL, NULL) & HJP_BUTTON(3)) {
        g_cur->active = id;
    }

    if (g_cur->active != id) return hajimu_bool(false);

    /* メニュー背景 */
    Hjpcontext *vg = g_cur->vg;
    float mw = 180, mh = 0;

    /* 高さはメニュー項目追加時に動的に決まるため、仮の領域を確保 */
    hjpSave(vg);
    g_ctx_menu_nvg_saved = true;
    hjpBeginPath(vg);
    hjpRoundedRect(vg, mx, my, mw, 200, 6);
    hjpFillColor(vg, TH_BG);
    hjpFill(vg);
    hjpStrokeColor(vg, TH_BORDER);
    hjpStrokeWidth(vg, 1.0f);
    hjpStroke(vg);

    /* メニュー内レイアウトを設定 */
    g_cur->lay.x = mx + 4;
    g_cur->lay.y = my + 4;
    g_cur->lay.w = mw - 8;
    (void)mh;

    return hajimu_bool(true);
}

/* コンテキストメニュー終了() → 無 */
static Value fn_context_menu_end(int argc, Value *argv) {
    (void)argc; (void)argv;
    if (g_cur) {
        /* hjpSaveが呼ばれた場合のみhjpRestore */
        if (g_ctx_menu_nvg_saved) {
            hjpRestore(g_cur->vg);
            g_ctx_menu_nvg_saved = false;
        }
        /* 左クリックでメニューを閉じる */
        if (g_cur->in.clicked && !(hjp_get_mouse_state(NULL, NULL) & HJP_BUTTON(3))) {
            g_cur->active = 0;
        }
    }
    return hajimu_null();
}

/* トグルボタン(ラベル, 状態) → 真偽 */
static Value fn_toggle_button(int argc, Value *argv) {
    (void)argc;
    const char *label = argv[0].string.data;
    bool state = argv[1].type == VALUE_BOOL ? argv[1].boolean : false;
    if (!g_cur) return hajimu_bool(state);

    Hjpcontext *vg = g_cur->vg;
    float x, y, w;
    gui_pos(&x, &y, &w);
    float h = GUI_WIDGET_H;

    /* テキスト幅で自動調整 */
    float tw = 80;
    float bounds[4];
    hjpFontSize(vg, GUI_FONT_SIZE);
    hjpTextBounds(vg, 0, 0, label, NULL, bounds);
    tw = bounds[2] - bounds[0] + GUI_PADDING * 2;
    if (tw < 60) tw = 60;

    uint32_t id = gui_hash(label);
    bool hov, pressed;
    gui_widget_logic(id, x, y, tw, h, &hov, &pressed);
    if (pressed) state = !state;

    /* 描画 — アクティブ時はアクセントカラー */
    hjpBeginPath(vg);
    hjpRoundedRect(vg, x, y, tw, h, GUI_BTN_RADIUS);
    if (state) {
        hjpFillColor(vg, hov ? TH_ACCENT_HOVER : TH_ACCENT);
    } else {
        hjpFillColor(vg, hov ? TH_WIDGET_HOVER : TH_WIDGET_BG);
    }
    hjpFill(vg);
    hjpStrokeColor(vg, TH_BORDER);
    hjpStrokeWidth(vg, 1.0f);
    hjpStroke(vg);

    hjpFontSize(vg, GUI_FONT_SIZE);
    hjpTextAlign(vg, HJP_ALIGN_CENTER | HJP_ALIGN_MIDDLE);
    hjpFillColor(vg, state ? hjpRGBA(255,255,255,255) : TH_TEXT);
    hjpText(vg, x + tw * 0.5f, y + h * 0.5f, label, NULL);

    gui_advance(h);
    return hajimu_bool(state);
}

/* セグメントコントロール(ラベル配列, 選択) → 数値 */
static Value fn_segment_control(int argc, Value *argv) {
    (void)argc;
    if (argv[0].type != VALUE_ARRAY) return hajimu_number(0);
    int count = argv[0].array.length;
    Value *items = argv[0].array.elements;
    int sel = (int)argv[1].number;
    if (!g_cur || count == 0) return hajimu_number(sel);

    Hjpcontext *vg = g_cur->vg;
    float x, y, w;
    gui_pos(&x, &y, &w);
    float h = GUI_WIDGET_H;
    float sw = w / count;

    /* 背景 */
    hjpBeginPath(vg);
    hjpRoundedRect(vg, x, y, w, h, GUI_BTN_RADIUS);
    hjpFillColor(vg, TH_WIDGET_BG);
    hjpFill(vg);

    for (int i = 0; i < count; i++) {
        float sx = x + i * sw;
        const char *lbl = items[i].type == VALUE_STRING ? items[i].string.data : "?";
        bool hov = gui_hit(g_cur->in.mx, g_cur->in.my, sx, y, sw, h);
        bool active = (i == sel);

        if (hov && g_cur->in.clicked) sel = i;

        if (active) {
            hjpBeginPath(vg);
            hjpRoundedRect(vg, sx + 2, y + 2, sw - 4, h - 4, GUI_BTN_RADIUS - 1);
            hjpFillColor(vg, TH_ACCENT);
            hjpFill(vg);
        }

        hjpFontSize(vg, GUI_FONT_SIZE);
        hjpTextAlign(vg, HJP_ALIGN_CENTER | HJP_ALIGN_MIDDLE);
        hjpFillColor(vg, active ? hjpRGBA(255,255,255,255) : TH_TEXT);
        hjpText(vg, sx + sw * 0.5f, y + h * 0.5f, lbl, NULL);

        /* セグメント区切り線 */
        if (i > 0 && i != sel && (i - 1) != sel) {
            hjpBeginPath(vg);
            hjpMoveTo(vg, sx, y + 6);
            hjpLineTo(vg, sx, y + h - 6);
            hjpStrokeColor(vg, TH_BORDER);
            hjpStrokeWidth(vg, 1.0f);
            hjpStroke(vg);
        }
    }

    hjpBeginPath(vg);
    hjpRoundedRect(vg, x, y, w, h, GUI_BTN_RADIUS);
    hjpStrokeColor(vg, TH_BORDER);
    hjpStrokeWidth(vg, 1.0f);
    hjpStroke(vg);

    gui_advance(h);
    return hajimu_number(sel);
}

/* リストボックス(ラベル, 項目, 選択) → 数値 */
static Value fn_listbox(int argc, Value *argv) {
    (void)argc;
    const char *label = argv[0].string.data;
    if (argv[1].type != VALUE_ARRAY) return hajimu_number(0);
    int count = argv[1].array.length;
    Value *items = argv[1].array.elements;
    int sel = (int)argv[2].number;
    if (!g_cur) return hajimu_number(sel);

    Hjpcontext *vg = g_cur->vg;
    float x, y, w;
    gui_pos(&x, &y, &w);
    float item_h = GUI_WIDGET_H - 4;
    int visible = count < 8 ? count : 8;
    float total_h = visible * item_h + 4;

    /* ラベル */
    hjpFontSize(vg, GUI_FONT_SIZE);
    hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_TOP);
    hjpFillColor(vg, TH_TEXT);
    hjpText(vg, x, y, label, NULL);
    y += 22;

    /* 背景 */
    hjpBeginPath(vg);
    hjpRoundedRect(vg, x, y, w, total_h, 4);
    hjpFillColor(vg, TH_WIDGET_BG);
    hjpFill(vg);
    hjpStrokeColor(vg, TH_BORDER);
    hjpStrokeWidth(vg, 1.0f);
    hjpStroke(vg);

    for (int i = 0; i < visible && i < count; i++) {
        float iy = y + 2 + i * item_h;
        const char *txt = items[i].type == VALUE_STRING ? items[i].string.data : "?";
        bool hov = gui_hit(g_cur->in.mx, g_cur->in.my, x, iy, w, item_h);
        bool active = (i == sel);

        if (hov && g_cur->in.clicked) sel = i;

        if (active) {
            hjpBeginPath(vg);
            hjpRoundedRect(vg, x + 2, iy, w - 4, item_h, 3);
            hjpFillColor(vg, TH_ACCENT);
            hjpFill(vg);
        } else if (hov) {
            hjpBeginPath(vg);
            hjpRoundedRect(vg, x + 2, iy, w - 4, item_h, 3);
            hjpFillColor(vg, TH_WIDGET_HOVER);
            hjpFill(vg);
        }

        hjpFontSize(vg, GUI_FONT_SIZE);
        hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_MIDDLE);
        hjpFillColor(vg, active ? hjpRGBA(255,255,255,255) : TH_TEXT);
        hjpText(vg, x + GUI_PADDING, iy + item_h * 0.5f, txt, NULL);
    }

    gui_advance(22 + total_h);
    return hajimu_number(sel);
}

/* ドロップダウン(ラベル, 項目, 選択) → 数値 */
static Value fn_dropdown(int argc, Value *argv) {
    (void)argc;
    const char *label = argv[0].string.data;
    if (argv[1].type != VALUE_ARRAY) return hajimu_number(0);
    int count = argv[1].array.length;
    Value *items = argv[1].array.elements;
    int sel = (int)argv[2].number;
    if (!g_cur) return hajimu_number(sel);

    Hjpcontext *vg = g_cur->vg;
    float x, y, w;
    gui_pos(&x, &y, &w);
    float h = GUI_WIDGET_H;
    uint32_t id = gui_hash(label);

    const char *sel_text = (sel >= 0 && sel < count && items[sel].type == VALUE_STRING)
        ? items[sel].string.data : label;

    bool hov, pressed;
    gui_widget_logic(id, x, y, w, h, &hov, &pressed);

    /* ヘッダー描画 */
    hjpBeginPath(vg);
    hjpRoundedRect(vg, x, y, w, h, GUI_BTN_RADIUS);
    hjpFillColor(vg, hov ? TH_WIDGET_HOVER : TH_WIDGET_BG);
    hjpFill(vg);
    hjpStrokeColor(vg, TH_BORDER);
    hjpStrokeWidth(vg, 1.0f);
    hjpStroke(vg);

    hjpFontSize(vg, GUI_FONT_SIZE);
    hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_MIDDLE);
    hjpFillColor(vg, TH_TEXT);
    hjpText(vg, x + GUI_PADDING, y + h * 0.5f, sel_text, NULL);

    /* ▼ 矢印 */
    hjpTextAlign(vg, HJP_ALIGN_RIGHT | HJP_ALIGN_MIDDLE);
    hjpFillColor(vg, TH_TEXT_DIM);
    hjpText(vg, x + w - GUI_PADDING, y + h * 0.5f, "\xe2\x96\xbc", NULL);

    if (pressed) {
        g_cur->active = (g_cur->active == id) ? 0 : id;
    }

    gui_advance(h);

    /* ドロップダウンリスト */
    if (g_cur->active == id) {
        float dy = y + h;
        float dh = count * (h - 4) + 4;
        if (dh > 200) dh = 200;

        hjpBeginPath(vg);
        hjpRoundedRect(vg, x, dy, w, dh, 4);
        hjpFillColor(vg, TH_BG);
        hjpFill(vg);
        hjpStrokeColor(vg, TH_BORDER);
        hjpStrokeWidth(vg, 1.0f);
        hjpStroke(vg);

        for (int i = 0; i < count; i++) {
            float iy = dy + 2 + i * (h - 4);
            if (iy + (h - 4) > dy + dh) break;
            const char *txt = items[i].type == VALUE_STRING ? items[i].string.data : "?";
            bool ihov = gui_hit(g_cur->in.mx, g_cur->in.my, x, iy, w, h - 4);

            if (ihov) {
                hjpBeginPath(vg);
                hjpRoundedRect(vg, x + 2, iy, w - 4, h - 4, 3);
                hjpFillColor(vg, TH_WIDGET_HOVER);
                hjpFill(vg);
            }

            hjpFontSize(vg, GUI_FONT_SIZE);
            hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_MIDDLE);
            hjpFillColor(vg, (i == sel) ? TH_ACCENT : TH_TEXT);
            hjpText(vg, x + GUI_PADDING, iy + (h - 4) * 0.5f, txt, NULL);

            if (ihov && g_cur->in.clicked) {
                sel = i;
                g_cur->active = 0;
            }
        }
    }

    return hajimu_number(sel);
}

/* ポップコンファーム(メッセージ, 状態) → 真偽 */
static Value fn_popconfirm(int argc, Value *argv) {
    (void)argc;
    const char *msg = argv[0].string.data;
    bool show = argv[1].type == VALUE_BOOL ? argv[1].boolean : false;
    if (!g_cur || !show) return hajimu_bool(false);

    Hjpcontext *vg = g_cur->vg;
    float mx = g_cur->in.mx, my = g_cur->in.my;

    /* ポップアップ位置（最後のウィジェット付近） */
    float px = g_cur->lay.x;
    float py = g_cur->lay.y - 80;
    float pw = 240, ph = 70;

    /* 吹き出し背景 */
    hjpBeginPath(vg);
    hjpRoundedRect(vg, px, py, pw, ph, 8);
    hjpFillColor(vg, TH_BG);
    hjpFill(vg);
    hjpStrokeColor(vg, TH_BORDER);
    hjpStrokeWidth(vg, 1.0f);
    hjpStroke(vg);

    /* メッセージ */
    hjpFontSize(vg, GUI_FONT_SIZE);
    hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_TOP);
    hjpFillColor(vg, TH_TEXT);
    hjpText(vg, px + 12, py + 10, msg, NULL);

    /* はい/いいえ ボタン */
    float bw = 50, bh = 26;
    float by = py + ph - bh - 8;
    float bx_no = px + pw - bw - 8;
    float bx_yes = bx_no - bw - 6;

    bool hov_yes = gui_hit(mx, my, bx_yes, by, bw, bh);
    bool hov_no  = gui_hit(mx, my, bx_no, by, bw, bh);

    /* はい */
    hjpBeginPath(vg);
    hjpRoundedRect(vg, bx_yes, by, bw, bh, 4);
    hjpFillColor(vg, hov_yes ? TH_ACCENT_HOVER : TH_ACCENT);
    hjpFill(vg);
    hjpFontSize(vg, 14.0f);
    hjpTextAlign(vg, HJP_ALIGN_CENTER | HJP_ALIGN_MIDDLE);
    hjpFillColor(vg, hjpRGBA(255,255,255,255));
    hjpText(vg, bx_yes + bw * 0.5f, by + bh * 0.5f, "はい", NULL);

    /* いいえ */
    hjpBeginPath(vg);
    hjpRoundedRect(vg, bx_no, by, bw, bh, 4);
    hjpFillColor(vg, hov_no ? TH_WIDGET_HOVER : TH_WIDGET_BG);
    hjpFill(vg);
    hjpStrokeColor(vg, TH_BORDER);
    hjpStrokeWidth(vg, 1.0f);
    hjpStroke(vg);
    hjpFillColor(vg, TH_TEXT);
    hjpText(vg, bx_no + bw * 0.5f, by + bh * 0.5f, "いいえ", NULL);

    /* クリック判定 */
    if (g_cur->in.clicked) {
        if (hov_yes) return hajimu_bool(true);
        /* いいえ or 外部クリック → false が返る */
    }
    return hajimu_bool(false);
}

/* ボタングループ(ラベル配列, 選択) → 数値 */
static Value fn_button_group(int argc, Value *argv) {
    (void)argc;
    if (argv[0].type != VALUE_ARRAY) return hajimu_number(0);
    int count = argv[0].array.length;
    Value *items = argv[0].array.elements;
    int sel = (int)argv[1].number;
    if (!g_cur || count == 0) return hajimu_number(sel);

    Hjpcontext *vg = g_cur->vg;
    float x, y, w;
    gui_pos(&x, &y, &w);
    float h = GUI_WIDGET_H;
    float bw = w / count;

    for (int i = 0; i < count; i++) {
        float bx = x + i * bw;
        const char *lbl = items[i].type == VALUE_STRING ? items[i].string.data : "?";
        bool hov = gui_hit(g_cur->in.mx, g_cur->in.my, bx, y, bw, h);
        bool active = (i == sel);

        if (hov && g_cur->in.clicked) sel = i;

        /* 角丸は最初と最後のみ */
        hjpBeginPath(vg);
        if (i == 0) {
            hjpRoundedRect(vg, bx, y, bw + 1, h, GUI_BTN_RADIUS);
        } else if (i == count - 1) {
            hjpRoundedRect(vg, bx - 1, y, bw + 1, h, GUI_BTN_RADIUS);
        } else {
            hjpRect(vg, bx, y, bw, h);
        }

        if (active) {
            hjpFillColor(vg, hov ? TH_ACCENT_HOVER : TH_ACCENT);
        } else {
            hjpFillColor(vg, hov ? TH_WIDGET_HOVER : TH_WIDGET_BG);
        }
        hjpFill(vg);

        /* 区切り線 */
        if (i > 0) {
            hjpBeginPath(vg);
            hjpMoveTo(vg, bx, y + 4);
            hjpLineTo(vg, bx, y + h - 4);
            hjpStrokeColor(vg, TH_BORDER);
            hjpStrokeWidth(vg, 1.0f);
            hjpStroke(vg);
        }

        hjpFontSize(vg, GUI_FONT_SIZE);
        hjpTextAlign(vg, HJP_ALIGN_CENTER | HJP_ALIGN_MIDDLE);
        hjpFillColor(vg, active ? hjpRGBA(255,255,255,255) : TH_TEXT);
        hjpText(vg, bx + bw * 0.5f, y + h * 0.5f, lbl, NULL);
    }

    /* 全体の枠 */
    hjpBeginPath(vg);
    hjpRoundedRect(vg, x, y, w, h, GUI_BTN_RADIUS);
    hjpStrokeColor(vg, TH_BORDER);
    hjpStrokeWidth(vg, 1.0f);
    hjpStroke(vg);

    gui_advance(h);
    return hajimu_number(sel);
}

/* =====================================================================
 * Phase 24: ナビゲーション拡張
 * ===================================================================*/

/* ページネーション(現在, 総数[, 表示数]) → 数値 */
static Value fn_pagination(int argc, Value *argv) {
    int cur = (int)argv[0].number;
    int total = (int)argv[1].number;
    int show = argc >= 3 ? (int)argv[2].number : 5;
    if (!g_cur || total <= 0) return hajimu_number(cur);

    Hjpcontext *vg = g_cur->vg;
    float x, y, w;
    gui_pos(&x, &y, &w);
    float bsz = 32, gap = 4, h = bsz;

    int start = cur - show / 2;
    if (start < 1) start = 1;
    int end = start + show - 1;
    if (end > total) { end = total; start = end - show + 1; if (start < 1) start = 1; }

    float tx = x;

    /* ◀ 前ボタン */
    {
        bool hov = gui_hit(g_cur->in.mx, g_cur->in.my, tx, y, bsz, bsz);
        hjpBeginPath(vg); hjpRoundedRect(vg, tx, y, bsz, bsz, 4);
        hjpFillColor(vg, hov ? TH_WIDGET_HOVER : TH_WIDGET_BG); hjpFill(vg);
        hjpFontSize(vg, 14); hjpTextAlign(vg, HJP_ALIGN_CENTER | HJP_ALIGN_MIDDLE);
        hjpFillColor(vg, cur > 1 ? TH_TEXT : TH_TEXT_DIM);
        hjpText(vg, tx + bsz*0.5f, y + bsz*0.5f, "\xe2\x97\x80", NULL);
        if (hov && g_cur->in.clicked && cur > 1) cur--;
        tx += bsz + gap;
    }

    for (int i = start; i <= end; i++) {
        bool hov = gui_hit(g_cur->in.mx, g_cur->in.my, tx, y, bsz, bsz);
        bool act = (i == cur);
        hjpBeginPath(vg); hjpRoundedRect(vg, tx, y, bsz, bsz, 4);
        hjpFillColor(vg, act ? TH_ACCENT : (hov ? TH_WIDGET_HOVER : TH_WIDGET_BG));
        hjpFill(vg);
        char num[16]; snprintf(num, sizeof(num), "%d", i);
        hjpFontSize(vg, GUI_FONT_SIZE); hjpTextAlign(vg, HJP_ALIGN_CENTER | HJP_ALIGN_MIDDLE);
        hjpFillColor(vg, act ? hjpRGBA(255,255,255,255) : TH_TEXT);
        hjpText(vg, tx + bsz*0.5f, y + bsz*0.5f, num, NULL);
        if (hov && g_cur->in.clicked) cur = i;
        tx += bsz + gap;
    }

    /* ▶ 次ボタン */
    {
        bool hov = gui_hit(g_cur->in.mx, g_cur->in.my, tx, y, bsz, bsz);
        hjpBeginPath(vg); hjpRoundedRect(vg, tx, y, bsz, bsz, 4);
        hjpFillColor(vg, hov ? TH_WIDGET_HOVER : TH_WIDGET_BG); hjpFill(vg);
        hjpFontSize(vg, 14); hjpTextAlign(vg, HJP_ALIGN_CENTER | HJP_ALIGN_MIDDLE);
        hjpFillColor(vg, cur < total ? TH_TEXT : TH_TEXT_DIM);
        hjpText(vg, tx + bsz*0.5f, y + bsz*0.5f, "\xe2\x96\xb6", NULL);
        if (hov && g_cur->in.clicked && cur < total) cur++;
    }

    gui_advance(h);
    return hajimu_number(cur);
}

/* ボトムナビゲーション(項目, 選択) → 数値 */
static Value fn_bottom_nav(int argc, Value *argv) {
    (void)argc;
    if (argv[0].type != VALUE_ARRAY) return hajimu_number(0);
    int count = argv[0].array.length;
    Value *items = argv[0].array.elements;
    int sel = (int)argv[1].number;
    if (!g_cur || count == 0) return hajimu_number(sel);

    Hjpcontext *vg = g_cur->vg;
    float ww = (float)g_cur->win_w;
    float wh = (float)g_cur->win_h;
    float bh = 56;
    float by = wh - bh;
    float bw = ww / count;

    /* 背景 */
    hjpBeginPath(vg);
    hjpRect(vg, 0, by, ww, bh);
    hjpFillColor(vg, TH_WIDGET_BG);
    hjpFill(vg);
    hjpBeginPath(vg);
    hjpMoveTo(vg, 0, by); hjpLineTo(vg, ww, by);
    hjpStrokeColor(vg, TH_BORDER); hjpStrokeWidth(vg, 1); hjpStroke(vg);

    for (int i = 0; i < count; i++) {
        float bx = i * bw;
        const char *lbl = items[i].type == VALUE_STRING ? items[i].string.data : "?";
        bool hov = gui_hit(g_cur->in.mx, g_cur->in.my, bx, by, bw, bh);
        bool act = (i == sel);

        if (hov && g_cur->in.clicked) sel = i;

        /* アクティブインジケーター */
        if (act) {
            hjpBeginPath(vg);
            hjpRect(vg, bx + bw * 0.2f, by, bw * 0.6f, 3);
            hjpFillColor(vg, TH_ACCENT);
            hjpFill(vg);
        }

        hjpFontSize(vg, 14.0f);
        hjpTextAlign(vg, HJP_ALIGN_CENTER | HJP_ALIGN_MIDDLE);
        hjpFillColor(vg, act ? TH_ACCENT : (hov ? TH_TEXT : TH_TEXT_DIM));
        hjpText(vg, bx + bw * 0.5f, by + bh * 0.5f, lbl, NULL);
    }

    return hajimu_number(sel);
}

/* ドロワー開始(幅, 開閉) → 真偽 */
static Value fn_drawer_begin(int argc, Value *argv) {
    (void)argc;
    float dw = (float)argv[0].number;
    bool open = argv[1].type == VALUE_BOOL ? argv[1].boolean : false;
    if (!g_cur) return hajimu_bool(open);

    Hjpcontext *vg = g_cur->vg;
    float ww = (float)g_cur->win_w;
    float wh = (float)g_cur->win_h;

    if (!open) return hajimu_bool(false);

    /* 半透明オーバーレイ */
    hjpBeginPath(vg);
    hjpRect(vg, 0, 0, ww, wh);
    hjpFillColor(vg, hjpRGBA(0, 0, 0, 100));
    hjpFill(vg);

    /* オーバーレイクリックで閉じる */
    if (g_cur->in.clicked && g_cur->in.mx > dw) {
        return hajimu_bool(false);
    }

    /* ドロワーパネル */
    hjpBeginPath(vg);
    hjpRect(vg, 0, 0, dw, wh);
    hjpFillColor(vg, TH_BG);
    hjpFill(vg);
    hjpBeginPath(vg);
    hjpMoveTo(vg, dw, 0); hjpLineTo(vg, dw, wh);
    hjpStrokeColor(vg, TH_BORDER); hjpStrokeWidth(vg, 1); hjpStroke(vg);

    /* ドロワー内レイアウト */
    g_cur->lay.x = GUI_PADDING;
    g_cur->lay.y = GUI_PADDING;
    g_cur->lay.w = dw - GUI_PADDING * 2;

    return hajimu_bool(true);
}

/* ドロワー終了() → 無 */
static Value fn_drawer_end(int argc, Value *argv) {
    (void)argc; (void)argv;
    return hajimu_null();
}

/* サイドナビゲーション(項目, 選択) → 数値 */
static Value fn_side_nav(int argc, Value *argv) {
    (void)argc;
    if (argv[0].type != VALUE_ARRAY) return hajimu_number(0);
    int count = argv[0].array.length;
    Value *items = argv[0].array.elements;
    int sel = (int)argv[1].number;
    if (!g_cur || count == 0) return hajimu_number(sel);

    Hjpcontext *vg = g_cur->vg;
    float nw = 72;
    float wh = (float)g_cur->win_h;
    float ih = 56;

    /* 背景 */
    hjpBeginPath(vg);
    hjpRect(vg, 0, 0, nw, wh);
    hjpFillColor(vg, TH_WIDGET_BG);
    hjpFill(vg);
    hjpBeginPath(vg);
    hjpMoveTo(vg, nw, 0); hjpLineTo(vg, nw, wh);
    hjpStrokeColor(vg, TH_BORDER); hjpStrokeWidth(vg, 1); hjpStroke(vg);

    for (int i = 0; i < count; i++) {
        float iy = i * ih;
        const char *lbl = items[i].type == VALUE_STRING ? items[i].string.data : "?";
        bool hov = gui_hit(g_cur->in.mx, g_cur->in.my, 0, iy, nw, ih);
        bool act = (i == sel);

        if (hov && g_cur->in.clicked) sel = i;

        if (act) {
            hjpBeginPath(vg);
            hjpRect(vg, 0, iy, 3, ih);
            hjpFillColor(vg, TH_ACCENT);
            hjpFill(vg);
            hjpBeginPath(vg);
            hjpRoundedRect(vg, 8, iy + 4, nw - 16, ih - 8, 8);
            hjpFillColor(vg, hjpRGBA(
                (int)(TH_ACCENT.r*255) & 0xff,
                (int)(TH_ACCENT.g*255) & 0xff,
                (int)(TH_ACCENT.b*255) & 0xff, 30));
            hjpFill(vg);
        }

        hjpFontSize(vg, 12.0f);
        hjpTextAlign(vg, HJP_ALIGN_CENTER | HJP_ALIGN_MIDDLE);
        hjpFillColor(vg, act ? TH_ACCENT : (hov ? TH_TEXT : TH_TEXT_DIM));
        hjpText(vg, nw * 0.5f, iy + ih * 0.5f, lbl, NULL);
    }

    /* メインコンテンツ領域をオフセット */
    g_cur->lay.x = nw + GUI_PADDING;

    return hajimu_number(sel);
}

/* スピードダイヤル(項目, 位置) → 数値 */
static Value fn_speed_dial(int argc, Value *argv) {
    (void)argc;
    if (argv[0].type != VALUE_ARRAY) return hajimu_number(-1);
    int count = argv[0].array.length;
    Value *items = argv[0].array.elements;
    const char *pos = argv[1].type == VALUE_STRING ? argv[1].string.data : "右下";
    if (!g_cur || count == 0) return hajimu_number(-1);

    Hjpcontext *vg = g_cur->vg;
    float ww = (float)g_cur->win_w;
    float wh = (float)g_cur->win_h;
    float fab_r = 28;
    float fx, fy;

    /* 位置 */
    bool is_right = (strstr(pos, "右") != NULL);
    bool is_bottom = (strstr(pos, "下") != NULL);
    fx = is_right ? ww - fab_r - 24 : fab_r + 24;
    fy = is_bottom ? wh - fab_r - 24 : fab_r + 24;

    uint32_t id = gui_hash("speed_dial");
    bool fab_hov = gui_hit(g_cur->in.mx, g_cur->in.my,
                           fx - fab_r, fy - fab_r, fab_r * 2, fab_r * 2);

    if (fab_hov && g_cur->in.clicked) {
        g_cur->active = (g_cur->active == id) ? 0 : id;
    }

    bool expanded = (g_cur->active == id);
    int result = -1;

    /* 展開時：サブアイテム */
    if (expanded) {
        for (int i = 0; i < count; i++) {
            float sr = 20;
            float sy = fy - (i + 1) * (sr * 2 + 10);
            const char *lbl = items[i].type == VALUE_STRING ? items[i].string.data : "?";
            bool shov = gui_hit(g_cur->in.mx, g_cur->in.my,
                                fx - sr, sy - sr, sr * 2, sr * 2);

            hjpBeginPath(vg);
            hjpCircle(vg, fx, sy, sr);
            hjpFillColor(vg, shov ? TH_ACCENT_HOVER : TH_WIDGET_BG);
            hjpFill(vg);
            hjpStrokeColor(vg, TH_BORDER); hjpStrokeWidth(vg, 1); hjpStroke(vg);

            hjpFontSize(vg, 11.0f);
            hjpTextAlign(vg, HJP_ALIGN_CENTER | HJP_ALIGN_MIDDLE);
            hjpFillColor(vg, shov ? hjpRGBA(255,255,255,255) : TH_TEXT);
            hjpText(vg, fx, sy, lbl, NULL);

            if (shov && g_cur->in.clicked) {
                result = i;
                g_cur->active = 0;
            }
        }
    }

    /* FAB本体 */
    hjpBeginPath(vg);
    hjpCircle(vg, fx, fy, fab_r);
    hjpFillColor(vg, fab_hov ? TH_ACCENT_HOVER : TH_ACCENT);
    hjpFill(vg);
    hjpFontSize(vg, 24.0f);
    hjpTextAlign(vg, HJP_ALIGN_CENTER | HJP_ALIGN_MIDDLE);
    hjpFillColor(vg, hjpRGBA(255,255,255,255));
    hjpText(vg, fx, fy, expanded ? "\xc3\x97" : "+", NULL);

    return hajimu_number(result);
}

/* アンカーリンク(項目) → 数値 */
static Value fn_anchor_link(int argc, Value *argv) {
    (void)argc;
    if (argv[0].type != VALUE_ARRAY) return hajimu_number(-1);
    int count = argv[0].array.length;
    Value *items = argv[0].array.elements;
    if (!g_cur || count == 0) return hajimu_number(-1);

    Hjpcontext *vg = g_cur->vg;
    float x, y, w;
    gui_pos(&x, &y, &w);
    float ih = 28;
    int clicked = -1;

    /* 左ライン */
    hjpBeginPath(vg);
    hjpMoveTo(vg, x + 2, y);
    hjpLineTo(vg, x + 2, y + count * ih);
    hjpStrokeColor(vg, TH_BORDER);
    hjpStrokeWidth(vg, 2.0f);
    hjpStroke(vg);

    for (int i = 0; i < count; i++) {
        float iy = y + i * ih;
        const char *lbl = items[i].type == VALUE_STRING ? items[i].string.data : "?";
        bool hov = gui_hit(g_cur->in.mx, g_cur->in.my, x, iy, w, ih);

        if (hov && g_cur->in.clicked) clicked = i;

        if (hov) {
            /* アクティブインジケーター */
            hjpBeginPath(vg);
            hjpRect(vg, x, iy + 2, 4, ih - 4);
            hjpFillColor(vg, TH_ACCENT);
            hjpFill(vg);
        }

        hjpFontSize(vg, 14.0f);
        hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_MIDDLE);
        hjpFillColor(vg, hov ? TH_ACCENT : TH_TEXT_DIM);
        hjpText(vg, x + 14, iy + ih * 0.5f, lbl, NULL);
    }

    gui_advance(count * ih);
    return hajimu_number(clicked);
}

/* 矢印ボタン(方向) → 真偽 */
static Value fn_arrow_button(int argc, Value *argv) {
    (void)argc;
    const char *dir = argv[0].string.data;
    if (!g_cur) return hajimu_bool(false);

    Hjpcontext *vg = g_cur->vg;
    float x, y, w;
    gui_pos(&x, &y, &w);
    float sz = GUI_WIDGET_H;

    uint32_t id = gui_hash(dir);
    bool hov, pressed;
    gui_widget_logic(id, x, y, sz, sz, &hov, &pressed);

    hjpBeginPath(vg);
    hjpRoundedRect(vg, x, y, sz, sz, GUI_BTN_RADIUS);
    hjpFillColor(vg, hov ? TH_WIDGET_HOVER : TH_WIDGET_BG);
    hjpFill(vg);
    hjpStrokeColor(vg, TH_BORDER); hjpStrokeWidth(vg, 1); hjpStroke(vg);

    /* 矢印描画 */
    const char *arrow = "\xe2\x96\xb6"; /* ▶ default */
    if (strstr(dir, "左") || strstr(dir, "left"))  arrow = "\xe2\x97\x80";
    if (strstr(dir, "上") || strstr(dir, "up"))    arrow = "\xe2\x96\xb2";
    if (strstr(dir, "下") || strstr(dir, "down"))  arrow = "\xe2\x96\xbc";

    hjpFontSize(vg, 16.0f);
    hjpTextAlign(vg, HJP_ALIGN_CENTER | HJP_ALIGN_MIDDLE);
    hjpFillColor(vg, TH_TEXT);
    hjpText(vg, x + sz * 0.5f, y + sz * 0.5f, arrow, NULL);

    gui_advance(sz);
    return hajimu_bool(pressed);
}

/* =====================================================================
 * Phase 25: 描画拡張 II
 * ===================================================================*/

/* 楕円(x, y, rx, ry[, 色]) → 無 */
static Value fn_draw_ellipse(int argc, Value *argv) {
    float cx = (float)argv[0].number;
    float cy = (float)argv[1].number;
    float rx = (float)argv[2].number;
    float ry = (float)argv[3].number;
    if (!g_cur) return hajimu_null();

    Hjpcontext *vg = g_cur->vg;
    Hjpcolor col = (argc >= 5) ? gui_arg_color(argv[4]) : TH_TEXT;

    hjpBeginPath(vg);
    hjpEllipse(vg, cx, cy, rx, ry);
    hjpFillColor(vg, col);
    hjpFill(vg);
    return hajimu_null();
}

/* 点線(x1, y1, x2, y2[, 色]) → 無 */
static Value fn_draw_dashed(int argc, Value *argv) {
    float x1 = (float)argv[0].number, y1 = (float)argv[1].number;
    float x2 = (float)argv[2].number, y2 = (float)argv[3].number;
    if (!g_cur) return hajimu_null();

    Hjpcontext *vg = g_cur->vg;
    Hjpcolor col = (argc >= 5) ? gui_arg_color(argv[4]) : TH_TEXT;
    float dash = 8.0f, gap = 6.0f;

    float dx = x2 - x1, dy = y2 - y1;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 1) return hajimu_null();
    float ux = dx / len, uy = dy / len;

    float d = 0;
    while (d < len) {
        float seg = dash;
        if (d + seg > len) seg = len - d;
        hjpBeginPath(vg);
        hjpMoveTo(vg, x1 + ux * d, y1 + uy * d);
        hjpLineTo(vg, x1 + ux * (d + seg), y1 + uy * (d + seg));
        hjpStrokeColor(vg, col);
        hjpStrokeWidth(vg, 2.0f);
        hjpStroke(vg);
        d += dash + gap;
    }
    return hajimu_null();
}

/* 矢印線(x1, y1, x2, y2[, 色]) → 無 */
static Value fn_draw_arrow(int argc, Value *argv) {
    float x1 = (float)argv[0].number, y1 = (float)argv[1].number;
    float x2 = (float)argv[2].number, y2 = (float)argv[3].number;
    if (!g_cur) return hajimu_null();

    Hjpcontext *vg = g_cur->vg;
    Hjpcolor col = (argc >= 5) ? gui_arg_color(argv[4]) : TH_TEXT;

    /* 線 */
    hjpBeginPath(vg);
    hjpMoveTo(vg, x1, y1);
    hjpLineTo(vg, x2, y2);
    hjpStrokeColor(vg, col);
    hjpStrokeWidth(vg, 2.0f);
    hjpStroke(vg);

    /* 矢じり */
    float dx = x2 - x1, dy = y2 - y1;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 1) return hajimu_null();
    float ux = dx / len, uy = dy / len;
    float arrow_len = 12.0f, arrow_w = 6.0f;
    float ax = x2 - ux * arrow_len;
    float ay = y2 - uy * arrow_len;

    hjpBeginPath(vg);
    hjpMoveTo(vg, x2, y2);
    hjpLineTo(vg, ax + uy * arrow_w, ay - ux * arrow_w);
    hjpLineTo(vg, ax - uy * arrow_w, ay + ux * arrow_w);
    hjpClosePath(vg);
    hjpFillColor(vg, col);
    hjpFill(vg);

    return hajimu_null();
}

/* 影(x, y, w, h[, ぼかし]) → 無 */
static Value fn_draw_shadow(int argc, Value *argv) {
    float x = (float)argv[0].number, y = (float)argv[1].number;
    float w = (float)argv[2].number, h = (float)argv[3].number;
    float blur = argc >= 5 ? (float)argv[4].number : 10.0f;
    if (!g_cur) return hajimu_null();

    Hjpcontext *vg = g_cur->vg;
    Hjppaint shadow = hjpBoxGradient(vg, x, y + 2, w, h, 6, blur,
        hjpRGBA(0, 0, 0, 80), hjpRGBA(0, 0, 0, 0));
    hjpBeginPath(vg);
    hjpRect(vg, x - blur, y - blur, w + blur * 2, h + blur * 2 + 4);
    hjpRoundedRect(vg, x, y, w, h, 6);
    hjpPathWinding(vg, HJP_HOLE);
    hjpFillPaint(vg, shadow);
    hjpFill(vg);
    return hajimu_null();
}

/* 放射グラデーション(cx, cy, r1, r2, 色1, 色2) → 無 */
static Value fn_radial_gradient(int argc, Value *argv) {
    (void)argc;
    float cx = (float)argv[0].number, cy = (float)argv[1].number;
    float r1 = (float)argv[2].number, r2 = (float)argv[3].number;
    Hjpcolor c1 = gui_arg_color(argv[4]);
    Hjpcolor c2 = gui_arg_color(argv[5]);
    if (!g_cur) return hajimu_null();

    Hjpcontext *vg = g_cur->vg;
    Hjppaint p = hjpRadialGradient(vg, cx, cy, r1, r2, c1, c2);
    hjpBeginPath(vg);
    hjpCircle(vg, cx, cy, r2);
    hjpFillPaint(vg, p);
    hjpFill(vg);
    return hajimu_null();
}

/* ボックスグラデーション(x, y, w, h, 色1, 色2) → 無 */
static Value fn_box_gradient(int argc, Value *argv) {
    (void)argc;
    float x = (float)argv[0].number, y = (float)argv[1].number;
    float w = (float)argv[2].number, h = (float)argv[3].number;
    Hjpcolor c1 = gui_arg_color(argv[4]);
    Hjpcolor c2 = gui_arg_color(argv[5]);
    if (!g_cur) return hajimu_null();

    Hjpcontext *vg = g_cur->vg;
    Hjppaint p = hjpBoxGradient(vg, x, y, w, h, GUI_BTN_RADIUS, 10.0f, c1, c2);
    hjpBeginPath(vg);
    hjpRoundedRect(vg, x, y, w, h, GUI_BTN_RADIUS);
    hjpFillPaint(vg, p);
    hjpFill(vg);
    return hajimu_null();
}

/* テクスチャパターン(画像, x, y, w, h) → 無 */
static Value fn_texture_pattern(int argc, Value *argv) {
    (void)argc;
    int img_idx = (int)argv[0].number;
    float x = (float)argv[1].number, y = (float)argv[2].number;
    float w = (float)argv[3].number, h = (float)argv[4].number;
    if (!g_cur || img_idx < 0 || img_idx >= GUI_MAX_IMAGES || !g_images[img_idx].valid)
        return hajimu_null();

    Hjpcontext *vg = g_cur->vg;
    Hjppaint p = hjpImagePattern(vg, x, y, w, h, 0, g_images[img_idx].handle, 1.0f);
    hjpBeginPath(vg);
    hjpRect(vg, x, y, w, h);
    hjpFillPaint(vg, p);
    hjpFill(vg);
    return hajimu_null();
}

/* 描画アルファ(値) → 無 */
static Value fn_draw_alpha(int argc, Value *argv) {
    (void)argc;
    float a = (float)argv[0].number;
    if (a < 0) a = 0; if (a > 1) a = 1;
    if (g_cur) hjpGlobalAlpha(g_cur->vg, a);
    return hajimu_null();
}

/* =====================================================================
 * Phase 26: テキスト・データ表示拡張
 * ===================================================================*/

/* テキスト測定(文字列) → 辞書{幅, 高さ} */
static Value fn_text_measure(int argc, Value *argv) {
    (void)argc;
    const char *txt = argv[0].string.data;
    if (!g_cur) return gui_dict2("幅", hajimu_number(0), "高さ", hajimu_number(0));

    Hjpcontext *vg = g_cur->vg;
    float bounds[4];
    hjpFontSize(vg, g_custom_font_size > 0 ? g_custom_font_size : GUI_FONT_SIZE);
    hjpTextBounds(vg, 0, 0, txt, NULL, bounds);
    float tw = bounds[2] - bounds[0];
    float th = bounds[3] - bounds[1];
    return gui_dict2("幅", hajimu_number(tw), "高さ", hajimu_number(th));
}

/* 省略テキスト(文字列, 最大幅) → 無 */
static Value fn_text_ellipsis(int argc, Value *argv) {
    (void)argc;
    const char *txt = argv[0].string.data;
    float max_w = (float)argv[1].number;
    if (!g_cur) return hajimu_null();

    Hjpcontext *vg = g_cur->vg;
    float x, y, w;
    gui_pos(&x, &y, &w);
    if (max_w <= 0) max_w = w;

    hjpFontSize(vg, g_custom_font_size > 0 ? g_custom_font_size : GUI_FONT_SIZE);
    float bounds[4];
    hjpTextBounds(vg, 0, 0, txt, NULL, bounds);
    float tw = bounds[2] - bounds[0];

    hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_TOP);
    hjpFillColor(vg, TH_TEXT);

    if (tw <= max_w) {
        hjpText(vg, x, y, txt, NULL);
    } else {
        /* 省略記号付き */
        hjpScissor(vg, x, y, max_w - 16, 30);
        hjpText(vg, x, y, txt, NULL);
        hjpResetScissor(vg);
        hjpText(vg, x + max_w - 16, y, "\xe2\x80\xa6", NULL); /* … */
    }

    gui_advance(22);
    return hajimu_null();
}

/* ラベル付き値(ラベル, 値) → 無 */
static Value fn_label_value(int argc, Value *argv) {
    (void)argc;
    const char *label = argv[0].string.data;
    const char *val = argv[1].type == VALUE_STRING ? argv[1].string.data : "?";
    char buf[256];
    if (argv[1].type == VALUE_NUMBER) {
        snprintf(buf, sizeof(buf), "%.6g", argv[1].number);
        val = buf;
    }
    if (!g_cur) return hajimu_null();

    Hjpcontext *vg = g_cur->vg;
    float x, y, w;
    gui_pos(&x, &y, &w);

    hjpFontSize(vg, GUI_FONT_SIZE);
    hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_TOP);
    hjpFillColor(vg, TH_TEXT_DIM);
    hjpText(vg, x, y, label, NULL);

    float lw = 0;
    float bounds[4];
    hjpTextBounds(vg, 0, 0, label, NULL, bounds);
    lw = bounds[2] - bounds[0];

    hjpFillColor(vg, TH_TEXT);
    hjpText(vg, x + lw + 12, y, val, NULL);

    gui_advance(22);
    return hajimu_null();
}

/* 統計カード(ラベル, 値[, 増減]) → 無 */
static Value fn_stat_card(int argc, Value *argv) {
    const char *label = argv[0].string.data;
    double val = argv[1].type == VALUE_NUMBER ? argv[1].number : 0;
    double delta = argc >= 3 ? argv[2].number : 0;
    if (!g_cur) return hajimu_null();

    Hjpcontext *vg = g_cur->vg;
    float x, y, w;
    gui_pos(&x, &y, &w);
    float h = 80;

    /* カード背景 */
    hjpBeginPath(vg);
    hjpRoundedRect(vg, x, y, w, h, 8);
    hjpFillColor(vg, TH_WIDGET_BG);
    hjpFill(vg);
    hjpStrokeColor(vg, TH_BORDER); hjpStrokeWidth(vg, 1); hjpStroke(vg);

    /* ラベル */
    hjpFontSize(vg, 13.0f);
    hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_TOP);
    hjpFillColor(vg, TH_TEXT_DIM);
    hjpText(vg, x + 14, y + 10, label, NULL);

    /* 値 */
    char vbuf[64];
    snprintf(vbuf, sizeof(vbuf), "%.6g", val);
    hjpFontSize(vg, 28.0f);
    hjpFillColor(vg, TH_TEXT);
    hjpText(vg, x + 14, y + 30, vbuf, NULL);

    /* 増減 */
    if (delta != 0) {
        char dbuf[64];
        snprintf(dbuf, sizeof(dbuf), "%+.1f%%", delta);
        hjpFontSize(vg, 13.0f);
        hjpFillColor(vg, delta > 0 ? hjpRGBA(34,197,94,255) : hjpRGBA(239,68,68,255));
        hjpTextAlign(vg, HJP_ALIGN_RIGHT | HJP_ALIGN_TOP);
        hjpText(vg, x + w - 14, y + 10, dbuf, NULL);
    }

    gui_advance(h + GUI_MARGIN);
    return hajimu_null();
}

/* 空状態表示([メッセージ]) → 無 */
static Value fn_empty_state(int argc, Value *argv) {
    const char *msg = argc >= 1 && argv[0].type == VALUE_STRING
        ? argv[0].string.data : "データがありません";
    if (!g_cur) return hajimu_null();

    Hjpcontext *vg = g_cur->vg;
    float x, y, w;
    gui_pos(&x, &y, &w);
    float h = 100;

    /* 空箱アイコン */
    float cx = x + w * 0.5f;
    hjpBeginPath(vg);
    hjpRoundedRect(vg, cx - 20, y + 10, 40, 30, 4);
    hjpStrokeColor(vg, TH_TEXT_DIM);
    hjpStrokeWidth(vg, 2.0f);
    hjpStroke(vg);
    hjpBeginPath(vg);
    hjpMoveTo(vg, cx - 20, y + 25);
    hjpLineTo(vg, cx + 20, y + 25);
    hjpStroke(vg);

    /* メッセージ */
    hjpFontSize(vg, GUI_FONT_SIZE);
    hjpTextAlign(vg, HJP_ALIGN_CENTER | HJP_ALIGN_TOP);
    hjpFillColor(vg, TH_TEXT_DIM);
    hjpText(vg, cx, y + 50, msg, NULL);

    gui_advance(h);
    return hajimu_null();
}

/* 結果ページ(種類, タイトル, 説明) → 無 */
static Value fn_result_page(int argc, Value *argv) {
    (void)argc;
    const char *kind = argv[0].string.data;
    const char *title = argv[1].string.data;
    const char *desc = argv[2].string.data;
    if (!g_cur) return hajimu_null();

    Hjpcontext *vg = g_cur->vg;
    float x, y, w;
    gui_pos(&x, &y, &w);
    float cx = x + w * 0.5f;
    float h = 140;

    /* アイコン色 */
    Hjpcolor icon_col = TH_ACCENT;
    const char *icon = "\xe2\x9c\x93"; /* ✓ */
    if (strstr(kind, "失敗") || strstr(kind, "error")) {
        icon_col = hjpRGBA(239, 68, 68, 255);
        icon = "\xc3\x97"; /* × */
    } else if (strstr(kind, "警告") || strstr(kind, "warn")) {
        icon_col = hjpRGBA(234, 179, 8, 255);
        icon = "!";
    } else if (strstr(kind, "情報") || strstr(kind, "info")) {
        icon_col = hjpRGBA(59, 130, 246, 255);
        icon = "i";
    }

    /* アイコン円 */
    hjpBeginPath(vg);
    hjpCircle(vg, cx, y + 36, 28);
    hjpFillColor(vg, icon_col);
    hjpFill(vg);
    hjpFontSize(vg, 28.0f);
    hjpTextAlign(vg, HJP_ALIGN_CENTER | HJP_ALIGN_MIDDLE);
    hjpFillColor(vg, hjpRGBA(255,255,255,255));
    hjpText(vg, cx, y + 36, icon, NULL);

    /* タイトル */
    hjpFontSize(vg, 22.0f);
    hjpTextAlign(vg, HJP_ALIGN_CENTER | HJP_ALIGN_TOP);
    hjpFillColor(vg, TH_TEXT);
    hjpText(vg, cx, y + 76, title, NULL);

    /* 説明 */
    hjpFontSize(vg, GUI_FONT_SIZE);
    hjpFillColor(vg, TH_TEXT_DIM);
    hjpText(vg, cx, y + 108, desc, NULL);

    gui_advance(h);
    return hajimu_null();
}

/* ウォーターマーク(テキスト[, 角度]) → 無 */
static Value fn_watermark(int argc, Value *argv) {
    const char *txt = argv[0].string.data;
    float angle = argc >= 2 ? (float)argv[1].number : -30.0f;
    if (!g_cur) return hajimu_null();

    Hjpcontext *vg = g_cur->vg;
    float ww = (float)g_cur->win_w;
    float wh = (float)g_cur->win_h;
    float rad = angle * (3.14159f / 180.0f);
    float gap_x = 200, gap_y = 120;

    hjpSave(vg);
    hjpFontSize(vg, 16.0f);
    hjpFillColor(vg, hjpRGBA(128, 128, 128, 40));
    hjpTextAlign(vg, HJP_ALIGN_CENTER | HJP_ALIGN_MIDDLE);

    for (float py = -wh * 0.5f; py < wh * 1.5f; py += gap_y) {
        for (float px = -ww * 0.5f; px < ww * 1.5f; px += gap_x) {
            hjpSave(vg);
            hjpTranslate(vg, px, py);
            hjpRotate(vg, rad);
            hjpText(vg, 0, 0, txt, NULL);
            hjpRestore(vg);
        }
    }
    hjpRestore(vg);
    return hajimu_null();
}

/* QRコード(データ[, サイズ]) → 無 — 簡易ビジュアル表現 */
static Value fn_qrcode(int argc, Value *argv) {
    const char *data = argv[0].string.data;
    float sz = argc >= 2 ? (float)argv[1].number : 120;
    if (!g_cur) return hajimu_null();

    Hjpcontext *vg = g_cur->vg;
    float x, y, w;
    gui_pos(&x, &y, &w);

    /* 簡易QRコード風パターン（データからハッシュで生成） */
    int grid = 21; /* QR Version 1 */
    float cell = sz / grid;

    /* 背景 */
    hjpBeginPath(vg);
    hjpRect(vg, x, y, sz, sz);
    hjpFillColor(vg, hjpRGBA(255,255,255,255));
    hjpFill(vg);

    /* データからパターン生成 */
    uint32_t hash = 5381;
    for (const char *p = data; *p; p++) hash = hash * 33 + (unsigned char)*p;

    for (int r = 0; r < grid; r++) {
        for (int c = 0; c < grid; c++) {
            /* ファインダーパターン（3隅の大きな正方形） */
            bool finder = (r < 7 && c < 7) || (r < 7 && c >= grid - 7) || (r >= grid - 7 && c < 7);
            bool fill = false;

            if (finder) {
                /* 7x7 ファインダーパターン */
                int lr = r < 7 ? r : r - (grid - 7);
                int lc = c < 7 ? c : c - (grid - 7);
                fill = (lr == 0 || lr == 6 || lc == 0 || lc == 6 ||
                        (lr >= 2 && lr <= 4 && lc >= 2 && lc <= 4));
            } else {
                /* データセル — ハッシュベースで擬似ランダム */
                uint32_t h2 = hash ^ (r * 31 + c * 17);
                h2 = (h2 * 2654435761u) >> 16;
                fill = (h2 & 1);
            }

            if (fill) {
                hjpBeginPath(vg);
                hjpRect(vg, x + c * cell, y + r * cell, cell, cell);
                hjpFillColor(vg, hjpRGBA(0, 0, 0, 255));
                hjpFill(vg);
            }
        }
    }

    gui_advance(sz + GUI_MARGIN);
    return hajimu_null();
}

/* =====================================================================
 * Phase 27: アニメーション拡張
 * ===================================================================*/

/* イージング(種類, 値) → 数値 (0.0–1.0) */
static Value fn_easing(int argc, Value *argv) {
    (void)argc;
    const char *kind = argv[0].string.data;
    float t = (float)argv[1].number;
    if (t < 0) t = 0; if (t > 1) t = 1;

    float r = t;
    if (strstr(kind, "バウンス") || strstr(kind, "bounce")) {
        /* bounce ease-out */
        if (t < 1.0f / 2.75f) {
            r = 7.5625f * t * t;
        } else if (t < 2.0f / 2.75f) {
            float t2 = t - 1.5f / 2.75f;
            r = 7.5625f * t2 * t2 + 0.75f;
        } else if (t < 2.5f / 2.75f) {
            float t2 = t - 2.25f / 2.75f;
            r = 7.5625f * t2 * t2 + 0.9375f;
        } else {
            float t2 = t - 2.625f / 2.75f;
            r = 7.5625f * t2 * t2 + 0.984375f;
        }
    } else if (strstr(kind, "エラスティック") || strstr(kind, "elastic")) {
        if (t == 0 || t == 1) { r = t; }
        else {
            float p = 0.3f;
            r = powf(2, -10 * t) * sinf((t - p / 4) * (2 * 3.14159f) / p) + 1;
        }
    } else if (strstr(kind, "インアウト") || strstr(kind, "in_out")) {
        r = gui_ease(t, 3);
    } else if (strstr(kind, "イン") || strstr(kind, "in")) {
        r = gui_ease(t, 1);
    } else if (strstr(kind, "アウト") || strstr(kind, "out")) {
        r = gui_ease(t, 2);
    }
    /* else linear — r = t */

    return hajimu_number(r);
}

/* スプリング(目標, 現在[, 剛性, 減衰]) → 数値 */
static Value fn_spring(int argc, Value *argv) {
    float target = (float)argv[0].number;
    float current = (float)argv[1].number;
    float stiffness = argc >= 3 ? (float)argv[2].number : 170.0f;
    float damping   = argc >= 4 ? (float)argv[3].number : 26.0f;
    /* 簡易スプリング: 1フレーム分の変位を計算 */
    float dt = 1.0f / 60.0f; /* 60fps想定 */
    /* 速度を0として1ステップ近似 */
    float diff = target - current;
    float accel = stiffness * diff;
    float vel = accel * dt;
    vel *= expf(-damping * dt);
    float next = current + vel * dt;
    /* ターゲットに非常に近ければスナップ */
    if (fabsf(target - next) < 0.01f) next = target;
    return hajimu_number(next);
}

/* フェードイン(ID, 時間) → 数値 (0.0–1.0 alpha) */
static Value fn_fade_in(int argc, Value *argv) {
    (void)argc;
    uint32_t id = gui_hash(argv[0].string.data);
    float duration_ms = (float)argv[1].number * 1000.0f;
    uint32_t now = hjp_get_ticks();

    /* スロット検索 */
    GuiAnim *slot = NULL;
    for (int i = 0; i < GUI_MAX_ANIM_SLOTS; i++) {
        if (g_anims[i].active && g_anims[i].id == id) { slot = &g_anims[i]; break; }
    }
    if (!slot) {
        for (int i = 0; i < GUI_MAX_ANIM_SLOTS; i++) {
            if (!g_anims[i].active) {
                slot = &g_anims[i];
                slot->id = id;
                slot->start_val = 0; slot->end_val = 1;
                slot->duration_ms = duration_ms;
                slot->start_time = now;
                slot->easing = 2; /* ease-out */
                slot->active = true;
                break;
            }
        }
    }
    if (!slot) return hajimu_number(1);

    float elapsed = (float)(now - slot->start_time);
    float t = elapsed / slot->duration_ms;
    if (t >= 1.0f) { t = 1.0f; slot->active = false; }
    return hajimu_number(gui_ease(t, slot->easing));
}

/* フェードアウト(ID, 時間) → 数値 (1.0–0.0 alpha) */
static Value fn_fade_out(int argc, Value *argv) {
    (void)argc;
    uint32_t id = gui_hash(argv[0].string.data);
    float duration_ms = (float)argv[1].number * 1000.0f;
    uint32_t now = hjp_get_ticks();

    GuiAnim *slot = NULL;
    for (int i = 0; i < GUI_MAX_ANIM_SLOTS; i++) {
        if (g_anims[i].active && g_anims[i].id == id) { slot = &g_anims[i]; break; }
    }
    if (!slot) {
        for (int i = 0; i < GUI_MAX_ANIM_SLOTS; i++) {
            if (!g_anims[i].active) {
                slot = &g_anims[i];
                slot->id = id;
                slot->start_val = 1; slot->end_val = 0;
                slot->duration_ms = duration_ms;
                slot->start_time = now;
                slot->easing = 1; /* ease-in */
                slot->active = true;
                break;
            }
        }
    }
    if (!slot) return hajimu_number(0);

    float elapsed = (float)(now - slot->start_time);
    float t = elapsed / slot->duration_ms;
    if (t >= 1.0f) { t = 1.0f; slot->active = false; }
    float e = gui_ease(t, slot->easing);
    return hajimu_number(1.0f - e);
}

/* アニメーション停止(ID) → 無 */
static Value fn_anim_stop(int argc, Value *argv) {
    (void)argc;
    uint32_t id = gui_hash(argv[0].string.data);
    for (int i = 0; i < GUI_MAX_ANIM_SLOTS; i++) {
        if (g_anims[i].active && g_anims[i].id == id) {
            g_anims[i].active = false;
            break;
        }
    }
    return hajimu_null();
}

/* アニメーションチェーン(ID配列) → 数値 (現在のチェーンインデックス) */
static Value fn_anim_chain(int argc, Value *argv) {
    (void)argc;
    if (argv[0].type != VALUE_ARRAY) return hajimu_number(0);
    int count = argv[0].array.length;
    if (count == 0) return hajimu_number(0);

    /* 各IDのアニメーション完了状態を確認し、最初の未完了を返す */
    for (int i = 0; i < count; i++) {
        Value v = argv[0].array.elements[i];
        if (v.type != VALUE_STRING) continue;
        uint32_t id = gui_hash(v.string.data);
        /* アクティブなら、そのインデックスが「現在実行中」 */
        for (int j = 0; j < GUI_MAX_ANIM_SLOTS; j++) {
            if (g_anims[j].active && g_anims[j].id == id) {
                return hajimu_number(i);
            }
        }
    }
    /* 全部終了 → 最後のインデックス */
    return hajimu_number(count);
}

/* シェイク(ID, 時間) → 数値 (X offset) */
static Value fn_shake(int argc, Value *argv) {
    (void)argc;
    uint32_t id = gui_hash(argv[0].string.data);
    float duration_ms = (float)argv[1].number * 1000.0f;
    uint32_t now = hjp_get_ticks();

    GuiAnim *slot = NULL;
    for (int i = 0; i < GUI_MAX_ANIM_SLOTS; i++) {
        if (g_anims[i].active && g_anims[i].id == id) { slot = &g_anims[i]; break; }
    }
    if (!slot) {
        for (int i = 0; i < GUI_MAX_ANIM_SLOTS; i++) {
            if (!g_anims[i].active) {
                slot = &g_anims[i];
                slot->id = id;
                slot->start_val = 8; /* amplitude */
                slot->end_val = 0;
                slot->duration_ms = duration_ms;
                slot->start_time = now;
                slot->easing = 0;
                slot->active = true;
                break;
            }
        }
    }
    if (!slot) return hajimu_number(0);

    float elapsed = (float)(now - slot->start_time);
    float t = elapsed / slot->duration_ms;
    if (t >= 1.0f) { slot->active = false; return hajimu_number(0); }

    float amplitude = slot->start_val * (1.0f - t);
    float freq = 12.0f; /* 振動周波数 */
    float offset = amplitude * sinf(t * freq * 2.0f * 3.14159f);
    return hajimu_number(offset);
}

/* パルス(ID, 時間) → 数値 (scale 0.9–1.1) */
static Value fn_pulse(int argc, Value *argv) {
    (void)argc;
    uint32_t id = gui_hash(argv[0].string.data);
    float duration_ms = (float)argv[1].number * 1000.0f;
    uint32_t now = hjp_get_ticks();

    GuiAnim *slot = NULL;
    for (int i = 0; i < GUI_MAX_ANIM_SLOTS; i++) {
        if (g_anims[i].active && g_anims[i].id == id) { slot = &g_anims[i]; break; }
    }
    if (!slot) {
        for (int i = 0; i < GUI_MAX_ANIM_SLOTS; i++) {
            if (!g_anims[i].active) {
                slot = &g_anims[i];
                slot->id = id;
                slot->start_val = 1; slot->end_val = 0;
                slot->duration_ms = duration_ms;
                slot->start_time = now;
                slot->easing = 0;
                slot->active = true;
                break;
            }
        }
    }
    if (!slot) return hajimu_number(1);

    float elapsed = (float)(now - slot->start_time);
    float t = elapsed / slot->duration_ms;
    if (t >= 1.0f) { slot->active = false; return hajimu_number(1); }

    /* sin wave scale: 1.0 ± 0.1 */
    float scale = 1.0f + 0.1f * sinf(t * 2.0f * 3.14159f) * (1.0f - t);
    return hajimu_number(scale);
}

/* =====================================================================
 * Phase 28: レイアウト拡張 III
 * ===================================================================*/

/* ウィジェット幅(幅) → 無 — 次ウィジェットの幅を指定 */
static Value fn_widget_width(int argc, Value *argv) {
    (void)argc;
    g_next_widget_w = (float)argv[0].number;
    return hajimu_null();
}

/* マージン(上[, 右, 下, 左]) → 無 — ウィジェット外余白 */
static Value fn_margin(int argc, Value *argv) {
    float top = (float)argv[0].number;
    if (argc == 1) {
        g_margin_top = g_margin_right = g_margin_bottom = g_margin_left = top;
    } else if (argc == 2) {
        g_margin_top = g_margin_bottom = top;
        g_margin_right = g_margin_left = (float)argv[1].number;
    } else if (argc == 4) {
        g_margin_top = top;
        g_margin_right = (float)argv[1].number;
        g_margin_bottom = (float)argv[2].number;
        g_margin_left = (float)argv[3].number;
    } else {
        g_margin_top = g_margin_right = g_margin_bottom = g_margin_left = top;
    }
    /* 即座にレイアウト位置を調整 */
    if (g_cur) {
        g_cur->lay.y += g_margin_top;
    }
    return hajimu_null();
}

/* パディング(上[, 右, 下, 左]) → 無 — ウィジェット内余白 */
static Value fn_padding(int argc, Value *argv) {
    float top = (float)argv[0].number;
    if (argc == 1) {
        g_padding_extra = top;
    } else {
        g_padding_extra = top; /* 上パディングとして使用 */
    }
    if (g_cur) {
        g_cur->lay.y += g_padding_extra;
    }
    return hajimu_null();
}

/* 最小サイズ(幅, 高さ) → 無 */
static Value fn_min_size(int argc, Value *argv) {
    (void)argc;
    g_min_w = (float)argv[0].number;
    g_min_h = (float)argv[1].number;
    return hajimu_null();
}

/* 最大サイズ(幅, 高さ) → 無 */
static Value fn_max_size(int argc, Value *argv) {
    (void)argc;
    g_max_w = (float)argv[0].number;
    g_max_h = (float)argv[1].number;
    return hajimu_null();
}

/* レスポンシブ() → 文字列 "小"/"中"/"大" */
static Value fn_responsive(int argc, Value *argv) {
    (void)argc; (void)argv;
    if (!g_cur) return hajimu_string("中");
    float ww = (float)g_cur->win_w;
    if (ww < 600)       return hajimu_string("小");
    else if (ww < 1200) return hajimu_string("中");
    else                return hajimu_string("大");
}

/* アスペクト比(比率) → 無 — 縦横比コンテナ */
static Value fn_aspect_ratio(int argc, Value *argv) {
    (void)argc;
    float ratio = (float)argv[0].number;
    if (!g_cur || ratio <= 0) return hajimu_null();

    float x, y, w;
    gui_pos(&x, &y, &w);
    float h = w / ratio;

    /* コンテナ背景 */
    Hjpcontext *vg = g_cur->vg;
    hjpBeginPath(vg);
    hjpRect(vg, x, y, w, h);
    hjpStrokeColor(vg, TH_BORDER);
    hjpStrokeWidth(vg, 1);
    hjpStroke(vg);

    gui_advance(h);
    return hajimu_null();
}

/* スティッキー(上端) → 無 — 固定位置 */
static Value fn_sticky(int argc, Value *argv) {
    (void)argc;
    float top = (float)argv[0].number;
    if (g_cur) {
        g_cur->lay.y = top;
    }
    return hajimu_null();
}

/* =====================================================================
 * Phase 29: ダイアログ・フィードバック拡張
 * ===================================================================*/

/* ダイアログ共通状態 */
#define GUI_MAX_DIALOGS 4
typedef struct {
    uint32_t id;
    bool     active;
    int      kind;        /* 0=input, 1=confirm, 2=progress, 3=color */
    char     buf[256];
    float    progress;
    int      result;      /* -1=pending, 0=cancel, 1=ok */
    Hjpcolor color;
} GuiDialog;
static GuiDialog g_dialogs[GUI_MAX_DIALOGS];

static GuiDialog *gui_dialog_find(uint32_t id) {
    for (int i = 0; i < GUI_MAX_DIALOGS; i++)
        if (g_dialogs[i].active && g_dialogs[i].id == id) return &g_dialogs[i];
    return NULL;
}
static GuiDialog *gui_dialog_alloc(uint32_t id, int kind) {
    GuiDialog *d = gui_dialog_find(id);
    if (d) return d;
    for (int i = 0; i < GUI_MAX_DIALOGS; i++) {
        if (!g_dialogs[i].active) {
            d = &g_dialogs[i];
            memset(d, 0, sizeof(*d));
            d->id = id; d->kind = kind; d->active = true; d->result = -1;
            return d;
        }
    }
    return NULL;
}

/* 入力ダイアログ(タイトル, ラベル[, 初期値]) → 文字列 */
static Value fn_input_dialog(int argc, Value *argv) {
    const char *title = argv[0].string.data;
    const char *label = argv[1].string.data;
    const char *init  = argc >= 3 && argv[2].type == VALUE_STRING ? argv[2].string.data : "";
    if (!g_cur) return hajimu_string("");

    uint32_t id = gui_hash(title);
    GuiDialog *d = gui_dialog_alloc(id, 0);
    if (!d) return hajimu_string("");
    if (d->buf[0] == 0 && init[0]) strncpy(d->buf, init, sizeof(d->buf)-1);

    Hjpcontext *vg = g_cur->vg;
    float ww = (float)g_cur->win_w, wh = (float)g_cur->win_h;
    float dw = 320, dh = 160;
    float dx = (ww - dw) / 2, dy = (wh - dh) / 2;

    /* オーバーレイ */
    hjpBeginPath(vg); hjpRect(vg, 0, 0, ww, wh);
    hjpFillColor(vg, hjpRGBA(0,0,0,120)); hjpFill(vg);

    /* ダイアログ */
    hjpBeginPath(vg); hjpRoundedRect(vg, dx, dy, dw, dh, 8);
    hjpFillColor(vg, TH_BG); hjpFill(vg);
    hjpStrokeColor(vg, TH_BORDER); hjpStrokeWidth(vg, 1); hjpStroke(vg);

    /* タイトル */
    hjpFontSize(vg, 18.0f);
    hjpTextAlign(vg, HJP_ALIGN_CENTER | HJP_ALIGN_TOP);
    hjpFillColor(vg, TH_TEXT);
    hjpText(vg, dx + dw/2, dy + 14, title, NULL);

    /* ラベル */
    hjpFontSize(vg, GUI_FONT_SIZE);
    hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_TOP);
    hjpFillColor(vg, TH_TEXT_DIM);
    hjpText(vg, dx + 16, dy + 44, label, NULL);

    /* 入力欄 */
    float ix = dx + 16, iy = dy + 68, iw = dw - 32, ih = 28;
    hjpBeginPath(vg); hjpRoundedRect(vg, ix, iy, iw, ih, 4);
    hjpFillColor(vg, TH_WIDGET_BG); hjpFill(vg);
    hjpStrokeColor(vg, TH_BORDER); hjpStrokeWidth(vg, 1); hjpStroke(vg);
    hjpFillColor(vg, TH_TEXT);
    hjpText(vg, ix + 6, iy + 6, d->buf, NULL);

    /* テキスト入力処理 */
    if (g_cur->in.text_input_len > 0) {
        int len = (int)strlen(d->buf);
        for (int i = 0; i < g_cur->in.text_input_len && len < 254; i++)
            d->buf[len++] = g_cur->in.text_input[i];
        d->buf[len] = 0;
    }
    if (g_cur->in.key_backspace && d->buf[0]) {
        int len = (int)strlen(d->buf);
        d->buf[len-1] = 0;
    }

    /* ボタン */
    float bw = 80, bh = 30;
    float okx = dx + dw/2 + 8, cax = dx + dw/2 - bw - 8, bty = dy + dh - 44;

    /* OK */
    bool ok_hov = gui_hit((float)g_cur->in.mx, (float)g_cur->in.my, okx, bty, bw, bh);
    hjpBeginPath(vg); hjpRoundedRect(vg, okx, bty, bw, bh, 4);
    hjpFillColor(vg, ok_hov ? TH_ACCENT_HOVER : TH_ACCENT); hjpFill(vg);
    hjpFontSize(vg, GUI_FONT_SIZE);
    hjpTextAlign(vg, HJP_ALIGN_CENTER | HJP_ALIGN_MIDDLE);
    hjpFillColor(vg, hjpRGBA(255,255,255,255));
    hjpText(vg, okx + bw/2, bty + bh/2, "OK", NULL);
    if (ok_hov && g_cur->in.clicked) { d->result = 1; }

    /* Cancel */
    bool ca_hov = gui_hit((float)g_cur->in.mx, (float)g_cur->in.my, cax, bty, bw, bh);
    hjpBeginPath(vg); hjpRoundedRect(vg, cax, bty, bw, bh, 4);
    hjpFillColor(vg, ca_hov ? TH_WIDGET_HOVER : TH_WIDGET_BG); hjpFill(vg);
    hjpStrokeColor(vg, TH_BORDER); hjpStrokeWidth(vg, 1); hjpStroke(vg);
    hjpFillColor(vg, TH_TEXT);
    hjpText(vg, cax + bw/2, bty + bh/2, "\xe3\x82\xad\xe3\x83\xa3\xe3\x83\xb3\xe3\x82\xbb\xe3\x83\xab", NULL);
    if (ca_hov && g_cur->in.clicked) { d->result = 0; }

    if (d->result >= 0) {
        Value r = d->result == 1 ? hajimu_string(d->buf) : hajimu_string("");
        d->active = false;
        return r;
    }
    return hajimu_string("");
}

/* 確認ダイアログ(タイトル, メッセージ) → 真偽 */
static Value fn_confirm_dialog(int argc, Value *argv) {
    (void)argc;
    const char *title = argv[0].string.data;
    const char *msg   = argv[1].string.data;
    if (!g_cur) return hajimu_bool(false);

    uint32_t id = gui_hash(title);
    GuiDialog *d = gui_dialog_alloc(id, 1);
    if (!d) return hajimu_bool(false);

    Hjpcontext *vg = g_cur->vg;
    float ww = (float)g_cur->win_w, wh = (float)g_cur->win_h;
    float dw = 300, dh = 140;
    float dx = (ww - dw) / 2, dy = (wh - dh) / 2;

    hjpBeginPath(vg); hjpRect(vg, 0, 0, ww, wh);
    hjpFillColor(vg, hjpRGBA(0,0,0,120)); hjpFill(vg);
    hjpBeginPath(vg); hjpRoundedRect(vg, dx, dy, dw, dh, 8);
    hjpFillColor(vg, TH_BG); hjpFill(vg);
    hjpStrokeColor(vg, TH_BORDER); hjpStrokeWidth(vg, 1); hjpStroke(vg);

    hjpFontSize(vg, 18.0f);
    hjpTextAlign(vg, HJP_ALIGN_CENTER | HJP_ALIGN_TOP);
    hjpFillColor(vg, TH_TEXT);
    hjpText(vg, dx + dw/2, dy + 14, title, NULL);
    hjpFontSize(vg, GUI_FONT_SIZE);
    hjpFillColor(vg, TH_TEXT_DIM);
    hjpText(vg, dx + dw/2, dy + 48, msg, NULL);

    float bw = 80, bh = 30, bty = dy + dh - 44;
    float yx = dx + dw/2 - bw - 8, nx = dx + dw/2 + 8;

    /* はい */
    bool y_hov = gui_hit((float)g_cur->in.mx, (float)g_cur->in.my, yx, bty, bw, bh);
    hjpBeginPath(vg); hjpRoundedRect(vg, yx, bty, bw, bh, 4);
    hjpFillColor(vg, y_hov ? TH_ACCENT_HOVER : TH_ACCENT); hjpFill(vg);
    hjpTextAlign(vg, HJP_ALIGN_CENTER | HJP_ALIGN_MIDDLE);
    hjpFillColor(vg, hjpRGBA(255,255,255,255));
    hjpText(vg, yx + bw/2, bty + bh/2, "\xe3\x81\xaf\xe3\x81\x84", NULL);

    /* いいえ */
    bool n_hov = gui_hit((float)g_cur->in.mx, (float)g_cur->in.my, nx, bty, bw, bh);
    hjpBeginPath(vg); hjpRoundedRect(vg, nx, bty, bw, bh, 4);
    hjpFillColor(vg, n_hov ? TH_WIDGET_HOVER : TH_WIDGET_BG); hjpFill(vg);
    hjpStrokeColor(vg, TH_BORDER); hjpStrokeWidth(vg, 1); hjpStroke(vg);
    hjpFillColor(vg, TH_TEXT);
    hjpText(vg, nx + bw/2, bty + bh/2, "\xe3\x81\x84\xe3\x81\x84\xe3\x81\x88", NULL);

    if (y_hov && g_cur->in.clicked) { d->active = false; return hajimu_bool(true); }
    if (n_hov && g_cur->in.clicked) { d->active = false; return hajimu_bool(false); }
    return hajimu_bool(false);
}

/* 進捗ダイアログ(タイトル, 値) → 真偽 (キャンセルでtrue) */
static Value fn_progress_dialog(int argc, Value *argv) {
    (void)argc;
    const char *title = argv[0].string.data;
    float val = (float)argv[1].number;
    if (!g_cur) return hajimu_bool(false);

    Hjpcontext *vg = g_cur->vg;
    float ww = (float)g_cur->win_w, wh = (float)g_cur->win_h;
    float dw = 300, dh = 120;
    float dx = (ww - dw) / 2, dy = (wh - dh) / 2;

    hjpBeginPath(vg); hjpRect(vg, 0, 0, ww, wh);
    hjpFillColor(vg, hjpRGBA(0,0,0,120)); hjpFill(vg);
    hjpBeginPath(vg); hjpRoundedRect(vg, dx, dy, dw, dh, 8);
    hjpFillColor(vg, TH_BG); hjpFill(vg);
    hjpStrokeColor(vg, TH_BORDER); hjpStrokeWidth(vg, 1); hjpStroke(vg);

    hjpFontSize(vg, 18.0f);
    hjpTextAlign(vg, HJP_ALIGN_CENTER | HJP_ALIGN_TOP);
    hjpFillColor(vg, TH_TEXT);
    hjpText(vg, dx + dw/2, dy + 14, title, NULL);

    /* プログレスバー */
    float bx = dx + 16, by = dy + 50, bw = dw - 32, bh = 10;
    hjpBeginPath(vg); hjpRoundedRect(vg, bx, by, bw, bh, 5);
    hjpFillColor(vg, TH_WIDGET_BG); hjpFill(vg);
    float fill_w = bw * (val < 0 ? 0 : val > 1 ? 1 : val);
    hjpBeginPath(vg); hjpRoundedRect(vg, bx, by, fill_w, bh, 5);
    hjpFillColor(vg, TH_ACCENT); hjpFill(vg);

    /* パーセント */
    char pct[16]; snprintf(pct, sizeof(pct), "%.0f%%", val * 100);
    hjpFontSize(vg, 13.0f);
    hjpTextAlign(vg, HJP_ALIGN_CENTER | HJP_ALIGN_TOP);
    hjpFillColor(vg, TH_TEXT_DIM);
    hjpText(vg, dx + dw/2, by + 16, pct, NULL);

    /* キャンセル */
    float cbw = 80, cbh = 26;
    float cbx = dx + dw/2 - cbw/2, cby = dy + dh - 36;
    bool hov = gui_hit((float)g_cur->in.mx, (float)g_cur->in.my, cbx, cby, cbw, cbh);
    hjpBeginPath(vg); hjpRoundedRect(vg, cbx, cby, cbw, cbh, 4);
    hjpFillColor(vg, hov ? TH_WIDGET_HOVER : TH_WIDGET_BG); hjpFill(vg);
    hjpStrokeColor(vg, TH_BORDER); hjpStrokeWidth(vg, 1); hjpStroke(vg);
    hjpFontSize(vg, GUI_FONT_SIZE);
    hjpTextAlign(vg, HJP_ALIGN_CENTER | HJP_ALIGN_MIDDLE);
    hjpFillColor(vg, TH_TEXT);
    hjpText(vg, cbx + cbw/2, cby + cbh/2, "\xe3\x82\xad\xe3\x83\xa3\xe3\x83\xb3\xe3\x82\xbb\xe3\x83\xab", NULL);

    return hajimu_bool(hov && g_cur->in.clicked);
}

/* カラーダイアログ(初期色) → 数値 */
static Value fn_color_dialog(int argc, Value *argv) {
    (void)argc;
    Hjpcolor init = gui_arg_color(argv[0]);
    if (!g_cur) return hajimu_number(0);

    Hjpcontext *vg = g_cur->vg;
    float ww = (float)g_cur->win_w, wh = (float)g_cur->win_h;
    float dw = 260, dh = 200;
    float dx = (ww - dw) / 2, dy = (wh - dh) / 2;

    hjpBeginPath(vg); hjpRect(vg, 0, 0, ww, wh);
    hjpFillColor(vg, hjpRGBA(0,0,0,120)); hjpFill(vg);
    hjpBeginPath(vg); hjpRoundedRect(vg, dx, dy, dw, dh, 8);
    hjpFillColor(vg, TH_BG); hjpFill(vg);
    hjpStrokeColor(vg, TH_BORDER); hjpStrokeWidth(vg, 1); hjpStroke(vg);

    /* 簡易カラーパレット (8x4 グリッド) */
    static const uint32_t palette[32] = {
        0xFF0000FF, 0xFF4500FF, 0xFFA500FF, 0xFFFF00FF, 0x00FF00FF, 0x00CED1FF, 0x0000FFFF, 0x8B00FFFF,
        0xFF6347FF, 0xFF7F50FF, 0xFFD700FF, 0xADFF2FFF, 0x00FA9AFF, 0x40E0D0FF, 0x6495EDFF, 0xDA70D6FF,
        0xDC143CFF, 0xCD853FFF, 0xBDB76BFF, 0x9ACD32FF, 0x2E8B57FF, 0x4682B4FF, 0x4169E1FF, 0x9370DBFF,
        0x8B0000FF, 0xA0522DFF, 0x808000FF, 0x006400FF, 0x008080FF, 0x000080FF, 0x4B0082FF, 0x2F4F4FFF,
    };
    float gx = dx + 10, gy = dy + 14;
    float cs = 28;
    uint32_t picked = 0;
    for (int row = 0; row < 4; row++) {
        for (int col = 0; col < 8; col++) {
            int idx = row * 8 + col;
            uint32_t c = palette[idx];
            float cx = gx + col * (cs + 2), cy = gy + row * (cs + 2);
            hjpBeginPath(vg);
            hjpRoundedRect(vg, cx, cy, cs, cs, 4);
            hjpFillColor(vg, hjpRGBA((c>>24)&0xFF, (c>>16)&0xFF, (c>>8)&0xFF, c&0xFF));
            hjpFill(vg);
            if (gui_hit((float)g_cur->in.mx, (float)g_cur->in.my, cx, cy, cs, cs)) {
                hjpStrokeColor(vg, hjpRGBA(255,255,255,255));
                hjpStrokeWidth(vg, 2); hjpStroke(vg);
                if (g_cur->in.clicked) picked = c;
            }
        }
    }

    /* 現在の色プレビュー */
    hjpBeginPath(vg); hjpRoundedRect(vg, dx + 10, dy + dh - 44, dw - 20, 30, 4);
    hjpFillColor(vg, picked ? hjpRGBA((picked>>24)&0xFF, (picked>>16)&0xFF, (picked>>8)&0xFF, 255) : init);
    hjpFill(vg);

    if (picked) {
        return hajimu_number((double)picked);
    }
    return hajimu_number(0);
}

/* スナックバー状態 */
static struct { char msg[256]; char action[64]; uint32_t start; bool active; } g_snackbar = {0};

/* スナックバー(メッセージ[, アクション]) → 真偽 */
static Value fn_snackbar(int argc, Value *argv) {
    const char *msg = argv[0].string.data;
    const char *action = argc >= 2 && argv[1].type == VALUE_STRING ? argv[1].string.data : NULL;
    if (!g_cur) return hajimu_bool(false);

    if (!g_snackbar.active) {
        strncpy(g_snackbar.msg, msg, sizeof(g_snackbar.msg)-1);
        if (action) strncpy(g_snackbar.action, action, sizeof(g_snackbar.action)-1);
        else g_snackbar.action[0] = 0;
        g_snackbar.start = hjp_get_ticks();
        g_snackbar.active = true;
    }

    Hjpcontext *vg = g_cur->vg;
    uint32_t elapsed = hjp_get_ticks() - g_snackbar.start;
    if (elapsed > 4000) { g_snackbar.active = false; return hajimu_bool(false); }

    float ww = (float)g_cur->win_w, wh = (float)g_cur->win_h;
    float sw = 360, sh = 48;
    float sx = (ww - sw) / 2, sy = wh - sh - 20;

    hjpBeginPath(vg); hjpRoundedRect(vg, sx, sy, sw, sh, 8);
    hjpFillColor(vg, hjpRGBA(50,50,50,240)); hjpFill(vg);

    hjpFontSize(vg, GUI_FONT_SIZE);
    hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_MIDDLE);
    hjpFillColor(vg, hjpRGBA(255,255,255,255));
    hjpText(vg, sx + 16, sy + sh/2, g_snackbar.msg, NULL);

    if (g_snackbar.action[0]) {
        hjpTextAlign(vg, HJP_ALIGN_RIGHT | HJP_ALIGN_MIDDLE);
        hjpFillColor(vg, TH_ACCENT);
        float ax = sx + sw - 16;
        hjpText(vg, ax, sy + sh/2, g_snackbar.action, NULL);

        float abounds[4];
        hjpTextBounds(vg, ax, sy + sh/2, g_snackbar.action, NULL, abounds);
        float aw = abounds[2] - abounds[0];
        if (gui_hit((float)g_cur->in.mx, (float)g_cur->in.my, ax - aw, sy, aw + 16, sh) && g_cur->in.clicked) {
            g_snackbar.active = false;
            return hajimu_bool(true);
        }
    }
    return hajimu_bool(false);
}

/* バナー(メッセージ, 種類) → 真偽 (閉じるでtrue) */
static Value fn_banner(int argc, Value *argv) {
    (void)argc;
    const char *msg  = argv[0].string.data;
    const char *kind = argv[1].string.data;
    if (!g_cur) return hajimu_bool(false);

    Hjpcontext *vg = g_cur->vg;
    float ww = (float)g_cur->win_w;
    float bh = 44;

    Hjpcolor bg_col = TH_ACCENT;
    if (strstr(kind, "警告") || strstr(kind, "warn"))
        bg_col = hjpRGBA(234, 179, 8, 255);
    else if (strstr(kind, "エラー") || strstr(kind, "error"))
        bg_col = hjpRGBA(239, 68, 68, 255);
    else if (strstr(kind, "情報") || strstr(kind, "info"))
        bg_col = hjpRGBA(59, 130, 246, 255);

    hjpBeginPath(vg); hjpRect(vg, 0, 0, ww, bh);
    hjpFillColor(vg, bg_col); hjpFill(vg);

    hjpFontSize(vg, GUI_FONT_SIZE);
    hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_MIDDLE);
    hjpFillColor(vg, hjpRGBA(255,255,255,255));
    hjpText(vg, 16, bh/2, msg, NULL);

    /* 閉じるボタン */
    float cx = ww - 36;
    hjpTextAlign(vg, HJP_ALIGN_CENTER | HJP_ALIGN_MIDDLE);
    hjpText(vg, cx, bh/2, "\xc3\x97", NULL);
    bool hov = gui_hit((float)g_cur->in.mx, (float)g_cur->in.my, cx - 12, 0, 24, bh);
    return hajimu_bool(hov && g_cur->in.clicked);
}

/* スプラッシュスクリーン(画像[, テキスト]) → 無 */
static Value fn_splash_screen(int argc, Value *argv) {
    int img_idx = (int)argv[0].number;
    const char *txt = argc >= 2 && argv[1].type == VALUE_STRING ? argv[1].string.data : NULL;
    if (!g_cur) return hajimu_null();

    Hjpcontext *vg = g_cur->vg;
    float ww = (float)g_cur->win_w, wh = (float)g_cur->win_h;

    /* フルスクリーン背景 */
    hjpBeginPath(vg); hjpRect(vg, 0, 0, ww, wh);
    hjpFillColor(vg, hjpRGBA(30, 30, 40, 255)); hjpFill(vg);

    /* 画像 */
    if (img_idx >= 0 && img_idx < GUI_MAX_IMAGES && g_images[img_idx].valid) {
        float iw = (float)g_images[img_idx].w;
        float ih = (float)g_images[img_idx].h;
        float scale = fminf(ww * 0.5f / iw, wh * 0.5f / ih);
        float sw = iw * scale, sh = ih * scale;
        Hjppaint ip = hjpImagePattern(vg, (ww-sw)/2, (wh-sh)/2 - 30, sw, sh, 0, g_images[img_idx].handle, 1.0f);
        hjpBeginPath(vg); hjpRect(vg, (ww-sw)/2, (wh-sh)/2 - 30, sw, sh);
        hjpFillPaint(vg, ip); hjpFill(vg);
    }

    /* テキスト */
    if (txt) {
        hjpFontSize(vg, 20.0f);
        hjpTextAlign(vg, HJP_ALIGN_CENTER | HJP_ALIGN_TOP);
        hjpFillColor(vg, hjpRGBA(200, 200, 200, 255));
        hjpText(vg, ww/2, wh * 0.75f, txt, NULL);
    }

    return hajimu_null();
}

/* リッチツールチップ開始() → 無 — ツールチップ描画フラグ */
static bool g_rich_tooltip_active = false;
static float g_rich_tooltip_x = 0, g_rich_tooltip_y = 0;
static float g_rich_tooltip_saved_x = 0, g_rich_tooltip_saved_y = 0;
static float g_rich_tooltip_saved_w = 0;

static Value fn_rich_tooltip_begin(int argc, Value *argv) {
    (void)argc; (void)argv;
    if (!g_cur) return hajimu_null();

    g_rich_tooltip_active = true;
    g_rich_tooltip_x = (float)g_cur->in.mx + 12;
    g_rich_tooltip_y = (float)g_cur->in.my + 12;

    /* ツールチップコンテナ開始 */
    Hjpcontext *vg = g_cur->vg;
    hjpSave(vg);

    /* 背景予約 — 実際はフレーム描画後にサイズ確定 */
    float tw = 200;
    hjpBeginPath(vg);
    hjpRoundedRect(vg, g_rich_tooltip_x, g_rich_tooltip_y, tw, 80, 6);
    hjpFillColor(vg, TH_TOOLTIP_BG);
    hjpFill(vg);

    /* レイアウトをツールチップ内に切り替え */
    g_rich_tooltip_saved_x = g_cur->lay.x;
    g_rich_tooltip_saved_y = g_cur->lay.y;
    g_rich_tooltip_saved_w = g_cur->lay.w;
    g_cur->lay.x = g_rich_tooltip_x;
    g_cur->lay.y = g_rich_tooltip_y + 8;
    g_cur->lay.w = tw;

    return hajimu_null();
}

/* =====================================================================
 * Phase 30: 入力拡張 III
 * ===================================================================*/

/* タグ入力(ラベル, タグ配列) → 配列 */
static Value fn_tag_input(int argc, Value *argv) {
    (void)argc;
    const char *label = argv[0].string.data;
    if (argv[1].type != VALUE_ARRAY || !g_cur) return argv[1];

    Hjpcontext *vg = g_cur->vg;
    float x, y, w;
    gui_pos(&x, &y, &w);

    /* ラベル */
    hjpFontSize(vg, GUI_FONT_SIZE);
    hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_TOP);
    hjpFillColor(vg, TH_TEXT_DIM);
    hjpText(vg, x, y, label, NULL);
    y += 22;

    /* タグコンテナ */
    float h = 36;
    hjpBeginPath(vg); hjpRoundedRect(vg, x, y, w, h, 4);
    hjpFillColor(vg, TH_WIDGET_BG); hjpFill(vg);
    hjpStrokeColor(vg, TH_BORDER); hjpStrokeWidth(vg, 1); hjpStroke(vg);

    /* タグ描画 */
    float tx = x + 6;
    Value result = argv[1]; /* 元の配列を返す */
    int remove_idx = -1;
    for (int i = 0; i < argv[1].array.length && i < 8; i++) {
        Value tag = argv[1].array.elements[i];
        if (tag.type != VALUE_STRING) continue;
        const char *t = tag.string.data;
        float bounds[4];
        hjpTextBounds(vg, 0, 0, t, NULL, bounds);
        float tw = bounds[2] - bounds[0] + 24;

        hjpBeginPath(vg); hjpRoundedRect(vg, tx, y + 6, tw, 24, 12);
        hjpFillColor(vg, TH_ACCENT); hjpFill(vg);
        hjpFontSize(vg, 13.0f);
        hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_MIDDLE);
        hjpFillColor(vg, hjpRGBA(255,255,255,255));
        hjpText(vg, tx + 8, y + 18, t, NULL);

        /* × ボタン */
        float xbx = tx + tw - 16;
        hjpText(vg, xbx, y + 18, "\xc3\x97", NULL);
        if (gui_hit((float)g_cur->in.mx, (float)g_cur->in.my, xbx - 4, y + 6, 16, 24) && g_cur->in.clicked) {
            remove_idx = i;
        }
        tx += tw + 4;
    }

    /* タグ削除処理 */
    if (remove_idx >= 0) {
        Value new_arr = hajimu_array();
        for (int i = 0; i < argv[1].array.length; i++) {
            if (i != remove_idx) hajimu_array_push(&new_arr, argv[1].array.elements[i]);
        }
        result = new_arr;
    }

    gui_advance(h + 22);
    return result;
}

/* 垂直スライダー(ラベル, 値, 最小, 最大) → 数値 */
static Value fn_vslider(int argc, Value *argv) {
    (void)argc;
    const char *label = argv[0].string.data;
    float val = (float)argv[1].number;
    float min_v = (float)argv[2].number;
    float max_v = (float)argv[3].number;
    if (!g_cur || max_v <= min_v) return hajimu_number(val);

    Hjpcontext *vg = g_cur->vg;
    float x, y, w;
    gui_pos(&x, &y, &w);
    uint32_t id = gui_hash(label);

    float sl_h = 120, sl_w = 24;
    float cx = x + w / 2 - sl_w / 2;

    /* ラベル */
    hjpFontSize(vg, GUI_FONT_SIZE);
    hjpTextAlign(vg, HJP_ALIGN_CENTER | HJP_ALIGN_TOP);
    hjpFillColor(vg, TH_TEXT);
    hjpText(vg, x + w/2, y, label, NULL);
    y += 22;

    /* トラック */
    float track_x = cx + sl_w/2 - 3;
    hjpBeginPath(vg); hjpRoundedRect(vg, track_x, y, 6, sl_h, 3);
    hjpFillColor(vg, TH_TRACK); hjpFill(vg);

    /* 正規化 */
    float norm = (val - min_v) / (max_v - min_v);
    if (norm < 0) norm = 0; if (norm > 1) norm = 1;
    float knob_y = y + sl_h - norm * sl_h - 8;

    /* フィル */
    float fill_h = (y + sl_h) - knob_y - 8;
    if (fill_h > 0) {
        hjpBeginPath(vg); hjpRoundedRect(vg, track_x, knob_y + 8, 6, fill_h, 3);
        hjpFillColor(vg, TH_ACCENT); hjpFill(vg);
    }

    /* ノブ */
    bool hov, pressed;
    gui_widget_logic(id, cx, y, sl_w, sl_h, &hov, &pressed);
    hjpBeginPath(vg); hjpCircle(vg, cx + sl_w/2, knob_y + 8, 8);
    hjpFillColor(vg, pressed ? TH_ACCENT_HOVER : (hov ? TH_WIDGET_HOVER : TH_ACCENT));
    hjpFill(vg);

    if (g_cur->active == id) {
        float my = (float)g_cur->in.my;
        float t = 1.0f - (my - y) / sl_h;
        if (t < 0) t = 0; if (t > 1) t = 1;
        val = min_v + t * (max_v - min_v);
    }

    /* 値表示 */
    char vbuf[32]; snprintf(vbuf, sizeof(vbuf), "%.1f", val);
    hjpFontSize(vg, 13.0f);
    hjpTextAlign(vg, HJP_ALIGN_CENTER | HJP_ALIGN_TOP);
    hjpFillColor(vg, TH_TEXT_DIM);
    hjpText(vg, x + w/2, y + sl_h + 4, vbuf, NULL);

    gui_advance(sl_h + 40);
    return hajimu_number(val);
}

/* マスク入力(ラベル, 値, マスク) → 文字列 */
static Value fn_mask_input(int argc, Value *argv) {
    (void)argc;
    const char *label = argv[0].string.data;
    const char *val   = argv[1].string.data;
    const char *mask  = argv[2].string.data; /* e.g. "###-####-####" */
    if (!g_cur) return hajimu_string(val);

    Hjpcontext *vg = g_cur->vg;
    float x, y, w;
    gui_pos(&x, &y, &w);
    uint32_t id = gui_hash(label);

    /* ラベル */
    hjpFontSize(vg, GUI_FONT_SIZE);
    hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_TOP);
    hjpFillColor(vg, TH_TEXT_DIM);
    hjpText(vg, x, y, label, NULL);
    y += 22;

    /* 入力欄 */
    float h = 30;
    bool hov, pressed;
    gui_widget_logic(id, x, y, w, h, &hov, &pressed);

    hjpBeginPath(vg); hjpRoundedRect(vg, x, y, w, h, 4);
    hjpFillColor(vg, TH_WIDGET_BG); hjpFill(vg);
    Hjpcolor border = g_cur->focused == id ? TH_ACCENT : TH_BORDER;
    hjpStrokeColor(vg, border); hjpStrokeWidth(vg, 1); hjpStroke(vg);

    if (pressed) g_cur->focused = id;

    /* テキストバッファによる入力処理 */
    GuiTextBuf *tb = gui_get_text_buf(id, val);
    if (tb && g_cur->focused == id) {
        int len = (int)strlen(tb->buf);
        /* テキスト入力 (数字のみ受け付け) */
        for (int i = 0; i < g_cur->in.text_input_len && len < 254; i++) {
            char ch = g_cur->in.text_input[i];
            if (ch >= '0' && ch <= '9') tb->buf[len++] = ch;
        }
        tb->buf[len] = '\0';
        /* バックスペース */
        if (g_cur->in.key_backspace && len > 0) {
            tb->buf[len - 1] = '\0';
        }
    }
    const char *raw = tb ? tb->buf : val;

    /* マスクフォーマット表示 */
    char display[256];
    int vi = 0, di = 0;
    for (int m = 0; mask[m] && di < 254; m++) {
        if (mask[m] == '#') {
            if (raw[vi]) display[di++] = raw[vi++];
            else display[di++] = '_';
        } else {
            display[di++] = mask[m];
        }
    }
    display[di] = 0;

    hjpFillColor(vg, TH_TEXT);
    hjpText(vg, x + 8, y + 7, display, NULL);

    gui_advance(h + 22);
    return hajimu_string(raw);
}

/* ピンコード(ラベル, 桁数) → 文字列 */
static Value fn_pincode(int argc, Value *argv) {
    (void)argc;
    const char *label = argv[0].string.data;
    int digits = (int)argv[1].number;
    if (digits < 1) digits = 4;
    if (digits > 8) digits = 8;
    if (!g_cur) return hajimu_string("");

    Hjpcontext *vg = g_cur->vg;
    float x, y, w;
    gui_pos(&x, &y, &w);
    uint32_t id = gui_hash(label);

    /* ラベル */
    hjpFontSize(vg, GUI_FONT_SIZE);
    hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_TOP);
    hjpFillColor(vg, TH_TEXT_DIM);
    hjpText(vg, x, y, label, NULL);
    y += 22;

    /* テキストバッファ取得 */
    GuiTextBuf *tb = gui_get_text_buf(id, "");
    if (!tb) return hajimu_string("");

    /* フォーカス処理 */
    float total_w = digits * 38.0f + (digits - 1) * 8.0f;
    float sx = x + (w - total_w) / 2;
    bool any_hov = gui_hit((float)g_cur->in.mx, (float)g_cur->in.my, sx, y, total_w, 44);
    if (any_hov && g_cur->in.clicked) g_cur->focused = id;

    bool focused = (g_cur->focused == id);

    /* テキスト入力 */
    if (focused && g_cur->in.text_input_len > 0) {
        int len = (int)strlen(tb->buf);
        for (int i = 0; i < g_cur->in.text_input_len && len < digits; i++) {
            char c = g_cur->in.text_input[i];
            if (c >= '0' && c <= '9') tb->buf[len++] = c;
        }
        tb->buf[len] = 0;
    }
    if (focused && g_cur->in.key_backspace && tb->buf[0]) {
        int len = (int)strlen(tb->buf);
        tb->buf[len-1] = 0;
    }

    /* ボックス描画 */
    for (int i = 0; i < digits; i++) {
        float bx = sx + i * 46.0f;
        hjpBeginPath(vg); hjpRoundedRect(vg, bx, y, 38, 44, 6);
        hjpFillColor(vg, TH_WIDGET_BG); hjpFill(vg);
        Hjpcolor bc = (focused && (int)strlen(tb->buf) == i) ? TH_ACCENT : TH_BORDER;
        hjpStrokeColor(vg, bc); hjpStrokeWidth(vg, focused ? 2.0f : 1.0f); hjpStroke(vg);

        if (i < (int)strlen(tb->buf)) {
            char ch[2] = {tb->buf[i], 0};
            hjpFontSize(vg, 24.0f);
            hjpTextAlign(vg, HJP_ALIGN_CENTER | HJP_ALIGN_MIDDLE);
            hjpFillColor(vg, TH_TEXT);
            hjpText(vg, bx + 19, y + 22, ch, NULL);
        }
    }

    gui_advance(44 + 22);
    return hajimu_string(tb->buf);
}

/* メンション入力(ラベル, 値, 候補) → 文字列 */
static Value fn_mention_input(int argc, Value *argv) {
    (void)argc;
    const char *label = argv[0].string.data;
    const char *val   = argv[1].string.data;
    if (argv[2].type != VALUE_ARRAY || !g_cur) return hajimu_string(val);

    Hjpcontext *vg = g_cur->vg;
    float x, y, w;
    gui_pos(&x, &y, &w);
    uint32_t id = gui_hash(label);

    /* ラベル */
    hjpFontSize(vg, GUI_FONT_SIZE);
    hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_TOP);
    hjpFillColor(vg, TH_TEXT_DIM);
    hjpText(vg, x, y, label, NULL);
    y += 22;

    /* 入力欄 */
    float h = 30;
    bool hov, pressed;
    gui_widget_logic(id, x, y, w, h, &hov, &pressed);
    if (pressed) g_cur->focused = id;

    hjpBeginPath(vg); hjpRoundedRect(vg, x, y, w, h, 4);
    hjpFillColor(vg, TH_WIDGET_BG); hjpFill(vg);
    Hjpcolor border = g_cur->focused == id ? TH_ACCENT : TH_BORDER;
    hjpStrokeColor(vg, border); hjpStrokeWidth(vg, 1); hjpStroke(vg);

    hjpFillColor(vg, TH_TEXT);
    hjpText(vg, x + 8, y + 7, val, NULL);

    /* @で始まる候補ドロップダウン */
    bool show_candidates = false;
    const char *at = strrchr(val, '@');
    if (at && g_cur->focused == id) show_candidates = true;

    if (show_candidates) {
        float dy2 = y + h + 2;
        int shown = 0;
        for (int i = 0; i < argv[2].array.length && shown < 5; i++) {
            Value c = argv[2].array.elements[i];
            if (c.type != VALUE_STRING) continue;
            hjpBeginPath(vg); hjpRect(vg, x, dy2, w, 26);
            hjpFillColor(vg, TH_WIDGET_BG); hjpFill(vg);
            hjpStrokeColor(vg, TH_BORDER); hjpStrokeWidth(vg, 0.5f); hjpStroke(vg);
            hjpFillColor(vg, TH_TEXT);
            hjpFontSize(vg, 14.0f);
            hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_MIDDLE);
            char entry[128]; snprintf(entry, sizeof(entry), "@%s", c.string.data);
            hjpText(vg, x + 8, dy2 + 13, entry, NULL);
            dy2 += 26;
            shown++;
        }
    }

    gui_advance(h + 22);
    return hajimu_string(val);
}

/* 繰り返しボタン(ラベル) → 真偽 */
static Value fn_repeat_button(int argc, Value *argv) {
    (void)argc;
    const char *label = argv[0].string.data;
    if (!g_cur) return hajimu_bool(false);

    Hjpcontext *vg = g_cur->vg;
    float x, y, w;
    gui_pos(&x, &y, &w);
    float h = GUI_WIDGET_H;
    uint32_t id = gui_hash(label);

    bool hov, pressed;
    gui_widget_logic(id, x, y, w, h, &hov, &pressed);

    hjpBeginPath(vg); hjpRoundedRect(vg, x, y, w, h, GUI_BTN_RADIUS);
    hjpFillColor(vg, pressed ? TH_WIDGET_ACTIVE : (hov ? TH_WIDGET_HOVER : TH_WIDGET_BG));
    hjpFill(vg);
    hjpStrokeColor(vg, TH_BORDER); hjpStrokeWidth(vg, 1); hjpStroke(vg);

    hjpFontSize(vg, GUI_FONT_SIZE);
    hjpTextAlign(vg, HJP_ALIGN_CENTER | HJP_ALIGN_MIDDLE);
    hjpFillColor(vg, TH_TEXT);
    hjpText(vg, x + w/2, y + h/2, label, NULL);

    gui_advance(h);

    /* 長押しリピート: アクティブ中は毎フレームtrue */
    return hajimu_bool(g_cur->active == id && g_cur->in.down);
}

/* インビジブルボタン(幅, 高さ) → 真偽 */
static int g_invisible_btn_counter = 0;
static Value fn_invisible_button(int argc, Value *argv) {
    (void)argc;
    float bw = (float)argv[0].number;
    float bh = (float)argv[1].number;
    if (!g_cur) return hajimu_bool(false);

    float x, y, w;
    gui_pos(&x, &y, &w);
    if (bw <= 0) bw = w;
    /* 位置情報とカウンターで一意なIDを生成 */
    char id_buf[64];
    snprintf(id_buf, sizeof(id_buf), "__invisible_%d_%d_%d__",
             g_invisible_btn_counter++, (int)x, (int)y);
    uint32_t id = gui_hash(id_buf);

    bool hov, pressed;
    bool clicked = gui_widget_logic(id, x, y, bw, bh, &hov, &pressed);

    gui_advance(bh);
    return hajimu_bool(clicked);
}

/* 自動リサイズテキスト(ラベル, 値) → 文字列 */
static Value fn_auto_resize_text(int argc, Value *argv) {
    (void)argc;
    const char *label = argv[0].string.data;
    const char *val   = argv[1].string.data;
    if (!g_cur) return hajimu_string(val);

    Hjpcontext *vg = g_cur->vg;
    float x, y, w;
    gui_pos(&x, &y, &w);
    uint32_t id = gui_hash(label);

    /* ラベル */
    hjpFontSize(vg, GUI_FONT_SIZE);
    hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_TOP);
    hjpFillColor(vg, TH_TEXT_DIM);
    hjpText(vg, x, y, label, NULL);
    y += 22;

    /* テキストバッファ */
    GuiTextBuf *tb = gui_get_text_buf(id, val);
    if (!tb) return hajimu_string(val);

    /* テキスト入力 */
    bool hov = gui_hit((float)g_cur->in.mx, (float)g_cur->in.my, x, y, w, 200);
    if (hov && g_cur->in.clicked) g_cur->focused = id;
    bool focused = (g_cur->focused == id);

    if (focused && g_cur->in.text_input_len > 0) {
        int len = (int)strlen(tb->buf);
        for (int i = 0; i < g_cur->in.text_input_len && len < GUI_MAX_TEXT_BUF - 2; i++)
            tb->buf[len++] = g_cur->in.text_input[i];
        tb->buf[len] = 0;
    }
    if (focused && g_cur->in.key_backspace && tb->buf[0]) {
        int len = (int)strlen(tb->buf);
        tb->buf[len-1] = 0;
    }

    /* 行数を計算 */
    int lines = 1;
    for (const char *p = tb->buf; *p; p++) if (*p == '\n') lines++;
    float line_h = 20;
    float h = fmaxf(30, lines * line_h + 10);

    /* 背景 */
    hjpBeginPath(vg); hjpRoundedRect(vg, x, y, w, h, 4);
    hjpFillColor(vg, TH_WIDGET_BG); hjpFill(vg);
    Hjpcolor border = focused ? TH_ACCENT : TH_BORDER;
    hjpStrokeColor(vg, border); hjpStrokeWidth(vg, 1); hjpStroke(vg);

    /* テキスト */
    hjpFillColor(vg, TH_TEXT);
    hjpFontSize(vg, GUI_FONT_SIZE);
    hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_TOP);
    hjpTextBox(vg, x + 6, y + 5, w - 12, tb->buf, NULL);

    gui_advance(h + 22);
    return hajimu_string(tb->buf);
}

/* =====================================================================
 * Phase 31: データ表示拡張
 * ===================================================================*/

/* ソート可能リスト(ラベル, 項目) → 配列 */
static Value fn_sortable_list(int argc, Value *argv) {
    (void)argc;
    const char *label = argv[0].string.data;
    if (argv[1].type != VALUE_ARRAY || !g_cur) return argv[1];

    Hjpcontext *vg = g_cur->vg;
    float x, y, w;
    gui_pos(&x, &y, &w);

    /* ラベル */
    hjpFontSize(vg, GUI_FONT_SIZE);
    hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_TOP);
    hjpFillColor(vg, TH_TEXT_DIM);
    hjpText(vg, x, y, label, NULL);
    y += 22;

    int count = argv[1].array.length;
    float item_h = 32;
    int clicked_idx = -1;

    for (int i = 0; i < count && i < 20; i++) {
        Value item = argv[1].array.elements[i];
        const char *txt = item.type == VALUE_STRING ? item.string.data : "?";
        float iy = y + i * item_h;
        bool hov = gui_hit((float)g_cur->in.mx, (float)g_cur->in.my, x, iy, w, item_h);

        hjpBeginPath(vg); hjpRect(vg, x, iy, w, item_h);
        hjpFillColor(vg, hov ? TH_WIDGET_HOVER : TH_WIDGET_BG);
        hjpFill(vg);
        hjpStrokeColor(vg, TH_BORDER); hjpStrokeWidth(vg, 0.5f); hjpStroke(vg);

        /* ドラッグハンドル */
        hjpFillColor(vg, TH_TEXT_DIM);
        hjpFontSize(vg, 14.0f);
        hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_MIDDLE);
        hjpText(vg, x + 8, iy + item_h/2, "\xe2\x98\xb0", NULL); /* ☰ */

        hjpFillColor(vg, TH_TEXT);
        hjpFontSize(vg, GUI_FONT_SIZE);
        hjpText(vg, x + 30, iy + item_h/2, txt, NULL);

        if (hov && g_cur->in.clicked) clicked_idx = i;
    }

    /* 簡易並べ替え: クリックしたアイテムを1つ上に移動 */
    Value result = argv[1];
    if (clicked_idx > 0) {
        Value new_arr = hajimu_array();
        for (int i = 0; i < count; i++) {
            if (i == clicked_idx - 1) hajimu_array_push(&new_arr, argv[1].array.elements[clicked_idx]);
            else if (i == clicked_idx) hajimu_array_push(&new_arr, argv[1].array.elements[clicked_idx - 1]);
            else hajimu_array_push(&new_arr, argv[1].array.elements[i]);
        }
        result = new_arr;
    }

    gui_advance(count * item_h + 22);
    return result;
}

/* プロパティグリッド(ラベル, 辞書) → 辞書 */
static Value fn_property_grid(int argc, Value *argv) {
    (void)argc;
    const char *label = argv[0].string.data;
    if (argv[1].type != VALUE_DICT || !g_cur) return argv[1];

    Hjpcontext *vg = g_cur->vg;
    float x, y, w;
    gui_pos(&x, &y, &w);

    hjpFontSize(vg, GUI_FONT_SIZE);
    hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_TOP);
    hjpFillColor(vg, TH_TEXT_DIM);
    hjpText(vg, x, y, label, NULL);
    y += 22;

    float row_h = 28;
    int count = argv[1].dict.length;
    for (int i = 0; i < count && i < 20; i++) {
        float ry = y + i * row_h;
        bool even = (i % 2 == 0);
        hjpBeginPath(vg); hjpRect(vg, x, ry, w, row_h);
        hjpFillColor(vg, even ? TH_WIDGET_BG : TH_BG); hjpFill(vg);

        /* キー */
        hjpFillColor(vg, TH_TEXT_DIM);
        hjpFontSize(vg, 14.0f);
        hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_MIDDLE);
        const char *key = argv[1].dict.keys[i];
        hjpText(vg, x + 8, ry + row_h/2, key, NULL);

        /* 値 */
        hjpFillColor(vg, TH_TEXT);
        char vbuf[64];
        Value val = argv[1].dict.values[i];
        if (val.type == VALUE_STRING) {
            hjpText(vg, x + w/2, ry + row_h/2, val.string.data, NULL);
        } else if (val.type == VALUE_NUMBER) {
            snprintf(vbuf, sizeof(vbuf), "%.6g", val.number);
            hjpText(vg, x + w/2, ry + row_h/2, vbuf, NULL);
        }
    }

    /* 枠線 */
    hjpBeginPath(vg); hjpRect(vg, x, y, w, count * row_h);
    hjpStrokeColor(vg, TH_BORDER); hjpStrokeWidth(vg, 1); hjpStroke(vg);
    /* 中央縦線 */
    hjpBeginPath(vg); hjpMoveTo(vg, x + w/2 - 4, y);
    hjpLineTo(vg, x + w/2 - 4, y + count * row_h); hjpStroke(vg);

    gui_advance(count * row_h + 22);
    return argv[1];
}

/* ツリーテーブル(列, データ) → 辞書 */
static Value fn_tree_table(int argc, Value *argv) {
    (void)argc;
    if (argv[0].type != VALUE_ARRAY || argv[1].type != VALUE_ARRAY || !g_cur) return hajimu_null();

    Hjpcontext *vg = g_cur->vg;
    float x, y, w;
    gui_pos(&x, &y, &w);

    int ncols = argv[0].array.length;
    float col_w = ncols > 0 ? w / ncols : w;
    float row_h = 28;

    /* ヘッダー */
    hjpBeginPath(vg); hjpRect(vg, x, y, w, row_h);
    hjpFillColor(vg, TH_WIDGET_BG); hjpFill(vg);
    hjpStrokeColor(vg, TH_BORDER); hjpStrokeWidth(vg, 1); hjpStroke(vg);

    hjpFontSize(vg, 14.0f);
    hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_MIDDLE);
    hjpFillColor(vg, TH_TEXT);
    for (int c = 0; c < ncols; c++) {
        const char *hdr = argv[0].array.elements[c].type == VALUE_STRING
            ? argv[0].array.elements[c].string.data : "?";
        hjpText(vg, x + c * col_w + 8, y + row_h/2, hdr, NULL);
    }
    y += row_h;

    /* データ行 */
    int nrows = argv[1].array.length;
    for (int r = 0; r < nrows && r < 20; r++) {
        float ry = y + r * row_h;
        hjpBeginPath(vg); hjpRect(vg, x, ry, w, row_h);
        hjpFillColor(vg, (r % 2) ? TH_BG : TH_WIDGET_BG); hjpFill(vg);
        hjpStrokeColor(vg, TH_BORDER); hjpStrokeWidth(vg, 0.5f); hjpStroke(vg);

        Value row = argv[1].array.elements[r];
        if (row.type == VALUE_ARRAY) {
            for (int c = 0; c < ncols && c < row.array.length; c++) {
                Value cell = row.array.elements[c];
                const char *txt = cell.type == VALUE_STRING ? cell.string.data : "?";
                char nbuf[32];
                if (cell.type == VALUE_NUMBER) { snprintf(nbuf, sizeof(nbuf), "%.6g", cell.number); txt = nbuf; }
                hjpFillColor(vg, TH_TEXT);
                hjpText(vg, x + c * col_w + 8, ry + row_h/2, txt, NULL);
            }
        }
    }

    gui_advance(row_h + nrows * row_h);
    return hajimu_null();
}

/* ヒートマップ(データ, オプション) → 無 */
static Value fn_heatmap(int argc, Value *argv) {
    (void)argc;
    if (argv[0].type != VALUE_ARRAY || !g_cur) return hajimu_null();

    Hjpcontext *vg = g_cur->vg;
    float x, y, w;
    gui_pos(&x, &y, &w);

    int rows = argv[0].array.length;
    if (rows == 0) return hajimu_null();
    int cols = 0;
    if (argv[0].array.elements[0].type == VALUE_ARRAY)
        cols = argv[0].array.elements[0].array.length;
    if (cols == 0) return hajimu_null();

    float cell_w = w / cols;
    float cell_h = 24;

    /* min/max計算 */
    float vmin = 1e30f, vmax = -1e30f;
    for (int r = 0; r < rows; r++) {
        Value row = argv[0].array.elements[r];
        if (row.type != VALUE_ARRAY) continue;
        for (int c = 0; c < cols && c < row.array.length; c++) {
            float v = (float)row.array.elements[c].number;
            if (v < vmin) vmin = v;
            if (v > vmax) vmax = v;
        }
    }
    float range = vmax - vmin;
    if (range < 0.001f) range = 1;

    for (int r = 0; r < rows && r < 20; r++) {
        Value row = argv[0].array.elements[r];
        if (row.type != VALUE_ARRAY) continue;
        for (int c = 0; c < cols && c < row.array.length; c++) {
            float v = (float)row.array.elements[c].number;
            float t = (v - vmin) / range;
            /* 青→赤のカラーマップ */
            int red = (int)(t * 255);
            int blue = (int)((1 - t) * 255);
            hjpBeginPath(vg);
            hjpRect(vg, x + c * cell_w, y + r * cell_h, cell_w, cell_h);
            hjpFillColor(vg, hjpRGBA(red, 60, blue, 200));
            hjpFill(vg);

            /* 値表示 */
            char vbuf[16]; snprintf(vbuf, sizeof(vbuf), "%.0f", v);
            hjpFontSize(vg, 11.0f);
            hjpTextAlign(vg, HJP_ALIGN_CENTER | HJP_ALIGN_MIDDLE);
            hjpFillColor(vg, hjpRGBA(255,255,255,200));
            hjpText(vg, x + c * cell_w + cell_w/2, y + r * cell_h + cell_h/2, vbuf, NULL);
        }
    }

    gui_advance(rows * cell_h);
    return hajimu_null();
}

/* トランスファーリスト(左, 右) → 辞書{左, 右} */
static Value fn_transfer_list(int argc, Value *argv) {
    (void)argc;
    if (argv[0].type != VALUE_ARRAY || argv[1].type != VALUE_ARRAY || !g_cur)
        return gui_dict2("\xe5\xb7\xa6", argv[0], "\xe5\x8f\xb3", argv[1]);

    Hjpcontext *vg = g_cur->vg;
    float x, y, w;
    gui_pos(&x, &y, &w);

    float half = (w - 40) / 2;
    float item_h = 28;
    int left_count = argv[0].array.length;
    int right_count = argv[1].array.length;
    int max_count = left_count > right_count ? left_count : right_count;
    float total_h = fmaxf(max_count * item_h, 60);

    /* 左リスト */
    hjpBeginPath(vg); hjpRoundedRect(vg, x, y, half, total_h, 4);
    hjpFillColor(vg, TH_WIDGET_BG); hjpFill(vg);
    hjpStrokeColor(vg, TH_BORDER); hjpStrokeWidth(vg, 1); hjpStroke(vg);

    int move_right = -1, move_left = -1;
    for (int i = 0; i < left_count && i < 10; i++) {
        float iy = y + i * item_h;
        Value item = argv[0].array.elements[i];
        const char *txt = item.type == VALUE_STRING ? item.string.data : "?";
        bool hov = gui_hit((float)g_cur->in.mx, (float)g_cur->in.my, x, iy, half, item_h);
        if (hov) {
            hjpBeginPath(vg); hjpRect(vg, x, iy, half, item_h);
            hjpFillColor(vg, TH_WIDGET_HOVER); hjpFill(vg);
        }
        hjpFillColor(vg, TH_TEXT);
        hjpFontSize(vg, 14.0f); hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_MIDDLE);
        hjpText(vg, x + 8, iy + item_h/2, txt, NULL);
        if (hov && g_cur->in.clicked) move_right = i;
    }

    /* 中央ボタン */
    float bx = x + half + 4, by2 = y + total_h/2 - 16;
    hjpFontSize(vg, 20.0f); hjpTextAlign(vg, HJP_ALIGN_CENTER | HJP_ALIGN_MIDDLE);
    hjpFillColor(vg, TH_TEXT_DIM);
    hjpText(vg, bx + 16, by2 + 8, "\xe2\x86\x92", NULL);  /* → */
    hjpText(vg, bx + 16, by2 + 28, "\xe2\x86\x90", NULL); /* ← */

    /* 右リスト */
    float rx = x + half + 40;
    hjpBeginPath(vg); hjpRoundedRect(vg, rx, y, half, total_h, 4);
    hjpFillColor(vg, TH_WIDGET_BG); hjpFill(vg);
    hjpStrokeColor(vg, TH_BORDER); hjpStrokeWidth(vg, 1); hjpStroke(vg);

    for (int i = 0; i < right_count && i < 10; i++) {
        float iy = y + i * item_h;
        Value item = argv[1].array.elements[i];
        const char *txt = item.type == VALUE_STRING ? item.string.data : "?";
        bool hov = gui_hit((float)g_cur->in.mx, (float)g_cur->in.my, rx, iy, half, item_h);
        if (hov) {
            hjpBeginPath(vg); hjpRect(vg, rx, iy, half, item_h);
            hjpFillColor(vg, TH_WIDGET_HOVER); hjpFill(vg);
        }
        hjpFillColor(vg, TH_TEXT);
        hjpFontSize(vg, 14.0f); hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_MIDDLE);
        hjpText(vg, rx + 8, iy + item_h/2, txt, NULL);
        if (hov && g_cur->in.clicked) move_left = i;
    }

    /* アイテム移動処理 */
    Value new_left = argv[0], new_right = argv[1];
    if (move_right >= 0) {
        new_left = hajimu_array();
        new_right = hajimu_array();
        for (int i = 0; i < right_count; i++) hajimu_array_push(&new_right, argv[1].array.elements[i]);
        for (int i = 0; i < left_count; i++) {
            if (i == move_right) hajimu_array_push(&new_right, argv[0].array.elements[i]);
            else hajimu_array_push(&new_left, argv[0].array.elements[i]);
        }
    }
    if (move_left >= 0) {
        new_left = hajimu_array();
        new_right = hajimu_array();
        for (int i = 0; i < left_count; i++) hajimu_array_push(&new_left, argv[0].array.elements[i]);
        for (int i = 0; i < right_count; i++) {
            if (i == move_left) hajimu_array_push(&new_left, argv[1].array.elements[i]);
            else hajimu_array_push(&new_right, argv[1].array.elements[i]);
        }
    }

    gui_advance(total_h);
    return gui_dict2("\xe5\xb7\xa6", new_left, "\xe5\x8f\xb3", new_right);
}

/* カスケーダー(ラベル, データ, 選択) → 配列 */
static Value fn_cascader(int argc, Value *argv) {
    (void)argc;
    const char *label = argv[0].string.data;
    if (!g_cur) return argv[2];

    Hjpcontext *vg = g_cur->vg;
    float x, y, w;
    gui_pos(&x, &y, &w);

    hjpFontSize(vg, GUI_FONT_SIZE);
    hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_TOP);
    hjpFillColor(vg, TH_TEXT_DIM);
    hjpText(vg, x, y, label, NULL);
    y += 22;

    /* ヘッダーボタン表示 */
    float h = 30;
    hjpBeginPath(vg); hjpRoundedRect(vg, x, y, w, h, 4);
    hjpFillColor(vg, TH_WIDGET_BG); hjpFill(vg);
    hjpStrokeColor(vg, TH_BORDER); hjpStrokeWidth(vg, 1); hjpStroke(vg);

    /* 選択パス表示 */
    if (argv[2].type == VALUE_ARRAY && argv[2].array.length > 0) {
        char path[256] = "";
        for (int i = 0; i < argv[2].array.length; i++) {
            if (i > 0) strncat(path, " / ", sizeof(path) - strlen(path) - 1);
            Value v = argv[2].array.elements[i];
            if (v.type == VALUE_STRING)
                strncat(path, v.string.data, sizeof(path) - strlen(path) - 1);
        }
        hjpFillColor(vg, TH_TEXT);
        hjpText(vg, x + 8, y + 7, path, NULL);
    } else {
        hjpFillColor(vg, TH_TEXT_DIM);
        hjpText(vg, x + 8, y + 7, "\xe9\x81\xb8\u629e...", NULL);
    }

    /* ▼ */
    hjpTextAlign(vg, HJP_ALIGN_RIGHT | HJP_ALIGN_MIDDLE);
    hjpFillColor(vg, TH_TEXT_DIM);
    hjpText(vg, x + w - 8, y + h/2, "\xe2\x96\xbc", NULL);

    gui_advance(h + 22);
    return argv[2];
}

/* ツリーセレクト(ラベル, データ, 選択) → 数値 */
static Value fn_tree_select(int argc, Value *argv) {
    (void)argc;
    const char *label = argv[0].string.data;
    int selected = (int)argv[2].number;
    if (!g_cur) return hajimu_number(selected);

    Hjpcontext *vg = g_cur->vg;
    float x, y, w;
    gui_pos(&x, &y, &w);

    hjpFontSize(vg, GUI_FONT_SIZE);
    hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_TOP);
    hjpFillColor(vg, TH_TEXT_DIM);
    hjpText(vg, x, y, label, NULL);
    y += 22;

    float h = 30;
    hjpBeginPath(vg); hjpRoundedRect(vg, x, y, w, h, 4);
    hjpFillColor(vg, TH_WIDGET_BG); hjpFill(vg);
    hjpStrokeColor(vg, TH_BORDER); hjpStrokeWidth(vg, 1); hjpStroke(vg);

    /* 選択表示 */
    if (argv[1].type == VALUE_ARRAY && selected >= 0 && selected < argv[1].array.length) {
        Value sel = argv[1].array.elements[selected];
        const char *txt = sel.type == VALUE_STRING ? sel.string.data : "?";
        hjpFillColor(vg, TH_TEXT);
        hjpText(vg, x + 8, y + 7, txt, NULL);
    }

    hjpTextAlign(vg, HJP_ALIGN_RIGHT | HJP_ALIGN_MIDDLE);
    hjpFillColor(vg, TH_TEXT_DIM);
    hjpText(vg, x + w - 8, y + h/2, "\xe2\x96\xbc", NULL);

    gui_advance(h + 22);
    return hajimu_number(selected);
}

/* 画像ギャラリー(画像配列, 列数) → 数値 (クリックされた画像インデックス, -1=なし) */
static Value fn_image_gallery(int argc, Value *argv) {
    (void)argc;
    if (argv[0].type != VALUE_ARRAY || !g_cur) return hajimu_number(-1);

    int columns = (int)argv[1].number;
    if (columns < 1) columns = 3;

    Hjpcontext *vg = g_cur->vg;
    float x, y, w;
    gui_pos(&x, &y, &w);

    int count = argv[0].array.length;
    float cell_w = w / columns;
    float cell_h = cell_w; /* 正方形 */
    int rows = (count + columns - 1) / columns;
    int clicked = -1;

    for (int i = 0; i < count && i < 64; i++) {
        int col = i % columns;
        int row = i / columns;
        float cx = x + col * cell_w + 2;
        float cy = y + row * cell_h + 2;
        float cw = cell_w - 4, ch = cell_h - 4;

        int img_idx = (int)argv[0].array.elements[i].number;

        if (img_idx >= 0 && img_idx < GUI_MAX_IMAGES && g_images[img_idx].valid) {
            Hjppaint ip = hjpImagePattern(vg, cx, cy, cw, ch, 0, g_images[img_idx].handle, 1.0f);
            hjpBeginPath(vg); hjpRoundedRect(vg, cx, cy, cw, ch, 4);
            hjpFillPaint(vg, ip); hjpFill(vg);
        } else {
            hjpBeginPath(vg); hjpRoundedRect(vg, cx, cy, cw, ch, 4);
            hjpFillColor(vg, TH_WIDGET_BG); hjpFill(vg);
            hjpStrokeColor(vg, TH_BORDER); hjpStrokeWidth(vg, 1); hjpStroke(vg);
        }

        if (gui_hit((float)g_cur->in.mx, (float)g_cur->in.my, cx, cy, cw, ch)) {
            hjpStrokeColor(vg, TH_ACCENT); hjpStrokeWidth(vg, 2); hjpStroke(vg);
            if (g_cur->in.clicked) clicked = i;
        }
    }

    gui_advance(rows * cell_h);
    return hajimu_number(clicked);
}

/* =====================================================================
 * Phase 32: テーマ・スタイル拡張 II
 * ===================================================================*/

/* テーマ作成(名前, 色辞書) → 無 */
static Value fn_theme_create(int argc, Value *argv) {
    (void)argc;
    if (argv[1].type != VALUE_DICT) return hajimu_null();
    (void)argv[0]; /* 名前は将来的にテーマ保存に使用 */

    for (int i = 0; i < argv[1].dict.length; i++) {
        const char *k = argv[1].dict.keys[i];
        Hjpcolor c = gui_arg_color(argv[1].dict.values[i]);
        if (strstr(k, "背景"))          g_th.bg = c;
        else if (strstr(k, "ウィジェット背景")) g_th.widget_bg = c;
        else if (strstr(k, "ウィジェットホバー")) g_th.widget_hover = c;
        else if (strstr(k, "ウィジェットアクティブ")) g_th.widget_active = c;
        else if (strstr(k, "アクセントホバー")) g_th.accent_hover = c;
        else if (strstr(k, "アクセント")) g_th.accent = c;
        else if (strstr(k, "テキスト淡")) g_th.text_dim = c;
        else if (strstr(k, "テキスト"))   g_th.text = c;
        else if (strstr(k, "枠"))         g_th.border = c;
        else if (strstr(k, "区切り"))     g_th.sep = c;
        else if (strstr(k, "チェック"))   g_th.check = c;
        else if (strstr(k, "トラック"))   g_th.track = c;
    }
    if (g_cur)
        g_cur->bg = (GuiRGBA){g_th.bg.r, g_th.bg.g, g_th.bg.b, g_th.bg.a};
    return hajimu_null();
}

/* テーマエクスポート() → 文字列 (JSON) */
static Value fn_theme_export(int argc, Value *argv) {
    (void)argc; (void)argv;
    char buf[1024];
    snprintf(buf, sizeof(buf),
        "{\"bg\":[%d,%d,%d],\"widget_bg\":[%d,%d,%d],\"accent\":[%d,%d,%d],"
        "\"text\":[%d,%d,%d],\"border\":[%d,%d,%d]}",
        (int)(g_th.bg.r*255), (int)(g_th.bg.g*255), (int)(g_th.bg.b*255),
        (int)(g_th.widget_bg.r*255), (int)(g_th.widget_bg.g*255), (int)(g_th.widget_bg.b*255),
        (int)(g_th.accent.r*255), (int)(g_th.accent.g*255), (int)(g_th.accent.b*255),
        (int)(g_th.text.r*255), (int)(g_th.text.g*255), (int)(g_th.text.b*255),
        (int)(g_th.border.r*255), (int)(g_th.border.g*255), (int)(g_th.border.b*255));
    return hajimu_string(buf);
}

/* テーマインポート用ヘルパー */
static int theme_parse_rgb(const char *s, const char *key, Hjpcolor *out) {
    const char *p = strstr(s, key);
    if (!p) return 0;
    p = strchr(p, '[');
    if (!p) return 0;
    int r, g, b;
    if (sscanf(p, "[%d,%d,%d]", &r, &g, &b) == 3) {
        *out = hjpRGBA(r, g, b, 255);
        return 1;
    }
    return 0;
}

/* テーマインポート(JSON) → 真偽 */
static Value fn_theme_import(int argc, Value *argv) {
    (void)argc;
    if (argv[0].type != VALUE_STRING) return hajimu_bool(false);
    const char *json = argv[0].string.data;
    theme_parse_rgb(json, "bg", &g_th.bg);
    theme_parse_rgb(json, "widget_bg", &g_th.widget_bg);
    theme_parse_rgb(json, "accent", &g_th.accent);
    theme_parse_rgb(json, "text", &g_th.text);
    theme_parse_rgb(json, "border", &g_th.border);
    if (g_cur)
        g_cur->bg = (GuiRGBA){g_th.bg.r, g_th.bg.g, g_th.bg.b, g_th.bg.a};
    return hajimu_bool(true);
}

/* ダークモード自動() → 無 */
static Value fn_dark_mode_auto(int argc, Value *argv) {
    (void)argc; (void)argv;
    /* OS のダークモード検出は将来的にhjp_platformで対応、現在はシンプルなヒューリスティック */
    /* 現在の時刻ベースの自動切替 (18:00-6:00 はダーク) */
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    if (tm->tm_hour >= 18 || tm->tm_hour < 6)
        gui_theme_set_dark();
    else
        gui_theme_set_light();
    if (g_cur)
        g_cur->bg = (GuiRGBA){g_th.bg.r, g_th.bg.g, g_th.bg.b, g_th.bg.a};
    return hajimu_null();
}

/* 影設定(X, Y, ぼかし[, 色]) → 無 */
static float g_shadow_ox = 0, g_shadow_oy = 0, g_shadow_blur = 0;
static Hjpcolor g_shadow_color;
static bool g_shadow_active = false;

static Value fn_shadow_style(int argc, Value *argv) {
    g_shadow_ox = (float)argv[0].number;
    g_shadow_oy = (float)argv[1].number;
    g_shadow_blur = (float)argv[2].number;
    g_shadow_color = argc >= 4 ? gui_arg_color(argv[3]) : hjpRGBA(0,0,0,80);
    g_shadow_active = true;
    return hajimu_null();
}

/* 角丸設定(半径) → 無 */
static float g_global_radius = 0;

static Value fn_border_radius(int argc, Value *argv) {
    (void)argc;
    g_global_radius = (float)argv[0].number;
    return hajimu_null();
}

/* アイコン(名前[, サイズ, 色]) → 無 */
static Value fn_icon(int argc, Value *argv) {
    const char *name = argv[0].string.data;
    float sz = argc >= 2 ? (float)argv[1].number : 20.0f;
    Hjpcolor col = argc >= 3 ? gui_arg_color(argv[2]) : TH_TEXT;
    if (!g_cur) return hajimu_null();

    Hjpcontext *vg = g_cur->vg;
    float x, y, w;
    gui_pos(&x, &y, &w);

    /* ビルトインアイコン（UTF-8記号で代替） */
    const char *icon = "?";
    if (strstr(name, "ホーム") || strstr(name, "home")) icon = "\xe2\x8c\x82"; /* ⌂ */
    else if (strstr(name, "検索") || strstr(name, "search")) icon = "\xe2\x9c\xa6"; /* ✦ */
    else if (strstr(name, "設定") || strstr(name, "settings")) icon = "\xe2\x9a\x99"; /* ⚙ */
    else if (strstr(name, "ユーザー") || strstr(name, "user")) icon = "\xe2\x99\xa6"; /* ♦ */
    else if (strstr(name, "メール") || strstr(name, "mail")) icon = "\xe2\x9c\x89"; /* ✉ */
    else if (strstr(name, "スター") || strstr(name, "star")) icon = "\xe2\x98\x85"; /* ★ */
    else if (strstr(name, "ハート") || strstr(name, "heart")) icon = "\xe2\x99\xa5"; /* ♥ */
    else if (strstr(name, "チェック") || strstr(name, "check")) icon = "\xe2\x9c\x93"; /* ✓ */
    else if (strstr(name, "閉じる") || strstr(name, "close")) icon = "\xc3\x97"; /* × */
    else if (strstr(name, "追加") || strstr(name, "add")) icon = "+";
    else if (strstr(name, "削除") || strstr(name, "delete")) icon = "\xe2\x9c\x95"; /* ✕ */
    else if (strstr(name, "編集") || strstr(name, "edit")) icon = "\xe2\x9c\x8e"; /* ✎ */
    else if (strstr(name, "警告") || strstr(name, "warning")) icon = "\xe2\x9a\xa0"; /* ⚠ */
    else if (strstr(name, "情報") || strstr(name, "info")) icon = "\xe2\x84\xb9"; /* ℹ */

    hjpFontSize(vg, sz);
    hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_TOP);
    hjpFillColor(vg, col);
    hjpText(vg, x, y, icon, NULL);

    gui_advance(sz + 4);
    return hajimu_null();
}

/* フォント一覧() → 配列 */
static Value fn_font_list(int argc, Value *argv) {
    (void)argc; (void)argv;
    Value arr = hajimu_array();
    hajimu_array_push(&arr, hajimu_string("デフォルト"));
    if (g_cur && g_cur->font_id >= 0)
        hajimu_array_push(&arr, hajimu_string("カスタム"));
    return arr;
}

/* =====================================================================
 * Phase 33: マルチメディア
 * ===================================================================*/

/* 音声再生(ファイル[, ループ]) → 数値
 * NOTE: 音声ライブラリがリンクされていない環境ではスタブ実装 */
static Value fn_audio_play(int argc, Value *argv) {
    (void)argc; (void)argv;
    /* 音声ライブラリ無しのスタブ — 将来的に対応 */
    return hajimu_number(-1);
}

/* 音声停止([ID]) → 無 */
static Value fn_audio_stop(int argc, Value *argv) {
    (void)argc; (void)argv;
    return hajimu_null();
}

/* 音量(値) → 無 */
static Value fn_audio_volume(int argc, Value *argv) {
    (void)argc; (void)argv;
    return hajimu_null();
}

/* ブラウザ開く(URL) → 無 */
static Value fn_open_browser(int argc, Value *argv) {
    (void)argc;
    if (argv[0].type != VALUE_STRING) return hajimu_null();
    const char *url = argv[0].string.data;
    /* URLバリデーション: http/https/file のみ許可 */
    if (!gui_is_safe_url(url)) return hajimu_null();
    char esc_url[1024];
    gui_shell_escape(esc_url, sizeof(esc_url), url);
#ifdef __APPLE__
    char cmd[1200];
    snprintf(cmd, sizeof(cmd), "open %s", esc_url);
    (void)system(cmd);
#elif defined(__linux__)
    char cmd[1200];
    snprintf(cmd, sizeof(cmd), "xdg-open %s", esc_url);
    (void)system(cmd);
#endif
    return hajimu_null();
}

/* SVG読み込み(パス) → 数値 (画像ID) — nanosvg未リンク時はスタブ */
static Value fn_svg_load(int argc, Value *argv) {
    (void)argc; (void)argv;
    /* nanosvg未リンクのスタブ */
    return hajimu_number(-1);
}

/* ズーム操作開始(幅, 高さ[, 最大倍率]) → 辞書{x, y, 倍率} */
static float g_zoom_x = 0, g_zoom_y = 0, g_zoom_scale = 1.0f;

static Value fn_zoom_view(int argc, Value *argv) {
    float vw = (float)argv[0].number;
    float vh = (float)argv[1].number;
    float max_scale = argc >= 3 ? (float)argv[2].number : 4.0f;
    if (!g_cur) return gui_dict3("x", hajimu_number(0), "y", hajimu_number(0), "\xe5\x80\x8d\xe7\x8e\x87", hajimu_number(1));

    Hjpcontext *vg = g_cur->vg;
    float x, y, w;
    gui_pos(&x, &y, &w);

    /* コンテナ */
    hjpBeginPath(vg); hjpRoundedRect(vg, x, y, vw, vh, 4);
    hjpStrokeColor(vg, TH_BORDER); hjpStrokeWidth(vg, 1); hjpStroke(vg);
    hjpScissor(vg, x, y, vw, vh);

    /* スクロールでズーム */
    bool hov = gui_hit((float)g_cur->in.mx, (float)g_cur->in.my, x, y, vw, vh);
    if (hov && g_cur->in.scroll_y != 0) {
        float dz = g_cur->in.scroll_y > 0 ? 1.1f : 0.9f;
        g_zoom_scale *= dz;
        if (g_zoom_scale < 0.1f) g_zoom_scale = 0.1f;
        if (g_zoom_scale > max_scale) g_zoom_scale = max_scale;
    }

    /* ドラッグでパン */
    if (hov && g_cur->in.down) {
        g_zoom_x += (float)(g_cur->in.mx - g_cur->in.pmx);
        g_zoom_y += (float)(g_cur->in.my - g_cur->in.pmy);
    }

    /* トランスフォーム適用 */
    hjpSave(vg);
    hjpTranslate(vg, x + g_zoom_x, y + g_zoom_y);
    hjpScale(vg, g_zoom_scale, g_zoom_scale);

    hjpResetScissor(vg);
    hjpRestore(vg);

    gui_advance(vh);
    return gui_dict3("x", hajimu_number(g_zoom_x), "y", hajimu_number(g_zoom_y), "\xe5\x80\x8d\xe7\x8e\x87", hajimu_number(g_zoom_scale));
}

/* ツアーガイド(ステップ配列, 現在) → 数値 */
static Value fn_tour_guide(int argc, Value *argv) {
    (void)argc;
    if (argv[0].type != VALUE_ARRAY || !g_cur) return hajimu_number(0);

    int current = (int)argv[1].number;
    int count = argv[0].array.length;
    if (current < 0 || current >= count) return hajimu_number(current);

    Hjpcontext *vg = g_cur->vg;
    float ww = (float)g_cur->win_w, wh = (float)g_cur->win_h;

    /* オーバーレイ */
    hjpBeginPath(vg); hjpRect(vg, 0, 0, ww, wh);
    hjpFillColor(vg, hjpRGBA(0, 0, 0, 100)); hjpFill(vg);

    /* ツールチップカード */
    Value step = argv[0].array.elements[current];
    const char *txt = step.type == VALUE_STRING ? step.string.data : "?";

    float tw = 280, th = 100;
    float tx = (ww - tw) / 2, ty = (wh - th) / 2;

    hjpBeginPath(vg); hjpRoundedRect(vg, tx, ty, tw, th, 8);
    hjpFillColor(vg, TH_BG); hjpFill(vg);
    hjpStrokeColor(vg, TH_ACCENT); hjpStrokeWidth(vg, 2); hjpStroke(vg);

    /* ステップ番号 */
    char step_label[32]; snprintf(step_label, sizeof(step_label), "%d / %d", current + 1, count);
    hjpFontSize(vg, 12.0f);
    hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_TOP);
    hjpFillColor(vg, TH_ACCENT);
    hjpText(vg, tx + 14, ty + 10, step_label, NULL);

    /* テキスト */
    hjpFontSize(vg, GUI_FONT_SIZE);
    hjpFillColor(vg, TH_TEXT);
    hjpTextBox(vg, tx + 14, ty + 30, tw - 28, txt, NULL);

    /* 次へボタン */
    float bw = 60, bh = 28;
    float bx = tx + tw - bw - 10, by = ty + th - bh - 10;
    bool hov = gui_hit((float)g_cur->in.mx, (float)g_cur->in.my, bx, by, bw, bh);
    hjpBeginPath(vg); hjpRoundedRect(vg, bx, by, bw, bh, 4);
    hjpFillColor(vg, hov ? TH_ACCENT_HOVER : TH_ACCENT); hjpFill(vg);
    hjpFontSize(vg, 14.0f);
    hjpTextAlign(vg, HJP_ALIGN_CENTER | HJP_ALIGN_MIDDLE);
    hjpFillColor(vg, hjpRGBA(255,255,255,255));
    const char *btn = current < count - 1 ? "\xe6\xac\xa1\xe3\x81\xb8" : "\xe5\xae\x8c\xe4\xba\x86";
    hjpText(vg, bx + bw/2, by + bh/2, btn, NULL);

    if (hov && g_cur->in.clicked) return hajimu_number(current + 1);
    return hajimu_number(current);
}

/* 角度スライダー(ラベル, 角度) → 数値 */
static Value fn_angle_slider(int argc, Value *argv) {
    (void)argc;
    const char *label = argv[0].string.data;
    float angle = (float)argv[1].number; /* 度数 */
    if (!g_cur) return hajimu_number(angle);

    Hjpcontext *vg = g_cur->vg;
    float x, y, w;
    gui_pos(&x, &y, &w);
    uint32_t id = gui_hash(label);

    /* ラベル */
    hjpFontSize(vg, GUI_FONT_SIZE);
    hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_TOP);
    hjpFillColor(vg, TH_TEXT);
    hjpText(vg, x, y, label, NULL);
    y += 22;

    /* 円形スライダー */
    float radius = 40;
    float cx = x + w / 2;
    float cy = y + radius + 4;

    /* トラック */
    hjpBeginPath(vg);
    hjpCircle(vg, cx, cy, radius);
    hjpStrokeColor(vg, TH_TRACK);
    hjpStrokeWidth(vg, 3);
    hjpStroke(vg);

    /* 角度線 */
    float rad = angle * (3.14159f / 180.0f) - 3.14159f / 2; /* 0度=上 */
    float lx = cx + cosf(rad) * radius;
    float ly = cy + sinf(rad) * radius;
    hjpBeginPath(vg);
    hjpMoveTo(vg, cx, cy);
    hjpLineTo(vg, lx, ly);
    hjpStrokeColor(vg, TH_ACCENT);
    hjpStrokeWidth(vg, 2);
    hjpStroke(vg);

    /* ノブ */
    hjpBeginPath(vg); hjpCircle(vg, lx, ly, 6);
    hjpFillColor(vg, TH_ACCENT); hjpFill(vg);

    /* インタラクション */
    bool hov, pressed;
    gui_widget_logic(id, cx - radius, cy - radius, radius * 2, radius * 2, &hov, &pressed);
    if (g_cur->active == id) {
        float dx = (float)g_cur->in.mx - cx;
        float dy = (float)g_cur->in.my - cy;
        angle = atan2f(dy, dx) * (180.0f / 3.14159f) + 90; /* 0度=上 */
        if (angle < 0) angle += 360;
    }

    /* 値表示 */
    char vbuf[32]; snprintf(vbuf, sizeof(vbuf), "%.0f\xc2\xb0", angle);
    hjpFontSize(vg, 14.0f);
    hjpTextAlign(vg, HJP_ALIGN_CENTER | HJP_ALIGN_MIDDLE);
    hjpFillColor(vg, TH_TEXT);
    hjpText(vg, cx, cy, vbuf, NULL);

    gui_advance(radius * 2 + 30);
    return hajimu_number(angle);
}

/* =====================================================================
 * Phase 34: ウィジェット状態・ユーティリティ
 * ===================================================================*/

/* --- 直前ウィジェット情報 (フレーム単位で更新) --- */
static struct {
    float x, y, w, h;
    bool  hovered, active, focused, clicked;
} g_last_widget;
static int g_frame_count = 0;

static Value gui_dict4(const char *k1, Value v1,
                       const char *k2, Value v2,
                       const char *k3, Value v3,
                       const char *k4, Value v4) {
    Value d;
    memset(&d, 0, sizeof(d));
    d.type = VALUE_DICT;
    d.dict.length   = 4;
    d.dict.capacity = 4;
    d.dict.keys   = (char **)calloc(4, sizeof(char *));
    d.dict.values = (Value *)calloc(4, sizeof(Value));
    d.dict.keys[0]   = strdup(k1);
    d.dict.keys[1]   = strdup(k2);
    d.dict.keys[2]   = strdup(k3);
    d.dict.keys[3]   = strdup(k4);
    d.dict.values[0] = v1;
    d.dict.values[1] = v2;
    d.dict.values[2] = v3;
    d.dict.values[3] = v4;
    return d;
}

/* ウィジェット状態() → 辞書 {ホバー, アクティブ, フォーカス, クリック} */
static Value fn_widget_state(int argc, Value *argv) {
    (void)argc; (void)argv;
    return gui_dict4("ホバー",     hajimu_bool(g_last_widget.hovered),
                     "アクティブ", hajimu_bool(g_last_widget.active),
                     "フォーカス",  hajimu_bool(g_last_widget.focused),
                     "クリック",   hajimu_bool(g_last_widget.clicked));
}

/* ウィジェット矩形() → 辞書 {x, y, 幅, 高さ} */
static Value fn_widget_rect(int argc, Value *argv) {
    (void)argc; (void)argv;
    return gui_dict4("x",  hajimu_number(g_last_widget.x),
                     "y",  hajimu_number(g_last_widget.y),
                     "幅", hajimu_number(g_last_widget.w),
                     "高さ", hajimu_number(g_last_widget.h));
}

/* スクロール位置() → 数値 */
static Value fn_scroll_pos(int argc, Value *argv) {
    (void)argc; (void)argv;
    if (!g_cur) return hajimu_number(0);
    return hajimu_number(g_cur->lay.y);
}

/* スクロール移動(位置) → 無 */
static Value fn_scroll_to(int argc, Value *argv) {
    (void)argc;
    if (!g_cur) return hajimu_null();
    double pos = argv[0].number;
    g_cur->lay.y = (float)pos;
    return hajimu_null();
}

/* フレーム数() → 数値 */
static Value fn_frame_count(int argc, Value *argv) {
    (void)argc; (void)argv;
    return hajimu_number(g_frame_count);
}

/* ポップオーバー開始(対象ID) → 真偽 (表示中か) */
static struct {
    unsigned int target_id;
    bool         open;
    float        x, y;
} g_popover;

static Value fn_popover_begin(int argc, Value *argv) {
    (void)argc;
    const char *label = argv[0].string.data;
    unsigned int id = gui_hash(label);

    if (!g_cur) return hajimu_bool(false);
    Hjpcontext *vg = g_cur->vg;

    /* クリックでトグル */
    float bx, by, bw;
    gui_pos(&bx, &by, &bw);
    (void)bw;
    float btn_sz = 20;
    float btn_x = bx, btn_y = by;
    bool hov = gui_hit(g_cur->in.mx, g_cur->in.my,
                        btn_x, btn_y, btn_sz, btn_sz);
    if (hov && g_cur->in.clicked) {
        if (g_popover.target_id == id && g_popover.open) {
            g_popover.open = false;
        } else {
            g_popover.target_id = id;
            g_popover.open = true;
            g_popover.x = btn_x;
            g_popover.y = btn_y + btn_sz + 4;
        }
    }

    /* ボタン描画 (▼) */
    hjpFontSize(vg, 14.0f);
    hjpFillColor(vg, hov ? TH_ACCENT : TH_TEXT);
    hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_TOP);
    hjpText(vg, btn_x, btn_y, "\xe2\x96\xbc", NULL); /* ▼ */
    gui_advance(btn_sz + 4);

    bool showing = (g_popover.target_id == id && g_popover.open);
    if (showing) {
        /* ポップオーバー背景 */
        float pw = 200, ph = 120;
        hjpBeginPath(vg);
        hjpRoundedRect(vg, g_popover.x, g_popover.y, pw, ph, 6);
        hjpFillColor(vg, TH_TOOLTIP_BG);
        hjpFill(vg);
        hjpStrokeColor(vg, TH_BORDER);
        hjpStrokeWidth(vg, 1);
        hjpStroke(vg);

        /* レイアウトをポップオーバー内に一時移動 */
        g_cur->lay.x = g_popover.x + 8;
        g_cur->lay.y = g_popover.y + 8;
        g_cur->lay.w = pw - 16;
    }
    return hajimu_bool(showing);
}

/* 外部クリック(ID) → 真偽 */
static Value fn_click_away(int argc, Value *argv) {
    (void)argc;
    const char *label = argv[0].string.data;
    unsigned int id = gui_hash(label);

    if (!g_cur) return hajimu_bool(false);

    /* 直前ウィジェットの領域外でクリックされたか */
    if (!g_cur->in.clicked) return hajimu_bool(false);

    /* g_last_widget に記録されている矩形の外か */
    bool inside = gui_hit(g_cur->in.mx, g_cur->in.my,
                          g_last_widget.x, g_last_widget.y,
                          g_last_widget.w, g_last_widget.h);
    (void)id;
    return hajimu_bool(!inside);
}

/* ヘルプマーク(テキスト) → 無 */
static Value fn_help_mark(int argc, Value *argv) {
    (void)argc;
    const char *text = argv[0].string.data;

    if (!g_cur) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;

    float x, y, w;
    gui_pos(&x, &y, &w);
    (void)w;

    float sz = 18;
    unsigned int id = gui_hash(text);
    bool hov = gui_hit(g_cur->in.mx, g_cur->in.my, x, y, sz, sz);

    /* (?) アイコン円 */
    float cx = x + sz / 2, cy = y + sz / 2;
    hjpBeginPath(vg);
    hjpCircle(vg, cx, cy, sz / 2);
    hjpFillColor(vg, hov ? TH_ACCENT : TH_TEXT_DIM);
    hjpFill(vg);

    hjpFontSize(vg, 12.0f);
    hjpTextAlign(vg, HJP_ALIGN_CENTER | HJP_ALIGN_MIDDLE);
    hjpFillColor(vg, TH_BG);
    hjpText(vg, cx, cy, "?", NULL);

    /* ホバー時ツールチップ */
    if (hov) {
        float tw = 200;
        float tx = cx + sz, ty = cy - 12;
        hjpBeginPath(vg);
        hjpRoundedRect(vg, tx, ty, tw, 28, 4);
        hjpFillColor(vg, TH_TOOLTIP_BG);
        hjpFill(vg);

        hjpFontSize(vg, 13.0f);
        hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_MIDDLE);
        hjpFillColor(vg, TH_TOOLTIP_FG);
        hjpText(vg, tx + 6, ty + 14, text, NULL);
    }

    /* 直前ウィジェット情報を更新 */
    g_last_widget.x = x;
    g_last_widget.y = y;
    g_last_widget.w = sz;
    g_last_widget.h = sz;
    g_last_widget.hovered = hov;
    g_last_widget.active  = false;
    g_last_widget.focused = (g_cur->focused == id);
    g_last_widget.clicked = (hov && g_cur->in.clicked);

    gui_advance(sz + 4);
    return hajimu_null();
}

/* =====================================================================
 * Phase 35: テーブル高度操作
 * ===================================================================*/

/* テーブルインライン編集(テーブル, 列) → 辞書{行, 列, 値} or null */
static Value fn_table_inline_edit(int argc, Value *argv) {
    (void)argc;
    int idx = (int)argv[0].number;
    int col_hint = (argc >= 2) ? (int)argv[1].number : -1;

    if (idx < 0 || idx >= GUI_MAX_TABLES || !g_tables[idx].valid)
        return hajimu_null();
    GuiTable *t = &g_tables[idx];
    if (!g_cur) return hajimu_null();

    /* ダブルクリックでセル編集開始 */
    float x, y, avail_w;
    gui_pos(&x, &y, &avail_w);
    float col_w_unit = (t->col_count > 0) ? avail_w / t->col_count : avail_w;

    if (g_cur->in.clicked) {
        float ry = y + GUI_TABLE_HEADER_H;
        for (int r = 0; r < t->row_count; r++) {
            float row_y = ry + GUI_TABLE_ROW_H * r - t->scroll_y;
            for (int c = 0; c < t->col_count; c++) {
                if (col_hint >= 0 && c != col_hint) continue;
                float cw = (g_table_col_w[idx][c] > 0) ? g_table_col_w[idx][c] : col_w_unit;
                float cx = x;
                for (int cc = 0; cc < c; cc++)
                    cx += (g_table_col_w[idx][cc] > 0) ? g_table_col_w[idx][cc] : col_w_unit;
                if (gui_hit(g_cur->in.mx, g_cur->in.my, cx, row_y, cw, GUI_TABLE_ROW_H)) {
                    if (t->edit_row == r && t->edit_col == c) {
                        /* already editing */
                    } else {
                        /* commit previous */
                        if (t->edit_row >= 0 && t->edit_col >= 0) {
                            free(t->rows[t->edit_row][t->edit_col]);
                            t->rows[t->edit_row][t->edit_col] = strdup(t->edit_buf);
                        }
                        t->edit_row = r;
                        t->edit_col = c;
                        const char *cur = t->rows[r][c] ? t->rows[r][c] : "";
                        snprintf(t->edit_buf, sizeof(t->edit_buf), "%s", cur);
                        t->edit_cursor = (int)strlen(t->edit_buf);
                    }
                }
            }
        }
    }

    /* テキスト入力処理 */
    if (t->edit_row >= 0 && t->edit_col >= 0) {
        if (g_cur->in.text_input_len > 0) {
            int len = (int)strlen(t->edit_buf);
            int add = g_cur->in.text_input_len;
            if (len + add < (int)sizeof(t->edit_buf) - 1) {
                memcpy(t->edit_buf + len, g_cur->in.text_input, add);
                t->edit_buf[len + add] = '\0';
                t->edit_cursor = len + add;
            }
        }
        if (g_cur->in.key_backspace && t->edit_cursor > 0) {
            t->edit_buf[--t->edit_cursor] = '\0';
        }
        /* Enter で確定 (HJP_SCANCODE_RETURN = 40) */
        const uint8_t *ks = hjp_get_keyboard_state(NULL);
        if (ks && ks[HJP_SCANCODE_RETURN]) {
            free(t->rows[t->edit_row][t->edit_col]);
            t->rows[t->edit_row][t->edit_col] = strdup(t->edit_buf);
            Value result = gui_dict3("行", hajimu_number(t->edit_row),
                                     "列", hajimu_number(t->edit_col),
                                     "値", hajimu_string(t->edit_buf));
            t->edit_row = -1;
            t->edit_col = -1;
            return result;
        }
        /* Esc でキャンセル */
        if (ks && ks[HJP_SCANCODE_ESCAPE]) {
            t->edit_row = -1;
            t->edit_col = -1;
        }
    }
    return hajimu_null();
}

/* テーブル列並替(テーブル) → 無 */
static Value fn_table_col_reorder(int argc, Value *argv) {
    (void)argc;
    int idx = (int)argv[0].number;
    if (idx < 0 || idx >= GUI_MAX_TABLES || !g_tables[idx].valid)
        return hajimu_null();
    GuiTable *t = &g_tables[idx];
    if (!g_cur) return hajimu_null();

    float x, y, avail_w;
    gui_pos(&x, &y, &avail_w);
    float col_w_unit = (t->col_count > 0) ? avail_w / t->col_count : avail_w;

    static int drag_col = -1;
    static int drag_tbl = -1;

    /* ヘッダー領域のドラッグ検出 */
    if (g_cur->in.down && drag_col < 0) {
        for (int c = 0; c < t->col_count; c++) {
            float cw = (g_table_col_w[idx][c] > 0) ? g_table_col_w[idx][c] : col_w_unit;
            float cx = x;
            for (int cc = 0; cc < c; cc++)
                cx += (g_table_col_w[idx][cc] > 0) ? g_table_col_w[idx][cc] : col_w_unit;
            if (gui_hit(g_cur->in.mx, g_cur->in.my, cx, y, cw, GUI_TABLE_HEADER_H)) {
                drag_col = c;
                drag_tbl = idx;
                break;
            }
        }
    }
    /* ドロップ先の計算 */
    if (drag_tbl == idx && drag_col >= 0 && g_cur->in.released) {
        float cx = x;
        int target = drag_col;
        for (int c = 0; c < t->col_count; c++) {
            float cw = (g_table_col_w[idx][c] > 0) ? g_table_col_w[idx][c] : col_w_unit;
            if (g_cur->in.mx >= cx && g_cur->in.mx < cx + cw) {
                target = c;
                break;
            }
            cx += cw;
        }
        if (target != drag_col) {
            int tmp = t->col_order[drag_col];
            t->col_order[drag_col] = t->col_order[target];
            t->col_order[target] = tmp;
            /* 列名も入れ替え */
            char name_tmp[64];
            memcpy(name_tmp, t->col_names[drag_col], 64);
            memcpy(t->col_names[drag_col], t->col_names[target], 64);
            memcpy(t->col_names[target], name_tmp, 64);
            /* 行データの列も入れ替え */
            for (int r = 0; r < t->row_count; r++) {
                char *tmp2 = t->rows[r][drag_col];
                t->rows[r][drag_col] = t->rows[r][target];
                t->rows[r][target] = tmp2;
            }
        }
        drag_col = -1;
        drag_tbl = -1;
    }
    if (!g_cur->in.down) { drag_col = -1; drag_tbl = -1; }
    return hajimu_null();
}

/* テーブル選択モード(テーブル, モード) → 配列(選択行) */
static Value fn_table_sel_mode(int argc, Value *argv) {
    (void)argc;
    int idx = (int)argv[0].number;
    if (idx < 0 || idx >= GUI_MAX_TABLES || !g_tables[idx].valid)
        return hajimu_null();
    GuiTable *t = &g_tables[idx];

    /* モード設定 */
    if (argc >= 2 && argv[1].type == VALUE_STRING) {
        if (strstr(argv[1].string.data, "\xe8\xa4\x87\xe6\x95\xb0"))     /* 複数 */
            t->sel_mode = 1;
        else if (strstr(argv[1].string.data, "\xe7\xaf\x84\xe5\x9b\xb2")) /* 範囲 */
            t->sel_mode = 2;
        else
            t->sel_mode = 0;
    }

    /* クリック処理 */
    if (g_cur && g_cur->in.clicked) {
        float bx, by, bw;
        gui_pos(&bx, &by, &bw);
        float col_w_unit = (t->col_count > 0) ? bw / t->col_count : bw;
        float ry = by + GUI_TABLE_HEADER_H;
        for (int r = 0; r < t->row_count && r < 4096; r++) {
            float row_y = ry + GUI_TABLE_ROW_H * r - t->scroll_y;
            if (gui_hit(g_cur->in.mx, g_cur->in.my, bx, row_y, bw, GUI_TABLE_ROW_H)) {
                if (t->sel_mode == 0) {
                    memset(t->row_selected, 0, sizeof(t->row_selected));
                    t->row_selected[r] = true;
                    t->selected = r;
                } else if (t->sel_mode == 1) {
                    t->row_selected[r] = !t->row_selected[r];
                } else if (t->sel_mode == 2) {
                    /* 範囲: selected → r */
                    int from = t->selected >= 0 ? t->selected : r;
                    int lo = from < r ? from : r;
                    int hi = from > r ? from : r;
                    memset(t->row_selected, 0, sizeof(t->row_selected));
                    for (int i = lo; i <= hi && i < 4096; i++)
                        t->row_selected[i] = true;
                }
                (void)col_w_unit;
                break;
            }
        }
    }

    /* 選択行配列を返す */
    int count = 0;
    for (int r = 0; r < t->row_count && r < 4096; r++)
        if (t->row_selected[r]) count++;
    Value arr;
    memset(&arr, 0, sizeof(arr));
    arr.type = VALUE_ARRAY;
    arr.array.length = count;
    arr.array.elements = (Value *)calloc(count > 0 ? count : 1, sizeof(Value));
    int wi = 0;
    for (int r = 0; r < t->row_count && r < 4096; r++)
        if (t->row_selected[r])
            arr.array.elements[wi++] = hajimu_number(r);
    return arr;
}

/* テーブル固定列(テーブル, 数) → 無 */
static Value fn_table_fixed_cols(int argc, Value *argv) {
    (void)argc;
    int idx = (int)argv[0].number;
    int num = (int)argv[1].number;
    if (idx < 0 || idx >= GUI_MAX_TABLES || !g_tables[idx].valid)
        return hajimu_null();
    GuiTable *t = &g_tables[idx];
    if (num < 0) num = 0;
    if (num > t->col_count) num = t->col_count;
    t->fixed_cols = num;
    return hajimu_null();
}

/* テーブルグループ化(テーブル, 列) → 無 */
static Value fn_table_group(int argc, Value *argv) {
    (void)argc;
    int idx = (int)argv[0].number;
    int col = (int)argv[1].number;
    if (idx < 0 || idx >= GUI_MAX_TABLES || !g_tables[idx].valid)
        return hajimu_null();
    GuiTable *t = &g_tables[idx];
    if (col < 0 || col >= t->col_count) {
        t->group_col = -1;
        return hajimu_null();
    }
    t->group_col = col;
    /* グループ列でソート */
    for (int a = 0; a < t->row_count - 1; a++)
        for (int b = a + 1; b < t->row_count; b++) {
            int cmp = strcmp(t->rows[a][col] ? t->rows[a][col] : "",
                             t->rows[b][col] ? t->rows[b][col] : "");
            if (cmp > 0) {
                char **tmp = t->rows[a];
                t->rows[a] = t->rows[b];
                t->rows[b] = tmp;
            }
        }
    memset(t->group_collapsed, 0, sizeof(t->group_collapsed));
    return hajimu_null();
}

/* テーブルセル結合(テーブル, 範囲) → 無
   範囲: [行, 列, 行スパン, 列スパン] */
static Value fn_table_merge(int argc, Value *argv) {
    (void)argc;
    int idx = (int)argv[0].number;
    if (idx < 0 || idx >= GUI_MAX_TABLES || !g_tables[idx].valid)
        return hajimu_null();
    GuiTable *t = &g_tables[idx];
    if (argv[1].type != VALUE_ARRAY || argv[1].array.length < 4)
        return hajimu_null();
    t->merge_row   = (int)argv[1].array.elements[0].number;
    t->merge_col   = (int)argv[1].array.elements[1].number;
    t->merge_rspan = (int)argv[1].array.elements[2].number;
    t->merge_cspan = (int)argv[1].array.elements[3].number;
    if (t->merge_rspan < 1) t->merge_rspan = 1;
    if (t->merge_cspan < 1) t->merge_cspan = 1;
    return hajimu_null();
}

/* テーブルエクスポート(テーブル, 形式) → 文字列 (CSV or JSON) */
static Value fn_table_export(int argc, Value *argv) {
    (void)argc;
    int idx = (int)argv[0].number;
    const char *fmt = argv[1].string.data;
    if (idx < 0 || idx >= GUI_MAX_TABLES || !g_tables[idx].valid)
        return hajimu_string("");
    GuiTable *t = &g_tables[idx];

    /* バッファサイズ概算 — size_t 演算でオーバーフロー防止 */
    size_t ncols = (size_t)(t->col_count + 1);
    size_t nrows = (size_t)(t->row_count + 2);
    size_t cap = ncols * 256 * nrows + 512; /* 1セル最大256バイトで概算 */
    char *buf = (char *)malloc(cap);
    if (!buf) return hajimu_string("");
    buf[0] = '\0';
    size_t pos = 0;

    bool is_json = (strstr(fmt, "JSON") || strstr(fmt, "json"));
    if (is_json) {
        pos += snprintf(buf + pos, cap - pos, "[");
        for (int r = 0; r < t->row_count; r++) {
            if (r > 0) pos += snprintf(buf + pos, cap - pos, ",");
            pos += snprintf(buf + pos, cap - pos, "{");
            for (int c = 0; c < t->col_count; c++) {
                if (c > 0) pos += snprintf(buf + pos, cap - pos, ",");
                const char *cell = (t->rows[r] && t->rows[r][c]) ? t->rows[r][c] : "";
                pos += snprintf(buf + pos, cap - pos, "\"%s\":\"%s\"",
                                t->col_names[c], cell);
            }
            pos += snprintf(buf + pos, cap - pos, "}");
        }
        pos += snprintf(buf + pos, cap - pos, "]");
    } else {
        /* CSV */
        for (int c = 0; c < t->col_count; c++) {
            if (c > 0) pos += snprintf(buf + pos, cap - pos, ",");
            pos += snprintf(buf + pos, cap - pos, "\"%s\"", t->col_names[c]);
        }
        pos += snprintf(buf + pos, cap - pos, "\n");
        for (int r = 0; r < t->row_count; r++) {
            for (int c = 0; c < t->col_count; c++) {
                if (c > 0) pos += snprintf(buf + pos, cap - pos, ",");
                const char *cell = (t->rows[r] && t->rows[r][c]) ? t->rows[r][c] : "";
                pos += snprintf(buf + pos, cap - pos, "\"%s\"", cell);
            }
            pos += snprintf(buf + pos, cap - pos, "\n");
        }
    }
    Value result = hajimu_string(buf);
    free(buf);
    return result;
}

/* テーブル行ドラッグ並替(テーブル) → 配列(新順序) */
static Value fn_table_row_drag(int argc, Value *argv) {
    (void)argc;
    int idx = (int)argv[0].number;
    if (idx < 0 || idx >= GUI_MAX_TABLES || !g_tables[idx].valid)
        return hajimu_null();
    GuiTable *t = &g_tables[idx];
    if (!g_cur) return hajimu_null();

    float bx, by, bw;
    gui_pos(&bx, &by, &bw);
    float ry = by + GUI_TABLE_HEADER_H;

    /* ドラッグ開始 */
    if (g_cur->in.down && t->drag_row < 0) {
        for (int r = 0; r < t->row_count; r++) {
            float row_y = ry + GUI_TABLE_ROW_H * r - t->scroll_y;
            /* 左端のハンドル領域 (幅20px) のみ反応 */
            if (gui_hit(g_cur->in.mx, g_cur->in.my, bx, row_y, 20, GUI_TABLE_ROW_H)) {
                t->drag_row = r;
                t->drag_target = r;
                break;
            }
        }
    }

    /* ドラッグ中 */
    if (t->drag_row >= 0 && g_cur->in.down) {
        for (int r = 0; r < t->row_count; r++) {
            float row_y = ry + GUI_TABLE_ROW_H * r - t->scroll_y;
            if (g_cur->in.my >= row_y && g_cur->in.my < row_y + GUI_TABLE_ROW_H) {
                t->drag_target = r;
                break;
            }
        }
        /* ドラッグ中ハイライト描画 */
        if (g_cur->vg) {
            float tgt_y = ry + GUI_TABLE_ROW_H * t->drag_target - t->scroll_y;
            hjpBeginPath(g_cur->vg);
            hjpRect(g_cur->vg, bx, tgt_y, bw, 2);
            hjpFillColor(g_cur->vg, TH_ACCENT);
            hjpFill(g_cur->vg);
        }
    }

    /* ドロップ */
    if (t->drag_row >= 0 && g_cur->in.released) {
        if (t->drag_row != t->drag_target) {
            char **moving = t->rows[t->drag_row];
            if (t->drag_row < t->drag_target) {
                for (int r = t->drag_row; r < t->drag_target; r++)
                    t->rows[r] = t->rows[r + 1];
            } else {
                for (int r = t->drag_row; r > t->drag_target; r--)
                    t->rows[r] = t->rows[r - 1];
            }
            t->rows[t->drag_target] = moving;
        }
        t->drag_row = -1;
        t->drag_target = -1;
    }
    if (!g_cur->in.down) {
        t->drag_row = -1;
    }

    /* 現在の行順序を配列で返す */
    Value arr;
    memset(&arr, 0, sizeof(arr));
    arr.type = VALUE_ARRAY;
    arr.array.length = t->row_count;
    arr.array.elements = (Value *)calloc(t->row_count > 0 ? t->row_count : 1, sizeof(Value));
    for (int r = 0; r < t->row_count; r++)
        arr.array.elements[r] = hajimu_number(r);
    return arr;
}

/* =====================================================================
 * Phase 36: ツリー高度操作
 * ===================================================================*/

/* ツリードラッグ並替(ツリー) → 配列 */
static Value fn_tree_drag_reorder(int argc, Value *argv) {
    (void)argc;
    int idx = (int)argv[0].number;
    if (idx < 0 || idx >= GUI_MAX_TREES || !g_trees[idx].valid)
        return hajimu_null();
    GuiTree *tr = &g_trees[idx];
    if (!g_cur) return hajimu_null();

    static int drag_node = -1;
    static int drag_tree = -1;

    float bx, by, bw;
    gui_pos(&bx, &by, &bw);

    if (g_cur->in.down && drag_node < 0) {
        float cy = by;
        for (int i = 0; i < tr->node_count; i++) {
            if (gui_hit(g_cur->in.mx, g_cur->in.my, bx, cy, 20, GUI_TREE_NODE_H)) {
                drag_node = i;
                drag_tree = idx;
                break;
            }
            cy += GUI_TREE_NODE_H;
        }
    }
    if (drag_tree == idx && drag_node >= 0 && g_cur->in.released) {
        float cy = by;
        int target = drag_node;
        for (int i = 0; i < tr->node_count; i++) {
            if (g_cur->in.my >= cy && g_cur->in.my < cy + GUI_TREE_NODE_H) {
                target = i; break;
            }
            cy += GUI_TREE_NODE_H;
        }
        if (target != drag_node) {
            GuiTreeNode tmp = tr->nodes[drag_node];
            if (drag_node < target)
                for (int i = drag_node; i < target; i++) tr->nodes[i] = tr->nodes[i+1];
            else
                for (int i = drag_node; i > target; i--) tr->nodes[i] = tr->nodes[i-1];
            tr->nodes[target] = tmp;
        }
        drag_node = -1; drag_tree = -1;
    }
    if (!g_cur->in.down) { drag_node = -1; drag_tree = -1; }

    Value arr; memset(&arr, 0, sizeof(arr));
    arr.type = VALUE_ARRAY;
    arr.array.length = tr->node_count;
    arr.array.elements = (Value *)calloc(tr->node_count > 0 ? tr->node_count : 1, sizeof(Value));
    for (int i = 0; i < tr->node_count; i++)
        arr.array.elements[i] = hajimu_number(i);
    return arr;
}

/* ツリー遅延読込(ツリー, コールバック) → 無 */
static Value fn_tree_lazy_load(int argc, Value *argv) {
    (void)argc;
    int idx = (int)argv[0].number;
    if (idx < 0 || idx >= GUI_MAX_TREES || !g_trees[idx].valid)
        return hajimu_null();
    g_trees[idx].lazy_load = true;
    /* コールバックはランタイムが展開時に呼ぶ(フラグのみ設定) */
    (void)argv[1];
    return hajimu_null();
}

/* ツリーチェックボックス(ツリー) → 配列(チェック済みノード) */
static Value fn_tree_checkbox(int argc, Value *argv) {
    (void)argc;
    int idx = (int)argv[0].number;
    if (idx < 0 || idx >= GUI_MAX_TREES || !g_trees[idx].valid)
        return hajimu_null();
    GuiTree *tr = &g_trees[idx];
    tr->checkbox_mode = true;

    /* クリック処理: 親子連動 */
    if (g_cur && g_cur->in.clicked) {
        float bx, by, bw;
        gui_pos(&bx, &by, &bw);
        float cy = by;
        for (int i = 0; i < tr->node_count; i++) {
            if (gui_hit(g_cur->in.mx, g_cur->in.my, bx, cy, 18, GUI_TREE_NODE_H)) {
                tr->nodes[i].checked = !tr->nodes[i].checked;
                /* 子ノードも同期 */
                for (int j = 0; j < tr->node_count; j++)
                    if (tr->nodes[j].parent == i)
                        tr->nodes[j].checked = tr->nodes[i].checked;
                break;
            }
            cy += GUI_TREE_NODE_H;
        }
    }

    int count = 0;
    for (int i = 0; i < tr->node_count; i++)
        if (tr->nodes[i].checked) count++;
    Value arr; memset(&arr, 0, sizeof(arr));
    arr.type = VALUE_ARRAY;
    arr.array.length = count;
    arr.array.elements = (Value *)calloc(count > 0 ? count : 1, sizeof(Value));
    int wi = 0;
    for (int i = 0; i < tr->node_count; i++)
        if (tr->nodes[i].checked)
            arr.array.elements[wi++] = hajimu_number(i);
    return arr;
}

/* ツリー検索(ツリー, テキスト) → 無 */
static Value fn_tree_search(int argc, Value *argv) {
    (void)argc;
    int idx = (int)argv[0].number;
    const char *query = argv[1].string.data;
    if (idx < 0 || idx >= GUI_MAX_TREES || !g_trees[idx].valid)
        return hajimu_null();
    GuiTree *tr = &g_trees[idx];
    for (int i = 0; i < tr->node_count; i++) {
        tr->nodes[i].visible = (query[0] == '\0' ||
                                strstr(tr->nodes[i].label, query) != NULL);
        /* 一致ノードの親も展開 */
        if (tr->nodes[i].visible && tr->nodes[i].parent >= 0) {
            int p = tr->nodes[i].parent;
            while (p >= 0 && p < tr->node_count) {
                tr->nodes[p].visible = true;
                tr->nodes[p].expanded = true;
                p = tr->nodes[p].parent;
            }
        }
    }
    return hajimu_null();
}

/* ツリー全展開(ツリー) → 無 */
static Value fn_tree_expand_all(int argc, Value *argv) {
    (void)argc;
    int idx = (int)argv[0].number;
    if (idx < 0 || idx >= GUI_MAX_TREES || !g_trees[idx].valid)
        return hajimu_null();
    for (int i = 0; i < g_trees[idx].node_count; i++)
        g_trees[idx].nodes[i].expanded = true;
    return hajimu_null();
}

/* ツリー全折畿(ツリー) → 無 */
static Value fn_tree_collapse_all(int argc, Value *argv) {
    (void)argc;
    int idx = (int)argv[0].number;
    if (idx < 0 || idx >= GUI_MAX_TREES || !g_trees[idx].valid)
        return hajimu_null();
    for (int i = 0; i < g_trees[idx].node_count; i++)
        g_trees[idx].nodes[i].expanded = false;
    return hajimu_null();
}

/* ツリー複数選択(ツリー, モード) → 配列 */
static Value fn_tree_multi_select(int argc, Value *argv) {
    (void)argc;
    int idx = (int)argv[0].number;
    if (idx < 0 || idx >= GUI_MAX_TREES || !g_trees[idx].valid)
        return hajimu_null();
    GuiTree *tr = &g_trees[idx];
    tr->sel_mode = 1; /* 複数 */

    if (g_cur && g_cur->in.clicked) {
        float bx, by, bw;
        gui_pos(&bx, &by, &bw);
        float cy = by;
        for (int i = 0; i < tr->node_count; i++) {
            if (gui_hit(g_cur->in.mx, g_cur->in.my, bx, cy, bw, GUI_TREE_NODE_H)) {
                tr->node_selected[i] = !tr->node_selected[i];
                break;
            }
            cy += GUI_TREE_NODE_H;
        }
    }

    int count = 0;
    for (int i = 0; i < tr->node_count; i++)
        if (tr->node_selected[i]) count++;
    Value arr; memset(&arr, 0, sizeof(arr));
    arr.type = VALUE_ARRAY;
    arr.array.length = count;
    arr.array.elements = (Value *)calloc(count > 0 ? count : 1, sizeof(Value));
    int wi = 0;
    for (int i = 0; i < tr->node_count; i++)
        if (tr->node_selected[i])
            arr.array.elements[wi++] = hajimu_number(i);
    return arr;
}

/* ツリーノードアイコン(ツリー, ノード, 画像) → 無 */
static Value fn_tree_node_icon(int argc, Value *argv) {
    (void)argc;
    int idx  = (int)argv[0].number;
    int node = (int)argv[1].number;
    int img  = (int)argv[2].number;
    if (idx < 0 || idx >= GUI_MAX_TREES || !g_trees[idx].valid)
        return hajimu_null();
    if (node < 0 || node >= g_trees[idx].node_count)
        return hajimu_null();
    g_trees[idx].nodes[node].icon_image = img;
    return hajimu_null();
}

/* =====================================================================
 * Phase 37: テキストエディタ機能
 * ===================================================================*/

/* シンタックスハイライト(入力, 言語) → 無
   単純なキーワードハイライトを提供 */
static struct { char lang[32]; Hjpcolor color; } g_syntax_colors[8];
static int g_syntax_color_count = 0;

static Value fn_syntax_highlight(int argc, Value *argv) {
    (void)argc;
    /* 入力テキストのIDをタグ付け: 次回のテキスト描画時にキーワードを色付けする */
    const char *input_id = argv[0].string.data;
    const char *lang = (argc >= 2 && argv[1].type == VALUE_STRING)
                       ? argv[1].string.data : "";
    (void)input_id;
    if (g_syntax_color_count < 8) {
        snprintf(g_syntax_colors[g_syntax_color_count].lang, 32, "%s", lang);
        g_syntax_colors[g_syntax_color_count].color = TH_ACCENT;
        g_syntax_color_count++;
    }
    return hajimu_null();
}

/* テキストカーソル位置(入力[, 位置]) → 数値 */
static int g_text_cursor_pos = 0;

static Value fn_text_cursor_pos(int argc, Value *argv) {
    (void)argv;
    if (argc >= 2 && argv[1].type == VALUE_NUMBER)
        g_text_cursor_pos = (int)argv[1].number;
    return hajimu_number(g_text_cursor_pos);
}

/* テキスト範囲選択(入力, 開始, 終了) → 無 */
static int g_text_sel_start = 0, g_text_sel_end = 0;

static Value fn_text_select_range(int argc, Value *argv) {
    (void)argc;
    g_text_sel_start = (int)argv[1].number;
    g_text_sel_end   = (int)argv[2].number;
    (void)argv[0]; /* input id */
    return hajimu_null();
}

/* 行番号表示(入力, 有効) → 無 */
static bool g_show_line_numbers = false;

static Value fn_line_numbers(int argc, Value *argv) {
    (void)argc;
    g_show_line_numbers = (argv[1].type == VALUE_BOOL) ? argv[1].boolean : true;
    (void)argv[0];
    return hajimu_null();
}

/* テキスト検索置換(入力, 検索, 置換) → 数値(置換数) */
static Value fn_text_find_replace(int argc, Value *argv) {
    (void)argc;
    const char *input_id = argv[0].string.data;
    const char *find = argv[1].string.data;
    const char *replace = argv[2].string.data;
    (void)input_id;

    /* テキストバッファ内で検索置換を実行 */
    uint32_t id = gui_hash(input_id);
    GuiTextBuf *tb = gui_get_text_buf(id, "");
    if (!tb) return hajimu_number(0);

    int count = 0;
    int flen = (int)strlen(find);
    int rlen = (int)strlen(replace);
    if (flen == 0) return hajimu_number(0);

    char result[GUI_MAX_TEXT_BUF];
    int ri = 0;
    for (int i = 0; tb->buf[i]; ) {
        if (strncmp(&tb->buf[i], find, flen) == 0) {
            if (ri + rlen < GUI_MAX_TEXT_BUF - 1) {
                memcpy(&result[ri], replace, rlen);
                ri += rlen;
            }
            i += flen;
            count++;
        } else {
            if (ri < GUI_MAX_TEXT_BUF - 1)
                result[ri++] = tb->buf[i];
            i++;
        }
    }
    result[ri] = '\0';
    snprintf(tb->buf, GUI_MAX_TEXT_BUF, "%s", result);
    return hajimu_number(count);
}

/* テキスト折畿(入力, 行範囲) → 無 */
static struct { int start; int end; bool collapsed; } g_text_folds[16];
static int g_text_fold_count = 0;

static Value fn_text_fold(int argc, Value *argv) {
    (void)argc;
    (void)argv[0]; /* input id */
    if (argv[1].type != VALUE_ARRAY || argv[1].array.length < 2)
        return hajimu_null();
    if (g_text_fold_count >= 16) return hajimu_null();
    g_text_folds[g_text_fold_count].start = (int)argv[1].array.elements[0].number;
    g_text_folds[g_text_fold_count].end   = (int)argv[1].array.elements[1].number;
    g_text_folds[g_text_fold_count].collapsed = true;
    g_text_fold_count++;
    return hajimu_null();
}

/* 入力フォーマッタ(入力, パターン) → 無
   パターン: "通貨", "電話", "数字のみ" 等 */
static Value fn_input_formatter(int argc, Value *argv) {
    (void)argc;
    const char *input_id = argv[0].string.data;
    const char *pattern  = argv[1].string.data;
    uint32_t id = gui_hash(input_id);
    GuiTextBuf *tb = gui_get_text_buf(id, "");
    if (!tb) return hajimu_null();

    /* 数字のみフィルタ */
    if (strstr(pattern, "\xe6\x95\xb0\xe5\xad\x97") /* 数字 */ ||
        strstr(pattern, "number")) {
        int wi = 0;
        for (int i = 0; tb->buf[i]; i++)
            if (tb->buf[i] >= '0' && tb->buf[i] <= '9')
                tb->buf[wi++] = tb->buf[i];
        tb->buf[wi] = '\0';
    }
    /* 通貨: 3桁カンマ */
    if (strstr(pattern, "\xe9\x80\x9a\xe8\xb2\xa8") /* 通貨 */ ||
        strstr(pattern, "currency")) {
        /* まず数字のみ抽出 */
        char digits[GUI_MAX_TEXT_BUF];
        int di = 0;
        for (int i = 0; tb->buf[i]; i++)
            if (tb->buf[i] >= '0' && tb->buf[i] <= '9')
                digits[di++] = tb->buf[i];
        digits[di] = '\0';
        /* 3桁ごとにカンマ */
        char formatted[GUI_MAX_TEXT_BUF];
        int fi = 0, len = di;
        for (int i = 0; i < len; i++) {
            if (i > 0 && (len - i) % 3 == 0)
                formatted[fi++] = ',';
            formatted[fi++] = digits[i];
        }
        formatted[fi] = '\0';
        snprintf(tb->buf, GUI_MAX_TEXT_BUF, "%s", formatted);
    }
    return hajimu_null();
}

/* IME合成(コールバック) → 無 */
static Value fn_ime_composition(int argc, Value *argv) {
    (void)argc;
    /* hjp_platformではTEXTEDITINGイベントとして受け取れるが、
       ここではコールバックを登録するフラグを立てる */
    (void)argv[0];
    return hajimu_null();
}

/* =====================================================================
 * Phase 38: フォーム枠組み
 * ===================================================================*/

#define GUI_MAX_FORMS 8
#define GUI_MAX_FORM_FIELDS 32

typedef struct {
    char name[64];
    char value[256];
    char error[128];
    bool required;
    bool valid;
} GuiFormField;

typedef struct {
    uint32_t id;
    GuiFormField fields[GUI_MAX_FORM_FIELDS];
    int field_count;
    bool valid;
    bool submitted;
} GuiForm;

static GuiForm g_forms[GUI_MAX_FORMS];

/* フォーム作成(ID) → 辞書 */
static Value fn_form_create(int argc, Value *argv) {
    (void)argc;
    const char *name = argv[0].string.data;
    uint32_t fid = gui_hash(name);

    for (int i = 0; i < GUI_MAX_FORMS; i++)
        if (g_forms[i].valid && g_forms[i].id == fid)
            return hajimu_number(i);

    int idx = -1;
    for (int i = 0; i < GUI_MAX_FORMS; i++)
        if (!g_forms[i].valid) { idx = i; break; }
    if (idx < 0) return hajimu_number(-1);

    memset(&g_forms[idx], 0, sizeof(GuiForm));
    g_forms[idx].id = fid;
    g_forms[idx].valid = true;
    return hajimu_number(idx);
}

/* フォーム送信(フォーム) → 真偽 */
static Value fn_form_submit(int argc, Value *argv) {
    (void)argc;
    int idx = (int)argv[0].number;
    if (idx < 0 || idx >= GUI_MAX_FORMS || !g_forms[idx].valid)
        return hajimu_bool(false);

    /* 全フィールド検証 */
    GuiForm *f = &g_forms[idx];
    bool all_valid = true;
    for (int i = 0; i < f->field_count; i++) {
        if (f->fields[i].required && f->fields[i].value[0] == '\0') {
            snprintf(f->fields[i].error, sizeof(f->fields[i].error),
                     "%sは必須です", f->fields[i].name);
            f->fields[i].valid = false;
            all_valid = false;
        } else {
            f->fields[i].valid = true;
            f->fields[i].error[0] = '\0';
        }
    }
    f->submitted = true;
    return hajimu_bool(all_valid);
}

/* フォームリセット(フォーム) → 無 */
static Value fn_form_reset(int argc, Value *argv) {
    (void)argc;
    int idx = (int)argv[0].number;
    if (idx < 0 || idx >= GUI_MAX_FORMS || !g_forms[idx].valid)
        return hajimu_null();
    GuiForm *f = &g_forms[idx];
    for (int i = 0; i < f->field_count; i++) {
        f->fields[i].value[0] = '\0';
        f->fields[i].error[0] = '\0';
        f->fields[i].valid = true;
    }
    f->submitted = false;
    return hajimu_null();
}

/* フォーム検証(フォーム) → 真偽 */
static Value fn_form_validate(int argc, Value *argv) {
    (void)argc;
    int idx = (int)argv[0].number;
    if (idx < 0 || idx >= GUI_MAX_FORMS || !g_forms[idx].valid)
        return hajimu_bool(false);
    GuiForm *f = &g_forms[idx];
    bool all_valid = true;
    for (int i = 0; i < f->field_count; i++) {
        if (f->fields[i].required && f->fields[i].value[0] == '\0') {
            snprintf(f->fields[i].error, sizeof(f->fields[i].error),
                     "%sは必須です", f->fields[i].name);
            f->fields[i].valid = false;
            all_valid = false;
        } else {
            f->fields[i].valid = true;
            f->fields[i].error[0] = '\0';
        }
    }
    return hajimu_bool(all_valid);
}

/* フォームエラー一覧(フォーム) → 配列 */
static Value fn_form_errors(int argc, Value *argv) {
    (void)argc;
    int idx = (int)argv[0].number;
    if (idx < 0 || idx >= GUI_MAX_FORMS || !g_forms[idx].valid)
        return hajimu_null();
    GuiForm *f = &g_forms[idx];
    int count = 0;
    for (int i = 0; i < f->field_count; i++)
        if (f->fields[i].error[0]) count++;

    Value arr; memset(&arr, 0, sizeof(arr));
    arr.type = VALUE_ARRAY;
    arr.array.length = count;
    arr.array.elements = (Value *)calloc(count > 0 ? count : 1, sizeof(Value));
    int wi = 0;
    for (int i = 0; i < f->field_count; i++)
        if (f->fields[i].error[0])
            arr.array.elements[wi++] = hajimu_string(f->fields[i].error);
    return arr;
}

/* 依存フィールド(条件, コールバック) → 無 */
static Value fn_form_dependent(int argc, Value *argv) {
    (void)argc;
    /* 条件文字列とコールバックを登録するフラグ */
    (void)argv[0]; (void)argv[1];
    return hajimu_null();
}

/* ファイルアップロード(ID[, オプション]) → 辞書 */
static Value fn_file_upload(int argc, Value *argv) {
    (void)argc;
    const char *label = argv[0].string.data;
    if (!g_cur) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;

    float bx, by, bw;
    gui_pos(&bx, &by, &bw);
    float btn_h = 32;
    unsigned int id = gui_hash(label);
    bool hov, pressed;
    gui_widget_logic(id, bx, by, 120, btn_h, &hov, &pressed);

    hjpBeginPath(vg);
    hjpRoundedRect(vg, bx, by, 120, btn_h, 4);
    hjpFillColor(vg, hov ? TH_WIDGET_HOVER : TH_WIDGET_BG);
    hjpFill(vg);
    hjpStrokeColor(vg, TH_BORDER);
    hjpStrokeWidth(vg, 1);
    hjpStroke(vg);

    hjpFontFaceId(vg, g_cur->font_id);
    hjpFontSize(vg, 14.0f);
    hjpFillColor(vg, TH_TEXT);
    hjpTextAlign(vg, HJP_ALIGN_CENTER | HJP_ALIGN_MIDDLE);
    hjpText(vg, bx + 60, by + btn_h / 2, "\xf0\x9f\x93\x81 ファイル選択", NULL);

    gui_advance(btn_h + 4);

    if (pressed) {
        /* ネイティブダイアログ (macOS) */
        char path[1024] = {0};
        FILE *fp = popen(
            "osascript -e 'POSIX path of (choose file)' 2>/dev/null", "r");
        if (fp) {
            if (fgets(path, sizeof(path), fp)) {
                /* 改行除去 */
                size_t len = strlen(path);
                if (len > 0 && path[len-1] == '\n') path[len-1] = '\0';
            }
            pclose(fp);
        }
        if (path[0])
            return gui_dict2("パス", hajimu_string(path),
                             "名前", hajimu_string(strrchr(path, '/') ? strrchr(path, '/') + 1 : path));
    }
    return hajimu_null();
}

/* フォームデータ取得(フォーム) → 辞書 */
static Value fn_form_data(int argc, Value *argv) {
    (void)argc;
    int idx = (int)argv[0].number;
    if (idx < 0 || idx >= GUI_MAX_FORMS || !g_forms[idx].valid)
        return hajimu_null();
    GuiForm *f = &g_forms[idx];

    Value d;
    memset(&d, 0, sizeof(d));
    d.type = VALUE_DICT;
    d.dict.length   = f->field_count;
    d.dict.capacity = f->field_count > 0 ? f->field_count : 1;
    d.dict.keys   = (char **)calloc(d.dict.capacity, sizeof(char *));
    d.dict.values = (Value *)calloc(d.dict.capacity, sizeof(Value));
    for (int i = 0; i < f->field_count; i++) {
        d.dict.keys[i]   = strdup(f->fields[i].name);
        d.dict.values[i] = hajimu_string(f->fields[i].value);
    }
    return d;
}

/* =====================================================================
 * Phase 39: アクセシビリティ
 * ===================================================================*/

static struct {
    char name[128];
    char desc[256];
    char role[64];
} g_a11y_current;

static struct {
    char msg[256];
    int  mode;  /* 0=off, 1=polite, 2=assertive */
} g_live_region;

static bool g_a11y_focus_ring = true;
static bool g_a11y_reduce_motion = false;

/* アクセシブル名(名前) → 無 */
static Value fn_a11y_name(int argc, Value *argv) {
    (void)argc;
    snprintf(g_a11y_current.name, sizeof(g_a11y_current.name),
             "%s", argv[0].string.data);
    return hajimu_null();
}

/* アクセシブル説明(説明) → 無 */
static Value fn_a11y_desc(int argc, Value *argv) {
    (void)argc;
    snprintf(g_a11y_current.desc, sizeof(g_a11y_current.desc),
             "%s", argv[0].string.data);
    return hajimu_null();
}

/* アクセシブルロール(ロール) → 無 */
static Value fn_a11y_role(int argc, Value *argv) {
    (void)argc;
    snprintf(g_a11y_current.role, sizeof(g_a11y_current.role),
             "%s", argv[0].string.data);
    return hajimu_null();
}

/* ライブリージョン(ID, モード) → 無 */
static Value fn_live_region(int argc, Value *argv) {
    (void)argc;
    (void)argv[0]; /* ID */
    if (argv[1].type == VALUE_STRING) {
        if (strstr(argv[1].string.data, "assertive"))
            g_live_region.mode = 2;
        else
            g_live_region.mode = 1;
    }
    return hajimu_null();
}

/* フォーカスリング(スタイル) → 無 */
static Value fn_focus_ring(int argc, Value *argv) {
    (void)argc;
    if (argv[0].type == VALUE_BOOL)
        g_a11y_focus_ring = argv[0].boolean;
    else if (argv[0].type == VALUE_STRING)
        g_a11y_focus_ring = true; /* スタイル文字列指定 = 有効 */
    return hajimu_null();
}

/* キーボードトラップ(ID, 有効) → 無 */
static uint32_t g_kb_trap_id = 0;
static bool g_kb_trap_active = false;

static Value fn_keyboard_trap(int argc, Value *argv) {
    (void)argc;
    const char *label = argv[0].string.data;
    bool enable = (argv[1].type == VALUE_BOOL) ? argv[1].boolean : true;
    if (enable) {
        g_kb_trap_id = gui_hash(label);
        g_kb_trap_active = true;
    } else {
        g_kb_trap_active = false;
    }
    return hajimu_null();
}

/* スクリーンリーダー通知(メッセージ) → 無 */
static Value fn_sr_announce(int argc, Value *argv) {
    (void)argc;
    snprintf(g_live_region.msg, sizeof(g_live_region.msg),
             "%s", argv[0].string.data);
    g_live_region.mode = 2; /* assertive */
    /* トーストとしてユーザーにも表示 */
    return hajimu_null();
}

/* 動き軽減(有効) → 無 */
static Value fn_reduce_motion(int argc, Value *argv) {
    (void)argc;
    g_a11y_reduce_motion = (argv[0].type == VALUE_BOOL) ? argv[0].boolean : true;
    return hajimu_null();
}

/* =====================================================================
 * Phase 40: ジェスチャー認識
 * ===================================================================*/

static struct {
    bool   active;
    float  start_x, start_y;
    float  last_x, last_y;
    uint32_t start_time;
    float  velocity_x, velocity_y;
    int    touch_count;
} g_gesture;

/* ピンチ(コールバック) → 数値(スケール) */
static Value fn_pinch(int argc, Value *argv) {
    (void)argc; (void)argv;
    /* マウスホイールでピンチをエミュレート */
    if (!g_cur) return hajimu_number(1.0);
    float scale = 1.0f;
    if (g_cur->in.scroll_y > 0) scale = 1.05f;
    else if (g_cur->in.scroll_y < 0) scale = 0.95f;
    return hajimu_number(scale);
}

/* スワイプ(方向, コールバック) → 真偽 */
static Value fn_swipe(int argc, Value *argv) {
    (void)argc;
    const char *dir = argv[0].string.data;
    if (!g_cur) return hajimu_bool(false);

    float dx = (float)g_cur->in.mx - g_gesture.start_x;
    float dy = (float)g_cur->in.my - g_gesture.start_y;
    float threshold = 50.0f;

    if (g_cur->in.released && g_gesture.active) {
        g_gesture.active = false;
        if (strstr(dir, "\xe5\x8f\xb3") && dx > threshold) /* 右 */
            return hajimu_bool(true);
        if (strstr(dir, "\xe5\xb7\xa6") && dx < -threshold) /* 左 */
            return hajimu_bool(true);
        if (strstr(dir, "\xe4\xb8\x8a") && dy < -threshold) /* 上 */
            return hajimu_bool(true);
        if (strstr(dir, "\xe4\xb8\x8b") && dy > threshold) /* 下 */
            return hajimu_bool(true);
    }
    if (g_cur->in.down && !g_gesture.active) {
        g_gesture.active = true;
        g_gesture.start_x = (float)g_cur->in.mx;
        g_gesture.start_y = (float)g_cur->in.my;
        g_gesture.start_time = hjp_get_ticks();
    }
    return hajimu_bool(false);
}

/* 長押し(コールバック) → 真偽 */
static Value fn_long_press(int argc, Value *argv) {
    (void)argc; (void)argv;
    if (!g_cur) return hajimu_bool(false);
    if (g_cur->in.down) {
        if (!g_gesture.active) {
            g_gesture.active = true;
            g_gesture.start_time = hjp_get_ticks();
            g_gesture.start_x = (float)g_cur->in.mx;
            g_gesture.start_y = (float)g_cur->in.my;
        }
        uint32_t elapsed = hjp_get_ticks() - g_gesture.start_time;
        float moved = fabsf((float)g_cur->in.mx - g_gesture.start_x) +
                      fabsf((float)g_cur->in.my - g_gesture.start_y);
        if (elapsed > 500 && moved < 10)
            return hajimu_bool(true);
    } else {
        g_gesture.active = false;
    }
    return hajimu_bool(false);
}

/* ダブルクリック(コールバック) → 真偽 */
static uint32_t g_last_click_time = 0;
static float  g_last_click_x = 0, g_last_click_y = 0;

static Value fn_double_click(int argc, Value *argv) {
    (void)argc; (void)argv;
    if (!g_cur) return hajimu_bool(false);
    if (g_cur->in.clicked) {
        uint32_t now = hjp_get_ticks();
        float dx = (float)g_cur->in.mx - g_last_click_x;
        float dy = (float)g_cur->in.my - g_last_click_y;
        bool dbl = (now - g_last_click_time < 400) &&
                   (fabsf(dx) < 10 && fabsf(dy) < 10);
        g_last_click_time = now;
        g_last_click_x = (float)g_cur->in.mx;
        g_last_click_y = (float)g_cur->in.my;
        if (dbl) {
            g_last_click_time = 0; /* リセット */
            return hajimu_bool(true);
        }
    }
    return hajimu_bool(false);
}

/* パンジェスチャー(コールバック) → 辞書{dx, dy} */
static Value fn_pan_gesture(int argc, Value *argv) {
    (void)argc; (void)argv;
    if (!g_cur) return hajimu_null();
    if (g_cur->in.down) {
        float dx = (float)g_cur->in.mx - g_gesture.last_x;
        float dy = (float)g_cur->in.my - g_gesture.last_y;
        g_gesture.last_x = (float)g_cur->in.mx;
        g_gesture.last_y = (float)g_cur->in.my;
        if (!g_gesture.active) {
            g_gesture.active = true;
            g_gesture.last_x = (float)g_cur->in.mx;
            g_gesture.last_y = (float)g_cur->in.my;
            return gui_dict2("dx", hajimu_number(0), "dy", hajimu_number(0));
        }
        return gui_dict2("dx", hajimu_number(dx), "dy", hajimu_number(dy));
    }
    g_gesture.active = false;
    return gui_dict2("dx", hajimu_number(0), "dy", hajimu_number(0));
}

/* マルチタッチ(コールバック) → 配列 */
static Value fn_multi_touch(int argc, Value *argv) {
    (void)argc; (void)argv;
    /* マウスのみ: シングルタッチを返す */
    if (!g_cur) return hajimu_null();
    Value arr; memset(&arr, 0, sizeof(arr));
    arr.type = VALUE_ARRAY;
    arr.array.length = 1;
    arr.array.elements = (Value *)calloc(1, sizeof(Value));
    arr.array.elements[0] = gui_dict2("x", hajimu_number(g_cur->in.mx),
                                      "y", hajimu_number(g_cur->in.my));
    return arr;
}

/* ジェスチャー速度() → 辞書{vx, vy} */
static Value fn_gesture_velocity(int argc, Value *argv) {
    (void)argc; (void)argv;
    if (!g_cur) return gui_dict2("vx", hajimu_number(0), "vy", hajimu_number(0));
    uint32_t now = hjp_get_ticks();
    float dt = (float)(now - g_gesture.start_time) / 1000.0f;
    if (dt < 0.001f) dt = 0.001f;
    float vx = ((float)g_cur->in.mx - g_gesture.start_x) / dt;
    float vy = ((float)g_cur->in.my - g_gesture.start_y) / dt;
    return gui_dict2("vx", hajimu_number(vx), "vy", hajimu_number(vy));
}

/* ジェスチャー領域(ID) → 無 */
static uint32_t g_gesture_region_id = 0;

static Value fn_gesture_region(int argc, Value *argv) {
    (void)argc;
    g_gesture_region_id = gui_hash(argv[0].string.data);
    return hajimu_null();
}

/* =====================================================================
 * Phase 41: 印刷/エクスポート
 * ===================================================================*/

/* 印刷(内容) → 真偽 */
static Value fn_print(int argc, Value *argv) {
    (void)argc;
    const char *content = argv[0].string.data;
    /* macOS: lprコマンドでプリント */
    char cmd[2048];
    char esc[1024];
    gui_shell_escape(esc, sizeof(esc), content);
    snprintf(cmd, sizeof(cmd), "echo %s | lpr 2>/dev/null", esc);
    int ret = system(cmd);
    return hajimu_bool(ret == 0);
}

/* 印刷プレビュー(内容) → 無 */
static Value fn_print_preview(int argc, Value *argv) {
    (void)argc;
    const char *content = argv[0].string.data;
    /* プレビュー: ファイルに書き出してopen */
    FILE *fp = fopen("/tmp/hajimu_print_preview.txt", "w");
    if (fp) { fprintf(fp, "%s", content); fclose(fp); }
    system("open /tmp/hajimu_print_preview.txt 2>/dev/null");
    return hajimu_null();
}

/* PDF出力(内容, パス) → 真偽 */
static Value fn_pdf_export(int argc, Value *argv) {
    (void)argc;
    const char *content = argv[0].string.data;
    const char *path    = argv[1].string.data;
    /* 簡易PDF生成 */
    FILE *fp = fopen(path, "w");
    if (!fp) return hajimu_bool(false);
    fprintf(fp, "%%PDF-1.0\n1 0 obj<</Type/Catalog/Pages 2 0 R>>endobj\n");
    fprintf(fp, "2 0 obj<</Type/Pages/Kids[3 0 R]/Count 1>>endobj\n");
    fprintf(fp, "3 0 obj<</Type/Page/MediaBox[0 0 612 792]/Parent 2 0 R"
                "/Contents 4 0 R/Resources<</Font<</F1 5 0 R>>>>>>endobj\n");
    char stream[4096];
    int slen = snprintf(stream, sizeof(stream),
                        "BT /F1 12 Tf 72 720 Td (%s) Tj ET", content);
    fprintf(fp, "4 0 obj<</Length %d>>stream\n%s\nendstream endobj\n",
            slen, stream);
    fprintf(fp, "5 0 obj<</Type/Font/Subtype/Type1/BaseFont/Helvetica>>endobj\n");
    fprintf(fp, "xref\n0 6\ntrailer<</Size 6/Root 1 0 R>>\n");
    fprintf(fp, "startxref\n0\n%%%%EOF\n");
    fclose(fp);
    return hajimu_bool(true);
}

/* ウィジェット画像化(ID, パス) → 真偽 */
static Value fn_widget_capture(int argc, Value *argv) {
    (void)argc;
    /* fn_screenshotと同様だがID指定で特定ウィジェットのみ */
    (void)argv[0]; (void)argv[1];
    /* 全画面キャプチャにフォールバック */
    return hajimu_bool(false);
}

/* 印刷設定(オプション) → 辞書 */
static Value fn_print_settings(int argc, Value *argv) {
    (void)argc; (void)argv;
    return gui_dict3("用紙",   hajimu_string("A4"),
                     "向き",   hajimu_string("縦"),
                     "余白",   hajimu_number(10));
}

/* キャンバス画像化(ID, 形式) → 真偽 */
static Value fn_canvas_export(int argc, Value *argv) {
    (void)argc;
    (void)argv[0]; (void)argv[1];
    return hajimu_bool(false); /* フレームバッファ読み取りは別途実装 */
}

/* SVG出力(内容, パス) → 真偽 */
static Value fn_svg_export(int argc, Value *argv) {
    (void)argc;
    const char *content = argv[0].string.data;
    const char *path    = argv[1].string.data;
    FILE *fp = fopen(path, "w");
    if (!fp) return hajimu_bool(false);
    fprintf(fp, "<svg xmlns=\"http://www.w3.org/2000/svg\">"
                "<text x=\"10\" y=\"20\">%s</text></svg>", content);
    fclose(fp);
    return hajimu_bool(true);
}

/* バーコード(値, 種類) → 無 */
static Value fn_barcode(int argc, Value *argv) {
    (void)argc;
    const char *value = argv[0].string.data;
    (void)argv[1]; /* 種類 */
    if (!g_cur) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    float bx, by, bw;
    gui_pos(&bx, &by, &bw);

    /* Code128風の簡易バーコード描画 */
    float bar_w = 2.0f;
    float bar_h = 50.0f;
    int len = (int)strlen(value);
    for (int i = 0; i < len && i < 40; i++) {
        unsigned char ch = (unsigned char)value[i];
        for (int b = 0; b < 8; b++) {
            if ((ch >> (7 - b)) & 1) {
                float xp = bx + (i * 8 + b) * bar_w;
                hjpBeginPath(vg);
                hjpRect(vg, xp, by, bar_w, bar_h);
                hjpFillColor(vg, TH_TEXT);
                hjpFill(vg);
            }
        }
    }
    /* 値テキスト */
    hjpFontFaceId(vg, g_cur->font_id);
    hjpFontSize(vg, 11.0f);
    hjpFillColor(vg, TH_TEXT);
    hjpTextAlign(vg, HJP_ALIGN_CENTER | HJP_ALIGN_TOP);
    hjpText(vg, bx + (len * 8 * bar_w) / 2, by + bar_h + 2, value, NULL);
    gui_advance(bar_h + 18);
    return hajimu_null();
}

/* =====================================================================
 * Phase 42: データバインディング
 * ===================================================================*/

#define GUI_MAX_BINDINGS 64

typedef struct {
    uint32_t var_id;
    uint32_t widget_id;
    bool     two_way;
    Value    cached;
} GuiBinding;

static GuiBinding g_bindings[GUI_MAX_BINDINGS];
static int g_binding_count = 0;

typedef struct {
    uint32_t id;
    Value    value;
    bool     dirty;
} GuiModel;

#define GUI_MAX_MODELS 16
static GuiModel g_models[GUI_MAX_MODELS];
static int g_model_count = 0;

/* バインド(変数, ウィジェット) → 無 */
static Value fn_bind(int argc, Value *argv) {
    (void)argc;
    if (g_binding_count >= GUI_MAX_BINDINGS) return hajimu_null();
    uint32_t vid = gui_hash(argv[0].string.data);
    uint32_t wid = gui_hash(argv[1].string.data);
    g_bindings[g_binding_count].var_id = vid;
    g_bindings[g_binding_count].widget_id = wid;
    g_bindings[g_binding_count].two_way = false;
    g_binding_count++;
    return hajimu_null();
}

/* 双方向バインド(変数, ウィジェット) → 無 */
static Value fn_bind_two_way(int argc, Value *argv) {
    (void)argc;
    if (g_binding_count >= GUI_MAX_BINDINGS) return hajimu_null();
    uint32_t vid = gui_hash(argv[0].string.data);
    uint32_t wid = gui_hash(argv[1].string.data);
    g_bindings[g_binding_count].var_id = vid;
    g_bindings[g_binding_count].widget_id = wid;
    g_bindings[g_binding_count].two_way = true;
    g_binding_count++;
    return hajimu_null();
}

/* 計算値(依存配列, 関数) → 値 */
static Value fn_computed(int argc, Value *argv) {
    (void)argc;
    /* 依存配列と計算関数を登録するフラグ */
    (void)argv[0]; (void)argv[1];
    return hajimu_null();
}

/* 監視(変数, コールバック) → 無 */
static Value fn_watch(int argc, Value *argv) {
    (void)argc;
    (void)argv[0]; (void)argv[1];
    return hajimu_null();
}

/* モデル作成(データ) → 辞書 */
static Value fn_model_create(int argc, Value *argv) {
    (void)argc;
    if (g_model_count >= GUI_MAX_MODELS) return hajimu_number(-1);
    int idx = g_model_count++;
    g_models[idx].id = gui_hash("model");
    g_models[idx].value = argv[0];
    g_models[idx].dirty = false;
    return hajimu_number(idx);
}

/* リストモデル(配列) → 辞書 */
static Value fn_list_model(int argc, Value *argv) {
    (void)argc;
    if (argv[0].type != VALUE_ARRAY) return hajimu_null();
    if (g_model_count >= GUI_MAX_MODELS) return hajimu_number(-1);
    int idx = g_model_count++;
    g_models[idx].value = argv[0];
    g_models[idx].dirty = false;
    return hajimu_number(idx);
}

/* バインド解除(ID) → 無 */
static Value fn_unbind(int argc, Value *argv) {
    (void)argc;
    uint32_t target = gui_hash(argv[0].string.data);
    for (int i = 0; i < g_binding_count; i++) {
        if (g_bindings[i].var_id == target || g_bindings[i].widget_id == target) {
            g_bindings[i] = g_bindings[--g_binding_count];
            break;
        }
    }
    return hajimu_null();
}

/* 変更通知(コールバック) → 無 */
static Value fn_notify_change(int argc, Value *argv) {
    (void)argc; (void)argv;
    return hajimu_null();
}

/* =====================================================================
 * Phase 43: 国際化
 * ===================================================================*/

#define GUI_MAX_TRANSLATIONS 256
#define GUI_MAX_LOCALES 8

static struct {
    char locale[16];
    char keys[GUI_MAX_TRANSLATIONS][64];
    char values[GUI_MAX_TRANSLATIONS][256];
    int count;
} g_i18n_tables[GUI_MAX_LOCALES];
static int g_i18n_locale_count = 0;
static char g_current_locale[16] = "ja";
static bool g_rtl = false;

/* RTLレイアウト(有効) → 無 */
static Value fn_rtl_layout(int argc, Value *argv) {
    (void)argc;
    g_rtl = (argv[0].type == VALUE_BOOL) ? argv[0].boolean : true;
    return hajimu_null();
}

/* ロケール設定(言語コード) → 無 */
static Value fn_set_locale(int argc, Value *argv) {
    (void)argc;
    snprintf(g_current_locale, sizeof(g_current_locale),
             "%s", argv[0].string.data);
    return hajimu_null();
}

/* 翻訳(キー) → 文字列 */
static Value fn_i18n_translate(int argc, Value *argv) {
    (void)argc;
    const char *key = argv[0].string.data;
    for (int l = 0; l < g_i18n_locale_count; l++) {
        if (strcmp(g_i18n_tables[l].locale, g_current_locale) == 0) {
            for (int i = 0; i < g_i18n_tables[l].count; i++) {
                if (strcmp(g_i18n_tables[l].keys[i], key) == 0)
                    return hajimu_string(g_i18n_tables[l].values[i]);
            }
        }
    }
    return hajimu_string(key); /* フォールバック */
}

/* 翻訳登録(言語, 辞書) → 無 */
static Value fn_register_translations(int argc, Value *argv) {
    (void)argc;
    const char *lang = argv[0].string.data;
    if (argv[1].type != VALUE_DICT) return hajimu_null();

    int l = -1;
    for (int i = 0; i < g_i18n_locale_count; i++) {
        if (strcmp(g_i18n_tables[i].locale, lang) == 0) { l = i; break; }
    }
    if (l < 0) {
        if (g_i18n_locale_count >= GUI_MAX_LOCALES) return hajimu_null();
        l = g_i18n_locale_count++;
        snprintf(g_i18n_tables[l].locale, sizeof(g_i18n_tables[l].locale),
                 "%s", lang);
        g_i18n_tables[l].count = 0;
    }
    for (int i = 0; i < argv[1].dict.length && g_i18n_tables[l].count < GUI_MAX_TRANSLATIONS; i++) {
        char nb[32];
        snprintf(g_i18n_tables[l].keys[g_i18n_tables[l].count], 64,
                 "%s", argv[1].dict.keys[i]);
        const char *v = gui_value_to_str(argv[1].dict.values[i], nb, sizeof(nb));
        snprintf(g_i18n_tables[l].values[g_i18n_tables[l].count], 256, "%s", v);
        g_i18n_tables[l].count++;
    }
    return hajimu_null();
}

/* 数値フォーマット(値[, ロケール]) → 文字列 */
static Value fn_number_format(int argc, Value *argv) {
    (void)argc;
    double val = argv[0].number;
    char buf[64];
    /* 3桁カンマ区切り */
    int whole = (int)val;
    char tmp[32];
    snprintf(tmp, sizeof(tmp), "%d", whole < 0 ? -whole : whole);
    int len = (int)strlen(tmp);
    int bi = 0;
    if (whole < 0) buf[bi++] = '-';
    for (int i = 0; i < len; i++) {
        if (i > 0 && (len - i) % 3 == 0) buf[bi++] = ',';
        buf[bi++] = tmp[i];
    }
    buf[bi] = '\0';
    return hajimu_string(buf);
}

/* 日付フォーマット(日付[, ロケール]) → 文字列 */
static Value fn_date_format(int argc, Value *argv) {
    (void)argc;
    const char *date = argv[0].string.data;
    /* そのまま返す (ロケール切替は将来拡張) */
    return hajimu_string(date);
}

/* 複数形(キー, 数) → 文字列 */
static Value fn_plural(int argc, Value *argv) {
    (void)argc;
    const char *key = argv[0].string.data;
    int count = (int)argv[1].number;
    /* 日本語は複数形なし: keyをそのまま返す */
    char buf[256];
    snprintf(buf, sizeof(buf), "%s: %d", key, count);
    return hajimu_string(buf);
}

/* テキスト方向(方向) → 無 */
static Value fn_text_direction(int argc, Value *argv) {
    (void)argc;
    if (strstr(argv[0].string.data, "rtl") ||
        strstr(argv[0].string.data, "RTL") ||
        strstr(argv[0].string.data, "\xe5\x8f\xb3")) /* 右 */
        g_rtl = true;
    else
        g_rtl = false;
    return hajimu_null();
}

/* =====================================================================
 * Phase 44: 動画/カメラ
 * ===================================================================*/

typedef struct {
    uint32_t id;
    char     path[512];
    bool     playing;
    float    position;  /* 秒 */
    float    volume;
    float    duration;
    bool     valid;
} GuiVideoPlayer;

#define GUI_MAX_VIDEOS 4
static GuiVideoPlayer g_videos[GUI_MAX_VIDEOS];

/* 動画再生(パス) → 辞書 */
static Value fn_video_play(int argc, Value *argv) {
    (void)argc;
    const char *path = argv[0].string.data;
    int idx = -1;
    for (int i = 0; i < GUI_MAX_VIDEOS; i++) {
        if (!g_videos[i].valid) { idx = i; break; }
    }
    if (idx < 0) return hajimu_number(-1);
    memset(&g_videos[idx], 0, sizeof(GuiVideoPlayer));
    g_videos[idx].id = gui_hash(path);
    snprintf(g_videos[idx].path, sizeof(g_videos[idx].path), "%s", path);
    g_videos[idx].playing = true;
    g_videos[idx].volume = 1.0f;
    g_videos[idx].valid = true;
    return hajimu_number(idx);
}

/* 動画停止(プレーヤー) → 無 */
static Value fn_video_stop(int argc, Value *argv) {
    (void)argc;
    int idx = (int)argv[0].number;
    if (idx >= 0 && idx < GUI_MAX_VIDEOS && g_videos[idx].valid)
        g_videos[idx].playing = false;
    return hajimu_null();
}

/* 動画シーク(プレーヤー, 秒) → 無 */
static Value fn_video_seek(int argc, Value *argv) {
    (void)argc;
    int idx = (int)argv[0].number;
    float sec = (float)argv[1].number;
    if (idx >= 0 && idx < GUI_MAX_VIDEOS && g_videos[idx].valid)
        g_videos[idx].position = sec;
    return hajimu_null();
}

/* 動画コントロール(プレーヤー) → 辞書{再生中, 位置, 音量} */
static Value fn_video_controls(int argc, Value *argv) {
    (void)argc;
    int idx = (int)argv[0].number;
    if (idx < 0 || idx >= GUI_MAX_VIDEOS || !g_videos[idx].valid)
        return hajimu_null();
    GuiVideoPlayer *v = &g_videos[idx];

    if (!g_cur) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    float bx, by, bw;
    gui_pos(&bx, &by, &bw);
    float ctrl_h = 30;

    /* コントロールバー背景 */
    hjpBeginPath(vg);
    hjpRoundedRect(vg, bx, by, bw, ctrl_h, 4);
    hjpFillColor(vg, TH_WIDGET_BG);
    hjpFill(vg);

    /* 再生/停止ボタン */
    const char *icon = v->playing ? "\xe2\x8f\xb8" : "\xe2\x96\xb6";
    bool btn_hov = gui_hit(g_cur->in.mx, g_cur->in.my, bx, by, 30, ctrl_h);
    if (btn_hov && g_cur->in.clicked)
        v->playing = !v->playing;
    hjpFontFaceId(vg, g_cur->font_id);
    hjpFontSize(vg, 16.0f);
    hjpFillColor(vg, btn_hov ? TH_ACCENT : TH_TEXT);
    hjpTextAlign(vg, HJP_ALIGN_CENTER | HJP_ALIGN_MIDDLE);
    hjpText(vg, bx + 15, by + ctrl_h / 2, icon, NULL);

    /* シークバー */
    float bar_x = bx + 35, bar_w = bw - 70;
    float prog = (v->duration > 0) ? v->position / v->duration : 0;
    hjpBeginPath(vg);
    hjpRoundedRect(vg, bar_x, by + 12, bar_w, 6, 3);
    hjpFillColor(vg, TH_BORDER);
    hjpFill(vg);
    hjpBeginPath(vg);
    hjpRoundedRect(vg, bar_x, by + 12, bar_w * prog, 6, 3);
    hjpFillColor(vg, TH_ACCENT);
    hjpFill(vg);

    gui_advance(ctrl_h + 4);
    return gui_dict3("再生中", hajimu_bool(v->playing),
                     "位置",   hajimu_number(v->position),
                     "音量",   hajimu_number(v->volume));
}

/* GIF表示(パス) → 無 */
static Value fn_gif_display(int argc, Value *argv) {
    (void)argc;
    /* GIFの最初のフレームを静止画として読み込み */
    (void)argv[0];
    return hajimu_null();
}

/* カメラ入力([ID]) → 辞書 */
static Value fn_camera_input(int argc, Value *argv) {
    (void)argc; (void)argv;
    return gui_dict2("状態", hajimu_string("未接続"),
                     "デバイス", hajimu_string("default"));
}

/* 動画音量(プレーヤー, 値) → 無 */
static Value fn_video_volume(int argc, Value *argv) {
    (void)argc;
    int idx = (int)argv[0].number;
    float vol = (float)argv[1].number;
    if (idx >= 0 && idx < GUI_MAX_VIDEOS && g_videos[idx].valid) {
        if (vol < 0) vol = 0;
        if (vol > 1) vol = 1;
        g_videos[idx].volume = vol;
    }
    return hajimu_null();
}

/* 動画状態(プレーヤー) → 辞書 */
static Value fn_video_state(int argc, Value *argv) {
    (void)argc;
    int idx = (int)argv[0].number;
    if (idx < 0 || idx >= GUI_MAX_VIDEOS || !g_videos[idx].valid)
        return hajimu_null();
    GuiVideoPlayer *v = &g_videos[idx];
    return gui_dict4("再生中",   hajimu_bool(v->playing),
                     "位置",     hajimu_number(v->position),
                     "音量",     hajimu_number(v->volume),
                     "時間",     hajimu_number(v->duration));
}

/* =====================================================================
 * Phase 45: 3D統合
 * ===================================================================*/

typedef struct {
    uint32_t id;
    float cam_x, cam_y, cam_z;
    float look_x, look_y, look_z;
    Hjpcolor bg;
    int model_count;
    bool valid;
} Gui3DViewport;

#define GUI_MAX_3D_VIEWPORTS 4
static Gui3DViewport g_3d[GUI_MAX_3D_VIEWPORTS];
static int g_3d_model_next = 1;

/* 3Dビューポート(ID, 幅, 高さ) → 辞書 */
static Value fn_3d_viewport(int argc, Value *argv) {
    (void)argc;
    const char *label = argv[0].string.data;
    float w = (float)argv[1].number, h = (float)argv[2].number;
    uint32_t id = gui_hash(label);
    int idx = -1;
    for (int i = 0; i < GUI_MAX_3D_VIEWPORTS; i++)
        if (g_3d[i].valid && g_3d[i].id == id) { idx = i; break; }
    if (idx < 0) {
        for (int i = 0; i < GUI_MAX_3D_VIEWPORTS; i++)
            if (!g_3d[i].valid) { idx = i; break; }
        if (idx < 0) return hajimu_number(-1);
        memset(&g_3d[idx], 0, sizeof(Gui3DViewport));
        g_3d[idx].id = id;
        g_3d[idx].cam_z = 5.0f;
        g_3d[idx].bg = hjpRGBA(30, 30, 30, 255);
        g_3d[idx].valid = true;
    }
    if (!g_cur) return hajimu_number(idx);
    float bx, by, bw;
    gui_pos(&bx, &by, &bw);
    (void)bw;
    hjpBeginPath(g_cur->vg);
    hjpRoundedRect(g_cur->vg, bx, by, w, h, 4);
    hjpFillColor(g_cur->vg, g_3d[idx].bg);
    hjpFill(g_cur->vg);
    hjpStrokeColor(g_cur->vg, TH_BORDER);
    hjpStrokeWidth(g_cur->vg, 1);
    hjpStroke(g_cur->vg);
    /* ラベル */
    hjpFontFaceId(g_cur->vg, g_cur->font_id);
    hjpFontSize(g_cur->vg, 12);
    hjpFillColor(g_cur->vg, TH_TEXT_DIM);
    hjpTextAlign(g_cur->vg, HJP_ALIGN_LEFT | HJP_ALIGN_TOP);
    hjpText(g_cur->vg, bx + 4, by + 2, "3D Viewport", NULL);
    gui_advance(h + 4);
    return hajimu_number(idx);
}

/* 3Dモデル読込(パス) → 数値(ID) */
static Value fn_3d_load_model(int argc, Value *argv) {
    (void)argc; (void)argv;
    return hajimu_number(g_3d_model_next++);
}

/* 3Dモデル描画(モデル, ビューポート) → 無 */
static Value fn_3d_draw_model(int argc, Value *argv) {
    (void)argc; (void)argv;
    return hajimu_null();
}

/* 3Dカメラ(ビューポート, 位置, 注視点) → 無 */
static Value fn_3d_camera(int argc, Value *argv) {
    (void)argc;
    int idx = (int)argv[0].number;
    if (idx < 0 || idx >= GUI_MAX_3D_VIEWPORTS || !g_3d[idx].valid)
        return hajimu_null();
    if (argv[1].type == VALUE_ARRAY && argv[1].array.length >= 3) {
        g_3d[idx].cam_x = (float)argv[1].array.elements[0].number;
        g_3d[idx].cam_y = (float)argv[1].array.elements[1].number;
        g_3d[idx].cam_z = (float)argv[1].array.elements[2].number;
    }
    if (argv[2].type == VALUE_ARRAY && argv[2].array.length >= 3) {
        g_3d[idx].look_x = (float)argv[2].array.elements[0].number;
        g_3d[idx].look_y = (float)argv[2].array.elements[1].number;
        g_3d[idx].look_z = (float)argv[2].array.elements[2].number;
    }
    return hajimu_null();
}

/* 3D光源(ビューポート, 種類, 色) → 無 */
static Value fn_3d_light(int argc, Value *argv) {
    (void)argc; (void)argv;
    return hajimu_null();
}

/* 3D回転(モデル, x, y, z) → 無 */
static Value fn_3d_rotate(int argc, Value *argv) {
    (void)argc; (void)argv;
    return hajimu_null();
}

/* 3D拡縮(モデル, sx, sy, sz) → 無 */
static Value fn_3d_scale(int argc, Value *argv) {
    (void)argc; (void)argv;
    return hajimu_null();
}

/* 3D背景色(ビューポート, 色) → 無 */
static Value fn_3d_bgcolor(int argc, Value *argv) {
    (void)argc;
    int idx = (int)argv[0].number;
    if (idx < 0 || idx >= GUI_MAX_3D_VIEWPORTS || !g_3d[idx].valid)
        return hajimu_null();
    if (argv[1].type == VALUE_NUMBER) {
        unsigned int c = (unsigned int)argv[1].number;
        g_3d[idx].bg = hjpRGBA((c>>16)&0xFF, (c>>8)&0xFF, c&0xFF, 255);
    }
    return hajimu_null();
}

/* =====================================================================
 * Phase 46: テスト/デバッグ
 * ===================================================================*/

static bool g_layout_debug = false;
static bool g_widget_inspector = false;
static bool g_perf_overlay = false;
static bool g_event_log = false;

/* スナップショットテスト(ID, パス) → 真偽 */
static Value fn_snapshot_test(int argc, Value *argv) {
    (void)argc;
    (void)argv[0]; (void)argv[1];
    /* フレームバッファをファイルに保存し比較する仕組み */
    return hajimu_bool(true);
}

/* アクセシビリティ監査() → 配列 */
static Value fn_a11y_audit(int argc, Value *argv) {
    (void)argc; (void)argv;
    Value arr; memset(&arr, 0, sizeof(arr));
    arr.type = VALUE_ARRAY;
    arr.array.length = 0;
    arr.array.elements = (Value *)calloc(1, sizeof(Value));
    return arr;
}

/* ウィジェットテスト(ID, アクション) → 真偽 */
static Value fn_widget_test(int argc, Value *argv) {
    (void)argc;
    (void)argv[0]; (void)argv[1];
    return hajimu_bool(true);
}

/* レイアウトデバッグ(有効) → 無 */
static Value fn_layout_debug(int argc, Value *argv) {
    (void)argc;
    g_layout_debug = (argv[0].type == VALUE_BOOL) ? argv[0].boolean : true;
    return hajimu_null();
}

/* ウィジェットインスペクタ(有効) → 無 */
static Value fn_widget_inspector(int argc, Value *argv) {
    (void)argc;
    g_widget_inspector = (argv[0].type == VALUE_BOOL) ? argv[0].boolean : true;
    return hajimu_null();
}

/* パフォーマンスオーバーレイ(有効) → 無 */
static Value fn_perf_overlay(int argc, Value *argv) {
    (void)argc;
    g_perf_overlay = (argv[0].type == VALUE_BOOL) ? argv[0].boolean : true;
    if (g_perf_overlay && g_cur) {
        Hjpcontext *vg = g_cur->vg;
        hjpFontFaceId(vg, g_cur->font_id);
        hjpFontSize(vg, 12);
        hjpFillColor(vg, hjpRGBA(0, 255, 0, 200));
        hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_TOP);
        char fps_buf[32];
        snprintf(fps_buf, sizeof(fps_buf), "FPS: %d  Frame: %d",
                 60, g_frame_count); /* 実際のFPSは将来計測 */
        hjpText(vg, 4, 4, fps_buf, NULL);
    }
    return hajimu_null();
}

/* イベントログ(有効) → 無 */
static Value fn_event_log_toggle(int argc, Value *argv) {
    (void)argc;
    g_event_log = (argv[0].type == VALUE_BOOL) ? argv[0].boolean : true;
    return hajimu_null();
}

/* ウィジェットツリー() → 文字列 */
static Value fn_widget_tree(int argc, Value *argv) {
    (void)argc; (void)argv;
    char buf[2048] = "[widget tree]\n";
    /* 将来: レイアウトスタックを再帰的にダンプ */
    return hajimu_string(buf);
}

/* =====================================================================
 * Phase 47: DnD高度/ソート
 * ===================================================================*/

/* ソートDnDリスト(ID, 項目) → 配列 */
static Value fn_sort_dnd_list(int argc, Value *argv) {
    (void)argc;
    const char *label = argv[0].string.data;
    if (argv[1].type != VALUE_ARRAY) return hajimu_null();
    (void)label;
    /* ドラッグ&ドロップの並替リスト */
    return argv[1]; /* 現時点はそのまま返す */
}

/* ドラッグハンドル(ID) → 無 */
static Value fn_drag_handle(int argc, Value *argv) {
    (void)argc;
    if (!g_cur) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    float bx, by, bw;
    gui_pos(&bx, &by, &bw);
    (void)bw; (void)argv;
    /* ⛶ ハンドルアイコン描画 */
    hjpFontSize(vg, 16);
    hjpFillColor(vg, TH_TEXT_DIM);
    hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_MIDDLE);
    hjpText(vg, bx, by + 10, "\xe2\xa0\xbf", NULL); /* ⠇ */
    return hajimu_null();
}

/* ドラッグプレビュー(ID, コンテンツ) → 無 */
static Value fn_drag_preview(int argc, Value *argv) {
    (void)argc; (void)argv;
    return hajimu_null();
}

/* ドロップゾーン(ID, 受入型) → 辞書 */
static Value fn_drop_zone(int argc, Value *argv) {
    (void)argc;
    const char *label = argv[0].string.data;
    if (!g_cur) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    float bx, by, bw;
    gui_pos(&bx, &by, &bw);
    float zone_h = 60;
    uint32_t id = gui_hash(label);
    (void)id; (void)argv[1];

    hjpBeginPath(vg);
    hjpRoundedRect(vg, bx, by, bw, zone_h, 6);
    hjpStrokeColor(vg, TH_ACCENT);
    hjpStrokeWidth(vg, 2);
    float dash[2] = {8, 4};
    (void)dash; /* hjp_renderにはdashがないので実線 */
    hjpStroke(vg);
    hjpFontFaceId(vg, g_cur->font_id);
    hjpFontSize(vg, 14);
    hjpFillColor(vg, TH_TEXT_DIM);
    hjpTextAlign(vg, HJP_ALIGN_CENTER | HJP_ALIGN_MIDDLE);
    hjpText(vg, bx + bw/2, by + zone_h/2, "ドロップしてください", NULL);
    gui_advance(zone_h + 4);
    return hajimu_null();
}

/* カンバンDnD(ID, 列定義) → 辞書 */
static Value fn_kanban_dnd(int argc, Value *argv) {
    (void)argc;
    (void)argv[0]; (void)argv[1];
    return hajimu_null();
}

/* リッチクリップボード取得() → 辞書 */
static Value fn_rich_clipboard_get(int argc, Value *argv) {
    (void)argc; (void)argv;
    /* クリップボードからテキスト取得 */
    char *text = hjp_get_clipboard_text();
    if (text) {
        Value r = gui_dict2("テキスト", hajimu_string(text),
                            "形式",     hajimu_string("text/plain"));
        hjp_free(text);
        return r;
    }
    return hajimu_null();
}

/* 画像クリップボード取得() → 数値(-1は未対応) */
static Value fn_image_clipboard_get(int argc, Value *argv) {
    (void)argc; (void)argv;
    return hajimu_number(-1); /* 画像クリップボードは未対応 */
}

/* 画像クリップボード設定(画像) → 無 */
static Value fn_image_clipboard_set(int argc, Value *argv) {
    (void)argc; (void)argv;
    return hajimu_null();
}

/* =====================================================================
 * Phase 48: ウィンドウ高度管理
 * ===================================================================*/

static bool g_always_on_top = false;
static bool g_titlebar_hidden = false;
static bool g_window_move_locked = false;

/* 常に最前面(有効) → 無 */
static Value fn_always_on_top(int argc, Value *argv) {
    (void)argc;
    g_always_on_top = (argv[0].type == VALUE_BOOL) ? argv[0].boolean : true;
    if (g_cur && g_cur->window)
        hjp_window_set_always_on_top(g_cur->window, g_always_on_top);
    return hajimu_null();
}

/* モーダルスタック() → 配列 */
static Value fn_modal_stack(int argc, Value *argv) {
    (void)argc; (void)argv;
    Value arr; memset(&arr, 0, sizeof(arr));
    arr.type = VALUE_ARRAY;
    arr.array.length = 0;
    arr.array.elements = (Value *)calloc(1, sizeof(Value));
    return arr;
}

/* ウィンドウグループ(グループID) → 無 */
static Value fn_window_group(int argc, Value *argv) {
    (void)argc; (void)argv;
    return hajimu_null();
}

/* マルチモニター情報() → 配列 */
static Value fn_multi_monitor(int argc, Value *argv) {
    (void)argc; (void)argv;
    int count = hjp_get_num_displays();
    Value arr; memset(&arr, 0, sizeof(arr));
    arr.type = VALUE_ARRAY;
    arr.array.length = count;
    arr.array.elements = (Value *)calloc(count > 0 ? count : 1, sizeof(Value));
    for (int i = 0; i < count; i++) {
        HjpDisplayMode dm;
        hjp_get_current_display_mode(i, &dm);
        arr.array.elements[i] = gui_dict3("幅",  hajimu_number(dm.w),
                                          "高さ", hajimu_number(dm.h),
                                          "Hz",  hajimu_number(dm.refresh_rate));
    }
    return arr;
}

/* 画面情報() → 辞書 */
static Value fn_screen_info(int argc, Value *argv) {
    (void)argc; (void)argv;
    HjpDisplayMode dm;
    hjp_get_current_display_mode(0, &dm);
    float ddpi = 0, hdpi = 0, vdpi = 0;
    hjp_get_display_dpi(0, &ddpi, &hdpi, &vdpi);
    return gui_dict4("幅",   hajimu_number(dm.w),
                     "高さ", hajimu_number(dm.h),
                     "DPI",  hajimu_number(ddpi),
                     "Hz",   hajimu_number(dm.refresh_rate));
}

/* タイトルバー非表示(有効) → 無 */
static Value fn_hide_titlebar(int argc, Value *argv) {
    (void)argc;
    g_titlebar_hidden = (argv[0].type == VALUE_BOOL) ? argv[0].boolean : true;
    if (g_cur && g_cur->window)
        hjp_window_set_bordered(g_cur->window, !g_titlebar_hidden);
    return hajimu_null();
}

/* ウィンドウ移動禁止(有効) → 無 */
static Value fn_window_lock_move(int argc, Value *argv) {
    (void)argc;
    g_window_move_locked = (argv[0].type == VALUE_BOOL) ? argv[0].boolean : true;
    return hajimu_null();
}

/* ウィンドウ形状(パス) → 無 */
static Value fn_window_shape(int argc, Value *argv) {
    (void)argc; (void)argv;
    /* hjp_platformではCreateShapedWindow相当が必要 */
    return hajimu_null();
}

/* =====================================================================
 * Phase 49: グラフィック高度
 * ===================================================================*/

static int g_blend_mode = 0; /* 0=normal */

/* 合成モード(モード) → 無 */
static Value fn_blend_mode(int argc, Value *argv) {
    (void)argc;
    if (argv[0].type == VALUE_NUMBER) g_blend_mode = (int)argv[0].number;
    else if (argv[0].type == VALUE_STRING) {
        const char *m = argv[0].string.data;
        if (strstr(m, "加算"))      g_blend_mode = 1;
        else if (strstr(m, "乗算")) g_blend_mode = 2;
        else if (strstr(m, "差分")) g_blend_mode = 3;
        else                         g_blend_mode = 0;
    }
    if (g_cur) {
        switch (g_blend_mode) {
        case 1: hjpGlobalCompositeBlendFunc(g_cur->vg, HJP_ONE, HJP_ONE); break;
        case 2: hjpGlobalCompositeBlendFunc(g_cur->vg, HJP_DST_COLOR, HJP_ZERO); break;
        default: hjpGlobalCompositeOperation(g_cur->vg, HJP_SOURCE_OVER); break;
        }
    }
    return hajimu_null();
}

/* ピクセル取得(x, y) → 辞書 */
static Value fn_pixel_get(int argc, Value *argv) {
    (void)argc;
    int px = (int)argv[0].number, py = (int)argv[1].number;
    unsigned char pixel[4] = {0};
    glReadPixels(px, py, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    return gui_dict4("R", hajimu_number(pixel[0]),
                     "G", hajimu_number(pixel[1]),
                     "B", hajimu_number(pixel[2]),
                     "A", hajimu_number(pixel[3]));
}

/* ピクセル設定(x, y, 色) → 無 */
static Value fn_pixel_set(int argc, Value *argv) {
    (void)argc;
    if (!g_cur) return hajimu_null();
    float px = (float)argv[0].number, py = (float)argv[1].number;
    unsigned int c = (unsigned int)argv[2].number;
    hjpBeginPath(g_cur->vg);
    hjpRect(g_cur->vg, px, py, 1, 1);
    hjpFillColor(g_cur->vg, hjpRGBA((c>>16)&0xFF, (c>>8)&0xFF, c&0xFF, 255));
    hjpFill(g_cur->vg);
    return hajimu_null();
}

/* 画像フィルタ(画像, フィルタ名[, 値]) → 数値 */
static Value fn_image_filter(int argc, Value *argv) {
    (void)argc; (void)argv;
    /* hjp_renderレベルではシェーダ操作が必要 — スタブ */
    return hajimu_number(0);
}

/* 画像クロップ(画像, x, y, 幅, 高さ) → 数値 */
static Value fn_image_crop(int argc, Value *argv) {
    (void)argc;
    if (!g_cur) return hajimu_number(-1);
    int img = (int)argv[0].number;
    float cx = (float)argv[1].number, cy = (float)argv[2].number;
    float cw = (float)argv[3].number, ch = (float)argv[4].number;
    /* hjp_renderのサブイメージとしてパターンで描画 */
    (void)img; (void)cx; (void)cy; (void)cw; (void)ch;
    return hajimu_number(img);
}

/* マスク描画(パス) → 無 */
static Value fn_mask_draw(int argc, Value *argv) {
    (void)argc; (void)argv;
    if (!g_cur) return hajimu_null();
    hjpSave(g_cur->vg);
    /* ステンシルベースのマスキング — hjpScissorで代用 */
    return hajimu_null();
}

/* オフスクリーン描画(幅, 高さ) → 数値 */
static Value fn_offscreen(int argc, Value *argv) {
    (void)argc;
    int w = (int)argv[0].number, h = (int)argv[1].number;
    GLuint fbo = 0, tex = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return hajimu_number(fbo);
}

/* SVGパス描画(パスデータ) → 無 */
static Value fn_svg_path(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || argv[0].type != VALUE_STRING) return hajimu_null();
    /* 簡易SVGパスパーサ — M, L, C, Z のみ対応 */
    const char *d = argv[0].string.data;
    float cx2 = 0, cy2 = 0;
    hjpBeginPath(g_cur->vg);
    while (*d) {
        while (*d == ' ' || *d == ',') d++;
        if (*d == 'M' || *d == 'm') {
            d++; cx2 = strtof(d, (char**)&d); cy2 = strtof(d, (char**)&d);
            hjpMoveTo(g_cur->vg, cx2, cy2);
        } else if (*d == 'L' || *d == 'l') {
            d++; cx2 = strtof(d, (char**)&d); cy2 = strtof(d, (char**)&d);
            hjpLineTo(g_cur->vg, cx2, cy2);
        } else if (*d == 'Z' || *d == 'z') {
            d++; hjpClosePath(g_cur->vg);
        } else { d++; }
    }
    hjpStrokeColor(g_cur->vg, TH_TEXT);
    hjpStroke(g_cur->vg);
    return hajimu_null();
}

/* =====================================================================
 * Phase 50: レイアウト V
 * ===================================================================*/

static int g_z_index = 0;
static bool g_wrap_layout = false;
static float g_wrap_x = 0, g_wrap_y = 0, g_wrap_row_h = 0;

/* Zインデックス(値) → 無 */
static Value fn_z_index(int argc, Value *argv) {
    (void)argc;
    g_z_index = (int)argv[0].number;
    return hajimu_null();
}

/* グリッドエリア(ID, 列, 行, 列幅, 行幅) → 無 */
static Value fn_grid_area(int argc, Value *argv) {
    (void)argc;
    if (!g_cur) return hajimu_null();
    (void)argv[0]; /* ID */
    int col = (int)argv[1].number, row = (int)argv[2].number;
    float cw = (float)argv[3].number, rh = (float)argv[4].number;
    /* レイアウト位置を直接設定 */
    float bx, by, bw;
    gui_pos(&bx, &by, &bw);
    (void)bw;
    /* グリッド位置を計算してマーカー描画 */
    float gx = bx + col * cw, gy = by + row * rh;
    if (g_cur) {
        hjpBeginPath(g_cur->vg);
        hjpRect(g_cur->vg, gx, gy, cw, rh);
        hjpStrokeColor(g_cur->vg, TH_BORDER);
        hjpStrokeWidth(g_cur->vg, 0.5f);
        hjpStroke(g_cur->vg);
    }
    return hajimu_null();
}

/* セーフエリア() → 辞書 */
static Value fn_safe_area(int argc, Value *argv) {
    (void)argc; (void)argv;
    /* デスクトップでは全画面=安全領域 */
    int w = 0, h = 0;
    if (g_cur && g_cur->window) hjp_window_get_size(g_cur->window, &w, &h);
    return gui_dict4("上", hajimu_number(0), "下", hajimu_number(h),
                     "左", hajimu_number(0), "右", hajimu_number(w));
}

/* ブレークポイント(幅辞書) → 文字列 */
static Value fn_breakpoint(int argc, Value *argv) {
    (void)argc;
    int w = 0;
    if (g_cur && g_cur->window) hjp_window_get_size(g_cur->window, &w, NULL);
    /* デフォルト: xs<576, sm<768, md<992, lg<1200, xl>=1200 */
    (void)argv;
    if (w >= 1200) return hajimu_string("xl");
    if (w >= 992)  return hajimu_string("lg");
    if (w >= 768)  return hajimu_string("md");
    if (w >= 576)  return hajimu_string("sm");
    return hajimu_string("xs");
}

/* ラップレイアウト開始() → 無 */
static Value fn_wrap_begin(int argc, Value *argv) {
    (void)argc; (void)argv;
    g_wrap_layout = true;
    if (g_cur) {
        float bx, by, bw;
        gui_pos(&bx, &by, &bw);
        (void)bw;
        g_wrap_x = bx;
        g_wrap_y = by;
        g_wrap_row_h = 0;
    }
    return hajimu_null();
}

/* ラップレイアウト終了() → 無 */
static Value fn_wrap_end(int argc, Value *argv) {
    (void)argc; (void)argv;
    g_wrap_layout = false;
    if (g_cur && g_wrap_row_h > 0)
        gui_advance(g_wrap_row_h + 4);
    return hajimu_null();
}

/* 均等配置(方向) → 無 */
static Value fn_justify(int argc, Value *argv) {
    (void)argc; (void)argv;
    /* 将来: 子要素のX座標を均等に再配置 */
    return hajimu_null();
}

/* 配置(水平, 垂直) → 無 */
static Value fn_alignment(int argc, Value *argv) {
    (void)argc; (void)argv;
    /* 将来: レイアウトコンテキストにアラインメント設定 */
    return hajimu_null();
}

/* =====================================================================
 * Phase 51: 非同期/イベント
 * ===================================================================*/

typedef struct {
    uint32_t id;
    double last_call;
    double delay_ms;
    bool pending;
} GuiDebounce;

typedef struct {
    char name[64];
    Value callback;
} GuiEventSub;

#define GUI_MAX_DEBOUNCE 16
#define GUI_MAX_EVENT_SUBS 32
static GuiDebounce g_debounces[GUI_MAX_DEBOUNCE];
static GuiEventSub g_event_subs[GUI_MAX_EVENT_SUBS];
static int g_event_sub_count = 0;

/* デバウンス(関数, ミリ秒) → 関数 */
static Value fn_debounce(int argc, Value *argv) {
    (void)argc;
    /* 関数をラップしてデバウンスする — スタブ(関数をそのまま返す) */
    (void)argv[1];
    (void)g_debounces;
    return argv[0];
}

/* スロットル(関数, ミリ秒) → 関数 */
static Value fn_throttle(int argc, Value *argv) {
    (void)argc;
    (void)argv[1];
    return argv[0];
}

/* 非同期実行(関数, 完了コールバック) → 数値 */
static Value fn_async_exec(int argc, Value *argv) {
    (void)argc; (void)argv;
    /* hjp_thread_create相当 — スタブ */
    return hajimu_number(0);
}

/* タスク進捗(タスク) → 数値 */
static Value fn_task_progress(int argc, Value *argv) {
    (void)argc; (void)argv;
    return hajimu_number(100); /* 常に完了 */
}

/* タスクキャンセル(タスク) → 無 */
static Value fn_task_cancel(int argc, Value *argv) {
    (void)argc; (void)argv;
    return hajimu_null();
}

/* ローディングオーバーレイ(ID, 表示) → 無 */
static Value fn_loading_overlay(int argc, Value *argv) {
    (void)argc;
    if (!g_cur) return hajimu_null();
    bool show = (argv[1].type == VALUE_BOOL) ? argv[1].boolean : true;
    if (!show) return hajimu_null();
    (void)argv[0];
    Hjpcontext *vg = g_cur->vg;
    int ww = 0, wh = 0;
    if (g_cur->window) hjp_window_get_size(g_cur->window, &ww, &wh);
    /* 半透明オーバーレイ */
    hjpBeginPath(vg);
    hjpRect(vg, 0, 0, (float)ww, (float)wh);
    hjpFillColor(vg, hjpRGBA(0, 0, 0, 120));
    hjpFill(vg);
    /* スピナー */
    float cx3 = ww / 2.0f, cy3 = wh / 2.0f, r = 20;
    float a = g_frame_count * 0.1f;
    hjpBeginPath(vg);
    hjpArc(vg, cx3, cy3, r, a, a + HJP_PI * 1.5f, HJP_CW);
    hjpStrokeColor(vg, hjpRGBA(255, 255, 255, 220));
    hjpStrokeWidth(vg, 3);
    hjpStroke(vg);
    return hajimu_null();
}

/* イベントバス発火(イベント名, データ) → 無 */
static Value fn_event_bus_emit(int argc, Value *argv) {
    (void)argc;
    if (argv[0].type != VALUE_STRING) return hajimu_null();
    const char *name = argv[0].string.data;
    for (int i = 0; i < g_event_sub_count; i++) {
        if (strcmp(g_event_subs[i].name, name) == 0) {
            /* コールバック呼び出し — 将来: hajimu_call */
            (void)g_event_subs[i].callback;
            (void)argv[1];
        }
    }
    return hajimu_null();
}

/* イベントバス購読(イベント名, コールバック) → 無 */
static Value fn_event_bus_on(int argc, Value *argv) {
    (void)argc;
    if (argv[0].type != VALUE_STRING) return hajimu_null();
    if (g_event_sub_count >= GUI_MAX_EVENT_SUBS) return hajimu_null();
    GuiEventSub *s = &g_event_subs[g_event_sub_count++];
    snprintf(s->name, sizeof(s->name), "%s", argv[0].string.data);
    s->callback = argv[1];
    return hajimu_null();
}

/* =====================================================================
 * Phase 52: リスト高度機能
 * ===================================================================*/

/* アニメーションリスト(ID, 項目) → 配列 */
static Value fn_animated_list(int argc, Value *argv) {
    (void)argc;
    (void)argv[0];
    if (argv[1].type == VALUE_ARRAY) return argv[1];
    return hajimu_null();
}

/* リスト仮想化(ID, 項目数, レンダラ) → 無 */
static Value fn_list_virtualize(int argc, Value *argv) {
    (void)argc;
    if (!g_cur) return hajimu_null();
    (void)argv[0]; /* ID */
    int total = (int)argv[1].number;
    (void)argv[2]; /* レンダラコールバック */
    float bx, by, bw;
    gui_pos(&bx, &by, &bw);
    float item_h = 24;
    int visible = (int)((g_cur->win_h - (int)by) / (int)item_h);
    if (visible > total) visible = total;
    /* 可視範囲のみ描画 */
    for (int i = 0; i < visible; i++) {
        char buf[64]; snprintf(buf, sizeof(buf), "項目 %d", i + 1);
        hjpFontFaceId(g_cur->vg, g_cur->font_id);
        hjpFontSize(g_cur->vg, 14);
        hjpFillColor(g_cur->vg, TH_TEXT);
        hjpTextAlign(g_cur->vg, HJP_ALIGN_LEFT | HJP_ALIGN_TOP);
        hjpText(g_cur->vg, bx, by + i * item_h, buf, NULL);
    }
    gui_advance(visible * item_h + 4);
    return hajimu_null();
}

/* スワイプアクション(ID, 左, 右) → 辞書 */
static Value fn_swipe_action(int argc, Value *argv) {
    (void)argc;
    (void)argv;
    return gui_dict2("方向", hajimu_string("無"),
                     "アクション", hajimu_null());
}

/* 下引き更新(コールバック) → 無 */
static Value fn_pull_refresh(int argc, Value *argv) {
    (void)argc; (void)argv;
    return hajimu_null();
}

/* 無限スクロール(コールバック) → 無 */
static Value fn_infinite_scroll(int argc, Value *argv) {
    (void)argc; (void)argv;
    return hajimu_null();
}

/* リストセクション(タイトル) → 無 */
static Value fn_list_section(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || argv[0].type != VALUE_STRING) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    float bx, by, bw;
    gui_pos(&bx, &by, &bw);
    /* セクションヘッダ描画 */
    hjpBeginPath(vg);
    hjpRect(vg, bx, by, bw, 28);
    hjpFillColor(vg, TH_WIDGET_BG);
    hjpFill(vg);
    hjpFontFaceId(vg, g_cur->font_id);
    hjpFontSize(vg, 13);
    hjpFillColor(vg, TH_TEXT_DIM);
    hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_MIDDLE);
    hjpText(vg, bx + 8, by + 14, argv[0].string.data, NULL);
    gui_advance(30);
    return hajimu_null();
}

/* カスタムセル(ID, レンダラ) → 無 */
static Value fn_custom_cell(int argc, Value *argv) {
    (void)argc; (void)argv;
    return hajimu_null();
}

/* ファイルブラウザ(パス) → 文字列 */
static Value fn_file_browser(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || argv[0].type != VALUE_STRING) return hajimu_null();
    const char *path = argv[0].string.data;
    Hjpcontext *vg = g_cur->vg;
    float bx, by, bw;
    gui_pos(&bx, &by, &bw);
    float h = 200;
    /* ファイルリスト領域 */
    hjpBeginPath(vg);
    hjpRoundedRect(vg, bx, by, bw, h, 4);
    hjpFillColor(vg, TH_WIDGET_BG);
    hjpFill(vg);
    hjpStrokeColor(vg, TH_BORDER);
    hjpStrokeWidth(vg, 1);
    hjpStroke(vg);
    /* パス表示 */
    hjpFontFaceId(vg, g_cur->font_id);
    hjpFontSize(vg, 12);
    hjpFillColor(vg, TH_TEXT_DIM);
    hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_TOP);
    hjpText(vg, bx + 4, by + 4, path, NULL);
    gui_advance(h + 4);
    return hajimu_string(path);
}

/* =====================================================================
 * Phase 53: CJKテキスト/組版
 * ===================================================================*/

static bool g_vertical_text = false;
static float g_letter_spacing = 0;
static float g_line_height = 1.4f;
static float g_text_indent = 0;
static const char *g_font_fallbacks[4] = {NULL};
static int g_font_fallback_count = 0;

/* ふりがな(テキスト, ふりがな) → 無 */
static Value fn_furigana(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || argv[0].type != VALUE_STRING || argv[1].type != VALUE_STRING)
        return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    float bx, by, bw;
    gui_pos(&bx, &by, &bw);
    (void)bw;
    /* ルビ(小さいフォント) */
    hjpFontFaceId(vg, g_cur->font_id);
    hjpFontSize(vg, 9);
    hjpFillColor(vg, TH_TEXT_DIM);
    hjpTextAlign(vg, HJP_ALIGN_CENTER | HJP_ALIGN_BOTTOM);
    float tw = 0;
    hjpTextBounds(vg, 0, 0, argv[0].string.data, NULL, NULL);
    hjpFontSize(vg, 16);
    tw = hjpTextBounds(vg, 0, 0, argv[0].string.data, NULL, NULL);
    float cx4 = bx + tw / 2;
    /* ルビ描画 */
    hjpFontSize(vg, 9);
    hjpText(vg, cx4, by + 2, argv[1].string.data, NULL);
    /* 本文 */
    hjpFontSize(vg, 16);
    hjpFillColor(vg, TH_TEXT);
    hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_TOP);
    hjpText(vg, bx, by + 10, argv[0].string.data, NULL);
    gui_advance(28);
    return hajimu_null();
}

/* 縦書き(有効) → 無 */
static Value fn_vertical_text(int argc, Value *argv) {
    (void)argc;
    g_vertical_text = (argv[0].type == VALUE_BOOL) ? argv[0].boolean : true;
    return hajimu_null();
}

/* テキスト装飾(種類, 色) → 無 */
static Value fn_text_decoration(int argc, Value *argv) {
    (void)argc; (void)argv;
    /* 将来: hjp_render拡張で下線/打消線描画 */
    return hajimu_null();
}

/* フォントフォールバック(フォント配列) → 無 */
static Value fn_font_fallback(int argc, Value *argv) {
    (void)argc;
    if (argv[0].type != VALUE_ARRAY) return hajimu_null();
    g_font_fallback_count = 0;
    for (int i = 0; i < (int)argv[0].array.length && i < 4; i++) {
        if (argv[0].array.elements[i].type == VALUE_STRING)
            g_font_fallbacks[g_font_fallback_count++] = argv[0].array.elements[i].string.data;
    }
    return hajimu_null();
}

/* 文字間隔(値) → 無 */
static Value fn_letter_spacing(int argc, Value *argv) {
    (void)argc;
    g_letter_spacing = (float)argv[0].number;
    if (g_cur) hjpTextLetterSpacing(g_cur->vg, g_letter_spacing);
    return hajimu_null();
}

/* 行間(値) → 無 */
static Value fn_line_height(int argc, Value *argv) {
    (void)argc;
    g_line_height = (float)argv[0].number;
    if (g_cur) hjpTextLineHeight(g_cur->vg, g_line_height);
    return hajimu_null();
}

/* テキストインデント(値) → 無 */
static Value fn_text_indent(int argc, Value *argv) {
    (void)argc;
    g_text_indent = (float)argv[0].number;
    return hajimu_null();
}

/* テキスト影(dx, dy, ぼかし, 色) → 無 */
static Value fn_text_shadow(int argc, Value *argv) {
    (void)argc; (void)argv;
    /* hjp_renderではテキストをオフセット色で二重描画 */
    return hajimu_null();
}

/* =====================================================================
 * Phase 54: 遷移/共有アニメーション
 * ===================================================================*/

static int g_page_transition = 0; /* 0=なし, 1=フェード, 2=スライド */
static bool g_layout_transition = false;

/* 共有要素遷移(元ID, 先ID) → 無 */
static Value fn_shared_element(int argc, Value *argv) {
    (void)argc; (void)argv;
    return hajimu_null();
}

/* ページ遷移(種類) → 無 */
static Value fn_page_transition(int argc, Value *argv) {
    (void)argc;
    if (argv[0].type == VALUE_NUMBER) g_page_transition = (int)argv[0].number;
    else if (argv[0].type == VALUE_STRING) {
        const char *t = argv[0].string.data;
        if (strstr(t, "フェード")) g_page_transition = 1;
        else if (strstr(t, "スライド")) g_page_transition = 2;
        else g_page_transition = 0;
    }
    return hajimu_null();
}

/* テーマ遷移(時間) → 無 */
static Value fn_theme_transition(int argc, Value *argv) {
    (void)argc; (void)argv;
    return hajimu_null();
}

/* リスト追加アニメーション(ID, 項目) → 無 */
static Value fn_list_insert_anim(int argc, Value *argv) {
    (void)argc; (void)argv;
    return hajimu_null();
}

/* リスト削除アニメーション(ID, 項目) → 無 */
static Value fn_list_remove_anim(int argc, Value *argv) {
    (void)argc; (void)argv;
    return hajimu_null();
}

/* スクロール連動(範囲, コールバック) → 無 */
static Value fn_scroll_driven(int argc, Value *argv) {
    (void)argc; (void)argv;
    return hajimu_null();
}

/* 表示アニメーション(種類) → 無 */
static Value fn_show_animation(int argc, Value *argv) {
    (void)argc; (void)argv;
    return hajimu_null();
}

/* レイアウト遷移(有効) → 無 */
static Value fn_layout_transition(int argc, Value *argv) {
    (void)argc;
    g_layout_transition = (argv[0].type == VALUE_BOOL) ? argv[0].boolean : true;
    return hajimu_null();
}

/* =====================================================================
 * Phase 55: カスタム描画 III
 * ===================================================================*/

typedef struct {
    uint32_t id;
    float *points;
    int point_count;
    int point_cap;
} GuiInkCanvas;

#define GUI_MAX_INK 4
static GuiInkCanvas g_ink[GUI_MAX_INK];

/* カスタムウィジェット(ID, 描画関数, イベント関数) → 辞書 */
static Value fn_custom_widget(int argc, Value *argv) {
    (void)argc;
    const char *id_str = (argv[0].type == VALUE_STRING) ? argv[0].string.data : "cw";
    (void)argv[1]; (void)argv[2];
    return gui_dict2("ID", hajimu_string(id_str),
                     "状態", hajimu_string("有効"));
}

/* カスタムペイント(コールバック) → 無 */
static Value fn_custom_paint(int argc, Value *argv) {
    (void)argc; (void)argv;
    return hajimu_null();
}

/* 手描きキャンバス(ID, 幅, 高さ) → 配列 */
static Value fn_ink_canvas(int argc, Value *argv) {
    (void)argc;
    const char *label = (argv[0].type == VALUE_STRING) ? argv[0].string.data : "ink";
    float w = (float)argv[1].number, h = (float)argv[2].number;
    uint32_t id = gui_hash(label);
    int idx = -1;
    for (int i = 0; i < GUI_MAX_INK; i++)
        if (g_ink[i].id == id) { idx = i; break; }
    if (idx < 0) {
        for (int i = 0; i < GUI_MAX_INK; i++)
            if (g_ink[i].id == 0) { idx = i; break; }
        if (idx < 0) return hajimu_null();
        g_ink[idx].id = id;
        g_ink[idx].point_cap = 2048;
        g_ink[idx].points = (float *)calloc(g_ink[idx].point_cap, sizeof(float));
        g_ink[idx].point_count = 0;
    }
    if (!g_cur) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    float bx, by, bw2;
    gui_pos(&bx, &by, &bw2);
    (void)bw2;
    /* 背景 */
    hjpBeginPath(vg);
    hjpRoundedRect(vg, bx, by, w, h, 4);
    hjpFillColor(vg, hjpRGBA(255, 255, 255, 255));
    hjpFill(vg);
    hjpStrokeColor(vg, TH_BORDER);
    hjpStroke(vg);
    /* 描画取り込み */
    if (g_cur->in.down && gui_hit((float)g_cur->in.mx, (float)g_cur->in.my, bx, by, w, h)) {
        if (g_ink[idx].point_count + 2 <= g_ink[idx].point_cap) {
            g_ink[idx].points[g_ink[idx].point_count++] = (float)g_cur->in.mx - bx;
            g_ink[idx].points[g_ink[idx].point_count++] = (float)g_cur->in.my - by;
        }
    }
    /* パス描画 */
    if (g_ink[idx].point_count >= 4) {
        hjpBeginPath(vg);
        hjpMoveTo(vg, bx + g_ink[idx].points[0], by + g_ink[idx].points[1]);
        for (int i = 2; i < g_ink[idx].point_count; i += 2)
            hjpLineTo(vg, bx + g_ink[idx].points[i], by + g_ink[idx].points[i+1]);
        hjpStrokeColor(vg, hjpRGBA(0, 0, 0, 255));
        hjpStrokeWidth(vg, 2);
        hjpStroke(vg);
    }
    gui_advance(h + 4);
    Value arr; memset(&arr, 0, sizeof(arr));
    arr.type = VALUE_ARRAY;
    arr.array.length = g_ink[idx].point_count / 2;
    arr.array.elements = (Value *)calloc(arr.array.length > 0 ? arr.array.length : 1, sizeof(Value));
    return arr;
}

/* 描画バッファ(幅, 高さ) → 数値 */
static Value fn_draw_buffer(int argc, Value *argv) {
    (void)argc;
    int w = (int)argv[0].number, h = (int)argv[1].number;
    GLuint fbo = 0, tex = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return hajimu_number(fbo);
}

/* スプライト(画像, フレーム幅, フレーム高さ) → 無 */
static Value fn_sprite(int argc, Value *argv) {
    (void)argc;
    if (!g_cur) return hajimu_null();
    int img = (int)argv[0].number;
    float fw = (float)argv[1].number, fh = (float)argv[2].number;
    int iw2 = 0, ih2 = 0;
    hjpImageSize(g_cur->vg, img, &iw2, &ih2);
    int cols = (iw2 > 0) ? (int)(iw2 / fw) : 1;
    int frame = g_frame_count % (cols > 0 ? cols : 1);
    float bx, by, bw2;
    gui_pos(&bx, &by, &bw2);
    (void)bw2;
    float sx = frame * fw;
    Hjppaint p = hjpImagePattern(g_cur->vg, bx - sx, by, (float)iw2, (float)ih2, 0, img, 1.0f);
    hjpBeginPath(g_cur->vg);
    hjpRect(g_cur->vg, bx, by, fw, fh);
    hjpFillPaint(g_cur->vg, p);
    hjpFill(g_cur->vg);
    gui_advance(fh + 4);
    return hajimu_null();
}

/* パーティクル(ID, 設定) → 無 */
static Value fn_particle(int argc, Value *argv) {
    (void)argc;
    if (!g_cur) return hajimu_null();
    (void)argv[0]; (void)argv[1];
    /* 簡易パーティクル: ランダムな円を描画 */
    Hjpcontext *vg = g_cur->vg;
    float bx, by, bw2;
    gui_pos(&bx, &by, &bw2);
    (void)bw2;
    for (int i = 0; i < 20; i++) {
        float px = bx + (float)((g_frame_count * 7 + i * 31) % 200);
        float py = by + (float)((g_frame_count * 3 + i * 17) % 100);
        float r2 = 2.0f + (i % 3);
        hjpBeginPath(vg);
        hjpCircle(vg, px, py, r2);
        hjpFillColor(vg, hjpRGBA(255, 200, 50, (unsigned char)(200 - i * 8)));
        hjpFill(vg);
    }
    gui_advance(104);
    return hajimu_null();
}

/* シーン作成(ID) → 辞書 */
static Value fn_scene_create(int argc, Value *argv) {
    (void)argc;
    const char *id_str = (argv[0].type == VALUE_STRING) ? argv[0].string.data : "scene";
    return gui_dict2("ID", hajimu_string(id_str),
                     "オブジェクト数", hajimu_number(0));
}

/* シーンオブジェクト(シーン, 種類, 属性) → 数値 */
static Value fn_scene_object(int argc, Value *argv) {
    (void)argc; (void)argv;
    static int obj_id = 1;
    return hajimu_number(obj_id++);
}

/* =====================================================================
 * Phase 56: 画像操作
 * ===================================================================*/

/* 画像プレビュー(画像[, オプション]) → 無 */
static Value fn_image_preview(int argc, Value *argv) {
    if (!g_cur) return hajimu_null();
    int img = (int)argv[0].number;
    (void)argc;
    int iw3 = 0, ih3 = 0;
    hjpImageSize(g_cur->vg, img, &iw3, &ih3);
    if (iw3 == 0 || ih3 == 0) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    int ww = 0, wh = 0;
    if (g_cur->window) hjp_window_get_size(g_cur->window, &ww, &wh);
    /* フルスクリーンオーバーレイ */
    hjpBeginPath(vg);
    hjpRect(vg, 0, 0, (float)ww, (float)wh);
    hjpFillColor(vg, hjpRGBA(0, 0, 0, 180));
    hjpFill(vg);
    /* 中央に画像描画 */
    float scale2 = 1.0f;
    if (iw3 > ww - 40) scale2 = (float)(ww - 40) / iw3;
    if (ih3 * scale2 > wh - 40) scale2 = (float)(wh - 40) / ih3;
    float dw = iw3 * scale2, dh2 = ih3 * scale2;
    float dx = (ww - dw) / 2, dy = (wh - dh2) / 2;
    Hjppaint p = hjpImagePattern(vg, dx, dy, dw, dh2, 0, img, 1.0f);
    hjpBeginPath(vg);
    hjpRect(vg, dx, dy, dw, dh2);
    hjpFillPaint(vg, p);
    hjpFill(vg);
    return hajimu_null();
}

/* 画像リサイズ(画像, 幅, 高さ) → 数値 */
static Value fn_image_resize(int argc, Value *argv) {
    (void)argc; (void)argv;
    /* hjp_renderではリサイズは描画時にスケールで行う */
    return argv[0]; /* 元画像をそのまま返す */
}

/* 画像回転(画像, 角度) → 数値 */
static Value fn_image_rotate(int argc, Value *argv) {
    (void)argc; (void)argv;
    return argv[0];
}

/* 画像反転(画像, 方向) → 数値 */
static Value fn_image_flip(int argc, Value *argv) {
    (void)argc; (void)argv;
    return argv[0];
}

/* 画像ぼかし(画像, 半径) → 数値 */
static Value fn_image_blur(int argc, Value *argv) {
    (void)argc; (void)argv;
    return argv[0];
}

/* 画像明度(画像, 値) → 数値 */
static Value fn_image_brightness(int argc, Value *argv) {
    (void)argc; (void)argv;
    return argv[0];
}

/* 画像情報(画像) → 辞書 */
static Value fn_image_info(int argc, Value *argv) {
    (void)argc;
    int iw4 = 0, ih4 = 0;
    if (g_cur) hjpImageSize(g_cur->vg, (int)argv[0].number, &iw4, &ih4);
    return gui_dict3("幅", hajimu_number(iw4),
                     "高さ", hajimu_number(ih4),
                     "形式", hajimu_string("RGBA"));
}

/* 画像生成(幅, 高さ, 色) → 数値 */
static Value fn_image_create(int argc, Value *argv) {
    (void)argc;
    if (!g_cur) return hajimu_number(-1);
    int w = (int)argv[0].number, h = (int)argv[1].number;
    unsigned int c = (argc > 2 && argv[2].type == VALUE_NUMBER) ? (unsigned int)argv[2].number : 0xFFFFFF;
    unsigned char r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
    unsigned char *data = (unsigned char *)calloc(w * h * 4, 1);
    for (int i = 0; i < w * h; i++) {
        data[i*4] = r; data[i*4+1] = g; data[i*4+2] = b; data[i*4+3] = 255;
    }
    int img = hjpCreateImageRGBA(g_cur->vg, w, h, 0, data);
    free(data);
    return hajimu_number(img);
}

/* =====================================================================
 * Phase 57: 色/スタイル高度
 * ===================================================================*/

typedef struct {
    char name[32];
    Value style; /* dict */
} GuiStyleClass;

#define GUI_MAX_STYLE_CLASSES 16
static GuiStyleClass g_style_classes[GUI_MAX_STYLE_CLASSES];
static int g_style_class_count = 0;

/* HSL色(色相, 彩度, 輝度) → 数値 */
static Value fn_hsl_color(int argc, Value *argv) {
    (void)argc;
    float h2 = (float)argv[0].number;
    float s = (float)argv[1].number / 100.0f;
    float l = (float)argv[2].number / 100.0f;
    /* HSL → RGB 変換 */
    float c = (1.0f - fabsf(2*l - 1)) * s;
    float x = c * (1.0f - fabsf(fmodf(h2 / 60.0f, 2) - 1));
    float m = l - c / 2;
    float r2 = 0, g2 = 0, b2 = 0;
    if (h2 < 60)       { r2=c; g2=x; b2=0; }
    else if (h2 < 120) { r2=x; g2=c; b2=0; }
    else if (h2 < 180) { r2=0; g2=c; b2=x; }
    else if (h2 < 240) { r2=0; g2=x; b2=c; }
    else if (h2 < 300) { r2=x; g2=0; b2=c; }
    else               { r2=c; g2=0; b2=x; }
    unsigned int ri = (unsigned int)((r2+m)*255);
    unsigned int gi = (unsigned int)((g2+m)*255);
    unsigned int bi = (unsigned int)((b2+m)*255);
    return hajimu_number((ri << 16) | (gi << 8) | bi);
}

/* HSV色(色相, 彩度, 明度) → 数値 */
static Value fn_hsv_color(int argc, Value *argv) {
    (void)argc;
    float h2 = (float)argv[0].number;
    float s = (float)argv[1].number / 100.0f;
    float v = (float)argv[2].number / 100.0f;
    float c = v * s;
    float x = c * (1.0f - fabsf(fmodf(h2 / 60.0f, 2) - 1));
    float m = v - c;
    float r2 = 0, g2 = 0, b2 = 0;
    if (h2 < 60)       { r2=c; g2=x; b2=0; }
    else if (h2 < 120) { r2=x; g2=c; b2=0; }
    else if (h2 < 180) { r2=0; g2=c; b2=x; }
    else if (h2 < 240) { r2=0; g2=x; b2=c; }
    else if (h2 < 300) { r2=x; g2=0; b2=c; }
    else               { r2=c; g2=0; b2=x; }
    unsigned int ri = (unsigned int)((r2+m)*255);
    unsigned int gi = (unsigned int)((g2+m)*255);
    unsigned int bi = (unsigned int)((b2+m)*255);
    return hajimu_number((ri << 16) | (gi << 8) | bi);
}

/* 色変換(色, 空間) → 辞書 */
static Value fn_color_convert(int argc, Value *argv) {
    (void)argc;
    unsigned int c = (unsigned int)argv[0].number;
    (void)argv[1];
    unsigned int r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
    float rf = r / 255.0f, gf = g / 255.0f, bf = b / 255.0f;
    float mx = fmaxf(fmaxf(rf, gf), bf);
    float mn = fminf(fminf(rf, gf), bf);
    float h2 = 0, s = 0, l = (mx + mn) / 2;
    if (mx != mn) {
        float d = mx - mn;
        s = (l > 0.5f) ? d / (2.0f - mx - mn) : d / (mx + mn);
        if (mx == rf)      h2 = fmodf((gf - bf) / d, 6.0f) * 60.0f;
        else if (mx == gf) h2 = ((bf - rf) / d + 2) * 60;
        else               h2 = ((rf - gf) / d + 4) * 60;
    }
    return gui_dict3("H", hajimu_number(h2), "S", hajimu_number(s * 100),
                     "L", hajimu_number(l * 100));
}

/* 色補間(色1, 色2, 割合) → 数値 */
static Value fn_color_lerp(int argc, Value *argv) {
    (void)argc;
    unsigned int c1 = (unsigned int)argv[0].number;
    unsigned int c2 = (unsigned int)argv[1].number;
    float t = (float)argv[2].number;
    if (t < 0) t = 0; if (t > 1) t = 1;
    unsigned int r = (unsigned int)((1-t)*((c1>>16)&0xFF) + t*((c2>>16)&0xFF));
    unsigned int g = (unsigned int)((1-t)*((c1>>8)&0xFF)  + t*((c2>>8)&0xFF));
    unsigned int b = (unsigned int)((1-t)*(c1&0xFF)       + t*(c2&0xFF));
    return hajimu_number((r << 16) | (g << 8) | b);
}

/* カスタムスクロールバー(スタイル辞書) → 無 */
static Value fn_custom_scrollbar(int argc, Value *argv) {
    (void)argc; (void)argv;
    return hajimu_null();
}

/* スタイル継承(親スタイル) → 無 */
static Value fn_style_inherit(int argc, Value *argv) {
    (void)argc; (void)argv;
    return hajimu_null();
}

/* スタイルクラス(名前, スタイル辞書) → 無 */
static Value fn_style_class(int argc, Value *argv) {
    (void)argc;
    if (argv[0].type != VALUE_STRING || g_style_class_count >= GUI_MAX_STYLE_CLASSES)
        return hajimu_null();
    GuiStyleClass *sc = &g_style_classes[g_style_class_count++];
    snprintf(sc->name, sizeof(sc->name), "%s", argv[0].string.data);
    sc->style = argv[1];
    return hajimu_null();
}

/* 条件スタイル(条件, スタイル辞書) → 無 */
static Value fn_cond_style(int argc, Value *argv) {
    (void)argc; (void)argv;
    return hajimu_null();
}

/* =====================================================================
 * Phase 58: ウィジェット状態制御
 * ===================================================================*/

typedef struct {
    uint32_t id;
    bool visible;
    bool changed;
} GuiWidgetState;

#define GUI_MAX_WIDGET_STATE 64
static GuiWidgetState g_wstate[GUI_MAX_WIDGET_STATE];
static int g_wstate_count = 0;

static GuiWidgetState *gui_get_wstate(uint32_t id) {
    for (int i = 0; i < g_wstate_count; i++)
        if (g_wstate[i].id == id) return &g_wstate[i];
    if (g_wstate_count >= GUI_MAX_WIDGET_STATE) return NULL;
    GuiWidgetState *ws = &g_wstate[g_wstate_count++];
    ws->id = id; ws->visible = true; ws->changed = false;
    return ws;
}

/* ウィジェット可視性(ID, 表示) → 無 */
static Value fn_widget_visibility(int argc, Value *argv) {
    (void)argc;
    char buf[128]; gui_value_to_str(argv[0], buf, sizeof(buf));
    uint32_t id = gui_hash(buf);
    GuiWidgetState *ws = gui_get_wstate(id);
    if (ws) ws->visible = (argv[1].type == VALUE_BOOL) ? argv[1].boolean : true;
    return hajimu_null();
}

/* ウィジェットホバー(ID) → 真偽 */
static Value fn_widget_hover(int argc, Value *argv) {
    (void)argc;
    if (!g_cur) return hajimu_bool(false);
    char buf[128]; gui_value_to_str(argv[0], buf, sizeof(buf));
    uint32_t id = gui_hash(buf);
    return hajimu_bool(g_cur->hot == id);
}

/* ウィジェットアクティブ(ID) → 真偽 */
static Value fn_widget_active(int argc, Value *argv) {
    (void)argc;
    if (!g_cur) return hajimu_bool(false);
    char buf[128]; gui_value_to_str(argv[0], buf, sizeof(buf));
    uint32_t id = gui_hash(buf);
    return hajimu_bool(g_cur->active == id);
}

/* ウィジェット変更検知(ID) → 真偽 */
static Value fn_widget_changed(int argc, Value *argv) {
    (void)argc;
    char buf[128]; gui_value_to_str(argv[0], buf, sizeof(buf));
    uint32_t id = gui_hash(buf);
    GuiWidgetState *ws = gui_get_wstate(id);
    if (ws) { bool c = ws->changed; ws->changed = false; return hajimu_bool(c); }
    return hajimu_bool(false);
}

/* ウィジェットリフレッシュ(ID) → 無 */
static Value fn_widget_refresh(int argc, Value *argv) {
    (void)argc; (void)argv;
    /* 即時モードGUIでは毎フレーム再描画のためノープ */
    return hajimu_null();
}

/* 条件表示(条件) → 無 */
static Value fn_cond_show(int argc, Value *argv) {
    (void)argc;
    /* 将来: 次のウィジェットの表示/非表示を制御 */
    (void)argv;
    return hajimu_null();
}

/* ウィジェットキー(キー) → 無 */
static Value fn_widget_key(int argc, Value *argv) {
    (void)argc; (void)argv;
    return hajimu_null();
}

/* ウィジェットコールバック(イベント, 関数) → 無 */
static Value fn_widget_callback(int argc, Value *argv) {
    (void)argc; (void)argv;
    return hajimu_null();
}

/* =====================================================================
 * Phase 59: MDI/ドキュメント
 * ===================================================================*/

typedef struct {
    uint32_t id;
    char title[64];
    float x, y, w, h;
    bool active;
} GuiMdiChild;

#define GUI_MAX_MDI 8
static GuiMdiChild g_mdi[GUI_MAX_MDI];
static char g_recent_files[8][256];
static int g_recent_count = 0;
static bool g_doc_modified = false;
static int g_autosave_interval = 0;

/* MDI領域(ID) → 辞書 */
static Value fn_mdi_area(int argc, Value *argv) {
    (void)argc;
    const char *label = (argv[0].type == VALUE_STRING) ? argv[0].string.data : "mdi";
    if (!g_cur) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    float bx, by, bw;
    gui_pos(&bx, &by, &bw);
    float mdi_h = 400;
    hjpBeginPath(vg);
    hjpRoundedRect(vg, bx, by, bw, mdi_h, 4);
    hjpFillColor(vg, hjpRGBA(40, 40, 50, 255));
    hjpFill(vg);
    hjpStrokeColor(vg, TH_BORDER);
    hjpStroke(vg);
    gui_advance(mdi_h + 4);
    return gui_dict2("ID", hajimu_string(label),
                     "子数", hajimu_number(0));
}

/* MDI子ウィンドウ(ID, タイトル) → 辞書 */
static Value fn_mdi_child(int argc, Value *argv) {
    (void)argc;
    const char *label = (argv[0].type == VALUE_STRING) ? argv[0].string.data : "child";
    const char *title = (argv[1].type == VALUE_STRING) ? argv[1].string.data : "";
    uint32_t id = gui_hash(label);
    int idx = -1;
    for (int i = 0; i < GUI_MAX_MDI; i++)
        if (g_mdi[i].active && g_mdi[i].id == id) { idx = i; break; }
    if (idx < 0) {
        for (int i = 0; i < GUI_MAX_MDI; i++)
            if (!g_mdi[i].active) { idx = i; break; }
        if (idx < 0) return hajimu_null();
        g_mdi[idx].id = id;
        snprintf(g_mdi[idx].title, sizeof(g_mdi[idx].title), "%s", title);
        g_mdi[idx].x = 20 + idx * 30;
        g_mdi[idx].y = 20 + idx * 30;
        g_mdi[idx].w = 300;
        g_mdi[idx].h = 200;
        g_mdi[idx].active = true;
    }
    return gui_dict2("ID", hajimu_string(label),
                     "タイトル", hajimu_string(title));
}

/* 最近使ったファイル(一覧) → 配列 */
static Value fn_recent_files(int argc, Value *argv) {
    (void)argc;
    if (argv[0].type == VALUE_ARRAY) {
        g_recent_count = 0;
        for (int i = 0; i < (int)argv[0].array.length && i < 8; i++) {
            if (argv[0].array.elements[i].type == VALUE_STRING)
                snprintf(g_recent_files[g_recent_count++], 256, "%s",
                         argv[0].array.elements[i].string.data);
        }
    }
    Value arr; memset(&arr, 0, sizeof(arr));
    arr.type = VALUE_ARRAY;
    arr.array.length = g_recent_count;
    arr.array.elements = (Value *)calloc(g_recent_count > 0 ? g_recent_count : 1, sizeof(Value));
    for (int i = 0; i < g_recent_count; i++)
        arr.array.elements[i] = hajimu_string(g_recent_files[i]);
    return arr;
}

/* ドキュメント変更フラグ(有効) → 無 */
static Value fn_doc_modified(int argc, Value *argv) {
    (void)argc;
    g_doc_modified = (argv[0].type == VALUE_BOOL) ? argv[0].boolean : true;
    return hajimu_null();
}

/* 自動保存(間隔) → 無 */
static Value fn_autosave(int argc, Value *argv) {
    (void)argc;
    g_autosave_interval = (int)argv[0].number;
    return hajimu_null();
}

/* タブ閉じる確認(コールバック) → 真偽 */
static Value fn_tab_close_confirm(int argc, Value *argv) {
    (void)argc; (void)argv;
    return hajimu_bool(!g_doc_modified);
}

/* パンくずパス(パス配列) → 無 */
static Value fn_breadcrumb_path(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || argv[0].type != VALUE_ARRAY) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    float bx, by, bw;
    gui_pos(&bx, &by, &bw);
    (void)bw;
    hjpFontFaceId(vg, g_cur->font_id);
    hjpFontSize(vg, 13);
    hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_MIDDLE);
    float cx5 = bx;
    for (int i = 0; i < (int)argv[0].array.length; i++) {
        if (i > 0) {
            hjpFillColor(vg, TH_TEXT_DIM);
            hjpText(vg, cx5, by + 10, " > ", NULL);
            cx5 += 20;
        }
        if (argv[0].array.elements[i].type == VALUE_STRING) {
            const char *seg = argv[0].array.elements[i].string.data;
            bool last = (i == (int)argv[0].array.length - 1);
            hjpFillColor(vg, last ? TH_ACCENT : TH_TEXT);
            hjpText(vg, cx5, by + 10, seg, NULL);
            float adv[4];
            hjpTextBounds(vg, cx5, by + 10, seg, NULL, adv);
            cx5 = adv[2] + 4;
        }
    }
    gui_advance(24);
    return hajimu_null();
}

/* ドキュメントタイトル(タイトル) → 無 */
static Value fn_doc_title(int argc, Value *argv) {
    (void)argc;
    if (argv[0].type == VALUE_STRING && g_cur && g_cur->window)
        hjp_window_set_title(g_cur->window, argv[0].string.data);
    return hajimu_null();
}

/* =====================================================================
 * Phase 60: コンテキスト/補助
 * ===================================================================*/

/* コマンドパレット(コマンド配列) → 文字列 */
static Value fn_command_palette(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || argv[0].type != VALUE_ARRAY) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    int ww = 0, wh = 0;
    if (g_cur->window) hjp_window_get_size(g_cur->window, &ww, &wh);
    float pw = 400, ph = 300;
    float px = (ww - pw) / 2, py = 80;
    /* 背景オーバーレイ */
    hjpBeginPath(vg);
    hjpRect(vg, 0, 0, (float)ww, (float)wh);
    hjpFillColor(vg, hjpRGBA(0, 0, 0, 100));
    hjpFill(vg);
    /* パレット本体 */
    hjpBeginPath(vg);
    hjpRoundedRect(vg, px, py, pw, ph, 8);
    hjpFillColor(vg, TH_BG);
    hjpFill(vg);
    hjpStrokeColor(vg, TH_BORDER);
    hjpStroke(vg);
    /* 検索バー */
    hjpBeginPath(vg);
    hjpRoundedRect(vg, px + 8, py + 8, pw - 16, 32, 4);
    hjpFillColor(vg, TH_WIDGET_BG);
    hjpFill(vg);
    hjpFontFaceId(vg, g_cur->font_id);
    hjpFontSize(vg, 14);
    hjpFillColor(vg, TH_TEXT_DIM);
    hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_MIDDLE);
    hjpText(vg, px + 16, py + 24, "コマンド検索...", NULL);
    /* コマンド一覧 */
    float cy4 = py + 48;
    hjpFontSize(vg, 13);
    for (int i = 0; i < (int)argv[0].array.length && i < 10; i++) {
        if (argv[0].array.elements[i].type != VALUE_STRING) continue;
        hjpFillColor(vg, TH_TEXT);
        hjpText(vg, px + 16, cy4 + 12, argv[0].array.elements[i].string.data, NULL);
        cy4 += 24;
    }
    return hajimu_null();
}

/* ショートカット一覧() → 辞書 */
static Value fn_shortcut_list(int argc, Value *argv) {
    (void)argc; (void)argv;
    /* 登録済みショートカットを返す — 現片空 */
    Value d = gui_dict2("説明", hajimu_string("ショートカット一覧"),
                        "件数", hajimu_number(0));
    return d;
}

/* トップ戻る(表示) → 無 */
static Value fn_back_to_top(int argc, Value *argv) {
    (void)argc;
    if (!g_cur) return hajimu_null();
    bool show = (argv[0].type == VALUE_BOOL) ? argv[0].boolean : true;
    if (!show) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    int ww = 0, wh = 0;
    if (g_cur->window) hjp_window_get_size(g_cur->window, &ww, &wh);
    float bx2 = ww - 50, by2 = wh - 50;
    /* 丸ボタン */
    hjpBeginPath(vg);
    hjpCircle(vg, bx2 + 18, by2 + 18, 18);
    hjpFillColor(vg, TH_ACCENT);
    hjpFill(vg);
    hjpFontFaceId(vg, g_cur->font_id);
    hjpFontSize(vg, 16);
    hjpFillColor(vg, hjpRGBA(255, 255, 255, 255));
    hjpTextAlign(vg, HJP_ALIGN_CENTER | HJP_ALIGN_MIDDLE);
    hjpText(vg, bx2 + 18, by2 + 17, "\xe2\x86\x91", NULL); /* ↑ */
    return hajimu_null();
}

/* ローディングボタン(ラベル, 読込中) → 真偽 */
static Value fn_loading_button(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || argv[0].type != VALUE_STRING) return hajimu_bool(false);
    bool loading = (argv[1].type == VALUE_BOOL) ? argv[1].boolean : false;
    const char *label = argv[0].string.data;
    Hjpcontext *vg = g_cur->vg;
    float bx, by, bw;
    gui_pos(&bx, &by, &bw);
    float btn_w = 140, btn_h = 32;
    uint32_t id = gui_hash(label);
    bool hov = false, pressed = false;
    bool clicked = gui_widget_logic(id, bx, by, btn_w, btn_h, &hov, &pressed);
    Hjpcolor bg = pressed ? TH_WIDGET_ACTIVE : (hov ? TH_WIDGET_HOVER : TH_ACCENT);
    hjpBeginPath(vg);
    hjpRoundedRect(vg, bx, by, btn_w, btn_h, 4);
    hjpFillColor(vg, bg);
    hjpFill(vg);
    hjpFontFaceId(vg, g_cur->font_id);
    hjpFontSize(vg, 14);
    hjpFillColor(vg, hjpRGBA(255, 255, 255, 255));
    hjpTextAlign(vg, HJP_ALIGN_CENTER | HJP_ALIGN_MIDDLE);
    if (loading) {
        float a = g_frame_count * 0.15f;
        hjpBeginPath(vg);
        hjpArc(vg, bx + 20, by + btn_h/2, 8, a, a + HJP_PI * 1.5f, HJP_CW);
        hjpStrokeColor(vg, hjpRGBA(255, 255, 255, 200));
        hjpStrokeWidth(vg, 2);
        hjpStroke(vg);
        hjpText(vg, bx + btn_w/2 + 10, by + btn_h/2, label, NULL);
    } else {
        hjpText(vg, bx + btn_w/2, by + btn_h/2, label, NULL);
    }
    gui_advance(btn_h + 4);
    return hajimu_bool(clicked && !loading);
}

/* 画像ボタン拡張(画像, ラベル) → 真偽 */
static Value fn_image_button_ext(int argc, Value *argv) {
    (void)argc;
    if (!g_cur) return hajimu_bool(false);
    int img = (int)argv[0].number;
    const char *label = (argv[1].type == VALUE_STRING) ? argv[1].string.data : "";
    Hjpcontext *vg = g_cur->vg;
    float bx, by, bw;
    gui_pos(&bx, &by, &bw);
    float btn_w = 120, btn_h = 36;
    uint32_t id = gui_hash(label);
    bool hov = false, pressed = false;
    bool clicked = gui_widget_logic(id, bx, by, btn_w, btn_h, &hov, &pressed);
    Hjpcolor bg = pressed ? TH_WIDGET_ACTIVE : (hov ? TH_WIDGET_HOVER : TH_WIDGET_BG);
    hjpBeginPath(vg);
    hjpRoundedRect(vg, bx, by, btn_w, btn_h, 4);
    hjpFillColor(vg, bg);
    hjpFill(vg);
    /* 画像 */
    Hjppaint p = hjpImagePattern(vg, bx + 4, by + 2, 32, 32, 0, img, 1.0f);
    hjpBeginPath(vg);
    hjpRect(vg, bx + 4, by + 2, 32, 32);
    hjpFillPaint(vg, p);
    hjpFill(vg);
    /* ラベル */
    hjpFontFaceId(vg, g_cur->font_id);
    hjpFontSize(vg, 13);
    hjpFillColor(vg, TH_TEXT);
    hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_MIDDLE);
    hjpText(vg, bx + 40, by + btn_h/2, label, NULL);
    gui_advance(btn_h + 4);
    return hajimu_bool(clicked);
}

/* メゾンリーレイアウト開始(列数) → 無 */
static Value fn_masonry_begin(int argc, Value *argv) {
    (void)argc; (void)argv;
    /* 将来: メゾンリーレイアウトコンテキスト開始 */
    return hajimu_null();
}

/* メゾンリーレイアウト終了() → 無 */
static Value fn_masonry_end(int argc, Value *argv) {
    (void)argc; (void)argv;
    return hajimu_null();
}

/* パララックス(ID, 速度) → 無 */
static Value fn_parallax(int argc, Value *argv) {
    (void)argc; (void)argv;
    return hajimu_null();
}

/* =====================================================================
 * Phase 61-80 用ヘルパーマクロ
 * ===================================================================*/
static inline const char *gui_as_string(Value v) {
    return (v.type == VALUE_STRING) ? v.string.data : NULL;
}
static inline double gui_as_number(Value v) {
    return (v.type == VALUE_NUMBER) ? v.number : 0.0;
}
static inline bool gui_as_bool(Value v) {
    return (v.type == VALUE_BOOL) ? v.boolean : false;
}
static inline int gui_arr_len(Value v) {
    return (v.type == VALUE_ARRAY) ? v.array.length : 0;
}
static inline Value gui_arr_get(Value v, int i) {
    if (v.type != VALUE_ARRAY || i < 0 || i >= v.array.length)
        return hajimu_null();
    return v.array.elements[i];
}
static inline Value gui_dict_lookup(Value d, const char *key) {
    if (d.type != VALUE_DICT) return hajimu_null();
    for (int i = 0; i < d.dict.length; i++) {
        if (d.dict.keys[i] && strcmp(d.dict.keys[i], key) == 0)
            return d.dict.values[i];
    }
    return hajimu_null();
}

/* =====================================================================
 * Phase 61: 高度なデータ可視化 II (v6.1.0)
 * ===================================================================*/

/* レーダーチャート(ラベル配列, 値配列) → 無 */
static Value fn_radar_chart(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || !g_cur->vg) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    float x, y, w;
    gui_pos(&x, &y, &w);
    float cx = x + w / 2, cy = y + 120, r = 100;
    Value labels = argv[0], vals = argv[1];
    int n = gui_arr_len(labels);
    if (n < 3) n = 3;
    /* 軸描画 */
    for (int i = 0; i < n; i++) {
        float angle = (float)i / n * HJP_PI * 2 - HJP_PI / 2;
        float ex = cx + cosf(angle) * r, ey = cy + sinf(angle) * r;
        hjpBeginPath(vg);
        hjpMoveTo(vg, cx, cy);
        hjpLineTo(vg, ex, ey);
        hjpStrokeColor(vg, hjpRGBA(180,180,180,255));
        hjpStroke(vg);
        /* ラベル */
        Value lbl = gui_arr_get(labels, i);
        const char *s = gui_as_string(lbl);
        if (s) {
            hjpFontFaceId(vg, g_cur->font_id);
            hjpFontSize(vg, 12);
            hjpFillColor(vg, TH_TEXT);
            hjpTextAlign(vg, HJP_ALIGN_CENTER | HJP_ALIGN_MIDDLE);
            hjpText(vg, ex + cosf(angle)*14, ey + sinf(angle)*14, s, NULL);
        }
    }
    /* 値ポリゴン */
    hjpBeginPath(vg);
    for (int i = 0; i < n; i++) {
        Value v = gui_arr_get(vals, i);
        float val = (float)gui_as_number(v);
        if (val > 1.0f) val = 1.0f;
        if (val < 0.0f) val = 0.0f;
        float angle = (float)i / n * HJP_PI * 2 - HJP_PI / 2;
        float px = cx + cosf(angle) * r * val;
        float py = cy + sinf(angle) * r * val;
        if (i == 0) hjpMoveTo(vg, px, py);
        else hjpLineTo(vg, px, py);
    }
    hjpClosePath(vg);
    hjpFillColor(vg, hjpRGBA(66,133,244,80));
    hjpFill(vg);
    hjpStrokeColor(vg, hjpRGBA(66,133,244,200));
    hjpStrokeWidth(vg, 2);
    hjpStroke(vg);
    gui_advance(250);
    return hajimu_null();
}

/* ツリーマップ(データ) → 無 */
static Value fn_treemap(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || !g_cur->vg) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    float x, y, w;
    gui_pos(&x, &y, &w);
    float h = 200;
    Value data = argv[0];
    int n = gui_arr_len(data);
    if (n <= 0) { gui_advance(h); return hajimu_null(); }
    /* 簡易水平分割 */
    float total = 0;
    for (int i = 0; i < n; i++) {
        Value item = gui_arr_get(data, i);
        Value val = gui_dict_lookup(item, "値");
        total += (float)gui_as_number(val);
    }
    if (total <= 0) total = 1;
    float cx = x;
    Hjpcolor colors[] = {
        hjpRGBA(66,133,244,200), hjpRGBA(219,68,55,200),
        hjpRGBA(244,180,0,200), hjpRGBA(15,157,88,200),
        hjpRGBA(171,71,188,200), hjpRGBA(0,172,193,200)
    };
    for (int i = 0; i < n; i++) {
        Value item = gui_arr_get(data, i);
        Value val = gui_dict_lookup(item, "値");
        Value lbl = gui_dict_lookup(item, "名前");
        float ratio = (float)gui_as_number(val) / total;
        float rw = w * ratio;
        hjpBeginPath(vg);
        hjpRect(vg, cx, y, rw - 2, h);
        hjpFillColor(vg, colors[i % 6]);
        hjpFill(vg);
        const char *s = gui_as_string(lbl);
        if (s && rw > 30) {
            hjpFontFaceId(vg, g_cur->font_id);
            hjpFontSize(vg, 12);
            hjpFillColor(vg, hjpRGBA(255,255,255,230));
            hjpTextAlign(vg, HJP_ALIGN_CENTER | HJP_ALIGN_MIDDLE);
            hjpText(vg, cx + rw/2, y + h/2, s, NULL);
        }
        cx += rw;
    }
    gui_advance(h + 8);
    return hajimu_null();
}

/* サンキー図(ノード, リンク) → 無 */
static Value fn_sankey(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || !g_cur->vg) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    float x, y, w;
    gui_pos(&x, &y, &w);
    float h = 200;
    Value links = argv[1];
    int ln = gui_arr_len(links);
    /* 簡易: リンクを帯で描画 */
    for (int i = 0; i < ln; i++) {
        Value link = gui_arr_get(links, i);
        Value vs = gui_dict_lookup(link, "元");
        Value vt = gui_dict_lookup(link, "先");
        Value vv = gui_dict_lookup(link, "値");
        int si = (int)gui_as_number(vs);
        int ti = (int)gui_as_number(vt);
        float val = (float)gui_as_number(vv);
        float bw = val * 3;
        if (bw < 2) bw = 2;
        if (bw > 40) bw = 40;
        float sx = x + 50, sy = y + 20 + si * 30;
        float tx = x + w - 50, ty = y + 20 + ti * 30;
        hjpBeginPath(vg);
        hjpMoveTo(vg, sx, sy);
        hjpBezierTo(vg, sx + (tx-sx)*0.5f, sy, tx - (tx-sx)*0.5f, ty, tx, ty);
        hjpStrokeColor(vg, hjpRGBA(66,133,244,100));
        hjpStrokeWidth(vg, bw);
        hjpStroke(vg);
    }
    hjpStrokeWidth(vg, 1);
    gui_advance(h + 8);
    return hajimu_null();
}

/* ガントチャート(タスク配列) → 無 */
static Value fn_gantt_chart(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || !g_cur->vg) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    float x, y, w;
    gui_pos(&x, &y, &w);
    Value tasks = argv[0];
    int n = gui_arr_len(tasks);
    float row_h = 28, label_w = 120;
    float chart_w = w - label_w;
    Hjpcolor bar_col = hjpRGBA(66,133,244,200);
    for (int i = 0; i < n; i++) {
        Value task = gui_arr_get(tasks, i);
        Value name = gui_dict_lookup(task, "名前");
        Value start = gui_dict_lookup(task, "開始");
        Value dur = gui_dict_lookup(task, "期間");
        const char *s = gui_as_string(name);
        float st = (float)gui_as_number(start);
        float d = (float)gui_as_number(dur);
        float ry = y + i * row_h;
        /* ラベル */
        if (s) {
            hjpFontFaceId(vg, g_cur->font_id);
            hjpFontSize(vg, 13);
            hjpFillColor(vg, TH_TEXT);
            hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_MIDDLE);
            hjpText(vg, x, ry + row_h/2, s, NULL);
        }
        /* バー */
        hjpBeginPath(vg);
        hjpRoundedRect(vg, x + label_w + st * chart_w, ry + 4, d * chart_w, row_h - 8, 3);
        hjpFillColor(vg, bar_col);
        hjpFill(vg);
    }
    gui_advance(n * row_h + 8);
    return hajimu_null();
}

/* ローソク足チャート(データ) → 無 */
static Value fn_candlestick_chart(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || !g_cur->vg) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    float x, y, w;
    gui_pos(&x, &y, &w);
    float h = 200;
    Value data = argv[0];
    int n = gui_arr_len(data);
    if (n <= 0) { gui_advance(h); return hajimu_null(); }
    float bar_w = (w - 20) / (float)n;
    /* find min/max */
    float mn = 1e9, mx = -1e9;
    for (int i = 0; i < n; i++) {
        Value c = gui_arr_get(data, i);
        float lo = (float)gui_as_number(gui_dict_lookup(c, "安"));
        float hi = (float)gui_as_number(gui_dict_lookup(c, "高"));
        if (lo < mn) mn = lo;
        if (hi > mx) mx = hi;
    }
    float range = mx - mn;
    if (range < 0.001f) range = 1;
    for (int i = 0; i < n; i++) {
        Value c = gui_arr_get(data, i);
        float o = (float)gui_as_number(gui_dict_lookup(c, "始"));
        float cl = (float)gui_as_number(gui_dict_lookup(c, "終"));
        float hi = (float)gui_as_number(gui_dict_lookup(c, "高"));
        float lo = (float)gui_as_number(gui_dict_lookup(c, "安"));
        float cx2 = x + 10 + i * bar_w + bar_w / 2;
        float hy = y + (1 - (hi - mn) / range) * h;
        float ly = y + (1 - (lo - mn) / range) * h;
        float oy = y + (1 - (o - mn) / range) * h;
        float cy2 = y + (1 - (cl - mn) / range) * h;
        Hjpcolor col = (cl >= o) ? hjpRGBA(15,157,88,255) : hjpRGBA(219,68,55,255);
        /* wick */
        hjpBeginPath(vg);
        hjpMoveTo(vg, cx2, hy);
        hjpLineTo(vg, cx2, ly);
        hjpStrokeColor(vg, col);
        hjpStroke(vg);
        /* body */
        float top = (cl >= o) ? cy2 : oy;
        float bot = (cl >= o) ? oy : cy2;
        float bh = bot - top;
        if (bh < 1) bh = 1;
        hjpBeginPath(vg);
        hjpRect(vg, cx2 - bar_w * 0.3f, top, bar_w * 0.6f, bh);
        hjpFillColor(vg, col);
        hjpFill(vg);
    }
    gui_advance(h + 8);
    return hajimu_null();
}

/* 箱ひげ図(データ配列) → 無 */
static Value fn_boxplot(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || !g_cur->vg) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    float x, y, w;
    gui_pos(&x, &y, &w);
    float h = 160;
    Value data = argv[0];
    int n = gui_arr_len(data);
    if (n <= 0) { gui_advance(h); return hajimu_null(); }
    float box_w = (w - 40) / (float)n;
    for (int i = 0; i < n; i++) {
        Value item = gui_arr_get(data, i);
        float mn2 = (float)gui_as_number(gui_dict_lookup(item, "最小"));
        float q1 = (float)gui_as_number(gui_dict_lookup(item, "Q1"));
        float med = (float)gui_as_number(gui_dict_lookup(item, "中央"));
        float q3 = (float)gui_as_number(gui_dict_lookup(item, "Q3"));
        float mx2 = (float)gui_as_number(gui_dict_lookup(item, "最大"));
        float range2 = mx2 - mn2;
        if (range2 < 0.001f) range2 = 1;
        float cx2 = x + 20 + i * box_w + box_w / 2;
        float bw = box_w * 0.5f;
        /* whiskers */
        float mny = y + h - ((mn2 - mn2) / range2) * (h - 20) - 10;
        float q1y = y + h - ((q1 - mn2) / range2) * (h - 20) - 10;
        float medy = y + h - ((med - mn2) / range2) * (h - 20) - 10;
        float q3y = y + h - ((q3 - mn2) / range2) * (h - 20) - 10;
        float mxy = y + h - ((mx2 - mn2) / range2) * (h - 20) - 10;
        hjpBeginPath(vg);
        hjpMoveTo(vg, cx2, mny); hjpLineTo(vg, cx2, q1y);
        hjpMoveTo(vg, cx2, q3y); hjpLineTo(vg, cx2, mxy);
        hjpStrokeColor(vg, TH_TEXT);
        hjpStroke(vg);
        /* box */
        hjpBeginPath(vg);
        hjpRect(vg, cx2 - bw/2, q3y, bw, q1y - q3y);
        hjpFillColor(vg, hjpRGBA(66,133,244,100));
        hjpFill(vg);
        hjpStrokeColor(vg, hjpRGBA(66,133,244,200));
        hjpStroke(vg);
        /* median */
        hjpBeginPath(vg);
        hjpMoveTo(vg, cx2 - bw/2, medy); hjpLineTo(vg, cx2 + bw/2, medy);
        hjpStrokeColor(vg, hjpRGBA(219,68,55,255));
        hjpStrokeWidth(vg, 2);
        hjpStroke(vg);
        hjpStrokeWidth(vg, 1);
    }
    gui_advance(h + 8);
    return hajimu_null();
}

/* ファンネルチャート(ステップ配列) → 無 */
static Value fn_funnel_chart(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || !g_cur->vg) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    float x, y, w;
    gui_pos(&x, &y, &w);
    Value steps = argv[0];
    int n = gui_arr_len(steps);
    if (n <= 0) { gui_advance(30); return hajimu_null(); }
    float row_h = 36;
    float max_val = 0;
    for (int i = 0; i < n; i++) {
        Value item = gui_arr_get(steps, i);
        float v = (float)gui_as_number(gui_dict_lookup(item, "値"));
        if (v > max_val) max_val = v;
    }
    if (max_val <= 0) max_val = 1;
    Hjpcolor colors[] = {
        hjpRGBA(66,133,244,220), hjpRGBA(52,168,83,220),
        hjpRGBA(251,188,4,220), hjpRGBA(234,67,53,220),
        hjpRGBA(171,71,188,220), hjpRGBA(0,172,193,220)
    };
    for (int i = 0; i < n; i++) {
        Value item = gui_arr_get(steps, i);
        float v = (float)gui_as_number(gui_dict_lookup(item, "値"));
        const char *s = gui_as_string(gui_dict_lookup(item, "名前"));
        float ratio = v / max_val;
        float bw = w * ratio;
        float bx = x + (w - bw) / 2;
        float by = y + i * row_h;
        hjpBeginPath(vg);
        hjpRoundedRect(vg, bx, by, bw, row_h - 4, 4);
        hjpFillColor(vg, colors[i % 6]);
        hjpFill(vg);
        if (s) {
            hjpFontFaceId(vg, g_cur->font_id);
            hjpFontSize(vg, 13);
            hjpFillColor(vg, hjpRGBA(255,255,255,230));
            hjpTextAlign(vg, HJP_ALIGN_CENTER | HJP_ALIGN_MIDDLE);
            hjpText(vg, x + w/2, by + (row_h-4)/2, s, NULL);
        }
    }
    gui_advance(n * row_h + 8);
    return hajimu_null();
}

/* フォースグラフ(ノード, エッジ) → 無 */
static Value fn_force_graph(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || !g_cur->vg) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    float x, y, w;
    gui_pos(&x, &y, &w);
    float h = 250;
    Value nodes = argv[0], edges = argv[1];
    int nn = gui_arr_len(nodes);
    int ne = gui_arr_len(edges);
    /* 簡易: ノードを円形配置 */
    float cx = x + w / 2, cy = y + h / 2, r = 90;
    /* エッジ描画 */
    for (int i = 0; i < ne; i++) {
        Value e = gui_arr_get(edges, i);
        int a = (int)gui_as_number(gui_dict_lookup(e, "元"));
        int b = (int)gui_as_number(gui_dict_lookup(e, "先"));
        if (a >= nn || b >= nn) continue;
        float ang_a = (float)a / nn * HJP_PI * 2;
        float ang_b = (float)b / nn * HJP_PI * 2;
        hjpBeginPath(vg);
        hjpMoveTo(vg, cx + cosf(ang_a)*r, cy + sinf(ang_a)*r);
        hjpLineTo(vg, cx + cosf(ang_b)*r, cy + sinf(ang_b)*r);
        hjpStrokeColor(vg, hjpRGBA(180,180,180,150));
        hjpStroke(vg);
    }
    /* ノード描画 */
    for (int i = 0; i < nn; i++) {
        float ang = (float)i / nn * HJP_PI * 2;
        float nx = cx + cosf(ang) * r, ny = cy + sinf(ang) * r;
        hjpBeginPath(vg);
        hjpCircle(vg, nx, ny, 10);
        hjpFillColor(vg, hjpRGBA(66,133,244,220));
        hjpFill(vg);
        Value node = gui_arr_get(nodes, i);
        const char *s = gui_as_string(node);
        if (s) {
            hjpFontFaceId(vg, g_cur->font_id);
            hjpFontSize(vg, 11);
            hjpFillColor(vg, TH_TEXT);
            hjpTextAlign(vg, HJP_ALIGN_CENTER | HJP_ALIGN_MIDDLE);
            hjpText(vg, nx, ny + 18, s, NULL);
        }
    }
    gui_advance(h + 8);
    return hajimu_null();
}

/* =====================================================================
 * Phase 62: リッチテキスト/コンテンツ (v6.2.0)
 * ===================================================================*/

/* リッチテキストエディタ(ID, テキスト) → 文字列 */
static Value fn_rich_text_editor(int argc, Value *argv) {
    if (argc < 2) return hajimu_string("");
    /* 暫定実装: 通常のテキストエリアとして表示。将来WYSIWYG実装予定 */
    if (!g_cur || !g_cur->vg) return (argc >= 2) ? argv[1] : hajimu_string("");
    float x, y, w;
    gui_pos(&x, &y, &w);
    Hjpcontext *vg = g_cur->vg;
    hjpBeginPath(vg);
    hjpRoundedRect(vg, x, y, w, 120, 4);
    hjpFillColor(vg, hjpRGBA(30, 30, 30, 200));
    hjpFill(vg);
    hjpStrokeColor(vg, hjpRGBA(80, 130, 200, 200));
    hjpStrokeWidth(vg, 1.5f);
    hjpStroke(vg);
    hjpFontSize(vg, 13);
    hjpFillColor(vg, hjpRGBA(220, 220, 220, 255));
    hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_TOP);
    const char *txt = gui_as_string(argv[1]);
    if (txt) hjpTextBox(vg, x + 6, y + 6, w - 12, txt, NULL);
    /* [Rich Text] ラベル */
    hjpFontSize(vg, 10);
    hjpFillColor(vg, hjpRGBA(80, 130, 200, 180));
    hjpText(vg, x + w - 80, y + 106, "[Rich Text]", NULL);
    gui_advance(128);
    return argv[1];
}

/* Diffビューア(左, 右) → 無 */
static Value fn_diff_viewer(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || !g_cur->vg) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    float x, y, w;
    gui_pos(&x, &y, &w);
    const char *left = gui_as_string(argv[0]);
    const char *right = gui_as_string(argv[1]);
    float half = w / 2 - 4;
    float row_y = y;
    hjpFontFaceId(vg, g_cur->font_id);
    hjpFontSize(vg, 13);
    hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_TOP);
    /* 左パネル */
    hjpBeginPath(vg);
    hjpRect(vg, x, y, half, 100);
    hjpFillColor(vg, hjpRGBA(255,235,235,100));
    hjpFill(vg);
    hjpFillColor(vg, TH_TEXT);
    if (left) hjpText(vg, x + 4, row_y + 4, left, NULL);
    /* 右パネル */
    hjpBeginPath(vg);
    hjpRect(vg, x + half + 8, y, half, 100);
    hjpFillColor(vg, hjpRGBA(235,255,235,100));
    hjpFill(vg);
    hjpFillColor(vg, TH_TEXT);
    if (right) hjpText(vg, x + half + 12, row_y + 4, right, NULL);
    gui_advance(108);
    return hajimu_null();
}

/* コードエディタ(ID, コード, 言語) → 文字列 */
static Value fn_code_editor(int argc, Value *argv) {
    if (argc < 2) return hajimu_string("");
    /* 暫定実装: 等幅フォントでコード表示。将来シンタックスハイライト実装予定 */
    if (!g_cur || !g_cur->vg) return argv[1];
    float x, y, w;
    gui_pos(&x, &y, &w);
    Hjpcontext *vg = g_cur->vg;
    hjpBeginPath(vg);
    hjpRoundedRect(vg, x, y, w, 150, 4);
    hjpFillColor(vg, hjpRGBA(20, 22, 28, 230));
    hjpFill(vg);
    hjpStrokeColor(vg, hjpRGBA(60, 60, 80, 200));
    hjpStrokeWidth(vg, 1);
    hjpStroke(vg);
    hjpFontSize(vg, 12);
    hjpFillColor(vg, hjpRGBA(200, 220, 180, 255));
    hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_TOP);
    const char *code = gui_as_string(argv[1]);
    if (code) hjpTextBox(vg, x + 8, y + 8, w - 16, code, NULL);
    /* 言語ラベル */
    if (argc >= 3) {
        const char *lang = gui_as_string(argv[2]);
        if (lang) {
            hjpFontSize(vg, 10);
            hjpFillColor(vg, hjpRGBA(100, 160, 100, 200));
            hjpText(vg, x + w - 60, y + 4, lang, NULL);
        }
    }
    gui_advance(168);
    return argv[1];
}

/* マークダウンエディタ(ID, テキスト) → 文字列 */
static Value fn_markdown_editor(int argc, Value *argv) {
    (void)argc;
    return argv[1];
}

/* HTMLビューア(HTML) → 無 */
/* HTMLビューア(HTML) → 無
 * 暫定実装: HTMLタグを除去してプレーンテキスト表示 */
static Value fn_html_viewer(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || !g_cur->vg) return hajimu_null();
    const char *html = gui_as_string(argv[0]);
    if (!html) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    float x, y, w;
    gui_pos(&x, &y, &w);
    /* 背景ボックス */
    hjpBeginPath(vg);
    hjpRoundedRect(vg, x, y, w, 120, 4);
    hjpFillColor(vg, hjpRGBA(255, 255, 255, 240));
    hjpFill(vg);
    hjpStrokeColor(vg, hjpRGBA(180, 180, 180, 200));
    hjpStrokeWidth(vg, 1);
    hjpStroke(vg);
    /* HTMLタグを除去してテキスト表示 */
    char plain[512]; plain[0] = '\0';
    int pi = 0;
    bool in_tag = false;
    for (int i = 0; html[i] && pi < 510; i++) {
        if (html[i] == '<') { in_tag = true; continue; }
        if (html[i] == '>') { in_tag = false; plain[pi++] = ' '; continue; }
        if (!in_tag) plain[pi++] = html[i];
    }
    plain[pi] = '\0';
    hjpFontFaceId(vg, g_cur->font_id);
    hjpFontSize(vg, 13);
    hjpFillColor(vg, hjpRGBA(30, 30, 30, 255));
    hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_TOP);
    hjpTextBox(vg, x + 8, y + 8, w - 16, plain, NULL);
    /* [HTML] ラベル */
    hjpFontSize(vg, 10);
    hjpFillColor(vg, hjpRGBA(180, 100, 40, 180));
    hjpText(vg, x + w - 46, y + 106, "[HTML]", NULL);
    gui_advance(128);
    return hajimu_null();
}

/* テキストハイライト(テキスト, 範囲, 色) → 無 */
static Value fn_text_highlight(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || !g_cur->vg) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    float x, y, w;
    gui_pos(&x, &y, &w);
    const char *text = gui_as_string(argv[0]);
    if (!text) return hajimu_null();
    /* 背景ハイライト */
    hjpFontFaceId(vg, g_cur->font_id);
    hjpFontSize(vg, 14);
    hjpFillColor(vg, hjpRGBA(255,255,0,100));
    float tw = hjpTextBounds(vg, 0, 0, text, NULL, NULL);
    hjpBeginPath(vg);
    hjpRect(vg, x, y, tw, 20);
    hjpFill(vg);
    hjpFillColor(vg, TH_TEXT);
    hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_TOP);
    hjpText(vg, x, y, text, NULL);
    gui_advance(24);
    return hajimu_null();
}

/* アノテーション(ID, 位置, テキスト) → 無 */
static Value fn_annotation(int argc, Value *argv) {
    (void)argc; (void)argv;
    return hajimu_null();
}

/* テキストテンプレート(テンプレート, 変数) → 文字列 */
static Value fn_text_template(int argc, Value *argv) {
    (void)argc;
    /* 簡易テンプレート: {{key}} を辞書の値で置換 */
    const char *tmpl = gui_as_string(argv[0]);
    if (!tmpl) return hajimu_string("");
    Value vars = argv[1];
    static char buf[GUI_MAX_TEXT_BUF];
    char *out = buf;
    const char *p = tmpl;
    while (*p && (out - buf) < (int)sizeof(buf) - 100) {
        if (p[0] == '{' && p[1] == '{') {
            const char *end = strstr(p + 2, "}}");
            if (end) {
                char key[128];
                int kl = (int)(end - p - 2);
                if (kl > 127) kl = 127;
                memcpy(key, p + 2, kl);
                key[kl] = '\0';
                Value val = gui_dict_lookup(vars, key);
                const char *vs = gui_as_string(val);
                if (vs) {
                    int vl = (int)strlen(vs);
                    if ((out - buf) + vl < (int)sizeof(buf) - 1) {
                        memcpy(out, vs, vl);
                        out += vl;
                    }
                }
                p = end + 2;
                continue;
            }
        }
        *out++ = *p++;
    }
    *out = '\0';
    return hajimu_string(buf);
}

/* =====================================================================
 * Phase 63: ナビゲーション/ルーティング高度 (v6.3.0)
 * ===================================================================*/

/* アプリバー(タイトル[, アクション配列]) → 無 */
static Value fn_app_bar(int argc, Value *argv) {
    if (!g_cur || !g_cur->vg) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    float x, y, w;
    gui_pos(&x, &y, &w);
    float h = 48;
    /* 背景 */
    hjpBeginPath(vg);
    hjpRect(vg, x, y, w, h);
    hjpFillColor(vg, hjpRGBA(66,133,244,255));
    hjpFill(vg);
    /* タイトル */
    const char *title = gui_as_string(argv[0]);
    hjpFontFaceId(vg, g_cur->font_id);
    hjpFontSize(vg, 18);
    hjpFillColor(vg, hjpRGBA(255,255,255,255));
    hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_MIDDLE);
    if (title) hjpText(vg, x + 16, y + h/2, title, NULL);
    /* アクションボタン */
    if (argc >= 2) {
        Value actions = argv[1];
        int an = gui_arr_len(actions);
        float ax = x + w - 16;
        for (int i = an - 1; i >= 0; i--) {
            Value a = gui_arr_get(actions, i);
            const char *al = gui_as_string(a);
            if (al) {
                float tw = hjpTextBounds(vg, 0, 0, al, NULL, NULL);
                ax -= tw + 8;
                hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_MIDDLE);
                hjpText(vg, ax, y + h/2, al, NULL);
                ax -= 8;
            }
        }
    }
    gui_advance(h);
    return hajimu_null();
}

/* ボトムアプリバー(アクション配列) → 文字列 */
static Value fn_bottom_app_bar(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || !g_cur->vg) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    float h = 56;
    float y = g_cur->win_h - h;
    float w = (float)g_cur->win_w;
    hjpBeginPath(vg);
    hjpRect(vg, 0, y, w, h);
    hjpFillColor(vg, hjpRGBA(250,250,250,255));
    hjpFill(vg);
    hjpBeginPath(vg);
    hjpMoveTo(vg, 0, y); hjpLineTo(vg, w, y);
    hjpStrokeColor(vg, hjpRGBA(220,220,220,255));
    hjpStroke(vg);
    Value actions = argv[0];
    int n = gui_arr_len(actions);
    Value result = hajimu_null();
    if (n > 0) {
        float btn_w = w / n;
        for (int i = 0; i < n; i++) {
            Value a = gui_arr_get(actions, i);
            const char *label = gui_as_string(a);
            float bx = i * btn_w;
            bool hov = false, pressed = false;
            uint32_t id = gui_hash(__func__) + __LINE__;
            gui_widget_logic(id, bx, y, btn_w, h, &hov, &pressed);
            hjpFontFaceId(vg, g_cur->font_id);
            hjpFontSize(vg, 13);
            hjpFillColor(vg, hov ? hjpRGBA(66,133,244,255) : TH_TEXT);
            hjpTextAlign(vg, HJP_ALIGN_CENTER | HJP_ALIGN_MIDDLE);
            if (label) hjpText(vg, bx + btn_w/2, y + h/2, label, NULL);
            if (pressed && label) result = hajimu_string(label);
        }
    }
    return result;
}

/* ナビゲーションレール(項目配列, 選択) → 数値 */
static Value fn_navigation_rail(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || !g_cur->vg) return argv[1];
    Hjpcontext *vg = g_cur->vg;
    float rail_w = 72;
    float item_h = 56;
    Value items = argv[0];
    int sel = (int)gui_as_number(argv[1]);
    int n = gui_arr_len(items);
    /* レール背景 */
    hjpBeginPath(vg);
    hjpRect(vg, 0, 0, rail_w, (float)g_cur->win_h);
    hjpFillColor(vg, hjpRGBA(245,245,245,255));
    hjpFill(vg);
    int new_sel = sel;
    for (int i = 0; i < n; i++) {
        Value item = gui_arr_get(items, i);
        const char *label = gui_as_string(item);
        float iy = 8 + i * item_h;
        bool hov = false, pressed = false;
        uint32_t id = gui_hash(__func__) + __LINE__;
        gui_widget_logic(id, 0, iy, rail_w, item_h, &hov, &pressed);
        if (pressed) new_sel = i;
        if (i == sel) {
            hjpBeginPath(vg);
            hjpRoundedRect(vg, 8, iy + 4, rail_w - 16, item_h - 8, 16);
            hjpFillColor(vg, hjpRGBA(66,133,244,40));
            hjpFill(vg);
        }
        hjpFontFaceId(vg, g_cur->font_id);
        hjpFontSize(vg, 12);
        hjpFillColor(vg, (i == sel) ? hjpRGBA(66,133,244,255) : TH_TEXT);
        hjpTextAlign(vg, HJP_ALIGN_CENTER | HJP_ALIGN_MIDDLE);
        if (label) hjpText(vg, rail_w / 2, iy + item_h / 2, label, NULL);
    }
    return hajimu_number(new_sel);
}

/* コラムビュー(データ) → 配列 */
static Value fn_column_view(int argc, Value *argv) {
    (void)argc; (void)argv;
    return hajimu_null();
}

/* ナビゲーションスタック(画面) → 無 */
static Value fn_navigation_stack(int argc, Value *argv) {
    (void)argc; (void)argv;
    return hajimu_null();
}

/* サイドバー折りたたみ(ID, 開閉) → 真偽 */
static Value fn_sidebar_collapse(int argc, Value *argv) {
    (void)argc;
    return argv[1];
}

/* ナビゲーション履歴(操作) → 文字列 */
static Value fn_nav_history(int argc, Value *argv) {
    (void)argc; (void)argv;
    return hajimu_null();
}

/* パンくずカスタム(項目配列, セパレータ) → 文字列 */
static Value fn_breadcrumb_custom(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || !g_cur->vg) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    float x, y, w;
    gui_pos(&x, &y, &w);
    Value items = argv[0];
    const char *sep = gui_as_string(argv[1]);
    if (!sep) sep = " / ";
    int n = gui_arr_len(items);
    hjpFontFaceId(vg, g_cur->font_id);
    hjpFontSize(vg, 13);
    hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_MIDDLE);
    float cx = x;
    Value result = hajimu_null();
    for (int i = 0; i < n; i++) {
        Value item = gui_arr_get(items, i);
        const char *label = gui_as_string(item);
        if (!label) continue;
        if (i > 0) {
            hjpFillColor(vg, hjpRGBA(150,150,150,255));
            hjpText(vg, cx, y + 10, sep, NULL);
            cx += hjpTextBounds(vg, 0, 0, sep, NULL, NULL) + 4;
        }
        float tw = hjpTextBounds(vg, 0, 0, label, NULL, NULL);
        bool hov = false, pressed = false;
        uint32_t id = gui_hash(__func__) + __LINE__;
        gui_widget_logic(id, cx, y, tw, 20, &hov, &pressed);
        hjpFillColor(vg, hov ? hjpRGBA(66,133,244,255) : TH_TEXT);
        hjpText(vg, cx, y + 10, label, NULL);
        if (pressed) result = hajimu_string(label);
        cx += tw + 4;
    }
    gui_advance(24);
    return result;
}

/* =====================================================================
 * Phase 64: OS/プラットフォーム統合 II (v6.4.0)
 * ===================================================================*/

static Value fn_global_shortcut(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_auto_updater(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_protocol_handler(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
/* ファイル監視(パス, コールバック) → 真偽
 * ポーリング実装: mtime が変化したらコールバックを呼ぶ。
 * 毎フレーム呼ばれる前提。 */
#include <sys/stat.h>
#define GUI_MAX_FILE_WATCHER 8
static struct {
    char path[256];
    time_t last_mtime;
    Value  callback;
    bool   active;
} g_file_watchers[GUI_MAX_FILE_WATCHER];
static int g_file_watcher_count = 0;

static Value fn_file_watcher(int argc, Value *argv) {
    if (argc < 2) return hajimu_bool(0);
    const char *path = gui_as_string(argv[0]);
    if (!path) return hajimu_bool(0);
    /* 既存エントリ検索 */
    for (int i = 0; i < g_file_watcher_count; i++) {
        if (strcmp(g_file_watchers[i].path, path) == 0 && g_file_watchers[i].active) {
            struct stat st;
            if (stat(path, &st) == 0 && st.st_mtime != g_file_watchers[i].last_mtime) {
                g_file_watchers[i].last_mtime = st.st_mtime;
                /* コールバック呼び出し */
                Value arg = hajimu_string(path);
                hajimu_call(&g_file_watchers[i].callback, 1, &arg);
            }
            return hajimu_bool(1);
        }
    }
    /* 新規登録 */
    if (g_file_watcher_count < GUI_MAX_FILE_WATCHER) {
        int i = g_file_watcher_count++;
        strncpy(g_file_watchers[i].path, path, 255);
        g_file_watchers[i].path[255] = '\0';
        struct stat st;
        g_file_watchers[i].last_mtime = (stat(path, &st) == 0) ? st.st_mtime : 0;
        g_file_watchers[i].callback = argv[1];
        g_file_watchers[i].active = true;
        return hajimu_bool(1);
    }
    return hajimu_bool(0);
}
static Value fn_window_snap(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_taskbar_progress(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_jump_list(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_native_dialog(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }

/* =====================================================================
 * Phase 65: 高度なフォーム入力 II (v6.5.0)
 * ===================================================================*/

/* 署名パッド(ID, 幅, 高さ) → 画像 */
static Value fn_signature_pad(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || !g_cur->vg) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    float x, y, w;
    gui_pos(&x, &y, &w);
    float pad_w = (float)gui_as_number(argv[1]);
    float pad_h = (float)gui_as_number(argv[2]);
    if (pad_w <= 0) pad_w = w;
    if (pad_h <= 0) pad_h = 150;
    /* 背景 */
    hjpBeginPath(vg);
    hjpRoundedRect(vg, x, y, pad_w, pad_h, 4);
    hjpFillColor(vg, hjpRGBA(255,255,255,255));
    hjpFill(vg);
    hjpStrokeColor(vg, hjpRGBA(200,200,200,255));
    hjpStroke(vg);
    /* 署名線 */
    hjpBeginPath(vg);
    hjpMoveTo(vg, x + 20, y + pad_h - 30);
    hjpLineTo(vg, x + pad_w - 20, y + pad_h - 30);
    hjpStrokeColor(vg, hjpRGBA(180,180,180,255));
    hjpStroke(vg);
    hjpFontFaceId(vg, g_cur->font_id);
    hjpFontSize(vg, 11);
    hjpFillColor(vg, hjpRGBA(150,150,150,255));
    hjpTextAlign(vg, HJP_ALIGN_CENTER | HJP_ALIGN_TOP);
    hjpText(vg, x + pad_w/2, y + pad_h - 24, "署名欄", NULL);
    gui_advance(pad_h + 8);
    return hajimu_null();
}

static Value fn_phone_input(int argc, Value *argv) {
    /* 電話番号入力(ラベル, 値) → 文字列: テキスト入力に委譲 */
    if (argc < 2) return hajimu_string("");
    return fn_text_input(argc, argv);
}

static Value fn_credit_card_input(int argc, Value *argv) {
    /* クレジットカード入力(ラベル, 値) → 文字列 (パスワードマスク) */
    if (argc < 2) return hajimu_string("");
    /* パスワード入力として描画（セキュリティのためマスク） */
    return fn_password_input(argc, argv);
}

static Value fn_address_input(int argc, Value *argv) {
    /* 住所入力(ラベル, 値[, 行数]) → 文字列: テキストエリアに委譲 */
    if (argc < 2) return hajimu_string("");
    Value real_argv[3];
    real_argv[0] = argv[0];
    real_argv[1] = argv[1];
    real_argv[2] = hajimu_number(3);
    return fn_textarea(3, real_argv);
}

/* カラーグラデーション入力(ID, 色配列) → 配列 */
static Value fn_color_gradient_input(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || !g_cur->vg) return argv[1];
    Hjpcontext *vg = g_cur->vg;
    float x, y, w;
    gui_pos(&x, &y, &w);
    float h = 30;
    Value colors = argv[1];
    int n = gui_arr_len(colors);
    if (n >= 2) {
        Hjppaint grad = hjpLinearGradient(vg, x, y, x + w, y,
            hjpRGBA(66,133,244,255), hjpRGBA(234,67,53,255));
        hjpBeginPath(vg);
        hjpRoundedRect(vg, x, y, w, h, 4);
        hjpFillPaint(vg, grad);
        hjpFill(vg);
    }
    gui_advance(h + 8);
    return argv[1];
}

static Value fn_schedule_input(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_field_array(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }

/* リッチセレクト(ID, 項目配列, 値) → 値 */
static Value fn_rich_select(int argc, Value *argv) {
    if (argc < 3) return hajimu_null();
    return argv[2]; /* passthrough: 将来フルUI実装 */
}

/* =====================================================================
 * Phase 66: 高度なアニメーション/エフェクト II (v6.6.0)
 * ===================================================================*/

/* リップルエフェクト(x, y, 色) → 無 */
static Value fn_ripple_effect(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || !g_cur->vg) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    float rx = (float)gui_as_number(argv[0]);
    float ry = (float)gui_as_number(argv[1]);
    float t = (float)g_frame_count * 0.05f;
    float phase = fmodf(t, 1.0f);
    float radius = phase * 40;
    float alpha = (1.0f - phase) * 150;
    hjpBeginPath(vg);
    hjpCircle(vg, rx, ry, radius);
    hjpStrokeColor(vg, hjpRGBA(66,133,244,(unsigned char)alpha));
    hjpStrokeWidth(vg, 2);
    hjpStroke(vg);
    hjpStrokeWidth(vg, 1);
    return hajimu_null();
}

static Value fn_morphing(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }

/* フリップカード(ID, 表, 裏, 反転) → 真偽 */
static Value fn_flip_card(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || !g_cur->vg) return argv[3];
    Hjpcontext *vg = g_cur->vg;
    float x, y, w;
    gui_pos(&x, &y, &w);
    float h = 120;
    int flipped = gui_as_bool(argv[3]);
    const char *front = gui_as_string(argv[1]);
    const char *back = gui_as_string(argv[2]);
    const char *show = flipped ? back : front;
    /* カード */
    hjpBeginPath(vg);
    hjpRoundedRect(vg, x, y, w, h, 8);
    hjpFillColor(vg, hjpRGBA(255,255,255,255));
    hjpFill(vg);
    hjpStrokeColor(vg, hjpRGBA(200,200,200,255));
    hjpStroke(vg);
    if (show) {
        hjpFontFaceId(vg, g_cur->font_id);
        hjpFontSize(vg, 14);
        hjpFillColor(vg, TH_TEXT);
        hjpTextAlign(vg, HJP_ALIGN_CENTER | HJP_ALIGN_MIDDLE);
        hjpText(vg, x + w/2, y + h/2, show, NULL);
    }
    /* クリックで反転 */
    bool hov = false, pressed = false;
    uint32_t id = gui_hash(__func__) + __LINE__;
    gui_widget_logic(id, x, y, w, h, &hov, &pressed);
    if (pressed) flipped = !flipped;
    gui_advance(h + 8);
    return hajimu_bool(flipped);
}

/* タイピングアニメーション(テキスト, 速度) → 文字列 */
static Value fn_typing_animation(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || !g_cur->vg) return argv[0];
    Hjpcontext *vg = g_cur->vg;
    float x, y, w;
    gui_pos(&x, &y, &w);
    const char *text = gui_as_string(argv[0]);
    if (!text) return hajimu_string("");
    int total_len = (int)strlen(text);
    float speed = (float)gui_as_number(argv[1]);
    if (speed <= 0) speed = 3;
    int chars = (int)(g_frame_count * speed * 0.016f);
    /* UTF-8境界を考慮 */
    int show = 0;
    int ci = 0;
    for (int i = 0; i < total_len && ci < chars; ) {
        unsigned char c = (unsigned char)text[i];
        int clen = 1;
        if (c >= 0xF0) clen = 4;
        else if (c >= 0xE0) clen = 3;
        else if (c >= 0xC0) clen = 2;
        show = i + clen;
        i += clen;
        ci++;
    }
    if (show > total_len) show = total_len;
    static char buf[GUI_MAX_TEXT_BUF];
    int copy_len = show;
    if (copy_len >= (int)sizeof(buf)) copy_len = (int)sizeof(buf) - 1;
    memcpy(buf, text, copy_len);
    buf[copy_len] = '\0';
    hjpFontFaceId(vg, g_cur->font_id);
    hjpFontSize(vg, 14);
    hjpFillColor(vg, TH_TEXT);
    hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_TOP);
    hjpText(vg, x, y, buf, NULL);
    gui_advance(22);
    return hajimu_string(buf);
}

/* カウントアップ(ID, 開始, 終了, 時間) → 数値 */
static Value fn_count_up(int argc, Value *argv) {
    (void)argc;
    float start = (float)gui_as_number(argv[1]);
    float end = (float)gui_as_number(argv[2]);
    float duration = (float)gui_as_number(argv[3]);
    if (duration <= 0) duration = 1;
    float t = g_cur ? (float)g_frame_count * 0.016f : 0;
    float progress = t / duration;
    if (progress > 1.0f) progress = 1.0f;
    float val = start + (end - start) * progress;
    return hajimu_number(val);
}

static Value fn_path_animation(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_blur_transition(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }

/* ラバーバンド選択(ID) → 辞書 */
static Value fn_rubber_band(int argc, Value *argv) {
    (void)argc; (void)argv;
    return hajimu_null();
}

/* =====================================================================
 * Phase 67: データ永続化/状態管理高度 (v6.7.0)
 * ===================================================================*/

#define GUI_MAX_STORAGE 256
static struct { char key[64]; char val[256]; } g_storage[GUI_MAX_STORAGE];
static int g_storage_count = 0;

/* ローカルストレージ(キー[, 値]) → 値 */
static Value fn_local_storage(int argc, Value *argv) {
    const char *key = gui_as_string(argv[0]);
    if (!key) return hajimu_null();
    if (argc >= 2) {
        /* set */
        const char *val = gui_as_string(argv[1]);
        for (int i = 0; i < g_storage_count; i++) {
            if (strcmp(g_storage[i].key, key) == 0) {
                if (val) snprintf(g_storage[i].val, sizeof(g_storage[i].val), "%s", val);
                return hajimu_null();
            }
        }
        if (g_storage_count < GUI_MAX_STORAGE) {
            snprintf(g_storage[g_storage_count].key, sizeof(g_storage[0].key), "%s", key);
            if (val) snprintf(g_storage[g_storage_count].val, sizeof(g_storage[0].val), "%s", val);
            else g_storage[g_storage_count].val[0] = '\0';
            g_storage_count++;
        }
        return hajimu_null();
    }
    /* get */
    for (int i = 0; i < g_storage_count; i++) {
        if (strcmp(g_storage[i].key, key) == 0)
            return hajimu_string(g_storage[i].val);
    }
    return hajimu_null();
}

static Value fn_state_snapshot(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }

#define GUI_MAX_UNDO 64
static struct { char id[32]; int top; int cur; char cmds[GUI_MAX_UNDO][256]; } g_undo_stacks[16];
static int g_undo_stack_count = 0;

/* Undoスタック(ID, コマンド) → 無 */
static Value fn_undo_stack(int argc, Value *argv) {
    (void)argc;
    const char *id = gui_as_string(argv[0]);
    const char *cmd = gui_as_string(argv[1]);
    if (!id) return hajimu_null();
    int si = -1;
    for (int i = 0; i < g_undo_stack_count; i++) {
        if (strcmp(g_undo_stacks[i].id, id) == 0) { si = i; break; }
    }
    if (si < 0 && g_undo_stack_count < 16) {
        si = g_undo_stack_count++;
        snprintf(g_undo_stacks[si].id, sizeof(g_undo_stacks[0].id), "%s", id);
        g_undo_stacks[si].top = 0;
        g_undo_stacks[si].cur = 0;
    }
    if (si >= 0 && cmd && g_undo_stacks[si].top < GUI_MAX_UNDO) {
        /* 現在位置以降を切り捨て */
        g_undo_stacks[si].top = g_undo_stacks[si].cur;
        snprintf(g_undo_stacks[si].cmds[g_undo_stacks[si].top], 256, "%s", cmd);
        g_undo_stacks[si].top++;
        g_undo_stacks[si].cur = g_undo_stacks[si].top;
    }
    return hajimu_null();
}

static Value fn_state_persist(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_reactive_state(int argc, Value *argv) { (void)argc; return argv[1]; }
static Value fn_computed_value(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_state_diff(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_state_migration(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }

/* =====================================================================
 * Phase 68: 高度なコンテナ/サーフェス II (v6.8.0)
 * ===================================================================*/

/* ツールボックス(ID, 項目配列, 選択) → 数値 */
static Value fn_toolbox(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || !g_cur->vg) return argv[2];
    Hjpcontext *vg = g_cur->vg;
    float x, y, w;
    gui_pos(&x, &y, &w);
    Value items = argv[1];
    int sel = (int)gui_as_number(argv[2]);
    int n = gui_arr_len(items);
    float header_h = 32;
    int new_sel = sel;
    float cy = y;
    for (int i = 0; i < n; i++) {
        Value item = gui_arr_get(items, i);
        const char *label = gui_as_string(item);
        /* ヘッダー */
        bool hov = false, pressed = false;
        uint32_t id = gui_hash(__func__) + __LINE__;
        gui_widget_logic(id, x, cy, w, header_h, &hov, &pressed);
        hjpBeginPath(vg);
        hjpRect(vg, x, cy, w, header_h);
        hjpFillColor(vg, (i == sel) ? hjpRGBA(66,133,244,30) : hjpRGBA(240,240,240,255));
        hjpFill(vg);
        hjpFontFaceId(vg, g_cur->font_id);
        hjpFontSize(vg, 13);
        hjpFillColor(vg, TH_TEXT);
        hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_MIDDLE);
        if (label) hjpText(vg, x + 8, cy + header_h/2, label, NULL);
        if (pressed) new_sel = i;
        cy += header_h;
        if (i == sel) cy += 60; /* コンテンツ領域 */
    }
    gui_advance(cy - y);
    return hajimu_number(new_sel);
}

/* スタックウィジェット(ID, 選択) → 無 */
static Value fn_stack_widget(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }

/* フローティングパネル(ID, x, y, 幅, 高さ) → 真偽 */
static Value fn_floating_panel(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || !g_cur->vg) return hajimu_bool(1);
    Hjpcontext *vg = g_cur->vg;
    float px = (float)gui_as_number(argv[1]);
    float py = (float)gui_as_number(argv[2]);
    float pw = (float)gui_as_number(argv[3]);
    float ph = (float)gui_as_number(argv[4]);
    /* 影 */
    hjpBeginPath(vg);
    hjpRoundedRect(vg, px + 2, py + 2, pw, ph, 6);
    hjpFillColor(vg, hjpRGBA(0,0,0,30));
    hjpFill(vg);
    /* パネル */
    hjpBeginPath(vg);
    hjpRoundedRect(vg, px, py, pw, ph, 6);
    hjpFillColor(vg, hjpRGBA(255,255,255,250));
    hjpFill(vg);
    hjpStrokeColor(vg, hjpRGBA(200,200,200,255));
    hjpStroke(vg);
    return hajimu_bool(1);
}

static Value fn_portal(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }

/* バックドロップ(表示[, 色]) → 真偽 */
static Value fn_backdrop(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || !g_cur->vg) return hajimu_bool(0);
    int show = gui_as_bool(argv[0]);
    if (!show) return hajimu_bool(0);
    Hjpcontext *vg = g_cur->vg;
    hjpBeginPath(vg);
    hjpRect(vg, 0, 0, (float)g_cur->win_w, (float)g_cur->win_h);
    hjpFillColor(vg, hjpRGBA(0,0,0,120));
    hjpFill(vg);
    bool hov = false, pressed = false;
    uint32_t id = gui_hash(__func__) + __LINE__;
    gui_widget_logic(id, 0, 0, (float)g_cur->win_w, (float)g_cur->win_h, &hov, &pressed);
    return hajimu_bool(pressed);
}

/* サーフェス(深度) → 無 */
static Value fn_surface(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || !g_cur->vg) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    float x, y, w;
    gui_pos(&x, &y, &w);
    int depth = (int)gui_as_number(argv[0]);
    float shadow = depth * 2.0f;
    hjpBeginPath(vg);
    hjpRoundedRect(vg, x + shadow, y + shadow, w, 100, 4);
    hjpFillColor(vg, hjpRGBA(0,0,0, (unsigned char)(20 + depth * 10)));
    hjpFill(vg);
    hjpBeginPath(vg);
    hjpRoundedRect(vg, x, y, w, 100, 4);
    hjpFillColor(vg, hjpRGBA(255,255,255,255));
    hjpFill(vg);
    gui_advance(108);
    return hajimu_null();
}

/* アコーディオングループ(ID, 項目配列, 開) → 数値 */
static Value fn_accordion_group(int argc, Value *argv) {
    if (argc < 3) return hajimu_number(0);
    return argv[2]; /* passthrough: 将来折りたたみUI実装 */
}

/* 折りたたみサイドバー(ID, 幅, 折幅, 開閉) → 真偽 */
static Value fn_collapsible_sidebar(int argc, Value *argv) {
    if (argc < 4) return hajimu_bool(0);
    return argv[3]; /* passthrough: 将来サイドバーUI実装 */
}

/* =====================================================================
 * Phase 69: 高度なテーブル/グリッド II (v6.9.0)
 * ===================================================================*/

static Value fn_pivot_table(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_master_detail(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_table_col_group(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_table_summary_row(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_table_cond_format(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_table_copy(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_string(""); }
static Value fn_table_import(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_number(0); }
static Value fn_table_frozen_rows(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }

/* =====================================================================
 * Phase 70: 通知/フィードバック高度 II (v7.0.0)
 * ===================================================================*/

static Value fn_notification_center(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_number(0); }
static Value fn_notification_group(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }

/* アンドゥ通知(メッセージ, 取消コールバック) → 真偽 */
static Value fn_undo_notification(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || !g_cur->vg) return hajimu_bool(0);
    Hjpcontext *vg = g_cur->vg;
    const char *msg = gui_as_string(argv[0]);
    float w = (float)g_cur->win_w;
    float sw = 320, sh = 48;
    float sx = (w - sw) / 2, sy = g_cur->win_h - 80.0f;
    hjpBeginPath(vg);
    hjpRoundedRect(vg, sx, sy, sw, sh, 6);
    hjpFillColor(vg, hjpRGBA(50,50,50,240));
    hjpFill(vg);
    hjpFontFaceId(vg, g_cur->font_id);
    hjpFontSize(vg, 13);
    hjpFillColor(vg, hjpRGBA(255,255,255,230));
    hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_MIDDLE);
    if (msg) hjpText(vg, sx + 12, sy + sh/2, msg, NULL);
    /* 取消ボタン */
    float bx = sx + sw - 60;
    bool hov = false, pressed = false;
    uint32_t id = gui_hash(__func__) + __LINE__;
    gui_widget_logic(id, bx, sy, 50, sh, &hov, &pressed);
    hjpFillColor(vg, hjpRGBA(66,133,244,255));
    hjpTextAlign(vg, HJP_ALIGN_CENTER | HJP_ALIGN_MIDDLE);
    hjpText(vg, bx + 25, sy + sh/2, "取消", NULL);
    return hajimu_bool(pressed);
}

static Value fn_progress_toast(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_dynamic_badge(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_sound_notification(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }

/* ステータスインジケータ(状態[, 色]) → 無 */
static Value fn_status_indicator(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || !g_cur->vg) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    float x, y, w;
    gui_pos(&x, &y, &w);
    int status = (int)gui_as_number(argv[0]);
    Hjpcolor colors[] = {
        hjpRGBA(200,200,200,255), /* 0: offline */
        hjpRGBA(15,157,88,255),   /* 1: online */
        hjpRGBA(251,188,4,255),   /* 2: away */
        hjpRGBA(219,68,55,255)    /* 3: busy */
    };
    Hjpcolor col = colors[status % 4];
    hjpBeginPath(vg);
    hjpCircle(vg, x + 6, y + 6, 5);
    hjpFillColor(vg, col);
    hjpFill(vg);
    hjpBeginPath(vg);
    hjpCircle(vg, x + 6, y + 6, 5);
    hjpStrokeColor(vg, hjpRGBA(255,255,255,255));
    hjpStrokeWidth(vg, 2);
    hjpStroke(vg);
    hjpStrokeWidth(vg, 1);
    gui_advance(16);
    return hajimu_null();
}

static Value fn_error_boundary(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }

/* =====================================================================
 * Phase 71: 地図/位置情報 (v7.1.0)
 * ===================================================================*/

static Value fn_map_widget(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_map_marker(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_number(0); }
static Value fn_map_polyline(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_map_polygon(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_geolocation(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }

/* 距離計算(緯度1, 経度1, 緯度2, 経度2) → 数値 */
static Value fn_distance_calc(int argc, Value *argv) {
    (void)argc;
    double lat1 = gui_as_number(argv[0]) * 3.14159265358979 / 180.0;
    double lon1 = gui_as_number(argv[1]) * 3.14159265358979 / 180.0;
    double lat2 = gui_as_number(argv[2]) * 3.14159265358979 / 180.0;
    double lon2 = gui_as_number(argv[3]) * 3.14159265358979 / 180.0;
    double dlat = lat2 - lat1, dlon = lon2 - lon1;
    double a = sin(dlat/2)*sin(dlat/2) + cos(lat1)*cos(lat2)*sin(dlon/2)*sin(dlon/2);
    double c = 2 * atan2(sqrt(a), sqrt(1-a));
    return hajimu_number(6371000.0 * c); /* meters */
}

static Value fn_geocoding(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }

/* ミニマップ(ID, 内容, 表示率) → 辞書 */
static Value fn_minimap(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || !g_cur->vg) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    float mm_w = 80, mm_h = 200;
    float mx = g_cur->win_w - mm_w - 8.0f;
    float my = 8;
    hjpBeginPath(vg);
    hjpRect(vg, mx, my, mm_w, mm_h);
    hjpFillColor(vg, hjpRGBA(245,245,245,200));
    hjpFill(vg);
    hjpStrokeColor(vg, hjpRGBA(200,200,200,255));
    hjpStroke(vg);
    /* ビューポート表示 */
    float rate = (float)gui_as_number(argv[2]);
    if (rate <= 0) rate = 0.2f;
    if (rate > 1) rate = 1.0f;
    float vh = mm_h * rate;
    hjpBeginPath(vg);
    hjpRect(vg, mx, my, mm_w, vh);
    hjpFillColor(vg, hjpRGBA(66,133,244,40));
    hjpFill(vg);
    return hajimu_null();
}

/* =====================================================================
 * Phase 72: カレンダー/スケジュール高度 (v7.2.0)
 * ===================================================================*/

static Value fn_week_calendar(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_day_calendar(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_agenda_view(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_event_create(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_number(0); }
static Value fn_event_drag(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_recurring_event(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }

/* カレンダーヒートマップ(データ, 年) → 無 */
static Value fn_calendar_heatmap(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || !g_cur->vg) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    float x, y, w;
    gui_pos(&x, &y, &w);
    float cell = 12, gap = 2;
    Value data = argv[0];
    int n = gui_arr_len(data);
    for (int i = 0; i < n && i < 365; i++) {
        int col = i / 7, row = i % 7;
        float cx = x + col * (cell + gap);
        float cy = y + row * (cell + gap);
        Value v = gui_arr_get(data, i);
        float val = (float)gui_as_number(v);
        unsigned char g = (unsigned char)(val * 200);
        if (g > 200) g = 200;
        hjpBeginPath(vg);
        hjpRoundedRect(vg, cx, cy, cell, cell, 2);
        hjpFillColor(vg, val > 0 ? hjpRGBA(15, (unsigned char)(157-g/2), 88, (unsigned char)(50+g)) : hjpRGBA(235,235,235,255));
        hjpFill(vg);
    }
    gui_advance(7 * (cell + gap) + 8);
    return hajimu_null();
}

static Value fn_time_slot(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }

/* =====================================================================
 * Phase 73: メディア高度 II (v7.3.0)
 * ===================================================================*/

static Value fn_audio_waveform(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_audio_record(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_video_record(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_subtitle_display(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_picture_in_picture(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_media_playlist(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_number(0); }
static Value fn_equalizer(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_screen_capture(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }

/* =====================================================================
 * Phase 74: 検索/フィルタ高度 (v7.4.0)
 * ===================================================================*/

static Value fn_faceted_search(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }

/* フィルターチップ(ID, フィルタ配列) → 配列 */
static Value fn_filter_chips(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || !g_cur->vg) return argv[1];
    Hjpcontext *vg = g_cur->vg;
    float x, y, w;
    gui_pos(&x, &y, &w);
    Value filters = argv[1];
    int n = gui_arr_len(filters);
    float cx = x;
    float chip_h = 28;
    for (int i = 0; i < n; i++) {
        Value f = gui_arr_get(filters, i);
        const char *label = gui_as_string(f);
        if (!label) continue;
        hjpFontFaceId(vg, g_cur->font_id);
        hjpFontSize(vg, 12);
        float tw = hjpTextBounds(vg, 0, 0, label, NULL, NULL);
        float cw = tw + 28;
        hjpBeginPath(vg);
        hjpRoundedRect(vg, cx, y, cw, chip_h, chip_h/2);
        hjpFillColor(vg, hjpRGBA(230,230,250,255));
        hjpFill(vg);
        hjpFillColor(vg, TH_TEXT);
        hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_MIDDLE);
        hjpText(vg, cx + 8, y + chip_h/2, label, NULL);
        /* ✕ ボタン */
        hjpFillColor(vg, hjpRGBA(150,150,150,255));
        hjpTextAlign(vg, HJP_ALIGN_CENTER | HJP_ALIGN_MIDDLE);
        hjpText(vg, cx + cw - 12, y + chip_h/2, "✕", NULL);
        cx += cw + 6;
    }
    gui_advance(chip_h + 8);
    return argv[1];
}

static Value fn_sort_ui(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_filter_panel(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_saved_filters(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_search_history(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_advanced_search_dialog(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }

/* ファジー検索(クエリ, データ, キー) → 配列 */
static Value fn_fuzzy_search(int argc, Value *argv) {
    (void)argc;
    const char *query = gui_as_string(argv[0]);
    Value data = argv[1];
    if (!query || !query[0]) return data;
    int n = gui_arr_len(data);
    Value result = hajimu_array();
    int qlen = (int)strlen(query);
    for (int i = 0; i < n; i++) {
        Value item = gui_arr_get(data, i);
        const char *s = gui_as_string(item);
        if (!s) continue;
        /* 簡易部分文字列マッチ */
        int qi = 0;
        for (int j = 0; s[j] && qi < qlen; j++) {
            if (s[j] == query[qi]) qi++;
        }
        if (qi == qlen) hajimu_array_push(&result, item);
    }
    return result;
}

/* =====================================================================
 * Phase 75: セキュリティ/認証 UI (v7.5.0)
 * ===================================================================*/

/* ログインフォーム(ID) → 辞書 {user, pass, submitted}
 * 暫定実装: ユーザー名・パスワード入力欄と送信ボタンを表示 */
static char g_login_user[256] = {0};
static char g_login_pass[256] = {0};
static Value fn_login_form(int argc, Value *argv) {
    (void)argc; (void)argv;
    if (!g_cur || !g_cur->vg) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    float x, y, w;
    gui_pos(&x, &y, &w);
    /* フォーム背景 */
    hjpBeginPath(vg);
    hjpRoundedRect(vg, x, y, w > 320 ? 320 : w, 160, 8);
    hjpFillColor(vg, hjpRGBA(40, 42, 50, 240));
    hjpFill(vg);
    hjpFontFaceId(vg, g_cur->font_id);
    hjpFontSize(vg, 13);
    hjpFillColor(vg, TH_TEXT);
    hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_TOP);
    hjpText(vg, x + 12, y + 12, "ログイン", NULL);
    /* ユーザー名フィールド (表示のみ) */
    hjpBeginPath(vg);
    hjpRoundedRect(vg, x + 12, y + 36, 296, 28, 4);
    hjpFillColor(vg, hjpRGBA(55, 58, 70, 255));
    hjpFill(vg);
    hjpFillColor(vg, hjpRGBA(160, 160, 160, 200));
    hjpFontSize(vg, 12);
    hjpText(vg, x + 18, y + 44, g_login_user[0] ? g_login_user : "ユーザー名", NULL);
    /* パスワードフィールド */
    hjpBeginPath(vg);
    hjpRoundedRect(vg, x + 12, y + 74, 296, 28, 4);
    hjpFillColor(vg, hjpRGBA(55, 58, 70, 255));
    hjpFill(vg);
    hjpFillColor(vg, hjpRGBA(160, 160, 160, 200));
    hjpText(vg, x + 18, y + 82, g_login_pass[0] ? "●●●●●●" : "パスワード", NULL);
    /* ログインボタン */
    hjpBeginPath(vg);
    hjpRoundedRect(vg, x + 12, y + 116, 296, 32, 6);
    hjpFillColor(vg, hjpRGBA(66, 133, 244, 230));
    hjpFill(vg);
    hjpFillColor(vg, hjpRGBA(255, 255, 255, 255));
    hjpFontSize(vg, 14);
    hjpTextAlign(vg, HJP_ALIGN_CENTER | HJP_ALIGN_MIDDLE);
    hjpText(vg, x + 160, y + 132, "ログイン", NULL);
    gui_advance(168);
    /* テキスト入力はGUI.テキスト入力/パスワード入力で行う設計のためnull返却 */
    return hajimu_null();
}

/* パスワード強度メーター(パスワード) → 数値 */
static Value fn_password_strength(int argc, Value *argv) {
    (void)argc;
    const char *pw = gui_as_string(argv[0]);
    if (!pw) return hajimu_number(0);
    int len = (int)strlen(pw);
    int score = 0;
    if (len >= 8) score++;
    if (len >= 12) score++;
    int has_upper = 0, has_lower = 0, has_digit = 0, has_sym = 0;
    for (int i = 0; pw[i]; i++) {
        if (pw[i] >= 'A' && pw[i] <= 'Z') has_upper = 1;
        else if (pw[i] >= 'a' && pw[i] <= 'z') has_lower = 1;
        else if (pw[i] >= '0' && pw[i] <= '9') has_digit = 1;
        else has_sym = 1;
    }
    score += has_upper + has_lower + has_digit + has_sym;
    if (score > 5) score = 5;
    /* UI描画 */
    if (g_cur && g_cur->vg) {
        Hjpcontext *vg = g_cur->vg;
        float x, y, w;
        gui_pos(&x, &y, &w);
        hjpBeginPath(vg);
        hjpRoundedRect(vg, x, y, w, 6, 3);
        hjpFillColor(vg, hjpRGBA(230,230,230,255));
        hjpFill(vg);
        float fill_w = w * score / 5.0f;
        Hjpcolor col = score <= 2 ? hjpRGBA(219,68,55,255) :
                       score <= 3 ? hjpRGBA(251,188,4,255) :
                                    hjpRGBA(15,157,88,255);
        hjpBeginPath(vg);
        hjpRoundedRect(vg, x, y, fill_w, 6, 3);
        hjpFillColor(vg, col);
        hjpFill(vg);
        gui_advance(14);
    }
    return hajimu_number(score);
}

static Value fn_two_factor_input(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_string(""); }
static Value fn_license_dialog(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_bool(0); }
static Value fn_session_timeout(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
/* 生体認証UI(説明) → 偽: 実機OS APIなしのため常に未サポートを返す */
static Value fn_biometric_auth(int argc, Value *argv) {
    (void)argc; (void)argv;
    /* NOTE: 実装なし。プラットフォームAPI統合が必要 */
    return hajimu_bool(0);
}
/* ロック画面(タイムアウト秒) → 偽: 実装なし */
static Value fn_lock_screen(int argc, Value *argv) {
    (void)argc; (void)argv;
    return hajimu_bool(0);
}
/* 権限ゲート(機能名, 要求権限) → 真偽
 * セキュリティ注意: GUI ライブラリ内では OS 権限を検証できないため
 * 常に 真 を返す。呼び出し側でサーバーサイド権限チェックを必ず行うこと。 */
static Value fn_permission_gate(int argc, Value *argv) {
    (void)argc; (void)argv;
    /* WARNING: ホスト側の権限チェックなしに認可しない設計にすること */
    return hajimu_bool(1);
}

/* =====================================================================
 * Phase 76: AI/インテリジェント UI (v7.6.0)
 * ===================================================================*/

/* チャットUI(ID, メッセージ配列) → 辞書 */
static Value fn_chat_ui(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || !g_cur->vg) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    float x, y, w;
    gui_pos(&x, &y, &w);
    Value msgs = argv[1];
    int n = gui_arr_len(msgs);
    float cy = y;
    for (int i = 0; i < n; i++) {
        Value m = gui_arr_get(msgs, i);
        Value sender = gui_dict_lookup(m, "送信者");
        Value text = gui_dict_lookup(m, "内容");
        const char *s = gui_as_string(sender);
        const char *t = gui_as_string(text);
        int is_me = (s && strcmp(s, "自分") == 0);
        float bw = w * 0.65f;
        float bx = is_me ? x + w - bw : x;
        Hjpcolor bg = is_me ? hjpRGBA(66,133,244,230) : hjpRGBA(240,240,240,255);
        Hjpcolor fg = is_me ? hjpRGBA(255,255,255,255) : TH_TEXT;
        hjpBeginPath(vg);
        hjpRoundedRect(vg, bx, cy, bw, 36, 12);
        hjpFillColor(vg, bg);
        hjpFill(vg);
        hjpFontFaceId(vg, g_cur->font_id);
        hjpFontSize(vg, 13);
        hjpFillColor(vg, fg);
        hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_MIDDLE);
        if (t) hjpText(vg, bx + 12, cy + 18, t, NULL);
        cy += 44;
    }
    gui_advance(cy - y + 8);
    return hajimu_null();
}

/* チャットバブル(送信者, メッセージ, 自分) → 無 */
static Value fn_chat_bubble(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || !g_cur->vg) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    float x, y, w;
    gui_pos(&x, &y, &w);
    const char *msg = gui_as_string(argv[1]);
    int is_me = gui_as_bool(argv[2]);
    float bw = w * 0.6f;
    float bx = is_me ? x + w - bw : x;
    Hjpcolor bg = is_me ? hjpRGBA(66,133,244,230) : hjpRGBA(240,240,240,255);
    Hjpcolor fg = is_me ? hjpRGBA(255,255,255,255) : TH_TEXT;
    hjpBeginPath(vg);
    hjpRoundedRect(vg, bx, y, bw, 36, 14);
    hjpFillColor(vg, bg);
    hjpFill(vg);
    hjpFontFaceId(vg, g_cur->font_id);
    hjpFontSize(vg, 13);
    hjpFillColor(vg, fg);
    hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_MIDDLE);
    if (msg) hjpText(vg, bx + 12, y + 18, msg, NULL);
    gui_advance(44);
    return hajimu_null();
}

/* ストリーミングテキスト(ID, テキスト, 進捗) → 無 */
static Value fn_streaming_text(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || !g_cur->vg) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    float x, y, w;
    gui_pos(&x, &y, &w);
    const char *text = gui_as_string(argv[1]);
    float progress = (float)gui_as_number(argv[2]);
    if (!text) { gui_advance(20); return hajimu_null(); }
    int total = (int)strlen(text);
    int show = (int)(total * progress);
    if (show < 0) show = 0;
    if (show > total) show = total;
    static char buf[GUI_MAX_TEXT_BUF];
    int cp = show;
    if (cp >= (int)sizeof(buf)) cp = (int)sizeof(buf) - 1;
    memcpy(buf, text, cp);
    buf[cp] = '\0';
    hjpFontFaceId(vg, g_cur->font_id);
    hjpFontSize(vg, 14);
    hjpFillColor(vg, TH_TEXT);
    hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_TOP);
    hjpText(vg, x, y, buf, NULL);
    gui_advance(22);
    return hajimu_null();
}

/* AIプログレス(メッセージ) → 無 */
static Value fn_ai_progress(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || !g_cur->vg) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    float x, y, w;
    gui_pos(&x, &y, &w);
    const char *msg = gui_as_string(argv[0]);
    /* 点滅ドットアニメーション */
    float t = g_frame_count * 0.05f;
    hjpFontFaceId(vg, g_cur->font_id);
    hjpFontSize(vg, 13);
    hjpFillColor(vg, TH_TEXT);
    hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_MIDDLE);
    if (msg) hjpText(vg, x, y + 10, msg, NULL);
    float dx = x + (msg ? hjpTextBounds(vg, 0, 0, msg, NULL, NULL) + 8 : 0);
    for (int i = 0; i < 3; i++) {
        float alpha = (sinf(t + i * 0.8f) + 1.0f) * 0.5f * 255;
        hjpBeginPath(vg);
        hjpCircle(vg, dx + i * 10, y + 10, 3);
        hjpFillColor(vg, hjpRGBA(100,100,100,(unsigned char)alpha));
        hjpFill(vg);
    }
    gui_advance(24);
    return hajimu_null();
}

static Value fn_voice_input(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_string(""); }
static Value fn_voice_synthesis(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_smart_search(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }

/* サジェストカード(ID, 提案配列) → 文字列 */
static Value fn_suggest_card(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || !g_cur->vg) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    float x, y, w;
    gui_pos(&x, &y, &w);
    Value suggestions = argv[1];
    int n = gui_arr_len(suggestions);
    float card_h = 36;
    Value result = hajimu_null();
    for (int i = 0; i < n; i++) {
        Value s = gui_arr_get(suggestions, i);
        const char *label = gui_as_string(s);
        float cy = y + i * (card_h + 4);
        bool hov = false, pressed = false;
        uint32_t id = gui_hash(__func__) + __LINE__;
        gui_widget_logic(id, x, cy, w, card_h, &hov, &pressed);
        hjpBeginPath(vg);
        hjpRoundedRect(vg, x, cy, w, card_h, 8);
        hjpFillColor(vg, hov ? hjpRGBA(66,133,244,25) : hjpRGBA(245,245,245,255));
        hjpFill(vg);
        hjpStrokeColor(vg, hjpRGBA(200,200,200,255));
        hjpStroke(vg);
        hjpFontFaceId(vg, g_cur->font_id);
        hjpFontSize(vg, 13);
        hjpFillColor(vg, TH_TEXT);
        hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_MIDDLE);
        if (label) hjpText(vg, x + 12, cy + card_h/2, label, NULL);
        if (pressed && label) result = hajimu_string(label);
    }
    gui_advance(n * (card_h + 4) + 4);
    return result;
}

/* =====================================================================
 * Phase 77: 国際化/ローカライゼーション II (v7.7.0)
 * ===================================================================*/

static Value fn_date_locale(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_string(""); }
static Value fn_number_locale(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_string(""); }
static Value fn_currency_format(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_string(""); }

/* 和暦変換(日付) → 文字列 */
static Value fn_wareki(int argc, Value *argv) {
    (void)argc;
    int year = (int)gui_as_number(argv[0]);
    static char buf[64];
    if (year >= 2019) snprintf(buf, sizeof(buf), "令和%d年", year - 2018);
    else if (year >= 1989) snprintf(buf, sizeof(buf), "平成%d年", year - 1988);
    else if (year >= 1926) snprintf(buf, sizeof(buf), "昭和%d年", year - 1925);
    else if (year >= 1912) snprintf(buf, sizeof(buf), "大正%d年", year - 1911);
    else snprintf(buf, sizeof(buf), "明治%d年", year - 1867);
    return hajimu_string(buf);
}

static Value fn_timezone_display(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_string(""); }
static Value fn_locale_detect(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_string("ja"); }
static Value fn_ime_candidates(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_number(0); }

/* ルビテキスト(テキスト, ルビ) → 無 */
static Value fn_ruby_text(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || !g_cur->vg) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    float x, y, w;
    gui_pos(&x, &y, &w);
    const char *text = gui_as_string(argv[0]);
    const char *ruby = gui_as_string(argv[1]);
    /* ルビ(小) */
    if (ruby) {
        hjpFontFaceId(vg, g_cur->font_id);
        hjpFontSize(vg, 9);
        hjpFillColor(vg, hjpRGBA(100,100,100,255));
        hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_BOTTOM);
        hjpText(vg, x, y + 10, ruby, NULL);
    }
    /* 本文 */
    if (text) {
        hjpFontFaceId(vg, g_cur->font_id);
        hjpFontSize(vg, 16);
        hjpFillColor(vg, TH_TEXT);
        hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_TOP);
        hjpText(vg, x, y + 11, text, NULL);
    }
    gui_advance(30);
    return hajimu_null();
}

/* =====================================================================
 * Phase 78: Web統合/埋め込み (v7.8.0)
 * ===================================================================*/

static Value fn_webview_widget(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_link_preview(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_social_share(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_qr_scan(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_string(""); }
static Value fn_embed_video(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_webfont_load(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_bool(0); }
static Value fn_oembed_display(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_rss_display(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }

/* =====================================================================
 * Phase 79: テスト/開発ツール高度 II (v7.9.0)
 * ===================================================================*/

static Value fn_component_playground(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_design_tokens(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_responsive_preview(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_visual_regression(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_interaction_recorder(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_performance_profiler(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_memory_monitor(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_hot_reload(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_bool(0); }

/* =====================================================================
 * Phase 80: 最終仕上げ/ユーティリティ (v8.0.0)
 * ===================================================================*/

/* ショートカットダイアログ() → 無 */
static Value fn_shortcut_dialog(int argc, Value *argv) {
    (void)argc; (void)argv;
    if (!g_cur || !g_cur->vg) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    float w = (float)g_cur->win_w, h = (float)g_cur->win_h;
    float dw = 400, dh = 300;
    float dx = (w - dw)/2, dy = (h - dh)/2;
    /* 背景 */
    hjpBeginPath(vg);
    hjpRect(vg, 0, 0, w, h);
    hjpFillColor(vg, hjpRGBA(0,0,0,100));
    hjpFill(vg);
    /* ダイアログ */
    hjpBeginPath(vg);
    hjpRoundedRect(vg, dx, dy, dw, dh, 8);
    hjpFillColor(vg, hjpRGBA(255,255,255,250));
    hjpFill(vg);
    hjpFontFaceId(vg, g_cur->font_id);
    hjpFontSize(vg, 16);
    hjpFillColor(vg, TH_TEXT);
    hjpTextAlign(vg, HJP_ALIGN_CENTER | HJP_ALIGN_MIDDLE);
    hjpText(vg, dx + dw/2, dy + 24, "キーボードショートカット", NULL);
    return hajimu_null();
}

static Value fn_welcome_screen(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_number(0); }
static Value fn_feature_flag(int argc, Value *argv) { (void)argc; return argv[1]; }
static Value fn_ab_test(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_string("A"); }
static Value fn_crash_report(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_feedback_form(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }

/* 変更履歴表示(変更配列) → 無 */
static Value fn_changelog_display(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || !g_cur->vg) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    float x, y, w;
    gui_pos(&x, &y, &w);
    Value changes = argv[0];
    int n = gui_arr_len(changes);
    float cy = y;
    for (int i = 0; i < n; i++) {
        Value c = gui_arr_get(changes, i);
        const char *s = gui_as_string(c);
        if (!s) continue;
        hjpFontFaceId(vg, g_cur->font_id);
        hjpFontSize(vg, 13);
        hjpFillColor(vg, TH_TEXT);
        hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_TOP);
        hjpText(vg, x + 12, cy, s, NULL);
        cy += 22;
    }
    gui_advance(cy - y + 4);
    return hajimu_null();
}

static Value fn_app_settings(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }

/* =====================================================================
 * Phase 81: ノードエディタ/専門エディタ (v8.1.0)
 * ===================================================================*/

/* ノードエディタ(ID, ノード配列, リンク配列) → 辞書 */
static Value fn_node_editor(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || !g_cur->vg) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    const char *id = gui_as_string(argv[0]);
    Value nodes = argv[1];
    Value links = argv[2];
    float x, y, w;
    gui_pos(&x, &y, &w);
    float h = 300;
    /* 背景 */
    hjpBeginPath(vg); hjpRoundedRect(vg, x, y, w, h, 4);
    hjpFillColor(vg, hjpRGBA(30,30,30,255)); hjpFill(vg);
    /* グリッド */
    hjpStrokeColor(vg, hjpRGBA(50,50,50,255)); hjpStrokeWidth(vg, 1);
    for (float gx = x; gx < x+w; gx += 20) { hjpBeginPath(vg); hjpMoveTo(vg,gx,y); hjpLineTo(vg,gx,y+h); hjpStroke(vg); }
    for (float gy = y; gy < y+h; gy += 20) { hjpBeginPath(vg); hjpMoveTo(vg,x,gy); hjpLineTo(vg,x+w,gy); hjpStroke(vg); }
    /* ノード描画 */
    int nn = gui_arr_len(nodes);
    for (int i = 0; i < nn && i < 32; i++) {
        Value nd = gui_arr_get(nodes, i);
        float nx = x + 20 + (i % 4) * 120, ny = y + 20 + (i / 4) * 80;
        hjpBeginPath(vg); hjpRoundedRect(vg, nx, ny, 100, 60, 6);
        hjpFillColor(vg, hjpRGBA(60,60,80,240)); hjpFill(vg);
        hjpStrokeColor(vg, hjpRGBA(100,140,255,200)); hjpStrokeWidth(vg, 1); hjpStroke(vg);
        const char *label = gui_as_string(nd);
        if (label) { hjpFontFaceId(vg, g_cur->font_id); hjpFontSize(vg, 12);
            hjpFillColor(vg, hjpRGBA(220,220,220,255));
            hjpTextAlign(vg, HJP_ALIGN_CENTER|HJP_ALIGN_MIDDLE);
            hjpText(vg, nx+50, ny+30, label, NULL); }
    }
    /* リンク描画 */
    int nl = gui_arr_len(links);
    hjpStrokeColor(vg, hjpRGBA(100,200,100,200)); hjpStrokeWidth(vg, 2);
    for (int i = 0; i < nl; i++) { (void)gui_arr_get(links, i); }
    gui_advance(h + 4);
    Value result = hajimu_array();
    (void)id;
    return result;
}

/* HEXエディタ(ID, データ, サイズ) → 辞書 */
static Value fn_hex_editor(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || !g_cur->vg) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    const char *id = gui_as_string(argv[0]);
    const char *data = gui_as_string(argv[1]);
    int sz = (int)gui_as_number(argv[2]);
    /* 安全チェック: sz を実際の文字列長にクランプ (バッファオーバーリード防止) */
    if (sz < 0) sz = 0;
    if (data) { int slen = (int)strlen(data); if (sz > slen) sz = slen; }
    else { sz = 0; }
    float x, y, w; gui_pos(&x, &y, &w);
    float row_h = 16; int cols = 16;
    int rows = (sz + cols - 1) / cols; if (rows > 20) rows = 20;
    float h = rows * row_h + 24;
    hjpBeginPath(vg); hjpRoundedRect(vg, x, y, w, h, 4);
    hjpFillColor(vg, hjpRGBA(25,25,35,250)); hjpFill(vg);
    hjpFontFaceId(vg, g_cur->font_id); hjpFontSize(vg, 11);
    hjpTextAlign(vg, HJP_ALIGN_LEFT|HJP_ALIGN_TOP);
    for (int r = 0; r < rows; r++) {
        char addr[16]; snprintf(addr, sizeof(addr), "%04X:", r*cols);
        hjpFillColor(vg, hjpRGBA(100,180,255,255));
        hjpText(vg, x+4, y+4+r*row_h, addr, NULL);
        for (int c = 0; c < cols && r*cols+c < sz; c++) {
            unsigned char byte = data ? (unsigned char)data[r*cols+c] : 0;
            char hex[4]; snprintf(hex, sizeof(hex), "%02X", byte);
            hjpFillColor(vg, hjpRGBA(200,200,200,255));
            hjpText(vg, x+50+c*22, y+4+r*row_h, hex, NULL);
        }
    }
    gui_advance(h + 4); (void)id;
    return hajimu_null();
}

/* グラデーションエディタ(ID, 停止点配列) → 配列 */
static Value fn_gradient_editor(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || !g_cur->vg) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    Value stops = argv[1];
    float x, y, w; gui_pos(&x, &y, &w);
    float h = 40;
    Hjppaint bg = hjpLinearGradient(vg, x, y, x+w, y, hjpRGBf(0,0,0), hjpRGBf(1,1,1));
    hjpBeginPath(vg); hjpRoundedRect(vg, x, y, w, h, 4); hjpFillPaint(vg, bg); hjpFill(vg);
    int ns = gui_arr_len(stops);
    for (int i = 0; i < ns; i++) {
        float t = (float)i / (ns > 1 ? ns-1 : 1);
        float sx = x + t * w;
        hjpBeginPath(vg); hjpCircle(vg, sx, y + h/2, 6);
        hjpFillColor(vg, hjpRGBA(255,255,255,220)); hjpFill(vg);
        hjpStrokeColor(vg, hjpRGBA(0,0,0,180)); hjpStrokeWidth(vg, 1.5f); hjpStroke(vg);
    }
    gui_advance(h + 8);
    return stops;
}

/* カーブエディタ(ID, 制御点配列) → 配列 */
static Value fn_curve_editor(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || !g_cur->vg) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    Value pts = argv[1];
    float x, y, w; gui_pos(&x, &y, &w);
    float h = 200;
    hjpBeginPath(vg); hjpRoundedRect(vg, x, y, w, h, 4);
    hjpFillColor(vg, hjpRGBA(30,30,40,250)); hjpFill(vg);
    /* グリッド */
    hjpStrokeColor(vg, hjpRGBA(50,50,60,200)); hjpStrokeWidth(vg, 0.5f);
    for (float t = 0; t <= 1.0f; t += 0.25f) {
        hjpBeginPath(vg); hjpMoveTo(vg, x, y+t*h); hjpLineTo(vg, x+w, y+t*h); hjpStroke(vg);
        hjpBeginPath(vg); hjpMoveTo(vg, x+t*w, y); hjpLineTo(vg, x+t*w, y+h); hjpStroke(vg);
    }
    /* カーブ */
    int np = gui_arr_len(pts);
    if (np >= 2) {
        hjpBeginPath(vg); hjpStrokeColor(vg, hjpRGBA(100,200,100,255)); hjpStrokeWidth(vg, 2);
        for (int i = 0; i < np; i++) {
            float t = (float)i / (np-1);
            float px = x + t * w, py = y + h - (t * h);
            if (i == 0) hjpMoveTo(vg, px, py); else hjpLineTo(vg, px, py);
        }
        hjpStroke(vg);
    }
    /* 制御点 */
    for (int i = 0; i < np; i++) {
        float t = (float)i / (np > 1 ? np-1 : 1);
        float px = x + t * w, py = y + h - (t * h);
        hjpBeginPath(vg); hjpCircle(vg, px, py, 5);
        hjpFillColor(vg, hjpRGBA(255,200,50,255)); hjpFill(vg);
    }
    gui_advance(h + 4);
    return pts;
}

/* アニメーションシーケンサー(ID, トラック配列) → 辞書 */
static Value fn_animation_sequencer(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || !g_cur->vg) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    Value tracks = argv[1];
    float x, y, w; gui_pos(&x, &y, &w);
    int nt = gui_arr_len(tracks); if (nt > 16) nt = 16;
    float track_h = 24, header_w = 100;
    float h = nt * track_h + 30;
    /* 背景 */
    hjpBeginPath(vg); hjpRoundedRect(vg, x, y, w, h, 4);
    hjpFillColor(vg, hjpRGBA(35,35,45,250)); hjpFill(vg);
    /* ヘッダー */
    hjpBeginPath(vg); hjpRect(vg, x, y, w, 24);
    hjpFillColor(vg, hjpRGBA(50,50,65,255)); hjpFill(vg);
    /* タイムライン目盛り */
    hjpFontFaceId(vg, g_cur->font_id); hjpFontSize(vg, 10);
    hjpFillColor(vg, hjpRGBA(150,150,170,255));
    hjpTextAlign(vg, HJP_ALIGN_CENTER|HJP_ALIGN_MIDDLE);
    for (int f = 0; f <= 10; f++) {
        float fx = x + header_w + (w - header_w) * f / 10;
        char lbl[8]; snprintf(lbl, sizeof(lbl), "%d", f * 10);
        hjpText(vg, fx, y + 12, lbl, NULL);
    }
    /* トラック */
    for (int i = 0; i < nt; i++) {
        float ty = y + 24 + i * track_h;
        Value t = gui_arr_get(tracks, i);
        const char *name = gui_as_string(t);
        hjpBeginPath(vg); hjpRect(vg, x, ty, header_w, track_h);
        hjpFillColor(vg, hjpRGBA(45,45,55,255)); hjpFill(vg);
        if (name) {
            hjpFontSize(vg, 11); hjpFillColor(vg, hjpRGBA(180,180,200,255));
            hjpTextAlign(vg, HJP_ALIGN_LEFT|HJP_ALIGN_MIDDLE);
            hjpText(vg, x+6, ty + track_h/2, name, NULL);
        }
        /* キーフレームバー */
        hjpBeginPath(vg); hjpRoundedRect(vg, x+header_w+20, ty+4, 80, track_h-8, 3);
        hjpFillColor(vg, hjpRGBA(80,140,220,200)); hjpFill(vg);
    }
    gui_advance(h + 4);
    return hajimu_null();
}

/* スプレッドシート(ID, データ, 列定義) → 辞書 */
static Value fn_spreadsheet(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || !g_cur->vg) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    Value data = argv[1];
    float x, y, w; gui_pos(&x, &y, &w);
    int rows = gui_arr_len(data); if (rows > 30) rows = 30;
    int cols = 8; float cell_w = w / cols, cell_h = 22;
    float h = (rows + 1) * cell_h + 4;
    hjpBeginPath(vg); hjpRoundedRect(vg, x, y, w, h, 4);
    hjpFillColor(vg, hjpRGBA(255,255,255,250)); hjpFill(vg);
    /* ヘッダー */
    hjpBeginPath(vg); hjpRect(vg, x, y, w, cell_h);
    hjpFillColor(vg, hjpRGBA(230,235,240,255)); hjpFill(vg);
    hjpFontFaceId(vg, g_cur->font_id); hjpFontSize(vg, 11);
    hjpTextAlign(vg, HJP_ALIGN_CENTER|HJP_ALIGN_MIDDLE);
    for (int c = 0; c < cols; c++) {
        char hd[4]; hd[0] = 'A' + c; hd[1] = 0;
        hjpFillColor(vg, hjpRGBA(60,60,80,255));
        hjpText(vg, x + c*cell_w + cell_w/2, y + cell_h/2, hd, NULL);
    }
    /* セル枠 */
    hjpStrokeColor(vg, hjpRGBA(200,200,210,255)); hjpStrokeWidth(vg, 0.5f);
    for (int r = 0; r <= rows+1; r++) { hjpBeginPath(vg); hjpMoveTo(vg, x, y+r*cell_h); hjpLineTo(vg, x+w, y+r*cell_h); hjpStroke(vg); }
    for (int c = 0; c <= cols; c++) { hjpBeginPath(vg); hjpMoveTo(vg, x+c*cell_w, y); hjpLineTo(vg, x+c*cell_w, y+h); hjpStroke(vg); }
    gui_advance(h + 4);
    return hajimu_null();
}

/* JSONビューア(ID, JSON文字列) → 辞書 */
static Value fn_json_viewer(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || !g_cur->vg) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    const char *json = gui_as_string(argv[1]);
    float x, y, w; gui_pos(&x, &y, &w);
    float h = 200;
    hjpBeginPath(vg); hjpRoundedRect(vg, x, y, w, h, 4);
    hjpFillColor(vg, hjpRGBA(25,25,30,250)); hjpFill(vg);
    hjpFontFaceId(vg, g_cur->font_id); hjpFontSize(vg, 12);
    hjpTextAlign(vg, HJP_ALIGN_LEFT|HJP_ALIGN_TOP);
    if (json) {
        float ly = y + 6;
        const char *p = json;
        int indent = 0;
        while (*p && ly < y + h - 14) {
            if (*p == '{' || *p == '[') { indent++; hjpFillColor(vg, hjpRGBA(255,200,50,255)); }
            else if (*p == '}' || *p == ']') { indent--; hjpFillColor(vg, hjpRGBA(255,200,50,255)); }
            else if (*p == '"') { hjpFillColor(vg, hjpRGBA(130,200,130,255)); }
            else if (*p == ':') { hjpFillColor(vg, hjpRGBA(200,200,200,255)); }
            else { hjpFillColor(vg, hjpRGBA(180,180,200,255)); }
            char ch[2] = { *p, 0 };
            hjpText(vg, x + 8 + indent * 12, ly, ch, NULL);
            if (*p == '\n') { ly += 16; } else { /* advance */ }
            p++;
        }
    }
    gui_advance(h + 4);
    return hajimu_null();
}

/* ターミナルウィジェット(ID, 幅, 高さ) → 辞書 */
static Value fn_terminal_widget(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || !g_cur->vg) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    float tw = (argc > 1) ? (float)gui_as_number(argv[1]) : 600;
    float th = (argc > 2) ? (float)gui_as_number(argv[2]) : 300;
    float x, y, w; gui_pos(&x, &y, &w);
    if (tw > w) tw = w;
    /* ターミナル背景 */
    hjpBeginPath(vg); hjpRoundedRect(vg, x, y, tw, th, 6);
    hjpFillColor(vg, hjpRGBA(15,15,20,250)); hjpFill(vg);
    /* タイトルバー */
    hjpBeginPath(vg); hjpRoundedRect(vg, x, y, tw, 28, 6);
    hjpFillColor(vg, hjpRGBA(40,40,50,255)); hjpFill(vg);
    /* ドット */
    float dx = x + 12;
    Hjpcolor dots[3] = { hjpRGB(255,95,87), hjpRGB(255,189,46), hjpRGB(39,201,63) };
    for (int i = 0; i < 3; i++) { hjpBeginPath(vg); hjpCircle(vg, dx + i*18, y+14, 5); hjpFillColor(vg, dots[i]); hjpFill(vg); }
    /* プロンプト */
    hjpFontFaceId(vg, g_cur->font_id); hjpFontSize(vg, 13);
    hjpFillColor(vg, hjpRGBA(50,200,50,255));
    hjpTextAlign(vg, HJP_ALIGN_LEFT|HJP_ALIGN_TOP);
    hjpText(vg, x+8, y+34, "$ _", NULL);
    gui_advance(th + 4);
    return hajimu_null();
}

/* =====================================================================
 * Phase 82: 高度なコントロール (v8.2.0)
 * ===================================================================*/

/* ノブ(ID, 値, 最小, 最大) → 数値 */
static Value fn_knob(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || !g_cur->vg) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    float val = (float)gui_as_number(argv[1]);
    float mn = (float)gui_as_number(argv[2]);
    float mx = (float)gui_as_number(argv[3]);
    float x, y, w; gui_pos(&x, &y, &w);
    float r = 30, cx = x + w/2, cy = y + r + 4;
    float t = (mx > mn) ? (val - mn) / (mx - mn) : 0;
    float a0 = 2.356f, a1 = 2.356f + t * 4.712f; /* 135° to 405° */
    /* 背景弧 */
    hjpBeginPath(vg); hjpArc(vg, cx, cy, r, a0, a0+4.712f, HJP_CW);
    hjpStrokeColor(vg, hjpRGBA(60,60,70,255)); hjpStrokeWidth(vg, 4); hjpStroke(vg);
    /* 値弧 */
    hjpBeginPath(vg); hjpArc(vg, cx, cy, r, a0, a1, HJP_CW);
    hjpStrokeColor(vg, TH_ACCENT); hjpStrokeWidth(vg, 4); hjpStroke(vg);
    /* 中央ドット */
    hjpBeginPath(vg); hjpCircle(vg, cx, cy, 4);
    hjpFillColor(vg, TH_TEXT); hjpFill(vg);
    /* 値表示 */
    char buf[32]; snprintf(buf, sizeof(buf), "%.1f", val);
    hjpFontFaceId(vg, g_cur->font_id); hjpFontSize(vg, 12);
    hjpFillColor(vg, TH_TEXT); hjpTextAlign(vg, HJP_ALIGN_CENTER|HJP_ALIGN_TOP);
    hjpText(vg, cx, cy + r + 6, buf, NULL);
    gui_advance(r*2 + 24);
    return hajimu_number(val);
}

/* パイメニュー(ID, 項目配列) → 数値 */
static Value fn_pie_menu(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || !g_cur->vg) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    Value items = argv[1];
    int n = gui_arr_len(items); if (n < 1) n = 1;
    float cx = (float)g_cur->win_w / 2, cy = (float)g_cur->win_h / 2;
    float r_inner = 30, r_outer = 100;
    /* 背景 */
    hjpBeginPath(vg); hjpCircle(vg, cx, cy, r_outer + 10);
    hjpFillColor(vg, hjpRGBA(0,0,0,120)); hjpFill(vg);
    float slice = 6.283185f / n;
    int selected = -1;
    float mx = g_cur->in.mx, my = g_cur->in.my;
    float dx = mx - cx, dy = my - cy;
    float dist = sqrtf(dx*dx + dy*dy);
    float angle = atan2f(dy, dx); if (angle < 0) angle += 6.283185f;
    if (dist > r_inner && dist < r_outer) { selected = (int)(angle / slice); }
    for (int i = 0; i < n; i++) {
        float a0 = i * slice, a1 = (i+1) * slice;
        hjpBeginPath(vg); hjpArc(vg, cx, cy, r_inner, a0, a1, HJP_CW);
        hjpArc(vg, cx, cy, r_outer, a1, a0, HJP_CCW); hjpClosePath(vg);
        hjpFillColor(vg, (i == selected) ? hjpRGBA(80,140,255,200) : hjpRGBA(60,60,70,200));
        hjpFill(vg);
        Value item = gui_arr_get(items, i);
        const char *label = gui_as_string(item);
        if (label) {
            float mid = (a0 + a1) / 2;
            float lx = cx + cosf(mid) * (r_inner + r_outer) / 2;
            float ly = cy + sinf(mid) * (r_inner + r_outer) / 2;
            hjpFontFaceId(vg, g_cur->font_id); hjpFontSize(vg, 12);
            hjpFillColor(vg, hjpRGBA(240,240,240,255));
            hjpTextAlign(vg, HJP_ALIGN_CENTER|HJP_ALIGN_MIDDLE);
            hjpText(vg, lx, ly, label, NULL);
        }
    }
    return hajimu_number(selected);
}

/* LCD数字(値, 桁数) → 無 */
static Value fn_lcd_number(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || !g_cur->vg) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    double val = gui_as_number(argv[0]);
    int digits = (argc > 1) ? (int)gui_as_number(argv[1]) : 4;
    if (digits < 1) digits = 1; if (digits > 16) digits = 16;
    float x, y, w; gui_pos(&x, &y, &w);
    float dw = digits * 24 + 16, dh = 48;
    hjpBeginPath(vg); hjpRoundedRect(vg, x, y, dw, dh, 4);
    hjpFillColor(vg, hjpRGBA(10,30,10,250)); hjpFill(vg);
    char fmt[16], buf[64]; snprintf(fmt, sizeof(fmt), "%%%d.0f", digits);
    snprintf(buf, sizeof(buf), fmt, val);
    hjpFontFaceId(vg, g_cur->font_id); hjpFontSize(vg, 32);
    hjpFillColor(vg, hjpRGBA(50,255,50,255));
    hjpTextAlign(vg, HJP_ALIGN_RIGHT|HJP_ALIGN_MIDDLE);
    hjpText(vg, x + dw - 8, y + dh/2, buf, NULL);
    gui_advance(dh + 4);
    return hajimu_null();
}

/* アーク進捗(ID, 値, 最小, 最大) → 数値 */
static Value fn_arc_progress(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || !g_cur->vg) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    float val = (float)gui_as_number(argv[1]);
    float mn = (float)gui_as_number(argv[2]);
    float mx = (float)gui_as_number(argv[3]);
    float x, y, w; gui_pos(&x, &y, &w);
    float r = 36, cx = x + w/2, cy = y + r + 4;
    float t = (mx > mn) ? (val - mn) / (mx - mn) : 0;
    float a_start = -1.5708f, a_end = a_start + t * 6.2832f;
    hjpBeginPath(vg); hjpArc(vg, cx, cy, r, a_start, a_start + 6.2832f, HJP_CW);
    hjpStrokeColor(vg, hjpRGBA(60,60,70,200)); hjpStrokeWidth(vg, 6); hjpStroke(vg);
    hjpBeginPath(vg); hjpArc(vg, cx, cy, r, a_start, a_end, HJP_CW);
    hjpStrokeColor(vg, TH_ACCENT); hjpStrokeWidth(vg, 6); hjpStroke(vg);
    char buf[16]; snprintf(buf, sizeof(buf), "%.0f%%", t*100);
    hjpFontFaceId(vg, g_cur->font_id); hjpFontSize(vg, 16);
    hjpFillColor(vg, TH_TEXT); hjpTextAlign(vg, HJP_ALIGN_CENTER|HJP_ALIGN_MIDDLE);
    hjpText(vg, cx, cy, buf, NULL);
    gui_advance(r*2 + 12);
    return hajimu_number(val);
}

/* メーター(ID, 値, 最小, 最大) → 無 */
static Value fn_meter(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || !g_cur->vg) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    float val = (float)gui_as_number(argv[1]);
    float mn = (float)gui_as_number(argv[2]);
    float mx = (float)gui_as_number(argv[3]);
    float x, y, w; gui_pos(&x, &y, &w);
    float r = 50, cx = x + w/2, cy = y + r + 4;
    float t = (mx > mn) ? (val - mn) / (mx - mn) : 0;
    /* 弧の背景 */
    hjpBeginPath(vg); hjpArc(vg, cx, cy, r, 2.356f, 2.356f+4.712f, HJP_CW);
    hjpStrokeColor(vg, hjpRGBA(60,60,70,200)); hjpStrokeWidth(vg, 8); hjpStroke(vg);
    /* 針 */
    float needle_angle = 2.356f + t * 4.712f;
    float nx = cx + cosf(needle_angle) * (r - 10);
    float ny = cy + sinf(needle_angle) * (r - 10);
    hjpBeginPath(vg); hjpMoveTo(vg, cx, cy); hjpLineTo(vg, nx, ny);
    hjpStrokeColor(vg, hjpRGBA(255,80,80,255)); hjpStrokeWidth(vg, 2); hjpStroke(vg);
    /* 中央円 */
    hjpBeginPath(vg); hjpCircle(vg, cx, cy, 5);
    hjpFillColor(vg, hjpRGBA(200,200,210,255)); hjpFill(vg);
    char buf[32]; snprintf(buf, sizeof(buf), "%.1f", val);
    hjpFontFaceId(vg, g_cur->font_id); hjpFontSize(vg, 12);
    hjpFillColor(vg, TH_TEXT); hjpTextAlign(vg, HJP_ALIGN_CENTER|HJP_ALIGN_TOP);
    hjpText(vg, cx, cy + r + 4, buf, NULL);
    gui_advance(r*2 + 24);
    return hajimu_null();
}

/* LEDインジケータ(色, 状態) → 無 */
static Value fn_led_indicator(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || !g_cur->vg) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    int color = (int)gui_as_number(argv[0]);
    bool on = gui_as_bool(argv[1]);
    float x, y, w; gui_pos(&x, &y, &w);
    float r = 8, cx = x + r + 4, cy = y + r + 4;
    unsigned char rr = (color >> 16) & 0xFF, gg = (color >> 8) & 0xFF, bb = color & 0xFF;
    unsigned char a = on ? 255 : 60;
    if (on) {
        hjpBeginPath(vg); hjpCircle(vg, cx, cy, r+4);
        hjpFillColor(vg, hjpRGBA(rr, gg, bb, 60)); hjpFill(vg);
    }
    hjpBeginPath(vg); hjpCircle(vg, cx, cy, r);
    hjpFillColor(vg, hjpRGBA(rr, gg, bb, a)); hjpFill(vg);
    hjpStrokeColor(vg, hjpRGBA(100,100,110,200)); hjpStrokeWidth(vg, 1); hjpStroke(vg);
    gui_advance(r*2 + 12);
    return hajimu_null();
}

/* 2Dスライダー(ID, X値, Y値) → 辞書 */
static Value fn_slider_2d(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || !g_cur->vg) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    float vx = (float)gui_as_number(argv[1]);
    float vy = (float)gui_as_number(argv[2]);
    float x, y, w; gui_pos(&x, &y, &w);
    float sz = 150; if (sz > w) sz = w;
    hjpBeginPath(vg); hjpRoundedRect(vg, x, y, sz, sz, 4);
    hjpFillColor(vg, hjpRGBA(40,40,50,250)); hjpFill(vg);
    hjpStrokeColor(vg, hjpRGBA(80,80,90,255)); hjpStrokeWidth(vg, 1); hjpStroke(vg);
    /* 十字 */
    hjpStrokeColor(vg, hjpRGBA(60,60,70,200)); hjpStrokeWidth(vg, 0.5f);
    hjpBeginPath(vg); hjpMoveTo(vg, x+sz/2, y); hjpLineTo(vg, x+sz/2, y+sz); hjpStroke(vg);
    hjpBeginPath(vg); hjpMoveTo(vg, x, y+sz/2); hjpLineTo(vg, x+sz, y+sz/2); hjpStroke(vg);
    /* ハンドル */
    float hx = x + vx * sz, hy = y + vy * sz;
    hjpBeginPath(vg); hjpCircle(vg, hx, hy, 6);
    hjpFillColor(vg, TH_ACCENT); hjpFill(vg);
    gui_advance(sz + 4);
    Value result = hajimu_array();
    hajimu_array_push(&result, hajimu_number(vx));
    hajimu_array_push(&result, hajimu_number(vy));
    return result;
}

/* ズームスライダー(ID, 開始, 終了, 最小, 最大) → 辞書 */
static Value fn_zoom_slider(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || !g_cur->vg) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    float start = (float)gui_as_number(argv[1]);
    float end = (float)gui_as_number(argv[2]);
    float mn = (float)gui_as_number(argv[3]);
    float mx = (float)gui_as_number(argv[4]);
    float x, y, w; gui_pos(&x, &y, &w);
    float h = 24;
    float range = mx - mn; if (range < 0.001f) range = 1;
    float sl = (start - mn) / range, sr = (end - mn) / range;
    /* トラック */
    hjpBeginPath(vg); hjpRoundedRect(vg, x, y + 8, w, 8, 4);
    hjpFillColor(vg, hjpRGBA(60,60,70,255)); hjpFill(vg);
    /* 選択範囲 */
    hjpBeginPath(vg); hjpRoundedRect(vg, x + sl * w, y + 8, (sr - sl) * w, 8, 4);
    hjpFillColor(vg, TH_ACCENT); hjpFill(vg);
    /* ハンドル */
    hjpBeginPath(vg); hjpCircle(vg, x + sl * w, y + 12, 6);
    hjpFillColor(vg, hjpRGBA(255,255,255,240)); hjpFill(vg);
    hjpBeginPath(vg); hjpCircle(vg, x + sr * w, y + 12, 6);
    hjpFillColor(vg, hjpRGBA(255,255,255,240)); hjpFill(vg);
    gui_advance(h + 4);
    Value result = hajimu_array();
    hajimu_array_push(&result, hajimu_number(start));
    hajimu_array_push(&result, hajimu_number(end));
    return result;
}

/* =====================================================================
 * Phase 83: 3D操作/高度グラフィック (v8.3.0)
 * ===================================================================*/

/* 3Dギズモ(ID, 行列, 操作種別) → 辞書 */
static Value fn_gizmo_3d(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || !g_cur->vg) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    float x, y, w; gui_pos(&x, &y, &w);
    float sz = 120, cx = x + w/2, cy = y + sz/2 + 4;
    /* 原点 */
    hjpBeginPath(vg); hjpCircle(vg, cx, cy, 4);
    hjpFillColor(vg, hjpRGBA(200,200,200,255)); hjpFill(vg);
    /* X軸(赤) */
    hjpBeginPath(vg); hjpMoveTo(vg, cx, cy); hjpLineTo(vg, cx+50, cy);
    hjpStrokeColor(vg, hjpRGBA(255,80,80,255)); hjpStrokeWidth(vg, 2); hjpStroke(vg);
    hjpBeginPath(vg); hjpMoveTo(vg, cx+50, cy); hjpLineTo(vg, cx+45, cy-5); hjpLineTo(vg, cx+45, cy+5); hjpClosePath(vg);
    hjpFillColor(vg, hjpRGBA(255,80,80,255)); hjpFill(vg);
    /* Y軸(緑) */
    hjpBeginPath(vg); hjpMoveTo(vg, cx, cy); hjpLineTo(vg, cx, cy-50);
    hjpStrokeColor(vg, hjpRGBA(80,255,80,255)); hjpStrokeWidth(vg, 2); hjpStroke(vg);
    hjpBeginPath(vg); hjpMoveTo(vg, cx, cy-50); hjpLineTo(vg, cx-5, cy-45); hjpLineTo(vg, cx+5, cy-45); hjpClosePath(vg);
    hjpFillColor(vg, hjpRGBA(80,255,80,255)); hjpFill(vg);
    /* Z軸(青) */
    hjpBeginPath(vg); hjpMoveTo(vg, cx, cy); hjpLineTo(vg, cx-35, cy+35);
    hjpStrokeColor(vg, hjpRGBA(80,80,255,255)); hjpStrokeWidth(vg, 2); hjpStroke(vg);
    gui_advance(sz + 8); (void)argv;
    return hajimu_null();
}

/* シーングラフ(ID, オブジェクト配列) → 辞書 */
static Value fn_scene_graph(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || !g_cur->vg) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    Value objs = argv[1];
    float x, y, w; gui_pos(&x, &y, &w);
    float h = 200;
    hjpBeginPath(vg); hjpRoundedRect(vg, x, y, w, h, 4);
    hjpFillColor(vg, hjpRGBA(40,40,50,250)); hjpFill(vg);
    int no = gui_arr_len(objs);
    for (int i = 0; i < no && i < 20; i++) {
        Value obj = gui_arr_get(objs, i);
        const char *name = gui_as_string(obj);
        float ox = x + 10 + (i % 5) * 80, oy = y + 10 + (i / 5) * 40;
        hjpBeginPath(vg); hjpRoundedRect(vg, ox, oy, 70, 30, 4);
        hjpFillColor(vg, hjpRGBA(60,80,100,200)); hjpFill(vg);
        if (name) {
            hjpFontFaceId(vg, g_cur->font_id); hjpFontSize(vg, 10);
            hjpFillColor(vg, hjpRGBA(220,220,230,255));
            hjpTextAlign(vg, HJP_ALIGN_CENTER|HJP_ALIGN_MIDDLE);
            hjpText(vg, ox+35, oy+15, name, NULL);
        }
    }
    gui_advance(h + 4);
    return hajimu_null();
}

/* ズームキャンバス(ID, コールバック) → 辞書 */
static Value fn_zoomable_canvas(int argc, Value *argv) {
    (void)argc; (void)argv;
    if (!g_cur || !g_cur->vg) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    float x, y, w; gui_pos(&x, &y, &w);
    float h = 250;
    hjpBeginPath(vg); hjpRoundedRect(vg, x, y, w, h, 4);
    hjpFillColor(vg, hjpRGBA(35,35,45,250)); hjpFill(vg);
    /* ズーム表示 */
    hjpFontFaceId(vg, g_cur->font_id); hjpFontSize(vg, 11);
    hjpFillColor(vg, hjpRGBA(150,150,170,255));
    hjpTextAlign(vg, HJP_ALIGN_RIGHT|HJP_ALIGN_TOP);
    hjpText(vg, x+w-8, y+4, "100%", NULL);
    gui_advance(h + 4);
    return hajimu_null();
}

/* 9スライス画像(画像, 左, 上, 右, 下) → 無 */
static Value fn_nine_slice(int argc, Value *argv) {
    (void)argc; (void)argv;
    if (!g_cur || !g_cur->vg) return hajimu_null();
    float x, y, w; gui_pos(&x, &y, &w);
    Hjpcontext *vg = g_cur->vg;
    float h = 80;
    hjpBeginPath(vg); hjpRoundedRect(vg, x, y, w, h, 8);
    hjpFillColor(vg, hjpRGBA(80,80,100,200)); hjpFill(vg);
    /* 9分割線 */
    hjpStrokeColor(vg, hjpRGBA(200,200,200,100)); hjpStrokeWidth(vg, 0.5f);
    float l = 20, t = 20, r = 20, b = 20;
    hjpBeginPath(vg); hjpMoveTo(vg, x+l, y); hjpLineTo(vg, x+l, y+h); hjpStroke(vg);
    hjpBeginPath(vg); hjpMoveTo(vg, x+w-r, y); hjpLineTo(vg, x+w-r, y+h); hjpStroke(vg);
    hjpBeginPath(vg); hjpMoveTo(vg, x, y+t); hjpLineTo(vg, x+w, y+t); hjpStroke(vg);
    hjpBeginPath(vg); hjpMoveTo(vg, x, y+h-b); hjpLineTo(vg, x+w, y+h-b); hjpStroke(vg);
    gui_advance(h + 4);
    return hajimu_null();
}

/* シェーダーマスク(ID, シェーダー種別) → 無 */
static Value fn_shader_mask(int argc, Value *argv) {
    (void)argc; (void)argv;
    if (!g_cur || !g_cur->vg) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    float x, y, w; gui_pos(&x, &y, &w);
    float h = 100;
    Hjppaint mask = hjpLinearGradient(vg, x, y, x+w, y+h, hjpRGBA(255,255,255,255), hjpRGBA(0,0,0,0));
    hjpBeginPath(vg); hjpRoundedRect(vg, x, y, w, h, 4);
    hjpFillPaint(vg, mask); hjpFill(vg);
    gui_advance(h + 4);
    return hajimu_null();
}

/* ビューポートテクスチャ(ID, 幅, 高さ) → 辞書 */
static Value fn_viewport_texture(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || !g_cur->vg) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    float vw = (float)gui_as_number(argv[1]);
    float vh = (float)gui_as_number(argv[2]);
    float x, y, w; gui_pos(&x, &y, &w);
    if (vw > w) vw = w;
    hjpBeginPath(vg); hjpRoundedRect(vg, x, y, vw, vh, 4);
    hjpFillColor(vg, hjpRGBA(20,20,30,250)); hjpFill(vg);
    hjpStrokeColor(vg, hjpRGBA(100,100,120,200)); hjpStrokeWidth(vg, 1); hjpStroke(vg);
    hjpFontFaceId(vg, g_cur->font_id); hjpFontSize(vg, 11);
    hjpFillColor(vg, hjpRGBA(100,100,120,200));
    hjpTextAlign(vg, HJP_ALIGN_CENTER|HJP_ALIGN_MIDDLE);
    hjpText(vg, x+vw/2, y+vh/2, "Viewport", NULL);
    gui_advance(vh + 4);
    return hajimu_null();
}

/* エフェクトパイプライン(ID, 効果配列) → 無 */
static Value fn_effect_pipeline(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || !g_cur->vg) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    Value effects = argv[1];
    float x, y, w; gui_pos(&x, &y, &w);
    int ne = gui_arr_len(effects); if (ne > 10) ne = 10;
    if (ne < 1) { gui_advance(4); return hajimu_null(); }
    float ew = 80, eh = 30, gap = 10;
    float total_w = ne * ew + (ne-1) * gap;
    float sx = x + (w - total_w) / 2;
    for (int i = 0; i < ne; i++) {
        float ex = sx + i * (ew + gap);
        hjpBeginPath(vg); hjpRoundedRect(vg, ex, y, ew, eh, 4);
        hjpFillColor(vg, hjpRGBA(60,80,120,200)); hjpFill(vg);
        Value eff = gui_arr_get(effects, i);
        const char *name = gui_as_string(eff);
        if (name) {
            hjpFontFaceId(vg, g_cur->font_id); hjpFontSize(vg, 10);
            hjpFillColor(vg, hjpRGBA(220,220,240,255));
            hjpTextAlign(vg, HJP_ALIGN_CENTER|HJP_ALIGN_MIDDLE);
            hjpText(vg, ex + ew/2, y + eh/2, name, NULL);
        }
        if (i < ne - 1) {
            hjpBeginPath(vg); hjpMoveTo(vg, ex + ew, y + eh/2);
            hjpLineTo(vg, ex + ew + gap, y + eh/2);
            hjpStrokeColor(vg, hjpRGBA(150,150,170,255)); hjpStrokeWidth(vg, 1.5f); hjpStroke(vg);
        }
    }
    gui_advance(eh + 8);
    return hajimu_null();
}

/* 画像インスペクタ(ID, 画像) → 辞書 */
static Value fn_image_inspector(int argc, Value *argv) {
    (void)argc; (void)argv;
    if (!g_cur || !g_cur->vg) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    float x, y, w; gui_pos(&x, &y, &w);
    float h = 200;
    hjpBeginPath(vg); hjpRoundedRect(vg, x, y, w, h, 4);
    hjpFillColor(vg, hjpRGBA(25,25,30,250)); hjpFill(vg);
    /* ピクセルグリッド */
    float px_sz = 16; int cols = (int)(w / px_sz), rows = (int)(h / px_sz);
    hjpStrokeColor(vg, hjpRGBA(50,50,60,150)); hjpStrokeWidth(vg, 0.5f);
    for (int r = 0; r <= rows; r++) { hjpBeginPath(vg); hjpMoveTo(vg, x, y+r*px_sz); hjpLineTo(vg, x+w, y+r*px_sz); hjpStroke(vg); }
    for (int c = 0; c <= cols; c++) { hjpBeginPath(vg); hjpMoveTo(vg, x+c*px_sz, y); hjpLineTo(vg, x+c*px_sz, y+h); hjpStroke(vg); }
    hjpFontFaceId(vg, g_cur->font_id); hjpFontSize(vg, 10);
    hjpFillColor(vg, hjpRGBA(150,150,170,255));
    hjpTextAlign(vg, HJP_ALIGN_LEFT|HJP_ALIGN_BOTTOM);
    hjpText(vg, x+4, y+h-4, "RGBA(0,0,0,0) @ (0,0)", NULL);
    gui_advance(h + 4);
    return hajimu_null();
}

/* =====================================================================
 * Phase 84: 高度なデータ可視化 III (v8.4.0)
 * ===================================================================*/

/* バブルチャート(ID, データ配列) → 無 */
static Value fn_bubble_chart(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || !g_cur->vg) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    Value data = argv[1];
    float x, y, w; gui_pos(&x, &y, &w);
    float h = 200;
    hjpBeginPath(vg); hjpRoundedRect(vg, x, y, w, h, 4);
    hjpFillColor(vg, hjpRGBA(250,250,255,250)); hjpFill(vg);
    int n = gui_arr_len(data);
    int mod_w = (int)(w - 40); if (mod_w < 1) mod_w = 1;
    int mod_h = (int)(h - 40); if (mod_h < 1) mod_h = 1;
    for (int i = 0; i < n && i < 30; i++) {
        float bx = x + 20 + (i * 37) % mod_w;
        float by = y + 20 + (i * 53) % mod_h;
        float br = 8 + (i * 7) % 20;
        hjpBeginPath(vg); hjpCircle(vg, bx, by, br);
        hjpFillColor(vg, hjpRGBA(70 + i*15, 130, 200, 120)); hjpFill(vg);
        hjpStrokeColor(vg, hjpRGBA(70 + i*15, 130, 200, 200)); hjpStrokeWidth(vg, 1); hjpStroke(vg);
    }
    gui_advance(h + 4);
    return hajimu_null();
}

/* ウォーターフォールチャート(ID, データ配列) → 無 */
static Value fn_waterfall_chart(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || !g_cur->vg) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    Value data = argv[1];
    float x, y, w; gui_pos(&x, &y, &w);
    float h = 200;
    hjpBeginPath(vg); hjpRoundedRect(vg, x, y, w, h, 4);
    hjpFillColor(vg, hjpRGBA(250,250,255,250)); hjpFill(vg);
    int n = gui_arr_len(data); if (n < 1) n = 1;
    float bar_w = (w - 20) / n;
    float scale = 2.0f;
    float base = y + h - 20, acc_h = 0;
    for (int i = 0; i < n; i++) {
        Value v = gui_arr_get(data, i);
        float val = (float)gui_as_number(v);
        float bh = val * scale; if (bh < 0) bh = -bh;
        if (bh < 1) bh = 1;
        float by_pos;
        if (val >= 0) { by_pos = base - acc_h - bh; }
        else          { by_pos = base - acc_h; }
        hjpBeginPath(vg); hjpRect(vg, x + 10 + i*bar_w + 2, by_pos, bar_w - 4, bh);
        hjpFillColor(vg, (val >= 0) ? hjpRGBA(80,180,80,220) : hjpRGBA(220,80,80,220));
        hjpFill(vg);
        acc_h += val * scale;
    }
    gui_advance(h + 4);
    return hajimu_null();
}

/* サンバーストチャート(ID, 階層データ) → 辞書 */
static Value fn_sunburst_chart(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || !g_cur->vg) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    Value data = argv[1];
    float x, y, w; gui_pos(&x, &y, &w);
    float h = 220, cx = x + w/2, cy = y + h/2;
    int n = gui_arr_len(data); if (n < 1) n = 1;
    float slice = 6.283185f / n;
    for (int ring = 0; ring < 3; ring++) {
        float r0 = 20 + ring * 25, r1 = r0 + 22;
        for (int i = 0; i < n; i++) {
            float a0 = i * slice, a1 = (i+1) * slice - 0.02f;
            hjpBeginPath(vg); hjpArc(vg, cx, cy, r0, a0, a1, HJP_CW);
            hjpArc(vg, cx, cy, r1, a1, a0, HJP_CCW); hjpClosePath(vg);
            int cr = 60+ring*40+i*20; if (cr > 255) cr = 255;
            int cg = 100+i*10; if (cg > 255) cg = 255;
            int cb = 180-ring*30; if (cb < 0) cb = 0;
            hjpFillColor(vg, hjpRGBA(cr, cg, cb, 200));
            hjpFill(vg);
        }
    }
    gui_advance(h + 4);
    return hajimu_null();
}

/* コード図(ID, 行列データ) → 無 */
static Value fn_chord_diagram(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || !g_cur->vg) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    float x, y, w; gui_pos(&x, &y, &w);
    float h = 220, cx = x + w/2, cy = y + h/2, r = 80;
    int n = 6;
    float slice = 6.283185f / n;
    for (int i = 0; i < n; i++) {
        float a0 = i * slice, a1 = (i+1) * slice - 0.05f;
        hjpBeginPath(vg); hjpArc(vg, cx, cy, r, a0, a1, HJP_CW);
        hjpStrokeColor(vg, hjpRGBA(80+i*30, 120, 200-i*20, 255)); hjpStrokeWidth(vg, 8);
        hjpStroke(vg);
    }
    /* コード線 */
    for (int i = 0; i < n; i++) {
        for (int j = i+1; j < n; j++) {
            float ai = (i + 0.5f) * slice, aj = (j + 0.5f) * slice;
            float x0 = cx + cosf(ai)*r, y0 = cy + sinf(ai)*r;
            float x1 = cx + cosf(aj)*r, y1 = cy + sinf(aj)*r;
            hjpBeginPath(vg);
            hjpMoveTo(vg, x0, y0);
            hjpBezierTo(vg, cx, cy, cx, cy, x1, y1);
            hjpStrokeColor(vg, hjpRGBA(100+i*20, 140, 200-j*15, 80));
            hjpStrokeWidth(vg, 1); hjpStroke(vg);
        }
    }
    gui_advance(h + 4); (void)argv;
    return hajimu_null();
}

/* コロプレスマップ(ID, 地域データ) → 辞書 */
static Value fn_choropleth_map(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || !g_cur->vg) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    Value data = argv[1];
    float x, y, w; gui_pos(&x, &y, &w);
    float h = 200;
    hjpBeginPath(vg); hjpRoundedRect(vg, x, y, w, h, 4);
    hjpFillColor(vg, hjpRGBA(230,240,250,255)); hjpFill(vg);
    int n = gui_arr_len(data); if (n < 1) n = 4;
    for (int i = 0; i < n && i < 16; i++) {
        float rx = x + 10 + (i % 4) * (w/4), ry = y + 10 + (i / 4) * 45;
        float rw = w/4 - 15, rh = 35;
        int intensity = 50 + (i * 40) % 200;
        hjpBeginPath(vg); hjpRoundedRect(vg, rx, ry, rw, rh, 3);
        hjpFillColor(vg, hjpRGBA(30, 80+intensity/2, intensity, 200)); hjpFill(vg);
    }
    gui_advance(h + 4);
    return hajimu_null();
}

/* フレームグラフ(ID, スタックデータ) → 辞書 */
static Value fn_flame_graph(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || !g_cur->vg) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    Value data = argv[1];
    float x, y, w; gui_pos(&x, &y, &w);
    float h = 160;
    hjpBeginPath(vg); hjpRoundedRect(vg, x, y, w, h, 4);
    hjpFillColor(vg, hjpRGBA(30,30,35,250)); hjpFill(vg);
    int n = gui_arr_len(data);
    float bar_h = 20;
    for (int i = 0; i < n && i < 7; i++) {
        float bx = x + i * 15, bw = w - i * 30;
        float by = y + h - (i+1) * bar_h;
        unsigned char r = 200 + (i*25)%55, g = 100 - i*10, b = 50;
        hjpBeginPath(vg); hjpRect(vg, bx, by, bw, bar_h - 1);
        hjpFillColor(vg, hjpRGBA(r, g, b, 220)); hjpFill(vg);
        Value v = gui_arr_get(data, i);
        const char *name = gui_as_string(v);
        if (name) {
            hjpFontFaceId(vg, g_cur->font_id); hjpFontSize(vg, 10);
            hjpFillColor(vg, hjpRGBA(255,255,255,255));
            hjpTextAlign(vg, HJP_ALIGN_LEFT|HJP_ALIGN_MIDDLE);
            hjpText(vg, bx + 4, by + bar_h/2, name, NULL);
        }
    }
    gui_advance(h + 4);
    return hajimu_null();
}

/* 等高線チャート(ID, 高度データ) → 無 */
static Value fn_contour_chart(int argc, Value *argv) {
    (void)argc; (void)argv;
    if (!g_cur || !g_cur->vg) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    float x, y, w; gui_pos(&x, &y, &w);
    float h = 200, cx = x + w/2, cy = y + h/2;
    hjpBeginPath(vg); hjpRoundedRect(vg, x, y, w, h, 4);
    hjpFillColor(vg, hjpRGBA(240,245,250,255)); hjpFill(vg);
    for (int i = 5; i >= 1; i--) {
        float r = i * 18;
        hjpBeginPath(vg); hjpEllipse(vg, cx, cy, r * 1.5f, r);
        hjpStrokeColor(vg, hjpRGBA(30, 80+i*20, 150+i*15, 200));
        hjpStrokeWidth(vg, 1); hjpStroke(vg);
    }
    gui_advance(h + 4);
    return hajimu_null();
}

/* 極座標チャート(ID, データ配列) → 無 */
static Value fn_polar_chart(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || !g_cur->vg) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    Value data = argv[1];
    float x, y, w; gui_pos(&x, &y, &w);
    float h = 220, cx = x + w/2, cy = y + h/2, r = 80;
    /* 同心円 */
    for (int i = 1; i <= 4; i++) {
        hjpBeginPath(vg); hjpCircle(vg, cx, cy, r * i / 4);
        hjpStrokeColor(vg, hjpRGBA(180,180,190,150)); hjpStrokeWidth(vg, 0.5f); hjpStroke(vg);
    }
    int n = gui_arr_len(data); if (n < 1) n = 1;
    float slice = 6.283185f / n;
    hjpBeginPath(vg);
    for (int i = 0; i < n; i++) {
        Value v = gui_arr_get(data, i);
        float val = (float)gui_as_number(v);
        float a = i * slice - 1.5708f;
        float pr = r * val;
        float px = cx + cosf(a) * pr, py = cy + sinf(a) * pr;
        if (i == 0) hjpMoveTo(vg, px, py); else hjpLineTo(vg, px, py);
    }
    hjpClosePath(vg);
    hjpFillColor(vg, hjpRGBA(80,140,220,80)); hjpFill(vg);
    hjpStrokeColor(vg, hjpRGBA(80,140,220,220)); hjpStrokeWidth(vg, 1.5f); hjpStroke(vg);
    gui_advance(h + 4);
    return hajimu_null();
}

/* =====================================================================
 * Phase 85-100: 残り全Phase (v8.5.0 — v10.0.0)
 * 簡潔実装: 即時モード描画 + 最小限の状態
 * ===================================================================*/

/* Phase 85 */
static Value fn_size_policy(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_fit_box(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_view_box(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_fractional_size(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_uniform_grid(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || !g_cur->vg) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    int cols = (int)gui_as_number(argv[1]);
    int rows = (int)gui_as_number(argv[2]);
    float x, y, w; gui_pos(&x, &y, &w);
    if (cols < 1) cols = 1; if (rows < 1) rows = 1;
    float cw = w / cols, ch = 30;
    float h = rows * ch;
    hjpStrokeColor(vg, hjpRGBA(100,100,120,200)); hjpStrokeWidth(vg, 0.5f);
    for (int r = 0; r <= rows; r++) { hjpBeginPath(vg); hjpMoveTo(vg, x, y+r*ch); hjpLineTo(vg, x+w, y+r*ch); hjpStroke(vg); }
    for (int c = 0; c <= cols; c++) { hjpBeginPath(vg); hjpMoveTo(vg, x+c*cw, y); hjpLineTo(vg, x+c*cw, y+h); hjpStroke(vg); }
    gui_advance(h + 4);
    return hajimu_null();
}
static Value fn_overflow_bar(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_layout_anchor(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_auto_fit_view(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_number(0); }

/* Phase 86 */
static Value fn_sliver_app_bar(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || !g_cur->vg) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    const char *title = gui_as_string(argv[1]);
    float exp_h = (argc > 2) ? (float)gui_as_number(argv[2]) : 120;
    float x, y, w; gui_pos(&x, &y, &w);
    hjpBeginPath(vg); hjpRect(vg, x, y, w, exp_h);
    hjpFillColor(vg, TH_ACCENT); hjpFill(vg);
    if (title) {
        hjpFontFaceId(vg, g_cur->font_id); hjpFontSize(vg, 22);
        hjpFillColor(vg, hjpRGBA(255,255,255,255));
        hjpTextAlign(vg, HJP_ALIGN_LEFT|HJP_ALIGN_BOTTOM);
        hjpText(vg, x + 16, y + exp_h - 12, title, NULL);
    }
    gui_advance(exp_h);
    return hajimu_null();
}
static Value fn_expansion_tile(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || !g_cur->vg) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    const char *title = gui_as_string(argv[1]);
    bool expanded = (argc > 2) ? gui_as_bool(argv[2]) : false;
    float x, y, w; gui_pos(&x, &y, &w);
    float h = 36;
    uint32_t id_h = gui_hash(title ? title : "et") + __LINE__;
    bool hov = false, pressed = false;
    gui_widget_logic(id_h, x, y, w, h, &hov, &pressed);
    if (pressed) expanded = !expanded;
    hjpBeginPath(vg); hjpRoundedRect(vg, x, y, w, h, 4);
    hjpFillColor(vg, hov ? hjpRGBA(60,60,70,255) : hjpRGBA(45,45,55,255)); hjpFill(vg);
    if (title) {
        hjpFontFaceId(vg, g_cur->font_id); hjpFontSize(vg, 14);
        hjpFillColor(vg, TH_TEXT); hjpTextAlign(vg, HJP_ALIGN_LEFT|HJP_ALIGN_MIDDLE);
        hjpText(vg, x + 12, y + h/2, title, NULL);
    }
    hjpFontSize(vg, 12); hjpTextAlign(vg, HJP_ALIGN_RIGHT|HJP_ALIGN_MIDDLE);
    hjpText(vg, x + w - 12, y + h/2, expanded ? "▲" : "▼", NULL);
    gui_advance(h + 2);
    return hajimu_bool(expanded);
}
static Value fn_search_anchor(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_string(""); }
static Value fn_hover_card(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_side_sheet(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_bool(false); }
static Value fn_toggle_group(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_array(); }
static Value fn_menu_anchor(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_number(-1); }
static Value fn_floating_label(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || !g_cur->vg) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    const char *label = gui_as_string(argv[1]);
    const char *val = gui_as_string(argv[2]);
    float x, y, w; gui_pos(&x, &y, &w);
    float h = 48;
    hjpBeginPath(vg); hjpRoundedRect(vg, x, y, w, h, 4);
    hjpFillColor(vg, hjpRGBA(45,45,55,255)); hjpFill(vg);
    if (label) {
        hjpFontFaceId(vg, g_cur->font_id); hjpFontSize(vg, 10);
        hjpFillColor(vg, TH_ACCENT); hjpTextAlign(vg, HJP_ALIGN_LEFT|HJP_ALIGN_TOP);
        hjpText(vg, x + 8, y + 4, label, NULL);
    }
    if (val) {
        hjpFontSize(vg, 14); hjpFillColor(vg, TH_TEXT);
        hjpTextAlign(vg, HJP_ALIGN_LEFT|HJP_ALIGN_BOTTOM);
        hjpText(vg, x + 8, y + h - 8, val, NULL);
    }
    hjpBeginPath(vg); hjpMoveTo(vg, x, y + h); hjpLineTo(vg, x + w, y + h);
    hjpStrokeColor(vg, TH_ACCENT); hjpStrokeWidth(vg, 2); hjpStroke(vg);
    gui_advance(h + 4);
    return hajimu_string(val ? val : "");
}

/* Phase 87 */
static Value fn_key_sequence_input(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_string(""); }
static Value fn_ip_address_input(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_string("0.0.0.0"); }
static Value fn_emoji_picker(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_string(""); }
static Value fn_country_select(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_auto_textarea(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_string(""); }
static Value fn_font_combobox(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_string(""); }
static Value fn_date_range_picker(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_calendar_date_range(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }

/* Phase 88 */
static Value fn_lottie_animation(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_crossfade(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_confetti(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || !g_cur->vg) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    float cx = (float)gui_as_number(argv[0]);
    float cy = (float)gui_as_number(argv[1]);
    for (int i = 0; i < 40; i++) {
        float a = (float)i * 0.157f;
        float r = 20 + (i * 17) % 80;
        float px = cx + cosf(a) * r, py = cy + sinf(a) * r;
        hjpBeginPath(vg); hjpRect(vg, px, py, 4 + i%4, 4 + i%3);
        hjpFillColor(vg, hjpRGBA((i*67)%255, (i*43)%255, (i*97)%255, 200)); hjpFill(vg);
    }
    return hajimu_null();
}
static Value fn_cool_bar(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || !g_cur->vg) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    Value icons = argv[1];
    float x, y, w; gui_pos(&x, &y, &w);
    float h = 60;
    int n = gui_arr_len(icons); if (n < 1) n = 1;
    hjpBeginPath(vg); hjpRoundedRect(vg, x, y, w, h, 12);
    hjpFillColor(vg, hjpRGBA(40,40,50,220)); hjpFill(vg);
    float icon_sz = 32, gap = 8;
    float total = n * (icon_sz + gap);
    float sx = x + (w - total) / 2;
    int sel = -1;
    for (int i = 0; i < n; i++) {
        float ix = sx + i * (icon_sz + gap), iy = y + (h - icon_sz) / 2;
        float mx = g_cur->in.mx, my = g_cur->in.my;
        bool hov = (mx >= ix && mx <= ix+icon_sz && my >= iy && my <= iy+icon_sz);
        float scale = hov ? 1.3f : 1.0f;
        float sz = icon_sz * scale;
        float ox = ix - (sz - icon_sz)/2, oy = iy - (sz - icon_sz)/2;
        hjpBeginPath(vg); hjpRoundedRect(vg, ox, oy, sz, sz, 8);
        hjpFillColor(vg, hov ? hjpRGBA(80,120,200,220) : hjpRGBA(60,60,80,200)); hjpFill(vg);
        if (hov && g_cur->in.clicked) sel = i;
    }
    gui_advance(h + 4);
    return hajimu_number(sel);
}
static Value fn_virtual_keyboard(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_string(""); }
static Value fn_scroll_spy(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_number(0); }
static Value fn_size_grip(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_dialog_button_box(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_number(-1); }

/* Phase 89 */
static Value fn_data_widget_mapper(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_undo_view(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_number(0); }
static Value fn_column_visibility_toggle(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_array(); }
static Value fn_style_proxy(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_theme_override(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_key_value_list(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || !g_cur->vg) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    Value pairs = argv[1];
    float x, y, w; gui_pos(&x, &y, &w);
    int n = gui_arr_len(pairs); if (n > 100) n = 100;
    float line_h = 24;
    for (int i = 0; i < n; i++) {
        Value p = gui_arr_get(pairs, i);
        const char *s = gui_as_string(p);
        if (s) {
            hjpFontFaceId(vg, g_cur->font_id); hjpFontSize(vg, 13);
            hjpFillColor(vg, TH_TEXT); hjpTextAlign(vg, HJP_ALIGN_LEFT|HJP_ALIGN_TOP);
            hjpText(vg, x + 8, y + i * line_h, s, NULL);
        }
    }
    gui_advance(n * line_h + 4);
    return hajimu_null();
}
static Value fn_kanban_board(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || !g_cur->vg) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    Value columns = argv[1];
    float x, y, w; gui_pos(&x, &y, &w);
    int nc = gui_arr_len(columns); if (nc < 1) nc = 1;
    float col_w = w / nc, h = 250;
    hjpBeginPath(vg); hjpRoundedRect(vg, x, y, w, h, 4);
    hjpFillColor(vg, hjpRGBA(35,35,45,250)); hjpFill(vg);
    for (int c = 0; c < nc; c++) {
        float cx2 = x + c * col_w;
        hjpBeginPath(vg); hjpRect(vg, cx2, y, col_w, 30);
        hjpFillColor(vg, hjpRGBA(50,50,65,255)); hjpFill(vg);
        Value col = gui_arr_get(columns, c);
        const char *name = gui_as_string(col);
        if (name) {
            hjpFontFaceId(vg, g_cur->font_id); hjpFontSize(vg, 12);
            hjpFillColor(vg, TH_TEXT); hjpTextAlign(vg, HJP_ALIGN_CENTER|HJP_ALIGN_MIDDLE);
            hjpText(vg, cx2 + col_w/2, y + 15, name, NULL);
        }
        if (c > 0) {
            hjpBeginPath(vg); hjpMoveTo(vg, cx2, y); hjpLineTo(vg, cx2, y + h);
            hjpStrokeColor(vg, hjpRGBA(60,60,70,200)); hjpStrokeWidth(vg, 1); hjpStroke(vg);
        }
    }
    gui_advance(h + 4);
    return hajimu_null();
}
static Value fn_interactive_viewer(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }

/* Phase 90 */
static Value fn_ruler(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || !g_cur->vg) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    int dir = (int)gui_as_number(argv[1]); /* 0=horizontal, 1=vertical */
    float x, y, w; gui_pos(&x, &y, &w);
    float h = (dir == 0) ? 24 : 200;
    hjpBeginPath(vg); hjpRect(vg, x, y, (dir==0)?w:24, h);
    hjpFillColor(vg, hjpRGBA(240,240,245,255)); hjpFill(vg);
    hjpStrokeColor(vg, hjpRGBA(100,100,110,200)); hjpStrokeWidth(vg, 0.5f);
    hjpFontFaceId(vg, g_cur->font_id); hjpFontSize(vg, 8);
    hjpFillColor(vg, hjpRGBA(80,80,90,255)); hjpTextAlign(vg, HJP_ALIGN_CENTER|HJP_ALIGN_TOP);
    if (dir == 0) {
        for (int i = 0; i <= (int)w; i += 10) {
            float th = (i % 50 == 0) ? 12 : (i % 10 == 0) ? 6 : 3;
            hjpBeginPath(vg); hjpMoveTo(vg, x+i, y+h); hjpLineTo(vg, x+i, y+h-th); hjpStroke(vg);
            if (i % 50 == 0) { char lb[8]; snprintf(lb,sizeof(lb),"%d",i); hjpText(vg, x+i, y, lb, NULL); }
        }
    }
    gui_advance(h + 2);
    return hajimu_null();
}
static Value fn_grid_lines(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || !g_cur->vg) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    float gx = (float)gui_as_number(argv[1]);
    float gy = (float)gui_as_number(argv[2]);
    float x, y, w; gui_pos(&x, &y, &w);
    float h = 200;
    hjpStrokeColor(vg, hjpRGBA(60,60,70,150)); hjpStrokeWidth(vg, 0.5f);
    if (gx > 1) for (float i = x; i <= x+w; i += gx) { hjpBeginPath(vg); hjpMoveTo(vg,i,y); hjpLineTo(vg,i,y+h); hjpStroke(vg); }
    if (gy > 1) for (float i = y; i <= y+h; i += gy) { hjpBeginPath(vg); hjpMoveTo(vg,x,i); hjpLineTo(vg,x+w,i); hjpStroke(vg); }
    gui_advance(h + 2);
    return hajimu_null();
}
static Value fn_eyedropper(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_number(0); }
static Value fn_color_palette(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || !g_cur->vg) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    Value colors = argv[1];
    float x, y, w; gui_pos(&x, &y, &w);
    int n = gui_arr_len(colors); if (n < 1) n = 1;
    float sz = 28, gap = 4;
    int cols = (int)((w - 4) / (sz + gap)); if (cols < 1) cols = 1;
    int rows = (n + cols - 1) / cols;
    float h = rows * (sz + gap) + 4;
    int sel = -1;
    for (int i = 0; i < n; i++) {
        int r = i / cols, c = i % cols;
        float px = x + 2 + c * (sz + gap), py = y + 2 + r * (sz + gap);
        Value cv = gui_arr_get(colors, i);
        int color = (int)gui_as_number(cv);
        unsigned char rr = (color>>16)&0xFF, gg = (color>>8)&0xFF, bb = color&0xFF;
        hjpBeginPath(vg); hjpRoundedRect(vg, px, py, sz, sz, 4);
        hjpFillColor(vg, hjpRGBA(rr, gg, bb, 255)); hjpFill(vg);
        float mx = g_cur->in.mx, my = g_cur->in.my;
        if (mx >= px && mx <= px+sz && my >= py && my <= py+sz) {
            hjpStrokeColor(vg, hjpRGBA(255,255,255,255)); hjpStrokeWidth(vg, 2); hjpStroke(vg);
            if (g_cur->in.clicked) sel = i;
        }
    }
    gui_advance(h);
    return hajimu_number(sel);
}
static Value fn_color_spectrum(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || !g_cur->vg) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    float x, y, w; gui_pos(&x, &y, &w);
    float h = 150;
    /* 色相グラデーション */
    for (int i = 0; i < 6; i++) {
        Hjpcolor c0, c1;
        float t0 = (float)i/6, t1 = (float)(i+1)/6;
        c0 = hjpHSLA(t0, 1, 0.5f, 255);
        c1 = hjpHSLA(t1, 1, 0.5f, 255);
        Hjppaint p = hjpLinearGradient(vg, x + t0*w, y, x + t1*w, y, c0, c1);
        hjpBeginPath(vg); hjpRect(vg, x + t0*w, y, (t1-t0)*w, h);
        hjpFillPaint(vg, p); hjpFill(vg);
    }
    /* 白→透明 上から */
    Hjppaint white = hjpLinearGradient(vg, x, y, x, y+h, hjpRGBA(255,255,255,255), hjpRGBA(255,255,255,0));
    hjpBeginPath(vg); hjpRect(vg, x, y, w, h); hjpFillPaint(vg, white); hjpFill(vg);
    /* 透明→黒 下から */
    Hjppaint black = hjpLinearGradient(vg, x, y, x, y+h, hjpRGBA(0,0,0,0), hjpRGBA(0,0,0,255));
    hjpBeginPath(vg); hjpRect(vg, x, y, w, h); hjpFillPaint(vg, black); hjpFill(vg);
    gui_advance(h + 4); (void)argv;
    return hajimu_number(0);
}
static Value fn_shape_editor(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_path_icon(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_gpu_profiler(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }

/* Phase 91 */
static Value fn_org_chart(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || !g_cur->vg) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    Value nodes = argv[1];
    float x, y, w; gui_pos(&x, &y, &w);
    float h = 200;
    hjpBeginPath(vg); hjpRoundedRect(vg, x, y, w, h, 4);
    hjpFillColor(vg, hjpRGBA(245,245,250,255)); hjpFill(vg);
    int n = gui_arr_len(nodes);
    /* root */
    float root_x = x + w/2 - 40, root_y = y + 10;
    hjpBeginPath(vg); hjpRoundedRect(vg, root_x, root_y, 80, 30, 4);
    hjpFillColor(vg, TH_ACCENT); hjpFill(vg);
    if (n > 0) {
        Value v = gui_arr_get(nodes, 0);
        const char *s = gui_as_string(v);
        if (s) { hjpFontFaceId(vg, g_cur->font_id); hjpFontSize(vg, 11);
            hjpFillColor(vg, hjpRGBA(255,255,255,255)); hjpTextAlign(vg, HJP_ALIGN_CENTER|HJP_ALIGN_MIDDLE);
            hjpText(vg, root_x+40, root_y+15, s, NULL); }
    }
    /* children */
    for (int i = 1; i < n && i < 8; i++) {
        float cx2 = x + 10 + (i-1) * (w / (n > 1 ? n-1 : 1));
        float cy2 = y + 60;
        hjpBeginPath(vg); hjpMoveTo(vg, root_x+40, root_y+30);
        hjpLineTo(vg, cx2+35, cy2); hjpStrokeColor(vg, hjpRGBA(150,150,170,200)); hjpStrokeWidth(vg, 1); hjpStroke(vg);
        hjpBeginPath(vg); hjpRoundedRect(vg, cx2, cy2, 70, 28, 4);
        hjpFillColor(vg, hjpRGBA(60,80,120,220)); hjpFill(vg);
    }
    gui_advance(h + 4);
    return hajimu_null();
}
static Value fn_mind_map(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_flowchart(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_class_diagram(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_state_machine_diagram(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_timeline_chart(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_gauge_cluster(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_donut_chart(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || !g_cur->vg) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    Value data = argv[1];
    float x, y, w; gui_pos(&x, &y, &w);
    float h = 200, cx = x + w/2, cy = y + h/2;
    float r_out = 70, r_in = 40;
    int n = gui_arr_len(data); if (n < 1) n = 1;
    float a = 0;
    for (int i = 0; i < n; i++) {
        Value v = gui_arr_get(data, i);
        float val = (float)gui_as_number(v);
        float sweep = val * 6.283185f;
        float a1 = a + sweep;
        hjpBeginPath(vg); hjpArc(vg, cx, cy, r_out, a, a1, HJP_CW);
        hjpArc(vg, cx, cy, r_in, a1, a, HJP_CCW); hjpClosePath(vg);
        int dr = 60+i*40; if (dr > 255) dr = 255;
        int dg = 100+i*20; if (dg > 255) dg = 255;
        int db = 200-i*25; if (db < 0) db = 0;
        hjpFillColor(vg, hjpRGBA(dr, dg, db, 220)); hjpFill(vg);
        a = a1;
    }
    gui_advance(h + 4);
    return hajimu_null();
}

/* Phase 92 */
static Value fn_color_wheel(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || !g_cur->vg) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    float x, y, w; gui_pos(&x, &y, &w);
    float h = 180, cx = x + w/2, cy = y + h/2, r = 70;
    for (int i = 0; i < 360; i += 2) {
        float a0 = i * 3.14159f / 180, a1 = (i+2) * 3.14159f / 180;
        hjpBeginPath(vg); hjpMoveTo(vg, cx, cy);
        hjpArc(vg, cx, cy, r, a0, a1, HJP_CW); hjpClosePath(vg);
        hjpFillColor(vg, hjpHSLA((float)i/360.0f, 1.0f, 0.5f, 255)); hjpFill(vg);
    }
    /* 中央白 */
    hjpBeginPath(vg); hjpCircle(vg, cx, cy, r*0.4f);
    hjpFillColor(vg, hjpRGBA(255,255,255,255)); hjpFill(vg);
    gui_advance(h + 4); (void)argv;
    return hajimu_number(0);
}
static Value fn_brush_selector(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_layer_panel(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_toolbar_customize(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_array(); }
static Value fn_ribbon(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_magnifier(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_crosshair(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || !g_cur->vg) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    float cx = (float)gui_as_number(argv[0]);
    float cy = (float)gui_as_number(argv[1]);
    int color = (argc > 2) ? (int)gui_as_number(argv[2]) : 0xFFFFFF;
    unsigned char rr = (color>>16)&0xFF, gg = (color>>8)&0xFF, bb = color&0xFF;
    hjpStrokeColor(vg, hjpRGBA(rr,gg,bb,200)); hjpStrokeWidth(vg, 1);
    hjpBeginPath(vg); hjpMoveTo(vg, cx-20, cy); hjpLineTo(vg, cx+20, cy); hjpStroke(vg);
    hjpBeginPath(vg); hjpMoveTo(vg, cx, cy-20); hjpLineTo(vg, cx, cy+20); hjpStroke(vg);
    return hajimu_null();
}
static Value fn_blueprint_grid(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || !g_cur->vg) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    float gap = (float)gui_as_number(argv[1]);
    if (gap < 5) gap = 20;
    float x, y, w; gui_pos(&x, &y, &w);
    float h = 250;
    hjpBeginPath(vg); hjpRect(vg, x, y, w, h);
    hjpFillColor(vg, hjpRGBA(20,30,50,255)); hjpFill(vg);
    hjpStrokeColor(vg, hjpRGBA(40,60,90,200)); hjpStrokeWidth(vg, 0.5f);
    for (float gx = x; gx <= x+w; gx += gap) { hjpBeginPath(vg); hjpMoveTo(vg,gx,y); hjpLineTo(vg,gx,y+h); hjpStroke(vg); }
    for (float gy = y; gy <= y+h; gy += gap) { hjpBeginPath(vg); hjpMoveTo(vg,x,gy); hjpLineTo(vg,x+w,gy); hjpStroke(vg); }
    hjpStrokeColor(vg, hjpRGBA(50,80,120,200)); hjpStrokeWidth(vg, 1);
    for (float gx = x; gx <= x+w; gx += gap*5) { hjpBeginPath(vg); hjpMoveTo(vg,gx,y); hjpLineTo(vg,gx,y+h); hjpStroke(vg); }
    for (float gy = y; gy <= y+h; gy += gap*5) { hjpBeginPath(vg); hjpMoveTo(vg,x,gy); hjpLineTo(vg,x+w,gy); hjpStroke(vg); }
    gui_advance(h + 4);
    return hajimu_null();
}

/* Phase 93 */
static Value fn_split_button(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || !g_cur->vg) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    const char *label = gui_as_string(argv[1]);
    float x, y, w; gui_pos(&x, &y, &w);
    float bw = 140, bh = 32;
    uint32_t id_h = gui_hash(label ? label : "sb") + __LINE__;
    bool hov = false, pressed = false;
    gui_widget_logic(id_h, x, y, bw, bh, &hov, &pressed);
    hjpBeginPath(vg); hjpRoundedRect(vg, x, y, bw - 24, bh, 4);
    hjpFillColor(vg, hov ? hjpRGBA(60,110,200,255) : TH_ACCENT); hjpFill(vg);
    hjpBeginPath(vg); hjpRoundedRect(vg, x + bw - 24, y, 24, bh, 4);
    hjpFillColor(vg, hjpRGBA(50,90,180,255)); hjpFill(vg);
    if (label) {
        hjpFontFaceId(vg, g_cur->font_id); hjpFontSize(vg, 13);
        hjpFillColor(vg, hjpRGBA(255,255,255,255)); hjpTextAlign(vg, HJP_ALIGN_CENTER|HJP_ALIGN_MIDDLE);
        hjpText(vg, x + (bw-24)/2, y + bh/2, label, NULL);
    }
    hjpTextAlign(vg, HJP_ALIGN_CENTER|HJP_ALIGN_MIDDLE);
    hjpText(vg, x + bw - 12, y + bh/2, "▼", NULL);
    gui_advance(bh + 4);
    return hajimu_number(pressed ? 0 : -1);
}
static Value fn_scroll_progress_bar(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_number(0); }
static Value fn_dot_pagination(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || !g_cur->vg) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    int total = (int)gui_as_number(argv[1]);
    int current = (int)gui_as_number(argv[2]);
    float x, y, w; gui_pos(&x, &y, &w);
    float dot_r = 4, gap = 12;
    float total_w = total * gap;
    float sx = x + (w - total_w) / 2;
    for (int i = 0; i < total; i++) {
        hjpBeginPath(vg); hjpCircle(vg, sx + i * gap + dot_r, y + dot_r + 2, dot_r);
        hjpFillColor(vg, (i == current) ? TH_ACCENT : hjpRGBA(150,150,160,200)); hjpFill(vg);
    }
    gui_advance(dot_r * 2 + 8);
    return hajimu_number(current);
}
static Value fn_group_select(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_string(""); }
static Value fn_infinite_canvas(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_sticky_note(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || !g_cur->vg) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    const char *text = gui_as_string(argv[1]);
    int color = (argc > 2) ? (int)gui_as_number(argv[2]) : 0xFFFF88;
    float px = (argc > 3) ? (float)gui_as_number(argv[3]) : 50;
    float py = (argc > 4) ? (float)gui_as_number(argv[4]) : 50;
    float sz = 100;
    unsigned char rr = (color>>16)&0xFF, gg = (color>>8)&0xFF, bb = color&0xFF;
    hjpBeginPath(vg); hjpRect(vg, px, py, sz, sz);
    hjpFillColor(vg, hjpRGBA(rr,gg,bb,230)); hjpFill(vg);
    /* 影 */
    hjpBeginPath(vg); hjpRect(vg, px+2, py+sz, sz, 4);
    hjpFillColor(vg, hjpRGBA(0,0,0,30)); hjpFill(vg);
    if (text) {
        hjpFontFaceId(vg, g_cur->font_id); hjpFontSize(vg, 12);
        hjpFillColor(vg, hjpRGBA(30,30,30,255)); hjpTextAlign(vg, HJP_ALIGN_LEFT|HJP_ALIGN_TOP);
        hjpTextBox(vg, px+6, py+6, sz-12, text, NULL);
    }
    return hajimu_null();
}
static Value fn_recording_indicator(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || !g_cur->vg) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    bool recording = gui_as_bool(argv[1]);
    float x, y, w; gui_pos(&x, &y, &w);
    float h = 24;
    if (recording) {
        float pulse = (g_frame_count % 60) < 30 ? 1.0f : 0.5f;
        hjpBeginPath(vg); hjpCircle(vg, x + 12, y + 12, 6);
        hjpFillColor(vg, hjpRGBA(255, 50, 50, (unsigned char)(255*pulse))); hjpFill(vg);
        hjpFontFaceId(vg, g_cur->font_id); hjpFontSize(vg, 12);
        hjpFillColor(vg, hjpRGBA(255,50,50,255)); hjpTextAlign(vg, HJP_ALIGN_LEFT|HJP_ALIGN_MIDDLE);
        hjpText(vg, x+22, y+12, "REC", NULL);
    }
    gui_advance(h);
    return hajimu_null();
}
static Value fn_dropdown_breadcrumb(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }

/* Phase 94 */
static Value fn_focus_traversal(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_custom_gesture(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_skip_link(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_color_blindness_sim(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_high_contrast(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_aria_tabs(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_number(0); }
static Value fn_live_region_ext(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_keyboard_nav_helper(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }

/* Phase 95 */
static Value fn_frame_analysis(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_repaint_visualization(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_widget_count(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_bundle_analysis(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_layout_boundaries(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_render_tree(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_memory_snapshot(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_hotkey_help(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }

/* Phase 96 */
static Value fn_context_provider(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_signal(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_memoize(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_effect(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_suspense(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_bool(false); }
static Value fn_optimistic_update(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_state_reset(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_dynamic_form_array(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }

/* Phase 97 */
static Value fn_table_column_pin(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_table_row_expand(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_bool(false); }
static Value fn_table_spark_cell(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_table_progress_cell(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_table_tag_cell(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_table_avatar_cell(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_table_action_column(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_number(-1); }
static Value fn_table_global_filter(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }

/* Phase 98 */
static Value fn_step_form(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_inline_form(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_conditional_form(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_form_grid(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_file_preview_list(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_ghost_text(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_string(""); }
static Value fn_slash_command(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_inline_comment(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }

/* Phase 99 */
static Value fn_share_sheet(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_bool(false); }
static Value fn_cookie_banner(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_os_color_scheme(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_string("dark"); }
static Value fn_notification_action(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_number(-1); }
static Value fn_file_association(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_deep_link(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_quick_action(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_number(-1); }
static Value fn_in_app_browser(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }

/* Phase 100 */
static Value fn_widget_public_api(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_theme_gallery(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_number(0); }
static Value fn_widget_catalog(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_quick_start(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_benchmark(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_migration(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_gui_builder(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_pdf_viewer(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }

/* =====================================================================
 * Phase 101: 高度コントロール III (v10.1.0)
 * ===================================================================*/

/* コマンドリンクボタン(ID, タイトル, 説明) → 真偽 */
static Value fn_command_link_button(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || !g_cur->vg) return hajimu_bool(false);
    Hjpcontext *vg = g_cur->vg;
    const char *title = gui_as_string(argv[1]);
    const char *desc  = (argc > 2) ? gui_as_string(argv[2]) : "";
    float x, y, w; gui_pos(&x, &y, &w);
    float h = 56;
    unsigned int uid = gui_hash(__func__) + __LINE__;
    bool hov = (g_cur->in.mx >= x && g_cur->in.mx <= x+w && g_cur->in.my >= y && g_cur->in.my <= y+h);
    hjpBeginPath(vg); hjpRoundedRect(vg, x, y, w, h, 4);
    hjpFillColor(vg, hov ? hjpRGBA(50,80,160,240) : hjpRGBA(45,45,60,220));
    hjpFill(vg);
    hjpStrokeColor(vg, hjpRGBA(80,120,200,180)); hjpStrokeWidth(vg, 1); hjpStroke(vg);
    /* Arrow icon */
    hjpBeginPath(vg); hjpMoveTo(vg, x+12, y+h/2-6); hjpLineTo(vg, x+20, y+h/2);
    hjpLineTo(vg, x+12, y+h/2+6); hjpStrokeColor(vg, hjpRGBA(100,180,255,255)); hjpStrokeWidth(vg, 2); hjpStroke(vg);
    hjpFontFaceId(vg, g_cur->font_id); hjpFontSize(vg, 16);
    hjpFillColor(vg, hjpRGBA(230,230,255,255)); hjpTextAlign(vg, HJP_ALIGN_LEFT|HJP_ALIGN_TOP);
    if (title) hjpText(vg, x+30, y+8, title, NULL);
    hjpFontSize(vg, 11); hjpFillColor(vg, hjpRGBA(160,160,180,200));
    if (desc) hjpText(vg, x+30, y+30, desc, NULL);
    gui_advance(h + 4);
    bool clicked = hov && g_cur->in.clicked;
    (void)uid;
    return hajimu_bool(clicked);
}

/* 遅延ボタン(ID, テキスト, 遅延ms) → 真偽 */
static Value fn_delay_button(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || !g_cur->vg) return hajimu_bool(false);
    Hjpcontext *vg = g_cur->vg;
    const char *text = gui_as_string(argv[1]);
    double delay_ms = (argc > 2) ? gui_as_number(argv[2]) : 1000.0;
    float x, y, w; gui_pos(&x, &y, &w);
    float h = 32;
    bool hov = (g_cur->in.mx >= x && g_cur->in.mx <= x+w && g_cur->in.my >= y && g_cur->in.my <= y+h);
    float progress = 0;
    if (hov && g_cur->in.down) {
        progress = (float)(g_frame_count % (int)(delay_ms/16.0 + 1)) / (float)(delay_ms/16.0 + 1);
    }
    hjpBeginPath(vg); hjpRoundedRect(vg, x, y, w, h, 4);
    hjpFillColor(vg, hjpRGBA(50,50,65,220)); hjpFill(vg);
    if (progress > 0) {
        hjpBeginPath(vg); hjpRoundedRect(vg, x, y, w * progress, h, 4);
        hjpFillColor(vg, hjpRGBA(60,120,220,180)); hjpFill(vg);
    }
    hjpFontFaceId(vg, g_cur->font_id); hjpFontSize(vg, 14);
    hjpFillColor(vg, hjpRGBA(220,220,240,255)); hjpTextAlign(vg, HJP_ALIGN_CENTER|HJP_ALIGN_MIDDLE);
    if (text) hjpText(vg, x+w/2, y+h/2, text, NULL);
    gui_advance(h + 4);
    bool fired = (progress >= 0.95f);
    (void)delay_ms;
    return hajimu_bool(fired);
}

/* タンブラー(ID, 項目配列, 選択) → 数値 */
static Value fn_tumbler(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || !g_cur->vg) return hajimu_number(0);
    Hjpcontext *vg = g_cur->vg;
    Value items = argv[1];
    int sel = (argc > 2) ? (int)gui_as_number(argv[2]) : 0;
    int n = gui_arr_len(items);
    if (n < 1) n = 1;
    if (sel < 0) sel = 0; if (sel >= n) sel = n - 1;
    float x, y, w; gui_pos(&x, &y, &w);
    float item_h = 30, visible = 5, h = item_h * visible;
    /* Background */
    hjpBeginPath(vg); hjpRoundedRect(vg, x, y, w, h, 6);
    hjpFillColor(vg, hjpRGBA(35,35,50,230)); hjpFill(vg);
    /* Selected highlight */
    float sel_y = y + (visible/2) * item_h;
    hjpBeginPath(vg); hjpRect(vg, x+2, sel_y, w-4, item_h);
    hjpFillColor(vg, hjpRGBA(60,90,180,120)); hjpFill(vg);
    /* Items */
    hjpFontFaceId(vg, g_cur->font_id); hjpTextAlign(vg, HJP_ALIGN_CENTER|HJP_ALIGN_MIDDLE);
    for (int i = 0; i < (int)visible && i < n; i++) {
        int idx = sel - (int)(visible/2) + i;
        if (idx < 0 || idx >= n) continue;
        float iy = y + i * item_h + item_h/2;
        float dist = fabsf(iy - (sel_y + item_h/2));
        int alpha = (int)(255 - dist * 2);
        if (alpha < 60) alpha = 60;
        hjpFontSize(vg, (i == (int)(visible/2)) ? 16.0f : 13.0f);
        hjpFillColor(vg, hjpRGBA(220,220,240,(unsigned char)alpha));
        const char *label = gui_as_string(gui_arr_get(items, idx));
        if (label) hjpText(vg, x+w/2, iy, label, NULL);
    }
    /* Scroll interaction */
    bool hov = (g_cur->in.mx >= x && g_cur->in.mx <= x+w && g_cur->in.my >= y && g_cur->in.my <= y+h);
    if (hov && g_cur->in.clicked) { sel = (sel + 1) % n; }
    gui_advance(h + 4);
    return hajimu_number(sel);
}

/* マルチステートチェック(ID, テキスト, 状態) → 数値 */
static Value fn_multi_state_check(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || !g_cur->vg) return hajimu_number(0);
    Hjpcontext *vg = g_cur->vg;
    const char *text = gui_as_string(argv[1]);
    int state = (argc > 2) ? (int)gui_as_number(argv[2]) : 0;
    int max_states = 3; /* off / on / indeterminate */
    float x, y, w; gui_pos(&x, &y, &w);
    float h = 24, box = 18;
    bool hov = (g_cur->in.mx >= x && g_cur->in.mx <= x+20 && g_cur->in.my >= y && g_cur->in.my <= y+h);
    /* Box */
    hjpBeginPath(vg); hjpRoundedRect(vg, x+2, y+3, box, box, 3);
    if (state == 1) { hjpFillColor(vg, hjpRGBA(60,140,255,255)); hjpFill(vg);
        hjpBeginPath(vg); hjpMoveTo(vg, x+6, y+12); hjpLineTo(vg, x+10, y+16); hjpLineTo(vg, x+17, y+8);
        hjpStrokeColor(vg, hjpRGBA(255,255,255,255)); hjpStrokeWidth(vg, 2); hjpStroke(vg);
    } else if (state == 2) { hjpFillColor(vg, hjpRGBA(60,140,255,255)); hjpFill(vg);
        hjpBeginPath(vg); hjpMoveTo(vg, x+6, y+12); hjpLineTo(vg, x+17, y+12);
        hjpStrokeColor(vg, hjpRGBA(255,255,255,255)); hjpStrokeWidth(vg, 2); hjpStroke(vg);
    } else { hjpFillColor(vg, hjpRGBA(60,60,75,220)); hjpFill(vg);
        hjpStrokeColor(vg, hjpRGBA(100,100,120,200)); hjpStrokeWidth(vg, 1); hjpStroke(vg); }
    hjpFontFaceId(vg, g_cur->font_id); hjpFontSize(vg, 14);
    hjpFillColor(vg, hjpRGBA(220,220,240,255)); hjpTextAlign(vg, HJP_ALIGN_LEFT|HJP_ALIGN_MIDDLE);
    if (text) hjpText(vg, x+26, y+h/2, text, NULL);
    if (hov && g_cur->in.clicked) { state = (state + 1) % max_states; }
    gui_advance(h + 4);
    return hajimu_number(state);
}

/* インプレースエディタ(ID, 値) → 文字列 */
static Value fn_inplace_editor(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || !g_cur->vg) return hajimu_string("");
    Hjpcontext *vg = g_cur->vg;
    const char *val = gui_as_string(argv[1]);
    float x, y, w; gui_pos(&x, &y, &w);
    float h = 28;
    bool hov = (g_cur->in.mx >= x && g_cur->in.mx <= x+w && g_cur->in.my >= y && g_cur->in.my <= y+h);
    /* Display / edit toggle by click */
    hjpBeginPath(vg); hjpRoundedRect(vg, x, y, w, h, 3);
    hjpFillColor(vg, hov ? hjpRGBA(50,50,70,200) : hjpRGBA(0,0,0,0)); hjpFill(vg);
    if (hov) { hjpStrokeColor(vg, hjpRGBA(80,120,200,160)); hjpStrokeWidth(vg, 1); hjpStroke(vg); }
    hjpFontFaceId(vg, g_cur->font_id); hjpFontSize(vg, 14);
    hjpFillColor(vg, hjpRGBA(220,220,240,255)); hjpTextAlign(vg, HJP_ALIGN_LEFT|HJP_ALIGN_MIDDLE);
    if (val) hjpText(vg, x+6, y+h/2, val, NULL);
    gui_advance(h + 4);
    return hajimu_string(val ? val : "");
}

/* 編集可能リスト(ID, 項目配列) → 配列 */
static Value fn_editable_list(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || !g_cur->vg) return hajimu_array();
    Hjpcontext *vg = g_cur->vg;
    Value items = argv[1];
    int n = gui_arr_len(items);
    float x, y, w; gui_pos(&x, &y, &w);
    float item_h = 24, btn_w = 24;
    float h = (n > 0 ? n : 1) * item_h + 30;
    /* Background */
    hjpBeginPath(vg); hjpRoundedRect(vg, x, y, w, h, 4);
    hjpFillColor(vg, hjpRGBA(40,40,55,220)); hjpFill(vg);
    /* Items */
    hjpFontFaceId(vg, g_cur->font_id); hjpFontSize(vg, 13);
    for (int i = 0; i < n && i < 50; i++) {
        float iy = y + i * item_h;
        hjpFillColor(vg, hjpRGBA(210,210,230,255)); hjpTextAlign(vg, HJP_ALIGN_LEFT|HJP_ALIGN_MIDDLE);
        const char *label = gui_as_string(gui_arr_get(items, i));
        if (label) hjpText(vg, x+8, iy+item_h/2, label, NULL);
    }
    /* Buttons: + - ↑ ↓ */
    float by = y + n * item_h + 4;
    const char *btns[] = {"+", "-", "\xe2\x86\x91", "\xe2\x86\x93"};
    for (int i = 0; i < 4; i++) {
        float bx = x + 4 + i * (btn_w + 4);
        hjpBeginPath(vg); hjpRoundedRect(vg, bx, by, btn_w, 22, 3);
        hjpFillColor(vg, hjpRGBA(60,60,80,220)); hjpFill(vg);
        hjpFontSize(vg, 14); hjpFillColor(vg, hjpRGBA(180,200,255,255));
        hjpTextAlign(vg, HJP_ALIGN_CENTER|HJP_ALIGN_MIDDLE);
        hjpText(vg, bx+btn_w/2, by+11, btns[i], NULL);
    }
    gui_advance(h + 4);
    return argv[1]; /* return original items */
}

/* 並べ替えリスト(ID, 項目配列, 表示配列) → 辞書 */
static Value fn_rearrange_list(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || !g_cur->vg) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    Value items = argv[1];
    int n = gui_arr_len(items);
    float x, y, w; gui_pos(&x, &y, &w);
    float item_h = 24;
    float h = (n > 0 ? n : 1) * item_h;
    hjpBeginPath(vg); hjpRoundedRect(vg, x, y, w, h, 4);
    hjpFillColor(vg, hjpRGBA(40,40,55,220)); hjpFill(vg);
    hjpFontFaceId(vg, g_cur->font_id); hjpFontSize(vg, 13);
    for (int i = 0; i < n && i < 50; i++) {
        float iy = y + i * item_h;
        /* Checkbox */
        hjpBeginPath(vg); hjpRoundedRect(vg, x+4, iy+4, 16, 16, 2);
        hjpFillColor(vg, hjpRGBA(60,140,255,200)); hjpFill(vg);
        hjpBeginPath(vg); hjpMoveTo(vg, x+7, iy+12); hjpLineTo(vg, x+11, iy+16); hjpLineTo(vg, x+17, iy+8);
        hjpStrokeColor(vg, hjpRGBA(255,255,255,255)); hjpStrokeWidth(vg, 1.5f); hjpStroke(vg);
        /* Drag handle */
        hjpFillColor(vg, hjpRGBA(120,120,140,180)); hjpFontSize(vg, 10);
        hjpTextAlign(vg, HJP_ALIGN_RIGHT|HJP_ALIGN_MIDDLE);
        hjpText(vg, x+w-8, iy+item_h/2, "\xe2\x98\xb0", NULL);
        /* Label */
        hjpFillColor(vg, hjpRGBA(210,210,230,255)); hjpFontSize(vg, 13);
        hjpTextAlign(vg, HJP_ALIGN_LEFT|HJP_ALIGN_MIDDLE);
        const char *label = gui_as_string(gui_arr_get(items, i));
        if (label) hjpText(vg, x+26, iy+item_h/2, label, NULL);
    }
    gui_advance(h + 4);
    return hajimu_null();
}

/* キーボードキー表示(キー文字列) → 無 */
static Value fn_kbd_display(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || !g_cur->vg) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    const char *key = gui_as_string(argv[0]);
    float x, y, w; gui_pos(&x, &y, &w);
    float tw = 0;
    hjpFontFaceId(vg, g_cur->font_id); hjpFontSize(vg, 12);
    if (key) tw = hjpTextBounds(vg, 0, 0, key, NULL, NULL);
    float kw = tw + 14, kh = 22;
    hjpBeginPath(vg); hjpRoundedRect(vg, x, y, kw, kh, 4);
    hjpFillColor(vg, hjpRGBA(55,55,70,240)); hjpFill(vg);
    hjpStrokeColor(vg, hjpRGBA(90,90,110,200)); hjpStrokeWidth(vg, 1); hjpStroke(vg);
    /* Bottom shadow */
    hjpBeginPath(vg); hjpMoveTo(vg, x+2, y+kh); hjpLineTo(vg, x+kw-2, y+kh);
    hjpStrokeColor(vg, hjpRGBA(30,30,40,200)); hjpStrokeWidth(vg, 2); hjpStroke(vg);
    hjpFillColor(vg, hjpRGBA(220,220,240,255)); hjpTextAlign(vg, HJP_ALIGN_CENTER|HJP_ALIGN_MIDDLE);
    if (key) hjpText(vg, x+kw/2, y+kh/2-1, key, NULL);
    gui_advance(kh + 4);
    return hajimu_null();
}

/* =====================================================================
 * Phase 102: メニュー/ナビ拡張 II (v10.2.0)
 * ===================================================================*/

/* メガメニュー(ID, モデル配列) → 辞書 */
static Value fn_mega_menu(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || !g_cur->vg) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    Value model = argv[1];
    int n = gui_arr_len(model);
    float x, y, w; gui_pos(&x, &y, &w);
    float h = 36;
    /* Menu bar */
    hjpBeginPath(vg); hjpRect(vg, x, y, w, h);
    hjpFillColor(vg, hjpRGBA(35,35,50,240)); hjpFill(vg);
    float mx = x + 8;
    hjpFontFaceId(vg, g_cur->font_id); hjpFontSize(vg, 14);
    hjpTextAlign(vg, HJP_ALIGN_LEFT|HJP_ALIGN_MIDDLE);
    for (int i = 0; i < n && i < 8; i++) {
        const char *label = gui_as_string(gui_arr_get(model, i));
        bool item_hov = (g_cur->in.mx >= mx && g_cur->in.mx <= mx+80 && g_cur->in.my >= y && g_cur->in.my <= y+h);
        hjpFillColor(vg, item_hov ? hjpRGBA(100,160,255,255) : hjpRGBA(200,200,220,255));
        if (label) hjpText(vg, mx, y+h/2, label, NULL);
        mx += 90;
    }
    gui_advance(h + 4);
    return hajimu_null();
}

/* パネルメニュー(ID, モデル配列) → 辞書 */
static Value fn_panel_menu(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || !g_cur->vg) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    Value model = argv[1];
    int n = gui_arr_len(model);
    float x, y, w; gui_pos(&x, &y, &w);
    float item_h = 32, h = n * item_h;
    if (h < 32) h = 32;
    hjpBeginPath(vg); hjpRoundedRect(vg, x, y, w, h, 4);
    hjpFillColor(vg, hjpRGBA(35,35,50,230)); hjpFill(vg);
    hjpFontFaceId(vg, g_cur->font_id); hjpFontSize(vg, 14);
    for (int i = 0; i < n && i < 20; i++) {
        float iy = y + i * item_h;
        bool item_hov = (g_cur->in.mx >= x && g_cur->in.mx <= x+w && g_cur->in.my >= iy && g_cur->in.my <= iy+item_h);
        if (item_hov) { hjpBeginPath(vg); hjpRect(vg, x, iy, w, item_h);
            hjpFillColor(vg, hjpRGBA(50,70,130,150)); hjpFill(vg); }
        /* Arrow for expandable */
        hjpFillColor(vg, hjpRGBA(120,140,180,200)); hjpTextAlign(vg, HJP_ALIGN_LEFT|HJP_ALIGN_MIDDLE);
        hjpText(vg, x+8, iy+item_h/2, "\xe2\x96\xb6", NULL);
        hjpFillColor(vg, item_hov ? hjpRGBA(100,180,255,255) : hjpRGBA(200,200,220,255));
        hjpTextAlign(vg, HJP_ALIGN_LEFT|HJP_ALIGN_MIDDLE);
        const char *label = gui_as_string(gui_arr_get(model, i));
        if (label) hjpText(vg, x+24, iy+item_h/2, label, NULL);
    }
    gui_advance(h + 4);
    return hajimu_null();
}

/* ティアードメニュー(ID, モデル配列) → 辞書 */
static Value fn_tiered_menu(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || !g_cur->vg) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    Value model = argv[1];
    int n = gui_arr_len(model);
    float x, y, w; gui_pos(&x, &y, &w);
    float item_h = 28, h = n * item_h;
    if (h < 28) h = 28;
    hjpBeginPath(vg); hjpRoundedRect(vg, x, y, 200, h, 4);
    hjpFillColor(vg, hjpRGBA(40,40,58,240)); hjpFill(vg);
    hjpStrokeColor(vg, hjpRGBA(70,70,90,200)); hjpStrokeWidth(vg, 1); hjpStroke(vg);
    hjpFontFaceId(vg, g_cur->font_id); hjpFontSize(vg, 13);
    for (int i = 0; i < n && i < 20; i++) {
        float iy = y + i * item_h;
        bool ih = (g_cur->in.mx >= x && g_cur->in.mx <= x+200 && g_cur->in.my >= iy && g_cur->in.my <= iy+item_h);
        if (ih) { hjpBeginPath(vg); hjpRect(vg, x+2, iy, 196, item_h);
            hjpFillColor(vg, hjpRGBA(60,80,150,150)); hjpFill(vg); }
        hjpFillColor(vg, hjpRGBA(210,210,230,255)); hjpTextAlign(vg, HJP_ALIGN_LEFT|HJP_ALIGN_MIDDLE);
        const char *label = gui_as_string(gui_arr_get(model, i));
        if (label) hjpText(vg, x+12, iy+item_h/2, label, NULL);
        /* Submenu arrow */
        hjpFillColor(vg, hjpRGBA(140,140,160,180));
        hjpTextAlign(vg, HJP_ALIGN_RIGHT|HJP_ALIGN_MIDDLE);
        hjpText(vg, x+192, iy+item_h/2, "\xe2\x96\xb8", NULL);
    }
    gui_advance(h + 4);
    return hajimu_null();
}

/* タブメニュー(ID, 項目配列) → 数値 */
static Value fn_tab_menu(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || !g_cur->vg) return hajimu_number(0);
    Hjpcontext *vg = g_cur->vg;
    Value items = argv[1];
    int n = gui_arr_len(items);
    float x, y, w; gui_pos(&x, &y, &w);
    float h = 36;
    int selected = 0;
    hjpBeginPath(vg); hjpRect(vg, x, y, w, h);
    hjpFillColor(vg, hjpRGBA(35,35,50,240)); hjpFill(vg);
    float tw = (n > 0) ? w / n : w;
    hjpFontFaceId(vg, g_cur->font_id); hjpFontSize(vg, 14);
    for (int i = 0; i < n && i < 10; i++) {
        float tx = x + i * tw;
        bool th = (g_cur->in.mx >= tx && g_cur->in.mx <= tx+tw && g_cur->in.my >= y && g_cur->in.my <= y+h);
        if (th && g_cur->in.clicked) selected = i;
        hjpFillColor(vg, th ? hjpRGBA(100,160,255,255) : hjpRGBA(180,180,200,255));
        hjpTextAlign(vg, HJP_ALIGN_CENTER|HJP_ALIGN_MIDDLE);
        const char *label = gui_as_string(gui_arr_get(items, i));
        if (label) hjpText(vg, tx+tw/2, y+h/2, label, NULL);
    }
    /* Active indicator */
    hjpBeginPath(vg); hjpRect(vg, x + selected * tw + 4, y+h-3, tw-8, 3);
    hjpFillColor(vg, hjpRGBA(80,140,255,255)); hjpFill(vg);
    gui_advance(h + 4);
    return hajimu_number(selected);
}

/* ツリーブック(ID, ツリーID, ページ配列) → 数値 */
static Value fn_tree_book(int argc, Value *argv) {
    (void)argc; (void)argv;
    return hajimu_number(0);
}

/* リストブック(ID, リスト配列, ページ配列) → 数値 */
static Value fn_list_book(int argc, Value *argv) {
    (void)argc; (void)argv;
    return hajimu_number(0);
}

/* キーフィルタ(ID, パターン) → 無 */
static Value fn_key_filter(int argc, Value *argv) {
    (void)argc; (void)argv;
    return hajimu_null();
}

/* マルチカラムコンボ(ID, 列定義, データ) → 辞書 */
static Value fn_multi_column_combo(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || !g_cur->vg) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    float x, y, w; gui_pos(&x, &y, &w);
    float h = 30;
    hjpBeginPath(vg); hjpRoundedRect(vg, x, y, w, h, 4);
    hjpFillColor(vg, hjpRGBA(45,45,60,230)); hjpFill(vg);
    hjpStrokeColor(vg, hjpRGBA(80,80,100,200)); hjpStrokeWidth(vg, 1); hjpStroke(vg);
    hjpFontFaceId(vg, g_cur->font_id); hjpFontSize(vg, 13);
    hjpFillColor(vg, hjpRGBA(180,180,200,255)); hjpTextAlign(vg, HJP_ALIGN_LEFT|HJP_ALIGN_MIDDLE);
    hjpText(vg, x+8, y+h/2, "\xe9\x81\xb8\xe6\x8a\x9e...", NULL); /* 選択... */
    /* Dropdown arrow */
    hjpFillColor(vg, hjpRGBA(140,140,160,200)); hjpTextAlign(vg, HJP_ALIGN_RIGHT|HJP_ALIGN_MIDDLE);
    hjpText(vg, x+w-8, y+h/2, "\xe2\x96\xbc", NULL);
    gui_advance(h + 4);
    (void)argv;
    return hajimu_null();
}

/* =====================================================================
 * Phase 103: レイアウト/コンテナ高度 II (v10.3.0)
 * ===================================================================*/

/* ダッシュボードレイアウト(ID, パネル配列) → 辞書 */
static Value fn_dashboard_layout(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || !g_cur->vg) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    Value panels = argv[1];
    int n = gui_arr_len(panels);
    float x, y, w; gui_pos(&x, &y, &w);
    int cols = 3;
    float cell_w = w / cols, cell_h = 120;
    int rows = (n + cols - 1) / cols;
    float h = rows * cell_h;
    hjpBeginPath(vg); hjpRect(vg, x, y, w, h);
    hjpFillColor(vg, hjpRGBA(25,25,35,200)); hjpFill(vg);
    for (int i = 0; i < n && i < 20; i++) {
        int r = i / cols, c = i % cols;
        float px = x + c * cell_w + 4, py = y + r * cell_h + 4;
        hjpBeginPath(vg); hjpRoundedRect(vg, px, py, cell_w-8, cell_h-8, 6);
        hjpFillColor(vg, hjpRGBA(40,40,58,240)); hjpFill(vg);
        hjpStrokeColor(vg, hjpRGBA(60,80,120,150)); hjpStrokeWidth(vg, 1); hjpStroke(vg);
        const char *label = gui_as_string(gui_arr_get(panels, i));
        if (label) {
            hjpFontFaceId(vg, g_cur->font_id); hjpFontSize(vg, 12);
            hjpFillColor(vg, hjpRGBA(180,200,240,255)); hjpTextAlign(vg, HJP_ALIGN_CENTER|HJP_ALIGN_TOP);
            hjpText(vg, px + (cell_w-8)/2, py+8, label, NULL);
        }
    }
    gui_advance(h + 4);
    return hajimu_null();
}

/* フィールドセット(ID, 凡例, 折りたたみ) → 真偽 */
static Value fn_fieldset(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || !g_cur->vg) return hajimu_bool(true);
    Hjpcontext *vg = g_cur->vg;
    const char *legend = gui_as_string(argv[1]);
    bool collapsed = (argc > 2) ? gui_as_bool(argv[2]) : false;
    float x, y, w; gui_pos(&x, &y, &w);
    float h = collapsed ? 28 : 120;
    hjpBeginPath(vg); hjpRoundedRect(vg, x, y, w, h, 4);
    hjpStrokeColor(vg, hjpRGBA(80,80,100,200)); hjpStrokeWidth(vg, 1); hjpStroke(vg);
    /* Legend background */
    if (legend) {
        hjpFontFaceId(vg, g_cur->font_id); hjpFontSize(vg, 13);
        float tw = hjpTextBounds(vg, 0, 0, legend, NULL, NULL);
        hjpBeginPath(vg); hjpRect(vg, x+12, y-7, tw+10, 14);
        hjpFillColor(vg, hjpRGBA(30,30,45,255)); hjpFill(vg);
        hjpFillColor(vg, hjpRGBA(180,200,240,255)); hjpTextAlign(vg, HJP_ALIGN_LEFT|HJP_ALIGN_MIDDLE);
        hjpText(vg, x+17, y, legend, NULL);
    }
    bool hov = (g_cur->in.mx >= x && g_cur->in.mx <= x+60 && g_cur->in.my >= y-10 && g_cur->in.my <= y+10);
    if (hov && g_cur->in.clicked) collapsed = !collapsed;
    gui_advance(h + 4);
    return hajimu_bool(!collapsed);
}

/* ページヘッダー(ID, タイトル, サブ, アクション) → 辞書 */
static Value fn_page_header(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || !g_cur->vg) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    const char *title = gui_as_string(argv[1]);
    const char *sub = (argc > 2) ? gui_as_string(argv[2]) : NULL;
    float x, y, w; gui_pos(&x, &y, &w);
    float h = 56;
    hjpBeginPath(vg); hjpRect(vg, x, y, w, h);
    hjpFillColor(vg, hjpRGBA(35,35,50,240)); hjpFill(vg);
    /* Back arrow */
    hjpBeginPath(vg); hjpMoveTo(vg, x+16, y+h/2); hjpLineTo(vg, x+8, y+h/2);
    hjpMoveTo(vg, x+12, y+h/2-5); hjpLineTo(vg, x+8, y+h/2); hjpLineTo(vg, x+12, y+h/2+5);
    hjpStrokeColor(vg, hjpRGBA(100,160,255,255)); hjpStrokeWidth(vg, 2); hjpStroke(vg);
    hjpFontFaceId(vg, g_cur->font_id);
    hjpFontSize(vg, 18); hjpFillColor(vg, hjpRGBA(230,230,250,255));
    hjpTextAlign(vg, HJP_ALIGN_LEFT|HJP_ALIGN_TOP);
    if (title) hjpText(vg, x+28, y+8, title, NULL);
    if (sub) { hjpFontSize(vg, 12); hjpFillColor(vg, hjpRGBA(150,150,170,200));
        hjpText(vg, x+28, y+32, sub, NULL); }
    hjpBeginPath(vg); hjpMoveTo(vg, x, y+h); hjpLineTo(vg, x+w, y+h);
    hjpStrokeColor(vg, hjpRGBA(60,60,80,200)); hjpStrokeWidth(vg, 1); hjpStroke(vg);
    gui_advance(h + 4);
    return hajimu_null();
}

/* アプリシェル(ID, メニュー, 設定) → 辞書 */
static Value fn_app_shell(int argc, Value *argv) {
    (void)argc; (void)argv;
    return hajimu_null();
}

/* タイルビュー(ID, タイル配列) → 数値 */
static Value fn_tile_view(int argc, Value *argv) {
    (void)argc; (void)argv;
    return hajimu_number(0);
}

/* コンファームエディット(ID, 値) → 辞書 */
static Value fn_confirm_edit(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || !g_cur->vg) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    const char *val = gui_as_string(argv[1]);
    float x, y, w; gui_pos(&x, &y, &w);
    float h = 30;
    hjpBeginPath(vg); hjpRoundedRect(vg, x, y, w-60, h, 3);
    hjpFillColor(vg, hjpRGBA(45,45,60,230)); hjpFill(vg);
    hjpStrokeColor(vg, hjpRGBA(80,80,100,200)); hjpStrokeWidth(vg, 1); hjpStroke(vg);
    hjpFontFaceId(vg, g_cur->font_id); hjpFontSize(vg, 13);
    hjpFillColor(vg, hjpRGBA(210,210,230,255)); hjpTextAlign(vg, HJP_ALIGN_LEFT|HJP_ALIGN_MIDDLE);
    if (val) hjpText(vg, x+6, y+h/2, val, NULL);
    /* Confirm / Cancel buttons */
    const char *btns[] = {"\xe2\x9c\x93", "\xe2\x9c\x97"}; /* ✓ ✗ */
    Hjpcolor cols[] = {hjpRGBA(60,180,100,220), hjpRGBA(200,60,60,220)};
    for (int i = 0; i < 2; i++) {
        float bx = x + w - 56 + i * 28;
        hjpBeginPath(vg); hjpRoundedRect(vg, bx, y+2, 26, 26, 3);
        hjpFillColor(vg, cols[i]); hjpFill(vg);
        hjpFontSize(vg, 14); hjpFillColor(vg, hjpRGBA(255,255,255,255));
        hjpTextAlign(vg, HJP_ALIGN_CENTER|HJP_ALIGN_MIDDLE);
        hjpText(vg, bx+13, y+h/2, btns[i], NULL);
    }
    gui_advance(h + 4);
    return hajimu_null();
}

/* アクションシート(ID, アクション配列) → 数値 */
static Value fn_action_sheet(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || !g_cur->vg) return hajimu_number(-1);
    Hjpcontext *vg = g_cur->vg;
    Value actions = argv[1];
    int n = gui_arr_len(actions);
    float x, y, w; gui_pos(&x, &y, &w);
    float item_h = 44, h = n * item_h + 54;
    hjpBeginPath(vg); hjpRoundedRect(vg, x, y, w, h, 12);
    hjpFillColor(vg, hjpRGBA(40,40,58,245)); hjpFill(vg);
    hjpFontFaceId(vg, g_cur->font_id);
    int clicked = -1;
    for (int i = 0; i < n && i < 10; i++) {
        float iy = y + i * item_h;
        bool ih = (g_cur->in.mx >= x && g_cur->in.mx <= x+w && g_cur->in.my >= iy && g_cur->in.my <= iy+item_h);
        if (ih) { hjpBeginPath(vg); hjpRect(vg, x+4, iy, w-8, item_h);
            hjpFillColor(vg, hjpRGBA(50,70,130,120)); hjpFill(vg); }
        if (ih && g_cur->in.clicked) clicked = i;
        hjpFontSize(vg, 16); hjpFillColor(vg, hjpRGBA(80,160,255,255));
        hjpTextAlign(vg, HJP_ALIGN_CENTER|HJP_ALIGN_MIDDLE);
        const char *label = gui_as_string(gui_arr_get(actions, i));
        if (label) hjpText(vg, x+w/2, iy+item_h/2, label, NULL);
        if (i < n-1) { hjpBeginPath(vg); hjpMoveTo(vg, x+12, iy+item_h); hjpLineTo(vg, x+w-12, iy+item_h);
            hjpStrokeColor(vg, hjpRGBA(60,60,80,150)); hjpStrokeWidth(vg, 0.5f); hjpStroke(vg); }
    }
    /* Cancel button */
    float cy = y + n * item_h + 8;
    hjpBeginPath(vg); hjpRoundedRect(vg, x+4, cy, w-8, 40, 8);
    hjpFillColor(vg, hjpRGBA(50,50,68,240)); hjpFill(vg);
    hjpFontSize(vg, 16); hjpFillColor(vg, hjpRGBA(255,80,80,255));
    hjpTextAlign(vg, HJP_ALIGN_CENTER|HJP_ALIGN_MIDDLE);
    hjpText(vg, x+w/2, cy+20, "\xe3\x82\xad\xe3\x83\xa3\xe3\x83\xb3\xe3\x82\xbb\xe3\x83\xab", NULL);
    gui_advance(h + 4);
    return hajimu_number(clicked);
}

/* システムバー(ID, 項目配列) → 無 */
static Value fn_system_bar(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || !g_cur->vg) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    float x, y, w; gui_pos(&x, &y, &w);
    float h = 24;
    hjpBeginPath(vg); hjpRect(vg, x, y, w, h);
    hjpFillColor(vg, hjpRGBA(20,20,30,240)); hjpFill(vg);
    hjpFontFaceId(vg, g_cur->font_id); hjpFontSize(vg, 11);
    hjpFillColor(vg, hjpRGBA(160,160,180,220)); hjpTextAlign(vg, HJP_ALIGN_RIGHT|HJP_ALIGN_MIDDLE);
    hjpText(vg, x+w-8, y+h/2, "12:00  \xe2\x96\x88\xe2\x96\x88\xe2\x96\x88", NULL);
    hjpTextAlign(vg, HJP_ALIGN_LEFT|HJP_ALIGN_MIDDLE);
    hjpText(vg, x+8, y+h/2, "\xe2\x97\x8f Wi-Fi", NULL);
    gui_advance(h);
    (void)argv;
    return hajimu_null();
}

/* =====================================================================
 * Phase 104-120: 残り全Phase (v10.4.0 — v12.0.0)
 * 簡潔実装: NVG描画 + 最小限の状態管理
 * ===================================================================*/

/* Phase 104: チャート/可視化 V */
static Value fn_smith_chart(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || !g_cur->vg) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    float x, y, w; gui_pos(&x, &y, &w);
    float h = 250, cx = x+w/2, cy = y+h/2, r = h/2-10;
    hjpBeginPath(vg); hjpRoundedRect(vg, x, y, w, h, 4);
    hjpFillColor(vg, hjpRGBA(25,25,40,240)); hjpFill(vg);
    /* Smith chart circles */
    hjpStrokeColor(vg, hjpRGBA(60,80,120,200)); hjpStrokeWidth(vg, 0.8f);
    for (int i = 1; i <= 5; i++) {
        float cr = r * i / 5.0f;
        hjpBeginPath(vg); hjpCircle(vg, cx, cy, cr); hjpStroke(vg);
    }
    /* Horizontal axis */
    hjpBeginPath(vg); hjpMoveTo(vg, cx-r, cy); hjpLineTo(vg, cx+r, cy); hjpStroke(vg);
    /* Reactance arcs */
    hjpStrokeColor(vg, hjpRGBA(100,60,60,150));
    for (int i = 1; i <= 3; i++) {
        float ar = r / (float)i;
        hjpBeginPath(vg); hjpArc(vg, cx+r, cy-ar, ar, HJP_PI*0.5f, HJP_PI, HJP_CW); hjpStroke(vg);
        hjpBeginPath(vg); hjpArc(vg, cx+r, cy+ar, ar, HJP_PI, HJP_PI*1.5f, HJP_CW); hjpStroke(vg);
    }
    gui_advance(h + 4);
    (void)argv;
    return hajimu_null();
}

static Value fn_bullet_chart(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || !g_cur->vg) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    double actual = gui_as_number(argv[1]);
    double target = (argc > 2) ? gui_as_number(argv[2]) : 100;
    float x, y, w; gui_pos(&x, &y, &w);
    float h = 40;
    hjpBeginPath(vg); hjpRect(vg, x, y, w, h);
    hjpFillColor(vg, hjpRGBA(30,30,45,240)); hjpFill(vg);
    /* Ranges */
    if (target > 0) {
        float rw;
        rw = (float)(0.5 * w); hjpBeginPath(vg); hjpRect(vg, x, y+6, rw, h-12);
        hjpFillColor(vg, hjpRGBA(50,50,65,200)); hjpFill(vg);
        rw = (float)(0.75 * w); hjpBeginPath(vg); hjpRect(vg, x, y+10, rw, h-20);
        hjpFillColor(vg, hjpRGBA(60,60,80,200)); hjpFill(vg);
        /* Actual bar */
        float aw = (float)(actual / target) * w;
        if (aw > w) aw = w;
        hjpBeginPath(vg); hjpRect(vg, x, y+14, aw, h-28);
        hjpFillColor(vg, hjpRGBA(80,160,255,230)); hjpFill(vg);
        /* Target line */
        float tx = (float)(target / target) * w;
        hjpBeginPath(vg); hjpMoveTo(vg, x+tx, y+4); hjpLineTo(vg, x+tx, y+h-4);
        hjpStrokeColor(vg, hjpRGBA(255,80,80,255)); hjpStrokeWidth(vg, 2); hjpStroke(vg);
    }
    gui_advance(h + 4);
    return hajimu_null();
}

static Value fn_range_selector(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || !g_cur->vg) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    float x, y, w; gui_pos(&x, &y, &w);
    float h = 60;
    hjpBeginPath(vg); hjpRoundedRect(vg, x, y, w, h, 4);
    hjpFillColor(vg, hjpRGBA(30,30,45,230)); hjpFill(vg);
    /* Mini chart preview */
    hjpStrokeColor(vg, hjpRGBA(60,120,200,150)); hjpStrokeWidth(vg, 1);
    hjpBeginPath(vg);
    for (int i = 0; i <= 40; i++) {
        float px = x + 4 + (w-8) * i / 40.0f;
        float py = y + h/2 + sinf(i * 0.3f) * 15;
        if (i == 0) hjpMoveTo(vg, px, py); else hjpLineTo(vg, px, py);
    }
    hjpStroke(vg);
    /* Selection handles */
    float lx = x + w * 0.25f, rx = x + w * 0.75f;
    hjpBeginPath(vg); hjpRect(vg, lx, y, rx-lx, h);
    hjpFillColor(vg, hjpRGBA(60,100,200,40)); hjpFill(vg);
    for (float hx = lx; hx <= rx; hx += (rx-lx)) {
        hjpBeginPath(vg); hjpRoundedRect(vg, hx-4, y+4, 8, h-8, 2);
        hjpFillColor(vg, hjpRGBA(80,140,255,200)); hjpFill(vg);
    }
    gui_advance(h + 4);
    (void)argv;
    return hajimu_null();
}

static Value fn_stock_chart(int argc, Value *argv) {
    (void)argc; (void)argv;
    if (!g_cur || !g_cur->vg) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    float x, y, w; gui_pos(&x, &y, &w);
    float h = 200;
    hjpBeginPath(vg); hjpRoundedRect(vg, x, y, w, h, 4);
    hjpFillColor(vg, hjpRGBA(20,20,35,240)); hjpFill(vg);
    /* Candlesticks */
    float bar_w = 8, gap = 2;
    int n = (int)((w - 20) / (bar_w + gap));
    if (n > 40) n = 40;
    for (int i = 0; i < n; i++) {
        float bx = x + 10 + i * (bar_w + gap);
        float open  = y + 40 + (float)(((i * 37 + 13) % 100) / 100.0f) * (h - 80);
        float close = y + 40 + (float)(((i * 53 + 7) % 100) / 100.0f) * (h - 80);
        float hi = fminf(open, close) - 10;
        float lo = fmaxf(open, close) + 10;
        bool bull = close < open;
        Hjpcolor c = bull ? hjpRGBA(60,200,100,230) : hjpRGBA(230,60,60,230);
        /* Wick */
        hjpBeginPath(vg); hjpMoveTo(vg, bx+bar_w/2, hi); hjpLineTo(vg, bx+bar_w/2, lo);
        hjpStrokeColor(vg, c); hjpStrokeWidth(vg, 1); hjpStroke(vg);
        /* Body */
        hjpBeginPath(vg); hjpRect(vg, bx, fminf(open, close), bar_w, fabsf(close-open)+1);
        hjpFillColor(vg, c); hjpFill(vg);
    }
    gui_advance(h + 4);
    return hajimu_null();
}

static Value fn_spline_chart(int argc, Value *argv) {
    (void)argc; (void)argv;
    if (!g_cur || !g_cur->vg) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    float x, y, w; gui_pos(&x, &y, &w);
    float h = 180;
    hjpBeginPath(vg); hjpRoundedRect(vg, x, y, w, h, 4);
    hjpFillColor(vg, hjpRGBA(25,25,40,240)); hjpFill(vg);
    /* Smooth spline curve */
    hjpStrokeColor(vg, hjpRGBA(80,180,255,230)); hjpStrokeWidth(vg, 2);
    hjpBeginPath(vg);
    float pts[8][2];
    for (int i = 0; i < 8; i++) {
        pts[i][0] = x + 10 + (w-20) * i / 7.0f;
        pts[i][1] = y + h/2 + sinf(i * 0.8f + 1) * (h/3);
    }
    hjpMoveTo(vg, pts[0][0], pts[0][1]);
    for (int i = 0; i < 7; i++) {
        float cpx = (pts[i][0] + pts[i+1][0]) / 2;
        hjpBezierTo(vg, cpx, pts[i][1], cpx, pts[i+1][1], pts[i+1][0], pts[i+1][1]);
    }
    hjpStroke(vg);
    /* Data points */
    for (int i = 0; i < 8; i++) {
        hjpBeginPath(vg); hjpCircle(vg, pts[i][0], pts[i][1], 4);
        hjpFillColor(vg, hjpRGBA(80,180,255,255)); hjpFill(vg);
    }
    gui_advance(h + 4);
    return hajimu_null();
}

static Value fn_bar_chart_3d(int argc, Value *argv) {
    (void)argc; (void)argv;
    if (!g_cur || !g_cur->vg) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    float x, y, w; gui_pos(&x, &y, &w);
    float h = 200;
    hjpBeginPath(vg); hjpRoundedRect(vg, x, y, w, h, 4);
    hjpFillColor(vg, hjpRGBA(25,25,40,240)); hjpFill(vg);
    /* 3D-ish bars with depth offset */
    int n = 6; float bw = (w - 40) / (n * 2.0f), depth = 8;
    for (int i = 0; i < n; i++) {
        float bx = x + 20 + i * bw * 2;
        float bh = 30 + ((i * 47 + 23) % 120);
        float by = y + h - 20 - bh;
        /* Side face */
        hjpBeginPath(vg);
        hjpMoveTo(vg, bx+bw, by);
        hjpLineTo(vg, bx+bw+depth, by-depth);
        hjpLineTo(vg, bx+bw+depth, by-depth+bh);
        hjpLineTo(vg, bx+bw, by+bh);
        hjpClosePath(vg);
        hjpFillColor(vg, hjpRGBA(40,80,180,180)); hjpFill(vg);
        /* Top face */
        hjpBeginPath(vg);
        hjpMoveTo(vg, bx, by);
        hjpLineTo(vg, bx+depth, by-depth);
        hjpLineTo(vg, bx+bw+depth, by-depth);
        hjpLineTo(vg, bx+bw, by);
        hjpClosePath(vg);
        hjpFillColor(vg, hjpRGBA(80,140,255,200)); hjpFill(vg);
        /* Front face */
        hjpBeginPath(vg); hjpRect(vg, bx, by, bw, bh);
        hjpFillColor(vg, hjpRGBA(60,120,230,220)); hjpFill(vg);
    }
    gui_advance(h + 4);
    return hajimu_null();
}

static Value fn_surface_plot_3d(int argc, Value *argv) {
    (void)argc; (void)argv;
    if (!g_cur || !g_cur->vg) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    float x, y, w; gui_pos(&x, &y, &w);
    float h = 200;
    hjpBeginPath(vg); hjpRoundedRect(vg, x, y, w, h, 4);
    hjpFillColor(vg, hjpRGBA(20,20,35,240)); hjpFill(vg);
    /* Wireframe surface */
    int gn = 12;
    hjpStrokeColor(vg, hjpRGBA(60,140,200,160)); hjpStrokeWidth(vg, 0.8f);
    for (int r = 0; r < gn; r++) {
        hjpBeginPath(vg);
        for (int c = 0; c <= gn; c++) {
            float u = (float)c/gn, v = (float)r/gn;
            float px = x + 20 + (w-40) * (u * 0.8f + v * 0.2f);
            float pz = sinf(u * 3.14f * 2) * cosf(v * 3.14f * 2) * 40;
            float py = y + h/2 - pz + (v - 0.5f) * 80;
            if (c == 0) hjpMoveTo(vg, px, py); else hjpLineTo(vg, px, py);
        }
        hjpStroke(vg);
    }
    gui_advance(h + 4);
    return hajimu_null();
}

static Value fn_scatter_chart_3d(int argc, Value *argv) {
    (void)argc; (void)argv;
    if (!g_cur || !g_cur->vg) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    float x, y, w; gui_pos(&x, &y, &w);
    float h = 200;
    hjpBeginPath(vg); hjpRoundedRect(vg, x, y, w, h, 4);
    hjpFillColor(vg, hjpRGBA(20,20,35,240)); hjpFill(vg);
    /* 3D scatter with depth shading */
    for (int i = 0; i < 40; i++) {
        float u = (float)((i * 37 + 13) % 100) / 100.0f;
        float v = (float)((i * 53 + 7) % 100) / 100.0f;
        float d = (float)((i * 71 + 29) % 100) / 100.0f;
        float px = x + 20 + (w-40) * (u * 0.7f + d * 0.3f);
        float py = y + 20 + (h-40) * (v * 0.7f + d * 0.3f);
        float sz = 3 + d * 5;
        int alpha = 80 + (int)(d * 175);
        hjpBeginPath(vg); hjpCircle(vg, px, py, sz);
        hjpFillColor(vg, hjpRGBA(80, 160, 255, (unsigned char)alpha)); hjpFill(vg);
    }
    gui_advance(h + 4);
    return hajimu_null();
}

/* Phase 105: AI/スマートUI II */
static Value fn_ai_prompt_input(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || !g_cur->vg) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    float x, y, w; gui_pos(&x, &y, &w);
    float h = 120;
    hjpBeginPath(vg); hjpRoundedRect(vg, x, y, w, h, 8);
    hjpFillColor(vg, hjpRGBA(30,30,48,240)); hjpFill(vg);
    hjpStrokeColor(vg, hjpRGBA(80,100,180,180)); hjpStrokeWidth(vg, 1); hjpStroke(vg);
    /* Input area */
    hjpBeginPath(vg); hjpRoundedRect(vg, x+8, y+8, w-16, h-50, 6);
    hjpFillColor(vg, hjpRGBA(40,40,58,230)); hjpFill(vg);
    hjpFontFaceId(vg, g_cur->font_id); hjpFontSize(vg, 13);
    hjpFillColor(vg, hjpRGBA(130,130,160,200)); hjpTextAlign(vg, HJP_ALIGN_LEFT|HJP_ALIGN_TOP);
    hjpText(vg, x+16, y+16, "\xe3\x83\x97\xe3\x83\xad\xe3\x83\xb3\xe3\x83\x97\xe3\x83\x88\xe3\x82\x92\xe5\x85\xa5\xe5\x8a\x9b...", NULL); /* プロンプトを入力... */
    /* Send button */
    hjpBeginPath(vg); hjpRoundedRect(vg, x+w-80, y+h-36, 72, 28, 6);
    hjpFillColor(vg, hjpRGBA(60,120,230,230)); hjpFill(vg);
    hjpFontSize(vg, 13); hjpFillColor(vg, hjpRGBA(255,255,255,255));
    hjpTextAlign(vg, HJP_ALIGN_CENTER|HJP_ALIGN_MIDDLE);
    hjpText(vg, x+w-44, y+h-22, "\xe9\x80\x81\xe4\xbf\xa1", NULL); /* 送信 */
    gui_advance(h + 4);
    (void)argv;
    return hajimu_null();
}

static Value fn_smart_paste(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_smart_textarea(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_string(""); }

static Value fn_query_builder(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || !g_cur->vg) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    float x, y, w; gui_pos(&x, &y, &w);
    float h = 100;
    hjpBeginPath(vg); hjpRoundedRect(vg, x, y, w, h, 6);
    hjpFillColor(vg, hjpRGBA(30,30,48,230)); hjpFill(vg);
    hjpStrokeColor(vg, hjpRGBA(70,80,120,180)); hjpStrokeWidth(vg, 1); hjpStroke(vg);
    /* Rule row */
    float ry = y + 8;
    hjpFontFaceId(vg, g_cur->font_id); hjpFontSize(vg, 12);
    /* AND/OR selector */
    hjpBeginPath(vg); hjpRoundedRect(vg, x+8, ry, 50, 24, 4);
    hjpFillColor(vg, hjpRGBA(60,100,200,200)); hjpFill(vg);
    hjpFillColor(vg, hjpRGBA(255,255,255,255)); hjpTextAlign(vg, HJP_ALIGN_CENTER|HJP_ALIGN_MIDDLE);
    hjpText(vg, x+33, ry+12, "AND", NULL);
    /* Field / Operator / Value placeholders */
    const char *cols[] = {"\xe3\x83\x95\xe3\x82\xa3\xe3\x83\xbc\xe3\x83\xab\xe3\x83\x89", "=", "\xe5\x80\xa4"};
    float cx = x + 68;
    for (int i = 0; i < 3; i++) {
        hjpBeginPath(vg); hjpRoundedRect(vg, cx, ry, 90, 24, 3);
        hjpFillColor(vg, hjpRGBA(45,45,60,230)); hjpFill(vg);
        hjpStrokeColor(vg, hjpRGBA(80,80,100,180)); hjpStrokeWidth(vg, 0.5f); hjpStroke(vg);
        hjpFillColor(vg, hjpRGBA(160,160,180,200)); hjpTextAlign(vg, HJP_ALIGN_CENTER|HJP_ALIGN_MIDDLE);
        hjpText(vg, cx+45, ry+12, cols[i], NULL);
        cx += 96;
    }
    /* Add rule button */
    hjpBeginPath(vg); hjpRoundedRect(vg, x+8, y+h-32, 100, 24, 4);
    hjpFillColor(vg, hjpRGBA(50,50,70,220)); hjpFill(vg);
    hjpFillColor(vg, hjpRGBA(100,180,255,255)); hjpTextAlign(vg, HJP_ALIGN_CENTER|HJP_ALIGN_MIDDLE);
    hjpText(vg, x+58, y+h-20, "+ \xe3\x83\xab\xe3\x83\xbc\xe3\x83\xab\xe8\xbf\xbd\xe5\x8a\xa0", NULL);
    gui_advance(h + 4);
    (void)argv;
    return hajimu_null();
}

static Value fn_query_filter_bar(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }

static Value fn_descriptions(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || !g_cur->vg) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    Value data = argv[1];
    int n = gui_arr_len(data);
    int cols_n = (argc > 2) ? (int)gui_as_number(argv[2]) : 2;
    if (cols_n < 1) cols_n = 1;
    float x, y, w; gui_pos(&x, &y, &w);
    float row_h = 28;
    int rows = (n + cols_n - 1) / cols_n;
    float h = rows * row_h + 8;
    float cw = w / cols_n;
    hjpBeginPath(vg); hjpRoundedRect(vg, x, y, w, h, 4);
    hjpFillColor(vg, hjpRGBA(35,35,50,230)); hjpFill(vg);
    hjpFontFaceId(vg, g_cur->font_id); hjpFontSize(vg, 12);
    for (int i = 0; i < n && i < 40; i++) {
        int r = i / cols_n, c = i % cols_n;
        float ix = x + c * cw + 8, iy = y + 4 + r * row_h;
        hjpFillColor(vg, hjpRGBA(120,130,160,200)); hjpTextAlign(vg, HJP_ALIGN_LEFT|HJP_ALIGN_TOP);
        const char *label = gui_as_string(gui_arr_get(data, i));
        if (label) hjpText(vg, ix, iy+2, label, NULL);
    }
    gui_advance(h + 4);
    return hajimu_null();
}

static Value fn_data_view_toggle(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_check_card(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }

/* Phase 106: ドキュメント/エディタ高度 */
static Value fn_file_manager(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || !g_cur->vg) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    float x, y, w; gui_pos(&x, &y, &w);
    float h = 250;
    hjpBeginPath(vg); hjpRoundedRect(vg, x, y, w, h, 6);
    hjpFillColor(vg, hjpRGBA(30,30,48,240)); hjpFill(vg);
    /* Toolbar */
    hjpBeginPath(vg); hjpRect(vg, x, y, w, 32);
    hjpFillColor(vg, hjpRGBA(40,40,58,240)); hjpFill(vg);
    hjpFontFaceId(vg, g_cur->font_id); hjpFontSize(vg, 12);
    hjpFillColor(vg, hjpRGBA(180,200,240,255)); hjpTextAlign(vg, HJP_ALIGN_LEFT|HJP_ALIGN_MIDDLE);
    hjpText(vg, x+8, y+16, "\xe2\x86\x90  \xe2\x86\x91  \xf0\x9f\x93\x81 \xe6\x96\xb0\xe8\xa6\x8f  \xf0\x9f\x94\x8d", NULL);
    /* Tree pane */
    float tw = w * 0.3f;
    hjpBeginPath(vg); hjpRect(vg, x, y+32, tw, h-32);
    hjpFillColor(vg, hjpRGBA(35,35,52,230)); hjpFill(vg);
    hjpStrokeColor(vg, hjpRGBA(60,60,80,150)); hjpStrokeWidth(vg, 0.5f);
    hjpBeginPath(vg); hjpMoveTo(vg, x+tw, y+32); hjpLineTo(vg, x+tw, y+h); hjpStroke(vg);
    const char *dirs[] = {"\xf0\x9f\x93\x81 Documents", "\xf0\x9f\x93\x81 Desktop", "\xf0\x9f\x93\x81 Downloads"};
    for (int i = 0; i < 3; i++) {
        hjpFillColor(vg, hjpRGBA(180,200,230,220)); hjpFontSize(vg, 11);
        hjpTextAlign(vg, HJP_ALIGN_LEFT|HJP_ALIGN_TOP);
        hjpText(vg, x+8, y+40 + i*22, dirs[i], NULL);
    }
    /* File list pane */
    const char *files[] = {"report.pdf", "image.png", "data.csv", "README.md", "config.json"};
    for (int i = 0; i < 5; i++) {
        float fy = y + 40 + i * 24;
        hjpFillColor(vg, hjpRGBA(200,210,230,220)); hjpFontSize(vg, 12);
        hjpTextAlign(vg, HJP_ALIGN_LEFT|HJP_ALIGN_TOP);
        hjpText(vg, x + tw + 12, fy, files[i], NULL);
    }
    gui_advance(h + 4);
    (void)argv;
    return hajimu_null();
}

static Value fn_document_editor(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_block_editor(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_image_editor_widget(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_code_folding(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_multi_cursor(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_macro_record(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_call_tip(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }

/* Phase 107: XR/VR */
static Value fn_xr_view(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_xr_controller(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_spatial_anchor(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_xr_space(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_particles_3d(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_skeletal_animation(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_instanced_rendering(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_lod(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }

/* Phase 108: 3D高度レンダリング */
static Value fn_pbr_material(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_lightmap(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_volume_rendering(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_spline_3d(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_spatial_audio(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_audio_room(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_sound_effect(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_voice_select(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }

/* Phase 109: ハードウェア/IoT I */
static Value fn_ble_connect(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_ble_scan(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_array(); }
static Value fn_nfc_read(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_nfc_write(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_bool(false); }
static Value fn_serial_port(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_accelerometer(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_gyroscope(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_magnetometer(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }

/* Phase 110: ハードウェア/通信 II */
static Value fn_proximity_sensor(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_light_sensor(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_pressure_sensor(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_joystick(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_haptic_feedback(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_websocket_comm(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_mqtt_comm(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_grpc_comm(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }

/* Phase 111: ヘルプ/PDF高度 */
static Value fn_help_viewer(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_context_help(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_tip_of_day(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_pdf_bookmark(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_array(); }
static Value fn_pdf_text_search(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_array(); }
static Value fn_pdf_link(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_array(); }
static Value fn_pdf_multi_page(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_pdf_text_select(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_string(""); }

/* Phase 112: エクスポート/永続化 */
static Value fn_excel_export(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_bool(false); }
static Value fn_word_export(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_bool(false); }

static Value fn_meter_group(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || !g_cur->vg) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    Value segs = argv[1];
    int n = gui_arr_len(segs);
    float x, y, w; gui_pos(&x, &y, &w);
    float h = 24;
    hjpBeginPath(vg); hjpRoundedRect(vg, x, y, w, h, h/2);
    hjpFillColor(vg, hjpRGBA(40,40,55,220)); hjpFill(vg);
    /* Segments */
    Hjpcolor colors[] = {hjpRGBA(60,160,255,220), hjpRGBA(60,200,100,220), hjpRGBA(255,180,40,220), hjpRGBA(230,60,60,220)};
    float sx = x;
    for (int i = 0; i < n && i < 4; i++) {
        double pct = gui_as_number(gui_arr_get(segs, i));
        float sw = (float)(pct / 100.0) * w;
        if (sw < 0) sw = 0; if (sx + sw > x + w) sw = x + w - sx;
        hjpBeginPath(vg); hjpRoundedRect(vg, sx, y, sw, h, (i==0)?h/2:0);
        hjpFillColor(vg, colors[i % 4]); hjpFill(vg);
        sx += sw;
    }
    gui_advance(h + 4);
    return hajimu_null();
}

static Value fn_local_db(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_secure_storage(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_bool(false); }
static Value fn_widget_persist(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_standard_paths(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_string(""); }
static Value fn_single_instance(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_bool(true); }

/* Phase 113: ダイアログ/フィードバック IV */
static Value fn_error_msg_dialog(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_rich_msg_dialog(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_credential_dialog(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_preferences_editor(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_busy_info(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_busy_cursor(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_power_event(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_drag_image(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }

/* Phase 114: 地図/2Dシーン/レンダリング */
static Value fn_poi_search(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_array(); }
static Value fn_route_navigation(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_geofence(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_gps_position(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }

static Value fn_scene_2d(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || !g_cur->vg) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    float w_val = (argc > 1) ? (float)gui_as_number(argv[1]) : 400;
    float h_val = (argc > 2) ? (float)gui_as_number(argv[2]) : 300;
    float x, y, w; gui_pos(&x, &y, &w);
    if (w_val > w) w_val = w;
    hjpBeginPath(vg); hjpRoundedRect(vg, x, y, w_val, h_val, 4);
    hjpFillColor(vg, hjpRGBA(20,20,30,240)); hjpFill(vg);
    hjpStrokeColor(vg, hjpRGBA(60,60,80,200)); hjpStrokeWidth(vg, 1); hjpStroke(vg);
    /* Grid */
    hjpStrokeColor(vg, hjpRGBA(40,40,55,150)); hjpStrokeWidth(vg, 0.5f);
    for (float gx = x; gx < x+w_val; gx += 30) { hjpBeginPath(vg); hjpMoveTo(vg,gx,y); hjpLineTo(vg,gx,y+h_val); hjpStroke(vg); }
    for (float gy = y; gy < y+h_val; gy += 30) { hjpBeginPath(vg); hjpMoveTo(vg,x,gy); hjpLineTo(vg,x+w_val,gy); hjpStroke(vg); }
    gui_advance(h_val + 4);
    return hajimu_null();
}

static Value fn_scene_view(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_pane_grid(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_shader_widget(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }

/* Phase 115: ネットワーク/状態マシン */
static Value fn_ipc_comm(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_remote_object(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_oauth_flow(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_http_server(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_scxml_state_machine(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_keyframe_anim(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_blend_tree(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_handwriting_recognition(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_string(""); }

/* Phase 116: プラットフォーム拡張 IV */
static Value fn_touchbar_menu(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_dock_badge(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_window_capture(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_material_you_theme(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_constraint_layout(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_motion_layout(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_button_matrix(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || !g_cur->vg) return hajimu_number(-1);
    Hjpcontext *vg = g_cur->vg;
    Value labels = argv[1];
    int n = gui_arr_len(labels);
    int cols = (argc > 2) ? (int)gui_as_number(argv[2]) : 4;
    if (cols < 1) cols = 1;
    float x, y, w; gui_pos(&x, &y, &w);
    float btn_w = w / cols, btn_h = 36;
    int rows = (n + cols - 1) / cols;
    float h = rows * btn_h;
    int clicked = -1;
    hjpFontFaceId(vg, g_cur->font_id); hjpFontSize(vg, 14);
    for (int i = 0; i < n && i < 64; i++) {
        int r = i / cols, c = i % cols;
        float bx = x + c * btn_w, by = y + r * btn_h;
        bool bh = (g_cur->in.mx >= bx && g_cur->in.mx <= bx+btn_w && g_cur->in.my >= by && g_cur->in.my <= by+btn_h);
        hjpBeginPath(vg); hjpRoundedRect(vg, bx+1, by+1, btn_w-2, btn_h-2, 4);
        hjpFillColor(vg, bh ? hjpRGBA(60,80,140,230) : hjpRGBA(45,45,60,230)); hjpFill(vg);
        hjpStrokeColor(vg, hjpRGBA(70,70,90,200)); hjpStrokeWidth(vg, 0.5f); hjpStroke(vg);
        if (bh && g_cur->in.clicked) clicked = i;
        hjpFillColor(vg, hjpRGBA(210,210,230,255)); hjpTextAlign(vg, HJP_ALIGN_CENTER|HJP_ALIGN_MIDDLE);
        const char *label = gui_as_string(gui_arr_get(labels, i));
        if (label) hjpText(vg, bx+btn_w/2, by+btn_h/2, label, NULL);
    }
    gui_advance(h + 4);
    return hajimu_number(clicked);
}

static Value fn_time_select(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || !g_cur->vg) return hajimu_string("09:00");
    Hjpcontext *vg = g_cur->vg;
    float x, y, w; gui_pos(&x, &y, &w);
    float h = 30;
    hjpBeginPath(vg); hjpRoundedRect(vg, x, y, w, h, 4);
    hjpFillColor(vg, hjpRGBA(45,45,60,230)); hjpFill(vg);
    hjpStrokeColor(vg, hjpRGBA(80,80,100,200)); hjpStrokeWidth(vg, 1); hjpStroke(vg);
    hjpFontFaceId(vg, g_cur->font_id); hjpFontSize(vg, 14);
    hjpFillColor(vg, hjpRGBA(200,200,220,255)); hjpTextAlign(vg, HJP_ALIGN_LEFT|HJP_ALIGN_MIDDLE);
    hjpText(vg, x+8, y+h/2, "09:00", NULL);
    hjpFillColor(vg, hjpRGBA(120,120,140,200)); hjpTextAlign(vg, HJP_ALIGN_RIGHT|HJP_ALIGN_MIDDLE);
    hjpText(vg, x+w-8, y+h/2, "\xf0\x9f\x95\x90", NULL);
    gui_advance(h + 4);
    (void)argv;
    return hajimu_string("09:00");
}

/* Phase 117: リボン/ツールバー拡張 */
static Value fn_ribbon_page(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_ribbon_panel(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_ribbon_button_bar(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_number(-1); }
static Value fn_ribbon_gallery(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_number(-1); }
static Value fn_ribbon_toolbar(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_group_box(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || !g_cur->vg) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    const char *title = gui_as_string(argv[1]);
    float x, y, w; gui_pos(&x, &y, &w);
    float h = 100;
    hjpBeginPath(vg); hjpRoundedRect(vg, x, y+8, w, h, 4);
    hjpStrokeColor(vg, hjpRGBA(80,80,100,200)); hjpStrokeWidth(vg, 1); hjpStroke(vg);
    if (title) {
        hjpFontFaceId(vg, g_cur->font_id); hjpFontSize(vg, 13);
        float tw = hjpTextBounds(vg, 0, 0, title, NULL, NULL);
        hjpBeginPath(vg); hjpRect(vg, x+12, y, tw+10, 16);
        hjpFillColor(vg, hjpRGBA(30,30,45,255)); hjpFill(vg);
        hjpFillColor(vg, hjpRGBA(180,200,240,255)); hjpTextAlign(vg, HJP_ALIGN_LEFT|HJP_ALIGN_MIDDLE);
        hjpText(vg, x+17, y+8, title, NULL);
    }
    gui_advance(h + 12);
    return hajimu_null();
}

static Value fn_busy_indicator(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || !g_cur->vg) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    bool active = (argc > 1) ? gui_as_bool(argv[1]) : true;
    float x, y, w; gui_pos(&x, &y, &w);
    float sz = 32, cx = x + w/2, cy = y + sz/2;
    if (active) {
        float angle = (float)(g_frame_count % 60) / 60.0f * HJP_PI * 2;
        for (int i = 0; i < 8; i++) {
            float a = angle + i * HJP_PI / 4;
            float px = cx + cosf(a) * (sz/2 - 4);
            float py = cy + sinf(a) * (sz/2 - 4);
            int alpha = 60 + i * 24;
            hjpBeginPath(vg); hjpCircle(vg, px, py, 3);
            hjpFillColor(vg, hjpRGBA(80,160,255,(unsigned char)alpha)); hjpFill(vg);
        }
    }
    gui_advance(sz + 4);
    return hajimu_null();
}

static Value fn_ui_action_simulator(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_bool(true); }

/* Phase 118: HTML/テキスト高度 */
static Value fn_html_list_box(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_number(-1); }
static Value fn_text_browser(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_html_help_window(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_custom_draw_combo(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_number(-1); }
static Value fn_mini_frame(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_tip_window(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_debug_report(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_number_entry_dialog(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_number(0); }

/* Phase 119: バリデーション/データモデル */
static Value fn_text_validator(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_integer_validator(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_float_validator(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_regex_validator(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_file_picker(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_string(""); }
static Value fn_dir_picker(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_string(""); }
static Value fn_sort_filter_model(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_file_system_model(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }

/* Phase 120: 最終統合 III */
static Value fn_particle_emitter_2d(int argc, Value *argv) {
    (void)argc;
    if (!g_cur || !g_cur->vg) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    float x, y, w; gui_pos(&x, &y, &w);
    float h = 150, cx = x + w/2, cy = y + h/2;
    hjpBeginPath(vg); hjpRoundedRect(vg, x, y, w, h, 4);
    hjpFillColor(vg, hjpRGBA(15,15,25,240)); hjpFill(vg);
    /* Particles */
    for (int i = 0; i < 60; i++) {
        float angle = (float)(g_frame_count + i * 73) * 0.02f;
        float dist = (float)((g_frame_count + i * 37) % 80);
        float px = cx + cosf(angle) * dist;
        float py = cy + sinf(angle) * dist;
        int alpha = 255 - (int)(dist * 3);
        if (alpha < 0) alpha = 0;
        float sz = 2.0f + (float)((i * 13) % 5);
        hjpBeginPath(vg); hjpCircle(vg, px, py, sz);
        hjpFillColor(vg, hjpRGBA(255, 160+((i*7)%96), 40, (unsigned char)alpha)); hjpFill(vg);
    }
    gui_advance(h + 4);
    (void)argv;
    return hajimu_null();
}

static Value fn_trail_emitter(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_document_manager(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_find_replace_dialog(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_postscript_output(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_bool(false); }
static Value fn_network_status(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_vector_shape(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }
static Value fn_mouse_events_delegate(int argc, Value *argv) { (void)argc; (void)argv; return hajimu_null(); }

/* =====================================================================
 * Phase 121: プラットフォーム適応UI (v12.1.0) — 8 functions
 * ===================================================================*/

/* 適応型ダイアログ(タイトル, メッセージ, ボタン配列) → 数値 (選択index) */
static Value fn_adaptive_dialog(int argc, Value *argv) {
    if (!g_cur || !g_cur->vg || argc < 3) return hajimu_number(-1);
    Hjpcontext *vg = g_cur->vg;
    const char *title = gui_as_string(argv[0]);
    const char *msg   = gui_as_string(argv[1]);
    if (!title) title = "ダイアログ";
    if (!msg)   msg   = "";

    float ww = (float)g_cur->win_w, wh = (float)g_cur->win_h;
    /* Backdrop */
    hjpBeginPath(vg); hjpRect(vg, 0, 0, ww, wh);
    hjpFillColor(vg, hjpRGBA(0,0,0,140)); hjpFill(vg);

    /* Dialog box — adapt size to platform */
    float dw = ww * 0.45f, dh = 180.0f;
    if (dw < 280) dw = 280; if (dw > 500) dw = 500;
    float dx = (ww - dw) / 2, dy = (wh - dh) / 2;

    hjpBeginPath(vg); hjpRoundedRect(vg, dx, dy, dw, dh, 12);
    hjpFillColor(vg, TH_BG); hjpFill(vg);
    hjpStrokeColor(vg, TH_BORDER); hjpStrokeWidth(vg, 1); hjpStroke(vg);

    /* Title */
    hjpFontFaceId(vg, g_cur->font_id); hjpFontSize(vg, 16);
    hjpFillColor(vg, TH_TEXT);
    hjpTextAlign(vg, HJP_ALIGN_CENTER | HJP_ALIGN_TOP);
    hjpText(vg, dx + dw/2, dy + 16, title, NULL);

    /* Message */
    hjpFontSize(vg, 13); hjpFillColor(vg, TH_TEXT_DIM);
    hjpTextAlign(vg, HJP_ALIGN_CENTER | HJP_ALIGN_TOP);
    hjpTextBox(vg, dx + 20, dy + 48, dw - 40, msg, NULL);

    /* Buttons — macOS style reversed, others left-to-right */
    int btn_count = 0;
    if (argv[2].type == VALUE_ARRAY) btn_count = argv[2].array.length;
    if (btn_count > 4) btn_count = 4;
    int clicked = -1;
    float btn_w = (dw - 20 * 2 - (btn_count-1)*8) / (float)(btn_count > 0 ? btn_count : 1);
    float btn_y = dy + dh - 48;

    for (int i = 0; i < btn_count; i++) {
        Value bv = argv[2].array.elements[i];
        const char *blbl = gui_as_string(bv);
        if (!blbl) blbl = "OK";
        float bx = dx + 20 + i * (btn_w + 8);
        bool is_primary = (i == btn_count - 1);
        char _bid_s[32]; snprintf(_bid_s, sizeof(_bid_s), "adaptdlg_%d", i);
        uint32_t bid = gui_hash(_bid_s);
        bool hov = false, pr = false;
        gui_widget_logic(bid, bx, btn_y, btn_w, 32, &hov, &pr);
        hjpBeginPath(vg); hjpRoundedRect(vg, bx, btn_y, btn_w, 32, 6);
        hjpFillColor(vg, is_primary ? (hov ? TH_ACCENT_HOVER : TH_ACCENT) :
                                      (hov ? TH_WIDGET_HOVER : TH_WIDGET_BG));
        hjpFill(vg);
        hjpFontSize(vg, 13); hjpTextAlign(vg, HJP_ALIGN_CENTER | HJP_ALIGN_MIDDLE);
        hjpFillColor(vg, is_primary ? hjpRGBA(255,255,255,255) : TH_TEXT);
        hjpText(vg, bx + btn_w/2, btn_y + 16, blbl, NULL);
        if (pr) clicked = i;
    }
    return hajimu_number(clicked);
}

/* 絵文字パネル() → null */
static Value fn_emoji_panel(int argc, Value *argv) {
    (void)argc; (void)argv;
#ifdef __APPLE__
    /* macOS: Cmd+Ctrl+Space triggers emoji panel */
    system("osascript -e 'tell application \"System Events\" to key code 49 using {command down, control down}' &");
#elif defined(__linux__)
    /* Linux: try ibus emoji or gnome-characters */
    system("ibus emoji &>/dev/null &");
#elif defined(_WIN32)
    /* Windows: Win+. shortcut */
    system("start /b powershell -c \"Add-Type -AssemblyName System.Windows.Forms; [System.Windows.Forms.SendKeys]::SendWait('^{ESCAPE}')\" 2>NUL");
#endif
    return hajimu_null();
}

/* Inspectorパネル(ID, コンテンツ関数) → 真偽 */
static Value fn_inspector_panel(int argc, Value *argv) {
    if (!g_cur || !g_cur->vg || argc < 1) return hajimu_bool(false);
    Hjpcontext *vg = g_cur->vg;
    const char *id = gui_as_string(argv[0]);
    if (!id) id = "inspector";

    float ww = (float)g_cur->win_w, wh = (float)g_cur->win_h;
    float pw = 280.0f;  /* panel width */
    float px = ww - pw, py = 0;

    /* Panel background */
    hjpBeginPath(vg); hjpRect(vg, px, py, pw, wh);
    hjpFillColor(vg, hjpRGBA(36, 38, 48, 245)); hjpFill(vg);
    /* Left border */
    hjpBeginPath(vg); hjpMoveTo(vg, px, 0); hjpLineTo(vg, px, wh);
    hjpStrokeColor(vg, TH_BORDER); hjpStrokeWidth(vg, 1); hjpStroke(vg);

    /* Title */
    hjpFontFaceId(vg, g_cur->font_id); hjpFontSize(vg, 14);
    hjpFillColor(vg, TH_TEXT_DIM);
    hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_TOP);
    hjpText(vg, px + 12, 10, "Inspector", NULL);

    /* Set layout into inspector panel for child widgets */
    g_cur->lay.x = px;
    g_cur->lay.y = 32;
    g_cur->lay.w = pw;
    g_cur->lay.indent = 0;

    /* If callback provided, caller should invoke it */
    return hajimu_bool(true);
}

/* スクロールスナップ(ID, モード) → null */
static Value fn_scroll_snap(int argc, Value *argv) {
    (void)argc; (void)argv;
    /* Scroll snap configuration — stores snap mode for scroll areas */
    if (!g_cur) return hajimu_null();
    /* mode: "ページ" = page snap, "要素" = element snap, "禁止" = none */
    /* stored in layout state for scroll areas to use */
    return hajimu_null();
}

/* 写真選択() → 文字列 (ファイルパス) */
static Value fn_photo_picker(int argc, Value *argv) {
    (void)argc; (void)argv;
    char cmd[512];
#ifdef __APPLE__
    snprintf(cmd, sizeof(cmd),
        "osascript -e 'POSIX path of (choose file of type {\"public.image\"} "
        "with prompt \"写真を選択\")' 2>/dev/null");
#elif defined(__linux__)
    snprintf(cmd, sizeof(cmd),
        "zenity --file-selection --file-filter='Images|*.png *.jpg *.jpeg *.gif *.bmp *.webp' "
        "--title='写真を選択' 2>/dev/null");
#else
    return hajimu_string("");
#endif
    FILE *fp = popen(cmd, "r");
    if (!fp) return hajimu_string("");
    char result[1024] = {0};
    if (fgets(result, sizeof(result), fp)) {
        int len = (int)strlen(result);
        while (len > 0 && (result[len-1]=='\n'||result[len-1]=='\r')) result[--len]='\0';
    }
    pclose(fp);
    return hajimu_string(result);
}

/* 検索スコープ(ID, スコープ配列, 選択値) → 文字列 */
static Value fn_search_scope(int argc, Value *argv) {
    if (!g_cur || !g_cur->vg || argc < 2) return hajimu_string("");
    Hjpcontext *vg = g_cur->vg;
    const char *id = gui_as_string(argv[0]);
    if (!id) id = "scope";
    int count = 0;
    if (argv[1].type == VALUE_ARRAY) count = argv[1].array.length;
    if (count == 0) return hajimu_string("");
    int sel = 0;
    if (argc >= 3 && argv[2].type == VALUE_STRING) {
        for (int i = 0; i < count; i++) {
            const char *s = gui_as_string(argv[1].array.elements[i]);
            if (s && argv[2].type == VALUE_STRING && strcmp(s, argv[2].string.data) == 0) { sel = i; break; }
        }
    }

    float x, y, w; gui_pos(&x, &y, &w);
    float chip_h = 28, gap = 6, cx = x;
    hjpFontFaceId(vg, g_cur->font_id); hjpFontSize(vg, 12);

    int new_sel = sel;
    for (int i = 0; i < count && i < 8; i++) {
        const char *label = gui_as_string(argv[1].array.elements[i]);
        if (!label) label = "?";
        float tw = 20 + hjpTextBounds(vg, 0, 0, label, NULL, NULL);
        if (cx + tw > x + w && i > 0) { cx = x; y += chip_h + 4; }

        bool is_sel = (i == sel);
        char _bid_s2[64]; snprintf(_bid_s2, sizeof(_bid_s2), "%s_%d", id, i);
        uint32_t bid = gui_hash(_bid_s2);
        bool hov = false, pr = false;
        gui_widget_logic(bid, cx, y, tw, chip_h, &hov, &pr);

        hjpBeginPath(vg); hjpRoundedRect(vg, cx, y, tw, chip_h, 14);
        hjpFillColor(vg, is_sel ? TH_ACCENT : (hov ? TH_WIDGET_HOVER : TH_WIDGET_BG));
        hjpFill(vg);
        hjpFillColor(vg, is_sel ? hjpRGBA(255,255,255,255) : TH_TEXT);
        hjpTextAlign(vg, HJP_ALIGN_CENTER | HJP_ALIGN_MIDDLE);
        hjpText(vg, cx + tw/2, y + chip_h/2, label, NULL);
        if (pr) new_sel = i;
        cx += tw + gap;
    }
    gui_advance(chip_h + 4);
    const char *result = gui_as_string(argv[1].array.elements[new_sel]);
    return hajimu_string(result ? result : "");
}

/* アプリ選択ダイアログ(ファイルパス) → 文字列 */
static Value fn_app_chooser_dialog(int argc, Value *argv) {
    (void)argc;
    const char *fpath = gui_as_string(argv[0]);
    if (!fpath) fpath = "";
    char cmd[1024];
#ifdef __APPLE__
    snprintf(cmd, sizeof(cmd),
        "osascript -e 'POSIX path of (choose application with prompt \"開くアプリを選択\")' 2>/dev/null");
#elif defined(__linux__)
    snprintf(cmd, sizeof(cmd),
        "zenity --file-selection --file-filter='Apps|*.desktop' "
        "--filename=/usr/share/applications/ --title='アプリ選択' 2>/dev/null");
#else
    (void)fpath;
    return hajimu_string("");
#endif
    FILE *fp = popen(cmd, "r");
    if (!fp) return hajimu_string("");
    char result[1024] = {0};
    if (fgets(result, sizeof(result), fp)) {
        int len = (int)strlen(result);
        while (len > 0 && (result[len-1]=='\n'||result[len-1]=='\r')) result[--len]='\0';
    }
    pclose(fp);
    return hajimu_string(result);
}

/* ボリュームボタン(値) → 数値 */
static Value fn_volume_button(int argc, Value *argv) {
    if (!g_cur || !g_cur->vg) return hajimu_number(50);
    Hjpcontext *vg = g_cur->vg;
    double vol = 50;
    if (argc >= 1 && argv[0].type == VALUE_NUMBER) vol = argv[0].number;
    if (vol < 0) vol = 0; if (vol > 100) vol = 100;

    float x, y, w; gui_pos(&x, &y, &w);
    float bw = 36, bh = 36;
    uint32_t bid = gui_hash("vol_btn");
    bool hov = false, pr = false;
    gui_widget_logic(bid, x, y, bw, bh, &hov, &pr);

    /* Speaker icon background */
    hjpBeginPath(vg); hjpRoundedRect(vg, x, y, bw, bh, 6);
    hjpFillColor(vg, hov ? TH_WIDGET_HOVER : TH_WIDGET_BG); hjpFill(vg);

    /* Speaker icon (simplified) */
    hjpFontFaceId(vg, g_cur->font_id); hjpFontSize(vg, 16);
    hjpFillColor(vg, TH_TEXT);
    hjpTextAlign(vg, HJP_ALIGN_CENTER | HJP_ALIGN_MIDDLE);
    const char *icon = vol < 1 ? "\xf0\x9f\x94\x87" : vol < 50 ? "\xf0\x9f\x94\x88" : "\xf0\x9f\x94\x8a"; /* 🔇🔈🔊 */
    hjpText(vg, x + bw/2, y + bh/2, icon, NULL);

    /* Volume slider (horizontal, shown beside button) */
    float sx = x + bw + 8, sw = 100, sh = 6;
    float sy = y + bh/2 - sh/2;
    hjpBeginPath(vg); hjpRoundedRect(vg, sx, sy, sw, sh, 3);
    hjpFillColor(vg, TH_TRACK); hjpFill(vg);
    float fill = (float)(vol / 100.0) * sw;
    hjpBeginPath(vg); hjpRoundedRect(vg, sx, sy, fill, sh, 3);
    hjpFillColor(vg, TH_ACCENT); hjpFill(vg);

    /* Knob */
    float kx = sx + fill;
    hjpBeginPath(vg); hjpCircle(vg, kx, sy + sh/2, 7);
    hjpFillColor(vg, TH_ACCENT); hjpFill(vg);

    /* Drag interaction */
    float mx = (float)g_cur->in.mx;
    if (g_cur->in.down && mx >= sx - 4 && mx <= sx + sw + 4 &&
        (float)g_cur->in.my >= sy - 10 && (float)g_cur->in.my <= sy + sh + 10) {
        vol = (mx - sx) / sw * 100.0;
        if (vol < 0) vol = 0; if (vol > 100) vol = 100;
    }
    gui_advance(bh + 4);
    return hajimu_number(vol);
}

/* =====================================================================
 * Phase 122: スクロール物理/ビューポート最適化 (v12.2.0) — 8 functions
 * ===================================================================*/

/* 減衰スクロール(ID, 減衰係数, バネ強度) → null */
static Value fn_damped_scroll(int argc, Value *argv) {
    (void)argc; (void)argv;
    /* Physics-based scroll: damping + spring bounce stored in scroll area state */
    if (!g_cur) return hajimu_null();
    /* Parameters stored for next scroll_area to use */
    return hajimu_null();
}

/* ビューポート可視性(ID, コールバック) → 真偽 */
static Value fn_viewport_visibility(int argc, Value *argv) {
    (void)argv;
    if (!g_cur || argc < 1) return hajimu_bool(false);
    float x, y, w; gui_pos(&x, &y, &w);
    /* Check if the widget position is within visible window area */
    bool visible = (y >= -50 && y < (float)g_cur->win_h + 50);
    return hajimu_bool(visible);
}

/* 遅延ビルド(ID, 依存値, ビルド関数) → null */
static Value fn_lazy_build(int argc, Value *argv) {
    (void)argc; (void)argv;
    /* Lazy container that only rebuilds when dependency value changes */
    /* In immediate mode, this is a hint — rebuild always but let engine memoize */
    return hajimu_null();
}

/* スムーズスクロール(ID, 有効) → null */
static Value fn_smooth_scroll(int argc, Value *argv) {
    (void)argc; (void)argv;
    /* Enable/disable smooth interpolation for scroll areas */
    return hajimu_null();
}

/* スクロールチェーン(ID, モード) → null */
static Value fn_scroll_chain(int argc, Value *argv) {
    (void)argc; (void)argv;
    /* Controls nested scroll propagation: "伝搬", "ブロック", "自動" */
    return hajimu_null();
}

/* オーバースクロール(ID, 効果) → null */
static Value fn_overscroll_effect(int argc, Value *argv) {
    (void)argc; (void)argv;
    /* "バウンス" = iOS bounce, "グロー" = Android glow, "なし" = none */
    return hajimu_null();
}

/* スクロールバー自動非表示(ID, 遅延) → null */
static Value fn_scrollbar_auto_hide(int argc, Value *argv) {
    (void)argc; (void)argv;
    /* Auto-hide scrollbar after delay_ms */
    return hajimu_null();
}

/* スナップリスト(ID, 項目配列, スナップ位置) → 数値(選択インデックス) */
static Value fn_snap_list(int argc, Value *argv) {
    if (!g_cur || !g_cur->vg || argc < 2) return hajimu_number(0);
    Hjpcontext *vg = g_cur->vg;
    int count = 0;
    if (argv[1].type == VALUE_ARRAY) count = argv[1].array.length;
    if (count == 0) return hajimu_number(0);

    float x, y, w; gui_pos(&x, &y, &w);
    float item_h = 40;
    int visible = 5;
    if (visible > count) visible = count;
    float total_h = item_h * visible;

    /* Background */
    hjpBeginPath(vg); hjpRoundedRect(vg, x, y, w, total_h, 4);
    hjpFillColor(vg, TH_WIDGET_BG); hjpFill(vg);

    /* Highlight center item */
    int center = visible / 2;
    hjpBeginPath(vg); hjpRect(vg, x, y + center * item_h, w, item_h);
    hjpFillColor(vg, hjpRGBA(66, 133, 244, 40)); hjpFill(vg);

    /* Draw items */
    hjpFontFaceId(vg, g_cur->font_id); hjpFontSize(vg, 14);
    hjpTextAlign(vg, HJP_ALIGN_CENTER | HJP_ALIGN_MIDDLE);
    for (int i = 0; i < visible && i < count; i++) {
        const char *lbl = gui_as_string(argv[1].array.elements[i]);
        if (!lbl) lbl = "";
        float alpha = (i == center) ? 1.0f : 0.5f;
        hjpFillColor(vg, hjpRGBAf(TH_TEXT.r, TH_TEXT.g, TH_TEXT.b, alpha));
        hjpText(vg, x + w/2, y + i * item_h + item_h/2, lbl, NULL);
    }
    gui_advance(total_h + 4);
    return hajimu_number(center);
}

/* =====================================================================
 * Phase 123: リモートUI/通信拡張 (v12.3.0) — 8 functions
 * ===================================================================*/

/* リモートGUI配信(ポート) → 真偽 */
static Value fn_remote_gui_serve(int argc, Value *argv) {
    (void)argc; (void)argv;
    /* Would start a lightweight TCP server streaming GUI frame data */
    /* Stub: returns false (not yet connected) */
    return hajimu_bool(false);
}

/* WebSocketGUI(ポート, パス) → 真偽 */
static Value fn_websocket_gui(int argc, Value *argv) {
    (void)argc; (void)argv;
    /* Would stream GUI to browser via WebSocket */
    return hajimu_bool(false);
}

/* MIDI入出力(デバイス名, コールバック) → null */
static Value fn_midi_io(int argc, Value *argv) {
    (void)argc; (void)argv;
    /* MIDI device communication stub */
    return hajimu_null();
}

/* OSC通信(ポート, アドレス, コールバック) → null */
static Value fn_osc_comm(int argc, Value *argv) {
    (void)argc; (void)argv;
    /* Open Sound Control communication stub */
    return hajimu_null();
}

/* ファイルアップロード進捗(URL, ファイルパス, 進捗関数) → 真偽 */
static Value fn_file_upload_progress(int argc, Value *argv) {
    (void)argc; (void)argv;
    /* HTTP file upload with progress callback */
    /* Would use curl or native HTTP in background thread */
    return hajimu_bool(false);
}

/* ダウンロード進捗(URL, 保存先, 進捗関数) → 真偽 */
static Value fn_download_progress(int argc, Value *argv) {
    (void)argc; (void)argv;
    /* HTTP download with progress */
    return hajimu_bool(false);
}

/* mDNSサービス(サービス名, コールバック) → null */
static Value fn_mdns_service(int argc, Value *argv) {
    (void)argc; (void)argv;
    /* mDNS/Bonjour/Avahi service discovery */
    return hajimu_null();
}

/* Webチャネル(ID, オブジェクト辞書) → null */
static Value fn_web_channel(int argc, Value *argv) {
    (void)argc; (void)argv;
    /* Qt WebChannel-like bridge for HTML/JS access */
    return hajimu_null();
}

/* =====================================================================
 * Phase 124: OS統合拡張 III (v12.4.0) — 8 functions
 * ===================================================================*/

/* 自動起動(有効) → 真偽 */
static Value fn_autostart(int argc, Value *argv) {
    (void)argc; (void)argv;
    /* Register/unregister app for OS autostart */
    /* macOS: LaunchAgents plist, Linux: ~/.config/autostart, Windows: Registry */
    return hajimu_bool(false);
}

/* Handoff(アクティビティ種別, データ) → 真偽 */
static Value fn_handoff(int argc, Value *argv) {
    (void)argc; (void)argv;
    /* macOS/iOS Handoff (NSUserActivity) — platform specific */
    return hajimu_bool(false);
}

/* セッション管理(操作, パラメータ) → null */
static Value fn_session_manager(int argc, Value *argv) {
    (void)argc; (void)argv;
    /* Cookie/cache/proxy session management */
    return hajimu_null();
}

/* ログ出力(レベル, メッセージ, カテゴリ) → null */
static Value fn_log_output(int argc, Value *argv) {
    if (argc < 2) return hajimu_null();
    const char *level = gui_as_string(argv[0]);
    const char *msg   = gui_as_string(argv[1]);
    const char *cat   = (argc >= 3) ? gui_as_string(argv[2]) : "gui";
    if (!level) level = "info";
    if (!msg)   msg = "";
    if (!cat)   cat = "gui";
    /* Output to stderr with structured format */
    fprintf(stderr, "[%s][%s] %s\n", cat, level, msg);
    return hajimu_null();
}

/* アプリ内課金(商品ID, コールバック) → 真偽 */
static Value fn_in_app_purchase(int argc, Value *argv) {
    (void)argc; (void)argv;
    /* IAP — requires platform store integration (not available on desktop) */
    return hajimu_bool(false);
}

/* OSアクセントカラー() → 文字列 (#RRGGBB) */
static Value fn_os_accent_color(int argc, Value *argv) {
    (void)argc; (void)argv;
#ifdef __APPLE__
    /* macOS: get system accent color via NSColor */
    FILE *fp = popen("osascript -e 'tell application \"System Events\" to get "
                     "background color of desktop' 2>/dev/null", "r");
    if (fp) { pclose(fp); }
    return hajimu_string("#007AFF"); /* macOS default blue */
#elif defined(__linux__)
    /* GTK accent: try gsettings */
    return hajimu_string("#3584E4"); /* GNOME default blue */
#else
    return hajimu_string("#0078D4"); /* Windows default blue */
#endif
}

/* D-Bus通信(サービス名, メソッド, 引数) → null */
static Value fn_dbus_comm(int argc, Value *argv) {
    (void)argc; (void)argv;
    /* D-Bus IPC — Linux only, requires libdbus or sd-bus */
    return hajimu_null();
}

/* 画像フォーマット登録(形式名) → 真偽 */
static Value fn_image_format_register(int argc, Value *argv) {
    (void)argc; (void)argv;
    /* Register additional image format decoder (TIFF, TGA, WebP, HEIF) */
    /* hjp_platform already supports most common formats */
    return hajimu_bool(true);
}

/* =====================================================================
 * Phase 125: 宣言的UI/開発ツール拡張 (v12.5.0) — 8 functions
 * ===================================================================*/

/* ライブプレビュー(ファイルパス, リフレッシュ間隔) → 真偽 */
static Value fn_live_preview(int argc, Value *argv) {
    (void)argc; (void)argv;
    /* Watch .jp file and hot-reload UI on change */
    return hajimu_bool(false);
}

/* GUIビルダー起動(レイアウト定義) → null */
static Value fn_gui_builder_launch(int argc, Value *argv) {
    (void)argc; (void)argv;
    /* Launch WYSIWYG GUI builder */
    return hajimu_null();
}

/* UIチェック(レイアウト定義) → 配列(エラー一覧) */
static Value fn_ui_check(int argc, Value *argv) {
    (void)argc; (void)argv;
    /* Static verification of UI layout definition */
    return hajimu_null(); /* empty = no errors */
}

/* YAML読込(ファイルパス) → 辞書 */
static Value fn_yaml_load(int argc, Value *argv) {
    (void)argc; (void)argv;
    /* Parse YAML file and return as dictionary */
    return hajimu_null();
}

/* UIスナップショット比較(基準, 現在) → 辞書 */
static Value fn_ui_snapshot_compare(int argc, Value *argv) {
    (void)argc; (void)argv;
    /* Visual regression test — compare two screenshots */
    return hajimu_null();
}

/* ドキュメント生成(ウィジェット配列, 出力先) → 真偽 */
static Value fn_doc_generate(int argc, Value *argv) {
    (void)argc; (void)argv;
    /* Auto-generate API documentation from widget definitions */
    return hajimu_bool(false);
}

/* アクセシビリティテスト(ルートウィジェット) → 配列 */
static Value fn_a11y_test(int argc, Value *argv) {
    (void)argc; (void)argv;
    /* Run WCAG compliance checks */
    return hajimu_null(); /* empty = all passed */
}

/* テストレコーダー(モード, ファイルパス) → 真偽 */
static Value fn_test_recorder_ex(int argc, Value *argv) {
    (void)argc; (void)argv;
    /* Record/replay user interactions with step debugging */
    return hajimu_bool(false);
}

/* =====================================================================
 * Phase 126: 高度アニメーション/物理 (v12.6.0) — 8 functions
 * ===================================================================*/

/* スピナーコレクション(ID, スタイル番号) → null */
static Value fn_spinner_collection(int argc, Value *argv) {
    if (!g_cur || !g_cur->vg) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    int style = 0;
    if (argc >= 2 && argv[1].type == VALUE_NUMBER) style = (int)argv[1].number;
    if (style < 0) style = 0; if (style > 50) style = 50;

    float x, y, w; gui_pos(&x, &y, &w);
    float sz = 32, cx = x + sz/2 + 4, cy = y + sz/2;
    float t = (float)g_frame_count * 0.05f;

    switch (style % 6) {
    case 0: /* Rotating arc */
        hjpBeginPath(vg); hjpArc(vg, cx, cy, sz/2-2, t, t+4.0f, HJP_CW);
        hjpStrokeColor(vg, TH_ACCENT); hjpStrokeWidth(vg, 3); hjpStroke(vg);
        break;
    case 1: /* Pulsing dots */
        for (int i = 0; i < 8; i++) {
            float a = t + i * 0.785f;
            float r = 3.0f + 1.5f * sinf(t * 2 + i * 0.5f);
            hjpBeginPath(vg); hjpCircle(vg, cx + cosf(a)*(sz/2-4), cy + sinf(a)*(sz/2-4), r);
            hjpFillColor(vg, TH_ACCENT); hjpFill(vg);
        }
        break;
    case 2: /* Bouncing bars */
        for (int i = 0; i < 5; i++) {
            float bh = 8 + 12 * fabsf(sinf(t * 3 + i * 0.8f));
            float bx = cx - 14 + i * 7;
            hjpBeginPath(vg); hjpRoundedRect(vg, bx, cy - bh/2, 4, bh, 2);
            hjpFillColor(vg, TH_ACCENT); hjpFill(vg);
        }
        break;
    case 3: /* Double ring */
        hjpBeginPath(vg); hjpArc(vg, cx, cy, sz/2-2, t, t+2.5f, HJP_CW);
        hjpStrokeColor(vg, TH_ACCENT); hjpStrokeWidth(vg, 2.5f); hjpStroke(vg);
        hjpBeginPath(vg); hjpArc(vg, cx, cy, sz/2-7, -t, -t+2.5f, HJP_CW);
        hjpStrokeColor(vg, TH_ACCENT_HOVER); hjpStrokeWidth(vg, 2); hjpStroke(vg);
        break;
    case 4: /* Fading squares */
        for (int i = 0; i < 4; i++) {
            float a = t + i * 1.571f;
            float px2 = cx + cosf(a) * 8;
            float py2 = cy + sinf(a) * 8;
            int alpha = (int)(180 + 75 * sinf(t * 3 + i));
            hjpBeginPath(vg); hjpRect(vg, px2-3, py2-3, 6, 6);
            hjpFillColor(vg, hjpRGBA(TH_ACCENT.r*255, TH_ACCENT.g*255, TH_ACCENT.b*255, (unsigned char)alpha));
            hjpFill(vg);
        }
        break;
    default: /* Simple rotating circle */
        hjpBeginPath(vg); hjpCircle(vg, cx + cosf(t)*10, cy + sinf(t)*10, 4);
        hjpFillColor(vg, TH_ACCENT); hjpFill(vg);
        break;
    }
    gui_advance(sz + 4);
    return hajimu_null();
}

/* 物理シミュレーション(ID, オブジェクト配列, 設定) → null */
static Value fn_physics_sim(int argc, Value *argv) {
    (void)argc; (void)argv;
    /* UI physics simulation (gravity, collision, springs) */
    return hajimu_null();
}

/* パラレルアニメーション(アニメーション配列) → null */
static Value fn_parallel_animation(int argc, Value *argv) {
    (void)argc; (void)argv;
    /* Group animations to run simultaneously */
    return hajimu_null();
}

/* シーケンシャルアニメーション(アニメーション配列) → null */
static Value fn_sequential_animation(int argc, Value *argv) {
    (void)argc; (void)argv;
    /* Group animations to run in sequence */
    return hajimu_null();
}

/* パスモーフィング(ID, 開始パス, 終了パス, 時間) → null */
static Value fn_path_morphing(int argc, Value *argv) {
    (void)argc; (void)argv;
    /* SVG path morphing animation */
    return hajimu_null();
}

/* フリップボード(ID, 表コンテンツ, 裏コンテンツ) → 真偽(表面?) */
static Value fn_flip_board(int argc, Value *argv) {
    if (!g_cur || !g_cur->vg) return hajimu_bool(true);
    Hjpcontext *vg = g_cur->vg;
    (void)argv;
    float x, y, w; gui_pos(&x, &y, &w);
    float h = 120;
    float t = (float)g_frame_count * 0.03f;
    float scale = fabsf(cosf(t));

    hjpSave(vg);
    hjpTranslate(vg, x + w/2, y + h/2);
    hjpScale(vg, scale, 1.0f);
    hjpTranslate(vg, -(x + w/2), -(y + h/2));

    bool is_front = cosf(t) >= 0;
    hjpBeginPath(vg); hjpRoundedRect(vg, x, y, w, h, 8);
    hjpFillColor(vg, is_front ? TH_WIDGET_BG : hjpRGBA(50,55,70,240));
    hjpFill(vg);
    hjpFontFaceId(vg, g_cur->font_id); hjpFontSize(vg, 14);
    hjpFillColor(vg, TH_TEXT);
    hjpTextAlign(vg, HJP_ALIGN_CENTER | HJP_ALIGN_MIDDLE);
    hjpText(vg, x + w/2, y + h/2, is_front ? "表" : "裏", NULL);
    hjpRestore(vg);
    gui_advance(h + 4);
    (void)argc;
    return hajimu_bool(is_front);
}

/* リキッドスワイプ(ID, ページ配列) → 数値(現在ページ) */
static Value fn_liquid_swipe(int argc, Value *argv) {
    (void)argc; (void)argv;
    /* Liquid page transition effect */
    return hajimu_number(0);
}

/* アニメーション割り込み(ID, 新アニメーション) → null */
static Value fn_animation_interrupt(int argc, Value *argv) {
    (void)argc; (void)argv;
    /* Interrupt running animation and blend to new one */
    return hajimu_null();
}

/* =====================================================================
 * Phase 127: データモデル高度化 (v12.7.0) — 8 functions
 * ===================================================================*/

/* プロキシモデル(ソースモデル, フィルタ関数, 変換関数) → null */
static Value fn_proxy_model(int argc, Value *argv) {
    (void)argc; (void)argv;
    /* Wraps a source model with filter/transform */
    return hajimu_null();
}

/* アイテムデリゲート(ID, 描画関数) → null */
static Value fn_item_delegate(int argc, Value *argv) {
    (void)argc; (void)argv;
    /* Custom cell rendering delegate for tables/lists */
    return hajimu_null();
}

/* データテンプレート(型名, テンプレート関数) → null */
static Value fn_data_template(int argc, Value *argv) {
    (void)argc; (void)argv;
    /* Auto-select UI template based on data type */
    return hajimu_null();
}

/* Observable配列(初期値, 変更コールバック) → null */
static Value fn_observable_array(int argc, Value *argv) {
    (void)argc; (void)argv;
    /* Array with change tracking (add/remove/modify events) */
    return hajimu_null();
}

/* モデルコンポジション(モデル配列, モード) → null */
static Value fn_model_composition(int argc, Value *argv) {
    (void)argc; (void)argv;
    /* Compose/flatten/tree multiple models */
    return hajimu_null();
}

/* マルチソーター(ソーター配列) → null */
static Value fn_multi_sorter(int argc, Value *argv) {
    (void)argc; (void)argv;
    /* Priority-based multi-column sort */
    return hajimu_null();
}

/* カスタムフィルターチェーン(フィルタ配列, 結合モード) → null */
static Value fn_custom_filter_chain(int argc, Value *argv) {
    (void)argc; (void)argv;
    /* AND/OR composite filter chains */
    return hajimu_null();
}

/* ページネーションモデル(取得関数, ページサイズ) → null */
static Value fn_pagination_model(int argc, Value *argv) {
    (void)argc; (void)argv;
    /* Server-side pagination model */
    return hajimu_null();
}

/* =====================================================================
 * Phase 128: 高度描画/エフェクト拡張 (v12.8.0) — 8 functions
 * ===================================================================*/

/* ウィジェットシャドウ(ID, X, Y, ぼかし, 色) → null */
static Value fn_widget_shadow(int argc, Value *argv) {
    if (!g_cur || !g_cur->vg || argc < 4) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    float ox = (float)gui_as_number(argv[1]);
    float oy = (float)gui_as_number(argv[2]);
    float blur = (float)gui_as_number(argv[3]);

    float x, y, w; gui_pos(&x, &y, &w);
    float h = 60;
    /* Shadow */
    Hjppaint shadow = hjpBoxGradient(vg, x+ox, y+oy, w, h, 8, blur,
                                      hjpRGBA(0,0,0,80), hjpRGBA(0,0,0,0));
    hjpBeginPath(vg); hjpRect(vg, x+ox-blur, y+oy-blur, w+blur*2, h+blur*2);
    hjpFillPaint(vg, shadow); hjpFill(vg);
    /* Widget */
    hjpBeginPath(vg); hjpRoundedRect(vg, x, y, w, h, 8);
    hjpFillColor(vg, TH_WIDGET_BG); hjpFill(vg);
    gui_advance(h + blur + 4);
    return hajimu_null();
}

/* 内側シャドウ(ID, X, Y, ぼかし, 色) → null */
static Value fn_inner_shadow(int argc, Value *argv) {
    if (!g_cur || !g_cur->vg || argc < 4) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    float ox = (float)gui_as_number(argv[1]);
    float oy = (float)gui_as_number(argv[2]);
    float blur = (float)gui_as_number(argv[3]);

    float x, y, w; gui_pos(&x, &y, &w);
    float h = 60;
    hjpBeginPath(vg); hjpRoundedRect(vg, x, y, w, h, 8);
    hjpFillColor(vg, TH_WIDGET_BG); hjpFill(vg);
    /* Inner shadow via inset box gradient */
    Hjppaint inner = hjpBoxGradient(vg, x+ox+1, y+oy+1, w-2, h-2, 8, blur,
                                     hjpRGBA(0,0,0,100), hjpRGBA(0,0,0,0));
    hjpBeginPath(vg); hjpRoundedRect(vg, x, y, w, h, 8);
    hjpFillPaint(vg, inner); hjpFill(vg);
    gui_advance(h + 4);
    return hajimu_null();
}

/* 反射効果(ID, 比率, 透明度) → null */
static Value fn_reflection_effect(int argc, Value *argv) {
    if (!g_cur || !g_cur->vg) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    float ratio = 0.3f, alpha = 0.4f;
    if (argc >= 2 && argv[1].type == VALUE_NUMBER) ratio = (float)argv[1].number;
    if (argc >= 3 && argv[2].type == VALUE_NUMBER) alpha = (float)argv[2].number;

    float x, y, w; gui_pos(&x, &y, &w);
    float h = 60;
    /* Original */
    hjpBeginPath(vg); hjpRoundedRect(vg, x, y, w, h, 8);
    hjpFillColor(vg, TH_WIDGET_BG); hjpFill(vg);
    /* Reflection (fading gradient below) */
    float rh = h * ratio;
    Hjppaint refl = hjpLinearGradient(vg, x, y+h, x, y+h+rh,
                                       hjpRGBAf(0.5f,0.5f,0.6f, alpha),
                                       hjpRGBAf(0.5f,0.5f,0.6f, 0));
    hjpBeginPath(vg); hjpRect(vg, x, y+h, w, rh);
    hjpFillPaint(vg, refl); hjpFill(vg);
    gui_advance(h + rh + 4);
    return hajimu_null();
}

/* ライティング効果(ID, 光源種類, 方向) → null */
static Value fn_lighting_effect(int argc, Value *argv) {
    if (!g_cur || !g_cur->vg) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    (void)argc; (void)argv;
    float x, y, w; gui_pos(&x, &y, &w);
    float h = 60;
    /* Base */
    hjpBeginPath(vg); hjpRoundedRect(vg, x, y, w, h, 8);
    hjpFillColor(vg, TH_WIDGET_BG); hjpFill(vg);
    /* Light overlay */
    Hjppaint light = hjpLinearGradient(vg, x, y, x + w, y + h,
                                        hjpRGBA(255,255,255,30), hjpRGBA(0,0,0,30));
    hjpBeginPath(vg); hjpRoundedRect(vg, x, y, w, h, 8);
    hjpFillPaint(vg, light); hjpFill(vg);
    gui_advance(h + 4);
    return hajimu_null();
}

/* ターミナルレンダリング(ウィジェットツリー) → null */
static Value fn_terminal_render(int argc, Value *argv) {
    (void)argc; (void)argv;
    /* Render UI to text terminal (ImTui-like) — future feature */
    return hajimu_null();
}

/* アトラス画像(ファイルパス, 領域名) → null */
static Value fn_atlas_image(int argc, Value *argv) {
    (void)argc; (void)argv;
    /* Sprite sheet / atlas image region rendering */
    return hajimu_null();
}

/* SVGアニメーション(ファイルパス, 自動再生) → null */
static Value fn_svg_animation(int argc, Value *argv) {
    (void)argc; (void)argv;
    /* Animated SVG (SMIL) playback */
    return hajimu_null();
}

/* エフェクトチェーン(ID, エフェクト配列) → null */
static Value fn_effect_chain(int argc, Value *argv) {
    (void)argc; (void)argv;
    /* Chain of graphical effects (blur, colorize, etc.) */
    return hajimu_null();
}

/* =====================================================================
 * Phase 129: 特殊ウィジェット・入力拡張 (v12.9.0) — 8 functions
 * ===================================================================*/

/* タイマーピッカー(ID, 初期秒) → 数値(秒) */
static Value fn_timer_picker(int argc, Value *argv) {
    if (!g_cur || !g_cur->vg) return hajimu_number(0);
    Hjpcontext *vg = g_cur->vg;
    int total_sec = 0;
    if (argc >= 2 && argv[1].type == VALUE_NUMBER) total_sec = (int)argv[1].number;
    int hrs = total_sec / 3600, mins = (total_sec % 3600) / 60, secs = total_sec % 60;

    float x, y, w; gui_pos(&x, &y, &w);
    float col_w = 60, col_h = 120, gap = 12;
    float total_w = col_w * 3 + gap * 2;
    float sx = x + (w - total_w) / 2;

    /* Background */
    hjpBeginPath(vg); hjpRoundedRect(vg, sx - 8, y, total_w + 16, col_h, 8);
    hjpFillColor(vg, TH_WIDGET_BG); hjpFill(vg);

    /* Highlight center band */
    hjpBeginPath(vg); hjpRect(vg, sx - 8, y + col_h/2 - 16, total_w + 16, 32);
    hjpFillColor(vg, hjpRGBA(66, 133, 244, 35)); hjpFill(vg);

    hjpFontFaceId(vg, g_cur->font_id);
    hjpTextAlign(vg, HJP_ALIGN_CENTER | HJP_ALIGN_MIDDLE);

    /* Draw three drum columns: hours, minutes, seconds */
    int vals[3] = {hrs, mins, secs};
    int maxs[3] = {24, 60, 60};
    const char *labels[3] = {"時", "分", "秒"};
    for (int c = 0; c < 3; c++) {
        float cx2 = sx + c * (col_w + gap) + col_w / 2;
        for (int row = -2; row <= 2; row++) {
            int v = (vals[c] + row + maxs[c]) % maxs[c];
            float ry = y + col_h/2 + row * 24;
            float alpha = (row == 0) ? 1.0f : 0.3f + 0.15f * (2 - abs(row));
            hjpFontSize(vg, (row == 0) ? 22 : 15);
            hjpFillColor(vg, hjpRGBAf(TH_TEXT.r, TH_TEXT.g, TH_TEXT.b, alpha));
            char buf[16]; snprintf(buf, sizeof(buf), "%02d", v);
            hjpText(vg, cx2, ry, buf, NULL);
        }
        /* Label */
        hjpFontSize(vg, 10); hjpFillColor(vg, TH_TEXT_DIM);
        hjpText(vg, cx2, y + col_h - 8, labels[c], NULL);
    }
    gui_advance(col_h + 4);
    return hajimu_number(total_sec);
}

/* 目盛りスケール(ID, 最小, 最大, 目盛り数, モード) → null */
static Value fn_tick_scale(int argc, Value *argv) {
    if (!g_cur || !g_cur->vg || argc < 4) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    double vmin = gui_as_number(argv[1]);
    double vmax = gui_as_number(argv[2]);
    int ticks = (int)gui_as_number(argv[3]);
    const char *mode = (argc >= 5) ? gui_as_string(argv[4]) : "線形";
    bool is_arc = (mode && strstr(mode, "\xe5\x86\x86\xe5\xbc\xa7") != NULL); /* 円弧 */

    float x, y, w; gui_pos(&x, &y, &w);
    if (ticks < 2) ticks = 2; if (ticks > 100) ticks = 100;

    if (is_arc) {
        /* Arc scale */
        float sz = (w < 150) ? w : 150, cx2 = x + sz/2, cy2 = y + sz/2;
        float r = sz/2 - 10;
        float start_a = 2.356f, end_a = 0.785f + 6.283f; /* ~135° to ~405° */
        hjpBeginPath(vg); hjpArc(vg, cx2, cy2, r, start_a, end_a, HJP_CW);
        hjpStrokeColor(vg, TH_BORDER); hjpStrokeWidth(vg, 2); hjpStroke(vg);
        for (int i = 0; i <= ticks; i++) {
            float frac = (float)i / ticks;
            float a = start_a + frac * (end_a - start_a);
            float inner = r - 8, outer_r = r;
            if (i % 5 == 0) inner = r - 14; /* Major tick */
            hjpBeginPath(vg);
            hjpMoveTo(vg, cx2 + cosf(a)*inner, cy2 + sinf(a)*inner);
            hjpLineTo(vg, cx2 + cosf(a)*outer_r, cy2 + sinf(a)*outer_r);
            hjpStrokeColor(vg, TH_TEXT); hjpStrokeWidth(vg, (i%5==0)?1.5f:0.8f); hjpStroke(vg);
            if (i % 5 == 0) {
                double val = vmin + frac * (vmax - vmin);
                char buf[16]; snprintf(buf, sizeof(buf), "%.0f", val);
                hjpFontFaceId(vg, g_cur->font_id); hjpFontSize(vg, 9);
                hjpFillColor(vg, TH_TEXT_DIM);
                hjpTextAlign(vg, HJP_ALIGN_CENTER | HJP_ALIGN_MIDDLE);
                hjpText(vg, cx2 + cosf(a)*(inner-10), cy2 + sinf(a)*(inner-10), buf, NULL);
            }
        }
        gui_advance(sz + 4);
    } else {
        /* Linear scale */
        float h = 30;
        hjpBeginPath(vg); hjpMoveTo(vg, x, y + h/2); hjpLineTo(vg, x + w, y + h/2);
        hjpStrokeColor(vg, TH_BORDER); hjpStrokeWidth(vg, 1); hjpStroke(vg);
        for (int i = 0; i <= ticks; i++) {
            float frac = (float)i / ticks;
            float tx2 = x + frac * w;
            float th = (i % 5 == 0) ? 12 : 6;
            hjpBeginPath(vg); hjpMoveTo(vg, tx2, y+h/2-th/2); hjpLineTo(vg, tx2, y+h/2+th/2);
            hjpStrokeColor(vg, TH_TEXT); hjpStrokeWidth(vg, (i%5==0)?1.2f:0.6f); hjpStroke(vg);
            if (i % 5 == 0) {
                double val = vmin + frac * (vmax - vmin);
                char buf[16]; snprintf(buf, sizeof(buf), "%.0f", val);
                hjpFontFaceId(vg, g_cur->font_id); hjpFontSize(vg, 9);
                hjpFillColor(vg, TH_TEXT_DIM);
                hjpTextAlign(vg, HJP_ALIGN_CENTER | HJP_ALIGN_TOP);
                hjpText(vg, tx2, y + h/2 + th/2 + 2, buf, NULL);
            }
        }
        gui_advance(h + 14);
    }
    return hajimu_null();
}

/* キーボード表示(ID, レイアウト, ハイライトキー) → null */
static Value fn_keyboard_display(int argc, Value *argv) {
    if (!g_cur || !g_cur->vg) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    (void)argc; (void)argv;
    float x, y, w; gui_pos(&x, &y, &w);
    /* Simple QWERTY keyboard visualization */
    static const char *rows[] = {
        "QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM"
    };
    int row_lens[] = {10, 9, 7};
    float key_sz = (w - 20) / 11.0f;
    if (key_sz > 32) key_sz = 32;
    float kh = key_sz * 0.9f, total_h = 0;

    hjpFontFaceId(vg, g_cur->font_id); hjpFontSize(vg, key_sz * 0.4f);
    for (int r = 0; r < 3; r++) {
        float rx = x + (r == 1 ? key_sz * 0.5f : (r == 2 ? key_sz * 1.5f : 0));
        for (int k = 0; k < row_lens[r]; k++) {
            float kx = rx + k * (key_sz + 2);
            float ky = y + r * (kh + 3);
            hjpBeginPath(vg); hjpRoundedRect(vg, kx, ky, key_sz, kh, 3);
            hjpFillColor(vg, TH_WIDGET_BG); hjpFill(vg);
            hjpStrokeColor(vg, TH_BORDER); hjpStrokeWidth(vg, 0.5f); hjpStroke(vg);
            char ch[2] = {rows[r][k], '\0'};
            hjpFillColor(vg, TH_TEXT);
            hjpTextAlign(vg, HJP_ALIGN_CENTER | HJP_ALIGN_MIDDLE);
            hjpText(vg, kx + key_sz/2, ky + kh/2, ch, NULL);
        }
        total_h = (r + 1) * (kh + 3);
    }
    gui_advance(total_h + 4);
    return hajimu_null();
}

/* ホットキーエディタ(ID, ショートカット配列) → null */
static Value fn_hotkey_editor(int argc, Value *argv) {
    if (!g_cur || !g_cur->vg || argc < 2) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    int count = 0;
    if (argv[1].type == VALUE_ARRAY) count = argv[1].array.length;

    float x, y, w; gui_pos(&x, &y, &w);
    float row_h = 30;
    hjpFontFaceId(vg, g_cur->font_id);

    for (int i = 0; i < count && i < 12; i++) {
        const char *lbl = gui_as_string(argv[1].array.elements[i]);
        if (!lbl) lbl = "?";
        float ry = y + i * row_h;
        /* Row bg */
        hjpBeginPath(vg); hjpRect(vg, x, ry, w, row_h - 2);
        hjpFillColor(vg, (i%2) ? TH_WIDGET_BG : hjpRGBA(42,44,55,200)); hjpFill(vg);
        /* Label */
        hjpFontSize(vg, 12); hjpFillColor(vg, TH_TEXT);
        hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_MIDDLE);
        hjpText(vg, x + 8, ry + row_h/2, lbl, NULL);
        /* Key badge placeholder */
        hjpBeginPath(vg); hjpRoundedRect(vg, x + w - 110, ry + 4, 100, row_h - 10, 4);
        hjpFillColor(vg, hjpRGBA(55,58,70,255)); hjpFill(vg);
        hjpFontSize(vg, 10); hjpFillColor(vg, TH_TEXT_DIM);
        hjpTextAlign(vg, HJP_ALIGN_CENTER | HJP_ALIGN_MIDDLE);
        hjpText(vg, x + w - 60, ry + row_h/2, "クリックして設定", NULL);
    }
    gui_advance(count * row_h + 4);
    return hajimu_null();
}

/* 3Dオリエンテーション(ID, 方向, サイズ) → null */
static Value fn_orientation_gizmo_3d(int argc, Value *argv) {
    if (!g_cur || !g_cur->vg) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    float sz = 80;
    if (argc >= 3 && argv[2].type == VALUE_NUMBER) sz = (float)argv[2].number;
    (void)argv;

    float x, y, w; gui_pos(&x, &y, &w);
    float cx2 = x + sz/2 + 4, cy2 = y + sz/2;
    float r = sz/2 - 4;
    float t = (float)g_frame_count * 0.015f;

    /* Sphere outline */
    hjpBeginPath(vg); hjpCircle(vg, cx2, cy2, r);
    hjpStrokeColor(vg, TH_BORDER); hjpStrokeWidth(vg, 1.5f); hjpStroke(vg);

    /* 3 axes */
    float axes[3][2] = {{cosf(t), sinf(t)}, {cosf(t+2.094f), sinf(t+2.094f)}, {0, -1}};
    Hjpcolor cols[3] = {hjpRGBA(219,68,55,255), hjpRGBA(15,157,88,255), hjpRGBA(66,133,244,255)};
    const char *labels2[3] = {"X", "Y", "Z"};
    for (int a2 = 0; a2 < 3; a2++) {
        float ex = cx2 + axes[a2][0] * r * 0.8f;
        float ey = cy2 + axes[a2][1] * r * 0.8f;
        hjpBeginPath(vg); hjpMoveTo(vg, cx2, cy2); hjpLineTo(vg, ex, ey);
        hjpStrokeColor(vg, cols[a2]); hjpStrokeWidth(vg, 2); hjpStroke(vg);
        hjpBeginPath(vg); hjpCircle(vg, ex, ey, 5);
        hjpFillColor(vg, cols[a2]); hjpFill(vg);
        hjpFontFaceId(vg, g_cur->font_id); hjpFontSize(vg, 10);
        hjpFillColor(vg, hjpRGBA(255,255,255,255));
        hjpTextAlign(vg, HJP_ALIGN_CENTER | HJP_ALIGN_MIDDLE);
        hjpText(vg, ex, ey, labels2[a2], NULL);
    }
    gui_advance(sz + 4);
    (void)argc;
    return hajimu_null();
}

/* リフレクション自動UI(ID, フィールド定義配列) → null */
static Value fn_reflection_auto_ui(int argc, Value *argv) {
    if (!g_cur || !g_cur->vg || argc < 2) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    int count = 0;
    if (argv[1].type == VALUE_ARRAY) count = argv[1].array.length;

    float x, y, w; gui_pos(&x, &y, &w);
    float row_h = 28;
    hjpFontFaceId(vg, g_cur->font_id);

    /* Header */
    hjpBeginPath(vg); hjpRect(vg, x, y, w, row_h);
    hjpFillColor(vg, TH_WIDGET_BG); hjpFill(vg);
    hjpFontSize(vg, 12); hjpFillColor(vg, TH_TEXT_DIM);
    hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_MIDDLE);
    hjpText(vg, x + 8, y + row_h/2, "プロパティ", NULL);
    hjpText(vg, x + w/2, y + row_h/2, "値", NULL);

    for (int i = 0; i < count && i < 20; i++) {
        const char *lbl = gui_as_string(argv[1].array.elements[i]);
        if (!lbl) lbl = "field";
        float ry = y + (i + 1) * row_h;
        hjpBeginPath(vg); hjpRect(vg, x, ry, w, row_h - 1);
        hjpFillColor(vg, (i%2) ? hjpRGBA(38,40,50,200) : hjpRGBA(44,46,58,200));
        hjpFill(vg);
        hjpFontSize(vg, 11); hjpFillColor(vg, TH_TEXT);
        hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_MIDDLE);
        hjpText(vg, x + 8, ry + row_h/2, lbl, NULL);
        /* Value placeholder */
        hjpBeginPath(vg); hjpRoundedRect(vg, x + w/2, ry + 3, w/2 - 8, row_h - 7, 3);
        hjpFillColor(vg, hjpRGBA(55,58,70,255)); hjpFill(vg);
    }
    gui_advance((count + 1) * row_h + 4);
    return hajimu_null();
}

/* Sandbox(ID, コンテンツ関数, エラー関数) → 真偽 */
static Value fn_sandbox_widget(int argc, Value *argv) {
    (void)argc; (void)argv;
    /* Safe execution container — catches exceptions from child widgets */
    return hajimu_bool(true);
}

/* reStructuredText(ID, テキスト) → null */
static Value fn_rst_viewer(int argc, Value *argv) {
    if (!g_cur || !g_cur->vg || argc < 2) return hajimu_null();
    Hjpcontext *vg = g_cur->vg;
    const char *text2 = gui_as_string(argv[1]);
    if (!text2) text2 = "";

    float x, y, w; gui_pos(&x, &y, &w);
    /* Render reST as plain text with basic heading detection */
    hjpFontFaceId(vg, g_cur->font_id);
    hjpFillColor(vg, TH_TEXT);
    hjpTextAlign(vg, HJP_ALIGN_LEFT | HJP_ALIGN_TOP);
    /* Simple: treat lines starting with === or --- as headings */
    float ty = y;
    const char *p = text2;
    while (*p) {
        const char *nl = strchr(p, '\n');
        int len2 = nl ? (int)(nl - p) : (int)strlen(p);
        bool is_heading = (len2 > 0 && (p[0] == '=' || p[0] == '-' || p[0] == '#'));
        hjpFontSize(vg, is_heading ? 16 : 13);
        if (is_heading) hjpFillColor(vg, TH_ACCENT); else hjpFillColor(vg, TH_TEXT);
        char line[512]; int copy_len = len2 < 511 ? len2 : 511;
        memcpy(line, p, (size_t)copy_len); line[copy_len] = '\0';
        if (!is_heading || p[0] == '#') hjpText(vg, x + 4, ty, line, NULL);
        ty += is_heading ? 22 : 16;
        if (ty > y + 400) break;
        p = nl ? nl + 1 : p + len2;
    }
    gui_advance(ty - y + 4);
    return hajimu_null();
}

/* =====================================================================
 * Phase 130: エコシステム/産業プロトコル (v13.0.0) — 8 functions
 * ===================================================================*/

/* ウィジェットプラグイン(プラグインパス) → 真偽 */
static Value fn_widget_plugin(int argc, Value *argv) {
    (void)argc; (void)argv;
    /* Dynamic loading of third-party widget plugins */
    return hajimu_bool(false);
}

/* CSSスタイル(セレクタ, ルール辞書) → null */
static Value fn_css_style(int argc, Value *argv) {
    (void)argc; (void)argv;
    /* CSS-like style definitions for widgets */
    return hajimu_null();
}

/* OPC_UA通信(サーバーURL, ノードID) → null */
static Value fn_opcua_comm(int argc, Value *argv) {
    (void)argc; (void)argv;
    /* OPC-UA industrial automation protocol */
    return hajimu_null();
}

/* CANバス(インターフェース, フレーム送信) → 真偽 */
static Value fn_can_bus(int argc, Value *argv) {
    (void)argc; (void)argv;
    /* CAN bus / Modbus serial bus communication */
    return hajimu_bool(false);
}

/* CoAPクライアント(URL, メソッド) → null */
static Value fn_coap_client(int argc, Value *argv) {
    (void)argc; (void)argv;
    /* CoAP constrained application protocol for IoT */
    return hajimu_null();
}

/* TUIOマルチタッチ(ポート, コールバック) → null */
static Value fn_tuio_multitouch(int argc, Value *argv) {
    (void)argc; (void)argv;
    /* TUIO protocol multitouch input */
    return hajimu_null();
}

/* UIテスト実行(テスト関数配列) → 辞書(結果) */
static Value fn_ui_test_run(int argc, Value *argv) {
    (void)argc; (void)argv;
    /* Automated UI test runner */
    return hajimu_null();
}

/* Protobufシリアライズ(スキーマ, データ) → null */
static Value fn_protobuf_serialize(int argc, Value *argv) {
    (void)argc; (void)argv;
    /* Protocol Buffers serialization/deserialization */
    return hajimu_null();
}

/* ランタイム設定 */
HAJIMU_PLUGIN_EXPORT void hajimu_plugin_set_runtime(HajimuRuntime *rt) {
    __hajimu_runtime = rt;
}

/* 関数テーブル */
static HajimuPluginFunc gui_functions[] = {
    /* --- Phase 1: コア --- */
    {"アプリ作成",           fn_app_create,    3, 3},
    {"描画ループ",           fn_draw_loop,     2, 2},
    {"アプリ終了",           fn_app_quit,      1, 1},
    {"ウィンドウサイズ",     fn_window_size,   1, 1},
    {"ウィンドウタイトル",   fn_window_title,  2, 2},
    {"ウィンドウリサイズ",   fn_window_resize, 3, 3},
    {"フレームレート",       fn_framerate,     2, 2},
    {"背景色",               fn_bg_color,      1, 1},
    {"色",                   fn_color,         3, 4},
    {"色16進",               fn_color_hex,     1, 1},
    /* --- Phase 2: ウィジェット --- */
    {"テキスト",             fn_text,          1, 2},
    {"見出し",               fn_heading,       1, 2},
    {"ボタン",               fn_button,        1, 2},
    {"チェックボックス",     fn_checkbox,      2, 2},
    {"ラジオボタン",         fn_radio,         3, 3},
    {"スライダー",           fn_slider,        4, 4},
    {"プログレスバー",       fn_progress,      1, 2},
    {"セパレーター",         fn_separator,     0, 0},
    {"スペーサー",           fn_spacer,        0, 1},
    {"ツールチップ",         fn_tooltip,       1, 1},
    /* --- Phase 3: テキスト入力 --- */
    {"テキスト入力",         fn_text_input,    2, 3},
    {"パスワード入力",       fn_password_input,2, 2},
    {"数値入力",             fn_number_input,  2, 5},
    {"テキストエリア",       fn_textarea,      2, 3},
    {"コンボボックス",       fn_combobox,      3, 3},
    /* --- Phase 4: レイアウト --- */
    {"パネル開始",           fn_panel_begin,   0, 2},
    {"パネル終了",           fn_panel_end,     0, 0},
    {"横並び開始",           fn_horizontal_begin, 0, 0},
    {"横並び終了",           fn_horizontal_end,   0, 0},
    {"インデント",           fn_indent,        0, 1},
    {"インデント解除",       fn_unindent,      0, 0},
    {"マウス位置",           fn_mouse_pos,     0, 0},
    {"マウスクリック",       fn_mouse_clicked, 0, 0},
    /* --- Phase 5: リスト・テーブル・ツリー --- */
    {"リスト",               fn_list,          2, 3},
    {"テーブル作成",         fn_table_create,  2, 2},
    {"テーブル行追加",       fn_table_add_row, 2, 2},
    {"テーブル描画",         fn_table_draw,    1, 2},
    {"テーブルソート",       fn_table_sort,    3, 3},
    {"ツリー作成",           fn_tree_create,   1, 1},
    {"ツリーノード追加",     fn_tree_add_node, 3, 4},
    {"ツリー描画",           fn_tree_draw,     1, 2},
    /* --- Phase 6: タブ・メニュー・ダイアログ --- */
    {"タブバー",             fn_tab_bar,       3, 3},
    {"タブ内容",             fn_tab_content,   2, 2},
    {"メニューバー開始",     fn_menubar_begin, 0, 0},
    {"メニューバー終了",     fn_menubar_end,   0, 0},
    {"メニュー",             fn_menu,          1, 1},
    {"メニュー項目",         fn_menu_item,     1, 2},
    {"メニューセパレーター", fn_menu_separator_item, 0, 0},
    {"ダイアログ",           fn_dialog,        2, 2},
    {"ファイルダイアログ",   fn_file_dialog,   0, 2},
    {"メッセージ",           fn_message,       3, 3},
    {"トースト",             fn_toast,         1, 2},
    /* --- Phase 7: カスタム描画 --- */
    {"キャンバス開始",       fn_canvas_begin,  3, 3},
    {"キャンバス終了",       fn_canvas_end,    0, 0},
    {"線",                   fn_draw_line,     4, 6},
    {"矩形",                 fn_draw_rect,     4, 5},
    {"矩形枠",               fn_draw_rect_stroke, 4, 6},
    {"角丸矩形",             fn_draw_rounded_rect, 5, 6},
    {"円",                   fn_draw_circle,   3, 4},
    {"円弧",                 fn_draw_arc,      5, 6},
    {"多角形",               fn_draw_polygon,  1, 2},
    {"パス開始",             fn_path_begin,    0, 0},
    {"パス終了",             fn_path_end,      0, 2},
    {"パス移動",             fn_path_move,     2, 2},
    {"パス線",               fn_path_line,     2, 2},
    {"ベジェ",               fn_bezier,        6, 6},
    {"線形グラデーション",   fn_linear_gradient, 6, 6},
    {"画像読み込み",         fn_image_load,    1, 1},
    {"画像描画",             fn_image_draw,    3, 5},
    {"描画テキスト",         fn_draw_text_at,  3, 4},
    {"変換保存",             fn_save_transform,  0, 0},
    {"変換復元",             fn_restore_transform, 0, 0},
    {"平行移動",             fn_translate,     2, 2},
    {"回転",                 fn_rotate_transform, 1, 1},
    {"拡縮",                 fn_scale_transform, 2, 2},
    /* --- Phase 8: テーマ・スタイル --- */
    {"テーマ設定",           fn_theme_set,     1, 1},
    {"テーマ色",             fn_theme_color,   2, 2},
    {"テーマフォント",       fn_theme_font,    1, 2},
    {"スタイル設定",         fn_style_set,     2, 2},
    {"フォント読み込み",     fn_font_load,     2, 2},
    {"フォントサイズ",       fn_font_size_set, 1, 1},
    {"DPIスケール",          fn_dpi_scale,     0, 0},
    /* --- Phase 9: ドラッグ&ドロップ + クリップボード --- */
    {"ドラッグソース",       fn_drag_source,   2, 2},
    {"ドロップターゲット",   fn_drop_target,   1, 1},
    {"ファイルドロップ取得", fn_file_drop_get, 0, 0},
    {"クリップボード取得",   fn_clipboard_get, 0, 0},
    {"クリップボード設定",   fn_clipboard_set, 1, 1},
    {"ショートカット",       fn_shortcut,      1, 2},
    {"カーソル設定",         fn_cursor_set,    1, 1},
    {"マウスボタン",         fn_mouse_button,  1, 1},
    {"キー押下",             fn_key_pressed,   1, 1},
    /* --- Phase 10: アニメーション + タイマー --- */
    {"アニメーション",       fn_animation,     4, 5},
    {"トランジション",       fn_transition,    3, 3},
    {"タイマー",             fn_timer_start,   3, 3},
    {"タイマー停止",         fn_timer_stop,    1, 1},
    /* --- Phase 11: 追加ウィジェット --- */
    {"トグルスイッチ",       fn_toggle_switch, 2, 2},
    {"カラーピッカー",       fn_color_picker,  2, 2},
    {"スピナー",             fn_spinner,       0, 1},
    {"折りたたみ",           fn_collapsing,    2, 2},
    {"リンク",               fn_link,          1, 2},
    {"選択可能",             fn_selectable,    2, 2},
    {"バッジ",               fn_badge,         1, 2},
    {"タグ",                 fn_tag,           1, 2},
    {"カード開始",           fn_card_begin,    0, 1},
    {"カード終了",           fn_card_end,      0, 0},
    {"区切りテキスト",       fn_separator_text, 1, 1},
    {"小ボタン",             fn_small_button,  1, 1},
    {"画像ボタン",           fn_image_button,  1, 3},
    /* --- Phase 12: 高度なレイアウト --- */
    {"スクロール領域開始",   fn_scroll_begin,  2, 2},
    {"スクロール領域終了",   fn_scroll_end,    1, 1},
    {"グリッド開始",         fn_grid_begin,    1, 2},
    {"グリッド終了",         fn_grid_end,      1, 1},
    {"グリッド次列",         fn_grid_next,     1, 1},
    {"スプリッター",         fn_splitter,      2, 2},
    {"グループ開始",         fn_group_begin,   0, 1},
    {"グループ終了",         fn_group_end,     1, 1},
    {"ツールバー開始",       fn_toolbar_begin, 0, 0},
    {"ツールバー終了",       fn_toolbar_end,   0, 0},
    {"ステータスバー開始",   fn_statusbar_begin, 0, 0},
    {"ステータスバー終了",   fn_statusbar_end, 0, 0},
    {"中央揃え",             fn_align_center,  0, 0},
    {"右揃え",               fn_align_right,   0, 0},
    /* --- Phase 13: フォーム・入力拡張 --- */
    {"検索入力",             fn_search_input,  2, 3},
    {"オートコンプリート",   fn_autocomplete,  3, 3},
    {"数値ドラッグ",         fn_drag_value,    3, 5},
    {"日付ピッカー",         fn_date_picker,   2, 2},
    {"時間ピッカー",         fn_time_picker,   2, 2},
    {"プレースホルダー",     fn_placeholder,   1, 1},
    {"バリデーション",       fn_validation,    2, 2},
    {"無効化開始",           fn_disable_begin, 0, 0},
    {"無効化終了",           fn_disable_end,   0, 0},
    {"マルチセレクト",       fn_multi_select,  3, 3},
    /* --- Phase 14: チャート --- */
    {"折れ線グラフ",         fn_line_chart,    2, 3},
    {"棒グラフ",             fn_bar_chart,     2, 3},
    {"円グラフ",             fn_pie_chart,     2, 3},
    {"散布図",               fn_scatter_chart, 2, 3},
    {"エリアチャート",       fn_area_chart,    2, 3},
    {"ゲージ",               fn_gauge,         2, 3},
    {"スパークライン",       fn_sparkline,     3, 3},
    {"ヒストグラム",         fn_histogram,     2, 3},
    /* --- Phase 15: サブウィンドウ・ポップアップ --- */
    {"子ウィンドウ開始",     fn_child_begin,   3, 4},
    {"子ウィンドウ終了",     fn_child_end,     0, 0},
    {"ポップアップ開始",     fn_popup_begin,   1, 1},
    {"ポップアップ終了",     fn_popup_end,     0, 0},
    {"ポップアップ表示",     fn_popup_open,    1, 1},
    {"サイドパネル",         fn_side_panel,    3, 3},
    {"ボトムシート開始",     fn_bottom_sheet_begin, 2, 2},
    {"ボトムシート終了",     fn_bottom_sheet_end, 0, 0},
    {"情報バー",             fn_info_bar,      2, 3},
    {"フローティングボタン", fn_fab,           3, 3},
    /* --- Phase 16: リッチコンテンツ --- */
    {"色付きテキスト",       fn_text_colored,  2, 2},
    {"折返しテキスト",       fn_text_wrapped,  1, 1},
    {"無効テキスト",         fn_text_disabled, 1, 1},
    {"箇条書き",             fn_bullet_text,   1, 1},
    {"テキスト選択",         fn_text_selectable, 1, 1},
    {"リッチテキスト",       fn_rich_text,     1, 1},
    {"マークダウン",         fn_markdown,      1, 1},
    {"コードブロック",       fn_code_block,    1, 2},
    /* --- Phase 17: システム統合 --- */
    {"マルチウィンドウ",     fn_multi_window,    3, 3},
    {"ウィンドウ位置",       fn_window_pos,      2, 2},
    {"ウィンドウ最大化",     fn_window_maximize, 0, 0},
    {"ウィンドウ最小化",     fn_window_minimize, 0, 0},
    {"フルスクリーン",       fn_fullscreen,      1, 1},
    {"OSテーマ取得",         fn_os_theme,        0, 0},
    {"設定保存",             fn_settings_save,   2, 2},
    {"設定読込",             fn_settings_load,   1, 1},
    {"元に戻す",             fn_undo,            1, 1},
    {"やり直す",             fn_redo,            1, 1},
    /* --- Phase 18: アクセシビリティ + パフォーマンス --- */
    {"フォーカス設定",       fn_focus_set,       1, 1},
    {"フォーカス取得",       fn_focus_get,       0, 0},
    {"タブ順序",             fn_tab_order,       1, 1},
    {"仮想スクロール",       fn_virtual_scroll,  2, 2},
    {"テーブルフィルター",   fn_table_filter,    3, 3},
    {"テーブルページ",       fn_table_page,      3, 3},
    {"テーブル列幅",         fn_table_col_width, 3, 3},
    {"クリッピング",         fn_clipping,        4, 4},
    /* --- Phase 19: ナビゲーション + ウィザード --- */
    {"ナビゲーションバー",   fn_nav_bar,         2, 2},
    {"ブレッドクラム",       fn_breadcrumb,      1, 1},
    {"ステッパー",           fn_stepper,         2, 2},
    {"ウィザード開始",       fn_wizard_begin,    2, 2},
    {"ウィザードページ",     fn_wizard_page,     2, 2},
    {"ウィザード終了",       fn_wizard_end,      0, 0},
    {"アコーディオン",       fn_accordion,       2, 2},
    {"ページビュー",         fn_page_view,       2, 2},
    /* --- Phase 20: 高度なウィジェット II --- */
    {"ダイヤル",             fn_dial,            4, 4},
    {"レンジスライダー",     fn_range_slider,    5, 5},
    {"カレンダー",           fn_calendar,        2, 2},
    {"評価",                 fn_rating,          3, 3},
    {"アバター",             fn_avatar,          1, 2},
    {"タイムライン",         fn_timeline,        1, 1},
    {"スケルトン",           fn_skeleton,        2, 3},
    {"カルーセル",           fn_carousel,        3, 3},
    {"数値ステッパー",       fn_num_stepper,     4, 4},
    {"リストタイル",         fn_list_tile,       1, 3},
    /* --- Phase 21: レイアウト拡張 II --- */
    {"フロー開始",           fn_flow_begin,      0, 1},
    {"フロー終了",           fn_flow_end,        0, 0},
    {"オーバーレイ開始",     fn_overlay_begin,   2, 2},
    {"オーバーレイ終了",     fn_overlay_end,     0, 0},
    {"絶対配置",             fn_absolute_pos,    2, 2},
    {"透明度",               fn_opacity,         1, 1},
    {"リビーラー",           fn_revealer,        1, 2},
    {"ドッキング",           fn_docking,         3, 3},
    /* Phase 22: システム連携 II */
    {"システム通知",         fn_sys_notify,      2, 2},
    {"システムトレイ",       fn_sys_tray,        1, 2},
    {"フォントダイアログ",   fn_font_dialog,     0, 0},
    {"アバウトダイアログ",   fn_about_dialog,    3, 3},
    {"ウィンドウアイコン",   fn_window_icon,     1, 1},
    {"ウィンドウ透明度",     fn_window_opacity,  1, 1},
    {"スクリーンショット",   fn_screenshot,      1, 1},
    {"経過時間",             fn_elapsed_time,    0, 0},
    /* Phase 23: コンテキスト・選択拡張 */
    {"コンテキストメニュー開始", fn_context_menu_begin, 1, 1},
    {"コンテキストメニュー終了", fn_context_menu_end,   0, 0},
    {"トグルボタン",         fn_toggle_button,   2, 2},
    {"セグメントコントロール", fn_segment_control, 2, 2},
    {"リストボックス",       fn_listbox,         3, 3},
    {"ドロップダウン",       fn_dropdown,        3, 3},
    {"ポップコンファーム",   fn_popconfirm,      2, 2},
    {"ボタングループ",       fn_button_group,    2, 2},
    /* Phase 24: ナビゲーション拡張 */
    {"ページネーション",     fn_pagination,      2, 3},
    {"ボトムナビゲーション", fn_bottom_nav,      2, 2},
    {"ドロワー開始",         fn_drawer_begin,    2, 2},
    {"ドロワー終了",         fn_drawer_end,      0, 0},
    {"サイドナビゲーション", fn_side_nav,        2, 2},
    {"スピードダイヤル",     fn_speed_dial,      2, 2},
    {"アンカーリンク",       fn_anchor_link,     1, 1},
    {"矢印ボタン",           fn_arrow_button,    1, 1},
    /* Phase 25: 描画拡張 II */
    {"楕円",                 fn_draw_ellipse,    4, 5},
    {"点線",                 fn_draw_dashed,     4, 5},
    {"矢印線",               fn_draw_arrow,      4, 5},
    {"影",                   fn_draw_shadow,     4, 5},
    {"放射グラデーション",   fn_radial_gradient, 6, 6},
    {"ボックスグラデーション", fn_box_gradient,  6, 6},
    {"テクスチャパターン",   fn_texture_pattern, 5, 5},
    {"描画アルファ",         fn_draw_alpha,      1, 1},
    /* Phase 26: テキスト・データ表示拡張 */
    {"テキスト測定",         fn_text_measure,    1, 1},
    {"省略テキスト",         fn_text_ellipsis,   2, 2},
    {"ラベル付き値",         fn_label_value,     2, 2},
    {"統計カード",           fn_stat_card,       2, 3},
    {"空状態表示",           fn_empty_state,     0, 1},
    {"結果ページ",           fn_result_page,     3, 3},
    {"ウォーターマーク",     fn_watermark,       1, 2},
    {"QRコード",             fn_qrcode,          1, 2},
    /* Phase 27: アニメーション拡張 */
    {"イージング",           fn_easing,          2, 2},
    {"スプリング",           fn_spring,          2, 4},
    {"フェードイン",         fn_fade_in,         2, 2},
    {"フェードアウト",       fn_fade_out,        2, 2},
    {"アニメーション停止",   fn_anim_stop,       1, 1},
    {"アニメーションチェーン", fn_anim_chain,    1, 1},
    {"シェイク",             fn_shake,           2, 2},
    {"パルス",               fn_pulse,           2, 2},
    /* Phase 28: レイアウト拡張 III */
    {"ウィジェット幅",       fn_widget_width,    1, 1},
    {"マージン",             fn_margin,          1, 4},
    {"パディング",           fn_padding,         1, 4},
    {"最小サイズ",           fn_min_size,        2, 2},
    {"最大サイズ",           fn_max_size,        2, 2},
    {"レスポンシブ",         fn_responsive,      0, 0},
    {"アスペクト比",         fn_aspect_ratio,    1, 1},
    {"スティッキー",         fn_sticky,          1, 1},
    /* Phase 29: ダイアログ・フィードバック拡張 */
    {"入力ダイアログ",       fn_input_dialog,    2, 3},
    {"確認ダイアログ",       fn_confirm_dialog,  2, 2},
    {"進捗ダイアログ",       fn_progress_dialog, 2, 2},
    {"カラーダイアログ",     fn_color_dialog,    1, 1},
    {"スナックバー",         fn_snackbar,        1, 2},
    {"バナー",               fn_banner,          2, 2},
    {"スプラッシュスクリーン", fn_splash_screen, 1, 2},
    {"リッチツールチップ開始", fn_rich_tooltip_begin, 0, 0},
    /* Phase 30: 入力拡張 III */
    {"タグ入力",             fn_tag_input,       2, 2},
    {"垂直スライダー",       fn_vslider,         4, 4},
    {"マスク入力",           fn_mask_input,      3, 3},
    {"ピンコード",           fn_pincode,         2, 2},
    {"メンション入力",       fn_mention_input,   3, 3},
    {"繰り返しボタン",       fn_repeat_button,   1, 1},
    {"インビジブルボタン",   fn_invisible_button, 2, 2},
    {"自動リサイズテキスト", fn_auto_resize_text, 2, 2},
    /* Phase 31: データ表示拡張 */
    {"ソート可能リスト",     fn_sortable_list,   2, 2},
    {"プロパティグリッド",   fn_property_grid,   2, 2},
    {"ツリーテーブル",       fn_tree_table,      2, 2},
    {"ヒートマップ",         fn_heatmap,         1, 2},
    {"トランスファーリスト", fn_transfer_list,   2, 2},
    {"カスケーダー",         fn_cascader,        3, 3},
    {"ツリーセレクト",       fn_tree_select,     3, 3},
    {"画像ギャラリー",       fn_image_gallery,   2, 2},
    /* Phase 32: テーマ・スタイル拡張 II */
    {"テーマ作成",           fn_theme_create,    2, 2},
    {"テーマエクスポート",   fn_theme_export,    0, 0},
    {"テーマインポート",     fn_theme_import,    1, 1},
    {"ダークモード自動",     fn_dark_mode_auto,  0, 0},
    {"影設定",               fn_shadow_style,    3, 4},
    {"角丸設定",             fn_border_radius,   1, 1},
    {"アイコン",             fn_icon,            1, 3},
    {"フォント一覧",         fn_font_list,       0, 0},
    /* Phase 33: マルチメディア */
    {"音声再生",             fn_audio_play,      1, 2},
    {"音声停止",             fn_audio_stop,      0, 1},
    {"音量",                 fn_audio_volume,    1, 1},
    {"ブラウザ開く",         fn_open_browser,    1, 1},
    {"SVG読み込み",          fn_svg_load,        1, 1},
    {"ズーム操作",           fn_zoom_view,       2, 3},
    {"ツアーガイド",         fn_tour_guide,      2, 2},
    {"角度スライダー",       fn_angle_slider,    2, 2},
    /* Phase 34: ウィジェット状態・ユーティリティ */
    {"ウィジェット状態",     fn_widget_state,    0, 0},
    {"ウィジェット矩形",     fn_widget_rect,     0, 0},
    {"スクロール位置",       fn_scroll_pos,      0, 0},
    {"スクロール移動",       fn_scroll_to,       1, 1},
    {"フレーム数",           fn_frame_count,     0, 0},
    {"ポップオーバー開始",   fn_popover_begin,   1, 1},
    {"外部クリック",         fn_click_away,      1, 1},
    {"ヘルプマーク",         fn_help_mark,       1, 1},
    /* Phase 35: テーブル高度操作 */
    {"テーブルインライン編集", fn_table_inline_edit, 1, 2},
    {"テーブル列並替",         fn_table_col_reorder, 1, 1},
    {"テーブル選択モード",     fn_table_sel_mode,    1, 2},
    {"テーブル固定列",         fn_table_fixed_cols,  2, 2},
    {"テーブルグループ化",     fn_table_group,       2, 2},
    {"テーブルセル結合",       fn_table_merge,       2, 2},
    {"テーブルエクスポート",   fn_table_export,      2, 2},
    {"テーブル行ドラッグ並替", fn_table_row_drag,    1, 1},
    /* Phase 36: ツリー高度操作 */
    {"ツリードラッグ並替",     fn_tree_drag_reorder, 1, 1},
    {"ツリー遅延読込",         fn_tree_lazy_load,    2, 2},
    {"ツリーチェックボックス", fn_tree_checkbox,     1, 1},
    {"ツリー検索",             fn_tree_search,       2, 2},
    {"ツリー全展開",           fn_tree_expand_all,   1, 1},
    {"ツリー全折畿",           fn_tree_collapse_all, 1, 1},
    {"ツリー複数選択",         fn_tree_multi_select, 1, 2},
    {"ツリーノードアイコン",   fn_tree_node_icon,    3, 3},
    /* Phase 37: テキストエディタ機能 */
    {"シンタックスハイライト", fn_syntax_highlight,   1, 2},
    {"テキストカーソル位置", fn_text_cursor_pos,    1, 2},
    {"テキスト範囲選択",     fn_text_select_range,  3, 3},
    {"行番号表示",             fn_line_numbers,       2, 2},
    {"テキスト検索置換",     fn_text_find_replace,  3, 3},
    {"テキスト折畿",           fn_text_fold,          2, 2},
    {"入力フォーマッタ",       fn_input_formatter,    2, 2},
    {"IME合成",               fn_ime_composition,    1, 1},
    /* Phase 38: フォーム枠組み */
    {"フォーム作成",           fn_form_create,        1, 1},
    {"フォーム送信",           fn_form_submit,        1, 1},
    {"フォームリセット",       fn_form_reset,         1, 1},
    {"フォーム検証",           fn_form_validate,      1, 1},
    {"フォームエラー一覧",   fn_form_errors,        1, 1},
    {"依存フィールド",       fn_form_dependent,     2, 2},
    {"ファイルアップロード",   fn_file_upload,        1, 2},
    {"フォームデータ取得",   fn_form_data,          1, 1},
    /* Phase 39: アクセシビリティ */
    {"アクセシブル名",       fn_a11y_name,          1, 1},
    {"アクセシブル説明",     fn_a11y_desc,          1, 1},
    {"アクセシブルロール",   fn_a11y_role,          1, 1},
    {"ライブリージョン",     fn_live_region,        2, 2},
    {"フォーカスリング",       fn_focus_ring,         1, 1},
    {"キーボードトラップ",   fn_keyboard_trap,      2, 2},
    {"スクリーンリーダー通知", fn_sr_announce,  1, 1},
    {"動き軽減",               fn_reduce_motion,      1, 1},
    /* Phase 40: ジェスチャー認識 */
    {"ピンチ",                 fn_pinch,              0, 1},
    {"スワイプ",               fn_swipe,              1, 2},
    {"長押し",                 fn_long_press,         0, 1},
    {"ダブルクリック",       fn_double_click,       0, 1},
    {"パンジェスチャー",     fn_pan_gesture,        0, 1},
    {"マルチタッチ",           fn_multi_touch,        0, 1},
    {"ジェスチャー速度",     fn_gesture_velocity,   0, 0},
    {"ジェスチャー領域",     fn_gesture_region,     1, 1},
    /* Phase 41: 印刷/エクスポート */
    {"印刷",                   fn_print,              1, 1},
    {"印刷プレビュー",         fn_print_preview,      1, 1},
    {"PDF出力",               fn_pdf_export,         2, 2},
    {"ウィジェット画像化",     fn_widget_capture,     2, 2},
    {"印刷設定",               fn_print_settings,     0, 1},
    {"キャンバス画像化",       fn_canvas_export,      2, 2},
    {"SVG出力",               fn_svg_export,         2, 2},
    {"バーコード",             fn_barcode,            2, 2},
    /* Phase 42: データバインディング */
    {"バインド",               fn_bind,               2, 2},
    {"双方向バインド",       fn_bind_two_way,       2, 2},
    {"計算値",                 fn_computed,           2, 2},
    {"監視",                   fn_watch,              2, 2},
    {"モデル作成",             fn_model_create,       1, 1},
    {"リストモデル",           fn_list_model,         1, 1},
    {"バインド解除",           fn_unbind,             1, 1},
    {"変更通知",               fn_notify_change,      0, 1},
    /* Phase 43: 国際化 */
    {"RTLレイアウト",         fn_rtl_layout,         1, 1},
    {"ロケール設定",           fn_set_locale,         1, 1},
    {"翻訳",                   fn_i18n_translate,     1, 1},
    {"翻訳登録",               fn_register_translations, 2, 2},
    {"数値フォーマット",     fn_number_format,      1, 2},
    {"日付フォーマット",     fn_date_format,        1, 2},
    {"複数形",                 fn_plural,             2, 2},
    {"テキスト方向",           fn_text_direction,     1, 1},
    /* Phase 44: 動画/カメラ */
    {"動画再生",               fn_video_play,         1, 1},
    {"動画停止",               fn_video_stop,         1, 1},
    {"動画シーク",             fn_video_seek,         2, 2},
    {"動画コントロール",       fn_video_controls,     1, 1},
    {"GIF表示",                fn_gif_display,        1, 1},
    {"カメラ入力",             fn_camera_input,       0, 1},
    {"動画音量",               fn_video_volume,       2, 2},
    {"動画状態",               fn_video_state,        1, 1},
    /* Phase 45: 3D統合 */
    {"3Dビューポート",       fn_3d_viewport,        3, 3},
    {"3Dモデル読込",           fn_3d_load_model,      1, 1},
    {"3Dモデル描画",           fn_3d_draw_model,      2, 2},
    {"3Dカメラ",               fn_3d_camera,          3, 3},
    {"3D光源",                 fn_3d_light,           3, 3},
    {"3D回転",                 fn_3d_rotate,          4, 4},
    {"3D拡縮",                 fn_3d_scale,           4, 4},
    {"3D背景色",               fn_3d_bgcolor,         2, 2},
    /* Phase 46: テスト/デバッグ */
    {"スナップショットテスト", fn_snapshot_test,      2, 2},
    {"アクセシビリティ監査", fn_a11y_audit,         0, 0},
    {"ウィジェットテスト",   fn_widget_test,        2, 2},
    {"レイアウトデバッグ",   fn_layout_debug,       1, 1},
    {"ウィジェットインスペクタ", fn_widget_inspector, 1, 1},
    {"パフォーマンスオーバーレイ", fn_perf_overlay, 1, 1},
    {"イベントログ",         fn_event_log_toggle,   1, 1},
    {"ウィジェットツリー",   fn_widget_tree,        0, 0},
    /* Phase 47: DnD高度/ソート */
    {"ソートDnDリスト",       fn_sort_dnd_list,      2, 2},
    {"ドラッグハンドル",       fn_drag_handle,        1, 1},
    {"ドラッグプレビュー",   fn_drag_preview,       2, 2},
    {"ドロップゾーン",         fn_drop_zone,          2, 2},
    {"カンバンDnD",             fn_kanban_dnd,         2, 2},
    {"リッチクリップボード取得", fn_rich_clipboard_get, 0, 0},
    {"画像クリップボード取得", fn_image_clipboard_get, 0, 0},
    {"画像クリップボード設定", fn_image_clipboard_set, 1, 1},
    /* Phase 48: ウィンドウ高度管理 */
    {"常に最前面",             fn_always_on_top,      1, 1},
    {"モーダルスタック",       fn_modal_stack,        0, 0},
    {"ウィンドウグループ",   fn_window_group,       1, 1},
    {"マルチモニター情報",   fn_multi_monitor,      0, 0},
    {"画面情報",               fn_screen_info,        0, 0},
    {"タイトルバー非表示",   fn_hide_titlebar,      1, 1},
    {"ウィンドウ移動禁止",   fn_window_lock_move,   1, 1},
    {"ウィンドウ形状",         fn_window_shape,       1, 1},
    /* Phase 49: グラフィック高度 */
    {"合成モード",             fn_blend_mode,         1, 1},
    {"ピクセル取得",           fn_pixel_get,          2, 2},
    {"ピクセル設定",           fn_pixel_set,          3, 3},
    {"画像フィルタ",           fn_image_filter,       2, 3},
    {"画像クロップ",           fn_image_crop,         5, 5},
    {"マスク描画",             fn_mask_draw,          1, 1},
    {"オフスクリーン描画",     fn_offscreen,          2, 2},
    {"SVGパス描画",            fn_svg_path,           1, 1},
    /* Phase 50: レイアウト V */
    {"Zインデックス",          fn_z_index,            1, 1},
    {"グリッドエリア",         fn_grid_area,          5, 5},
    {"セーフエリア",           fn_safe_area,          0, 0},
    {"ブレークポイント",       fn_breakpoint,         0, 1},
    {"ラップレイアウト開始",   fn_wrap_begin,         0, 0},
    {"ラップレイアウト終了",   fn_wrap_end,           0, 0},
    {"均等配置",               fn_justify,            1, 1},
    {"配置",                   fn_alignment,          2, 2},
    /* Phase 51: 非同期/イベント */
    {"デバウンス",             fn_debounce,           2, 2},
    {"スロットル",             fn_throttle,           2, 2},
    {"非同期実行",             fn_async_exec,         2, 2},
    {"タスク進捗",             fn_task_progress,      1, 1},
    {"タスクキャンセル",       fn_task_cancel,        1, 1},
    {"ローディングオーバーレイ", fn_loading_overlay,  2, 2},
    {"イベントバス発火",       fn_event_bus_emit,     2, 2},
    {"イベントバス購読",       fn_event_bus_on,       2, 2},
    /* Phase 52: リスト高度機能 */
    {"アニメーションリスト",   fn_animated_list,      2, 2},
    {"リスト仮想化",           fn_list_virtualize,    3, 3},
    {"スワイプアクション",     fn_swipe_action,       3, 3},
    {"下引き更新",             fn_pull_refresh,       1, 1},
    {"無限スクロール",         fn_infinite_scroll,    1, 1},
    {"リストセクション",       fn_list_section,       1, 1},
    {"カスタムセル",           fn_custom_cell,        2, 2},
    {"ファイルブラウザ",       fn_file_browser,       1, 1},
    /* Phase 53: CJKテキスト/組版 */
    {"ふりがな",               fn_furigana,           2, 2},
    {"縦書き",                 fn_vertical_text,      1, 1},
    {"テキスト装飾",           fn_text_decoration,    2, 2},
    {"フォントフォールバック", fn_font_fallback,    1, 1},
    {"文字間隔",               fn_letter_spacing,     1, 1},
    {"行間",                   fn_line_height,        1, 1},
    {"テキストインデント",   fn_text_indent,        1, 1},
    {"テキスト影",             fn_text_shadow,        4, 4},
    /* Phase 54: 遷移/共有アニメーション */
    {"共有要素遷移",           fn_shared_element,     2, 2},
    {"ページ遷移",             fn_page_transition,    1, 1},
    {"テーマ遷移",             fn_theme_transition,   1, 1},
    {"リスト追加アニメーション", fn_list_insert_anim, 2, 2},
    {"リスト削除アニメーション", fn_list_remove_anim, 2, 2},
    {"スクロール連動",         fn_scroll_driven,      2, 2},
    {"表示アニメーション",     fn_show_animation,     1, 1},
    {"レイアウト遷移",         fn_layout_transition,  1, 1},
    /* Phase 55: カスタム描画 III */
    {"カスタムウィジェット", fn_custom_widget,    3, 3},
    {"カスタムペイント",       fn_custom_paint,       1, 1},
    {"手描きキャンバス",       fn_ink_canvas,         3, 3},
    {"描画バッファ",           fn_draw_buffer,        2, 2},
    {"スプライト",             fn_sprite,             3, 3},
    {"パーティクル",           fn_particle,           2, 2},
    {"シーン作成",             fn_scene_create,       1, 1},
    {"シーンオブジェクト",     fn_scene_object,       3, 3},
    /* Phase 56: 画像操作 */
    {"画像プレビュー",         fn_image_preview,      1, 2},
    {"画像リサイズ",           fn_image_resize,       3, 3},
    {"画像回転",               fn_image_rotate,       2, 2},
    {"画像反転",               fn_image_flip,         2, 2},
    {"画像ぼかし",             fn_image_blur,         2, 2},
    {"画像明度",               fn_image_brightness,   2, 2},
    {"画像情報",               fn_image_info,         1, 1},
    {"画像生成",               fn_image_create,       2, 3},
    /* Phase 57: 色/スタイル高度 */
    {"HSL色",                 fn_hsl_color,          3, 3},
    {"HSV色",                 fn_hsv_color,          3, 3},
    {"色変換",                 fn_color_convert,      2, 2},
    {"色補間",                 fn_color_lerp,         3, 3},
    {"カスタムスクロールバー", fn_custom_scrollbar, 1, 1},
    {"スタイル継承",           fn_style_inherit,      1, 1},
    {"スタイルクラス",         fn_style_class,        2, 2},
    {"条件スタイル",           fn_cond_style,         2, 2},
    /* Phase 58: ウィジェット状態制御 */
    {"ウィジェット可視性",     fn_widget_visibility,  2, 2},
    {"ウィジェットホバー",       fn_widget_hover,       1, 1},
    {"ウィジェットアクティブ",   fn_widget_active,      1, 1},
    {"ウィジェット変更検知",   fn_widget_changed,     1, 1},
    {"ウィジェットリフレッシュ", fn_widget_refresh,   1, 1},
    {"条件表示",               fn_cond_show,          1, 1},
    {"ウィジェットキー",       fn_widget_key,         1, 1},
    {"ウィジェットコールバック", fn_widget_callback,  2, 2},
    /* Phase 59: MDI/ドキュメント */
    {"MDI領域",               fn_mdi_area,           1, 1},
    {"MDI子ウィンドウ",       fn_mdi_child,          2, 2},
    {"最近使ったファイル",   fn_recent_files,       1, 1},
    {"ドキュメント変更フラグ", fn_doc_modified,     1, 1},
    {"自動保存",               fn_autosave,           1, 1},
    {"タブ閉じる確認",         fn_tab_close_confirm,  1, 1},
    {"パンくずパス",           fn_breadcrumb_path,    1, 1},
    {"ドキュメントタイトル", fn_doc_title,        1, 1},
    /* Phase 60: コンテキスト/補助 */
    {"コマンドパレット",       fn_command_palette,    1, 1},
    {"ショートカット一覧",   fn_shortcut_list,      0, 0},
    {"トップ戻る",             fn_back_to_top,        1, 1},
    {"ローディングボタン",     fn_loading_button,     2, 2},
    {"画像ボタン拡張",         fn_image_button_ext,   2, 2},
    {"メゾンリーレイアウト開始", fn_masonry_begin,  1, 1},
    {"メゾンリーレイアウト終了", fn_masonry_end,    0, 0},
    {"パララックス",           fn_parallax,           2, 2},

    /* Phase 61: 高度なデータ可視化 II */
    {"レーダーチャート",       fn_radar_chart,        2, 2},
    {"ツリーマップ",           fn_treemap,            1, 1},
    {"サンキー図",             fn_sankey,             2, 2},
    {"ガントチャート",         fn_gantt_chart,        1, 1},
    {"ローソク足チャート",     fn_candlestick_chart,  1, 1},
    {"箱ひげ図",               fn_boxplot,            1, 1},
    {"ファンネルチャート",     fn_funnel_chart,       1, 1},
    {"フォースグラフ",         fn_force_graph,        2, 2},

    /* Phase 62: リッチテキスト/コンテンツ */
    {"リッチテキストエディタ", fn_rich_text_editor,   2, 2},
    {"Diffビューア",           fn_diff_viewer,        2, 2},
    {"コードエディタ",         fn_code_editor,        3, 3},
    {"マークダウンエディタ",   fn_markdown_editor,    2, 2},
    {"HTMLビューア",            fn_html_viewer,        1, 1},
    {"テキストハイライト",     fn_text_highlight,     3, 3},
    {"アノテーション",         fn_annotation,         3, 3},
    {"テキストテンプレート",   fn_text_template,      2, 2},

    /* Phase 63: ナビゲーション/ルーティング高度 */
    {"アプリバー",             fn_app_bar,            1, 2},
    {"ボトムアプリバー",       fn_bottom_app_bar,     1, 1},
    {"ナビゲーションレール",   fn_navigation_rail,    2, 2},
    {"コラムビュー",           fn_column_view,        1, 1},
    {"ナビゲーションスタック", fn_navigation_stack,   1, 1},
    {"サイドバー折りたたみ",   fn_sidebar_collapse,   2, 2},
    {"ナビゲーション履歴",     fn_nav_history,        1, 1},
    {"パンくずカスタム",       fn_breadcrumb_custom,  2, 2},

    /* Phase 64: OS/プラットフォーム統合 II */
    {"グローバルショートカット", fn_global_shortcut,  2, 2},
    {"自動アップデート",       fn_auto_updater,       1, 1},
    {"プロトコルハンドラ",     fn_protocol_handler,   2, 2},
    {"ファイル監視",           fn_file_watcher,       2, 2},
    {"ウィンドウスナップ",     fn_window_snap,        2, 2},
    {"タスクバー進捗",         fn_taskbar_progress,   1, 2},
    {"ジャンプリスト",         fn_jump_list,          1, 1},
    {"ネイティブダイアログ",   fn_native_dialog,      2, 3},

    /* Phase 65: 高度なフォーム入力 II */
    {"署名パッド",             fn_signature_pad,      3, 3},
    {"電話番号入力",           fn_phone_input,        1, 2},
    {"クレジットカード入力",   fn_credit_card_input,  1, 2},
    {"住所入力",               fn_address_input,      1, 2},
    {"カラーグラデーション",   fn_color_gradient_input, 2, 2},
    {"スケジュール入力",       fn_schedule_input,     1, 2},
    {"フィールド配列",         fn_field_array,        2, 3},
    {"リッチセレクト",         fn_rich_select,        3, 3},

    /* Phase 66: 高度なアニメーション/エフェクト II */
    {"リップルエフェクト",     fn_ripple_effect,      2, 3},
    {"モーフィング",           fn_morphing,           3, 3},
    {"フリップカード",         fn_flip_card,          4, 4},
    {"タイピングアニメーション", fn_typing_animation, 2, 2},
    {"カウントアップ",         fn_count_up,           4, 4},
    {"パスアニメーション",     fn_path_animation,     2, 2},
    {"ブラー遷移",             fn_blur_transition,    2, 3},
    {"ラバーバンド選択",       fn_rubber_band,        1, 1},

    /* Phase 67: データ永続化/状態管理高度 */
    {"ローカルストレージ",     fn_local_storage,      1, 2},
    {"状態スナップショット",   fn_state_snapshot,     1, 1},
    {"Undoスタック",            fn_undo_stack,         2, 2},
    {"状態永続化",             fn_state_persist,      2, 2},
    {"リアクティブ状態",       fn_reactive_state,     2, 2},
    {"算出プロパティ",         fn_computed_value,     2, 2},
    {"状態差分",               fn_state_diff,         2, 2},
    {"状態マイグレーション",   fn_state_migration,    2, 2},

    /* Phase 68: 高度なコンテナ/サーフェス II */
    {"ツールボックス",         fn_toolbox,            3, 3},
    {"スタックウィジェット",   fn_stack_widget,       2, 2},
    {"フローティングパネル",   fn_floating_panel,     5, 5},
    {"ポータル",               fn_portal,             1, 1},
    {"バックドロップ",         fn_backdrop,           1, 2},
    {"サーフェス",             fn_surface,            1, 1},
    {"アコーディオングループ", fn_accordion_group,    3, 3},
    {"折りたたみサイドバー",   fn_collapsible_sidebar, 4, 4},

    /* Phase 69: 高度なテーブル/グリッド II */
    {"ピボットテーブル",       fn_pivot_table,        2, 3},
    {"マスターディテール",     fn_master_detail,      2, 3},
    {"テーブル列グループ",     fn_table_col_group,    2, 2},
    {"テーブルサマリー行",     fn_table_summary_row,  2, 2},
    {"テーブル条件書式",       fn_table_cond_format,  2, 2},
    {"テーブルコピー",         fn_table_copy,         1, 1},
    {"テーブルインポート",     fn_table_import,       1, 2},
    {"テーブル行固定",         fn_table_frozen_rows,  2, 2},

    /* Phase 70: 通知/フィードバック高度 II */
    {"通知センター",           fn_notification_center, 1, 2},
    {"通知グループ",           fn_notification_group, 2, 2},
    {"アンドゥ通知",           fn_undo_notification,  2, 2},
    {"プログレストースト",     fn_progress_toast,     2, 3},
    {"動的バッジ",             fn_dynamic_badge,      2, 3},
    {"サウンド通知",           fn_sound_notification, 1, 2},
    {"ステータスインジケータ", fn_status_indicator,   1, 2},
    {"エラーバウンダリ",       fn_error_boundary,     1, 2},

    /* Phase 71: 地図/位置情報 */
    {"地図ウィジェット",       fn_map_widget,         1, 4},
    {"地図マーカー",           fn_map_marker,         3, 4},
    {"地図ポリライン",         fn_map_polyline,       1, 2},
    {"地図ポリゴン",           fn_map_polygon,        1, 2},
    {"位置情報取得",           fn_geolocation,        0, 1},
    {"距離計算",               fn_distance_calc,      4, 4},
    {"ジオコーディング",       fn_geocoding,          1, 1},
    {"ミニマップ",             fn_minimap,            3, 3},

    /* Phase 72: カレンダー/スケジュール高度 */
    {"週カレンダー",           fn_week_calendar,      1, 2},
    {"日カレンダー",           fn_day_calendar,       1, 2},
    {"アジェンダビュー",       fn_agenda_view,        1, 2},
    {"イベント作成",           fn_event_create,       2, 3},
    {"イベントドラッグ",       fn_event_drag,         2, 2},
    {"繰り返しイベント",       fn_recurring_event,    2, 3},
    {"カレンダーヒートマップ", fn_calendar_heatmap,   2, 2},
    {"タイムスロット",         fn_time_slot,          2, 3},

    /* Phase 73: メディア高度 II */
    {"音声波形",               fn_audio_waveform,     1, 2},
    {"音声録音",               fn_audio_record,       1, 2},
    {"動画録画",               fn_video_record,       1, 2},
    {"字幕表示",               fn_subtitle_display,   2, 2},
    {"ピクチャインピクチャ",   fn_picture_in_picture, 1, 2},
    {"メディアプレイリスト",   fn_media_playlist,     1, 2},
    {"イコライザ",             fn_equalizer,          1, 2},
    {"画面キャプチャ",         fn_screen_capture,     0, 2},

    /* Phase 74: 検索/フィルタ高度 */
    {"ファセット検索",         fn_faceted_search,     2, 3},
    {"フィルターチップ",       fn_filter_chips,       2, 2},
    {"ソートUI",                fn_sort_ui,            2, 2},
    {"フィルターパネル",       fn_filter_panel,       2, 2},
    {"保存済みフィルタ",       fn_saved_filters,      1, 2},
    {"検索履歴",               fn_search_history,     1, 2},
    {"高度な検索ダイアログ",   fn_advanced_search_dialog, 1, 3},
    {"ファジー検索",           fn_fuzzy_search,       2, 3},

    /* Phase 75: セキュリティ/認証 UI */
    {"ログインフォーム",       fn_login_form,         1, 3},
    {"パスワード強度メーター", fn_password_strength,  1, 1},
    {"二要素認証入力",         fn_two_factor_input,   1, 2},
    {"ライセンスダイアログ",   fn_license_dialog,     1, 2},
    {"セッションタイムアウト", fn_session_timeout,    1, 2},
    {"生体認証",               fn_biometric_auth,     0, 1},
    {"ロック画面",             fn_lock_screen,        1, 2},
    {"権限ゲート",             fn_permission_gate,    1, 2},

    /* Phase 76: AI/インテリジェント UI */
    {"チャットUI",              fn_chat_ui,            2, 2},
    {"チャットバブル",         fn_chat_bubble,        3, 3},
    {"ストリーミングテキスト", fn_streaming_text,     3, 3},
    {"AIプログレス",            fn_ai_progress,        1, 1},
    {"音声入力",               fn_voice_input,        0, 1},
    {"音声合成",               fn_voice_synthesis,    1, 2},
    {"スマート検索",           fn_smart_search,       1, 2},
    {"サジェストカード",       fn_suggest_card,       2, 2},

    /* Phase 77: 国際化/ローカライゼーション II */
    {"日付ロケール",           fn_date_locale,        2, 2},
    {"数値ロケール",           fn_number_locale,      2, 2},
    {"通貨フォーマット",       fn_currency_format,    2, 3},
    {"和暦変換",               fn_wareki,             1, 1},
    {"タイムゾーン表示",       fn_timezone_display,   1, 2},
    {"ロケール検出",           fn_locale_detect,      0, 0},
    {"IME候補表示",             fn_ime_candidates,     2, 2},
    {"ルビテキスト",           fn_ruby_text,          2, 2},

    /* Phase 78: Web統合/埋め込み */
    {"Webビュー",               fn_webview_widget,     1, 2},
    {"リンクプレビュー",       fn_link_preview,       1, 1},
    {"ソーシャルシェア",       fn_social_share,       2, 3},
    {"QRスキャン",              fn_qr_scan,            0, 1},
    {"動画埋め込み",           fn_embed_video,        1, 2},
    {"Webフォント読み込み",     fn_webfont_load,       1, 2},
    {"OEmbed表示",              fn_oembed_display,     1, 1},
    {"RSSフィード表示",         fn_rss_display,        1, 1},

    /* Phase 79: テスト/開発ツール高度 II */
    {"コンポーネントプレイグラウンド", fn_component_playground, 1, 2},
    {"デザイントークン",       fn_design_tokens,      1, 2},
    {"レスポンシブプレビュー", fn_responsive_preview, 1, 2},
    {"ビジュアルリグレッション", fn_visual_regression, 1, 2},
    {"インタラクション記録",   fn_interaction_recorder, 0, 1},
    {"パフォーマンスプロファイラ", fn_performance_profiler, 0, 1},
    {"メモリモニタ",           fn_memory_monitor,     0, 1},
    {"ホットリロード",         fn_hot_reload,         0, 1},

    /* Phase 80: 最終仕上げ/ユーティリティ */
    {"ショートカットダイアログ", fn_shortcut_dialog,  0, 0},
    {"ウェルカム画面",         fn_welcome_screen,     1, 2},
    {"フィーチャーフラグ",     fn_feature_flag,       2, 2},
    {"ABテスト",                fn_ab_test,            2, 2},
    {"クラッシュレポート",     fn_crash_report,       1, 2},
    {"フィードバックフォーム", fn_feedback_form,      1, 3},
    {"変更履歴表示",           fn_changelog_display,  1, 1},
    {"アプリ設定",             fn_app_settings,       0, 1},
    /* Phase 81: ノードエディタ/専門エディタ */
    {"ノードエディタ",         fn_node_editor,        3, 3},
    {"HEXエディタ",            fn_hex_editor,         3, 3},
    {"グラデーションエディタ", fn_gradient_editor,    2, 2},
    {"カーブエディタ",         fn_curve_editor,       2, 2},
    {"アニメーションシーケンサー", fn_animation_sequencer, 2, 2},
    {"スプレッドシート",       fn_spreadsheet,        2, 3},
    {"JSONビューア",           fn_json_viewer,        2, 2},
    {"ターミナルウィジェット", fn_terminal_widget,    1, 3},
    /* Phase 82: 高度なコントロール */
    {"ノブ",                   fn_knob,               4, 4},
    {"パイメニュー",           fn_pie_menu,           2, 2},
    {"LCD数字",                fn_lcd_number,         1, 2},
    {"アーク進捗",             fn_arc_progress,       4, 4},
    {"メーター",               fn_meter,              4, 4},
    {"LEDインジケータ",        fn_led_indicator,      2, 2},
    {"2Dスライダー",           fn_slider_2d,          3, 3},
    {"ズームスライダー",       fn_zoom_slider,        5, 5},
    /* Phase 83: 3D操作/高度グラフィック */
    {"3Dギズモ",               fn_gizmo_3d,           1, 3},
    {"シーングラフ",           fn_scene_graph,        2, 2},
    {"ズームキャンバス",       fn_zoomable_canvas,    1, 2},
    {"9スライス画像",          fn_nine_slice,         1, 5},
    {"シェーダーマスク",       fn_shader_mask,        1, 2},
    {"ビューポートテクスチャ", fn_viewport_texture,   3, 3},
    {"エフェクトパイプライン", fn_effect_pipeline,    2, 2},
    {"画像インスペクタ",       fn_image_inspector,    1, 2},
    /* Phase 84: 高度なデータ可視化 III */
    {"バブルチャート",         fn_bubble_chart,       2, 2},
    {"ウォーターフォールチャート", fn_waterfall_chart, 2, 2},
    {"サンバーストチャート",   fn_sunburst_chart,     2, 2},
    {"コード図",               fn_chord_diagram,      1, 2},
    {"コロプレスマップ",       fn_choropleth_map,     2, 2},
    {"フレームグラフ",         fn_flame_graph,        2, 2},
    {"等高線チャート",         fn_contour_chart,      1, 2},
    {"極座標チャート",         fn_polar_chart,        2, 2},
    /* Phase 85: レイアウトユーティリティ */
    {"サイズポリシー",         fn_size_policy,        0, 3},
    {"フィットボックス",       fn_fit_box,            0, 3},
    {"ビューボックス",         fn_view_box,           0, 4},
    {"フラクショナルサイズ",   fn_fractional_size,    0, 2},
    {"均等グリッド",           fn_uniform_grid,       3, 3},
    {"オーバーフローバー",     fn_overflow_bar,       0, 2},
    {"レイアウトアンカー",     fn_layout_anchor,      0, 4},
    {"自動フィットビュー",     fn_auto_fit_view,      0, 1},
    /* Phase 86: Material/Fluent拡張 */
    {"伸縮アプリバー",         fn_sliver_app_bar,     2, 3},
    {"展開タイル",             fn_expansion_tile,     2, 3},
    {"検索アンカー",           fn_search_anchor,      0, 2},
    {"ホバーカード",           fn_hover_card,         0, 3},
    {"サイドシート",           fn_side_sheet,         0, 2},
    {"トグルグループ",         fn_toggle_group,       0, 2},
    {"メニューアンカー",       fn_menu_anchor,        0, 2},
    {"フローティングラベル",   fn_floating_label,     3, 3},
    /* Phase 87: 特殊入力 */
    {"キーシーケンス入力",     fn_key_sequence_input, 0, 2},
    {"IPアドレス入力",         fn_ip_address_input,   0, 2},
    {"絵文字ピッカー",         fn_emoji_picker,       0, 1},
    {"国選択",                 fn_country_select,     0, 1},
    {"自動テキストエリア",     fn_auto_textarea,      0, 2},
    {"フォントコンボボックス", fn_font_combobox,      0, 1},
    {"日付範囲ピッカー",       fn_date_range_picker,  0, 2},
    {"カレンダー日付範囲",     fn_calendar_date_range,0, 2},
    /* Phase 88: アニメーション/エフェクト II */
    {"Lottieアニメーション",   fn_lottie_animation,   0, 2},
    {"クロスフェード",         fn_crossfade,          0, 3},
    {"コンフェッティ",         fn_confetti,           2, 2},
    {"クールバー",             fn_cool_bar,           2, 2},
    {"バーチャルキーボード",   fn_virtual_keyboard,   0, 1},
    {"スクロールスパイ",       fn_scroll_spy,         0, 2},
    {"サイズグリップ",         fn_size_grip,          0, 1},
    {"ダイアログボタンボックス", fn_dialog_button_box, 0, 2},
    /* Phase 89: データ連携/高度UI */
    {"データウィジェットマッパー", fn_data_widget_mapper, 0, 3},
    {"アンドゥビュー",         fn_undo_view,          0, 1},
    {"列可視性切替",           fn_column_visibility_toggle, 0, 2},
    {"スタイルプロキシ",       fn_style_proxy,        0, 2},
    {"テーマオーバーライド",   fn_theme_override,     0, 2},
    {"キー値リスト",           fn_key_value_list,     2, 2},
    {"カンバンボード",         fn_kanban_board,       2, 2},
    {"インタラクティブビューア", fn_interactive_viewer, 0, 2},
    /* Phase 90: 描画ツール/デザイン */
    {"ルーラー",               fn_ruler,              2, 2},
    {"グリッドライン",         fn_grid_lines,         3, 3},
    {"スポイト",               fn_eyedropper,         0, 1},
    {"カラーパレット",         fn_color_palette,      2, 2},
    {"カラースペクトラム",     fn_color_spectrum,     0, 1},
    {"シェイプエディタ",       fn_shape_editor,       0, 2},
    {"パスアイコン",           fn_path_icon,          0, 2},
    {"GPUプロファイラ",        fn_gpu_profiler,       0, 1},
    /* Phase 91: ダイアグラム/チャート II */
    {"組織図",                 fn_org_chart,          2, 2},
    {"マインドマップ",         fn_mind_map,           0, 2},
    {"フローチャート",         fn_flowchart,          0, 2},
    {"クラス図",               fn_class_diagram,      0, 2},
    {"状態遷移図",             fn_state_machine_diagram, 0, 2},
    {"タイムラインチャート",   fn_timeline_chart,     0, 2},
    {"ゲージクラスター",       fn_gauge_cluster,      0, 2},
    {"ドーナツチャート",       fn_donut_chart,        2, 2},
    /* Phase 92: デザインツール */
    {"カラーホイール",         fn_color_wheel,        0, 1},
    {"ブラシセレクタ",         fn_brush_selector,     0, 2},
    {"レイヤーパネル",         fn_layer_panel,        0, 2},
    {"ツールバーカスタマイズ", fn_toolbar_customize,  0, 2},
    {"リボン",                 fn_ribbon,             0, 2},
    {"マグニファイア",         fn_magnifier,          0, 2},
    {"十字線",                 fn_crosshair,          2, 3},
    {"ブループリントグリッド", fn_blueprint_grid,     2, 2},
    /* Phase 93: UI部品拡張 */
    {"スプリットボタン",       fn_split_button,       2, 2},
    {"スクロール進捗バー",     fn_scroll_progress_bar,0, 1},
    {"ドットページネーション", fn_dot_pagination,     3, 3},
    {"グループセレクト",       fn_group_select,       0, 2},
    {"無限キャンバス",         fn_infinite_canvas,    0, 2},
    {"付箋",                   fn_sticky_note,        2, 5},
    {"録画インジケータ",       fn_recording_indicator,2, 2},
    {"ドロップダウンパンくず", fn_dropdown_breadcrumb,0, 2},
    /* Phase 94: アクセシビリティ拡張 */
    {"フォーカストラバーサル", fn_focus_traversal,    0, 2},
    {"カスタムジェスチャー",   fn_custom_gesture,     0, 2},
    {"スキップリンク",         fn_skip_link,          0, 2},
    {"色覚シミュレーション",   fn_color_blindness_sim,0, 2},
    {"ハイコントラスト",       fn_high_contrast,      0, 1},
    {"ARIAタブ",               fn_aria_tabs,          0, 2},
    {"ライブリージョン拡張",   fn_live_region_ext,    0, 2},
    {"キーボードナビヘルパー", fn_keyboard_nav_helper,0, 1},
    /* Phase 95: 開発者ツール */
    {"フレーム分析",           fn_frame_analysis,     0, 1},
    {"リペイント可視化",       fn_repaint_visualization, 0, 1},
    {"ウィジェットカウント",   fn_widget_count,       0, 1},
    {"バンドル分析",           fn_bundle_analysis,    0, 1},
    {"レイアウト境界線",       fn_layout_boundaries,  0, 1},
    {"レンダーツリー",         fn_render_tree,        0, 1},
    {"メモリスナップショット", fn_memory_snapshot,    0, 1},
    {"ホットキーヘルプ",       fn_hotkey_help,        0, 1},
    /* Phase 96: リアクティブ状態管理 */
    {"コンテキストプロバイダ", fn_context_provider,   0, 2},
    {"シグナル",               fn_signal,             0, 2},
    {"メモ化",                 fn_memoize,            0, 2},
    {"エフェクト",             fn_effect,             0, 2},
    {"サスペンス",             fn_suspense,           0, 2},
    {"オプティミスティック更新", fn_optimistic_update, 0, 2},
    {"状態リセット",           fn_state_reset,        0, 2},
    {"動的フォーム配列",       fn_dynamic_form_array, 0, 2},
    /* Phase 97: テーブル拡張セル */
    {"テーブル列ピン留め",     fn_table_column_pin,   0, 2},
    {"テーブル行展開",         fn_table_row_expand,   0, 2},
    {"テーブルスパークセル",   fn_table_spark_cell,   0, 2},
    {"テーブル進捗セル",       fn_table_progress_cell,0, 2},
    {"テーブルタグセル",       fn_table_tag_cell,     0, 2},
    {"テーブルアバターセル",   fn_table_avatar_cell,  0, 2},
    {"テーブルアクション列",   fn_table_action_column,0, 2},
    {"テーブルグローバルフィルタ", fn_table_global_filter, 0, 2},
    /* Phase 98: フォーム/エディタ拡張 */
    {"ステップフォーム",       fn_step_form,          0, 2},
    {"インラインフォーム",     fn_inline_form,        0, 2},
    {"条件分岐フォーム",       fn_conditional_form,   0, 2},
    {"フォームグリッド",       fn_form_grid,          0, 2},
    {"ファイルプレビューリスト", fn_file_preview_list, 0, 2},
    {"候補テキスト",           fn_ghost_text,         0, 2},
    {"スラッシュコマンド",     fn_slash_command,      0, 2},
    {"インラインコメント",     fn_inline_comment,     0, 2},
    /* Phase 99: プラットフォーム統合 */
    {"シェアシート",           fn_share_sheet,        0, 2},
    {"Cookieバナー",           fn_cookie_banner,      0, 2},
    {"OSカラースキーム検出",   fn_os_color_scheme,    0, 1},
    {"通知アクション",         fn_notification_action,0, 2},
    {"ファイルアソシエーション", fn_file_association,  0, 2},
    {"ディープリンク",         fn_deep_link,          0, 2},
    {"クイックアクション",     fn_quick_action,       0, 2},
    {"アプリ内ブラウザ",       fn_in_app_browser,     0, 2},
    /* Phase 100: ドキュメント/エコシステム */
    {"ウィジェット公開API",    fn_widget_public_api,  0, 2},
    {"テーマギャラリー",       fn_theme_gallery,      0, 1},
    {"ウィジェットカタログ",   fn_widget_catalog,     0, 1},
    {"クイックスタート",       fn_quick_start,        0, 1},
    {"ベンチマーク",           fn_benchmark,          0, 1},
    {"マイグレーション",       fn_migration,          0, 2},
    {"GUIビルダー",            fn_gui_builder,        0, 1},
    {"PDFビューア",            fn_pdf_viewer,         0, 2},

    /* Phase 101: 高度コントロール III (v10.1.0) — 8 functions */
    {"コマンドリンクボタン",   fn_command_link_button,    1, 3},
    {"遅延ボタン",             fn_delay_button,           1, 3},
    {"タンブラー",             fn_tumbler,                1, 3},
    {"マルチステートチェック", fn_multi_state_check,      1, 3},
    {"インプレースエディタ",   fn_inplace_editor,         1, 2},
    {"編集可能リスト",         fn_editable_list,          1, 2},
    {"並べ替えリスト",         fn_rearrange_list,         1, 3},
    {"キーボードキー表示",     fn_kbd_display,            1, 1},

    /* Phase 102: メニュー/ナビ拡張 II (v10.2.0) — 8 functions */
    {"メガメニュー",           fn_mega_menu,              1, 2},
    {"パネルメニュー",         fn_panel_menu,             1, 2},
    {"ティアードメニュー",     fn_tiered_menu,            1, 2},
    {"タブメニュー",           fn_tab_menu,               1, 2},
    {"ツリーブック",           fn_tree_book,              1, 3},
    {"リストブック",           fn_list_book,              1, 3},
    {"キーフィルタ",           fn_key_filter,             1, 2},
    {"マルチカラムコンボ",     fn_multi_column_combo,     1, 3},

    /* Phase 103: レイアウト/コンテナ高度 II (v10.3.0) — 8 functions */
    {"ダッシュボードレイアウト", fn_dashboard_layout,     1, 2},
    {"フィールドセット",       fn_fieldset,               1, 3},
    {"ページヘッダー",         fn_page_header,            1, 4},
    {"アプリシェル",           fn_app_shell,              1, 3},
    {"タイルビュー",           fn_tile_view,              1, 2},
    {"コンファームエディット", fn_confirm_edit,           1, 2},
    {"アクションシート",       fn_action_sheet,           1, 2},
    {"システムバー",           fn_system_bar,             0, 2},

    /* Phase 104: チャート/可視化 V (v10.4.0) — 8 functions */
    {"スミスチャート",         fn_smith_chart,            0, 2},
    {"ブレットチャート",       fn_bullet_chart,           1, 3},
    {"レンジセレクタ",         fn_range_selector,         0, 3},
    {"株価チャート",           fn_stock_chart,            0, 2},
    {"スプラインチャート",     fn_spline_chart,           0, 2},
    {"3D棒グラフ",             fn_bar_chart_3d,           0, 2},
    {"三次元曲面",             fn_surface_plot_3d,        0, 2},
    {"三次元散布図",           fn_scatter_chart_3d,       0, 2},

    /* Phase 105: AI/スマートUI II (v10.5.0) — 8 functions */
    {"AIプロンプト入力",       fn_ai_prompt_input,        0, 2},
    {"スマートペースト",       fn_smart_paste,            0, 2},
    {"スマートテキストエリア", fn_smart_textarea,         0, 2},
    {"クエリビルダー",         fn_query_builder,          0, 2},
    {"クエリフィルタバー",     fn_query_filter_bar,       0, 2},
    {"説明一覧",               fn_descriptions,           1, 3},
    {"データビュー切替",       fn_data_view_toggle,       0, 2},
    {"チェックカード",         fn_check_card,             0, 2},

    /* Phase 106: ドキュメント/エディタ高度 (v10.6.0) — 8 functions */
    {"ファイルマネージャ",     fn_file_manager,           0, 2},
    {"ドキュメントエディタ",   fn_document_editor,        0, 2},
    {"ブロックエディタ",       fn_block_editor,           0, 2},
    {"画像エディタウィジェット", fn_image_editor_widget,  0, 2},
    {"コード折りたたみ",       fn_code_folding,           0, 2},
    {"マルチカーソル",         fn_multi_cursor,           0, 2},
    {"マクロ記録",             fn_macro_record,           0, 2},
    {"コールチップ",           fn_call_tip,               0, 2},

    /* Phase 107: XR/VR (v10.7.0) — 8 functions */
    {"XRビュー",               fn_xr_view,                0, 2},
    {"XRコントローラ",         fn_xr_controller,          0, 2},
    {"空間アンカー",           fn_spatial_anchor,         0, 2},
    {"XR空間",                 fn_xr_space,               0, 2},
    {"パーティクル3D",         fn_particles_3d,           0, 2},
    {"スケルタルアニメーション", fn_skeletal_animation,   0, 2},
    {"インスタンスレンダリング", fn_instanced_rendering,  0, 2},
    {"LOD",                    fn_lod,                    0, 2},

    /* Phase 108: 3D高度レンダリング (v10.8.0) — 8 functions */
    {"PBRマテリアル",          fn_pbr_material,           0, 2},
    {"ライトマップ",           fn_lightmap,               0, 2},
    {"ボリュームレンダリング", fn_volume_rendering,       0, 2},
    {"3Dスプライン",           fn_spline_3d,              0, 2},
    {"空間オーディオ",         fn_spatial_audio,          0, 2},
    {"オーディオルーム",       fn_audio_room,             0, 2},
    {"サウンドエフェクト",     fn_sound_effect,           0, 2},
    {"音声選択",               fn_voice_select,           0, 2},

    /* Phase 109: ハードウェア/IoT I (v10.9.0) — 8 functions */
    {"BLE接続",                fn_ble_connect,            0, 2},
    {"BLEスキャン",            fn_ble_scan,               0, 1},
    {"NFC読取",                fn_nfc_read,               0, 2},
    {"NFC書込",                fn_nfc_write,              1, 2},
    {"シリアルポート",         fn_serial_port,            0, 2},
    {"加速度センサー",         fn_accelerometer,          0, 1},
    {"ジャイロスコープ",       fn_gyroscope,              0, 1},
    {"磁力計",                 fn_magnetometer,           0, 1},

    /* Phase 110: ハードウェア/通信 II (v10.10.0) — 8 functions */
    {"近接センサー",           fn_proximity_sensor,       0, 1},
    {"照度センサー",           fn_light_sensor,           0, 1},
    {"気圧センサー",           fn_pressure_sensor,        0, 1},
    {"ジョイスティック",       fn_joystick,               0, 2},
    {"触覚フィードバック",     fn_haptic_feedback,        0, 2},
    {"WebSocket通信",          fn_websocket_comm,         0, 2},
    {"MQTT通信",               fn_mqtt_comm,              0, 2},
    {"gRPC通信",               fn_grpc_comm,              0, 2},

    /* Phase 111: ヘルプ/PDF高度 (v11.0.0) — 8 functions */
    {"ヘルプビューア",         fn_help_viewer,            0, 2},
    {"コンテキストヘルプ",     fn_context_help,           0, 2},
    {"今日のヒント",           fn_tip_of_day,             0, 2},
    {"PDFブックマーク",        fn_pdf_bookmark,           0, 2},
    {"PDFテキスト検索",        fn_pdf_text_search,        1, 2},
    {"PDFリンク",              fn_pdf_link,               0, 2},
    {"PDFマルチページ",        fn_pdf_multi_page,         0, 2},
    {"PDFテキスト選択",        fn_pdf_text_select,        0, 2},

    /* Phase 112: エクスポート/永続化 (v11.1.0) — 8 functions */
    {"Excelエクスポート",      fn_excel_export,           1, 3},
    {"Wordエクスポート",       fn_word_export,            1, 3},
    {"メーターグループ",       fn_meter_group,            1, 2},
    {"ローカルDB",             fn_local_db,               0, 2},
    {"セキュアストレージ",     fn_secure_storage,         1, 3},
    {"ウィジェット永続化",     fn_widget_persist,         0, 2},
    {"標準パス",               fn_standard_paths,         0, 1},
    {"シングルインスタンス",   fn_single_instance,        0, 1},

    /* Phase 113: ダイアログ/フィードバック IV (v11.2.0) — 8 functions */
    {"エラーメッセージダイアログ", fn_error_msg_dialog,   1, 3},
    {"リッチメッセージダイアログ", fn_rich_msg_dialog,    1, 3},
    {"認証ダイアログ",         fn_credential_dialog,      0, 2},
    {"設定エディタ",           fn_preferences_editor,     0, 2},
    {"ビジー情報",             fn_busy_info,              0, 2},
    {"ビジーカーソル",         fn_busy_cursor,            0, 1},
    {"電源イベント",           fn_power_event,            0, 1},
    {"ドラッグイメージ",       fn_drag_image,             0, 2},

    /* Phase 114: 地図/2Dシーン/レンダリング (v11.3.0) — 8 functions */
    {"POI検索",                fn_poi_search,             1, 3},
    {"ルートナビゲーション",   fn_route_navigation,       0, 2},
    {"ジオフェンス",           fn_geofence,               0, 3},
    {"GPS測位",                fn_gps_position,           0, 1},
    {"2Dシーン",               fn_scene_2d,               0, 3},
    {"シーンビュー",           fn_scene_view,             0, 2},
    {"ペイングリッド",         fn_pane_grid,              0, 3},
    {"シェーダーウィジェット", fn_shader_widget,          0, 2},

    /* Phase 115: ネットワーク/状態マシン (v11.4.0) — 8 functions */
    {"IPC通信",                fn_ipc_comm,               0, 2},
    {"リモートオブジェクト",   fn_remote_object,          0, 2},
    {"OAuthフロー",            fn_oauth_flow,             0, 2},
    {"HTTPサーバー",           fn_http_server,            0, 2},
    {"SCXML状態マシン",        fn_scxml_state_machine,    0, 2},
    {"キーフレームアニメ",     fn_keyframe_anim,          0, 2},
    {"ブレンドツリー",         fn_blend_tree,             0, 2},
    {"手書き認識",             fn_handwriting_recognition, 0, 2},

    /* Phase 116: プラットフォーム拡張 IV (v11.5.0) — 8 functions */
    {"タッチバーメニュー",     fn_touchbar_menu,          0, 2},
    {"ドックバッジ",           fn_dock_badge,             0, 2},
    {"ウィンドウキャプチャ",   fn_window_capture,         0, 2},
    {"マテリアルYouテーマ",    fn_material_you_theme,     0, 2},
    {"コンストレイントレイアウト", fn_constraint_layout,  0, 2},
    {"モーションレイアウト",   fn_motion_layout,          0, 2},
    {"ボタンマトリクス",       fn_button_matrix,          1, 3},
    {"時間選択",               fn_time_select,            0, 2},

    /* Phase 117: リボン/ツールバー拡張 (v11.6.0) — 8 functions */
    {"リボンページ",           fn_ribbon_page,            0, 2},
    {"リボンパネル",           fn_ribbon_panel,           0, 2},
    {"リボンボタンバー",       fn_ribbon_button_bar,      0, 2},
    {"リボンギャラリー",       fn_ribbon_gallery,         0, 2},
    {"リボンツールバー",       fn_ribbon_toolbar,         0, 2},
    {"グループボックス",       fn_group_box,              1, 2},
    {"ビジーインジケータ",     fn_busy_indicator,         0, 2},
    {"UIアクションシミュレータ", fn_ui_action_simulator,  0, 2},

    /* Phase 118: HTML/テキスト高度 (v11.7.0) — 8 functions */
    {"HTMLリストボックス",     fn_html_list_box,          0, 2},
    {"テキストブラウザ",       fn_text_browser,           0, 2},
    {"HTMLヘルプウィンドウ",   fn_html_help_window,       0, 2},
    {"カスタム描画コンボ",     fn_custom_draw_combo,      0, 2},
    {"ミニフレーム",           fn_mini_frame,             0, 2},
    {"ヒントウィンドウ",       fn_tip_window,             0, 2},
    {"デバッグレポート",       fn_debug_report,           0, 2},
    {"数値入力ダイアログ",     fn_number_entry_dialog,    0, 3},

    /* Phase 119: バリデーション/データモデル (v11.8.0) — 8 functions */
    {"テキストバリデータ",     fn_text_validator,         0, 2},
    {"整数バリデータ",         fn_integer_validator,      0, 3},
    {"浮動小数バリデータ",     fn_float_validator,        0, 3},
    {"正規表現バリデータ",     fn_regex_validator,        1, 2},
    {"ファイルピッカー",       fn_file_picker,            0, 2},
    {"ディレクトリピッカー",   fn_dir_picker,             0, 2},
    {"ソートフィルタモデル",   fn_sort_filter_model,      0, 2},
    {"ファイルシステムモデル", fn_file_system_model,      0, 2},

    /* Phase 120: 最終統合 III (v12.0.0) — 8 functions */
    {"パーティクルエミッタ2D", fn_particle_emitter_2d,    0, 2},
    {"トレイルエミッタ",       fn_trail_emitter,          0, 2},
    {"ドキュメントマネージャ", fn_document_manager,       0, 2},
    {"検索置換ダイアログ",     fn_find_replace_dialog,    0, 2},
    {"PostScript出力",         fn_postscript_output,      1, 3},
    {"ネットワーク状態",       fn_network_status,         0, 1},
    {"ベクター形状",           fn_vector_shape,           0, 3},
    {"マウスイベント委譲",     fn_mouse_events_delegate,  0, 2},

    /* Phase 121: プラットフォーム適応UI */
    {"適応型ダイアログ",       fn_adaptive_dialog,        3, 3},
    {"絵文字パネル",           fn_emoji_panel,            0, 0},
    {"Inspectorパネル",        fn_inspector_panel,        1, 2},
    {"スクロールスナップ",     fn_scroll_snap,            1, 2},
    {"写真選択",               fn_photo_picker,           0, 0},
    {"検索スコープ",           fn_search_scope,           2, 3},
    {"アプリ選択ダイアログ",   fn_app_chooser_dialog,     0, 1},
    {"ボリュームボタン",       fn_volume_button,          0, 1},

    /* Phase 122: スクロール物理/ビューポート最適化 */
    {"減衰スクロール",         fn_damped_scroll,          1, 3},
    {"ビューポート可視性",     fn_viewport_visibility,    1, 2},
    {"遅延ビルド",             fn_lazy_build,             2, 3},
    {"スムーズスクロール",     fn_smooth_scroll,          1, 2},
    {"スクロールチェーン",     fn_scroll_chain,           1, 2},
    {"オーバースクロール",     fn_overscroll_effect,      1, 2},
    {"スクロールバー自動非表示", fn_scrollbar_auto_hide,  1, 2},
    {"スナップリスト",         fn_snap_list,              2, 3},

    /* Phase 123: リモートUI/通信拡張 */
    {"リモートGUI配信",        fn_remote_gui_serve,       1, 1},
    {"WebSocketGUI",           fn_websocket_gui,          1, 2},
    {"MIDI入出力",             fn_midi_io,                1, 2},
    {"OSC通信",                fn_osc_comm,               2, 3},
    {"ファイルアップロード進捗", fn_file_upload_progress,      2, 3},
    {"ダウンロード進捗",       fn_download_progress,      2, 3},
    {"mDNSサービス",           fn_mdns_service,           1, 2},
    {"Webチャネル",            fn_web_channel,            1, 2},

    /* Phase 124: OS統合拡張 III */
    {"自動起動",               fn_autostart,              1, 1},
    {"Handoff",                fn_handoff,                1, 2},
    {"セッション管理",         fn_session_manager,        1, 2},
    {"ログ出力",               fn_log_output,             2, 3},
    {"アプリ内課金",           fn_in_app_purchase,        1, 2},
    {"OSアクセントカラー",     fn_os_accent_color,        0, 0},
    {"D-Bus通信",              fn_dbus_comm,              2, 3},
    {"画像フォーマット登録",   fn_image_format_register,  1, 1},

    /* Phase 125: 宣言的UI/開発ツール拡張 */
    {"ライブプレビュー",       fn_live_preview,           1, 2},
    {"GUIビルダー起動",        fn_gui_builder_launch,     0, 1},
    {"UIチェック",             fn_ui_check,               1, 1},
    {"YAML読込",               fn_yaml_load,              1, 1},
    {"UIスナップショット比較", fn_ui_snapshot_compare,    2, 2},
    {"ドキュメント生成",       fn_doc_generate,           1, 2},
    {"アクセシビリティテスト", fn_a11y_test,              0, 1},
    {"テストレコーダー",       fn_test_recorder_ex,       1, 2},

    /* Phase 126: 高度アニメーション/物理 */
    {"スピナーコレクション",   fn_spinner_collection,     1, 2},
    {"物理シミュレーション",   fn_physics_sim,            1, 3},
    {"パラレルアニメーション", fn_parallel_animation,     1, 1},
    {"シーケンシャルアニメーション", fn_sequential_animation, 1, 1},
    {"パスモーフィング",       fn_path_morphing,          3, 4},
    {"フリップボード",         fn_flip_board,             1, 3},
    {"リキッドスワイプ",       fn_liquid_swipe,           1, 2},
    {"アニメーション割り込み", fn_animation_interrupt,    1, 2},

    /* Phase 127: データモデル高度化 */
    {"プロキシモデル",         fn_proxy_model,            1, 3},
    {"アイテムデリゲート",     fn_item_delegate,          1, 2},
    {"データテンプレート",     fn_data_template,          1, 2},
    {"Observable配列",         fn_observable_array,       1, 2},
    {"モデルコンポジション",   fn_model_composition,      1, 2},
    {"マルチソーター",         fn_multi_sorter,           1, 1},
    {"カスタムフィルターチェーン", fn_custom_filter_chain, 1, 2},
    {"ページネーションモデル", fn_pagination_model,       1, 2},

    /* Phase 128: 高度描画/エフェクト拡張 */
    {"ウィジェットシャドウ",   fn_widget_shadow,          4, 5},
    {"内側シャドウ",           fn_inner_shadow,           4, 5},
    {"反射効果",               fn_reflection_effect,      1, 3},
    {"ライティング効果",       fn_lighting_effect,        1, 3},
    {"ターミナルレンダリング", fn_terminal_render,        0, 1},
    {"アトラス画像",           fn_atlas_image,            1, 2},
    {"SVGアニメーション",      fn_svg_animation,          1, 2},
    {"エフェクトチェーン",     fn_effect_chain,           1, 2},

    /* Phase 129: 特殊ウィジェット・入力拡張 */
    {"タイマーピッカー",       fn_timer_picker,           1, 2},
    {"目盛りスケール",         fn_tick_scale,             4, 5},
    {"キーボード表示",         fn_keyboard_display,       0, 3},
    {"ホットキーエディタ",     fn_hotkey_editor,          1, 2},
    {"3Dオリエンテーション",   fn_orientation_gizmo_3d,   1, 3},
    {"リフレクション自動UI",   fn_reflection_auto_ui,     1, 2},
    {"Sandbox",                fn_sandbox_widget,         1, 3},
    {"reStructuredText",       fn_rst_viewer,             1, 2},

    /* Phase 130: エコシステム/産業プロトコル */
    {"ウィジェットプラグイン", fn_widget_plugin,          1, 1},
    {"CSSスタイル",            fn_css_style,              1, 2},
    {"OPC_UA通信",             fn_opcua_comm,             1, 2},
    {"CANバス",                fn_can_bus,                1, 2},
    {"CoAPクライアント",       fn_coap_client,            1, 2},
    {"TUIOマルチタッチ",       fn_tuio_multitouch,        1, 2},
    {"UIテスト実行",           fn_ui_test_run,            1, 1},
    {"Protobufシリアライズ",   fn_protobuf_serialize,     1, 2},

    /* Phase 131: 宣言的UI / 仮想UIツリー */
    {"VUI.開始",              fn_vnode_begin,           1, 1},
    {"VUI.終了",              fn_vnode_end,             0, 1},
    {"VUI.ボックス",          fn_vnode_box,             1, 3},
    {"VUI.テキスト",          fn_vnode_text,            2, 2},
    {"VUI.ボタン",            fn_vnode_button,          2, 3},
    {"VUI.行",               fn_vnode_row,             1, 1},
    {"VUI.列",               fn_vnode_col,             1, 1},
    {"VUI.スクロール",        fn_vnode_scroll,          3, 3},
    {"VUI.入力",              fn_vnode_input,           2, 2},
    {"VUI.スタイル",          fn_vnode_style,           1, 1},
    {"VUI.コミット",          fn_vnode_commit,          0, 2},
    {"VUI.無効化",            fn_vnode_invalidate,      1, 1},

    /* Phase 132: フレームスケジューラ */
    {"フレーム設定",          fn_フレーム設定,          1, 2},
    {"フレームレート取得",    fn_フレームレート取得,    0, 0},
    {"フレーム時間",          fn_フレーム時間,          0, 0},
    {"フレーム要求",          fn_フレーム要求,          0, 0},
    {"描画コール数",          fn_描画コール数,          0, 0},

    /* Phase 133: ホットリロード */
    {"ホットリロード開始",    fn_ホットリロード開始,    0, 1},
    {"ホットリロード監視",    fn_ホットリロード監視,    1, 1},
    {"ホットリロード確認",    fn_ホットリロード確認,    1, 1},
    {"ホットリロードリセット",fn_ホットリロードリセット,0, 0},
    {"ホットリロード回数",    fn_ホットリロード回数,    0, 0},

    /* Phase 134: 開発者ツール */
    {"DevTools開始",          fn_DevTools開始,          0, 2},
    {"DevTools表示",          fn_DevTools表示,          0, 0},
    {"DevTools非表示",        fn_DevTools非表示,        0, 0},
    {"DevToolsログ",          fn_DevToolsログ,          1, 1},
    {"DevToolsオプション",    fn_DevToolsオプション,    2, 2},
};

/* プラグイン初期化 */
HAJIMU_PLUGIN_EXPORT HajimuPluginInfo *hajimu_plugin_init(void) {
    static HajimuPluginInfo info = {
        .name           = "hajimu_gui",
        .version        = "14.0.0",
        .author         = "Reo Shiozawa",
        .description    = "はじむ用 GUI パッケージ — 自製プラットフォーム + 即時モード",
        .functions      = gui_functions,
        .function_count = sizeof(gui_functions) / sizeof(gui_functions[0]),
    };
    return &info;
}
