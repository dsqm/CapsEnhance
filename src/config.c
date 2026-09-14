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
   本文件的部分内容派生自 AltSnap
     AltSnap: Copyright (C) 2015 Stefan Sundin
              Modified By Raymond Gillibert in 2022
              https://github.com/RamonUnch/AltSnap
   AltSnap 以 GPL-3.0-or-later 授权；本项目同样以 GPL-3.0-or-later 授权。
   ------------------------------------------------------------------ */

#include "cap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <string>
#include <vector>

/* ================= 配置模块（CapsEnhance.ini，与 exe 同目录） =================
   零依赖 INI 风格：分节保存，便于以后继续加值。
     [General]         键值项：Key = Value
     [NativeWordJump]  进程名名单：不做文本探测，直接发 Ctrl+←/→
     [AutoDisable]     进程名名单：前台是它时**整段不介入**（CapsLock 恢复正常）
     [NoSmoothScroll]  进程名名单：持续滚动只发整格（不累积小数刻度的程序）
   `;` 开头的行是注释（`#` **不是**）；空行忽略；`[段名]` 行切段。
   进程名完全匹配（大小写不敏感，.exe 后缀可写可不写）。
   文件不存在时写出一个带注释的模板。

   三份名单**各自独立**：它们语义完全不同（改通道 / 整段不介入 / 只改滚动
   粒度），共用一份就等于"任一命中都会触发所有行为"。所以名单是"加了索引
   的数组"，而不是"一个数组 + 几个常量"。
   占用：3 × 128 条 × 64 字符的静态数组（约 48KB），无动态增长。 */

/* CFG_MAX_ENTRIES / CFG_NAME_MAX 在 cap.h（设置程序也要用） */
#define CFG_VAL_LEN     256   /* 配置值缓冲：键名短，值留宽一点，宁可截断不可越界 */

/* 名单编号：只在 config.c 内部使用（对外一律用具名查询函数，见文件末尾）。
   加一份名单 = 加一个枚举值 + 一个段名 + 一个查询函数，解析/存储不用改。 */
typedef enum {
    CFGLIST_NATIVE     = CFG_LIST_NATIVE,      /* [NativeWordJump] */
    CFGLIST_AUTODISABLE= CFG_LIST_AUTODISABLE, /* [AutoDisable]    */
    CFGLIST_NOSMOOTH   = CFG_LIST_NOSMOOTH,    /* [NoSmoothScroll] */
    CFGLIST_DRAGWIN    = CFG_LIST_DRAGWIN,     /* [DragWindowBlacklist]  */
    CFGLIST_DRAGPROC   = CFG_LIST_DRAGPROC,    /* [DragProcessBlacklist] */
    CFGLIST_COUNT      = CFG_LIST_COUNT
} CfgListId;

/* 横向滚动的默认倍数：**1**（不放大）。
   横向是"每次滚轮挪一列"的语义，倍数 1 = 一个物理格换一次横向滚动，
   手感与系统原生横向滚动一致；想更快再调大，0 = 完全关闭该功能。
   为什么不跟纵向一样默认放大：横向默认放大反而会让横向移动过头（列宽通常
   远小于行高，横向更容易"越过目标"）。

   **改默认值时务必全局搜这个数值**：源码注释、ini 模板、ini 升级块、以及
   test_config.c 的断言与断言消息里都可能写死它（历史上"默认从 3 改成 2"时
   就有三处文案没跟着改，见 README 的"改默认值"小节）。 */
#define WHEEL_HMUL_DEFAULT 1
#define STR2(x) #x
#define STR(x)  STR2(x)

/* 日志文件大小上限（KB），0 = 不限制。
   上限是**安全**问题而不是口味问题：main.c 的 log_rotate_if_needed 会按
   "上限的一半"分配缓冲（`malloc((size_t)keep + 1)`，keep = maxKB*512），
   在 32 位构建下 (size_t)keep 可能回绕成一个很小的数 —— malloc 拿到小缓冲，
   随后的 fread 却按回绕前的大长度写进去，堆越界。当前是 x64 构建不会触发，
   但把上限钳住就**永远**不可能触发。 */
#define LOG_MAX_KB_DEFAULT 2048
#define LOG_MAX_KB_MAX     65536

/* 「Caps + 滚轮」的放大**倍数**（默认 3）：滚一格 = 系统原本一格 + 我们补 (N-1) 格。
   只在这一处定值：模板与升级块用 STR() 拼进去，避免"代码改了、字符串没改"的漂移。

   为什么又回到 3（这个值改过两轮，值得记下原因）：
     · 最初是 3，后来因为"滚动时能感觉到卡顿"改成 2 —— 但那次是**误诊**：
       卡顿的真因是补发时的 Sleep 跑在**主线程**上（主线程不在"等消息"状态
       会让系统跳过低级钩子），不是"多补了一格"；
     · 补发搬到 act.c 的滚轮专用线程之后，Sleep 只阻塞那个线程，倍数 3
       不再有任何卡顿。手感的账要算在"线程位置"上，不是"格数"上。
   `0` / `1` = 不放大（等同关闭，滚轮完全按系统原本行为）。
   **本项只管「Caps+滚轮」**：自动滚动完全不看它，有自己的两个速度项
   （AutoScrollInterval / AutoScrollFastMs）—— 这是刻意的。倍数量与"毫秒/格"
   是两种量，既不能比较也不能互相约束，硬凑成一个值只会两边都不好用。 */
#define WHEEL_MUL_DEFAULT 3
#define WHEEL_MUL_MAX     16   /* 单次最多补发 15 格（每格间隔 2ms） */

/* 自动滚动的**初始**节拍（ms）：启动后每 N 毫秒滚"一格"，即最慢档的速度。
   同向继续滚会逐档提速，**最快到 AutoScrollFastMs**（另一项，独立配置）。
   两个值同单位（毫秒/格）但**语义独立**：本值管"刚启动时多快"，FastMs 管
   "最快能多快"，各自调、互不牵制。

   默认值的依据是**人的实际阅读速度**，不是手感试出来的（前几版就是靠试，
   结果 120 -> 300 -> 500 -> 4000 改了四轮都还偏快）：
     · 成人中文默读约 300..500 字/分钟（阅读心理学通行的量级；
       《义务教育语文课程标准》对 7-9 年级的要求是"每分钟不少于 500 字"）
     · 需要边读边想的**技术文本/精读**约 80..150 字/分钟
     · 中文朗读 150..250 字/分钟（电视播音 240 左右），比默读慢得多
   换算假设：Windows 默认"每次滚 3 行"，一行按 45 字（最大化窗口的中间值）：
        行/分钟 = 180000 / 本值(ms)      字/分钟 = 行/分钟 × 45
        每格ms   行/分钟   字/分钟
         120     1500     67500   <- 最初的值，比人快两个数量级
         500      360     16200
        4000       45      2025   <- 中间一版，仍是人的 4~7 倍，所以还是偏快
        1500      120      5400   <- **当前默认**：不是阅读速度，是"按住就快速
                                      浏览"（持续滚动现在定位成翻页工具）
       18000       10       450   <- 通用默读
       24000      7.5       338   <- 想按阅读速度读长文就设这个（落在 300..500
                                      的偏慢半区，对应"边读边理解"的舒适区）
       60000        3       135   <- 技术精读（80..150 字/分钟）
   嫌慢就再滚一下提速，或把本值调小；想"读文档刚刚好"就调到 24000 一带。
   （上表的值与"当前默认"必须跟着 AUTOSCROLL_INTERVAL_DEFAULT 一起改 ——
     历史上把默认从 24000 改成 1500 时这张表没动，注释里留下了 24000 是
     默认的说法，与代码不一致。） */
#define AUTOSCROLL_INTERVAL_DEFAULT 1500

/* AutoScrollInterval 的可调范围（ms）。下限 100 对应 1800 行/分钟这种
   非人速的扫读，留着只是不设人为障碍；上限 60000（3 行/分钟 ≈ 135 字/分钟）
   覆盖"技术精读"这一档 —— 只要有需求就不该被上限挡住。 */
#define AUTOSCROLL_INTERVAL_MIN 100
#define AUTOSCROLL_INTERVAL_MAX 60000

/* 「Caps + Alt/D + 滚轮」持续滚动的**最快速度**（毫秒/格，越小越快）。
   自动滚动的速度阶梯 = 从 AutoScrollInterval 起步、逐档提速到本值为止
   （几何插值，见 act.c），所以本值是"上限"而不是"起始值"。
   默认 800 = 1.25 格/秒 ≈ 3.8 行/秒 ≈ 225 行/分钟（按系统每次滚 3 行）—— 已经比
   任何阅读速度都快，属于"看完了、快速往下翻"的档位；想更快就调小（下限 20），
   想让它也慢下来就调大。
   注：本项与 AutoScrollInterval **有意分开、不共用** —— 虽然同单位，但一个管
   "起始速度"、一个管"上限"，拆开各自调才不会互相牵制。 */
