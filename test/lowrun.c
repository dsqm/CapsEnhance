/* lowrun：以"普通用户完整性级别（Medium）"启动指定程序并等待其结束。
   用法： lowrun <exe> [args...]
   原理：拿当前用户 shell（explorer.exe，Medium IL）的进程令牌复制成主令牌，
   再 CreateProcessAsUser。本程序自己通常是 High（管理员），
   用于验证"管理员权限下 COM 附着失败 / 普通权限成功"这类权限相关问题。
   子进程的 stdout/stderr 直接继承（可重定向到文件）。纯 ASCII。 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <userenv.h>
#include <stdio.h>
#include <string.h>

#pragma comment(lib, "userenv.lib")

static DWORD FindExplorer(void) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe; memset(&pe, 0, sizeof(pe)); pe.dwSize = sizeof(pe);
    DWORD found = 0;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"explorer.exe") == 0) {
                /* PROCESSENTRY32W 没有会话字段：改查进程令牌所属会话 */
                DWORD sid = 0;
                if (ProcessIdToSessionId(pe.th32ProcessID, &sid) && sid != 0) {
                    found = pe.th32ProcessID;
                    break;
                }
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) { fwprintf(stderr, L"usage: lowrun <exe> [args...]\n"); return 2; }

    WCHAR cmd[4096] = L"";
    for (int i = 1; i < argc; i++) {
        if (i > 1) wcscat_s(cmd, 4096, L" ");
        wcscat_s(cmd, 4096, argv[i]);
    }

    DWORD pid = FindExplorer();
    if (!pid) { fwprintf(stderr, L"explorer.exe not found\n"); return 3; }
    HANDLE hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hp) {
        /* 有时需要更多权限 */
        hp = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);
    }
    if (!hp) { fwprintf(stderr, L"OpenProcess(PID %lu) FAIL err=%lu\n", pid, GetLastError()); return 4; }

    HANDLE tok = NULL;
    if (!OpenProcessToken(hp, TOKEN_DUPLICATE | TOKEN_QUERY | TOKEN_ASSIGN_PRIMARY, &tok)) {
        fwprintf(stderr, L"OpenProcessToken FAIL err=%lu\n", GetLastError());
        return 5;
    }
    HANDLE dup = NULL;
    if (!DuplicateTokenEx(tok, TOKEN_ALL_ACCESS, NULL, SecurityImpersonation,
                          TokenPrimary, &dup)) {
        fwprintf(stderr, L"DuplicateTokenEx FAIL err=%lu\n", GetLastError());
        return 6;
    }
    LPVOID env = NULL;
    CreateEnvironmentBlock(&env, dup, FALSE);

    STARTUPINFOW si; PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si)); si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));
    /* CreateProcessAsUser 需要 SeAssignPrimaryTokenPrivilege（管理员默认**没有**，
       报 1314）；CreateProcessWithTokenW 只需 SeImpersonatePrivilege（管理员有）。 */
    BOOL ok = CreateProcessWithTokenW(dup, LOGON_WITH_PROFILE, NULL, cmd,
                                      CREATE_UNICODE_ENVIRONMENT, env, NULL, &si, &pi);
    if (!ok) {
        SetLastError(0);
        ok = CreateProcessAsUserW(dup, NULL, cmd, NULL, NULL, TRUE,
                                  CREATE_UNICODE_ENVIRONMENT, env, NULL, &si, &pi);
    }
    if (!ok) {
        fwprintf(stderr, L"CreateProcessWithToken/AsUser FAIL err=%lu (elevation required?)\n", GetLastError());
        return 7;
    }
    WaitForSingleObject(pi.hProcess, 60000);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    fwprintf(stderr, L"[lowrun] child exited with %lu\n", (unsigned long)code);
    if (env) DestroyEnvironmentBlock(env);
    CloseHandle(dup); CloseHandle(tok); CloseHandle(hp);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    return 0;
}
