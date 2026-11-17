#ifndef HJP_TASKS_H
#define HJP_TASKS_H
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <math.h>

/* GUIスレッドだけが管理する子プロセス。言語ランタイムを共有しない。 */
#ifdef _WIN32
#include <windows.h>
typedef HANDLE HjpTaskPipe;
#else
#include <spawn.h>
#include <signal.h>
#include <sys/wait.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#ifndef HJP_TASK_WAITPID
#define HJP_TASK_WAITPID waitpid
#endif
#ifndef HJP_TASK_KILL
#define HJP_TASK_KILL kill
#endif
extern char **environ;
typedef int HjpTaskPipe;
#endif
#include <errno.h>
#define HJP_TASK_OUTPUT_LIMIT 16384
#define HJP_TASK_READ_BUDGET 16384
typedef struct {
    HjpTaskPipe fd;
    unsigned char bytes[HJP_TASK_OUTPUT_LIMIT];
    size_t length;
    uint64_t total;
    bool truncated, eof;
    int error;
} HjpTaskOutput;
typedef struct {
    int id, state, error;
    int64_t exit_code;
#ifdef _WIN32
    HANDLE pid, job;
#else
    pid_t pid;
#endif
    const void *owner;
    bool notified;
    bool cancel_requested;
    bool capture_output;
    HjpTaskOutput out, err;
    HjpTaskPipe progress_fd;
    double progress;
    char progress_line[128];
    size_t progress_length;
    bool progress_overflow;
} HjpTask;
enum { HJP_TASK_RUNNING = 1, HJP_TASK_SUCCEEDED, HJP_TASK_FAILED, HJP_TASK_CANCELLED, HJP_TASK_CANCELLING };
static HjpTask hjp_tasks[32];
static int hjp_task_next_id = 1;

