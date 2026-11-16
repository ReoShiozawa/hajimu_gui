#include <sys/wait.h>
static pid_t control_waitpid(pid_t pid, int *status, int flags);
static int control_kill(pid_t group, int signal);
#define HJP_TASK_WAITPID control_waitpid
#define HJP_TASK_KILL control_kill
#include "../src/hajimu_gui.c"
#include "../src/hjp_native_queue.h"
#include <assert.h>
void hjp_test_native_event(HjpEvent *event);

static pid_t deferred_pid;
static int deferred_polls;
static GuiApp *closing_capture_app;
static bool closing_capture_requested;
static int control_kill_error;
static int control_kill(pid_t group, int signal) {
    if (control_kill_error) { errno = control_kill_error; return -1; }
    return kill(group, signal);
}
static pid_t control_waitpid(pid_t pid, int *status, int flags) {
    assert(flags == WNOHANG);
    if (pid == deferred_pid && deferred_polls > 0) {
        const char *capture = getenv("GUI_CLOSE_CAPTURE");
        if (capture && closing_capture_app && closing_capture_app->close_frames >= 2 && !closing_capture_requested) {
            snprintf(closing_capture_app->screenshot_path, sizeof(closing_capture_app->screenshot_path), "%s", capture);
            closing_capture_app->screenshot_pending = closing_capture_requested = true;
        }
        deferred_polls--; return 0;
    }
    return waitpid(pid, status, flags);
}

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

static int ime_notifications;
static const char *ime_expected;
static Value ime_observe(Value *callback, int argc, Value *argv) {
    assert(argc == 1 && argv[0].type == VALUE_DICT);
    assert(!strcmp(gui_dict_lookup(argv[0], "未確定文字列").string.data, ime_expected));
    assert(gui_dict_lookup(argv[0], "変換中").boolean == (ime_expected[0] != '\0'));
    assert(gui_dict_lookup(argv[0], "変更番号").number == (double)g_cur->composition_version);
    ime_notifications++;
    /* 通知済みの更新を、再入したコールバックから再配送しない。 */
    assert(!fn_ime_composition(1, callback).boolean);
    return hajimu_null();
}

static void check_ime_notifications(void) {
    HajimuRuntime *previous_runtime = __hajimu_runtime;
    HajimuRuntime runtime = {.call = ime_observe};
    Value callback = {.type = VALUE_FUNCTION};
    gui_ime_update(g_cur, "に");
    assert(!fn_ime_composition(1, &callback).boolean);
    __hajimu_runtime = &runtime;
    ime_expected = "に";
    assert(fn_ime_composition(1, &callback).boolean);
    assert(!fn_ime_composition(1, &callback).boolean);
    gui_ime_update(g_cur, "に");
    assert(!fn_ime_composition(1, &callback).boolean);
    gui_ime_update(g_cur, "日本");
    gui_ime_update(g_cur, "日本語");
    ime_expected = "日本語";
    assert(!fn_ime_composition(0, NULL).boolean);
    Value invalid = hajimu_null();
    assert(!fn_ime_composition(1, &invalid).boolean);
    snprintf(g_cur->modal_title, sizeof(g_cur->modal_title), "IME test");
    assert(!fn_ime_composition(1, &callback).boolean);
    g_cur->modal_entered = true;
    assert(fn_ime_composition(1, &callback).boolean);
    g_cur->modal_entered = false;
    g_cur->modal_title[0] = '\0';
    gui_ime_update(g_cur, "");
    ime_expected = "";
    assert(fn_ime_composition(1, &callback).boolean);
    assert(ime_notifications == 3);
    char long_text[301];
    for (int i = 0; i < 100; i++) memcpy(long_text + i * 3, "あ", 3);
    long_text[300] = '\0';
    gui_ime_update(g_cur, long_text);
    assert(strlen(g_cur->composition) == 126);
    uint64_t version = g_cur->composition_version;
    gui_ime_update(g_cur, long_text);
    assert(g_cur->composition_version == version);
    gui_ime_update(g_cur, "");
    g_cur->composition_notified = g_cur->composition_version;
    GuiApp *app = g_cur;
    g_cur = NULL;
    assert(!fn_ime_composition(1, &callback).boolean);
    g_cur = app;
    __hajimu_runtime = previous_runtime;
}

static void check_textarea_layout(void) {
    frame(false, false, 500);
    Hjpcontext *vg = g_cur->vg;
    hjpFontFaceId(vg, g_cur->font_id);
    hjpFontSize(vg, GUI_FONT_SIZE);
    HjptextRow rows[128];
    const char *empty = "";
    int count = gui_textarea_rows(vg, empty, 60, rows, 128);
    assert(count == 1 && gui_textarea_caret_row(rows, count, empty) == 0);
    const char *trailing = "a\n\n";
    count = gui_textarea_rows(vg, trailing, 60, rows, 128);
    assert(count == 3 && gui_textarea_caret_row(rows, count, trailing + 3) == 2);
    const char *wrapped = "abcdefghijABCDEFGHIJ";
    count = gui_textarea_rows(vg, wrapped, 30, rows, 128);
    assert(count > 2);
    assert(rows[0].next == rows[1].start);
    assert(gui_textarea_caret_row(rows, count, rows[1].start) == 1);
    assert(gui_textarea_caret_row(rows, count, wrapped + strlen(wrapped)) == count - 1);
    GuiTextBuf buffer = {.len = 6, .cursor = 3};
    memcpy(buffer.buf, "前後", 7);
    char display[GUI_MAX_TEXT_BUF + 128];
    int caret = gui_text_display(&buffer, "日本語", display);
    assert(!strcmp(display, "前日本語後") && caret == 12);
    assert(!strcmp(buffer.buf, "前後") && buffer.len == 6 && buffer.cursor == 3);
    count = gui_textarea_rows(vg, display, 24, rows, 128);
    assert(count > 1);
    int ci = gui_textarea_caret_row(rows, count, display + caret);
    assert(display + caret >= rows[ci].start && display + caret <= rows[ci].end);

    Value args[] = {hajimu_string("textarea-layout"), hajimu_string("一\n二\n三\n四\n五\n六"), hajimu_number(2)};
    uint32_t id = gui_hash("textarea-layout");
    g_cur->focused = id;
    GuiTextBuf *tb = gui_get_text_buf(id, args[1].string.data);
    fn_textarea(3, args);
    assert(tb->scroll_y > 0);
    tb->cursor = 0;
    frame(false, false, 500);
    fn_textarea(3, args);
    assert(tb->scroll_y == 0);
    gui_ime_update(g_cur, "未確定");
    frame(false, false, 500);
    fn_textarea(3, args);
    assert(!strcmp(tb->buf, args[1].string.data));
    gui_ime_update(g_cur, "");
    frame(false, false, 500);
    tb->cursor = tb->sel_start = tb->sel_end = tb->len;
    g_cur->in.key_return = true;
    memcpy(g_cur->in.text_input, "確定", 7);
    g_cur->in.text_input_len = 6;
    Value result = fn_textarea(3, args);
    assert(result.type == VALUE_STRING && strstr(result.string.data, "六確定\n"));
    assert(strstr(tb->buf, "六確定\n") && !g_cur->in.key_return);
    frame(false, false, 500);
    gui_ime_update(g_cur, "未確定");
    int saved_len = tb->len;
    g_cur->in.key_return = true;
    fn_textarea(3, args);
    assert(tb->len == saved_len && g_cur->in.key_return);
    gui_ime_update(g_cur, "");
    g_cur->focused = 0;
    hjpCancelFrame(vg);
}

static void check_text_formatting(void) {
    frame(false, false, 500);
    Value args[] = {hajimu_string("format-safety"), hajimu_string("通貨")};
    GuiTextBuf *tb = gui_get_text_buf(gui_hash(args[0].string.data), "1234567");
    fn_input_formatter(2, args);
    assert(!strcmp(tb->buf, "1,234,567") && tb->len == 9 && tb->cursor <= tb->len);
    fn_input_formatter(2, args);
    assert(!strcmp(tb->buf, "1,234,567") && tb->len == 9);
    memset(tb->buf, '9', GUI_MAX_TEXT_BUF - 1);
    tb->buf[GUI_MAX_TEXT_BUF - 1] = '\0';
    tb->len = tb->cursor = GUI_MAX_TEXT_BUF - 1;
    tb->sel_start = 1; tb->sel_end = tb->len;
    fn_input_formatter(2, args);
    assert(tb->len == GUI_MAX_TEXT_BUF - 1 && tb->cursor == tb->len && tb->sel_start == 1);
    for (int i = 0; i < tb->len; i++) assert(tb->buf[i] == '9');
    strcpy(tb->buf, "日本123円"); tb->len = tb->cursor = (int)strlen(tb->buf);
    tb->sel_start = 0; tb->sel_end = tb->len;
    args[1] = hajimu_string("数字のみ");
    gui_ime_update(g_cur, "変換中");
    fn_input_formatter(2, args);
    assert(!strcmp(tb->buf, "日本123円"));
    gui_ime_update(g_cur, "");
    g_disabled = true; fn_input_formatter(2, args); g_disabled = false;
    assert(!strcmp(tb->buf, "日本123円"));
    fn_input_formatter(2, args);
    assert(!strcmp(tb->buf, "123") && tb->len == 3 && tb->cursor == 3);
    assert(tb->sel_start == 3 && tb->sel_end == 3);
    g_cur->focused = tb->id;
    g_cur->in = (GuiInput){.key_backspace = true};
    gui_text_buf_process(tb);
    assert(!strcmp(tb->buf, "12") && tb->len == 2);
    fn_input_formatter(0, NULL);
    g_cur->focused = 0;
    hjpCancelFrame(g_cur->vg);
}

