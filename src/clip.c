/* ======================================================================
   CapsEnhance —— Windows CapsLock 快捷键增强程序
   Copyright (C) 2026  qm

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <https://www.gnu.org/licenses/>.
   ------------------------------------------------------------------ */

#include "cap.h"

#include <stdio.h>
#include <string.h>
#include <wchar.h>

/* ================= 剪贴板浮层的"开合"（主程序侧，Caps+V） =================
   窗口本体在**独立 exe**（clip/clip.c → bin/CapsEnhanceClip.exe）里，这里只做
   四件事：**拉起它**、**告诉它显示**、**代收 Esc**、**发现它没了**。跨进程机制与
   帮助窗口（src/help.c）完全同构 —— 注册消息 + FindWindow + 送达重试，那边的
   每一条理由在这里同样成立，不重复写一遍。**心跳两边都已删掉**：收场改由
   Job Object（父死子退）与进程句柄等待（子死父知）负责，见 src/child.c。

   与帮助窗口唯一的、也是最关键的差别：**寿命不跟和弦走**。
     · 帮助是"按住 F1 才显示"，松开就隐 —— 寿命 = 和弦，所以 HelpHide 会在
       松开 Caps 时被自动调用（ResetEngine）。
     · 剪贴板是"按一下就开、之后自己活着"：松开 V 之后窗口必须还在，否则
       用户根本来不及挑条目。所以这里**没有**"松开 Caps 就隐"这条路径，
       只有明确的关闭途径（Esc / X / 点击外部 / 粘贴后 / 再按一次热键）。
   这个差别决定了 ClipShow 是 **toggle**：窗口已经在时，再按一次 Caps+V
   是"关掉它"，而不是"再显示一次"。

   状态机比帮助窗口多一位：`s_want` 之外还需要"窗口是否存在"这个事实 ——
   toggle 要先知道"现在开没开"，而"开没开"在冷启动那 100ms 里是不知道的
   （进程刚拉起、窗口还没注册）。所以 toggle 判定用 **s_want**（意图），
   不用 FindWindow 的结果：否则"快速按两下"会变成"开了两个"或"关不掉"。 */

static UINT  s_msgShow = 0;
static UINT  s_msgHide = 0;
static UINT  s_msgEsc  = 0;
static UINT  s_msgKey  = 0;
static UINT  s_msgClosed = 0;
static BOOL  s_want    = FALSE;   /* 期望：可见？ */
static int   s_delivered = 0;     /* 已送达：-1 未知 / 0 已隐 / 1 已显 */
static BOOL  s_spawned = FALSE;   /* 本轮是否已经 CreateProcess 过 */
static HANDLE s_spawnProc = NULL; /* 本轮拉起来的那个进程：始终送不到就收掉它（见 ClipTick） */
static ChildWatch s_watch;        /* 浮层进程的死亡通知（见 ClipGone） */
static int   s_ticks   = 0;

static void EnsureMsgs(void) {
    if (!s_msgShow) s_msgShow = RegisterWindowMessageW(CLIP_MSG_SHOW);
    if (!s_msgHide) s_msgHide = RegisterWindowMessageW(CLIP_MSG_HIDE);
    if (!s_msgEsc)  s_msgEsc  = RegisterWindowMessageW(CLIP_MSG_ESC);
    if (!s_msgKey)  s_msgKey  = RegisterWindowMessageW(CLIP_MSG_KEY);
    if (!s_msgClosed) s_msgClosed = RegisterWindowMessageW(CLIP_MSG_CLOSED);
}

BOOL ClipIsOpen(void) {
    return s_want;
}

static void StopTick(void) {
    if (g_mainWnd) KillTimer(g_mainWnd, CLIP_TIMER);
    s_ticks = 0;
}

/* 驻留策略（2026-09-21）：浮层收起/粘贴后**进程继续待命 10 分钟**，下一次
   Caps+V 直接复用（免冷启动：读库 + 起缩略图 + 建窗口都在省）。这段时间由
   **浮层自己的** GRACE_MS 计时管着（clip/clip.c）—— 它是唯一知道"自己刚被收起"
   的那一方，主程序再记一份就是两个钟各算各的（原来确实如此：主程序的
   CLIP_IDLE_TIMER 与浮层的 TID_GRACE 同事件、同时长）。
   待命期间主程序**没有任何周期性动作**：唤出时发一条消息，死活由 Job Object
   与进程句柄等待负责（见 src/child.c）。 */