static bool hjp_task_pipe_open(HjpTaskPipe fd) {
#ifdef _WIN32
    return fd && fd != INVALID_HANDLE_VALUE;
#else
    return fd >= 0;
#endif
}
static void hjp_task_close_fd(HjpTaskPipe *fd) {
#ifdef _WIN32
    if (hjp_task_pipe_open(*fd)) CloseHandle(*fd);
    *fd = NULL;
#else
    if (*fd >= 0) close(*fd);
    *fd = -1;
#endif
}
static void hjp_task_close_io(HjpTask *task) {
    hjp_task_close_fd(&task->out.fd);
    hjp_task_close_fd(&task->err.fd);
    hjp_task_close_fd(&task->progress_fd);
}
static void hjp_task_keep_output(HjpTaskOutput *output, const unsigned char *bytes, size_t n) {
    output->total += n;
    if (n >= HJP_TASK_OUTPUT_LIMIT) {
        memcpy(output->bytes, bytes + n - HJP_TASK_OUTPUT_LIMIT, HJP_TASK_OUTPUT_LIMIT);
        output->truncated |= output->length > 0 || n > HJP_TASK_OUTPUT_LIMIT;
        output->length = HJP_TASK_OUTPUT_LIMIT;
        return;
    }
    if (output->length + n > HJP_TASK_OUTPUT_LIMIT) {
        size_t drop = output->length + n - HJP_TASK_OUTPUT_LIMIT;
        memmove(output->bytes, output->bytes + drop, output->length - drop);
        output->length -= drop;
        output->truncated = true;
    }
    memcpy(output->bytes + output->length, bytes, n);
    output->length += n;
}
/* GUIスレッドだけが読む。Windowsも受信済みのバイト数までに制限する。 */
static int hjp_task_read_some(HjpTaskPipe *fd, unsigned char *bytes, size_t size, bool *eof, int *error) {
    if (!hjp_task_pipe_open(*fd)) return 0;
#ifdef _WIN32
    DWORD available = 0, count = 0;
    if (PeekNamedPipe(*fd, NULL, 0, NULL, &available, NULL)) {
        if (!available) return 0;
        DWORD request = available < size ? available : (DWORD)size;
        if (ReadFile(*fd, bytes, request, &count, NULL)) {
            if (count) return (int)count;
            *eof = true; hjp_task_close_fd(fd); return 0;
        }
    }
    DWORD code = GetLastError();
    if (code == ERROR_BROKEN_PIPE || code == ERROR_PIPE_NOT_CONNECTED) *eof = true;
    else *error = (int)code;
#else
    ssize_t count = read(*fd, bytes, size);
    if (count > 0) return (int)count;
    if (!count) *eof = true;
    else if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return 0;
    else *error = errno;
#endif
    hjp_task_close_fd(fd);
    return 0;
}
static void hjp_task_read_output(HjpTaskOutput *output) {
    unsigned char buffer[4096];
    size_t remaining = HJP_TASK_READ_BUDGET;
    while (hjp_task_pipe_open(output->fd) && remaining) {
        int n = hjp_task_read_some(&output->fd, buffer, remaining < sizeof(buffer) ? remaining : sizeof(buffer), &output->eof, &output->error);
        if (n > 0 && (size_t)n <= sizeof(buffer)) {
            hjp_task_keep_output(output, buffer, (size_t)n);
            remaining -= (size_t)n;
        } else break;
    }
}
static void hjp_task_progress_line(HjpTask *task) {
    if (!task->progress_overflow && task->progress_length) {
        task->progress_line[task->progress_length] = '\0';
        const char *end = task->progress_line;
        while (*end == ' ' || *end == '\t') end++;
        double value = 0;
        bool digits = false;
        while (*end >= '0' && *end <= '9') { digits = true; value = value * 10 + (*end++ - '0'); }
        if (*end == '.') {
            double scale = 0.1; end++;
            while (*end >= '0' && *end <= '9') { digits = true; value += (*end++ - '0') * scale; scale *= 0.1; }
        }
        while (*end == ' ' || *end == '\t' || *end == '\r') end++;
        if (digits && !*end && isfinite(value) && value >= 0 && value <= 100)
            task->progress = value;
    }
    task->progress_length = 0;
    task->progress_overflow = false;
}
static void hjp_task_read_progress(HjpTask *task) {
    unsigned char buffer[4096];
    size_t remaining = HJP_TASK_READ_BUDGET;
    bool eof = false;
    int error = 0;
    while (hjp_task_pipe_open(task->progress_fd) && remaining) {
        int n = hjp_task_read_some(&task->progress_fd, buffer, remaining < sizeof(buffer) ? remaining : sizeof(buffer), &eof, &error);
        if (n > 0) {
            for (int i = 0; i < n; i++) {
                if (buffer[i] == '\n') hjp_task_progress_line(task);
                else if (!buffer[i] || task->progress_length >= sizeof(task->progress_line) - 1) task->progress_overflow = true;
                else task->progress_line[task->progress_length++] = (char)buffer[i];
            }
            remaining -= (size_t)n;
        } else break;
    }
    if (eof) hjp_task_progress_line(task);
}

/* 生出力は末尾だけ保持する。文字列APIではNUL/不正UTF-8を置換し、受信途中の文字は保留する。 */
static size_t hjp_task_output_text(const HjpTaskOutput *output, char *text) {
    size_t used = 0;
    for (size_t i = 0; i < output->length;) {
        unsigned char first = output->bytes[i];
        size_t n = first < 0x80 ? 1 : first >= 0xc2 && first <= 0xdf ? 2 :
            first >= 0xe0 && first <= 0xef ? 3 : first >= 0xf0 && first <= 0xf4 ? 4 : 0;
        if (n > output->length - i && hjp_task_pipe_open(output->fd)) break;
        bool valid = n && n <= output->length - i && first != 0;
        for (size_t j = 1; valid && j < n; j++) valid = (output->bytes[i + j] & 0xc0) == 0x80;
        if (valid && n >= 3) valid = !(first == 0xe0 && output->bytes[i + 1] < 0xa0) &&
            !(first == 0xed && output->bytes[i + 1] >= 0xa0) && !(first == 0xf0 && output->bytes[i + 1] < 0x90) &&
            !(first == 0xf4 && output->bytes[i + 1] >= 0x90);
        if (valid) { memcpy(text + used, output->bytes + i, n); used += n; i += n; }
        else { memcpy(text + used, "\xef\xbf\xbd", 3); used += 3; i++; }
    }
    text[used] = '\0';
    return used;
}