static void check_text_selection(void) {
    GuiTextBuf tb = {.len = 7, .cursor = 7};
    memcpy(tb.buf, "a日本", 8);
    g_cur->in = (GuiInput){.key_left = true, .mod_shift = true};
    gui_text_buf_process(&tb);
    assert(tb.cursor == 4 && tb.sel_start == 7 && tb.sel_end == 4);
    gui_text_buf_process(&tb);
    assert(tb.cursor == 1 && tb.sel_start == 7 && tb.sel_end == 1);
    char visual[GUI_MAX_TEXT_BUF + 128];
    assert(gui_text_display(&tb, "仮", visual) == 4);
    assert(!strcmp(visual, "a仮") && !strcmp(tb.buf, "a日本"));
    g_cur->in = (GuiInput){.text_input_len = 3};
    memcpy(g_cur->in.text_input, "語", 4);
    gui_text_buf_process(&tb);
    assert(!strcmp(tb.buf, "a語") && tb.len == 4 && tb.cursor == 4 && tb.sel_start == tb.sel_end);
    g_cur->in = (GuiInput){.key_a = true};
    gui_text_buf_process(&tb);
    assert(tb.sel_start == 0 && tb.sel_end == tb.len);
    g_cur->in = (GuiInput){.key_left = true};
    gui_text_buf_process(&tb);
    assert(tb.cursor == 0 && tb.sel_start == tb.sel_end);
    g_cur->in = (GuiInput){.key_right = true, .mod_shift = true};
    gui_text_buf_process(&tb);
    assert(tb.sel_start == 0 && tb.sel_end == 1);
    g_cur->in = (GuiInput){.key_delete = true};
    gui_text_buf_process(&tb);
    assert(!strcmp(tb.buf, "語") && tb.cursor == 0);
    g_cur->in = (GuiInput){.key_end = true};
    gui_text_buf_process(&tb);
    g_cur->in = (GuiInput){.key_backspace = true};
    gui_text_buf_process(&tb);
    assert(tb.len == 0 && tb.cursor == 0);

    memset(tb.buf, 'a', GUI_MAX_TEXT_BUF - 1);
    tb.buf[GUI_MAX_TEXT_BUF - 1] = '\0';
    tb.len = tb.cursor = GUI_MAX_TEXT_BUF - 1;
    tb.sel_start = tb.sel_end = tb.cursor;
    assert(!gui_text_replace_selection(&tb, "語", 3));
    tb.sel_start = tb.cursor - 3;
    tb.sel_end = tb.cursor;
    assert(gui_text_replace_selection(&tb, "語", 3));
    assert(tb.len == GUI_MAX_TEXT_BUF - 1 && !strcmp(tb.buf + tb.len - 3, "語"));
    tb.sel_start = 0; tb.sel_end = tb.len;
    g_cur->in = (GuiInput){.key_x = true};
    gui_text_buf_edit(&tb, false);
    assert(tb.len == GUI_MAX_TEXT_BUF - 1);

    Value label = hajimu_string("selection-api");
    GuiTextBuf *field = gui_get_text_buf(gui_hash(label.string.data), "a日本b");
    Value range[] = {label, hajimu_number(3), hajimu_number(1)};
    assert(fn_text_select_range(3, range).boolean);
    assert(field->sel_start == 7 && field->sel_end == 1 && field->cursor == 1);
    Value pos[] = {label, hajimu_number(2)};
    assert(fn_text_cursor_pos(2, pos).number == 2 && field->cursor == 4);
    assert(field->sel_start == field->sel_end);
    pos[1] = hajimu_number(NAN);
    assert(fn_text_cursor_pos(2, pos).number == -1 && field->cursor == 4);
    pos[1] = hajimu_number(1.5);
    assert(fn_text_cursor_pos(2, pos).number == -1);
    range[0] = hajimu_string("missing-field");
    assert(!fn_text_select_range(3, range).boolean);
    Value replace[] = {label, hajimu_string("日本"), hajimu_string("語")};
    assert(fn_text_find_replace(3, replace).number == 1);
    assert(field->len == 5 && !strcmp(field->buf, "a語b"));
    assert(field->cursor == 4 && field->sel_start == field->sel_end);
    char too_large[GUI_MAX_TEXT_BUF + 1];
    memset(too_large, 'a', GUI_MAX_TEXT_BUF);
    too_large[GUI_MAX_TEXT_BUF] = '\0';
    replace[1] = hajimu_string("語");
    replace[2] = hajimu_string(too_large);
    assert(fn_text_find_replace(3, replace).number == -1);
    assert(field->len == 5 && !strcmp(field->buf, "a語b"));
    g_cur->in = (GuiInput){0};
}

static void check_grapheme_editing(void) {
    const char *clusters[] = {
        "e\xcc\x81", "\xe3\x81\x8b\xe3\x82\x99",
        "\xf0\x9f\x91\x8d\xf0\x9f\x8f\xbd",
        "\xf0\x9f\x87\xaf\xf0\x9f\x87\xb5",
        "\xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x91\xa7",
        "\xe0\xa4\x95\xe0\xa5\x8d\xe0\xa4\xb7", "\r\n"
    };
    frame(false, false, 500);
    Hjpcontext *vg = g_cur->vg;
    for (size_t i = 0; i < sizeof(clusters) / sizeof(clusters[0]); i++) {
        GuiTextBuf original = {0};
        snprintf(original.buf, sizeof(original.buf), "A%sZ", clusters[i]);
        original.len = (int)strlen(original.buf); original.cursor = original.len - 1;
        GuiTextBuf tb = original;
        g_cur->in = (GuiInput){.key_left = true}; gui_text_buf_process(&tb);
        assert(tb.cursor == 1);
        g_cur->in = (GuiInput){.key_right = true, .mod_shift = true}; gui_text_buf_process(&tb);
        assert(tb.sel_start == 1 && tb.sel_end == original.len - 1);
        assert(gui_text_replace_selection(&tb, "X", 1) && !strcmp(tb.buf, "AXZ"));
        tb = original;
        g_cur->in = (GuiInput){.key_backspace = true}; gui_text_buf_process(&tb);
        assert(!strcmp(tb.buf, "AZ") && tb.cursor == 1);
        tb = original; tb.cursor = 1;
        g_cur->in = (GuiInput){.key_delete = true}; gui_text_buf_process(&tb);
        assert(!strcmp(tb.buf, "AZ") && tb.cursor == 1);
        /* 公開APIなどが結合列の内部へ位置指定した後の通常削除も分断しない。 */
        for (int backward = 0; backward < 2; backward++) {
            tb = original; tb.cursor = utf8_next(tb.buf, 1, tb.len);
            g_cur->in = (GuiInput){.key_delete = !backward, .key_backspace = backward};
            gui_text_buf_process(&tb);
            assert(!strcmp(tb.buf, "AZ") && tb.cursor == 1);
        }
        for (int x = -10; x < 300; x++) {
            for (int masked = 0; masked < 2; masked++) {
                int hit = gui_text_hit_offset(vg, original.buf, original.buf + original.len, x, masked);
                assert(hit == 0 || hit == 1 || hit == original.len - 1 || hit == original.len);
            }
        }
        HjptextRow rows[64];
        int count = gui_textarea_rows(vg, original.buf, 1, rows, 64);
        assert(count >= 2);
        for (int row = 0; row < count; row++) {
            assert(hjp_grapheme_floor(original.buf, original.len, (int)(rows[row].start - original.buf)) == rows[row].start - original.buf);
            assert(hjp_grapheme_floor(original.buf, original.len, (int)(rows[row].end - original.buf)) == rows[row].end - original.buf);
            assert(hjp_grapheme_floor(original.buf, original.len, (int)(rows[row].next - original.buf)) == rows[row].next - original.buf);
        }
        if (!strcmp(clusters[i], "\r\n")) assert(rows[0].next == original.buf + 3);
    }
    Value label = hajimu_string("grapheme-codepoint-api");
    GuiTextBuf *field = gui_get_text_buf(gui_hash(label.string.data), "e\xcc\x81Z");
    Value pos[] = {label, hajimu_number(1)};
    assert(fn_text_cursor_pos(2, pos).number == 1 && field->cursor == 1);
    Value selection[] = {label, hajimu_number(1), hajimu_number(2)};
    assert(fn_text_select_range(3, selection).boolean && field->sel_start == 1 && field->sel_end == 3);
    GuiTextBuf full = {0};
    memset(full.buf, 'a', GUI_MAX_TEXT_BUF - 2);
    full.len = full.cursor = GUI_MAX_TEXT_BUF - 2;
    assert(!gui_text_replace_selection(&full, "e\xcc\x81", 3));
    assert(full.len == GUI_MAX_TEXT_BUF - 2);
    full.sel_start = full.cursor - 1; full.sel_end = full.cursor;
    assert(!gui_text_replace_selection(&full, "e\xcc\x81", 3));
    assert(full.sel_end - full.sel_start == 1 && full.buf[full.sel_start] == 'a');
    char initial[GUI_MAX_TEXT_BUF + 8];
    memset(initial, 'a', GUI_MAX_TEXT_BUF - 2);
    strcpy(initial + GUI_MAX_TEXT_BUF - 2, "e\xcc\x81Z");
    GuiTextBuf *clipped = gui_get_text_buf(gui_hash("grapheme-capacity"), initial);
    assert(clipped->len == GUI_MAX_TEXT_BUF - 2);
    GuiTextBuf joined = {.buf = "e\xcc\x81", .len = 3, .cursor = 1};
    g_cur->in = (GuiInput){.text_input_len = 1, .text_input = "a"};
    gui_text_buf_process(&joined);
    assert(!strcmp(joined.buf, "ea\xcc\x81") && joined.cursor == 4);
    g_cur->in = (GuiInput){0};
    hjpCancelFrame(vg);
}

