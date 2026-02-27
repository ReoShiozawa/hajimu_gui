/***********************************************************************
 * hjp_platform_win32.c — はじむGUI Windows プラットフォーム実装
 *
 * Win32 API + WGL + GDI フォントレンダリング
 * Win32 (x86) / Win64 (x86_64 / ARM64) 対応
 *
 * コンパイル依存:
 *   -lopengl32 -lgdi32 -luser32 -lkernel32 -lshell32
 *
 * Copyright (c) 2026 Reo Shiozawa — MIT License
 ***********************************************************************/

/*
 * ビルドターゲット判定:
 *   _WIN32  — Windows 32bit / 64bit 共通で定義される (MSVC/GCC/Clang)
 *   _WIN64  — 64bit ビルド時のみ定義される
 *
 * このファイルは Win64 (x86_64 / ARM64) を主ターゲットとしつつ、
 * Win32 (x86) でも正しく動くよう設計する。
 *
 * ポイント:
 *   - GWL_STYLE 等の操作は常に GetWindowLongPtrW / SetWindowLongPtrW を使用
 *   - ポインタサイズの整数は LONG_PTR / UINT_PTR を使用
 *   - size_t を使ったメモリ計算で 32bit でも 64bit でも安全な確保
 */

/* _WIN32 は Win32/Win64 両方で定義される */
#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <windowsx.h>
#include <GL/gl.h>
#include <shellapi.h>

/* WGL拡張 */
#include "hjp_platform.h"

/* GL関数ローダー */
#define HJP_GL_LOADER
#define HJP_GL_LOADER_IMPL
#include "hjp_gl_funcs.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

/* MSVC: 自動リンク */
#ifdef _MSC_VER
#  pragma comment(lib, "opengl32.lib")
#  pragma comment(lib, "gdi32.lib")
#  pragma comment(lib, "user32.lib")
#  pragma comment(lib, "shell32.lib")
#  pragma comment(lib, "kernel32.lib")
#endif

/* 型サイズの静的アサート */
#ifdef _WIN64
  typedef char hjp__check_ptr64[(sizeof(void*) == 8) ? 1 : -1];
#endif

/* =====================================================================
 * WGL拡張定数
 * ===================================================================*/
#ifndef WGL_DRAW_TO_WINDOW_ARB
#define WGL_DRAW_TO_WINDOW_ARB        0x2001
#define WGL_SUPPORT_OPENGL_ARB        0x2010
#define WGL_DOUBLE_BUFFER_ARB         0x2011
#define WGL_PIXEL_TYPE_ARB            0x2013
#define WGL_TYPE_RGBA_ARB             0x202B
#define WGL_COLOR_BITS_ARB            0x2014
#define WGL_DEPTH_BITS_ARB            0x2022
#define WGL_STENCIL_BITS_ARB          0x2023
#define WGL_ACCELERATION_ARB          0x2003
#define WGL_FULL_ACCELERATION_ARB     0x2027
#define WGL_SAMPLE_BUFFERS_ARB        0x2041
#define WGL_SAMPLES_ARB               0x2042
#endif

#ifndef WGL_CONTEXT_MAJOR_VERSION_ARB
#define WGL_CONTEXT_MAJOR_VERSION_ARB 0x2091
#define WGL_CONTEXT_MINOR_VERSION_ARB 0x2092
#define WGL_CONTEXT_PROFILE_MASK_ARB  0x9126
#define WGL_CONTEXT_CORE_PROFILE_BIT_ARB 0x00000001
#endif

/* =====================================================================
 * 内部構造体
 * ===================================================================*/

#define HJP_MAX_EVENTS 256

struct HjpWindow {
    HWND    hwnd;
    HDC     hdc;
    HGLRC   hrc;
    int     width, height;
    int     fb_width, fb_height;
    int     id;
    bool    fullscreen;
    RECT     pre_fullscreen_rect;  /* フルスクリーン前のウィンドウ位置 */
    LONG_PTR pre_fullscreen_style;  /* Win64: LONG_PTR で GWL_STYLE を保持 */
};

static struct {
    /* イベントリングバッファ */
    HjpEvent    events[HJP_MAX_EVENTS];
    int         ev_head, ev_tail;

    /* キーボード/マウス状態 */
    uint8_t     keys[512];
    int         mouseX, mouseY;
    uint32_t    mouseButtons;

    /* テキスト入力 */
    bool        text_input_active;

    /* WM_CHAR サロゲートペア蓄積 (Win64でも WM_CHAR は2回来る) */
    WCHAR       surrogate_high;

    /* クリップボード */
    char       *clipboard_text;

    /* 時刻基準 */
    LARGE_INTEGER perf_freq;
    LARGE_INTEGER start_time;

    /* ウィンドウクラス登録済み */
    bool        wc_registered;
    WNDCLASSEXW wc;

    bool        inited;
    int         next_window_id;

    /* 現在のウィンドウ (イベントコールバック用) */
    struct HjpWindow *current_win;
} g_hjp;

/* =====================================================================
 * イベントキュー
 * ===================================================================*/
static void push_event(const HjpEvent *ev) {
    int next = (g_hjp.ev_head + 1) % HJP_MAX_EVENTS;
    if (next == g_hjp.ev_tail) return;
    g_hjp.events[g_hjp.ev_head] = *ev;
    g_hjp.ev_head = next;
}

