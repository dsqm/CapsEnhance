/* ============================================================
   CapsEnhance 设置程序（CapsEnhanceSettings.exe）

   一个**表单式**的 CapsEnhance.ini 编辑器：布尔项是勾选框、数值项是输入框、
   五份名单各是一个可增删的列表。列表那一行还有"选取窗口"：点它再点别的窗口，
   取到的东西**先填进左侧输入框**（不直接入名单），确认后点"添加"才加进去 ——
   这样能先看清取到了什么、也能顺手改一改。
   取什么由名单决定：进程名单取进程名，窗口类名单取**类名**（原样，无前缀）。
   保存 = 写回 ini（「保存」）+ 让本体重启（「保存并重启」，走
   `CapsEnhance.exe --restart`，本体重读配置才算生效）。
   ★ 只有这两颗按钮会写 ini。「退出」（以及标题栏的 ✕）**丢弃**改动：有未保存
   改动时弹一次"退出将丢弃？"的确认，没改动就直接关。

   "有没有未保存改动"的判据是**与基线比对**，不是"动过就脏"（EDIT 收到
   WM_SETTEXT 是**无条件**发 EN_CHANGE 的，把同样的值写回去也会"动一下"）：
   加载后与保存后各拍一次基线快照（字段快照 + 五份名单拍平的文本），任何控件
   通知来了就重比一遍 —— 真的不同才脏，改回原样 `*` 自己就消失。

   为什么不是"直接编辑文本"：配置项的值域（LogMaxKB 0..65536、
   AutoScrollInterval 100..60000 …）与布尔语义（只认 0/1）在**配置文件里
   只是注释**，把原文摊给用户编辑等于让每一行都可能写出一个"解析得出来但
   不合理"的值。表单把这些约束变成控件本身（勾选框 / 数字框），写回的
   永远是钳制过的值。

   为什么是独立 exe 而不是给本体加个窗口：本体的主线程是 WH_KEYBOARD_LL
   的宿主 —— 钩子回调跑在装它的线程上（见 src/act.c 文件头）。设置窗口要跑
   消息循环、持有控件、还要写文件，任何一处阻塞都会直接表现为"键盘卡住 /
   钩子被系统摘掉"。这是两类进程，不该共用一个主线程。

   值域与键名**不在这里重复定义**：读用 CfgInt / CfgBool / CfgStr，写用
   CfgSetInt / CfgSetBool / CfgSetStr（钳制与解析同源，值域也来自 config.c
   的 kCfg 表），写回走 ConfigSaveFile —— 它按行做增量更新，用户的注释、
   未知段、BOM 与行尾风格都保留。

   高 DPI：进程声明 Per-Monitor V2（拿不到就退回系统级 DPI 感知），字体按
   当前 DPI 取系统消息字体，所有像素尺寸经 S() 缩放；拖到不同缩放的显示器
   上时按 WM_DPICHANGED 重建字体并重排（见 MakeFont / Layout）。

   滚动：一页的内容比可视区高时出竖向滚动条，滚轮和滚动条都能滚；底部按钮与
   状态栏是"窗框"，不参与滚动。实现上全部表单控件住在一个**承载窗**里，滚动 =
   每帧只挪这一个窗口（**不再逐控件重排**），滚轮按系统"每次滚 N 行"折算步长、
   帧由高精度计时器按刷新周期的一半出（240Hz → 2ms 一帧），按时间做 ease-out
   缓动 —— 详见下文"滚动：只挪一个承载窗"一节的取舍与踩坑史。
   滚轮若停在**某个列表框上**，滚的是那个列表框而不是整个表单
   （见 WndProc 的 WM_MOUSEWHEEL / ScrollListBy）。

   页签：表单分四页 —— 「常规」（数值 / 开关这些配置项）、「超级复制」
   （超级复制预览浮层的**全部**参数：停留时长 / 位置 / 宽度 / 透明度 / 圆角 /
   字体 / 显示量）、「窗口信息」（C+F12 弹窗的字体 / 字号，以及「常用」视图的
   有序字段编排）、「名单」（五份按程序 / 窗口生效的名单：按词跳转 / 自动屏蔽 /
   整格滚动 / 拖动的两份黑名单）。后两页都是"独立一件事"才拆出来的：浮层参数
   有十几行、窗口信息那组带一个可增删排序的字段列表，混在常规页里会把
   "按词跳转 / 滚动"那几段挤到很下面。
   分页是结构性的：配置项按 group 归页（见 FieldPage），名单全在名单页，
   页签只决定"显示哪些控件" —— 控件都在承载窗里，Load / Save 与
   WM_COMMAND 照旧按 id 找、不分页（见 kTabNames / g_curTab / Layout）。
   页签**外观自绘**（PaintTab：comctl 默认的浮凸小块太旧，改成"文本 +
   强调色下划线"的现代样式，行为不动）；常规页的配置项按 group 列**分段**
   渲染段头（同族 + 真 Bold，见 kFields 的 group 列 / InitGroups）。「窗口信息」页末尾是
   那块「常用字段」编排：弹窗「常用」视图显示哪些字段、按什么顺序，就在这里
   增删排序（见"窗口信息「常用」视图的字段编排"一节）。
   ============================================================ */
#define WIN32_LEAN_AND_MEAN
#include "cap.h"
#include "settings_int.h"
#include "info_shared.h"   /* 字号默认值 / 值域：与 config.c、弹窗同一份 */
#include "info_fields.h"   /* 窗口信息弹窗的字段注册表：与弹窗同一份词汇表 */

#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <shellapi.h>      /* ShellExecuteW（CreateProcess 失败时的兜底） */
#include <commctrl.h>      /* 页签控件（WC_TABCONTROL / TCM_* / TCN_SELCHANGE） */
#include <commdlg.h>      /* ChooseFontW（窗口信息弹窗的字体对话框）*/
                          /* WIN32_LEAN_AND_MEAN 把 commdlg.h 从 windows.h 里剔掉了，
                             所以这一行不是多余的 */

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")   /* ChooseFontW（窗口信息弹窗的字体） */

#define APP_TITLE   L"CapsEnhance 设置"
#define WND_CLASS   L"CapsEnhanceSettingsMain"
#define FOOTER_CLASS L"CapsEnhanceSettingsFooter"
#define FORM_CLASS  L"CapsEnhanceSettingsForm"   /* 承载窗（表单内容容器，见"滚动"一节） */
#define WM_SCROLL_TICK (WM_APP + 1)  /* 滚动动画帧（帧线程 -> UI 线程，见 AnimThreadProc） */
#define INI_NAME    L"CapsEnhance.ini"
#define MAIN_EXE    L"CapsEnhance.exe"

/* 控件 ID 全在 settings_int.h（两个编译单元共用，见那里的说明）。 */

/* ---- 页签 ----
   四页：常规 = 通用/按键类配置项（kFields 里除"超级复制""窗口信息"之外的段），
   超级复制 = 预览浮层那一批参数，窗口信息 = C+F12 弹窗的字体/字号 +「常用」
   字段编排，名单 = 全部名单（kLists）。
   分页是**结构性的**（按程序 / 窗口生效的东西全在名单页，浮层参数单独一页，
   窗口信息那组又带一个"有序字段列表"这种非标量控件，见文件头"页签"段），
   所以表里不需要显式的页签列 —— 归页由 FieldPage 按 group 现算。
   页签只决定显隐：控件本体不分家，Load / Save / WM_COMMAND 照旧按 id 找。 */
enum { TAB_GENERAL = 0, TAB_COPY = 1, TAB_INFO = 2, TAB_LISTS = 3, TAB_N = 4 };
static const WCHAR* const kTabNames[TAB_N] = { L"常规", L"超级复制", L"窗口信息", L"名单" };

/* 配置项归哪一页：按 group 现算（实现在 kFields 之后，因为要遍历它）。
   按 group 算而不是给 Field 加一列 —— 同一段的行必须连排，所以"段 -> 页"
   是天然的一对多；再加一列就多一处可能与 group 不一致的状态。 */
#define COPY_GROUP_NAME L"超级复制"
#define INFO_GROUP_NAME L"窗口信息"

/* ---------- 字段表：控件的"是什么"，不是"长什么样" ----------
   加一个配置项 = 这里加一行（另两处是 config.c 的 kCfg 表与 README 的配置表）。
   `cfg` 列指向 config.c 的配置项编号：Load / Save 按它直取直写，**没有分支**；
   值域也从那边取（InitFieldRanges），这里不写第二份。
   标签只写中文说明 —— ini 里的键名属于配置文件的内部拼法，界面不重复；
   数值单位（KB / 毫秒 …）单独一列，显示在输入框右侧。
   group 一列是**归属段**（例如「通用」「滚动」「输入法」）：
   每段第一行渲染一个段头（同族 + 真 Bold），同一段的行必须连排（段头按"换段"渲染）。
   ★ 这个 struct 本身住在 settings_int.h。 */

static Field kFields[] = {
    { IDC_RUN_ADMIN,  CFG_RUN_AS_ADMIN,        L"以管理员身份运行（会弹 UAC）",                   NULL,      1, 0, 0, 0, 0,      L"通用" },
    { IDC_LOG_KB,     CFG_LOG_MAX_KB,          L"日志大小上限（0 = 不限制）",                    L"KB",     0, 0, 0, 0, 0,      L"通用" },
    { IDC_HYPER,      CFG_HYPER_PASSTHROUGH,   L"Hyper 透传：按下未映射键时临时按住 RWin+RCtrl+RShift 送出（AHK 绑 Hyper 用）", NULL, 1, 0, 0, 0, 0, L"通用" },
    { IDC_STEPS,      CFG_MAX_ARROW_STEPS,     L"按词跳转逐格上限（0 = 不限制，上限 256）",       L"格",     0, 0, 0, 0, 0,      L"按词跳转" },
    { IDC_OVER_CTRL,  CFG_OVER_STEP_CTRL,      L"超过逐格上限时改发 Ctrl+方向（取消勾选只发一次方向键）", NULL, 1, 0, 0, 0, 0, L"按词跳转" },
    { IDC_SCI,        CFG_ALLOW_SCINTILLA_READ, L"允许读取 Scintilla 程序内存（关掉则退回逐格）", NULL,    1, 0, 0, 0, 0,      L"按词跳转" },
    { IDC_WMUL,       CFG_WHEEL_SCROLL_MUL,    L"Caps+滚轮 放大倍数（0/1 = 关闭）",              L"倍",     0, 0, 0, 0, 0,      L"滚动" },
    { IDC_INTERVAL,   CFG_AUTOSCROLL_INTERVAL, L"持续滚动初始速度（越大越慢）",                  L"毫秒/格", 0, 0, 0, 0, 0,      L"滚动" },
    { IDC_FASTMS,     CFG_AUTOSCROLL_FAST_MS,  L"持续滚动最快速度（越小越快）",                  L"毫秒/格", 0, 0, 0, 0, 0,      L"滚动" },
    { IDC_HMUL,       CFG_WHEEL_HSCROLL_MUL,   L"Caps+Shift+滚轮 横向倍数（0 = 关闭）",          L"倍",     0, 0, 0, 0, 0,      L"滚动" },
    /* 单键切换输入法：0=不处理 1=切中文 2=切英文（值域与语义只在 config.c 定义） */
    { IDC_IME_CAPS,   CFG_IME_CAPS_MODE,       L"单击 Caps 时切输入法（0=不处理 1=中文 2=英文）",  NULL,    0, 0, 0, 0, 0,      L"输入法" },
    { IDC_IME_LSHF,   CFG_IME_LSHIFT_MODE,     L"单击左 Shift 时切输入法（同上；英文键盘下都不处理）", NULL, 0, 0, 0, 0, 0,   L"输入法" },
    { IDC_IME_RSHF,   CFG_IME_RSHIFT_MODE,     L"单击右 Shift 时切输入法（同上；英文键盘下都不处理）", NULL, 0, 0, 0, 0, 0,   L"输入法" },
    /* 窗口信息弹窗（C+F12）：字体名是文本项，所以"标签独占一行、输入框占满下一行"
       （见 Layout 的 isText 分支）—— 字体名要看得全，也要能手打。
       紧跟着的「选择字体…」把系统对话框的结果**填进这个框**，不直接写配置：
       与"选窗"那条同一套礼貌，用户还能改，点保存才落盘。 */
    { IDC_INFO_FONT,   CFG_INFO_FONT,          L"字体（留空 = 依次回落 Cascadia Mono / Consolas / 微软雅黑）", NULL, 0, 1, 0, 0, 0, L"窗口信息" },
    { IDC_INFO_FSIZE,  CFG_INFO_FONT_SIZE,     L"字号",                                            L"磅",     0, 0, 0, 0, 0,      L"窗口信息" },
    { IDC_INFO_FONTBTN, -1,                    L"选择字体…",                                       NULL,     0, 0, 1, 0, 0,      L"窗口信息" },
    /* 超级复制（Caps+C）的预览浮层。顺序即界面顺序：先"什么时候消失"，
       再"摆在哪、长什么样"，最后"显示多少"。
       两处文案要点：① 停留时长分成功/失败两个（失败那句原因要读完才决定
       下一步，默认更长）；② 行数/字数**只影响浮层显示**，剪贴板永远是全文 ——
       不写清楚的话，用户会以为复制被截断了。 */
    { IDC_COPY_STAY,      CFG_COPY_STAY_MS,       L"复制成功提示停留（0 无效，最小 500）",             L"毫秒",   0, 0, 0, 0, 0,      L"超级复制" },
    { IDC_COPY_FAILSTAY,  CFG_COPY_FAIL_STAY_MS,  L"复制失败提示停留（原因要读完再决定怎么办，通常更长）", L"毫秒", 0, 0, 0, 0, 0,      L"超级复制" },
    { IDC_COPY_POS,       CFG_COPY_POS_PERCENT,   L"浮层纵向位置（工作区高度百分比，水平恒为居中）",    L"%",      0, 0, 0, 0, 0,      L"超级复制" },
    { IDC_COPY_WMAX,      CFG_COPY_WIDTH_MAX,     L"浮层宽度上限（96 DPI 逻辑像素，窄内容会自动收窄）", L"px",     0, 0, 0, 0, 0,      L"超级复制" },
    { IDC_COPY_ALPHA,     CFG_COPY_ALPHA,         L"浮层不透明度（60..255，242 ≈ 95%）",                NULL,      0, 0, 0, 0, 0,      L"超级复制" },
    { IDC_COPY_RADIUS,    CFG_COPY_RADIUS,        L"浮层圆角半径（0 = 直角）",                         L"px",     0, 0, 0, 0, 0,      L"超级复制" },
    { IDC_COPY_FONT,      CFG_COPY_FONT,          L"字体（留空 = Microsoft YaHei UI；标题 9 磅 / 正文 10 磅固定）", NULL, 0, 1, 0, 0, 0, L"超级复制" },
    { IDC_COPY_FONTBTN,   -1,                     L"选择字体…",                                       NULL,      0, 0, 1, 0, 0,      L"超级复制" },
    { IDC_COPY_MAXLINES,  CFG_COPY_MAX_LINES,     L"预览最多显示行数（超出用 … 截断，剪贴板仍是全文）",  L"行",     0, 0, 0, 0, 0,      L"超级复制" },
    { IDC_COPY_MAXCHARS,  CFG_COPY_MAX_CHARS,     L"预览最多显示字数（同上）",                         L"字",     0, 0, 0, 0, 0,      L"超级复制" },
    { IDC_COPY_SHOWCOUNT, CFG_COPY_SHOW_COUNT,    L"标题行右侧显示「字数 · 行数」（单行文本只显示字数）",  NULL,      1, 0, 0, 0, 0,      L"超级复制" },
    { IDC_COPY_STRIPTAIL, CFG_COPY_STRIP_TAIL,    L"复制时去掉尾部空白与换行（内部换行不受影响）",      NULL,      1, 0, 0, 0, 0,      L"超级复制" },
};
#define FIELD_N ((int)(sizeof(kFields) / sizeof(kFields[0])))

/* 配置项归哪一页（见 kTabNames 处的说明）。 */
static int FieldPage(int i) {
    const WCHAR* g = kFields[i].group;
    if (g) {
        if (wcscmp(g, COPY_GROUP_NAME) == 0) return TAB_COPY;
        if (wcscmp(g, INFO_GROUP_NAME) == 0) return TAB_INFO;
    }
    return TAB_GENERAL;
}

/* 值域从 config.c 的表灌进来（界面不再写第二份 min/max）。
   表是运行期可变的，所以在这里填一次；建控件之前调用。 */
static void InitFieldRanges(void) {
    for (int i = 0; i < FIELD_N; i++) {
        if (kFields[i].cfg < 0) continue;
        kFields[i].min = CfgLo(kFields[i].cfg);
        kFields[i].max = CfgHi(kFields[i].cfg);
    }
}


/* ---------- 段头映射 ----------
   group 列按"换段"渲染段头：这里把每行映射到它的段头控件 id（0 = 不带头，
   沿用上一段的头）。段头 STATIC 在建控件时按这张表创建（WinMain），字体走
   FontApplyCb 的半粗分支，落位与显隐在 Layout。 */
static int g_grpHead[FIELD_N];

static void InitGroups(void) {
    const WCHAR* cur = NULL;
    int n = 0;
    for (int i = 0; i < FIELD_N; i++) {
        const WCHAR* g = kFields[i].group;
        if (g && (!cur || wcscmp(g, cur) != 0)) {
            cur = g;
            g_grpHead[i] = (n < GROUP_MAX) ? IDC_GROUP0 + n : 0;
            n++;
        } else {
            g_grpHead[i] = 0;
        }
    }
}