static void check_text_navigation(void) {
    frame(false, false, 500);
    Hjpcontext *vg = g_cur->vg;
    hjpFontFaceId(vg, g_cur->font_id);
    hjpFontSize(vg, GUI_FONT_SIZE);
    const char *text = "a日本b";
    float first = hjpTextBounds(vg, 0, 0, text, text + 1, NULL);
    float second = hjpTextBounds(vg, 0, 0, text, text + 4, NULL);
    assert(gui_text_hit_offset(vg, text, text + 8, -100, false) == 0);
    assert(gui_text_hit_offset(vg, text, text + 8, first + (second - first) * 0.25f, false) == 1);
    assert(gui_text_hit_offset(vg, text, text + 8, first + (second - first) * 0.75f, false) == 4);
    assert(gui_text_hit_offset(vg, text, text + 8, 10000, false) == 8);
    GuiTextBuf tb = {.len = 14, .cursor = 5};
    memcpy(tb.buf, "abcde\nx\nabcde", 14);
    tb.len = (int)strlen(tb.buf);
    uint32_t id = 123456;
    g_cur->focused = id;
    g_cur->in = (GuiInput){.key_down = true};
    gui_textarea_navigate(&tb, id, false, 0, 0, 300, 100);
    assert(tb.cursor == 7 && tb.preferred_x_valid);
    g_cur->in.key_down = true;
    gui_textarea_navigate(&tb, id, false, 0, 0, 300, 100);
    assert(tb.cursor == tb.len);
    g_cur->in = (GuiInput){.key_up = true, .mod_shift = true};
    gui_textarea_navigate(&tb, id, false, 0, 0, 300, 100);
    assert(tb.cursor == 7 && tb.sel_start == tb.len && tb.sel_end == 7);
    g_cur->in = (GuiInput){.clicked = true, .down = true};
    gui_text_mouse(&tb, id, true, 1);
    assert(g_cur->text_drag == id && tb.cursor == 1 && tb.sel_start == 1 && !g_cur->in.clicked);
    g_cur->in = (GuiInput){.down = true};
    gui_text_mouse(&tb, id, false, tb.len);
    assert(tb.sel_start == 1 && tb.sel_end == tb.len);
    g_cur->in = (GuiInput){.released = true};
    gui_text_mouse(&tb, id, false, 6);
    assert(g_cur->text_drag == 0 && tb.cursor == 6 && tb.sel_start == 1);
    g_cur->in = (GuiInput){.clicked = true, .down = true};
    g_disabled = true;
    gui_text_mouse(&tb, id, true, 0);
    assert(tb.cursor == 6 && g_cur->text_drag == 0);
    g_disabled = false;
    gui_text_collapse(&tb, 1);
    g_cur->in = (GuiInput){.clicked = true, .down = true, .mod_shift = true};
    gui_text_mouse(&tb, id, true, 5);
    assert(tb.sel_start == 1 && tb.sel_end == 5);
    g_cur->in = (GuiInput){.down = true, .mx = 500, .my = 200};
    gui_textarea_navigate(&tb, id, false, 0, 0, 300, 30);
    assert(tb.scroll_y > 0 && tb.cursor == tb.len);
    g_cur->in = (GuiInput){.released = true};
    gui_text_mouse(&tb, id, false, tb.cursor);
    assert(g_cur->text_drag == 0);
    g_cur->in = (GuiInput){.clicked = true, .down = true};
    gui_text_mouse(&tb, id, true, 5);
    g_cur->in = (GuiInput){.released = true};
    gui_text_mouse(&tb, id, false, 1);
    assert(tb.sel_start == 5 && tb.sel_end == 1 && tb.cursor == 1);
    GuiInput modifiers = {0};
    gui_input_modifiers(&modifiers, HJP_KMOD_SHIFT);
    assert(modifiers.mod_shift);
    gui_input_modifiers(&modifiers, 0);
    assert(!modifiers.mod_shift && !modifiers.mod_ctrl);
    g_cur->focused = 0;
    g_cur->in = (GuiInput){0};
    hjpCancelFrame(vg);
}

static void check_document_edits(void) {
    uint32_t id = gui_hash("document-edit");
    GuiTextBuf *tb = gui_get_text_buf(id, "A😀日本Z");
    g_cur->focused = id;
    HjpEvent event = {.type = HJP_EVENT_TEXTSELECTION};
    event.edit.window = g_cur->window;
    event.edit.generation = hjp_window_get_id(g_cur->window);
    event.edit.id = id; event.edit.start = 5; event.edit.end = event.edit.cursor = 11;
    assert(gui_apply_document_event(g_cur, &event));
    assert(tb->sel_start == 5 && tb->sel_end == 11);
    event.type = HJP_EVENT_TEXTREPLACE;
    snprintf(event.edit.text, sizeof(event.edit.text), "%s", "日本語");
    assert(gui_apply_document_event(g_cur, &event));
    assert(!strcmp(tb->buf, "A😀日本語Z") && tb->len == 15 && tb->cursor == 14);
    event.edit.start = 2; event.edit.end = 3;
    assert(!gui_apply_document_event(g_cur, &event));
    assert(!strcmp(tb->buf, "A😀日本語Z"));
    event.edit.start = 1; event.edit.end = 5; event.edit.text[0] = '\0';
    assert(gui_apply_document_event(g_cur, &event));
    assert(!strcmp(tb->buf, "A日本語Z"));
    event.edit.generation++;
    assert(!gui_apply_document_event(g_cur, &event));
    g_cur->focused = 0;
    assert(!gui_apply_document_event(g_cur, &event));
}

static void check_accessible_text(void) {
    bool previous_navigation = g_cur->nav_enabled;
    g_cur->nav_enabled = false;
    frame(false, false, 500);
    hjp_accessibility_begin(g_cur->window);
    Value args[] = {hajimu_string("accessible-text"), hajimu_string("元の本文")};
    fn_text_input(2, args);
    hjp_accessibility_end(g_cur->window);
    uint32_t id = gui_hash(args[0].string.data);
    GuiTextBuf *tb = gui_get_text_buf(id, "");
    assert(tb->ax_visible && tb->ax_enabled && !tb->ax_secure);
    HjpEvent event = {.type = HJP_EVENT_ACCESSIBILITY_FOCUS};
    event.edit.window = g_cur->window;
    event.edit.generation = hjp_window_get_id(g_cur->window);
    event.edit.id = id; event.edit.cursor = 1;
    assert(gui_apply_accessibility_focus(g_cur, &event) && g_cur->focused == id);
    frame(false, false, 500);
    gui_nav_begin();
    fn_text_input(2, args);
    gui_nav_end();
    assert(g_cur->focused == id);
    event.type = HJP_EVENT_ACCESSIBILITY_TEXTREPLACE;
    event.edit.start = 0; event.edit.end = -1;
    snprintf(event.edit.text, sizeof(event.edit.text), "%s", "編集済み");
    assert(gui_apply_document_event(g_cur, &event) && !strcmp(tb->buf, "編集済み"));
    tb->ax_secure = true;
    assert(!gui_apply_document_event(g_cur, &event));
    tb->ax_secure = false;
    tb->ax_enabled = false;
    assert(!gui_apply_document_event(g_cur, &event));
    event.type = HJP_EVENT_ACCESSIBILITY_FOCUS;
    assert(!gui_apply_accessibility_focus(g_cur, &event));
    tb->ax_enabled = true; tb->ax_visible = false;
    assert(!gui_apply_accessibility_focus(g_cur, &event));
    tb->ax_visible = true; event.edit.cursor = 0;
    assert(gui_apply_accessibility_focus(g_cur, &event) && g_cur->focused == 0);
    g_cur->nav_enabled = previous_navigation;
    hjpCancelFrame(g_cur->vg);
}

static int cancel_notifications;
static Value cancellation_observe(Value *callback, int argc, Value *argv) {
    (void)callback;
    assert(argc == 1 && argv[0].type == VALUE_DICT);
    assert(!strcmp(gui_dict_lookup(argv[0], "状態").string.data, "取消"));
    assert(gui_dict_lookup(argv[0], "終了コード").number == 128 + SIGKILL);
    assert(gui_dict_lookup(argv[0], "進捗").number == -1);
    cancel_notifications++;
    return hajimu_null();
}

static void check_task_cancellation(void) {
    char *args[] = {"/bin/sleep", "10", NULL};
    int id = hjp_task_start_with_io(g_cur, args, true, true);
    HjpTask *task = hjp_task_find(id, g_cur);
    assert(task);
    deferred_pid = task->pid; deferred_polls = 50;
    Value argument = hajimu_number(id);
    assert(fn_task_cancel(1, &argument).boolean);
    assert(task->state == HJP_TASK_CANCELLING && task->pid == deferred_pid && deferred_polls == 49);
    assert(fn_task_progress(1, &argument).number == -1);
    Value state = fn_task_state(1, &argument);
    assert(!strcmp(gui_dict_lookup(state, "状態").string.data, "取消中"));
    assert(!fn_task_release(1, &argument).boolean);
    HajimuRuntime *previous = __hajimu_runtime;
    HajimuRuntime runtime = {.call = cancellation_observe};
    __hajimu_runtime = &runtime;
    Value notify[] = {argument, {.type = VALUE_FUNCTION}};
    for (int i = 0; i < 5; i++) {
        frame(false, false, 500);
        assert(!fn_task_notify(2, notify).boolean);
        hjpCancelFrame(g_cur->vg);
    }
    assert(cancel_notifications == 0 && task->pid == deferred_pid && deferred_polls > 0);
    deferred_polls = 0;
    for (int i = 0; i < 1000 && task->pid; i++) {
        hjp_task_poll(task);
        if (task->pid) hjp_delay(1);
    }
    assert(!task->pid && task->state == HJP_TASK_CANCELLED);
    assert(fn_task_notify(2, notify).boolean && cancel_notifications == 1);
    assert(!fn_task_notify(2, notify).boolean);
    assert(fn_task_release(1, &argument).boolean);
    assert(waitpid(deferred_pid, NULL, WNOHANG) == -1 && errno == ECHILD);
    __hajimu_runtime = previous;
}

static int close_callback_tag, close_guard_tag, close_timer_tag;
static int closing_callbacks, closing_guards, closing_timers, closing_task_id, closing_history_slot;
static bool fail_first_close;
static Value closing_app;
static Value lifecycle_observe(Value *callback, int argc, Value *argv) {
    (void)argv;
    assert(argc == 0);
    if (callback->function.definition == (struct ASTNode *)&close_guard_tag) {
        closing_guards++;
        return hajimu_bool(true);
    }
    if (callback->function.definition == (struct ASTNode *)&close_timer_tag) {
        closing_timers++;
        return hajimu_null();
    }
    assert(callback->function.definition == (struct ASTNode *)&close_callback_tag);
    closing_callbacks++;
    if (closing_callbacks == 1) {
        Value reentry[] = {closing_app, *callback};
        assert(fn_draw_loop(2, reentry).type == VALUE_NULL && closing_callbacks == 1);
        Value initial[] = {hajimu_string("keep history until shutdown completes")};
        closing_history_slot = gui_history_index(fn_history_create(1, initial));
        assert(closing_history_slot >= 0);
        char *args[] = {"/bin/sleep", "10", NULL};
        closing_task_id = hjp_task_start_with_io(g_cur, args, true, true);
        HjpTask *task = hjp_task_find(closing_task_id, g_cur);
        assert(task);
        deferred_pid = task->pid; deferred_polls = 30;
        if (fail_first_close) control_kill_error = EPERM;
    } else {
        assert(fail_first_close && closing_callbacks == 2);
        assert(g_cur->running && !g_cur->closing && g_cur->close_error == EPERM);
        assert(hjp_task_find(closing_task_id, g_cur)->pid == deferred_pid);
        assert(g_histories[closing_history_slot].id != 0);
        control_kill_error = 0;
    }
    gui_ime_update(g_cur, "仮");
    assert(fn_request_close(1, &closing_app).boolean);
    assert(g_cur->closing && g_cur->loop_active && g_cur->vg && g_cur->window);
    int guards = closing_guards;
    assert(fn_request_close(1, &closing_app).boolean && closing_guards == guards);
    fn_app_quit(1, &closing_app);
    assert(g_cur->valid && g_cur->window);
    assert(!fn_ime_composition(1, callback).boolean);
    Value configuration = gui_dict2("実行ファイル", hajimu_string("/bin/true"), "引数",
        (Value){.type = VALUE_ARRAY});
    Value spawn[] = {configuration, hajimu_null()};
    assert(fn_async_exec(2, spawn).number == -1);
    return hajimu_null();
}