static bool pop_event(HjpEvent *ev) {
    if (g_hjp.ev_tail == g_hjp.ev_head) return false;
    *ev = g_hjp.events[g_hjp.ev_tail];
    g_hjp.ev_tail = (g_hjp.ev_tail + 1) % HJP_MAX_EVENTS;
    return true;
}

/* =====================================================================
 * VK → HJP キーコード変換
 * ===================================================================*/
static HjpKeycode vk_to_hjp(WPARAM vk) {
    if (vk >= 'A' && vk <= 'Z') return (HjpKeycode)(vk + 32); /* lowercase */
    if (vk >= '0' && vk <= '9') return (HjpKeycode)vk;
    switch (vk) {
        case VK_RETURN:     return HJPK_RETURN;
        case VK_ESCAPE:     return HJPK_ESCAPE;
        case VK_BACK:       return HJPK_BACKSPACE;
        case VK_TAB:        return HJPK_TAB;
        case VK_DELETE:     return HJPK_DELETE;
        case VK_LEFT:       return HJPK_LEFT;
        case VK_RIGHT:      return HJPK_RIGHT;
        case VK_UP:         return HJPK_UP;
        case VK_DOWN:       return HJPK_DOWN;
        case VK_HOME:       return HJPK_HOME;
        case VK_END:        return HJPK_END;
        case VK_SPACE:      return ' ';
        default:            return (HjpKeycode)vk;
    }
}