#define AUTOSCROLL_FAST_DEFAULT 800
#define AUTOSCROLL_FAST_MIN      20
#define AUTOSCROLL_FAST_MAX   60000

/* Scintilla 通道是否允许读取目标进程内存（默认开）。
   Scintilla 的 SCI_GETTEXT 只接受"对方进程地址空间里的缓冲区"，所以读全文
   必然要 OpenProcess(PROCESS_VM_OPERATION | PROCESS_VM_READ) + VirtualAllocEx
   + ReadProcessMemory（详见 nav.c 的 SciGetTextCaret）。这是对目标进程的
   实质性权限，虽然范围已经限定为"类名白名单 + 最多 2MB"，但**是否允许**
   应该由用户决定：设 0 即整条 Scintilla 通道放弃，退回 UIA / IA2 /
   原生 Ctrl+方向（Notepad3 等仍能跳，只是不再是瞬跳）。 */
#define ALLOW_SCINTILLA_READ_DEFAULT 1

/* 逐格按词跳转的最大步数：判定是"达到即改道"（n >= 该值就改发 Ctrl+方向），
   所以 4 表示"4 格及以上瞬跳、3 格以内逐格"。0 = 不限制（硬上限仍是 256）。
   为什么需要它而不一律逐格：逐格是 SendInput 一对 down/up + 两次 Sleep，
   约 13ms/格且目标程序要逐个处理，长跳会有可见的"光标爬行"；改发原生
   Ctrl+方向是一次到位（代价是落点由目标程序的词切分决定，需回读补正）。 */
#define MAX_ARROW_STEPS_DEFAULT 4
#define MAX_ARROW_STEPS_MAX     256   /* 硬上限：任何路径都不会超过这个步数 */

/* 横向倍数的上限：与纵向同一个口径（以前是散在解析分支里的字面量 16） */
#define WHEEL_HMUL_MAX 16

static WCHAR g_names[CFGLIST_COUNT][CFG_MAX_ENTRIES][CFG_NAME_MAX];
static int   g_count[CFGLIST_COUNT];
/* 缺省**不提权**（缺省配置就是 RunAsAdmin = 0）。
   为什么改默认：绝大多数功能（和弦映射、按词跳转、滚动、Caps+左键拖动普通窗口）
   都不需要管理员；默认提权只会让每次启动多弹一次 UAC，还带来两个副作用 ——
   管理员进程看不到普通权限程序的 COM/UIA 对象（WPS 得额外走降权桥），
   以及进程级测试在本机沙箱里会卡在 UAC 等待。
   什么时候需要设 1：**要拖动/操作以管理员运行的窗口** —— UIPI 会拦住低完整性
   进程对高完整性窗口的 SetWindowPos，不提权就拖不动它们。 */
static BOOL  g_runAsAdmin = FALSE;
static int   g_logMaxKB   = LOG_MAX_KB_DEFAULT;   /* 日志大小上限（KB），0 = 不限制 */
static int   g_maxArrowSteps = MAX_ARROW_STEPS_DEFAULT;  /* 逐格步数上限，达到即改道 */
static BOOL  g_overStepCtrl = TRUE;  /* TRUE=超限改发 Ctrl+方向（原生按词跳转） */
static int   g_wheelMul = WHEEL_MUL_DEFAULT;  /* Caps+滚轮 放大倍数，<2 = 不放大 */
static int   g_autoScrollFastMs = AUTOSCROLL_FAST_DEFAULT;       /* 自动滚动最快速度(ms/格) */
static int   g_autoScrollInterval = AUTOSCROLL_INTERVAL_DEFAULT; /* 自动滚动初始节拍(ms) */
static int   g_wheelHMul = WHEEL_HMUL_DEFAULT; /* Caps+Shift+滚轮 的横向滚动倍数，0=关闭 */
static BOOL  g_allowSciRead = ALLOW_SCINTILLA_READ_DEFAULT; /* Scintilla 通道读目标进程内存 */
/* 注：AutoScrollSmooth（全局平滑开关）已**删除**：它要表达的是"某些程序
   不累积小数刻度"，那天然是"按程序区分"的事，做成全局开关就等于让所有程序
   一起陪着退化。现在由 [NoSmoothScroll] 名单承担（见 ActAutoScrollWheel），
   老配置里那行既不解析也不报错 —— 留着无害。 */

/* 整数：只认前导十进制数字，其余（空/非法）返回默认值。
   超过 1e8 直接判非法：没有哪个配置项需要这么大，而继续累加会让
   n = n*10 + d 有符号溢出（UB）。 */
static int ParseIntW(const WCHAR* v, int def) {
    if (!v || !v[0]) return def;
    long long n = 0; BOOL any = FALSE;
    for (const WCHAR* p = v; *p; p++) {
        if (*p < L'0' || *p > L'9') break;
        n = n * 10 + (*p - L'0');
        if (n > 100000000LL) return def;
        any = TRUE;
    }
    return any ? (int)n : def;
}

typedef enum { SEC_NONE, SEC_GENERAL, SEC_NATIVE, SEC_AUTODISABLE,
               SEC_NOSMOOTH, SEC_DRAGWIN, SEC_DRAGPROC,
               SEC_OTHER } CfgSec;

/* 段名 -> 名单编号；不是名单段返回 -1。
   段名一律大小写不敏感、并容忍 "Native Word Jump" 这种带空格的老写法。 */
static int SecList(CfgSec sec) {
    switch (sec) {
    case SEC_NATIVE:      return CFGLIST_NATIVE;
    case SEC_AUTODISABLE: return CFGLIST_AUTODISABLE;
    case SEC_NOSMOOTH:    return CFGLIST_NOSMOOTH;
    case SEC_DRAGWIN:     return CFGLIST_DRAGWIN;
    case SEC_DRAGPROC:    return CFGLIST_DRAGPROC;
    default:              return -1;
    }
}

static void ConfigDefaultPath(WCHAR* out, size_t cap) {
    WCHAR wexe[MAX_PATH]; DWORD n = GetModuleFileNameW(NULL, wexe, MAX_PATH);
    WCHAR* slash = NULL;
    for (DWORD i = 0; i < n; i++) if (wexe[i] == L'\\') slash = &wexe[i];
    if (slash) *slash = 0;
    _snwprintf_s(out, cap, _TRUNCATE, L"%s\\CapsEnhance.ini", wexe);
}

/* 去掉结尾的 .exe（大小写不敏感） */
static void StripExeW(WCHAR* s) {
    size_t l = wcslen(s);
    if (l >= 4 && _wcsicmp(s + l - 4, L".exe") == 0) s[l - 4] = 0;
}

/* 行首尾去空白 */
static void TrimW(WCHAR* s) {
    size_t l = wcslen(s);
    while (l > 0 && (s[l-1] == L' ' || s[l-1] == L'\t' || s[l-1] == L'\r')) s[--l] = 0;
    WCHAR* p = s;
    while (*p == L' ' || *p == L'\t') p++;
    if (p != s) memmove(s, p, (wcslen(p) + 1) * sizeof(WCHAR));
}

static void LowerW(WCHAR* s) {
    for (; *s; s++) {
        if (*s >= L'A' && *s <= L'Z') *s = (WCHAR)(*s - L'A' + L'a');
    }
}

/* 布尔值只认 0/1：仅 "1" 为真，其余（含 0、空、其它写法）一律为假 */
static BOOL ParseBoolW(const WCHAR* v) {
    return (v[0] == L'1' && v[1] == L'\0');
}

/* ---------- 钳制：一份判据，解析与"设置程序改值"共用 ----------
   以前这些上下限只写在解析分支里；设置程序要改值就只能再抄一遍，两处一旦
   分叉就是"界面允许 999、加载回来变成 16"这类说不清的问题。现在解析与
   ConfigSet* 都走这里。 */
static int ClampInt(int v, int lo, int hi) {
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return v;
}
static int ClampLogMaxKB(int v)  { return ClampInt(v, 0, LOG_MAX_KB_MAX); }
static int ClampMaxArrowSteps(int v) { return ClampInt(v, 0, MAX_ARROW_STEPS_MAX); }
static int ClampWheelMul(int v)  { return ClampInt(v, 0, WHEEL_MUL_MAX); }
static int ClampWheelHMul(int v) { return ClampInt(v, 0, WHEEL_HMUL_MAX); }
static int ClampInterval(int v)  { return ClampInt(v, AUTOSCROLL_INTERVAL_MIN, AUTOSCROLL_INTERVAL_MAX); }
static int ClampFastMs(int v)    { return ClampInt(v, AUTOSCROLL_FAST_MIN, AUTOSCROLL_FAST_MAX); }

