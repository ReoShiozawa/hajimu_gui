// 実装を同じ翻訳単位へ読み込み、ARC管理フィールドの寿命を検査する。
#include "../src/hjp_platform_macos.m"
#include <assert.h>
#include <malloc/malloc.h>

int main(int argc, char **argv) {
    assert(argc == 2);
    @autoreleasepool {
        assert(hjp_init());
        for (int i = 0; i < 20; i++) {
            __weak HjpWindowDelegate *delegate;
            @autoreleasepool {
                HjpWindow *win = hjp_window_create("寿命検査", 0, 0, 320, 200, 0);
                assert(win != NULL);
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
