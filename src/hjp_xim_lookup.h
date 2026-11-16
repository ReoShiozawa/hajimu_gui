#ifndef HJP_XIM_LOOKUP_H
#define HJP_XIM_LOOKUP_H
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include "hjp_document_event.h"
#include "hjp_grapheme.h"

#ifndef HJP_XIM_LOOKUP
#define HJP_XIM_LOOKUP Xutf8LookupString
#endif
#ifndef HJP_XIM_ALLOC
#define HJP_XIM_ALLOC malloc
#endif

/* XBufferOverflowでは同じイベントを必要容量で再取得する。再試行は一度だけ。
 * https://xorg.freedesktop.org/releases/X11R6.9.0/doc/html/Xutf8LookupString.3.html */
static inline bool hjp_xim_lookup(XIC context, XKeyPressedEvent *key, KeySym *symbol, HjpEvent *event) {
    *symbol = NoSymbol;
    if (event->type != HJP_EVENT_TEXTINPUT || event->text.owned_text) return false;
    char local[128], *buffer = local;
    int capacity = (int)sizeof(local) - 1;
    Status status = XLookupNone;
    KeySym result = NoSymbol;
    int length = HJP_XIM_LOOKUP(context, key, buffer, capacity, &result, &status);
    if (status == XBufferOverflow) {
        if (length <= 0 || length >= HJP_TEXT_DOCUMENT_LIMIT) return false;
        capacity = length;
        buffer = HJP_XIM_ALLOC((size_t)capacity + 1);
        if (!buffer) return false;
        result = NoSymbol; status = XLookupNone;
        length = HJP_XIM_LOOKUP(context, key, buffer, capacity, &result, &status);
    }
    bool accepted = false;
    if (status == XLookupKeySym || status == XLookupBoth) *symbol = result;
    if ((status == XLookupChars || status == XLookupBoth) && length > 0 && length <= capacity) {
        int offset = 0;
        while (offset < length) {
            utf8proc_int32_t character;
            utf8proc_ssize_t size = utf8proc_iterate((const utf8proc_uint8_t *)buffer + offset,
                                                    length - offset, &character);
            if (size <= 0 || character == 0) break;
            offset += (int)size;
        }
        if (offset == length && (unsigned char)buffer[0] >= 32 && (unsigned char)buffer[0] != 127) {
            buffer[length] = '\0';
            if (buffer != local && length >= (int)sizeof(event->text.text)) {
                event->text.owned_text = buffer;
                buffer = local;
                accepted = true;
            } else accepted = hjp_input_set_text(event, buffer);
        }
    }
    if (buffer != local) free(buffer);
    return accepted;
}
#endif