/* 选窗时该取什么：进程名单取进程名（`xxx.exe`）；**窗口类名单要取类名**
   —— 把进程名填进"窗口类"名单里永远匹配不上。取到的类名**原样**填进输入框
   （不带任何前缀：本项目的窗口名单只按类名判定，没有"标题"这一维）。 */
#define PICK_PROC  0
#define PICK_CLASS 1

typedef struct {
    const WCHAR* label;
    int          list;      /* CFG_LIST_* */
    int          pickKind;  /* PICK_PROC / PICK_CLASS */
} ListGroup;
static const ListGroup kLists[] = {
    { L"原生按词跳转 [NativeWordJump]：Caps+N/O 不再探测文本，直接发 Ctrl+←/→",
      CFG_LIST_NATIVE,     PICK_PROC },
    { L"自动屏蔽 [AutoDisable]：这些程序在前台时，本程序完全不介入（CapsLock 恢复正常）",
      CFG_LIST_AUTODISABLE, PICK_PROC },
    { L"持续滚动只发整格 [NoSmoothScroll]：不累积小数刻度（滚了不动）的程序",
      CFG_LIST_NOSMOOTH,   PICK_PROC },
    { L"拖动窗口黑名单 [DragWindowBlacklist]：这些窗口类不许 Caps+左键拖动",
      CFG_LIST_DRAGWIN,    PICK_CLASS },
    { L"拖动进程黑名单 [DragProcessBlacklist]：这些进程的窗口一律不许拖动",
      CFG_LIST_DRAGPROC,   PICK_PROC },
};
#define LIST_N ((int)(sizeof(kLists) / sizeof(kLists[0])))

/* 下面几个与 settings_int.h 的声明对应。能不共享的都继续 static —-
   这张清单越短越好。 */
HWND   g_hwnd    = NULL;
static HWND   g_header  = NULL;         /* 顶部"窗框"：页签条的实底托盘（挡住滚上来的内容） */
static HWND   g_footer  = NULL;         /* 底部"窗框"：状态栏 + 按钮的实底托盘 */
static HWND   g_status  = NULL;
static WCHAR  g_dir[MAX_PATH]  = L"";    /* exe 所在目录（结尾带 '\'） */
static WCHAR  g_path[MAX_PATH] = L"";    /* CapsEnhance.ini 完整路径 */
static BOOL   g_dirty   = FALSE;
static BOOL   g_loading = FALSE;         /* 装载期屏蔽 EN_CHANGE，否则一打开就"未保存" */
int    g_dpi     = 96;                  /* 当前窗口 DPI（96 = 100%） */
HFONT  g_font    = NULL;
HFONT  g_fontHead = NULL;               /* 段头 / 选中页签：同族 + FW_BOLD（真 Bold 字面，见 MakeFont） */
static HWND   g_form    = NULL;          /* 承载窗：**全部表单控件**的父窗口（滚动只挪它） */
static int    g_viewH    = 0;            /* 内容可视区高度（底部按钮/状态栏不算） */
static int    g_viewTop  = 0;            /* 内容可视区上沿（页签条窗框的下沿） */
static int    g_contentH = 0;            /* 内容总高；比可视区高就出滚动条 */
static int    g_off      = 0;            /* 当前滚动偏移（像素，0 = 内容顶与可视区顶对齐） */
static int    g_target   = 0;            /* 缓动目标偏移（滚轮只改它，每帧追一点） */
static int    g_wheelRem = 0;            /* 滚轮小数刻度累计（高精度滚轮单条 delta < 120） */
static volatile LONG g_anim = FALSE;     /* 缓动是否在跑（帧线程每帧读它） */
static HANDLE s_animGo   = NULL;         /* 自动复位事件：唤醒帧线程 */
static HANDLE s_animThread = NULL;       /* 帧线程：高精度计时器按刷新周期的一半出帧 */
static volatile LONG s_animQuit = FALSE; /* 退出标记（WM_DESTROY 置位） */
static volatile LONG s_tickPending = 0;  /* 已投递、还没被处理的帧（UI 线程跟不上时合并） */
static LARGE_INTEGER s_qpcFreq;          /* 动画时钟（QPC 计数/秒），见 AnimTick */
static LONGLONG g_animT0 = 0;            /* 本轮动画的起点（QPC 计数） */
static int    g_animFrom = 0;            /* 本轮动画的起点偏移 */
static int    g_animDur  = 120;          /* 本轮动画时长（毫秒，按距离取 90..200） */
static int    g_tickMs   = 0;            /* 帧间隔（刷新周期的一半，见 AnimKick；0 = 还没算） */
static HWND   g_tab     = NULL;          /* 页签条（comctl32 页签控件；建失败则退回单页滚动） */
static int    g_curTab  = TAB_GENERAL;   /* 当前页：换页 = 改它 + Layout（显隐在 Layout 里） */
static int    g_tabPadDpi = 0;           /* 页签内边距是按哪个 DPI 设的（变了就重设） */

/* 像素按 DPI 缩放：所有尺寸常量都必须过一遍它，否则高分屏上控件挤成一团
   （96 DPI 下量出来的坐标在 200% 屏上只有一半大）。 */
static int S(int v) { return MulDiv(v, g_dpi, 96); }

/* 表单控件的统一取柄：它们全都住在**承载窗**里（见"滚动"一节），不是主窗口的
   直接子窗口。页签 / 状态栏 / 底部四颗按钮不在内 —— 它们住在 header/footer 窗框，
   各自用 GetDlgItem(g_header|g_footer, …) 取。 */
static HWND Ctl(int id) { return g_form ? GetDlgItem(g_form, id) : NULL; }

/* ---------------- 小工具 ---------------- */

void SetStatus(const WCHAR* fmt, ...) {
    WCHAR buf[512];
    va_list ap; va_start(ap, fmt);
    _vsnwprintf_s(buf, 512, _TRUNCATE, fmt, ap);
    va_end(ap);
    if (g_status) SetWindowTextW(g_status, buf);
}

static void UpdateTitle(void) {
    WCHAR t[256];
    _snwprintf_s(t, 256, _TRUNCATE, L"%s%s", APP_TITLE, g_dirty ? L" *" : L"");
    SetWindowTextW(g_hwnd, t);
}

/* （"有没有未保存改动"的判定在下面那一节：UpdateDirty，按基线比对） */

/* 输入校验：**只认十进制数字**（与配置解析器同一套严格度）。
   不合法的直接拦下并说清范围 —— 静默写个 0 进去才是对用户最坏的结果。 */
BOOL ReadIntField(HWND edit, const Field* f, int* out) {
    WCHAR buf[64] = L"";
    GetWindowTextW(edit, buf, 64);
    const WCHAR* p = buf;
    while (*p == L' ' || *p == L'\t') p++;
    if (!*p) { *out = f->min; return FALSE; }
    long long v = 0;
    for (const WCHAR* q = p; *q; q++) {
        if (*q < L'0' || *q > L'9') { *out = f->min; return FALSE; }
        v = v * 10 + (*q - L'0');
        if (v > 100000000LL) { *out = f->min; return FALSE; }
    }
    *out = (int)v;
    return TRUE;
}

void SetIntField(HWND edit, int v) {
    WCHAR buf[32];
    _snwprintf_s(buf, 32, _TRUNCATE, L"%d", v);
    SetWindowTextW(edit, buf);
}