static void check_closing_loop(void) {
    GuiApp *original = g_cur;
    HajimuRuntime *previous = __hajimu_runtime;
    HajimuRuntime runtime = {.call = lifecycle_observe};
    GuiTimer original_timer = g_timers[0];
    for (int failure = 0; failure < 2; failure++) {
        fail_first_close = failure != 0;
        closing_callbacks = closing_guards = closing_timers = 0;
        Value create[] = {hajimu_string("Closing lifecycle regression"), hajimu_number(480), hajimu_number(240)};
        closing_app = fn_app_create(3, create);
        GuiApp *app = &g_apps[(int)closing_app.number];
        closing_capture_app = failure ? NULL : app;
        app->target_fps = 120;
        app->close_guard = (Value){.type = VALUE_FUNCTION, .function.definition = (struct ASTNode *)&close_guard_tag};
        g_timers[0].active = true; g_timers[0].interval_ms = 0;
        g_timers[0].callback = (Value){.type = VALUE_FUNCTION, .function.definition = (struct ASTNode *)&close_timer_tag};
        __hajimu_runtime = &runtime;
        Value draw[] = {closing_app, {.type = VALUE_FUNCTION, .function.definition = (struct ASTNode *)&close_callback_tag}};
        g_cur = NULL;
        hjp_gl_make_current(original->window, original->gl);
        fn_draw_loop(2, draw);
        assert(!app->loop_active && !app->running && !app->closing && app->valid);
        assert(app->close_frames >= 5 && closing_callbacks == (failure ? 2 : 1));
        assert(closing_guards == closing_callbacks && closing_timers == 0);
        assert(!hjp_task_find(closing_task_id, app));
        assert(g_histories[closing_history_slot].id == 0);
        assert(waitpid(deferred_pid, NULL, WNOHANG) == -1 && errno == ECHILD);
        Value state = fn_close_state(1, &closing_app);
        assert(!strcmp(gui_dict_lookup(state, "状態").string.data, "停止"));
        assert(gui_dict_lookup(state, "終了待機フレーム").number >= 5);
        assert(gui_dict_lookup(state, "未回収タスク").number == 0);
        fn_app_quit(1, &closing_app);
        assert(!app->valid);
    }
    const char *capture = getenv("GUI_CLOSE_CAPTURE");
    if (capture) {
        assert(closing_capture_requested);
        FILE *file = fopen(capture, "rb");
        assert(file);
        char line[128]; int width, height;
        assert(fgets(line, sizeof(line), file) && !strcmp(line, "P6\n"));
        assert(fgets(line, sizeof(line), file) && sscanf(line, "%d %d", &width, &height) == 2);
        assert(width > 0 && height > 0 && width < 10000 && height < 10000);
        assert(fgets(line, sizeof(line), file) && !strcmp(line, "255\n"));
        size_t size = (size_t)width * height * 3;
        unsigned char *pixels = malloc(size);
        assert(pixels && fread(pixels, 1, size, file) == size);
        int contrast = 0;
        for (int y = height / 3; y < height * 2 / 3; y++) for (int x = width / 4; x < width * 3 / 4; x++) {
            size_t p = ((size_t)y * width + x) * 3;
            if (abs((int)pixels[p] - pixels[0]) + abs((int)pixels[p + 1] - pixels[1]) +
                abs((int)pixels[p + 2] - pixels[2]) > 120) contrast++;
        }
        assert(contrast > 50);
        free(pixels); fclose(file);
    }
    closing_capture_app = NULL;
    g_timers[0] = original_timer;
    __hajimu_runtime = previous;
    g_cur = original;
    hjp_gl_make_current(original->window, original->gl);
}

static HjpEvent source_event(GuiApp *app, int type) {
    HjpEvent event = {.type = type};
    event.source.window = app->window;
    event.source.generation = hjp_window_get_id(app->window);
    return event;
}
static void check_window_routing(void) {
    GuiApp *original = g_cur;
    hjp_event_queue_clear(&original->events);
    Value create[] = {hajimu_string("Routing regression"), hajimu_number(400), hajimu_number(240)};
    Value other_id = fn_app_create(3, create);
    GuiApp *other = &g_apps[(int)other_id.number];
    HjpEvent event = source_event(other, HJP_EVENT_KEYDOWN), result;
    /* ウィンドウ生成で発生した実OSのフォーカス通知と、注入する列を分ける。 */
    while (hjp_poll_event(&result)) hjp_event_dispose(&result);
    event.key.sym = HJPK_TAB;
    assert(!gui_route_event(original, &event) && other->events.count == 1);
    event = source_event(other, HJP_EVENT_TEXTEDITING);
    strcpy(event.text.text, "に");
    assert(!gui_route_event(original, &event));
    event = source_event(other, HJP_EVENT_DROPFILE); event.drop.file = strdup("owned-route.jp");
    assert(!gui_route_event(original, &event));
    event = source_event(other, HJP_EVENT_QUIT);
    assert(!gui_route_event(original, &event) && original->running && !original->os_close_pending);
    assert(gui_poll_event(other, &result) && result.type == HJP_EVENT_KEYDOWN && result.key.sym == HJPK_TAB);
    assert(gui_poll_event(other, &result) && result.type == HJP_EVENT_TEXTEDITING && !strcmp(result.text.text, "に"));
    assert(gui_poll_event(other, &result) && result.type == HJP_EVENT_DROPFILE);
    hjp_event_dispose(&result);
    assert(gui_poll_event(other, &result) && result.type == HJP_EVENT_QUIT);
    uint64_t rejected = original->rejected_events;
    event = source_event(other, HJP_EVENT_DROPFILE); event.source.generation++;
    event.drop.file = strdup("stale-route.jp");
    assert(!gui_route_event(original, &event) && !event.drop.file);
    assert(original->rejected_events == rejected + 1);
    event = source_event(other, HJP_EVENT_TEXTREPLACE);
    event.edit.window = original->window; event.edit.generation = hjp_window_get_id(original->window);
    assert(!gui_route_event(original, &event) && other->events.count == 0);
    event = (HjpEvent){.type = HJP_EVENT_KEYDOWN};
    assert(!gui_route_event(original, &event));
    HjpEvent stale = source_event(other, HJP_EVENT_KEYDOWN);
    event = source_event(other, HJP_EVENT_DROPFILE); event.drop.file = strdup("clear-on-destroy.jp");
    assert(!gui_route_event(original, &event));
    fn_app_quit(1, &other_id);
    assert(!other->events.count);
    other_id = fn_app_create(3, create);
    other = &g_apps[(int)other_id.number];
    assert(!gui_route_event(original, &stale));
    fn_app_quit(1, &other_id);
    g_cur = original;
    hjp_gl_make_current(original->window, original->gl);
}

