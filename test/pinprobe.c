/* 临时验收探针（不入库）：量移植版的三件事
     ① 边框窗矩形 == 目标 DWM 外框四边各外扩 thickness；
     ② 屏幕像素上真的有强调色；
     ③ 60 次定节奏移动：未钉 vs 钉住的耗时差、每帧跟随延迟（最大）。 */
#include "cap.h"
#include <dwmapi.h>
#include <stdio.h>
#include <string.h>
#pragma comment(lib,"dwmapi.lib")

std::atomic<bool> g_recording{ false };
GlobalState g_state;
HWND g_mainWnd = NULL;
void CapLog(const char* fmt, ...) { (void)fmt; }
BOOL WinIsShellClass(HWND hwnd) { (void)hwnd; return FALSE; }

static void DpiAware(void){ typedef BOOL (WINAPI* PFN)(HANDLE); HMODULE u=GetModuleHandleW(L"user32.dll");
    PFN f=u?(PFN)GetProcAddress(u,"SetProcessDpiAwarenessContext"):NULL; if(f) f((HANDLE)(INT_PTR)-4); }
static DWORD g_pid=0; static HWND g_found=NULL;
static BOOL CALLBACK FindByPid(HWND h, LPARAM lp){ (void)lp; DWORD pid=0; GetWindowThreadProcessId(h,&pid);
    if(pid!=g_pid||!IsWindowVisible(h)) return TRUE; if(GetWindow(h,GW_OWNER)) return TRUE;
    RECT r; if(!GetWindowRect(h,&r)||(r.right-r.left)<120) return TRUE; g_found=h; return FALSE; }

