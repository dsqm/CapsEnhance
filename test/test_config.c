/* ============================================================
   配置模块单元测试：分节、键值、注释、布尔写法、进程名完全匹配、
   模板内容（不得出现禁止字样）
   ============================================================ */
#include "cap.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* 被测接口全部在 cap.h 声明（本文件 include 了它），这里不再抄一份 ——
   抄一份的下场是改名之后一半原型漏改、报一堆"没有匹配的重载"。 */

/* 模板期望值里拼默认值用（与 src/config.c 同款，模板改默认值这里自动跟） */
#define STR2(x) #x
#define STR(x)  STR2(x)


static int fails = 0;
#define EXPECT(cond, name) do { \
    if (cond) printf("  [OK]   %s\n", name); \
    else      { printf("  [FAIL] %s\n", name); fails++; } } while (0)

static void WriteIni(const char* body) {
    FILE* f = NULL;
    if (_wfopen_s(&f, L"obj/test/cfgtest.ini", L"wb") == 0 && f) {
        fwrite(body, 1, strlen(body), f);
        fclose(f);
    }
}

/* 读回刚生成的模板，检查禁止字样 */
static BOOL TemplateHasForbidden(void) {
    FILE* f = NULL;
    if (_wfopen_s(&f, L"obj/test/CapsEnhance.ini", L"rb") != 0 || !f) return TRUE;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    char* buf = (char*)malloc((size_t)sz + 1);
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[n] = 0;
    BOOL bad = (strstr(buf, "VSCode") != NULL) || (strstr(buf, "vsc") != NULL);
    free(buf);
    return bad;
}

/* 生成的模板里是否含某个键（连默认值一起匹配）—— 新增配置项忘了写进模板
   的话，用户根本不知道有这个开关 */
static BOOL TemplateHas(const char* needle) {
    FILE* f = NULL;
    if (_wfopen_s(&f, L"obj/test/CapsEnhance.ini", L"rb") != 0 || !f) return FALSE;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    char* buf = (char*)malloc((size_t)sz + 1);
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[n] = 0;
    BOOL has = strstr(buf, needle) != NULL;
    free(buf);
    return has;
}

static int CountStr(const char* hay, const char* needle) {
    int n = 0;
    size_t nl = strlen(needle);
    for (const char* p = hay; (p = strstr(p, needle)) != NULL; p += nl) n++;
    return n;
}

