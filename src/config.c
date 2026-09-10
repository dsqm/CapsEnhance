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
   为什么不跟纵向一样默认 2：纵向默认 2 是"补一格让滚动更跟手"，而横向
   默认放大反而会让横向移动过头（列宽通常远小于行高，横向更容易"越过目标"）。 */
#define WHEEL_HMUL_DEFAULT 1
#define STR2(x) #x
#define STR(x)  STR2(x)

/* 滚轮滚动的**最快速度**：每 N 毫秒滚一格 —— 与 AutoScrollInterval **同一个单位**
   （毫秒/格），**值越小越快**。这一个值同时管两件事（本来就是"同一个速度"的两种用法）：
     · 「Caps + 滚轮」：按住 Caps 滚滚轮，就按这个节奏连续滚动；滚轮一停立即停；
     · 「Caps + Alt/D + 滚轮」：自动滚动从这里往上的**上限** —— 只会更慢，不会更快。
   0 = 关闭上面两个纵向滚轮功能（滚轮完全退回系统原生行为；横向另看 WheelHScrollMul）。
   默认 100 = 10 格/秒 = 30 行/秒 ≈ 1800 行/分钟（按系统每次滚 3 行）："快速下翻"的手感。
   注意历史值 1000（1 格/秒）比人正常滚轮还慢 —— 于是"按住 Caps 增强"反而更慢了，
   这正是它被改掉的原因。嫌快就调大、嫌慢就调小。 */
#define WHEEL_MS_DEFAULT 100
#define WHEEL_MS_MIN     0      /* 0 = 关闭（见上）；下限只管住"负数"这种非法写法 */
#define WHEEL_MS_MAX     60000

/* 自动滚动的**初始**节拍（ms）：启动后每 N 毫秒滚"一格"，即最慢档的速度。
   同向继续滚会逐档提速，**最快只到 WheelScrollMs**（跟手档的速度）
   —— 所以本值是"初始速度"，WheelScrollMs 是"最快速度"。

   默认值的依据是**人的实际阅读速度**，不是手感试出来的（前几版就是靠试，
   结果 120 -> 300 -> 500 -> 4000 改了四轮都还偏快）：
     · 成人中文默读约 300..500 字/分钟（阅读心理学通行的量级；
       《义务教育语文课程标准》对 7-9 年级的要求是"每分钟不少于 500 字"）
     · 需要边读边想的**技术文本/精读**约 80..150 字/分钟
     · 中文朗读 150..250 字/分钟（电视播音 240 左右），比默读慢得多
   换算假设：Windows 默认"每次滚 3 行"，一行按 45 字（最大化窗口的中间值），
   于是  字/分钟 ≈ 135000 / 本值(ms) ：
        每格ms   行/分钟   字/分钟
         120     1500     67500   <- 最初的值，比人快两个数量级
         500      360     16200
        4000       45      2025   <- 上一版，仍是人的 4~7 倍，所以还是偏快
       18000       10       450   <- 通用默读
       24000      7.5       338   <- **默认**：落在 300..500 的偏慢半区，
                                      对应"边读边理解"的舒适区
       60000        3       135   <- 技术精读（80..150 字/分钟）
   嫌慢就再滚一下提速，或把本值调小（6 档从本值一路缩到 WheelScrollMs）。 */
#define AUTOSCROLL_INTERVAL_DEFAULT 24000

/* AutoScrollInterval 的可调范围（ms）。下限 100 对应" 1350 行/分钟"这种
   非人速的扫读，留着只是不设人为障碍；上限 60000（3 行/分钟 ≈ 135 字/分钟）
   覆盖"技术精读"这一档 —— 只要有需求就不该被上限挡住。 */
#define AUTOSCROLL_INTERVAL_MIN 100
#define AUTOSCROLL_INTERVAL_MAX 60000

/* 自动滚动是否用"高分频"平滑注入（默认开）。
   标准滚轮刻度 WHEEL_DELTA = 120 是"一格"，但 Windows 明确支持**小于**
   WHEEL_DELTA 的增量（文档：zDelta 是 WHEEL_DELTA 的"倍数或约数"）——
   触控板/自由滚轮就是这么滚的：次数多、每次小。于是同一速度下可以有更多
   注入点、每次只挪一点点，手感从"一格一格跳"变成连续滑动。
   个别程序对小数刻度不做累积（表现为完全不动），把这里设 0 退回整格模式。 */
#define AUTOSCROLL_SMOOTH_DEFAULT 1