static void AddEntry(int list, const WCHAR* name) {
    if (list < 0 || list >= CFGLIST_COUNT) return;
    if (g_count[list] >= CFG_MAX_ENTRIES) return;
    WCHAR tmp[CFG_NAME_MAX];
    _snwprintf_s(tmp, CFG_NAME_MAX, _TRUNCATE, L"%s", name);
    TrimW(tmp);
    StripExeW(tmp);
    if (!tmp[0]) return;
    wcscpy_s(g_names[list][g_count[list]], CFG_NAME_MAX, tmp);
    g_count[list]++;
}

/* 名单是否为空 -> 决定查询是否可以"零成本短路"（名单空时连进程名都不用查） */
static BOOL ListEmpty(int list) {
    return (list < 0 || list >= CFGLIST_COUNT) ? TRUE : (g_count[list] == 0);
}

/* 两份拖动黑名单的**内置默认**，与 AltSnap 的默认黑名单一一对应
   （AltSnap.dni 的 [Blacklist] Windows 与 Processes 两条，AltSnap v1.68）。

   AltSnap 那两条就是两类：**窗口**（按类名）与**进程**（按 exe 名）。这里也分成
   两份名单，各自独立缺省回落 —— 谁的段没写就用谁的内置默认。

   条目形态（见 ConfigIsDragBlacklisted）：
     · 窗口名单：`*|ClassName`（AltSnap 语法；`*|` 直接写 `ClassName` 也认）。
       条目里的**标题字段不支持** —— 取另一个进程的标题要发 WM_GETTEXT，那是跨进程
       IPC，而判定发生在鼠标钩子上（钩子上绝不能做跨进程消息）。AltSnap 的默认条目
       每条都带类名，所以默认语义仍然完整。
     · 进程名单：`name.exe`（.exe 可省，大小写不敏感）。
   只有 ini 里没有对应的段时才用内置默认；段一存在，文件说了算。 */
static const WCHAR* const kDragWinDefaults[] = {
    /* —— AltSnap [Blacklist] Windows —— */
    L"*|Progman", L"*|MultitaskingViewFrame", L"*|TaskSwitcherWnd",
    L"*|TaskSwitcherOverlayWnd", L"*|WorkerW", L"*|Shell_TrayWnd",
    L"*|BaseBar", L"*|#32768", L"*|XamlExplorerHostIslandWindow",
    L"*|MozillaDropShadowWindowClass", L"*|VistaSwitcher_SwitcherWnd",
    L"*|TaskListThumbnailWnd", L"*|NotifyIconOverflowWindow",
    L"*|Windows.UI.Core.CoreWindow", L"*|NativeHWNDHost",
    L"*|Xaml_WindowedPopupClass", L"*|Shell_SecondaryTrayWnd",
    L"*|SimpleWindowSwitcher_{BEA057BB-66C7-4758-A610-FAE6013E9F98}",
    L"*|YuiWnd",
};
static const WCHAR* const kDragProcDefaults[] = {
    /* —— AltSnap [Blacklist] Processes —— */
    L"StartMenuExperienceHost.exe", L"SearchApp.exe", L"mstsc.exe", L"msrdc.exe",
    L"osk.exe", L"Virtual PC.exe", L"LeagueClient.exe", L"LeagueClientUx.exe",
    L"LeagueClientUxRender.exe",
};
/* 非 static：供单元测试直接喂指定路径 */
void ConfigLoadFrom(const WCHAR* path) {
    for (int i = 0; i < CFGLIST_COUNT; i++) g_count[i] = 0;
    g_runAsAdmin = FALSE;      /* 缺省不提权（理由见 g_runAsAdmin 定义处） */
    g_logMaxKB = LOG_MAX_KB_DEFAULT;
    g_maxArrowSteps = MAX_ARROW_STEPS_DEFAULT;
    g_overStepCtrl = TRUE;
    g_wheelMul = WHEEL_MUL_DEFAULT;
    g_autoScrollFastMs = AUTOSCROLL_FAST_DEFAULT;
    g_autoScrollInterval = AUTOSCROLL_INTERVAL_DEFAULT;
    g_wheelHMul = WHEEL_HMUL_DEFAULT;
    g_allowSciRead = ALLOW_SCINTILLA_READ_DEFAULT;

    FILE* f = NULL;
    if (_wfopen_s(&f, path, L"rb") != 0 || !f) return;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 64 * 1024) { fclose(f); return; }   /* 防呆：超大文件忽略 */

    /* 文件内容与宽字符副本都交给容器持有：以前是两对 malloc/free，每条提前
       返回路径都得记得 free（漏一处就是泄漏），现在由析构负责。 */
    std::vector<char> raw((size_t)sz + 1);
    size_t rd = fread(raw.data(), 1, (size_t)sz, f);
    fclose(f);
    raw[rd] = 0;

    const char* p = raw.data();
    if (rd >= 3 && (unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB &&
        (unsigned char)p[2] == 0xBF)
        p += 3;                                              /* 跳过 UTF-8 BOM */

    /* 一次性算出源长度：下面两次调用必须用同一个值（以前写成两遍
       `(int)(rd - (p - raw))`，改一处漏一处的风险白担） */
    int srcLen = (int)(rd - (size_t)(p - raw.data()));
    int wl = MultiByteToWideChar(CP_UTF8, 0, p, srcLen, NULL, 0);
    if (wl <= 0) return;
    std::vector<WCHAR> widev((size_t)wl + 1);
    MultiByteToWideChar(CP_UTF8, 0, p, srcLen, widev.data(), wl);
    widev[wl] = 0;
    WCHAR* wide = widev.data();

    /* 两份拖动黑名单的段是否在文件里出现过（没出现就用各自的内置默认） */
    BOOL sawDragWin = FALSE, sawDragProc = FALSE;
    CfgSec sec = SEC_NONE;
    WCHAR* line = wide;
    while (line) {
        WCHAR* nl = wcschr(line, L'\n');
        if (nl) *nl = 0;
        TrimW(line);

        if (line[0] == L';' || !line[0]) {
            /* 注释 / 空行：忽略 */
        } else if (line[0] == L'[') {
            WCHAR name[CFG_NAME_MAX];
            _snwprintf_s(name, CFG_NAME_MAX, _TRUNCATE, L"%s", line + 1);
            WCHAR* rb = wcschr(name, L']');
            if (rb) *rb = 0;
            TrimW(name);
            LowerW(name);
            if (_wcsicmp(name, L"general") == 0)          sec = SEC_GENERAL;
            else if (_wcsicmp(name, L"nativewordjump") == 0 ||
                     _wcsicmp(name, L"native word jump") == 0) sec = SEC_NATIVE;
            else if (_wcsicmp(name, L"autodisable") == 0 ||
                     _wcsicmp(name, L"auto disable") == 0)     sec = SEC_AUTODISABLE;
            else if (_wcsicmp(name, L"nosmoothscroll") == 0 ||
                     _wcsicmp(name, L"no smooth scroll") == 0) sec = SEC_NOSMOOTH;
            else if (_wcsicmp(name, L"dragwindowblacklist") == 0 ||
                     _wcsicmp(name, L"drag window blacklist") == 0) {
                sec = SEC_DRAGWIN; sawDragWin = TRUE;
            } else if (_wcsicmp(name, L"dragprocessblacklist") == 0 ||
                       _wcsicmp(name, L"drag process blacklist") == 0) {
                sec = SEC_DRAGPROC; sawDragProc = TRUE;
            }
            else                                          sec = SEC_OTHER;
        } else if (sec == SEC_GENERAL) {
            WCHAR* eq = wcschr(line, L'=');
            if (eq) {
                *eq = 0;
                WCHAR key[CFG_NAME_MAX], val[CFG_VAL_LEN];
                _snwprintf_s(key, CFG_NAME_MAX, _TRUNCATE, L"%s", line);
                _snwprintf_s(val, CFG_VAL_LEN, _TRUNCATE, L"%s", eq + 1);
                TrimW(key); TrimW(val); LowerW(key);
                if (_wcsicmp(key, L"runasadmin") == 0) g_runAsAdmin = ParseBoolW(val);
                else if (_wcsicmp(key, L"logmaxkb") == 0)
                    g_logMaxKB = ClampLogMaxKB(ParseIntW(val, LOG_MAX_KB_DEFAULT));
                else if (_wcsicmp(key, L"maxarrowsteps") == 0)
                    g_maxArrowSteps = ClampMaxArrowSteps(ParseIntW(val, MAX_ARROW_STEPS_DEFAULT));
                else if (_wcsicmp(key, L"overstepmode") == 0) g_overStepCtrl = ParseBoolW(val);
                else if (_wcsicmp(key, L"wheelscrollmul") == 0)
                    g_wheelMul = ClampWheelMul(ParseIntW(val, WHEEL_MUL_DEFAULT));
                /* 过渡期曾把本条改名为 WheelScrollMs（毫秒/格）。两种量不可换算，
                   所以旧键名既不解析、也不据它推算任何值 —— 直接忽略即可，
                   升级机制会补上新的 WheelScrollMul（老配置里那行 WheelScrollMs
                   会一直躺着，无害）。
                   **键名必须是 wheelscrollms**：早期这里误写成 wheelscrollmul，
                   与上面那条同名的分支重复 —— 那条永远先匹配，于是这块成了
                   永不执行的死代码，还让人误以为旧键被特殊处理过。 */
                else if (_wcsicmp(key, L"wheelscrollms") == 0) { /* 忽略（旧语义） */ }
                else if (_wcsicmp(key, L"autoscrollinterval") == 0)
                    g_autoScrollInterval = ClampInterval(ParseIntW(val, AUTOSCROLL_INTERVAL_DEFAULT));
                else if (_wcsicmp(key, L"autoscrollfastms") == 0)
                    g_autoScrollFastMs = ClampFastMs(ParseIntW(val, AUTOSCROLL_FAST_DEFAULT));
                else if (_wcsicmp(key, L"wheelhscrollmul") == 0)
                    g_wheelHMul = ClampWheelHMul(ParseIntW(val, WHEEL_HMUL_DEFAULT));
                /* autoscrollsmooth 已废弃（由 [NoSmoothScroll] 名单取代）：
                   既不解析、也不据它推算任何值 —— 写进去完全无副作用。 */
                else if (_wcsicmp(key, L"autoscrollsmooth") == 0) { /* 忽略（已废弃） */ }
                else if (_wcsicmp(key, L"allowscintillaread") == 0)
                    g_allowSciRead = ParseBoolW(val);
                /* 其余 [General] 项一律忽略：本项目所有配置项都在上面具名解析。
                   不认识的键（含已废弃的旧键名）留着也只是噪音，不解析、不保存。 */
                else { /* 忽略未知/废弃键 */ }
            }
        } else if (SecList(sec) >= 0) {
            AddEntry(SecList(sec), line);
        } else if (sec == SEC_NONE) {
            /* 兼容早期"不分节、一行一个进程名"的写法（归入原生按词跳转名单） */
            if (!wcschr(line, L'=')) AddEntry(CFGLIST_NATIVE, line);
        }
        line = nl ? nl + 1 : NULL;
    }

    /* 段缺失 -> 装各自的内置默认（取自 AltSnap 的默认黑名单）。
       这样老配置不用动文件就能得到一份合理的默认黑名单；一旦文件里出现了这个段
       （哪怕是空的），就以文件为准 —— "把段删空"是用户关掉那份名单的合法方式。
       设置程序保存时会把内存里的名单（含默认）整段写回文件，于是默认值对老用户
       也变成看得见、可编辑的。 */
    if (!sawDragWin)
        for (size_t di = 0; di < sizeof(kDragWinDefaults) / sizeof(kDragWinDefaults[0]); di++)
            AddEntry(CFGLIST_DRAGWIN, kDragWinDefaults[di]);
    if (!sawDragProc)
        for (size_t di = 0; di < sizeof(kDragProcDefaults) / sizeof(kDragProcDefaults[0]); di++)
            AddEntry(CFGLIST_DRAGPROC, kDragProcDefaults[di]);
}

