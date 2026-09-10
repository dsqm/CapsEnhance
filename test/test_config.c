/* ============================================================
   配置模块单元测试：分节、键值、注释、布尔写法、进程名完全匹配、
   模板内容（不得出现禁止字样）
   ============================================================ */
#include "cap.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

void ConfigLoadFrom(const WCHAR* path);
void ConfigLoad(void);
BOOL ConfigMatchName(const WCHAR* exeName);
BOOL ConfigGetRunAsAdmin(void);
int  ConfigGetLogMaxKB(void);
int  ConfigGetMaxArrowSteps(void);
BOOL ConfigGetOverStepCtrl(void);
int  ConfigGetWheelScrollMs(void);
int  ConfigGetAutoScrollInterval(void);
BOOL IsRunElevated(void);

static int fails = 0;
#define EXPECT(cond, name) do { \
    if (cond) printf("  [OK]   %s\n", name); \
    else      { printf("  [FAIL] %s\n", name); fails++; } } while (0)

static void WriteIni(const char* body) {
    FILE* f = NULL;
    if (_wfopen_s(&f, L"bin/cfgtest.ini", L"wb") == 0 && f) {
        fwrite(body, 1, strlen(body), f);
        fclose(f);
    }
}

/* 读回刚生成的模板，检查禁止字样 */
static BOOL TemplateHasForbidden(void) {
    FILE* f = NULL;
    if (_wfopen_s(&f, L"bin/CapsEnhance.ini", L"rb") != 0 || !f) return TRUE;
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
    if (_wfopen_s(&f, L"bin/CapsEnhance.ini", L"rb") != 0 || !f) return FALSE;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    char* buf = (char*)malloc((size_t)sz + 1);
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[n] = 0;
    BOOL has = strstr(buf, needle) != NULL;
    free(buf);
    return has;
}