static Value modal_list(const char *first, const char *second) {
    Value array = hajimu_array();
    if (first) hajimu_array_push(&array, hajimu_string(first));
    if (second) hajimu_array_push(&array, hajimu_string(second));
    return array;
}
static void check_modal_stack(void) {
    frame(false, false, 500); gui_nav_begin();
    Value none = modal_list(NULL, NULL), parent = modal_list("parent", NULL), both = modal_list("parent", "child");
    Value bad = modal_list("parent", "parent");
    Value limit = hajimu_array();
    for (int i = 0; i < GUI_MAX_MODALS; i++) {
        char title[32]; snprintf(title, sizeof(title), "modal-%d", i);
        hajimu_array_push(&limit, hajimu_string(title));
    }
    assert(fn_modal_reserve(1, &limit).boolean && g_cur->modal_count == GUI_MAX_MODALS);
    hajimu_array_push(&limit, hajimu_string("overflow"));
    assert(!fn_modal_reserve(1, &limit).boolean && g_cur->modal_count == GUI_MAX_MODALS);
    Value empty_name = modal_list("", NULL), wrong_type = hajimu_number(5);
    assert(!fn_modal_reserve(1, &empty_name).boolean && !fn_modal_reserve(1, &wrong_type).boolean);
    char long_name[257]; memset(long_name, 'a', 256); long_name[256] = '\0';
    Value too_long = hajimu_string(long_name);
    assert(!fn_modal_reserve(1, &too_long).boolean);
    fn_modal_reserve(1, &none);
    g_cur->nav_focus = 801; g_cur->focused = 802;
    assert(fn_modal_reserve(1, &parent).boolean && !g_cur->focused && !g_cur->nav_focus);
    g_cur->nav_focus = 901; g_cur->focused = 902;
    assert(fn_modal_reserve(1, &both).boolean && !g_cur->focused);
    assert(!fn_modal_reserve(1, &bad).boolean && g_cur->modal_count == 2);
    Value snapshot = fn_modal_stack(0, NULL);
    assert(snapshot.array.length == 2 && !strcmp(snapshot.array.elements[0].string.data, "parent"));
    assert(!strcmp(snapshot.array.elements[1].string.data, "child"));
    hajimu_array_push(&snapshot, hajimu_string("independent"));
    assert(g_cur->modal_count == 2 && snapshot.array.length == 3);
    assert(fn_modal_reserve(1, &parent).boolean && g_cur->nav_focus == 901 && g_cur->focused == 902);
    assert(fn_modal_reserve(1, &none).boolean && g_cur->nav_focus == 801 && g_cur->focused == 802);
    gui_nav_end();
    assert(g_cur->focused == 802); /* 復帰したフレームでは早過ぎる非表示判定をしない。 */
    frame(false, false, 500); gui_nav_begin(); gui_nav_end();
    assert(!g_cur->focused); /* 次フレームにも存在しなければ解除する。 */

    Value origin_args[] = {hajimu_string("modal-origin"), hajimu_string("a日本")};
    GuiTextBuf *origin = gui_get_text_buf(gui_hash("modal-origin"), "a日本");
    gui_text_collapse(origin, 1); g_cur->focused = origin->id;
    origin->ax_visible = true;
    fn_modal_reserve(1, &parent); assert(!origin->ax_visible);
    fn_modal_reserve(1, &none);
    frame(false, false, 500); gui_nav_begin(); fn_text_input(2, origin_args); gui_nav_end();
    assert(g_cur->focused == origin->id && origin->cursor == 1);
    g_cur->focused = 0;

    frame(false, false, 500); gui_nav_begin();
    assert(fn_modal_reserve(1, &both).boolean);
    gui_nav_end();
    frame(false, false, 500); gui_nav_begin();
    g_cur->in.key_return = true;
    fn_modal_reserve(1, &both);
    Value outer[] = {hajimu_string("outer-modal-clip"), hajimu_number(100), hajimu_number(80), hajimu_bool(true)};
    assert(fn_child_begin(4, outer).boolean);
    GuiLayout original = g_cur->lay;
    int render_depth = hjpStateDepth(g_cur->vg);
    Value parent_args[] = {hajimu_string("parent"), hajimu_bool(true)};
    Value child_args[] = {hajimu_string("child"), hajimu_bool(true)};
    assert(fn_dialog_begin(2, parent_args).boolean && !g_cur->modal_entered);
    GuiLayout parent_layout = g_cur->lay;
    g_cur->in.clicked = true; g_cur->in.mx = 400; g_cur->in.my = 200;
    bool hovered, pressed;
    assert(!gui_widget_logic(gui_hash("blocked"), 380, 180, 60, 40, &hovered, &pressed));
    assert(!hovered && !pressed);
    assert(fn_dialog_begin(2, child_args).boolean && g_cur->modal_entered && g_cur->in.key_return);
    uint32_t child_id = gui_hash("ok");
    g_cur->in.accessibility_press = child_id;
    assert(gui_widget_logic(child_id, 380, 180, 60, 40, &hovered, &pressed));
    assert(!gui_widget_logic(child_id, 380, 180, 60, 40, &hovered, &pressed));
    g_cur->in.accessibility_press = gui_hash("outside");
    assert(!gui_widget_logic(gui_hash("outside"), 20, 20, 30, 30, &hovered, &pressed));
    assert(fn_dialog_end(0, NULL).boolean && !g_cur->modal_entered && !g_cur->in.key_return);
    assert(!memcmp(&g_cur->lay, &parent_layout, sizeof(parent_layout)));
    assert(!fn_dialog_begin(2, child_args).boolean); /* 同フレームの入力再配送を拒否する。 */
    assert(fn_dialog_end(0, NULL).boolean && hjpStateDepth(g_cur->vg) == render_depth);
    assert(!memcmp(&g_cur->lay, &original, sizeof(original)) && g_scrolls[0].active);
    assert(!fn_dialog_end(0, NULL).boolean);
    fn_child_end(0, NULL);
    fn_modal_reserve(1, &none); gui_nav_end();

    frame(false, false, 500); gui_nav_begin();
    fn_modal_reserve(1, &parent);
    render_depth = hjpStateDepth(g_cur->vg);
    assert(fn_dialog_begin(2, parent_args).boolean);
    assert(fn_child_begin(4, outer).boolean);
    assert(!fn_dialog_end(0, NULL).boolean && hjpStateDepth(g_cur->vg) == render_depth);
    assert(!g_scrolls[0].active && !g_dialog_depth[g_cur - g_apps]);
    fn_modal_reserve(1, &none); gui_nav_end();

    frame(false, false, 500); gui_nav_begin(); fn_modal_reserve(1, &parent);
    render_depth = hjpStateDepth(g_cur->vg);
    while (hjpStateDepth(g_cur->vg) < 64) hjpSave(g_cur->vg);
    assert(!fn_dialog_begin(2, parent_args).boolean && !g_cur->modals[0].drawn);
    assert(hjpRestoreStateDepth(g_cur->vg, render_depth));
    assert(fn_dialog_begin(2, parent_args).boolean);
    gui_nav_end(); /* 終了呼出の省略も次フレームへ状態を残さない。 */
    assert(!g_dialog_depth[g_cur - g_apps] && hjpStateDepth(g_cur->vg) == render_depth);
    fn_modal_reserve(1, &none);
    g_cur->nav_focus = g_cur->focused = 0;
    frame(false, false, 500); gui_nav_begin(); fn_modal_reserve(1, &parent);
    assert(fn_dialog_begin(2, parent_args).boolean);
    g_scrolls[0].scroll_y = 55;
    Value changed = modal_list("changed", NULL);
    fn_modal_reserve(1, &changed);
    assert(fn_dialog_end(0, NULL).boolean);
    assert(g_dialog_scrolls[g_cur - g_apps][0][0].scroll_y == 0);
    fn_modal_reserve(1, &none); gui_nav_end();
    frame(false, false, 500); gui_nav_begin();
    g_cur->in.key_return = true;
    fn_disable_begin(0, NULL);
    fn_modal_reserve(1, &parent);
    assert(fn_dialog_begin(2, parent_args).boolean && !g_disabled);
    assert(fn_dialog_end(0, NULL).boolean && g_disabled);
    fn_modal_reserve(1, &none);
    fn_disable_end(0, NULL);
    assert(!g_cur->in.key_return && !g_disabled);
    gui_nav_end();
    hjpCancelFrame(g_cur->vg);
}

static void check_text_block_layout(void) {
    frame(false, false, 500);
    Value options = make_dict();
    dict_set(&options, "幅", hajimu_number(90));
    dict_set(&options, "サイズ", hajimu_number(20));
    dict_set(&options, "行高さ", hajimu_number(32));
    Value args[] = {hajimu_string("説明文は内容に応じて折り返します。\r\n次の行\n"), options};
    GuiLayout saved = g_cur->lay;
    GuiLastWidget last = g_cur->last;
    g_next_widget_w = 171;
    int depth = hjpStateDepth(g_cur->vg);
    float font_line_before, font_line_after;
    hjpTextMetrics(g_cur->vg, NULL, NULL, &font_line_before);
    Value result = fn_text_measure(2, args);
    hjpTextMetrics(g_cur->vg, NULL, NULL, &font_line_after);
    assert(font_line_before == font_line_after);
    double height = gui_dict_lookup(result, "高さ").number;
    double lines = gui_dict_lookup(result, "行数").number;
    assert(lines > 3 && height == lines * 32);
    assert(!memcmp(&saved, &g_cur->lay, sizeof(saved)) && !memcmp(&last, &g_cur->last, sizeof(last)));
    assert(g_next_widget_w == 171 && hjpStateDepth(g_cur->vg) == depth);
    fn_text(2, args);
    assert(g_cur->last.w == 90 && g_cur->last.h == height);
    assert(fabs(g_cur->lay.y - height - GUI_MARGIN) < 0.01);
    float following = g_cur->lay.y;
    fn_text_wrapped(2, args);
    assert(g_cur->last.h == height && g_cur->last.y == following);
    args[0] = hajimu_string(""); result = fn_text_measure(2, args);
    assert(gui_dict_lookup(result, "行数").number == 1 && gui_dict_lookup(result, "高さ").number == 32);
    args[0] = hajimu_string("A\r\n\r\n"); result = fn_text_measure(2, args);
    assert(gui_dict_lookup(result, "行数").number == 3);
    char many[262];
    for (int i = 0; i < 130; i++) { many[i * 2] = 'a'; many[i * 2 + 1] = '\n'; }
    many[260] = 0;
    args[0] = hajimu_string(many); result = fn_text_measure(2, args);
    assert(gui_dict_lookup(result, "行数").number == 131);
    args[0] = hajimu_string("e\xcc\x81" "\xf0\x9f\x87\xaf\xf0\x9f\x87\xb5");
    dict_set(&options, "幅", hajimu_number(1)); args[1] = options;
    result = fn_text_measure(2, args);
    assert(gui_dict_lookup(result, "行数").number == 2);
    dict_set(&options, "幅", hajimu_number(NAN)); args[1] = options;
    saved = g_cur->lay; g_next_widget_w = 171;
    assert(fn_text_measure(2, args).type == VALUE_NULL);
    dict_set(&options, "幅", hajimu_number(90));
    dict_set(&options, "色", hajimu_number(1e100)); args[1] = options;
    assert(fn_text_measure(2, args).type == VALUE_NULL);
    fn_text_wrapped(2, args);
    assert(!memcmp(&saved, &g_cur->lay, sizeof(saved)) && g_next_widget_w == 171);
    assert(hjpStateDepth(g_cur->vg) == depth);
    hjpCancelFrame(g_cur->vg);
}

