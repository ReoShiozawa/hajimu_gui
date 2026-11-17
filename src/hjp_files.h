#ifndef HJP_FILES_H
#define HJP_FILES_H
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#else
#include <unistd.h>
#ifdef __APPLE__
#include <sys/stdio.h>
#elif defined(__linux__) && defined(_GNU_SOURCE)
#include <sys/syscall.h>
#include <linux/fs.h>
#endif
#endif

typedef bool (*HjpCopyProgress)(uint64_t done, uint64_t total, void *context);
typedef struct { uint64_t size, modified; unsigned long fraction; unsigned mode; } HjpFileStamp;

#ifdef _WIN32
static wchar_t *hjp_file_wide(const char *path) {
    if (!path || !path[0]) return NULL;
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
    if (count <= 0 || count > 32767) return NULL;
    wchar_t *wide = malloc((size_t)count * sizeof(*wide));
    if (wide && !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, count)) { free(wide); return NULL; }
    return wide;
}
#endif
static inline FILE *hjp_file_open(const char *path, bool write) {
#ifdef _WIN32
    wchar_t *wide = hjp_file_wide(path);
    if (!wide) return NULL;
    FILE *file = _wfopen(wide, write ? L"wb" : L"rb");
    free(wide); return file;
#else
    return fopen(path, write ? "wb" : "rb");
#endif
}
static bool hjp_file_remove(const char *path) {
#ifdef _WIN32
    wchar_t *wide = hjp_file_wide(path);
    if (!wide) return false;
    bool ok = _wremove(wide) == 0; free(wide); return ok;
#else
    return unlink(path) == 0;
#endif
}
static bool hjp_file_stamp(FILE *file, HjpFileStamp *stamp) {
#ifdef _WIN32
    HANDLE handle = (HANDLE)_get_osfhandle(_fileno(file));
    BY_HANDLE_FILE_INFORMATION info;
    if (GetFileType(handle) != FILE_TYPE_DISK || !GetFileInformationByHandle(handle, &info) ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return false;
    *stamp = (HjpFileStamp){.size = ((uint64_t)info.nFileSizeHigh << 32) | info.nFileSizeLow,
        .modified = ((uint64_t)info.ftLastWriteTime.dwHighDateTime << 32) | info.ftLastWriteTime.dwLowDateTime};
#else
    struct stat info;
    if (fstat(fileno(file), &info) || !S_ISREG(info.st_mode) || info.st_size < 0) return false;
    *stamp = (HjpFileStamp){.size = (uint64_t)info.st_size, .modified = (uint64_t)info.st_mtime, .mode = info.st_mode & 0777};
#ifdef __APPLE__
#if !defined(_POSIX_C_SOURCE) || defined(_DARWIN_C_SOURCE)
    stamp->fraction = (unsigned long)info.st_mtimespec.tv_nsec;
#else
    stamp->fraction = (unsigned long)info.st_mtimensec;
#endif
#else
    stamp->fraction = (unsigned long)info.st_mtim.tv_nsec;
#endif
#endif
    return true;
}
static FILE *hjp_file_read_regular(const char *path, HjpFileStamp *stamp) {
#ifdef _WIN32
    FILE *file = hjp_file_open(path, false);
#else
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    FILE *file = fd >= 0 ? fdopen(fd, "rb") : NULL;
    if (!file && fd >= 0) close(fd);
#endif
    if (!file) return NULL;
    if (!hjp_file_stamp(file, stamp)) { fclose(file); return NULL; }
    return file;
}
/* 公開先が既にある場合、上書き禁止ではその内容を変更しない。 */
static bool hjp_file_publish(const char *source, const char *target, bool overwrite) {
    if (!source || !target || !source[0] || !target[0] || !strcmp(source, target)) return false;
#ifdef _WIN32
    wchar_t *from = hjp_file_wide(source), *to = hjp_file_wide(target);
    DWORD attributes = from ? GetFileAttributesW(from) : INVALID_FILE_ATTRIBUTES;
    bool ok = from && to && attributes != INVALID_FILE_ATTRIBUTES &&
        !(attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) &&
        MoveFileExW(from, to, MOVEFILE_WRITE_THROUGH | (overwrite ? MOVEFILE_REPLACE_EXISTING : 0));
    free(from); free(to); return ok;
#else
    struct stat info;
    if (lstat(source, &info) || !S_ISREG(info.st_mode)) return false;
    if (overwrite) return rename(source, target) == 0;
#if defined(__APPLE__) && defined(RENAME_EXCL)
    if (!renamex_np(source, target, RENAME_EXCL)) return true;
    if (errno != ENOSYS && errno != EINVAL && errno != ENOTSUP) return false;
#elif defined(__linux__) && defined(_GNU_SOURCE) && defined(SYS_renameat2)
    if (!syscall(SYS_renameat2, AT_FDCWD, source, AT_FDCWD, target, RENAME_NOREPLACE)) return true;
    if (errno != ENOSYS && errno != EINVAL && errno != EOPNOTSUPP) return false;
#endif
    if (link(source, target)) return false;
    /* 公開済みなら成功。一時名の削除失敗で公開結果を失敗扱いにしない。 */
    (void)unlink(source); return true;
#endif
}
static FILE *hjp_file_temporary(const char *target, char **path) {
    static uint64_t sequence;
    size_t length = strlen(target);
    if (length > SIZE_MAX - 96) return NULL;
    *path = malloc(length + 96);
    if (!*path) return NULL;
    for (int attempt = 0; attempt < 100; attempt++) {
        if (sequence == UINT64_MAX) break;
#ifdef _WIN32
        unsigned long pid = GetCurrentProcessId();
#else
        unsigned long pid = (unsigned long)getpid();
#endif
        snprintf(*path, length + 96, "%s.hajimu-copy-%lu-%llu", target, pid, (unsigned long long)++sequence);
#ifdef _WIN32
        wchar_t *wide = hjp_file_wide(*path);
        if (!wide) break;
        int fd = _wopen(wide, _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY | _O_NOINHERIT, _S_IREAD | _S_IWRITE);
        free(wide);
        if (fd >= 0) {
            FILE *file = _fdopen(fd, "wb");
            if (!file) { _close(fd); hjp_file_remove(*path); }
            return file;
        }
#else
        int fd = open(*path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd >= 0) {
            FILE *file = fdopen(fd, "wb");
            if (!file) { close(fd); hjp_file_remove(*path); }
            return file;
        }
#endif
        if (errno != EEXIST) break;
    }
    free(*path); *path = NULL; return NULL;
}
/* 一時ファイルだけを書き、取消/読取失敗/書込失敗では既存の公開先を残す。 */
static bool hjp_file_copy(const char *source, const char *target, HjpCopyProgress notify, void *context) {
    if (!source || !target || !source[0] || !target[0]) return false;
    HjpFileStamp initial, final;
    FILE *input = hjp_file_read_regular(source, &initial);
    if (!input) return false;
    if (!strcmp(source, target)) {
        bool ok = !notify || notify(initial.size, initial.size, context);
        return fclose(input) == 0 && ok;
    }
    char *temporary = NULL;
    FILE *output = hjp_file_temporary(target, &temporary);
    if (!output) { free(temporary); fclose(input); return false; }
    bool ok = !notify || notify(0, initial.size, context);
#ifndef _WIN32
    struct stat previous;
    unsigned mode = !lstat(target, &previous) && S_ISREG(previous.st_mode) ? previous.st_mode & 0777 : initial.mode;
    if (fchmod(fileno(output), mode)) ok = false;
#endif
    unsigned char buffer[64 * 1024];
    uint64_t copied = 0, reported = 0;
    while (ok) {
        size_t count = fread(buffer, 1, sizeof(buffer), input);
        if (!count) break;
        if (count > initial.size - copied) { ok = false; break; }
        if (fwrite(buffer, 1, count, output) != count) { ok = false; break; }
        copied += count;
        if (notify && copied - reported >= 1024 * 1024) {
            ok = notify(copied, initial.size, context); reported = copied;
        }
    }
    if (ferror(input) || !hjp_file_stamp(input, &final) || initial.size != copied ||
        initial.size != final.size || initial.modified != final.modified || initial.fraction != final.fraction) ok = false;
    if (fflush(output)) ok = false;
#ifdef _WIN32
    if (_commit(_fileno(output))) ok = false;
#else
    if (fsync(fileno(output))) ok = false;
#endif
    if (fclose(input)) ok = false;
    if (fclose(output)) ok = false;
    if (ok && notify && reported != copied) ok = notify(copied, initial.size, context);
    if (ok) ok = hjp_file_publish(temporary, target, true);
    if (!ok) hjp_file_remove(temporary);
    free(temporary); return ok;
}
#endif
