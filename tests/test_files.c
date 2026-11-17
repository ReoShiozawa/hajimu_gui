#include "../src/hjp_files.h"
#include <assert.h>
#ifdef _WIN32
#include <direct.h>
#else
#include <dirent.h>
#endif

static char source[4096], target[4096];
static unsigned notifications;
static uint64_t last_done, expected_size;
static int cancel_after;
static bool report(uint64_t done, uint64_t total, void *context) {
    assert(context == &notifications && done >= last_done && total == expected_size && done <= total);
    last_done = done; notifications++;
    return cancel_after < 0 || (int)notifications < cancel_after;
}
static void write_text(const char *path, const char *text) {
    FILE *file = hjp_file_open(path, true); assert(file);
    size_t size = strlen(text); assert(fwrite(text, 1, size, file) == size && !fclose(file));
}
static void check_text(const char *path, const char *expected) {
    FILE *file = hjp_file_open(path, false); assert(file);
    char text[128]; size_t size = fread(text, 1, sizeof(text) - 1, file); text[size] = 0;
    assert(!ferror(file) && !fclose(file) && !strcmp(text, expected));
}
static void compare_files(const char *left, const char *right) {
    FILE *a = hjp_file_open(left, false), *b = hjp_file_open(right, false); assert(a && b);
    unsigned char one[4096], two[4096];
    size_t size;
    do {
        size = fread(one, 1, sizeof(one), a);
        assert(fread(two, 1, sizeof(two), b) == size && !memcmp(one, two, size));
    } while (size);
    assert(!ferror(a) && !ferror(b)); assert(!fclose(a) && !fclose(b));
}
static bool change_source(uint64_t done, uint64_t total, void *context) {
    (void)total; (void)context;
    if (!done) write_text(source, "changed");
    return true;
}
static bool grow_source(uint64_t done, uint64_t total, void *context) {
    (void)context; assert(done <= total);
    if (!done) write_text(source, "a longer source after copy started");
    return true;
}
static unsigned resource_count(void) {
#ifdef _WIN32
    DWORD count; assert(GetProcessHandleCount(GetCurrentProcess(), &count)); return count;
#else
    unsigned count = 0;
    for (int fd = 0; fd < 1024; fd++) if (fcntl(fd, F_GETFD) >= 0) count++;
    return count;
#endif
}
int main(void) {
    const char *directory = getenv("HJP_FILE_TEST_DIR"); assert(directory && strlen(directory) < 3500);
    snprintf(source, sizeof(source), "%s/日本語 元.bin", directory);
    snprintf(target, sizeof(target), "%s/日本語 先.bin", directory);
    FILE *file = hjp_file_open(source, true); assert(file);
    unsigned char block[65536];
    for (size_t i = 0; i < sizeof(block); i++) block[i] = (unsigned char)i;
    for (int i = 0; i < 80; i++) assert(fwrite(block, 1, sizeof(block), file) == sizeof(block));
    assert(!fclose(file)); expected_size = sizeof(block) * 80;
    write_text(target, "previous output");
    notifications = 0; last_done = 0; cancel_after = 2;
    assert(!hjp_file_copy(source, target, report, &notifications));
    assert(notifications == 2 && last_done == 1024 * 1024); check_text(target, "previous output");
    notifications = 0; last_done = 0; cancel_after = -1;
    assert(hjp_file_copy(source, target, report, &notifications));
    assert(last_done == expected_size && notifications == 6); compare_files(source, target);
    assert(hjp_file_copy(source, source, NULL, NULL)); compare_files(source, target);
    char stage[4096], absent[4096], folder[4096], alias[4096];
    snprintf(stage, sizeof(stage), "%s/stage.bin", directory);
    snprintf(absent, sizeof(absent), "%s/absent.bin", directory);
    snprintf(folder, sizeof(folder), "%s/folder", directory);
    snprintf(alias, sizeof(alias), "%s/alias.bin", directory);
    write_text(stage, "new output");
    assert(!hjp_file_publish(stage, target, false)); check_text(stage, "new output"); compare_files(source, target);
    assert(hjp_file_publish(stage, absent, false)); check_text(absent, "new output");
    assert(!hjp_file_open(stage, false));
#ifdef _WIN32
    wchar_t *wide_folder = hjp_file_wide(folder), *wide_alias = hjp_file_wide(alias), *wide_source = hjp_file_wide(source);
    assert(wide_folder && wide_alias && wide_source && !_wmkdir(wide_folder));
    assert(CreateHardLinkW(wide_alias, wide_source, NULL));
#else
    assert(!mkdir(folder, 0700) && !link(source, alias));
    assert(!chmod(target, 0600));
#endif
    assert(!hjp_file_copy(folder, target, NULL, NULL)); compare_files(source, target);
    assert(!hjp_file_publish(folder, target, false));
    assert(hjp_file_copy(source, alias, NULL, NULL)); compare_files(source, alias);
#ifndef _WIN32
    struct stat mode; assert(!stat(target, &mode) && (mode.st_mode & 0777) == 0600);
    char pipe[4096]; snprintf(pipe, sizeof(pipe), "%s/pipe.bin", directory);
    assert(!mkfifo(pipe, 0600)); assert(!hjp_file_copy(pipe, target, NULL, NULL)); assert(!unlink(pipe));
    assert(!unlink(alias) && !symlink(source, alias));
    assert(!hjp_file_publish(alias, target, false)); compare_files(source, target);
#endif
    assert(hjp_file_remove(alias));
    assert(!hjp_file_copy(stage, target, NULL, NULL)); compare_files(source, target);
    write_text(target, "unchanged after source mutation");
    assert(!hjp_file_copy(source, target, change_source, NULL));
    check_text(target, "unchanged after source mutation"); check_text(source, "changed");
    assert(!hjp_file_copy(source, target, grow_source, NULL));
    check_text(target, "unchanged after source mutation");
    unsigned resources = resource_count();
    for (int i = 0; i < 30; i++) assert(hjp_file_copy(source, absent, NULL, NULL));
    assert(resource_count() == resources);
    assert(hjp_file_remove(source) && hjp_file_remove(target) && hjp_file_remove(absent));
#ifdef _WIN32
    assert(!_wrmdir(wide_folder)); free(wide_folder); free(wide_alias); free(wide_source);
    /* 残存する一時ファイルがあれば、呼出側のディレクトリ削除も失敗する。 */
    wchar_t *wide_directory = hjp_file_wide(directory); assert(wide_directory && !_wrmdir(wide_directory)); free(wide_directory);
#else
    assert(!rmdir(folder) && !rmdir(directory));
#endif
    puts("files passed: binary/Unicode copy, progress, cancellation, old-output protection, aliases, no-clobber publish and cleanup");
    return 0;
}
