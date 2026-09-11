/* traymemtest.c - 定位"右键托盘图标内存涨到 ~21MB"的来源。
 *
 * 流程：
 *   1. 启动 CapsEnhance.exe，等主窗口出现
 *   2. 快照：工作集 + 已加载模块列表（模块基址+大小）
 *   3. 向主窗口投递托盘回调消息（等价于托盘右键），弹菜单后发 ESC 关闭
 *   4. 再快照，打印工作集增量与"新增加载的模块"——模块名单直接指认元凶
 *   5. 发 WM_CLOSE 让实例干净退出
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#pragma comment(lib, "psapi.lib")

/* cap.h 的消息常量（WM_APP+5 = 托盘回调）直接用数值，避免链 main.c */
#ifndef WM_TOGGLE_TRAY
#define WM_TOGGLE_TRAY (WM_APP + 5)
#endif

static int fails = 0;
#define EXPECT(cond, name) do { \
    if (cond) printf("  [OK]   %s\n", name); \
    else      { printf("  [FAIL] %s\n", name); fails++; } } while (0)

typedef struct { WCHAR name[128]; DWORD64 size; } Mod;
static Mod    g_mods[512];
static int    g_nMods = 0;
static Mod    g_base[512];
static int    g_nBase = 0;

static SIZE_T Snapshot(DWORD pid) {
    g_nMods = 0;
    PROCESS_MEMORY_COUNTERS pmc;
    memset(&pmc, 0, sizeof(pmc));
    pmc.cb = sizeof(pmc);
    HANDLE hp = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!hp) return 0;
    GetProcessMemoryInfo(hp, &pmc, sizeof(pmc));
    HMODULE mods[512];
    DWORD needed = 0;
    if (EnumProcessModulesEx(hp, mods, sizeof(mods), &needed, LIST_MODULES_ALL)) {
        int n = (int)(needed / sizeof(HMODULE));
        if (n > 512) n = 512;
        for (int i = 0; i < n; i++) {
            MODULEINFO mi;
            memset(&mi, 0, sizeof(mi));
            if (GetModuleInformation(hp, mods[i], &mi, sizeof(mi))) {
                WCHAR base[128];
                if (GetModuleBaseNameW(hp, mods[i], base, 128)) {
                    wcscpy_s(g_mods[g_nMods].name, 128, base);
                    g_mods[g_nMods].size = (DWORD64)mi.SizeOfImage;
                    g_nMods++;
                }
            }
        }
    }
    CloseHandle(hp);
    return pmc.WorkingSetSize;
}

/* 打印 mod2 相对 mod1 新增的模块 */
static void DiffModules(const WCHAR* tag) {
    printf("  [%ls] 新加载的模块:\n", tag);
    int found = 0;
    for (int i = 0; i < g_nMods; i++) {   /* g_mods 此时是第二份快照 */
        BOOL existed = FALSE;
        for (int j = 0; j < g_nBase; j++)
            if (_wcsicmp(g_mods[i].name, g_base[j].name) == 0) { existed = TRUE; break; }
        if (!existed) {
            printf("    %-42ls %6.1f MB\n", g_mods[i].name,
                   g_mods[i].size / 1048576.0);
            found++;
        }
    }
    if (!found) printf("    (none)\n");
}

static void SendEsc(void) {
    INPUT d, u;
    memset(&d, 0, sizeof(d));
    d.type = INPUT_KEYBOARD;
    d.ki.wVk = VK_ESCAPE;
    d.ki.wScan = (WORD)MapVirtualKeyW(VK_ESCAPE, MAPVK_VK_TO_VSC);
    u = d; u.ki.dwFlags |= KEYEVENTF_KEYUP;
    SendInput(1, &d, sizeof(INPUT));
    SendInput(1, &u, sizeof(INPUT));
}

int main(void) {
    WCHAR exe[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH);
    WCHAR* slash = NULL;
    for (DWORD i = 0; i < n; i++) if (exe[i] == L'\\') slash = &exe[i];
    if (slash) *(slash + 1) = 0;
    wcscat_s(exe, MAX_PATH, L"CapsEnhance.exe");

    STARTUPINFOW si; PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si)); si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));
    if (!CreateProcessW(exe, exe, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        printf("cannot start CapsEnhance.exe\n");
        return 2;
    }
    DWORD pid = pi.dwProcessId;
    HANDLE hProc = pi.hProcess;
    CloseHandle(pi.hThread);

    HWND hwnd = NULL;
    for (int i = 0; i < 30 && !hwnd; i++) {
        hwnd = FindWindowW(L"CapsEnhanceMain", NULL);
        if (!hwnd) Sleep(100);
    }
    if (!hwnd) { printf("main window not found\n"); TerminateProcess(hProc, 2); return 2; }
    Sleep(500);

    SIZE_T ws1 = Snapshot(pid);
    memcpy(g_base, g_mods, sizeof(Mod) * (size_t)g_nMods);
    g_nBase = g_nMods;
    printf("baseline ws=%.1f MB  modules=%d\n", ws1 / 1048576.0, g_nBase);

    /* 托盘右键：Explorer 会 PostMessage(hwnd, WM_TOGGLE_TRAY, 0, WM_RBUTTONUP) */
    PostMessageW(hwnd, WM_TOGGLE_TRAY, 0, WM_RBUTTONUP);
    Sleep(500);                 /* 菜单弹出（模态） */
    SendEsc();                  /* ESC 关闭菜单 */
    Sleep(800);

    SIZE_T ws2 = Snapshot(pid);
    printf("after menu ws=%.1f MB  modules=%d  delta=%+.0f KB\n",
           ws2 / 1048576.0, g_nMods, (double)((LONGLONG)ws2 - (LONGLONG)ws1) / 1024.0);
    DiffModules(L"tray-menu");

    /* 复测一次：菜单开销应趋于平稳（排除"每次都涨"） */
    SIZE_T before2 = ws2;
    PostMessageW(hwnd, WM_TOGGLE_TRAY, 0, WM_RBUTTONUP);
    Sleep(400);
    SendEsc();
    Sleep(600);
    SIZE_T ws3 = Snapshot(pid);
    printf("2nd menu ws=%.1f MB  delta=%+.0f KB\n",
           ws3 / 1048576.0, (double)((LONGLONG)ws3 - (LONGLONG)before2) / 1024.0);

    PostMessageW(hwnd, WM_CLOSE, 0, 0);
    WaitForSingleObject(hProc, 3000);
    CloseHandle(hProc);

    printf(fails ? "\n== %d 项失败 ==\n" : "\n== 完成 ==\n", fails);
    (void)fails;
    return 0;
}
