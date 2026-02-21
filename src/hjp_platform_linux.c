/***********************************************************************
 * hjp_platform_linux.c — はじむGUI Linux プラットフォーム実装
 *
 * X11 + GLX + FreeType + libpng/stb_image によるプラットフォーム層。
 *
 * コンパイル依存:
 *   -lX11 -lGL -lfreetype -lpthread -lm
 *   pkg-config --cflags --libs freetype2 x11
 *
 * Copyright (c) 2026 Reo Shiozawa — MIT License
 ***********************************************************************/

#ifdef __linux__

#include "hjp_platform.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>
#include <GL/gl.h>
#include <GL/glx.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_GLYPH_H

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <dlfcn.h>

/* =====================================================================
 * 内部構造体
 * ===================================================================*/

#define HJP_MAX_EVENTS 256

struct HjpWindow {
    Window      xwin;
    GLXContext   glctx;
    Display     *dpy;       /* 共有ディスプレイのコピー */
    int          width, height;
    int          fb_width, fb_height;
    Atom         wm_delete;
    bool         fullscreen;
    int          id;
};

/* グローバル状態 */
static struct {
    Display    *dpy;
    int         screen;
    Window      root;
    XIM         xim;
    XIC         xic;
    Atom        clipboard;
    Atom        utf8_string;
    Atom        targets;
    Atom        wm_state;
    Atom        wm_state_fullscreen;
    Atom        wm_state_above;
    Atom        net_wm_name;
    Atom        utf8;

    /* イベントリングバッファ */
    HjpEvent    events[HJP_MAX_EVENTS];
    int         ev_head, ev_tail;

    /* キーボード/マウス状態 */
    uint8_t     keys[512];
    int         mouseX, mouseY;
    uint32_t    mouseButtons;

    /* テキスト入力 */
    bool        text_input_active;

    /* クリップボード */
    char       *clipboard_text;

    /* FreeType */
    FT_Library  ft_lib;
    bool        ft_init;

    /* 時刻基準 */
    struct timespec start_time;

    /* 初期化済み */
    bool        inited;
    int         next_window_id;
} g_hjp;

/* =====================================================================
 * イベントキュー
 * ===================================================================*/