void ConfigLoad(void) {
    WCHAR path[MAX_PATH];
    ConfigDefaultPath(path, MAX_PATH);

    FILE* probe = NULL;
    if (_wfopen_s(&probe, path, L"rb") != 0 || !probe) {
        /* 文件不存在：写一个带注释的模板，方便发现与编辑 */
        static const char kTemplate[] =
            "; 以 ; 开头的行是注释（空行忽略）；[段名] 行用于切段\n"
            "; 改动后需重启程序生效\n"
            ";\n"
            "[General]\n"
            "; 以管理员身份运行：启用后启动时自动申请管理员权限（会弹 UAC）\n"
            "; 1=启用，0=禁用\n"
            "; 默认 0：普通用法不需要管理员；只有要拖动/操作**以管理员运行的窗口**\n"
            "; （UIPI 会拦住低权限进程对高权限窗口的 SetWindowPos）时才需要设 1。\n"
            "RunAsAdmin = 0\n"
            ";\n"
            "; 日志文件大小上限（KB）：超过后保留最近的一半内容，0 = 不限制\n"
            "; 范围 0.." STR(LOG_MAX_KB_MAX) "，超出按上限钳制\n"
            "LogMaxKB = " STR(LOG_MAX_KB_DEFAULT) "\n"
            ";\n"
            "; 逐格按词跳转的最大步数：达到该步数就不再逐格发送，\n"
            "; 改按 OverStepMode 处理（" STR(MAX_ARROW_STEPS_DEFAULT) " = 该步数及以上瞬跳），避免方向键发太久。\n"
            "; 0 = 不限制（一律逐格），最大 256\n"
            "MaxArrowSteps = " STR(MAX_ARROW_STEPS_DEFAULT) "\n"
            ";\n"
            "; 超过 MaxArrowSteps 时的替代方式\n"
            ";   0 = 只发送一个方向键\n"
            ";   1 = 改发 Ctrl+方向键\n"
            "OverStepMode = 1\n"
            ";\n"
            "; 按住 C 时滚轮快速滚动：滚一格当 N 格（倍数 WheelScrollMul）。\n"
            "; 0 或 1 = 关闭（滚轮按系统原本行为）。范围 0..16。\n"
            "WheelScrollMul = 3\n"
            ";\n"
            "; 自动滚动的初始速度（毫秒/格，越大越慢）\n"
            "; 最快到 AutoScrollFastMs\n"
            "; 可调范围 100..60000。\n"
            "AutoScrollInterval = 1500\n"
            ";\n"
            "; 持续滚动速度上限\n"
            "; 与上面 AutoScrollInterval（初始速度）各自独立：一个管起步、一个管顶档。\n"
            "; 范围 20..60000；想更快就调小，想让它也慢下来就调大。\n"
            "AutoScrollFastMs = 800\n"
            ";\n"
            "; 横向滚动：按住 C + Shift 滚滚轮，横向挪动。倍数 1 = 一滚一格（默认），\n"
            "; 调大更快，0 = 关闭横向滚动（该组合退回原来的纵向语义）。\n"
            "WheelHScrollMul = 1\n"
            ";\n"
            "; Scintilla 通道（Notepad3 / Notepad++ / SciTE 等）读全文必须\n"
            "; 在目标进程里开一块内存再读回来（要 PROCESS_VM_OPERATION|VM_READ）。\n"
            "; 设为 0 即整条通道放弃，退回 UIA / IA2 / 原生 Ctrl+方向（仍能跳，\n"
            "; 只是不再是瞬跳）。\n"
            "AllowScintillaRead = " STR(ALLOW_SCINTILLA_READ_DEFAULT) "\n"
            ";\n"
            "[NativeWordJump]\n"
            "; 命中名单的程序：Caps+N / Caps+O 不做文本探测，\n"
            "; 直接发送 Ctrl+← / Ctrl+→，由该程序自身的词导航完成按词跳转（Shift 位移同样支持）。\n"
            "; 适用于文本读取不到、或读到的文本坐标与实际光标不符（按词跳转超调、光标乱飘）的程序。\n"
            "; 每行一个进程名：完全匹配、大小写不敏感、.exe 后缀可写可不写\n"
            "Obsidian.exe\n"
            ";\n"
            "[AutoDisable]\n"
            "; 命中名单的程序**在前台**时：本程序整段不介入 —— CapsLock 恢复正常\n"
            "; 功能，C+任意键也照常打字（不是“暂停后仍会吞键”，是真的完全不管）。\n"
            "; 适合那些自己吃掉了 CapsLock、或 Caps 组合与它自己的快捷键冲突的程序\n"
            "; （全屏游戏、远程桌面、虚拟机、部分 IDE）。\n"
            "; 每行一个进程名：完全匹配、大小写不敏感、.exe 后缀可写可不写\n"
            "; 例：mstsc.exe\n"
            ";\n"
            "[NoSmoothScroll]\n"
            "; 持续滚动（C+Alt/D+滚轮）默认只发“不足一格”的小刻度（手感像触控板）。\n"
            "; 少数程序不累积这种小刻度 —— 表现为滚了却完全不动。把它的进程名\n"
            "; 写进这里，就只对它改回每次发整格（120）。\n"
            "; 每行一个进程名：完全匹配、大小写不敏感、.exe 后缀可写可不写\n"
            "; 例：someapp.exe\n"
            ";\n"
            "[DragWindowBlacklist]\n"
            "; 按住 Caps 用**左键拖动**可以挪动当前窗口；命中名单的窗口不被接管\n"
            "; （左键原样透传）。默认值与 AltSnap 的 [Blacklist] Windows 一致。\n"
            "; 条目写法：*|ClassName（AltSnap 写法）或直接写 ClassName。\n"
            "; 注意条目里的**标题字段不支持** —— 读另一个进程的标题要发 WM_GETTEXT\n"
            "; （跨进程 IPC），而判定发生在鼠标钩子上，钩子上不能做跨进程消息。\n"
            "Progman\n"
            "MultitaskingViewFrame\n"
            "TaskSwitcherWnd\n"
            "TaskSwitcherOverlayWnd\n"
            "WorkerW\n"
            "Shell_TrayWnd\n"
            "BaseBar\n"
            "#32768\n"
            "XamlExplorerHostIslandWindow\n"
            "MozillaDropShadowWindowClass\n"
            "VistaSwitcher_SwitcherWnd\n"
            "TaskListThumbnailWnd\n"
            "NotifyIconOverflowWindow\n"
            "Windows.UI.Core.CoreWindow\n"
            "NativeHWNDHost\n"
            "Xaml_WindowedPopupClass\n"
            "Shell_SecondaryTrayWnd\n"
            "YuiWnd\n"
            ";\n"
            "[DragProcessBlacklist]\n"
            "; 同上，但按**进程名**判定。默认值与 AltSnap 的 [Blacklist] Processes 一致。\n"
            "; 条目写法：name.exe（.exe 可省，大小写不敏感）\n"
            "StartMenuExperienceHost.exe\n"
            "SearchApp.exe\n"
            "mstsc.exe\n"
            "msrdc.exe\n"
            "osk.exe\n"
            "Virtual PC.exe\n"
            "LeagueClient.exe\n"
            "LeagueClientUx.exe\n"
            "LeagueClientUxRender.exe\n"
            ";\n"
            "; 注：只要对应的段存在，就以文件为准（该份内置默认不再生效）——\n"
            "; 想把某份名单清空就把它下面那几行删掉。";
        FILE* w = NULL;
        if (_wfopen_s(&w, path, L"wb") == 0 && w) {
            fwrite(kTemplate, 1, sizeof(kTemplate) - 1, w);
            fclose(w);
        }
        return;
    }
    fclose(probe);
    ConfigUpgradeFile(path);   /* 老配置补齐新增键（无缺失则不改动） */
    ConfigLoadFrom(path);
}

