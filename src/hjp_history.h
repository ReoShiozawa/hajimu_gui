#ifndef HJP_HISTORY_H
#define HJP_HISTORY_H
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *text, *label, *merge;
    size_t bytes;
    uint64_t token;
} HjpHistoryEntry;
typedef struct {
    HjpHistoryEntry *entries, draft;
    size_t count, cursor, capacity, bytes, budget;
    uint64_t sequence, clean;
    bool merging, grouping;
} HjpHistory;
static char *hjp_history_copy(const char *text) {
    size_t n = strlen(text) + 1;
    char *copy = malloc(n);
    if (copy) memcpy(copy, text, n);
    return copy;
}
static void hjp_history_entry_free(HjpHistoryEntry *entry) {
    free(entry->text); free(entry->label); free(entry->merge);
    memset(entry, 0, sizeof(*entry));
}
static bool hjp_history_entry(HjpHistoryEntry *entry, const char *text, const char *label,
                             const char *merge, size_t budget) {
    size_t a = strlen(text), b = strlen(label), c = strlen(merge);
    if (a > budget || b > 255 || c > 255 || a + b + c + 3 > budget) return false;
    *entry = (HjpHistoryEntry){.text = hjp_history_copy(text), .label = hjp_history_copy(label),
                              .merge = hjp_history_copy(merge), .bytes = a + b + c + 3};
    if (!entry->text || !entry->label || !entry->merge) {
        hjp_history_entry_free(entry); return false;
    }
    return true;
}
static void hjp_history_destroy(HjpHistory *history) {
    for (size_t i = 0; i < history->count; i++) hjp_history_entry_free(&history->entries[i]);
    free(history->entries);
    hjp_history_entry_free(&history->draft);
    memset(history, 0, sizeof(*history));
}
static bool hjp_history_init(HjpHistory *history, const char *initial, size_t capacity, size_t budget) {
    if (!capacity || capacity > 1000 || budget < 3 || budget > 64u * 1024u * 1024u) return false;
    HjpHistory value = {.capacity = capacity, .budget = budget, .sequence = 1, .clean = 1};
    value.entries = calloc(capacity + 2, sizeof(*value.entries));
    if (!value.entries) return false;
    if (!hjp_history_entry(&value.entries[0], initial, "", "", budget)) {
        free(value.entries); return false;
    }
    value.entries[0].token = 1;
    value.count = 1;
    value.bytes = value.entries[0].bytes;
    *history = value;
    return true;
}
static const char *hjp_history_current(const HjpHistory *history) {
    return history->count ? history->entries[history->cursor].text : NULL;
}
static bool hjp_history_push(HjpHistory *history, const char *text, const char *label, const char *merge) {
    if (!history->count || history->sequence == UINT64_MAX) return false;
    HjpHistoryEntry next;
    if (!hjp_history_entry(&next, text, history->grouping ? history->draft.label : label,
                           history->grouping ? "" : merge,
                           history->grouping ? history->budget - history->bytes : history->budget)) return false;
    if (history->grouping) {
        hjp_history_entry_free(&history->draft);
        history->draft = next;
        return true;
    }
    if (strcmp(text, hjp_history_current(history)) == 0) {
        hjp_history_entry_free(&next); return true;
    }
    /* 配置先を確保してからRedoを破棄し、失敗時には履歴を変更しない。 */
    while (history->count > history->cursor + 1) {
        HjpHistoryEntry *last = &history->entries[--history->count];
        history->bytes -= last->bytes;
        hjp_history_entry_free(last);
    }
    HjpHistoryEntry *current = &history->entries[history->cursor];
    if (history->merging && history->cursor && *merge && current->token != history->clean &&
        strcmp(current->merge, merge) == 0) {
        history->bytes -= current->bytes;
        hjp_history_entry_free(current);
    } else {
        history->cursor = history->count++;
        current = &history->entries[history->cursor];
    }
    next.token = ++history->sequence;
    *current = next;
    history->bytes += next.bytes;
    history->merging = true;
    while (history->count > history->capacity + 1 || history->bytes > history->budget) {
        history->bytes -= history->entries[0].bytes;
        hjp_history_entry_free(&history->entries[0]);
        memmove(history->entries, history->entries + 1, (--history->count) * sizeof(*history->entries));
        memset(&history->entries[history->count], 0, sizeof(*history->entries));
        history->cursor--;
    }
    return true;
}
static const char *hjp_history_move(HjpHistory *history, int direction) {
    if (history->grouping || !history->count || (direction < 0 && !history->cursor) ||
        (direction > 0 && history->cursor + 1 >= history->count)) return NULL;
    if (direction < 0) history->cursor--; else history->cursor++;
    history->merging = false;
    return hjp_history_current(history);
}
static bool hjp_history_mark_clean(HjpHistory *history) {
    if (!history->count || history->grouping) return false;
    history->clean = history->entries[history->cursor].token;
    history->merging = false;
    return true;
}
static bool hjp_history_is_clean(const HjpHistory *history) {
    return history->count && !history->grouping && history->entries[history->cursor].token == history->clean;
}
static bool hjp_history_group_begin(HjpHistory *history, const char *label) {
    if (!history->count || history->grouping || strlen(label) > 255) return false;
    if (!hjp_history_entry(&history->draft, hjp_history_current(history), label, "", history->budget - history->bytes)) return false;
    history->grouping = true;
    history->merging = false;
    return true;
}
static bool hjp_history_group_end(HjpHistory *history, bool commit) {
    if (!history->grouping) return false;
    history->grouping = false;
    bool ok = !commit || hjp_history_push(history, history->draft.text, history->draft.label, "");
    if (!ok) { history->grouping = true; return false; }
    hjp_history_entry_free(&history->draft);
    history->merging = false;
    return true;
}
#endif
