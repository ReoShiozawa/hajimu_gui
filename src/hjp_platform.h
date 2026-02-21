/***********************************************************************
 * hjp_platform.h — はじむGUI プラットフォーム抽象レイヤー
 *
 * SDL2 を完全に置き換える自作プラットフォーム層。
 * macOS: Cocoa + CoreText + ImageIO
 * Linux: X11 + GLX + FreeType (予定)
 * Windows: Win32 + WGL + GDI (予定)
 *
 * Copyright (c) 2026 Reo Shiozawa — MIT License
 ***********************************************************************/
#ifndef HJP_PLATFORM_H
#define HJP_PLATFORM_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =====================================================================
 * 型定義
 * ===================================================================*/

typedef uint32_t HjpTicks;

/* ウィンドウハンドル (不透明) */
typedef struct HjpWindow HjpWindow;

/* GLコンテキストハンドル (不透明) */
typedef void *HjpGLContext;

/* カーソルハンドル */
typedef void *HjpCursor;

/* キーコード (ASCII互換 + 特殊キー) */
typedef int32_t HjpKeycode;

/* キー修飾子フラグ */
typedef uint32_t HjpKeymod;

/* =====================================================================
 * 定数
 * ===================================================================*/

/* --- ウィンドウフラグ --- */
#define HJP_WINDOW_OPENGL       0x0001
#define HJP_WINDOW_RESIZABLE    0x0002
#define HJP_WINDOW_HIGHDPI      0x0004
#define HJP_WINDOW_SHOWN        0x0008
#define HJP_WINDOW_FULLSCREEN   0x0010
#define HJP_WINDOW_BORDERLESS   0x0020

/* --- イベントタイプ --- */
enum HjpEventType {
    HJP_EVENT_NONE = 0,
    HJP_EVENT_QUIT,
    HJP_EVENT_MOUSEMOTION,
    HJP_EVENT_MOUSEBUTTONDOWN,
    HJP_EVENT_MOUSEBUTTONUP,
    HJP_EVENT_MOUSEWHEEL,
    HJP_EVENT_WINDOWEVENT,
    HJP_EVENT_TEXTINPUT,
    HJP_EVENT_KEYDOWN,
    HJP_EVENT_KEYUP,
    HJP_EVENT_DROPFILE,
};

/* --- ウィンドウイベントサブタイプ --- */
enum HjpWindowEvent {
    HJP_WINDOWEVENT_NONE = 0,
    HJP_WINDOWEVENT_SIZE_CHANGED,
    HJP_WINDOWEVENT_CLOSE,
    HJP_WINDOWEVENT_FOCUS_GAINED,
    HJP_WINDOWEVENT_FOCUS_LOST,
};

/* --- マウスボタン --- */
enum HjpMouseButton {
    HJP_BUTTON_LEFT   = 1,
    HJP_BUTTON_MIDDLE = 2,
    HJP_BUTTON_RIGHT  = 3,
};

/* --- 特殊キーコード --- */
enum HjpKeycodes {
    HJPK_UNKNOWN    = 0,
    HJPK_RETURN     = '\r',
    HJPK_ESCAPE     = 27,
    HJPK_BACKSPACE  = '\b',
    HJPK_TAB        = '\t',
    HJPK_DELETE     = 127,
    HJPK_RIGHT      = 0x4000004F,
    HJPK_LEFT       = 0x40000050,
    HJPK_DOWN       = 0x40000051,
    HJPK_UP         = 0x40000052,
    HJPK_HOME       = 0x4000004A,
    HJPK_END        = 0x4000004D,
    HJPK_KP_ENTER   = 0x40000058,
    HJPK_a = 'a', HJPK_c = 'c', HJPK_v = 'v', HJPK_x = 'x',
};

/* --- キー修飾子ビット --- */
#define HJP_KMOD_NONE   0x0000
#define HJP_KMOD_SHIFT  0x0001
#define HJP_KMOD_CTRL   0x0002
#define HJP_KMOD_ALT    0x0004
#define HJP_KMOD_GUI    0x0008  /* Cmd on macOS */

/* --- スキャンコード (キーボード状態用) --- */
enum HjpScancode {
    HJP_SCANCODE_RETURN = 40,
    HJP_SCANCODE_ESCAPE = 41,
    HJP_SCANCODE_BACKSPACE = 42,
    HJP_SCANCODE_TAB = 43,
    HJP_SCANCODE_UP = 82,
    HJP_SCANCODE_DOWN = 81,
    HJP_SCANCODE_LEFT = 80,
    HJP_SCANCODE_RIGHT = 79,
};

/* --- システムカーソル --- */
enum HjpSystemCursor {
    HJP_CURSOR_ARROW = 0,
    HJP_CURSOR_HAND,
    HJP_CURSOR_IBEAM,
    HJP_CURSOR_CROSSHAIR,
    HJP_CURSOR_SIZEALL,
};

/* =====================================================================
 * イベント構造体
 * ===================================================================*/

typedef struct {
    int x, y;
} HjpMouseMotionEvent;

typedef struct {
    int button;
    int x, y;
} HjpMouseButtonEvent;

typedef struct {
    int x, y;    /* 水平, 垂直 */
} HjpMouseWheelEvent;

