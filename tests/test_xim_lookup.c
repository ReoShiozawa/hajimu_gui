#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int lookup(XIC, XKeyPressedEvent *, char *, int, KeySym *, Status *);
static void *allocate(size_t);
#define HJP_XIM_LOOKUP lookup
#define HJP_XIM_ALLOC allocate
#include "../src/hjp_xim_lookup.h"

enum { NORMAL, OVERFLOW_AGAIN, KEY_ONLY, NO_DATA, NEGATIVE_LENGTH, INVALID_LENGTH };
static const char *fixture;
static int fixture_length, scenario, calls, allocations;
static bool fail_allocation;
static XKeyPressedEvent *expected_key;

static void *allocate(size_t size) {
    allocations++;
    return fail_allocation ? NULL : malloc(size);
}

static int lookup(XIC context, XKeyPressedEvent *key, char *buffer, int capacity,
                  KeySym *symbol, Status *status) {
    assert(context == NULL && key == expected_key && key->type == KeyPress);
    calls++;
    if (scenario == KEY_ONLY) { *symbol = XK_Return; *status = XLookupKeySym; return 0; }
    if (scenario == NO_DATA) { *status = XLookupNone; return 0; }
    if (scenario == NEGATIVE_LENGTH) { *status = XLookupChars; return -1; }
    if (scenario == INVALID_LENGTH) { *status = XLookupChars; return capacity + 1; }
    if (fixture_length > capacity || (scenario == OVERFLOW_AGAIN && calls == 2)) {
        *status = XBufferOverflow;
        return fixture_length + (calls == 2 ? 1 : 0);
    }
    memcpy(buffer, fixture, (size_t)fixture_length);
    *symbol = XK_a; *status = XLookupBoth;
    return fixture_length;
}

static void configure(const char *text, int length, int mode) {
    fixture = text; fixture_length = length; scenario = mode;
    calls = allocations = 0; fail_allocation = false;
}

int main(void) {
    XKeyPressedEvent key = {.type = KeyPress};
    expected_key = &key;
    KeySym symbol;
    HjpEvent event = {.type = HJP_EVENT_TEXTINPUT};
    configure("日本語", 9, NORMAL);
    assert(hjp_xim_lookup(NULL, &key, &symbol, &event));
    assert(calls == 1 && !allocations && symbol == XK_a && !strcmp(hjp_input_text(&event), "日本語"));
    hjp_document_dispose(&event);
    char *long_text = malloc(90001);
    assert(long_text);
    for (int i = 0; i < 10000; i++) memcpy(long_text + i * 9, "日本語", 9);
    long_text[90000] = '\0';
    for (int i = 0; i < 200; i++) {
        event = (HjpEvent){.type = HJP_EVENT_TEXTINPUT};
        configure(long_text, 90000, NORMAL);
        assert(hjp_xim_lookup(NULL, &key, &symbol, &event));
        assert(calls == 2 && allocations == 1 && symbol == XK_a);
        assert(event.text.owned_text && !strcmp(hjp_input_text(&event), long_text));
        hjp_document_dispose(&event);
    }
    int failures[] = {OVERFLOW_AGAIN, NO_DATA, NEGATIVE_LENGTH, INVALID_LENGTH};
    for (size_t i = 0; i < sizeof(failures) / sizeof(failures[0]); i++) {
        event = (HjpEvent){.type = HJP_EVENT_TEXTINPUT};
        strcpy(event.text.text, "保持");
        configure(long_text, 90000, failures[i]);
        assert(!hjp_xim_lookup(NULL, &key, &symbol, &event));
        assert(!event.text.owned_text && !strcmp(event.text.text, "保持"));
        assert(calls <= 2 && symbol == NoSymbol);
    }
    configure("", HJP_TEXT_DOCUMENT_LIMIT, NORMAL);
    assert(!hjp_xim_lookup(NULL, &key, &symbol, &event));
    assert(calls == 1 && allocations == 0);
    configure(long_text, 90000, NORMAL); fail_allocation = true;
    assert(!hjp_xim_lookup(NULL, &key, &symbol, &event));
    assert(calls == 1 && allocations == 1 && !event.text.owned_text);
    configure("", 0, KEY_ONLY);
    assert(!hjp_xim_lookup(NULL, &key, &symbol, &event) && symbol == XK_Return);
    configure("\xe3\x81", 2, NORMAL);
    assert(!hjp_xim_lookup(NULL, &key, &symbol, &event) && !event.text.owned_text);
    configure("a\0b", 3, NORMAL);
    assert(!hjp_xim_lookup(NULL, &key, &symbol, &event) && !event.text.owned_text);
    configure("\x7f", 1, NORMAL);
    assert(!hjp_xim_lookup(NULL, &key, &symbol, &event) && symbol == XK_a);
    configure("\r", 1, NORMAL);
    assert(!hjp_xim_lookup(NULL, &key, &symbol, &event) && symbol == XK_a);
    char *maximum = malloc(HJP_TEXT_DOCUMENT_LIMIT);
    assert(maximum);
    memset(maximum, 'x', HJP_TEXT_DOCUMENT_LIMIT - 1); maximum[HJP_TEXT_DOCUMENT_LIMIT - 1] = '\0';
    configure(maximum, HJP_TEXT_DOCUMENT_LIMIT - 1, NORMAL);
    assert(hjp_xim_lookup(NULL, &key, &symbol, &event));
    assert(!strcmp(hjp_input_text(&event), maximum) && calls == 2);
    hjp_document_dispose(&event);
    free(maximum); free(long_text);
    puts("XIM lookup passed: short/90KB/max text, bounded retry, status, allocation failure, invalid UTF-8 and ownership");
}
