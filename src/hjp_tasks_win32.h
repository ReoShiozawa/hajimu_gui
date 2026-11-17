#ifndef HJP_TASKS_WIN32_H
#define HJP_TASKS_WIN32_H
#include <stdio.h>
#include <wchar.h>

#ifndef HJP_TASK_CREATE_PROCESS
#define HJP_TASK_CREATE_PROCESS CreateProcessW
#endif
#ifndef HJP_TASK_ASSIGN_JOB
#define HJP_TASK_ASSIGN_JOB AssignProcessToJobObject
#endif
#ifndef HJP_TASK_RESUME_THREAD
#define HJP_TASK_RESUME_THREAD ResumeThread
#endif
#ifndef HJP_TASK_TERMINATE_JOB
#define HJP_TASK_TERMINATE_JOB TerminateJobObject
#endif

static wchar_t *hjp_task_wide(const char *text) {
    int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, NULL, 0);
    if (size <= 0 || size > 32767) return NULL;
    wchar_t *wide = malloc((size_t)size * sizeof(*wide));
    if (wide && !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, wide, size)) {
        free(wide); return NULL;
    }
    return wide;
}
static char *hjp_task_executable_path(void) {
    wchar_t *wide = calloc(32768, sizeof(*wide));
    if (!wide) return NULL;
    DWORD length = GetModuleFileNameW(NULL, wide, 32768);
    int size = length && length < 32768 ? WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1, NULL, 0, NULL, NULL) : 0;
    char *text = size > 0 ? malloc((size_t)size) : NULL;
    if (text && !WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1, text, size, NULL, NULL)) {
        free(text); text = NULL;
    }
    free(wide); return text;
}
static bool hjp_task_command_char(wchar_t *command, size_t *length, wchar_t c) {
    if (*length >= 32766) return false;
    command[(*length)++] = c;
    return true;
}
/* MSCRTの引用規則。シェルを使わず、空引数・引用符・末尾の逆斜線を保持する。 */
static wchar_t *hjp_task_command(char *const *args) {
    wchar_t *command = malloc(32767 * sizeof(*command));
    if (!command) return NULL;
    size_t length = 0;
    for (size_t i = 0; args[i]; i++) {
        if (i >= 65) goto fail;
        wchar_t *word = hjp_task_wide(args[i]);
        if (!word) goto fail;
        bool ok = (!i || hjp_task_command_char(command, &length, L' ')) &&
                  hjp_task_command_char(command, &length, L'"');
        const wchar_t *p = word;
        while (ok && *p) {
            size_t slashes = 0;
            while (*p == L'\\') { slashes++; p++; }
            size_t count = *p == L'"' || !*p ? slashes * 2 : slashes;
            if (*p == L'"') count++;
            while (ok && count--) ok = hjp_task_command_char(command, &length, L'\\');
            if (*p) ok = ok && hjp_task_command_char(command, &length, *p++);
        }
        ok = ok && hjp_task_command_char(command, &length, L'"');
        free(word);
        if (!ok) goto fail;
    }
    command[length] = 0;
    return command;
fail:
    free(command); return NULL;
}
static int hjp_task_env_order(const void *left, const void *right) {
    return CompareStringOrdinal(*(const wchar_t *const *)left, -1,
        *(const wchar_t *const *)right, -1, TRUE) - CSTR_EQUAL;
}
static wchar_t *hjp_task_environment(HANDLE progress) {
    wchar_t *source = GetEnvironmentStringsW();
    if (!source) return NULL;
    size_t count = 0, size = 2;
    for (const wchar_t *p = source; *p; p += wcslen(p) + 1) {
        size += wcslen(p) + 1; count++;
        if (size > 8 * 1024 * 1024) { FreeEnvironmentStringsW(source); return NULL; }
    }
    wchar_t extra[80];
    swprintf(extra, sizeof(extra) / sizeof(*extra), L"HAJIMU_PROGRESS_HANDLE=%llu", (unsigned long long)(uintptr_t)progress);
    size += wcslen(extra) + 1;
    wchar_t **entries = calloc(count + 1, sizeof(*entries));
    wchar_t *block = calloc(size, sizeof(*block));
    if (!entries || !block) { free(entries); free(block); FreeEnvironmentStringsW(source); return NULL; }
    size_t used = 0;
    for (wchar_t *p = source; *p; p += wcslen(p) + 1) {
        if (_wcsnicmp(p, L"HAJIMU_PROGRESS_FD=", 19) &&
            _wcsnicmp(p, L"HAJIMU_PROGRESS_HANDLE=", 23)) entries[used++] = p;
    }
    if (progress) entries[used++] = extra;
    qsort(entries, used, sizeof(*entries), hjp_task_env_order);
    wchar_t *at = block;
    for (size_t i = 0; i < used; i++) {
        size_t length = wcslen(entries[i]) + 1;
        memcpy(at, entries[i], length * sizeof(*at)); at += length;
    }
    free(entries); FreeEnvironmentStringsW(source);
    return block;
}
static bool hjp_task_pipe(HANDLE pair[2]) {
    SECURITY_ATTRIBUTES security = {sizeof(security), NULL, TRUE};
    if (!CreatePipe(&pair[0], &pair[1], &security, 0)) return false;
    if (SetHandleInformation(pair[0], HANDLE_FLAG_INHERIT, 0)) return true;
    hjp_task_close_fd(&pair[0]); hjp_task_close_fd(&pair[1]); return false;
}
static HANDLE hjp_task_standard(DWORD kind, bool input) {
    HANDLE source = GetStdHandle(kind), copy = NULL;
    if (hjp_task_pipe_open(source)) {
        if (DuplicateHandle(GetCurrentProcess(), source, GetCurrentProcess(), &copy, 0, TRUE, DUPLICATE_SAME_ACCESS)) return copy;
        return NULL;
    }
    SECURITY_ATTRIBUTES security = {sizeof(security), NULL, TRUE};
    copy = CreateFileW(L"NUL", input ? GENERIC_READ : GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
        &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    return copy == INVALID_HANDLE_VALUE ? NULL : copy;
}
static void hjp_task_poll(HjpTask *task) {
    if (!task || !task->id) return;
    hjp_task_read_output(&task->out); hjp_task_read_output(&task->err); hjp_task_read_progress(task);
    if (!task->pid) return;
    DWORD wait = WaitForSingleObject(task->pid, 0);
    if (wait == WAIT_TIMEOUT) return;
    if (wait != WAIT_OBJECT_0) { task->error = (int)GetLastError(); return; }
    DWORD code;
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION info;
    if (!GetExitCodeProcess(task->pid, &code) ||
        !QueryInformationJobObject(task->job, JobObjectBasicAccountingInformation, &info, sizeof(info), NULL)) {
        task->error = (int)GetLastError(); return;
    }
    /* 親が先に終わっても、子孫が残る間は取消と終了待ちの所有権を維持する。 */
    if (info.ActiveProcesses) return;
    hjp_task_close_fd(&task->pid); hjp_task_close_fd(&task->job);
    task->exit_code = code; task->error = 0;
    task->state = task->cancel_requested ? HJP_TASK_CANCELLED : code == 0 ? HJP_TASK_SUCCEEDED : HJP_TASK_FAILED;
}
static int hjp_task_start_with_io(const void *owner, char *const *args, bool capture, bool progress) {
    if (!args || !args[0] || !args[0][0]) return -1;
    HjpTask *slot = NULL;
    for (size_t i = 0; i < 32; i++) if (!hjp_tasks[i].id) { slot = &hjp_tasks[i]; break; }
    if (!slot || hjp_task_next_id == INT_MAX) return -1;
    wchar_t *command = hjp_task_command(args), *name = hjp_task_wide(args[0]), *environment = NULL;
    wchar_t *executable = calloc(32768, sizeof(*executable));
    HANDLE pipes[3][2] = {{0}}, standard[3] = {0}, job = NULL;
    PROCESS_INFORMATION process = {0};
    STARTUPINFOEXW startup = {0};
    SIZE_T attribute_size = 0;
    bool attribute_ready = false;
    if (!command || !name || !executable) goto cleanup;
    DWORD path_length = SearchPathW(NULL, name, L".exe", 32768, executable, NULL);
    if (!path_length || path_length >= 32768) goto cleanup;
    for (int i = 0; i < 3; i++) if ((i < 2 ? capture : progress) && !hjp_task_pipe(pipes[i])) goto cleanup;
    /* 標準入力はNUL。親の端末から入力を奪わない。 */
    SECURITY_ATTRIBUTES security = {sizeof(security), NULL, TRUE};
    standard[0] = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (!hjp_task_pipe_open(standard[0])) goto cleanup;
    for (int i = 1; i < 3; i++) {
        standard[i] = capture ? pipes[i - 1][1] : hjp_task_standard(i == 1 ? STD_OUTPUT_HANDLE : STD_ERROR_HANDLE, false);
        if (!hjp_task_pipe_open(standard[i])) goto cleanup;
    }
    HANDLE inherited[] = {standard[0], standard[1], standard[2], pipes[2][1]};
    InitializeProcThreadAttributeList(NULL, 1, 0, &attribute_size);
    startup.lpAttributeList = malloc(attribute_size);
    if (!startup.lpAttributeList || !InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &attribute_size)) goto cleanup;
    attribute_ready = true;
    if (!UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
        inherited, (progress ? 4 : 3) * sizeof(HANDLE), NULL, NULL)) goto cleanup;
    environment = hjp_task_environment(pipes[2][1]);
    if (!environment) goto cleanup;
    job = CreateJobObjectW(NULL, NULL);
    if (!job) goto cleanup;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {0};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) goto cleanup;
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = standard[0]; startup.StartupInfo.hStdOutput = standard[1]; startup.StartupInfo.hStdError = standard[2];
    if (!HJP_TASK_CREATE_PROCESS(executable, command, NULL, NULL, TRUE,
        CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT,
        environment, NULL, &startup.StartupInfo, &process)) goto cleanup;
    /* 実行前にJobへ収容する。失敗時は追跡不能なプロセスとして実行しない。 */
    if (!HJP_TASK_ASSIGN_JOB(job, process.hProcess) || HJP_TASK_RESUME_THREAD(process.hThread) == (DWORD)-1) goto cleanup;
    *slot = (HjpTask){.id = hjp_task_next_id++, .state = HJP_TASK_RUNNING,
        .exit_code = -1, .pid = process.hProcess, .job = job, .owner = owner, .progress = -1,
        .capture_output = capture, .out.fd = pipes[0][0], .err.fd = pipes[1][0], .progress_fd = pipes[2][0]};
    process.hProcess = NULL; job = NULL;
    pipes[0][0] = pipes[1][0] = pipes[2][0] = NULL;
cleanup:
    if (process.hProcess) {
        TerminateProcess(process.hProcess, ERROR_CANCELLED);
        WaitForSingleObject(process.hProcess, INFINITE);
    }
    hjp_task_close_fd(&process.hProcess); hjp_task_close_fd(&process.hThread); hjp_task_close_fd(&job);
    for (int i = 0; i < 3; i++) {
        if (capture && i > 0) standard[i] = NULL;
        hjp_task_close_fd(&standard[i]);
        hjp_task_close_fd(&pipes[i][0]); hjp_task_close_fd(&pipes[i][1]);
    }
    if (attribute_ready) DeleteProcThreadAttributeList(startup.lpAttributeList);
    free(startup.lpAttributeList); free(environment); free(command); free(name); free(executable);
    return slot->id ? slot->id : -1;
}
static bool hjp_task_cancel(HjpTask *task) {
    if (!task || !task->pid || task->cancel_requested) return false;
    hjp_task_poll(task);
    if (!task->pid) return false;
    if (!HJP_TASK_TERMINATE_JOB(task->job, ERROR_CANCELLED)) { task->error = (int)GetLastError(); return false; }
    task->cancel_requested = true; task->state = HJP_TASK_CANCELLING; task->error = 0;
    return true;
}
#endif
