/* ilcheck：打印指定 PID（或命令行给的 exe 名的所有进程）的完整性级别/位数/权限。
   用途：判定 WPS 是否以管理员（High IL）运行 —— 跨完整性级别时 COM 的
   GetActiveObject/ROT 会被系统屏蔽（返回 MK_E_UNAVAILABLE），这是排查这类
   "附着不上运行中实例"问题的关键一条。纯 ASCII。 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static FILE* g_out = NULL;
static void P(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vfprintf(stdout, fmt, ap); va_end(ap);
    if (g_out) { va_start(ap, fmt); vfprintf(g_out, fmt, ap); va_end(ap); }
    fflush(stdout);
    if (g_out) fflush(g_out);
}

static void Report(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) {
        P("PID %lu: OpenProcess(QUERY_LIMITED) FAIL err=%lu  "
          "(higher-integrity process?)\n", (unsigned long)pid, GetLastError());
        return;
    }
    WCHAR path[512] = L"";
    DWORD n = 512;
    QueryFullProcessImageNameW(h, 0, path, &n);

    BOOL isWow = FALSE;
    IsWow64Process(h, &isWow);

    HANDLE tok = NULL;
    const char* il = "unknown";
    char ilbuf[32] = "";
    if (OpenProcessToken(h, TOKEN_QUERY, &tok)) {
        DWORD need = 0;
        GetTokenInformation(tok, TokenIntegrityLevel, NULL, 0, &need);
        BYTE* buf = (BYTE*)malloc(need ? need : 256);
        if (buf && GetTokenInformation(tok, TokenIntegrityLevel, buf, need, &need)) {
            TOKEN_MANDATORY_LABEL* tml = (TOKEN_MANDATORY_LABEL*)buf;
            DWORD rid = *GetSidSubAuthority(tml->Label.Sid,
                            *GetSidSubAuthorityCount(tml->Label.Sid) - 1);
            if (rid >= SECURITY_MANDATORY_PROTECTED_PROCESS_RID) il = "PROTECTED";
            else if (rid >= SECURITY_MANDATORY_SYSTEM_RID) il = "SYSTEM";
            else if (rid >= SECURITY_MANDATORY_HIGH_RID) il = "HIGH(elevated)";
            else if (rid >= SECURITY_MANDATORY_MEDIUM_RID) il = "MEDIUM(normal user)";
            else il = "LOW";
            sprintf_s(ilbuf, sizeof(ilbuf), "0x%04lX", (unsigned long)rid);
        }
        free(buf);
        CloseHandle(tok);
    }
    P("PID %-6lu bits=%s integrity=%s (%s)\n     %ls\n",
      (unsigned long)pid, isWow ? "32" : "64", il, ilbuf, path);
    CloseHandle(h);
}

int main(int argc, char** argv) {
    fopen_s(&g_out, "ilcheck.out", "wb");
    P("self: ");
    Report(GetCurrentProcessId());

    if (argc < 2) {
        /* 没有参数：把 wps/winword/et/wpp 相关进程都列一遍 */
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap != INVALID_HANDLE_VALUE) {
            PROCESSENTRY32W pe; memset(&pe, 0, sizeof(pe)); pe.dwSize = sizeof(pe);
            if (Process32FirstW(snap, &pe)) {
                do {
                    if (_wcsicmp(pe.szExeFile, L"wps.exe") == 0 ||
                        _wcsicmp(pe.szExeFile, L"wpscloudsvr.exe") == 0 ||
                        _wcsicmp(pe.szExeFile, L"et.exe") == 0 ||
                        _wcsicmp(pe.szExeFile, L"wpp.exe") == 0 ||
                        _wcsicmp(pe.szExeFile, L"winword.exe") == 0 ||
                        _wcsicmp(pe.szExeFile, L"CapsEnhance.exe") == 0) {
                        P("---- %ls ----\n", pe.szExeFile);
                        Report(pe.th32ProcessID);
                    }
                } while (Process32NextW(snap, &pe));
            }
            CloseHandle(snap);
        }
    } else {
        for (int i = 1; i < argc; i++) Report((DWORD)strtoul(argv[i], NULL, 10));
    }
    if (g_out) fclose(g_out);
    return 0;
}
