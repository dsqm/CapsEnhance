#include "cap.h"
#include "copy_text.h"
#include "plain_text.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* ================= 纯文本粘贴（Caps+V） =================
   把剪贴板里的内容**去掉格式**粘进当前前台窗口。

   为什么是"改写剪贴板 + 发粘贴键"，而不是逐字打印 / WM_PASTE：
     · 逐字 SendInput Unicode 打字：长文本慢，中文与 emoji 要过输入法/框架，
       部分程序（WPF、Electron、终端）根本收不到；剪贴板粘贴是唯一的通用路径。
     · WM_PASTE / EM_REPLACESEL：只对经典 Edit / RichEdit 有效，等于只给记事本
       做功能。
     · 三家成熟实现（Ditto 的 DoTextOnlyPaste、clippi 的 paste_plain_from_clipboard、
       QuickClipboard 的 PasteAction::PlainText）全部走这条路：把剪贴板收成
       纯文本格式，再注入目标程序自己认的粘贴键。
   关键细节也都对齐了它们：
     · 只写 CF_UNICODETEXT：不写 CF_TEXT —— ANSI 版按系统代码页落盘，不在该代码页
       里的字符会丢或乱（Ditto 两个格式都写，所以它的纯文本路径带着这条风险）；
     · 打 "Clipboard Viewer Ignore" 私标记，剪贴板管理器别把这份派生文本记进历史；
     · 注入前释放物理按住的修饰键（否则 Shift 按着时 Ctrl+V 变成 Ctrl+Shift+V，
       多数程序直接不粘 —— clippi 为这条专门改成批量 SendInput）；
     · 注入前校验前台窗口还是开火那一刻那个（执行线程是异步的，粘错窗口比
       不粘更糟）。

   **不恢复原剪贴板**：与三家参考一致（Ditto 的 Restore 只服务它自己的复制缓冲，
   clippi 的 §6.4 明确接受写回保留）。恢复要枚举所有格式 + GlobalLock 复制，
   而延迟渲染格式（Office 大内容）根本没法安全快照 —— 默认不做，也就不需要
   一个"看起来能用其实会丢格式"的开关。

   失败一律**静默**：不上屏、不响音、剪贴板一个字节都不动；唯一痕迹是记录
   模式（C + 反引号）里的一行日志。 */

#define PLAIN_OPEN_TRIES 10   /* 抢剪贴板：与 textgrab.c 的 TgClipWrite 同参数 */
#define PLAIN_OPEN_WAIT  20   /* ms */
#define PLAIN_SETTLE_MS  20   /* 写完到注入之间：让剪贴板通知先走完，再让目标去读 */
#define PLAIN_DROP_MAX   4096 /* CF_HDROP 条目上限：异常巨量句柄不该把内存吃光 */

/* 日志只在记录模式写（全项目一致：日志文件只在 C + 反引号 期间存在） */
static void PlainLog(const char* fmt, ...) {
    if (!g_recording.load()) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    CapLog("%s", buf);
}

/* 剪贴板是全局单例：另一个程序正在用它时 OpenClipboard 会失败，而这在
   "刚复制过东西"的时序里很常见（与 textgrab.c 同参数同理由）。 */
static BOOL PlainClipOpen(void) {
    for (int i = 0; i < PLAIN_OPEN_TRIES; i++) {
        if (OpenClipboard(NULL)) return TRUE;
        Sleep(PLAIN_OPEN_WAIT);
    }
    return FALSE;
}

/* 有界长度：剪贴板的 HGLOBAL 大小可信、内容里的结尾 0 未必在界内，
   一律按容量截断，不调 wcslen（那是"相信对方"的写法）。 */
static size_t BoundLenW(const WCHAR* s, size_t max) {
    size_t n = 0;
    while (n < max && s[n]) n++;
    return n;
}

/* 剪贴板已打开的前提下读文本：CF_UNICODETEXT 优先，缺失退回 CF_TEXT（ANSI）。
   成功返回长度，*out 为 malloc 缓冲（调用方 free）；0 = 没有文本。 */
