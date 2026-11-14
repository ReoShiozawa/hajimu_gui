#include <sys/wait.h>
static pid_t checked_waitpid(pid_t pid, int *status, int flags);
static int checked_kill(pid_t group, int signal);
#define HJP_TASK_WAITPID checked_waitpid
#define HJP_TASK_KILL checked_kill
#include "../src/hjp_tasks.h"
#include <assert.h>
#include <stdio.h>
#include <time.h>

static pid_t delayed_pid;
static int delay_polls;
static int kill_error;
static int checked_kill(pid_t group, int signal) {
    assert(group < 0 && signal == SIGKILL);
    if (kill_error) { errno = kill_error; return -1; }
    return kill(group, signal);
}
static pid_t checked_waitpid(pid_t pid, int *status, int flags) {
    assert(flags == WNOHANG);
    if (pid == delayed_pid && delay_polls > 0) { delay_polls--; return 0; }
    return waitpid(pid, status, flags);
}

static void finish(HjpTask *task) {
    for (int i = 0; i < 3000 && (task->pid || task->out.fd >= 0 || task->err.fd >= 0 || task->progress_fd >= 0); i++) {
        hjp_task_poll(task);
        struct timespec delay = {0, 1000000};
        nanosleep(&delay, NULL);
    }
    assert(task->pid == 0);
    assert(task->out.fd < 0 && task->err.fd < 0 && task->progress_fd < 0);
}