static void check_responsive_fields(void) {
    float original_font = g_custom_font_size;
    bool original_nav = g_cur->nav_enabled;
    g_cur->nav_enabled = false;
    Value choices = modal_list("A", "B");
    const float widths[] = {40, 120, 300, 600};
    const float sizes[] = {16, 28, 48};
    for (int font = 0; font < 3; font++) for (int width = 0; width < 4; width++) {
        g_custom_font_size = sizes[font];
        for (int control = 0; control < 4; control++) {
            frame(false, false, 800); g_cur->lay.w = widths[width];
            Value label = hajimu_string("幅に合わせて折り返す長いラベル");
            Value text[] = {label, hajimu_string("値")};
            Value number[] = {label, hajimu_number(1)};
            Value combo[] = {label, choices, hajimu_string("A")};
            if (control == 0) fn_text_input(2, text);
            if (control == 1) fn_password_input(2, text);
            if (control == 2) fn_number_input(2, number);
            if (control == 3) fn_combobox(3, combo);
            assert(g_cur->last.x >= 0 && g_cur->last.w > 0);
            assert(g_cur->last.x + g_cur->last.w <= widths[width] + 0.01f);
            assert(g_cur->last.h >= sizes[font] * 1.4f);
            assert(g_cur->lay.y >= g_cur->last.y + g_cur->last.h);
            hjpCancelFrame(g_cur->vg);
        }
    }
    g_custom_font_size = 0;
    frame(false, false, 800); g_cur->lay.w = 600;
    GuiFieldGeometry multiline;
    assert(gui_form_field("name\nrequired", 80, &multiline));
    assert(multiline.y >= GUI_FONT_SIZE * 2.8f && multiline.width > 500);
    Value number[] = {hajimu_string("count"), hajimu_number(1), hajimu_number(0), hajimu_number(10), hajimu_number(1)};
    frame(false, false, 150); g_cur->in.scroll_y = 2; g_disabled = true;
    assert(fn_number_input(5, number).number == 1 && g_cur->in.scroll_y == 2); g_disabled = false;
    frame(false, false, 150); g_cur->in.scroll_y = 2;
    Value scrolled = fn_number_input(5, number);
    if (scrolled.number != 3 || g_cur->in.scroll_y)
        fprintf(stderr, "number wheel: value=%g wheel=%d field=%g,%g,%g,%g modal=%s disabled=%d closing=%d ime=%s\n",
                scrolled.number, g_cur->in.scroll_y, g_cur->last.x, g_cur->last.y, g_cur->last.w, g_cur->last.h,
                g_cur->modal_title, g_disabled, g_cur->closing, g_cur->composition);
    assert(scrolled.number == 3 && !g_cur->in.scroll_y);
    frame(false, false, 800); g_cur->nav_enabled = true;
    g_cur->nav_focus = gui_hash("count") ^ 0xC3D4; g_cur->focused = 0; g_cur->in.key_return = true;
    assert(fn_number_input(5, number).number == 2 && !g_cur->in.key_return);
    g_cur->nav_enabled = false;
    frame(false, false, 800); number[1] = hajimu_number(NAN);
    assert(isnan(fn_number_input(5, number).number) && g_cur->lay.y == 0);
    number[1] = hajimu_number(1); number[4] = hajimu_number(0);
    assert(fn_number_input(5, number).number == 1 && g_cur->lay.y == 0);

    Value combo[] = {hajimu_string("choice"), choices, hajimu_string("A")};
    frame(true, false, 150); fn_combobox(3, combo);
    assert(g_cur->combo_open == gui_hash("choice") && !g_cur->in.clicked);
    GuiApp *original_app = g_cur;
    Value app_args[] = {hajimu_string("Separate choice window"), hajimu_number(320), hajimu_number(240)};
    Value other = fn_app_create(3, app_args);
    assert(other.type == VALUE_NUMBER);
    g_cur = &g_apps[(int)other.number];
    hjp_gl_make_current(g_cur->window, g_cur->gl);
    frame(false, false, 800); fn_combobox(3, combo); assert(!g_cur->combo_open);
    hjpCancelFrame(g_cur->vg);
    fn_app_quit(1, &other);
    g_cur = original_app;
    hjp_gl_make_current(g_cur->window, g_cur->gl);
    assert(g_cur->combo_open == gui_hash("choice"));
    frame(false, false, 150); g_disabled = true; fn_combobox(3, combo); g_disabled = false;
    assert(!g_cur->combo_open);
    frame(true, false, 150); gui_nav_begin(); fn_combobox(3, combo); gui_nav_end();
    assert(g_cur->combo_open == gui_hash("choice"));
    frame(false, false, 800); gui_nav_begin(); gui_nav_end(); assert(!g_cur->combo_open);
    frame(true, false, 150); fn_combobox(3, combo);
    Value modal = modal_list("new-modal", NULL), none = modal_list(NULL, NULL);
    fn_modal_reserve(1, &modal); assert(!g_cur->combo_open); fn_modal_reserve(1, &none);
    Value valid_font = hajimu_number(28), invalid_font = hajimu_number(INFINITY);
    fn_font_size_set(1, &valid_font); fn_font_size_set(1, &invalid_font);
    assert(g_custom_font_size == 28);
    g_custom_font_size = original_font; g_cur->nav_enabled = original_nav;
    hjpCancelFrame(g_cur->vg);
}

static void check_toggle_slider_layout(void) {
    float original_font = g_custom_font_size;
    bool original_nav = g_cur->nav_enabled;
    g_cur->nav_enabled = false;
    const float widths[] = {1, 40, 120, 300, 600}, sizes[] = {16, 28, 48};
    for (int font = 0; font < 3; font++) for (int width = 0; width < 5; width++) {
        g_custom_font_size = sizes[font];
        for (int control = 0; control < 3; control++) {
            frame(false, false, 800); g_cur->lay.w = widths[width];
            GuiLastWidget before = g_cur->last;
            Value label = hajimu_string("長い説明文も入力欄の幅に合わせて表示します");
            Value check[] = {label, hajimu_bool(true)};
            Value radio[] = {label, hajimu_string("group"), hajimu_string("other")};
            Value slider[] = {label, hajimu_number(50), hajimu_number(0), hajimu_number(100)};
            if (control == 0) assert(fn_checkbox(2, check).boolean);
            if (control == 1) assert(!strcmp(fn_radio(3, radio).string.data, "other"));
            if (control == 2) assert(fn_slider(4, slider).number == 50);
            if (widths[width] <= GUI_PADDING * 2) {
                assert(g_cur->lay.y == 0 && !memcmp(&before, &g_cur->last, sizeof(before)));
                hjpCancelFrame(g_cur->vg);
                continue;
            }
            assert(g_cur->last.x >= 0 && g_cur->last.w >= 0);
            assert(g_cur->last.x + g_cur->last.w <= widths[width] + 0.01f);
            assert(g_cur->lay.y >= g_cur->last.y + g_cur->last.h);
            hjpCancelFrame(g_cur->vg);
        }
    }
    g_custom_font_size = 0;
    Value slider[] = {hajimu_string("volume"), hajimu_number(50), hajimu_number(0), hajimu_number(100)};
    frame(false, false, 800); g_cur->nav_enabled = true; g_cur->nav_focus = gui_hash("volume"); g_cur->focused = 0;
    g_cur->in.key_right = true;
    assert(fn_slider(4, slider).number == 51 && !g_cur->in.key_right);
    frame(false, false, 800); g_cur->in.key_home = true;
    assert(fn_slider(4, slider).number == 0 && !g_cur->in.key_home);
    frame(false, false, 800); g_cur->in.key_end = true;
    assert(fn_slider(4, slider).number == 100 && !g_cur->in.key_end);
    frame(true, false, 600); g_cur->active = gui_hash("volume"); g_disabled = true;
    assert(fn_slider(4, slider).number == 50 && !g_cur->active); g_disabled = false;
    frame(true, false, 600); g_cur->active = gui_hash("volume"); strcpy(g_cur->composition, "a");
    assert(fn_slider(4, slider).number == 50 && !g_cur->active); g_cur->composition[0] = 0;
    frame(false, false, 800); slider[1] = hajimu_number(NAN);
    assert(isnan(fn_slider(4, slider).number) && g_cur->lay.y == 0);
    slider[1] = hajimu_number(50); slider[3] = hajimu_number(0);
    assert(fn_slider(4, slider).number == 50 && g_cur->lay.y == 0);
    g_cur->active = 0; g_custom_font_size = original_font; g_cur->nav_enabled = original_nav;
    hjpCancelFrame(g_cur->vg);
}

static void check_modal_forms(void) {
    frame(false, false, 500); gui_nav_begin();
    Value parent = modal_list("form", NULL), none = modal_list(NULL, NULL);
    Value options = make_dict(); dict_set(&options, "幅", hajimu_number(600)); dict_set(&options, "高さ", hajimu_number(340));
    Value args[] = {hajimu_string("form"), hajimu_bool(true), options};
    int baseline = g_cur->text_buf_count;
    for (int i = 0; i < 200; i++) {
        assert(fn_modal_reserve(1, &parent).boolean);
        assert(fn_dialog_begin(3, args).boolean);
        GuiTextBuf *tb = gui_get_text_buf(gui_hash("value"), i % 2 ? "second" : "first");
        assert(tb && !strcmp(tb->buf, i % 2 ? "second" : "first") && tb->modal_epoch != 0);
        assert(fn_dialog_end(0, NULL).boolean);
        assert(fn_modal_reserve(1, &none).boolean && g_cur->text_buf_count == baseline);
        hjpCancelFrame(g_cur->vg);
    }
    uint64_t serial = g_cur->modal_epoch_counter;
    g_cur->modal_epoch_counter = UINT64_MAX;
    assert(!fn_modal_reserve(1, &parent).boolean && !g_cur->modal_count);
    g_cur->modal_epoch_counter = serial;
    assert(fn_modal_reserve(1, &parent).boolean);
    int depth = hjpStateDepth(g_cur->vg);
    dict_set(&options, "幅", hajimu_number(NAN)); args[2] = options;
    assert(!fn_dialog_begin(3, args).boolean && hjpStateDepth(g_cur->vg) == depth);
    dict_set(&options, "幅", hajimu_number(1e20)); dict_set(&options, "高さ", hajimu_number(1e20)); args[2] = options;
    assert(fn_dialog_begin(3, args).boolean);
    GuiDialogScope *scope = &g_dialog_scopes[g_cur - g_apps][0];
    assert(scope->content_x >= 16 && scope->content_y >= 56);
    assert(scope->content_x + scope->content_w <= g_cur->win_w - 16);
    assert(scope->content_y + scope->content_h <= g_cur->win_h - 16);
    assert(fn_dialog_end(0, NULL).boolean);
    fn_modal_reserve(1, &none); gui_nav_end(); hjpCancelFrame(g_cur->vg);
}