static HjpTask *hjp_task_find(int id, const void *owner) {
    for (size_t i = 0; i < 32; i++)
        if (hjp_tasks[i].id == id && hjp_tasks[i].owner == owner) return &hjp_tasks[i];
    return NULL;
}
#ifndef _WIN32
static void hjp_task_poll(HjpTask *task) {
    if (!task || !task->id) return;
    hjp_task_read_output(&task->out);
    hjp_task_read_output(&task->err);
    hjp_task_read_progress(task);
    if (!task->pid) return;
    int status = 0;
    pid_t done = HJP_TASK_WAITPID(task->pid, &status, WNOHANG);
    if (done == 0 || (done < 0 && errno == EINTR)) return;
    if (done < 0) {
        task->pid = 0; task->state = HJP_TASK_FAILED; task->error = errno; return;
    }
    if (!WIFEXITED(status) && !WIFSIGNALED(status)) return;
    task->pid = 0;
    task->exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    task->state = task->cancel_requested ? HJP_TASK_CANCELLED : task->exit_code == 0 ? HJP_TASK_SUCCEEDED : HJP_TASK_FAILED;
}
/* fd 3を進捗専用に予約する。親の標準FDが閉じていてもdup2先と衝突させない。 */
static bool hjp_task_pipe(int pair[2]) {
    if (pipe(pair) < 0) return false;
    for (int i = 0; i < 2; i++) {
        if (pair[i] < 4) {
            int moved = fcntl(pair[i], F_DUPFD_CLOEXEC, 4);
            if (moved < 0) goto fail;
            close(pair[i]); pair[i] = moved;
        }
        if (fcntl(pair[i], F_SETFD, FD_CLOEXEC) < 0) goto fail;
    }
    int flags = fcntl(pair[0], F_GETFL);
    if (flags < 0 || fcntl(pair[0], F_SETFL, flags | O_NONBLOCK) < 0) goto fail;
    return true;
fail:
    hjp_task_close_fd(&pair[0]); hjp_task_close_fd(&pair[1]);
    return false;
}
static int hjp_task_start_with_io(const void *owner, char *const *args, bool capture, bool progress) {
    if (!args || !args[0] || !args[0][0]) return -1;
    HjpTask *slot = NULL;
    for (size_t i = 0; i < 32; i++) if (!hjp_tasks[i].id) { slot = &hjp_tasks[i]; break; }
    if (!slot || hjp_task_next_id == INT_MAX) return -1;
    int pipes[3][2] = {{-1, -1}, {-1, -1}, {-1, -1}};
    for (int i = 0; i < 3; i++) if ((i < 2 ? capture : progress) && !hjp_task_pipe(pipes[i])) goto cleanup;
    char **child_env = environ, **allocated_env = NULL;
    char progress_env[] = "HAJIMU_PROGRESS_FD=3";
    if (progress) {
        size_t count = 0;
        while (environ[count]) count++;
        allocated_env = calloc(count + 2, sizeof(*allocated_env));
        if (!allocated_env) goto cleanup;
        size_t used = 0;
        for (size_t i = 0; i < count; i++) if (strncmp(environ[i], "HAJIMU_PROGRESS_FD=", 19) != 0) allocated_env[used++] = environ[i];
        allocated_env[used] = progress_env;
        child_env = allocated_env;
    }
    posix_spawnattr_t attr;
    int error = posix_spawnattr_init(&attr);
    if (error) { free(allocated_env); goto cleanup; }
    posix_spawn_file_actions_t actions;
    error = posix_spawn_file_actions_init(&actions);
    if (error) { posix_spawnattr_destroy(&attr); free(allocated_env); goto cleanup; }
    for (int i = 0; !error && i < 3; i++) if (pipes[i][0] >= 0) {
        error = posix_spawn_file_actions_addclose(&actions, pipes[i][0]);
        if (!error) error = posix_spawn_file_actions_adddup2(&actions, pipes[i][1], i < 2 ? i + 1 : 3);
        if (!error) error = posix_spawn_file_actions_addclose(&actions, pipes[i][1]);
    }
    short flags = POSIX_SPAWN_SETPGROUP;
#ifdef POSIX_SPAWN_CLOEXEC_DEFAULT
    flags |= POSIX_SPAWN_CLOEXEC_DEFAULT;
#endif
    if (!error) error = posix_spawnattr_setflags(&attr, flags);
    if (!error) error = posix_spawnattr_setpgroup(&attr, 0);
    pid_t pid = 0;
    if (!error) error = posix_spawnp(&pid, args[0], &actions, &attr, args, child_env);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attr);
    free(allocated_env);
    if (error) goto cleanup;
    *slot = (HjpTask){.id = hjp_task_next_id++, .state = HJP_TASK_RUNNING,
                      .exit_code = -1, .pid = pid, .owner = owner, .progress = -1, .capture_output = capture,
                      .out.fd = pipes[0][0], .err.fd = pipes[1][0], .progress_fd = pipes[2][0]};
    for (int i = 0; i < 3; i++) hjp_task_close_fd(&pipes[i][1]);
    return slot->id;