static void push_event(const HjpEvent *ev) {
    int next = (g_hjp.ev_head + 1) % HJP_MAX_EVENTS;
    if (next == g_hjp.ev_tail) return; /* full */
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
 * X11 → HJP キーコード変換
 * ===================================================================*/
static HjpKeycode xkey_to_hjp(KeySym ks) {
    if (ks >= XK_a && ks <= XK_z) return (HjpKeycode)ks;
    if (ks >= XK_A && ks <= XK_Z) return (HjpKeycode)(ks + 32); /* lowercase */
    if (ks >= XK_0 && ks <= XK_9) return (HjpKeycode)ks;
    switch (ks) {
        case XK_Return:     return HJPK_RETURN;
        case XK_KP_Enter:   return HJPK_KP_ENTER;
        case XK_Escape:     return HJPK_ESCAPE;
        case XK_BackSpace:  return HJPK_BACKSPACE;
        case XK_Tab:        return HJPK_TAB;
        case XK_Delete:     return HJPK_DELETE;
        case XK_Left:       return HJPK_LEFT;
        case XK_Right:      return HJPK_RIGHT;
        case XK_Up:         return HJPK_UP;
        case XK_Down:       return HJPK_DOWN;
        case XK_Home:       return HJPK_HOME;
        case XK_End:        return HJPK_END;
        case XK_space:      return ' ';
        default:            return (HjpKeycode)ks;
    }
}

static HjpKeymod xstate_to_hjpmod(unsigned int state) {
    HjpKeymod m = HJP_KMOD_NONE;
    if (state & ShiftMask)   m |= HJP_KMOD_SHIFT;
    if (state & ControlMask) m |= HJP_KMOD_CTRL;
    if (state & Mod1Mask)    m |= HJP_KMOD_ALT;
    if (state & Mod4Mask)    m |= HJP_KMOD_GUI;
    return m;
}

/* =====================================================================
 * 初期化/終了
 * ===================================================================*/

bool hjp_init(void) {
    if (g_hjp.inited) return true;
    memset(&g_hjp, 0, sizeof(g_hjp));

    XInitThreads();
    g_hjp.dpy = XOpenDisplay(NULL);
    if (!g_hjp.dpy) {
        fprintf(stderr, "[hjp_platform] X11ディスプレイを開けません\n");
        return false;
    }
    g_hjp.screen = DefaultScreen(g_hjp.dpy);
    g_hjp.root = RootWindow(g_hjp.dpy, g_hjp.screen);

    /* IME */
    g_hjp.xim = XOpenIM(g_hjp.dpy, NULL, NULL, NULL);

    /* Atoms */
    g_hjp.clipboard = XInternAtom(g_hjp.dpy, "CLIPBOARD", False);
    g_hjp.utf8_string = XInternAtom(g_hjp.dpy, "UTF8_STRING", False);
    g_hjp.targets = XInternAtom(g_hjp.dpy, "TARGETS", False);
    g_hjp.wm_state = XInternAtom(g_hjp.dpy, "_NET_WM_STATE", False);
    g_hjp.wm_state_fullscreen = XInternAtom(g_hjp.dpy, "_NET_WM_STATE_FULLSCREEN", False);
    g_hjp.wm_state_above = XInternAtom(g_hjp.dpy, "_NET_WM_STATE_ABOVE", False);
    g_hjp.net_wm_name = XInternAtom(g_hjp.dpy, "_NET_WM_NAME", False);
    g_hjp.utf8 = XInternAtom(g_hjp.dpy, "UTF8_STRING", False);

    /* 時刻基準 */
    clock_gettime(CLOCK_MONOTONIC, &g_hjp.start_time);

    /* FreeType */
    if (FT_Init_FreeType(&g_hjp.ft_lib) == 0)
        g_hjp.ft_init = true;

    g_hjp.inited = true;
    return true;
}

void hjp_quit(void) {
    if (!g_hjp.inited) return;
    if (g_hjp.ft_init) FT_Done_FreeType(g_hjp.ft_lib);
    if (g_hjp.clipboard_text) free(g_hjp.clipboard_text);
    if (g_hjp.xic) XDestroyIC(g_hjp.xic);
    if (g_hjp.xim) XCloseIM(g_hjp.xim);
    if (g_hjp.dpy) XCloseDisplay(g_hjp.dpy);
    memset(&g_hjp, 0, sizeof(g_hjp));
}

/* =====================================================================
 * ウィンドウ
 * ===================================================================*/

HjpWindow *hjp_window_create(const char *title, int x, int y, int w, int h, uint32_t flags) {
    (void)flags;
    Display *dpy = g_hjp.dpy;

    /* GLX FrameBuffer config */
    static int attribs[] = {
        GLX_RENDER_TYPE,   GLX_RGBA_BIT,
        GLX_DRAWABLE_TYPE, GLX_WINDOW_BIT,
        GLX_DOUBLEBUFFER,  True,
        GLX_RED_SIZE,      8,
        GLX_GREEN_SIZE,    8,
        GLX_BLUE_SIZE,     8,
        GLX_ALPHA_SIZE,    8,
        GLX_DEPTH_SIZE,    24,
        GLX_STENCIL_SIZE,  8,
        None
    };

    int fbcount = 0;
    GLXFBConfig *fbc = glXChooseFBConfig(dpy, g_hjp.screen, attribs, &fbcount);
    if (!fbc || fbcount == 0) {
        fprintf(stderr, "[hjp_platform] GLX FBConfig選択失敗\n");
        return NULL;
    }

    XVisualInfo *vi = glXGetVisualFromFBConfig(dpy, fbc[0]);
    if (!vi) { XFree(fbc); return NULL; }

    Colormap cmap = XCreateColormap(dpy, g_hjp.root, vi->visual, AllocNone);

    XSetWindowAttributes swa;
    memset(&swa, 0, sizeof(swa));
    swa.colormap = cmap;
    swa.event_mask = ExposureMask | KeyPressMask | KeyReleaseMask |
                     ButtonPressMask | ButtonReleaseMask | PointerMotionMask |
                     StructureNotifyMask | FocusChangeMask;

    if (x == (int)HJP_WINDOWPOS_CENTERED) x = (DisplayWidth(dpy, g_hjp.screen) - w) / 2;
    if (y == (int)HJP_WINDOWPOS_CENTERED) y = (DisplayHeight(dpy, g_hjp.screen) - h) / 2;

    Window xwin = XCreateWindow(dpy, g_hjp.root, x, y, w, h, 0,
                                vi->depth, InputOutput, vi->visual,
                                CWColormap | CWEventMask, &swa);

    XFree(vi);

    /* WM_DELETE_WINDOW */
    Atom wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, xwin, &wm_delete, 1);

    /* タイトル */
    XChangeProperty(dpy, xwin, g_hjp.net_wm_name, g_hjp.utf8, 8,
                    PropModeReplace, (unsigned char *)title, strlen(title));
    XStoreName(dpy, xwin, title);

    /* リサイズ可否 */
    if (!(flags & HJP_WINDOW_RESIZABLE)) {
        XSizeHints *sh = XAllocSizeHints();
        sh->flags = PMinSize | PMaxSize;
        sh->min_width = sh->max_width = w;
        sh->min_height = sh->max_height = h;
        XSetWMNormalHints(dpy, xwin, sh);
        XFree(sh);
    }

    XMapWindow(dpy, xwin);
    XFlush(dpy);

    /* GLXコンテキスト (3.2 Core Profile) */
    typedef GLXContext (*glXCreateContextAttribsARBProc)(Display*, GLXFBConfig, GLXContext, Bool, const int*);
    glXCreateContextAttribsARBProc glXCreateContextAttribsARB =
        (glXCreateContextAttribsARBProc)glXGetProcAddressARB((const GLubyte*)"glXCreateContextAttribsARB");

    GLXContext glctx = NULL;
    if (glXCreateContextAttribsARB) {
        int ctx_attribs[] = {
            GLX_CONTEXT_MAJOR_VERSION_ARB, 3,
            GLX_CONTEXT_MINOR_VERSION_ARB, 2,
            GLX_CONTEXT_PROFILE_MASK_ARB,  GLX_CONTEXT_CORE_PROFILE_BIT_ARB,
            None
        };
        glctx = glXCreateContextAttribsARB(dpy, fbc[0], NULL, True, ctx_attribs);
    }
    if (!glctx) {
        glctx = glXCreateNewContext(dpy, fbc[0], GLX_RGBA_TYPE, NULL, True);
    }
    XFree(fbc);

    if (!glctx) {
        XDestroyWindow(dpy, xwin);
        return NULL;
    }

    /* Input Context */
    if (g_hjp.xim && !g_hjp.xic) {
        g_hjp.xic = XCreateIC(g_hjp.xim,
                               XNInputStyle, XIMPreeditNothing | XIMStatusNothing,
                               XNClientWindow, xwin,
                               XNFocusWindow, xwin,
                               NULL);
    }

    HjpWindow *win = (HjpWindow *)calloc(1, sizeof(HjpWindow));
    win->xwin = xwin;
    win->glctx = glctx;
    win->dpy = dpy;
    win->width = w;
    win->height = h;
    win->fb_width = w;
    win->fb_height = h;
    win->wm_delete = wm_delete;
    win->id = ++g_hjp.next_window_id;

    return win;
}