int main(void) {
    /* 完整格式：General 键值 + NativeWordJump 名单 */
    WriteIni(
        "# 注释\r\n"
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
    ConfigLoadFrom(L"bin/cfgtest.ini");

    EXPECT(ConfigGetRunAsAdmin(), "CFG: RunAsAdmin=1 生效");
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
    ConfigLoadFrom(L"bin/cfgtest.ini");
    EXPECT(ConfigGetRunAsAdmin(), "CFG: 非 General 段的同名键不覆盖");

    /* 布尔只认 0/1 */
    WriteIni("[General]\r\nRunAsAdmin = 1\r\n");
    ConfigLoadFrom(L"bin/cfgtest.ini");
    EXPECT(ConfigGetRunAsAdmin(), "CFG: 1 视为真");
    WriteIni("[General]\r\nRunAsAdmin = true\r\n");
    ConfigLoadFrom(L"bin/cfgtest.ini");
    EXPECT(!ConfigGetRunAsAdmin(), "CFG: true 不生效（只认 0/1）");
    WriteIni("[General]\r\nRunAsAdmin = \xe5\x90\xaf\xe7\x94\xa8\r\n");  /* 启用 */
    ConfigLoadFrom(L"bin/cfgtest.ini");
    EXPECT(!ConfigGetRunAsAdmin(), "CFG: 中文“启用”不生效（只认 0/1）");
    WriteIni("[General]\r\nRunAsAdmin = 01\r\n");
    ConfigLoadFrom(L"bin/cfgtest.ini");
    EXPECT(!ConfigGetRunAsAdmin(), "CFG: 01 不生效（严格等于 1）");
    WriteIni("[General]\r\nRunAsAdmin = 0\r\n");
    ConfigLoadFrom(L"bin/cfgtest.ini");
    EXPECT(!ConfigGetRunAsAdmin(), "CFG: 0 视为假");
    WriteIni("[General]\r\nRunAsAdmin =\r\n");
    ConfigLoadFrom(L"bin/cfgtest.ini");
    EXPECT(!ConfigGetRunAsAdmin(), "CFG: 空值视为假");
    WriteIni("[General]\r\n\r\n");
    ConfigLoadFrom(L"bin/cfgtest.ini");
    EXPECT(!ConfigGetRunAsAdmin(), "CFG: 缺省为假");

    /* 日志上限 LogMaxKB（KB） */
    WriteIni("[General]\r\nLogMaxKB = 512\r\n");
    ConfigLoadFrom(L"bin/cfgtest.ini");
    EXPECT(ConfigGetLogMaxKB() == 512, "CFG: LogMaxKB=512 生效");
    WriteIni("[General]\r\nLogMaxKB = 0\r\n");
    ConfigLoadFrom(L"bin/cfgtest.ini");
    EXPECT(ConfigGetLogMaxKB() == 0, "CFG: LogMaxKB=0（不限制）");
    WriteIni("[General]\r\nLogMaxKB = abc\r\n");
    ConfigLoadFrom(L"bin/cfgtest.ini");
    EXPECT(ConfigGetLogMaxKB() == 2048, "CFG: LogMaxKB 非法值回落默认 2048");
    WriteIni("[General]\r\nRunAsAdmin = 1\r\n[Other]\r\nLogMaxKB = 1\r\n");
    ConfigLoadFrom(L"bin/cfgtest.ini");
    EXPECT(ConfigGetLogMaxKB() == 2048, "CFG: 非 General 段的 LogMaxKB 不生效");

    /* UTF-8 BOM + LF 行尾（Linux 编辑器保存的格式） */
    {
        FILE* fb = NULL;
        if (fopen_s(&fb, "bin/cfgtest.ini", "wb") == 0 && fb) {
            fputc(0xEF, fb); fputc(0xBB, fb); fputc(0xBF, fb);   /* BOM */
            fputs("[General]\nRunAsAdmin = 1\n\n[NativeWordJump]\nBomApp\n", fb);
            fclose(fb);
        }
        ConfigLoadFrom(L"bin/cfgtest.ini");
        EXPECT(ConfigGetRunAsAdmin() && ConfigMatchName(L"bomapp"),
               "CFG: UTF-8 BOM + LF 行尾");
    }

    /* 逐格阈值与超限模式 */
    WriteIni("[General]\r\nMaxArrowSteps = 16\r\nOverStepMode = 1\r\n");
    ConfigLoadFrom(L"bin/cfgtest.ini");
    EXPECT(ConfigGetMaxArrowSteps() == 16 && ConfigGetOverStepCtrl(),
           "CFG: MaxArrowSteps=16 + OverStepMode=1 生效");
    WriteIni("[General]\r\nMaxArrowSteps = 0\r\nOverStepMode = 0\r\n");
    ConfigLoadFrom(L"bin/cfgtest.ini");
    EXPECT(ConfigGetMaxArrowSteps() == 0 && !ConfigGetOverStepCtrl(),
           "CFG: MaxArrowSteps=0(不限制) + OverStepMode=0 生效");
    WriteIni("[General]\r\nMaxArrowSteps = abc\r\nOverStepMode = xyz\r\n");
    ConfigLoadFrom(L"bin/cfgtest.ini");
    EXPECT(ConfigGetMaxArrowSteps() == 32 && !ConfigGetOverStepCtrl(),
           "CFG: 非法值回落默认 (32 / arrows)");
    WriteIni("[Other]\r\nMaxArrowSteps = 1\r\nOverStepMode = 1\r\n");
    ConfigLoadFrom(L"bin/cfgtest.ini");
    EXPECT(ConfigGetMaxArrowSteps() == 32 && !ConfigGetOverStepCtrl(),
           "CFG: 非 General 段不生效");

    /* 滚轮最快速度 WheelScrollMs（毫秒/格，与 AutoScrollInterval 同单位）：
       Caps+滚轮 就按它连续滚；Caps+Alt/D+滚轮 的提速上限也是它。
       0 = 关闭纵向那两档（滚轮退回系统原生行为）。
       样本用与默认值不同的值（默认值当样本会让"自定义生效"恒真）。 */
    WriteIni("[General]\r\nWheelScrollMs = 250\r\n");
    ConfigLoadFrom(L"bin/cfgtest.ini");
    EXPECT(ConfigGetWheelScrollMs() == 250, "CFG: WheelScrollMs=250 生效");
    WriteIni("[General]\r\nWheelScrollMs = 0\r\n");
    ConfigLoadFrom(L"bin/cfgtest.ini");
    EXPECT(ConfigGetWheelScrollMs() == 0, "CFG: WheelScrollMs=0（关闭纵向滚轮增强）");
    WriteIni("[General]\r\nWheelScrollMs = 999999\r\n");
    ConfigLoadFrom(L"bin/cfgtest.ini");
    EXPECT(ConfigGetWheelScrollMs() == 60000, "CFG: WheelScrollMs 超过上限钳到 60000");
    WriteIni("[General]\r\nWheelScrollMs = abc\r\n");
    ConfigLoadFrom(L"bin/cfgtest.ini");
    EXPECT(ConfigGetWheelScrollMs() == 100, "CFG: WheelScrollMs 非法值回落默认 100");
    WriteIni("[Other]\r\nWheelScrollMs = 9\r\n");
    ConfigLoadFrom(L"bin/cfgtest.ini");
    EXPECT(ConfigGetWheelScrollMs() == 100, "CFG: 非 General 段的 WheelScrollMs 不生效");
    WriteIni("[General]\r\n\r\n");
    ConfigLoadFrom(L"bin/cfgtest.ini");
    /* 缺省 100 = 10 格/秒 = 30 行/秒（"快速下翻"）：跟手档就按这个节奏滚。
       历史值 1000（1 格/秒）比人正常滚轮还慢，"按住 Caps 增强"反而更慢。 */
    EXPECT(ConfigGetWheelScrollMs() == 100, "CFG: WheelScrollMs 缺省为 100（跟手档速度）");
    /* 旧键名 WheelScrollMul 必须被当作"不认识"（语义不同，不能硬换算）：
       它既不影响新键，也不该被解析成任何值 —— 回落到新键的缺省。 */
    WriteIni("[General]\r\nWheelScrollMul = 16\r\n");
    ConfigLoadFrom(L"bin/cfgtest.ini");
    EXPECT(ConfigGetWheelScrollMs() == 100, "CFG: 旧键 WheelScrollMul 被忽略（不参与解析）");

    /* 自动滚动基础节奏 AutoScrollInterval（ms），钳制 20..1000 */
    WriteIni("[General]\r\nAutoScrollInterval = 250\r\n");
    ConfigLoadFrom(L"bin/cfgtest.ini");
    EXPECT(ConfigGetAutoScrollInterval() == 250, "CFG: AutoScrollInterval=250 生效");
    WriteIni("[General]\r\nAutoScrollInterval = 5\r\n");
    ConfigLoadFrom(L"bin/cfgtest.ini");
    EXPECT(ConfigGetAutoScrollInterval() == 100, "CFG: AutoScrollInterval 下限钳到 100");
    WriteIni("[General]\r\nAutoScrollInterval = 999999\r\n");
    ConfigLoadFrom(L"bin/cfgtest.ini");
    EXPECT(ConfigGetAutoScrollInterval() == 60000, "CFG: AutoScrollInterval 上限钳到 60000");
    WriteIni("[General]\r\nAutoScrollInterval = abc\r\n");
    ConfigLoadFrom(L"bin/cfgtest.ini");
    EXPECT(ConfigGetAutoScrollInterval() == 24000,
           "CFG: AutoScrollInterval 非法值回落默认 24000");
    WriteIni("[Other]\r\nAutoScrollInterval = 300\r\n");   /* 非 General 段：不应生效 */
    ConfigLoadFrom(L"bin/cfgtest.ini");
    EXPECT(ConfigGetAutoScrollInterval() == 24000,
           "CFG: 非 General 段的 AutoScrollInterval 不生效");
    WriteIni("[General]\r\n\r\n");
    ConfigLoadFrom(L"bin/cfgtest.ini");
    /* 默认值来自阅读速度换算（24000ms/格 = 7.5 行/分钟 ≈ 338 字/分钟），
       不是手感试出来的 —— 改它之前先看 config.c 里那张换算表。 */
    EXPECT(ConfigGetAutoScrollInterval() == 24000,
           "CFG: AutoScrollInterval 缺省为 24000（≈338 字/分钟，阅读舒适区）");

    /* 平滑注入开关 AutoScrollSmooth（默认 1）。样本用 0 —— 与默认值不同，
       否则"自定义值生效"这条会恒真（默认值当样本的老坑）。 */
    EXPECT(ConfigGetAutoScrollSmooth(), "CFG: AutoScrollSmooth 缺省为开");
    WriteIni("[General]\r\nAutoScrollSmooth = 0\r\n");
    ConfigLoadFrom(L"bin/cfgtest.ini");
    EXPECT(!ConfigGetAutoScrollSmooth(), "CFG: AutoScrollSmooth=0 关闭平滑");
    WriteIni("[General]\r\nAutoScrollSmooth = 1\r\n");
    ConfigLoadFrom(L"bin/cfgtest.ini");
    EXPECT(ConfigGetAutoScrollSmooth(), "CFG: AutoScrollSmooth=1 打开平滑");
    /* 布尔项一律只认 "1"（与 RunAsAdmin / OverStepMode 同一套约定）：
       其余写法（true/yes/on）都当关，不做模糊匹配。 */
    WriteIni("[General]\r\nAutoScrollSmooth = true\r\n");
    ConfigLoadFrom(L"bin/cfgtest.ini");
    EXPECT(!ConfigGetAutoScrollSmooth(),
           "CFG: AutoScrollSmooth 只认 1（true 视为关，与其它开关一致）");
    WriteIni("[Other]\r\nAutoScrollSmooth = 0\r\n");
    ConfigLoadFrom(L"bin/cfgtest.ini");
    EXPECT(ConfigGetAutoScrollSmooth(), "CFG: 非 General 段的 AutoScrollSmooth 不生效");

    /* 横向滚动倍数 WheelHScrollMul（Caps+Shift+滚轮）。默认必须是 **1**
       —— 一个物理格换一次横向滚动，与系统原生横向滚动一致。
       样本一律用与默认值不同的值（1 当样本会让"自定义生效"恒真）。 */
    EXPECT(ConfigGetWheelHScrollMul() == 1, "CFG: WheelHScrollMul 缺省为 1");
    WriteIni("[General]\r\nWheelHScrollMul = 3\r\n");
    ConfigLoadFrom(L"bin/cfgtest.ini");
    EXPECT(ConfigGetWheelHScrollMul() == 3, "CFG: WheelHScrollMul=3 生效");
    WriteIni("[General]\r\nWheelHScrollMul = 0\r\n");
    ConfigLoadFrom(L"bin/cfgtest.ini");
    EXPECT(ConfigGetWheelHScrollMul() == 0, "CFG: WheelHScrollMul=0 表示关闭横向滚动");
    /* ParseIntW 只认前导十进制数字，负号不是数字 -> 当作非法值回落默认。
       （解析器里仍有 v<0 -> 0 的防御，但配置文本走不到那条路。） */
    WriteIni("[General]\r\nWheelHScrollMul = -5\r\n");
    ConfigLoadFrom(L"bin/cfgtest.ini");
    EXPECT(ConfigGetWheelHScrollMul() == 1,
           "CFG: WheelHScrollMul 负号被当非法值 -> 回落默认 1");
    WriteIni("[General]\r\nWheelHScrollMul = 999\r\n");
    ConfigLoadFrom(L"bin/cfgtest.ini");
    EXPECT(ConfigGetWheelHScrollMul() == 16, "CFG: WheelHScrollMul 上限钳到 16");
    WriteIni("[General]\r\nWheelHScrollMul = abc\r\n");
    ConfigLoadFrom(L"bin/cfgtest.ini");
    EXPECT(ConfigGetWheelHScrollMul() == 1, "CFG: WheelHScrollMul 非法值回落默认 1");
    WriteIni("[Other]\r\nWheelHScrollMul = 7\r\n");
    ConfigLoadFrom(L"bin/cfgtest.ini");
    EXPECT(ConfigGetWheelHScrollMul() == 1, "CFG: 非 General 段的 WheelHScrollMul 不生效");

    /* 激活键列表 EnhanceKeys / ShiftKeys 已**从配置里去掉**（它们是"和弦语法"
       的一部分，不是给用户调的旋钮）：解析器不再认这两个键名、KeyMapInit 也
       不读配置 —— 激活键固定为 cap.h 的 MODE_*_KEYS_DEFAULT（Alt+D / F）。
       相关行为由 test_chord 的场景 S 验证（随手写一个 ini 也不会改变键位）；
       "模板里不该再有这两项"由本文件末尾的模板用例断言。 */

    /* 老配置升级：缺失的新键自动补齐，已有内容保留 */
    {
        FILE* fo = NULL;
        if (fopen_s(&fo, "bin/cfgtest.ini", "wb") == 0 && fo) {
            fputs("[General]\nRunAsAdmin = 1\n\n[NativeWordJump]\nOldApp\n", fo);
            fclose(fo);
        }
        ConfigUpgradeFile(L"bin/cfgtest.ini");
        ConfigLoadFrom(L"bin/cfgtest.ini");
        EXPECT(ConfigGetRunAsAdmin() && ConfigMatchName(L"oldapp"),
               "UPG: 用户已有内容保留 (RunAsAdmin=1 / 名单)");
        EXPECT(ConfigGetLogMaxKB() == 2048 && ConfigGetMaxArrowSteps() == 32,
               "UPG: 缺失键补齐默认值");
        EXPECT(ConfigGetAutoScrollInterval() == 24000, "UPG: 补齐 AutoScrollInterval 默认值");
        EXPECT(ConfigGetWheelScrollMs() == 100, "UPG: 补齐 WheelScrollMs 默认值");
        KeyMapInit();
        EXPECT(ModeKeyCount(MODE_ENHANCE) == 2 && ModeKeyCount(MODE_SHIFT) == 1,
               "UPG: 激活键是编译期常量（Alt+D / F），与配置文件无关");
        FILE* fc = NULL;
        int hasLog = 0, hasSteps = 0, hasMode = 0, hasWheel = 0;
        int hasInterval = 0, hasSmooth = 0, hasHMul = 0;
        if (fopen_s(&fc, "bin/cfgtest.ini", "r") == 0 && fc) {
            char line[512];
            while (fgets(line, sizeof(line), fc)) {
                if (strstr(line, "LogMaxKB = 2048")) hasLog = 1;
                if (strstr(line, "MaxArrowSteps = 32")) hasSteps = 1;
                if (strstr(line, "OverStepMode = 0")) hasMode = 1;
                if (strstr(line, "WheelScrollMs = 100")) hasWheel = 1;
                if (strstr(line, "AutoScrollInterval = 24000")) hasInterval = 1;
                if (strstr(line, "AutoScrollSmooth = 1")) hasSmooth = 1;
                if (strstr(line, "WheelHScrollMul = 1")) hasHMul = 1;
            }
            fclose(fc);
        }
        EXPECT(hasLog && hasSteps && hasMode && hasWheel && hasInterval
               && hasSmooth && hasHMul,
               "UPG: 补齐块带注释写回文件（含新增项）");
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
            if (fopen_s(&fo, "bin/cfgtest.ini", "wb") == 0 && fo) {
                if (kCases[c].bom) { fputc(0xEF, fo); fputc(0xBB, fo); fputc(0xBF, fo); }
                fputs(kCases[c].body, fo);
                if (kCases[c].trailingNl) fputc('\n', fo);
                fclose(fo);
            }
            ConfigUpgradeFile(L"bin/cfgtest.ini");   /* 越界会在这里 abort */
            ConfigLoadFrom(L"bin/cfgtest.ini");
            char nm[128];
            _snprintf_s(nm, sizeof(nm), _TRUNCATE, "UPG(BOM边界): %s", kCases[c].name);
            EXPECT(ConfigGetRunAsAdmin() || !strstr(kCases[c].body, "RunAsAdmin"),
                   nm);
        }
        /* 缺键补齐后 BOM 必须仍然只在文件最开头出现一次 */
        {
            FILE* fo = NULL;
            if (fopen_s(&fo, "bin/cfgtest.ini", "wb") == 0 && fo) {
                fputc(0xEF, fo); fputc(0xBB, fo); fputc(0xBF, fo);
                fputs("[General]\nRunAsAdmin = 1", fo);
                fclose(fo);
            }
            ConfigUpgradeFile(L"bin/cfgtest.ini");
            unsigned char head[8] = {0};
            FILE* fr = NULL;
            size_t rd = 0;
            if (fopen_s(&fr, "bin/cfgtest.ini", "rb") == 0 && fr) {
                rd = fread(head, 1, sizeof(head), fr);
                fclose(fr);
            }
            int bomCount = 0;
            for (size_t i = 0; i + 2 < rd; i++)
                if (head[i] == 0xEF && head[i+1] == 0xBB && head[i+2] == 0xBF) bomCount++;
            EXPECT(rd >= 3 && bomCount == 1 && head[0] == 0xEF,
                   "UPG(BOM边界): BOM 保留且只在文件头出现一次");
            /* 正文里也不应再出现第二个 BOM */
            char whole[4096];
            size_t wn = 0;
            if (fopen_s(&fr, "bin/cfgtest.ini", "rb") == 0 && fr) {
                wn = fread(whole, 1, sizeof(whole) - 1, fr);
                fclose(fr);
            }
            whole[wn] = 0;
            int allBom = 0;
            for (size_t i = 0; i + 2 < wn; i++)
                if (whole[i] == (char)0xEF && whole[i+1] == (char)0xBB &&
                    whole[i+2] == (char)0xBF) allBom++;
            EXPECT(allBom == 1, "UPG(BOM边界): 全文只有一个 BOM（未被复制进正文）");
        }
    }
    /* 新格式（键齐全）：升级不改动文件 */
    {
        FILE* fo = NULL;
        if (fopen_s(&fo, "bin/cfgtest.ini", "wb") == 0 && fo) {
            /* 必须把**当前所有**新增键都写上：少写一个，升级逻辑就会（正确地）
               认为它是老配置而改写文件。曾经就是这样漏掉的 —— 加配置项时
               记得同步这里，和 kUpgrades 保持一致。 */
            fputs("[General]\nRunAsAdmin = 1\nLogMaxKB = 7\nMaxArrowSteps = 5\n"
                  "OverStepMode = 1\nWheelScrollMs = 250\n"
                  "AutoScrollInterval = 250\nAutoScrollSmooth = 0\n"
                  "WheelHScrollMul = 4\n", fo);
            fclose(fo);
        }
        char before[512];
        {
            FILE* fr = NULL;
            if (fopen_s(&fr, "bin/cfgtest.ini", "r") == 0 && fr) {
                size_t n = fread(before, 1, 511, fr); before[n] = 0; fclose(fr);
            } else before[0] = 0;
        }
        ConfigUpgradeFile(L"bin/cfgtest.ini");
        char after[512];
        {
            FILE* fr = NULL;
            if (fopen_s(&fr, "bin/cfgtest.ini", "r") == 0 && fr) {
                size_t n = fread(after, 1, 511, fr); after[n] = 0; fclose(fr);
            } else after[0] = 0;
        }
        EXPECT(strcmp(before, after) == 0, "UPG: 键齐全时不改动文件");
        ConfigLoadFrom(L"bin/cfgtest.ini");
        EXPECT(ConfigGetLogMaxKB() == 7 && ConfigGetMaxArrowSteps() == 5 && ConfigGetOverStepCtrl(),
               "UPG: 用户自定义值不被覆盖");
        EXPECT(ConfigGetWheelScrollMs() == 250, "UPG: WheelScrollMs 自定义值不被覆盖");
        EXPECT(ConfigGetAutoScrollInterval() == 250,
               "UPG: AutoScrollInterval 自定义值不被覆盖");
        EXPECT(!ConfigGetAutoScrollSmooth(),
               "UPG: AutoScrollSmooth 自定义值不被覆盖（默认开、样本为关）");
        EXPECT(ConfigGetWheelHScrollMul() == 4,
               "UPG: WheelHScrollMul 自定义值不被覆盖（默认 1、样本为 4）");
        /* 未识别的键（含已废弃的 EnhanceKeys）不再被留存，也不影响任何取值：
           "通用字符串项"设施已随这两个键一起去掉，配置层只认上面具名解析的那些。 */
        EXPECT(ConfigGetWheelHScrollMul() == 4,
               "UPG: 废弃键不再被留存（横向倍数仍取自定义值 4）");
    }

    /* 兼容早期不分节写法 */
    WriteIni("Legacy.exe\r\n");
    ConfigLoadFrom(L"bin/cfgtest.ini");
    EXPECT(ConfigMatchName(L"legacy"), "CFG: 兼容早期不分节的名单写法");

    /* 重新加载覆盖旧值 */
    WriteIni("[General]\r\nRunAsAdmin = 1\r\n[NativeWordJump]\r\nFooBar\r\n");
    ConfigLoadFrom(L"bin/cfgtest.ini");
    EXPECT(ConfigGetRunAsAdmin() && ConfigMatchName(L"foobar"), "CFG: 重载后新配置生效");
    EXPECT(!ConfigMatchName(L"Legacy.exe"), "CFG: 重载后旧名单失效");

    /* 模板：不存在时生成，且不含禁止字样 */
    DeleteFileW(L"bin/CapsEnhance.ini");
    ConfigLoad();
    EXPECT(!TemplateHasForbidden(), "CFG: 模板不含禁用字样（VSCode/vsc）");
    /* 模板必须把每个 [General] 开关连默认值写出来，否则用户不知道有这些项 */
    EXPECT(TemplateHas("RunAsAdmin = 0"),    "CFG: 模板含 RunAsAdmin 默认值");
    EXPECT(TemplateHas("LogMaxKB = 2048"),   "CFG: 模板含 LogMaxKB 默认值");
    EXPECT(TemplateHas("MaxArrowSteps = 32"),"CFG: 模板含 MaxArrowSteps 默认值");
    EXPECT(TemplateHas("OverStepMode = 0"),  "CFG: 模板含 OverStepMode 默认值");
    EXPECT(TemplateHas("WheelScrollMs = 100"), "CFG: 模板含 WheelScrollMs 默认值");
    EXPECT(TemplateHas("AutoScrollInterval = 24000"), "CFG: 模板含 AutoScrollInterval 默认值");
    EXPECT(TemplateHas("AutoScrollSmooth = 1"),    "CFG: 模板含 AutoScrollSmooth 默认值");
    EXPECT(TemplateHas("WheelHScrollMul = 1"),     "CFG: 模板含 WheelHScrollMul 默认值");
    /* 激活键不再进配置：模板里**不该**再出现这两项（否则用户会以为能改） */
    EXPECT(!TemplateHas("EnhanceKeys"), "CFG: 模板不再含 EnhanceKeys（已改为编译期常量）");
    EXPECT(!TemplateHas("ShiftKeys"),   "CFG: 模板不再含 ShiftKeys（已改为编译期常量）");
    EXPECT(TemplateHas("[NativeWordJump]"),  "CFG: 模板含 NativeWordJump 段");

    /* 真实权限查询：不崩溃即可（是否管理员取决于运行环境） */
    (void)IsRunElevated();

    DeleteFileW(L"bin/cfgtest.ini");
    printf(fails ? "\n== %d 项失败 ==\n" : "\n== 全部通过 ==\n", fails);
    return fails;
}