/* ---------- Ditto 目录：相对 -> 绝对 ----------
   **基准只有本进程知道**（自己的 exe 目录），所以展开必须在这里做完再传给
   子进程 —— 让浮层自己再解一次，就是同一件事的两份实现，而且它拿不到
   "主程序是从哪个目录启动的"这个事实（其它进程以随机 cwd 拉起本程序）。
   不引入 shlwapi 只为 `PathIsRelativeW`：一条三行的判定就够，不值得为它
   加一个库依赖。 */
static BOOL PathIsAbsoluteW(const WCHAR* p) {
    if (!p || !p[0]) return FALSE;
    if (p[0] == L'\\') return TRUE;                    /* \dir（根相对）或 \\server（UNC） */
    return (p[1] == L':' && p[2] == L'\\');           /* C:\... */
}

static void ResolveDittoDir(WCHAR* out, size_t cap) {
    out[0] = 0;
    const WCHAR* raw = CfgStr(CFG_DITTO_DIR);
    if (!raw || !raw[0]) return;                      /* 未配置：浮层自己会提示 */
    if (PathIsAbsoluteW(raw)) { wcscpy_s(out, cap, raw); return; }
    WCHAR dir[MAX_PATH];
    if (!GetModuleFileNameW(NULL, dir, MAX_PATH)) return;
    WCHAR* slash = wcsrchr(dir, L'\\');
    if (!slash) return;
    *slash = 0;
    /* 拼出来的可能带 `..`（如 "...\bin\..\Ditto"）：sqlite 和 CreateProcess
       都自己会展开，不必在这里归一化 —— 归一化要引入 PathCanonicalizeW。 */
    _snwprintf_s(out, cap, _TRUNCATE, L"%s\\%s", dir, raw);
}

/* 拉起剪贴板 exe（同目录）。与 SpawnHelp 同一条规矩：**工作目录显式传它自己
   所在目录**（本程序是以"随机 cwd + 绝对路径"启动的）。 */
static void SpawnClip(void) {
    WCHAR exe[MAX_PATH] = L"";
    WCHAR dir[MAX_PATH] = L"";
    WCHAR dd[DITTO_DIR_MAX] = L"";
    WCHAR font[CLIP_FONT_MAX] = L"";
    int   sizePt = CfgInt(CFG_CLIP_FONT_SIZE);
    WCHAR cmd[MAX_PATH + DITTO_DIR_MAX + CLIP_FONT_MAX + 96];

    if (!GetModuleFileNameW(NULL, exe, MAX_PATH)) return;
    wcscpy_s(dir, MAX_PATH, exe);
    WCHAR* slash = wcsrchr(dir, L'\\');
    if (!slash) return;
    *slash = 0;
    _snwprintf_s(exe, MAX_PATH, _TRUNCATE, L"%s\\%s", dir, CLIP_EXE);

    /* --dir / --font 都用引号包住：路径与字体名几乎必然带空格。
       没配的项就不传，浮层用它自己的内置默认 —— "没配"与"配成空"语义一致。
       传的是**目录**，不是 db 路径：浮层还要用同一目录找 Ditto.exe（转本体）。 */
    ResolveDittoDir(dd, DITTO_DIR_MAX);
    wcscpy_s(font, CLIP_FONT_MAX, CfgStr(CFG_CLIP_FONT));

    if (dd[0] && font[0])
        _snwprintf_s(cmd, (size_t)(MAX_PATH + DITTO_DIR_MAX + CLIP_FONT_MAX + 96), _TRUNCATE,
                     L"\"%s\" --dir \"%s\" --font \"%s\" --size %d", exe, dd, font, sizePt);
    else if (dd[0])
        _snwprintf_s(cmd, (size_t)(MAX_PATH + DITTO_DIR_MAX + CLIP_FONT_MAX + 96), _TRUNCATE,
                     L"\"%s\" --dir \"%s\"", exe, dd);
    else if (font[0])
        _snwprintf_s(cmd, (size_t)(MAX_PATH + DITTO_DIR_MAX + CLIP_FONT_MAX + 96), _TRUNCATE,
                     L"\"%s\" --font \"%s\" --size %d", exe, font, sizePt);
    else
        _snwprintf_s(cmd, (size_t)(MAX_PATH + DITTO_DIR_MAX + CLIP_FONT_MAX + 96), _TRUNCATE,
                     L"\"%s\"", exe);

    /* 交给 child.c：挂起起 -> 挂进 job（父死子退）-> 放行，并带回进程句柄
       （始终送不到时靠它把孤儿窗口收掉，见 ClipTick 的超时分支）。 */
    if (ChildSpawn(exe, cmd, dir, &s_spawnProc))
        return;
    if (g_recording.load())
        CapLog("  [剪贴板] 启动 %ls 失败 err=%lu", CLIP_EXE, GetLastError());
}