static WCHAR g_names[CFG_MAX_ENTRIES][CFG_NAME_MAX];
static int   g_count = 0;
static BOOL  g_runAsAdmin = FALSE;
static int   g_logMaxKB   = 2048;   /* 日志大小上限（KB），0 = 不限制 */
static int   g_maxArrowSteps = 32;  /* 逐格词跳最大步数，超过按 overStepMode 处理 */
static BOOL  g_overStepCtrl = FALSE; /* TRUE=超限改发 Ctrl+方向（原生词跳） */
static int   g_wheelMs = WHEEL_MS_DEFAULT;    /* 滚轮最快速度：毫秒/格，0=关闭纵向滚轮增强 */
static int   g_autoScrollInterval = AUTOSCROLL_INTERVAL_DEFAULT; /* 自动滚动初始节拍(ms) */
static BOOL  g_autoScrollSmooth = AUTOSCROLL_SMOOTH_DEFAULT;     /* 自动滚动平滑注入 */
static int   g_wheelHMul = WHEEL_HMUL_DEFAULT; /* Caps+Shift+滚轮 的横向滚动倍数，0=关闭 */

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
    g_runAsAdmin = FALSE;
    g_logMaxKB = 2048;
    g_maxArrowSteps = 32;
    g_overStepCtrl = FALSE;
    g_wheelMs = WHEEL_MS_DEFAULT;
    g_autoScrollInterval = AUTOSCROLL_INTERVAL_DEFAULT;
    g_autoScrollSmooth = AUTOSCROLL_SMOOTH_DEFAULT;
    g_wheelHMul = WHEEL_HMUL_DEFAULT;

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

        if (line[0] == L';' || line[0] == L'#' || !line[0]) {
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
                else if (_wcsicmp(key, L"logmaxkb") == 0) g_logMaxKB = ParseIntW(val, 2048);
                else if (_wcsicmp(key, L"maxarrowsteps") == 0) g_maxArrowSteps = ParseIntW(val, 32);
                else if (_wcsicmp(key, L"overstepmode") == 0) g_overStepCtrl = ParseBoolW(val);
                else if (_wcsicmp(key, L"wheelscrollms") == 0) {
                    int v = ParseIntW(val, WHEEL_MS_DEFAULT);
                    if (v < WHEEL_MS_MIN) v = WHEEL_MS_MIN;   /* 负数按非法处理 */
                    if (v > WHEEL_MS_MAX) v = WHEEL_MS_MAX;
                    g_wheelMs = v;
                }
                /* 兼容旧键名 WheelScrollMul（倍数语义）：**不**换算成毫秒 ——
                   "放大 2 倍"和"每格 2 毫秒"是两种量，硬换算只会得到荒谬的值。
                   旧键留在这里被忽略；升级机制会补上新的 WheelScrollMs。 */
                else if (_wcsicmp(key, L"wheelscrollmul") == 0) { /* 忽略（旧语义） */ }
                else if (_wcsicmp(key, L"autoscrollinterval") == 0) {
                    int v = ParseIntW(val, AUTOSCROLL_INTERVAL_DEFAULT);
                    if (v < AUTOSCROLL_INTERVAL_MIN) v = AUTOSCROLL_INTERVAL_MIN;
                    if (v > AUTOSCROLL_INTERVAL_MAX) v = AUTOSCROLL_INTERVAL_MAX;
                    g_autoScrollInterval = v;
                }
                else if (_wcsicmp(key, L"wheelhscrollmul") == 0) {
                    int v = ParseIntW(val, WHEEL_HMUL_DEFAULT);
                    if (v < 0) v = 0;      /* 0 = 关闭横向滚动 */
                    if (v > 16) v = 16;    /* 与纵向 WheelScrollMul 同一个上限口径 */
                    g_wheelHMul = v;
                }
                else if (_wcsicmp(key, L"autoscrollsmooth") == 0)
                    g_autoScrollSmooth = ParseBoolW(val);
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
            "; ============================================================\n"
            "; CapsEnhance 配置文件\n"
            "; ============================================================\n"
            "; ; 或 # 开头的行是注释；空行忽略；[段名] 行用于切段\n"
            "; 改动后需重启程序生效（或用托盘菜单的\"重启程序\"）\n"
            ";\n"
            "\n"
            "[General]\n"
            "; 以管理员身份运行：启用后启动时自动申请管理员权限（会弹 UAC）\n"
            "; 只认 0/1：1=启用，0=禁用（其它写法一律视为 0）\n"
            "RunAsAdmin = 0\n"
            "\n"
            "; 日志文件大小上限（KB）：超过后保留最近的一半内容，0 = 不限制\n"
            "LogMaxKB = 2048\n"
            "\n"
            "; 逐格词跳的最大步数（字符数）：单次要发的方向键达到这个数就不再逐格，\n"
            "; 改按 OverStepMode 处理，避免方向键发太久。0 = 不限制（逐格发完）。\n"
            "; 无论填多少，单次逐格发送另有 256 步的硬上限\n"
            "MaxArrowSteps = 32\n"
            "\n"
            "; 超过 MaxArrowSteps 时的替代方式，只认 0/1：\n"
            ";   0 = 只发送一个方向键（跳一格即停）\n"
            ";   1 = 改发 Ctrl+方向键（由目标程序自己的词跳完成，瞬时）\n"
            "OverStepMode = 0\n"
            "\n"
            "; ============ 滚轮（单位「毫秒/格」，越小越快）============\n"
            "; 滚轮滚动的**最快速度**：每 N 毫秒滚一格。\n"
            ";   · 按住 C 滚滚轮 = 就按这个节奏连续滚，滚轮停 / 反向 / 松开 C 立即停；\n"
            ";   · 按住 C + Alt/D 滚滚轮 = 自动滚动，从这里起步慢慢滚、逐档提速，\n"
            ";     但**这个值就是上限**（只会更慢，不会更快）。\n"
            "; 0 = 关闭上面两个纵向滚轮功能（滚轮完全按系统原本行为）。\n"
            "; 缺省 100 = 10 格/秒 = 30 行/秒 ≈ 1800 行/分钟（按每次滚 3 行）；\n"
            "; 嫌快就调大、嫌慢就调小。可调范围 0.."
            STR(WHEEL_MS_MAX) "。\n"
            "; 只做输入注入，**不改系统设置**（不碰 SPI_SETWHEELSCROLLLINES），也不写注册表。\n"
            "WheelScrollMs = " STR(WHEEL_MS_DEFAULT) "\n"
            "\n"
            "; 自动滚动的初始速度（毫秒/格）：越大越慢。同向继续滚会逐档提速，\n"
            "; 最快只到 WheelScrollMs（跟手档的速度），不会更快。\n"
            "; 缺省 24000 = 7.5 行/分钟 ≈ 338 字/分钟，落在成人中文默读\n"
            "; 300..500 字/分钟的舒适区；最慢可到 " STR(AUTOSCROLL_INTERVAL_MAX)
            "（约 135 字/分钟，技术精读）。\n"
            "; 可调范围 " STR(AUTOSCROLL_INTERVAL_MIN) ".." STR(AUTOSCROLL_INTERVAL_MAX) "。\n"
            "AutoScrollInterval = " STR(AUTOSCROLL_INTERVAL_DEFAULT) "\n"
            "\n"
            "; 自动滚动是否平滑注入（1=是 / 0=否）。1 时只发\"不足一格\"的小刻度、\n"
            "; 次数更多，手感接近触控板连续滑动；个别程序不累积小刻度（完全不动）时设 0。\n"
            "AutoScrollSmooth = " STR(AUTOSCROLL_SMOOTH_DEFAULT) "\n"
            "\n"
            "; 横向滚动：按住 C + Shift 滚滚轮，横向挪动。倍数 1 = 一滚一格（默认），\n"
            "; 调大更快，0 = 关闭横向滚动（该组合退回原来的纵向语义）。\n"
            "; 方向同浏览器：滚轮向上 = 向左，滚轮向下 = 向右。\n"
            "WheelHScrollMul = " STR(WHEEL_HMUL_DEFAULT) "\n"
            "\n"
            "[NativeWordJump]\n"
            "; 命中名单的程序：Caps+N / Caps+M 不做文本探测，\n"
            "; 直接发送 Ctrl+\xe2\x86\x90 / Ctrl+\xe2\x86\x92，\n"
            "; 由该程序自身的词导航完成按词跳转（Shift 位移同样支持）。\n"
            "; 适用于文本读取不到、或读到的文本坐标与实际光标不符\n"
            "; （跳词超调、光标乱飘）的程序。\n"
            "; 每行一个进程名：完全匹配、大小写不敏感、.exe 后缀可写可不写\n"
            ";\n"
            "; 示例（去掉行首 ; 即可生效）：\n"
            "; Editor.exe\n"
            "; SomeApp\n";
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
      "; 只认 0/1：1=启用，0=禁用（其它写法一律视为 0）\n"
      "RunAsAdmin = 0\n" },
    { "logmaxkb",
      "\n"
      "; 日志文件大小上限（KB）：超过后保留最近的一半内容，0 = 不限制\n"
      "LogMaxKB = 2048\n" },
    { "maxarrowsteps",
      "\n"
      "; 逐格词跳的最大步数（字符数）：超过该距离的逐格跳转不再逐格发送，\n"
      "; 改按 OverStepMode 处理，避免方向键发太久。0 = 不限制（逐格发完）。\n"
      "; 无论填多少，单次逐格发送另有 256 步的硬上限\n"
      "MaxArrowSteps = 32\n" },
    { "overstepmode",
      "\n"
      "; 超过 MaxArrowSteps 时的替代方式，只认 0/1：\n"
      ";   0 = 只发送一个方向键（跳一格即停）\n"
      ";   1 = 改发 Ctrl+方向键（由目标程序自己的词跳完成，瞬时）\n"
      "OverStepMode = 0\n" },
    { "wheelscrollms",
      "\n"
      "; 滚轮滚动的**最快速度**：每 N 毫秒滚一格（与 AutoScrollInterval 同单位）。\n"
      ";   · 按住 C 滚滚轮 = 就按这个节奏连续滚，滚轮停 / 反向 / 松开 C 立即停；\n"
      ";   · 按住 C + Alt/D 滚滚轮 = 自动滚动，从这里起步、逐档提速，\n"
      ";     但**这个值就是上限**（只会更慢，不会更快）。\n"
      "; 0 = 关闭上面两个纵向滚轮功能（滚轮按系统原本行为）。\n"
      "; 缺省 100 = 10 格/秒 = 30 行/秒 ≈ 1800 行/分钟；嫌快就调大、嫌慢就调小。\n"
      "; 范围 0.."
      STR(WHEEL_MS_MAX) "。\n"
      "; 注：旧版这里是倍数项 WheelScrollMul，语义不同，无法自动换算，已废弃。\n"
      "WheelScrollMs = " STR(WHEEL_MS_DEFAULT) "\n" },
    { "autoscrollinterval",
      "\n"
      "; 自动滚动的初始速度（毫秒/格，越大越慢）：同向继续滚会逐档提速，\n"
      "; 最快只到 WheelScrollMs（跟手档的速度），不会更快。\n"
      "; 可调范围 " STR(AUTOSCROLL_INTERVAL_MIN) ".." STR(AUTOSCROLL_INTERVAL_MAX) "。\n"
      "AutoScrollInterval = " STR(AUTOSCROLL_INTERVAL_DEFAULT) "\n" },
    { "autoscrollsmooth",
      "\n"
      "; 自动滚动是否平滑注入（1=是 / 0=否）。1 时只发\"不足一格\"的小刻度、\n"
      "; 次数更多，手感接近触控板连续滑动；个别程序不累积小刻度（完全不动）时设 0。\n"
      "AutoScrollSmooth = " STR(AUTOSCROLL_SMOOTH_DEFAULT) "\n" },
    { "wheelhscrollmul",
      "\n"
      "; 横向滚动：按住 C + Shift 滚滚轮，横向挪动。倍数 1 = 一滚一格（默认），\n"
      "; 调大更快，0 = 关闭横向滚动（该组合退回原来的纵向语义）。\n"
      "; 方向同浏览器：滚轮向上 = 向左，滚轮向下 = 向右。\n"
      "WheelHScrollMul = " STR(WHEEL_HMUL_DEFAULT) "\n" },
};

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
static BOOL OutAddStr(OutBuf* b, const char* s) { return OutAdd(b, s, strlen(s)); }
static BOOL OutAddCh(OutBuf* b, char c)          { return OutAdd(b, &c, 1); }
static void OutFree(OutBuf* b) { free(b->p); b->p = NULL; b->n = b->cap = 0; }

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
        const char* nl = memchr(line, '\n', (size_t)(body + bodyLen - line));
        size_t ll = nl ? (size_t)(nl - line) : (size_t)(body + bodyLen - line);
        char tmp[160];
        size_t n = ll < 159 ? ll : 159;
        memcpy(tmp, line, n); tmp[n] = 0;
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
            const char* nl = memchr(l, '\n', (size_t)(body + bodyLen - l));
            size_t ll = nl ? (size_t)(nl - l) : (size_t)(body + bodyLen - l);
            char tmp[160];
            size_t n = ll < 159 ? ll : 159;
            memcpy(tmp, l, n); tmp[n] = 0;
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

int ConfigGetWheelScrollMs(void) {
    return g_wheelMs;
}

int ConfigGetAutoScrollInterval(void) {
    return g_autoScrollInterval;
}

BOOL ConfigGetAutoScrollSmooth(void) {
    return g_autoScrollSmooth;
}

int ConfigGetWheelHScrollMul(void) {
    return g_wheelHMul;
}

/* 进程可执行文件名是否命中原生词跳名单（完全匹配，.exe 可选） */
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
