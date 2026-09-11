#include "cap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* ================= 配置模块（CapsEnhance.ini，与 exe 同目录） =================
   零依赖 INI 风格：分节保存，便于以后继续加值。
     [General]         键值项：Key = Value
     [NativeWordJump]  进程名名单（每行一个）
   ; 或 # 开头的行是注释；空行忽略；[段名] 行切段。
   进程名完全匹配（大小写不敏感，.exe 后缀可写可不写）。
   文件不存在时写出一个带注释的模板。
   占用：固定 128 条 × 64 字符的静态数组（约 16KB），无动态增长。 */

#define CFG_MAX_ENTRIES 128
#define CFG_NAME_MAX    64
#define CFG_VAL_LEN     256   /* 配置值缓冲：键名短，值留宽一点，宁可截断不可越界 */

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
   默认 1000 = 1 格/秒 = 3 行/秒 ≈ 180 行/分钟（按系统每次滚 3 行）—— 已经比
   任何阅读速度都快，属于"看完了、快速往下翻"的档位；想更快就调小（下限 20），
   想让它也慢下来就调大。
   注：本项与 AutoScrollInterval **有意分开、不共用** —— 虽然同单位，但一个管
   "起始速度"、一个管"上限"，拆开各自调才不会互相牵制。 */
#define AUTOSCROLL_FAST_DEFAULT 800
#define AUTOSCROLL_FAST_MIN      20
#define AUTOSCROLL_FAST_MAX   60000

/* 自动滚动是否用"高分频"平滑注入（默认开）。
   标准滚轮刻度 WHEEL_DELTA = 120 是"一格"，但 Windows 明确支持**小于**
   WHEEL_DELTA 的增量（文档：zDelta 是 WHEEL_DELTA 的"倍数或约数"）——
   触控板/自由滚轮就是这么滚的：次数多、每次小。于是同一速度下可以有更多
   注入点、每次只挪一点点，手感从"一格一格跳"变成连续滑动。
   个别程序对小数刻度不做累积（表现为完全不动），把这里设 0 退回整格模式。 */
#define AUTOSCROLL_SMOOTH_DEFAULT 1

/* Scintilla 通道是否允许读取目标进程内存（默认开）。
   Scintilla 的 SCI_GETTEXT 只接受"对方进程地址空间里的缓冲区"，所以读全文
   必然要 OpenProcess(PROCESS_VM_OPERATION | PROCESS_VM_READ) + VirtualAllocEx
   + ReadProcessMemory（详见 nav.c 的 SciGetTextCaret）。这是对目标进程的
   实质性权限，虽然范围已经限定为"类名白名单 + 最多 2MB"，但**是否允许**
   应该由用户决定：设 0 即整条 Scintilla 通道放弃，退回 UIA / IA2 /
   原生 Ctrl+方向（Notepad3 等仍能跳，只是不再是瞬跳）。 */
#define ALLOW_SCINTILLA_READ_DEFAULT 1

static WCHAR g_names[CFG_MAX_ENTRIES][CFG_NAME_MAX];
static int   g_count = 0;
static BOOL  g_runAsAdmin = TRUE;    /* 缺省启用（缺省配置就是 RunAsAdmin = 1） */
static int   g_logMaxKB   = LOG_MAX_KB_DEFAULT;   /* 日志大小上限（KB），0 = 不限制 */
static int   g_maxArrowSteps = 8;   /* 逐格按词跳转最大步数，超过按 overStepMode 处理 */
static BOOL  g_overStepCtrl = TRUE;  /* TRUE=超限改发 Ctrl+方向（原生按词跳转） */
static int   g_wheelMul = WHEEL_MUL_DEFAULT;  /* Caps+滚轮 放大倍数，<2 = 不放大 */
static int   g_autoScrollFastMs = AUTOSCROLL_FAST_DEFAULT;       /* 自动滚动最快速度(ms/格) */
static int   g_autoScrollInterval = AUTOSCROLL_INTERVAL_DEFAULT; /* 自动滚动初始节拍(ms) */
static BOOL  g_autoScrollSmooth = AUTOSCROLL_SMOOTH_DEFAULT;     /* 自动滚动平滑注入 */
static int   g_wheelHMul = WHEEL_HMUL_DEFAULT; /* Caps+Shift+滚轮 的横向滚动倍数，0=关闭 */
static BOOL  g_allowSciRead = ALLOW_SCINTILLA_READ_DEFAULT; /* Scintilla 通道读目标进程内存 */

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

typedef enum { SEC_NONE, SEC_GENERAL, SEC_NATIVE, SEC_OTHER } CfgSec;

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