/* ---- 配置文件升级：把新增键（连同注释）补进老用户的配置文件 ----
   模板只在文件不存在时生成，老用户的 ini 会缺少后来新增的键。
   这里检测 [General] 段内缺失的键，把"注释 + 默认值"插入该段末尾；
   没有 [General] 段的老格式则在文件头新建一段。用户已有内容原样保留，
   无缺失时不做任何写入。每次新增配置项后，在 kUpgrades 表里补一条即可。 */
static const struct {
    const char* key;
    const char* block;
} kUpgrades[] = {
    { "runasadmin",
      "\n"
      "; 以管理员身份运行：启用后启动时自动申请管理员权限（会弹 UAC）\n"
      "; 1=启用，0=禁用\n"
      "; 默认 0：只有要拖动/操作**以管理员运行的窗口**时才需要设 1。\n"
      "RunAsAdmin = 0\n" },
    { "logmaxkb",
      "\n"
      "; 日志文件大小上限（KB）：超过后保留最近的一半内容，0 = 不限制\n"
      "; 范围 0.." STR(LOG_MAX_KB_MAX) "，超出按上限钳制\n"
      "LogMaxKB = " STR(LOG_MAX_KB_DEFAULT) "\n" },
    { "maxarrowsteps",
      "\n"
      "; 逐格按词跳转的最大步数：达到该步数就不再逐格发送，\n"
      "; 改按 OverStepMode 处理（" STR(MAX_ARROW_STEPS_DEFAULT) " = 该步数及以上瞬跳），避免方向键发太久。\n"
      "; 0 = 不限制（一律逐格），最大 256\n"
      "MaxArrowSteps = " STR(MAX_ARROW_STEPS_DEFAULT) "\n" },
    { "overstepmode",
      "\n"
      "; 超过 MaxArrowSteps 时的替代方式\n"
      ";   0 = 只发送一个方向键\n"
      ";   1 = 改发 Ctrl+方向键\n"
      "OverStepMode = 1\n" },
    { "wheelscrollmul",
      "\n"
      "; 按住 C 时滚轮快速滚动：滚一格当 N 格（倍数 WheelScrollMul）。\n"
      "; 0 或 1 = 关闭（滚轮按系统原本行为）。范围 0.." STR(WHEEL_MUL_MAX) "。\n"
      "WheelScrollMul = " STR(WHEEL_MUL_DEFAULT) "\n" },
    { "autoscrollinterval",
      "\n"
      "; 自动滚动的初始速度（毫秒/格，越大越慢）\n"
      "; 最快到 AutoScrollFastMs\n"
      "; 可调范围 " STR(AUTOSCROLL_INTERVAL_MIN) ".." STR(AUTOSCROLL_INTERVAL_MAX) "。\n"
      "AutoScrollInterval = " STR(AUTOSCROLL_INTERVAL_DEFAULT) "\n" },
    { "autoscrollfastms",
      "\n"
      "; 持续滚动速度上限\n"
      "; 与上面 AutoScrollInterval（初始速度）各自独立：一个管起步、一个管顶档。\n"
      "; 范围 " STR(AUTOSCROLL_FAST_MIN) ".." STR(AUTOSCROLL_FAST_MAX)
      "；想更快就调小，想让它也慢下来就调大。\n"
      "AutoScrollFastMs = " STR(AUTOSCROLL_FAST_DEFAULT) "\n" },
    /* autoscrollsmooth 曾在这里：它已被 [NoSmoothScroll] 名单取代，
       **故意不再补进老配置** —— 补一个已经没人读的键只会误导用户去改它。 */
    { "wheelhscrollmul",
      "\n"
      "; 横向滚动：按住 C + Shift 滚滚轮，横向挪动。倍数 1 = 一滚一格（默认），\n"
      "; 调大更快，0 = 关闭横向滚动（该组合退回原来的纵向语义）。\n"
      "WheelHScrollMul = " STR(WHEEL_HMUL_DEFAULT) "\n" },
    { "allowscintillaread",
      "\n"
      "; Scintilla 通道（Notepad3 / Notepad++ / SciTE 等）读全文必须\n"
      "; 在目标进程里开一块内存再读回来（要 PROCESS_VM_OPERATION|VM_READ）。\n"
      "; 设为 0 即整条通道放弃，退回 UIA / IA2 / 原生 Ctrl+方向（仍能跳，\n"
      "; 只是不再是瞬跳）。\n"
      "AllowScintillaRead = " STR(ALLOW_SCINTILLA_READ_DEFAULT) "\n" },
};

/* ---- 升级键清单的只读访问（供单元测试**自动**生成 fixture）----
   为什么值得专门开两个接口：test_config 有一条"键齐全时升级不改动文件"的
   用例，它需要一份"把所有当前键都写上"的临时配置。此前那份配置是**手写**
   的，于是每加一个配置项都要记得回来补一行 —— 忘了就是一条"升级逻辑被误
   判成有缺失"的假失败（实测已经栽过一次，注释里也留了警告，但警告拦不住
   人忘）。改成从 kUpgrades 现读现生成之后，加配置项再也无需动测试。 */
int ConfigUpgradeKeyCount(void) {
    return (int)(sizeof(kUpgrades) / sizeof(kUpgrades[0]));
}
const char* ConfigUpgradeKeyName(int i) {
    if (i < 0 || i >= ConfigUpgradeKeyCount()) return NULL;
    return kUpgrades[i].key;
}

/* ---- 输出缓冲：已改用 std::string ----
   以前这里是一个手工算容量的裸缓冲（OutInit / OutAdd / OutAddCh / OutFree）：
   容量一旦算错，后面每一次 memcpy 都是堆越界写 —— 本项目已两次栽在这上面
   （见 776f468 与 ConfigUpgradeFile 里的 BOM 漏算）。std::string 自己管增长，
   "容量算错"这个缺陷类别直接从代码里消失，所以这层包装不再需要。 */

