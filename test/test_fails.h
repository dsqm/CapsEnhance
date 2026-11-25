#ifndef TEST_FAILS_H
#define TEST_FAILS_H

#include <stdio.h>
#include <string.h>

/* ================= 失败项清单（所有测试共用） =================
   ★ 为什么需要：一次跑几千条断言，[FAIL] 那几行夹在中间，汇总行只说"1 项失败"，
   想知道是哪一条只能往上翻。这里在断言失败时把**名字**记下来，末尾的汇总再把
   它们列一遍 —— 底部直接就能看到哪几条没过。
   ★ 只记名字，不参与任何判定：判定的唯一依据仍然是各自文件里的 CHECK/EXPECT 宏。
   cap 取 64：真红到 64 条以上时，问题已经不在"哪一条失败"，而在整片环境。 */
#define TF_MAX  64
#define TF_NAME 192

static char g_tfNames[TF_MAX][TF_NAME];
static int  g_tfN = 0;

static __inline void TfAddFail(const char* name) {
    if (!name || g_tfN >= TF_MAX) return;
    strncpy_s(g_tfNames[g_tfN], TF_NAME, name, _TRUNCATE);
    g_tfN++;
}

/* 只列清单（没有失败项时什么都不打）。给"自带汇总格式"的用例用（如
   "(0 failures)" 那种），它们自己那行汇总保持不变。 */
static __inline void TfFailList(void) {
    if (!g_tfN) return;
    printf("\n失败项（共 %d 条）：\n", g_tfN);
    for (int i = 0; i < g_tfN; i++) printf("  - %s\n", g_tfNames[i]);
}

/* 标准汇总：全过 -> "== 全部通过 =="；有失败 -> "== N 项失败 ==" + 失败清单。 */
static __inline void TfSummary(int fails) {
    if (!fails) {
        printf("\n== 全部通过 ==\n");
        return;
    }
    printf("\n== %d 项失败 ==\n", fails);
    TfFailList();
}

#endif /* TEST_FAILS_H */
