/* ============================================================
   和弦引擎回归测试：直接调用底层钩子进程，注入按键序列，
   用另一个捕获钩子统计软件真实注入的方向键/Shift。
   覆盖：模式修饰（增强=Alt/D、位移=F）在"按键乱序到达"下仍正确、
   长按重复/松手补齐、原生词跳按键序列，
   以及"Caps+滚轮 = 逐格放大（倍数）"、"Caps+Alt/D+滚轮 = 持续滚动"
   （都用注入计数钩子观测），"Caps+Shift+滚轮 = 横向滚动"
   （真实 Shift 决定轴向，且钩子吞掉物理格）。
   注意：引擎只问"某个模式是否生效"，从不判断按的是哪个键 ——
   键位表在 keymap.c，激活键是编译期常量（场景S验证"配置里写了也忽略"）。
   ============================================================ */
#define WinMain __CapsEnhanceRealWinMain_Unused
#include "cap.h"
#include "main.c"
#undef WinMain
#include "hook.c"
#include "send.c"      /* hook.c 的 SendActOnce 现在走统一发送模块 */
#include "act.c"       /* 动作实际由执行线程完成：本测试要能起/停/等它 */
#include <stdio.h>

/* 词跳桩：本测试只验证模式修饰/补齐/自动滚动，不测文本解析 */
int NavJump(int dir, BOOL shift) { (void)dir; (void)shift; return 0; }
void NavProbe(void) { /* 诊断探测：本测试桩掉 */ }
void NavBaseInfo(void) { /* 启动基线采集：本测试桩掉 */ }
void NavSelTrackReset(void) { /* 选区锚跟踪：本测试桩掉 */ }

/* ---- 捕获钩子：记录注入的完整事件序列（按下/抬起），并可统计次数 ---- */
typedef struct { int vk; int n; } Rec;
static Rec g_rec[256]; static int g_recN = 0;
/* 事件序列：验证发送顺序用（如 Ctrl 必须先于方向键按下、后于方向键抬起） */
typedef struct { int vk; int up; } Ev;
static Ev g_seq[512]; static int g_seqN = 0;
static void RecAdd(int vk, int up) {
    if (!up) {
        for (int i = 0; i < g_recN; i++)
            if (g_rec[i].vk == vk) { g_rec[i].n++; return; }
        if (g_recN < 256) { g_rec[g_recN].vk = vk; g_rec[g_recN].n = 1; g_recN++; }
    }
    if (g_seqN < 512) { g_seq[g_seqN].vk = vk; g_seq[g_seqN].up = up; g_seqN++; }
}
LRESULT CALLBACK CapProc(int n, WPARAM w, LPARAM l) {
    if (n == HC_ACTION) {
        KBDLLHOOKSTRUCT *p = (KBDLLHOOKSTRUCT*)l;
        if (p->flags & LLKHF_INJECTED) {
            BOOL down = (w == WM_KEYDOWN || w == WM_SYSKEYDOWN);
            BOOL upEv = (w == WM_KEYUP   || w == WM_SYSKEYUP);
            if (down || upEv) RecAdd((int)p->vkCode, !down);
        }
    }
    return CallNextHookEx(NULL, n, w, l);
}
static int CountVk(int vk) {
    for (int i = 0; i < g_recN; i++) if (g_rec[i].vk == vk) return g_rec[i].n;
    return 0;
}
/* 事件序列里某个 (vk, up) 的索引；找不到返回 -1 */
static int SeqIdx(int vk, int up) {
    for (int i = 0; i < g_seqN; i++)
        if (g_seq[i].vk == vk && g_seq[i].up == up) return i;
    return -1;
}
static void ResetRec(void) { g_recN = 0; g_seqN = 0; }

/* ---- 滚轮注入计数钩子（场景Q/T 用）----
   统计"我们注入出去的滚轮事件"格数与方向和，纵轴与横轴各一套。这**不是**生产
   那个 LowLevelMouseProc（它做决策），所以不存在"注入事件被自己再放大"的风险。 */
static int g_wheelInj = 0;     /* 注入的**纵轴**事件条数 */
static int g_wheelSum = 0;     /* 注入的纵轴 delta 之和（方向也在这里） */
static int g_whInj = 0;        /* 注入的**横轴**事件条数 */
static int g_whSum = 0;        /* 注入的横轴 delta 之和（正=右，负=左） */
static void ResetWheelRec(void) { g_wheelInj = 0; g_wheelSum = 0; g_whInj = 0; g_whSum = 0; }
LRESULT CALLBACK WheelCapProc(int n, WPARAM w, LPARAM l) {
    if (n == HC_ACTION && (w == WM_MOUSEWHEEL || w == WM_MOUSEHWHEEL)) {
        const MSLLHOOKSTRUCT* m = (const MSLLHOOKSTRUCT*)l;
        if (m && (m->flags & LLMHF_INJECTED)) {
            int d = (int)(short)HIWORD(m->mouseData);
            if (w == WM_MOUSEWHEEL) { g_wheelInj++; g_wheelSum += d; }
            else                    { g_whInj++;    g_whSum    += d; }
        }
    }
    return CallNextHookEx(NULL, n, w, l);
}

/* ---- 仿真工具 ---- */
static void Key(int vk, BOOL down) {
    KBDLLHOOKSTRUCT ks; memset(&ks, 0, sizeof(ks));
    ks.vkCode = (DWORD)vk; ks.flags = 0;
    LowLevelKeyboardProc(HC_ACTION,
        down ? WM_KEYDOWN : WM_KEYUP, (LPARAM)&ks);
}
/* 系统键路径：真实 Alt 的按下/抬起是 WM_SYSKEYDOWN / WM_SYSKEYUP */
static void KeyAlt(int vk, BOOL down) {
    KBDLLHOOKSTRUCT ks; memset(&ks, 0, sizeof(ks));
    ks.vkCode = (DWORD)vk; ks.flags = 0;
    LowLevelKeyboardProc(HC_ACTION,
        down ? WM_SYSKEYDOWN : WM_SYSKEYUP, (LPARAM)&ks);
}

/* ---- 取走钩子投递的滚轮请求 ----
   这里**不**去数"目标窗口收到了几条 WM_MOUSEWHEEL"：滚轮消息的落点取决于
   焦点窗口 / 光标下方的窗口（还受系统"悬停滚动非活动窗口"设置影响），让测试
   依赖系统把消息派发到哪个窗口是不稳的 —— 实测 10 次里有 1 次因为光标被挪开
   而完全收不到。
   改成验证**钩子的决策**：它该不该投递请求、投递的参数对不对，以及**有没有
   吞掉物理那一格**（返回 1）。这部分完全在本进程内可观测，稳定。 */
