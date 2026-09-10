/* ============================================================
   配置模块单元测试：分节、键值、注释、布尔写法、进程名完全匹配、
   模板内容（不得出现禁止字样）
   ============================================================ */
#include "cap.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void ConfigLoadFrom(const WCHAR* path);
void ConfigLoad(void);
BOOL ConfigMatchName(const WCHAR* exeName);
BOOL ConfigGetRunAsAdmin(void);
int  ConfigGetLogMaxKB(void);
int  ConfigGetMaxArrowSteps(void);
BOOL ConfigGetOverStepCtrl(void);
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

    /* 真实权限查询：不崩溃即可（是否管理员取决于运行环境） */
    (void)IsRunElevated();

    DeleteFileW(L"bin/cfgtest.ini");
    printf(fails ? "\n== %d 项失败 ==\n" : "\n== 全部通过 ==\n", fails);
    return fails;
}