static void AddEntry(const WCHAR* name) {
    if (g_count >= CFG_MAX_ENTRIES) return;
    WCHAR tmp[CFG_NAME_MAX];
    _snwprintf_s(tmp, CFG_NAME_MAX, _TRUNCATE, L"%s", name);
    TrimW(tmp);
    StripExeW(tmp);
    if (!tmp[0]) return;
    wcscpy_s(g_names[g_count], CFG_NAME_MAX, tmp);
    g_count++;
}

/* 非 static：供单元测试直接喂指定路径 */
void ConfigLoadFrom(const WCHAR* path) {
    g_count = 0;
    g_runAsAdmin = TRUE;
    g_logMaxKB = LOG_MAX_KB_DEFAULT;
    g_maxArrowSteps = 8;
    g_overStepCtrl = TRUE;
    g_wheelMul = WHEEL_MUL_DEFAULT;
    g_autoScrollFastMs = AUTOSCROLL_FAST_DEFAULT;
    g_autoScrollInterval = AUTOSCROLL_INTERVAL_DEFAULT;
    g_autoScrollSmooth = AUTOSCROLL_SMOOTH_DEFAULT;
    g_wheelHMul = WHEEL_HMUL_DEFAULT;
    g_allowSciRead = ALLOW_SCINTILLA_READ_DEFAULT;

    FILE* f = NULL;
    if (_wfopen_s(&f, path, L"rb") != 0 || !f) return;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 64 * 1024) { fclose(f); return; }   /* 防呆：超大文件忽略 */

    char* raw = (char*)malloc((size_t)sz + 1);
    if (!raw) { fclose(f); return; }
    size_t rd = fread(raw, 1, (size_t)sz, f);
    fclose(f);
    raw[rd] = 0;

    const char* p = raw;
    if (rd >= 3 && (unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB &&
        (unsigned char)p[2] == 0xBF)
        p += 3;                                              /* 跳过 UTF-8 BOM */

    int wl = MultiByteToWideChar(CP_UTF8, 0, p, (int)(rd - (p - raw)), NULL, 0);
    if (wl <= 0) { free(raw); return; }
    WCHAR* wide = (WCHAR*)malloc(((size_t)wl + 1) * sizeof(WCHAR));
    if (!wide) { free(raw); return; }
    MultiByteToWideChar(CP_UTF8, 0, p, (int)(rd - (p - raw)), wide, wl);
    wide[wl] = 0;
    free(raw);

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
                else if (_wcsicmp(key, L"logmaxkb") == 0) {
                    int v = ParseIntW(val, LOG_MAX_KB_DEFAULT);
                    if (v < 0) v = 0;                        /* 0 = 不限制 */
                    if (v > LOG_MAX_KB_MAX) v = LOG_MAX_KB_MAX;
                    g_logMaxKB = v;
                }
                else if (_wcsicmp(key, L"maxarrowsteps") == 0) g_maxArrowSteps = ParseIntW(val, 8);
                else if (_wcsicmp(key, L"overstepmode") == 0) g_overStepCtrl = ParseBoolW(val);
                else if (_wcsicmp(key, L"wheelscrollmul") == 0) {
                    int v = ParseIntW(val, WHEEL_MUL_DEFAULT);
                    if (v < 0) v = 0;
                    if (v > WHEEL_MUL_MAX) v = WHEEL_MUL_MAX;
                    g_wheelMul = v;
                }
                /* 过渡期曾把本条改名为 WheelScrollMs（毫秒/格）。两种量不可换算，
                   所以旧键名既不解析、也不据它推算任何值 —— 直接忽略即可，
                   升级机制会补上新的 WheelScrollMul（老配置里那行 WheelScrollMs
                   会一直躺着，无害）。
                   **键名必须是 wheelscrollms**：早期这里误写成 wheelscrollmul，
                   与上面那条同名的分支重复 —— 那条永远先匹配，于是这块成了
                   永不执行的死代码，还让人误以为旧键被特殊处理过。 */
                else if (_wcsicmp(key, L"wheelscrollms") == 0) { /* 忽略（旧语义） */ }
                else if (_wcsicmp(key, L"autoscrollinterval") == 0) {
                    int v = ParseIntW(val, AUTOSCROLL_INTERVAL_DEFAULT);
                    if (v < AUTOSCROLL_INTERVAL_MIN) v = AUTOSCROLL_INTERVAL_MIN;
                    if (v > AUTOSCROLL_INTERVAL_MAX) v = AUTOSCROLL_INTERVAL_MAX;
                    g_autoScrollInterval = v;
                }
                else if (_wcsicmp(key, L"autoscrollfastms") == 0) {
                    int v = ParseIntW(val, AUTOSCROLL_FAST_DEFAULT);
                    if (v < AUTOSCROLL_FAST_MIN) v = AUTOSCROLL_FAST_MIN;
                    if (v > AUTOSCROLL_FAST_MAX) v = AUTOSCROLL_FAST_MAX;
                    g_autoScrollFastMs = v;
                }
                else if (_wcsicmp(key, L"wheelhscrollmul") == 0) {
                    int v = ParseIntW(val, WHEEL_HMUL_DEFAULT);
                    if (v < 0) v = 0;      /* 0 = 关闭横向滚动 */
                    if (v > 16) v = 16;    /* 与纵向 WheelScrollMul 同一个上限口径 */
                    g_wheelHMul = v;
                }
                else if (_wcsicmp(key, L"autoscrollsmooth") == 0)
                    g_autoScrollSmooth = ParseBoolW(val);
                else if (_wcsicmp(key, L"allowscintillaread") == 0)
                    g_allowSciRead = ParseBoolW(val);
                /* 其余 [General] 项一律忽略：本项目所有配置项都在上面具名解析。
                   不认识的键（含已废弃的旧键名）留着也只是噪音，不解析、不保存。 */
                else { /* 忽略未知/废弃键 */ }
            }
        } else if (sec == SEC_NATIVE) {
            AddEntry(line);
        } else if (sec == SEC_NONE) {
            /* 兼容早期"不分节、一行一个进程名"的写法 */
            if (!wcschr(line, L'=')) AddEntry(line);
        }
        line = nl ? nl + 1 : NULL;
    }
    free(wide);
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
            "RunAsAdmin = 1\n"
            ";\n"
            "; 日志文件大小上限（KB）：超过后保留最近的一半内容，0 = 不限制\n"
            "; 范围 0.." STR(LOG_MAX_KB_MAX) "，超出按上限钳制\n"
            "LogMaxKB = " STR(LOG_MAX_KB_DEFAULT) "\n"
            ";\n"
            "; 逐格按词跳转的最大步数：超过该距离的逐格跳转不再逐格发送，\n"
            "; 改按 OverStepMode 处理，避免方向键发太久。0 = 不限制，最大 256\n"
            "MaxArrowSteps = 8\n"
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
            "; 自动滚动是否平滑注入（1=是 / 0=否）。1 时只发\"不足一格\"的小刻度、\n"
            "; 次数更多，手感接近触控板连续滑动；个别程序不累积小刻度（完全不动）时设 0。\n"
            "AutoScrollSmooth = 1\n"
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
            "; 命中名单的程序：Caps+N / Caps+M 不做文本探测，\n"
            "; 直接发送 Ctrl+← / Ctrl+→，由该程序自身的词导航完成按词跳转（Shift 位移同样支持）。\n"
            "; 适用于文本读取不到、或读到的文本坐标与实际光标不符（按词跳转超调、光标乱飘）的程序。\n"
            "; 每行一个进程名：完全匹配、大小写不敏感、.exe 后缀可写可不写\n"
            "Obsidian.exe";
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
      "RunAsAdmin = 1\n" },
    { "logmaxkb",
      "\n"
      "; 日志文件大小上限（KB）：超过后保留最近的一半内容，0 = 不限制\n"
      "; 范围 0.." STR(LOG_MAX_KB_MAX) "，超出按上限钳制\n"
      "LogMaxKB = " STR(LOG_MAX_KB_DEFAULT) "\n" },
    { "maxarrowsteps",
      "\n"
      "; 逐格按词跳转的最大步数：超过该距离的逐格跳转不再逐格发送，\n"
      "; 改按 OverStepMode 处理，避免方向键发太久。0 = 不限制，最大 256\n"
      "MaxArrowSteps = 8\n" },
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
    { "autoscrollsmooth",
      "\n"
      "; 自动滚动是否平滑注入（1=是 / 0=否）。1 时只发\"不足一格\"的小刻度、\n"
      "; 次数更多，手感接近触控板连续滑动；个别程序不累积小刻度（完全不动）时设 0。\n"
      "AutoScrollSmooth = " STR(AUTOSCROLL_SMOOTH_DEFAULT) "\n" },
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

