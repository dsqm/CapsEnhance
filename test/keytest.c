/* keytest.c - Chromium key delivery probe (Edge/VSCode share this engine).
 *
 * Why: Caps+U / Caps+O (word jump) fails in Notepad3 / EmEditor / VSCode.
 *      Suspect: the way we build SendInput events (scan code, EXTENDED flag,
 *      batched vs one-by-one, whether Ctrl is really held) makes the target
 *      receive a different key than intended. This tool runs every variant
 *      against one window and reports what the target actually received and
 *      whether the caret really moved.
 *
 * Pairs with test/keytest.html: the page mirrors keydown/keyup plus
 * selectionStart into document.title, so we can read results back from the
 * window title (no UIA needed).
 *
 * Usage: open keytest.html in Edge first, then run keytest.exe.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>

static HWND g_hwnd = NULL;

static int g_count = 0;
static BOOL CALLBACK FindCb(HWND h, LPARAM lp) {
    (void)lp;
    char cls[256] = "", ttl[512] = "";
    if (!IsWindowVisible(h)) return TRUE;
    GetClassNameA(h, cls, 256);
    if (!GetWindowTextA(h, ttl, 512)) ttl[0] = 0;
    if (g_count < 40) printf("  [enum] hwnd=0x%p cls=%s title=%.110s\n", (void*)h, cls, ttl);
    g_count++;
    if (strstr(ttl, "KEYTEST") == NULL) return TRUE;
    g_hwnd = h;
    return FALSE;
}

static void PutTitle(void) {
    char t[600] = "";
    GetWindowTextA(g_hwnd, t, 600);
    char* p = strstr(t, " - Microsoft");
    if (p) *p = 0;
    printf("      -> %s\n", t);
}

/* one keyboard event: vk / scan / extended / up */
static void One(WORD vk, WORD sc, BOOL ext, BOOL up) {
    INPUT i;
    memset(&i, 0, sizeof(i));
    i.type = INPUT_KEYBOARD;
    i.ki.wVk = vk;
    i.ki.wScan = sc;
    i.ki.dwFlags = (ext ? KEYEVENTF_EXTENDEDKEY : 0) | (up ? KEYEVENTF_KEYUP : 0);
    SendInput(1, &i, sizeof(INPUT));
}

static WORD ScOf(WORD vk) { return (WORD)MapVirtualKeyW(vk, MAPVK_VK_TO_VSC); }

/* single arrow key, no modifier.
   mode: 0=scan,noEXT  1=scan,EXT  2=noscan,EXT  3=noscan,noEXT */
static void VariantArrow(const char* name, WORD vk, int mode, BOOL perEvent) {
    printf("\n[arrow] %s\n", name);
    WORD sc = (mode == 0 || mode == 1) ? ScOf(vk) : 0;
    BOOL ex = (mode == 1 || mode == 2);
    if (perEvent) {
        One(vk, sc, ex, FALSE);
        Sleep(30);
        One(vk, sc, ex, TRUE);
    } else {
        INPUT arr[2]; memset(arr, 0, sizeof(arr));
        arr[0].type = INPUT_KEYBOARD; arr[0].ki.wVk = vk;
        arr[0].ki.wScan = sc;
        arr[0].ki.dwFlags = ex ? KEYEVENTF_EXTENDEDKEY : 0;
        arr[1] = arr[0]; arr[1].ki.dwFlags |= KEYEVENTF_KEYUP;
        SendInput(2, arr, sizeof(INPUT));
    }
    Sleep(250);
    PutTitle();
}

