#include "cap.h"

#include <stdio.h>
#include <string.h>
#include <wchar.h>

/* ================= 键位映射：动作表 + 修饰模式表 =================
   这个模块存在的理由：**"某个键是什么意思"必须只有一份定义。**

   以前它是散开的：hook.c 的 kMap[]（触发键 -> 动作）、hook.c 的 ActToVk()
   switch（动作 -> 目标键）、main.c 的 IsSingleShotAct() switch（动作属性）、
   hook.c 里写死的 `vk == 'D' || vk == VK_LMENU`（增益修饰）、
   hook.c 的 `vk == 'F'`（位移修饰）。
   加一个动作要改三处、漏一处就冒出诡异现象（例如"新增的翻转键长按会切偶数次"）；
   而"把 D 换成别的键"更是要动钩子逻辑本身。

   现在全部收敛成两张声明式表，上面那些地方只剩查询：
     · 动作表   —— 触发键 / 目标键 / 属性标志（能否重复、由谁执行）
     · 修饰表   —— 每个模式的激活键集合（可由配置覆盖）

   特别注意：**引擎从不判断具体按的是哪个键，只问"这个模式生效了吗"。**
   所以把 D 删掉、只留 Alt，或者换成 Caps+右Ctrl，都只是配置问题。 */

/* ================= 动作表 ================= */

typedef struct {
    int      act;    /* 动作 id（ACT_*）                          */
    int      vk;     /* 触发键；0 = 没有触发键（只能由内部投递）    */
    int      outVk;  /* 目标虚拟键；0 = 不发键（见 flags）         */
    int      dir;    /* 词跳方向 DIR_*（仅 ACTF_WORDJUMP 有意义）  */
    unsigned flags;
} ActionDef;

/* 顺序不敏感，ActOfVk 线性扫（表很小，且只在按键事件里查一次）。 */
static const ActionDef kActions[] = {
    { ACT_LEFT,      'J',        VK_LEFT,     -1, 0 },
    { ACT_DOWN,      'K',        VK_DOWN,     -1, 0 },
    { ACT_RIGHT,     'L',        VK_RIGHT,    -1, 0 },
    { ACT_UP,        'I',        VK_UP,       -1, 0 },
    { ACT_HOME,      'P',        VK_HOME,     -1, 0 },
    { ACT_END,       VK_OEM_1,   VK_END,      -1, 0 },   /* ; */
    { ACT_BACKSPACE, 'R',        VK_BACK,     -1, 0 },
    { ACT_DELETE,    'W',        VK_DELETE,   -1, 0 },
    { ACT_ENTER,     VK_SPACE,   VK_RETURN,   -1, 0 },
    /* 开关键：翻转动作，按一次只切一次 —— 属性写在表里，见 ACTF_SINGLE_SHOT。
       同时 ACTF_NO_SHIFT：它不是光标动作，不该被位移模式套上 Shift。 */
    { ACT_CAPSLOCK,  VK_TAB,     VK_CAPITAL,  -1, ACTF_SINGLE_SHOT | ACTF_NO_SHIFT },
    /* 词跳：方向写在表里，免得运行时靠 ACT_WORD_L/R 的数值去反推左右 */
    { ACT_WORD_L,    'N',        0,          DIR_LEFT,  ACTF_WORDJUMP },
    { ACT_WORD_R,    'M',        0,          DIR_RIGHT, ACTF_WORDJUMP },
    /* 诊断：没有触发键（由 PROBE_KEY 的特殊路径投递） */
    { ACT_PROBE,     0,          0,          -1, ACTF_PROBE },
};
#define ACT_N ((int)(sizeof(kActions) / sizeof(kActions[0])))

static const ActionDef* FindAct(int act) {
    for (int i = 0; i < ACT_N; i++)
        if (kActions[i].act == act) return &kActions[i];
    return NULL;
}

int ActOfVk(int vk) {
    if (!vk) return ACT_NONE;
    for (int i = 0; i < ACT_N; i++)
        if (kActions[i].vk && kActions[i].vk == vk) return kActions[i].act;
    return ACT_NONE;
}

