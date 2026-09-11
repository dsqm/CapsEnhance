#include "cap.h"

/* ================= 模拟按键发送 =================
   这一层只关心"把键正确地送出去"，不含任何业务逻辑。三条硬规则都是踩过坑的：

   1) 扩展键必须带 KEYEVENTF_EXTENDEDKEY。方向键/Home/End/Del 等的扫描码
      与小键盘共用，缺了扩展标志，目标程序拿到的是"小键盘 4/6"而不是
      "左/右箭头"。实测（Chromium）：不加标志时 keydown 的 code=Numpad4 /
      Numpad6，依赖 code 派发的程序（VSCode）就会认错键；加上后 code=ArrowLeft。
      反过来 VK_RETURN 绝不能加 —— 主键盘回车不扩展，加了会变成小键盘回车。

   2) 逐条 SendInput + 微小间隔，而不是一次塞一整批。批量发送在部分程序里
      只认第一个键（这也是 SendArrows 一直逐条发的原因）。

   3) 组合键里修饰键必须真的按住：Shift/Ctrl 全程在目标键之前按下、之后抬起。
      之前兜底按词跳转把 Ctrl↓ Ctrl↑ 连续发出，目标收到的方向键 ctrl=0，
      于是退化成"单格方向键"——这正是 caps+N/M 在多个程序里"只挪一格"的原因。
*/

/* 需要 KEYEVENTF_EXTENDEDKEY 的键 */
static BOOL IsExtendedVk(int vk) {
    switch (vk) {
    case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN:
    case VK_HOME: case VK_END:
    case VK_PRIOR: case VK_NEXT:
    case VK_INSERT: case VK_DELETE:
    case VK_SNAPSHOT:
    case VK_LWIN: case VK_RWIN: case VK_APPS:
    case VK_DIVIDE:                 /* 小键盘 / */
        return TRUE;
    default:
        return FALSE;
    }
}

/* 我们注入过"按下"但还没配"抬起"的修饰键：进程在组合键中途退出/重启时
   会留下真正的卡键，退出前无条件补发抬起（与用户是否物理按住无关，
   因为这些键本来就只该由我们按下）。 */
#define MOD_COUNT 8
static const int kMods[MOD_COUNT] = {
    VK_LSHIFT, VK_RSHIFT, VK_LCONTROL, VK_RCONTROL,
    VK_LMENU,  VK_RMENU,  VK_LWIN,     VK_RWIN
};
/* volatile：SendEvent 由 act.c 的**工作线程**调用写入，而 SendClearStuckModifiers
   在退出/重启时由主线程读取。两处之间有 ActStop() 的线程 join 建立
   happens-before，所以理论上不加也算对 —— 但这份正确性完全依赖"调用方记得
   先 ActStop"，写成 volatile 至少让"没同步就跨线程读"这件事在代码审查时
   一眼可见，不需要去推理调用顺序。 */
static volatile BOOL g_injectedDown[MOD_COUNT] = { 0 };

static int ModIndex(int vk) {
    for (int i = 0; i < MOD_COUNT; i++) if (kMods[i] == vk) return i;
    return -1;
}

/* 单条键盘事件（按下 / 抬起） */
static void SendEvent(int vk, BOOL up) {
    int mi = ModIndex(vk);
    if (mi >= 0) g_injectedDown[mi] = !up;
    INPUT i;
    memset(&i, 0, sizeof(i));
    i.type = INPUT_KEYBOARD;
    i.ki.wVk = (WORD)vk;
    i.ki.wScan = (WORD)MapVirtualKeyW((UINT)vk, MAPVK_VK_TO_VSC);
    if (IsExtendedVk(vk)) i.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    if (up)                i.ki.dwFlags |= KEYEVENTF_KEYUP;
    /* 返回值必须看：目标窗口的完整性级别高于本进程时（例如对方以管理员
       运行而本程序没有），UIPI 会把整批注入拒掉、SendInput 返回 0。此前
       丢弃返回值，于是现象是"按词跳转完全没反应"而日志干净无事 —— 无法诊断。
       只在记录模式写日志：常规模式不打扰用户，也不改变任何行为。 */
    if (SendInput(1, &i, sizeof(INPUT)) == 0 && g_recording)
        CapLog("  !! SendInput 被拒 vk=0x%02X up=%d err=%lu",
               (unsigned)vk, up ? 1 : 0, (unsigned long)GetLastError());
}