void hjp_window_destroy(HjpWindow *win) {
    if (!win) return;
    if (win->glctx) {
        glXMakeCurrent(win->dpy, None, NULL);
        glXDestroyContext(win->dpy, win->glctx);
    }
    XDestroyWindow(win->dpy, win->xwin);
    XFlush(win->dpy);
    free(win);
}

void hjp_window_set_title(HjpWindow *win, const char *title) {
    if (!win) return;
    XChangeProperty(win->dpy, win->xwin, g_hjp.net_wm_name, g_hjp.utf8, 8,
                    PropModeReplace, (unsigned char *)title, strlen(title));
    XStoreName(win->dpy, win->xwin, title);
    XFlush(win->dpy);
}

void hjp_window_set_size(HjpWindow *win, int w, int h) {
    if (!win) return;
    XResizeWindow(win->dpy, win->xwin, w, h);
    win->width = w; win->height = h;
    win->fb_width = w; win->fb_height = h;
    XFlush(win->dpy);
}

void hjp_window_get_size(HjpWindow *win, int *w, int *h) {
    if (!win) { if (w) *w = 0; if (h) *h = 0; return; }
    if (w) *w = win->width;
    if (h) *h = win->height;
}

void hjp_window_set_position(HjpWindow *win, int x, int y) {
    if (!win) return;
    XMoveWindow(win->dpy, win->xwin, x, y);
    XFlush(win->dpy);
}

