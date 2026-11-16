#ifndef HJP_DOCUMENT_EVENT_H
#define HJP_DOCUMENT_EVENT_H
#include "hjp_platform.h"
#include <stdlib.h>
#include <string.h>

static inline bool hjp_document_event(const HjpEvent *event) {
    return event->type == HJP_EVENT_TEXTREPLACE || event->type == HJP_EVENT_ACCESSIBILITY_TEXTREPLACE;
}

/* 短い置換は従来形式を維持。長い置換も一単位で配送する。 */
static inline bool hjp_document_set_text(HjpEvent *event, const char *text) {
    if (!hjp_document_event(event) || event->edit.owned_text) return false;
    size_t length = 0;
    while (length < HJP_TEXT_DOCUMENT_LIMIT && text[length]) length++;
    if (length >= HJP_TEXT_DOCUMENT_LIMIT) return false;
    if (length < sizeof(event->edit.text)) memcpy(event->edit.text, text, length + 1);
    else {
        event->edit.owned_text = malloc(length + 1);
        if (!event->edit.owned_text) return false;
        memcpy(event->edit.owned_text, text, length + 1);
    }
    return true;
}

static inline const char *hjp_document_text(const HjpEvent *event) {
    return event->edit.owned_text ? event->edit.owned_text : event->edit.text;
}

static inline void hjp_document_dispose(HjpEvent *event) {
    if (hjp_document_event(event)) {
        free(event->edit.owned_text);
        event->edit.owned_text = NULL;
    }
}
#endif