/* 事件之间的间隔：让目标程序的消息队列逐个消化 */
#define KEY_GAP        5      /* ms：同一组合键内主键的按下/抬起之间 */
#define ARROW_GAP      8      /* ms：连续箭头之间 */
/* 修饰键（Shift/Ctrl）与主键之间的间隔。实测：注入式按键在虚拟机上约
   60ms 才到达目标，间隔太小时目标处理主键时读到的修饰键状态还没更新——
   表现是"Ctrl+方向"退化成单格移动（Quicker/WPF 上抓到的：同一段文本里
   Ctrl+← 时灵时不灵，日志交替出现 -1 与整词位移）。取 40ms（本机注入
   延迟实测约 60ms/键，间隔要留足）：代价是组合键单次多 ~80ms，换来的是
   不再退化成单格移动。 */
#define COMBO_GAP      40     /* ms：修饰键 <-> 主键之间 */

/* 单击一个键；shift 全程按住（Shift↓ 键↓ 键↑ Shift↑） */
void SendKeyOnce(int vk, BOOL shift) {
    if (shift) { SendEvent(VK_LSHIFT, FALSE); Sleep(COMBO_GAP); }
    SendEvent(vk, FALSE);
    Sleep(KEY_GAP);
    SendEvent(vk, TRUE);
    if (shift) { Sleep(COMBO_GAP); SendEvent(VK_LSHIFT, TRUE); }
}

/* 逐格输出的打断机制：
   SendArrows 的循环跑在主线程，期间主线程被占用、无法处理任何消息，
   所以"松开 C"和"改道按键"这些打断意图只能由钩子线程置 volatile 标志，
   循环里每发完一对键检查一次（最坏 ~13ms 内响应）。
   触发条件（hook.c 调用）：
     1) C（CapsLock）松开 —— 映射模式结束，立即停；
     2) 按下与当前输出方向不同的触发键 —— 改道（如正在 ← 时按 L）。
   打断后循环立即退出，循环外的收尾（抬起 Shift）照常执行；
   改道按键的 TRIG_DOWN 已在主线程队列里，循环结束后自然开火衔接。 */
static volatile BOOL g_arrBusy = FALSE;    /* 正在逐格输出 */
static volatile int  g_arrDir  = -1;       /* 当前输出方向 (DIR_*) */
static volatile BOOL g_arrBreak = FALSE;   /* 打断请求 */

void SendArrowsRequestInterrupt(void) { g_arrBreak = TRUE; }

/* act 是否与正在逐格输出的方向冲突（不同触发键即改道） */
BOOL SendArrowsConflicts(int act) {
    if (!g_arrBusy) return FALSE;
    int curAct = (g_arrDir == DIR_LEFT) ? ACT_WORD_L : ACT_WORD_R;
    return act != curAct;
}

/* 逐方向键移动：发送 n 个左/右箭头（可带 Shift 位移）。
   每个箭头单独发送并微小间隔，避免大批量 SendInput 在部分程序中只认第一个键；
   Shift 在整个按词跳转期间持续按住以维持选区。 */
#define MAX_ARROW_STEPS 256
/* 真正的逐格发送：不做任何超限判定，n 步就是 n 步。 */
static void ArrowsEmit(int dir, int n, BOOL shift) {
    int vk = (dir == DIR_LEFT) ? VK_LEFT : VK_RIGHT;
    g_arrDir = dir; g_arrBusy = TRUE; g_arrBreak = FALSE;
    if (shift) { SendEvent(VK_LSHIFT, FALSE); Sleep(COMBO_GAP); }
    for (int i = 0; i < n && i < MAX_ARROW_STEPS; i++) {
        if (g_arrBreak) break;
        SendEvent(vk, FALSE);
        Sleep(KEY_GAP);
        if (g_arrBreak) break;
        SendEvent(vk, TRUE);
        if (g_arrBreak) break;
        Sleep(ARROW_GAP);
    }
    g_arrBusy = FALSE;
    if (shift) SendEvent(VK_LSHIFT, TRUE);   /* 打断时也要抬 Shift，选区停在此处 */
}

/* 补正专用：即使步数达到 MaxArrowSteps 也逐格走完，**不**改道。
   补正的步数是"目标 - 实测落点"算出来的，它不是用户要跳的距离；若再判一次
   超限就又会改道一次，而改道之后没有任何人再回读落点 —— 补正白做、光标又
   落回段中间（这正是 QQ 场景里"按词跳转落到段中间"的根因之一）。 */
void SendArrowsStrict(int dir, int n, BOOL shift) {
    ArrowsEmit(dir, n, shift);
}