static HjpKeymod get_modifiers(void) {
    HjpKeymod m = HJP_KMOD_NONE;
    if (GetKeyState(VK_SHIFT)   & 0x8000) m |= HJP_KMOD_SHIFT;
    if (GetKeyState(VK_CONTROL) & 0x8000) m |= HJP_KMOD_CTRL;
    if (GetKeyState(VK_MENU)    & 0x8000) m |= HJP_KMOD_ALT;
    if ((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000) m |= HJP_KMOD_GUI;
    return m;
}

/* =====================================================================
 * ウィンドウプロシージャ
 * ===================================================================*/
static LRESULT CALLBACK hjp_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    HjpEvent ev;
    memset(&ev, 0, sizeof(ev));

    switch (msg) {
        case WM_CLOSE:
            ev.type = HJP_EVENT_QUIT;
            push_event(&ev);
            return 0;

        case WM_SIZE:
            ev.type = HJP_EVENT_WINDOWEVENT;
            ev.window.event = HJP_WINDOWEVENT_SIZE_CHANGED;
            ev.window.data1 = LOWORD(lp);
            ev.window.data2 = HIWORD(lp);
            push_event(&ev);
            if (g_hjp.current_win) {
                g_hjp.current_win->width = LOWORD(lp);
                g_hjp.current_win->height = HIWORD(lp);
                g_hjp.current_win->fb_width = LOWORD(lp);
                g_hjp.current_win->fb_height = HIWORD(lp);
            }
            break;

        case WM_SETFOCUS:
            ev.type = HJP_EVENT_WINDOWEVENT;
            ev.window.event = HJP_WINDOWEVENT_FOCUS_GAINED;
            push_event(&ev);
            break;

        case WM_KILLFOCUS:
            ev.type = HJP_EVENT_WINDOWEVENT;
            ev.window.event = HJP_WINDOWEVENT_FOCUS_LOST;
            push_event(&ev);
            break;

        case WM_MOUSEMOVE:
            ev.type = HJP_EVENT_MOUSEMOTION;
            ev.motion.x = GET_X_LPARAM(lp);
            ev.motion.y = GET_Y_LPARAM(lp);
            g_hjp.mouseX = ev.motion.x;
            g_hjp.mouseY = ev.motion.y;
            push_event(&ev);
            break;

        case WM_LBUTTONDOWN:
            ev.type = HJP_EVENT_MOUSEBUTTONDOWN;
            ev.button.button = HJP_BUTTON_LEFT;
            ev.button.x = GET_X_LPARAM(lp);
            ev.button.y = GET_Y_LPARAM(lp);
            g_hjp.mouseButtons |= (1 << 0);
            push_event(&ev);
            SetCapture(hwnd);
            break;
        case WM_LBUTTONUP:
            ev.type = HJP_EVENT_MOUSEBUTTONUP;
            ev.button.button = HJP_BUTTON_LEFT;
            ev.button.x = GET_X_LPARAM(lp);
            ev.button.y = GET_Y_LPARAM(lp);
            g_hjp.mouseButtons &= ~(1 << 0);
            push_event(&ev);
            ReleaseCapture();
            break;

        case WM_RBUTTONDOWN:
            ev.type = HJP_EVENT_MOUSEBUTTONDOWN;
            ev.button.button = HJP_BUTTON_RIGHT;
            ev.button.x = GET_X_LPARAM(lp);
            ev.button.y = GET_Y_LPARAM(lp);
            g_hjp.mouseButtons |= (1 << 2);
            push_event(&ev);
            break;
        case WM_RBUTTONUP:
            ev.type = HJP_EVENT_MOUSEBUTTONUP;
            ev.button.button = HJP_BUTTON_RIGHT;
            ev.button.x = GET_X_LPARAM(lp);
            ev.button.y = GET_Y_LPARAM(lp);
            g_hjp.mouseButtons &= ~(1 << 2);
            push_event(&ev);
            break;

        case WM_MBUTTONDOWN:
            ev.type = HJP_EVENT_MOUSEBUTTONDOWN;
            ev.button.button = HJP_BUTTON_MIDDLE;
            ev.button.x = GET_X_LPARAM(lp);
            ev.button.y = GET_Y_LPARAM(lp);
            g_hjp.mouseButtons |= (1 << 1);
            push_event(&ev);
            break;
        case WM_MBUTTONUP:
            ev.type = HJP_EVENT_MOUSEBUTTONUP;
            ev.button.button = HJP_BUTTON_MIDDLE;
            ev.button.x = GET_X_LPARAM(lp);
            ev.button.y = GET_Y_LPARAM(lp);
            g_hjp.mouseButtons &= ~(1 << 1);
            push_event(&ev);
            break;

        case WM_MOUSEWHEEL:
            ev.type = HJP_EVENT_MOUSEWHEEL;
            ev.wheel.x = 0;
            ev.wheel.y = GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA;
            push_event(&ev);
            break;

        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            ev.type = HJP_EVENT_KEYDOWN;
            ev.key.sym = vk_to_hjp(wp);
            ev.key.mod = get_modifiers();
            push_event(&ev);
            if (wp < 512) g_hjp.keys[wp] = 1;
            break;

        case WM_KEYUP:
        case WM_SYSKEYUP:
            ev.type = HJP_EVENT_KEYUP;
            ev.key.sym = vk_to_hjp(wp);
            ev.key.mod = get_modifiers();
            push_event(&ev);
            if (wp < 512) g_hjp.keys[wp] = 0;
            break;

        case WM_CHAR: {
            /* WM_CHAR は常に UTF-16 コードユニット (WCHAR 1文字分) を届ける。
             * BMP外文字 (U+10000〜) はサロゲートペア 2回の WM_CHAR になる。
             * Win64 でも WPARAM は下位16bit に値を持つ。 */
            WCHAR ch = (WCHAR)(wp & 0xFFFF);

            /* 上位サロゲート (D800–DBFF) を蓄積 */
            if (ch >= 0xD800 && ch <= 0xDBFF) {
                g_hjp.surrogate_high = ch;
                return 0;
            }

            uint32_t codepoint;
            if (ch >= 0xDC00 && ch <= 0xDFFF && g_hjp.surrogate_high) {
                /* 下位サロゲート — ペアを結合して U+10000〜 を復元 */
                codepoint = 0x10000u +
                    ((uint32_t)(g_hjp.surrogate_high - 0xD800u) << 10) +
                    (uint32_t)(ch - 0xDC00u);
                g_hjp.surrogate_high = 0;
            } else {
                g_hjp.surrogate_high = 0;
                codepoint = (uint32_t)ch;
            }

            if (g_hjp.text_input_active && codepoint >= 32 && codepoint != 127) {
                ev.type = HJP_EVENT_TEXTINPUT;
                /* UTF-32 コードポイント → UTF-8 変換 */
                if (codepoint < 0x80u) {
                    ev.text.text[0] = (char)codepoint;
                } else if (codepoint < 0x800u) {
                    ev.text.text[0] = (char)(0xC0 | (codepoint >> 6));
                    ev.text.text[1] = (char)(0x80 | (codepoint & 0x3F));
                } else if (codepoint < 0x10000u) {
                    ev.text.text[0] = (char)(0xE0 | (codepoint >> 12));
                    ev.text.text[1] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
                    ev.text.text[2] = (char)(0x80 | (codepoint & 0x3F));
                } else {
                    /* BMP外: 4バイト UTF-8 (絵文字など) */
                    ev.text.text[0] = (char)(0xF0 | (codepoint >> 18));
                    ev.text.text[1] = (char)(0x80 | ((codepoint >> 12) & 0x3F));
                    ev.text.text[2] = (char)(0x80 | ((codepoint >>  6) & 0x3F));
                    ev.text.text[3] = (char)(0x80 | (codepoint & 0x3F));
                }
                push_event(&ev);
            }
            return 0;
        }

        case WM_DROPFILES: {
            /* Win64: HDROP は HANDLE (ポインタサイズ)。WPARAM から直接キャスト可 */
            HDROP hDrop = (HDROP)(UINT_PTR)wp;
            UINT count = DragQueryFileW(hDrop, 0xFFFFFFFF, NULL, 0);
            for (UINT i = 0; i < count; i++) {
                WCHAR wpath[MAX_PATH];
                DragQueryFileW(hDrop, i, wpath, MAX_PATH);
                /* UTF-16 → UTF-8 */
                int len = WideCharToMultiByte(CP_UTF8, 0, wpath, -1, NULL, 0, NULL, NULL);
                if (len <= 0) continue; /* 変換失敗はスキップ */
                char *path = (char *)malloc((size_t)len);
                if (!path) continue; /* OOM はスキップ */
                WideCharToMultiByte(CP_UTF8, 0, wpath, -1, path, len, NULL, NULL);
                ev.type = HJP_EVENT_DROPFILE;
                ev.drop.file = path;
                push_event(&ev);
            }
            DragFinish(hDrop);
            return 0;
        }

        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* =====================================================================
 * 初期化/終了
 * ===================================================================*/

bool hjp_init(void) {
    if (g_hjp.inited) return true;
    memset(&g_hjp, 0, sizeof(g_hjp));

    QueryPerformanceFrequency(&g_hjp.perf_freq);
    QueryPerformanceCounter(&g_hjp.start_time);

    /* ウィンドウクラス登録 */
    if (!g_hjp.wc_registered) {
        memset(&g_hjp.wc, 0, sizeof(g_hjp.wc));
        g_hjp.wc.cbSize = sizeof(WNDCLASSEXW);
        g_hjp.wc.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
        g_hjp.wc.lpfnWndProc = hjp_wnd_proc;
        g_hjp.wc.hInstance = GetModuleHandle(NULL);
        g_hjp.wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        g_hjp.wc.lpszClassName = L"HjpWindowClass";
        RegisterClassExW(&g_hjp.wc);
        g_hjp.wc_registered = true;
    }

    /* DPI awareness (Windows 10+) */
    {
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        if (user32) {
            typedef BOOL (WINAPI *SetProcessDPIAwarenessContextProc)(HANDLE);
            SetProcessDPIAwarenessContextProc fn =
                (SetProcessDPIAwarenessContextProc)GetProcAddress(user32, "SetProcessDpiAwarenessContext");
            if (fn) fn((HANDLE)-4); /* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 */
        }
    }

    g_hjp.inited = true;
    return true;
}

void hjp_quit(void) {
    if (!g_hjp.inited) return;
    free(g_hjp.clipboard_text);
    memset(&g_hjp, 0, sizeof(g_hjp));
}

/* =====================================================================
 * ウィンドウ
 * ===================================================================*/

HjpWindow *hjp_window_create(const char *title, int x, int y, int w, int h, uint32_t flags) {
    /* タイトル UTF-8→UTF-16 (title が NULL の場合も安全に処理) */
    WCHAR *wtitle = NULL;
    if (title) {
        int wlen = MultiByteToWideChar(CP_UTF8, 0, title, -1, NULL, 0);
        if (wlen > 0) {
            wtitle = (WCHAR *)malloc((size_t)wlen * sizeof(WCHAR));
            if (wtitle)
                MultiByteToWideChar(CP_UTF8, 0, title, -1, wtitle, wlen);
        }
    }

    DWORD style = WS_OVERLAPPEDWINDOW;
    if (flags & HJP_WINDOW_BORDERLESS) style = WS_POPUP;

    RECT rc = { 0, 0, w, h };
    AdjustWindowRect(&rc, style, FALSE);

    int cx = (x == (int)HJP_WINDOWPOS_CENTERED) ? CW_USEDEFAULT : x;
    int cy = (y == (int)HJP_WINDOWPOS_CENTERED) ? CW_USEDEFAULT : y;

    HWND hwnd = CreateWindowExW(
        WS_EX_ACCEPTFILES,
        L"HjpWindowClass", wtitle ? wtitle : L"",
        style,
        cx, cy,
        rc.right - rc.left, rc.bottom - rc.top,
        NULL, NULL, GetModuleHandle(NULL), NULL
    );
    free(wtitle);
    if (!hwnd) return NULL;

    HDC hdc = GetDC(hwnd);
    if (!hdc) { DestroyWindow(hwnd); return NULL; }

    /* PixelFormat */
    PIXELFORMATDESCRIPTOR pfd;
    memset(&pfd, 0, sizeof(pfd));
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;
    pfd.cStencilBits = 8;
    int pf = ChoosePixelFormat(hdc, &pfd);
    if (pf == 0) { ReleaseDC(hwnd, hdc); DestroyWindow(hwnd); return NULL; }
    if (!SetPixelFormat(hdc, pf, &pfd)) { ReleaseDC(hwnd, hdc); DestroyWindow(hwnd); return NULL; }

    /* WGLコンテキスト (まず1.0で作成、それを使って3.2 Coreに昇格) */
    HGLRC tmp = wglCreateContext(hdc);
    if (!tmp) { ReleaseDC(hwnd, hdc); DestroyWindow(hwnd); return NULL; }
    wglMakeCurrent(hdc, tmp);

    /* wglCreateContextAttribsARB を取得 */
    typedef HGLRC (WINAPI *wglCreateContextAttribsARBProc)(HDC, HGLRC, const int*);
    wglCreateContextAttribsARBProc wglCreateContextAttribsARB =
        (wglCreateContextAttribsARBProc)wglGetProcAddress("wglCreateContextAttribsARB");

    HGLRC hrc = NULL;
    if (wglCreateContextAttribsARB) {
        int attribs[] = {
            WGL_CONTEXT_MAJOR_VERSION_ARB, 3,
            WGL_CONTEXT_MINOR_VERSION_ARB, 2,
            WGL_CONTEXT_PROFILE_MASK_ARB,  WGL_CONTEXT_CORE_PROFILE_BIT_ARB,
            0
        };
        hrc = wglCreateContextAttribsARB(hdc, NULL, attribs);
    }
    if (!hrc) hrc = tmp;
    else { wglMakeCurrent(NULL, NULL); wglDeleteContext(tmp); }

    wglMakeCurrent(hdc, hrc);

    /* GL関数ポインタロード */
    hjp_gl_load_functions();

    if (flags & HJP_WINDOW_SHOWN)
        ShowWindow(hwnd, SW_SHOW);

    HjpWindow *win = (HjpWindow *)calloc(1, sizeof(HjpWindow));
    if (!win) {
        /* OOM: GL コンテキストとウィンドウを解放して NULL を返す */
        wglMakeCurrent(NULL, NULL);
        wglDeleteContext(hrc);
        ReleaseDC(hwnd, hdc);
        DestroyWindow(hwnd);
        return NULL;
    }
    win->hwnd = hwnd;
    win->hdc  = hdc;
    win->hrc  = hrc;
    win->width    = w;
    win->height   = h;
    win->fb_width  = w;
    win->fb_height = h;
    win->id = ++g_hjp.next_window_id;
    g_hjp.current_win = win;

    return win;
}

void hjp_window_destroy(HjpWindow *win) {
    if (!win) return;
    wglMakeCurrent(NULL, NULL);
    if (win->hrc) wglDeleteContext(win->hrc);
    if (win->hdc) ReleaseDC(win->hwnd, win->hdc);
    DestroyWindow(win->hwnd);
    if (g_hjp.current_win == win) g_hjp.current_win = NULL;
    free(win);
}

void hjp_window_set_title(HjpWindow *win, const char *title) {
    if (!win || !title) return;
    int wlen = MultiByteToWideChar(CP_UTF8, 0, title, -1, NULL, 0);
    if (wlen <= 0) { SetWindowTextW(win->hwnd, L""); return; }
    WCHAR *wt = (WCHAR *)malloc((size_t)wlen * sizeof(WCHAR));
    if (!wt) return;
    MultiByteToWideChar(CP_UTF8, 0, title, -1, wt, wlen);
    SetWindowTextW(win->hwnd, wt);
    free(wt);
}

void hjp_window_set_size(HjpWindow *win, int w, int h) {
    if (!win || w <= 0 || h <= 0) return;
    RECT rc = {0, 0, w, h};
    /* Win64互換: GetWindowLongPtrW を使用 */
    AdjustWindowRect(&rc, (DWORD)GetWindowLongPtrW(win->hwnd, GWL_STYLE), FALSE);
    SetWindowPos(win->hwnd, NULL, 0, 0, rc.right-rc.left, rc.bottom-rc.top,
                 SWP_NOMOVE | SWP_NOZORDER);
    win->width = w; win->height = h;
    win->fb_width = w; win->fb_height = h;
}

void hjp_window_get_size(HjpWindow *win, int *w, int *h) {
    if (!win) { if (w) *w = 0; if (h) *h = 0; return; }
    RECT rc;
    GetClientRect(win->hwnd, &rc);
    win->width = rc.right - rc.left;
    win->height = rc.bottom - rc.top;
    if (w) *w = win->width;
    if (h) *h = win->height;
}

void hjp_window_set_position(HjpWindow *win, int x, int y) {
    if (!win) return;
    SetWindowPos(win->hwnd, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
}

void hjp_window_maximize(HjpWindow *win) {
    if (!win) return;
    ShowWindow(win->hwnd, SW_MAXIMIZE);
}

void hjp_window_minimize(HjpWindow *win) {
    if (!win) return;
    ShowWindow(win->hwnd, SW_MINIMIZE);
}

void hjp_window_set_fullscreen(HjpWindow *win, bool on) {
    if (!win) return;
    if (on && !win->fullscreen) {
        /* Win64互換: GetWindowLongPtrW / SetWindowLongPtrW */
        win->pre_fullscreen_style = GetWindowLongPtrW(win->hwnd, GWL_STYLE);
        GetWindowRect(win->hwnd, &win->pre_fullscreen_rect);
        SetWindowLongPtrW(win->hwnd, GWL_STYLE,
                          win->pre_fullscreen_style & ~(LONG_PTR)WS_OVERLAPPEDWINDOW);
        MONITORINFO mi = { sizeof(mi) };
        GetMonitorInfo(MonitorFromWindow(win->hwnd, MONITOR_DEFAULTTOPRIMARY), &mi);
        SetWindowPos(win->hwnd, HWND_TOP,
                     mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        win->fullscreen = true;
    } else if (!on && win->fullscreen) {
        SetWindowLongPtrW(win->hwnd, GWL_STYLE, win->pre_fullscreen_style);
        SetWindowPos(win->hwnd, NULL,
                     win->pre_fullscreen_rect.left, win->pre_fullscreen_rect.top,
                     win->pre_fullscreen_rect.right - win->pre_fullscreen_rect.left,
                     win->pre_fullscreen_rect.bottom - win->pre_fullscreen_rect.top,
                     SWP_NOOWNERZORDER | SWP_FRAMECHANGED | SWP_NOZORDER);
        win->fullscreen = false;
    }
}

void hjp_window_set_opacity(HjpWindow *win, float op) {
    if (!win) return;
    /* op を [0.0, 1.0] にクランプしてからBYTEに変換 */
    if (op < 0.0f) op = 0.0f;
    if (op > 1.0f) op = 1.0f;
    /* Win64互換: GetWindowLongPtrW / SetWindowLongPtrW */
    SetWindowLongPtrW(win->hwnd, GWL_EXSTYLE,
                      GetWindowLongPtrW(win->hwnd, GWL_EXSTYLE) | WS_EX_LAYERED);
    SetLayeredWindowAttributes(win->hwnd, 0, (BYTE)(op * 255.0f), LWA_ALPHA);
}

void hjp_window_set_bordered(HjpWindow *win, bool on) {
    if (!win) return;
    /* Win64互換: LONG_PTR で取得・設定 */
    LONG_PTR style = GetWindowLongPtrW(win->hwnd, GWL_STYLE);
    if (on) style |= WS_CAPTION | WS_THICKFRAME;
    else    style &= ~(LONG_PTR)(WS_CAPTION | WS_THICKFRAME);
    SetWindowLongPtrW(win->hwnd, GWL_STYLE, style);
    SetWindowPos(win->hwnd, NULL, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
}

void hjp_window_set_always_on_top(HjpWindow *win, bool on) {
    if (!win) return;
    SetWindowPos(win->hwnd, on ? HWND_TOPMOST : HWND_NOTOPMOST,
                 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
}

int hjp_window_get_id(HjpWindow *win) {
    return win ? win->id : 0;
}

/* =====================================================================
 * OpenGL コンテキスト
 * ===================================================================*/

HjpGLContext hjp_gl_create_context(HjpWindow *win) {
    return win ? (HjpGLContext)win->hrc : NULL;
}

void hjp_gl_delete_context(HjpGLContext ctx) { (void)ctx; }

void hjp_gl_make_current(HjpWindow *win, HjpGLContext ctx) {
    if (!win) return;
    wglMakeCurrent(win->hdc, (HGLRC)ctx);
}

void hjp_gl_set_swap_interval(int interval) {
    typedef BOOL (WINAPI *wglSwapIntervalEXTProc)(int);
    static wglSwapIntervalEXTProc fn = NULL;
    static bool loaded = false;
    if (!loaded) {
        fn = (wglSwapIntervalEXTProc)wglGetProcAddress("wglSwapIntervalEXT");
        loaded = true;
    }
    if (fn) fn(interval);
}

void hjp_gl_swap_window(HjpWindow *win) {
    if (!win) return;
    SwapBuffers(win->hdc);
}

void hjp_gl_get_drawable_size(HjpWindow *win, int *w, int *h) {
    if (!win) { if (w) *w = 0; if (h) *h = 0; return; }
    RECT rc;
    GetClientRect(win->hwnd, &rc);
    if (w) *w = rc.right - rc.left;
    if (h) *h = rc.bottom - rc.top;
}

/* =====================================================================
 * イベント
 * ===================================================================*/

bool hjp_poll_event(HjpEvent *event) {
    MSG msg;
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return pop_event(event);
}

/* =====================================================================
 * タイマー
 * ===================================================================*/

HjpTicks hjp_get_ticks(void) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (HjpTicks)((now.QuadPart - g_hjp.start_time.QuadPart) * 1000 /
                       g_hjp.perf_freq.QuadPart);
}

void hjp_delay(uint32_t ms) {
    Sleep(ms);
}

/* =====================================================================
 * クリップボード
 * ===================================================================*/

char *hjp_get_clipboard_text(void) {
    if (!OpenClipboard(NULL)) return strdup("");
    HANDLE hData = GetClipboardData(CF_UNICODETEXT);
    if (!hData) { CloseClipboard(); return strdup(""); }
    WCHAR *wtext = (WCHAR *)GlobalLock(hData);
    if (!wtext) { CloseClipboard(); return strdup(""); }
    int len = WideCharToMultiByte(CP_UTF8, 0, wtext, -1, NULL, 0, NULL, NULL);
    char *text;
    if (len <= 0) {
        text = strdup(""); /* 変換失敗: 空文字列を返す */
    } else {
        text = (char *)malloc((size_t)len);
        if (text) {
            WideCharToMultiByte(CP_UTF8, 0, wtext, -1, text, len, NULL, NULL);
        } else {
            text = strdup(""); /* OOM フォールバック */
        }
    }
    GlobalUnlock(hData);
    CloseClipboard();
    return text;
}

void hjp_set_clipboard_text(const char *text) {
    if (!text) return;
    int wlen = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
    if (wlen <= 0) return;
    /* メモリ確保をOpenClipboard前に行い、失敗時はクリップボードを汚染しない */
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, (size_t)wlen * sizeof(WCHAR));
    if (!hMem) return;
    WCHAR *dst = (WCHAR *)GlobalLock(hMem);
    if (!dst) { GlobalFree(hMem); return; }
    MultiByteToWideChar(CP_UTF8, 0, text, -1, dst, wlen);
    GlobalUnlock(hMem);
    if (!OpenClipboard(NULL)) { GlobalFree(hMem); return; }
    EmptyClipboard();
    /* SetClipboardData成功時はhMemの所有権がOSに移る */
    if (!SetClipboardData(CF_UNICODETEXT, hMem)) {
        GlobalFree(hMem);
    }
    CloseClipboard();
}

/* =====================================================================
 * テキスト入力
 * ===================================================================*/

void hjp_start_text_input(void) { g_hjp.text_input_active = true; }
void hjp_stop_text_input(void)  { g_hjp.text_input_active = false; }

/* =====================================================================
 * キーボード/マウス状態
 * ===================================================================*/

const uint8_t *hjp_get_keyboard_state(int *numkeys) {
    if (numkeys) *numkeys = 512;
    return g_hjp.keys;
}

uint32_t hjp_get_mouse_state(int *x, int *y) {
    if (x) *x = g_hjp.mouseX;
    if (y) *y = g_hjp.mouseY;
    return g_hjp.mouseButtons;
}

/* =====================================================================
 * カーソル
 * ===================================================================*/

HjpCursor hjp_create_system_cursor(int id) {
    LPCTSTR cur;
    switch (id) {
        case HJP_CURSOR_HAND:      cur = IDC_HAND; break;
        case HJP_CURSOR_IBEAM:     cur = IDC_IBEAM; break;
        case HJP_CURSOR_CROSSHAIR: cur = IDC_CROSS; break;
        case HJP_CURSOR_SIZEALL:   cur = IDC_SIZEALL; break;
        default:                   cur = IDC_ARROW; break;
    }
    return (HjpCursor)LoadCursor(NULL, cur);
}

void hjp_set_cursor(HjpCursor cursor) {
    SetCursor((HCURSOR)cursor);
}

void hjp_free_cursor(HjpCursor cursor) {
    /* システムカーソルは DestroyCursor 不要 */
    (void)cursor;
}

/* =====================================================================
 * ディスプレイ情報
 * ===================================================================*/

int hjp_get_num_displays(void) {
    return GetSystemMetrics(SM_CMONITORS);
}

void hjp_get_current_display_mode(int display_index, HjpDisplayMode *mode) {
    if (!mode) return;
    (void)display_index;
    DEVMODEW dm;
    memset(&dm, 0, sizeof(dm));
    dm.dmSize = sizeof(dm);
    EnumDisplaySettingsW(NULL, ENUM_CURRENT_SETTINGS, &dm);
    mode->w = dm.dmPelsWidth;
    mode->h = dm.dmPelsHeight;
    mode->refresh_rate = dm.dmDisplayFrequency;
}

void hjp_get_display_dpi(int display_index, float *ddpi, float *hdpi, float *vdpi) {
    (void)display_index;
    HDC hdc = GetDC(NULL);
    float h = (float)GetDeviceCaps(hdc, LOGPIXELSX);
    float v = (float)GetDeviceCaps(hdc, LOGPIXELSY);
    ReleaseDC(NULL, hdc);
    if (ddpi) *ddpi = (h + v) / 2.0f;
    if (hdpi) *hdpi = h;
    if (vdpi) *vdpi = v;
}

/* =====================================================================
 * メモリ
 * ===================================================================*/
void hjp_free(void *ptr) { free(ptr); }

/* =====================================================================
 * フォント (GDI + Uniscribe ベース)
 * ===================================================================*/

typedef struct {
    HFONT   hfont;
    HDC     hdc;     /* メモリDC */
    HBITMAP hbmp;
    int     bmp_w, bmp_h;
    unsigned char *bits;
    float   pixel_size;
} HjpFontInternal;

HjpFont hjp_font_create_from_file(const char *path) {
    if (!path) return NULL;
    /* WindowsではAddFontResourceExでシステムに登録 */
    int wlen = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
    if (wlen <= 0) return NULL;
    WCHAR *wpath = (WCHAR *)malloc((size_t)wlen * sizeof(WCHAR));
    if (!wpath) return NULL;
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, wlen);
    int added = AddFontResourceExW(wpath, FR_PRIVATE, 0);
    free(wpath);
    if (added == 0) return NULL;

    HjpFontInternal *f = (HjpFontInternal *)calloc(1, sizeof(HjpFontInternal));
    if (!f) return NULL;
    f->hdc = CreateCompatibleDC(NULL);
    if (!f->hdc) { free(f); return NULL; }
    f->pixel_size = 16;

    /* デフォルトフォント名取得 (最初の登録フォントを使う) */
    f->hfont = CreateFontW(-(int)f->pixel_size, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                           CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, NULL);
    if (!f->hfont) { DeleteDC(f->hdc); free(f); return NULL; }
    SelectObject(f->hdc, f->hfont);
    return (HjpFont)f;
}

HjpFont hjp_font_create_from_mem(const unsigned char *data, int ndata) {
    /* メモリからフォント追加 */
    DWORD numFonts = 0;
    HANDLE hMem = AddFontMemResourceEx((void *)data, ndata, NULL, &numFonts);
    if (!hMem || numFonts == 0) return NULL;

    HjpFontInternal *f = (HjpFontInternal *)calloc(1, sizeof(HjpFontInternal));
    if (!f) return NULL;
    f->hdc = CreateCompatibleDC(NULL);
    if (!f->hdc) { free(f); return NULL; }
    f->pixel_size = 16;
    f->hfont = CreateFontW(-(int)f->pixel_size, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                           CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, NULL);
    if (!f->hfont) { DeleteDC(f->hdc); free(f); return NULL; }
    SelectObject(f->hdc, f->hfont);
    return (HjpFont)f;
}

void hjp_font_destroy(HjpFont font) {
    if (!font) return;
    HjpFontInternal *f = (HjpFontInternal *)font;
    if (f->hdc) {
        /* DC が保持している GDI オブジェクトを先にストックオブジェクトに
         * 差し替えてから DeleteObject する (select 中に Delete するとリーク) */
        SelectObject(f->hdc, GetStockObject(SYSTEM_FONT));
        SelectObject(f->hdc, GetStockObject(DEFAULT_GUI_FONT));
        if (f->hbmp)  { SelectObject(f->hdc, GetStockObject(NULL_BRUSH)); DeleteObject(f->hbmp); }
        if (f->hfont) { DeleteObject(f->hfont); }
        DeleteDC(f->hdc);
    }
    free(f);
}

static void ensure_font_size(HjpFontInternal *f, float size) {
    if (f->pixel_size != size) {
        HFONT newfont = CreateFontW(-(int)size, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                   DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                                   CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, NULL);
        if (!newfont) return; /* フォント作成失敗時は変更しない */
        HFONT old = f->hfont;
        f->hfont = newfont;
        f->pixel_size = size;
        SelectObject(f->hdc, f->hfont);
        if (old) DeleteObject(old);
    }
}

int hjp_font_get_glyph(HjpFont font, float size, uint32_t codepoint,
                        unsigned char **bitmap, int *w, int *h,
                        int *xoff, int *yoff, float *advance) {
    if (!font) return 0;
    HjpFontInternal *f = (HjpFontInternal *)font;
    ensure_font_size(f, size);

    /* codepoint → WCHAR */
    WCHAR wch[2] = {0};
    int wchLen = 1;
    if (codepoint < 0x10000) {
        wch[0] = (WCHAR)codepoint;
    } else {
        /* サロゲートペア */
        codepoint -= 0x10000;
        wch[0] = (WCHAR)(0xD800 + (codepoint >> 10));
        wch[1] = (WCHAR)(0xDC00 + (codepoint & 0x3FF));
        wchLen = 2;
    }

    /* テキスト幅をGetTextExtentPoint32Wで取得 */
    SIZE textSize;
    GetTextExtentPoint32W(f->hdc, wch, wchLen, &textSize);

    int gw = textSize.cx;
    int gh = textSize.cy;
    if (gw <= 0 || gh <= 0) { gw = 1; gh = 1; }

    /* DIBSection を作成してグリフを描画 */
    if (f->bmp_w < gw || f->bmp_h < gh) {
        /* 旧BMPを先にDCから切り離してからDeleteObject (GDI破損防止) */
        SelectObject(f->hdc, GetStockObject(BLACK_BRUSH));
        if (f->hbmp) { DeleteObject(f->hbmp); f->hbmp = NULL; }
        f->bits = NULL;
        BITMAPINFO bmi;
        memset(&bmi, 0, sizeof(bmi));
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = gw;
        bmi.bmiHeader.biHeight = -gh; /* top-down */
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        f->hbmp = CreateDIBSection(f->hdc, &bmi, DIB_RGB_COLORS, (void **)&f->bits, NULL, 0);
        if (!f->hbmp || !f->bits) return 0; /* DIBSection作成失敗 */
        SelectObject(f->hdc, f->hbmp);
        /* フォントを再選択 (SelectObject(BLACK_BRUSH) でフォントが外れる場合に備える) */
        SelectObject(f->hdc, f->hfont);
        f->bmp_w = gw;
        f->bmp_h = gh;
    }

    /* DIBSectionを黒でクリア (前フレームのゴースト防止) */
    memset(f->bits, 0, (size_t)f->bmp_w * (size_t)f->bmp_h * 4);

    /* 黒背景に白文字で描画 */
    RECT rc = {0, 0, gw, gh};
    SetBkColor(f->hdc, RGB(0, 0, 0));
    SetTextColor(f->hdc, RGB(255, 255, 255));
    ExtTextOutW(f->hdc, 0, 0, ETO_OPAQUE, &rc, wch, wchLen, NULL);
    GdiFlush();

    /* アルファマップ抽出 (整数オーバーフロー対策: size_t で計算) */
    if (bitmap) {
        size_t npx = (size_t)gw * (size_t)gh;
        *bitmap = (unsigned char *)malloc(npx);
        if (!*bitmap) return 0;
        for (int row = 0; row < gh; row++) {
            for (int col = 0; col < gw; col++) {
                /* BGRAの R チャンネルをアルファとして使用 */
                const unsigned char *px = f->bits + ((size_t)row * (size_t)gw + (size_t)col) * 4;
                (*bitmap)[(size_t)row * (size_t)gw + (size_t)col] = px[2]; /* R */
            }
        }
    }
    if (w) *w = gw;
    if (h) *h = gh;
    if (xoff) *xoff = 0;
    if (yoff) *yoff = 0;
    if (advance) *advance = (float)textSize.cx;

    return 1;
}

void hjp_font_metrics(HjpFont font, float size,
                      float *ascent, float *descent, float *line_gap) {
    if (!font) {
        if (ascent) *ascent = size * 0.8f;
        if (descent) *descent = size * 0.2f;
        if (line_gap) *line_gap = size * 0.1f;
        return;
    }
    HjpFontInternal *f = (HjpFontInternal *)font;
    ensure_font_size(f, size);
    TEXTMETRICW tm;
    GetTextMetricsW(f->hdc, &tm);
    if (ascent)   *ascent   = (float)tm.tmAscent;
    if (descent)  *descent  = (float)tm.tmDescent;
    if (line_gap) *line_gap = (float)tm.tmExternalLeading;
}

float hjp_font_text_width(HjpFont font, float size, const char *str, const char *end) {
    if (!font || !str) return 0;
    HjpFontInternal *f = (HjpFontInternal *)font;
    ensure_font_size(f, size);

    int len = end ? (int)(end - str) : (int)strlen(str);
    if (len <= 0) return 0;
    int wlen = MultiByteToWideChar(CP_UTF8, 0, str, len, NULL, 0);
    if (wlen <= 0) return 0;
    WCHAR *wstr = (WCHAR *)malloc(((size_t)wlen + 1) * sizeof(WCHAR));
    if (!wstr) return 0;
    MultiByteToWideChar(CP_UTF8, 0, str, len, wstr, wlen);
    wstr[wlen] = L'\0';

    SIZE textSize;
    GetTextExtentPoint32W(f->hdc, wstr, wlen, &textSize);
    free(wstr);
    return (float)textSize.cx;
}

/* =====================================================================
 * 画像読み込み (WIC — Windows Imaging Component)
 * ===================================================================*/

unsigned char *hjp_image_load_mem(const unsigned char *data, int ndata,
                                  int *w, int *h) {
    if (!data || ndata < 8) return NULL;

    /* フォールバック: 1x1 透明ピクセル
       完全なWIC実装は大規模なため、将来的に対応 */
    *w = 1; *h = 1;
    unsigned char *px = (unsigned char *)calloc(4, 1);
    return px;
}

void hjp_image_free(unsigned char *pixels) { free(pixels); }

#endif /* _WIN32 */