int ActOutVk(int act) {
    const ActionDef* d = FindAct(act);
    return d ? d->outVk : 0;
}

int ActWordDir(int act) {
    const ActionDef* d = FindAct(act);
    if (!d || !(d->flags & ACTF_WORDJUMP)) return -1;
    return d->dir;
}

BOOL ActHasFlag(int act, unsigned flag) {
    const ActionDef* d = FindAct(act);
    return d ? ((d->flags & flag) != 0) : FALSE;
}

/* ================= 修饰（和弦）模式表 ================= */

typedef struct {
    int          id;
    const char*  name;      /* 日志用（ASCII）                    */
    const char*  iniKey;    /* [General] 里对应的配置项名（ASCII） */
    const char*  defKeys;   /* 默认激活键文本（cap.h 里单点定义）  */
    int          keys[MODE_KEY_MAX];   /* 当前生效的激活键          */
    int          nKeys;
    volatile DWORD downMask;           /* bit i = keys[i] 物理按住（钩子线程写） */
    volatile DWORD pendMask;           /* bit i = 本次和弦内按下过（门闩）      */
} ModeDef;

static ModeDef kModes[MODE_COUNT] = {
    { MODE_ENHANCE, "enhance", "EnhanceKeys", MODE_ENHANCE_KEYS_DEFAULT, { 0 }, 0, 0, 0 },
    { MODE_SHIFT,   "shift",   "ShiftKeys",   MODE_SHIFT_KEYS_DEFAULT,   { 0 }, 0, 0, 0 },
};

static ModeDef* ModeOf(int mode) {
    if (mode < 0 || mode >= MODE_COUNT) return NULL;
    return &kModes[mode];
}

/* 窄串 -> 宽串（配置项的键名本身是 ASCII，转换不会丢字符） */
static void Widen(const char* s, WCHAR* out, size_t cap) {
    size_t i = 0;
    for (; s[i] && i + 1 < cap; i++) out[i] = (WCHAR)(unsigned char)s[i];
    out[i] = 0;
}

/* ---- 键名 -> 虚拟键 ----
   为什么要有名字表而不是只认单字符：修饰键是非打印键，配置里写不出字符，
   只能写名字（Alt / LAlt / RAlt / Ctrl / Shift / Win）。
   名字比较不区分大小写（_strnicmp）；**单字符键名也必须大小写都认** ——
   默认值写的是 "Alt, D"，配置文件里用户也常写大写，只认小写会让默认的
   D 悄悄解析不出来（增强模式退化成只有 Alt），见 VkOfName。 */
static const struct { const char* name; int vk; } kKeyNames[] = {
    { "alt",      VK_LMENU    },   /* 为兼容习惯：Alt = 左 Alt */
    { "lalt",     VK_LMENU    },
    { "ralt",     VK_RMENU    },
    { "ctrl",     VK_LCONTROL },
    { "control",  VK_LCONTROL },
    { "lctrl",    VK_LCONTROL },
    { "lcontrol", VK_LCONTROL },
    { "rctrl",    VK_RCONTROL },
    { "rcontrol", VK_RCONTROL },
    { "shift",    VK_LSHIFT   },
    { "lshift",   VK_LSHIFT   },
    { "rshift",   VK_RSHIFT   },
    { "win",      VK_LWIN     },
    { "lwin",     VK_LWIN     },
    { "rwin",     VK_RWIN     },
    { "space",    VK_SPACE    },
    { "tab",      VK_TAB      },
    { "enter",    VK_RETURN   },
    { "backspace",VK_BACK     },
    { "esc",      VK_ESCAPE   },
};
#define KEY_NAME_N ((int)(sizeof(kKeyNames) / sizeof(kKeyNames[0])))