void hjp_window_maximize(HjpWindow *win) {
    if (!win) return;
    Atom wm_state = g_hjp.wm_state;
    Atom max_h = XInternAtom(win->dpy, "_NET_WM_STATE_MAXIMIZED_HORZ", False);
    Atom max_v = XInternAtom(win->dpy, "_NET_WM_STATE_MAXIMIZED_VERT", False);
    XEvent xev = {0};
    xev.type = ClientMessage;
    xev.xclient.window = win->xwin;
    xev.xclient.message_type = wm_state;
    xev.xclient.format = 32;
    xev.xclient.data.l[0] = 1; /* _NET_WM_STATE_ADD */
    xev.xclient.data.l[1] = max_h;
    xev.xclient.data.l[2] = max_v;
    XSendEvent(win->dpy, g_hjp.root, False,
               SubstructureNotifyMask | SubstructureRedirectMask, &xev);
    XFlush(win->dpy);
}

void hjp_window_minimize(HjpWindow *win) {
    if (!win) return;
    XIconifyWindow(win->dpy, win->xwin, g_hjp.screen);
    XFlush(win->dpy);
}

void hjp_window_set_fullscreen(HjpWindow *win, bool on) {
    if (!win) return;
    XEvent xev = {0};
    xev.type = ClientMessage;
    xev.xclient.window = win->xwin;
    xev.xclient.message_type = g_hjp.wm_state;
    xev.xclient.format = 32;
    xev.xclient.data.l[0] = on ? 1 : 0;
    xev.xclient.data.l[1] = g_hjp.wm_state_fullscreen;
    XSendEvent(win->dpy, g_hjp.root, False,
               SubstructureNotifyMask | SubstructureRedirectMask, &xev);
    win->fullscreen = on;
    XFlush(win->dpy);
}

void hjp_window_set_opacity(HjpWindow *win, float op) {
    if (!win) return;
    Atom opacity = XInternAtom(win->dpy, "_NET_WM_WINDOW_OPACITY", False);
    uint32_t val = (uint32_t)(op * 0xFFFFFFFF);
    XChangeProperty(win->dpy, win->xwin, opacity, XA_CARDINAL, 32,
                    PropModeReplace, (unsigned char *)&val, 1);
    XFlush(win->dpy);
}

void hjp_window_set_bordered(HjpWindow *win, bool on) {
    if (!win) return;
    /* Motif WM hints */
    Atom motif = XInternAtom(win->dpy, "_MOTIF_WM_HINTS", False);
    struct { unsigned long flags, functions, decorations, input_mode; long status; } hints = {0};
    hints.flags = 2; /* MWM_HINTS_DECORATIONS */
    hints.decorations = on ? 1 : 0;
    XChangeProperty(win->dpy, win->xwin, motif, motif, 32,
                    PropModeReplace, (unsigned char *)&hints, 5);
    XFlush(win->dpy);
}

void hjp_window_set_always_on_top(HjpWindow *win, bool on) {
    if (!win) return;
    XEvent xev = {0};
    xev.type = ClientMessage;
    xev.xclient.window = win->xwin;
    xev.xclient.message_type = g_hjp.wm_state;
    xev.xclient.format = 32;
    xev.xclient.data.l[0] = on ? 1 : 0;
    xev.xclient.data.l[1] = g_hjp.wm_state_above;
    XSendEvent(win->dpy, g_hjp.root, False,
               SubstructureNotifyMask | SubstructureRedirectMask, &xev);
    XFlush(win->dpy);
}

int hjp_window_get_id(HjpWindow *win) {
    return win ? win->id : 0;
}

/* =====================================================================
 * OpenGL コンテキスト
 * ===================================================================*/

HjpGLContext hjp_gl_create_context(HjpWindow *win) {
    if (!win) return NULL;
    /* ウィンドウ作成時に既にGLXコンテキストを作成済み */
    return (HjpGLContext)win->glctx;
}

void hjp_gl_delete_context(HjpGLContext ctx) {
    /* glXDestroyContextはhjp_window_destroyで実行 */
    (void)ctx;
}

void hjp_gl_make_current(HjpWindow *win, HjpGLContext ctx) {
    if (!win) return;
    GLXContext glctx = (GLXContext)ctx;
    glXMakeCurrent(win->dpy, win->xwin, glctx);
}