static void SetCheck(HWND btn, BOOL on) {
    SendMessageW(btn, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
}
static BOOL GetCheck(HWND btn) {
    return SendMessageW(btn, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

/* ---------------- 窗口信息「常用」视图的字段编排 ----------------
   `C+F12` 弹窗有两个视图：「常用」按这里排好的**有序字段列表**平铺显示；
   「全部」是全量分组信息（两个视图都可编辑）。这块只编排序（谁在里面、
   什么顺序），不承载任何值 —— 结果直接写回 `[General] InfoCommonFields`
   （逗号分隔的字段标识，顺序即显示顺序）。

   字段词汇表在 src/info_fields.h：ini 里用的标识 / 所属层 / 「全部」视图里的
   键名 / 「常用」视图里的友好名，四者同源于一张表（弹窗与设置不各写一份）。

   为什么是"列表 + 四颗按钮"而不是勾选清单：顺序本身就是需求的一部分，勾选框
   只能表达"在不在"，排序还得再来一个界面。按钮贴着列表竖排，一眼看出动的是
   这个列表。

   ★ 最少留一项：空列表在配置层是"用默认那 11 项"的哨兵（与弹窗的判据同源），
   删空再保存等于把用户的选择悄悄换回默认值，所以最后一颗不许删。 */
#define CMN_KEY_MAX 16                                  /* 一个字段标识的缓冲（最长 exstylesym） */
#define CMN_TEXT_MAX INFO_COMMON_CHARS                  /* 整串上限与 config.c / 弹窗同源 */

static WCHAR g_baseCommon[CMN_TEXT_MAX];                /* 已保存基线（拼接文本，与落盘同口径） */

static HWND CmnList(void) { return Ctl(IDC_CMN_LIST); }

/* 第 i 项的字段编号（存在 LB_SETITEMDATA 里；无效返回 -1） */
static int CmnIdAt(int i) {
    if (i < 0) return -1;
    LRESULT d = SendMessageW(CmnList(), LB_GETITEMDATA, (WPARAM)i, 0);
    if (d == LB_ERR) return -1;
    int id = (int)d;
    return (id >= 0 && id < IF_COUNT) ? id : -1;
}

static int CmnIndexOf(int id) {
    int n = (int)SendMessageW(CmnList(), LB_GETCOUNT, 0, 0);
    for (int i = 0; i < n; i++) if (CmnIdAt(i) == id) return i;
    return -1;
}

static void CmnInsert(int at, int id) {
    HWND lb = CmnList();
    SendMessageW(lb, LB_INSERTSTRING, (WPARAM)at, (LPARAM)kInfoFields[id].name);
    SendMessageW(lb, LB_SETITEMDATA, (WPARAM)at, (LPARAM)id);
}

/* 字段标识 -> 宽串（标识是纯 ASCII 的编译期常量） */
static void CmnKeyW(int id, WCHAR* out) {
    out[0] = 0;
    if (id < 0 || id >= IF_COUNT) return;
    MultiByteToWideChar(CP_ACP, 0, kInfoFields[id].key, -1, out, CMN_KEY_MAX);
}

/* 当前列表 -> "key,key,..."。**用标识而不是友好名**：标识才是写进 ini 的东西，
   比对基线与落盘必须同一口径，否则改名/翻译会让界面"看起来没改却是脏的"。 */
static void CommonSnapshot(WCHAR* out, size_t cap) {
    out[0] = 0;
    int n = (int)SendMessageW(CmnList(), LB_GETCOUNT, 0, 0);
    BOOL first = TRUE;
    for (int i = 0; i < n; i++) {
        int id = CmnIdAt(i);
        if (id < 0) continue;
        WCHAR key[CMN_KEY_MAX];
        CmnKeyW(id, key);
        if (!key[0]) continue;
        if (!first) wcscat_s(out, cap, L",");
        wcscat_s(out, cap, key);
        first = FALSE;
    }
}

/* 把逗号串里的已知标识追加进列表（未知的跳过、重复的跳过），返回加了几项。 */
static int CmnAddKeys(const char* src) {
    int added = 0, kn = 0;
    char key[CMN_KEY_MAX];
    for (const char* p = src; ; p++) {
        if (*p == ',' || *p == 0) {
            key[kn] = 0;
            if (kn) {
                int id = InfoFieldByKey(key);
                if (id >= 0 && CmnIndexOf(id) < 0) {
                    CmnInsert((int)SendMessageW(CmnList(), LB_GETCOUNT, 0, 0), id);
                    added++;
                }
            }
            kn = 0;
            if (*p == 0) break;
        } else if (kn < CMN_KEY_MAX - 1) {
            key[kn++] = *p;
        }
    }
    return added;
}

/* 装载：配置里的逗号串 -> 列表。空串（或一个都不认识）= 默认那 11 项，
   与弹窗的 ParseCommon 同一判据 —— 两处判据不一致就会出现"设置里显示 X、
   弹窗里显示 Y"这种查半天的怪事。 */
static void CmnFill(void) {
    SendMessageW(CmnList(), LB_RESETCONTENT, 0, 0);
    const WCHAR* s = CfgStr(CFG_INFO_COMMON);
    char narrow[CMN_TEXT_MAX] = "";
    if (s && s[0]) WideCharToMultiByte(CP_ACP, 0, s, -1, narrow, CMN_TEXT_MAX, NULL, NULL);
    if (!narrow[0] || !CmnAddKeys(narrow))
        CmnAddKeys(INFO_COMMON_DEFAULT);
}

/* 保存：列表 -> 逗号串。列表为空就写空串（配置层解释成默认那 11 项）——
   与"最少留一项"的约束不冲突：真走到这里只可能是外部把控件内容清空了。 */
static void CmnSave(void) {
    WCHAR out[CMN_TEXT_MAX] = L"";
    CommonSnapshot(out, CMN_TEXT_MAX);
    CfgSetStr(CFG_INFO_COMMON, out);
}

/* 五颗按钮的可用性（「重置默认」始终可用）：没选中就不能删/移；首项不能上移、末项不能下移；
   只剩一项不许删；全加进来了就没有可添加的。 */
static void CmnSyncButtons(void) {
    int n   = (int)SendMessageW(CmnList(), LB_GETCOUNT, 0, 0);
    int sel = (int)SendMessageW(CmnList(), LB_GETCURSEL, 0, 0);
    BOOL has = (sel >= 0 && sel != LB_ERR);
    EnableWindow(Ctl(IDC_CMN_DEL),  has && n > 1);
    EnableWindow(Ctl(IDC_CMN_UP),   has && sel > 0);
    EnableWindow(Ctl(IDC_CMN_DOWN), has && sel < n - 1);
    EnableWindow(Ctl(IDC_CMN_ADD),  n < IF_COUNT);
}

/* 「添加…」：弹出"还没有的字段"菜单（按窗口层 / 进程层分两段），选中即追加到
   末尾并选中它 —— 追加比"插到选中项之后"可预期，位置用上移/下移调。 */
static void CmnAddDlg(void) {
    HMENU m = CreatePopupMenu();
    if (!m) return;
    UINT cnt = 0;
    int  lastSec = -1;
    for (int i = 0; i < IF_COUNT; i++) {
        if (CmnIndexOf(i) >= 0) continue;
        if (kInfoFields[i].section != lastSec) {
            if (cnt) AppendMenuW(m, MF_SEPARATOR, 0, NULL);
            lastSec = kInfoFields[i].section;
        }
        /* 命令号 = 字段编号 + 1（0 保留给"没选"） */
        AppendMenuW(m, MF_STRING, (UINT_PTR)(i + 1), kInfoFields[i].name);
        cnt++;
    }
    if (!cnt) {
        DestroyMenu(m);
        SetStatus(L"全部字段都已经在常用里了");
        return;
    }
    HWND btn = Ctl(IDC_CMN_ADD);
    RECT r;
    if (!btn || !GetWindowRect(btn, &r)) { DestroyMenu(m); return; }
    int pick = (int)TrackPopupMenu(m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN,
                                   r.left, r.bottom, 0, g_hwnd, NULL);
    DestroyMenu(m);
    if (pick <= 0) return;

    int id = pick - 1;
    if (id < 0 || id >= IF_COUNT) return;
    int at = (int)SendMessageW(CmnList(), LB_GETCOUNT, 0, 0);
    CmnInsert(at, id);
    SendMessageW(CmnList(), LB_SETCURSEL, (WPARAM)at, 0);
    CmnSyncButtons();
    UpdateDirty();
    SetStatus(L"已把「%s」加到最后 —— 用「上移」调整它的位置", kInfoFields[id].name);
}

static void CmnDel(void) {
    HWND lb = CmnList();
    int sel = (int)SendMessageW(lb, LB_GETCURSEL, 0, 0);
    int n   = (int)SendMessageW(lb, LB_GETCOUNT, 0, 0);
    if (sel < 0 || sel == LB_ERR) return;
    if (n <= 1) {
        SetStatus(L"常用至少要留一项（空列表等于用默认的 11 项）");
        return;
    }
    SendMessageW(lb, LB_DELETESTRING, (WPARAM)sel, 0);
    n--;
    SendMessageW(lb, LB_SETCURSEL, (WPARAM)(sel < n ? sel : n - 1), 0);
    CmnSyncButtons();
    UpdateDirty();
}

/* 上移 / 下移一格（delta = -1 / +1）。删了再插回目标位置 —— 比交换文本简单，
   也不会在"项数据与文本"之间出现对不齐。移动后选中跟着那一项走。 */
static void CmnMove(int delta) {
    HWND lb = CmnList();
    int sel = (int)SendMessageW(lb, LB_GETCURSEL, 0, 0);
    int n   = (int)SendMessageW(lb, LB_GETCOUNT, 0, 0);
    if (sel < 0 || sel == LB_ERR) return;
    int to = sel + delta;
    if (to < 0 || to >= n) return;
    int id = CmnIdAt(sel);
    if (id < 0) return;
    SendMessageW(lb, LB_DELETESTRING, (WPARAM)sel, 0);
    CmnInsert(to, id);
    SendMessageW(lb, LB_SETCURSEL, (WPARAM)to, 0);
    CmnSyncButtons();
    UpdateDirty();
}

/* 「重置默认」：把列表恢复成内置那 11 项、顺序也回到默认。先问一句 —— 这个动作
   会把用户排好的顺序整块丢掉，而它没有撤销（唯一"后悔药"是不点保存就退出）。 */
static void CmnReset(void) {
    if (MessageBoxW(g_hwnd,
                    L"把「常用」恢复成默认的 11 项？\n\n"
                    L"当前的增删与顺序会被丢弃（点「保存」才真正写入配置）。",
                    APP_TITLE, MB_OKCANCEL | MB_ICONQUESTION) != IDOK) return;
    SendMessageW(CmnList(), LB_RESETCONTENT, 0, 0);
    CmnAddKeys(INFO_COMMON_DEFAULT);
    SendMessageW(CmnList(), LB_SETCURSEL, 0, 0);
    CmnSyncButtons();
    UpdateDirty();
    SetStatus(L"已恢复默认的 11 项（点「保存」才写入配置）");
}

/* ---------------- "有没有未保存的改动"：与**基线**比，不是"动过就脏" ----------------

   判据为什么必须是比对而不是记账：EDIT 控件收到 WM_SETTEXT **无条件**发
   EN_CHANGE —— 哪怕写进去的文本和原来一个字都不差。于是"尺寸选取窗把同样
   的值填回来"、"重新加载后又写一遍"这些**没有改动**的动作也会把窗口标成
   已修改，标题栏凭空多一个 `*`，用户以为自己改了什么、退出时还得犹豫一下。
   所以这里存一份"已保存基线快照"（加载后 / 保存后各拍一次），任何控件通知
   来了就**重新比对一遍**：真的不同才脏，改回原样就自动不脏。

   快照形态：字段存文本（勾选框存 "0"/"1"，字体按钮存"字体名|字号"，因为它
   的值在配置层而不在控件里）；名单存 '\n' 连接的整段 —— 都是纯字符串比较，
   没有第二套判据。 */

#define LIST_SNAP_MAX (CFG_MAX_ENTRIES * (CFG_NAME_MAX + 1))   /* 一份名单拍平的字数上限 */
#define FIELD_TEXT_MAX 320                                      /* 文本项快照缓冲（当前没有文本项，留作通用能力） */
static WCHAR g_baseField[FIELD_N][FIELD_TEXT_MAX];
static WCHAR g_baseList[LIST_N][LIST_SNAP_MAX];
static WCHAR g_snapTmp[LIST_SNAP_MAX];        /* 比对时的临时缓冲（一次用一份） */

/* 第 i 个字段的当前值 -> 文本 */
static void FieldSnapshot(int i, WCHAR* out, size_t cap) {
    const Field* f = &kFields[i];
    HWND c = Ctl(f->id);
    if (!c) { out[0] = 0; return; }
    if (f->isBool) { out[0] = GetCheck(c) ? L'1' : L'0'; out[1] = 0; return; }
    GetWindowTextW(c, out, (int)cap);
}

/* 第 i 份名单的当前内容 -> 文本（'\n' 连接，顺序即界面顺序） */
static void ListSnapshot(int i, WCHAR* out, size_t cap) {
    HWND lb = Ctl(IDC_LIST0 + i * LIST_STRIDE);
    out[0] = 0;
    if (!lb) return;
    int n = (int)SendMessageW(lb, LB_GETCOUNT, 0, 0);
    for (int j = 0; j < n && j < CFG_MAX_ENTRIES; j++) {
        WCHAR name[CFG_NAME_MAX] = L"";
        if (SendMessageW(lb, LB_GETTEXT, (WPARAM)j, (LPARAM)name) == LB_ERR) break;
        wcscat_s(out, cap, name);
        wcscat_s(out, cap, L"\n");
    }
}

/* 拍一次基线（加载后 / 保存成功后调用 —— 那一刻"界面 == ini"） */
static void TakeSnapshot(void) {
    for (int i = 0; i < FIELD_N; i++) FieldSnapshot(i, g_baseField[i], FIELD_TEXT_MAX);
    for (int i = 0; i < LIST_N; i++)  ListSnapshot(i, g_baseList[i], LIST_SNAP_MAX);
    CommonSnapshot(g_baseCommon, CMN_TEXT_MAX);
}

/* 重新判定：当前界面 vs 基线。装载期不判（那会儿正在往控件里写值）。 */
void UpdateDirty(void) {
    if (g_loading) return;
    BOOL d = FALSE;
    for (int i = 0; i < FIELD_N && !d; i++) {
        FieldSnapshot(i, g_snapTmp, LIST_SNAP_MAX);
        if (wcscmp(g_snapTmp, g_baseField[i]) != 0) d = TRUE;
    }
    for (int i = 0; i < LIST_N && !d; i++) {
        ListSnapshot(i, g_snapTmp, LIST_SNAP_MAX);
        if (wcscmp(g_snapTmp, g_baseList[i]) != 0) d = TRUE;
    }
    if (!d) {
        WCHAR cur[CMN_TEXT_MAX];
        CommonSnapshot(cur, CMN_TEXT_MAX);
        if (wcscmp(cur, g_baseCommon) != 0) d = TRUE;
    }
    if (d != g_dirty) { g_dirty = d; UpdateTitle(); }
}

/* 页签显隐：按 WS_VISIBLE **风格位**判断，状态没变就不发 ShowWindow。
   不用 IsWindowVisible —— 初次 Layout 跑在主窗口 ShowWindow 之前，那时
   IsWindowVisible 把风格上可见的控件也判成隐藏，判断依据就是错的。 */
static void ShowCtrl(HWND h, BOOL show) {
    if (!h) return;
    BOOL vis = (GetWindowLongW(h, GWL_STYLE) & WS_VISIBLE) != 0;
    if (vis != show) ShowWindow(h, show ? SW_SHOW : SW_HIDE);
}

/* ---------------- 字体 / DPI ---------------- */

typedef BOOL  (WINAPI* PFN_SetProcessDpiAwarenessContext)(HANDLE);
typedef BOOL  (WINAPI* PFN_SetProcessDPIAware)(void);
typedef UINT  (WINAPI* PFN_GetDpiForWindow)(HWND);
typedef BOOL  (WINAPI* PFN_SystemParametersInfoForDpi)(UINT, UINT, PVOID, UINT, UINT);

/* 声明 DPI 感知：**必须早于创建任何窗口**。不声明的话系统会按 96 DPI 把
   整个窗口**位图拉伸**到高 DPI 上 —— 文字发虚就是这个原因，而不是"字体太小"。
   Per-Monitor V2 拿不到（Win10 1607 之前）就退回进程级感知，再不行就听天由命。 */
static void InitDpiAwareness(void) {
    HMODULE u = GetModuleHandleW(L"user32.dll");
    if (u) {
        PFN_SetProcessDpiAwarenessContext p =
            (PFN_SetProcessDpiAwarenessContext)GetProcAddress(u, "SetProcessDpiAwarenessContext");
        /* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 == (HANDLE)-4 */
        if (p && p((HANDLE)(INT_PTR)-4)) return;
        PFN_SetProcessDPIAware q =
            (PFN_SetProcessDPIAware)GetProcAddress(u, "SetProcessDPIAware");
        if (q) q();
    }
}

static UINT QueryDpi(void) {
    HMODULE u = GetModuleHandleW(L"user32.dll");
    if (u && g_hwnd) {
        PFN_GetDpiForWindow p = (PFN_GetDpiForWindow)GetProcAddress(u, "GetDpiForWindow");
        if (p) { UINT d = p(g_hwnd); if (d >= 96) return d; }
    }
    HDC hdc = GetDC(NULL);
    UINT d = 96;
    if (hdc) { d = (UINT)GetDeviceCaps(hdc, LOGPIXELSY); ReleaseDC(NULL, hdc); }
    return d ? d : 96;
}

/* 字体平滑质量：系统开了 ClearType 就用 CLEARTYPE_QUALITY，否则退化成不抗锯齿。
   为什么要显式设：NONCLIENTMETRICS 给的消息字体 `lfQuality` 是 0（DEFAULT_QUALITY，
   走灰度抗锯齿），粗体在灰度 AA 下笔画会糊成一团 —— 这正是"加粗显示不太好"的
   主因（不是合成了粗体：实测 FW_BOLD/600 都映射到 msyhbd.ttc 里 700 的真字面，
   `GetTextMetrics` 返回 tmWeight=700）。 */
static BYTE FontQuality(void) {
    BOOL smooth = FALSE;
    if (SystemParametersInfoW(SPI_GETFONTSMOOTHING, 0, &smooth, 0) && smooth)
        return CLEARTYPE_QUALITY;
    return NONANTIALIASED_QUALITY;
}

/* 按当前 DPI 建字体：优先用系统为这个 DPI 算好的消息字体（NONCLIENTMETRICS
   里的 lfMessageFont 已经缩放过），拿不到就按 DPI 自己算一个字号。
   段头 = 同一套字族 + **FW_BOLD（700）**：Windows 的 CJK UI 字体族里
   `Microsoft YaHei UI` 带真 Bold 字面（msyhbd.ttc），所以这里是真粗体、
   不是 GDI 合成加粗；再配上 ClearType 才有"强调"且不糊。
   （若某台机器只装了 Light/Regular，GDI 会回落到合成加粗 —— 那种情况下
   显示会差一些，但没有更好的选择：换 SimHei / 等线是另一套字形，混排更怪。） */
static void MakeFont(void) {
    HFONT nf = NULL, hf = NULL;
    BYTE q = FontQuality();
    HMODULE u = GetModuleHandleW(L"user32.dll");
    if (u) {
        PFN_SystemParametersInfoForDpi p =
            (PFN_SystemParametersInfoForDpi)GetProcAddress(u, "SystemParametersInfoForDpi");
        if (p) {
            NONCLIENTMETRICSW ncm;
            memset(&ncm, 0, sizeof(ncm));
            ncm.cbSize = sizeof(ncm);
            if (p(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, (UINT)g_dpi)) {
                LOGFONTW lf = ncm.lfMessageFont;
                lf.lfQuality = q;
                lf.lfWeight  = FW_NORMAL;
                nf = CreateFontIndirectW(&lf);
                LOGFONTW lb = lf;
                lb.lfWeight = FW_BOLD;
                hf = CreateFontIndirectW(&lb);
            }
        }
    }
    if (!nf) {
        nf = CreateFontW(-MulDiv(9, g_dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                         DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                         q, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
        hf = CreateFontW(-MulDiv(9, g_dpi, 72), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                         DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                         q, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    }
    if (!nf) { nf = (HFONT)GetStockObject(DEFAULT_GUI_FONT); hf = nf; }
    if (!hf) hf = nf;
    if (g_font && g_font != nf) DeleteObject(g_font);
    g_font = nf;
    /* hf 可能与 nf 同柄（兜底路径），同柄时绝不能在这里删掉 */
    if (g_fontHead && g_fontHead != hf && g_fontHead != g_font) DeleteObject(g_fontHead);
    g_fontHead = hf;
}

static BOOL CALLBACK FontApplyCb(HWND h, LPARAM lp) {
    /* 段头（含「常用字段」那块子段头）走粗体，其余控件统一消息字体 */
    int id = GetDlgCtrlID(h);
    HFONT f = ((id >= IDC_GROUP0 && id < IDC_GROUP0 + GROUP_MAX) || id == IDC_CMN_HEAD)
                  ? g_fontHead : g_font;
    SendMessageW(h, WM_SETFONT, (WPARAM)f, TRUE);
    (void)lp;
    return TRUE;
}
static void ApplyFont(void) { EnumChildWindows(g_hwnd, FontApplyCb, 0); }

/* ---------------- 载入 / 保存 ---------------- */

static void FillList(HWND lb, int list) {
    SendMessageW(lb, LB_RESETCONTENT, 0, 0);
    for (int i = 0; i < ConfigListCount(list); i++)
        SendMessageW(lb, LB_ADDSTRING, 0, (LPARAM)ConfigListAt(list, i));
}

static void LoadConfig(void) {
    /* 文件不存在时让 config.c 写一份带注释的模板（与本体首次启动一致），
       再来读 —— 否则界面是空的，用户不知道该填什么。 */
    if (GetFileAttributesW(g_path) == INVALID_FILE_ATTRIBUTES)
        ConfigLoad();
    ConfigLoadFrom(g_path);

    g_loading = TRUE;
    for (int i = 0; i < FIELD_N; i++) {
        const Field* f = &kFields[i];
        if (f->cfg < 0) continue;                  /* 纯界面控件不承载配置值 */
        HWND c = Ctl(f->id);
        if (!c) continue;
        if (f->isBool)      SetCheck(c, CfgBool(f->cfg));
        else if (f->isText) SetWindowTextW(c, CfgStr(f->cfg));
        else                SetIntField(c, CfgInt(f->cfg));
    }
    for (int i = 0; i < LIST_N; i++) {
        FillList(Ctl(IDC_LIST0 + i * LIST_STRIDE), kLists[i].list);
        SetWindowTextW(Ctl(IDC_NAME0 + i * LIST_STRIDE), L"");
    }
    CmnFill();           /* 「常用」字段的顺序（空 = 默认那 11 项，判据与弹窗同源） */
    CmnSyncButtons();
    g_loading = FALSE;
    /* 装载完之后拍基线：此刻"界面 == ini"，之后任何偏离才算"未保存的改动" */
    TakeSnapshot();
    g_dirty = FALSE;
    UpdateTitle();
    SetStatus(L"");
}

/* 保存 = 界面 -> 配置层 -> 增量写回 ini -> 让本体重启。
   为什么不整份重写：ini 里有大量注释，用户也可能留下我们不认识的段/键，
   整份重写会把它们冲掉。ConfigSaveFile 只改"我们认识的那几行"。 */
static BOOL SaveConfig(void) {
    for (int i = 0; i < FIELD_N; i++) {
        if (kFields[i].isBool || kFields[i].isText || kFields[i].isBtn) continue;
        /* 文本框与按钮没有"值域"可校验 */
        HWND c = Ctl(kFields[i].id);
        if (!c) continue;
        int v = 0;
        if (!ReadIntField(c, &kFields[i], &v)) {
            WCHAR msg[320];
            _snwprintf_s(msg, 320, _TRUNCATE,
                         L"「%s」只能填数字（%d..%d）。", kFields[i].label,
                         kFields[i].min, kFields[i].max);
            MessageBoxW(g_hwnd, msg, APP_TITLE, MB_ICONEXCLAMATION | MB_OK);
            SetFocus(c);
            return FALSE;
        }
    }

    for (int i = 0; i < FIELD_N; i++) {
        const Field* f = &kFields[i];
        if (f->cfg < 0) continue;                  /* 按钮不承载配置值 */
        HWND c = Ctl(f->id);
        if (!c) continue;
        if (f->isBool) {
            CfgSetBool(f->cfg, GetCheck(c));
        } else if (f->isText) {
            WCHAR t[FIELD_TEXT_MAX];
            GetWindowTextW(c, t, FIELD_TEXT_MAX);
            CfgSetStr(f->cfg, t);
        } else {
            int v = 0;
            ReadIntField(c, f, &v);                /* 校验已过，这里必然成功 */
            CfgSetInt(f->cfg, v);
        }
    }

    /* 名单：以列表框为准整份替换（顺序即界面顺序）。
       条数上限由 ConfigSetList 在配置层守住。 */
    for (int i = 0; i < LIST_N; i++) {
        HWND lb = Ctl(IDC_LIST0 + i * LIST_STRIDE);
        int n = (int)SendMessageW(lb, LB_GETCOUNT, 0, 0);
        static WCHAR   names[CFG_MAX_ENTRIES][CFG_NAME_MAX];
        static const WCHAR* ptr[CFG_MAX_ENTRIES];
        if (n > CFG_MAX_ENTRIES) n = CFG_MAX_ENTRIES;
        for (int j = 0; j < n; j++) {
            SendMessageW(lb, LB_GETTEXT, (WPARAM)j, (LPARAM)names[j]);
            ptr[j] = names[j];
        }
        ConfigSetList(kLists[i].list, n ? ptr : NULL, n);
    }

    /* 「常用」字段顺序：以列表框为准整份替换（顺序即界面顺序）。它不在
       kFields / kLists 里，所以这一段得手写 —— 别指望上面两个循环覆盖它。 */
    CmnSave();

    if (!ConfigSaveFile(g_path)) {
        SetStatus(L"保存失败：写不进 %s（文件被占用或目录只读？）", g_path);
        MessageBoxW(g_hwnd, L"保存失败：无法写入配置文件。\n"
                            L"可能文件正被其它程序占用，或所在目录只读。",
                    APP_TITLE, MB_ICONERROR | MB_OK);
        return FALSE;
    }
    /* 写回成功 = 界面与 ini 重新对齐：重拍基线（比"直接置 g_dirty=FALSE"
       稳 —— 万一某些值被配置层钳过，界面对不上，那也该照实显示为已改动）。 */
    TakeSnapshot();
    UpdateDirty();
    /* 文案对"只保存"和"保存并重启"都成立：真重启的话 RestartMain 会马上
       把状态栏覆写成"已保存并请求重启"。 */
    SetStatus(L"已保存 %s（重启 CapsEnhance 后生效）", g_path);
    return TRUE;
}

/* 让本体重启：带 --restart 启动一次 —— 若本体在跑，它只是转告在跑的那个实例
   重启后自己退出；若没在跑，这次启动就是"配好之后直接开跑"。 */
static void RestartMain(void) {
    WCHAR path[MAX_PATH];
    _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s%s", g_dir, MAIN_EXE);
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
        SetStatus(L"已保存，但同目录下没有 %s —— 本体不会重启", MAIN_EXE);
        return;
    }
    WCHAR cmd[MAX_PATH + 32];
    _snwprintf_s(cmd, MAX_PATH + 32, _TRUNCATE, L"\"%s\" --restart", path);

    STARTUPINFOW si; PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si)); si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));
    if (CreateProcessW(path, cmd, NULL, NULL, FALSE, 0, NULL, g_dir, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        SetStatus(L"已保存并请求重启 CapsEnhance");
        return;
    }
    if ((int)(INT_PTR)ShellExecuteW(NULL, L"open", path, L"--restart", g_dir,
                                    SW_SHOWNORMAL) > 32) {
        SetStatus(L"已保存并请求重启 CapsEnhance");
        return;
    }
    SetStatus(L"已保存，但重启 CapsEnhance 失败（请从托盘手动重启）");
}

static void DoSave(void) {
    if (SaveConfig()) RestartMain();
}

/* 关窗 = **丢弃**（不写 ini）。
   有未保存改动时先问一句 —— 手滑点错还能救回来；没改动就直接关，不多话。
   默认按钮给"否"：这个对话框默认动作必须是"不丢东西"的那一边。
   为什么问句不是"要保存吗"：界面上已经有「保存」/「保存并重启」两颗明确的
   提交按钮，退出再问一遍等于把决定权从用户手里拿回来一次；而且问句里那个
   "保存并重启"会把"我只是想关掉"变成"顺手重启了本体"。 */
static void CloseNoSave(void) {
    if (g_dirty &&
        MessageBoxW(g_hwnd, L"有未保存的改动，退出将丢弃。\n确定退出？",
                    APP_TITLE, MB_ICONQUESTION | MB_YESNO | MB_DEFBUTTON2) != IDYES)
        return;
    DestroyWindow(g_hwnd);
}

/* ---------------- 界面 ---------------- */

/* ---------------- 滚动：**只挪一个承载窗** ----------------
   结构：主窗口 → 承载窗（g_form）→ 全部表单控件；页签 / 状态栏 / 四颗按钮住在
   顶部与底部的"窗框"里，不归承载窗管。滚动 = 把承载窗整体上下挪 g_off 像素，
   子控件相对它一动不动。重排（Layout）只在建窗 / 换页 / 缩放 / DPI 变化时跑，
   **滚动路径里没有 Layout**。

   ★ 为什么不再"逐个子控件按虚拟 y − off 重排"（旧写法）：
     ① 每滚一格要 MoveWindow 几十个子控件，每个都带一次失效/重绘 —— 观感是
        几十次重排叠出来的顿挫，也就是"总感觉是跳过去的"；
     ② 上一个版本试过 DeferWindowPos + WM_SETREDRAW + 手动 FillRect +
        RedrawWindow(RDW_UPDATENOW) 去治顿挫，结果更糟：同步整帧重画要几十
        毫秒，消息循环回不去，系统就把后续滚轮**回落给焦点窗口** —— 表现是
        "设置页滚动，背后的程序跟着滚"。那次改动已整体回退（git a5defb0 引入、
        64c8637 / 9454402 回退），而回退后的旧写法从未出现该现象 —— 因为
        "处理得慢"才是那个 bug 的燃料（WM_MOUSEWHEEL 发给**焦点窗口**，
        DefWindowProc 只沿父链往上冒泡；悬停滚动是系统额外加的一层路由，
        它等不及就落回去）。
     ③ 承载窗把每帧代价从 N 次窗口移动降到 **1 次 SetWindowPos**：滚轮消息
        只改一个目标值就返回，绝不阻塞消息循环。两条硬约束（不跳、不把滚轮
        漏给背后程序）因此同时成立。
   ★ 为什么不是 ScrollWindowEx(SW_SCROLLCHILDREN) 直接滚主窗口：MSDN 写明
     子窗口只露出一部分时，系统**不会**正确擦除/重绘滚出源矩形的部分
     （文档建议这种场景改用 DeferWindowPos）—— 那正是以前"拖影"的来源。
     承载窗里的子控件永远完整地呆在它的客户区内，不存在"部分可见的子控件
     被人挪了"这种没人负责的像素。

   滚动条仍由主窗口的 WS_VSCROLL 持有（外部 UIA / 读屏也能拨它）；g_off 是
   本进程的事实来源，两者在 WM_PAINT 里对账（外部拨动直接对齐，不补动画）。 */

static int MaxScroll(void) {
    return g_contentH > g_viewH ? g_contentH - g_viewH : 0;
}

static int ScrollPos(void) {
    SCROLLINFO si;
    memset(&si, 0, sizeof(si));
    si.cbSize = sizeof(si);
    si.fMask  = SIF_POS;
    GetScrollInfo(g_hwnd, SB_VERT, &si);
    return si.nPos;
}

static void UpdateScrollBar(int pos) {
    SCROLLINFO si;
    memset(&si, 0, sizeof(si));
    si.cbSize = sizeof(si);
    si.fMask  = SIF_RANGE | SIF_PAGE | SIF_POS | SIF_DISABLENOSCROLL;
    si.nMin   = 0;
    si.nMax   = g_contentH > 0 ? g_contentH - 1 : 0;
    si.nPage  = (UINT)(g_viewH > 0 ? g_viewH : 1);
    si.nPos   = pos;
    SetScrollInfo(g_hwnd, SB_VERT, &si, TRUE);
}

/* 把承载窗挪到当前偏移。**整条滚动路径上唯一真正干活的调用**。
   oldOff 用来算出让出来的那一条：内容往上走时承载窗底边也在上移，露在可视区
   里的那截归主窗口的背景负责（其余被页签窗框 / 底部窗框盖住）。只失效这一条，
   不碰任何子控件 —— 它们相对承载窗没动，没有重画的理由。 */
static void MoveHost(int oldOff) {
    if (!g_form) return;
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    int hostH = g_contentH > g_viewH ? g_contentH : g_viewH;   /* 与 Layout 同一口径 */
    int oldBottom = g_viewTop - oldOff + hostH;
    int newBottom = g_viewTop - g_off + hostH;
    SetWindowPos(g_form, NULL, 0, g_viewTop - g_off, 0, 0,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSIZE);
    if (newBottom < oldBottom) {
        RECT strip;
        strip.left   = 0;
        strip.right  = rc.right;
        strip.top    = newBottom;
        strip.bottom = oldBottom;
        if (strip.top < 0) strip.top = 0;
        if (strip.bottom > rc.bottom) strip.bottom = rc.bottom;
        if (strip.bottom > strip.top) InvalidateRect(g_hwnd, &strip, TRUE);
    }
}

/* 缓动帧的来源：**高精度可等待计时器 + 常驻帧线程**。
   这条路上踩过三层坑，都记在这里，别再回头试：
     ① SetTimer 的周期下限是 USER_TIMER_MINIMUM（10ms），怎么设都到不了 100fps
        以上；240Hz 屏一次刷新只有 ~4.17ms，60/100fps 的动画每帧要被合成 2~4 次
        —— 这就是"台阶"的来源。
     ② CreateTimerQueueTimer(4ms) 实测**仍然**是 ~16ms 一帧：Windows 会把计时器
        队列按系统节拍（15.6ms）合并，timeBeginPeriod(1) 也救不了它（实测同进程
        Sleep(4) 是 4.7ms，队列计时器还是 17ms）。
     ③ DwmFlush 跟着合成器出帧看着最理想，但**没有合成时它不返回** —— 实测动画
        会整个卡在第一帧。不能用。
   现在：帧线程里用 CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 的等时器（实测 2ms
   请求落在 2.3~3.7ms，且不需要改全局计时精度）。间隔取**刷新周期的一半**
   （240Hz → 2ms，60Hz → 8ms）：保证每次合成都有新状态可取，又不在低刷屏上
   白烧 CPU。UI 线程跟不上时用 s_tickPending 合并 —— 曲线按时间算，丢的只是
   采样点，不会拖慢到位时间。 */
static DWORD WINAPI AnimThreadProc(LPVOID p) {
    (void)p;
    HANDLE timer = CreateWaitableTimerExW(NULL, NULL,
                                          CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                          TIMER_ALL_ACCESS);
    for (;;) {
        if (WaitForSingleObject(s_animGo, INFINITE) != WAIT_OBJECT_0) break;
        if (s_animQuit) break;
        while (g_anim) {
            if (InterlockedExchange(&s_tickPending, 1) == 0)
                PostMessageW(g_hwnd, WM_SCROLL_TICK, 0, 0);
            DWORD ms = (DWORD)(g_tickMs > 0 ? g_tickMs : 4);
            if (timer) {
                LARGE_INTEGER due;
                due.QuadPart = -((LONGLONG)ms * 10000);   /* 相对时刻，单位 100ns */
                if (SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE))
                    WaitForSingleObject(timer, 500);
                else
                    Sleep(ms);
            } else {
                Sleep(ms);      /* 没有高精度计时器：Sleep 会被节拍放大，但至少能走 */
            }
        }
    }
    if (timer) CloseHandle(timer);
    return 0;
}

static void AnimStop(void) {
    g_anim = FALSE;      /* 线程每帧检查；当前这一帧最多再走完一次就退出内层 */
}

/* 窗口所在显示器的刷新率（拿不到返回 0）。按**显示器**问而不是查屏幕 DC：
   窗口拖到副屏（60Hz 主屏 + 240Hz 副屏）时，帧间隔要跟着副屏走。 */
static int ScreenHz(void) {
    MONITORINFOEXW mi;
    memset(&mi, 0, sizeof(mi));
    mi.cbSize = sizeof(mi);
    HMONITOR mon = MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST);
    if (mon && GetMonitorInfoW(mon, (MONITORINFO*)&mi)) {
        DEVMODEW dm;
        memset(&dm, 0, sizeof(dm));
        dm.dmSize = sizeof(dm);
        if (EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) &&
            dm.dmDisplayFrequency >= 30 && dm.dmDisplayFrequency <= 500)
            return (int)dm.dmDisplayFrequency;
    }
    return 0;
}