typedef struct {
    int event;   /* HjpWindowEvent */
    int data1, data2;
} HjpWindowEventData;

typedef struct {
    char text[128];
} HjpTextInputEvent;

typedef struct {
    HjpKeycode  sym;
    HjpKeymod   mod;
} HjpKeyEvent;

typedef struct {
    char *file;   /* hjp_free() で解放 */
} HjpDropEvent;

typedef struct {
    int type;     /* HjpEventType */
    union {
        HjpMouseMotionEvent  motion;
        HjpMouseButtonEvent  button;
        HjpMouseWheelEvent   wheel;
        HjpWindowEventData   window;
        HjpTextInputEvent    text;
        HjpKeyEvent          key;
        HjpDropEvent         drop;
    };
} HjpEvent;

/* =====================================================================
 * ディスプレイ情報
 * ===================================================================*/
typedef struct {
    int w, h;
    int refresh_rate;
} HjpDisplayMode;

/* =====================================================================
 * API 関数
 * ===================================================================*/

/* --- 初期化/終了 --- */
bool hjp_init(void);
void hjp_quit(void);

/* --- ウィンドウ --- */
HjpWindow *hjp_window_create(const char *title, int x, int y, int w, int h, uint32_t flags);
void       hjp_window_destroy(HjpWindow *win);
void       hjp_window_set_title(HjpWindow *win, const char *title);
void       hjp_window_set_size(HjpWindow *win, int w, int h);
void       hjp_window_get_size(HjpWindow *win, int *w, int *h);
void       hjp_window_set_position(HjpWindow *win, int x, int y);
void       hjp_window_maximize(HjpWindow *win);
void       hjp_window_minimize(HjpWindow *win);
void       hjp_window_set_fullscreen(HjpWindow *win, bool on);
void       hjp_window_set_opacity(HjpWindow *win, float op);
void       hjp_window_set_bordered(HjpWindow *win, bool on);
void       hjp_window_set_always_on_top(HjpWindow *win, bool on);
int        hjp_window_get_id(HjpWindow *win);

/* --- OpenGLコンテキスト --- */
HjpGLContext hjp_gl_create_context(HjpWindow *win);
void         hjp_gl_delete_context(HjpGLContext ctx);
void         hjp_gl_make_current(HjpWindow *win, HjpGLContext ctx);
void         hjp_gl_set_swap_interval(int interval);
void         hjp_gl_swap_window(HjpWindow *win);
void         hjp_gl_get_drawable_size(HjpWindow *win, int *w, int *h);

/* --- イベント --- */
bool hjp_poll_event(HjpEvent *event);

/* --- タイマー --- */
HjpTicks hjp_get_ticks(void);
void     hjp_delay(uint32_t ms);

/* --- クリップボード --- */
char *hjp_get_clipboard_text(void);
void  hjp_set_clipboard_text(const char *text);

/* --- テキスト入力 --- */
void hjp_start_text_input(void);
void hjp_stop_text_input(void);

/* --- キーボード状態 --- */
const uint8_t *hjp_get_keyboard_state(int *numkeys);
uint32_t hjp_get_mouse_state(int *x, int *y);
#define HJP_BUTTON(n) (1 << ((n)-1))

/* --- カーソル --- */
HjpCursor hjp_create_system_cursor(int id);
void      hjp_set_cursor(HjpCursor cursor);
void      hjp_free_cursor(HjpCursor cursor);

/* --- ディスプレイ --- */
int  hjp_get_num_displays(void);
void hjp_get_current_display_mode(int display_index, HjpDisplayMode *mode);
void hjp_get_display_dpi(int display_index, float *ddpi, float *hdpi, float *vdpi);

/* --- メモリ --- */
void hjp_free(void *ptr);

/* =====================================================================
 * フォントレンダリング (CoreText/FreeType 抽象化)
 * nanovg互換レンダラ (hjp_render.c) から呼ばれる
 * ===================================================================*/

/* フォントハンドル */
typedef void *HjpFont;

HjpFont hjp_font_create_from_file(const char *path);
HjpFont hjp_font_create_from_mem(const unsigned char *data, int ndata);
void    hjp_font_destroy(HjpFont font);

/* グリフビットマップ取得 (アルファのみ, 呼び出し側がfreeする) */
int hjp_font_get_glyph(HjpFont font, float size, uint32_t codepoint,
                        unsigned char **bitmap, int *w, int *h,
                        int *xoff, int *yoff, float *advance);

/* フォントメトリクス */
void hjp_font_metrics(HjpFont font, float size,
                      float *ascent, float *descent, float *line_gap);

/* テキスト幅計測 */
float hjp_font_text_width(HjpFont font, float size, const char *str, const char *end);

/* =====================================================================
 * 画像読み込み (ImageIO/stb_image 抽象化)
 * ===================================================================*/

/* メモリ上の画像データをデコード → RGBA ピクセル */
unsigned char *hjp_image_load_mem(const unsigned char *data, int ndata,
                                  int *w, int *h);
void hjp_image_free(unsigned char *pixels);

/* =====================================================================
 * ウィンドウ位置定数
 * ===================================================================*/
#define HJP_WINDOWPOS_CENTERED  0x2FFF0000

#ifdef __cplusplus
}
#endif

#endif /* HJP_PLATFORM_H */
