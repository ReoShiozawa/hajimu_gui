#include <sys/wait.h>
static pid_t control_waitpid(pid_t pid, int *status, int flags);
static int control_kill(pid_t group, int signal);
#define HJP_TASK_WAITPID control_waitpid
#define HJP_TASK_KILL control_kill
#include "../src/hajimu_gui.c"
#include <assert.h>

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
    g_cur->focused = 0;
    hjpCancelFrame(vg);
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
    check_ime_notifications();
    check_textarea_layout();
    check_text_selection();
    check_text_navigation();
    check_document_edits();
    check_accessible_text();
    check_task_cancellation();
    check_closing_loop();
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