/* 起/续一轮缓动。已经有一条动画在跑时只"换目标 + 重算时长"：连续滚轮时目标
   每几毫秒就可能变一次，重建帧源反而更抖。 */
static void AnimKick(void) {
    int dist = g_target - g_off;
    if (dist < 0) dist = -dist;
    g_animFrom = g_off;
    {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        g_animT0 = now.QuadPart;
    }
    g_animDur  = 70 + dist * 45 / 100;      /* 距离越长给的时间越长 */
    if (g_animDur < 90)  g_animDur = 90;
    if (g_animDur > 200) g_animDur = 200;
    if (!g_tickMs) {
        /* 帧间隔 = 刷新周期的一半（见文件头"滚动"一节）。拿不到就按 120Hz 算 */
        int hz = ScreenHz();
        if (hz < 30) hz = 120;
        g_tickMs = 500 / hz;
        if (g_tickMs < 2) g_tickMs = 2;
        if (g_tickMs > 8) g_tickMs = 8;
    }
    if (g_anim) return;
    if (!s_animThread || !s_animGo) {
        /* 帧线程没建起来（极端资源不足）：直接落到目标，别把界面卡在半路 */
        int old = g_off;
        g_off = g_target;
        MoveHost(old);
        UpdateScrollBar(g_off);
        return;
    }
    g_anim = TRUE;
    SetEvent(s_animGo);
}

/* 缓动本身：**按时间算**（ease-out cubic），不是"每帧吃掉固定比例的剩余距离"。
   按帧算的曲线在 240Hz 上会比 60Hz 上更早到位，手感与刷新率绑定；按时间算则
   同一时长在任何刷新率下都是同一条曲线，帧只是采样点。
   ★ 时间必须用 QueryPerformanceCounter，不能用 GetTickCount：后者只有 ~15.6ms
     分辨率，帧间隔 2~8ms 时会出现"同一毫秒里连画几帧位置不动、跨过这 15.6ms
     才跳一大步"—— 帧源再快也白搭（这是实测踩到的）。
   ★ 刻意**不**跟随系统"动画效果"开关（SPI_GETCLIENTAREAANIMATION）：这不是
     装饰动画，它就是滚动本体 —— 跟随的话，关掉系统动画的用户拿到的仍是
     一跳一跳的滚动。这一条是明确决策，别"顺手"补成跟随。 */
static void AnimTick(void) {
    if (!g_anim) return;                    /* 停掉之后可能还残留一条已投递的帧 */
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double el = (double)(now.QuadPart - g_animT0) * 1000.0 / (double)s_qpcFreq.QuadPart;
    int pos;
    if (el >= (double)g_animDur) {
        pos = g_target;
    } else {
        double p = el / (double)g_animDur;
        double q = 1.0 - p;
        double e = 1.0 - q * q * q;         /* ease-out cubic：起步快、收尾稳 */
        pos = g_animFrom + (int)((g_target - g_animFrom) * e);
    }
    if (pos != g_off) {
        int old = g_off;
        g_off = pos;
        MoveHost(old);
        UpdateScrollBar(g_off);
    }
    if (el >= (double)g_animDur) AnimStop();
}

/* 滚到指定位置（钳位）。animate = FALSE 用于：滚动条拖拽（必须 1:1 跟手）、
   外部拨动（自愈不补动画）、换页归零。 */
static void ScrollTo(int pos, BOOL animate) {
    if (pos < 0) pos = 0;
    if (pos > MaxScroll()) pos = MaxScroll();
    if (!animate) {
        int old = g_off;
        AnimStop();
        g_off = g_target = pos;
        MoveHost(old);
        UpdateScrollBar(g_off);
        return;
    }
    g_target = pos;
    if (g_off == g_target) { AnimStop(); return; }
    AnimKick();
}

/* 滚轮：一格 = 系统"每次滚 N 行" × 表单一行（S(30)）≈ 90 逻辑像素。
   与 ScrollListBy 同一口径：系统设 0 行 = 不滚、设"一屏" = 一页。
   ★ 小数刻度必须累计：高精度滚轮 / 触控板一条 WM_MOUSEWHEEL 的 delta 常常
     只有 30、60（甚至 1），旧写法 `delta / WHEEL_DELTA` 是整数除法，直接截成
     0 —— 表现为"触控板滚设置页完全不动"。累计够一格才走一步。 */
static void WheelScroll(int delta) {
    DWORD lines = 3;
    if (!SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0)) lines = 3;
    if (lines == 0) return;                 /* 系统设置：不使用滚轮滚动 */
    int step = (lines == WHEEL_PAGESCROLL) ? (g_viewH - S(40)) : (int)lines * S(30);
    if (step < 1) step = 1;
    g_wheelRem += delta;
    int notches = g_wheelRem / WHEEL_DELTA;
    if (!notches) return;
    g_wheelRem -= notches * WHEEL_DELTA;
    ScrollTo(g_target - notches * step, TRUE);   /* 滚轮向上 = delta 正 = 内容下移 */
}

/* 「选择字体…」这类**动作按钮**的宽度：按文字实测宽度 + 左右内边距，不再拉满
   整行。拉满整行会把它画成"一块区域"而不是"一个按钮"，也把这一行的视觉重心
   从输入框上抢走。上限仍是整行宽（极长文案时不会被裁）。 */
static int BtnTextW(HWND btn, int maxW) {
    WCHAR txt[128] = L"";
    if (btn) GetWindowTextW(btn, txt, 128);
    int tw = 0;
    HDC dc = btn ? GetDC(btn) : NULL;
    if (dc) {
        HGDIOBJ of = SelectObject(dc, g_font ? g_font : GetStockObject(DEFAULT_GUI_FONT));
        SIZE sz;
        if (GetTextExtentPoint32W(dc, txt, (int)wcslen(txt), &sz)) tw = sz.cx;
        SelectObject(dc, of);
        ReleaseDC(btn, dc);
    }
    int bw = tw + S(28);
    if (bw < S(104)) bw = S(104);
    if (bw > maxW)   bw = maxW;
    return bw;
}

/* 行高是固定的，位置按"从上往下排"算 —— 比手填坐标不容易错位。
   落位坐标是**承载窗内部坐标**（0 = 内容顶），与滚动偏移无关：滚动只挪承载窗，
   不改任何子控件的坐标（见"滚动"一节）。
   显隐也在这一轮（"显隐轮"）：先把两页的控件按当前页藏好/放开，再只给
   当前页落位 —— 显隐的**唯一事实来源**是这里，别处只改 g_curTab，
   不自己动 ShowWindow。 */