/* ---- 带边界检查的输出缓冲 ----
   本项目已两次栽在"手工计算分配容量"上（见 776f468 与 ConfigUpgradeFile
   的 BOM 漏算）：容量一旦算错，后面每一次 memcpy 都是堆越界写。这里把
   "按索引写裸缓冲"换成"追加 + 容量校验"，算错时退化为不写而不是越界。 */
typedef struct { char* p; size_t n, cap; } OutBuf;

static BOOL OutInit(OutBuf* b, size_t cap) {
    b->n = 0; b->cap = cap;
    b->p = (char*)malloc(cap ? cap : 1);
    return b->p != NULL;
}
static BOOL OutAdd(OutBuf* b, const void* src, size_t len) {
    if (!b->p || b->n + len > b->cap) return FALSE;
    if (len) memcpy(b->p + b->n, src, len);
    b->n += len;
    return TRUE;
}
static BOOL OutAddCh(OutBuf* b, char c)          { return OutAdd(b, &c, 1); }
static void OutFree(OutBuf* b) { free(b->p); b->p = NULL; b->n = b->cap = 0; }

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
    char* raw = (char*)malloc((size_t)sz + 1);
    if (!raw) { fclose(f); return; }
    size_t rd = fread(raw, 1, (size_t)sz, f);
    fclose(f);
    raw[rd] = 0;

    const char* body = raw;
    size_t bodyLen = rd;
    int hasBom = (rd >= 3 && (unsigned char)raw[0] == 0xEF &&
                  (unsigned char)raw[1] == 0xBB && (unsigned char)raw[2] == 0xBF);
    if (hasBom) { body = raw + 3; bodyLen = rd - 3; }

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
    size_t blockLens[KUP_N];
    int nMiss = 0;
    for (int i = 0; i < KUP_N; i++) {
        blockLens[i] = strlen(kUpgrades[i].block);
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
    if (nMiss == 0) { free(raw); return; }   /* 无缺失：不动文件 */

    /* 组装新内容：BOM + 头部 + [General] 行(若新建) + 缺失块 + 尾部。
       容量逐项算清：BOM 只在 hasBom 时占用，且它**不计入 bodyLen**
       （body 已经跳过 BOM）—— 漏算它就是越界写（Debug CRT 下 abort）。 */
    static const char kGenHdr[] = "[General]\n";
    size_t insOff = genStart ? (size_t)(genEnd - body) : 0;
    int needNl = (genStart && insOff > 0 && body[insOff - 1] != '\n');
    size_t blockTotal = 0;
    for (int i = 0; i < KUP_N; i++)
        if (missing[i]) blockTotal += blockLens[i];

    size_t bomLen = hasBom ? 3 : 0;
    size_t genHdr = genStart ? 0 : sizeof(kGenHdr) - 1;
    size_t total  = bomLen + insOff + genHdr + (needNl ? 1u : 0u)
                  + blockTotal + (bodyLen - insOff);

    OutBuf ob;
    if (!OutInit(&ob, total)) { free(raw); return; }
    BOOL ok = TRUE;
    if (hasBom)              ok = OutAdd(&ob, "\xEF\xBB\xBF", 3);
    if (ok)                  ok = OutAdd(&ob, body, insOff);
    if (ok && !genStart)     ok = OutAdd(&ob, kGenHdr, genHdr);
    if (ok && needNl)        ok = OutAddCh(&ob, '\n');
    for (int i = 0; ok && i < KUP_N; i++)
        if (missing[i])      ok = OutAdd(&ob, kUpgrades[i].block, blockLens[i]);
    if (ok)                  ok = OutAdd(&ob, body + insOff, bodyLen - insOff);

    if (ok && _wfopen_s(&f, path, L"wb") == 0 && f) {
        fwrite(ob.p, 1, ob.n, f);
        fclose(f);
    }
    OutFree(&ob);
    free(raw);
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

BOOL ConfigGetAutoScrollSmooth(void) {
    return g_autoScrollSmooth;
}

BOOL ConfigGetAllowScintillaRead(void) {
    return g_allowSciRead;
}

int ConfigGetWheelHScrollMul(void) {
    return g_wheelHMul;
}

/* 进程可执行文件名是否命中原生按词跳转名单（完全匹配，.exe 可选） */
BOOL ConfigMatchName(const WCHAR* exeName) {
    if (g_count == 0 || !exeName || !exeName[0]) return FALSE;
    WCHAR name[CFG_NAME_MAX];
    _snwprintf_s(name, CFG_NAME_MAX, _TRUNCATE, L"%s", exeName);
    StripExeW(name);
    for (int i = 0; i < g_count; i++)
        if (_wcsicmp(name, g_names[i]) == 0) return TRUE;
    return FALSE;
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
    if (g_count == 0 || !focus) return FALSE;
    WCHAR fname[MAX_PATH];
    if (!ConfigGetProcessName(focus, fname, MAX_PATH)) return FALSE;
    return ConfigMatchName(fname);
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
