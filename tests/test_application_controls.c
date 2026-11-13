#include "../src/hajimu_gui.c"
#include <assert.h>

/* OSの実入力を送らず、部品が参照するフレーム入力を再現する。 */
static void frame(bool down, bool released, int mx) {
    g_cur->lay = (GuiLayout){0};
    g_cur->lay.w = 300;
    g_cur->in = (GuiInput){0};
    g_cur->in.mx = mx;
    g_cur->in.my = 10;
    g_cur->in.clicked = down;
    g_cur->in.released = released;
    g_next_widget_w = 0;
    hjpBeginFrame(g_cur->vg, 640, 400, 1);
}

int main(void) {
    size_t function_count = sizeof(gui_functions) / sizeof(gui_functions[0]);
    for (size_t i = 0; i < function_count; i++) {
        for (size_t j = i + 1; j < function_count; j++)
            assert(strcmp(gui_functions[i].name, gui_functions[j].name) != 0);
        if (strcmp(gui_functions[i].name, "省略テキスト") == 0) {
            assert(gui_functions[i].fn == fn_ellipsis_text);
            assert(gui_functions[i].min_args == 1);
        }
    }
    Value app_args[] = {hajimu_string("Control regression"), hajimu_number(640), hajimu_number(400)};
    Value app = fn_app_create(3, app_args);
    assert(app.type == VALUE_NUMBER);
    g_cur = &g_apps[(int)app.number];
    frame(false, false, 30);
    g_next_widget_w = 123;
    assert(fn_available_width(0, NULL).number == 123);
    assert(fn_available_width(0, NULL).number == 123);
    assert(g_next_widget_w == 123);
    g_next_widget_w = 0;
    Value zero_text[] = {hajimu_string("幅ゼロ"), hajimu_number(0)};
    fn_ellipsis_text(2, zero_text);
    assert(g_cur->last.w == 0 && !g_cur->last.hovered);
    hjpFontFaceId(g_cur->vg, g_cur->font_id);
    hjpFontSize(g_cur->vg, GUI_FONT_SIZE);
    const char *texts[] = {"", "abc", "日本語の長い作品名", "A日本B語", "𠮷田の作品"};
    for (size_t i = 0; i < sizeof(texts) / sizeof(texts[0]); i++) {
        const char *s = texts[i];
        size_t n = strlen(s);
        for (int width = 0; width < 400; width++) {
            size_t end = gui_text_prefix(g_cur->vg, s, n, (float)width);
            assert(end <= n);
            assert(((unsigned char)s[end] & 0xc0) != 0x80);
            assert(hjpTextBounds(g_cur->vg, 0, 0, s, s + end, NULL) <= width + 0.001f);
            if (end < n) {
                size_t next = end + 1;
                while (next < n && ((unsigned char)s[next] & 0xc0) == 0x80) next++;
                assert(hjpTextBounds(g_cur->vg, 0, 0, s, s + next, NULL) > width);
            }
        }
    }
    Value row[] = {hajimu_string("row"), hajimu_string("作品名"), hajimu_string("/path/project")};
    frame(true, false, 30);
    assert(!fn_detail_row(3, row).boolean);
    frame(false, true, 30);
    assert(fn_detail_row(3, row).boolean);
    frame(true, false, 30);
    fn_detail_row(3, row);
    frame(false, true, 500);
    assert(!fn_detail_row(3, row).boolean);
    Value opts = make_dict();
    dict_set(&opts, "有効", hajimu_bool(false));
    Value button[] = {hajimu_string("disabled"), opts};
    frame(true, false, 30);
    assert(!fn_button(2, button).boolean);
    assert(!g_disabled && g_cur->active == 0);
    frame(false, true, 30);
    assert(!fn_button(2, button).boolean);
    dict_set(&opts, "有効", hajimu_bool(true));
    button[1] = opts;
    frame(true, false, 30);
    assert(!fn_button(2, button).boolean);
    frame(false, true, 30);
    assert(fn_button(2, button).boolean);
    g_disabled = true;
    frame(true, false, 30);
    assert(!fn_detail_row(3, row).boolean);
    assert(g_disabled);
    g_disabled = false;
    Value items = hajimu_array();
    for (int i = 0; i < 30; i++) hajimu_array_push(&items, hajimu_number(i));
    Value list[] = {hajimu_string("list-shrink"), items, hajimu_number(-1)};
    frame(false, false, 30);
    g_cur->in.my = 50;
    g_cur->in.scroll_y = -100;
    fn_list(3, list);
    list[1].array.length = 1;
    frame(true, false, 30);
    g_cur->in.my = 50;
    assert(fn_list(3, list).number == 0);
    frame(true, false, 30);
    g_cur->in.my = 50;
    g_disabled = true;
    assert(fn_list(3, list).number == -1);
    g_disabled = false;
    frame(true, false, 30);
    g_cur->in.my = 399;
    assert(fn_list(3, list).number == -1);
    bool hovered, pressed;
    assert(!gui_widget_logic(42, 0, 0, 0, 30, &hovered, &pressed));
    assert(!hovered);
    hjpCancelFrame(g_cur->vg);
    fn_app_quit(1, &app);
    puts("application controls passed: registration, UTF-8, width peek, clicks, disabled, zero width, list shrink");
    return 0;
}
