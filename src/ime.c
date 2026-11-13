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

   ------------------------------------------------------------------
   中英切换基于 IMM32 的 WM_IME_CONTROL 事实标准（命令常量与 open status /
   conversion mode 状态模型均为公开通用的做法），本文件为独立实现。
   本项目以 GPL-3.0-or-later 授权。
   ------------------------------------------------------------------ */

#include "cap.h"
#include "lane.h"

#include <imm.h>

#pragma comment(lib, "imm32.lib")

/* 这几条不都是 imm.h 给的，**缺了就得自己补**（实测 Windows SDK 10.0.26100）：
     · IMC_SETCONVERSIONMODE(0x2) / IMC_SETOPENSTATUS(0x6) 在 **immdev.h**
       里 —— 那是给"写输入法的人"用的头，imm.h 不包含它，普通程序拿不到；
     · IMC_GETCONVERSIONMODE(0x1) / IMC_GETOPENSTATUS(0x5) **整个 SDK 都没有**
       定义（连 immdev.h 里也只有 SET 那两条）；
     · IME_CMODE_NATIVE(0x1) 在 imm.h **间接**包含（#include <ime_cmodes.h>）
       里，通常拿得到，这里的兜底只是保险。
   数值本身是二十多年来 IMM32 的事实标准（KBLAutoSwitch / InputTip 等一大批
   工具都在用同一组值），但微软的 WM_IME_CONTROL 文档页**只收录了 0x7 及以上**
   那几条窗口/字体相关的命令，这几条中英切换命令没有正式文档 —— 这也是
   "有些第三方输入法不认"的一部分原因（它们只需保证自己那套能用）。 */
#ifndef IMC_GETCONVERSIONMODE
#define IMC_GETCONVERSIONMODE 0x0001
#endif
#ifndef IMC_SETCONVERSIONMODE
#define IMC_SETCONVERSIONMODE 0x0002
#endif
#ifndef IMC_GETOPENSTATUS
#define IMC_GETOPENSTATUS     0x0005
#endif
#ifndef IMC_SETOPENSTATUS
#define IMC_SETOPENSTATUS     0x0006
#endif
#ifndef IME_CMODE_NATIVE
#define IME_CMODE_NATIVE      0x0001
#endif

/* ================= 单键切换输入法（专用线程） =================
   为什么必须有自己的线程：切换要向**别的进程**的 IME 窗口连发 3 条
   SendMessageTimeout（每条最多等 IME_QUERY_TIMEOUT_MS）。低级键盘钩子跑在
   装它的线程上 = 主线程，在那里等就是把全局键盘输入冻住几百毫秒 —— 与
   act.c 存在的理由完全同源（详见 act.c 文件头）。
   所以本模块只从**工作线程**碰 IME，主线程（钩子）只调 ImeRequest 投递一下。

   "最新一次 wins"：待办只有一个槽位。连点两下 Caps 只会切一次 —— 切输入法
   是幂等的，第二次请求没有新信息，而队列化只会让"刚才那一下"被延迟执行。
   （与 drag.c 拖动落点的取舍相同：这类"状态型"动作只在意最终值。）

   两条闸门（详见 cap.h 的接口注释）：
     · **英文键盘（主语言 0x09）一律不处理** —— 包括 Caps。本程序只改"当前输入法"
       的中/英状态，**不替用户换键盘布局**：那等于改他的输入环境（详见
       DEVELOPMENT.md 的"单键切换输入法"）。所以停在英文键盘上时按这三个键就是没反应，这是刻意的。
     · [AutoDisable] 名单命中 -> 不介入。
*/

/* 一次 IME 查询/设置的等待上限。
   200ms 与 IMEIndicator 同源：它给的是"目标进程卡住"的兜底，不是给慢机器留的
   余量 —— 关键在于等待发生在**本线程**，等满也不会影响键盘响应。 */
#define IME_QUERY_TIMEOUT_MS 200

/* HKL 低 16 位是语言 ID，其中低字节是主语言。
   ★ 只判"是不是英文键盘"这一件事：本程序**不按中文主语言去找布局、也不切布局**
   （历史版本会替用户把键盘换成中文，已按需求去掉 —— 见 ImeRunKey 里的说明）。 */
#define LANG_ID_EN 0x09

/* 本模块的日志一律只在记录模式下写：切换是**每次按键**都可能发生的动作，
   无条件落盘就等于给"按一下 Caps"配一次文件写（见 main.c 的日志一节）。 */
#define ILOG(...) do { if (g_recording.load()) CapLog(__VA_ARGS__); } while (0)

