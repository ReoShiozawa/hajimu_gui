#include <windows.h>
#include <assert.h>
#include <stdio.h>
#include <wchar.h>
#include <stdint.h>
#include <stdlib.h>

static int failure_stage;
static BOOL assign_job(HANDLE job, HANDLE process) {
    if (failure_stage == 1) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
    return AssignProcessToJobObject(job, process);
}
static DWORD resume_thread(HANDLE thread) {
    if (failure_stage == 2) { SetLastError(ERROR_ACCESS_DENIED); return (DWORD)-1; }
    return ResumeThread(thread);
}
static BOOL terminate_job(HANDLE job, UINT code) {
    if (failure_stage == 3) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
    return TerminateJobObject(job, code);
}
#define HJP_TASK_ASSIGN_JOB assign_job
#define HJP_TASK_RESUME_THREAD resume_thread
#define HJP_TASK_TERMINATE_JOB terminate_job
#include "../src/hjp_tasks.h"

static void write_bytes(HANDLE handle, const void *bytes, DWORD size) {
    DWORD done;
    assert(WriteFile(handle, bytes, size, &done, NULL) && done == size);
}
static int child(int argc, wchar_t **argv) {
    if (!wcscmp(argv[1], L"--args")) {
        assert(argc == 8);
        assert(!wcscmp(argv[2], L""));
        assert(!wcscmp(argv[3], L"日本語 空白"));
        assert(!wcscmp(argv[4], L"quote\"inside"));
        assert(!wcscmp(argv[5], L"C:\\末尾の空白 \\"));
        assert(!wcscmp(argv[6], L"\\\\\""));
        assert(!wcscmp(argv[7], L"&echo not-a-shell|{};%PATH%"));
        return 0;
    }
    if (!wcscmp(argv[1], L"--exit")) return (int)wcstoul(argv[2], NULL, 10);
    if (!wcscmp(argv[1], L"--sleep")) { Sleep(30000); return 0; }
    if (!wcscmp(argv[1], L"--inherit")) {
        HANDLE unrelated = (HANDLE)(uintptr_t)_wcstoui64(argv[2], NULL, 10);
        SetEvent(unrelated);
        return 0;
    }
    if (!wcscmp(argv[1], L"--descendant")) {
        wchar_t command[32768];
        swprintf(command, 32768, L"\"%ls\" --sleep", argv[0]);
        STARTUPINFOW startup = {0}; startup.cb = sizeof(startup);
        PROCESS_INFORMATION process;
        assert(CreateProcessW(argv[0], command, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &startup, &process));
        char pid[40]; int size = snprintf(pid, sizeof(pid), "%lu\n", (unsigned long)process.dwProcessId);
        write_bytes(GetStdHandle(STD_OUTPUT_HANDLE), pid, (DWORD)size);
        CloseHandle(process.hProcess); CloseHandle(process.hThread);
        return 0;
    }
    if (!wcscmp(argv[1], L"--io")) {
        wchar_t value[80];
        assert(!GetEnvironmentVariableW(L"HAJIMU_PROGRESS_FD", value, 80));
        assert(GetEnvironmentVariableW(L"HAJIMU_PROGRESS_HANDLE", value, 80));
        HANDLE progress = (HANDLE)(uintptr_t)_wcstoui64(value, NULL, 10);
        char block[8192]; memset(block, 'x', sizeof(block));
        for (int i = 0; i < 16; i++) {
            write_bytes(GetStdHandle(STD_OUTPUT_HANDLE), block, sizeof(block));
            write_bytes(GetStdHandle(STD_ERROR_HANDLE), block, sizeof(block));
        }
        const unsigned char tail[] = {'E', 'N', 'D', 0, 0xff, 0xe6, 0x97, 0xa5};
        write_bytes(GetStdHandle(STD_OUTPUT_HANDLE), tail, sizeof(tail));
        write_bytes(GetStdHandle(STD_ERROR_HANDLE), "ERR", 3);
        const char report[] = "12\njunk\n101\n33.5\n";
        write_bytes(progress, report, sizeof(report) - 1);
        return 0;
    }
    if (!wcscmp(argv[1], L"--env")) {
        wchar_t value[80];
        assert(!GetEnvironmentVariableW(L"HAJIMU_PROGRESS_FD", value, 80));
        assert(!GetEnvironmentVariableW(L"HAJIMU_PROGRESS_HANDLE", value, 80));
        return 0;
    }
    assert(!"unknown child mode"); return 1;
}
static void wait_task(HjpTask *task) {
    ULONGLONG start = GetTickCount64();
    do {
        hjp_task_poll(task);
        assert(GetTickCount64() - start < 15000);
        Sleep(1);
    } while (task->pid || hjp_task_pipe_open(task->out.fd) || hjp_task_pipe_open(task->err.fd) || hjp_task_pipe_open(task->progress_fd));
}
static void release_task(HjpTask *task) {
    assert(!task->pid && !task->job);
    hjp_task_close_io(task); memset(task, 0, sizeof(*task));
}
static HjpTask *start_task(char *const *args, bool capture, bool progress) {
    int id = hjp_task_start_with_io(&failure_stage, args, capture, progress);
    assert(id > 0);
    assert(!hjp_task_find(id, args));
    HjpTask *task = hjp_task_find(id, &failure_stage);
    assert(task && task->state == HJP_TASK_RUNNING);
    return task;
}
static char *utf8(const wchar_t *wide) {
    int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1, NULL, 0, NULL, NULL);
    assert(size > 0);
    char *text = malloc((size_t)size);
    assert(text && WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1, text, size, NULL, NULL));
    return text;
}
int wmain(int argc, wchar_t **argv) {
    if (argc > 1) return child(argc, argv);
    wchar_t self[32768], directory[32768], copied[32768];
    assert(GetModuleFileNameW(NULL, self, 32768));
    DWORD size = GetTempPathW(32000, directory); assert(size && size < 32000);
    swprintf(directory + size, 32768 - size, L"hajimu-tasks-%lu", (unsigned long)GetCurrentProcessId());
    assert(CreateDirectoryW(directory, NULL));
    swprintf(copied, 32768, L"%ls\\日本語 空白.exe", directory);
    assert(CopyFileW(self, copied, TRUE));
    char *executable = utf8(copied);
    puts("Windows task lifetime tests starting");
    DWORD baseline; assert(GetProcessHandleCount(GetCurrentProcess(), &baseline));
    assert(SetEnvironmentVariableW(L"HAJIMU_PROGRESS_FD", L"123"));
    assert(SetEnvironmentVariableW(L"HAJIMU_PROGRESS_HANDLE", L"456"));

    char *arguments[] = {executable, "--args", "", "日本語 空白", "quote\"inside", "C:\\末尾の空白 \\", "\\\\\"", "&echo not-a-shell|{};%PATH%", NULL};
    HjpTask *task = start_task(arguments, true, false); wait_task(task);
    assert(task->state == HJP_TASK_SUCCEEDED); release_task(task);
    DWORD after_first; assert(GetProcessHandleCount(GetCurrentProcess(), &after_first));
    fprintf(stderr, "handles baseline=%lu first-task=%lu\n", (unsigned long)baseline, (unsigned long)after_first);
    char *env[] = {executable, "--env", NULL};
    task = start_task(env, false, false); wait_task(task); assert(task->exit_code == 0); release_task(task);
    char *io[] = {executable, "--io", NULL};
    task = start_task(io, true, true); wait_task(task);
    assert(task->state == HJP_TASK_SUCCEEDED && task->progress == 33.5);
    assert(task->out.total == 131080 && task->err.total == 131075);
    assert(task->out.length == HJP_TASK_OUTPUT_LIMIT && task->out.truncated && task->err.truncated);
    assert(task->out.eof && task->err.eof && !task->out.error && !task->err.error);
    char text[HJP_TASK_OUTPUT_LIMIT * 3 + 1]; hjp_task_output_text(&task->out, text);
    assert(strstr(text, "END\xef\xbf\xbd\xef\xbf\xbd\xe6\x97\xa5")); release_task(task);
    char *exit_args[] = {executable, "--exit", "259", NULL};
    task = start_task(exit_args, true, false); wait_task(task);
    assert(task->exit_code == 259 && task->state == HJP_TASK_FAILED); release_task(task);
    exit_args[2] = "4026531841";
    task = start_task(exit_args, true, false); wait_task(task);
    assert(task->exit_code == INT64_C(4026531841)); release_task(task);

    SECURITY_ATTRIBUTES security = {sizeof(security), NULL, TRUE};
    HANDLE event = CreateEventW(&security, TRUE, FALSE, NULL); assert(event);
    char value[40]; snprintf(value, sizeof(value), "%llu", (unsigned long long)(uintptr_t)event);
    char *inherit[] = {executable, "--inherit", value, NULL};
    task = start_task(inherit, true, false); wait_task(task);
    assert(task->exit_code == 0 && WaitForSingleObject(event, 0) == WAIT_TIMEOUT);
    release_task(task); CloseHandle(event);

    char *descendant[] = {executable, "--descendant", NULL};
    task = start_task(descendant, true, false);
    ULONGLONG start = GetTickCount64();
    do { hjp_task_poll(task); Sleep(1); assert(GetTickCount64() - start < 15000); }
    while (!task->out.length || WaitForSingleObject(task->pid, 0) != WAIT_OBJECT_0);
    assert(task->pid && task->state == HJP_TASK_RUNNING);
    hjp_task_output_text(&task->out, text);
    DWORD descendant_pid = (DWORD)strtoul(text, NULL, 10);
    HANDLE descendant_process = OpenProcess(SYNCHRONIZE, FALSE, descendant_pid); assert(descendant_process);
    failure_stage = 3;
    assert(!hjp_task_cancel(task) && !task->cancel_requested && task->pid && task->error == ERROR_ACCESS_DENIED);
    failure_stage = 0;
    assert(hjp_task_cancel(task) && task->state == HJP_TASK_CANCELLING);
    wait_task(task); assert(task->state == HJP_TASK_CANCELLED);
    assert(WaitForSingleObject(descendant_process, 0) == WAIT_OBJECT_0);
    CloseHandle(descendant_process); release_task(task);

    char *sleep_args[] = {executable, "--sleep", NULL};
    task = start_task(sleep_args, true, false);
    int error;
    start = GetTickCount64();
    while (hjp_tasks_close_step(&failure_stage, &error) != 1) {
        assert(!error && GetTickCount64() - start < 15000); Sleep(1);
    }
    assert(!task->id);
    char *missing[] = {"Z:\\nonexistent-hajimu-test.exe", NULL};
    assert(hjp_task_start_with_io(&failure_stage, missing, true, false) == -1);
    char *invalid[] = {executable, "\xff", NULL};
    assert(hjp_task_start_with_io(&failure_stage, invalid, true, false) == -1);
    char *long_word = malloc(32768); assert(long_word); memset(long_word, 'x', 32767); long_word[32767] = 0;
    invalid[1] = long_word; assert(hjp_task_start_with_io(&failure_stage, invalid, true, false) == -1); free(long_word);
    for (int stage = 1; stage <= 2; stage++) {
        failure_stage = stage;
        assert(hjp_task_start_with_io(&failure_stage, sleep_args, true, true) == -1);
    }
    failure_stage = 0;
    for (int i = 0; i < 30; i++) {
        task = start_task(env, true, false); wait_task(task); assert(!task->exit_code); release_task(task);
        DWORD count; assert(GetProcessHandleCount(GetCurrentProcess(), &count));
        fprintf(stderr, "handles repetition=%d count=%lu\n", i, (unsigned long)count);
    }
    hjp_tasks_close(&failure_stage);
    DWORD final; assert(GetProcessHandleCount(GetCurrentProcess(), &final));
    fprintf(stderr, "handles final=%lu baseline=%lu\n", (unsigned long)final, (unsigned long)baseline);
    assert(final == baseline);
    wchar_t unchanged[80]; assert(GetEnvironmentVariableW(L"HAJIMU_PROGRESS_HANDLE", unchanged, 80) && !wcscmp(unchanged, L"456"));
    free(executable); assert(DeleteFileW(copied)); assert(RemoveDirectoryW(directory));
    puts("Windows tasks passed: Unicode argv, bounded output, progress, descendants, cancellation retry, startup failures and handle lifetime");
    return 0;
}