static int PlainReadTextLocked(WCHAR** out) {
    *out = NULL;

    HGLOBAL h = (HGLOBAL)GetClipboardData(CF_UNICODETEXT);
    if (h) {
        const WCHAR* p = (const WCHAR*)GlobalLock(h);
        if (p) {
            size_t maxc = (size_t)GlobalSize(h) / sizeof(WCHAR);
            size_t n = BoundLenW(p, maxc);
            if (n > 0 && n < (size_t)0x7FFFFFFF) {
                WCHAR* buf = (WCHAR*)malloc((n + 1) * sizeof(WCHAR));
                if (buf) {
                    memcpy(buf, p, n * sizeof(WCHAR));
                    buf[n] = 0;
                    *out = buf;
                }
            }
            GlobalUnlock(h);
            if (*out) return (int)n;
        }
    }

    /* 只有 ANSI 版的老程序：按系统代码页转。写入方按别的代码页转过的内容
       这里救不回来，不做猜测（与 Ditto 只做标准转换一致）。 */
    h = (HGLOBAL)GetClipboardData(CF_TEXT);
    if (h) {
        const char* p = (const char*)GlobalLock(h);
        if (p) {
            size_t maxc = (size_t)GlobalSize(h);
            size_t n = 0;
            while (n < maxc && p[n]) n++;
            if (n > 0 && n < (size_t)0x7FFFFFFF) {
                int wl = MultiByteToWideChar(CP_ACP, 0, p, (int)n, NULL, 0);
                if (wl > 0) {
                    WCHAR* buf = (WCHAR*)malloc(((size_t)wl + 1) * sizeof(WCHAR));
                    if (buf) {
                        MultiByteToWideChar(CP_ACP, 0, p, (int)n, buf, wl);
                        buf[wl] = 0;
                        *out = buf;
                        n = (size_t)wl;
                    }
                }
            }
            GlobalUnlock(h);
            if (*out) return (int)n;
        }
    }
    return 0;
}

/* 剪贴板已打开的前提下读 CF_HDROP（资源管理器里复制的文件 / 文件夹），
   拼成"每行一个完整路径"。成功返回长度，*out 为 malloc 缓冲；0 = 没有文件。 */
static int PlainReadDropLocked(WCHAR** out) {
    *out = NULL;

    HGLOBAL h = (HGLOBAL)GetClipboardData(CF_HDROP);
    if (!h) return 0;
    HDROP drop = (HDROP)h;

    UINT n = DragQueryFileW(drop, 0xFFFFFFFFu, NULL, 0);
    if (n == 0 || n > PLAIN_DROP_MAX) return 0;

    /* 两遍：先算总长度，再一次分配。路径长度是 DragQueryFileW 给的（不含 0），
       每段 +1 存结尾 0。 */
    size_t pool = 0;
    for (UINT i = 0; i < n; i++) {
        UINT len = DragQueryFileW(drop, i, NULL, 0);
        if (len) pool += (size_t)len + 1;
    }
    if (pool == 0) return 0;

    WCHAR* buf = (WCHAR*)malloc(pool * sizeof(WCHAR));
    const WCHAR** parts = (const WCHAR**)malloc((size_t)n * sizeof(WCHAR*));
    if (!buf || !parts) { free(buf); free(parts); return 0; }

    int m = 0;
    WCHAR* w = buf;
    for (UINT i = 0; i < n; i++) {
        UINT len = DragQueryFileW(drop, i, NULL, 0);
        if (len == 0) continue;
        if (DragQueryFileW(drop, i, w, len + 1) == 0) continue;
        parts[m++] = w;
        w += (size_t)len + 1;
    }

    int len = 0;
    if (m > 0) {
        /* 行间 CRLF（2*（m-1) 个 WCHAR）+ 结尾 0，多留 2 个富余 */
        int cap = (int)(pool + (size_t)m * 2 + 2);
        WCHAR* text = (WCHAR*)malloc((size_t)cap * sizeof(WCHAR));
        if (text) {
            len = PlainJoinPaths(parts, m, text, cap);
            if (len > 0) *out = text;
            else          free(text);
        }
    }
    free(buf);
    free(parts);
    return len > 0 ? len : 0;
}

/* 把文本收成"只有纯文本"写进剪贴板（原本的 HTML / RTF / 图片等格式一并剥掉）。
   ★ 两处顺序上的讲究与 textgrab.c 的 TgClipWrite 完全相同：
     1. **先分配、后 EmptyClipboard** —— 分配失败时不能先清掉用户原来的剪贴板；
     2. 容量按 2*len+1 算：写出去的是 CRLF，每个 \n 多占一个 WCHAR。 */
static BOOL PlainWriteText(const WCHAR* text, int len) {
    if (!text || len <= 0) return FALSE;

    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, ((size_t)len * 2 + 1) * sizeof(WCHAR));
    if (!h) return FALSE;
    WCHAR* p = (WCHAR*)GlobalLock(h);
    if (!p) { GlobalFree(h); return FALSE; }
    int o = 0;
    for (int i = 0; i < len; i++) {
        if (text[i] == L'\n') p[o++] = L'\r';   /* 剪贴板约定是 CRLF */
        p[o++] = text[i];
    }
    p[o] = 0;
    GlobalUnlock(h);

    if (!PlainClipOpen()) { GlobalFree(h); return FALSE; }
    BOOL ok = FALSE;
    if (EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, h))
        ok = TRUE;                              /* 成功时所有权归系统 */
    if (!ok) GlobalFree(h);

    /* "这条不是我复制的新内容"：注册私格式做标记。通用的跨程序做法 ——
       Ditto 用同一条已注册格式，clippi / QuickClipboard 用各自的暂停监视标志。
       于是剪贴板历史里留下的还是用户原本那条富文本，而不是这份派生品。 */
    if (ok) {
        UINT cf = RegisterClipboardFormatW(L"Clipboard Viewer Ignore");
        if (cf) {
            HGLOBAL m = GlobalAlloc(GMEM_MOVEABLE, 2);
            if (m) {
                BYTE* q = (BYTE*)GlobalLock(m);
                if (q) {
                    q[0] = 1;
                    q[1] = 0;
                    GlobalUnlock(m);
                    if (!SetClipboardData(cf, m)) GlobalFree(m);
                } else {
                    GlobalFree(m);
                }
            }
        }
    }
    CloseClipboard();
    return ok;
}