static BOOL TakeWheelAccel(int* delta, int* mul) {
    MSG m;
    if (PeekMessageW(&m, g_mainWnd, WM_WHEEL_ACCEL, WM_WHEEL_ACCEL, PM_REMOVE)) {
        if (delta) *delta = (int)m.wParam;
        if (mul)   *mul   = (int)m.lParam;
        return TRUE;
    }
    return FALSE;
}
/* 取走钩子投递的"持续滚动"请求。 */
static BOOL TakeWheelEnh(int* delta) {
    MSG m;
    if (PeekMessageW(&m, g_mainWnd, WM_WHEEL_ENH, WM_WHEEL_ENH, PM_REMOVE)) {
        if (delta) *delta = (int)m.wParam;
        return TRUE;
    }
    return FALSE;
}
/* 取走钩子投递的**横向**滚动请求（Caps+Shift+滚轮）。 */
static BOOL TakeWheelHAccel(int* delta, int* mul) {
    MSG m;
    if (PeekMessageW(&m, g_mainWnd, WM_WHEEL_HACCEL, WM_WHEEL_HACCEL, PM_REMOVE)) {
        if (delta) *delta = (int)m.wParam;
        if (mul)   *mul   = (int)m.lParam;
        return TRUE;
    }
    return FALSE;
}
/* 模拟硬件滚轮：直接喂给生产用的鼠标钩子过程（flags=0 表示非注入），
   返回钩子的判定（1 = 这一格被吞掉，事件不再往下传）。
   注意只模拟"钩子的判断"，不会真的产生那原始一格。 */
static LRESULT Wheel(int delta, DWORD flags) {
    MSLLHOOKSTRUCT ms; memset(&ms, 0, sizeof(ms));
    ms.mouseData = (DWORD)(USHORT)(SHORT)delta << 16;   /* 高字是滚轮增量 */
    ms.flags = flags;
    return LowLevelMouseProc(HC_ACTION, WM_MOUSEWHEEL, (LPARAM)&ms);
}
/* 排干所有待处理消息（含残留 TIMER/TRIG） */
static void Sink(void) {
    MSG m;
    while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE))
        DispatchMessageW(&m);
}

/* 一边等一边泵：**用 MsgWaitForMultipleObjects，不要用 Sleep**。
   Sleep 期间本线程不在"等消息"状态，系统对低级钩子的处理是**直接跳过**
   （不是延后补发）—— 于是"另一个线程注入、本线程钩子捕获"的事件会时有时无
   地被漏记，断言就飘（实测 12 次里飘 1 次，表现为 LSHIFT 一次都没记到）。
   MsgWait 是真正的等消息状态，钩子能在第一时间被调用。 */
static void SinkWait(int ms) {
    MSG m;
    MsgWaitForMultipleObjects(0, NULL, FALSE, (DWORD)ms, QS_ALLINPUT);
    while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE))
        DispatchMessageW(&m);
}
/* 只泵消息，不等待执行线程。
   时序敏感的场景（"稳定期内抬手"）靠它 —— 一旦在这里等执行线程，
   CHORD_SETTLE 的 50ms 稳定期就在场景中途被绕过，测的东西就变了。
   需要等发送做完时，在断言前单独调 Drain()。
   用 SinkWait 而不是 Sleep：要一直在"等消息"状态，钩子才不会漏记注入事件。 */
static void Pump(int ms) {
    ULONGLONG end = GetTickCount64() + ms;
    while (GetTickCount64() < end)
        SinkWait(1);
    Sink();   /* 收尾：把压着的 TIMER 也处理掉 */
}

/* 等执行线程把当前场景的动作做完，并把注入事件"看全"。 */
static void Drain(void) {
    for (int i = 0; i < 3000 && ActInFlight() != 0; i++)
        SinkWait(1);
    /* 再等一段：注入事件到达本线程的钩子有延迟（本机实测可达 ~60ms） */
    ULONGLONG end = GetTickCount64() + 200;
    while (GetTickCount64() < end)
        SinkWait(1);
}

/* 场景收尾：松开 C、泵完消息，再等执行线程把注入做完 */
#define END_SCENE() do { Key(CLI_CAPS, FALSE); Pump(50); Drain(); } while (0)

/* 场景间复位：清引擎状态与残留消息，保证场景相互独立 */
static void Reset(void) {
    g_cMod = FALSE;
    ModeResetAll();          /* 模式表整体复位：门闩 + 按住位图（替代旧的 g_dMod/g_pendD） */
    g_actEnh = FALSE; g_actShf = FALSE; g_btnCount = 0;
    ActAutoScrollStop();     /* 上一场景可能还在自动滚动：先停，避免尾流串到下一场景 */
    memset((void*)g_phys, 0, sizeof(g_phys));   /* 物理按键表也归零 */
    PostMessageW(g_mainWnd, WM_CAPS_DOWN, 0, 0);  /* → ResetEngine：清 pending/active + 停表 */
    Sink();
    Drain();   /* 等上一场景的尾巴收干净，否则残留注入会被算进下一场景 */
    ResetRec();
}

static int fails = 0;

/* 写一份临时 ini 供 ConfigLoadFrom 用（场景O：超限改道的契约验证）。
   ConfigLoadFrom 会先把所有项复位成缺省再应用文件内容，所以每写必"干净"。 */
static void WriteChordIni(const char* body) {
    FILE* f = NULL;
    if (fopen_s(&f, "bin/chordtest.ini", "wb") == 0 && f) {
        fputs(body, f);
        fclose(f);
    }
}
#define CHECK(cond, name) do { \
    if (cond) printf("  [OK]   %s\n", name); \
    else      { printf("  [FAIL] %s   (captured: ", name); \
               for(int __i=0;__i<g_recN;__i++) printf("%d:%d ",g_rec[__i].vk,g_rec[__i].n);\
               printf(")\n"); fails++; } } while (0)