int main(void) {
    /* 完整格式：General 键值 + NativeWordJump 名单 */
    WriteIni(
        "; 注释\r\n"
        "; 也是注释\r\n"
        "\r\n"
        "[General]\r\n"
        "RunAsAdmin = 1\r\n"
        "; 其他段\r\n"
        "[Other]\r\n"
        "RunAsAdmin = 0        ; 其它段的同名键不应影响 General\r\n"
        "\r\n"
        "[NativeWordJump]\r\n"
        "Editor.exe\r\n"
        "SomeApp\r\n"
        "  Other.EXE  \r\n"
        "; Ghost.exe  <- 注释里的名字不生效\r\n"
    );
    ConfigLoadFrom(L"obj/test/cfgtest.ini");

    EXPECT(CfgBool(CFG_RUN_AS_ADMIN), "CFG: RunAsAdmin=1 生效");
    EXPECT(ConfigMatchName(L"Editor.exe"),   "CFG: Editor.exe 命中（名单带 .exe）");
    EXPECT(ConfigMatchName(L"editor"),       "CFG: editor 命中（大小写不敏感）");
    EXPECT(ConfigMatchName(L"SomeApp.exe"),  "CFG: SomeApp 命中（名单不带 .exe）");
    EXPECT(ConfigMatchName(L"OTHER"),        "CFG: Other.EXE 命中（大小写+空白容错）");
    EXPECT(!ConfigMatchName(L"Ghost.exe"),   "CFG: 注释里的名字不生效");
    EXPECT(!ConfigMatchName(L"ditor"),       "CFG: 子串不命中（完全匹配）");
    EXPECT(!ConfigMatchName(L"SomeApp2"),    "CFG: 前缀同名不命中");
    EXPECT(!ConfigMatchName(L""),            "CFG: 空名不命中");

    /* Other 段的同名键不应被当成 General */
    WriteIni("[General]\r\nRunAsAdmin = 1\r\n[Other]\r\nRunAsAdmin = 0\r\n");
    ConfigLoadFrom(L"obj/test/cfgtest.ini");
    EXPECT(CfgBool(CFG_RUN_AS_ADMIN), "CFG: 非 General 段的同名键不覆盖");

    /* 布尔只认 0/1 */
    WriteIni("[General]\r\nRunAsAdmin = 1\r\n");
    ConfigLoadFrom(L"obj/test/cfgtest.ini");
    EXPECT(CfgBool(CFG_RUN_AS_ADMIN), "CFG: 1 视为真");
    WriteIni("[General]\r\nRunAsAdmin = true\r\n");
    ConfigLoadFrom(L"obj/test/cfgtest.ini");
    EXPECT(!CfgBool(CFG_RUN_AS_ADMIN), "CFG: true 不生效（只认 0/1）");
    WriteIni("[General]\r\nRunAsAdmin = \xe5\x90\xaf\xe7\x94\xa8\r\n");  /* 启用 */
    ConfigLoadFrom(L"obj/test/cfgtest.ini");
    EXPECT(!CfgBool(CFG_RUN_AS_ADMIN), "CFG: 中文“启用”不生效（只认 0/1）");
    WriteIni("[General]\r\nRunAsAdmin = 01\r\n");
    ConfigLoadFrom(L"obj/test/cfgtest.ini");
    EXPECT(!CfgBool(CFG_RUN_AS_ADMIN), "CFG: 01 不生效（严格等于 1）");
    WriteIni("[General]\r\nRunAsAdmin = 0\r\n");
    ConfigLoadFrom(L"obj/test/cfgtest.ini");
    EXPECT(!CfgBool(CFG_RUN_AS_ADMIN), "CFG: 0 视为假");
    WriteIni("[General]\r\nRunAsAdmin =\r\n");
    ConfigLoadFrom(L"obj/test/cfgtest.ini");
    EXPECT(!CfgBool(CFG_RUN_AS_ADMIN), "CFG: 空值视为假");
    WriteIni("[General]\r\n\r\n");
    ConfigLoadFrom(L"obj/test/cfgtest.ini");
    /* ★ 缺省已改为**不提权**（RunAsAdmin = 0）：普通用法不需要管理员，
       只有要拖动/操作"以管理员运行的窗口"时才需要设 1（UIPI 会拦住低权限进程对高权限窗口的
       SetWindowPos）。这条断言就是那个默认值的保险丝。 */
    EXPECT(!CfgBool(CFG_RUN_AS_ADMIN), "CFG: 缺省为假（缺省配置就是 RunAsAdmin = 0）");

    /* 日志上限 LogMaxKB（KB） */
    WriteIni("[General]\r\nLogMaxKB = 512\r\n");
    ConfigLoadFrom(L"obj/test/cfgtest.ini");
    EXPECT(CfgInt(CFG_LOG_MAX_KB) == 512, "CFG: LogMaxKB=512 生效");
    WriteIni("[General]\r\nLogMaxKB = 0\r\n");
    ConfigLoadFrom(L"obj/test/cfgtest.ini");
    EXPECT(CfgInt(CFG_LOG_MAX_KB) == 0, "CFG: LogMaxKB=0（不限制）");
    WriteIni("[General]\r\nLogMaxKB = abc\r\n");
    ConfigLoadFrom(L"obj/test/cfgtest.ini");
    EXPECT(CfgInt(CFG_LOG_MAX_KB) == 2048, "CFG: LogMaxKB 非法值回落默认 2048");
    /* 上限钳制是**安全**要求而不是口味：main.c 的日志轮转按"上限的一半"分配
       缓冲（keep = maxKB*512），不钳的话 (size_t)keep 在 32 位构建下会回绕成
       一个小数 —— malloc 拿到小缓冲、fread 却按回绕前的长度写进去，就是堆越界。
       钳住上限让这条路径永远不可能触发（当前 x64 构建本来就到不了，但"到不了"
       和"不可能"是两回事）。 */
    WriteIni("[General]\r\nLogMaxKB = 99999999\r\n");
    ConfigLoadFrom(L"obj/test/cfgtest.ini");
    EXPECT(CfgInt(CFG_LOG_MAX_KB) == 65536, "CFG: LogMaxKB 超过上限钳到 65536");
    WriteIni("[General]\r\nRunAsAdmin = 1\r\n[Other]\r\nLogMaxKB = 1\r\n");
    ConfigLoadFrom(L"obj/test/cfgtest.ini");
    EXPECT(CfgInt(CFG_LOG_MAX_KB) == 2048, "CFG: 非 General 段的 LogMaxKB 不生效");

    /* UTF-8 BOM + LF 行尾（Linux 编辑器保存的格式） */
    {
        FILE* fb = NULL;
        if (fopen_s(&fb, "obj/test/cfgtest.ini", "wb") == 0 && fb) {
            fputc(0xEF, fb); fputc(0xBB, fb); fputc(0xBF, fb);   /* BOM */
            fputs("[General]\nRunAsAdmin = 1\n\n[NativeWordJump]\nBomApp\n", fb);
            fclose(fb);
        }
        ConfigLoadFrom(L"obj/test/cfgtest.ini");
        EXPECT(CfgBool(CFG_RUN_AS_ADMIN) && ConfigMatchName(L"bomapp"),
               "CFG: UTF-8 BOM + LF 行尾");
    }

    /* 逐格阈值与超限模式 */
    WriteIni("[General]\r\nMaxArrowSteps = 16\r\nOverStepMode = 1\r\n");
    ConfigLoadFrom(L"obj/test/cfgtest.ini");
    EXPECT(CfgInt(CFG_MAX_ARROW_STEPS) == 16 && CfgBool(CFG_OVER_STEP_CTRL),
           "CFG: MaxArrowSteps=16 + OverStepMode=1 生效");
    WriteIni("[General]\r\nMaxArrowSteps = 0\r\nOverStepMode = 0\r\n");
    ConfigLoadFrom(L"obj/test/cfgtest.ini");
    EXPECT(CfgInt(CFG_MAX_ARROW_STEPS) == 0 && !CfgBool(CFG_OVER_STEP_CTRL),
           "CFG: MaxArrowSteps=0(不限制) + OverStepMode=0 生效");
    WriteIni("[General]\r\nMaxArrowSteps = abc\r\nOverStepMode = xyz\r\n");
    ConfigLoadFrom(L"obj/test/cfgtest.ini");
    EXPECT(CfgInt(CFG_MAX_ARROW_STEPS) == 4 && !CfgBool(CFG_OVER_STEP_CTRL),
           "CFG: 非法值回落默认 (4 / OverStepMode=1)");
    /* [Other] 里写与缺省**不同**的值，才能证明它真的没生效
       （缺省 MaxArrowSteps=4 / OverStepMode=1）。 */
    WriteIni("[Other]\r\nMaxArrowSteps = 1\r\nOverStepMode = 0\r\n");
    ConfigLoadFrom(L"obj/test/cfgtest.ini");
    EXPECT(CfgInt(CFG_MAX_ARROW_STEPS) == 4 && CfgBool(CFG_OVER_STEP_CTRL),
           "CFG: 非 General 段不生效（仍取缺省 4 / 1）");

    /* 按住 C 时滚轮的放大倍数 WheelScrollMul（只管 Caps+滚轮；与自动滚动无关）。
       样本一律用与默认值不同的值（默认值当样本会让"自定义生效"恒真）。 */
    WriteIni("[General]\r\nWheelScrollMul = 5\r\n");
    ConfigLoadFrom(L"obj/test/cfgtest.ini");
    EXPECT(CfgInt(CFG_WHEEL_SCROLL_MUL) == 5, "CFG: WheelScrollMul=5 生效");
    WriteIni("[General]\r\nWheelScrollMul = 0\r\n");
    ConfigLoadFrom(L"obj/test/cfgtest.ini");
    EXPECT(CfgInt(CFG_WHEEL_SCROLL_MUL) == 0, "CFG: WheelScrollMul=0（不放大）");
    WriteIni("[General]\r\nWheelScrollMul = 1\r\n");
    ConfigLoadFrom(L"obj/test/cfgtest.ini");
    EXPECT(CfgInt(CFG_WHEEL_SCROLL_MUL) == 1, "CFG: WheelScrollMul=1（不放大）");
    WriteIni("[General]\r\nWheelScrollMul = 9999\r\n");
    ConfigLoadFrom(L"obj/test/cfgtest.ini");
    EXPECT(CfgInt(CFG_WHEEL_SCROLL_MUL) == 16, "CFG: WheelScrollMul 超过上限钳到 16");
    WriteIni("[General]\r\nWheelScrollMul = abc\r\n");
    ConfigLoadFrom(L"obj/test/cfgtest.ini");
    EXPECT(CfgInt(CFG_WHEEL_SCROLL_MUL) == 3, "CFG: WheelScrollMul 非法值回落默认 3");
    WriteIni("[Other]\r\nWheelScrollMul = 9\r\n");
    ConfigLoadFrom(L"obj/test/cfgtest.ini");
    EXPECT(CfgInt(CFG_WHEEL_SCROLL_MUL) == 3, "CFG: 非 General 段的 WheelScrollMul 不生效");
    WriteIni("[General]\r\n\r\n");
    ConfigLoadFrom(L"obj/test/cfgtest.ini");
    EXPECT(CfgInt(CFG_WHEEL_SCROLL_MUL) == 3, "CFG: WheelScrollMul 缺省为 3");
    /* 过渡期的键名 WheelScrollMs（毫秒/格）已废弃：两种量不可换算，所以既不解析
       也不据它推算任何值 —— 写进去必须完全无副作用。 */
    WriteIni("[General]\r\nWheelScrollMs = 50\r\n");
    ConfigLoadFrom(L"obj/test/cfgtest.ini");
    EXPECT(CfgInt(CFG_WHEEL_SCROLL_MUL) == 3, "CFG: 废弃键 WheelScrollMs 被忽略（不参与解析）");

    /* 自动滚动基础节奏 AutoScrollInterval（ms），钳制 100..60000 */
    WriteIni("[General]\r\nAutoScrollInterval = 250\r\n");
    ConfigLoadFrom(L"obj/test/cfgtest.ini");
    EXPECT(CfgInt(CFG_AUTOSCROLL_INTERVAL) == 250, "CFG: AutoScrollInterval=250 生效");
    WriteIni("[General]\r\nAutoScrollInterval = 5\r\n");
    ConfigLoadFrom(L"obj/test/cfgtest.ini");
    EXPECT(CfgInt(CFG_AUTOSCROLL_INTERVAL) == 100, "CFG: AutoScrollInterval 下限钳到 100");
    WriteIni("[General]\r\nAutoScrollInterval = 999999\r\n");
    ConfigLoadFrom(L"obj/test/cfgtest.ini");
    EXPECT(CfgInt(CFG_AUTOSCROLL_INTERVAL) == 60000, "CFG: AutoScrollInterval 上限钳到 60000");
    WriteIni("[General]\r\nAutoScrollInterval = abc\r\n");
    ConfigLoadFrom(L"obj/test/cfgtest.ini");
    EXPECT(CfgInt(CFG_AUTOSCROLL_INTERVAL) == 1500,
           "CFG: AutoScrollInterval 非法值回落默认 1500");
    WriteIni("[Other]\r\nAutoScrollInterval = 300\r\n");   /* 非 General 段：不应生效 */
    ConfigLoadFrom(L"obj/test/cfgtest.ini");
    EXPECT(CfgInt(CFG_AUTOSCROLL_INTERVAL) == 1500,
           "CFG: 非 General 段的 AutoScrollInterval 不生效");
    WriteIni("[General]\r\n\r\n");
    ConfigLoadFrom(L"obj/test/cfgtest.ini");
    /* 默认值 1500 是“按住就快速浏览”的定位，不是阅读速度；想按阅读速度读
       长文要调到 24000 一带。换算表在 config.c 的
       AUTOSCROLL_INTERVAL_DEFAULT 上方 —— 改默认值前先看它，并且
       **全局搜这个数值**：注释、ini 模板、ini 升级块、本文件的断言消息
       里都可能写死（历史上就漏改过三处）。 */
    EXPECT(CfgInt(CFG_AUTOSCROLL_INTERVAL) == 1500,
           "CFG: AutoScrollInterval 缺省为 1500ms/格（约 2 格/秒）");

    /* 持续滚动的最快速度 AutoScrollFastMs（毫秒/格，越小越快）。它与初始速度
       **各自独立**：一个管起步、一个管顶档，互不牵制。 */
    WriteIni("[General]\r\nAutoScrollFastMs = 250\r\n");
    ConfigLoadFrom(L"obj/test/cfgtest.ini");
    EXPECT(CfgInt(CFG_AUTOSCROLL_FAST_MS) == 250, "CFG: AutoScrollFastMs=250 生效");
    WriteIni("[General]\r\nAutoScrollFastMs = 1\r\n");
    ConfigLoadFrom(L"obj/test/cfgtest.ini");
    EXPECT(CfgInt(CFG_AUTOSCROLL_FAST_MS) == 20, "CFG: AutoScrollFastMs 下限钳到 20");
    WriteIni("[General]\r\nAutoScrollFastMs = 999999\r\n");
    ConfigLoadFrom(L"obj/test/cfgtest.ini");
    EXPECT(CfgInt(CFG_AUTOSCROLL_FAST_MS) == 60000, "CFG: AutoScrollFastMs 上限钳到 60000");
    WriteIni("[General]\r\nAutoScrollFastMs = abc\r\n");
    ConfigLoadFrom(L"obj/test/cfgtest.ini");
    EXPECT(CfgInt(CFG_AUTOSCROLL_FAST_MS) == 60, "CFG: AutoScrollFastMs 非法值回落默认 60");
    WriteIni("[Other]\r\nAutoScrollFastMs = 99\r\n");
    ConfigLoadFrom(L"obj/test/cfgtest.ini");
    EXPECT(CfgInt(CFG_AUTOSCROLL_FAST_MS) == 60, "CFG: 非 General 段的 AutoScrollFastMs 不生效");
    WriteIni("[General]\r\n\r\n");
    ConfigLoadFrom(L"obj/test/cfgtest.ini");
    EXPECT(CfgInt(CFG_AUTOSCROLL_FAST_MS) == 60,
           "CFG: AutoScrollFastMs 缺省为 60ms/格（比初始档更快）");
    /* 两项互不影响：改一个不该动另一个（这正是"单位/数值各自独立"的要求） */
    WriteIni("[General]\r\nAutoScrollInterval = 8000\r\n");
    ConfigLoadFrom(L"obj/test/cfgtest.ini");
    EXPECT(CfgInt(CFG_AUTOSCROLL_FAST_MS) == 60 && CfgInt(CFG_AUTOSCROLL_INTERVAL) == 8000,
           "CFG: 改初始速度不影响最快速度（两项独立）");
    WriteIni("[General]\r\nAutoScrollFastMs = 300\r\n");
    ConfigLoadFrom(L"obj/test/cfgtest.ini");
    EXPECT(CfgInt(CFG_AUTOSCROLL_FAST_MS) == 300 && CfgInt(CFG_AUTOSCROLL_INTERVAL) == 1500,
           "CFG: 改最快速度不影响初始速度（两项独立）");

    /* 三份进程名名单**各自独立**：[NativeWordJump] / [AutoDisable] /
       [NoSmoothScroll]。名字互不相同，才能证明"写进 A 段的不会命中 B 段" ——
       它们语义完全不同（改通道 / 整段不介入 / 只改滚动粒度），共用一份就等于
       任一命中都会触发所有行为。 */
    WriteIni("[NativeWordJump]\r\nA1.exe\r\n[AutoDisable]\r\nB1.exe\r\n"
             "[NoSmoothScroll]\r\nC1.exe\r\n");
    ConfigLoadFrom(L"obj/test/cfgtest.ini");
    EXPECT(ConfigMatchList(CFG_LIST_NATIVE, L"A1.exe"),
           "CFG: A1 命中 NativeWordJump");
    EXPECT(ConfigMatchList(CFG_LIST_AUTODISABLE, L"B1"),
           "CFG: B1 命中 AutoDisable（.exe 后缀可省）");
    EXPECT(ConfigMatchList(CFG_LIST_NOSMOOTH, L"c1.EXE"),
           "CFG: C1 命中 NoSmoothScroll（大小写不敏感）");
    EXPECT(!ConfigMatchList(CFG_LIST_AUTODISABLE, L"A1.exe") &&
           !ConfigMatchList(CFG_LIST_NOSMOOTH, L"A1.exe"),
           "CFG: NativeWordJump 的名字不污染另两份名单");
    EXPECT(!ConfigMatchList(CFG_LIST_NATIVE, L"B1.exe") &&
           !ConfigMatchList(CFG_LIST_NATIVE, L"C1.exe"),
           "CFG: 另两份名单的名字不污染 NativeWordJump");
    EXPECT(!ConfigMatchList(CFG_LIST_AUTODISABLE, L"C1.exe"),
           "CFG: AutoDisable 与 NoSmoothScroll 互不命中");
    /* 注释不进名单（与 NativeWordJump 同一套解析） */
    WriteIni("[AutoDisable]\r\n; Ghost.exe\r\nReal.exe\r\n[NoSmoothScroll]\r\nN1.exe\r\n");
    ConfigLoadFrom(L"obj/test/cfgtest.ini");
    EXPECT(!ConfigMatchList(CFG_LIST_AUTODISABLE, L"Ghost.exe"),
           "CFG: 注释里的名字不进 AutoDisable 名单");
    EXPECT(ConfigMatchList(CFG_LIST_AUTODISABLE, L"real"),
           "CFG: AutoDisable 段里的正常名字照常命中");
    EXPECT(!ConfigMatchList(CFG_LIST_NATIVE, L"Real.exe"),
           "CFG: AutoDisable 段的名字不进 NativeWordJump");
    /* 名单编号越界：一律 FALSE（防御：调用方算错编号时不能读越界内存） */
    EXPECT(!ConfigMatchList(-1, L"A1.exe") && !ConfigMatchList(CFG_LIST_COUNT, L"A1.exe"),
           "CFG: 名单编号越界返回 FALSE");
    /* 废弃键 AutoScrollSmooth（已被 [NoSmoothScroll] 名单取代）：既不解析、
       也不据它推算任何值 —— 写进去必须完全无副作用。 */
    WriteIni("[General]\r\nAutoScrollSmooth = 0\r\n");
    ConfigLoadFrom(L"obj/test/cfgtest.ini");
    EXPECT(CfgInt(CFG_AUTOSCROLL_INTERVAL) == 1500,
           "CFG: 废弃键 AutoScrollSmooth 被忽略（滚动速度仍取缺省 1500）");

    /* 横向滚动倍数 WheelHScrollMul（Caps+Shift+滚轮）。默认必须是 **1**
       —— 一个物理格换一次横向滚动，与系统原生横向滚动一致。
       样本一律用与默认值不同的值（1 当样本会让"自定义生效"恒真）。 */
    EXPECT(CfgInt(CFG_WHEEL_HSCROLL_MUL) == 1, "CFG: WheelHScrollMul 缺省为 1");
    WriteIni("[General]\r\nWheelHScrollMul = 3\r\n");
    ConfigLoadFrom(L"obj/test/cfgtest.ini");
    EXPECT(CfgInt(CFG_WHEEL_HSCROLL_MUL) == 3, "CFG: WheelHScrollMul=3 生效");
    WriteIni("[General]\r\nWheelHScrollMul = 0\r\n");
    ConfigLoadFrom(L"obj/test/cfgtest.ini");
    EXPECT(CfgInt(CFG_WHEEL_HSCROLL_MUL) == 0, "CFG: WheelHScrollMul=0 表示关闭横向滚动");
    /* ParseIntW 只认前导十进制数字，负号不是数字 -> 当作非法值回落默认。
       （解析器里仍有 v<0 -> 0 的防御，但配置文本走不到那条路。） */
    WriteIni("[General]\r\nWheelHScrollMul = -5\r\n");
    ConfigLoadFrom(L"obj/test/cfgtest.ini");
    EXPECT(CfgInt(CFG_WHEEL_HSCROLL_MUL) == 1,
           "CFG: WheelHScrollMul 负号被当非法值 -> 回落默认 1");
    WriteIni("[General]\r\nWheelHScrollMul = 999\r\n");
    ConfigLoadFrom(L"obj/test/cfgtest.ini");
    EXPECT(CfgInt(CFG_WHEEL_HSCROLL_MUL) == 16, "CFG: WheelHScrollMul 上限钳到 16");
    WriteIni("[General]\r\nWheelHScrollMul = abc\r\n");
    ConfigLoadFrom(L"obj/test/cfgtest.ini");
    EXPECT(CfgInt(CFG_WHEEL_HSCROLL_MUL) == 1, "CFG: WheelHScrollMul 非法值回落默认 1");
    WriteIni("[Other]\r\nWheelHScrollMul = 7\r\n");
    ConfigLoadFrom(L"obj/test/cfgtest.ini");
    EXPECT(CfgInt(CFG_WHEEL_HSCROLL_MUL) == 1, "CFG: 非 General 段的 WheelHScrollMul 不生效");

    /* Scintilla 通道读目标进程内存的开关 AllowScintillaRead（默认开）。
       Scintilla 的 SCI_GETTEXT 只接受"对方进程地址空间里的缓冲区"，所以读全文
       必然要对目标进程 OpenProcess(VM_OPERATION|VM_READ) + ReadProcessMemory。
       这是实质性权限，默认开（否则 Notepad3 这类编辑器只能退化成逐格箭头），
       但要给用户一个关掉它的旋钮：关掉后整条 Scintilla 通道放弃，退回
       UIA / IA2 / 原生 Ctrl+方向（仍能跳，只是不再瞬跳）。 */
    EXPECT(CfgBool(CFG_ALLOW_SCINTILLA_READ), "CFG: AllowScintillaRead 缺省为开");
    WriteIni("[General]\r\nAllowScintillaRead = 0\r\n");
    ConfigLoadFrom(L"obj/test/cfgtest.ini");
    EXPECT(!CfgBool(CFG_ALLOW_SCINTILLA_READ), "CFG: AllowScintillaRead=0 生效（关闭该通道）");
    WriteIni("[General]\r\nAllowScintillaRead = true\r\n");
    ConfigLoadFrom(L"obj/test/cfgtest.ini");
    EXPECT(!CfgBool(CFG_ALLOW_SCINTILLA_READ),
           "CFG: AllowScintillaRead 只认 0/1，true 视为关（不是回落默认）");
    WriteIni("[Other]\r\nAllowScintillaRead = 0\r\n");
    ConfigLoadFrom(L"obj/test/cfgtest.ini");
    EXPECT(CfgBool(CFG_ALLOW_SCINTILLA_READ), "CFG: 非 General 段的 AllowScintillaRead 不生效");

    /* 单键切换输入法：ImeCapsMode / ImeLShiftMode / ImeRShiftMode
         IME_MODE_OFF(0)=不处理   IME_MODE_CN(1)=切中文   IME_MODE_EN(2)=切英文
       三个键只差"切到哪一边"这一个维度，所以是**每键一个模式值**而不是三个
       独立开关：加一个键只加一行常量，hook/ime 的判定代码一处不动。
       默认值（Caps=中文 / 左Shift=英文 / 右Shift=中文）就是需求里定的行为 ——
       这几条断言是它的保险丝（左 Shift 必须是"切英文"，写反了整个功能反着来）。 */
    WriteIni("[General]\r\n\r\n");
    ConfigLoadFrom(L"obj/test/cfgtest.ini");
    EXPECT(ConfigGetImeMode(IME_KEY_CAPS)   == IME_MODE_CN &&
           ConfigGetImeMode(IME_KEY_LSHIFT) == IME_MODE_EN &&
           ConfigGetImeMode(IME_KEY_RSHIFT) == IME_MODE_CN,
           "CFG: 输入法键缺省为 Caps=中文 / 左Shift=英文 / 右Shift=中文");
    WriteIni("[General]\r\nImeCapsMode = 0\r\nImeLShiftMode = 1\r\nImeRShiftMode = 2\r\n");
    ConfigLoadFrom(L"obj/test/cfgtest.ini");
    EXPECT(ConfigGetImeMode(IME_KEY_CAPS)   == IME_MODE_OFF &&
           ConfigGetImeMode(IME_KEY_LSHIFT) == IME_MODE_CN &&
           ConfigGetImeMode(IME_KEY_RSHIFT) == IME_MODE_EN,
           "CFG: 三个输入法键各自独立生效（0/1/2 互不干扰）");
    /* 越界钳到上限 2（IME_MODE_MAX）—— 数值走的是 ClampInt，不是"非法回落默认"。
       这里很容易写成"回落默认"，但模式值和滚轮倍数一样是**钳制**语义：
       用户写了 9，得到的是"切英文"而不是"用缺省的中文"（后者会让人以为没生效）。 */
    WriteIni("[General]\r\nImeCapsMode = 9\r\nImeLShiftMode = 9\r\nImeRShiftMode = 9\r\n");
    ConfigLoadFrom(L"obj/test/cfgtest.ini");
    EXPECT(ConfigGetImeMode(IME_KEY_CAPS)   == IME_MODE_EN &&
           ConfigGetImeMode(IME_KEY_LSHIFT) == IME_MODE_EN &&
           ConfigGetImeMode(IME_KEY_RSHIFT) == IME_MODE_EN,
           "CFG: 输入法模式超过 2 钳到 2（切英文）");
    WriteIni("[General]\r\nImeCapsMode = abc\r\nImeLShiftMode =\r\n");
    ConfigLoadFrom(L"obj/test/cfgtest.ini");
    EXPECT(ConfigGetImeMode(IME_KEY_CAPS)   == IME_MODE_CN &&
           ConfigGetImeMode(IME_KEY_LSHIFT) == IME_MODE_EN &&
           ConfigGetImeMode(IME_KEY_RSHIFT) == IME_MODE_CN,
           "CFG: 输入法模式非法/空值回落各自默认值");
    WriteIni("[Other]\r\nImeCapsMode = 0\r\nImeLShiftMode = 0\r\nImeRShiftMode = 0\r\n");
    ConfigLoadFrom(L"obj/test/cfgtest.ini");
    EXPECT(ConfigGetImeMode(IME_KEY_CAPS)   == IME_MODE_CN &&
           ConfigGetImeMode(IME_KEY_LSHIFT) == IME_MODE_EN &&
           ConfigGetImeMode(IME_KEY_RSHIFT) == IME_MODE_CN,
           "CFG: 非 General 段的输入法键不生效");
    /* 编号越界（调用方算错 IME_KEY_*）：一律"不处理"，且绝不读越界内存。
       "不处理"是最安全的缺省 —— 宁可什么都不做，也不要对一个错的键去切输入法。 */
    EXPECT(ConfigGetImeMode(-1) == IME_MODE_OFF &&
           ConfigGetImeMode(IME_KEY_COUNT) == IME_MODE_OFF,
           "CFG: 输入法键编号越界返回 0（不处理）");
    /* 写回接口同样走钳制，越界键号直接忽略（设置界面改值后 ConfigSaveFile
       落盘的必须是合法值） */
    ConfigSetImeMode(IME_KEY_CAPS, 5);
    ConfigSetImeMode(IME_KEY_LSHIFT, 0);
    ConfigSetImeMode(IME_KEY_RSHIFT, -3);
    ConfigSetImeMode(IME_KEY_COUNT, 1);          /* 越界键号：应被忽略 */
    EXPECT(ConfigGetImeMode(IME_KEY_CAPS)   == IME_MODE_EN &&
           ConfigGetImeMode(IME_KEY_LSHIFT) == IME_MODE_OFF &&
           ConfigGetImeMode(IME_KEY_RSHIFT) == IME_MODE_OFF,
           "CFG: ConfigSetImeMode 钳制到 0..2，越界键号被忽略");

    /* 激活键列表 EnhanceKeys / ShiftKeys 已**从配置里去掉**（它们是"和弦语法"
       的一部分，不是给用户调的旋钮）：解析器不再认这两个键名、KeyMapInit 也
       不读配置 —— 激活键固定为 cap.h 的 MODE_*_KEYS_DEFAULT（Alt+D / F）。
       相关行为由 test_chord 的场景 S 验证（随手写一个 ini 也不会改变键位）；
       "模板里不该再有这两项"由本文件末尾的模板用例断言。 */

    /* 老配置升级：缺失的新键自动补齐，已有内容保留 */
    {
        FILE* fo = NULL;
        if (fopen_s(&fo, "obj/test/cfgtest.ini", "wb") == 0 && fo) {
            fputs("[General]\nRunAsAdmin = 1\n\n[NativeWordJump]\nOldApp\n", fo);
            fclose(fo);
        }
        ConfigUpgradeFile(L"obj/test/cfgtest.ini");
        ConfigLoadFrom(L"obj/test/cfgtest.ini");
        EXPECT(CfgBool(CFG_RUN_AS_ADMIN) && ConfigMatchName(L"oldapp"),
               "UPG: 用户已有内容保留 (RunAsAdmin=1 / 名单)");
        EXPECT(CfgInt(CFG_LOG_MAX_KB) == 2048 && CfgInt(CFG_MAX_ARROW_STEPS) == 4,
               "UPG: 缺失键补齐默认值");
        EXPECT(CfgInt(CFG_AUTOSCROLL_INTERVAL) == 1500, "UPG: 补齐 AutoScrollInterval 默认值");
        EXPECT(CfgInt(CFG_WHEEL_SCROLL_MUL) == 3, "UPG: 补齐 WheelScrollMul 默认值");
        EXPECT(CfgInt(CFG_AUTOSCROLL_FAST_MS) == 60, "UPG: 补齐 AutoScrollFastMs 默认值");
        EXPECT(ConfigGetImeMode(IME_KEY_CAPS)   == IME_MODE_CN &&
               ConfigGetImeMode(IME_KEY_LSHIFT) == IME_MODE_EN &&
               ConfigGetImeMode(IME_KEY_RSHIFT) == IME_MODE_CN,
               "UPG: 补齐三个输入法键的默认值");
        KeyMapInit();
        EXPECT(ModeKeyCount(MODE_ENHANCE) == 2 && ModeKeyCount(MODE_SHIFT) == 1,
               "UPG: 激活键是编译期常量（Alt+D / F），与配置文件无关");
        FILE* fc = NULL;
        int hasLog = 0, hasSteps = 0, hasMode = 0, hasWheel = 0;
        int hasInterval = 0, hasHMul = 0, hasFast = 0;
        int hasImeCaps = 0, hasImeL = 0, hasImeR = 0;
        if (fopen_s(&fc, "obj/test/cfgtest.ini", "r") == 0 && fc) {
            char line[512];
            while (fgets(line, sizeof(line), fc)) {
                if (strstr(line, "LogMaxKB = 2048")) hasLog = 1;
                if (strstr(line, "MaxArrowSteps = 4")) hasSteps = 1;
                if (strstr(line, "OverStepMode = 1")) hasMode = 1;
                if (strstr(line, "WheelScrollMul = 3")) hasWheel = 1;
                if (strstr(line, "AutoScrollFastMs = 60")) hasFast = 1;
                if (strstr(line, "AutoScrollInterval = 1500")) hasInterval = 1;
                if (strstr(line, "WheelHScrollMul = 1")) hasHMul = 1;
                if (strstr(line, "ImeCapsMode = 1"))   hasImeCaps = 1;
                if (strstr(line, "ImeLShiftMode = 2")) hasImeL = 1;
                if (strstr(line, "ImeRShiftMode = 1")) hasImeR = 1;
            }
            fclose(fc);
        }
        EXPECT(hasLog && hasSteps && hasMode && hasWheel && hasInterval
               && hasHMul && hasFast,
               "UPG: 补齐块带注释写回文件（含新增项）");
        EXPECT(hasImeCaps && hasImeL && hasImeR,
               "UPG: 补齐块含三个输入法键（连默认值写回）");
    }
    /* 升级的容量算式必须正交覆盖四组边界：
         BOM 有/无  ×  末尾有/无换行  ×  [General] 段有/无  ×  缺部分/缺全部键
       历史缺陷正是"带 BOM 且缺键"时漏算 BOM 的 3 字节 —— 越界写 1~2 字节。
       这里靠 Debug CRT（/MTd）在 free() 时校验堆块，配合断言兜住回归。 */
    {
        static const struct {
            const char* name;
            int  bom;
            int  trailingNl;
            const char* body;      /* 不含 BOM 与结尾换行 */
        } kCases[] = {
            { "BOM + 无尾换行 + 有 General + 缺键", 1, 0,
              "[General]\nRunAsAdmin = 1" },
            { "BOM + 有尾换行 + 有 General + 缺键", 1, 1,
              "[General]\nRunAsAdmin = 1\n" },
            { "BOM + 无尾换行 + 无 General 段",     1, 0,
              "[NativeWordJump]\nOldApp" },
            { "无 BOM + 无尾换行 + 有 General",     0, 0,
              "[General]\nRunAsAdmin = 1" },
            { "无 BOM + 有尾换行 + 无 General 段",  0, 1,
              "[NativeWordJump]\nOldApp\n" },
        };
        for (int c = 0; c < (int)(sizeof(kCases) / sizeof(kCases[0])); c++) {
            FILE* fo = NULL;
            if (fopen_s(&fo, "obj/test/cfgtest.ini", "wb") == 0 && fo) {
                if (kCases[c].bom) { fputc(0xEF, fo); fputc(0xBB, fo); fputc(0xBF, fo); }
                fputs(kCases[c].body, fo);
                if (kCases[c].trailingNl) fputc('\n', fo);
                fclose(fo);
            }
            ConfigUpgradeFile(L"obj/test/cfgtest.ini");   /* 越界会在这里 abort */
            ConfigLoadFrom(L"obj/test/cfgtest.ini");
            char nm[128];
            _snprintf_s(nm, sizeof(nm), _TRUNCATE, "UPG(BOM边界): %s", kCases[c].name);
            EXPECT(CfgBool(CFG_RUN_AS_ADMIN) || !strstr(kCases[c].body, "RunAsAdmin"),
                   nm);
        }
        /* 缺键补齐后 BOM 必须仍然只在文件最开头出现一次 */
        {
            FILE* fo = NULL;
            if (fopen_s(&fo, "obj/test/cfgtest.ini", "wb") == 0 && fo) {
                fputc(0xEF, fo); fputc(0xBB, fo); fputc(0xBF, fo);
                fputs("[General]\nRunAsAdmin = 1", fo);
                fclose(fo);
            }
            ConfigUpgradeFile(L"obj/test/cfgtest.ini");
            unsigned char head[8] = {0};
            FILE* fr = NULL;
            size_t rd = 0;
            if (fopen_s(&fr, "obj/test/cfgtest.ini", "rb") == 0 && fr) {
                rd = fread(head, 1, sizeof(head), fr);
                fclose(fr);
            }
            int bomCount = 0;
            for (size_t i = 0; i + 2 < rd; i++)
                if (head[i] == 0xEF && head[i+1] == 0xBB && head[i+2] == 0xBF) bomCount++;
            EXPECT(rd >= 3 && bomCount == 1 && head[0] == 0xEF,
                   "UPG(BOM边界): BOM 保留且只在文件头出现一次");
            /* 正文里也不应再出现第二个 BOM */
            char whole[65536];
            size_t wn = 0;
            if (fopen_s(&fr, "obj/test/cfgtest.ini", "rb") == 0 && fr) {
                wn = fread(whole, 1, sizeof(whole) - 1, fr);
                fclose(fr);
            }
            whole[wn] = 0;
            int allBom = 0;
            /* 用 unsigned char 比较：char 在本工具链下是有符号的，
               `(char)0xEF` 会被 /W4 判为"截断常量值"（C4310），
               而且有符号比较对 >= 0x80 的字节本来就不正确。 */
            const unsigned char* ub = (const unsigned char*)whole;
            for (size_t i = 0; i + 2 < wn; i++)
                if (ub[i] == 0xEF && ub[i+1] == 0xBB &&
                    ub[i+2] == 0xBF) allBom++;
            EXPECT(allBom == 1, "UPG(BOM边界): 全文只有一个 BOM（未被复制进正文）");
        }
    }
    /* 新格式（键齐全）：升级不改动文件。
       fixture **由 ConfigUpgradeKeyName 现读现生成**，不再手写键名列表 ——
       手写时"加配置项忘了同步这里"就会让升级逻辑（正确地）判定有缺失、
       改写文件，表现为一条假失败。实测栽过一次，所以干脆让它不可能再发生。 */
    {
        /* 值仍写成**自定义值**（非默认），这样下面几条"自定义值不被覆盖"
           断言才有意义。键名则一律从 ConfigUpgradeKeyName 现读 —— 新增项
           不写进这张值表也没关系（会落到兜底值 "1"），但**一定**会被写进
           fixture，所以"键齐全"这条不会因加配置项而假失败。 */
        static const struct { const char* key; const char* val; } kSamples[] = {
            { "runasadmin",         "1"   },
            { "logmaxkb",           "7"   },
            { "maxarrowsteps",      "5"   },
            { "overstepmode",       "1"   },
            { "wheelscrollmul",     "5"   },
            { "autoscrollinterval", "250" },
            { "autoscrollfastms",   "300" },
            { "autoscrollsmooth",   "0"   },
            { "wheelhscrollmul",    "4"   },
            { "imecapsmode",        "2"   },
            { "imelshiftmode",      "0"   },
            { "imershiftmode",      "2"   },
        };
        FILE* fo = NULL;
        if (fopen_s(&fo, "obj/test/cfgtest.ini", "wb") == 0 && fo) {
            fputs("[General]\n", fo);
            int kn = ConfigUpgradeKeyCount();
            for (int i = 0; i < kn; i++) {
                const char* k = ConfigUpgradeKeyName(i);
                if (!k) continue;
                const char* v = "1";
                for (int j = 0; j < (int)(sizeof(kSamples)/sizeof(kSamples[0])); j++)
                    if (_stricmp(k, kSamples[j].key) == 0) { v = kSamples[j].val; break; }
                fprintf(fo, "%s = %s\n", k, v);
            }
            fclose(fo);
        }
        char before[512];
        {
            FILE* fr = NULL;
            if (fopen_s(&fr, "obj/test/cfgtest.ini", "r") == 0 && fr) {
                size_t n = fread(before, 1, 511, fr); before[n] = 0; fclose(fr);
            } else before[0] = 0;
        }
        ConfigUpgradeFile(L"obj/test/cfgtest.ini");
        char after[512];
        {
            FILE* fr = NULL;
            if (fopen_s(&fr, "obj/test/cfgtest.ini", "r") == 0 && fr) {
                size_t n = fread(after, 1, 511, fr); after[n] = 0; fclose(fr);
            } else after[0] = 0;
        }
        EXPECT(strcmp(before, after) == 0, "UPG: 键齐全时不改动文件");
        ConfigLoadFrom(L"obj/test/cfgtest.ini");
        EXPECT(CfgInt(CFG_LOG_MAX_KB) == 7 && CfgInt(CFG_MAX_ARROW_STEPS) == 5 && CfgBool(CFG_OVER_STEP_CTRL),
               "UPG: 用户自定义值不被覆盖");
        EXPECT(CfgInt(CFG_WHEEL_SCROLL_MUL) == 5, "UPG: WheelScrollMul 自定义值不被覆盖");
        EXPECT(CfgInt(CFG_AUTOSCROLL_FAST_MS) == 300,
               "UPG: AutoScrollFastMs 自定义值不被覆盖（默认 1000、样本 300）");
        EXPECT(CfgInt(CFG_AUTOSCROLL_INTERVAL) == 250,
               "UPG: AutoScrollInterval 自定义值不被覆盖");
        EXPECT(CfgInt(CFG_WHEEL_HSCROLL_MUL) == 4,
               "UPG: WheelHScrollMul 自定义值不被覆盖（默认 1、样本为 4）");
        EXPECT(ConfigGetImeMode(IME_KEY_CAPS)   == IME_MODE_EN &&
               ConfigGetImeMode(IME_KEY_LSHIFT) == IME_MODE_OFF &&
               ConfigGetImeMode(IME_KEY_RSHIFT) == IME_MODE_EN,
               "UPG: 输入法键的自定义值不被覆盖（样本 2/0/2）");
        /* 未识别的键（含已废弃的 EnhanceKeys）不再被留存，也不影响任何取值：
           "通用字符串项"设施已随这两个键一起去掉，配置层只认上面具名解析的那些。 */
        EXPECT(CfgInt(CFG_WHEEL_HSCROLL_MUL) == 4,
               "UPG: 废弃键不再被留存（横向倍数仍取自定义值 4）");
    }

    /* ---- 写回（设置程序用）：增量更新，注释与未知段必须原样保留 ----
       整份重写会把用户写的注释、不认识的段和键一起冲掉，那是"我写的东西没了"
       级别的问题，所以这里逐条把它钉住。 */
    {
        const char* before =
            "; 我的备注\r\n"
            "\r\n"
            "[General]\r\n"
            "; 自定义说明\r\n"
            "RunAsAdmin = 1\r\n"
            "LogMaxKB = 777\r\n"
            "\r\n"
            "[Mine]\r\n"
            "Whatever = 1\r\n"
            "\r\n"
            "[NativeWordJump]\r\n"
            "; 名单注释\r\n"
            "Old1.exe\r\n"
            "Old2.exe\r\n";
        WriteIni(before);
        ConfigLoadFrom(L"obj/test/cfgtest.ini");
        EXPECT(CfgInt(CFG_LOG_MAX_KB) == 777 && ConfigListCount(CFG_LIST_NATIVE) == 2,
               "SAVE: 改前读到的就是文件里的值");

        CfgSetBool(CFG_RUN_AS_ADMIN, FALSE);
        CfgSetInt(CFG_LOG_MAX_KB, 999999);                 /* 越界 -> 应被钳到 65536 */
        CfgSetInt(CFG_MAX_ARROW_STEPS, -5);                /* 越界 -> 钳到 0 */
        CfgSetInt(CFG_WHEEL_HSCROLL_MUL, 4);
        ConfigSetImeMode(IME_KEY_CAPS,   IME_MODE_OFF);   /* 0 = 不处理 */
        ConfigSetImeMode(IME_KEY_LSHIFT, IME_MODE_CN);    /* 1 = 切中文 */
        ConfigSetImeMode(IME_KEY_RSHIFT, 7);              /* 越界 -> 钳到 2 */
        {
            static const WCHAR* names[2] = { L"New1.exe", L"  New2  " };
            ConfigSetList(CFG_LIST_NATIVE, names, 2);       /* 整份替换 + 去空白 */
            static const WCHAR* dis[1] = { L"mstsc.exe" };
            ConfigSetList(CFG_LIST_AUTODISABLE, dis, 1);    /* 原本没有这一段 */
        }
        EXPECT(ConfigSaveFile(L"obj/test/cfgtest.ini"), "SAVE: 写回成功");

        ConfigLoadFrom(L"obj/test/cfgtest.ini");
        EXPECT(!CfgBool(CFG_RUN_AS_ADMIN), "SAVE: 布尔改成 0 已落盘");
        EXPECT(CfgInt(CFG_LOG_MAX_KB) == 65536, "SAVE: 越界值写回时被钳到上限 65536");
        EXPECT(CfgInt(CFG_MAX_ARROW_STEPS) == 0, "SAVE: 负数被钳到 0");
        EXPECT(CfgInt(CFG_WHEEL_HSCROLL_MUL) == 4, "SAVE: 数值改动已落盘");
        EXPECT(ConfigGetImeMode(IME_KEY_CAPS)   == IME_MODE_OFF &&
               ConfigGetImeMode(IME_KEY_LSHIFT) == IME_MODE_CN &&
               ConfigGetImeMode(IME_KEY_RSHIFT) == IME_MODE_EN,
               "SAVE: 输入法键改动已落盘（原本缺键则补进 [General]，越界钳到 2）");
        EXPECT(ConfigListCount(CFG_LIST_NATIVE) == 2 &&
               ConfigListAt(CFG_LIST_NATIVE, 0) &&
               _wcsicmp(ConfigListAt(CFG_LIST_NATIVE, 0), L"New1") == 0 &&
               _wcsicmp(ConfigListAt(CFG_LIST_NATIVE, 1), L"New2") == 0,
               "SAVE: 名单被整份替换（旧名字消失、空白被去掉）");
        EXPECT(ConfigMatchList(CFG_LIST_AUTODISABLE, L"mstsc.exe"),
               "SAVE: 原本不存在的名单段被补进文件");

        /* 原样保留的部分（这是"增量更新"的全部意义）。
           ★ 读回缓冲必须比写回产物大：[General] 里补写的缺失键每个都带
             完整注释块，输出早已超过 4KB —— 缓冲小了，排在文件尾部的
             [Mine]/名单段就被截在读回窗口外，看着像"没保留"。 */
        FILE* fr = NULL;
        char whole[65536];
        size_t wn = 0;
        if (fopen_s(&fr, "obj/test/cfgtest.ini", "rb") == 0 && fr) {
            wn = fread(whole, 1, sizeof(whole) - 1, fr);
            fclose(fr);
        }
        whole[wn] = 0;
        EXPECT(strstr(whole, "; 我的备注") != NULL, "SAVE: 文件头的注释保留");
        EXPECT(strstr(whole, "; 自定义说明") != NULL, "SAVE: [General] 里的注释保留");
        EXPECT(strstr(whole, "[Mine]") != NULL && strstr(whole, "Whatever = 1") != NULL,
               "SAVE: 未知段与未知键原样保留");
        EXPECT(strstr(whole, "; 名单注释") != NULL, "SAVE: 名单段里的注释保留");
        EXPECT(strstr(whole, "Old1.exe") == NULL, "SAVE: 旧名字确实被替换掉（不是追加）");
        EXPECT(strstr(whole, "\r\n") != NULL, "SAVE: 保持原本的 CRLF 行尾");
        /* 键名不是"前缀匹配"：LogMaxKBExtra = 1 不该被当成 LogMaxKB 改掉 */
        EXPECT(strstr(whole, "LogMaxKB = 65536") != NULL, "SAVE: 键写成 键 = 值 的形式");
    }
    /* 文件不存在时写回 = 新建（[General] 连带注释块 + 名单段） */
    {
        DeleteFileW(L"obj/test/cfgsave.ini");
        WriteIni("[General]\r\nRunAsAdmin = 1\r\n");
        ConfigLoadFrom(L"obj/test/cfgtest.ini");
        CfgSetInt(CFG_WHEEL_HSCROLL_MUL, 7);
        {
            static const WCHAR* n[1] = { L"a.exe" };
            ConfigSetList(CFG_LIST_NOSMOOTH, n, 1);
        }
        EXPECT(ConfigSaveFile(L"obj/test/cfgsave.ini"), "SAVE: 新文件写回成功");
        ConfigLoadFrom(L"obj/test/cfgsave.ini");
        EXPECT(CfgInt(CFG_WHEEL_HSCROLL_MUL) == 7, "SAVE: 新建文件里自定义值正确");
        EXPECT(ConfigMatchList(CFG_LIST_NOSMOOTH, L"a.exe"), "SAVE: 新建文件含名单段");
        DeleteFileW(L"obj/test/cfgsave.ini");
    }
    /* 读端宽容、写端也必须宽容到同一个程度。
       读端认 `[General ]`（trim 之后再比）与 `[Auto Disable]`（带空格的旧写法），
       而写端过去要求 `]` 紧贴段名 —— 于是这些写法在"保存"时被当成别的段：
         · `[General ]` -> 另起一份规范 `[General]`，读时后段胜出，用户在设置界面
           改的东西全落进没人读的那一份（表现为"改了永远没反应"）；
         · `[Auto Disable]` -> 名单每 load/save 一轮复制一份，涨到 CFG_MAX_ENTRIES
           之后新条目被静默丢弃。
       这两条都是**用户配置文件里真实可能出现的写法**，所以钉成断言。 */
    {
        WriteIni("[General ]\r\nLogMaxKB = 777\r\n");
        ConfigLoadFrom(L"obj/test/cfgtest.ini");
        EXPECT(CfgInt(CFG_LOG_MAX_KB) == 777, "SEC: [General ] 读端认得");
        CfgSetInt(CFG_LOG_MAX_KB, 222);
        EXPECT(ConfigSaveFile(L"obj/test/cfgtest.ini"), "SEC: [General ] 写回成功");
        {
            FILE* fr = NULL; char whole[65536]; size_t wn = 0;
            if (fopen_s(&fr, "obj/test/cfgtest.ini", "rb") == 0 && fr) {
                wn = fread(whole, 1, sizeof(whole) - 1, fr); fclose(fr);
            }
            whole[wn] = 0;
            EXPECT(CountStr(whole, "[General") == 1,
                   "SEC: [General ] 没有再补一份规范段（只有一份 General）");
        }
        ConfigLoadFrom(L"obj/test/cfgtest.ini");
        EXPECT(CfgInt(CFG_LOG_MAX_KB) == 222,
               "SEC: [General ] 那份被就地改掉（设置界面的改动能落盘）");
    }
    {
        WriteIni("[Auto Disable]\r\nProbeApp.exe\r\n");
        for (int round = 0; round < 3; round++) {     /* 三个来回：复制会在这里现形 */
            ConfigLoadFrom(L"obj/test/cfgtest.ini");
            EXPECT(ConfigListCount(CFG_LIST_AUTODISABLE) == 1,
                   "SEC: [Auto Disable] 旧写法每轮只读到一条（不累积）");
            EXPECT(ConfigSaveFile(L"obj/test/cfgtest.ini"), "SEC: 旧段名写回成功");
        }
        FILE* fr = NULL; char whole[65536]; size_t wn = 0;
        if (fopen_s(&fr, "obj/test/cfgtest.ini", "rb") == 0 && fr) {
            wn = fread(whole, 1, sizeof(whole) - 1, fr); fclose(fr);
        }
        whole[wn] = 0;
        /* 判据用"条目翻倍"：段头字样本身还会以注释的形式出现在那些说明块里
           （注释会写 "[AutoDisable] ……"），所以数段头会假失败。
           名字同样用自造的 ProbeApp：拖动名单的内置默认里有 mstsc，拿它当样本
           会被那份默认值污染成"看起来翻倍了"。 */
        EXPECT(CountStr(whole, "ProbeApp") == 1,
               "SEC: 旧段名不复制名单（三个来回之后文件里仍只有一条）");
    }
    /* 超长的字符串值必须**截断**。
       这条不是防御性的：wcscpy_s 把"目标放不下"当无效参数处理 → fastfail
       （0xC0000409），所以一个手写的长字体名会让本体与设置程序一起启动即崩，
       而用户唯一的自救入口（设置界面）也打不开。能跑到下面这一行就说明没崩。 */
    {
        WCHAR longFont[CLIP_FONT_MAX + 64];
        for (int i = 0; i < (int)(sizeof(longFont) / sizeof(longFont[0]) - 1); i++)
            longFont[i] = L'字';
        longFont[sizeof(longFont) / sizeof(longFont[0]) - 1] = 0;
        CfgSetStr(CFG_CLIP_FONT, longFont);
        EXPECT(wcslen(CfgStr(CFG_CLIP_FONT)) == CLIP_FONT_MAX - 1,
               "CFG: 超长 ClipFont 被截断存下（不是 fastfail）");
        CfgSetStr(CFG_CLIP_FONT, L"");
        EXPECT(CfgStr(CFG_CLIP_FONT)[0] == 0, "CFG: ClipFont 可清空（= 内置默认）");
    }

    /* '#' 开头不再算注释（只认 ';'）。可观测的差别：名单段里一行 "# Ghost.exe"
       过去会被整行跳过，现在会被当成一个（带 # 的）进程名收进名单。
       不会因此崩掉解析 —— 只是"# 不再是注释"这件事有了明确后果。 */
    WriteIni("[NativeWordJump]\r\n# Ghost.exe\r\nReal.exe\r\n");
    ConfigLoadFrom(L"obj/test/cfgtest.ini");
    EXPECT(ConfigMatchName(L"# Ghost.exe"),
           "CFG: '#' 行不再被当注释（名单段里带 # 的行会被当成名字）");
    EXPECT(ConfigMatchName(L"Real.exe"), "CFG: 同一段里的正常名字照常命中");

    /* 兼容早期不分节写法 */
    WriteIni("Legacy.exe\r\n");
    ConfigLoadFrom(L"obj/test/cfgtest.ini");
    EXPECT(ConfigMatchName(L"legacy"), "CFG: 兼容早期不分节的名单写法");

    /* 重新加载覆盖旧值 */
    WriteIni("[General]\r\nRunAsAdmin = 1\r\n[NativeWordJump]\r\nFooBar\r\n");
    ConfigLoadFrom(L"obj/test/cfgtest.ini");
    EXPECT(CfgBool(CFG_RUN_AS_ADMIN) && ConfigMatchName(L"foobar"), "CFG: 重载后新配置生效");
    EXPECT(!ConfigMatchName(L"Legacy.exe"), "CFG: 重载后旧名单失效");

    /* 模板：不存在时生成，且不含禁止字样。
       ConfigLoad() 写的是**真实配置路径** obj/test/CapsEnhance.ini，所以先把用户
       现有的 ini 挪开、验完再放回去 —— 否则跑一次单测就把用户调过的值冲成
       默认值（实测踩过：用户的 ini 被整份覆盖成模板）。 */
    BOOL hadRealIni = (GetFileAttributesW(L"obj/test/CapsEnhance.ini") != INVALID_FILE_ATTRIBUTES);
    if (hadRealIni) MoveFileW(L"obj/test/CapsEnhance.ini", L"obj/test/__ini_saved__.ini");
    DeleteFileW(L"obj/test/CapsEnhance.ini");
    ConfigLoad();
    EXPECT(!TemplateHasForbidden(), "CFG: 模板不含禁用字样（VSCode/vsc）");
    /* 模板必须把每个 [General] 开关连默认值写出来，否则用户不知道有这些项 */
    EXPECT(TemplateHas("RunAsAdmin = 0"),    "CFG: 模板含 RunAsAdmin 默认值 0（默认不提权）");
    EXPECT(TemplateHas("LogMaxKB = 2048"),   "CFG: 模板含 LogMaxKB 默认值");
    /* 默认值来自 config.c 的编译期常量：这里按取值现拼，改默认不会漏改断言 */
    { char tpl[64];
      _snprintf_s(tpl, sizeof(tpl), _TRUNCATE, "MaxArrowSteps = %d", CfgInt(CFG_MAX_ARROW_STEPS));
      EXPECT(TemplateHas(tpl), "CFG: 模板含 MaxArrowSteps 默认值"); }
    EXPECT(TemplateHas("OverStepMode = 1"),  "CFG: 模板含 OverStepMode 默认值");
    EXPECT(TemplateHas("WheelScrollMul = 3"), "CFG: 模板含 WheelScrollMul 默认值");
    EXPECT(TemplateHas("AutoScrollFastMs = 60"), "CFG: 模板含 AutoScrollFastMs 默认值");
    EXPECT(TemplateHas("AutoScrollInterval = 1500"), "CFG: 模板含 AutoScrollInterval 默认值");
    EXPECT(TemplateHas("WheelHScrollMul = 1"),     "CFG: 模板含 WheelHScrollMul 默认值");
    /* AutoScrollSmooth 已改为 [NoSmoothScroll] 名单：模板里不该再有这个键
       （留着会让用户去改一个没人读的开关） */
    EXPECT(!TemplateHas("AutoScrollSmooth"), "CFG: 模板不再含 AutoScrollSmooth（已废弃）");
    EXPECT(TemplateHas("AllowScintillaRead = 1"),  "CFG: 模板含 AllowScintillaRead 默认值");
    EXPECT(TemplateHas("ImeCapsMode = 1"),   "CFG: 模板含 ImeCapsMode 默认值（切中文）");
    EXPECT(TemplateHas("ImeLShiftMode = 2"), "CFG: 模板含 ImeLShiftMode 默认值（切英文）");
    EXPECT(TemplateHas("ImeRShiftMode = 1"), "CFG: 模板含 ImeRShiftMode 默认值（切中文）");
    /* DittoDir 是字符串配置项（Ditto 文件夹，Ditto.db/Ditto.exe 都从它派生），
       默认值是**空**（未配置 = 功能不开），
       所以断言的是"键在、且写成了空值"，不是"键 = 某值"。同时钉住解析与写回：
       它是字符串，走的是与整数不同的那条路。 */
    EXPECT(TemplateHas("DittoDir = "), "CFG: 模板含 DittoDir（默认空 = 不启用剪贴板）");
    {
        CfgSetStr(CFG_DITTO_DIR, L"..\\Ditto");
        const WCHAR* got = CfgStr(CFG_DITTO_DIR);
        EXPECT(got && wcscmp(got, L"..\\Ditto") == 0,
               "CFG: DittoDir 原样存取（相对路径不在配置层展开）");
        CfgSetStr(CFG_DITTO_DIR, L"");
        EXPECT(CfgStr(CFG_DITTO_DIR)[0] == 0, "CFG: DittoDir 可清空（= 关闭该功能）");
    }
    /* 快速粘贴的字体/字号：字号是**磅**（与系统字体对话框一致），有钳制。
       默认值 10 在这里写字面量（STR 宏是 config.c 的内部实现，测试不引入）——
       改默认值时这里会失败，正好逼着人来核对。 */
    EXPECT(TemplateHas("ClipFont = "),    "CFG: 模板含 ClipFont（默认空 = 内置字体）");
    EXPECT(TemplateHas("ClipFontSize = 10"), "CFG: 模板含 ClipFontSize 默认值");
    {
        CfgSetStr(CFG_CLIP_FONT, L"MiSans Medium");
        EXPECT(wcscmp(CfgStr(CFG_CLIP_FONT), L"MiSans Medium") == 0,
               "CFG: ClipFont 原样存取（带字重后缀的字面名不改写）");
        CfgSetInt(CFG_CLIP_FONT_SIZE, 999);
        EXPECT(CfgInt(CFG_CLIP_FONT_SIZE) == CLIP_FONT_SIZE_MAX, "CFG: 字号超上限被钳制");
        CfgSetInt(CFG_CLIP_FONT_SIZE, 1);
        EXPECT(CfgInt(CFG_CLIP_FONT_SIZE) == 6, "CFG: 字号低于下限被钳到 6");
        CfgSetStr(CFG_CLIP_FONT, L"");
        CfgSetInt(CFG_CLIP_FONT_SIZE, CLIP_FONT_SIZE_DEFAULT);
    }
    /* 快速粘贴窗口的尺寸：96 DPI 下的像素，写进 ini 的默认值必须等于浮层
       原来写死的那两个（560 / 374），否则"什么都没配"也会变个尺寸。 */
    EXPECT(TemplateHas("ClipW = " STR(CLIP_W_DEFAULT)), "CFG: 模板含 ClipW 默认值");
    EXPECT(TemplateHas("ClipH = " STR(CLIP_H_DEFAULT)), "CFG: 模板含 ClipH 默认值");
    {
        CfgSetInt(CFG_CLIP_W, 9999);
        EXPECT(CfgInt(CFG_CLIP_W) == CLIP_W_MAX, "CFG: 宽度超上限被钳制");
        CfgSetInt(CFG_CLIP_W, 1);
        EXPECT(CfgInt(CFG_CLIP_W) == CLIP_W_MIN, "CFG: 宽度低于下限被钳到最小值");
        CfgSetInt(CFG_CLIP_H, 9999);
        EXPECT(CfgInt(CFG_CLIP_H) == CLIP_H_MAX, "CFG: 高度超上限被钳制");
        CfgSetInt(CFG_CLIP_H, 1);
        EXPECT(CfgInt(CFG_CLIP_H) == CLIP_H_MIN, "CFG: 高度低于下限被钳到最小值");
        CfgSetInt(CFG_CLIP_W, CLIP_W_DEFAULT);
        CfgSetInt(CFG_CLIP_H, CLIP_H_DEFAULT);
    }
    /* 激活键不再进配置：模板里**不该**再出现这两项（否则用户会以为能改） */
    EXPECT(!TemplateHas("EnhanceKeys"), "CFG: 模板不再含 EnhanceKeys（已改为编译期常量）");
    EXPECT(!TemplateHas("ShiftKeys"),   "CFG: 模板不再含 ShiftKeys（已改为编译期常量）");
    EXPECT(TemplateHas("[NativeWordJump]"),  "CFG: 模板含 NativeWordJump 段");
    EXPECT(TemplateHas("[AutoDisable]"),     "CFG: 模板含 AutoDisable 段");
    EXPECT(TemplateHas("[NoSmoothScroll]"),  "CFG: 模板含 NoSmoothScroll 段");
    DeleteFileW(L"obj/test/CapsEnhance.ini");            /* 删掉刚生成的模板 */
    if (hadRealIni) MoveFileW(L"obj/test/__ini_saved__.ini", L"obj/test/CapsEnhance.ini");

    /* 真实权限查询：不崩溃即可（是否管理员取决于运行环境） */
    (void)IsRunElevated();

    DeleteFileW(L"obj/test/cfgtest.ini");
    printf(fails ? "\n== %d 项失败 ==\n" : "\n== 全部通过 ==\n", fails);
    return fails;
}