static void check_form_scrolling(void) {
    frame(false, false, 500);
    Value parent[] = {hajimu_string("auto-scroll-parent"), hajimu_number(220), hajimu_number(120)};
    assert(fn_child_begin(3, parent).boolean);
    Value automatic[] = {hajimu_number(-1), hajimu_number(-1)};
    Value auto_slot = fn_scroll_begin(2, automatic);
    assert(auto_slot.number >= 0);
    GuiScrollRegion *auto_region = &g_scrolls[(int)auto_slot.number];
    assert(auto_region->y + auto_region->h <= g_scrolls[0].y + g_scrolls[0].h);
    fn_scroll_end(1, &auto_slot); fn_child_end(0, NULL);
    frame(false, false, 500);
    Value dimensions[] = {hajimu_number(220), hajimu_number(100)};
    Value slot = fn_scroll_begin(2, dimensions);
    assert(slot.number >= 0);
    GuiScrollRegion *region = &g_scrolls[(int)slot.number];
    region->scroll_y = 0; g_cur->lay.y = region->y + 500;
    g_cur->in.mx = (int)region->x + 10; g_cur->in.my = (int)region->y + 10; g_cur->in.scroll_y = -1;
    fn_scroll_end(1, &slot);
    assert(region->scroll_y == 30 && !g_cur->in.scroll_y);
    frame(false, false, 500); slot = fn_scroll_begin(2, dimensions);
    g_cur->lay.y = region->y + 500 - region->scroll_y;
    g_cur->in.mx = (int)(region->x + region->w - 5); g_cur->in.my = (int)region->y + 90;
    g_cur->in.down = g_cur->in.clicked = true;
    fn_scroll_end(1, &slot);
    assert(region->scroll_y == 400 && !g_cur->in.clicked);
    frame(false, false, 500); slot = fn_scroll_begin(2, dimensions);
    g_cur->lay.y = region->y + 20 - region->scroll_y;
    fn_scroll_end(1, &slot);
    assert(region->scroll_y == 0 && !g_cur->active);
    frame(false, false, 500); slot = fn_scroll_begin(2, dimensions);
    region->scroll_y = 200; g_cur->lay.y = region->y + 300;
    g_cur->in.mx = (int)(region->x + region->w - 5);
    g_cur->in.my = (int)region->y + 43;
    g_cur->in.down = g_cur->in.clicked = true;
    fn_scroll_end(1, &slot);
    assert(region->scroll_y == 200 && region->drag_offset == 3);
    frame(false, false, 500); slot = fn_scroll_begin(2, dimensions);
    g_cur->lay.y = region->y + 300;
    g_cur->in.mx = (int)(region->x + region->w - 5);
    g_cur->in.my = (int)region->y + 53; g_cur->in.down = true;
    fn_scroll_end(1, &slot);
    assert(region->scroll_y == 250);
    g_cur->active = 0;
    frame(false, false, 500); slot = fn_scroll_begin(2, dimensions);
    g_cur->lay.y = region->y + 250;
    g_cur->in.mx = (int)region->x + 10; g_cur->in.my = (int)region->y + 10;
    g_cur->in.scroll_y = -1; g_disabled = true;
    fn_scroll_end(1, &slot); g_disabled = false;
    assert(region->scroll_y == 250 && g_cur->in.scroll_y == -1);
    Value bad = hajimu_number(NAN); fn_scroll_end(1, &bad);
    dimensions[0] = bad; assert(fn_scroll_begin(2, dimensions).number == -1);
    hjpCancelFrame(g_cur->vg);
}

static int event_callback_tag, event_guard_tag, event_callbacks, event_guards;
static bool event_model_committed, event_overflow;
static Value window_event_observe(Value *callback, int argc, Value *argv) {
    (void)argv; assert(argc == 0);
    if (callback->function.definition == (struct ASTNode *)&event_guard_tag) {
        assert(event_model_committed);
        event_guards++; return hajimu_bool(true);
    }
    assert(callback->function.definition == (struct ASTNode *)&event_callback_tag);
    Value args[] = {hajimu_string("event-field"), hajimu_string("before")};
    Value text = fn_text_input(2, args);
    assert(!strcmp(text.string.data, event_overflow ? "before" : "beforeX"));
    if (!event_overflow) assert(!g_cur->in.down && !g_cur->active && !g_cur->text_drag);
    else assert(!g_cur->in.down && !g_cur->active && !g_cur->text_drag && !g_cur->focused && !g_cur->composition[0]);
    event_callbacks++; event_model_committed = true;
    return hajimu_null();
}
static void check_window_close_input(void) {
    GuiApp *original = g_cur;
    HajimuRuntime *previous = __hajimu_runtime;
    HajimuRuntime runtime = {.call = window_event_observe};
    for (int overflow = 0; overflow < 3; overflow++) {
        event_overflow = overflow != 0;
        event_model_committed = false; event_callbacks = event_guards = 0;
        Value create[] = {hajimu_string("Input before close regression"), hajimu_number(400), hajimu_number(240)};
        Value app_id = fn_app_create(3, create);
        GuiApp *app = &g_apps[(int)app_id.number];
        HjpEvent discarded;
        while (hjp_poll_event(&discarded)) hjp_event_dispose(&discarded);
        g_cur = app;
        app->focused = gui_hash("event-field");
        gui_get_text_buf(app->focused, "before");
        app->in.down = true; app->active = 77; app->text_drag = 88;
        app->close_guard = (Value){.type = VALUE_FUNCTION, .function.definition = (struct ASTNode *)&event_guard_tag};
        HjpEvent event = source_event(app, HJP_EVENT_WINDOWEVENT);
        event.window.event = HJP_WINDOWEVENT_FOCUS_LOST;
        if (!overflow) hjp_event_queue_push(&app->events, &event);
        event = source_event(app, HJP_EVENT_TEXTINPUT); strcpy(event.text.text, "X");
        if (overflow == 2) {
            /* 古い別配送の編集も、OS側の破棄通知より先に適用してはならない。 */
            HjpEvent old_edit = source_event(app, HJP_EVENT_TEXTREPLACE);
            old_edit.edit.window = app->window; old_edit.edit.generation = hjp_window_get_id(app->window);
            old_edit.edit.id = app->focused; old_edit.edit.end = 6;
            strcpy(old_edit.edit.text, "BAD");
            hjp_event_queue_push(&app->events, &old_edit);
            for (int i = 0; i < HJP_NATIVE_EVENTS; i++) hjp_test_native_event(&event);
            event = source_event(app, HJP_EVENT_QUIT); hjp_test_native_event(&event);
        } else {
            for (int i = 0; i < (overflow ? HJP_DEFERRED_EVENTS + 1 : 1); i++) hjp_event_queue_push(&app->events, &event);
            event = source_event(app, HJP_EVENT_QUIT); hjp_event_queue_push(&app->events, &event);
        }
        Value draw[] = {app_id, {.type = VALUE_FUNCTION, .function.definition = (struct ASTNode *)&event_callback_tag}};
        g_cur = NULL; __hajimu_runtime = &runtime;
        fn_draw_loop(2, draw);
        assert(event_callbacks == 1 && event_guards == 1 && !app->running);
        if (overflow) {
            Value state = fn_input_state(1, &app_id);
            assert(gui_dict_lookup(state, "破棄入力数").number == (overflow == 2 ? 1 : HJP_DEFERRED_EVENTS + 1));
            assert(gui_dict_lookup(state, "OSキュー破棄入力数").number == (overflow == 2 ? HJP_NATIVE_EVENTS : 0));
            assert(!gui_dict_lookup(state, "入力リセット待ち").boolean);
        }
        fn_app_quit(1, &app_id);
    }
    __hajimu_runtime = previous;
    g_cur = original;
    hjp_gl_make_current(original->window, original->gl);
}