int SendArrows(int dir, int n, BOOL shift) {
    /* 发送处统一判定：本次要发的步数达到 MaxArrowSteps 就改为
         OverStepMode=1 -> 发原生 Ctrl+方向（目标程序自己的按词跳转，瞬时）
         OverStepMode=0 -> 只发一个方向键（跳一格即停）
       放在这里而不是调用方，是为了让所有调用路径都受同一约束；
       MaxArrowSteps=0 表示不限制（硬上限仍是 256）。 */
    int maxSteps = ConfigGetMaxArrowSteps();
    if (maxSteps > 0 && n >= maxSteps) {
        int want = n;
        if (ConfigGetOverStepCtrl()) {
            SendNativeWordJump(dir, shift);
            if (g_recording)
                CapLog("  -> 逐格步数 %d 达到 MaxArrowSteps(%d)，改发 Ctrl+arrow",
                       want, maxSteps);
            /* 改道只是"让目标程序自己跳一格词"，落点由**它的**词切分决定，
               跟我们的算法不一定一致（中文尤其明显，见 cap.h 的说明），
               所以必须把"本次是改道"告诉调用方去回读校验。 */
            return ARROW_SEND_HANDOFF;
        }
        if (g_recording)
            CapLog("  -> 逐格步数 %d 达到 MaxArrowSteps(%d)，只发一格",
                   want, maxSteps);
        ArrowsEmit(dir, 1, shift);
        return ARROW_SEND_ONE;   /* 用户明确要"跳一格即停"：不做补正 */
    }
    ArrowsEmit(dir, n, shift);
    return ARROW_SEND_STEPS;
}

/* 逐格注入 n 个"同值刻度"，纵轴与横轴共用这一段 —— 只有 dwFlags 不同
   （MOUSEEVENTF_WHEEL / MOUSEEVENTF_HWHEEL），"发几格"由调用方决定，
   注入语义完全一样，所以只留一处实现，免得"改了间隔忘了改另一条路"。

   **不要**改成"一个事件塞 delta = 120*N"，实测（跨进程量目标窗口收到的消息）：
     · 一次 SendInput 批 3 个 120    -> 目标只收到 1 条、delta=360
     · 3 个事件间隔 2ms 逐个发       -> 目标收到 3 条、每条 delta=120
   系统会把"目标还没派发的"滚轮事件合并成一个 WM_MOUSEWHEEL 并把 delta 相加；
   更糟的是 +120 与 -120 同批会被合成 delta=0，等于什么都没滚。
   为什么在乎这个差别：不少程序只看 delta 的符号、每条消息只滚一格（Blender
   就为此专门把大 delta 拆成多个单格事件），所以"逐格"才是通用写法。

   每格 2ms 的间隔在倍数上限 16 下最坏阻塞 30ms。也正因为有 Sleep，这段**不能**
   跑在主线程上：调用它的通道都在 act.c 的专用线程里（见 act.c 顶部说明）。
   全程只做输入注入：不调 SystemParametersInfo（不改系统的"每次滚动行数"），
   也不写注册表。 */
#define WHEEL_EXTRA_MAX 15
#define WHEEL_GAP_MS    2

static void SendWheelAxis(int delta, int n, BOOL horizontal) {
    if (delta == 0 || n <= 0) return;
    if (n > WHEEL_EXTRA_MAX) n = WHEEL_EXTRA_MAX;
    for (int i = 0; i < n; i++) {
        INPUT in;
        memset(&in, 0, sizeof(in));
        in.type = INPUT_MOUSE;
        in.mi.dwFlags = horizontal ? MOUSEEVENTF_HWHEEL : MOUSEEVENTF_WHEEL;
        in.mi.mouseData = (DWORD)(LONG)delta;   /* 有符号增量：正=上/右，负=下/左 */
        if (SendInput(1, &in, sizeof(INPUT)) == 0) {
            /* UIPI 等场景会被整体拒绝。只在记录模式写日志、且不重试：
               重试只会继续失败并把日志刷爆 */
            if (g_recording)
                CapLog("  !! SendInput 滚轮被拒 %s i=%d delta=%d err=%lu",
                       horizontal ? "横" : "纵", i, delta,
                       (unsigned long)GetLastError());
            break;
        }
        if (i + 1 < n) Sleep(WHEEL_GAP_MS);
    }
}

void SendWheelNotches(int delta, int n)  { SendWheelAxis(delta, n, FALSE); }

/* 纵向放大：倍数 -> 要补发的格数。单独拎出来是为了能被单测直接断言换算
   （真注入的条数没法在本进程里稳定观测，见 test_chord 场景Q 的说明）。
   注意只补 (mul-1) 格 —— 物理那一格由系统照常送达（跟手/放大是"加强"，
   不是"替换"），所以"倍数 2"总共恰好是两格。 */
int SendWheelExtraCount(int mul) {
    if (mul <= 1) return 0;
    int extra = mul - 1;
    return extra > WHEEL_EXTRA_MAX ? WHEEL_EXTRA_MAX : extra;
}