/* 复制一行到定长缓冲（结果保证以 0 结尾）。
   长度同时受**三个**约束：缓冲容量、到缓冲结尾的距离、行内长度。
   显式写出"到缓冲结尾的距离"这一项不是多余的：`ll < 159 ? ll : 159` 这种
   两方 min 在语义上已经够，但 MSVC /analyze 无法把 `memchr` 的返回值与
   分配长度关联起来，于是报 C6385（实测是误报）——每次都要人工排除一遍。
   把三个约束都写进同一个表达式，它就变成可静态证明的。 */
static void CopyLine(const char* start, const char* bufEnd, char* out, size_t cap) {
    size_t avail = (start < bufEnd) ? (size_t)(bufEnd - start) : 0;
    size_t n = (cap - 1) < avail ? (cap - 1) : avail;
    if (n) memcpy(out, start, n);
    out[n] = 0;
}

void ConfigUpgradeFile(const WCHAR* path) {    FILE* f = NULL;
    if (_wfopen_s(&f, path, L"rb") != 0 || !f) return;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 64 * 1024) { fclose(f); return; }
    std::vector<char> raw((size_t)sz + 1);
    size_t rd = fread(raw.data(), 1, (size_t)sz, f);
    fclose(f);
    raw[rd] = 0;

    const char* body = raw.data();
    size_t bodyLen = rd;
    int hasBom = (rd >= 3 && (unsigned char)body[0] == 0xEF &&
                  (unsigned char)body[1] == 0xBB && (unsigned char)body[2] == 0xBF);
    if (hasBom) { body += 3; bodyLen = rd - 3; }

    /* 定位 [General] 段：genStart=段行，genEnd=段结束（下一个段行或 EOF） */
    const char* genStart = NULL;
    const char* genEnd = body + bodyLen;
    const char* line = body;
    while (line < body + bodyLen) {
        const char* nl = (const char*)memchr(line, '\n', (size_t)(body + bodyLen - line));
        char tmp[160];
        CopyLine(line, body + bodyLen, tmp, sizeof(tmp));
        char* s = tmp;
        while (*s == ' ' || *s == '\t' || *s == '\r') s++;
        if (*s == '[') {
            if (_strnicmp(s, "[general]", 9) == 0 && !genStart)
                genStart = line;
            else if (genStart) { genEnd = line; break; }
        }
        line = nl ? nl + 1 : body + bodyLen;
    }

    /* 检查哪些键缺失（只在 [General] 段范围内找） */
    #define KUP_N ((int)(sizeof(kUpgrades)/sizeof(kUpgrades[0])))
    BOOL missing[KUP_N];
    int nMiss = 0;
    for (int i = 0; i < KUP_N; i++) {
        missing[i] = TRUE;
        for (const char* l = genStart ? genStart : body;
             l < (genStart ? genEnd : body + bodyLen) && missing[i]; ) {
            const char* nl = (const char*)memchr(l, '\n', (size_t)(body + bodyLen - l));
            char tmp[160];
            CopyLine(l, body + bodyLen, tmp, sizeof(tmp));
            char* s = tmp;
            while (*s == ' ' || *s == '\t' || *s == '\r') s++;
            size_t klen = strlen(kUpgrades[i].key);
            if (_strnicmp(s, kUpgrades[i].key, klen) == 0) {
                char c = s[klen];
                if (c == '=' || c == ' ' || c == '\t' || c == 0) missing[i] = FALSE;
            }
            l = nl ? nl + 1 : body + bodyLen;
        }
        if (missing[i]) nMiss++;
    }
    if (nMiss == 0) return;   /* 无缺失：不动文件 */

    /* 组装新内容：BOM + 头部 + [General] 行(若新建) + 缺失块 + 尾部。
       **不再逐项算容量**：以前要把 BOM / 段头 / 换行 / 各块长度加成一个
       total，并记住"BOM 不计入 bodyLen"这条隐含约束 —— 漏算它就是越界写
       （Debug CRT 下 abort）。std::string 自己增长，那类 bug 不可能再出现。 */
    static const char kGenHdr[] = "[General]\n";
    size_t insOff = genStart ? (size_t)(genEnd - body) : 0;
    int needNl = (genStart && insOff > 0 && body[insOff - 1] != '\n');

    std::string out;
    if (hasBom)          out.append("\xEF\xBB\xBF", 3);
    out.append(body, insOff);
    if (!genStart)       out.append(kGenHdr, sizeof(kGenHdr) - 1);
    if (needNl)          out.push_back('\n');
    for (int i = 0; i < KUP_N; i++)
        if (missing[i])  out.append(kUpgrades[i].block);
    out.append(body + insOff, bodyLen - insOff);

    if (_wfopen_s(&f, path, L"wb") == 0 && f) {
        fwrite(out.data(), 1, out.size(), f);
        fclose(f);
    }
    #undef KUP_N
}

BOOL ConfigGetRunAsAdmin(void) {
    return g_runAsAdmin;
}

int ConfigGetLogMaxKB(void) {
    return g_logMaxKB;
}

int ConfigGetMaxArrowSteps(void) {
    return g_maxArrowSteps;
}

BOOL ConfigGetOverStepCtrl(void) {
    return g_overStepCtrl;
}

int ConfigGetWheelScrollMul(void) {
    return g_wheelMul;
}

int ConfigGetAutoScrollInterval(void) {
    return g_autoScrollInterval;
}

int ConfigGetAutoScrollFastMs(void) {
    return g_autoScrollFastMs;
}

BOOL ConfigGetAllowScintillaRead(void) {
    return g_allowSciRead;
}

int ConfigGetWheelHScrollMul(void) {
    return g_wheelHMul;
}

/* 进程可执行文件名是否命中某份名单（完全匹配，.exe 可选） */
BOOL ConfigMatchList(int list, const WCHAR* exeName) {
    if (ListEmpty(list) || !exeName || !exeName[0]) return FALSE;
    WCHAR name[CFG_NAME_MAX];
    _snwprintf_s(name, CFG_NAME_MAX, _TRUNCATE, L"%s", exeName);
    StripExeW(name);
    for (int i = 0; i < g_count[list]; i++)
        if (_wcsicmp(name, g_names[list][i]) == 0) return TRUE;
    return FALSE;
}

BOOL ConfigMatchName(const WCHAR* exeName) {
    return ConfigMatchList(CFGLIST_NATIVE, exeName);
}

/* 名单查询的统一入口：先取窗口所属进程的 exe 名，再查名单。
   "名单为空"一律先短路 —— 绝大多数用户一份名单都不写，于是不必为每次
   Caps 按下（或每次启动持续滚动）付出 OpenProcess +
   QueryFullProcessImageName 的代价。 */
static BOOL MatchFocus(int list, HWND focus) {
    if (ListEmpty(list) || !focus) return FALSE;
    WCHAR fname[MAX_PATH];
    if (!ConfigGetProcessName(focus, fname, MAX_PATH)) return FALSE;
    return ConfigMatchList(list, fname);
}
BOOL ConfigIsAutoDisabled(HWND focus) {
    return MatchFocus(CFGLIST_AUTODISABLE, focus);
}
BOOL ConfigIsNoSmoothScroll(HWND focus) {
    return MatchFocus(CFGLIST_NOSMOOTH, focus);
}

/* 取焦点窗口所属进程的 exe 文件名（含后缀）；失败返回 FALSE */
BOOL ConfigGetProcessName(HWND focus, WCHAR* out, size_t cap) {
    if (!focus || !out || cap == 0) return FALSE;
    out[0] = 0;
    DWORD pid = 0;
    GetWindowThreadProcessId(focus, &pid);
    if (!pid) return FALSE;
    /* 只要查询权限；查不到名字就不算命中，正常走文本通道 */
    HANDLE hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hp) return FALSE;
    WCHAR exe[MAX_PATH];
    DWORD sz = MAX_PATH;
    BOOL ok = QueryFullProcessImageNameW(hp, 0, exe, &sz);
    CloseHandle(hp);
    if (!ok || !sz) return FALSE;

    WCHAR* fname = exe;
    for (WCHAR* q = exe; *q; q++) if (*q == L'\\') fname = q + 1;
    _snwprintf_s(out, cap, _TRUNCATE, L"%s", fname);
    return out[0] != 0;
}

BOOL ConfigIsNativeWordJump(HWND focus) {
    return MatchFocus(CFGLIST_NATIVE, focus);
}