static int VkOfName(const char* tok, size_t len) {
    if (len == 1) {
        char c = tok[0];
        if (c >= 'a' && c <= 'z') return (int)(c - 'a' + 'A');
        if (c >= 'A' && c <= 'Z') return (int)c;   /* 大写单字符同样有效 */
        if (c >= '0' && c <= '9') return (int)c;
        if (c == ';') return VK_OEM_1;
        return 0;
    }
    for (int i = 0; i < KEY_NAME_N; i++) {
        if (strlen(kKeyNames[i].name) == len &&
            _strnicmp(kKeyNames[i].name, tok, len) == 0)
            return kKeyNames[i].vk;
    }
    return 0;
}

/* 解析激活键列表：逗号 / 空格 / 制表符 / '+' 都是分隔符（三种写法都常见）。
   认不出的名字**跳过并保留原表**：配置写错不该让功能整体失效。
   返回写出的键数（0 表示一个都没认出来，调用方应回退默认值）。 */
static int ParseKeys(const char* text, int* out, int cap) {
    int n = 0;
    const char* p = text;
    while (*p && n < cap) {
        while (*p == ' ' || *p == '\t' || *p == ',' || *p == '+' ||
               *p == '\r' || *p == '\n') p++;
        if (!*p) break;
        const char* s = p;
        while (*p && *p != ' ' && *p != '\t' && *p != ',' && *p != '+' &&
               *p != '\r' && *p != '\n') p++;
        int vk = VkOfName(s, (size_t)(p - s));
        if (vk) out[n++] = vk;
    }
    return n;
}

/* 宽字符版：配置文件里的值读出来是 WCHAR（可含中文注释外的任意字符，
   但键名本身是 ASCII）。为了与 ASCII 默认值共用同一个解析器，
   这里先窄化 —— 非 ASCII 字符一律丢弃，它们本来也不可能是合法键名。 */
static int ParseKeysW(const WCHAR* text, int* out, int cap) {
    char buf[MODE_KEYS_MAXLEN];
    int i = 0;
    for (; text && text[i] && i < MODE_KEYS_MAXLEN - 1; i++)
        buf[i] = (text[i] < 128) ? (char)text[i] : ' ';
    buf[i] = 0;
    return ParseKeys(buf, out, cap);
}

/* 给一个模式换一套激活键。n <= 0 时不动（调用方保证回退语义）。 */
static void ModeSetKeys(int mode, const int* keys, int n) {
    ModeDef* m = ModeOf(mode);
    if (!m || n <= 0) return;
    if (n > MODE_KEY_MAX) n = MODE_KEY_MAX;
    m->nKeys = n;
    for (int i = 0; i < n; i++) m->keys[i] = keys[i];
    /* 键集合变了，旧位图不再对应任何键：清空，避免残留位指向错误的键 */
    m->downMask = 0;
    m->pendMask = 0;
}

/* 启动时装配：配置里的键列表优先，没配/全认不出则用表里的默认值。
   注意默认值也走**同一个解析器**（MODE_*_KEYS_DEFAULT 是文本）——
   这样"默认配置"和"用户配置"走同一条路径，默认值天然被测试覆盖。 */
void KeyMapInit(void) {
    for (int i = 0; i < MODE_COUNT; i++) {
        ModeDef* m = &kModes[i];
        int keys[MODE_KEY_MAX];
        int n = 0;

        WCHAR iniKey[64], cfg[MODE_KEYS_MAXLEN];
        Widen(m->iniKey, iniKey, 64);
        if (ConfigGetStr(iniKey, cfg, MODE_KEYS_MAXLEN))
            n = ParseKeysW(cfg, keys, MODE_KEY_MAX);
        if (n == 0)   /* 未配置 / 空 / 一个名字都不认得 -> 回退默认 */
            n = ParseKeys(m->defKeys, keys, MODE_KEY_MAX);
        ModeSetKeys(i, keys, n);
    }
    /* 刻意不在这里写日志：本项目约定"启动不写日志文件"（见 main.c 的
       CapLog 注释与 WinMain 收尾）。按键位写进 C+` 记录开始时的那条
       "配置: ..." 日志，排查"改了配置没生效"时一样看得到。 */
}