static void Kick(void) {
    if (!g_mainWnd) { s_delivered = s_want ? 1 : 0; return; }
    s_ticks = 0;
    SetTimer(g_mainWnd, CLIP_TIMER, CLIP_TICK_MS, NULL);
    ClipTick();
}

void ClipShow(void) {
    EnsureMsgs();
    if (s_want) {
        /* toggle 前先看窗口**还在不在**：浮层"点外面/粘贴后"会自己藏起来，
           但进程要再活 300ms（退出缓冲）+ 主程序这边要再等 600ms 才确认它死了。
           这段空窗里 s_want 还是 TRUE —— 直接当 toggle 处理就会把这次按键
           吃成"关一个已经不在的窗口"，表现就是"关掉后再按要按两次才出来"。
           所以：窗口真的不在 = 当作打开。 */
        if (!FindWindowW(CLIP_CLASS, NULL)) {
            s_want      = FALSE;
            s_delivered = 0;
        } else {
            ClipHide();
            return;
        }
    }
    s_want      = TRUE;
    s_delivered = -1;
    s_spawned   = FALSE;
    Kick();
    if (g_recording.load()) CapLog("  [剪贴板] 请求显示");
}

void ClipHide(void) {
    if (!s_want && s_delivered == 0) return;
    s_want      = FALSE;
    s_delivered = -1;
    Kick();
    if (g_recording.load()) CapLog("  [剪贴板] 请求关闭");
}

/* 钩子代收的 Esc：转发给浮层。为什么不直接"主程序自己把 s_want 置假"就算完 ——
   浮层那边还有"置顶/钉住"状态与它自己的退出定时器，让它自己收到关闭指令才能
   把那些状态一并收干净；这里是同一条指令的两个副本（一个给浮层、一个记账）。 */
void ClipEsc(void) {
    EnsureMsgs();
    HWND w = FindWindowW(CLIP_CLASS, NULL);
    if (w && s_msgEsc) PostMessageW(w, s_msgEsc, 0, 0);
    if (s_want) ClipHide();
}

/* 浮层自行关闭（粘贴后收起 / ✕ / 超时）的通知：清 s_want，让钩子恢复放行。
   ★ 不清的后果（实测）：钩子继续替一个已隐藏的窗口吞回车 —— 宽限期内
   WM_CLIP_KEY 进隐藏浮层（= 再粘一次），窗口销毁后按键凭空消失
   （= 回车永远发不出去）。 */
void ClipClosed(void) {
    if (!s_want) return;
    s_want = FALSE;
    if (g_recording.load())
        CapLog("  [剪贴板] 浮层自行关闭，恢复按键放行");
}

BOOL ClipIsClosedMsg(UINT msg) {
    EnsureMsgs();
    return s_msgClosed && msg == s_msgClosed;
}

/* 浮层**进程**没了（崩溃 / 被任务管理器杀掉 / 主程序退出时 job 收尾）。
   与 ClipClosed 是同一件事的两种来源，差别在"怎么知道的"：
     · ClipClosed 靠浮层自己发消息 —— 只有它能优雅收场（粘贴 / ✕ / Esc）时才有。
     · 这里靠内核：注册在进程句柄上的等待到点（src/child.c）。崩溃、被杀这类
       "来不及说再见"的收场只有它能发现，而它正是原来最容易漏掉的一半 ——
       心跳时代要轮询 3 拍（600ms）才敢下结论，那 600ms 里钩子一直替一个已经
       不存在的窗口吞 Esc / 回车 / 方向键。 */
void ClipGone(void) {
    ChildWatchStop(&s_watch);        /* 复位还有一层意义：换个新进程时能重新盯上 */
    if (!s_want) return;
    s_want      = FALSE;
    s_delivered = 0;
    if (g_recording.load())
        CapLog("  [剪贴板] 浮层进程已退出，恢复按键放行");
}