static void Layout(int w, int h) {
    int x = S(12);
    int rowBool = S(24), rowInt = S(26), gap = S(4);
    int editW = S(92);

    /* 页签条：住在顶部"窗框"（g_header）里，坐标是窗框内的。高度跟着字体走
       —— 页签自身的实际高度从 TCM_GETITEMRECT 取（换字体 / 换 DPI 都会变），
       控件高度再补一点内边距。页签内边距也按 DPI 重设（comctl 默认太挤）。 */
    int tabTop = S(6), tabH = S(30);
    if (g_tab) {
        RECT tr;
        if (g_tabPadDpi != g_dpi) {
            g_tabPadDpi = g_dpi;
            SendMessageW(g_tab, TCM_SETPADDING, 0, MAKELPARAM(S(14), S(7)));
        }
        if (TabCtrl_GetItemRect(g_tab, 0, &tr))
            tabH = tr.bottom + S(6);
        MoveWindow(g_tab, x, tabTop, w - x * 2, tabH, TRUE);
    }
    int viewTop = tabTop + tabH + S(4);   /* 内容可视区的上沿 */
    g_viewTop = viewTop;

    /* 内容可视区 = 页签条以下、底部固定区（状态栏 + 按钮 + 空隙）以上。
       窗口被拖得很矮时内容区保底 S(120)，此时底部窗框会被挤得薄于 S(60)、
       按钮会被裁掉一截 —— 本窗口没有最小尺寸保护（见下方 footer 注释）。 */
    g_viewH = (h - S(60)) - viewTop;
    if (g_viewH < S(120)) g_viewH = S(120);

    /* 布局 = 尺度变了：动画收掉；偏移等算完新内容高之后再钳（见承载窗那段） */
    AnimStop();

    /* 显隐轮：分页是结构性的（配置项 <-> 名单，见 kTabNames 处的说明），
       所以按"控件属于哪张表"整批显隐，不逐项记状态 —— 每次全量重排的
       同一个道理：状态与布局不脱节。 */
    BOOL infoPage    = (g_curTab == TAB_INFO);
    BOOL listPage    = (g_curTab == TAB_LISTS);
    for (int i = 0; i < FIELD_N; i++) {
        BOOL on = (FieldPage(i) == g_curTab);
        int id = kFields[i].id;
        ShowCtrl(Ctl(id), on);
        ShowCtrl(Ctl(id + 500), on);
        ShowCtrl(Ctl(id + 600), on);
        /* 段头随段显隐。id == 0 必须跳过：两个"窗框"曾经以 hMenu=NULL 创建
           （控件 ID = 0），GetDlgItem(..., 0) 会命中它们 —— 名单页把底部
           窗框（按钮托盘）当段头藏掉，按钮整条消失（实测踩过）。窗框现在
           有正式 ID，这里再加 id 判断双保险。 */
        if (g_grpHead[i])
            ShowCtrl(Ctl(g_grpHead[i]), on);
    }
    /* 「常用」字段编排属于「窗口信息」页（紧跟那一段的三行字体设置） */
    ShowCtrl(Ctl(IDC_CMN_HEAD), infoPage);
    ShowCtrl(Ctl(IDC_CMN_LIST), infoPage);
    for (int k = 0; k < 5; k++)
        ShowCtrl(Ctl(IDC_CMN_ADD + k), infoPage);
    for (int i = 0; i < LIST_N; i++) {
        int base = IDC_LIST0 + i * LIST_STRIDE;
        for (int k = 0; k <= 5; k++)      /* 组内控件占 0..5 六个 id */
            ShowCtrl(Ctl(base + k), listPage);
    }

    int y = S(4);                     /* 承载窗内部坐标：0 = 内容顶 */
    for (int i = 0; i < FIELD_N; i++) {
        if (FieldPage(i) != g_curTab) continue;   /* 不属于本页：一行都不排 */
        if (g_grpHead[i]) {               /* 换段：先排段头。首段不额外加空 ——
                                             页签条下面已经留了呼吸感 */
            if (i && FieldPage(i - 1) == g_curTab) y += S(12);
            MoveWindow(Ctl(g_grpHead[i]), x, y, w - x * 2, S(20), TRUE);
            y += S(26);
        }
        int id = kFields[i].id;
        HWND c = Ctl(id);
        if (!c) continue;
        if (kFields[i].isBtn) {
            /* 动作按钮按文字宽走（见 BtnTextW），不拉满整行 */
            MoveWindow(c, x, y, BtnTextW(c, w - x * 2), S(24), TRUE);
            y += rowBool;
        } else if (kFields[i].isBool) {
            MoveWindow(c, x, y, w - x * 2, rowBool - S(2), TRUE);
            y += rowBool;
        } else if (kFields[i].isText) {
            /* 文本行：**标签独占一行、输入框占满下一行**。
               照数字行那样"框在左、标签吃剩下的"是行不通的 —— 路径要看得全，
               框必须撑满；而标签是一整句，塞不进一行的剩余宽度。 */
            HWND lab = Ctl(id + 500);
            if (lab) MoveWindow(lab, x, y, w - x * 2, S(18), TRUE);
            y += S(20);
            MoveWindow(c, x, y, w - x * 2, S(22), TRUE);
            y += S(24);
        } else {
            /* 数字框放**左边**、单位紧跟在框右侧、标签吃剩下的空间：
               标签是长句，把输入框放在标签之后的话，窗口一窄它就被挤出
               客户区 —— 用户看到"有说明、没地方填"。输入框与单位宽固定。 */
            HWND lab  = Ctl(id + 500);
            HWND unit = Ctl(id + 600);
            int ux = x + editW + S(4), unitW = S(52);
            MoveWindow(c, x, y, editW, S(22), TRUE);
            if (unit)
                MoveWindow(unit, ux, y + S(4), unitW, S(18), TRUE);
            if (lab)
                MoveWindow(lab, ux + unitW + S(8), y + S(4),
                           w - (ux + unitW + S(8)) - x, S(18), TRUE);
            y += rowInt;
        }
        y += gap;
    }

    /* 「常用」字段编排（「窗口信息」页，紧跟字体那三行）：
       子段头 + 左侧有序列表 + 右侧竖排五颗按钮。按钮贴着列表，动的是哪个
       列表不用猜。列表给足默认那 11 项的高度（行高随字体走，所以按 DIP
       折算而不是按"行数"写死，高分屏上不会只剩半截）；再多它自己出滚动条。 */
    if (infoPage) {
        int cmnW = S(240), cmnRow = S(26), cmnGap = S(4);
        int cmnH = S(170);
        int bx = x + cmnW + S(8), bw = S(96);
        y += S(12);
        MoveWindow(Ctl(IDC_CMN_HEAD), x, y, w - x * 2, S(20), TRUE);
        y += S(24);
        MoveWindow(Ctl(IDC_CMN_LIST), x, y, cmnW, cmnH, TRUE);
        for (int k = 0; k < 5; k++)
            MoveWindow(Ctl(IDC_CMN_ADD + k),
                       bx, y + k * (cmnRow + cmnGap), bw, cmnRow, TRUE);
        y += cmnH + S(6);
    }

    /* 名单高度固定（约五行，再多名单自己有滚动条），五组顺序往下排 */
    y += S(6);
    int listH = S(84);
    /* 名单列表框**定宽**，不跟窗口拉伸：进程名撑死几十个字符，宽列表框
       右边全是用不上的白（328 ≈ 底部四个按钮并排 376 再砍 13%）；真有超长名字时
       列表框自带横向滚动条兜底 */
    int listW = w - x * 2;
    if (listW > S(328)) listW = S(328);
    for (int i = 0; i < LIST_N; i++) {
        if (!listPage) break;             /* 非名单页：名单一组都不排 */
        int base = IDC_LIST0 + i * LIST_STRIDE;
        MoveWindow(Ctl(base + 4), x, y, w - x * 2, S(18), TRUE);  /* 标题 */
        y += S(20);
        MoveWindow(Ctl(base), x, y, listW, listH, TRUE);
        y += listH + S(4);
        MoveWindow(Ctl(base + 1), x, y, S(220), S(24), TRUE);     /* 名字输入 */
        MoveWindow(Ctl(base + 2), x + S(228), y, S(70), S(24), TRUE);
        MoveWindow(Ctl(base + 3), x + S(304), y, S(70), S(24), TRUE);
        MoveWindow(Ctl(base + 5), x + S(380), y, S(88), S(24), TRUE);  /* 选取窗口 */
        y += S(28) + S(8);
    }
    /* 内容总高 = 承载窗高度。MaxScroll = contentH − viewH，滚到头时内容底正好
       贴可视区底。偏移在这里钳位（尺度假定已经变了），动画已在开头收掉。 */
    g_contentH = y + S(6);
    if (g_off > MaxScroll()) g_off = MaxScroll();
    if (g_off < 0) g_off = 0;
    g_target = g_off;

    /* 承载窗：宽度铺满客户区、高度 = max(内容总高, 可视区高)，位置 = 可视区
       上沿 − 偏移。**高度取 max 是有意的**：内容比可视区矮时也让承载窗铺满
       可视区，这样视口里永远没有"主窗口的空白"要擦 —— 切页/滚动时的重绘
       全部收敛在承载窗自己身上。
       用 SetWindowPos 而不是 MoveWindow(..., TRUE)：布局只改几何，重画交给
       控件自己的失效（换页另有整帧重画，见 WM_NOTIFY）。
       z 序：它在两个"窗框"之下（窗框在下面各自钉顶），移动时用 SWP_NOZORDER
       不许它把自己抬上来 —— 否则滚上去的内容会盖住页签条。 */
    if (g_form) {
        int hostH = g_contentH > g_viewH ? g_contentH : g_viewH;
        SetWindowPos(g_form, NULL, 0, viewTop - g_off, w, hostH,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }

    /* 顶部"窗框"：从客户区顶一直盖到内容可视区上沿，页签条是它的子控件 ——
       滚动中爬到可视区上方的内容滑到它底下被挡住，不会盖到页签条上。
       z 序与底部窗框同一待遇：每次布局显式钉到顶（谁上谁下并不可靠）。 */
    if (g_header) {
        MoveWindow(g_header, 0, 0, w, viewTop, TRUE);
        SetWindowPos(g_header, HWND_TOP, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    /* 底部固定区：一块实底"窗框"从可视内容区一直盖到客户区底，状态栏与
       按钮是它的子控件 —— 滚动中溢出的内容滑到它底下就被挡住，不会从
       按钮周围的缝隙里露出来。窗口高度正常时窗框高恒为 S(60)，内部坐标
       因此是常量；**矮窗口下会被内容区下限挤薄**（见 Layout 开头）。 */
    if (g_footer) {
        MoveWindow(g_footer, 0, viewTop + g_viewH, w, h - (viewTop + g_viewH), TRUE);
        /* 窗框的职责就是压住表单内容：每次布局都把 z 序重新钉到顶。
           （子窗口创建/移动后谁在上谁在下并不可靠，实测本机上"最后建的
           反而在底下"，所以这里不赌，显式钉。） */
        SetWindowPos(g_footer, HWND_TOP, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    /* 状态栏独占一行（按钮在它下面那行），给足整个客户区宽：
       窄窗口下提示文案也能完整显示，不用给按钮预留右侧空间 */
    if (g_status) MoveWindow(g_status, x, S(4), w - x * 2, S(18), TRUE);
    int bw = S(88), bg = S(8), fby = S(26);
    /* 右对齐排列，左->右：重新加载、保存、保存并重启、退出 */
    MoveWindow(GetDlgItem(g_footer, IDC_RELOAD),   w - x - bw * 4 - bg * 3, fby, bw, S(26), TRUE);
    MoveWindow(GetDlgItem(g_footer, IDC_SAVEONLY), w - x - bw * 3 - bg * 2, fby, bw, S(26), TRUE);
    MoveWindow(GetDlgItem(g_footer, IDC_SAVE),     w - x - bw * 2 - bg,     fby, bw, S(26), TRUE);
    MoveWindow(GetDlgItem(g_footer, IDC_EXIT),     w - x - bw,              fby, bw, S(26), TRUE);

    UpdateScrollBar(g_off);
}

/* ---------------- 按钮的自绘 ----------------
   默认的 Win32 按钮在 Win10/11 上是一块浅灰浮凸，一排按钮摆在一起既看不出
   主次、也看不出鼠标指没指着。这里在既有子类里把按钮的 WM_PAINT 整个接管：
     · "保存并重启"是主操作 -> 强调蓝填充，悬停变亮、按下变深（Fluent 主按钮）；
     · 其余按钮 -> 白底描边，悬停浅蓝底 + 蓝边（Fluent 次级按钮）；
     · 圆角、焦点虚线框、禁用态、空格键按下（BM_SETSTATE）一并画出。
   状态（悬停/按下/主次）存**窗口属性**：GWLP_USERDATA 已被原过程占用。
   点击行为不动 —— 按下/抬起仍交给按钮默认过程，BN_CLICKED 照常发给父窗口，
   所以跨进程 BM_CLICK、Tab 焦点、键盘触发全都和原来一样。 */

#define BTNPROP_PRIMARY L"capPrimary"
#define BTNPROP_HOVER   L"capHover"
#define BTNPROP_PRESSED L"capPressed"
#define TABPROP_HOT     L"capTabHot"   /* 页签悬停项下标 +1（属性不存在 = -1） */

static BOOL BtnFlag(HWND h, const WCHAR* prop) {
    return (BOOL)(INT_PTR)GetPropW(h, prop);
}
static void SetBtnFlag(HWND h, const WCHAR* prop, BOOL v) {
    if (BtnFlag(h, prop) != v) {
        SetPropW(h, prop, (HANDLE)(INT_PTR)(v ? 1 : 0));
        InvalidateRect(h, NULL, TRUE);
        UpdateWindow(h);          /* 悬停/按下要"即时"反馈，等下次 PAINT 太钝 */
    }
}

static void TrackBtnHover(HWND h) {
    TRACKMOUSEEVENT t;
    memset(&t, 0, sizeof(t));
    t.cbSize = sizeof(t);
    t.dwFlags = TME_LEAVE;
    t.hwndTrack = h;
    TrackMouseEvent(&t);
}

static void PaintButton(HWND h, HDC dc) {
    RECT rc;
    GetClientRect(h, &rc);
    BOOL primary = BtnFlag(h, BTNPROP_PRIMARY);
    BOOL hover   = BtnFlag(h, BTNPROP_HOVER);
    BOOL pressed = BtnFlag(h, BTNPROP_PRESSED);
    BOOL enabled = IsWindowEnabled(h);

    /* 先铺父窗口的底色：圆角露出的四个角才像是"透明"的 */
    {
        HBRUSH bg = CreateSolidBrush(GetSysColor(COLOR_BTNFACE));
        FillRect(dc, &rc, bg);
        DeleteObject(bg);
    }

    COLORREF fill, border, text;
    if (!enabled) {
        fill = RGB(246, 246, 246); border = RGB(189, 189, 189); text = RGB(160, 160, 160);
    } else if (primary) {
        fill   = pressed ? RGB(0, 90, 158)   : hover ? RGB(26, 134, 217) : RGB(0, 120, 215);
        border = fill;
        text   = RGB(255, 255, 255);
    } else {
        fill   = pressed ? RGB(204, 228, 247) : hover ? RGB(229, 241, 251) : RGB(252, 252, 252);
        border = pressed ? RGB(0, 84, 153)    : hover ? RGB(0, 120, 215)   : RGB(173, 173, 173);
        text   = RGB(0, 0, 0);
    }

    int rad = S(4);
    HPEN pen = CreatePen(PS_SOLID, 1, border);   /* 画笔没有 W/A 之分 */
    HBRUSH br = CreateSolidBrush(fill);
    HGDIOBJ open = SelectObject(dc, pen);
    HGDIOBJ obr = SelectObject(dc, br);
    /* 右/下各收 1px：RoundRect 的边画在包围矩形**边上**，不收会被客户区裁掉 */
    RoundRect(dc, rc.left, rc.top, rc.right - 1, rc.bottom - 1, rad * 2, rad * 2);
    SelectObject(dc, open);
    SelectObject(dc, obr);
    DeleteObject(pen);
    DeleteObject(br);

    WCHAR txt[96];
    GetWindowTextW(h, txt, 96);
    HFONT f = (HFONT)SendMessageW(h, WM_GETFONT, 0, 0);
    if (!f) f = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    HGDIOBJ of = SelectObject(dc, f);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, text);
    RECT tr = rc;
    if (pressed && !primary) OffsetRect(&tr, 0, 1);   /* 次级按钮按下时文字下沉一点 */
    DrawTextW(dc, txt, -1, &tr,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(dc, of);

    if (GetFocus() == h) {                 /* 键盘焦点：虚线框（对话框惯例） */
        RECT frc = rc;
        InflateRect(&frc, -S(3), -S(3));
        DrawFocusRect(dc, &frc);
    }
}

/* 只自绘**普通下压按钮**。类名判断远远不够 —— 勾选框（BS_AUTOCHECKBOX）、
   单选钮、分组框全是 "Button" 类，按类名放行会把它们也画成下压按钮
   （实测：勾选框整个变成一长条按钮，复选框本身消失了）。
   只认 BS_PUSHBUTTON / BS_DEFPUSHBUTTON 两种式样。 */
static BOOL BtnIsPush(HWND h) {
    WCHAR cls[16];
    if (!(GetClassNameW(h, cls, 16) > 0 && _wcsicmp(cls, L"Button") == 0))
        return FALSE;
    DWORD st = (DWORD)GetWindowLongPtrW(h, GWL_STYLE) & BS_TYPEMASK;
    return st == BS_PUSHBUTTON || st == BS_DEFPUSHBUTTON;
}

/* 页签条也是自绘的（见 PaintTab）：按类名认出它 */
static BOOL TabIsTab(HWND h) {
    WCHAR cls[24];
    return GetClassNameW(h, cls, 24) > 0 && _wcsicmp(cls, L"SysTabControl32") == 0;
}

/* 页签条自绘：comctl 页签的默认样式还是 Win95 的浮凸小块，整个接管它的
   WM_PAINT —— 平底、一条浅灰分隔线贯穿脚下；选中页 = 强调色文本 +
   压在分隔线上的强调条（现代设置页的"下划线页签"）；悬停 = 淡蓝圆角底
   （与次级按钮悬停同色）。
   **行为不动**：点击、方向键、TCN_SELCHANGE 仍走控件默认过程 —— 点击判定
   用的 TCM_GETITEMRECT 与这里画的是同一组矩形，画得到的就是点得到的。 */
static void PaintTab(HWND h, HDC dc) {
    RECT rc;
    GetClientRect(h, &rc);
    {   /* 先铺底色（与父窗口/窗框同色，整条看起来是"平"的） */
        HBRUSH bg = CreateSolidBrush(GetSysColor(COLOR_BTNFACE));
        FillRect(dc, &rc, bg);
        DeleteObject(bg);
    }
    int n   = (int)SendMessageW(h, TCM_GETITEMCOUNT, 0, 0);
    int sel = (int)SendMessageW(h, TCM_GETCURSEL, 0, 0);
    int hot = (int)(INT_PTR)GetPropW(h, TABPROP_HOT) - 1;
    {   /* 分隔线：贯穿整条，所有页签都"站"在它上面 */
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(214, 214, 214));
        HGDIOBJ op = SelectObject(dc, pen);
        MoveToEx(dc, rc.left, rc.bottom - 1, NULL);
        LineTo(dc, rc.right, rc.bottom - 1);
        SelectObject(dc, op);
        DeleteObject(pen);
    }
    for (int i = 0; i < n; i++) {
        RECT tr;
        if (!TabCtrl_GetItemRect(h, i, &tr)) continue;
        WCHAR txt[64];
        TCITEMW ti;
        memset(&ti, 0, sizeof(ti));
        ti.mask = TCIF_TEXT;
        ti.pszText = txt;
        ti.cchTextMax = 64;
        SendMessageW(h, TCM_GETITEMW, (WPARAM)i, (LPARAM)&ti);

        if (i == sel) {
            /* 选中页：强调条从页签脚下长出来，贴住分隔线 */
            RECT br = { tr.left + S(6), rc.bottom - S(4),
                        tr.right - S(6), rc.bottom - 1 };
            HBRUSH ab = CreateSolidBrush(RGB(0, 120, 215));
            FillRect(dc, &br, ab);
            DeleteObject(ab);
        } else if (i == hot) {
            HBRUSH hb = CreateSolidBrush(RGB(229, 241, 251));
            HGDIOBJ ob = SelectObject(dc, hb);
            HGDIOBJ open = SelectObject(dc, GetStockObject(NULL_PEN));
            RoundRect(dc, tr.left + 1, tr.top + 1, tr.right - 1, tr.bottom - 1,
                      S(8), S(8));
            SelectObject(dc, open);
            SelectObject(dc, ob);
            DeleteObject(hb);
        }

        /* 选中页**不加粗**：粗体只留给段头；页签靠强调色 + 脚下那条下划线区分。
           好处是选中/未选中切换时文字宽度不跳，整条页签不会因为变粗而抖一下。 */
        HFONT f = (HFONT)SendMessageW(h, WM_GETFONT, 0, 0);
        if (!f) f = g_font;
        HGDIOBJ of = SelectObject(dc, f);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, i == sel ? RGB(0, 90, 158)
                                  : i == hot ? RGB(0, 0, 0) : RGB(84, 84, 84));
        DrawTextW(dc, txt, -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, of);
    }
}

/* 表单子控件的子类，干三件事：
   1. 接 Ctrl+S / Ctrl+回车 —— 焦点多半在某个子控件上，主窗口收不到按键；
   2. 把滚轮转给主窗口 —— 滚轮默认落在**焦点**控件头上，不转的话页面永远
      滚不动（列表框也转：名单很短，滚轮滚页面比滚列表更符合直觉）；
   3. 按钮全部自绘（见上"按钮的自绘"）。
   原过程存 GWLP_USERDATA：EDIT/BUTTON/LISTBOX 各有各的默认过程，
   不能共用一个全局变量。 */
static LRESULT CALLBACK ChildProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_KEYDOWN && (w == 'S' || w == VK_RETURN) &&
        (GetKeyState(VK_CONTROL) & 0x8000)) {
        DoSave();
        return 0;
    }
    if (m == WM_MOUSEWHEEL) {
        SendMessageW(g_hwnd, m, w, l);
        return 0;
    }
    if (TabIsTab(h)) {
        /* 页签：只接管外观相关的消息（见 PaintTab），其余落回默认过程 ——
           点击/键盘换页、TCN_SELCHANGE 全是控件自己干的。悬停项存窗口属性
           （GWLP_USERDATA 已被子类过程占用），变了就整条重画（面积很小）。 */
        switch (m) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            if (dc) PaintTab(h, dc);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;                    /* 整块自画，先擦一遍只会闪 */
        case WM_MOUSEMOVE: {
            TCHITTESTINFO hi;
            memset(&hi, 0, sizeof(hi));
            hi.pt.x = (SHORT)(short)LOWORD(l);
            hi.pt.y = (SHORT)(short)HIWORD(l);
            int idx = (int)SendMessageW(h, TCM_HITTEST, 0, (LPARAM)&hi);
            int cur = (int)(INT_PTR)GetPropW(h, TABPROP_HOT) - 1;
            if (idx != cur) {
                SetPropW(h, TABPROP_HOT, (HANDLE)(INT_PTR)(idx + 1));
                InvalidateRect(h, NULL, TRUE);
                UpdateWindow(h);
            }
            TrackBtnHover(h);            /* 注册/续期 LEAVE */
            break;
        }
        case WM_MOUSELEAVE:
            if (GetPropW(h, TABPROP_HOT)) {
                SetPropW(h, TABPROP_HOT, (HANDLE)(INT_PTR)0);
                InvalidateRect(h, NULL, TRUE);
            }
            break;
        default:
            break;
        }
    }
    if (BtnIsPush(h)) {
        switch (m) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            if (dc) PaintButton(h, dc);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;                    /* 整块区域自画，先擦一遍只会闪 */
        case WM_MOUSEMOVE:
            if (IsWindowEnabled(h) && !BtnFlag(h, BTNPROP_HOVER)) {
                SetBtnFlag(h, BTNPROP_HOVER, TRUE);
                TrackBtnHover(h);        /* 只在第一次进入时注册 LEAVE */
            }
            break;
        case WM_MOUSELEAVE:
            SetBtnFlag(h, BTNPROP_HOVER, FALSE);
            break;
        case WM_LBUTTONDOWN:
            SetBtnFlag(h, BTNPROP_PRESSED, TRUE);
            break;
        case WM_LBUTTONUP:
        case WM_CAPTURECHANGED:
            SetBtnFlag(h, BTNPROP_PRESSED, FALSE);
            break;
        case BM_SETSTATE:                /* 键盘空格触发：默认过程不发消息只置状态 */
            SetBtnFlag(h, BTNPROP_PRESSED, w != 0);
            break;
        case WM_ENABLE:                  /* 必须落到默认过程：lParam 是新状态 */
        case WM_SETFONT:                 /* wParam 是新字体，WM_GETFONT 要取得到 */
        case WM_SETFOCUS:
        case WM_KILLFOCUS:
            InvalidateRect(h, NULL, TRUE);
            break;
        default:
            break;
        }
    }
    return CallWindowProcW((WNDPROC)GetWindowLongPtrW(h, GWLP_USERDATA), h, m, w, l);
}
/* 这个窗口是不是某个名单列表框（滚轮判定用） */
static int ListIndexFromHwnd(HWND h) {
    if (!h) return -1;
    for (int i = 0; i < LIST_N; i++)
        if (h == Ctl(IDC_LIST0 + i * LIST_STRIDE)) return i;
    return -1;
}

