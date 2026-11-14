#ifndef HJP_ACTIONS_H
#define HJP_ACTIONS_H
/* hajimu_gui.cの基本部品定義後に読み込む。コールバックは保持しない。 */
static int gui_action_index(const char *id) {
    if (!g_cur) return -1;
    for (int i = 0; i < g_cur->action_count; i++)
        if (strcmp(g_cur->actions[i].id, id) == 0) return i;
    return -1;
}
static bool gui_action_allowed(int i) {
    return i >= 0 && g_cur->actions[i].enabled && !g_disabled && !g_cur->composition[0] &&
        (!g_cur->modal_title[0] || g_cur->modal_entered);
}
static Value fn_action_register(int argc, Value *argv) {
    if (!g_cur || argc < 2 || argv[0].type != VALUE_STRING || argv[1].type != VALUE_STRING ||
        !argv[0].string.data[0] || strlen(argv[0].string.data) >= 128 || strlen(argv[1].string.data) >= 256)
        return hajimu_bool(false);
    const char *key = "", *mods = "";
    bool enabled = true, input_allowed = false;
    if (argc > 2 && argv[2].type == VALUE_DICT) {
        for (int k = 0; k < argv[2].dict.length; k++) {
            const char *name = argv[2].dict.keys[k];
            Value value = argv[2].dict.values[k];
            if (strcmp(name, "有効") == 0 && value.type == VALUE_BOOL) enabled = value.boolean;
            if (strcmp(name, "入力中") == 0 && value.type == VALUE_BOOL) input_allowed = value.boolean;
            if (strcmp(name, "キー") == 0 && value.type == VALUE_STRING) key = value.string.data;
            if (strcmp(name, "修飾") == 0 && value.type == VALUE_STRING) mods = value.string.data;
        }
    }
    if (strlen(key) >= 32 || strlen(mods) >= 32) return hajimu_bool(false);
    int i = gui_action_index(argv[0].string.data);
    if (i < 0) {
        if (g_cur->action_count == 64) return hajimu_bool(false);
        i = g_cur->action_count++;
        memset(&g_cur->actions[i], 0, sizeof(g_cur->actions[i]));
    }
    snprintf(g_cur->actions[i].id, 128, "%s", argv[0].string.data);
    snprintf(g_cur->actions[i].label, 256, "%s", argv[1].string.data);
    snprintf(g_cur->actions[i].key, 32, "%s", key);
    snprintf(g_cur->actions[i].modifiers, 32, "%s", mods);
    snprintf(g_cur->actions[i].shortcut, 96, "%s%s%s", mods, *mods && *key ? "+" : "", key);
    g_cur->actions[i].enabled = enabled;
    g_cur->actions[i].input_allowed = input_allowed;
    if (!gui_action_allowed(i)) g_cur->actions[i].pending = false;
    else if (*key && (!g_cur->focused || input_allowed)) {
        Value keys[2] = {{0}, {0}};
        keys[0].type = keys[1].type = VALUE_STRING;
        keys[0].string.data = g_cur->actions[i].key;
        keys[1].string.data = g_cur->actions[i].modifiers;
        bool shift = strstr(mods, "Shift") || strstr(mods, "shift");
        if (fn_shortcut(2, keys).boolean && g_cur->in.mod_shift == shift) {
            g_cur->actions[i].pending = true;
            g_cur->in.last_key = 0;
        }
    }
    return hajimu_bool(true);
}
static Value fn_action_button(int argc, Value *argv) {
    if (argc < 1 || argv[0].type != VALUE_STRING) return hajimu_bool(false);
    int i = gui_action_index(argv[0].string.data);
    if (i < 0) return hajimu_bool(false);
    char *names[] = {"補助", "有効", "ID"};
    Value values[] = {hajimu_bool(true), hajimu_bool(gui_action_allowed(i)), argv[0]};
    Value button[2] = {{0}, {0}};
    button[0].type = VALUE_STRING; button[0].string.data = g_cur->actions[i].label;
    button[1].type = VALUE_DICT; button[1].dict.keys = names; button[1].dict.values = values;
    button[1].dict.length = button[1].dict.capacity = 3;
    if (fn_button(2, button).boolean && gui_action_allowed(i)) g_cur->actions[i].pending = true;
    return hajimu_null();
}
static Value fn_action_menu(int argc, Value *argv) {
    if (argc < 1 || argv[0].type != VALUE_STRING) return hajimu_bool(false);
    int i = gui_action_index(argv[0].string.data);
    if (i < 0) return hajimu_bool(false);
    bool previous = g_disabled;
    g_disabled = !gui_action_allowed(i);
    Value menu[2] = {{0}, {0}};
    menu[0].type = menu[1].type = VALUE_STRING;
    menu[0].string.data = g_cur->actions[i].label;
    menu[1].string.data = g_cur->actions[i].shortcut;
    if (fn_menu_item(2, menu).boolean && !g_disabled) g_cur->actions[i].pending = true;
    g_disabled = previous;
    return hajimu_null();
}
static Value fn_action_take(int argc, Value *argv) {
    if (argc < 1 || argv[0].type != VALUE_STRING) return hajimu_bool(false);
    int i = gui_action_index(argv[0].string.data);
    if (!gui_action_allowed(i) || !g_cur->actions[i].pending || g_cur->actions[i].consumed) return hajimu_bool(false);
    g_cur->actions[i].pending = false;
    g_cur->actions[i].consumed = true;
    return hajimu_bool(true);
}
#endif