int main(void) {
    /* 仮想macOSのソフトウェアGLは2.1止まりの場合がある。実装失敗と区別する。 */
    if (getenv("CONTROL_ALLOW_UNSUPPORTED_GL")) {
        if (!hjp_init()) return 1;
        HjpWindow *probe = hjp_window_create("GL capability", 0, 0, 64, 64, HJP_WINDOW_OPENGL);
        if (!probe) return 1;
        HjpGLContext context = hjp_gl_create_context(probe);
        if (!context) { hjp_window_destroy(probe); return 1; }
        const char *version = (const char *)glGetString(GL_VERSION);
        int major = 0, minor = 0;
        bool parsed = version && sscanf(version, "%d.%d", &major, &minor) == 2;
        bool unsupported = parsed && (major < 3 || (major == 3 && minor < 2));
        fprintf(stderr, "native control GL capability: %s\n", version ? version : "unavailable");
        hjp_gl_delete_context(context);
        hjp_window_destroy(probe);
        hjp_quit();
        if (!parsed) return 1;
        if (unsupported) return 77;
    }
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
    hjp_window_set_size(g_cur->window, 620, 380);
    int client_width, client_height;
    hjp_window_get_size(g_cur->window, &client_width, &client_height);
    assert(client_width == 620 && client_height == 380);
    hjp_window_set_size(g_cur->window, 640, 400);
    check_ime_notifications();
    check_textarea_layout();
    check_text_formatting();
    check_text_selection();
    check_grapheme_editing();
    check_text_navigation();
    check_document_edits();
    check_accessible_text();
    check_task_cancellation();
    check_closing_loop();
    check_window_routing();
    check_window_close_input();
    check_modal_stack();
    check_modal_forms();
    check_responsive_fields();
    check_toggle_slider_layout();
    check_text_block_layout();
    check_form_scrolling();
    char oversized[GUI_MAX_TEXT_BUF + 8];
    memset(oversized, 'a', GUI_MAX_TEXT_BUF - 2);
    memcpy(oversized + GUI_MAX_TEXT_BUF - 2, "日本", 7);
    GuiTextBuf *limited = gui_get_text_buf(gui_hash("utf8-initial-limit"), oversized);
    assert(limited->len == GUI_MAX_TEXT_BUF - 2);
    assert(fn_focus_traversal(0, NULL).boolean);
    frame(false, false, 500);
    gui_nav_begin();
    gui_nav_control(101, 0, 0, 100, 30);
    gui_nav_control(102, 0, 40, 100, 30);
    gui_nav_end();
    frame(false, false, 500);
    g_cur->in.key_tab = true;
    gui_nav_begin();
    assert(g_cur->nav_focus == 101);
    g_cur->in.key_return = true;
    assert(gui_nav_control(101, 0, 0, 100, 30));
    assert(!gui_nav_control(101, 0, 0, 100, 30));
    gui_nav_control(102, 0, 40, 100, 30);
    gui_nav_end();
    frame(false, false, 500);
    g_cur->in.key_tab = true;
    g_cur->in.mod_shift = true;
    gui_nav_begin();
    assert(g_cur->nav_focus == 102);
    g_cur->in.last_key = ' ';
    assert(gui_nav_control(102, 0, 40, 100, 30));
    g_disabled = true;
    assert(!gui_nav_control(103, 0, 80, 100, 30));
    g_disabled = false;
    gui_nav_end();
    frame(false, false, 500);
    gui_nav_begin();
    gui_nav_control(101, 0, 0, 100, 30);
    gui_nav_end();
    assert(g_cur->nav_focus == 0);
    g_cur->nav_focus = 0;
    Value text[] = {hajimu_string("field"), hajimu_string("initial")};
    Value check[] = {hajimu_string("check"), hajimu_bool(false)};
    frame(false, false, 500);
    gui_nav_begin();
    fn_text_input(2, text);
    fn_checkbox(2, check);
    gui_nav_end();
    frame(false, false, 500);
    g_cur->in.key_tab = true;
    gui_nav_begin();
    fn_text_input(2, text);
    assert(g_cur->focused == gui_hash("field"));
    fn_checkbox(2, check);
    gui_nav_end();
    frame(false, false, 500);
    g_cur->in.key_tab = true;
    gui_nav_begin();
    fn_text_input(2, text);
    assert(g_cur->focused == 0);
    g_cur->in.last_key = ' ';
    assert(fn_checkbox(2, check).boolean);
    gui_nav_end();
    frame(false, false, 500);
    g_cur->in.key_tab = true;
    g_cur->in.mod_shift = true;
    gui_nav_begin();
    fn_text_input(2, text);
    assert(g_cur->focused == gui_hash("field"));
    gui_nav_end();
    frame(false, false, 500);
    gui_nav_begin();
    gui_nav_end();
    assert(g_cur->focused == 0);
    frame(true, false, 200);
    g_disabled = true;
    memcpy(g_cur->in.text_input, "bad", 4);
    g_cur->in.text_input_len = 3;
    Value unchanged = fn_text_input(2, text);
    assert(strcmp(unchanged.string.data, "initial") == 0);
    assert(g_cur->focused == 0);
    g_disabled = false;
    Value (*inputs[])(int, Value *) = {fn_password_input, fn_textarea};
    for (size_t input = 0; input < 2; input++) {
        frame(false, false, 500);
        g_cur->nav_focus = gui_hash("field");
        gui_nav_begin();
        inputs[input](2, text);
        assert(g_cur->focused == gui_hash("field"));
        gui_nav_end();
        frame(true, false, 200);
        g_disabled = true;
        memcpy(g_cur->in.text_input, "bad", 4);
        g_cur->in.text_input_len = 3;
        Value result = inputs[input](2, text);
        assert(strcmp(result.string.data, "initial") == 0);
        assert(g_cur->focused == 0);
        g_disabled = false;
    }
    frame(true, false, 30);
    gui_nav_begin();
    g_cur->nav_focus = 701;
    g_cur->active = 702;
    Value modal[] = {hajimu_string("modal")};
    Value wrong_dialog[] = {hajimu_string("other"), hajimu_bool(true)};
    Value dialog[] = {modal[0], hajimu_bool(true)};
    assert(fn_modal_reserve(1, modal).boolean);
    assert(!g_cur->in.clicked && g_cur->active == 0);
    assert(!gui_nav_control(701, 0, 0, 100, 30));
    assert(g_cur->nav_current_count == 0);
    assert(!fn_dialog(2, wrong_dialog).boolean);
    assert(fn_dialog(2, dialog).boolean);
    assert(!g_cur->in.clicked);
    assert(!gui_nav_control(703, 0, 0, 100, 30));
    gui_nav_end();
    frame(false, false, 30);
    g_cur->in.key_tab = true;
    gui_nav_begin();
    assert(g_cur->nav_focus == 703);
    g_cur->in.key_return = true;
    g_cur->in.mod_ctrl = true;
    fn_modal_reserve(1, modal);
    assert(!g_cur->in.key_return && !g_cur->in.mod_ctrl);
    assert(!gui_nav_control(701, 0, 0, 100, 30));
    fn_modal_reserve(1, modal);
    fn_dialog(2, dialog);
    assert(g_cur->in.key_return && g_cur->in.mod_ctrl);
    assert(gui_nav_control(703, 0, 0, 100, 30));
    gui_nav_end();
    frame(false, false, 30);
    gui_nav_begin();
    Value close_modal[] = {hajimu_string("")};
    fn_modal_reserve(1, close_modal);
    assert(g_cur->nav_focus == 701);
    gui_nav_control(701, 0, 0, 100, 30);
    gui_nav_end();
    g_cur->nav_enabled = false;
    frame(false, false, 500);
    GuiTextBuf composing = {0};
    strcpy(composing.buf, "kept");
    composing.len = composing.cursor = 4;
    strcpy(g_cur->composition, "変換中");
    g_cur->in.key_backspace = true;
    gui_text_buf_process(&composing);
    assert(strcmp(composing.buf, "kept") == 0);
    g_cur->composition[0] = '\0';
    memset(&g_cur->in, 0, sizeof(g_cur->in));
    strcpy(g_cur->in.text_input, "日本語");
    g_cur->in.text_input_len = 9;
    gui_text_buf_process(&composing);
    assert(strcmp(composing.buf, "kept日本語") == 0);
    memset(&composing, 0, sizeof(composing));
    memset(composing.buf, 'a', GUI_MAX_TEXT_BUF - 3);
    composing.len = composing.cursor = GUI_MAX_TEXT_BUF - 3;
    gui_text_buf_process(&composing);
    assert(composing.len == GUI_MAX_TEXT_BUF - 3);
    *gui_list_scroll(1) = 80;
    assert(*gui_list_scroll(65) == 0);
    *gui_list_scroll(65) = 160;
    assert(*gui_list_scroll(1) == 80);
    GuiApp *original = g_cur;
    GuiApp *other = calloc(1, sizeof(*other));
    assert(other);
    g_cur = other;
    assert(*gui_list_scroll(1) == 0);
    *gui_list_scroll(1) = 10;
    g_cur = original;
    assert(*gui_list_scroll(1) == 80);
    free(other);
    for (uint32_t id = 1000; id < 1300; id++) {
        assert(*gui_list_scroll(id) == 0);
        *gui_list_scroll(id) = (float)id;
    }
    assert(*gui_list_scroll(1299) == 1299);
    assert(*gui_list_scroll(1000) == 0);
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
    float before_disabled = *gui_list_scroll(gui_hash("list-shrink"));
    frame(true, false, 30);
    g_cur->in.my = 50;
    assert(fn_list(3, list).number == 20);
    frame(false, false, 30);
    g_cur->in.my = 50;
    g_cur->in.scroll_y = 1;
    g_disabled = true;
    fn_list(3, list);
    assert(*gui_list_scroll(gui_hash("list-shrink")) == before_disabled);
    g_disabled = false;
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
    char long_name[160] = {0};
    for (int c = 0; c < 50; c++) strcat(long_name, "語");
    Value ax_name = hajimu_string(long_name);
    fn_a11y_name(1, &ax_name);
    assert(strlen(g_a11y_current.name) == 126);
    frame(false, false, 500);
    g_cur->in.accessibility_press = gui_hash("accessible");
    Value accessible_button[] = {hajimu_string("accessible")};
    assert(fn_button(1, accessible_button).boolean);
    assert(!fn_button(1, accessible_button).boolean);
    frame(false, false, 500);
    g_cur->in.accessibility_press = gui_hash("check");
    assert(fn_checkbox(2, check).boolean);
    frame(false, false, 500);
    g_cur->in.accessibility_press = gui_hash("accessible");
    g_disabled = true;
    assert(!fn_button(1, accessible_button).boolean);
    g_disabled = false;
    frame(false, false, 500);
    g_cur->in.accessibility_press = gui_hash("accessible");
    g_cur->lay.y = 1000;
    assert(!fn_button(1, accessible_button).boolean);
    frame(true, false, 30);
    g_cur->in.my = 50;
    g_scrolls[0] = (GuiScrollRegion){.active = true, .x = 0, .y = 0, .w = 100, .h = 20};
    assert(!gui_widget_logic(901, 0, 0, 100, 100, &hovered, &pressed) && !hovered);
    g_scrolls[0].active = false;
    Value action_options = make_dict();
    dict_set(&action_options, "キー", hajimu_string("s"));
    dict_set(&action_options, "修飾", hajimu_string("Ctrl"));
    Value action[] = {hajimu_string("save"), hajimu_string("保存"), action_options};
    frame(false, false, 500);
    gui_nav_begin();
    g_cur->in.last_key = 's'; g_cur->in.mod_ctrl = true;
    assert(fn_action_register(3, action).boolean);
    assert(fn_action_take(1, action).boolean);
    assert(!fn_action_take(1, action).boolean);
    fn_action_register(3, action);
    assert(!fn_action_take(1, action).boolean);
    frame(false, false, 500);
    gui_nav_begin();
    g_cur->in.last_key = 's'; g_cur->in.mod_ctrl = true;
    g_cur->focused = 55;
    fn_action_register(3, action);
    assert(!fn_action_take(1, action).boolean);
    dict_set(&action_options, "入力中", hajimu_bool(true)); action[2] = action_options;
    fn_action_register(3, action);
    assert(fn_action_take(1, action).boolean);
    g_cur->focused = 0;
    frame(false, false, 500);
    gui_nav_begin();
    g_cur->in.last_key = 's'; g_cur->in.mod_ctrl = true;
    strcpy(g_cur->composition, "変換");
    fn_action_register(3, action);
    assert(!fn_action_take(1, action).boolean);
    g_cur->composition[0] = '\0';
    frame(true, false, 30);
    gui_nav_begin(); fn_action_register(3, action); fn_action_button(1, action);
    frame(false, true, 30);
    gui_nav_begin(); fn_action_register(3, action); fn_action_button(1, action);
    assert(fn_action_take(1, action).boolean);
    frame(false, false, 500);
    gui_nav_begin();
#ifdef __APPLE__
    fn_action_register(3, action);
    g_use_native_menubar = true;
    g_native_menu_prefix[0] = '\0';
    g_native_clicked_tag = gui_hash(".保存");
    fn_action_menu(1, action);
    assert(fn_action_take(1, action).boolean);
    g_use_native_menubar = false;
#endif
    frame(false, false, 500);
    gui_nav_begin();
    g_cur->in.last_key = 's'; g_cur->in.mod_ctrl = true;
    dict_set(&action_options, "有効", hajimu_bool(false)); action[2] = action_options;
    fn_action_register(3, action);
    assert(!fn_action_take(1, action).boolean);
    dict_set(&action_options, "有効", hajimu_bool(true)); action[2] = action_options;
    Value action_modal[] = {hajimu_string("block-actions")};
    fn_modal_reserve(1, action_modal);
    fn_action_register(3, action);
    assert(!fn_action_take(1, action).boolean);
    fn_modal_reserve(1, close_modal);
    frame(false, false, 500); gui_nav_begin();
    assert(!fn_action_take(1, action).boolean);
    hjpCancelFrame(g_cur->vg);
    Value history_start[] = {hajimu_string("owned history")};
    Value history_id = fn_history_create(1, history_start);
    int history_slot = gui_history_index(history_id);
    assert(history_slot >= 0);
    char *shutdown_args[] = {"/bin/sleep", "10", NULL};
    int shutdown_task = hjp_task_start(g_cur, shutdown_args);
    assert(shutdown_task > 0);
    pid_t shutdown_pid = hjp_task_find(shutdown_task, g_cur)->pid;
    g_cur = NULL;
    fn_app_quit(1, &app);
    assert(g_histories[history_slot].id == 0);
    assert(waitpid(shutdown_pid, NULL, WNOHANG) == -1 && errno == ECHILD);
    puts("application controls passed: registration, UTF-8, text selection/navigation, clicks, list isolation/LRU, visible rows, disabled wheel, shrink");
    return 0;
}