/* 把 Ditto 的快速粘贴窗口拉到前台（原理见 clip/clip.c 同名注释：Ditto 自己的
   SetForegroundWindow 会因前台转移规则失败，但它启动时 ASFW_ANY 授权了任何
   进程可代劳）。-open 子进程等本体显示完才退出，所以先等它，通常一击即中。
   ★ 本函数**只能在一次性线程上跑**（见 DittoThreadProc）：最坏 1500ms + 20×30ms
     ≈ 2.1 秒，而主线程是低级钩子的宿主。 */
static void RaiseDittoWindow(HANDLE hProc) {
    if (hProc) WaitForSingleObject(hProc, 1500);
    for (int i = 0; i < 20; i++) {
        HWND w = FindWindowW(L"QPasteClass", NULL);
        if (w && IsWindowVisible(w)) {
            SetForegroundWindow(w);
            SetWindowPos(w, HWND_TOP, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            return;
        }
        Sleep(30);
    }
}

/* ---------- 转 Ditto 本体 ----------
   浮层只有"挑一条粘"这一种能力，Ditto 本体还有编辑/分组/搜索 —— 浮层干不了
   的活，交回本体。做法用 Ditto 的**官方命令行** `-open`（CP_Main.cpp 的
   DittoCommandLineInfo）：它向已运行实例的保存 HWND 发 WM_OPEN_CLOSE_WINDOW，
   由本体自己弹出快速粘贴窗口（带激活检查与位置逻辑，都是它自己的）。
   两个边界：
     · Ditto 没在运行时 `-open` 是**安全无操作**（新实例发不出消息就退出，
       不会替用户把 Ditto 拉起来 —— 它的历史都没在收集，拉起来也没意义）。
     · 浮层是 NOACTIVATE，前台一直是目标应用，这里不需要归还焦点；
       先关浮层再拉本体，两窗短暂的共存期也不抢焦点。 */
/* 一次性线程的入参：Ditto.exe 与它的目录（ResolveDittoDir 已在主线程算好，
   工作线程不碰配置 —— 与 folder.c 的 FolderJob 同形）。 */
typedef struct {
    WCHAR exe[MAX_PATH];
    WCHAR dir[DITTO_DIR_MAX];
} DittoJob;

/* 拉起 Ditto 本体并等它的窗口出来。
   ★ 为什么必须离开主线程：这一段最坏 1500ms + 20×30ms ≈ 2.1 秒，而主线程是
     低级钩子的宿主 —— 它不在"等消息"状态期间系统会把到达的钩子事件**直接跳过**
     （见 cap.h 与 main.c 的 WM_WHEEL_ACCEL 说明）。漏掉的若是某个修饰模式的
     抬起，keymap.c 的 ModeDown 只看按住位图、不现读核对，于是增强模式粘住：
     之后每个物理滚轮被吞、每次点按都被补齐成 3 次，直到暂停/重启才恢复。
     这条动作是"单击 Caps"触发的（钩子里 PostMessage 过来），主线程一挡就把
     用户最直接的按键反馈弄丢了，所以退回同步也不行 —— 起不来线程就什么都不做。 */
static DWORD WINAPI DittoThreadProc(LPVOID param) {
    DittoJob* job = (DittoJob*)param;
    WCHAR cmd[MAX_PATH + 16];
    _snwprintf_s(cmd, (size_t)(MAX_PATH + 16), _TRUNCATE, L"\"%s\" -open", job->exe);
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    memset(&pi, 0, sizeof(pi));
    si.cb = sizeof(si);
    if (CreateProcessW(job->exe, cmd, NULL, NULL, FALSE, 0, NULL, job->dir, &si, &pi)) {
        RaiseDittoWindow(pi.hProcess);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        if (g_recording.load()) CapLog("  [剪贴板] 已转交 Ditto 本体");
    } else if (g_recording.load()) {
        CapLog("  [剪贴板] CreateProcess(Ditto) 失败");
    }
    free(job);
    return 0;
}

void ClipCallDitto(void) {
    WCHAR dittoExe[MAX_PATH] = L"";
    WCHAR dd[DITTO_DIR_MAX]  = L"";
    ResolveDittoDir(dd, DITTO_DIR_MAX);
    if (!dd[0]) {
        if (g_recording.load()) CapLog("  [剪贴板] 转本体失败：未配置 DittoDir");
        return;
    }
    _snwprintf_s(dittoExe, MAX_PATH, _TRUNCATE, L"%s\\Ditto.exe", dd);
    if (GetFileAttributesW(dittoExe) == INVALID_FILE_ATTRIBUTES) {
        if (g_recording.load()) CapLog("  [剪贴板] 转本体失败：Ditto.exe 不在 DittoDir 里");
        return;
    }
    ClipHide();
    DittoJob* job = (DittoJob*)calloc(1, sizeof(DittoJob));
    if (!job) return;      /* 分配不到 = 一次"没反应"，与 folder.c 同一取舍 */
    _snwprintf_s(job->exe, MAX_PATH, _TRUNCATE, L"%s", dittoExe);
    _snwprintf_s(job->dir, DITTO_DIR_MAX, _TRUNCATE, L"%s", dd);
    HANDLE h = CreateThread(NULL, 0, DittoThreadProc, job, 0, NULL);
    if (!h) { free(job); return; }
    CloseHandle(h);        /* 线程对象在线程退出时回收；不需要 join */
}

/* 导航键转发：浮层是 NOACTIVATE，键盘消息根本到不了它 —— 回车 / 方向键 /
   Alt+数字 全都得由钩子拦下来，从这里转过去。 */
void ClipKey(int vk, unsigned flags) {
    EnsureMsgs();
    HWND w = FindWindowW(CLIP_CLASS, NULL);
    if (w && s_msgKey) PostMessageW(w, s_msgKey, (WPARAM)vk, (LPARAM)flags);
}

void ClipTick(void) {
    if (!g_mainWnd) { StopTick(); return; }
    if (s_delivered == (s_want ? 1 : 0)) { StopTick(); return; }

    EnsureMsgs();
    HWND w = FindWindowW(CLIP_CLASS, NULL);
    if (w) {
        PostMessageW(w, s_want ? s_msgShow : s_msgHide, 0, 0);
        s_delivered = s_want ? 1 : 0;
        StopTick();
        /* 送达成功 = 从现在起盯住它的进程：它一死（崩溃 / 被杀 / 被 job 收掉）
           就给主窗口发 WM_CLIP_GONE，钩子立刻恢复放行。
           拿句柄用"窗口的 pid"而不是 CreateProcess 的返回值 —— **两种来源一视同仁**：
           冷启动那个是我们拉起的，"驻留复用"这份不是（甚至可能是手动双击起来的），
           按 pid 走就只有一条路径。 */
        DWORD pid = 0;
        GetWindowThreadProcessId(w, &pid);
        if (pid) ChildWatchPid(&s_watch, pid, g_mainWnd, WM_CLIP_GONE);
        if (s_spawnProc) { CloseHandle(s_spawnProc); s_spawnProc = NULL; }
        return;
    }

    if (s_want && !s_spawned) {
        s_spawned = TRUE;
        SpawnClip();
    }
    if (++s_ticks > CLIP_TRIES_MAX) {
        BOOL want = s_want;        /* 先留一份：下面要清 s_want，而日志说的是"当时" */
        StopTick();
        /* ★ 拉不起来就必须把 s_want 清掉：ClipIsOpen() 直接返回 s_want，而钩子
           据此决定"要不要替浮层吞 Esc"。留着 TRUE 就等于**永久吃掉用户的 Esc**
           （窗口根本不存在，却一直在拦键）—— 这比"没反应"糟糕得多。 */
        if (s_want) s_want = FALSE;
        /* 本轮自己拉起过、却始终没等到它的窗口：把它收掉。心跳时代这一步是白送的
           （心跳一停它自己就退了），现在没有心跳，不主动收就是一个**留在屏幕上的
           孤儿窗口** —— 冷启动慢过 1 秒重试预算时真会发生（杀软扫描、机械盘）。 */
        if (s_spawnProc) {
            TerminateProcess(s_spawnProc, 0);
            CloseHandle(s_spawnProc);
            s_spawnProc = NULL;
        }
        if (g_recording.load())
            CapLog("  [剪贴板] 送达超时（%d 次 x %dms）：%s", CLIP_TRIES_MAX,
                   CLIP_TICK_MS, want ? "没能拉起剪贴板程序" : "窗口没出生就已关闭");
    }
}