/* 用滚轮滚列表框。
   为什么自己发 WM_VSCROLL 而不是把 WM_MOUSEWHEEL 原样转给列表框：所有子控件都
   被子类过程 ChildProc 接管，而它会把滚轮**转回主窗口**（那是以前"滚轮只滚整个
   表单"的做法）—— 原样转发就会转成死循环。
   步长跟随系统设置（SPI_GETWHEELSCROLLLINES）；用户把"每次滚多少行"设成 0
   表示不滚动，那就真的不滚。 */
static void ScrollListBy(HWND lb, int delta) {
    int notches = delta / WHEEL_DELTA;
    if (!notches) return;
    DWORD lines = 3;
    if (!SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0)) lines = 3;
    const BOOL page = (lines == WHEEL_PAGESCROLL);
    if (!page && lines == 0) return;                 /* 系统设置：不使用滚轮滚动 */
    int n = (int)(page ? 1 : lines) * (notches < 0 ? -notches : notches);
    const WPARAM cmd = (WPARAM)(page ? (notches > 0 ? SB_PAGEUP : SB_PAGEDOWN)
                                     : (notches > 0 ? SB_LINEUP : SB_LINEDOWN));
    for (int k = 0; k < n; k++) SendMessageW(lb, WM_VSCROLL, cmd, 0);
}

static void HookChild(HWND h) {
    if (h) SetWindowLongPtrW(h, GWLP_USERDATA,
                             SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR)ChildProc));
}

/* 把一条进程名加进第 i 组名单：去首尾空白、去 .exe 后缀、去重。
   返回是否真的加了（重名/空名不算）。名单里存的是"不带后缀"的形态
   （匹配时两边都去），界面显示的样子就该是**真正存进去的样子**。 */
static BOOL AddListEntry(int i, const WCHAR* raw) {
    int base = IDC_LIST0 + i * LIST_STRIDE;
    HWND lb = Ctl(base);
    WCHAR trim[CFG_NAME_MAX];
    _snwprintf_s(trim, CFG_NAME_MAX, _TRUNCATE, L"%s", raw);

    /* 去首尾空白：界面里粘进来的名字常带空格，而名单匹配是**完全匹配** */
    size_t l = wcslen(trim);
    while (l > 0 && (trim[l - 1] == L' ' || trim[l - 1] == L'\t')) trim[--l] = 0;
    const WCHAR* lead = trim;
    while (*lead == L' ' || *lead == L'\t') lead++;

    WCHAR buf[CFG_NAME_MAX];
    _snwprintf_s(buf, CFG_NAME_MAX, _TRUNCATE, L"%s", lead);
    l = wcslen(buf);
    if (l >= 4 && _wcsicmp(buf + l - 4, L".exe") == 0) buf[l - 4] = 0;
    if (!buf[0]) return FALSE;

    /* 去重：同样的名字出现两次没有意义（完全匹配，重复项纯噪音） */
    if (SendMessageW(lb, LB_FINDSTRINGEXACT, (WPARAM)-1, (LPARAM)buf) != LB_ERR)
        return FALSE;
    SendMessageW(lb, LB_ADDSTRING, 0, (LPARAM)buf);
    return TRUE;
}

static void AddName(int i) {
    int base = IDC_LIST0 + i * LIST_STRIDE;
    HWND ed = Ctl(base + 1);
    WCHAR raw[CFG_NAME_MAX] = L"";
    GetWindowTextW(ed, raw, CFG_NAME_MAX);
    if (AddListEntry(i, raw)) UpdateDirty();
    SetWindowTextW(ed, L"");
    SetFocus(ed);
}

static void DelName(int i) {
    int base = IDC_LIST0 + i * LIST_STRIDE;
    HWND lb = Ctl(base);
    int sel = (int)SendMessageW(lb, LB_GETCURSEL, 0, 0);
    if (sel == LB_ERR) return;
    SendMessageW(lb, LB_DELETESTRING, (WPARAM)sel, 0);
    int n = (int)SendMessageW(lb, LB_GETCOUNT, 0, 0);
    SendMessageW(lb, LB_SETCURSEL, (WPARAM)((sel < n) ? sel : n - 1), 0);
    UpdateDirty();
}

/* ---------------- 选窗添加 ----------------
   Spy++ / AutoIt "Finder Tool" 的标准做法（微软 KB Q143045）：点"选窗添加"
   后 SetCapture 把鼠标输入全收归本窗口 —— 目标窗口收不到这次点击、也不会被
   切到前台；移动时 WindowFromPoint + GetAncestor(GA_ROOT) 找光标下的**顶层**
   窗口（名单匹配的是进程，子控件没有自己的进程），用 PatBlt(DSTINVERT) 在
   屏幕上给它描一圈反色边（反色再画一遍 = 擦除，不需要保存位图）；单击就把
   该窗口的进程名（OpenProcess + QueryFullProcessImageNameW）加进名单。
   Esc 或右键取消。 */

static int  g_pickList = -1;      /* 正在为哪份名单选窗（-1 = 没在选窗） */
static HWND g_pickHwnd = NULL;    /* 当前高亮的顶层窗口 */

#ifndef PROCESS_QUERY_LIMITED_INFORMATION
#define PROCESS_QUERY_LIMITED_INFORMATION 0x1000
#endif
typedef BOOL (WINAPI* PFN_QueryFullProcessImageNameW)(HANDLE, DWORD, LPWSTR, PDWORD);

/* 选中窗口的进程名（纯文件名，不含路径）。失败多半是权限不够。 */
static BOOL PickProcName(HWND hwnd, WCHAR* out, int cch) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid) return FALSE;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return FALSE;
    WCHAR path[1024];
    DWORD sz = 1024;
    BOOL ok = FALSE;
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    PFN_QueryFullProcessImageNameW q =
        k32 ? (PFN_QueryFullProcessImageNameW)GetProcAddress(k32,
                                                       "QueryFullProcessImageNameW")
            : NULL;
    if (q && q(h, 0, path, &sz)) {
        const WCHAR* f = wcsrchr(path, L'\\');
        _snwprintf_s(out, cch, _TRUNCATE, L"%s", f ? f + 1 : path);
        ok = TRUE;
    }
    CloseHandle(h);
    return ok;
}

/* 在屏幕上把窗口描一圈反色边；对同一个窗口调两次正好抵消。 */
static void PickFrame(HWND hwnd) {
    RECT r;
    if (!hwnd || !GetWindowRect(hwnd, &r)) return;
    HDC dc = GetDC(NULL);
    if (!dc) return;
    int th = S(3), ww = r.right - r.left, wh = r.bottom - r.top;
    PatBlt(dc, r.left, r.top, ww, th, DSTINVERT);
    PatBlt(dc, r.left, r.bottom - th, ww, th, DSTINVERT);
    PatBlt(dc, r.left, r.top, th, wh, DSTINVERT);
    PatBlt(dc, r.right - th, r.top, th, wh, DSTINVERT);
    ReleaseDC(NULL, dc);
}

static HWND PickHit(void) {
    POINT pt;
    if (!GetCursorPos(&pt)) return NULL;
    HWND hit = WindowFromPoint(pt);
    return hit ? GetAncestor(hit, GA_ROOT) : NULL;
}

/* 跟随光标更新高亮：目标变了就"旧框再画一遍（=擦）+ 新框画一遍"。
   设置窗口自己不算数 —— 把自己加进名单没有意义，悬停它时不描框。 */
static void PickTrack(void) {
    HWND root = PickHit();
    if (root == g_hwnd) root = NULL;
    if (root == g_pickHwnd) return;
    PickFrame(g_pickHwnd);
    g_pickHwnd = root;
    PickFrame(g_pickHwnd);
}

/* 退出选窗态：擦掉高亮框、还鼠标、释放捕获。 */
static void PickEnd(void) {
    if (g_pickList < 0) return;
    PickFrame(g_pickHwnd);
    g_pickHwnd = NULL;
    g_pickList = -1;
    ReleaseCapture();
    SetCursor(LoadCursorW(NULL, (LPCWSTR)(ULONG_PTR)IDC_ARROW));
    SetStatus(L"");
}

static void PickBegin(int i) {
    PickEnd();                       /* 上一次没结束就又点了一次：先收尾 */
    g_pickList = i;
    SetCapture(g_hwnd);
    SetCursor(LoadCursorW(NULL, (LPCWSTR)(ULONG_PTR)IDC_CROSS));
    SetStatus(kLists[i].pickKind == PICK_CLASS
                  ? L"选窗中：点哪个窗口就取它的窗口类填进输入框（Esc/右键取消）"
                  : L"选窗中：点哪个窗口就取它的进程名填进输入框（Esc/右键取消）");
    PickTrack();
}

/* 单击落定：取光标下窗口的进程名加进名单。必须在**抬起**时收尾 ——
   若在按下时释放捕获，配对的抬起会漏给目标窗口，等于替用户在那里面点了一下。 */
static void PickFinish(void) {
    int i = g_pickList;
    if (i < 0) return;
    HWND root = PickHit();
    if (root == g_hwnd) {
        /* 点到设置窗口自己：不算数，也**不退出选窗**，提示用户去点别的窗口 */
        SetStatus(L"这是设置窗口自己，不算 —— 请点击其它程序的窗口（Esc 取消）");
        return;
    }
    PickEnd();
    if (!root) { SetStatus(L"选窗失败：这里没有可取的窗口"); return; }

    /* 取什么由名单决定：进程名单取进程名，窗口类名单取类名（原样，无前缀）。 */
    WCHAR name[CFG_NAME_MAX] = L"";
    if (kLists[i].pickKind == PICK_CLASS) {
        WCHAR cls[128] = L"";
        if (!GetClassNameW(root, cls, 128) || !cls[0]) {
            SetStatus(L"选窗失败：拿不到该窗口的类名");
            return;
        }
        _snwprintf_s(name, CFG_NAME_MAX, _TRUNCATE, L"%s", cls);
    } else if (!PickProcName(root, name, CFG_NAME_MAX)) {
        SetStatus(L"选窗失败：拿不到该窗口的进程名（权限不足？）");
        return;
    }

    /* ★ 只**填进输入框**，不直接加进名单 —— 由用户确认后点"添加"。
       这样能先看清取到的是什么、也能顺手改一改（比如只留进程名的主体）。 */
    {
        HWND ed = Ctl(IDC_LIST0 + i * LIST_STRIDE + 1);
        SetWindowTextW(ed, name);
        SetFocus(ed);
        SendMessageW(ed, EM_SETSEL, 0, -1);     /* 全选：直接打字就能覆盖 */
        SetStatus(L"已取到 %s —— 确认后点「添加」才会加进名单", name);
    }
}

