#ifndef HJP_GRAPHEME_H
#define HJP_GRAPHEME_H
#ifndef UTF8PROC_STATIC
#define UTF8PROC_STATIC
#endif
#if !defined(_WIN32) && defined(__GNUC__) && !defined(UTF8PROC_DLLEXPORT)
#define UTF8PROC_DLLEXPORT __attribute__((visibility("hidden")))
#endif
#include "../vendor/utf8proc/utf8proc.h"

/* startは既知の書記素境界。状態を境界でのみ初期化し、RI/ZWJの文脈を保つ。
 * 不正UTF-8は1バイトずつ独立させ、範囲外を読まず必ず前進する。 */
static inline int hjp_grapheme_step(const char *text, int length, int start) {
    if (start < 0) start = 0;
    if (start >= length) return length;
    utf8proc_int32_t previous, state = 0;
    utf8proc_ssize_t size = utf8proc_iterate((const utf8proc_uint8_t *)text + start, length - start, &previous);
    if (size <= 0) return start + 1;
    int offset = start + (int)size;
    while (offset < length) {
        utf8proc_int32_t next;
        size = utf8proc_iterate((const utf8proc_uint8_t *)text + offset, length - offset, &next);
        if (size <= 0 || utf8proc_grapheme_break_stateful(previous, next, &state)) break;
        previous = next;
        offset += (int)size;
    }
    return offset;
}
/* 任意のバイト位置からの移動では、先頭から境界を復元する。 */
static inline int hjp_grapheme_prev(const char *text, int length, int position) {
    if (position > length) position = length;
    int offset = 0;
    while (offset < position) {
        int next = hjp_grapheme_step(text, length, offset);
        if (next >= position) return offset;
        offset = next;
    }
    return 0;
}
static inline int hjp_grapheme_next(const char *text, int length, int position) {
    if (position < 0) position = 0;
    int offset = 0;
    while (offset < length) {
        offset = hjp_grapheme_step(text, length, offset);
        if (offset > position) return offset;
    }
    return length;
}
static inline int hjp_grapheme_floor(const char *text, int length, int position) {
    if (position <= 0) return 0;
    if (position >= length) return length;
    int previous = hjp_grapheme_prev(text, length, position);
    return hjp_grapheme_step(text, length, previous) == position ? position : previous;
}
static inline int hjp_grapheme_ceil(const char *text, int length, int position) {
    if (position <= 0) return 0;
    if (position >= length) return length;
    int floor = hjp_grapheme_floor(text, length, position);
    return floor == position ? floor : hjp_grapheme_step(text, length, floor);
}
#endif
