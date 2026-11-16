#ifndef HJP_EVENT_QUEUE_H
#define HJP_EVENT_QUEUE_H
#include "hjp_platform.h"
#include "hjp_document_event.h"
#include <string.h>

#define HJP_DEFERRED_EVENTS 64
typedef struct {
    HjpEvent events[HJP_DEFERRED_EVENTS], quit, resize;
    unsigned head, count;
    uint64_t dropped;
    bool blocked, has_quit, has_resize;
} HjpEventQueue;

static void hjp_event_dispose(HjpEvent *event) {
    hjp_document_dispose(event);
    if (event->type == HJP_EVENT_DROPFILE && event->drop.file) hjp_free(event->drop.file);
    *event = (HjpEvent){0};
}
static void hjp_event_queue_clear(HjpEventQueue *queue) {
    for (unsigned i = 0; i < queue->count; i++)
        hjp_event_dispose(&queue->events[(queue->head + i) % HJP_DEFERRED_EVENTS]);
    queue->head = queue->count = 0;
    queue->blocked = queue->has_quit = queue->has_resize = false;
}
static void hjp_event_queue_loss(HjpEventQueue *queue, unsigned count) {
    queue->dropped = UINT64_MAX - queue->dropped < count ? UINT64_MAX : queue->dropped + count;
}
static void hjp_event_queue_invalidate(HjpEventQueue *queue) {
    hjp_event_queue_loss(queue, queue->count);
    for (unsigned i = 0; i < queue->count; i++)
        hjp_event_dispose(&queue->events[(queue->head + i) % HJP_DEFERRED_EVENTS]);
    queue->head = queue->count = 0;
    queue->blocked = true;
}
/* 所有権を受け取る。上限超過時は中途半端なキー/置換列を残さずリセットする。 */
static bool hjp_event_queue_push(HjpEventQueue *queue, HjpEvent *event) {
    if (event->type == HJP_EVENT_INPUT_RESET) { hjp_event_queue_invalidate(queue); return true; }
    if (event->type == HJP_EVENT_QUIT) { queue->quit = *event; queue->has_quit = true; return true; }
    if (event->type == HJP_EVENT_WINDOWEVENT && event->window.event == HJP_WINDOWEVENT_SIZE_CHANGED) {
        queue->resize = *event; queue->has_resize = true; return true;
    }
    if (!queue->blocked && queue->count && event->type == HJP_EVENT_MOUSEMOTION) {
        unsigned last = (queue->head + queue->count - 1) % HJP_DEFERRED_EVENTS;
        if (queue->events[last].type == HJP_EVENT_MOUSEMOTION) { queue->events[last] = *event; return true; }
    }
    if (queue->blocked || queue->count == HJP_DEFERRED_EVENTS) {
        if (!queue->blocked) {
            hjp_event_queue_invalidate(queue);
        }
        hjp_event_queue_loss(queue, 1);
        hjp_event_dispose(event);
        return false;
    }
    queue->events[(queue->head + queue->count++) % HJP_DEFERRED_EVENTS] = *event;
    return true;
}
static bool hjp_event_queue_pop(HjpEventQueue *queue, HjpEvent *event) {
    if (queue->blocked) { queue->blocked = false; *event = (HjpEvent){.type = HJP_EVENT_INPUT_RESET}; return true; }
    if (queue->has_resize) { queue->has_resize = false; *event = queue->resize; return true; }
    if (queue->count) {
        *event = queue->events[queue->head];
        queue->events[queue->head] = (HjpEvent){0};
        queue->head = (queue->head + 1) % HJP_DEFERRED_EVENTS;
        queue->count--;
        return true;
    }
    if (queue->has_quit) { queue->has_quit = false; *event = queue->quit; return true; }
    return false;
}
#endif
