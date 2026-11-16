#include <windows.h>
#include <imm.h>
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static HIMC WINAPI test_context(HWND);
static BOOL WINAPI test_release(HWND, HIMC);
static LONG WINAPI test_string(HIMC, DWORD, LPVOID, DWORD);
static BOOL WINAPI test_candidate(HIMC, LPCANDIDATEFORM);
static BOOL WINAPI test_composition(HIMC, LPCOMPOSITIONFORM);
static BOOL WINAPI test_notify(HIMC, DWORD, DWORD, DWORD);
static HWND WINAPI test_focus(void);
static void *test_malloc(size_t);
static void test_free(void *);
#define ImmGetContext test_context
#define ImmReleaseContext test_release
#define ImmGetCompositionStringW test_string
#define ImmSetCandidateWindow test_candidate
#define ImmSetCompositionWindow test_composition
#define ImmNotifyIME test_notify
#define GetFocus test_focus
#define malloc test_malloc
#define free test_free
#include "../src/hjp_platform_win32.c"
#undef malloc
#undef free

static const WCHAR *marked_text = L"次", *result_text = L"確定";
static int marked_units = 1, result_units = 2;
static LONG cursor = 1;
static const BYTE *attributes;
static int attribute_length;
static int failure, acquired, released, canceled, placements;
static DWORD failure_index;
static unsigned allocated, disposed;
static bool fail_allocation;
static HWND focused;
static CANDIDATEFORM last_candidate;
static COMPOSITIONFORM last_composition;

static void *test_malloc(size_t size) {
    void *pointer = fail_allocation ? NULL : malloc(size);
    if (pointer) allocated++;
    return pointer;
}
static void test_free(void *pointer) { if (pointer) disposed++; free(pointer); }
static HIMC WINAPI test_context(HWND window) { assert(window == focused); acquired++; return (HIMC)(UINT_PTR)1; }
static BOOL WINAPI test_release(HWND window, HIMC context) {
    assert(window == focused && context == (HIMC)(UINT_PTR)1); released++; return TRUE;
}
static HWND WINAPI test_focus(void) { return focused; }
static BOOL WINAPI test_candidate(HIMC context, LPCANDIDATEFORM form) {
    assert(context == (HIMC)(UINT_PTR)1); last_candidate = *form; placements++; return TRUE;
}
static BOOL WINAPI test_composition(HIMC context, LPCOMPOSITIONFORM form) {
    assert(context == (HIMC)(UINT_PTR)1); last_composition = *form; return TRUE;
}
static BOOL WINAPI test_notify(HIMC context, DWORD action, DWORD index, DWORD value) {
    assert(context == (HIMC)(UINT_PTR)1 && action == NI_COMPOSITIONSTR && index == CPS_CANCEL && !value);
    canceled++; return TRUE;
}
static LONG WINAPI test_string(HIMC context, DWORD index, LPVOID buffer, DWORD capacity) {
    assert(context == (HIMC)(UINT_PTR)1);
    if (index == GCS_CURSORPOS) return cursor;
    if (index == GCS_COMPATTR) {
        if (!attributes) return IMM_ERROR_NODATA;
        if (buffer) { assert(capacity >= (DWORD)attribute_length); memcpy(buffer, attributes, attribute_length); }
        return attribute_length;
    }
    assert(index == GCS_COMPSTR || index == GCS_RESULTSTR);
    int mode = !failure_index || failure_index == index ? failure : 0;
    if (mode == 1) return IMM_ERROR_GENERAL;
    if (mode == 2) return 3;
    if (mode == 3) return HJP_TEXT_DOCUMENT_LIMIT * 2;
    LONG bytes = (index == GCS_COMPSTR ? marked_units : result_units) * (LONG)sizeof(WCHAR);
    if (buffer) {
        if (mode == 4) return bytes - 2;
        assert(capacity >= (DWORD)bytes);
        memcpy(buffer, index == GCS_COMPSTR ? marked_text : result_text, bytes);
    }
    return bytes;
}
static void drain(void) { HjpEvent event; while (hjp_native_pop(&g_hjp.queue, &event)) hjp_native_dispose(&event); }

