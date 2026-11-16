#ifndef HJP_WIN_IME_H
#define HJP_WIN_IME_H
#include <windows.h>
#include <imm.h>
#include "hjp_document_event.h"

/* IMMの文字列長はUTF-16でもバイト数。再取得が変化した時は部分反映しない。 */
static bool hjp_win_ime_read(HIMC context, DWORD index, HjpEvent *event) {
    bool marked = index == GCS_COMPSTR;
    if ((!marked && index != GCS_RESULTSTR) || event->type != (marked ? HJP_EVENT_TEXTEDITING : HJP_EVENT_TEXTINPUT) ||
        event->text.owned_text) return false;
    LONG bytes = ImmGetCompositionStringW(context, index, NULL, 0);
    if (bytes < 0 || bytes % sizeof(WCHAR) || bytes > (HJP_TEXT_DOCUMENT_LIMIT - 1) * 2) return false;
    int units = bytes / sizeof(WCHAR);
    WCHAR *wide = malloc((size_t)bytes + sizeof(WCHAR));
    if (!wide) return false;
    if (bytes && ImmGetCompositionStringW(context, index, wide, (DWORD)bytes) != bytes) { free(wide); return false; }
    for (int i = 0; i < units; i++) if (!wide[i]) { free(wide); return false; }
    wide[units] = 0;
    int length = units ? WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, units, NULL, 0, NULL, NULL) : 0;
    if ((units && !length) || length >= HJP_TEXT_DOCUMENT_LIMIT) { free(wide); return false; }
    int start = units, end = units;
    if (marked) {
        LONG cursor = ImmGetCompositionStringW(context, GCS_CURSORPOS, NULL, 0);
        if (cursor >= 0) start = end = cursor;
        if (end > units) { free(wide); return false; }
        LONG attributes = ImmGetCompositionStringW(context, GCS_COMPATTR, NULL, 0);
        if (attributes == units && units) {
            BYTE *values = malloc((size_t)units);
            if (!values) { free(wide); return false; }
            if (ImmGetCompositionStringW(context, GCS_COMPATTR, values, units) != units) { free(values); free(wide); return false; }
            for (int i = 0; i < units; i++) {
                if (values[i] != ATTR_TARGET_CONVERTED && values[i] != ATTR_TARGET_NOTCONVERTED) continue;
                start = i; end = i + 1;
                while (end < units && (values[end] == ATTR_TARGET_CONVERTED || values[end] == ATTR_TARGET_NOTCONVERTED)) end++;
                break;
            }
            free(values);
        }
        int a = start ? WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, start, NULL, 0, NULL, NULL) : 0;
        int b = end ? WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, end, NULL, 0, NULL, NULL) : 0;
        if ((start && !a) || (end && !b)) { free(wide); return false; }
        start = a; end = b;
    }
    char *text = malloc((size_t)length + 1);
    if (!text) { free(wide); return false; }
    if (length && WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, units, text, length, NULL, NULL) != length) {
        free(text); free(wide); return false;
    }
    free(wide); text[length] = '\0';
    if (length < (int)sizeof(event->text.text)) { memcpy(event->text.text, text, (size_t)length + 1); free(text); }
    else event->text.owned_text = text;
    event->text.ime_result = !marked;
    if (marked) {
        event->text.has_selection = true;
        event->text.selection_start = start; event->text.selection_end = end;
    }
    return true;
}
#endif