static int open_fds(void) {
    int count = 0;
    for (int fd = 0; fd < 512; fd++) if (fcntl(fd, F_GETFD) >= 0) count++;
    return count;
}
int main(void) {
    int owner = 0, other = 0;
    char *success[] = {"/bin/sh", "-c", "exit 0", NULL};
    char *failure[] = {"/bin/sh", "-c", "exit 7", NULL};
    char *slow[] = {"/bin/sleep", "10", NULL};
    char *missing[] = {"/not/a/real/executable", NULL};
    assert(hjp_task_start(&owner, missing) == -1);
    int id = hjp_task_start(&owner, success);
    assert(id > 0 && !hjp_task_find(id, &other));
    HjpTask *task = hjp_task_find(id, &owner);
    finish(task);
    assert(task->state == HJP_TASK_SUCCEEDED && task->exit_code == 0);
    assert(!hjp_task_cancel(task));
    id = hjp_task_start(&owner, failure);
    task = hjp_task_find(id, &owner);
    finish(task);
    assert(task->state == HJP_TASK_FAILED && task->exit_code == 7);
    id = hjp_task_start(&owner, slow);
    task = hjp_task_find(id, &owner);
    pid_t child = task->pid;
    delayed_pid = child; delay_polls = 10;
    kill_error = EPERM;
    assert(!hjp_task_cancel(task));
    assert(task->pid == child && task->state == HJP_TASK_RUNNING && task->error == EPERM);
    kill_error = 0;
    assert(hjp_task_cancel(task));
    assert(task->state == HJP_TASK_CANCELLING && task->pid == child && delay_polls == 8 && task->error == 0);
    assert(!hjp_task_cancel(task));
    finish(task);
    assert(task->state == HJP_TASK_CANCELLED && task->pid == 0);
    assert(waitpid(child, NULL, WNOHANG) == -1 && errno == ECHILD);
    char *group[] = {"/bin/sh", "-c", "sleep 30 & printf 'ready'; wait", NULL};
    id = hjp_task_start_with_io(&owner, group, true, false);
    task = hjp_task_find(id, &owner);
    assert(task);
    for (int i = 0; i < 1000 && !task->out.length; i++) {
        hjp_task_poll(task);
        struct timespec delay = {0, 1000000}; nanosleep(&delay, NULL);
    }
    assert(task->out.length == 5);
    assert(hjp_task_cancel(task));
    finish(task);
    assert(task->state == HJP_TASK_CANCELLED && task->out.eof && task->err.eof);
    int before_fds = open_fds();
    for (int i = 0; i < 30; i++) assert(hjp_task_start_with_io(&owner, missing, true, true) == -1);
    assert(open_fds() == before_fds);
    id = hjp_task_start_with_io(&owner, slow, true, true);
    task = hjp_task_find(id, &owner);
    child = task->pid;
    delayed_pid = child; delay_polls = 30;
    int close_error = 0;
    kill_error = EPERM;
    assert(hjp_tasks_close_step(&owner, &close_error) == -1 && close_error == EPERM);
    assert(hjp_task_find(id, &owner) == task && task->pid == child);
    kill_error = 0;
    assert(hjp_tasks_close_step(&owner, &close_error) == 0 && close_error == 0);
    assert(task->pid == child && task->state == HJP_TASK_CANCELLING);
    delay_polls = 0;
    int closed = 0;
    for (int i = 0; i < 3000 && !closed; i++) {
        closed = hjp_tasks_close_step(&owner, &close_error);
        assert(closed >= 0 && close_error == 0);
        struct timespec delay = {0, 1000000}; nanosleep(&delay, NULL);
    }
    assert(closed == 1 && !hjp_task_find(id, &owner) && open_fds() == before_fds);
    assert(waitpid(child, NULL, WNOHANG) == -1 && errno == ECHILD);
    const char *original_env = getenv("HAJIMU_PROGRESS_FD");
    char *captured[] = {"/bin/sh", "-c", "[ \"$HAJIMU_PROGRESS_FD\" = 3 ] || exit 91; printf '日本語\\n'; printf 'error\\n' >&2; printf '25.5\\n' >&3; sleep 0.1; printf '75\\n' >&3", NULL};
    id = hjp_task_start_with_io(&owner, captured, true, true);
    assert(id > 0 && getenv("HAJIMU_PROGRESS_FD") == original_env);
    task = hjp_task_find(id, &owner);
    bool intermediate = false;
    for (int i = 0; i < 1000 && task->pid; i++) {
        hjp_task_poll(task);
        if (task->pid && task->progress == 25.5) { intermediate = true; break; }
        struct timespec delay = {0, 1000000}; nanosleep(&delay, NULL);
    }
    assert(intermediate);
    finish(task);
    assert(task->state == HJP_TASK_SUCCEEDED && task->progress == 75);
    char text[HJP_TASK_OUTPUT_LIMIT * 3 + 1];
    hjp_task_output_text(&task->out, text);
    assert(!strcmp(text, "日本語\n"));
    hjp_task_output_text(&task->err, text);
    assert(!strcmp(text, "error\n"));
    hjp_tasks_close(&owner);
    assert(open_fds() == before_fds);
    char *flood[] = {"/bin/sh", "-c", "i=0; while [ $i -lt 6000 ]; do printf '0123456789abcdef'; printf 'fedcba9876543210' >&2; i=$((i+1)); done", NULL};
    id = hjp_task_start_with_io(&owner, flood, true, false);
    task = hjp_task_find(id, &owner);
    finish(task);
    assert(task->state == HJP_TASK_SUCCEEDED);
    assert(task->out.total == 96000 && task->err.total == 96000);
    assert(task->out.truncated && task->err.truncated);
    assert(task->out.length == HJP_TASK_OUTPUT_LIMIT && task->err.length == HJP_TASK_OUTPUT_LIMIT);
    hjp_task_output_text(&task->out, text);
    assert(strlen(text) == HJP_TASK_OUTPUT_LIMIT && !strcmp(text + strlen(text) - 16, "0123456789abcdef"));
    hjp_tasks_close(&owner);
    id = hjp_task_start_with_io(&owner, slow, true, true);
    task = hjp_task_find(id, &owner);
    assert(hjp_task_cancel(task));
    finish(task);
    assert(task->out.fd < 0 && task->err.fd < 0 && task->progress_fd < 0);
    hjp_tasks_close(&owner);
    assert(open_fds() == before_fds);
    HjpTaskOutput invalid = {.fd = -1, .length = 5, .bytes = {'a', 0, 0xff, 0xe3, 0x81}};
    hjp_task_output_text(&invalid, text);
    assert(!strcmp(text, "a\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd"));
    invalid.fd = 10; invalid.length = 3; memcpy(invalid.bytes, "a\xe3\x81", 3);
    hjp_task_output_text(&invalid, text);
    assert(!strcmp(text, "a"));
    invalid.fd = -1; invalid.length = HJP_TASK_OUTPUT_LIMIT;
    memset(invalid.bytes, 0xff, sizeof(invalid.bytes));
    assert(hjp_task_output_text(&invalid, text) == HJP_TASK_OUTPUT_LIMIT * 3);
    HjpTask parsing = {.progress = 42};
    const char *bad_progress[] = {"nan", "inf", "101", "-1", "0x10", "12x", "25,5"};
    for (size_t i = 0; i < sizeof(bad_progress) / sizeof(bad_progress[0]); i++) {
        snprintf(parsing.progress_line, sizeof(parsing.progress_line), "%s", bad_progress[i]);
        parsing.progress_length = strlen(parsing.progress_line);
        hjp_task_progress_line(&parsing);
        assert(parsing.progress == 42);
    }
    child = fork();
    assert(child >= 0);
    if (!child) {
        close(0); close(1); close(2);
        int isolated = hjp_task_start_with_io(&other, captured, true, true);
        assert(isolated > 0);
        HjpTask *closed_standard = hjp_task_find(isolated, &other);
        finish(closed_standard);
        assert(closed_standard->exit_code == 0 && closed_standard->progress == 75);
        hjp_tasks_close(&other);
        _exit(0);
    }
    int child_status;
    assert(waitpid(child, &child_status, 0) == child && WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0);
    hjp_tasks_close(&owner);
    assert(!hjp_task_find(id, &owner));
    for (int i = 0; i < 32; i++) assert(hjp_task_start(&owner, success) > 0);
    assert(hjp_task_start(&owner, success) == -1);
    hjp_tasks_close(&owner);
    id = hjp_task_start(&owner, slow);
    child = hjp_task_find(id, &owner)->pid;
    hjp_tasks_close(&owner);
    assert(waitpid(child, NULL, WNOHANG) == -1 && errno == ECHILD);
    puts("tasks passed: output/progress, UTF-8, bounds, fd cleanup, closed standard fds, state, cancellation, shutdown");
}
