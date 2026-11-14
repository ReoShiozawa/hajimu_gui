#ifndef HJP_NATIVE_QUEUE_H
#define HJP_NATIVE_QUEUE_H
#include "hjp_platform.h"
#include <string.h>

#define HJP_NATIVE_EVENTS 256
#define HJP_NATIVE_WINDOWS 32
typedef struct {
    HjpWindow *window;
    int generation;
    bool reset, has_quit, has_resize;
    uint64_t dropped;
    HjpEvent resize;
} HjpNativeLane;
typedef struct {
    HjpEvent events[HJP_NATIVE_EVENTS];
    unsigned head, tail;
    HjpNativeLane lanes[HJP_NATIVE_WINDOWS];
    bool global_quit;
} HjpNativeQueue;

static inline void hjp_native_dispose(HjpEvent *event) {
    if (event->type == HJP_EVENT_DROPFILE && event->drop.file) hjp_free(event->drop.file);
    *event = (HjpEvent){0};
}
static inline HjpNativeLane *hjp_native_lane(HjpNativeQueue *q, const HjpEvent *event) {
    for (unsigned i = 0; i < HJP_NATIVE_WINDOWS; i++)
        if (q->lanes[i].window && q->lanes[i].window == event->source.window &&
            q->lanes[i].generation == event->source.generation) return &q->lanes[i];
    return NULL;
}
static inline bool hjp_native_register(HjpNativeQueue *q, HjpWindow *window, int generation) {
    if (!window || generation <= 0) return false;
    for (unsigned i = 0; i < HJP_NATIVE_WINDOWS; i++) if (q->lanes[i].window == window) return false;
    for (unsigned i = 0; i < HJP_NATIVE_WINDOWS; i++) if (!q->lanes[i].window) {
        q->lanes[i] = (HjpNativeLane){.window = window, .generation = generation};
        return true;
    }
    return false;
}
static inline void hjp_native_unregister(HjpNativeQueue *q, HjpWindow *window) {
    /* 輪の順序を保ったまま、このウィンドウの所有イベントだけを取り除く。 */
    unsigned read = q->tail, write = q->tail;
    while (read != q->head) {
        HjpEvent event = q->events[read];
        read = (read + 1) % HJP_NATIVE_EVENTS;
        if (event.source.window == window) hjp_native_dispose(&event);
        else { q->events[write] = event; write = (write + 1) % HJP_NATIVE_EVENTS; }
    }
    q->head = write;
    for (unsigned i = 0; i < HJP_NATIVE_WINDOWS; i++) if (q->lanes[i].window == window)
        q->lanes[i] = (HjpNativeLane){0};
}
static inline unsigned hjp_native_free(const HjpNativeQueue *q) {
    for (unsigned i = 0; i < HJP_NATIVE_WINDOWS; i++) if (q->lanes[i].reset) return 0;
    return HJP_NATIVE_EVENTS - 1 - (q->head + HJP_NATIVE_EVENTS - q->tail) % HJP_NATIVE_EVENTS;
}
static inline void hjp_native_loss(HjpNativeQueue *q, HjpEvent *event) {
    HjpNativeLane *lane = hjp_native_lane(q, event);
    if (lane && lane->dropped < UINT64_MAX) lane->dropped++;
    hjp_native_dispose(event);
}
static inline bool hjp_native_push(HjpNativeQueue *q, const HjpEvent *input) {
    HjpEvent event = *input;
    HjpNativeLane *lane = hjp_native_lane(q, &event);
    if ((event.source.window || event.source.generation) && !lane) {
        hjp_native_dispose(&event); return false;
    }
    if (event.type == HJP_EVENT_QUIT) {
        if (lane) lane->has_quit = true; else q->global_quit = true;
        return true;
    }
    if (lane && event.type == HJP_EVENT_WINDOWEVENT && event.window.event == HJP_WINDOWEVENT_SIZE_CHANGED) {
        lane->resize = event; lane->has_resize = true; return true;
    }
    if (!hjp_native_free(q)) {
        /* 未処理の列全体を破棄し、途中の置換や押下だけを残さない。
         * 終了/サイズは別枠なので、リセットが必要でも失われない。 */
        while (q->tail != q->head) {
            hjp_native_loss(q, &q->events[q->tail]);
            q->tail = (q->tail + 1) % HJP_NATIVE_EVENTS;
        }
        for (unsigned i = 0; i < HJP_NATIVE_WINDOWS; i++) if (q->lanes[i].window) q->lanes[i].reset = true;
        hjp_native_loss(q, &event);
        return false;
    }
    q->events[q->head] = event;
    q->head = (q->head + 1) % HJP_NATIVE_EVENTS;
    return true;
}
static inline bool hjp_native_pop(HjpNativeQueue *q, HjpEvent *event) {
    for (unsigned i = 0; i < HJP_NATIVE_WINDOWS; i++) if (q->lanes[i].reset) {
        HjpNativeLane *lane = &q->lanes[i];
        *event = (HjpEvent){.type = HJP_EVENT_INPUT_RESET};
        event->source.window = lane->window; event->source.generation = lane->generation;
        event->reset.dropped = lane->dropped;
        lane->reset = false; lane->dropped = 0;
        return true;
    }
    for (unsigned i = 0; i < HJP_NATIVE_WINDOWS; i++) if (q->lanes[i].has_resize) {
        *event = q->lanes[i].resize; q->lanes[i].has_resize = false; return true;
    }
    if (q->tail != q->head) {
        *event = q->events[q->tail];
        q->events[q->tail] = (HjpEvent){0};
        q->tail = (q->tail + 1) % HJP_NATIVE_EVENTS;
        return true;
    }
    for (unsigned i = 0; i < HJP_NATIVE_WINDOWS; i++) if (q->lanes[i].has_quit) {
        *event = (HjpEvent){.type = HJP_EVENT_QUIT};
        event->source.window = q->lanes[i].window; event->source.generation = q->lanes[i].generation;
        q->lanes[i].has_quit = false; return true;
    }
    if (q->global_quit) { *event = (HjpEvent){.type = HJP_EVENT_QUIT}; q->global_quit = false; return true; }
    return false;
}
static inline void hjp_native_clear(HjpNativeQueue *q) {
    while (q->tail != q->head) {
        hjp_native_dispose(&q->events[q->tail]); q->tail = (q->tail + 1) % HJP_NATIVE_EVENTS;
    }
    memset(q, 0, sizeof(*q));
}
#endif
