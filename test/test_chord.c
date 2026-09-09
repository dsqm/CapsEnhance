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
#include <stdio.h>

/* 词跳桩：本测试只验证 D/F 与补齐，不测文本解析 */
int NavJump(int dir, BOOL shift) { (void)dir; (void)shift; return 0; }
void NavProbe(void) { /* 诊断探测：本测试桩掉 */ }
void NavBaseInfo(void) { /* 启动基线采集：本测试桩掉 */ }

/* ---- 捕获钩子：统计注入的按键（只计按下，按下+抬起=一次点按） ---- */
typedef struct { int vk; int n; } Rec;
static Rec g_rec[256]; static int g_recN = 0;
static void RecAdd(int vk) {
    for (int i = 0; i < g_recN; i++)
        if (g_rec[i].vk == vk) { g_rec[i].n++; return; }
    g_rec[g_recN].vk = vk; g_rec[g_recN].n = 1; g_recN++;
}
LRESULT CALLBACK CapProc(int n, WPARAM w, LPARAM l) {
    if (n == HC_ACTION) {
        KBDLLHOOKSTRUCT *p = (KBDLLHOOKSTRUCT*)l;
        if ((p->flags & LLKHF_INJECTED) &&
            (w == WM_KEYDOWN || w == WM_SYSKEYDOWN))
            RecAdd((int)p->vkCode);
    }
    return CallNextHookEx(NULL, n, w, l);
}
static int CountVk(int vk) {
    for (int i = 0; i < g_recN; i++) if (g_rec[i].vk == vk) return g_rec[i].n;
    return 0;
}
static void ResetRec(void) { g_recN = 0; }

/* ---- 仿真工具 ---- */
static void Key(int vk, BOOL down) {
    KBDLLHOOKSTRUCT ks; memset(&ks, 0, sizeof(ks));
    ks.vkCode = (DWORD)vk; ks.flags = 0;
    LowLevelKeyboardProc(HC_ACTION,
        down ? WM_KEYDOWN : WM_KEYUP, (LPARAM)&ks);
}
/* 排干所有待处理消息（含残留 TIMER/TRIG） */
static void Sink(void) {
    MSG m;
    while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE))
        DispatchMessageW(&m);
}
/* 场景间复位：清引擎状态与残留消息，保证场景相互独立 */
static void Reset(void) {
    g_cMod = FALSE; g_dMod = FALSE; g_fMod = FALSE;
    g_pendD = FALSE; g_pendF = FALSE;
    g_actD = FALSE; g_actF = FALSE; g_btnCount = 0;
    PostMessageW(g_mainWnd, WM_CAPS_DOWN, 0, 0);  /* → ResetEngine：清 pending/active + 停表 */
    Sink();
    ResetRec();
}
static void Pump(int ms) {
    ULONGLONG end = GetTickCount64() + ms;
    while (GetTickCount64() < end) {
        Sink();
        Sleep(2);
    }
    Sink();   /* 收尾：把压着的 TIMER 也处理掉 */
}

static int fails = 0;
#define CHECK(cond, name) do { \
    if (cond) printf("  [OK]   %s\n", name); \
    else      { printf("  [FAIL] %s   (captured: ", name); \
               for(int __i=0;__i<g_recN;__i++) printf("%d:%d ",g_rec[__i].vk,g_rec[__i].n);\
               printf(")\n"); fails++; } } while (0)

int main(void) {
    g_state.paused = FALSE;
    SetWindowsHookExW(WH_KEYBOARD_LL, CapProc, GetModuleHandleW(NULL), 0);

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
    Key(CLI_CAPS, FALSE);
    Pump(50);
    CHECK(CountVk(VK_LEFT) == 3, "A: 乱序 C+J+D 点按 = 恰好 3 次左键");

    /* ---- 场景B：F 使 Shift 生效（严格序 C F J） ---- */
    Reset();
    Key(CLI_CAPS, TRUE); Pump(2);
    Key('F', TRUE); Pump(2);     /* 门闩 F */
    Key('J', TRUE);  Pump(2);
    Key('J', FALSE); Pump(2);    /* 短点：Shift 生效 */
    Key('F', FALSE); Pump(2);
    Key(CLI_CAPS, FALSE);
    Pump(50);
    CHECK(CountVk(VK_LEFT) == 1 && CountVk(VK_LSHIFT) >= 1,
          "B: C+F+J = Shift+左键 (1次)");

    /* ---- 场景C：F 乱序（J 先于 F）---- */
    Reset();
    Key(CLI_CAPS, TRUE); Pump(2);
    Key('J', TRUE);  Pump(2);
    Key('F', TRUE);  Pump(2);    /* F 迟到仍未生效于本次短点 */
    Key('J', FALSE); Pump(2);
    Key('F', FALSE); Pump(2);
    Key(CLI_CAPS, FALSE);
    Pump(50);
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
    Key(CLI_CAPS, FALSE);
    Pump(50);
    CHECK(CountVk(VK_LEFT) == 3 && CountVk(VK_LSHIFT) >= 3,
          "D: 乱序 C+D+F+J = Shift+左键 x3");

    /* ---- 场景E：长按 C+J（无 D）连续重复，松手不留残余 ---- */
    Reset();
    Key(CLI_CAPS, TRUE); Pump(2);
    Key('J', TRUE); Pump(300);   /* 稳定期开火 + 首次150ms重复 + 若干 */
    int beforeUp = CountVk(VK_LEFT);
    Key('J', FALSE); Pump(2);    /* 抬手：不应补齐(无D) */
    Key(CLI_CAPS, FALSE);
    Pump(50);
    int afterUp = CountVk(VK_LEFT);
    CHECK(beforeUp >= 2 && afterUp == beforeUp,
          "E: 长按 C+J 重复且松手无多余左键");

    /* ---- 场景F：单点 C+J 无 D = 1 左键（不补齐） ---- */
    Reset();
    Key(CLI_CAPS, TRUE); Pump(2);
    Key('J', TRUE);  Pump(2);
    Key('J', FALSE); Pump(2);    /* 未到稳定期 -> 短点 */
    Key(CLI_CAPS, FALSE);
    Pump(50);
    CHECK(CountVk(VK_LEFT) == 1, "F: 单点 C+J = 1 左键");

    /* ---- 场景G：按住 C 时 F 单独按住（不触发任何 trigger）= 0 注入 ---- */
    Reset();
    Key(CLI_CAPS, TRUE); Pump(2);
    Key('F', TRUE);  Pump(150);
    Key('F', FALSE); Pump(2);
    Key(CLI_CAPS, FALSE);
    Pump(50);
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
    Key(CLI_CAPS, FALSE);
    Pump(50);
    CHECK(CountVk(VK_LEFT) == 1 && CountVk(VK_DOWN) == 1 && CountVk(VK_LSHIFT) >= 2,
          "H: C+F 按住点 J/K 均带 Shift");

    printf(fails ? "\n== %d 项失败 ==\n" : "\n== 全部通过 ==\n", fails);
    return fails;
}