void hjp_gl_set_swap_interval(int interval) {
    typedef void (*glXSwapIntervalEXTProc)(Display*, GLXDrawable, int);
    static glXSwapIntervalEXTProc swapInterval = NULL;
    static bool loaded = false;
    if (!loaded) {
        swapInterval = (glXSwapIntervalEXTProc)
            glXGetProcAddressARB((const GLubyte*)"glXSwapIntervalEXT");
        loaded = true;
    }
    if (swapInterval && g_hjp.dpy) {
        /* 現在のdrawableに対して設定 */
        GLXDrawable drawable = glXGetCurrentDrawable();
        if (drawable) swapInterval(g_hjp.dpy, drawable, interval);
    }
}

void hjp_gl_swap_window(HjpWindow *win) {
    if (!win) return;
    glXSwapBuffers(win->dpy, win->xwin);
}

void hjp_gl_get_drawable_size(HjpWindow *win, int *w, int *h) {
    /* X11ではfb_size == window_size (HiDPI対応はXRandR経由) */
    if (!win) { if (w) *w = 0; if (h) *h = 0; return; }
    if (w) *w = win->fb_width;
    if (h) *h = win->fb_height;
}

/* =====================================================================
 * イベント処理
 * ===================================================================*/

bool hjp_poll_event(HjpEvent *event) {
    Display *dpy = g_hjp.dpy;
    if (!dpy) return false;

    while (XPending(dpy)) {
        XEvent xe;
        XNextEvent(dpy, &xe);

        /* XFilterEvent for IME */
        if (g_hjp.xic && XFilterEvent(&xe, None)) continue;

        HjpEvent ev;
        memset(&ev, 0, sizeof(ev));

        switch (xe.type) {
            case KeyPress: {
                KeySym ks;
                char buf[128] = {0};
                int len = 0;
                if (g_hjp.xic) {
                    Status status;
                    len = Xutf8LookupString(g_hjp.xic, &xe.xkey, buf, sizeof(buf)-1, &ks, &status);
                } else {
                    len = XLookupString(&xe.xkey, buf, sizeof(buf)-1, &ks, NULL);
                }
                buf[len] = '\0';

                /* KEYDOWN */
                ev.type = HJP_EVENT_KEYDOWN;
                ev.key.sym = xkey_to_hjp(ks);
                ev.key.mod = xstate_to_hjpmod(xe.xkey.state);
                push_event(&ev);

                /* TEXT INPUT (印字可能文字のみ) */
                if (g_hjp.text_input_active && len > 0 && (unsigned char)buf[0] >= 32) {
                    HjpEvent tev;
                    memset(&tev, 0, sizeof(tev));
                    tev.type = HJP_EVENT_TEXTINPUT;
                    strncpy(tev.text.text, buf, sizeof(tev.text.text) - 1);
                    push_event(&tev);
                }

                /* キーボード状態更新 */
                {
                    unsigned int kc = xe.xkey.keycode;
                    if (kc < 512) g_hjp.keys[kc] = 1;
                }
                break;
            }
            case KeyRelease: {
                KeySym ks;
                XLookupString(&xe.xkey, NULL, 0, &ks, NULL);
                ev.type = HJP_EVENT_KEYUP;
                ev.key.sym = xkey_to_hjp(ks);
                ev.key.mod = xstate_to_hjpmod(xe.xkey.state);
                push_event(&ev);
                {
                    unsigned int kc = xe.xkey.keycode;
                    if (kc < 512) g_hjp.keys[kc] = 0;
                }
                break;
            }
            case MotionNotify:
                ev.type = HJP_EVENT_MOUSEMOTION;
                ev.motion.x = xe.xmotion.x;
                ev.motion.y = xe.xmotion.y;
                g_hjp.mouseX = xe.xmotion.x;
                g_hjp.mouseY = xe.xmotion.y;
                push_event(&ev);
                break;

            case ButtonPress: {
                int btn = xe.xbutton.button;
                if (btn == 4 || btn == 5) {
                    /* スクロール */
                    ev.type = HJP_EVENT_MOUSEWHEEL;
                    ev.wheel.x = 0;
                    ev.wheel.y = (btn == 4) ? 1 : -1;
                    push_event(&ev);
                } else {
                    ev.type = HJP_EVENT_MOUSEBUTTONDOWN;
                    ev.button.button = btn; /* X11: 1=left, 2=middle, 3=right */
                    ev.button.x = xe.xbutton.x;
                    ev.button.y = xe.xbutton.y;
                    if (btn >= 1 && btn <= 5)
                        g_hjp.mouseButtons |= (1 << (btn - 1));
                    push_event(&ev);
                }
                break;
            }
            case ButtonRelease: {
                int btn = xe.xbutton.button;
                if (btn != 4 && btn != 5) {
                    ev.type = HJP_EVENT_MOUSEBUTTONUP;
                    ev.button.button = btn;
                    ev.button.x = xe.xbutton.x;
                    ev.button.y = xe.xbutton.y;
                    if (btn >= 1 && btn <= 5)
                        g_hjp.mouseButtons &= ~(1 << (btn - 1));
                    push_event(&ev);
                }
                break;
            }
            case ConfigureNotify: {
                HjpWindow *w = NULL;
                /* ウィンドウサイズ変更を検出 — 簡易方式 */
                ev.type = HJP_EVENT_WINDOWEVENT;
                ev.window.event = HJP_WINDOWEVENT_SIZE_CHANGED;
                ev.window.data1 = xe.xconfigure.width;
                ev.window.data2 = xe.xconfigure.height;
                push_event(&ev);
                break;
            }
            case FocusIn:
                ev.type = HJP_EVENT_WINDOWEVENT;
                ev.window.event = HJP_WINDOWEVENT_FOCUS_GAINED;
                push_event(&ev);
                break;

            case FocusOut:
                ev.type = HJP_EVENT_WINDOWEVENT;
                ev.window.event = HJP_WINDOWEVENT_FOCUS_LOST;
                push_event(&ev);
                break;

            case ClientMessage:
                /* WM_DELETE_WINDOW */
                if ((Atom)xe.xclient.data.l[0] == XInternAtom(dpy, "WM_DELETE_WINDOW", False)) {
                    ev.type = HJP_EVENT_QUIT;
                    push_event(&ev);
                }
                break;

            default:
                break;
        }
    }

    return pop_event(event);
}

