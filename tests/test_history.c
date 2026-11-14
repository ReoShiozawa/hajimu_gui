#include "../src/hjp_history.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
    HjpHistory h = {0};
    assert(hjp_history_init(&h, "initial", 3, 1000));
    assert(hjp_history_is_clean(&h));
    assert(hjp_history_push(&h, "a", "typing", "text"));
    assert(hjp_history_push(&h, "ab", "typing", "text"));
    assert(h.count == 2);
    assert(strcmp(hjp_history_move(&h, -1), "initial") == 0);
    assert(hjp_history_is_clean(&h));
    assert(strcmp(hjp_history_move(&h, 1), "ab") == 0);
    assert(hjp_history_mark_clean(&h));
    assert(hjp_history_push(&h, "abc", "typing", "text"));
    assert(h.count == 3);
    assert(strcmp(hjp_history_move(&h, -1), "ab") == 0 && hjp_history_is_clean(&h));
    assert(hjp_history_group_begin(&h, "compound"));
    assert(!hjp_history_group_begin(&h, "nested"));
    assert(!hjp_history_move(&h, -1) && !hjp_history_mark_clean(&h));
    assert(hjp_history_push(&h, "step1", "step", ""));
    assert(hjp_history_push(&h, "step2", "step", ""));
    assert(hjp_history_group_end(&h, true));
    assert(!hjp_history_move(&h, 1));
    assert(strcmp(hjp_history_move(&h, -1), "ab") == 0);
    assert(strcmp(hjp_history_move(&h, 1), "step2") == 0);
    assert(strcmp(h.entries[h.cursor].label, "compound") == 0);
    assert(hjp_history_group_begin(&h, "cancel"));
    assert(hjp_history_push(&h, "discard", "", ""));
    assert(h.bytes + h.draft.bytes <= h.budget);
    assert(hjp_history_group_end(&h, false));
    assert(strcmp(hjp_history_current(&h), "step2") == 0);
    for (int i = 0; i < 10000; i++) {
        char text[32]; snprintf(text, sizeof(text), "edit%d", i);
        assert(hjp_history_push(&h, text, "", ""));
        assert(h.count <= 4 && h.bytes <= h.budget);
    }
    assert(!hjp_history_is_clean(&h));
    hjp_history_destroy(&h);
    assert(hjp_history_init(&h, "small", 3, 20));
    assert(!hjp_history_push(&h, "this snapshot is too large", "", ""));
    assert(hjp_history_is_clean(&h));
    assert(strcmp(hjp_history_current(&h), "small") == 0);
    assert(!hjp_history_group_begin(&h, "too-long-for-budget"));
    assert(hjp_history_is_clean(&h));
    assert(hjp_history_push(&h, "123456789012345", "", ""));
    assert(h.count == 1 && h.cursor == 0);
    hjp_history_destroy(&h);
    puts("history passed: merge, clean point, branch, group commit/cancel, 10000 edits, memory bound, rejection");
}