/* ================= 设置程序用：改值 + 写回（增量更新） =================
   设置程序（settings/settings.c）是一个**表单**界面，它改的是"某个配置项"，
   而不是整份文本 —— 所以这里要提供两个东西：
     · ConfigSet*：改内存里的值（钳制复用解析那套 Clamp*，不另写一份）；
     · ConfigSaveFile：把内存里的值**增量**写回 ini。
   为什么不是"整份重新生成"：配置文件里带大量注释，用户也可能留下我们不认识
   的段或键；整份重写会把它们全冲掉（用户视角就是"我写的东西没了"）。
   增量更新的规则：
     · [General] 段：已有的键**就地替换值**；缺失的键补到该段末尾（连同
       kUpgrades 里那份注释块，值用当前值，不是默认值）；
     · 名单段：注释与空行保留，**名字行整段重建**（顺序即界面里的顺序）；
     · 其它段 / 未知键：原样保留；BOM 与行尾风格（LF / CRLF）跟随原文件；
     · 文件不存在：按"新建"处理（[General] + 全部键 + 三份名单段）。 */

void ConfigSetRunAsAdmin(BOOL v)          { g_runAsAdmin = !!v; }
void ConfigSetLogMaxKB(int v)             { g_logMaxKB = ClampLogMaxKB(v); }
void ConfigSetMaxArrowSteps(int v)        { g_maxArrowSteps = ClampMaxArrowSteps(v); }
void ConfigSetOverStepCtrl(BOOL v)        { g_overStepCtrl = !!v; }
void ConfigSetWheelScrollMul(int v)       { g_wheelMul = ClampWheelMul(v); }
void ConfigSetAutoScrollInterval(int v)   { g_autoScrollInterval = ClampInterval(v); }
void ConfigSetAutoScrollFastMs(int v)     { g_autoScrollFastMs = ClampFastMs(v); }
void ConfigSetWheelHScrollMul(int v)      { g_wheelHMul = ClampWheelHMul(v); }
void ConfigSetAllowScintillaRead(BOOL v)  { g_allowSciRead = !!v; }

/* 名单整体替换（顺序 = 传入顺序）。逐条走 AddEntry 才能拿到"去空白 / 去 .exe
   / 去空行"那套规整 —— 界面里手写的名字与文件里读出来的必须是同一个样子。 */
void ConfigSetList(int list, const WCHAR* const* names, int n) {
    if (list < 0 || list >= CFGLIST_COUNT) return;
    g_count[list] = 0;
    if (!names) return;
    for (int i = 0; i < n; i++) AddEntry(list, names[i]);
}

int ConfigListCount(int list) {
    return (list >= 0 && list < CFGLIST_COUNT) ? g_count[list] : 0;
}
const WCHAR* ConfigListAt(int list, int i) {
    if (list < 0 || list >= CFGLIST_COUNT || i < 0 || i >= g_count[list]) return NULL;
    return g_names[list][i];
}

/* 窗口名单是否命中这个类名。
   条目形态：`*|ClassName`（AltSnap 语法）或直接写 `ClassName` —— 两种都取
   `|` 右边那段的语义（没有 `|` 时整条就是类名）。
   **不支持标题**：取另一个进程的窗口标题要发 WM_GETTEXT（跨进程 IPC），
   而这个查询发生在**鼠标钩子**上（按下左键那一刻）—— 钩子上绝不能做跨进程消息。 */
static BOOL DragWinHit(const WCHAR* cls) {
    if (!cls || !cls[0]) return FALSE;
    for (int i = 0; i < g_count[CFGLIST_DRAGWIN]; i++) {
        const WCHAR* it = g_names[CFGLIST_DRAGWIN][i];
        const WCHAR* bar = wcsrchr(it, L'|');
        if (_wcsicmp(bar ? bar + 1 : it, cls) == 0) return TRUE;
    }
    return FALSE;
}

/* 进程名单是否命中这个 exe 名（与其它名单一样：完全匹配、忽略大小写、.exe 可省） */
static BOOL DragProcHit(const WCHAR* exe) {
    if (!exe || !exe[0]) return FALSE;
    for (int i = 0; i < g_count[CFGLIST_DRAGPROC]; i++) {
        WCHAR a[CFG_NAME_MAX], e[CFG_NAME_MAX];
        _snwprintf_s(a, CFG_NAME_MAX, _TRUNCATE, L"%s", g_names[CFGLIST_DRAGPROC][i]);
        _snwprintf_s(e, CFG_NAME_MAX, _TRUNCATE, L"%s", exe);
        StripExeW(a); StripExeW(e);
        if (_wcsicmp(a, e) == 0) return TRUE;
    }
    return FALSE;
}

/* 命中任一份名单即算黑名单。
   两份名单各自先看"空不空"再决定要不要去查对应的东西 —— 类名是本地调用很便宜，
   进程名要 OpenProcess（[AutoDisable] 同一条路），名单空时别白付这个代价。 */
BOOL ConfigIsDragBlacklisted(HWND hwnd) {
    if (!hwnd) return FALSE;
    BOOL hasWin = !ListEmpty(CFGLIST_DRAGWIN);
    BOOL hasProc = !ListEmpty(CFGLIST_DRAGPROC);
    if (!hasWin && !hasProc) return FALSE;

    if (hasWin) {
        WCHAR cls[128] = L"";
        if (GetClassNameW(hwnd, cls, 128) && DragWinHit(cls)) return TRUE;
    }
    if (hasProc) {
        WCHAR exe[MAX_PATH] = L"";
        if (ConfigGetProcessName(hwnd, exe, MAX_PATH) && DragProcHit(exe)) return TRUE;
    }
    return FALSE;
}

/* 名单段名（与解析时认的写法一致；新增名单段改这一处即可） */
static const char* const kListSec[CFGLIST_COUNT] = {
    "NativeWordJump", "AutoDisable", "NoSmoothScroll",
    "DragWindowBlacklist", "DragProcessBlacklist"
};

/* 当前所有 [General] 键的"键 = 值"（供写回用）。键名一律小写 —— 与解析时
   比较用的形态一致，也保证"文件里 Key 写成任意大小写"都能被就地替换。 */
/* key = 比较用的形态（小写，与解析/升级表一致）；
   name = **写进文件的形态**（与模板一致的大小写）—— 两者不能只留一个：
   用小写写回的话，用户文件里的 RunAsAdmin 会变成 runasadmin，解析没问题，
   但"我的配置被人改了大小写"是实打实的观感问题。 */
typedef struct { const char* key; const char* name; char val[24]; } KeyVal;
static int GeneralValues(KeyVal* out, int cap) {
    int n = 0;
#define KV(k, nm, fmt, arg) do { if (n < cap) { \
        out[n].key = (k); out[n].name = (nm); \
        _snprintf_s(out[n].val, sizeof(out[n].val), _TRUNCATE, fmt, arg); \
        n++; } } while (0)
    KV("runasadmin",         "RunAsAdmin",         "%d", g_runAsAdmin ? 1 : 0);
    KV("logmaxkb",           "LogMaxKB",           "%d", g_logMaxKB);
    KV("maxarrowsteps",      "MaxArrowSteps",      "%d", g_maxArrowSteps);
    KV("overstepmode",       "OverStepMode",       "%d", g_overStepCtrl ? 1 : 0);
    KV("wheelscrollmul",     "WheelScrollMul",     "%d", g_wheelMul);
    KV("autoscrollinterval", "AutoScrollInterval", "%d", g_autoScrollInterval);
    KV("autoscrollfastms",   "AutoScrollFastMs",   "%d", g_autoScrollFastMs);
    KV("wheelhscrollmul",    "WheelHScrollMul",    "%d", g_wheelHMul);
    KV("allowscintillaread", "AllowScintillaRead", "%d", g_allowSciRead ? 1 : 0);
#undef KV
    return n;
}

static BOOL ReadAllBytes(const WCHAR* path, std::string* out, int* hasBom) {
    if (hasBom) *hasBom = 0;
    FILE* f = NULL;
    if (_wfopen_s(&f, path, L"rb") != 0 || !f) return FALSE;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 1024 * 1024) { fclose(f); return FALSE; }
    std::vector<char> raw((size_t)sz);
    size_t rd = fread(raw.data(), 1, (size_t)sz, f);
    fclose(f);
    if (hasBom && rd >= 3 && (unsigned char)raw[0] == 0xEF &&
        (unsigned char)raw[1] == 0xBB && (unsigned char)raw[2] == 0xBF)
        *hasBom = 1;
    out->assign(raw.data(), rd);
    return TRUE;
}

/* 行首跳过空白后是否匹配 key，且后面是 '=' 或行尾（避免 "LogMaxKB2 = 1"）
   —— 与 ConfigUpgradeFile 的判据同一套，别写成两样。 */
