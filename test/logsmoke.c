/* logsmoke.c - 冒烟验证 CapLog 的日志落盘（log_path 宽字符路径 + _wfopen，
 * 以及"一次记录一个 log\ 子目录里的时间戳文件"这条新规矩）。
 * 启动 CapsEnhance.exe -> 投递 WM_PROBE（等价 C+` 开始记录）-> 确认新建的
 * 会话日志里出现 "REC START" -> 把它撑到超上限 -> 再投递 WM_PROBE（结束
 * 记录，仍写同一文件）应自动轮转 -> 结束进程、清掉冒烟产物。 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>

/* 文件字节数（失败返回 -1） */
static long long file_size(const WCHAR* p) {
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(p, GetFileExInfoStandard, &d)) return -1;
    return ((long long)d.nFileSizeHigh << 32) | (long long)d.nFileSizeLow;
}

/* 删掉 log\ 里上一次冒烟留下的日志：本次要能分辨"是不是新建的文件"，
   目录必须起始为空（会话文件名带秒级时间戳，残留会让"最新的那份"判断失真）。 */
static void clear_logs(void) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(L"bin\\log\\CapsEnhance*.log", &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        WCHAR p[MAX_PATH];
        _snwprintf_s(p, MAX_PATH, _TRUNCATE, L"bin\\log\\%s", fd.cFileName);
        DeleteFileW(p);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

/* 找到本次记录新建的会话日志；有多份时取最后写的那份 */
static int find_session(WCHAR* out, size_t cap) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(L"bin\\log\\CapsEnhance-*.log", &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    FILETIME best = fd.ftLastWriteTime;
    _snwprintf_s(out, cap, _TRUNCATE, L"bin\\log\\%s", fd.cFileName);
    while (FindNextFileW(h, &fd)) {
        if (CompareFileTime(&fd.ftLastWriteTime, &best) > 0) {
            best = fd.ftLastWriteTime;
            _snwprintf_s(out, cap, _TRUNCATE, L"bin\\log\\%s", fd.cFileName);
        }
    }
    FindClose(h);
    return 1;
}

int main(void) {
    WCHAR exe[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH);
    WCHAR* slash = NULL;
    for (DWORD i = 0; i < n; i++) if (exe[i] == L'\\') slash = &exe[i];
    if (slash) *(slash + 1) = 0;
    wcscat_s(exe, MAX_PATH, L"CapsEnhance.exe");

    /* 每次都从干净的 log\ 开始 */
    clear_logs();

    STARTUPINFOW si; PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si)); si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));
    if (!CreateProcessW(exe, exe, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        wprintf(L"cannot start CapsEnhance.exe (path='%s') GetLastError=%lu\n",
                exe, GetLastError());
        return 2;
    }
    DWORD pid = pi.dwProcessId;      /* 只在下面那条失败信息里用得上 */
    CloseHandle(pi.hThread);

    HWND h = NULL;
    for (int i = 0; i < 30 && !h; i++) {
        h = FindWindowW(L"CapsEnhanceMain", NULL);
        if (!h) Sleep(100);
    }
    if (!h) {
        printf("no window (pid=%lu)\n", (unsigned long)pid);
        TerminateProcess(pi.hProcess, 2); CloseHandle(pi.hProcess);
        return 2;
    }
    Sleep(300);

    /* WM_PROBE = WM_APP+6：第一次投递 = C+` 开始记录，应新建一份带时间戳的会话日志 */
    PostMessageW(h, WM_APP + 6, 0, 0);
    Sleep(800);

    int ok = 1;
    WCHAR sess[MAX_PATH];
    if (find_session(sess, MAX_PATH)) {
        printf("[OK]   开始记录新建了会话日志 %ls\n", sess);
    } else {
        printf("[FAIL] log\\ 里没有时间戳会话日志（记录没开成新文件）\n");
        TerminateProcess(pi.hProcess, 0); CloseHandle(pi.hProcess);
        return 1;
    }

    FILE* f = NULL;
    int started = 0;
    if (_wfopen_s(&f, sess, L"r") == 0 && f) {
        char line[512];
        while (fgets(line, sizeof(line), f)) {
            if (strstr(line, "REC START")) { started = 1; break; }
        }
        fclose(f);
    }
    printf(started ? "[OK]   log written via wide path (_wfopen)\n"
                   : "[FAIL] log not written\n");
    if (!started) ok = 0;

    /* 轮转：把本次会话日志撑到超过 LogMaxKB（默认 2048），写真实文本行
       （约 3MB），而不是 NUL 填充——更接近实际日志 */
    FILE* big = NULL;
    if (_wfopen_s(&big, sess, L"a") == 0 && big) {
        for (int i = 0; i < 42000; i++)
            fprintf(big, "%08d padding line to grow the log file, xxxxxxxxxxxxxxxxxxxxxxxx\n", i);
        fputs("LOGROTATE-TAIL-MARKER last line of the old log\n", big);
        fclose(big);
    }
    long long before = file_size(sess);

    /* 第二次投递 = 结束记录：REC END 仍写同一个文件，这次写入应触发轮转 */
    PostMessageW(h, WM_APP + 6, 0, 0);
    Sleep(800);

    /* 轮转：超过 LogMaxKB 后应保留最近的一半，并留下标记行 */
    long long after = file_size(sess);
    int rotated = 0, keptTail = 0;
    FILE* f2 = NULL;
    if (_wfopen_s(&f2, sess, L"r") == 0 && f2) {
        char line[512];
        while (fgets(line, sizeof(line), f2)) {
            if (strstr(line, "日志已达上限")) rotated = 1;
            if (strstr(line, "LOGROTATE-TAIL-MARKER")) keptTail = 1;
        }
        fclose(f2);
    }
    int sizeOK = (before > 2 * 1024 * 1024) && (after > 0) && (after <= 2048LL * 1024);
    printf(sizeOK ? "[OK]   log rotated: %lld KB -> %lld KB (<= LogMaxKB)\n"
                  : "[FAIL] log size not trimmed (before=%lld after=%lld)\n",
           before / 1024, after / 1024, before, after);
    printf(rotated ? "[OK]   rotation marker written\n"
                   : "[FAIL] rotation marker missing\n");
    printf(keptTail ? "[OK]   recent content kept\n"
                    : "[FAIL] recent content lost\n");
    if (!sizeOK || !rotated || !keptTail) ok = 0;

    /* NavProbe 会弹 MessageBox 阻塞主线程，直接结束进程即可 */
    TerminateProcess(pi.hProcess, 0);
    CloseHandle(pi.hProcess);
    clear_logs();                   /* 清掉冒烟日志 */
    return ok ? 0 : 1;
}