/* =====================================================================
 * タイマー
 * ===================================================================*/

HjpTicks hjp_get_ticks(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (HjpTicks)((now.tv_sec - g_hjp.start_time.tv_sec) * 1000 +
                      (now.tv_nsec - g_hjp.start_time.tv_nsec) / 1000000);
}

void hjp_delay(uint32_t ms) {
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* =====================================================================
 * クリップボード (X11 簡易実装)
 * ===================================================================*/

char *hjp_get_clipboard_text(void) {
    if (!g_hjp.dpy) return strdup("");

    Window owner = XGetSelectionOwner(g_hjp.dpy, g_hjp.clipboard);
    if (owner == None) return strdup("");

    /* リクエストを送信して SelectionNotify を待つ */
    Atom clip_prop = XInternAtom(g_hjp.dpy, "HJP_CLIPBOARD", False);
    XConvertSelection(g_hjp.dpy, g_hjp.clipboard, g_hjp.utf8_string,
                      clip_prop, g_hjp.root, CurrentTime);
    XFlush(g_hjp.dpy);

    /* SelectionNotify を最大100ms待機 */
    XEvent ev;
    for (int i = 0; i < 50; i++) {
        if (XCheckTypedEvent(g_hjp.dpy, SelectionNotify, &ev)) {
            if (ev.xselection.property != None) {
                Atom type;
                int format;
                unsigned long nitems, remain;
                unsigned char *data = NULL;
                XGetWindowProperty(g_hjp.dpy, g_hjp.root, clip_prop, 0, 65536,
                                   True, AnyPropertyType, &type, &format,
                                   &nitems, &remain, &data);
                if (data) {
                    char *result = strdup((char *)data);
                    XFree(data);
                    return result;
                }
            }
            break;
        }
        usleep(2000); /* 2ms */
    }
    return strdup("");
}

void hjp_set_clipboard_text(const char *text) {
    if (!g_hjp.dpy || !text) return;
    free(g_hjp.clipboard_text);
    g_hjp.clipboard_text = strdup(text);
    XSetSelectionOwner(g_hjp.dpy, g_hjp.clipboard, g_hjp.root, CurrentTime);
    XFlush(g_hjp.dpy);
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
    unsigned int shape;
    switch (id) {
        case HJP_CURSOR_HAND:      shape = XC_hand2; break;
        case HJP_CURSOR_IBEAM:     shape = XC_xterm; break;
        case HJP_CURSOR_CROSSHAIR: shape = XC_crosshair; break;
        case HJP_CURSOR_SIZEALL:   shape = XC_fleur; break;
        default:                   shape = XC_left_ptr; break;
    }
    Cursor c = XCreateFontCursor(g_hjp.dpy, shape);
    return (HjpCursor)(uintptr_t)c;
}

void hjp_set_cursor(HjpCursor cursor) {
    if (!g_hjp.dpy) return;
    Cursor c = (Cursor)(uintptr_t)cursor;
    /* 全ウィンドウに適用 — ルートウィンドウ経由 */
    XDefineCursor(g_hjp.dpy, g_hjp.root, c);
    XFlush(g_hjp.dpy);
}

void hjp_free_cursor(HjpCursor cursor) {
    if (!g_hjp.dpy) return;
    Cursor c = (Cursor)(uintptr_t)cursor;
    XFreeCursor(g_hjp.dpy, c);
}

/* =====================================================================
 * ディスプレイ情報
 * ===================================================================*/

int hjp_get_num_displays(void) {
    if (!g_hjp.dpy) return 1;
    return ScreenCount(g_hjp.dpy);
}

void hjp_get_current_display_mode(int display_index, HjpDisplayMode *mode) {
    if (!mode) return;
    Display *dpy = g_hjp.dpy;
    int screen = (display_index >= 0 && display_index < ScreenCount(dpy))
                 ? display_index : g_hjp.screen;
    mode->w = DisplayWidth(dpy, screen);
    mode->h = DisplayHeight(dpy, screen);
    mode->refresh_rate = 60; /* X11 ではXRandR経由で取得可能だが、デフォルト60 */
}

void hjp_get_display_dpi(int display_index, float *ddpi, float *hdpi, float *vdpi) {
    Display *dpy = g_hjp.dpy;
    int screen = (display_index >= 0 && display_index < ScreenCount(dpy))
                 ? display_index : g_hjp.screen;
    int w_px = DisplayWidth(dpy, screen);
    int w_mm = DisplayWidthMM(dpy, screen);
    int h_px = DisplayHeight(dpy, screen);
    int h_mm = DisplayHeightMM(dpy, screen);
    float h = (w_mm > 0) ? (w_px * 25.4f / w_mm) : 96.0f;
    float v = (h_mm > 0) ? (h_px * 25.4f / h_mm) : 96.0f;
    float d = (h + v) / 2.0f;
    if (ddpi) *ddpi = d;
    if (hdpi) *hdpi = h;
    if (vdpi) *vdpi = v;
}

/* =====================================================================
 * メモリ
 * ===================================================================*/
void hjp_free(void *ptr) { free(ptr); }

/* =====================================================================
 * フォント (FreeType)
 * ===================================================================*/

typedef struct {
    FT_Face face;
    unsigned char *mem; /* from_mem の場合のバッファコピー */
} HjpFontInternal;

HjpFont hjp_font_create_from_file(const char *path) {
    if (!g_hjp.ft_init) return NULL;
    HjpFontInternal *f = (HjpFontInternal *)calloc(1, sizeof(HjpFontInternal));
    if (FT_New_Face(g_hjp.ft_lib, path, 0, &f->face) != 0) {
        free(f);
        return NULL;
    }
    return (HjpFont)f;
}

HjpFont hjp_font_create_from_mem(const unsigned char *data, int ndata) {
    if (!g_hjp.ft_init) return NULL;
    HjpFontInternal *f = (HjpFontInternal *)calloc(1, sizeof(HjpFontInternal));
    f->mem = (unsigned char *)malloc(ndata);
    memcpy(f->mem, data, ndata);
    if (FT_New_Memory_Face(g_hjp.ft_lib, f->mem, ndata, 0, &f->face) != 0) {
        free(f->mem);
        free(f);
        return NULL;
    }
    return (HjpFont)f;
}

void hjp_font_destroy(HjpFont font) {
    if (!font) return;
    HjpFontInternal *f = (HjpFontInternal *)font;
    if (f->face) FT_Done_Face(f->face);
    free(f->mem);
    free(f);
}

int hjp_font_get_glyph(HjpFont font, float size, uint32_t codepoint,
                        unsigned char **bitmap, int *w, int *h,
                        int *xoff, int *yoff, float *advance) {
    if (!font) return 0;
    HjpFontInternal *f = (HjpFontInternal *)font;
    FT_Face face = f->face;

    FT_Set_Pixel_Sizes(face, 0, (FT_UInt)size);
    FT_UInt gi = FT_Get_Char_Index(face, codepoint);
    if (gi == 0) return 0;
    if (FT_Load_Glyph(face, gi, FT_LOAD_RENDER) != 0) return 0;

    FT_GlyphSlot slot = face->glyph;
    FT_Bitmap *bmp = &slot->bitmap;

    int gw = (int)bmp->width;
    int gh = (int)bmp->rows;

    if (bitmap && gw > 0 && gh > 0) {
        *bitmap = (unsigned char *)malloc(gw * gh);
        for (int row = 0; row < gh; row++)
            memcpy(*bitmap + row * gw, bmp->buffer + row * bmp->pitch, gw);
    } else if (bitmap) {
        *bitmap = NULL;
    }
    if (w) *w = gw;
    if (h) *h = gh;
    if (xoff) *xoff = slot->bitmap_left;
    if (yoff) *yoff = -(int)slot->bitmap_top;
    if (advance) *advance = slot->advance.x / 64.0f;

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
    FT_Set_Pixel_Sizes(f->face, 0, (FT_UInt)size);
    float scale = size / f->face->units_per_EM;
    if (ascent)   *ascent   = f->face->ascender * scale;
    if (descent)  *descent  = -(f->face->descender * scale);
    if (line_gap) *line_gap = (f->face->height - f->face->ascender + f->face->descender) * scale;
}

float hjp_font_text_width(HjpFont font, float size, const char *str, const char *end) {
    if (!font || !str) return 0;
    HjpFontInternal *f = (HjpFontInternal *)font;
    FT_Set_Pixel_Sizes(f->face, 0, (FT_UInt)size);
    float width = 0;
    const char *p = str;
    while (*p && (!end || p < end)) {
        /* UTF-8デコード簡易版 */
        uint32_t cp;
        unsigned char c = (unsigned char)*p;
        if (c < 0x80) { cp = c; p += 1; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; p++; if (*p) { cp = (cp<<6)|(*p&0x3F); p++; } }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; p++; for (int i=0;i<2&&*p;i++) { cp = (cp<<6)|(*p&0x3F); p++; } }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; p++; for (int i=0;i<3&&*p;i++) { cp = (cp<<6)|(*p&0x3F); p++; } }
        else { cp = '?'; p++; }

        FT_UInt gi = FT_Get_Char_Index(f->face, cp);
        if (FT_Load_Glyph(f->face, gi, FT_LOAD_DEFAULT) == 0)
            width += f->face->glyph->advance.x / 64.0f;
    }
    return width;
}

