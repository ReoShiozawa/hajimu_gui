#include "../src/hjp_tasks.h"
#include <assert.h>
#include <stdio.h>
#include <time.h>

static void finish(HjpTask *task) {
    for (int i = 0; i < 3000 && task->pid; i++) {
        hjp_task_poll(task);
        struct timespec delay = {0, 1000000};
        nanosleep(&delay, NULL);
    }
    assert(task->pid == 0);
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
    assert(hjp_task_cancel(task));
    assert(task->state == HJP_TASK_CANCELLED && task->pid == 0);
    assert(waitpid(child, NULL, WNOHANG) == -1 && errno == ECHILD);
    hjp_tasks_close(&owner);
    assert(!hjp_task_find(id, &owner));
    for (int i = 0; i < 32; i++) assert(hjp_task_start(&owner, success) > 0);
    assert(hjp_task_start(&owner, success) == -1);
    hjp_tasks_close(&owner);
    id = hjp_task_start(&owner, slow);
    child = hjp_task_find(id, &owner)->pid;
    hjp_tasks_close(&owner);
    assert(waitpid(child, NULL, WNOHANG) == -1 && errno == ECHILD);
    puts("tasks passed: success/failure, spawn error, ownership, cancellation/reaping, capacity, shutdown");
}