/* 目标是不是"Ctrl+V 不管用"的控制台 / 终端系（类名判定见 plain_text.c） */
static BOOL PlainTargetIsConsole(HWND hwnd) {
    WCHAR cls[128] = L"";
    if (!hwnd || !GetClassNameW(hwnd, cls, 128)) return FALSE;
    return PlainIsConsoleClass(cls);
}

/* ================= 入口 =================
   **只在动作执行线程上调用**（见 act.c 的 RunJob）：这里要抢全局剪贴板
   （OpenClipboard 最多重试 10×20ms）并注入全局按键，全是在钩子宿主线程上
   不能做的阻塞。target 是 action 开火那一刻的前台窗口（ActJob.hwnd 快照）。 */
void PlainPasteRun(HWND target) {
    if (!target || !IsWindow(target)) {
        PlainLog("  [粘贴] 没有目标窗口（前台是桌面，或窗口已关闭）");
        return;
    }
    DWORD pid = 0;
    GetWindowThreadProcessId(target, &pid);
    if (pid == GetCurrentProcessId() || WinIsShellClass(target)) {
        PlainLog("  [粘贴] 目标是桌面壳 / 本程序自己的窗口，不粘");
        return;
    }

    WCHAR* text = NULL;
    int len = 0;
    if (!PlainClipOpen()) {
        PlainLog("  [粘贴] 剪贴板被其他程序占用（重试 %d 次仍打不开）", PLAIN_OPEN_TRIES);
        return;
    }
    len = PlainReadTextLocked(&text);
    if (len <= 0) len = PlainReadDropLocked(&text);
    CloseClipboard();

    if (len <= 0) {
        PlainLog("  [粘贴] 剪贴板里没有文本（图片 / 文件以外的空内容）");
        free(text);
        return;
    }

    /* 换行归一：CRLF / 裸 CR 一律先变成 LF。
       ★ 这一步不能省：剪贴板里的 CF_UNICODETEXT 几乎都是 CRLF（Word / 网页 /
       表格都这样），而写回时 PlainWriteText 还要把每个 \n 展开成 CRLF —— 不先归一
       就是 \r\r\n，粘出来每两行之间多一个空行（从网页复制多行文本必现）。
       复用 copy_text.c 的 CopyClean，与"超级复制"同一套换行语义；stripTail=FALSE：
       粘贴要原样，尾部空白不动。 */
    {
        WCHAR* norm = (WCHAR*)malloc(((size_t)len + 1) * sizeof(WCHAR));
        if (!norm) {
            PlainLog("  [粘贴] 换行归一：内存不足（%d 字）", len);
            free(text);
            return;
        }
        int nlen = CopyClean(text, len, FALSE, norm, len + 1);
        free(text);
        text = norm;
        len = nlen;
    }

    if (len <= 0 || PlainIsBlank(text, len)) {
        PlainLog("  [粘贴] 剪贴板文本是空白，不粘（%d 字）", len);
        free(text);
        return;
    }

    /* 前台校验（一）：读剪贴板可能要等两百毫秒，中间切了窗就不粘 */
    if (GetForegroundWindow() != target) {
        PlainLog("  [粘贴] 读剪贴板期间前台已改变，放弃");
        free(text);
        return;
    }

    if (!PlainWriteText(text, len)) {
        PlainLog("  [粘贴] 写剪贴板失败（被占用或内存不足），不粘");
        free(text);
        return;
    }

    /* 前台校验（二）：写完到注入之间还有一步，粘错窗口比不粘更糟 */
    if (GetForegroundWindow() != target) {
        PlainLog("  [粘贴] 写剪贴板期间前台已改变，放弃注入");
        free(text);
        return;
    }

    BOOL console = PlainTargetIsConsole(target);
    Sleep(PLAIN_SETTLE_MS);
    SendPasteCombo(console);
    PlainLog("  [粘贴] 纯文本 %d 字 -> hwnd=%p，粘贴键=%s",
             len, (void*)target, console ? "Shift+Insert" : "Ctrl+V");
    free(text);
}