/* ---------- 取"输入真正落在哪个窗口上" ----------
   GetForegroundWindow 只回答"哪个顶层窗口在前台"，而键盘布局与 IME 都是
   **按线程**的：输入附着在焦点控件上（GUITHREADINFO.hwndFocus）。
   取不到就退回前台窗口 —— 比"查不到就放弃"实用得多（部分自绘控件与
   权限更高的进程不给焦点控件）。 */
static HWND ImeFocusWindow(void) {
    HWND fg = GetForegroundWindow();
    if (!fg) return NULL;
    DWORD tid = GetWindowThreadProcessId(fg, NULL);
    GUITHREADINFO gi;
    ZeroMemory(&gi, sizeof(gi));
    gi.cbSize = sizeof(gi);
    if (tid && GetGUIThreadInfo(tid, &gi) && gi.hwndFocus) return gi.hwndFocus;
    return fg;
}

/* 该窗口所属线程的键盘布局**主语言**；拿不到返回 -1（调用方据此别乱判） */
static int ImeLangOf(HWND w) {
    if (!w) return -1;
    DWORD tid = GetWindowThreadProcessId(w, NULL);
    HKL hkl = GetKeyboardLayout(tid);
    return (int)(((UINT_PTR)hkl) & 0xFF);
}

/* 向 IME 窗口发一条 WM_IME_CONTROL。out 为 NULL 表示"只要发出去就行"。
   SMTO_ABORTIFHUNG：对方消息循环卡住时立刻放弃，绝不陪它一起卡。 */
static BOOL ImeCtl(HWND ime, WPARAM cmd, LPARAM val, DWORD_PTR* out) {
    DWORD_PTR v = 0;
    if (!SendMessageTimeoutW(ime, WM_IME_CONTROL, cmd, val,
                             SMTO_ABORTIFHUNG, IME_QUERY_TIMEOUT_MS, &v))
        return FALSE;
    if (out) *out = v;
    return TRUE;
}

/* ---------- 切"中文 / 英文"这一步 ----------
   两个信号一起设，缺一不可：
     · IMC_SETOPENSTATUS    打开/关闭输入法；
     · IMC_SETCONVERSIONMODE 清/置 IME_CMODE_NATIVE（中文模式位）。
   只设 open 不动转换模式的输入法确实存在，反之亦然；两个都设才对第三方输入法
   普遍有效。（KBLAutoSwitch 的 setIME 也是这个顺序：先取转换模式 -> 改 Native 位
   -> 设回 -> 最后设 open。这里保留其它模式位（全角/符号等），不像它那样把
   转换模式遮成 11 位 —— 那些位是用户的其它设置，不该被我们清掉。）
   顺序**不能反**：先设转换模式再设 open。部分输入法在 open=0 时会拒绝接受
   转换模式的改动，反过来的话"切中文"会只打开输入法、却停在英文模式。 */
static BOOL ImeSetChinese(HWND ime, BOOL cn) {
    DWORD_PTR conv = 0;
    if (!ImeCtl(ime, IMC_GETCONVERSIONMODE, 0, &conv)) {
        ILOG("  [输入法] 读不到转换模式（IME 窗口无响应）—— 本次不切换");
        return FALSE;
    }
    DWORD_PTR want = cn ? (DWORD_PTR)IME_CMODE_NATIVE : 0;
    DWORD_PTR newConv = (conv & ~(DWORD_PTR)IME_CMODE_NATIVE) | want;
    if (!ImeCtl(ime, IMC_SETCONVERSIONMODE, (LPARAM)newConv, NULL)) return FALSE;
    if (!ImeCtl(ime, IMC_SETOPENSTATUS, (LPARAM)(cn ? 1 : 0), NULL)) return FALSE;
    ILOG("  [输入法] 转换模式 0x%03X -> 0x%03X，open=%d",
         (unsigned)conv, (unsigned)newConv, cn ? 1 : 0);
    return TRUE;
}

/* 执行一次切换。key = IME_KEY_*（不是"目标" —— 目标从配置现读，
   这样改完配置不必等下一次按键才生效）。 */