/* 「选择字体…」（窗口信息弹窗的字体与字号）。
   走系统的 ChooseFont —— 不自己造字体列表：枚举、预览、字形与脚本的过滤、
   非西文名的本地化全归系统管，与 Notepad3 用的是同一个对话框。
   结果只**填进上面两行**（字体名 / 字号），由用户点保存才落盘 —— 与名单页
   "选窗只把名字填进输入框"同一套礼貌：看得见、改得动、可反悔。
   CF_NOVERTFONTS：@ 开头的竖排字体对横排正文没有意义，列出来只会让人选错。 */
static void PickInfoFont(void) {
    HWND edFace = Ctl(IDC_INFO_FONT);
    HWND edSize = Ctl(IDC_INFO_FSIZE);
    if (!edFace || !edSize) return;

    WCHAR face[LF_FACESIZE] = L"";
    GetWindowTextW(edFace, face, LF_FACESIZE);
    if (!face[0]) _snwprintf_s(face, LF_FACESIZE, _TRUNCATE, L"%s", L"Consolas");

    WCHAR sz[32] = L"";
    GetWindowTextW(edSize, sz, 32);
    int pt = _wtoi(sz);
    if (pt < INFO_FONT_SIZE_MIN || pt > INFO_FONT_SIZE_MAX) pt = INFO_FONT_SIZE_DEFAULT;

    LOGFONTW lf;
    memset(&lf, 0, sizeof(lf));
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfHeight  = -MulDiv(pt, g_dpi, 72);      /* 磅 -> 逻辑高度（与弹窗同一算法） */
    _snwprintf_s(lf.lfFaceName, LF_FACESIZE, _TRUNCATE, L"%s", face);

    CHOOSEFONTW cf;
    memset(&cf, 0, sizeof(cf));
    cf.lStructSize = sizeof(cf);
    cf.hwndOwner   = g_hwnd;
    cf.lpLogFont   = &lf;
    cf.Flags       = CF_SCREENFONTS | CF_INITTOLOGFONTSTRUCT | CF_FORCEFONTEXIST |
                     CF_NOVERTFONTS | CF_LIMITSIZE;
    cf.nSizeMin    = INFO_FONT_SIZE_MIN;
    cf.nSizeMax    = INFO_FONT_SIZE_MAX;
    if (!ChooseFontW(&cf)) return;

    SetWindowTextW(edFace, lf.lfFaceName);
    _snwprintf_s(sz, 32, _TRUNCATE, L"%d", cf.iPointSize / 10);   /* iPointSize 是 1/10 磅 */
    SetWindowTextW(edSize, sz);
    SetStatus(L"已选 %s %d 磅 —— 保存后生效", lf.lfFaceName, cf.iPointSize / 10);
    UpdateDirty();
}

/* 「选择字体…」（超级复制预览浮层）。与 PickInfoFont 同一套做法，只差两点：
     · 只回填**字体名**，没有字号行 —— 浮层的字号是固定的（标题 9 磅 / 正文
       10 磅），一个只活一两秒的提示不需要字号旋钮；
     · 因此不做 CF_LIMITSIZE，选中的磅数直接丢弃。 */
static void PickCopyFont(void) {
    HWND edFace = Ctl(IDC_COPY_FONT);
    if (!edFace) return;

    WCHAR face[LF_FACESIZE] = L"";
    GetWindowTextW(edFace, face, LF_FACESIZE);
    if (!face[0]) _snwprintf_s(face, LF_FACESIZE, _TRUNCATE, L"%s", L"Microsoft YaHei UI");

    LOGFONTW lf;
    memset(&lf, 0, sizeof(lf));
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfHeight  = -MulDiv(10, g_dpi, 72);   /* 与 toast.c 的正文字号同一算法 */
    _snwprintf_s(lf.lfFaceName, LF_FACESIZE, _TRUNCATE, L"%s", face);

    CHOOSEFONTW cf;
    memset(&cf, 0, sizeof(cf));
    cf.lStructSize = sizeof(cf);
    cf.hwndOwner   = g_hwnd;
    cf.lpLogFont   = &lf;
    cf.Flags       = CF_SCREENFONTS | CF_INITTOLOGFONTSTRUCT | CF_FORCEFONTEXIST |
                     CF_NOVERTFONTS;
    if (!ChooseFontW(&cf)) return;

    SetWindowTextW(edFace, lf.lfFaceName);
    SetStatus(L"已选 %s —— 保存后生效", lf.lfFaceName);
    UpdateDirty();
}


/* "窗框"（顶部与底部共用的实底子窗口类）：状态栏/按钮/页签条放在它上面。
   为什么要它：表单内容滚动时，滑出可视区的部分不会被自动裁掉 —— 底部会从
   状态栏/按钮周围的缝隙里露出来，顶部则直接盖到页签条上；窗框在 z 序上
   压住它们，就挡住了。
   带 WS_EX_CONTROLPARENT，Tab 焦点能进到它里面的按钮/页签；
   子控件的通知（按钮点击、页签的 WM_NOTIFY）原样转给主窗口，处理逻辑不分家。 */