/* =====================================================================
 * 画像読み込み (stb_image なしの組み込み PNG デコーダー代替)
 *
 * Linux ではシステムの libpng を試み、なければシンプルなフォールバック
 * ===================================================================*/

/* PNG ヘッダー検出用 */
static const unsigned char png_sig[8] = {137,80,78,71,13,10,26,10};

unsigned char *hjp_image_load_mem(const unsigned char *data, int ndata,
                                  int *w, int *h) {
    if (!data || ndata < 8) return NULL;

    /* dlopen で libpng を動的ロード (リンク不要) */
    void *libpng = dlopen("libpng16.so", RTLD_LAZY);
    if (!libpng) libpng = dlopen("libpng16.so.16", RTLD_LAZY);
    if (!libpng) libpng = dlopen("libpng.so", RTLD_LAZY);

    if (libpng && memcmp(data, png_sig, 8) == 0) {
        /* TODO: libpng経由デコード — 完全実装は大規模なため、
           ここでは簡易的に1x1透明ピクセルを返す */
        dlclose(libpng);
    }

    /* フォールバック: 1x1 透明ピクセル */
    *w = 1; *h = 1;
    unsigned char *px = (unsigned char *)calloc(4, 1);
    return px;
}

void hjp_image_free(unsigned char *pixels) { free(pixels); }

#endif /* __linux__ */