int ModeFindByVk(int vk) {
    if (!vk) return -1;
    for (int m = 0; m < MODE_COUNT; m++) {
        const ModeDef* d = &kModes[m];
        for (int i = 0; i < d->nKeys; i++)
            if (d->keys[i] == vk) return m;
    }
    return -1;
}

/* 钩子线程调用。位图按"键在 keys[] 里的下标"记 ——
   这样一个模式挂多个键时，松开其中一个不会误清另一个。 */
void ModeKeyEvent(int mode, int vk, BOOL down) {
    ModeDef* m = ModeOf(mode);
    if (!m) return;
    for (int i = 0; i < m->nKeys; i++) {
        if (m->keys[i] != vk) continue;
        DWORD bit = (DWORD)1 << i;
        if (down) { m->downMask |= bit; m->pendMask |= bit; }
        else      { m->downMask &= ~bit; m->pendMask &= ~bit; }
        return;
    }
}

BOOL ModeDown(int mode) {
    const ModeDef* m = ModeOf(mode);
    return m ? (m->downMask != 0) : FALSE;
}

BOOL ModePending(int mode) {
    const ModeDef* m = ModeOf(mode);
    return m ? (m->pendMask != 0) : FALSE;
}

void ModeResetPending(void) {
    for (int i = 0; i < MODE_COUNT; i++) kModes[i].pendMask = 0;
}

void ModeResetAll(void) {
    for (int i = 0; i < MODE_COUNT; i++) {
        kModes[i].pendMask = 0;
        kModes[i].downMask = 0;
    }
}

int ModeKeyCount(int mode) {
    const ModeDef* m = ModeOf(mode);
    return m ? m->nKeys : 0;
}

int ModeKeyAt(int mode, int i) {
    const ModeDef* m = ModeOf(mode);
    if (!m || i < 0 || i >= m->nKeys) return 0;
    return m->keys[i];
}

/* 该虚拟键最简短的名字（"ctrl" 比 "lcontrol" 可读）；无则 -1。 */
static int ShortestNameIdx(int vk) {
    int best = -1; size_t bestLen = 0;
    for (int k = 0; k < KEY_NAME_N; k++) {
        if (kKeyNames[k].vk != vk) continue;
        size_t l = strlen(kKeyNames[k].name);
        if (best < 0 || l < bestLen) { best = k; bestLen = l; }
    }
    return best;
}

/* 虚拟键 -> 可读文本（"Alt" / "D" / "vk5B"）。 */
static void VkText(int vk, WCHAR* out, size_t cap) {
    int k = ShortestNameIdx(vk);
    if (k >= 0) {
        WCHAR nm[32];
        MultiByteToWideChar(CP_ACP, 0, kKeyNames[k].name, -1, nm, 32);
        if (nm[0] >= L'a' && nm[0] <= L'z') nm[0] = (WCHAR)(nm[0] - L'a' + L'A');
        _snwprintf_s(out, cap, _TRUNCATE, L"%s", nm);
    } else if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) {
        _snwprintf_s(out, cap, _TRUNCATE, L"%c", (WCHAR)vk);
    } else {
        _snwprintf_s(out, cap, _TRUNCATE, L"vk%02X", vk);
    }
}

/* 拼可读文本，便于日志与测试断言（默认增强模式 -> "Alt, D"）。 */
int ModeKeysText(int mode, WCHAR* out, size_t cap) {
    const ModeDef* m = ModeOf(mode);
    if (!out || cap == 0) return 0;
    out[0] = 0;
    if (!m) return 0;
    WCHAR* w = out;
    size_t left = cap;
    for (int i = 0; i < m->nKeys; i++) {
        WCHAR one[48];
        VkText(m->keys[i], one, 48);
        int wrote = _snwprintf_s(w, left, _TRUNCATE,
                                 (i == 0) ? L"%s" : L", %s", one);
        if (wrote < 0) break;                 /* 缓冲截断：停下即可 */
        w += wrote; left -= (size_t)wrote;
    }
    return m->nKeys;
}
