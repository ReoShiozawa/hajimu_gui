// 実装を同じ翻訳単位へ読み込み、ARC管理フィールドの寿命を検査する。
#include "../src/hjp_platform_macos.m"
#include <assert.h>
#include <malloc/malloc.h>

int main(int argc, char **argv) {
    assert(argc == 2);
    @autoreleasepool {
        assert(hjp_init());
        hjp_native_menubar_init();
        hjp_native_menubar_begin_menu("Action Test");
        hjp_native_menubar_add_item("Save", "Ctrl+S", 321);
        NSMenuItem *save = [[NSApp.mainMenu itemWithTitle:@"Action Test"].submenu itemWithTag:321];
        assert(save);
        hjp_native_menubar_enable_item(321, false);
        assert(!save.enabled);
        hjp_native_menubar_enable_item(321, true);
        assert(save.enabled);
        for (int i = 0; i < 20; i++) {
            __weak HjpWindowDelegate *delegate;
            __weak HjpAccessibilityElement *weakElement;
            __weak HjpOpenGLView *weakView;
            @autoreleasepool {
                HjpWindow *win = hjp_window_create("寿命検査", 0, 0, 320, 200, i == 0 ? HJP_WINDOW_OPENGL : 0);
                assert(win != NULL);
                if (i == 0) {
                    HjpOpenGLView *view = win->glview;
                    [view setMarkedText:@"にほん" selectedRange:NSMakeRange(1, 1) replacementRange:NSMakeRange(NSNotFound, 0)];
                    assert(view.hasMarkedText);
                    assert(view.markedRange.length == 3);
                    assert(view.selectedRange.location == 1);
                    NSRange actual;
                    assert([[[view attributedSubstringForProposedRange:NSMakeRange(1, 99) actualRange:&actual] string] isEqualToString:@"ほん"]);
                    assert(actual.length == 2);
                    hjp_set_text_input_rect(win, 25, 40, 1, 30);
                    NSRect candidate = [view firstRectForCharacterRange:NSMakeRange(0, 1) actualRange:&actual];
                    assert(candidate.size.height == 30 && isfinite(candidate.origin.x));
                    g_hjp.evHead = g_hjp.evTail = 0;
                    NSMutableString *commit = [NSMutableString string];
                    for (int c = 0; c < 300; c++) [commit appendString:@"語"];
                    [view insertText:commit replacementRange:NSMakeRange(NSNotFound, 0)];
                    assert(!view.hasMarkedText && view.markedRange.location == NSNotFound);
                    HjpEvent e;
                    size_t bytes = 0;
                    bool cleared = false;
                    while (hjp__popEvent(&e)) {
                        if (e.type == HJP_EVENT_TEXTEDITING) cleared = e.text.text[0] == '\0';
                        if (e.type == HJP_EVENT_TEXTINPUT) {
                            assert([[NSString alloc] initWithUTF8String:e.text.text] != nil);
                            bytes += strlen(e.text.text);
                        }
                    }
                    assert(cleared && bytes == strlen(commit.UTF8String));
                    hjp_accessibility_begin(win);
                    assert(hjp_accessibility_add(win, 801, "保存", "作品を保存", 0, 20, 30, 100, 40, true, false));
                    assert(hjp_accessibility_add(win, 802, "自動保存", "", 1, 20, 80, 100, 40, false, true));
                    hjp_accessibility_end(win);
                    assert(view.accessibilityChildren.count == 2);
                    HjpAccessibilityElement *button = view.accessibilityChildren[0];
                    HjpAccessibilityElement *check = view.accessibilityChildren[1];
                    assert([button.accessibilityLabel isEqualToString:@"保存"]);
                    assert([button.accessibilityRole isEqualToString:NSAccessibilityButtonRole]);
                    assert([button.accessibilityHelp isEqualToString:@"作品を保存"]);
                    assert(button.accessibilityParent == view);
                    assert(button.accessibilityFrame.size.width == 100);
                    NSRect beforeMove = button.accessibilityFrame;
                    NSPoint origin = win->nswin.frame.origin;
                    [win->nswin setFrameOrigin:NSMakePoint(origin.x + 10, origin.y + 10)];
                    assert(fabs(button.accessibilityFrame.origin.x - beforeMove.origin.x - 10) < 1);
                    NSPoint hit = NSMakePoint(NSMidX(button.accessibilityFrame), NSMidY(button.accessibilityFrame));
                    assert([view accessibilityHitTest:hit] == button);
                    [win->nswin setFrameOrigin:origin];
                    assert([check.accessibilityRole isEqualToString:NSAccessibilityCheckBoxRole]);
                    assert([check.accessibilityValue boolValue]);
                    assert(![check accessibilityPerformPress]);
                    assert([button accessibilityPerformPress]);
                    assert(hjp__popEvent(&e) && e.type == HJP_EVENT_ACCESSIBILITY_PRESS);
                    assert(e.accessibility.id == 801 && e.accessibility.window == win);
                    assert(e.accessibility.generation == hjp_accessibility_window_id(win));
                    hjp_accessibility_begin(win);
                    hjp_accessibility_add(win, 801, "保存済み", "", 0, 20, 30, 100, 40, true, false);
                    hjp_accessibility_end(win);
                    assert(view.accessibilityChildren.count == 1 && view.accessibilityChildren[0] == button);
                    assert([button.accessibilityLabel isEqualToString:@"保存済み"]);
                    assert(![check accessibilityPerformPress]);
                    weakElement = button;
                    weakView = view;
                    hjp_accessibility_begin(win);
                    for (uint32_t id = 1000; id < 1512; id++)
                        assert(hjp_accessibility_add(win, id, "項目", "", 0, 20, 30, 10, 10, true, false));
                    assert(!hjp_accessibility_add(win, 1512, "超過", "", 0, 20, 30, 10, 10, true, false));
                    hjp_accessibility_end(win);
                    assert(view.accessibilityChildren.count == 512 && view.accessibleCache.count == 512);
                    hjp_accessibility_begin(win);
                    hjp_accessibility_end(win);
                    assert(view.accessibilityChildren.count == 0 && view.accessibleCache.count == 0);
                    assert(![button accessibilityPerformPress]);
                }
                delegate = win->delegate;
                assert(delegate != nil);
                assert([win->nswin delegate] == delegate);
                // ネイティブの閉じる要求が、破棄でなくイベントとして届くことを確認する。
                assert(![delegate windowShouldClose:win->nswin]);
                HjpEvent event;
                bool close_received = false;
                while (hjp_poll_event(&event)) {
                    if (event.type == HJP_EVENT_QUIT) close_received = true;
                }
                assert(close_received);
                hjp_window_destroy(win);
            }
            assert(delegate == nil);
            assert(weakElement == nil && weakView == nil);
        }
        HjpFont font = hjp_font_create_from_file(argv[1]);
        assert(font != NULL);
        for (int i = 0; i < 100; i++) assert(hjp_font_text_width(font, 18, "文字幅の寿命検査", NULL) > 0);
        malloc_statistics_t before = {0}, after = {0};
        malloc_zone_statistics(NULL, &before);
        for (int i = 0; i < 10000; i++) assert(hjp_font_text_width(font, 18, "文字幅の寿命検査", NULL) > 0);
        malloc_zone_statistics(NULL, &after);
        assert(after.size_in_use <= before.size_in_use + 1024 * 1024);
        hjp_font_destroy(font);
        hjp_quit();
        printf("macOS ARC lifetime passed: 20 windows, 10000 width calculations, heap delta %lld bytes\n",
               (long long)after.size_in_use - (long long)before.size_in_use);
    }
    return 0;
}
