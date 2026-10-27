#ifndef KEYMAP_TABLE_H
#define KEYMAP_TABLE_H

#include "cap.h"   /* ACT_* / ACTF_* / VK_* / DIR_* */

/* ================= 键位表（"某个键是什么意思"的唯一一份定义） =================
   这个文件存在的理由：**"某个键是什么意思"必须只有一份定义。**

   以前它是散开的：hook.c 的 kMap[]（触发键 -> 动作）、hook.c 的 ActToVk()
   switch（动作 -> 目标键）、main.c 的 IsSingleShotAct() switch（动作属性）、
   hook.c 里写死的 `vk == 'D' || vk == VK_LMENU`（增益修饰）、
   hook.c 的 `vk == 'F'`（选区修饰）。
   加一个动作要改三处、漏一处就冒出诡异现象（例如"新增的翻转键长按会切偶数次"）；
   而"把 D 换成别的键"更是要动钩子逻辑本身。

   现在全部收敛成两张声明式表，上面那些地方只剩查询：
     · 动作表   —— 触发键 / 目标键 / 属性标志（能否重复、由谁执行）
     · 修饰表   —— 每个模式的激活键集合（编译期常量，见 cap.h）
   特别注意：**引擎从不判断具体按的是哪个键，只问"这个模式生效了吗"。**
   所以想把增强键从 Alt+D 改成"只留 Alt"、或换成 Caps+右Ctrl，只改
   cap.h 里 MODE_*_KEYS_DEFAULT 一行即可，不必碰任何逻辑（更不读配置：
   这两个键属于"和弦语法"，不是给用户调的旋钮）。

   ★ 为什么这张表是**头文件**而不是留在 keymap.c 里：
   帮助程序（`help/help.c`）是**独立 exe**，它要把这张表画成一张键位图。
   文案若在帮助程序里再抄一份，就是同一件事的两份定义 —— 改键位时忘改一处，
   帮助图就会指着旧键位骗人。所以表放在头里，keymap.c 与帮助程序各自 include
   （各自一份静态副本，没有链接依赖，帮助 exe 因此**不需要链接任何主程序模块**）。

   ★ 本头**必须**在 cap.h 之后包含（要用 VK_* 与 ACTF_*）。 */

typedef struct {
    int      act;    /* 动作 id（ACT_*）                          */
    int      vk;     /* 触发键；0 = 没有触发键（只能由内部投递）    */
    int      outVk;  /* 目标虚拟键；0 = 不发键（见 flags）         */
    int      dir;    /* 按词跳转方向 DIR_*（仅 ACTF_WORDJUMP 有意义）  */
    unsigned flags;
    const char* name; /* 日志可读名（诊断打点用；**不参与任何判定**） */
    const char* label;/* 帮助图上的功能名（UTF-8）；帮助程序用。
                         与 name 分开是因为两者的用法完全不同：日志要能一眼看出
                         "是哪个键触发的"（写作 `←(J)`），图上只要功能本身（`←`），
                         字母已经由键帽自己印着了。NULL = 该动作无图上文案。
                         文案里的 `|` 是**显式断行点**："打开|程序目录" 画成两行。
                         中文没有词间空格，让程序按宽度自动断会把"打开程序目录"
                         切成"打开程 / 序目录"——语义上难看，所以断点由写文案的人
                         决定；不写 `|` 的才交给按宽度自动折行。
                         ★ 文案长没关系：帮助图的字号是**逐键帽实测**的，长文案的键
                         （如这里的 F3）会自动用小一号的字，不拖累其他键。 */
} ActionDef;

