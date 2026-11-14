#ifndef HJP_HISTORY_API_H
#define HJP_HISTORY_API_H
static int gui_history_index(Value id) {
    if (id.type != VALUE_NUMBER || !isfinite(id.number) || id.number <= 0 ||
        id.number >= INT_MAX || floor(id.number) != id.number) return -1;
    for (int i = 0; i < 16; i++) if (g_histories[i].id == (int)id.number &&
        (!g_histories[i].owner || g_histories[i].owner == g_cur)) return i;
    return -1;
}
static Value fn_history_create(int argc, Value *argv) {
    if (argc < 1 || argv[0].type != VALUE_STRING || g_history_next_id == INT_MAX) return hajimu_number(-1);
    double capacity = 50, budget = 8 * 1024 * 1024;
    if (argc > 1) { if (argv[1].type != VALUE_NUMBER) return hajimu_number(-1); capacity = argv[1].number; }
    if (argc > 2) { if (argv[2].type != VALUE_NUMBER) return hajimu_number(-1); budget = argv[2].number; }
    if (!isfinite(capacity) || capacity < 1 || capacity > 1000 || floor(capacity) != capacity ||
        !isfinite(budget) || budget < 3 || budget > 64 * 1024 * 1024 || floor(budget) != budget) return hajimu_number(-1);
    for (int i = 0; i < 16; i++) if (!g_histories[i].id) {
        if (!hjp_history_init(&g_histories[i].data, argv[0].string.data, (size_t)capacity, (size_t)budget)) return hajimu_number(-1);
        g_histories[i].id = g_history_next_id++;
        g_histories[i].owner = g_cur;
        return hajimu_number(g_histories[i].id);
    }
    return hajimu_number(-1);
}
static Value fn_history_push(int argc, Value *argv) {
    if (argc < 2 || argv[1].type != VALUE_STRING) return hajimu_bool(false);
    int i = gui_history_index(argv[0]);
    if (i < 0 || (argc > 2 && argv[2].type != VALUE_STRING) || (argc > 3 && argv[3].type != VALUE_STRING)) return hajimu_bool(false);
    return hajimu_bool(hjp_history_push(&g_histories[i].data, argv[1].string.data,
        argc > 2 ? argv[2].string.data : "", argc > 3 ? argv[3].string.data : ""));
}
static Value gui_history_move_value(int argc, Value *argv, int direction) {
    int i = argc ? gui_history_index(argv[0]) : -1;
    const char *text = i < 0 ? NULL : hjp_history_move(&g_histories[i].data, direction);
    return text ? hajimu_string(text) : hajimu_null();
}
static Value fn_history_undo(int argc, Value *argv) { return gui_history_move_value(argc, argv, -1); }
static Value fn_history_redo(int argc, Value *argv) { return gui_history_move_value(argc, argv, 1); }
static Value fn_history_current(int argc, Value *argv) {
    int i = argc ? gui_history_index(argv[0]) : -1;
    if (i < 0) return hajimu_null();
    HjpHistory *h = &g_histories[i].data;
    return hajimu_string(h->grouping ? h->draft.text : hjp_history_current(h));
}
static Value fn_history_state(int argc, Value *argv) {
    int i = argc ? gui_history_index(argv[0]) : -1;
    if (i < 0) return hajimu_null();
    HjpHistory *h = &g_histories[i].data;
    Value result = make_dict();
    dict_set(&result, "元に戻せる", hajimu_bool(!h->grouping && h->cursor > 0));
    dict_set(&result, "やり直せる", hajimu_bool(!h->grouping && h->cursor + 1 < h->count));
    dict_set(&result, "保存済み", hajimu_bool(hjp_history_is_clean(h)));
    dict_set(&result, "まとめ中", hajimu_bool(h->grouping));
    dict_set(&result, "件数", hajimu_number(h->count));
    dict_set(&result, "バイト数", hajimu_number(h->bytes + h->draft.bytes));
    dict_set(&result, "元に戻す操作", hajimu_string(h->cursor ? h->entries[h->cursor].label : ""));
    dict_set(&result, "やり直す操作", hajimu_string(h->cursor + 1 < h->count ? h->entries[h->cursor + 1].label : ""));
    return result;
}
static Value fn_history_clean(int argc, Value *argv) {
    int i = argc ? gui_history_index(argv[0]) : -1;
    if (i >= 0 && argc > 1) {
        if (argv[1].type != VALUE_BOOL || g_histories[i].data.grouping) return hajimu_bool(false);
        if (!argv[1].boolean) {
            g_histories[i].data.clean = 0;
            g_histories[i].data.merging = false;
            return hajimu_bool(true);
        }
    }
    return hajimu_bool(i >= 0 && hjp_history_mark_clean(&g_histories[i].data));
}
static Value fn_history_group_begin(int argc, Value *argv) {
    int i = argc ? gui_history_index(argv[0]) : -1;
    return hajimu_bool(i >= 0 && argc > 1 && argv[1].type == VALUE_STRING &&
        hjp_history_group_begin(&g_histories[i].data, argv[1].string.data));
}
static Value fn_history_group_end(int argc, Value *argv) {
    int i = argc ? gui_history_index(argv[0]) : -1;
    return hajimu_bool(i >= 0 && argc > 1 && argv[1].type == VALUE_BOOL &&
        hjp_history_group_end(&g_histories[i].data, argv[1].boolean));
}
static Value fn_history_destroy(int argc, Value *argv) {
    int i = argc ? gui_history_index(argv[0]) : -1;
    if (i < 0) return hajimu_bool(false);
    hjp_history_destroy(&g_histories[i].data);
    memset(&g_histories[i], 0, sizeof(g_histories[i]));
    return hajimu_bool(true);
}
#endif
