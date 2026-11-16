#include "../src/hjp_event_queue.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static int allocated, disposed;
void hjp_free(void *pointer) { if (pointer) disposed++; free(pointer); }
static HjpEvent dropped_file(void) {
    HjpEvent event = {.type = HJP_EVENT_DROPFILE};
    event.drop.file = malloc(32);
    assert(event.drop.file);
    strcpy(event.drop.file, "owned-file.jp"); allocated++;
    return event;
}
int main(void) {
    HjpEventQueue queue = {0};
    char long_text[8192];
    memset(long_text, 'x', sizeof(long_text) - 1); long_text[sizeof(long_text) - 1] = '\0';
    for (int i = 0; i < 1000; i++) {
        HjpEvent edit = {.type = HJP_EVENT_TEXTREPLACE}, received;
        assert(hjp_document_set_text(&edit, long_text));
        assert(hjp_event_queue_push(&queue, &edit));
        assert(hjp_event_queue_pop(&queue, &received) && !strcmp(hjp_document_text(&received), long_text));
        hjp_event_dispose(&received);
    }
    for (int i = 0; i <= HJP_DEFERRED_EVENTS; i++) {
        HjpEvent edit = {.type = HJP_EVENT_TEXTREPLACE};
        assert(hjp_document_set_text(&edit, long_text));
        hjp_event_queue_push(&queue, &edit);
    }
    hjp_event_queue_clear(&queue);
    queue.dropped = 0;
    HjpEvent event = {.type = HJP_EVENT_KEYDOWN}, result;
    event.key.sym = 'a'; assert(hjp_event_queue_push(&queue, &event));
    event.key.sym = 'b'; assert(hjp_event_queue_push(&queue, &event));
    event.type = HJP_EVENT_QUIT; assert(hjp_event_queue_push(&queue, &event));
    assert(hjp_event_queue_pop(&queue, &result) && result.key.sym == 'a');
    assert(hjp_event_queue_pop(&queue, &result) && result.key.sym == 'b');
    assert(hjp_event_queue_pop(&queue, &result) && result.type == HJP_EVENT_QUIT);
    assert(!hjp_event_queue_pop(&queue, &result));
    event.type = HJP_EVENT_MOUSEMOTION;
    for (int i = 0; i < 200; i++) { event.motion.x = i; assert(hjp_event_queue_push(&queue, &event)); }
    assert(queue.count == 1);
    event = dropped_file(); assert(hjp_event_queue_push(&queue, &event));
    event = (HjpEvent){.type = HJP_EVENT_MOUSEMOTION}; assert(hjp_event_queue_push(&queue, &event));
    assert(queue.count == 3);
    hjp_event_queue_clear(&queue);
    assert(disposed == allocated);
    event.type = HJP_EVENT_QUIT; hjp_event_queue_push(&queue, &event);
    event.type = HJP_EVENT_WINDOWEVENT; event.window.event = HJP_WINDOWEVENT_SIZE_CHANGED;
    event.window.data1 = 900; hjp_event_queue_push(&queue, &event);
    event.type = HJP_EVENT_KEYDOWN;
    for (int i = 0; i < HJP_DEFERRED_EVENTS; i++) assert(hjp_event_queue_push(&queue, &event));
    event = dropped_file(); assert(!hjp_event_queue_push(&queue, &event));
    event = dropped_file(); assert(!hjp_event_queue_push(&queue, &event));
    assert(queue.dropped == HJP_DEFERRED_EVENTS + 2 && disposed == allocated);
    assert(hjp_event_queue_pop(&queue, &result) && result.type == HJP_EVENT_INPUT_RESET);
    assert(hjp_event_queue_pop(&queue, &result) && result.type == HJP_EVENT_WINDOWEVENT && result.window.data1 == 900);
    assert(hjp_event_queue_pop(&queue, &result) && result.type == HJP_EVENT_QUIT);
    assert(!hjp_event_queue_pop(&queue, &result));
    for (int i = 0; i < 10000; i++) {
        event = i % 3 == 0 ? dropped_file() : (HjpEvent){.type = HJP_EVENT_KEYDOWN};
        event.source.generation = i + 1;
        assert(hjp_event_queue_push(&queue, &event));
        assert(hjp_event_queue_pop(&queue, &result) && result.source.generation == i + 1);
        hjp_event_dispose(&result);
    }
    assert(disposed == allocated);
    event = dropped_file(); hjp_event_queue_push(&queue, &event);
    hjp_event_queue_clear(&queue);
    assert(disposed == allocated);
    puts("event queue passed: order, coalescing, overflow/reset, close/resize retention, ownership, 10000 wraps");
}