int main(void) {
    g_state.paused = FALSE;
    SetWindowsHookExW(WH_KEYBOARD_LL, CapProc, GetModuleHandleW(NULL), 0);
    /* 滚轮注入计数（场景Q/T）：只计数，不做任何决策 */
    SetWindowsHookExW(WH_MOUSE_LL, WheelCapProc, GetModuleHandleW(NULL), 0);

    /* 生产路径里动作由 act.c 的执行线程发送，测试必须起同一个线程，
       否则测的是"同步兜底"分支，覆盖不到真实时序。 */
    ActStart();

    /* 键位表必须在引擎工作前装配好。生产路径在 WinMain 里调 KeyMapInit；
       本测试不走 WinMain（main.c 的 WinMain 被改名弃用），所以要显式调 ——
       漏掉它模式表就是空的，Alt/D/F 一个都不生效（所有修饰场景会全红）。 */
    KeyMapInit();

    WNDCLASSW wc = {0};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"CapsEnhanceTest";
    RegisterClassW(&wc);
    g_mainWnd = CreateWindowW(L"CapsEnhanceTest", L"t",
                              WS_POPUP | WS_OVERLAPPEDWINDOW,
                              CW_USEDEFAULT, CW_USEDEFAULT, 1, 1,
                              NULL, NULL, GetModuleHandleW(NULL), NULL);

    /* ---- 场景A：J 先于 D 到达（乱序和弦），点按 -= 3 左键 ---- */
    Reset();
    Key(CLI_CAPS, TRUE); Pump(2);
    Key('J', TRUE);  Pump(2);     /* J 先到 -> 进待发 */
    Key('D', TRUE);  Pump(2);     /* D 迟到 (乱序) -> 门闩 D */
    Key('J', FALSE); Pump(2);     /* 未到稳定期即抬手 -> 短点开火+补齐到3 */
    Key('D', FALSE); Pump(2);
    END_SCENE();
    CHECK(CountVk(VK_LEFT) == 3, "A: 乱序 C+J+D 点按 = 恰好 3 次左键");

    /* ---- 场景B：F 使 Shift 生效（严格序 C F J） ---- */
    Reset();
    Key(CLI_CAPS, TRUE); Pump(2);
    Key('F', TRUE); Pump(2);     /* 门闩 F */
    Key('J', TRUE);  Pump(2);
    Key('J', FALSE); Pump(2);    /* 短点：Shift 生效 */
    Key('F', FALSE); Pump(2);
    END_SCENE();
    CHECK(CountVk(VK_LEFT) == 1 && CountVk(VK_LSHIFT) >= 1,
          "B: C+F+J = Shift+左键 (1次)");

    /* ---- 场景C：F 乱序（J 先于 F）---- */
    Reset();
    Key(CLI_CAPS, TRUE); Pump(2);
    Key('J', TRUE);  Pump(2);
    Key('F', TRUE);  Pump(2);    /* F 迟到仍未生效于本次短点 */
    Key('J', FALSE); Pump(2);
    Key('F', FALSE); Pump(2);
    END_SCENE();
    CHECK(CountVk(VK_LEFT) == 1 && CountVk(VK_LSHIFT) >= 1,
          "C: 乱序 C+F+J 仍 = Shift+左键 (1次)");

    /* ---- 场景D：C+D+F+J 乱序 = Shift+左键 x3 ---- */
    Reset();
    Key(CLI_CAPS, TRUE); Pump(2);
    Key('J', TRUE);  Pump(2);
    Key('D', TRUE);  Pump(2);
    Key('F', TRUE);  Pump(2);
    Key('J', FALSE); Pump(2);    /* 短点：D/F 门闩均生效 -> 3 次 Shift+左键 */
    Key('D', FALSE); Pump(2);
    Key('F', FALSE); Pump(2);
    END_SCENE();
    CHECK(CountVk(VK_LEFT) == 3 && CountVk(VK_LSHIFT) >= 3,
          "D: 乱序 C+D+F+J = Shift+左键 x3");

    /* ---- 场景E：长按 C+J（无 D）连续重复，松手后不再输出 ----
       发送是异步的：读计数前先 Drain，比较点都取在"松手之后"，
       这样断言的是"松手之后不再产生新的输出"，而不是跟异步时序赛跑。 */
    Reset();
    Key(CLI_CAPS, TRUE); Pump(2);
    Key('J', TRUE); Pump(300);   /* 稳定期开火 + 首次150ms重复 + 若干 */
    Drain();
    int duringHold = CountVk(VK_LEFT);
    Key('J', FALSE); Pump(2);    /* 抬手：不应再补(无D) */
    END_SCENE();
    int afterUp = CountVk(VK_LEFT);
    Pump(400); Drain();
    CHECK(duringHold >= 2, "E: 长按 C+J 连续重复");
    CHECK(afterUp - duringHold <= 1, "E: 松手后最多只剩已派发的那一发");
    CHECK(CountVk(VK_LEFT) == afterUp, "E: 松手后不再继续输出左键");

    /* ---- 场景F：单点 C+J 无 D = 1 左键（不补齐） ---- */
    Reset();
    Key(CLI_CAPS, TRUE); Pump(2);
    Key('J', TRUE);  Pump(2);
    Key('J', FALSE); Pump(2);    /* 未到稳定期 -> 短点 */
    END_SCENE();
    CHECK(CountVk(VK_LEFT) == 1, "F: 单点 C+J = 1 左键");

    /* ---- 场景G：按住 C 时 F 单独按住（不触发任何 trigger）= 0 注入 ---- */
    Reset();
    Key(CLI_CAPS, TRUE); Pump(2);
    Key('F', TRUE);  Pump(150);
    Key('F', FALSE); Pump(2);
    END_SCENE();
    CHECK(CountVk(VK_LEFT) == 0 && CountVk(VK_LSHIFT) == 0,
          "G: C+F 按住无 trigger = 不注入任何键");

    /* ---- 场景H：按住 C+F 依次点 J、K，两次都应 Shift 位移 ---- */
    Reset();
    Key(CLI_CAPS, TRUE); Pump(2);
    Key('F', TRUE);  Pump(2);    /* F 一直按住 */
    Key('J', TRUE);  Pump(2);
    Key('J', FALSE); Pump(2);    /* Shift+左 */
    Key('K', TRUE);  Pump(2);
    Key('K', FALSE); Pump(2);    /* Shift+下 */
    Key('F', FALSE); Pump(2);
    END_SCENE();
    CHECK(CountVk(VK_LEFT) == 1 && CountVk(VK_DOWN) == 1 && CountVk(VK_LSHIFT) >= 2,
          "H: C+F 按住点 J/K 均带 Shift");

    /* ---- 场景I：J、K 同时按住（曾导致"一直输出左键"）----
       旧实现钩子只记一个按住的键：J 按住时再按 K 会把 J 顶掉，
       J 的抬起事件再也发不出去，引擎收不到 UP 就永远重复 LEFT。 */
    Reset();
    Key(CLI_CAPS, TRUE); Pump(2);
    Key('J', TRUE);  Pump(200);      /* 长按 J：重复左键 */
    int leftAtK = CountVk(VK_LEFT);
    Key('K', TRUE);  Pump(200);      /* J 未抬起时按 K：应切到下键 */
    int downAtK = CountVk(VK_DOWN);
    Key('K', FALSE); Pump(120);      /* 松开 K：退回仍按着的 J，继续左键 */
    int leftAfterKUp = CountVk(VK_LEFT);
    Key('J', FALSE); Pump(2);        /* 松开 J：必须停止 */
    Pump(50); Drain();
    int leftFinal = CountVk(VK_LEFT);
    int downFinal = CountVk(VK_DOWN);
    Pump(400); Drain();              /* 再等 400ms：不应再有任何注入 */
    END_SCENE();
    CHECK(downAtK >= 1, "I: J 按住时再按 K -> 切到下键");
    CHECK(leftAfterKUp > leftAtK, "I: 松开 K 后退回仍按着的 J（继续左键）");
    CHECK(CountVk(VK_LEFT) == leftFinal && CountVk(VK_DOWN) == downFinal,
          "I: 全部抬起后重复停止（不再无限输出左键）");

    /* ---- 场景J：左 Alt 等同 D（增益），走系统键消息路径 ---- */
    Reset();
    Key(CLI_CAPS, TRUE); Pump(2);
    KeyAlt(VK_LMENU, TRUE);  Pump(2);   /* WM_SYSKEYDOWN：门闩 D */
    Key('J', TRUE);  Pump(2);
    Key('J', FALSE); Pump(2);           /* 短点：增益补齐到 3 */
    KeyAlt(VK_LMENU, FALSE); Pump(2);
    END_SCENE();
    CHECK(CountVk(VK_LEFT) == 3, "J: 左Alt 等同 D（Caps+Alt+J = 3 次左键）");
    CHECK(CountVk(VK_LMENU) == 0, "J: Alt 本身不外泄（被屏蔽）");

    /* ---- 场景K：长按 J 中途松开 Caps -> 重复必须立刻停止 ---- */
    Reset();
    Key(CLI_CAPS, TRUE); Pump(2);
    Key('J', TRUE);  Pump(200);          /* 长按重复左键 */
    Key(CLI_CAPS, FALSE); Pump(2);       /* J 还按着就松开 C：映射失效 */
    Key('J', FALSE); Pump(2);
    Pump(50); Drain();
    int nKStopped = CountVk(VK_LEFT);
    Pump(400); Drain();                  /* 再等 400ms：不应再有注入 */
    CHECK(CountVk(VK_LEFT) == nKStopped, "K: 松开 Caps 后重复立刻停止");

    /* ---- 场景L：清卡键（SendClearStuckModifiers 的判定）----
       卡键 = OS 逻辑按下 && 物理未按。注入的键同样会置位逻辑状态，
       但不会更新物理表 —— 所以注入 down 之后调用清理，应补发 up。 */
    Reset();
    SendEvent(VK_LCONTROL, FALSE); Pump(30);     /* 注入 Ctrl down（逻辑按下） */
    CHECK(g_phys[VK_LCONTROL] == 0, "L: 注入的键不进物理表");
    SendClearStuckModifiers(); Pump(30); Drain();
    CHECK(SeqIdx(VK_LCONTROL, 1) >= 0, "L: 卡住的 Ctrl 被补发抬起");
    SendEvent(VK_LCONTROL, TRUE); Pump(10);      /* 收尾：确保系统状态干净 */
    Pump(30);

    /* 物理按住的键不能被清掉（用户真的按着 Ctrl 时清理应放过它）。
       模拟：注入 down 后手工把"注入过"标记清零、物理表置位，
       相当于"用户物理按住且与我们无关"的状态。 */
    SendEvent(VK_LCONTROL, FALSE); Pump(30);
    /* 直接清 send.c 的注入标记：g_injectedDown 在同一编译单元 */
    for (int i = 0; i < 8; i++)
        if (kMods[i] == VK_LCONTROL) g_injectedDown[i] = FALSE;
    g_phys[VK_LCONTROL] = 1;                     /* 物理按住 */
    ResetRec();
    SendClearStuckModifiers(); Pump(30); Drain();
    CHECK(SeqIdx(VK_LCONTROL, 1) < 0, "L: 物理按住的 Ctrl 不被清理");
    g_phys[VK_LCONTROL] = 0;
    SendEvent(VK_LCONTROL, TRUE); Pump(10);      /* 收尾 */
    Pump(30);

    /* ---- 场景M：原生词跳的按键序列（Ctrl 提前抬起 bug 的回归）----
       Ctrl 必须先于方向键按下、后于方向键抬起，否则目标收到的
       只是单格方向键。 */
    Reset();
    SendNativeWordJump(DIR_LEFT, FALSE);
    Drain();   /* 注入事件要泵消息才会被本线程的捕获钩子看到 */
    int cd  = SeqIdx(VK_LCONTROL, 0);
    int ld  = SeqIdx(VK_LEFT, 0);
    int lu  = SeqIdx(VK_LEFT, 1);
    int cu  = SeqIdx(VK_LCONTROL, 1);
    CHECK(cd >= 0 && ld >= 0 && lu >= 0 && cu >= 0,
          "M: Ctrl+方向 四段序列齐全");
    CHECK(cd < ld && ld < lu && lu < cu,
          "M: 顺序正确 Ctrl↓ -> ←↓ -> ←↑ -> Ctrl↑");
    CHECK(CountVk(VK_LCONTROL) == 1, "M: Ctrl 只按一次");

    /* ---- 场景N：按住 C 时鼠标滚轮的决策（三路分流 + 吞格只发生在横向） ----
       Caps + 滚轮            = **逐格放大**（WheelScrollMul 倍）-> WM_WHEEL_ACCEL
       Caps + Alt/D + 滚轮     = **持续滚动**                   -> WM_WHEEL_ENH
       Caps + Shift + 滚轮     = **横向滚动**（吞物理格）        -> WM_WHEEL_HACCEL
       纵向两路都**不吞**物理格（它们是"加强"不是"替换"——尤其持续滚动档，
       若吞掉那一格，缺省最慢档 24000ms/格 下"滚一下二十多秒没反应"）。
       注：不把生产的 LowLevelMouseProc 真正挂进钩子链 —— 万一哪天
       LLMHF_INJECTED 守卫被删掉，真挂链上会让注入事件自我放大成风暴。
       直接调用它喂"硬件事件"/"注入事件"两条路径，都能确定性地覆盖到。 */
    Reset();
    int d = 0, mul = 0;

    Key(CLI_CAPS, FALSE); Pump(2);       /* 先确保未按 C 的基线 */
    CHECK(Wheel(120, 0) != 1 && !TakeWheelAccel(NULL, NULL),
          "N: 未按 C -> 不吞、不投递");

    Key(CLI_CAPS, TRUE); Pump(2);
    CHECK(Wheel(120, 0) != 1, "N: 只按 C（逐格放大）-> 物理那一格照常放行");
    CHECK(TakeWheelAccel(&d, &mul) && d == 120 && mul == 2,
          "N: 只按 C -> 投递放大请求（倍数 = WheelScrollMul 缺省 2）");
    CHECK(!TakeWheelAccel(NULL, NULL), "N: 一次滚轮只投递一次请求");

    Wheel(-120, 0);                      /* 方向要带对 */
    CHECK(TakeWheelAccel(&d, &mul) && d == -120 && mul == 2,
          "N: 按住 C 下滚 -> 投递 delta=-120（方向原样带过去）");

    /* 加上增强修饰键 -> 持续滚动（另一条消息） */
    Key('D', TRUE); Pump(2);
    CHECK(Wheel(120, 0) != 1, "N: C+D+滚轮（持续滚动）-> 物理那一格也放行");
    CHECK(TakeWheelEnh(&d) && d == 120, "N: C+D+滚轮 -> 投递持续滚动请求");
    CHECK(!TakeWheelAccel(NULL, NULL), "N: 持续滚动时不走放大通道（互斥）");
    Key('D', FALSE); Pump(2);

    Wheel(120, LLMHF_INJECTED);          /* 我们注入出去的滚轮回到钩子 */
    CHECK(!TakeWheelAccel(NULL, NULL) && !TakeWheelEnh(NULL),
          "N: 注入的滚轮不再投递（防无限放大）");

    g_state.paused = TRUE;               /* 暂停映射：也不接管 */
    CHECK(Wheel(120, 0) != 1 && !TakeWheelAccel(NULL, NULL), "N: 暂停时不吞也不投递");
    g_state.paused = FALSE;

    /* WheelScrollMul < 2 -> 放大关闭：不投递放大请求（物理格本来就没吞） */
    WriteChordIni("[General]\r\nWheelScrollMul = 1\r\n");
    ConfigLoadFrom(L"bin/chordtest.ini");
    CHECK(Wheel(120, 0) != 1 && !TakeWheelAccel(NULL, NULL),
          "N: WheelScrollMul=1 -> 不放大（完全按系统原本行为）");
    ConfigLoadFrom(L"bin/__absent__.ini");   /* 回缺省（2） */

    Key(CLI_CAPS, FALSE); Pump(2);

    /* ---- 场景P：Caps+Tab 是"单发"翻转 —— 按一次只切一次 ----
       ACT_CAPSLOCK 原来被当作可重复、可补齐的普通动作：长按期间每
       REPEAT_INTERVAL(35ms) 切一次，切换次数取决于按住时长（偶数次 = 没切，
       CapsLock 看上去"没反应"），现象就是"caps+tab 有时要按两下才能取消锁定"。
       现在：只切一次，不进重复节奏、也不做松手补齐。
       注意 VK_CAPITAL 是真实开关键，注入会切换**本机**大小写状态 —— 测完恢复。 */
    int capsWasOn = (GetKeyState(VK_CAPITAL) & 1) ? 1 : 0;

    Reset();
    Key(CLI_CAPS, TRUE); Pump(2);
    Key(VK_TAB, TRUE); Pump(250);        /* 远长于稳定期 50ms + 多个重复周期 */
    Key(VK_TAB, FALSE); Pump(2);
    END_SCENE();
    CHECK(CountVk(VK_CAPITAL) == 1, "P: 长按 C+Tab 只切换一次（不重复）");

    Reset();
    Key(CLI_CAPS, TRUE); Pump(2);
    Key('D', TRUE); Pump(2);             /* 增益门闩：补齐规则对开关键不适用 */
    Key(VK_TAB, TRUE); Pump(2);
    Key(VK_TAB, FALSE); Pump(2);         /* 短点 */
    Key('D', FALSE); Pump(2);
    END_SCENE();
    CHECK(CountVk(VK_CAPITAL) == 1, "P: 带增益短点 C+D+Tab 也只切一次（不补齐）");

    /* 争议路径：另一个键正在重复时按下 Tab（开关键），只切一次；
       随后松开那个键（引擎的"退回上一个仍按着的键"）不能把它再切一次
       —— 切两次等于又切回去，用户看到的还是"没反应"。 */
    Reset();
    Key(CLI_CAPS, TRUE); Pump(2);
    Key('J', TRUE); Pump(200);           /* J 进入长按重复 */
    Key(VK_TAB, TRUE); Pump(2);          /* 切换分支：应立即切一次 */
    Drain();
    int capsAfterTab = CountVk(VK_CAPITAL);
    Key('J', FALSE); Pump(2);            /* 松开 J：不应"退回"到 Tab 再切 */
    Drain();
    int capsAfterJUp = CountVk(VK_CAPITAL);
    Key(VK_TAB, FALSE); Pump(2);
    END_SCENE();
    CHECK(capsAfterTab == 1 && capsAfterJUp == 1 && CountVk(VK_CAPITAL) == 1,
          "P: J 重复中按 Tab 只切一次（松手不会退回补切）");

    /* 恢复本机 CapsLock 状态（测试不改用户的键盘状态） */
    if (((GetKeyState(VK_CAPITAL) & 1) ? 1 : 0) != capsWasOn) {
        SendKeyOnce(VK_CAPITAL, FALSE);
        Pump(30); Drain();
    }

    /* ---- 场景Q：滚轮注入跑到专用线程上（不在主线程 Sleep） ----
       注入里带 Sleep（逐格 2ms），主线程一旦不在"等消息"状态，系统对低级钩子的
       事件是**直接跳过**（不是延后补发）——松开 Caps 的 keyup 被漏掉就卡住
       g_cMod，表现是"松开 C 后还在滚 + 之后按 Tab 冒出大写锁定"。
       这里验证经专用线程发出去之后，格数与方向都正确。两轴语义不同：
         · 纵向放大：只补 (mul-1) 格（物理那格由系统照常送达）；
         · 横向滚动：注入**全部** n 格（物理那格已被钩子吞掉）。
       倍数换算单独断言 SendWheelExtraCount（真注入条数不好稳定观测）。
       左右各来一次（净滚动量为 0），避免测试真的把光标下的窗口滚走。 */
    Reset();
    Pump(150); Sink();                   /* 先把残留的请求冲干净 */

    CHECK(SendWheelExtraCount(0) == 0 && SendWheelExtraCount(1) == 0,
          "Q: 倍数 0/1 -> 不补格（等同关闭）");
    CHECK(SendWheelExtraCount(2) == 1 && SendWheelExtraCount(3) == 2,
          "Q: 倍数 -> 补 N-1 格");
    CHECK(SendWheelExtraCount(999) == WHEEL_EXTRA_MAX,
          "Q: 倍数超上限 -> 补格数被钳住");

    ResetWheelRec();
    ActWheelPost(120, 2);                /* 纵向倍数 2 -> 补 1 格 */
    Pump(100); Drain();
    CHECK(g_wheelInj == 1 && g_wheelSum != 0, "Q: 纵向倍数 2 -> 补 1 格");
    CHECK(g_whInj == 0, "Q: 纵向不会误发到横轴");

    ResetWheelRec();
    ActWheelPost(-120, 3);               /* 方向原样带过去 */
    Pump(100); Drain();
    CHECK(g_wheelInj == 2 && g_wheelSum < 0, "Q: 倍数 3 -> 补 2 格、方向为负");

    ResetWheelRec();
    ActWheelPost(120, 1);                /* 倍数 1 = 不放大 -> 一格都不补 */
    Pump(100); Drain();
    CHECK(g_wheelInj == 0, "Q: 倍数 1 -> 一格都不补（物理格交给系统）");

    ResetWheelRec();
    ActWheelPostH(120, 1);               /* 横向倍数 1 -> 注入 1 格 */
    Pump(100); Drain();
    CHECK(g_whInj == 1 && g_whSum != 0, "Q: 横向倍数 1 -> 注入 1 格");

    ResetWheelRec();
    ActWheelPostH(120, 3);               /* 横向倍数 3 -> 注入 3 格 */
    Pump(100); Drain();
    CHECK(g_whInj == 3, "Q: 横向倍数 3 -> 注入 3 格");
    CHECK(g_wheelInj == 0, "Q: 横向不会误发到纵轴");

    ResetWheelRec();
    ActWheelPostH(-120, 2);              /* 方向原样带过去 */
    Pump(100); Drain();
    CHECK(g_whInj == 2, "Q: 横向反向同样注入 2 格");

    ResetWheelRec();
    ActWheelPostH(120, 0);               /* 无效参数 */
    ActWheelPostH(0, 3);
    Pump(100); Drain();
    CHECK(g_whInj == 0, "Q: 倍数 0 或 delta=0 -> 一格都不注入");

    /* ---- 场景O：超限"改道"的落点校验契约 ----
       背景（QQ 实测）：MaxArrowSteps 超限后 SendArrows 改发原生 Ctrl+方向，
       但改道的落点由**目标程序自己的词切分**决定 —— QQ/Chromium 按 ICU
       词典切、1~2 字一步，我们按"连续汉字段"整段算（一次 10 字），两者不一致
       时改道会把光标丢在整段中间（日志实例：期望 82，改道后停在 92，再按一次
       停在 90）。修法：SendArrows 把"本次是改道"返回给调用方，nav.c 对改道这
       一次也回读落点并按差额补足；补正自己走 SendArrowsStrict，绝不再改道。
       真实落点要有真编辑器才能验；这里验的是发送层契约（可确定性断言）：
       哪种情形返回哪个模式、注入的键序列对不对。 */
    WriteChordIni("[General]\r\nMaxArrowSteps = 3\r\nOverStepMode = 1\r\n");
    ConfigLoadFrom(L"bin/chordtest.ini");

    ResetRec();
    int mode = SendArrows(DIR_LEFT, 5, FALSE);     /* 5 >= 3：超限 */
    Pump(30);
    CHECK(mode == ARROW_SEND_HANDOFF, "O: 超限+OverStepMode=1 -> 返回\"已改道\"");
    CHECK(CountVk(VK_LCONTROL) == 1 && CountVk(VK_LEFT) == 1 && CountVk(VK_RIGHT) == 0,
          "O: 改道发的是 Ctrl+方向（一支，不逐格）");
    CHECK(SeqIdx(VK_LCONTROL, 0) < SeqIdx(VK_LEFT, 0)
          && SeqIdx(VK_LEFT, 1) < SeqIdx(VK_LCONTROL, 1),
          "O: Ctrl 先按后抬（夹住方向键）");

    ResetRec();
    mode = SendArrows(DIR_LEFT, 2, FALSE);         /* 2 < 3：不超限 */
    Pump(30);
    CHECK(mode == ARROW_SEND_STEPS && CountVk(VK_LEFT) == 2 && CountVk(VK_LCONTROL) == 0,
          "O: 未超限 -> 逐格 2 步、不带 Ctrl");

    ResetRec();
    SendArrowsStrict(DIR_LEFT, 5, FALSE);          /* 补正：即使超限也必须逐格 */
    Pump(30);
    CHECK(CountVk(VK_LEFT) == 5 && CountVk(VK_LCONTROL) == 0,
          "O: 补正路径不因超限再改道（逐格 5 步、无 Ctrl）");

    WriteChordIni("[General]\r\nMaxArrowSteps = 3\r\nOverStepMode = 0\r\n");
    ConfigLoadFrom(L"bin/chordtest.ini");
    ResetRec();
    mode = SendArrows(DIR_LEFT, 5, FALSE);
    Pump(30);
    CHECK(mode == ARROW_SEND_ONE && CountVk(VK_LEFT) == 1 && CountVk(VK_LCONTROL) == 0,
          "O: OverStepMode=0 -> 只发一格，返回\"不补正\"");

    /* 复位成缺省：本场景是最后一个，但别给以后新加的场景留坑
       （SendArrows 的行为受 MaxArrowSteps/OverStepMode 影响） */
    ConfigLoadFrom(L"bin/__absent__.ini");         /* 文件不存在 -> 全部回缺省 */

    /* ---- 场景R：Caps+增强修饰+滚轮 = 持续滚动 ----
       "滚一下就一直滚、继续同向滚提速、反向滚/松开修饰/松开 C 立即停"，
       速度两端 = AutoScrollInterval（起步）/ AutoScrollFastMs（上限），
       **各自独立配置**（不再跟倍数项 WheelScrollMul 共用任何值）。
       状态机（active/dir/gear）由主线程在锁内维护，可确定性断言；
       "真的在注入"由滚轮计数钩子观测（方向要与 dir 一致）。
       计数钩子只数**我们注入的**滚轮（LLMHF_INJECTED），与场景Q/T 共用，
       所以每段观测前先 ResetWheelRec。 */
    Reset();
    int base = ConfigGetAutoScrollInterval();     /* 最慢档 = 24000 */
    int fast = ConfigGetAutoScrollFastMs();        /* 最快档 = 1000（独立配置项） */
    CHECK(fast == 1000, "R: 最快档 = AutoScrollFastMs 缺省 1000ms/格");
    CHECK(ConfigGetWheelScrollMul() == 2,
          "R: 倍数项与水平无关（WheelScrollMul 仍是 2，两边各自独立）");
    CHECK(ActAutoScrollTick() == 0, "R: 未启动时速度为 0");
    CHECK(!ActAutoScrollActive(), "R: 初始未在连续滚动");

    Key(CLI_CAPS, TRUE); Pump(2);
    KeyAlt(VK_LMENU, TRUE); Pump(2);              /* 默认增强键：左 Alt */
    CHECK(ModeDown(MODE_ENHANCE), "R: 按住 Alt -> 增强模式生效");

    /* 初始速度按**人的阅读速度**定的（见 config.c 的换算表）：
       24000ms/格 = 7.5 行/分钟 ≈ 338 字/分钟，落在默读 300..500 的舒适区。 */
    CHECK(base == 24000, "R: 初始速度缺省 24000ms（≈338 字/分钟，按阅读速度定）");
    CHECK(ConfigGetAutoScrollSmooth(), "R: 平滑注入缺省开启");

    ActAutoScrollWheel(120);               /* 第一滚：增强档、最慢档、向上 */
    CHECK(ActAutoScrollActive() &&
          ActAutoScrollDir() == 1 && ActAutoScrollGear() == 1,
          "R: 从最慢档启动（档 1 / 向上）");
    CHECK(ActAutoScrollTick() == base, "R: 第 1 档速度 = AutoScrollInterval");

    /* 平滑注入：把一格拆成小数刻度、发得更密，但**速度不变**。
       最慢档（24000ms/格）已经拆到整数刻度的下限：delta = 1。 */
    CHECK(ActAutoScrollDelta() == 1,
          "R: 最慢档拆到最小整数刻度（delta=1，整刻度的物理下限）");
    CHECK(ActAutoScrollBeat() < ActAutoScrollTick(),
          "R: 平滑模式节拍更短（注入更密）");
    {
        /* 速度等价：delta/beat 应等于 120/每格时长。delta 是整数刻度，必然
           有四舍五入，容差取半个节拍（|d - x| <= 0.5  =>  |d*tick - 120*beat| <= tick/2）。 */
        int lhs = ActAutoScrollDelta() * ActAutoScrollTick();
        int rhs = WHEEL_STD_DELTA * ActAutoScrollBeat();
        int tol = ActAutoScrollTick() / 2 + 1;
        CHECK(lhs >= rhs - tol && lhs <= rhs + tol,
              "R: 平滑前后速度等价（与整格模式一致，仅整数刻度舍入误差）");
    }

    /* 逐档提速：中间档不止两档，速度单调变快（每格时长单调变短） */
    int t1 = ActAutoScrollTick();
    ActAutoScrollWheel(120);
    int t2 = ActAutoScrollTick();
    CHECK(ActAutoScrollGear() == 2 && t2 < t1, "R: 同向再滚 -> 进第 2 档、速度变快");
    ActAutoScrollWheel(120);
    int t3 = ActAutoScrollTick();
    CHECK(ActAutoScrollGear() == 3 && t3 < t2, "R: 再滚 -> 第 3 档（中间档不止两档）");

    /* 到顶：速度恰好等于 WheelScrollMs，**不会超过**（用户要的上限） */
    for (int i = 0; i < 10; i++) ActAutoScrollWheel(120);
    int tTop = ActAutoScrollTick(), gTop = ActAutoScrollGear();
    ActAutoScrollWheel(120);
    CHECK(ActAutoScrollGear() == gTop && ActAutoScrollTick() == tTop,
          "R: 到顶档后不再提速（档位饱和）");
    CHECK(tTop == fast, "R: 顶档速度 == AutoScrollFastMs");
    CHECK(tTop <= base, "R: 最快档不会比初始档还慢（阶梯方向正确）");
    CHECK(ActAutoScrollDelta() > 0 && ActAutoScrollDelta() < 120,
          "R: 顶档仍是小数刻度（平滑提速靠加 delta，不是加格数）");

    /* 关掉平滑 -> 退回整格：delta 恒为 120、节拍就是速度档 */
    WriteChordIni("[General]\r\nAutoScrollSmooth = 0\r\n");
    ConfigLoadFrom(L"bin/chordtest.ini");
    CHECK(!ConfigGetAutoScrollSmooth(), "R: AutoScrollSmooth=0 生效");
    CHECK(ActAutoScrollDelta() == WHEEL_STD_DELTA,
          "R: 关平滑后每拍发整格 120（兼容不累积小数刻度的程序）");
    CHECK(ActAutoScrollBeat() == ActAutoScrollTick(),
          "R: 关平滑后节拍 = 速度档");
    ConfigLoadFrom(L"bin/__absent__.ini");        /* 回缺省（平滑开） */
    CHECK(ConfigGetAutoScrollSmooth(), "R: 回缺省后平滑重新开启");

    ResetWheelRec();
    Pump(300);                                    /* 顶档 1000ms/格，300ms 内至少 1 拍 */
    CHECK(g_wheelInj >= 1 && g_wheelSum > 0, "R: 连续滚动确实在持续注入上滚");
    CHECK(ActAutoScrollDelta() > 0 && ActAutoScrollDelta() < 120,
          "R: 顶档也是小数刻度（平滑提速靠加 delta，不是加格数）");
    CHECK(ActAutoScrollBeat() < ActAutoScrollTick(),
          "R: 平滑节拍仍短于每格时长（细水长流而非一次一大格）");

    ActAutoScrollWheel(-120);              /* 反向：立即终止 + 清零速度 */
    CHECK(!ActAutoScrollActive() && ActAutoScrollDir() == 0 && ActAutoScrollGear() == 0,
          "R: 反向滚动 -> 立即终止并清零档位");

    /* 反向打断后再滚：必须从**最慢档**重来，不残留打断前的速度 */
    ActAutoScrollWheel(120);
    CHECK(ActAutoScrollActive() && ActAutoScrollGear() == 1 && ActAutoScrollTick() == base,
          "R: 反向打断后重置速度 -> 再滚从第 1 档（最慢）重来");
    ActAutoScrollWheel(-120);              /* 收起，免得影响后面的用例 */

    ResetWheelRec();
    ActAutoScrollWheel(-120);              /* 下滚启动（方向 -1） */
    CHECK(ActAutoScrollActive() && ActAutoScrollDir() == -1, "R: 下滚启动（方向 -1）");
    CHECK(ActAutoScrollDelta() < 0, "R: 下滚时 delta 为负（带方向）");
    Pump(300);
    CHECK(g_wheelSum < 0, "R: 下滚方向正确（与上方上滚大致抵消）");

    /* 松开修饰键：钩子投递 WM_MODE_UP -> 终止 */
    KeyAlt(VK_LMENU, FALSE); Pump(2);
    Sink();
    CHECK(!ActAutoScrollActive(), "R: 松开 Alt -> 立即终止连续滚动");

    /* 松开 C：WM_CAPS_UP -> ResetEngine -> 终止 */
    KeyAlt(VK_LMENU, TRUE); Pump(2);              /* 重新按住 Alt 才能再启动 */
    ActAutoScrollWheel(120);
    CHECK(ActAutoScrollActive(), "R: 重新启动成功");
    END_SCENE();                                  /* 松开 C */
    CHECK(!ActAutoScrollActive(), "R: 松开 Caps -> 立即终止连续滚动");

    /* 两端配成相等 -> 阶梯退化为恒定速度（而不是出错或反向提速） */
    WriteChordIni("[General]\r\nAutoScrollInterval = 1000\r\n"
                  "AutoScrollFastMs = 1000\r\n");
    ConfigLoadFrom(L"bin/chordtest.ini");
    ActAutoScrollWheel(120);
    CHECK(ActAutoScrollTick() == 1000, "R: 两端配成相等 -> 速度恒定 1000ms/格");

    /* 端到端：钩子路由（增强档 / 手动档）与吞格 */
    ConfigLoadFrom(L"bin/__absent__.ini");        /* 回缺省（1000ms） */
    Reset();
    Key(CLI_CAPS, TRUE); Pump(2);
    KeyAlt(VK_LMENU, TRUE); Pump(2);
    Sink();                                       /* 冲掉可能残留的消息 */
    ResetWheelRec();
    CHECK(Wheel(120, 0) != 1, "R: 持续滚动档 -> 物理格放行（不吞）");
    Pump(300);                                    /* 泵消息：驱动状态机并注入 */
    CHECK(ActAutoScrollActive() && ActAutoScrollDir() == 1 && g_wheelInj >= 1,
          "R: 经钩子路由 -> 启动持续滚动");
    END_SCENE();
    ConfigLoadFrom(L"bin/__absent__.ini");

    /* ---- 场景T：Caps+Shift+滚轮 = 横向滚动 ----
       只需真实 Shift（与 keymap 的"位移模式 F"无关）；速度 WheelHScrollMul 默认 1。
       轴向优先级高于纵向两路：Shift 排最前。
       方向照抄系统约定：滚轮**向上 = 向左**（浏览器 / 资源管理器 / 表格都是这个
       方向，Firefox 官方快捷键表就写着 Shift+上滚 = 向左）。 */
    Reset();
    CHECK(ConfigGetWheelHScrollMul() == 1, "T: WheelHScrollMul 缺省为 1（一滚一格）");

    Key(CLI_CAPS, TRUE); Pump(2);
    Key(VK_LSHIFT, TRUE); Pump(2);                /* 真实左 Shift */
    Sink();

    int hd = 0, hm = 0;
    LRESULT consumed = Wheel(120, 0);
    CHECK(consumed == 1, "T: Caps+Shift+滚轮 -> 钩子吞掉物理这一格（防横纵混滚）");
    CHECK(TakeWheelHAccel(&hd, &hm), "T: 投递横向滚动请求");
    CHECK(hd == 120 && hm == 1, "T: 请求带原始方向与倍数 1");
    CHECK(!TakeWheelAccel(NULL, NULL), "T: 不走纵向放大通道（轴向互斥）");
    CHECK(!TakeWheelEnh(NULL), "T: 也不启动持续滚动（按的是 Shift）");
    CHECK(!ActAutoScrollActive(), "T: 确实没有启动持续滚动");

    /* 方向：滚轮向上(delta>0) = 向左(负) —— 与系统 Shift+滚轮一致 */
    ResetWheelRec();
    ActWheelPostH(120, 1);
    Pump(100); Drain();
    CHECK(g_whSum < 0, "T: 滚轮向上 -> 向左（负 delta，同浏览器 Shift+上滚）");
    ResetWheelRec();
    ActWheelPostH(-120, 1);
    Pump(100); Drain();
    CHECK(g_whSum > 0, "T: 滚轮向下 -> 向右（正 delta）");

    /* 倍数可配：3 -> 钩子仍然只投递一条请求（由工作线程注入 3 格） */
    WriteChordIni("[General]\r\nWheelHScrollMul = 3\r\n");
    ConfigLoadFrom(L"bin/chordtest.ini");
    Sink();
    int hd3 = 0, hm3 = 0;
    CHECK(Wheel(120, 0) == 1 && TakeWheelHAccel(&hd3, &hm3) && hm3 == 3,
          "T: WheelHScrollMul=3 -> 请求倍数 3");
    ResetWheelRec();
    ActWheelPostH(120, 3);
    Pump(100); Drain();
    CHECK(g_whInj == 3 && g_whSum < 0, "T: 倍数 3 -> 注入 3 格、方向仍向左");
    ConfigLoadFrom(L"bin/__absent__.ini");        /* 回缺省（1） */

    /* 关闭（0）：不吞这一格，退回纵向语义 */
    WriteChordIni("[General]\r\nWheelHScrollMul = 0\r\n");
    ConfigLoadFrom(L"bin/chordtest.ini");
    Key(VK_LSHIFT, FALSE); Pump(2); Sink();
    Key(VK_LSHIFT, TRUE);  Pump(2); Sink();
    ResetWheelRec();
    CHECK(Wheel(120, 0) != 1, "T: WheelHScrollMul=0 -> 改由纵向接管（放大档，不吞物理格）");
    CHECK(!TakeWheelHAccel(NULL, NULL), "T: 关闭时不再投递横向请求");
    CHECK(TakeWheelAccel(NULL, NULL), "T: 关闭横向后退回纵向放大");
    ConfigLoadFrom(L"bin/__absent__.ini");

    /* 松开 Shift 就回到纵向；Caps+Shift 之外不受影响 */
    Key(VK_LSHIFT, FALSE); Pump(2); Sink();
    CHECK(Wheel(120, 0) != 1, "T: 松开 Shift 后退回纵向放大（不吞物理格）");
    int dS = 0, mS = 0;
    CHECK(TakeWheelAccel(&dS, &mS) && mS == 2, "T: 松开 Shift -> 纵向放大（缺省 2 倍）");
    CHECK(!TakeWheelHAccel(NULL, NULL), "T: 松开 Shift 后不再横向");

    /* 真 Shift 的按住状态必须被正确清掉（否则会粘住） */
    CHECK(!g_shiftMod, "T: 松开 Shift 后 g_shiftMod 归 FALSE");
    ModHoldReset();                               /* 暂停/重启路径 */
    CHECK(!g_shiftMod && !g_cMod, "T: ModHoldReset 清掉按住状态");

    Key(CLI_CAPS, TRUE); Pump(2);                 /* 用 Ctrl 也一样不该横向 */
    Key(VK_LCONTROL, TRUE); Pump(2); Sink();
    CHECK(Wheel(120, 0) != 1 && TakeWheelAccel(NULL, NULL),
          "T: Caps+Ctrl+滚轮 仍是纵向（只有 Shift 决定轴向）");
    CHECK(!TakeWheelHAccel(NULL, NULL), "T: Caps+Ctrl+滚轮 不横向");
    Key(VK_LCONTROL, FALSE); Pump(2);
    END_SCENE();

    /* ---- 场景S：EnhanceKeys / ShiftKeys 已**不再读配置** ----
       "配置项中，去掉 EnhanceKeys = Alt, D 与 ShiftKeys = F，没必要读取配置"
       —— 激活键改成编译期常量（cap.h 的 MODE_*_KEYS_DEFAULT）：配置文件里
       即使写了这两项也一律忽略（解析器不认这两个键名，KeyMapInit 也不读）。
       这么做之所以安全，是因为"模式"这层抽象还在 —— 引擎只问"增强模式生效
       吗"，从不判断按的是哪个键；以后真要改/删激活键，只改常量一处即可。 */
    WriteChordIni("[General]\r\nEnhanceKeys = X\r\nShiftKeys = Y\r\n");
    ConfigLoadFrom(L"bin/chordtest.ini");
    KeyMapInit();
    CHECK(ModeFindByVk(VK_LMENU) == MODE_ENHANCE && ModeFindByVk('D') == MODE_ENHANCE,
          "S: 配置里的 EnhanceKeys 被忽略 —— 增强键仍是固定的 Alt + D");
    CHECK(ModeFindByVk('F') == MODE_SHIFT, "S: 位移键仍是固定的 F");
    CHECK(ModeFindByVk('X') == -1 && ModeFindByVk('Y') == -1,
          "S: 配置里写的 X / Y 不会变成激活键");

    Reset();
    Key(CLI_CAPS, TRUE); Pump(2);
    KeyAlt(VK_LMENU, TRUE); Pump(2);              /* 固定的增强键：Alt */
    Key('J', TRUE);  Pump(2);
    Key('J', FALSE); Pump(2);                     /* 短点：增强模式 -> 补齐到 3 次 */
    KeyAlt(VK_LMENU, FALSE); Pump(2);
    END_SCENE();
    CHECK(CountVk(VK_LEFT) == 3, "S: 增强键仍是 Alt+D：C+Alt+J 补齐到 3 次左键");

    Reset();
    Key(CLI_CAPS, TRUE); Pump(2);
    Key('D', TRUE); Pump(2);                      /* D 同样是固定的增强键 */
    Key('J', TRUE);  Pump(2);
    Key('J', FALSE); Pump(2);
    Key('D', FALSE); Pump(2);
    END_SCENE();
    CHECK(CountVk(VK_LEFT) == 3, "S: C+D+J 同样补齐到 3 次左键（D 仍是增强键）");

    /* 复位默认键位（别给以后新增的场景留坑） */
    ConfigLoadFrom(L"bin/__absent__.ini");
    KeyMapInit();

    /* ---- 场景V：横向滚动的**注入方向**（与系统约定一致） ----
       "横向滚动的方向反了" —— 修好之后：滚轮**向上** = 向左（负 delta），
       向下 = 向右（正 delta）。这不是随便定的：`Shift + 滚轮` 在浏览器 /
       资源管理器 / 表格里就是这个方向（Firefox 官方快捷键表明写
       `Shift + 上滚 = 向左滚动`），照抄系统约定用户才不用重建肌肉记忆。
       这里直接查注入出去的横轴刻度符号（计数钩子只数 LLMHF_INJECTED）。 */
    Reset();
    Pump(150); Sink();                            /* 冲掉残留请求 */
    ResetWheelRec();
    ActWheelPostH(120, 1);                        /* 物理上滚 */
    Pump(100); Drain();
    CHECK(g_whInj == 1 && g_whSum < 0,
          "V: 滚轮向上 -> 注入向左的横轴刻度（负 delta）");
    ResetWheelRec();
    ActWheelPostH(-120, 1);                       /* 物理下滚 */
    Pump(100); Drain();
    CHECK(g_whInj == 1 && g_whSum > 0,
          "V: 滚轮向下 -> 注入向右的横轴刻度（正 delta）");
    CHECK(g_wheelInj == 0, "V: 横向不会误发到纵轴");

    printf(fails ? "\n== %d 项失败 ==\n" : "\n== 全部通过 ==\n", fails);
    return fails;
}