static BOOL LineIsKey(const char* s, size_t len, const char* key) {
    size_t i = 0;
    while (i < len && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r')) i++;
    size_t klen = strlen(key);
    if (len - i < klen) return FALSE;
    if (_strnicmp(s + i, key, klen) != 0) return FALSE;
    i += klen;
    while (i < len && (s[i] == ' ' || s[i] == '\t')) i++;
    return (i >= len) || (s[i] == '=');
}
/* 是否是 [段名] 行；sec 为空则"任意段行" */
static BOOL LineIsSection(const char* s, size_t len, const char* sec) {
    size_t i = 0;
    while (i < len && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r')) i++;
    if (i >= len || s[i] != '[') return FALSE;
    i++;
    if (!sec) return TRUE;
    size_t sl = strlen(sec);
    if (len - i < sl + 1) return FALSE;
    if (_strnicmp(s + i, sec, sl) != 0) return FALSE;
    return s[i + sl] == ']';
}
static BOOL LineIsBlankOrComment(const char* s, size_t len) {
    size_t i = 0;
    while (i < len && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r')) i++;
    return (i >= len) || (s[i] == ';');
}

/* 输出缓冲：一行一个 '\n' 结尾。刻意**不用** std::vector<std::string> ——
   本项目不开 C++ 异常（无 /EHsc），容器析构会让编译器报 C4530
   （"用了异常处理程序但未启用展开语义"），而我们要保持 lint 零告警。
   std::string 自己管增长，"容量算错"这类缺陷也不会回来。 */
static void AppendLine(std::string* out, const std::string& line) {
    out->append(line);
    out->push_back('\n');
}

/* 把"键 = 当前值"连同 kUpgrades 里那份注释块追加进去（缺键时才用）。
   注释块最后一行是 "Key = 默认值"，把它换成当前值 —— 不掉注释，也不写错值。 */
/* key：小写、用于**匹配**注释块里的那一行；name：写进文件的形态 */
static void AppendKeyBlock(std::string* out, const char* key, const char* name,
                           const char* val) {
    const char* block = NULL;
    for (int i = 0; i < (int)(sizeof(kUpgrades) / sizeof(kUpgrades[0])); i++)
        if (_stricmp(kUpgrades[i].key, key) == 0) { block = kUpgrades[i].block; break; }
    std::string kv = std::string(name) + " = " + val;
    if (!block) { AppendLine(out, kv); return; }
    const char* p = block;
    while (*p) {
        const char* nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        std::string line(p, len);
        size_t e = line.size();
        while (e > 0 && (line[e - 1] == '\r')) e--;
        if (LineIsKey(line.c_str(), e, key)) AppendLine(out, kv);
        else AppendLine(out, line.substr(0, e));
        if (!nl) break;
        p = nl + 1;
    }
}

BOOL ConfigSaveFile(const WCHAR* path) {
    std::string raw;
    int hasBom = 0;
    BOOL exists = ReadAllBytes(path, &raw, &hasBom);
    /* 行尾风格跟随原文件（新建的文件用 CRLF —— Windows 记事本打开才不会
       挤成一行，虽然我们的解析器两种都认）。 */
    BOOL crlf = exists ? (raw.find('\r') != std::string::npos) : TRUE;
    const char* body = raw.data();
    size_t bodyLen = raw.size();
    if (hasBom && bodyLen >= 3) { body += 3; bodyLen -= 3; }

    KeyVal kv[16];
    int nkv = GeneralValues(kv, (int)(sizeof(kv) / sizeof(kv[0])));
    BOOL done[16];
    for (int i = 0; i < nkv; i++) done[i] = FALSE;

    std::string out;
    BOOL seenGeneral = FALSE;
    BOOL seenList[CFGLIST_COUNT];
    for (int i = 0; i < CFGLIST_COUNT; i++) seenList[i] = FALSE;
    int sec = -1;          /* -1=文件头 0=General 1..3=名单段 99=其它段 */
    int listId = -1;

    /* 段结束时的收尾：把没写到的键 / 名单补进去。
       （写成宏而不是 lambda：同样是避开"容器内放非平凡对象 + 无 /EHsc"的
         C4530；这里只是往 out 里追加几行，没有捕获任何东西的必要。） */
    #define FLUSH_SECTION() do { \
        if (sec == 0) { \
            for (int i = 0; i < nkv; i++) \
                if (!done[i]) { AppendKeyBlock(&out, kv[i].key, kv[i].name, kv[i].val); done[i] = TRUE; } \
        } else if (listId >= 0) { \
            char u[CFG_NAME_MAX * 4]; \
            for (int i = 0; i < g_count[listId]; i++) { \
                WideCharToMultiByte(CP_UTF8, 0, g_names[listId][i], -1, \
                                    u, (int)sizeof(u), NULL, NULL); \
                AppendLine(&out, std::string(u)); \
            } \
        } \
    } while (0)

    size_t pos = 0;
    while (pos <= bodyLen) {
        const char* nl = (const char*)memchr(body + pos, '\n', bodyLen - pos);
        size_t len = nl ? (size_t)(nl - (body + pos)) : bodyLen - pos;
        std::string line(body + pos, len);
        size_t e = line.size();
        while (e > 0 && line[e - 1] == '\r') e--;
        std::string core = line.substr(0, e);

        BOOL isSec = LineIsSection(core.c_str(), core.size(), NULL);
        if (isSec) {
            FLUSH_SECTION();
            sec = 99; listId = -1;
            if (LineIsSection(core.c_str(), core.size(), "general")) {
                sec = 0; seenGeneral = TRUE;
            } else {
                for (int i = 0; i < CFGLIST_COUNT; i++)
                    if (LineIsSection(core.c_str(), core.size(), kListSec[i])) {
                        sec = 100 + i; listId = i; seenList[i] = TRUE;
                    }
            }
            AppendLine(&out, core);
        } else if (sec == 0) {
            BOOL hit = FALSE;
            for (int i = 0; i < nkv && !hit; i++)
                if (!done[i] && LineIsKey(core.c_str(), core.size(), kv[i].key)) {
                    AppendLine(&out, std::string(kv[i].name) + " = " + kv[i].val);
                    done[i] = TRUE; hit = TRUE;
                }
            if (!hit) AppendLine(&out, core);
        } else if (listId >= 0) {
            /* 名单段：只留注释与空行，名字行由 FLUSH_SECTION 统一重建 */
            if (LineIsBlankOrComment(core.c_str(), core.size())) AppendLine(&out, core);
        } else {
            AppendLine(&out, core);
        }

        if (!nl) break;
        pos = (size_t)(nl - body) + 1;
        if (pos >= bodyLen && bodyLen > 0 && body[bodyLen - 1] == '\n') break;
    }
    FLUSH_SECTION();
    #undef FLUSH_SECTION

    /* 新建：[General] 段（连同全部键的注释块）插到最前面 */
    if (!seenGeneral) {
        std::string head;
        AppendLine(&head, std::string("[General]"));
        for (int i = 0; i < nkv; i++)
            if (!done[i]) { AppendKeyBlock(&head, kv[i].key, kv[i].name, kv[i].val); done[i] = TRUE; }
        out.insert(0, head);
    }
    /* 名单段缺失但名单非空：补在文件末尾 */
    for (int i = 0; i < CFGLIST_COUNT; i++) {
        if (seenList[i] || g_count[i] == 0) continue;
        AppendLine(&out, std::string("[") + kListSec[i] + "]");
        char u[CFG_NAME_MAX * 4];
        for (int j = 0; j < g_count[i]; j++) {
            WideCharToMultiByte(CP_UTF8, 0, g_names[i][j], -1, u, (int)sizeof(u), NULL, NULL);
            AppendLine(&out, std::string(u));
        }
    }

    /* 行尾风格统一（行在缓冲里一律以 '\n' 结尾，CRLF 在这里补） */
    std::string text;
    if (crlf)
        for (size_t i = 0; i < out.size(); i++) {
            if (out[i] == '\n') text.append("\r\n");
            else text.push_back(out[i]);
        }
    else
        text = out;

    FILE* w = NULL;
    if (_wfopen_s(&w, path, L"wb") != 0 || !w) return FALSE;
    BOOL ok = TRUE;
    if (hasBom) ok = (fwrite("\xEF\xBB\xBF", 1, 3, w) == 3);
    if (ok && !text.empty()) ok = (fwrite(text.data(), 1, text.size(), w) == text.size());
    fclose(w);
    return ok;
}

/* 真实的运行权限（查进程令牌，与配置值无关）：托盘提示据此标注 */
BOOL IsRunElevated(void) {
    HANDLE h = NULL;
    TOKEN_ELEVATION e;
    DWORD sz = 0;
    BOOL ok = FALSE;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &h)) {
        if (GetTokenInformation(h, TokenElevation, &e, sizeof(e), &sz))
            ok = !!e.TokenIsElevated;
        CloseHandle(h);
    }
    return ok;
}
