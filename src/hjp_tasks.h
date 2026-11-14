#ifndef HJP_TASKS_H
#define HJP_TASKS_H
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <limits.h>

/* GUIスレッドだけが管理する子プロセス。言語ランタイムを共有しない。 */
#ifndef _WIN32
#include <spawn.h>
#include <signal.h>
#include <sys/wait.h>
#include <errno.h>
extern char **environ;
typedef struct {
    int id, state, exit_code, error;
    pid_t pid;
    const void *owner;
    bool notified;
} HjpTask;
enum { HJP_TASK_RUNNING = 1, HJP_TASK_SUCCEEDED, HJP_TASK_FAILED, HJP_TASK_CANCELLED };
static HjpTask hjp_tasks[32];
static int hjp_task_next_id = 1;

static HjpTask *hjp_task_find(int id, const void *owner) {
    for (size_t i = 0; i < 32; i++)
        if (hjp_tasks[i].id == id && hjp_tasks[i].owner == owner) return &hjp_tasks[i];
    return NULL;
}
static void hjp_task_poll(HjpTask *task) {
    if (!task || !task->pid) return;
    int status;
    pid_t done = waitpid(task->pid, &status, WNOHANG);
    if (done == 0 || (done < 0 && errno == EINTR)) return;
    task->pid = 0;
    if (done < 0) {
        task->state = HJP_TASK_FAILED; task->error = errno; return;
    }
    task->exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    task->state = task->exit_code == 0 ? HJP_TASK_SUCCEEDED : HJP_TASK_FAILED;
}
static int hjp_task_start(const void *owner, char *const *args) {
    HjpTask *slot = NULL;
    for (size_t i = 0; i < 32; i++) if (!hjp_tasks[i].id) { slot = &hjp_tasks[i]; break; }
    if (!slot || hjp_task_next_id == INT_MAX) return -1;
    posix_spawnattr_t attr;
    int error = posix_spawnattr_init(&attr);
    if (error) return -1;
    short flags = POSIX_SPAWN_SETPGROUP;
#ifdef POSIX_SPAWN_CLOEXEC_DEFAULT
    flags |= POSIX_SPAWN_CLOEXEC_DEFAULT;
#endif
    error = posix_spawnattr_setflags(&attr, flags);
    if (!error) error = posix_spawnattr_setpgroup(&attr, 0);
    pid_t pid = 0;
    if (!error) error = posix_spawnp(&pid, args[0], NULL, &attr, args, environ);
    posix_spawnattr_destroy(&attr);
    if (error) return -1;
    *slot = (HjpTask){.id = hjp_task_next_id++, .state = HJP_TASK_RUNNING,
                      .exit_code = -1, .pid = pid, .owner = owner};
    return slot->id;
}
static bool hjp_task_cancel(HjpTask *task) {
    if (!task || !task->pid) return false;
    hjp_task_poll(task);
    if (!task->pid) return false;
    /* プロセスグループ全体を停止してから必ず回収する。 */
    if (kill(-task->pid, SIGKILL) < 0 && errno != ESRCH) return false;
    int status;
    while (waitpid(task->pid, &status, 0) < 0) if (errno != EINTR) break;
    task->pid = 0;
    task->state = HJP_TASK_CANCELLED;
    return true;
}
static void hjp_tasks_close(const void *owner) {
    for (size_t i = 0; i < 32; i++) if (hjp_tasks[i].id && hjp_tasks[i].owner == owner) {
        hjp_task_cancel(&hjp_tasks[i]);
        memset(&hjp_tasks[i], 0, sizeof(hjp_tasks[i]));
    }
}
#endif
#endif
