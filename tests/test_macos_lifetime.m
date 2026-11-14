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
            __weak HjpAccessibilityTextElement *weakText;
            __weak HjpOpenGLView *weakView;
            @autoreleasepool {
                HjpWindow *win = hjp_window_create("寿命検査", 0, 0, 320, 200, i == 0 ? HJP_WINDOW_OPENGL : 0);
                assert(win != NULL);
                if (i == 0) {
                    HjpOpenGLView *view = win->glview;
                    g_hjp.evHead = g_hjp.evTail = 0;
                    NSEvent *mouse = [NSEvent mouseEventWithType:NSEventTypeLeftMouseDown
                        location:NSMakePoint(25, 50) modifierFlags:NSEventModifierFlagShift
                        timestamp:0 windowNumber:win->nswin.windowNumber context:nil
                        eventNumber:0 clickCount:1 pressure:1];
                    [view mouseDown:mouse];
                    HjpEvent press;
                    assert(hjp__popEvent(&press) && press.type == HJP_EVENT_MOUSEBUTTONDOWN);
                    assert(press.source.window == win && press.source.generation == win->windowId);
                    assert(press.button.mod & HJP_KMOD_SHIFT);
                    mouse = [NSEvent mouseEventWithType:NSEventTypeLeftMouseUp
                        location:NSMakePoint(25, 50) modifierFlags:0
                        timestamp:0 windowNumber:win->nswin.windowNumber context:nil
                        eventNumber:0 clickCount:1 pressure:0];
                    [view mouseUp:mouse];
                    assert(hjp__popEvent(&press) && press.type == HJP_EVENT_MOUSEBUTTONUP);
                    assert(!(press.button.mod & HJP_KMOD_SHIFT));
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
                            assert(e.source.window == win && e.source.generation == win->windowId);
                            assert([[NSString alloc] initWithUTF8String:e.text.text] != nil);
                            bytes += strlen(e.text.text);
                        }
                    }
                    assert(cleared && bytes == strlen(commit.UTF8String));
                    hjp_set_text_input_context(win, 901, "A😀日本Z", 5, 11, 2048);
                    assert(NSEqualRanges(view.selectedRange, NSMakeRange(3, 2)));
                    [view setMarkedText:@"にほん" selectedRange:NSMakeRange(1, 1)
                        replacementRange:NSMakeRange(NSNotFound, 0)];
                    assert(NSEqualRanges(view.markedRange, NSMakeRange(3, 3)));
                    assert(NSEqualRanges(view.selectedRange, NSMakeRange(4, 1)));
                    assert([[[view attributedSubstringForProposedRange:NSMakeRange(0, 99)
                        actualRange:&actual] string] isEqualToString:@"A😀にほんZ"]);
                    assert(hjp__popEvent(&e) && e.type == HJP_EVENT_TEXTSELECTION);
                    assert(e.edit.id == 901 && e.edit.start == 5 && e.edit.end == 11);
                    assert(hjp__popEvent(&e) && e.type == HJP_EVENT_TEXTEDITING);
                    [view insertText:@"日本語" replacementRange:NSMakeRange(NSNotFound, 0)];
                    assert(hjp__popEvent(&e) && e.type == HJP_EVENT_TEXTREPLACE);
                    assert(e.edit.start == 5 && e.edit.end == 11 && !strcmp(e.edit.text, "日本語"));
                    assert(e.edit.window == win && e.edit.generation == win->windowId);
                    assert([view.inputDocument isEqualToString:@"A😀日本語Z"]);
                    assert(hjp__popEvent(&e) && e.type == HJP_EVENT_TEXTEDITING);
                    [view insertText:@"bad" replacementRange:NSMakeRange(2, 0)];
                    assert([view.inputDocument isEqualToString:@"A😀日本語Z"]);
                    assert(!hjp__popEvent(&e));
                    [view insertText:@"" replacementRange:NSMakeRange(1, 2)];
                    assert(hjp__popEvent(&e) && e.type == HJP_EVENT_TEXTREPLACE);
                    assert(e.edit.start == 1 && e.edit.end == 5 && !e.edit.text[0]);
                    assert([view.inputDocument isEqualToString:@"A日本語Z"]);
                    while (hjp__popEvent(&e)) {}
                    hjp_set_text_input_context(win, 902, "AB", 1, 1, 2048);
                    [view insertText:@"日" replacementRange:NSMakeRange(NSNotFound, 0)];
                    [view insertText:@"本" replacementRange:NSMakeRange(NSNotFound, 0)];
                    int edits = 0;
                    while (hjp__popEvent(&e)) if (e.type == HJP_EVENT_TEXTREPLACE) {
                        assert(e.edit.start == (edits == 0 ? 1 : 4));
                        edits++;
                    }
                    assert(edits == 2 && [view.inputDocument isEqualToString:@"A日本B"]);
                    hjp_set_text_input_context(win, 902, "A😀B", 6, 6, 2048);
                    [view doCommandBySelector:@selector(moveLeftAndModifySelection:)];
                    assert(NSEqualRanges(view.selectedRange, NSMakeRange(3, 1)));
                    assert(hjp__popEvent(&e) && e.type == HJP_EVENT_TEXTSELECTION);
                    assert(e.edit.start == 5 && e.edit.end == 6 && e.edit.cursor == 5);
                    [view doCommandBySelector:@selector(moveLeftAndModifySelection:)];
                    assert(NSEqualRanges(view.selectedRange, NSMakeRange(1, 3)));
                    assert(hjp__popEvent(&e) && e.edit.start == 1 && e.edit.end == 6 && e.edit.cursor == 1);
                    [view insertText:@"語" replacementRange:NSMakeRange(NSNotFound, 0)];
                    assert([view.inputDocument isEqualToString:@"A語"]);
                    while (hjp__popEvent(&e)) {}
                    hjp_set_text_input_context(win, 902, "AB", 2, 2, 2048);
                    NSEvent *backspace = [NSEvent keyEventWithType:NSEventTypeKeyDown
                        location:NSZeroPoint modifierFlags:0 timestamp:0 windowNumber:win->nswin.windowNumber
                        context:nil characters:@"\177" charactersIgnoringModifiers:@"\177" isARepeat:NO keyCode:51];
                    [view keyDown:backspace];
                    assert([view.inputDocument isEqualToString:@"A"]);
                    int deletions = 0;
                    while (hjp__popEvent(&e)) {
                        assert(e.type != HJP_EVENT_KEYDOWN);
                        if (e.type == HJP_EVENT_TEXTREPLACE) deletions++;
                    }
                    assert(deletions == 1);
                    hjp_set_text_input_context(win, 902, "AB", 1, 1, 2048);
                    [view insertText:commit replacementRange:NSMakeRange(NSNotFound, 0)];
                    bytes = 0;
                    while (hjp__popEvent(&e)) if (e.type == HJP_EVENT_TEXTREPLACE) {
                        assert([[NSString alloc] initWithUTF8String:e.edit.text] != nil);
                        assert(e.edit.start == (int)(1 + bytes) && e.edit.end == e.edit.start);
                        bytes += strlen(e.edit.text);
                    }
                    assert(bytes == strlen(commit.UTF8String));
                    hjp_set_text_input_context(win, 902, "AB", 1, 1, 2048);
                    [view setMarkedText:@"に" selectedRange:NSMakeRange(1, 0)
                        replacementRange:NSMakeRange(NSNotFound, 0)];
                    [view unmarkText];
                    assert(!view.hasMarkedText && [view.inputDocument isEqualToString:@"AにB"]);
                    while (hjp__popEvent(&e)) {}
                    hjp_set_text_input_context(win, 902, "AB", 1, 1, 2048);
                    HjpEvent occupied = {0};
                    for (int q = 0; q < HJP_MAX_EVENTS - 1; q++) hjp__pushEvent(&occupied);
                    int head = g_hjp.evHead;
                    [view insertText:commit replacementRange:NSMakeRange(NSNotFound, 0)];
                    assert(g_hjp.evHead == head && [view.inputDocument isEqualToString:@"AB"]);
                    while (hjp__popEvent(&e)) {}
                    hjp_set_text_input_context(win, 903, "日本", 0, 3, 8);
                    [view insertText:@"語語" replacementRange:NSMakeRange(NSNotFound, 0)];
                    assert([view.inputDocument isEqualToString:@"語本"]);
                    while (hjp__popEvent(&e)) {}
                    hjp_set_text_input_context(win, 0, "", 0, 0, 0);
                    assert(!view.documentID && view.inputDocument.length == 0);
                    hjp_accessibility_begin(win);
                    assert(hjp_accessibility_text(win, 904, "本文", "編集する本文", "A😀日本", 5, 11,
                        10, 10, 200, 30, true, false, false, false));
                    hjp_accessibility_end(win);
                    HjpAccessibilityTextElement *field = (HjpAccessibilityTextElement *)view.accessibilityChildren[0];
                    weakText = field;
                    assert([field.accessibilityRole isEqualToString:NSAccessibilityTextFieldRole]);
                    assert([field.accessibilityValue isEqualToString:@"A😀日本"]);
                    assert(field.accessibilityNumberOfCharacters == 5);
                    assert([field.accessibilitySelectedText isEqualToString:@"日本"]);
                    assert(NSEqualRanges(field.accessibilitySelectedTextRange, NSMakeRange(3, 2)));
                    [field setAccessibilitySelectedTextRange:NSMakeRange(2, 0)];
                    assert(!hjp__popEvent(&e));
                    NSMutableString *overLimit = [NSMutableString string];
                    for (int p = 0; p < 2048; p++) [overLimit appendString:@"x"];
                    [field setAccessibilityValue:overLimit];
                    assert(!hjp__popEvent(&e));
                    for (int q = 0; q < HJP_MAX_EVENTS - 1; q++) hjp__pushEvent(&occupied);
                    head = g_hjp.evHead;
                    [field setAccessibilityValue:commit];
                    assert(g_hjp.evHead == head);
                    while (hjp__popEvent(&e)) {}
                    [field setAccessibilitySelectedTextRange:NSMakeRange(1, 2)];
                    assert(hjp__popEvent(&e) && e.type == HJP_EVENT_ACCESSIBILITY_FOCUS && e.edit.cursor == 1);
                    assert(hjp__popEvent(&e) && e.type == HJP_EVENT_ACCESSIBILITY_TEXTSELECTION);
                    assert(e.edit.id == 904 && e.edit.start == 1 && e.edit.end == 5);
                    assert(!hjp__popEvent(&e));
                    hjp_accessibility_begin(win);
                    assert(hjp_accessibility_text(win, 904, "本文", "", "A😀日本", 1, 5,
                        10, 10, 200, 30, true, true, false, false));
                    hjp_accessibility_end(win);
                    assert(view.accessibilityChildren[0] == field && [field.accessibilitySelectedText isEqualToString:@"😀"]);
                    [field setAccessibilitySelectedText:@"語"];
                    assert(hjp__popEvent(&e) && e.type == HJP_EVENT_ACCESSIBILITY_FOCUS);
                    assert(hjp__popEvent(&e) && e.type == HJP_EVENT_ACCESSIBILITY_TEXTREPLACE);
                    assert(e.edit.start == 0 && e.edit.end == -1 && !strcmp(e.edit.text, "A語日本"));
                    assert(!hjp__popEvent(&e));
                    [field setAccessibilityValue:@"stale-snapshot"];
                    [field setAccessibilitySelectedTextRange:NSMakeRange(0, 1)];
                    assert(!hjp__popEvent(&e));
                    hjp_accessibility_begin(win);
                    assert(hjp_accessibility_text(win, 904, "本文", "", "A語日本", 4, 4,
                        10, 10, 200, 30, true, true, false, true));
                    hjp_accessibility_end(win);
                    assert([field.accessibilityRole isEqualToString:NSAccessibilityTextAreaRole]);
                    [field setAccessibilityValue:commit];
                    assert(hjp__popEvent(&e) && e.type == HJP_EVENT_ACCESSIBILITY_FOCUS);
                    bytes = 0;
                    while (hjp__popEvent(&e)) {
                        assert(e.type == HJP_EVENT_ACCESSIBILITY_TEXTREPLACE);
                        assert(e.edit.start == (int)bytes && e.edit.end == (bytes ? (int)bytes : -1));
                        assert([[NSString alloc] initWithUTF8String:e.edit.text] != nil);
                        bytes += strlen(e.edit.text);
                    }
                    assert(bytes == strlen(commit.UTF8String));
                    hjp_accessibility_begin(win);
                    assert(hjp_accessibility_text(win, 904, "無効入力", "", "AB", 0, 0,
                        10, 10, 200, 30, false, false, false, false));
                    hjp_accessibility_end(win);
                    [field setAccessibilityValue:@"拒否"];
                    [field setAccessibilityFocused:YES];
                    assert(!hjp__popEvent(&e));
                    assert(![field isAccessibilitySelectorAllowed:@selector(setAccessibilityValue:)]);
                    hjp_accessibility_begin(win);
                    assert(hjp_accessibility_text(win, 904, "秘密", "", "hidden-secret", 0, 0,
                        10, 10, 200, 30, true, false, true, false));
                    hjp_accessibility_end(win);
                    assert(field.accessibilityValue == nil && field.accessibilitySelectedText == nil);
                    assert(field.snapshotText.length == 0 && field.accessibilityNumberOfCharacters == 0);
                    assert(field.accessibilitySelectedTextRange.location == NSNotFound);
                    assert(field.accessibilityProtectedContent);
                    assert([field.accessibilitySubrole isEqualToString:NSAccessibilitySecureTextFieldSubrole]);
                    [field setAccessibilityValue:@"must-not-change"];
                    assert(!hjp__popEvent(&e));
                    [field setAccessibilityFocused:YES];
                    assert(hjp__popEvent(&e) && e.type == HJP_EVENT_ACCESSIBILITY_FOCUS);
                    assert(!view.documentID && view.inputDocument.length == 0);
                    [field setAccessibilityFocused:NO];
                    assert(hjp__popEvent(&e) && e.type == HJP_EVENT_ACCESSIBILITY_FOCUS && !e.edit.cursor);
                    hjp_accessibility_begin(win);
                    hjp_accessibility_end(win);
                    [field setAccessibilityValue:@"hidden"];
                    assert(field.accessibilityValue == nil && !hjp__popEvent(&e));
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
                    if (event.type == HJP_EVENT_QUIT && event.source.window == win && event.source.generation == win->windowId) close_received = true;
                }
                assert(close_received);
                hjp_window_destroy(win);
            }
            assert(delegate == nil);
            assert(weakElement == nil && weakText == nil && weakView == nil);
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