static void ImeRunKey(int key) {
    int mode = ConfigGetImeMode(key);
    if (mode == IME_MODE_OFF) return;

    HWND fg = GetForegroundWindow();
    if (!fg) { ILOG("  [输入法] 没有前台窗口，不切换"); return; }
    HWND focus = ImeFocusWindow();
    if (!focus) focus = fg;
    int lang = ImeLangOf(focus);

    /* ★ 英文键盘（主语言 0x09）：三个键一律不处理，**Caps 也一样**。
       历史版本在这里会替用户把键盘布局换成中文再设状态 —— 已按需求去掉：
       本程序只切"当前输入法"的中/英，不替用户增删/切换键盘（那是他的输入环境，
       动了就等于改系统设置，见 DEVELOPMENT.md 的"单键切换输入法"）。所以停在英文键盘上时按了
       没反应，这是刻意的行为，不是失灵。放在黑名单之前：它只是一次
       GetKeyboardLayout，比后面那份名单的 OpenProcess 便宜得多。 */
    if (lang == LANG_ID_EN) {
        ILOG("  [输入法] 前台是英文键盘（0x%02X），Caps/Shift 都不处理", lang);
        return;
    }

    /* 黑名单：[AutoDisable] 与"Caps 整段不介入"同一份名单、同一套语义。
       Shift 也走这里 —— 用户要求"Shift 也要遵守程序黑名单"。 */
    if (ConfigIsAutoDisabled(fg)) {
        ILOG("  [输入法] 前台程序命中 [AutoDisable] 名单，不切换");
        return;
    }

    BOOL cn = (mode != IME_MODE_EN);
    HWND ime = ImmGetDefaultIMEWnd(focus);
    if (!ime && focus != fg) ime = ImmGetDefaultIMEWnd(fg);
    if (!ime) { ILOG("  [输入法] 取不到 IME 窗口，不切换"); return; }

    /* 回读校验：**发了不等于生效** —— 目标进程的 IME 完全可以无视这两条消息
       （TSF 系的输入法尤其常见）。这一行是"按了没反应"时唯一的现场。 */
    DWORD_PTR before = 0, after = 0;
    ImeCtl(ime, IMC_GETOPENSTATUS, 0, &before);
    ImeSetChinese(ime, cn);
    if (ImeCtl(ime, IMC_GETOPENSTATUS, 0, &after)) {
        if (after != before)
            ILOG("  [输入法] 已切到%s（open %d -> %d）",
                 cn ? "中文" : "英文", (int)before, (int)after);
        else
            ILOG("  [输入法] <<< 切换命令已发出，但状态没变（open 仍为 %d）", (int)after);
    }
}

/* ================= 专用线程 =================
   临界区 / 事件 / 线程 / 停止位交给 lane.c（见 lane.h）。 */
static Lane    s_lane;
static int     s_pending = -1;   /* 待执行的 IME_KEY_*；-1 = 没有待办 */

static DWORD WINAPI ImeThread(LPVOID p) {
    (void)p;
    for (;;) {
        if (LaneStopped(&s_lane)) break;
        WaitForSingleObject(s_lane.ev, 1000);   /* 1s 超时兜底，防事件被吞 */
        if (LaneStopped(&s_lane)) break;
        int key;
        EnterCriticalSection(&s_lane.cs);
        key = s_pending;
        s_pending = -1;                    /* "最新一次 wins"：只做最后一次 */
        LeaveCriticalSection(&s_lane.cs);
        if (key >= 0) ImeRunKey(key);
    }
    return 0;
}

/* 钩子线程（= 主线程）调用：**只投递，绝不阻塞**。
   "这个键设成了不处理"由**钩子**负责拦（见 hook.c 的两处调用点）—— 那样
   关掉的功能连一次函数调用都不会发生，与 [AutoDisable] 名单"空则短路"是
   同一条规矩：不用的功能不该在输入路径上留下成本。
   这里只挡"键号非法"与"线程没起来"两种不该发生的情况。 */
void ImeRequest(int key) {
    if (key < 0 || key >= IME_KEY_COUNT) return;
    if (!LaneLive(&s_lane)) return;    /* 线程没起来 -> 静默不可用（见下面的说明） */
    EnterCriticalSection(&s_lane.cs);
    s_pending = key;
    LeaveCriticalSection(&s_lane.cs);
    LanePoke(&s_lane);
}

/* 起/停。与拖拽通道同一条哲学：起不来就**静默不可用**，绝不在主线程同步兜底 ——
   同步执行会把"等别的进程 200ms"塞回承载钩子的线程，那正是本文件要躲的东西。
   （对比滚轮通道：它选择同步兜底，因为"少补一格"用户立刻能察觉；而输入法
   切换的代价是冻住键盘，两者不在一个量级。） */
void ImeLaneStart(void) {
    s_pending = -1;
    LaneStart(&s_lane, ImeThread);
}

void ImeLaneStop(void) {
    /* 一次切换最长约 3 × 200ms + 50ms，给 2s 足够收尾 */
    LaneStop(&s_lane, 2000);
}