static LRESULT CALLBACK FooterProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_COMMAND || m == WM_NOTIFY) {
        SendMessageW(g_hwnd, m, w, l);
        return 0;
    }
    /* 窗框上的空白（状态栏两侧、页签条右边）也要能滚表单：与子控件那一份
       同一去向，处理逻辑只在主窗口里有一份。 */
    if (m == WM_MOUSEWHEEL) {
        SendMessageW(g_hwnd, m, w, l);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

/* 承载窗（表单内容容器）的过程。它本身不画东西，只做两件事：
    ① 把子控件的通知转给主窗口 —— 按钮的 WM_COMMAND、编辑框的 EN_CHANGE、
       列表框的 LBN_* 都是发给**父窗口**的，而它们的父现在是承载窗；
    ② 把落在承载窗空白处（不在任何子控件上）的滚轮转给主窗口。
   ★ 承载窗**不许**自己滚：滚动只由主窗口的 WM_MOUSEWHEEL / WM_VSCROLL 驱动、
     只改 g_target。一旦在这里就地重排，滚轮路径就又变长了 —— 那正是
     "滚轮漏给背后程序"那个 bug 的成因（见文件头"滚动"一节）。 */
static LRESULT CALLBACK FormProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_COMMAND:
    case WM_NOTIFY:
    case WM_MOUSEWHEEL:
        SendMessageW(g_hwnd, m, w, l);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_COMMAND: {
        int id = LOWORD(w);
        if (id == IDC_SAVEONLY) {
            /* 只保存不重启：改完几个数值想攒着一起生效，或者本体压根没在跑
               （下次启动自然读到新配置）时用这个。 */
            SaveConfig();
            return 0;
        }
        if (id == IDC_SAVE)   { DoSave(); return 0; }
        if (id == IDC_EXIT)   { CloseNoSave(); return 0; }
        if (id == IDC_RELOAD) {
            if (g_dirty && MessageBoxW(h, L"放弃未保存的改动并重新加载？", APP_TITLE,
                                       MB_ICONQUESTION | MB_YESNO) != IDYES) return 0;
            LoadConfig();
            return 0;
        }
        if (id == IDC_INFO_FONTBTN) { PickInfoFont(); return 0; }
        if (id == IDC_COPY_FONTBTN) { PickCopyFont(); return 0; }
        /* 「常用」字段编排（窗口信息弹窗的两个视图之一） */
        if (id == IDC_CMN_ADD)  { CmnAddDlg(); return 0; }
        if (id == IDC_CMN_DEL)  { CmnDel(); return 0; }
        if (id == IDC_CMN_UP)   { CmnMove(-1); return 0; }
        if (id == IDC_CMN_DOWN) { CmnMove(+1); return 0; }
        if (id == IDC_CMN_RESET) { CmnReset(); return 0; }
        if (id == IDC_CMN_LIST) {          /* 选中项变了：按钮可用性跟着变 */
            if (HIWORD(w) == LBN_SELCHANGE) CmnSyncButtons();
            return 0;
        }
        if (id >= IDC_LIST0 && id < IDC_LIST0 + LIST_N * LIST_STRIDE) {
            int i = (id - IDC_LIST0) / LIST_STRIDE;
            int sub = (id - IDC_LIST0) % LIST_STRIDE;
            if (sub == 2) { AddName(i); return 0; }
            if (sub == 3) { DelName(i); return 0; }
            if (sub == 5) { PickBegin(i); return 0; }
            if (sub == 0 && HIWORD(w) == LBN_DBLCLK) { DelName(i); return 0; }
            return 0;
        }
        /* 勾选框 / 输入框：任何改动都算"有未保存的改动" */
        if (HIWORD(w) == BN_CLICKED || HIWORD(w) == EN_CHANGE) UpdateDirty();
        return 0;
    }
    case WM_NOTIFY: {
        /* 页签切换（TCN_SELCHANGE 在选完之后发，取当前页即可）：改 g_curTab、
           偏移归零，再走一趟 Layout —— 换页后内容总高变了，旧偏移没有意义；
           显隐轮与全量重排都在 Layout 里。归零走"不播动画"的那条路。 */
        NMHDR* nm = (NMHDR*)l;
        if (nm->idFrom == IDC_TAB && nm->code == TCN_SELCHANGE) {
            int t = (int)SendMessageW(g_tab, TCM_GETCURSEL, 0, 0);
            if (t >= 0 && t < TAB_N) g_curTab = t;
            /* 先把页签条的新选中态**立刻**画出来，再干下面那些重活：换页要做
               冻结、整页重排、整帧重画（几十毫秒）；如果等高亮一起排到最后，
               用户看到的就是"点了没反应，过一会儿下划线才跳过去"。这个
               UpdateWindow 只重画页签条本身，代价极小。 */
            if (g_tab) { InvalidateRect(g_tab, NULL, FALSE); UpdateWindow(g_tab); }
            /* 换页：整段在"关重绘"里完成，最后一次性画完。
               为什么不能边藏边露：旧页控件一个个 ShowWindow(SW_HIDE)，每次都会
               让承载窗 / 主窗露出一块背景并被擦成灰底；240Hz 下这些中间状态会被
               合成器采样到 —— 用户看到的就是"闪一下"。关掉重绘后中间状态一个都
               不上屏，最后按下面顺序只画一次：
                 ① 承载窗空白处（控件之间的缝）铺底色；
                 ② 子控件各自重画（**不擦** —— 避免出现"先全灰、再出字"的一帧）；
                 ③ 主窗口自己的空白（承载窗铺满视口，通常什么也没有）。
               这是**用户点击**路径，不是滚轮路径 —— 这里同步重画是安全的
               （滚轮路径才绝不能同步重画，见文件头"滚动"一节）。 */
            SendMessageW(h, WM_SETREDRAW, FALSE, 0);
            if (g_form) SendMessageW(g_form, WM_SETREDRAW, FALSE, 0);
            AnimStop();
            g_off = g_target = 0;
            g_wheelRem = 0;
            {
                RECT rc;
                GetClientRect(h, &rc);
                Layout(rc.right, rc.bottom);
            }
            if (g_form) SendMessageW(g_form, WM_SETREDRAW, TRUE, 0);
            SendMessageW(h, WM_SETREDRAW, TRUE, 0);
            if (g_form) {
                /* ★ 这里必须是 RDW_ALLCHILDREN。只擦承载窗自己的空白是不够的：
                   WS_CLIPCHILDREN 会把**已经隐藏**的子控件矩形也一起裁掉，
                   父窗的擦除永远够不到"旧控件让出来的那块像素"（实测：外部
                   强制 InvalidateRect+UpdateWindow 之后残影依旧，而加上
                   ALLCHILDREN 的那次重画立刻干净）。代价是这一帧要把新页的
                   几十个控件同步画一遍 —— 换页是用户点击，几十毫秒可以接受；
                   页签高亮在上面已经先画出来了，不会跟着一起等。 */
                RedrawWindow(g_form, NULL, NULL,
                             RDW_INVALIDATE | RDW_ERASE |
                             RDW_ALLCHILDREN | RDW_UPDATENOW);
            }
            RedrawWindow(h, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
            return 0;
        }
        break;
    }
    case WM_MOUSEMOVE:
        if (g_pickList >= 0) {          /* 捕获生效期间所有移动都归我们 */
            PickTrack();
            SetCursor(LoadCursorW(NULL, (LPCWSTR)(ULONG_PTR)IDC_CROSS));
            return 0;
        }
        break;
    case WM_LBUTTONDOWN:
        if (g_pickList >= 0) return 0;  /* 按下不做事，等抬起（见 PickFinish） */
        break;
    case WM_LBUTTONUP:
        if (g_pickList >= 0) { PickFinish(); return 0; }
        break;
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_MBUTTONUP:
        if (g_pickList >= 0) { PickEnd(); return 0; }   /* 右键/中键 = 取消 */
        break;
    case WM_CAPTURECHANGED:
        /* 捕获被别人抢走（正常流程不会走到）：也得收掉选窗态，别让高亮框留在屏上。
           自己 PickEnd 里的 ReleaseCapture 也会发这条，那时 g_pickList 已复位，自然跳过。 */
        if (g_pickList >= 0 && (HWND)l != g_hwnd) {
            PickFrame(g_pickHwnd);
            g_pickHwnd = NULL;
            g_pickList = -1;
            SetStatus(L"选窗已取消");
        }
        return 0;
    case WM_ERASEBKGND: {
        /* 主窗口只负责**承载窗够不着的地方**：内容比可视区矮时的那一条、以及
           滚动时承载窗让出来的那条。子控件区域被 WS_CLIPCHILDREN 排除，
           这里不可能擦到表单内容上（擦到就是闪烁）。 */
        RECT rc;
        GetClientRect(h, &rc);
        FillRect((HDC)w, &rc, GetSysColorBrush(COLOR_BTNFACE));
        return 1;
    }
    case WM_PAINT:
        /* 滚动条可能被外部（读屏 / UIA 的 Scroll 模式）直接拨动而不经过我们的
           WM_VSCROLL：画之前对一下账。以滚动条为准**直接对齐** —— 谁来拨谁就该
           立刻看到结果，外部拨动不补动画。 */
        if (g_hwnd && !g_anim) {
            int sp = ScrollPos();
            if (sp != g_off) ScrollTo(sp, FALSE);
        }
        ValidateRect(h, NULL);
        return DefWindowProcW(h, m, w, l);
    case WM_SCROLL_TICK:
        InterlockedExchange(&s_tickPending, 0);   /* 这一帧开始处理：允许投下一帧 */
        AnimTick();
        return 0;
    case WM_VSCROLL: {
        /* 行/页以**目标值**为基准（不是当前偏移）：连着点时命令要累加，
           否则动画还没走完的第二次点击会被"当前偏移"吃掉一半。 */
        switch (LOWORD(w)) {
        case SB_LINEUP:        ScrollTo(g_target - S(30), TRUE); break;
        case SB_LINEDOWN:      ScrollTo(g_target + S(30), TRUE); break;
        case SB_PAGEUP:        ScrollTo(g_target - (g_viewH - S(40)), TRUE); break;
        case SB_PAGEDOWN:      ScrollTo(g_target + (g_viewH - S(40)), TRUE); break;
        case SB_TOP:           ScrollTo(0, TRUE); break;
        case SB_BOTTOM:        ScrollTo(MaxScroll(), TRUE); break;
        case SB_THUMBTRACK:    /* 拖拽 = 1:1 跟手，绝不缓动 */
        case SB_THUMBPOSITION: ScrollTo((int)HIWORD(w), FALSE); break;
        default:               return 0;    /* SB_ENDSCROLL 等 */
        }
        return 0;
    }
    case WM_MOUSEWHEEL: {
        /* 子控件（ChildProc）、承载窗（FormProc）、窗框（FooterProc）与页签都把
           滚轮转到这里 —— 只有这一处做决定：
             ① 选窗态下滚轮无意义 -> 吞掉；
             ② 光标停在**列表框**上（五份名单 + 「窗口信息」页的「常用」字段）
                -> 滚那个列表框。列表框比整页短，滚列表比滚整页更符合直觉；
                而且「窗口信息」页本身没有可滚空间，不同样处理的话就是"滚不动"；
             ③ 否则滚整个表单。
           ★ 返回 0 = 已消费。**不要**在这里重排 / 同步重画：处理得慢，系统就会
             把后续滚轮回落给焦点窗口（背后的程序跟着滚）—— 这条是踩过的坑。 */
        if (g_pickList >= 0) return 0;
        POINT pt;
        pt.x = (LONG)(short)LOWORD(l);
        pt.y = (LONG)(short)HIWORD(l);
        HWND over = WindowFromPoint(pt);
        if (ListIndexFromHwnd(over) >= 0 || over == Ctl(IDC_CMN_LIST)) {
            ScrollListBy(over, (int)(short)HIWORD(w));
            return 0;
        }
        WheelScroll((int)(short)HIWORD(w));
        return 0;
    }
    case WM_DPICHANGED:
        /* 拖到另一块不同缩放的显示器上：DPI 变了 -> 字体与所有尺寸都要重算，
           并按系统给的建议矩形调整窗口（否则窗口大小不变、内容挤在一起）。
           顺带把帧间隔作废重算 —— 新屏的刷新率可能与旧屏不同（60 ↔ 240）。 */
        g_dpi = (int)HIWORD(w);
        if (g_dpi < 96) g_dpi = 96;
        g_tickMs = 0;
        MakeFont();
        ApplyFont();
        if (l) {
            RECT* r = (RECT*)l;
            SetWindowPos(h, NULL, r->left, r->top,
                         r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return 0;
    case WM_MOVE:
        /* 换显示器 = 刷新率可能变（60Hz 副屏 ↔ 240Hz 主屏）：帧间隔作废重算 */
        g_tickMs = 0;
        break;
    case WM_SIZE:
        Layout(LOWORD(l), HIWORD(l));
        return 0;
    case WM_CLOSE:
        CloseNoSave();
        return 0;
    case WM_DESTROY:
        /* 帧线程是常驻的（平时阻塞在 s_animGo 上）：窗口没了必须叫醒它退出，
           否则它会继续往一个已销毁的窗口 PostMessage。 */
        AnimStop();
        s_animQuit = TRUE;
        if (s_animGo) SetEvent(s_animGo);
        if (s_animThread) {
            WaitForSingleObject(s_animThread, 300);
            CloseHandle(s_animThread);
            s_animThread = NULL;
        }
        if (s_animGo) { CloseHandle(s_animGo); s_animGo = NULL; }
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(h, m, w, l);
    }
    /* 选窗态下"不归我们管"的消息（g_pickList < 0）从上面的 break 落到这里 */
    return DefWindowProcW(h, m, w, l);
}

static HWND MakeBtn(HINSTANCE hi, HWND parent, const WCHAR* text, int id) {
    HWND b = CreateWindowExW(0, L"BUTTON", text,
                             WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | BS_PUSHBUTTON | WS_TABSTOP,
                             0, 0, 10, 10, parent, (HMENU)(INT_PTR)id, hi, NULL);
    /* "保存并重启"是主操作 -> 自绘时上强调蓝；要在第一次绘制前设好 */
    if (b && id == IDC_SAVE)
        SetPropW(b, BTNPROP_PRIMARY, (HANDLE)(INT_PTR)1);
    HookChild(b);        /* 按钮也转发滚轮 / 接 Ctrl+S（焦点常停在按钮上） */
    return b;
}
static HWND MakeLabel(HINSTANCE hi, HWND parent, const WCHAR* text, int id) {
    return CreateWindowExW(0, L"STATIC", text,
                           WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | SS_LEFTNOWORDWRAP,
                           0, 0, 10, 10, parent, (HMENU)(INT_PTR)id, hi, NULL);
}

/* 形参写成与 SDK 声明一致的 SAL 批注：不加的话 /analyze 会报 C28251
   （"WinMain 的批注不一致"）。纯噪音，但要保持 lint 零告警就得加。 */
int WINAPI WinMain(_In_ HINSTANCE hInst, _In_opt_ HINSTANCE hPrev,
                   _In_ LPSTR cmd, _In_ int show) {
    (void)hPrev; (void)cmd; (void)show;

    /* 单实例：已经开着就把那个窗口叫到前面 —— 两个设置窗口同时改同一个 ini
       只会互相覆盖（后保存的赢），用户却看不出为什么改动丢了。 */
    {
        HWND prev = FindWindowW(WND_CLASS, NULL);
        if (prev) {
            /* 隐藏/最小化都要先显出来再提前台 —— 否则"再点一次设置"只是
               对着一扇看不见的窗口喊话。 */
            if (IsIconic(prev))       ShowWindow(prev, SW_RESTORE);
            else if (!IsWindowVisible(prev)) ShowWindow(prev, SW_SHOWNORMAL);
            BringWindowToTop(prev);
            SetForegroundWindow(prev);
            return 0;
        }
    }

    InitDpiAwareness();      /* 必须在任何窗口创建之前 */

    InitFieldRanges();       /* 值域从 config.c 的表灌进来（建控件前） */
    InitGroups();            /* 段头映射：纯表数据，建控件前算好 */

    /* 页签控件是 comctl32 的通用控件：先注册它的窗口类，CreateWindow 找得到。
       拿不到（极老系统）也只退回"没有页签的单页滚动"，其余功能照常。 */
    {
        INITCOMMONCONTROLSEX icc;
        memset(&icc, 0, sizeof(icc));
        icc.dwSize = sizeof(icc);
        icc.dwICC = ICC_TAB_CLASSES;
        InitCommonControlsEx(&icc);
    }

    /* 配置文件的路径 = **本 exe 同目录**，不是当前工作目录：
       用户可能从任意地方（快捷方式 / 资源管理器）启动它。 */
    {
        WCHAR exe[MAX_PATH] = L"";
        if (!GetModuleFileNameW(NULL, exe, MAX_PATH)) return 1;
        WCHAR* slash = wcsrchr(exe, L'\\');
        if (slash) *(slash + 1) = 0;
        _snwprintf_s(g_dir, MAX_PATH, _TRUNCATE, L"%s", exe);
        _snwprintf_s(g_path, MAX_PATH, _TRUNCATE, L"%s%s", g_dir, INI_NAME);
    }

    WNDCLASSW wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    /* IDC_ARROW 是 MAKEINTRESOURCE(...)（窄字符串常量），这里调的是 W 版，显式转一步 */
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)(ULONG_PTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = WND_CLASS;
    if (!RegisterClassW(&wc)) return 1;

    /* 底部"窗框"类：实底 + 无边框，见 FooterProc 处的说明 */
    WNDCLASSW fc = wc;
    fc.lpfnWndProc = FooterProc;
    fc.lpszClassName = FOOTER_CLASS;
    if (!RegisterClassW(&fc)) return 1;

    /* 承载窗类：表单内容的容器。滚动只挪它一个窗口（见"滚动"一节与 FormProc） */
    WNDCLASSW mc = wc;
    mc.lpfnWndProc = FormProc;
    mc.lpszClassName = FORM_CLASS;
    if (!RegisterClassW(&mc)) return 1;

    /* DPI 要在**建窗之前**知道：窗口尺寸本身就得按它算（否则 200% 屏上
       窗口只有一半大）。QueryDpi 在没有 hwnd 时退回查屏幕 DC。 */
    g_dpi = (int)QueryDpi();
    if (g_dpi < 96) g_dpi = 96;

    /* 初始大小按 DPI 缩放，并**钳到工作区以内**：高 DPI 笔记本（150% + 768 高）
       上按 96 DPI 算出来的窗口会超出屏幕底部，底部的按钮与状态栏
       就永远看不见了。 */
    /* 高度按**名单页**给：五份名单竖排约 740 逻辑像素，加上页签条与底部窗框
       一屏放得下（切过去不用滚）；常规页的配置项更矮，天然富余。 */
    /* 宽度贴着内容走：最宽的是名单区标题句（约 490）和"输入框 + 单位 +
       说明标签"一行（约 500），再留滚动条与边框的余量 —— 再宽的话
       名单框右侧就是一大块用不上的空白 */
    int winW = S(560), winH = S(880);
    int winX = CW_USEDEFAULT, winY = CW_USEDEFAULT;
    {
        RECT wa;
        if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0)) {
            int aw = wa.right - wa.left - S(40), ah = wa.bottom - wa.top - S(60);
            if (aw > S(480) && winW > aw) winW = aw;
            if (ah > S(360) && winH > ah) winH = ah;
            /* **工作区**居中（不含任务栏）：CW_USEDEFAULT 是级联摆放，
               窗口会落在屏幕左上角 —— 这么大一张表单贴着边放很难受。
               钳位防的是"窗口比工作区还大"时算出负坐标。 */
            winX = wa.left + ((wa.right - wa.left) - winW) / 2;
            winY = wa.top + ((wa.bottom - wa.top) - winH) / 2;
            if (winX < wa.left) winX = wa.left;
            if (winY < wa.top) winY = wa.top;
        }
    }

    g_hwnd = CreateWindowExW(0, WND_CLASS, APP_TITLE,
                             WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_VSCROLL,
                             winX, winY, winW, winH,
                             NULL, NULL, hInst, NULL);
    if (!g_hwnd) return 1;

    /* 窗口已存在：再问一次 Per-Monitor 的精确值（这块屏可能是 144 而主屏是 96） */
    g_dpi = (int)QueryDpi();
    if (g_dpi < 96) g_dpi = 96;
    MakeFont();

    /* 顶部"窗框"先建：一块实底托盘，页签条住在里面，z 序压住滚上来的内容
       （与底部窗框同一思路，落位与钉顶都在 Layout）。 */
    g_header = CreateWindowExW(WS_EX_CONTROLPARENT, FOOTER_CLASS, L"",
                               WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
                               0, 0, 10, 10, g_hwnd, (HMENU)(INT_PTR)IDC_HEADER, hInst, NULL);
    /* 页签条：只决定"显示哪些控件"（显隐轮在 Layout）。控件本体住在下面的
       承载窗里，Load / Save / WM_COMMAND 全按 id 找，不分页。
       换页的 WM_NOTIFY 由窗框原样转给主窗口（见 FooterProc）。 */
    g_tab = CreateWindowExW(0, WC_TABCONTROLW, L"",
                            WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_TABSTOP,
                            0, 0, 10, 10, g_header, (HMENU)(INT_PTR)IDC_TAB, hInst, NULL);
    if (g_tab) {
        for (int i = 0; i < TAB_N; i++) {
            TCITEMW ti;
            memset(&ti, 0, sizeof(ti));
            ti.mask = TCIF_TEXT;
            ti.pszText = (LPWSTR)kTabNames[i];   /* 字符串常量，控件只读 */
            SendMessageW(g_tab, TCM_INSERTITEMW, (WPARAM)i, (LPARAM)&ti);
        }
        HookChild(g_tab);    /* 滚轮转发 / Ctrl+S：与其它子控件同一待遇 */
    }

    /* 承载窗：**全部表单控件**的父窗口。它被当作一个整体上下移动 —— 这是
       "滚动不跳、也不把滚轮漏给背后程序"的结构前提（见"滚动"一节）。
       WS_EX_CONTROLPARENT 让 IsDialogMessage 的 Tab 焦点能进到它的子控件；
       WS_CLIPCHILDREN 保证它擦自己底色时不会覆盖子控件（反过来也一样）。 */
    g_form = CreateWindowExW(WS_EX_CONTROLPARENT, FORM_CLASS, L"",
                             WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
                             0, 0, 10, 10, g_hwnd, (HMENU)(INT_PTR)IDC_FORM, hInst, NULL);
    if (!g_form) return 1;

    for (int i = 0; i < FIELD_N; i++) {
        int id = kFields[i].id;
        if (g_grpHead[i])                 /* 段头：普通 STATIC，字体走 FontApplyCb 的段头分支 */
            MakeLabel(hInst, g_form, kFields[i].group, g_grpHead[i]);
        if (kFields[i].isBtn) {
            /* 按钮自己就是一行：它不承载配置值，点击行为在 WM_COMMAND 里按 id 分支 */
            HWND b = CreateWindowExW(0, L"BUTTON", kFields[i].label,
                                     WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_TABSTOP,
                                     0, 0, 10, 10, g_form, (HMENU)(INT_PTR)id, hInst, NULL);
            HookChild(b);
        } else if (kFields[i].isBool) {
            HWND b = CreateWindowExW(0, L"BUTTON", kFields[i].label,
                                     WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | BS_AUTOCHECKBOX | WS_TABSTOP,
                                     0, 0, 10, 10, g_form, (HMENU)(INT_PTR)id, hInst, NULL);
            HookChild(b);
        } else {
            MakeLabel(hInst, g_form, kFields[i].label, id + 500);
            if (kFields[i].unit)
                MakeLabel(hInst, g_form, kFields[i].unit, id + 600);  /* 输入框右侧的单位 */
            HWND e = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                     WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | ES_AUTOHSCROLL | WS_TABSTOP,
                                     0, 0, 10, 10, g_form, (HMENU)(INT_PTR)id, hInst, NULL);
            /* 数字框：限 8 位（最大值 60000 也远用不完），防止粘进一大段文本。
               文本框按配置项自己的上限来限，不是 8 —— 路径比数字长得多。 */
            SendMessageW(e, EM_LIMITTEXT,
                         kFields[i].isText ? (FIELD_TEXT_MAX - 1) : 8, 0);
            HookChild(e);
        }
    }

    /* 「常用」字段编排（窗口信息弹窗的「常用」视图）：子段头 + 有序列表 +
       添加…/删除/上移/下移。列表项的文本是友好名，字段编号存在项数据里。 */
    MakeLabel(hInst, g_form,
              L"「常用」视图的字段（自上而下就是弹窗里的顺序）",
              IDC_CMN_HEAD);
    HookChild(CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
                              WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS |
                              WS_VSCROLL | WS_TABSTOP |
                              LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
                              0, 0, 10, 10, g_form,
                              (HMENU)(INT_PTR)IDC_CMN_LIST, hInst, NULL));
    MakeBtn(hInst, g_form, L"添加…", IDC_CMN_ADD);
    MakeBtn(hInst, g_form, L"删除",  IDC_CMN_DEL);
    MakeBtn(hInst, g_form, L"上移",  IDC_CMN_UP);
    MakeBtn(hInst, g_form, L"下移",  IDC_CMN_DOWN);
    MakeBtn(hInst, g_form, L"重置默认", IDC_CMN_RESET);

    for (int i = 0; i < LIST_N; i++) {
        int base = IDC_LIST0 + i * LIST_STRIDE;
        MakeLabel(hInst, g_form, kLists[i].label, base + 4);
        HookChild(CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
                                  WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS |
                                  WS_VSCROLL | WS_HSCROLL | WS_TABSTOP |
                                  LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
                                  0, 0, 10, 10, g_form,
                                  (HMENU)(INT_PTR)base, hInst, NULL));
        HWND ed = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                  WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | ES_AUTOHSCROLL | WS_TABSTOP,
                                  0, 0, 10, 10, g_form,
                                  (HMENU)(INT_PTR)(base + 1), hInst, NULL);
        SendMessageW(ed, EM_LIMITTEXT, CFG_NAME_MAX - 1, 0);
        HookChild(ed);
        MakeBtn(hInst, g_form, L"添加", base + 2);
        MakeBtn(hInst, g_form, L"删除", base + 3);
        MakeBtn(hInst, g_form, L"选取窗口", base + 5);   /* 只填输入框，还得点"添加" */
    }

    /* 底部"窗框"最后建：z 序压在表单内容之上，滚动时挡住溢出的内容 */
    g_footer = CreateWindowExW(WS_EX_CONTROLPARENT, FOOTER_CLASS, L"",
                               WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
                               0, 0, 10, 10, g_hwnd, (HMENU)(INT_PTR)IDC_FOOTER, hInst, NULL);
    g_status = MakeLabel(hInst, g_footer, L"", IDC_STATUS);
    MakeBtn(hInst, g_footer, L"重新加载", IDC_RELOAD);
    MakeBtn(hInst, g_footer, L"保存", IDC_SAVEONLY);
    MakeBtn(hInst, g_footer, L"保存并重启", IDC_SAVE);
    MakeBtn(hInst, g_footer, L"退出", IDC_EXIT);

    ApplyFont();          /* 一次给所有子控件设字体（含刚建出来的） */
    LoadConfig();
    {
        RECT rc; GetClientRect(g_hwnd, &rc);
        Layout(rc.right, rc.bottom);
    }
    ShowWindow(g_hwnd, SW_SHOWNORMAL);
    /* 主动提前台：从托盘（后台进程）拉起时，系统会拒绝子进程的前台请求，
       窗口就落在别的程序后面 —— 用户看到的是"点了设置什么都没发生"。
       拉起方已调 AllowSetForegroundWindow 授权，这里再补一次确保落在前面。 */
    SetForegroundWindow(g_hwnd);
    BringWindowToTop(g_hwnd);
    UpdateWindow(g_hwnd);

    /* 动画时钟：QPC 是唯一有亚毫秒精度的选择（见 AnimTick 的说明） */
    QueryPerformanceFrequency(&s_qpcFreq);

    /* 滚动动画的帧线程：跟着合成器走（见 AnimThreadProc）。常驻，但只在动画
       期间干活，其余时间阻塞在事件上 —— 与"闲时零流量"的要求不冲突。
       建不出来（极端资源不足）也不致命：AnimKick 会退化成直接跳到目标。 */
    s_animGo = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (s_animGo)
        s_animThread = CreateThread(NULL, 0, AnimThreadProc, NULL, 0, NULL);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        /* 选窗态下 Esc 取消：焦点多半在某个按钮/输入框上，主窗口的 WndProc
           收不到这个按键，所以在循环里拦（这是唯一能稳稳看到所有按键的地方） */
        if (g_pickList >= 0 && msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) {
            PickEnd();
            SetStatus(L"选窗已取消");
            continue;
        }
        /* Tab 在控件之间移动焦点：IsDialogMessage 让输入框也吃 Tab，
           否则设置界面里没法用键盘走完一整张表单。 */
        if (!IsDialogMessageW(g_hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return 0;
}
