#include <windows.h>
static LRESULT CALLBACK P(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }
int WINAPI WinMain(HINSTANCE i, HINSTANCE p, LPSTR c, int n) {
    (void)p; (void)c; (void)n;
    WNDCLASSW wc; ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = P; wc.hInstance = i; wc.lpszClassName = L"PinProbeTarget";
    RegisterClassW(&wc);
    HWND w = CreateWindowW(L"PinProbeTarget", L"PinProbe", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                           400, 300, 600, 400, NULL, NULL, i, NULL);
    if (!w) return 1;
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    return 0;
}