cleanup:
    for (int i = 0; i < 3; i++) { hjp_task_close_fd(&pipes[i][0]); hjp_task_close_fd(&pipes[i][1]); }
    return -1;
}
static bool hjp_task_cancel(HjpTask *task) {
    if (!task || !task->pid || task->cancel_requested) return false;
    hjp_task_poll(task);
    if (!task->pid) return false;
    /* 要求だけを送り、回収済みになるまでPIDとパイプを保持する。 */
    if (HJP_TASK_KILL(-task->pid, SIGKILL) < 0 && errno != ESRCH) { task->error = errno; return false; }
    task->cancel_requested = true;
    task->state = HJP_TASK_CANCELLING;
    task->error = 0;
    return true;
}
#else
#include "hjp_tasks_win32.h"
#endif
static inline int hjp_task_start(const void *owner, char *const *args) {
    return hjp_task_start_with_io(owner, args, false, false);
}
/* 1回だけ取消/回収を進める。待機0、完了1、要求失敗-1。所有権は回収まで保持する。 */
static int hjp_tasks_close_step(const void *owner, int *error) {
    bool pending = false;
    *error = 0;
    for (size_t i = 0; i < 32; i++) if (hjp_tasks[i].id && hjp_tasks[i].owner == owner) {
        HjpTask *task = &hjp_tasks[i];
        hjp_task_poll(task);
        if (task->pid && !task->cancel_requested) {
            if (!hjp_task_cancel(task) && task->pid) *error = task->error ? task->error : EIO;
        }
        if (task->pid) pending = true;
        else { hjp_task_close_io(task); memset(task, 0, sizeof(*task)); }
    }
    return *error ? -1 : pending ? 0 : 1;
}
static void hjp_tasks_close(const void *owner) {
    for (size_t i = 0; i < 32; i++) if (hjp_tasks[i].id && hjp_tasks[i].owner == owner) {
        hjp_task_cancel(&hjp_tasks[i]);
        /* アプリ破棄時の同期回収は別契約。待機中もパイプを読み、出力詰まりを防ぐ。 */
        while (hjp_tasks[i].pid) {
            hjp_task_poll(&hjp_tasks[i]);
            if (hjp_tasks[i].pid) {
#ifdef _WIN32
                Sleep(1);
#else
                struct timespec pause = {0, 1000000}; nanosleep(&pause, NULL);
#endif
            }
        }
        hjp_task_close_io(&hjp_tasks[i]);
        memset(&hjp_tasks[i], 0, sizeof(hjp_tasks[i]));
    }
}
#endif