/* Ctrl+Arrow (native word jump) variants */
static void VariantCtrlArrow(const char* name, WORD vk, BOOL ext,
                             BOOL batch, BOOL holdCtrl) {
    printf("\n[ctrl+arrow] %s\n", name);
    WORD sc = ext ? ScOf(vk) : ScOf(vk);
    WORD scc = ScOf(VK_LCONTROL);
    if (batch) {
        INPUT a[6]; memset(a, 0, sizeof(a));
        int k = 0;
        a[k].type = INPUT_KEYBOARD; a[k].ki.wVk = VK_LCONTROL; a[k].ki.wScan = scc; k++;
        if (!holdCtrl) {
            a[k].type = INPUT_KEYBOARD; a[k].ki.wVk = VK_LCONTROL;
            a[k].ki.wScan = scc; a[k].ki.dwFlags = KEYEVENTF_KEYUP; k++;
        }
        a[k].type = INPUT_KEYBOARD; a[k].ki.wVk = vk; a[k].ki.wScan = sc;
        a[k].ki.dwFlags = ext ? KEYEVENTF_EXTENDEDKEY : 0; k++;
        a[k] = a[k - 1]; a[k].ki.dwFlags |= KEYEVENTF_KEYUP; k++;
        a[k].type = INPUT_KEYBOARD; a[k].ki.wVk = VK_LCONTROL; a[k].ki.wScan = scc;
        a[k].ki.dwFlags = KEYEVENTF_KEYUP; k++;
        SendInput(k, a, sizeof(INPUT));
    } else {
        One(VK_LCONTROL, scc, FALSE, FALSE); Sleep(25);
        One(vk, sc, ext, FALSE);              Sleep(25);
        One(vk, sc, ext, TRUE);               Sleep(25);
        One(VK_LCONTROL, scc, FALSE, TRUE);
    }
    Sleep(250);
    PutTitle();
}

int main(int argc, char** argv) {
    EnumWindows(FindCb, 0);
    if (!g_hwnd) {
        printf("no window with KEYTEST in title. open test/keytest.html in Edge first.\n");
        return 1;
    }
    printf("target hwnd=0x%p\n", (void*)g_hwnd);

    /* "reload": 只把页面刷新一下（改了 html 后用），不做按键实验 */
    if (argc > 1 && strcmp(argv[1], "reload") == 0) {
        SetForegroundWindow(g_hwnd);
        Sleep(200);
        One(VK_F5, ScOf(VK_F5), FALSE, FALSE); Sleep(30);
        One(VK_F5, ScOf(VK_F5), FALSE, TRUE);
        Sleep(800);
        PutTitle();
        return 0;
    }

    if (!SetForegroundWindow(g_hwnd)) {
        DWORD myTid = GetCurrentThreadId();
        DWORD fgTid = GetWindowThreadProcessId(GetForegroundWindow(), NULL);
        if (fgTid && AttachThreadInput(myTid, fgTid, TRUE)) {
            SetForegroundWindow(g_hwnd);
            AttachThreadInput(myTid, fgTid, FALSE);
        }
    }
    Sleep(700);
    printf("\n== initial ==\n"); PutTitle();
    printf("fg=%d\n", (int)(GetForegroundWindow() == g_hwnd));

    printf("\n=== A. plain arrow variants (VK_RIGHT) ===");
    VariantArrow("A1 scan,noEXT  (current CapsEnhance style)", VK_RIGHT, 0, TRUE);
    VariantArrow("A2 scan,EXT", VK_RIGHT, 1, TRUE);
    VariantArrow("A3 noscan,EXT", VK_RIGHT, 2, TRUE);
    VariantArrow("A4 noscan,noEXT", VK_RIGHT, 3, TRUE);

    printf("\n=== B. plain arrow variants (VK_LEFT) ===");
    VariantArrow("B1 scan,noEXT  (current CapsEnhance style)", VK_LEFT, 0, TRUE);
    VariantArrow("B2 scan,EXT", VK_LEFT, 1, TRUE);
    VariantArrow("B3 noscan,EXT", VK_LEFT, 2, TRUE);
    VariantArrow("B4 noscan,noEXT", VK_LEFT, 3, TRUE);

    printf("\n=== C. batched vs one-by-one (VK_LEFT, scan,noEXT) ===");
    VariantArrow("C1 batched SendInput(2)", VK_LEFT, 0, FALSE);
    VariantArrow("C2 one-by-one SendInput", VK_LEFT, 0, TRUE);

    printf("\n=== D. Ctrl+Arrow: current impl vs fixed ===");
    VariantCtrlArrow("D1 current: batch, Ctrl down+up, noEXT", VK_LEFT, FALSE, TRUE, FALSE);
    VariantCtrlArrow("D2 batch, Ctrl held, noEXT", VK_LEFT, FALSE, TRUE, TRUE);
    VariantCtrlArrow("D3 batch, Ctrl held, EXT", VK_LEFT, TRUE, TRUE, TRUE);
    VariantCtrlArrow("D4 one-by-one, Ctrl held, EXT", VK_LEFT, TRUE, FALSE, TRUE);
    VariantCtrlArrow("D5 one-by-one, Ctrl held, EXT (RIGHT)", VK_RIGHT, TRUE, FALSE, TRUE);

    printf("\ndone.\n");
    return 0;
}