/* 顺序不敏感，ActOfVk 线性扫（表很小，且只在按键事件里查一次）。 */
static const ActionDef kActions[] = {
    { ACT_LEFT,      'J',        VK_LEFT,     -1, 0,                  "←(J)", "←" },
    { ACT_DOWN,      'K',        VK_DOWN,     -1, 0,                  "↓(K)", "↓" },
    { ACT_RIGHT,     'L',        VK_RIGHT,    -1, 0,                  "→(L)", "→" },
    { ACT_UP,        'I',        VK_UP,       -1, 0,                  "↑(I)", "↑" },
    { ACT_HOME,      'P',        VK_HOME,     -1, 0,                  "Home(P)", "Home" },
    { ACT_END,       VK_OEM_1,   VK_END,      -1, 0,                  "End(;)", "End" },
    /* 翻页（C+- / C+=）：与 Home/End 同类 —— 普通发键、可长按重复、选区模式
       生效时带 Shift（编辑器里 Shift+PgUp/PgDn = 整页选择）。 */
    { ACT_PGUP,      VK_OEM_MINUS, VK_PRIOR,  -1, 0,                  "PgUp(-)", "PgUp" },
    { ACT_PGDOWN,    VK_OEM_PLUS,  VK_NEXT,   -1, 0,                  "PgDn(=)", "PgDn" },
    { ACT_BACKSPACE, 'R',        VK_BACK,     -1, 0,                  "Bs(R)", "退格" },
    { ACT_DELETE,    'W',        VK_DELETE,   -1, 0,                  "Del(W)", "DEL" },
    { ACT_ENTER,     VK_SPACE,   VK_RETURN,   -1, 0,                  "Enter", "回车" },
    /* 开关键：翻转动作，按一次只切一次 —— 属性写在表里，见 ACTF_SINGLE_SHOT。
       同时 ACTF_NO_SHIFT：它不是光标动作，不该被选区模式套上 Shift。 */
    { ACT_CAPSLOCK,  VK_TAB,     VK_CAPITAL,  -1, ACTF_SINGLE_SHOT | ACTF_NO_SHIFT, "Caps(Tab)", "CapsLock" },
    /* 按词跳转：方向写在表里，免得运行时靠 ACT_WORD_L/R 的数值去反推左右 */
    { ACT_WORD_L,    'N',        0,          DIR_LEFT,  ACTF_WORDJUMP, "词←(N)", "←词" },
    { ACT_WORD_R,    'O',        0,          DIR_RIGHT, ACTF_WORDJUMP, "词→(O)", "词→" },
    /* 挪窗口：单发命令（不发键、与光标/选区无关）。位置由光标决定，所以
       "增强模式发 3 次"没有意义 —— SINGLE_SHOT 同时挡掉重复与松手补齐。 */
    { ACT_MOVEWIN,   'Q',        0,          -1, ACTF_SINGLE_SHOT | ACTF_NO_SHIFT | ACTF_MOVEWIN, "窗口→光标(Q)", "窗口" },
    /* 窗口置顶（T）：翻转类，按一次切一次 —— 与 Caps(Tab) 同理必须是单发。
       逻辑与视觉对齐 PowerToys 的 AlwaysOnTop（见 src/pin.cpp 的文件头）：
       钉住时外面套一圈强调色边框、响一声 Speech On，放开时响 Speech Sleep。 */
    { ACT_PIN,       'T',        0,          -1, ACTF_SINGLE_SHOT | ACTF_NO_SHIFT | ACTF_PIN, "置顶(T)", "置顶" },
    /* 打开所在文件夹：单发命令（不发键）。目标是**按下那一刻**的前台窗口 ——
       窗口由 main.c 的 PostAct 快照进 ActJob.hwnd，这里只声明它是这类动作。 */
    { ACT_OPENFOLDER, VK_F3,     0,          -1, ACTF_SINGLE_SHOT | ACTF_NO_SHIFT | ACTF_OPENFOLDER, "文件夹(F3)", "打开|程序目录" },
    /* 按键帮助：单发命令，同样不发键。它由 main.c 直接在和弦上开合
       （不走执行线程，也不占动作队列 —— 见 help.c）：按下开、抬起关。 */
    { ACT_HELP,      VK_F1,      0,          -1, ACTF_SINGLE_SHOT | ACTF_NO_SHIFT | ACTF_HELP, "帮助(F1)", "帮助" },
    /* 系统音量：走普通"发键"通道（outVk = 多媒体键，系统自己处理，与前台程序无关）。
       静音是**开关**：重复一次就多切一次（来回横跳），必须单发 —— 与 Caps(Tab) 同理。
       音量加减是**步进**：长按重复（与真实音量键的手感一致，重复节奏即引擎的
       REPEAT_*），增强模式点按补齐到 3 步也顺理成章（增强 = ×3 的既有语义）。
       两条都带 ACTF_NO_SHIFT：选区模式的 Shift 对系统音量毫无意义。 */
    { ACT_MUTE,      VK_F6,      VK_VOLUME_MUTE, -1, ACTF_SINGLE_SHOT | ACTF_NO_SHIFT, "静音(F6)", "静音" },
    { ACT_VOLDOWN,   VK_F7,      VK_VOLUME_DOWN, -1, ACTF_NO_SHIFT, "音量-(F7)", "音量-" },
    { ACT_VOLUP,     VK_F8,      VK_VOLUME_UP,   -1, ACTF_NO_SHIFT, "音量+(F8)", "音量+" },
    /* 诊断：没有触发键（由 PROBE_KEY 的特殊路径投递，见 hook.c） */
    { ACT_PROBE,     0,          0,          -1, ACTF_PROBE, "探测", "记录日志" },
};
#define ACT_N ((int)(sizeof(kActions) / sizeof(kActions[0])))

#endif /* KEYMAP_TABLE_H */