void SendWheelAccel(int delta, int mul) {
    SendWheelNotches(delta, SendWheelExtraCount(mul));
}

/* 横向滚动：n 个**固定** ±120 的刻度。这里不沿用物理格的原值而统一成
   WHEEL_STD_DELTA —— 横向是"每次滚轮换一列"的语义，放大倍数才有确定含义
   （倍数 1 = 正好一格），物理格的原始大小（高精度滚轮可能小于 120）不参与。

   **方向（与滚轮上下相反）**：滚轮**向上** -> 向左，向下 -> 向右。
   这不是我们随便定的，而是"Shift + 滚轮"在浏览器 / 资源管理器 / 表格里的
   通用约定（Firefox 官方快捷键表就写着 Shift+上滚 = 向左滚动）。照抄系统
   约定用户才不用重新适应肌肉记忆。 */
void SendWheelHNotches(int delta, int n) {
    if (delta == 0) return;
    SendWheelAxis((delta > 0) ? -WHEEL_STD_DELTA : WHEEL_STD_DELTA, n, TRUE);
}

/* 兜底：文字读取失败时，用系统原生 Ctrl(+Shift)+方向键做按词跳转。
   Ctrl 必须在方向键之前按下、之后抬起，否则目标收到的只是单格方向键。 */
void SendNativeWordJump(int dir, BOOL shift) {
    int vk = (dir == DIR_LEFT) ? VK_LEFT : VK_RIGHT;
    if (shift) { SendEvent(VK_LSHIFT, FALSE); Sleep(COMBO_GAP); }
    SendEvent(VK_LCONTROL, FALSE);
    Sleep(COMBO_GAP);
    SendEvent(vk, FALSE);
    Sleep(KEY_GAP);
    SendEvent(vk, TRUE);
    Sleep(COMBO_GAP);
    SendEvent(VK_LCONTROL, TRUE);
    if (shift) { Sleep(COMBO_GAP); SendEvent(VK_LSHIFT, TRUE); }
}

/* 释放一个修饰键。Alt / Win 的"抬起"被系统单独看到时会被解释为
   "按下并释放了 Alt/Win" —— 从而激活菜单栏或开始菜单（AHK 为此专门
   引入 #MenuMaskKey：用一个中性键插在中间，让系统认为中间按过别的键）。
   这里用未定义虚拟键 0xFF 作掩码键，行为与 AHK 的 vkFF 一致。 */
#define MASK_VK 0xFF
static void ReleaseMod(int vk) {
    if (vk == VK_LMENU || vk == VK_RMENU || vk == VK_LWIN || vk == VK_RWIN) {
        SendEvent(MASK_VK, FALSE);
        SendEvent(MASK_VK, TRUE);
    }
    SendEvent(vk, TRUE);
}

/* 重启 / 退出时清键盘状态（AHK 同理：区分"逻辑状态"与"物理状态"）。
   GetAsyncKeyState 反映的是 OS 的逻辑状态——被注入的键同样置位，
   所以卡键恰恰表现为"OS 认为它按着"。判据是：
       逻辑按下（OS 认为按着） && 物理未按（用户没真的按） == 卡键
   物理状态只能由低级钩子自己记录（见 hook.c 的 PhysKeyDown）。
   注：此前写成"逻辑未按才补发抬起"，等于把所有真正卡住的键全跳过了，
   这就是"重启程序清不掉卡键"的原因。 */
void SendClearStuckModifiers(void) {
    for (int i = 0; i < MOD_COUNT; i++) {
        int vk = kMods[i];
        BOOL logicalDown = (GetAsyncKeyState(vk) & 0x8000) != 0;
        if (logicalDown && !PhysKeyDown(vk)) ReleaseMod(vk);
    }
    /* 我们注入过的修饰键：无条件补抬起（这些键本不该残留 down 状态） */
    for (int i = 0; i < MOD_COUNT; i++)
        if (g_injectedDown[i]) ReleaseMod(kMods[i]);
    /* CapsLock 是开关键，逻辑状态表示"灯亮"，不是卡键；只在明确没物理按住
       且灯亮时才切一次，避免把用户的大小写状态改乱 —— 保守起见不处理。 */
}

/* 强制重置键盘状态（命令行 --reset-keys）：不看任何状态，对所有修饰键
   各补一次抬起。用于程序已被杀死、状态表丢失的场合（等价 AHK 社区常用的
   "把所有修饰键 Send Up 一遍"）。 */
void SendForceResetKeys(void) {
    for (int i = 0; i < MOD_COUNT; i++) ReleaseMod(kMods[i]);
}