static BOOL BorderMatches(HWND t, HWND b, int th){
    RECT fb; memset(&fb,0,sizeof(fb));
    if (FAILED(DwmGetWindowAttribute(t, DWMWA_EXTENDED_FRAME_BOUNDS, &fb, sizeof(fb)))) return FALSE;
    RECT br; if(!GetWindowRect(b,&br)) return FALSE;
    return br.left==fb.left-th && br.top==fb.top-th && br.right==fb.right+th && br.bottom==fb.bottom+th;
}
static COLORREF Accent(void){
    DWORD v=0,sz=sizeof(v);
    if(RegGetValueW(HKEY_CURRENT_USER,L"Software\\Microsoft\\Windows\\DWM",L"AccentColor",
                    RRF_RT_REG_DWORD,NULL,&v,&sz)==ERROR_SUCCESS)
        return RGB(v&0xFF,(v>>8)&0xFF,(v>>16)&0xFF);
    return RGB(0,0x99,0xCC);
}
int main(void){
    DpiAware();
    STARTUPINFOW si; PROCESS_INFORMATION pi; memset(&si,0,sizeof(si)); memset(&pi,0,sizeof(pi)); si.cb=sizeof(si);
    WCHAR cmd[]=L"obj\\test\\pintarget.exe";
    if(!CreateProcessW(NULL,cmd,NULL,NULL,FALSE,0,NULL,NULL,&si,&pi)){printf("no target\n");return 1;}
    g_pid=pi.dwProcessId;
    for(int i=0;i<60&&!g_found;i++){Sleep(100);EnumWindows(FindByPid,0);}
    if(!g_found){printf("target not found\n");TerminateProcess(pi.hProcess,0);return 1;}
    HWND t=g_found; Sleep(500);

    LARGE_INTEGER fq; QueryPerformanceFrequency(&fq);
    int th=2; { typedef UINT (WINAPI* PFN)(HWND); HMODULE u=GetModuleHandleW(L"user32.dll");
                PFN f=u?(PFN)GetProcAddress(u,"GetDpiForWindow"):NULL; UINT d=f?f(t):96; th=MulDiv(2,(int)d,96); }

    RECT r0; GetWindowRect(t,&r0);

    /* ① 计时轮：纯"移动 + 等 16ms"，两边都不轮询 —— 口径一致才谈得上差值 */
    double baseMs=0;
    { LARGE_INTEGER a,b; QueryPerformanceCounter(&a);
      for(int i=0;i<60;i++){
        RECT rr; GetWindowRect(t,&rr);
        SetWindowPos(t,NULL,(int)rr.left+2,(int)rr.top,NULL,0,
                     SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE|SWP_ASYNCWINDOWPOS);
        Sleep(16);
      }
      QueryPerformanceCounter(&b); baseMs=(double)(b.QuadPart-a.QuadPart)*1000.0/(double)fq.QuadPart; }

    PinInit(); PinToggle(t); Sleep(700);
    HWND border=FindWindowW(L"CapsEnhancePinBorder",NULL);
    if(!border){printf("border not created\n");PinShutdown();TerminateProcess(pi.hProcess,0);return 1;}

    /* ② 几何 + 像素 */
    RECT fb; memset(&fb,0,sizeof(fb));
    DwmGetWindowAttribute(t, DWMWA_EXTENDED_FRAME_BOUNDS, &fb, sizeof(fb));
    RECT br; GetWindowRect(border,&br);
    printf("几何: border=(%ld,%ld)-(%ld,%ld)  期望=(%ld,%ld)-(%ld,%ld)  %s\n",
        br.left,br.top,br.right,br.bottom, fb.left-th,fb.top-th,fb.right+th,fb.bottom+th,
        BorderMatches(t,border,th)?"一致":"**不一致**");
    COLORREF want=Accent();
    { int mx=(int)((fb.left+fb.right)/2), my=(int)((fb.top+fb.bottom)/2);
      HDC dc=GetDC(NULL);
      /* 沿左边缘横向扫一条带：PowerToys 的环是"从窗外 1px 起、向内压 1px"，
         粗细 2 时可见宽度只有 1px，必然带抗锯齿 —— 精确比色没有意义，
         这里把整条带打出来，并给出"最接近强调色"的那一像素。 */
      int bestD=9999, bestX=0; COLORREF bestC=0;
      printf("左边缘像素带（x = frame.left-3 .. frame.left+1）:\n   ");
      for(int x=(int)fb.left-3; x<=(int)fb.left+1; x++){
          COLORREF c=GetPixel(dc,x,my);
          int d=abs((int)GetRValue(c)-(int)GetRValue(want))+
                abs((int)GetGValue(c)-(int)GetGValue(want))+
                abs((int)GetBValue(c)-(int)GetBValue(want));
          printf(" %02X%02X%02X", GetRValue(c),GetGValue(c),GetBValue(c));
          if(d<bestD){ bestD=d; bestX=x; bestC=c; }
      }
      ReleaseDC(NULL,dc);
      printf("\n   强调色 #%02X%02X%02X  最接近的一格 x=%d 是 #%02X%02X%02X（差 %d/765）\n",
          GetRValue(want),GetGValue(want),GetBValue(want), bestX,
          GetRValue(bestC),GetGValue(bestC),GetBValue(bestC), bestD);
    }

    /* ③-a 计时轮（钉住）：与上面完全同口径 */
    double pinMs=0;
    { LARGE_INTEGER a,b; QueryPerformanceCounter(&a);
      for(int i=0;i<60;i++){
        RECT rr; GetWindowRect(t,&rr);
        SetWindowPos(t,NULL,(int)rr.left+2,(int)rr.top,NULL,0,
                     SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE|SWP_ASYNCWINDOWPOS);
        Sleep(16);
      }
      QueryPerformanceCounter(&b); pinMs=(double)(b.QuadPart-a.QuadPart)*1000.0/(double)fq.QuadPart; }

    /* ③-b 延迟轮：每移动一次，等到边框就位为止（这一轮只量延迟，不计时） */
    int worst=0; long sum=0;
    for(int i=0;i<60;i++){
        RECT rr; GetWindowRect(t,&rr);
        SetWindowPos(t,NULL,(int)rr.left+2,(int)rr.top,NULL,0,
                     SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE|SWP_ASYNCWINDOWPOS);
        LARGE_INTEGER s0,s1; QueryPerformanceCounter(&s0);
        int lag=999;
        for(int k=0;k<400;k++){
            if(BorderMatches(t,border,th)){ QueryPerformanceCounter(&s1);
                lag=(int)((double)(s1.QuadPart-s0.QuadPart)*1000.0/(double)fq.QuadPart); break; }
            Sleep(1);
        }
        sum+=lag; if(lag>worst) worst=lag;
        Sleep(16);
    }

    printf("60 次定节奏移动: 未钉 %.0f ms / 钉住 %.0f ms  -> 每帧多 %.1f ms\n",
           baseMs, pinMs, (pinMs-baseMs)/60.0);
    printf("跟随延迟: 平均 %.1f ms, 最差 %d ms\n", (double)sum/60.0, worst);

    PinToggle(t); Sleep(300);
    printf("取消后: count=%d topmost=%d border=%s\n", PinCount(),
        (int)((GetWindowLongPtrW(t,GWL_EXSTYLE)&WS_EX_TOPMOST)!=0),
        FindWindowW(L"CapsEnhancePinBorder",NULL)?"还在":"已销毁");
    PinShutdown();
    TerminateProcess(pi.hProcess,0); CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    return 0;
}
