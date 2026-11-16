#include "../src/hjp_native_queue.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static unsigned allocated, disposed;
void hjp_free(void *p) { if (p) disposed++; free(p); }
static int owners[HJP_NATIVE_WINDOWS + 1];
static HjpEvent input(unsigned owner, int type) {
    HjpEvent event = {.type = type};
    event.source.window = (HjpWindow *)&owners[owner]; event.source.generation = (int)owner + 1;
    if (type == HJP_EVENT_DROPFILE) {
        event.drop.file = malloc(32); assert(event.drop.file);
        strcpy(event.drop.file, "owned-native.jp"); allocated++;
    }
    return event;
}
int main(void) {
    HjpNativeQueue q = {0};
    HjpEvent event, result;
    assert(hjp_native_register(&q, (HjpWindow *)&owners[0], 1));
    assert(!hjp_native_register(&q, (HjpWindow *)&owners[0], 1));
    assert(hjp_native_register(&q, (HjpWindow *)&owners[1], 2));
    for (unsigned i = 0; i < 10000; i++) {
        event = input(i % 2, HJP_EVENT_DROPFILE);
        assert(hjp_native_push(&q, &event));
        assert(hjp_native_pop(&q, &result) && result.source.window == event.source.window);
        hjp_native_dispose(&result);
    }
    /* 通常枠を埋めても、各ウィンドウの終了要求と最新サイズは保持する。 */
    for (unsigned i = 0; i < HJP_NATIVE_EVENTS - 1; i++) {
        event = input(i % 2, i % 5 ? HJP_EVENT_KEYDOWN : HJP_EVENT_DROPFILE);
        assert(hjp_native_push(&q, &event));
    }
    for (unsigned i = 0; i < 2; i++) {
        event = input(i, HJP_EVENT_QUIT); assert(hjp_native_push(&q, &event));
        event = input(i, HJP_EVENT_WINDOWEVENT); event.window.event = HJP_WINDOWEVENT_SIZE_CHANGED;
        for (int size = 400; size < 500; size++) {
            event.window.data1 = size; assert(hjp_native_push(&q, &event));
        }
    }
    event = (HjpEvent){.type = HJP_EVENT_QUIT}; assert(hjp_native_push(&q, &event));
    event = input(0, HJP_EVENT_DROPFILE); assert(!hjp_native_push(&q, &event));
    assert(allocated == disposed && !hjp_native_free(&q));
    event = input(1, HJP_EVENT_KEYUP); assert(!hjp_native_push(&q, &event));
    uint64_t lost = 0;
    for (unsigned i = 0; i < 2; i++) {
        assert(hjp_native_pop(&q, &result) && result.type == HJP_EVENT_INPUT_RESET);
        assert(result.source.window == (HjpWindow *)&owners[i]); lost += result.reset.dropped;
    }
    assert(lost == HJP_NATIVE_EVENTS + 1 && hjp_native_free(&q) == HJP_NATIVE_EVENTS - 1);
    for (unsigned i = 0; i < 2; i++) {
        assert(hjp_native_pop(&q, &result) && result.type == HJP_EVENT_WINDOWEVENT);
        assert(result.source.window == (HjpWindow *)&owners[i] && result.window.data1 == 499);
    }
    for (unsigned i = 0; i < 2; i++) {
        assert(hjp_native_pop(&q, &result) && result.type == HJP_EVENT_QUIT);
        assert(result.source.window == (HjpWindow *)&owners[i]);
    }
    assert(hjp_native_pop(&q, &result) && result.type == HJP_EVENT_QUIT && !result.source.window);
    assert(!hjp_native_pop(&q, &result));
    /* 再利用したポインタでも古い世代は配送しない。循環の途中の削除も順序を保つ。 */
    for (unsigned i = 0; i < 200; i++) {
        event = input(0, HJP_EVENT_KEYDOWN); hjp_native_push(&q, &event); hjp_native_pop(&q, &result);
    }
    for (unsigned i = 0; i < 100; i++) {
        event = input(i % 2, HJP_EVENT_DROPFILE);
        assert(hjp_native_push(&q, &event));
    }
    hjp_native_unregister(&q, (HjpWindow *)&owners[0]);
    assert(hjp_native_register(&q, (HjpWindow *)&owners[0], 10));
    event = input(0, HJP_EVENT_DROPFILE); assert(!hjp_native_push(&q, &event));
    unsigned survivors = 0;
    while (hjp_native_pop(&q, &result)) {
        assert(result.source.window == (HjpWindow *)&owners[1]);
        hjp_native_dispose(&result); survivors++;
    }
    assert(survivors == 50 && allocated == disposed);
    event = input(1, HJP_EVENT_DROPFILE); hjp_native_push(&q, &event);
    hjp_native_clear(&q);
    assert(allocated == disposed);
    for (unsigned i = 0; i < HJP_NATIVE_WINDOWS; i++)
        assert(hjp_native_register(&q, (HjpWindow *)&owners[i], (int)i + 1));
    assert(!hjp_native_register(&q, (HjpWindow *)&owners[HJP_NATIVE_WINDOWS], 100));
    hjp_native_clear(&q);
    assert(hjp_native_register(&q, (HjpWindow *)&owners[0], 1));
    char long_text[8192];
    memset(long_text, 'x', sizeof(long_text) - 1); long_text[sizeof(long_text) - 1] = '\0';
    for (int i = 0; i < 1000; i++) {
        event = input(0, HJP_EVENT_TEXTINPUT);
        assert(hjp_input_set_text(&event, long_text));
        assert(hjp_native_push(&q, &event));
        assert(hjp_native_pop(&q, &result) && !strcmp(hjp_input_text(&result), long_text));
        hjp_native_dispose(&result);
    }
    event = input(0, HJP_EVENT_TEXTREPLACE);
    assert(hjp_document_set_text(&event, long_text));
    assert(hjp_native_push(&q, &event));
    hjp_native_unregister(&q, (HjpWindow *)&owners[0]);
    assert(!hjp_native_pop(&q, &result));
    event = input(0, HJP_EVENT_TEXTREPLACE);
    assert(hjp_document_set_text(&event, long_text));
    assert(!hjp_native_push(&q, &event));
    hjp_native_clear(&q);
    assert(hjp_native_register(&q, (HjpWindow *)&owners[0], 1));
    for (unsigned i = 0; i < HJP_NATIVE_EVENTS; i++) {
        event = input(0, HJP_EVENT_TEXTREPLACE);
        assert(hjp_document_set_text(&event, long_text));
        assert(hjp_native_push(&q, &event) == (i < HJP_NATIVE_EVENTS - 1));
    }
    assert(hjp_native_pop(&q, &result) && result.type == HJP_EVENT_INPUT_RESET);
    assert(!hjp_native_pop(&q, &result));
    hjp_native_clear(&q);
    puts("native queue passed: 10000 wraps, per-window reset/loss, close/resize retention, ownership, generations, bounded registration");
}
