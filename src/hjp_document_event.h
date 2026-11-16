#ifndef HJP_DOCUMENT_EVENT_H
#define HJP_DOCUMENT_EVENT_H
#include "hjp_platform.h"
#include <stdlib.h>
#include <string.h>

static inline bool hjp_document_event(const HjpEvent *event) {
    return event->type == HJP_EVENT_TEXTREPLACE || event->type == HJP_EVENT_ACCESSIBILITY_TEXTREPLACE;
}

/* 短い置換は従来形式を維持。長い置換も一単位で配送する。 */
static inline bool hjp_event_store_text(char *buffer, size_t capacity, char **owned, const char *text) {
    if (*owned || !text) return false;
    size_t length = 0;
    while (length < HJP_TEXT_DOCUMENT_LIMIT && text[length]) length++;
    if (length >= HJP_TEXT_DOCUMENT_LIMIT) return false;
    if (length < capacity) memcpy(buffer, text, length + 1);
    else {
        *owned = malloc(length + 1);
        if (!*owned) return false;
        memcpy(*owned, text, length + 1);
    }
    return true;
}

static inline bool hjp_document_set_text(HjpEvent *event, const char *text) {
    return hjp_document_event(event) && hjp_event_store_text(event->edit.text,
        sizeof(event->edit.text), &event->edit.owned_text, text);
}

static inline bool hjp_composition_set_text(HjpEvent *event, const char *text) {
    return event->type == HJP_EVENT_TEXTEDITING && hjp_event_store_text(event->text.text,
        sizeof(event->text.text), &event->text.owned_text, text);
}

static inline bool hjp_input_set_text(HjpEvent *event, const char *text) {
    return event->type == HJP_EVENT_TEXTINPUT && hjp_event_store_text(event->text.text,
        sizeof(event->text.text), &event->text.owned_text, text);
}

static inline const char *hjp_input_text(const HjpEvent *event) {
    return event->text.owned_text ? event->text.owned_text : event->text.text;
}

static inline const char *hjp_composition_text(const HjpEvent *event) {
    return hjp_input_text(event);
}

static inline const char *hjp_document_text(const HjpEvent *event) {
    return event->edit.owned_text ? event->edit.owned_text : event->edit.text;
}

static inline void hjp_document_dispose(HjpEvent *event) {
    if (hjp_document_event(event)) {
        free(event->edit.owned_text);
        event->edit.owned_text = NULL;
    } else if (event->type == HJP_EVENT_TEXTEDITING || event->type == HJP_EVENT_TEXTINPUT) {
        free(event->text.owned_text);
        event->text.owned_text = NULL;
    }
}
#endif