int main(void) {
    HIMC context = (HIMC)(UINT_PTR)1;
    HjpEvent event = {.type = HJP_EVENT_TEXTINPUT};
    assert(hjp_win_ime_read(context, GCS_RESULTSTR, &event));
    assert(event.text.ime_result && !strcmp(hjp_input_text(&event), "確定"));
    hjp_native_dispose(&event);
    WCHAR *large = malloc(30001 * sizeof(WCHAR));
    char *expected = malloc(90001);
    assert(large && expected);
    WCHAR characters[] = {0x65e5, 0x672c, 0x8a9e};
    for (int i = 0; i < 30000; i++) large[i] = characters[i % 3];
    large[30000] = 0;
    for (int i = 0; i < 10000; i++) memcpy(expected + i * 9, "日本語", 9);
    expected[90000] = 0;
    result_text = large; result_units = 30000;
    for (int i = 0; i < 200; i++) {
        event = (HjpEvent){.type = HJP_EVENT_TEXTINPUT};
        assert(hjp_win_ime_read(context, GCS_RESULTSTR, &event));
        assert(event.text.owned_text && !strcmp(hjp_input_text(&event), expected));
        hjp_native_dispose(&event);
    }
    for (failure = 1; failure <= 4; failure++) {
        event = (HjpEvent){.type = HJP_EVENT_TEXTINPUT};
        assert(!hjp_win_ime_read(context, GCS_RESULTSTR, &event) && !event.text.owned_text);
    }
    failure = 0; fail_allocation = true;
    assert(!hjp_win_ime_read(context, GCS_RESULTSTR, &event));
    fail_allocation = false;
    WCHAR invalid[] = {0xd800};
    result_text = invalid; result_units = 1;
    assert(!hjp_win_ime_read(context, GCS_RESULTSTR, &event));
    WCHAR embedded[] = {L'A', 0, L'B'};
    result_text = embedded; result_units = 3;
    assert(!hjp_win_ime_read(context, GCS_RESULTSTR, &event));
    marked_text = L"前日本後"; marked_units = 4; cursor = 4;
    BYTE target[] = {ATTR_INPUT, ATTR_TARGET_CONVERTED, ATTR_TARGET_CONVERTED, ATTR_CONVERTED};
    attributes = target; attribute_length = 4;
    event = (HjpEvent){.type = HJP_EVENT_TEXTEDITING};
    assert(hjp_win_ime_read(context, GCS_COMPSTR, &event));
    assert(event.text.selection_start == 3 && event.text.selection_end == 9);
    hjp_native_dispose(&event);
    WCHAR emoji[] = {0xd83d, 0xde00, 0x8a9e};
    attributes = NULL; marked_text = emoji; marked_units = 3; cursor = 1;
    event = (HjpEvent){.type = HJP_EVENT_TEXTEDITING};
    assert(!hjp_win_ime_read(context, GCS_COMPSTR, &event));
    cursor = 2;
    assert(hjp_win_ime_read(context, GCS_COMPSTR, &event));
    assert(event.text.selection_start == 4 && event.text.selection_end == 4);
    hjp_native_dispose(&event);

    focused = CreateWindowExW(0, L"STATIC", L"IME regression", WS_POPUP, 0, 0, 640, 400, NULL, NULL, GetModuleHandleW(NULL), NULL);
    assert(focused);
    HjpWindow owner = {.hwnd = focused, .id = 1, .width = 640, .height = 400};
    SetWindowLongPtrW(focused, GWLP_USERDATA, (LONG_PTR)&owner);
    assert(hjp_native_register(&g_hjp.queue, &owner, 1));
    g_hjp.text_input_active = true;
    hjp_set_text_input_rect(&owner, 10, 20, 1, 25);
    assert(placements && last_candidate.dwStyle == CFS_EXCLUDE && last_candidate.rcArea.bottom == 45);
    assert(last_candidate.ptCurrentPos.x == 10 && last_composition.ptCurrentPos.y == 20);
    int previous = placements;
    hjp_set_text_input_rect(&owner, NAN, 20, 1, 25);
    assert(placements == previous);
    result_text = L"確定"; result_units = 2; marked_text = L"次"; marked_units = 1; cursor = 1;
    hjp_wnd_proc(focused, WM_IME_STARTCOMPOSITION, 0, 0);
    assert(owner.ime_composing);
    hjp_wnd_proc(focused, WM_KEYDOWN, VK_RETURN, 0);
    hjp_wnd_proc(focused, WM_KEYUP, VK_RETURN, 0);
    assert(!hjp_native_pop(&g_hjp.queue, &event));
    hjp_wnd_proc(focused, WM_IME_COMPOSITION, 0, GCS_RESULTSTR | GCS_COMPSTR);
    assert(hjp_native_pop(&g_hjp.queue, &event) && event.type == HJP_EVENT_TEXTINPUT && event.text.ime_result);
    assert(!strcmp(hjp_input_text(&event), "確定")); hjp_native_dispose(&event);
    assert(hjp_native_pop(&g_hjp.queue, &event) && event.type == HJP_EVENT_TEXTEDITING);
    assert(!strcmp(hjp_composition_text(&event), "次") && event.text.selection_end == 3); hjp_native_dispose(&event);
    hjp_wnd_proc(focused, WM_CHAR, 0x78ba, 0);
    hjp_wnd_proc(focused, WM_IME_CHAR, 0x78ba, 0);
    assert(!hjp_native_pop(&g_hjp.queue, &event));
    hjp_wnd_proc(focused, WM_IME_ENDCOMPOSITION, 0, 0);
    assert(!owner.ime_composing);
    assert(hjp_native_pop(&g_hjp.queue, &event) && event.type == HJP_EVENT_TEXTEDITING && !hjp_input_text(&event)[0]);
    hjp_wnd_proc(focused, WM_IME_CHAR, 0x78ba, 0);
    assert(!hjp_native_pop(&g_hjp.queue, &event));
    hjp_wnd_proc(focused, WM_IME_COMPOSITION, 0, GCS_RESULTSTR);
    assert(!owner.ime_composing);
    assert(hjp_native_pop(&g_hjp.queue, &event) && event.type == HJP_EVENT_TEXTINPUT && event.text.ime_result);
    hjp_native_dispose(&event);
    assert(hjp_native_pop(&g_hjp.queue, &event) && event.type == HJP_EVENT_TEXTEDITING);
    assert(!hjp_native_pop(&g_hjp.queue, &event));
    hjp_wnd_proc(focused, WM_CHAR, 0x65e5, 0);
    assert(hjp_native_pop(&g_hjp.queue, &event) && event.type == HJP_EVENT_TEXTINPUT && !event.text.ime_result);
    assert(!strcmp(hjp_input_text(&event), "日")); hjp_native_dispose(&event);
    owner.ime_composing = true;
    hjp_wnd_proc(focused, WM_KILLFOCUS, 0, 0);
    assert(!owner.ime_composing && canceled == 1); drain();
    owner.ime_composing = true;
    hjp_stop_text_input();
    assert(!g_hjp.text_input_active && !owner.ime_composing && canceled == 2); drain();
    g_hjp.text_input_active = true;
    for (int i = 0; i < HJP_NATIVE_EVENTS - 1; i++) {
        event = (HjpEvent){.type = HJP_EVENT_KEYDOWN}; event.source.window = &owner; event.source.generation = 1;
        assert(hjp_native_push(&g_hjp.queue, &event));
    }
    unsigned head = g_hjp.queue.head;
    hjp_wnd_proc(focused, WM_IME_COMPOSITION, 0, GCS_RESULTSTR | GCS_COMPSTR);
    assert(g_hjp.queue.head == head); drain();
    fail_allocation = true;
    hjp_wnd_proc(focused, WM_IME_COMPOSITION, 0, GCS_RESULTSTR | GCS_COMPSTR);
    fail_allocation = false;
    assert(!hjp_native_pop(&g_hjp.queue, &event));
    result_text = large; result_units = 30000; failure = 1; failure_index = GCS_COMPSTR;
    hjp_wnd_proc(focused, WM_IME_COMPOSITION, 0, GCS_RESULTSTR | GCS_COMPSTR);
    assert(!hjp_native_pop(&g_hjp.queue, &event));
    failure = 0; failure_index = 0;
    assert(acquired == released);
    hjp_native_unregister(&g_hjp.queue, &owner);
    SetWindowLongPtrW(focused, GWLP_USERDATA, 0); DestroyWindow(focused);
    free(large); free(expected);
    assert(allocated == disposed);
    puts("Windows IME passed: UTF-16/90KB, cursor/target ranges, failures, message order, no duplicate chars, cancel and candidate rect");
}
