/* ============================================================
   和弦引擎回归测试：直接调用底层钩子进程，注入按键序列，
   用另一个捕获钩子统计软件真实注入的方向键/Shift，
   验证 D/F 修饰在"按键乱序到达"下仍能正确工作。
   ============================================================ */
#define WinMain __CapsEnhanceRealWinMain_Unused
#include "cap.h"
#include "main.c"
#undef WinMain
#include "hook.c"
#include "send.c"      /* hook.c 的 SendActOnce 现在走统一发送模块 */
#include "act.c"       /* 动作实际由执行线程完成：本测试要能起/停/等它 */
#include <stdio.h>

/* 词跳桩：本测试只验证 D/F 与补齐，不测文本解析 */
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

/* ---- 滚轮注入计数钩子（场景Q用）----
   统计"我们注入出去的滚轮事件"格数与方向和：验证挪到专用线程以后仍然逐格
   补发、格数换算正确。这**不是**生产那个 LowLevelMouseProc（它做加速决策），
   所以不存在"注入事件被自己再放大"的风险 —— 本钩子只计数。 */
static int g_wheelInj = 0;     /* 注入的滚轮事件条数 */
static int g_wheelSum = 0;     /* 注入的 delta 之和（方向也在这里） */
static void ResetWheelRec(void) { g_wheelInj = 0; g_wheelSum = 0; }
LRESULT CALLBACK WheelCapProc(int n, WPARAM w, LPARAM l) {
    if (n == HC_ACTION && w == WM_MOUSEWHEEL) {
        const MSLLHOOKSTRUCT* m = (const MSLLHOOKSTRUCT*)l;
        if (m && (m->flags & LLMHF_INJECTED)) {
            g_wheelInj++;
            g_wheelSum += (int)(short)HIWORD(m->mouseData);
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

/* ---- 滚轮加速：取走钩子投递的加速请求 ----
   这里**不**去数"目标窗口收到了几条 WM_MOUSEWHEEL"：滚轮消息的落点取决于
   焦点窗口 / 光标下方的窗口（还受系统"悬停滚动非活动窗口"设置影响），让测试
   依赖系统把消息派发到哪个窗口是不稳的 —— 实测 10 次里有 1 次因为光标被挪开
   而完全收不到。
   改成验证**钩子的决策**：它该不该把加速请求投递出来、投递的参数对不对。
   这部分完全在本进程内可观测，稳定。真正"逐格注入并经系统送达目标"的
   行为由一次性探针在跨进程场景下验证过（一次批 3 个会被系统合并成 1 条
   delta=360；间隔 2ms 逐个发则目标收到 3 条 delta=120），结论记在 send.c。 */
static BOOL TakeWheelAccel(int* delta, int* mul) {
    MSG m;
    if (PeekMessageW(&m, g_mainWnd, WM_WHEEL_ACCEL, WM_WHEEL_ACCEL, PM_REMOVE)) {
        if (delta) *delta = (int)m.wParam;
        if (mul)   *mul   = (int)m.lParam;
        return TRUE;
    }
    return FALSE;
}
/* 模拟硬件滚轮：直接喂给生产用的鼠标钩子过程（flags=0 表示非注入）。
   注意只模拟"钩子的判断"，不会真的产生那原始一格。 */
static void Wheel(int delta, DWORD flags) {
    MSLLHOOKSTRUCT ms; memset(&ms, 0, sizeof(ms));
    ms.mouseData = (DWORD)(USHORT)(SHORT)delta << 16;   /* 高字是滚轮增量 */
    ms.flags = flags;
    LowLevelMouseProc(HC_ACTION, WM_MOUSEWHEEL, (LPARAM)&ms);
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
    g_cMod = FALSE; g_dMod = FALSE; g_fMod = FALSE;
    g_pendD = FALSE; g_pendF = FALSE;
    g_actD = FALSE; g_actF = FALSE; g_btnCount = 0;
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
    /* 滚轮注入计数（场景Q）：只计数，不做任何加速决策 */
    SetWindowsHookExW(WH_MOUSE_LL, WheelCapProc, GetModuleHandleW(NULL), 0);

    /* 生产路径里动作由 act.c 的执行线程发送，测试必须起同一个线程，
       否则测的是"同步兜底"分支，覆盖不到真实时序。 */
    ActStart();

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

    /* ---- 场景N：按住 C 时鼠标滚轮的加速决策 ----
       配置里 WheelScrollMul 的缺省值是 2（本测试没读 ini，用的就是静态缺省），
       所以钩子应以 mul=2 投递加速请求，delta 原样带过去（方向也在里面）。
       注：不把生产的 LowLevelMouseProc 真正挂进钩子链 —— 万一哪天
       LLMHF_INJECTED 守卫被删掉，真挂链上会让注入事件自我放大成风暴。
       直接调用它喂"硬件事件"/"注入事件"两条路径，都能确定性地覆盖到。 */
    Reset();
    int d = 0, mul = 0;

    Key(CLI_CAPS, FALSE); Pump(2);       /* 先确保未按 C 的基线 */
    Wheel(120, 0);
    CHECK(!TakeWheelAccel(NULL, NULL), "N: 未按 C 时不投递加速请求");

    Key(CLI_CAPS, TRUE); Pump(2);
    Wheel(120, 0);                       /* 硬件上滚 */
    CHECK(TakeWheelAccel(&d, &mul) && d == 120 && mul == 2,
          "N: 按住 C 上滚 -> 投递 delta=120 mul=2");
    CHECK(!TakeWheelAccel(NULL, NULL), "N: 一次滚轮只投递一次请求");

    Wheel(-120, 0);                      /* 硬件下滚：方向要带对 */
    CHECK(TakeWheelAccel(&d, &mul) && d == -120 && mul == 2,
          "N: 按住 C 下滚 -> 投递 delta=-120");

    Wheel(120, LLMHF_INJECTED);          /* 我们注入出去的滚轮回到钩子 */
    CHECK(!TakeWheelAccel(NULL, NULL),
          "N: 注入的滚轮不再投递加速（防无限放大）");

    g_state.paused = TRUE;               /* 暂停映射：也不加速 */
    Wheel(120, 0);
    CHECK(!TakeWheelAccel(NULL, NULL), "N: 暂停时不投递加速请求");
    g_state.paused = FALSE;

    /* 倍数 -> 补发格数的换算（真注入条数无法在本进程稳定观测，单独断言换算） */
    CHECK(SendWheelExtraCount(3) == 2,  "N: mul=3 -> 补发 2 格");
    CHECK(SendWheelExtraCount(1) == 0,  "N: mul=1 -> 不补发");
    CHECK(SendWheelExtraCount(0) == 0,  "N: mul=0 -> 不补发");
    CHECK(SendWheelExtraCount(16) == 15, "N: mul=16 -> 补发 15 格");
    CHECK(SendWheelExtraCount(999) == 15, "N: 超过上限也只补发 15 格");

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

    /* ---- 场景Q：滚轮加速搬到专用线程后仍逐格补发 ----
       原来主线程自己在 Sleep(2ms) 循环里补发：主线程不在"等消息"状态期间，
       系统对低级钩子的事件是**直接跳过**（不是延后补发）——松开 Caps 的 keyup
       被漏掉就卡住 g_cMod，表现是"松开 C 后滚轮还在滚 + 之后按 Tab 冒出大写锁定"。
       这里验证搬运之后行为不变：格数换算与方向都正确。
       上下各来一次（净滚动量为 0），避免测试真的把光标下的窗口滚走。 */
    Reset();
    Pump(150); Sink();                   /* 先把残留的加速请求冲干净 */
    ResetWheelRec();

    ActWheelPost(120, 3);                /* mul=3 -> 补发 2 格 */
    Pump(100); Drain();
    CHECK(g_wheelInj == 2 && g_wheelSum == 240,
          "Q: mul=3 补发 2 格 delta=120（由工作线程注入）");

    ResetWheelRec();
    ActWheelPost(-120, 3);               /* 方向原样带过去 */
    Pump(100); Drain();
    CHECK(g_wheelInj == 2 && g_wheelSum == -240,
          "Q: 下滚同样补发 2 格 delta=-120");

    ResetWheelRec();
    ActWheelPost(120, 1);                /* 不放大 */
    ActWheelPost(120, 0);
    ActWheelPost(0, 3);
    Pump(100); Drain();
    CHECK(g_wheelInj == 0, "Q: mul<=1 或 delta=0 -> 一格都不补发");

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

    printf(fails ? "\n== %d 项失败 ==\n" : "\n== 全部通过 ==\n", fails);
    return fails;
}