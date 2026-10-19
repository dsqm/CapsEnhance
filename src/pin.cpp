/* ================= 窗口置顶（`Caps + T`）—— PowerToys AlwaysOnTop 的移植 =================

   本文件是 **Microsoft PowerToys 的 AlwaysOnTop 模块的移植版**，不是"自己写一个
   差不多的"。原始代码：

     src/modules/AlwaysOnTop/AlwaysOnTop/
       AlwaysOnTop.cpp      模块主体：热键 / 事件订阅 / 钉住表 / 边框分配
       WindowBorder.cpp     每个被钉窗口一个浮层窗（layered + color-key + blur-behind）
       FrameDrawer.cpp      D2D 画"外矩形减内矩形"的环
       ScalingUtils.cpp     DPI 缩放
       WindowCornersUtil.cpp DWM 圆角偏好 -> 半径
       VirtualDesktopUtils.cpp 当前虚拟桌面判定
       Sound.h              Speech On / Speech Sleep

   保留的是**全部逻辑与窗口模型**：7 个 WinEvent 钩子、每边框 100ms 定时器、
   `SetProp` 标记、`IsTopmost` 判据、最小化丢边框/恢复重钉、前台变化时刷新、
   layered + color-key + DWM blur-behind 的透明做法、D2D HwndRenderTarget 绘制。
   上一版（GDI+ + UpdateLayeredWindow）在结构上就是另一个东西，跟手与观感都不同。

   ★ 去掉了什么（这些是 PowerToys 的**外壳**，与本项目的形态不符）：
     · Settings / SettingsObserver / FileWatcher —— 本程序零配置，参数写成下面的
       常量（值取 PowerToys 的默认值，边框粗细取 2）。
     · 系统菜单集成（"窗口右键标题栏加一项置顶"）、不透明度热键
       （Win+Ctrl+/-）、游戏模式判定 —— 本程序不做这三项。
     · 通知（NotificationUtil）—— 提权失败改用本程序的托盘气泡（WM_PIN_ELEVATED）。
     · winrt（com_ptr / UISettings）—— 换成 WRL 的 ComPtr；强调色改读注册表
       （同一个值的来源，见 ReadAccentColor 的注释）。
     · trace / Logger —— 换成 CapLog，且只在记录模式（C+`）下写。

   ★ 为什么本文件**不 include cap.h**：cap.h 为了让 C 风格的 COM 代码编过，
     定义了 CINTERFACE + COBJMACROS，那会让 SDK 交出 C 形态的 vtable ——
     WRL 的 ComPtr 与 D2D 的 C++ 接口都活不了。所以这里直接 include windows.h，
     并把要用到的四个外部符号（日志 / 记录开关 / 主窗口 / 壳窗口判定）自己声明。
     本项目的独立诊断工具（uiprobe / wspset）用的是同一条规矩。

   ★ 线程模型：整个模块住在**自己的线程**里（PinInit 起、PinShutdown 收）。
     主线程是低级钩子的宿主、动作线程是串行队列，两边都不能被"对方窗口卡住"的
     SetWindowPos 拖住；而 WinEvent 回调与定时器都要求"安装了它的线程在泵消息"。
     动作线程只 PostMessage 一个 HWND 过来。 */

#include <windows.h>
#include <d2d1.h>
#include <d2d1helper.h>
#include <dwmapi.h>
#include <shobjidl.h>
#include <mmsystem.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <vector>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "winmm.lib")

using Microsoft::WRL::ComPtr;

/* ---------- 本模块依赖的四个外部符号（定义在 main.c / winmove.c） ----------
   刻意不在 pin.cpp 里 include cap.h：理由见文件头。 */
extern std::atomic<bool> g_recording;
extern HWND g_mainWnd;
void CapLog(const char* fmt, ...);
BOOL WinIsShellClass(HWND hwnd);

/* ---------- 主线程消息（与 cap_core.h 里的定义必须一致） ---------- */
#define WM_PIN_ELEVATED (WM_APP + 20)
#define WM_PIN_TOGGLE   (WM_APP + 41)
#define WM_PIN_UNPIN_ALL (WM_APP + 42)
#define WM_PIN_QUIT     (WM_APP + 43)

/* ================= 常量（原 Settings 的默认值，零配置） ================= */
namespace
{
    constexpr wchar_t TOOL_WINDOW_CLASS_NAME[] = L"CapsEnhancePinTool";
    constexpr wchar_t BORDER_WINDOW_CLASS_NAME[] = L"CapsEnhancePinBorder";
    constexpr wchar_t WINDOW_IS_PINNED_PROP[] = L"CapsEnhance_Pinned";

    constexpr int  FRAME_THICKNESS = 2;        /* PowerToys 默认 4；这里取 2（可见宽度 = 粗细 - 1） */
    constexpr int  FRAME_OPACITY = 100;        /* 百分比 */
    constexpr bool FRAME_ACCENT_COLOR = true;  /* 跟随系统强调色 */
    constexpr bool ROUND_CORNERS = true;       /* Win11 上跟随窗口圆角 */
    constexpr bool ENABLE_FRAME = true;
    constexpr bool ENABLE_SOUND = true;
    constexpr COLORREF FRAME_COLOR_FALLBACK = RGB(0x00, 0x99, 0xCC); /* PowerToys 的 FrameColor 默认 */
    constexpr size_t MAX_PINNED = 32;          /* 本程序的上限（PowerToys 无上限） */

    constexpr UINT_PTR REFRESH_BORDER_TIMER_ID = 123;
    constexpr UINT     REFRESH_BORDER_INTERVAL = 100;

    /* 只在记录模式下写日志（平时一个字都不写，与项目其它诊断同一规矩） */
    void PinLog(const char* fmt, ...)
    {
        if (!g_recording.load()) return;
        char buf[512];
        va_list ap;
        va_start(ap, fmt);
        _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
        va_end(ap);
        CapLog("  [置顶] %s", buf);
    }

    /* 系统强调色：PowerToys 走 UISettings.AccentColor（WinRT），这里读它的注册表
       来源 `HKCU\...\DWM\AccentColor`（ABGR）。同一个值，省掉一整套 WinRT。 */
    COLORREF ReadAccentColor()
    {
        DWORD v = 0, sz = sizeof(v);
        LONG rc = RegGetValueW(HKEY_CURRENT_USER,
                               L"Software\\Microsoft\\Windows\\DWM",
                               L"AccentColor",
                               RRF_RT_REG_DWORD, nullptr, &v, &sz);
        if (rc == ERROR_SUCCESS && sz == sizeof(v))
            return RGB(v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF);
        return FRAME_COLOR_FALLBACK;
    }
}

/* ================= ScalingUtils（原样移植） ================= */
namespace ScalingUtils
{
    float ScalingFactor(HWND window) noexcept
    {
        /* 原实现走 DPIAware::GetScreenDPIForWindow；本进程已声明 Per-Monitor V2，
           GetDpiForWindow 拿到的就是该窗口所在显示器的真实 DPI。 */
        typedef UINT(WINAPI* PFN_GetDpiForWindow)(HWND);
        static PFN_GetDpiForWindow pfn = nullptr;
        static bool tried = false;
        if (!tried)
        {
            tried = true;
            if (HMODULE u = GetModuleHandleW(L"user32.dll"))
                pfn = (PFN_GetDpiForWindow)GetProcAddress(u, "GetDpiForWindow");
        }
        if (!pfn || !window) return 1.0f;
        UINT dpi = pfn(window);
        if (!dpi) return 1.0f;
        return dpi / 96.0f;
    }
}

/* ================= WindowCornersUtil（原样移植） ================= */
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif

namespace WindowCornerUtils
{
    int CornerPreference(HWND window) noexcept
    {
        int cornerPreference = -1;
        HRESULT res = DwmGetWindowAttribute(window, DWMWA_WINDOW_CORNER_PREFERENCE,
                                            &cornerPreference, sizeof(cornerPreference));
        if (res != S_OK)
        {
            /* Win10 上没有这个属性：E_INVALIDARG 是正常情况，不刷日志 */
            if (res != E_INVALIDARG)
                PinLog("取圆角偏好失败 0x%08lX", (unsigned long)res);
        }
        return cornerPreference;
    }

    int CornersRadius(HWND window) noexcept
    {
        switch (CornerPreference(window))
        {
        case 2: return 8;   /* DWMWCP_ROUND */
        case 3: return 4;   /* DWMWCP_ROUNDSMALL */
        case 0: return 8;   /* DWMWCP_DEFAULT */
        default: return 0;  /* DONOTROUND / 拿不到 */
        }
    }
}

/* ================= Sound（原样移植） =================
   PowerToys 用相对路径 "Media\\Speech On.wav"，依赖系统的搜索路径；
   这里拼成 %SystemRoot%\Media\... 的绝对路径，找不到再退回蜂鸣 ——
   与 main.c 的 RecBeep 是同一条兜底思路。 */
namespace Sound
{
    enum class Type { On, Off, IncreaseOpacity, DecreaseOpacity };

    void Play(Type type)
    {
        const wchar_t* leaf = nullptr;
        switch (type)
        {
        case Type::On:  leaf = L"\\Media\\Speech On.wav"; break;
        case Type::Off: leaf = L"\\Media\\Speech Sleep.wav"; break;
        case Type::IncreaseOpacity: leaf = L"\\Media\\Windows Hardware Insert.wav"; break;
        case Type::DecreaseOpacity: leaf = L"\\Media\\Windows Hardware Remove.wav"; break;
        default: return;
        }

        WCHAR path[MAX_PATH];
        UINT n = GetWindowsDirectoryW(path, MAX_PATH);
        if (n > 0 && n < MAX_PATH - 32)
        {
            wcscat_s(path, MAX_PATH, leaf);
            if (PlaySoundW(path, nullptr, SND_FILENAME | SND_ASYNC | SND_NODEFAULT))
                return;
        }
        MessageBeep(type == Type::On ? MB_ICONASTERISK : MB_ICONHAND);
    }
}

/* ================= VirtualDesktopUtils（原样移植） ================= */
namespace
{
    class VirtualDesktopUtils
    {
    public:
        VirtualDesktopUtils()
        {
            HRESULT res = CoCreateInstance(CLSID_VirtualDesktopManager, nullptr, CLSCTX_ALL,
                                           IID_IVirtualDesktopManager,
                                           reinterpret_cast<void**>(m_vdManager.GetAddressOf()));
            if (FAILED(res))
                PinLog("VirtualDesktopManager 创建失败 0x%08lX", (unsigned long)res);
        }

        bool IsWindowOnCurrentDesktop(HWND window) const
        {
            GUID id{};
            return GetDesktopId(window, &id);
        }

    private:
        bool GetDesktopId(HWND window, GUID* id) const
        {
            BOOL isWindowOnCurrentDesktop = FALSE;
            if (m_vdManager &&
                m_vdManager->IsWindowOnCurrentVirtualDesktop(window, &isWindowOnCurrentDesktop) == S_OK &&
                isWindowOnCurrentDesktop)
            {
                /* 过滤开始菜单、任务切换器这类窗口（原注释同） */
                if (m_vdManager->GetWindowDesktopId(window, id) == S_OK && *id != GUID_NULL)
                    return true;
            }
            return false;
        }

        ComPtr<IVirtualDesktopManager> m_vdManager;
    };
}

/* ================= FrameDrawer（原样移植，winrt::com_ptr -> WRL ComPtr） ================= */
class FrameDrawer
{
public:
    explicit FrameDrawer(HWND window) : m_window(window) {}

    static std::unique_ptr<FrameDrawer> Create(HWND window)
    {
        auto self = std::make_unique<FrameDrawer>(window);
        if (self->Init()) return self;
        return nullptr;
    }

    void Show()
    {
        ShowWindow(m_window, SW_SHOWNA);
        Render();
    }

    void Hide() { ShowWindow(m_window, SW_HIDE); }

    void SetBorderRect(RECT windowRect, COLORREF rgb, float alpha, int thickness, float radius)
    {
        DrawableRect newSceneRect{};
        newSceneRect.borderColor = ConvertColor(rgb, alpha);
        newSceneRect.thickness = thickness;
        if (radius != 0)
        {
            newSceneRect.hasRoundedRect = true;
            newSceneRect.roundedRect = ConvertRect(windowRect, thickness, radius);
        }
        else
        {
            newSceneRect.hasRect = true;
            newSceneRect.rect = ConvertRect(windowRect, thickness);
        }

        const bool colorUpdated = std::memcmp(&m_sceneRect.borderColor, &newSceneRect.borderColor,
                                              sizeof(newSceneRect.borderColor)) != 0;
        const bool thicknessUpdated = m_sceneRect.thickness != newSceneRect.thickness;
        const bool rectangleUpdated =
            m_sceneRect.hasRect != newSceneRect.hasRect ||
            (m_sceneRect.hasRect && !AreEqual(m_sceneRect.rect, newSceneRect.rect));
        const bool roundedRectangleUpdated =
            m_sceneRect.hasRoundedRect != newSceneRect.hasRoundedRect ||
            (m_sceneRect.hasRoundedRect && !AreEqual(m_sceneRect.roundedRect, newSceneRect.roundedRect));
        const bool needsRedraw = colorUpdated || thicknessUpdated || rectangleUpdated || roundedRectangleUpdated;

        RECT clientRect{};
        if (FAILED(DwmGetWindowAttribute(m_window, DWMWA_EXTENDED_FRAME_BOUNDS,
                                         &clientRect, sizeof(clientRect))))
            return;

        m_sceneRect = newSceneRect;

        const D2D1_SIZE_U renderTargetSize = D2D1::SizeU(clientRect.right - clientRect.left,
                                                         clientRect.bottom - clientRect.top);
        const bool atTheDesiredSize = m_renderTarget &&
                                      renderTargetSize.width == m_rtWidth &&
                                      renderTargetSize.height == m_rtHeight;
        if (!atTheDesiredSize)
        {
            bool resizeOk = m_renderTarget &&
                            SUCCEEDED(m_renderTarget->Resize(renderTargetSize));
            if (resizeOk)
            {
                m_rtWidth = renderTargetSize.width;
                m_rtHeight = renderTargetSize.height;
            }
            else if (!CreateRenderTargets(clientRect))
            {
                PinLog("创建 D2D 渲染目标失败");
            }
        }

        if (colorUpdated && m_borderBrush)
            m_borderBrush->SetColor(m_sceneRect.borderColor);

        if (!atTheDesiredSize || needsRedraw)
            Render();
    }

private:
    /* 原实现用 std::optional 表达"这次是直角还是圆角"；本项目按 C++14 编，
       所以换成一对 has* 标志 + 值（语义完全相同，少一层模板）。 */
    struct DrawableRect
    {
        bool hasRect = false;
        bool hasRoundedRect = false;
        D2D1_RECT_F rect{};
        D2D1_ROUNDED_RECT roundedRect{};
        D2D1_COLOR_F borderColor{};
        int thickness = 0;
    };

    static bool AreEqual(const D2D1_RECT_F& l, const D2D1_RECT_F& r)
    {
        return l.left == r.left && l.top == r.top && l.right == r.right && l.bottom == r.bottom;
    }
    static bool AreEqual(const D2D1_ROUNDED_RECT& l, const D2D1_ROUNDED_RECT& r)
    {
        return AreEqual(l.rect, r.rect) && l.radiusX == r.radiusX && l.radiusY == r.radiusY;
    }

    static ID2D1Factory* GetD2DFactory()
    {
        static ID2D1Factory* factory = []
        {
            ID2D1Factory* res = nullptr;
            D2D1CreateFactory(D2D1_FACTORY_TYPE_MULTI_THREADED, &res);
            return res;
        }();
        return factory;
    }

    static D2D1_COLOR_F ConvertColor(COLORREF color, float alpha)
    {
        return D2D1::ColorF(GetRValue(color) / 255.f, GetGValue(color) / 255.f,
                            GetBValue(color) / 255.f, alpha);
    }

    static D2D1_ROUNDED_RECT ConvertRect(RECT rect, int thickness, float radius)
    {
        float halfThickness = thickness / 2.0f;
        /* 1 用来消掉边框与窗口之间那道缝（原注释同） */
        auto d2d1Rect = D2D1::RectF(static_cast<float>(rect.left) + halfThickness + 1,
                                    static_cast<float>(rect.top) + halfThickness + 1,
                                    static_cast<float>(rect.right) - halfThickness - 1,
                                    static_cast<float>(rect.bottom) - halfThickness - 1);
        return D2D1::RoundedRect(d2d1Rect, radius, radius);
    }

    static D2D1_RECT_F ConvertRect(RECT rect, int thickness)
    {
        float halfThickness = thickness / 2.0f;
        return D2D1::RectF(static_cast<float>(rect.left) + halfThickness + 1,
                           static_cast<float>(rect.top) + halfThickness + 1,
                           static_cast<float>(rect.right) - halfThickness - 1,
                           static_cast<float>(rect.bottom) - halfThickness - 1);
    }

    /* 外几何减内几何 = 环。原实现用 ID2D1GeometryGroup + FILL_MODE_ALTERNATE，
       这里照抄。 */
    ComPtr<ID2D1Geometry> CreateBorderGeometry(const DrawableRect& drawableRect)
    {
        ID2D1Factory* factory = GetD2DFactory();
        if (!factory) return nullptr;

        const float halfThickness = drawableRect.thickness / 2.0f;
        ComPtr<ID2D1Geometry> outerGeometry;
        ComPtr<ID2D1Geometry> innerGeometry;

        if (drawableRect.hasRoundedRect)
        {
            const auto& borderRect = drawableRect.roundedRect;
            const auto outerRect = D2D1::RoundedRect(
                D2D1::RectF(borderRect.rect.left - halfThickness,
                            borderRect.rect.top - halfThickness,
                            borderRect.rect.right + halfThickness,
                            borderRect.rect.bottom + halfThickness),
                borderRect.radiusX + halfThickness,
                borderRect.radiusY + halfThickness);
            const auto innerRect = D2D1::RoundedRect(
                D2D1::RectF(borderRect.rect.left + halfThickness,
                            borderRect.rect.top + halfThickness,
                            borderRect.rect.right - halfThickness,
                            borderRect.rect.bottom - halfThickness),
                (std::max)(borderRect.radiusX - halfThickness, 0.0f),
                (std::max)(borderRect.radiusY - halfThickness, 0.0f));

            ComPtr<ID2D1RoundedRectangleGeometry> outerRounded;
            if (FAILED(factory->CreateRoundedRectangleGeometry(outerRect, outerRounded.GetAddressOf())))
                return nullptr;
            outerGeometry = outerRounded;

            if (innerRect.rect.left >= innerRect.rect.right ||
                innerRect.rect.top >= innerRect.rect.bottom)
                return outerGeometry;

            ComPtr<ID2D1RoundedRectangleGeometry> innerRounded;
            if (FAILED(factory->CreateRoundedRectangleGeometry(innerRect, innerRounded.GetAddressOf())))
                return nullptr;
            innerGeometry = innerRounded;
        }
        else if (drawableRect.hasRect)
        {
            const auto& borderRect = drawableRect.rect;
            const auto outerRect = D2D1::RectF(borderRect.left - halfThickness,
                                               borderRect.top - halfThickness,
                                               borderRect.right + halfThickness,
                                               borderRect.bottom + halfThickness);
            const auto innerRect = D2D1::RectF(borderRect.left + halfThickness,
                                               borderRect.top + halfThickness,
                                               borderRect.right - halfThickness,
                                               borderRect.bottom - halfThickness);

            ComPtr<ID2D1RectangleGeometry> outerRectangle;
            if (FAILED(factory->CreateRectangleGeometry(outerRect, outerRectangle.GetAddressOf())))
                return nullptr;
            outerGeometry = outerRectangle;

            if (innerRect.left >= innerRect.right || innerRect.top >= innerRect.bottom)
                return outerGeometry;

            ComPtr<ID2D1RectangleGeometry> innerRectangle;
            if (FAILED(factory->CreateRectangleGeometry(innerRect, innerRectangle.GetAddressOf())))
                return nullptr;
            innerGeometry = innerRectangle;
        }

        if (!outerGeometry || !innerGeometry) return nullptr;

        ID2D1Geometry* geometries[] = { outerGeometry.Get(), innerGeometry.Get() };
        ComPtr<ID2D1GeometryGroup> borderGeometry;
        if (FAILED(factory->CreateGeometryGroup(D2D1_FILL_MODE_ALTERNATE, geometries,
                                                ARRAYSIZE(geometries), borderGeometry.GetAddressOf())))
            return nullptr;
        return borderGeometry;
    }

    void Render()
    {
        if (!m_renderTarget || !m_borderBrush) return;

        m_renderTarget->BeginDraw();
        m_renderTarget->Clear(D2D1::ColorF(0.f, 0.f, 0.f, 0.f));

        if (auto borderGeometry = CreateBorderGeometry(m_sceneRect))
            m_renderTarget->FillGeometry(borderGeometry.Get(), m_borderBrush.Get());

        HRESULT hr = m_renderTarget->EndDraw();
        if (hr == D2DERR_RECREATE_TARGET)
        {
            m_borderBrush.Reset();
            m_renderTarget.Reset();
            m_rtWidth = m_rtHeight = 0;
        }
        else if (FAILED(hr))
        {
            PinLog("画边框失败 0x%08lX", (unsigned long)hr);
        }
    }

    bool CreateRenderTargets(const RECT& clientRect)
    {
        constexpr float DPI = 96.f;   /* DPI 感知进程里恒用 96（原注释同） */
        const auto renderTargetProperties = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_UNKNOWN, D2D1_ALPHA_MODE_PREMULTIPLIED),
            DPI, DPI);

        const auto renderTargetSize = D2D1::SizeU(clientRect.right - clientRect.left,
                                                  clientRect.bottom - clientRect.top);
        if (m_renderTarget && renderTargetSize.width == m_rtWidth &&
            renderTargetSize.height == m_rtHeight)
            return true;

        m_renderTarget.Reset();
        m_borderBrush.Reset();

        ID2D1Factory* factory = GetD2DFactory();
        if (!factory) return false;

        const auto hwndRenderTargetProperties =
            D2D1::HwndRenderTargetProperties(m_window, renderTargetSize, D2D1_PRESENT_OPTIONS_NONE);

        HRESULT hr = factory->CreateHwndRenderTarget(renderTargetProperties, hwndRenderTargetProperties,
                                                     m_renderTarget.GetAddressOf());
        if (FAILED(hr) || !m_renderTarget) return false;

        m_renderTarget->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

        hr = m_renderTarget->CreateSolidColorBrush(m_sceneRect.borderColor,
                                                   m_borderBrush.GetAddressOf());
        if (FAILED(hr))
        {
            m_renderTarget.Reset();
            return false;
        }

        m_rtWidth = renderTargetSize.width;
        m_rtHeight = renderTargetSize.height;
        return true;
    }

    bool Init()
    {
        RECT clientRect{};
        if (FAILED(DwmGetWindowAttribute(m_window, DWMWA_EXTENDED_FRAME_BOUNDS,
                                         &clientRect, sizeof(clientRect))))
            return false;
        return CreateRenderTargets(clientRect);
    }

    HWND m_window = nullptr;
    UINT32 m_rtWidth = 0;
    UINT32 m_rtHeight = 0;
    ComPtr<ID2D1HwndRenderTarget> m_renderTarget;
    ComPtr<ID2D1SolidColorBrush> m_borderBrush;
    DrawableRect m_sceneRect{};
};

/* ================= WindowBorder（原样移植） ================= */
bool GetFrameRect(HWND window, RECT* out)
{
    RECT rect{};
    if (FAILED(DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &rect, sizeof(rect))))
        return false;

    int border = FRAME_THICKNESS;
    rect.top -= border;
    rect.left -= border;
    rect.right += border;
    rect.bottom += border;
    *out = rect;
    return true;
}

class WindowBorder
{
    explicit WindowBorder(HWND window) : m_trackingWindow(window) {}

public:
    static std::unique_ptr<WindowBorder> Create(HWND window, HINSTANCE hinstance)
    {
        auto self = std::unique_ptr<WindowBorder>(new WindowBorder(window));
        if (self->Init(hinstance)) return self;
        return nullptr;
    }

    ~WindowBorder()
    {
        if (m_frameDrawer)
        {
            m_frameDrawer->Hide();
            m_frameDrawer.reset();
        }
        if (m_window)
        {
            SetWindowLongPtrW(m_window, GWLP_USERDATA, 0);
            DestroyWindow(m_window);
        }
    }

    void UpdateBorderPosition() const
    {
        if (!m_trackingWindow || !m_frameDrawer || !m_window) return;

        RECT rect{};
        if (!GetFrameRect(m_trackingWindow, &rect))
        {
            m_frameDrawer->Hide();
            return;
        }
        SetWindowPos(m_window, m_trackingWindow, rect.left, rect.top,
                     rect.right - rect.left, rect.bottom - rect.top,
                     SWP_NOREDRAW | SWP_NOACTIVATE);
    }

    void UpdateBorderProperties() const
    {
        if (!m_trackingWindow || !m_frameDrawer) return;

        RECT windowRect{};
        if (!GetFrameRect(m_trackingWindow, &windowRect)) return;
        SetWindowPos(m_window, m_trackingWindow, windowRect.left, windowRect.top,
                     windowRect.right - windowRect.left, windowRect.bottom - windowRect.top,
                     SWP_NOREDRAW | SWP_NOACTIVATE);

        RECT frameRect{ 0, 0, windowRect.right - windowRect.left,
                        windowRect.bottom - windowRect.top };

        COLORREF color = FRAME_ACCENT_COLOR ? ReadAccentColor() : FRAME_COLOR_FALLBACK;
        float opacity = FRAME_OPACITY / 100.0f;
        float scalingFactor = ScalingUtils::ScalingFactor(m_trackingWindow);
        float thickness = FRAME_THICKNESS * scalingFactor;
        float cornerRadius = 0.0f;
        if (ROUND_CORNERS)
            cornerRadius = WindowCornerUtils::CornersRadius(m_trackingWindow) * scalingFactor;

        m_frameDrawer->SetBorderRect(frameRect, color, opacity,
                                     static_cast<int>(thickness), cornerRadius);
    }

private:
    static LRESULT CALLBACK s_WndProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
    {
        auto thisRef = reinterpret_cast<WindowBorder*>(GetWindowLongPtr(window, GWLP_USERDATA));
        if ((thisRef == nullptr) && (message == WM_CREATE))
        {
            auto createStruct = reinterpret_cast<LPCREATESTRUCT>(lparam);
            thisRef = static_cast<WindowBorder*>(createStruct->lpCreateParams);
            SetWindowLongPtr(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(thisRef));
        }
        return (thisRef != nullptr) ? thisRef->WndProc(message, wparam, lparam)
                                    : DefWindowProc(window, message, wparam, lparam);
    }

    LRESULT WndProc(UINT message, WPARAM wparam, LPARAM lparam)
    {
        switch (message)
        {
        case WM_TIMER:
            if (wparam == REFRESH_BORDER_TIMER_ID)
            {
                KillTimer(m_window, m_timer_id);
                m_timer_id = SetTimer(m_window, REFRESH_BORDER_TIMER_ID, REFRESH_BORDER_INTERVAL, nullptr);
                UpdateBorderPosition();
                UpdateBorderProperties();
            }
            return 0;

        case WM_NCDESTROY:
            KillTimer(m_window, m_timer_id);
            DefWindowProc(m_window, message, wparam, lparam);
            SetWindowLongPtr(m_window, GWLP_USERDATA, 0);
            return 0;

        case WM_ERASEBKGND:
            return TRUE;

        /* 点到边框时不要"叮"一声（原注释同） */
        case WM_SETCURSOR:
            if (HCURSOR hCursor = LoadCursorW(nullptr, IDC_ARROW))
                SetCursor(hCursor);
            return TRUE;

        default:
            return DefWindowProc(m_window, message, wparam, lparam);
        }
    }

    bool Init(HINSTANCE hinstance)
    {
        if (!m_trackingWindow) return false;

        RECT windowRect{};
        if (!GetFrameRect(m_trackingWindow, &windowRect)) return false;

        WNDCLASSEXW wcex{};
        wcex.cbSize = sizeof(WNDCLASSEX);
        wcex.lpfnWndProc = s_WndProc;
        wcex.hInstance = hinstance;
        wcex.lpszClassName = BORDER_WINDOW_CLASS_NAME;
        wcex.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        RegisterClassExW(&wcex);

        m_window = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
                                   BORDER_WINDOW_CLASS_NAME, L"",
                                   WS_POPUP | WS_DISABLED,
                                   windowRect.left, windowRect.top,
                                   windowRect.right - windowRect.left,
                                   windowRect.bottom - windowRect.top,
                                   nullptr, nullptr, hinstance, this);
        if (!m_window) return false;

        /* 把窗口本身做成全透明：DWM blur-behind 挂一个屏幕外的 1x1 区域，
           等于"没有模糊区域"——窗口内容不再由 DWM 画，只留下 D2D 画的东西。
           （这是原实现的做法，照搬；配合下面的 color-key 一起用。） */
        int const pos = -GetSystemMetrics(SM_CXVIRTUALSCREEN) - 8;
        if (HRGN hrgn = CreateRectRgn(pos, 0, pos + 1, 1))
        {
            DWM_BLURBEHIND bh = { DWM_BB_ENABLE | DWM_BB_BLURREGION, TRUE, hrgn, FALSE };
            DwmEnableBlurBehindWindow(m_window, &bh);
            DeleteObject(hrgn);
        }

        if (!SetLayeredWindowAttributes(m_window, RGB(0, 0, 0), 0, LWA_COLORKEY)) return false;
        if (!SetLayeredWindowAttributes(m_window, 0, 255, LWA_ALPHA)) return false;

        /* 把边框窗压到目标窗**后面**（原注释：防止反复开关边框时边框互相覆盖） */
        SetWindowPos(m_trackingWindow, m_window, windowRect.left, windowRect.top,
                     windowRect.right - windowRect.left, windowRect.bottom - windowRect.top,
                     SWP_NOMOVE | SWP_NOSIZE);

        BOOL val = TRUE;
        DwmSetWindowAttribute(m_window, DWMWA_EXCLUDED_FROM_PEEK, &val, sizeof(val));

        m_frameDrawer = FrameDrawer::Create(m_window);
        if (!m_frameDrawer) return false;

        m_frameDrawer->Show();
        UpdateBorderPosition();
        UpdateBorderProperties();
        m_timer_id = SetTimer(m_window, REFRESH_BORDER_TIMER_ID, REFRESH_BORDER_INTERVAL, nullptr);
        return true;
    }

    UINT_PTR m_timer_id = 0;
    HWND m_window = nullptr;
    HWND m_trackingWindow = nullptr;
    std::unique_ptr<FrameDrawer> m_frameDrawer;
};

/* ================= AlwaysOnTop（模块主体，原样移植） ================= */
/* 钉住数量变了要同步给主线程（托盘菜单据此置灰）。定义在文件末尾 ——
   这里只声明，避免把 volatile 镜像的读写散进模块内部。 */
void PinCountChanged(size_t n);

class AlwaysOnTop
{
public:
    explicit AlwaysOnTop(HINSTANCE hinstance) : m_hinstance(hinstance) { s_instance = this; }

    ~AlwaysOnTop() { CleanUp(); }

    bool Init()
    {
        if (!InitMainWindow()) return false;
        SubscribeToEvents();
        StartTrackingTopmostWindows();
        return true;
    }

    void ProcessCommand(HWND window)
    {
        if (!window || !IsWindow(window)) return;

        /* 本程序自己的两条附加判据（PowerToys 没有"自己钉自己"的场景）：
           不钉本进程的窗口、不钉桌面/任务栏。 */
        DWORD pid = 0;
        GetWindowThreadProcessId(window, &pid);
        if (pid == GetCurrentProcessId()) return;
        if (WinIsShellClass(window)) return;

        Sound::Type soundType = Sound::Type::Off;
        bool stateChanged = false;
        bool topmost = IsTopmost(window);
        if (topmost)
        {
            if (UnpinTopmostWindow(window))
            {
                stateChanged = true;
                auto iter = m_topmostWindows.find(window);
                if (iter != m_topmostWindows.end())
                    m_topmostWindows.erase(iter);
                PinCountChanged(m_topmostWindows.size());
                PinLog("取消置顶 hwnd=%p", (void*)window);
            }
        }
        else
        {
            if (m_topmostWindows.size() >= MAX_PINNED)
            {
                PinLog("置顶失败：已达上限 %zu", MAX_PINNED);
                return;
            }
            if (PinTopmostWindow(window))
            {
                stateChanged = true;
                soundType = Sound::Type::On;
                AssignBorder(window);
                PinCountChanged(m_topmostWindows.size());
                PinLog("置顶 hwnd=%p", (void*)window);
            }
            else
            {
                DWORD err = GetLastError();
                /* UIPI：目标进程权限更高时 SetWindowPos 会被拦下。
                   必须让用户看见，否则就是"按了没反应"。 */
                if (err == ERROR_ACCESS_DENIED && g_mainWnd)
                    PostMessageW(g_mainWnd, WM_PIN_ELEVATED, 0, 0);
                PinLog("置顶失败（权限不足）hwnd=%p", (void*)window);
            }
        }

        if (stateChanged && ENABLE_SOUND)
            Sound::Play(soundType);
    }

    void UnpinAll()
    {
        for (auto& [topWindow, border] : m_topmostWindows)
        {
            if (!UnpinTopmostWindow(topWindow))
                PinLog("取消置顶失败 hwnd=%p", (void*)topWindow);
        }
        m_topmostWindows.clear();
        PinCountChanged(0);
    }

    size_t Count() const { return m_topmostWindows.size(); }
    HWND Window() const { return m_window; }

    void CleanUp()
    {
        for (auto hook : m_staticWinEventHooks)
            if (hook) UnhookWinEvent(hook);
        m_staticWinEventHooks.clear();

        UnpinAll();
        if (m_window)
        {
            DestroyWindow(m_window);
            m_window = nullptr;
        }
        UnregisterClass(TOOL_WINDOW_CLASS_NAME, m_hinstance);
    }

private:
    static LRESULT CALLBACK WndProc_Helper(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
    {
        auto thisRef = reinterpret_cast<AlwaysOnTop*>(GetWindowLongPtr(window, GWLP_USERDATA));
        if ((thisRef == nullptr) && (message == WM_CREATE))
        {
            auto createStruct = reinterpret_cast<LPCREATESTRUCT>(lparam);
            thisRef = static_cast<AlwaysOnTop*>(createStruct->lpCreateParams);
            SetWindowLongPtr(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(thisRef));
        }
        return (thisRef != nullptr) ? thisRef->WndProc(window, message, wparam, lparam)
                                    : DefWindowProc(window, message, wparam, lparam);
    }

    LRESULT WndProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
    {
        switch (message)
        {
        case WM_PIN_TOGGLE:
            ProcessCommand(reinterpret_cast<HWND>(lparam));
            return 0;
        case WM_PIN_UNPIN_ALL:
            {
                bool had = !m_topmostWindows.empty();
                UnpinAll();
                if (ENABLE_SOUND && had) Sound::Play(Sound::Type::Off);
            }
            return 0;
        case WM_QUERYENDSESSION:
            return TRUE;
        case WM_ENDSESSION:
            if (wparam) PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProc(window, message, wparam, lparam);
        }
    }

    bool InitMainWindow()
    {
        WNDCLASSEXW wcex{};
        wcex.cbSize = sizeof(WNDCLASSEX);
        wcex.lpfnWndProc = WndProc_Helper;
        wcex.hInstance = m_hinstance;
        wcex.lpszClassName = TOOL_WINDOW_CLASS_NAME;
        RegisterClassExW(&wcex);

        m_window = CreateWindowExW(WS_EX_TOOLWINDOW, TOOL_WINDOW_CLASS_NAME, L"",
                                   WS_POPUP, 0, 0, 0, 0,
                                   nullptr, nullptr, m_hinstance, this);
        return m_window != nullptr;
    }

    void SubscribeToEvents()
    {
        const DWORD events[] = {
            EVENT_OBJECT_LOCATIONCHANGE,
            EVENT_SYSTEM_MINIMIZESTART,
            EVENT_SYSTEM_MINIMIZEEND,
            EVENT_SYSTEM_MOVESIZEEND,
            EVENT_SYSTEM_FOREGROUND,
            EVENT_OBJECT_DESTROY,
            EVENT_OBJECT_FOCUS,
        };
        for (auto event : events)
        {
            if (HWINEVENTHOOK hook = SetWinEventHook(event, event, nullptr, WinHookProc,
                                                     0, 0,
                                                     WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS))
                m_staticWinEventHooks.push_back(hook);
            else
                PinLog("SetWinEventHook 失败 event=0x%lX", (unsigned long)event);
        }
    }

    /* 启动时把"上次留下的、带本程序标记的窗口"接回来 —— 原实现用于
       PowerToys 重启后恢复边框；本程序正常退出会 UnpinAll，所以只有
       **崩溃/被强杀**之后才会走到这里。 */
    void StartTrackingTopmostWindows()
    {
        struct Ctx { AlwaysOnTop* self; std::vector<HWND> windows; } ctx{ this, {} };

        EnumWindows([](HWND hwnd, LPARAM param) -> BOOL
        {
            auto* c = reinterpret_cast<Ctx*>(param);
            if (!IsWindowVisible(hwnd)) return TRUE;
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            if (pid == GetCurrentProcessId()) return TRUE;
            if (WinIsShellClass(hwnd)) return TRUE;
            if (GetWindowTextLength(hwnd) > 0) c->windows.push_back(hwnd);
            return TRUE;
        }, reinterpret_cast<LPARAM>(&ctx));

        for (HWND window : ctx.windows)
            if (IsPinned(window))
                AssignBorder(window);
    }

    bool AssignBorder(HWND window)
    {
        if (m_virtualDesktopUtils.IsWindowOnCurrentDesktop(window) && ENABLE_FRAME)
        {
            if (auto border = WindowBorder::Create(window, m_hinstance))
                m_topmostWindows[window] = std::move(border);
            else
                m_topmostWindows[window] = nullptr;
        }
        else
        {
            m_topmostWindows[window] = nullptr;
        }
        return true;
    }

    bool IsTopmost(HWND window) const
    {
        LONG_PTR exStyle = GetWindowLongPtrW(window, GWL_EXSTYLE);
        return (exStyle & WS_EX_TOPMOST) == WS_EX_TOPMOST;
    }

    bool IsPinned(HWND window) const
    {
        return GetPropW(window, WINDOW_IS_PINNED_PROP) != nullptr;
    }

    bool PinTopmostWindow(HWND window) const
    {
        if (!SetPropW(window, WINDOW_IS_PINNED_PROP, reinterpret_cast<HANDLE>(1)))
            PinLog("SetProp 失败 hwnd=%p", (void*)window);

        if (!SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE))
        {
            /* 没钉上就别留标记：否则下次启动的"接回"会认领一个其实没钉住的窗口 */
            RemovePropW(window, WINDOW_IS_PINNED_PROP);
            return false;
        }
        return true;
    }

    bool UnpinTopmostWindow(HWND window) const
    {
        RemovePropW(window, WINDOW_IS_PINNED_PROP);
        return SetWindowPos(window, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE) != FALSE;
    }

    void RefreshBorders()
    {
        for (auto& [window, border] : m_topmostWindows)
        {
            if (m_virtualDesktopUtils.IsWindowOnCurrentDesktop(window))
            {
                if (!border) AssignBorder(window);
            }
            else
            {
                if (border) m_topmostWindows[window] = nullptr;
            }
        }
    }

    void HandleWinHookEvent(DWORD event, HWND hwnd, LONG idObject, LONG idChild)
    {
        /* 原实现对这两个字段不做筛选（它只关心"哪个窗口"，事件本身已经按 hwnd
           派发），这里保持同样的语义，只是把"故意不用"写明。 */
        (void)idObject;
        (void)idChild;

        switch (event)
        {
        case EVENT_SYSTEM_MINIMIZESTART:
            if (auto iter = m_topmostWindows.find(hwnd); iter != m_topmostWindows.end())
                m_topmostWindows[hwnd] = nullptr;
            return;

        case EVENT_SYSTEM_MINIMIZEEND:
            if (auto iter = m_topmostWindows.find(hwnd); iter != m_topmostWindows.end())
            {
                /* 恢复时重新置顶：有些程序在最小化期间会把这个位弄丢 */
                PinTopmostWindow(hwnd);
                AssignBorder(hwnd);
            }
            return;

        case EVENT_SYSTEM_MOVESIZEEND:
        case EVENT_OBJECT_LOCATIONCHANGE:
        {
            auto iter = m_topmostWindows.find(hwnd);
            if (iter != m_topmostWindows.end() && iter->second)
                iter->second->UpdateBorderPosition();
            return;
        }

        case EVENT_OBJECT_DESTROY:
        {
            auto iter = m_topmostWindows.find(hwnd);
            if (iter != m_topmostWindows.end())
                m_topmostWindows.erase(iter);
            return;
        }

        case EVENT_SYSTEM_FOREGROUND:
            RefreshBorders();
            return;

        case EVENT_OBJECT_FOCUS:
            for (auto& [window, border] : m_topmostWindows)
            {
                /* 有些程序会把 topmost 位弄丢（原实现 issue #19168），补回来 */
                if (!IsTopmost(window))
                    PinTopmostWindow(window);
            }
            return;

        default:
            return;
        }
    }

    static void CALLBACK WinHookProc(HWINEVENTHOOK, DWORD event, HWND hwnd,
                                     LONG idObject, LONG idChild, DWORD, DWORD)
    {
        if (s_instance && hwnd)
            s_instance->HandleWinHookEvent(event, hwnd, idObject, idChild);
    }

    HINSTANCE m_hinstance = nullptr;
    HWND m_window = nullptr;
    std::map<HWND, std::unique_ptr<WindowBorder>> m_topmostWindows;
    std::vector<HWINEVENTHOOK> m_staticWinEventHooks;
    VirtualDesktopUtils m_virtualDesktopUtils;
    static inline AlwaysOnTop* s_instance = nullptr;
};

/* ================= 线程与对外接口 ================= */
namespace
{
    HANDLE s_thread = nullptr;
    HANDLE s_ready = nullptr;
    DWORD  s_tid = 0;
    HWND   s_tool = nullptr;              /* 主线程只读它来投递，之后不再变 */
    AlwaysOnTop* s_module = nullptr;      /* 只在置顶线程里用 */
    volatile LONG s_pinCount = 0;         /* 给主线程读的镜像 */

    DWORD WINAPI PinThread(LPVOID)
    {
        /* 虚拟桌面判定要 COM（IVirtualDesktopManager） */
        bool comInited = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));

        s_module = new AlwaysOnTop(GetModuleHandleW(nullptr));
        if (s_module->Init())
            s_tool = s_module->Window();

        if (s_ready) SetEvent(s_ready);

        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0)
        {
            if (msg.hwnd == nullptr && msg.message == WM_PIN_QUIT) break;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        delete s_module;
        s_module = nullptr;
        s_tool = nullptr;
        if (comInited) CoUninitialize();
        return 0;
    }
}

/* ---------- 纯函数（单测覆盖；签名见 cap_window.h） ---------- */
BOOL PinFrameRect(const RECT* target, int thickness, RECT* out)
{
    if (!target || !out) return FALSE;
    if (thickness < 0) thickness = 0;
    out->left = target->left - thickness;
    out->top = target->top - thickness;
    out->right = target->right + thickness;
    out->bottom = target->bottom + thickness;
    return TRUE;
}

int PinCornerRadiusFor(int pref)
{
    switch (pref)
    {
    case 2: return 8;   /* DWMWCP_ROUND */
    case 3: return 4;   /* DWMWCP_ROUNDSMALL */
    case 0: return 8;   /* DWMWCP_DEFAULT */
    default: return 0;
    }
}

/* ---------- 对外接口 ---------- */
void PinInit(void)
{
    if (s_thread) return;
    s_ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!s_ready) return;
    s_thread = CreateThread(nullptr, 0, PinThread, nullptr, 0, &s_tid);
    if (!s_thread)
    {
        CloseHandle(s_ready);
        s_ready = nullptr;
        return;
    }
    /* 等它把窗口建好（正常几毫秒）。等不到也不致命：s_tool 为 NULL 时
       所有投递都直接丢掉，功能静默不可用，但不会崩。 */
    WaitForSingleObject(s_ready, 3000);
}

void PinShutdown(void)
{
    if (!s_thread) return;
    if (s_tid) PostThreadMessageW(s_tid, WM_PIN_QUIT, 0, 0);
    if (WaitForSingleObject(s_thread, 2000) == WAIT_TIMEOUT)
        return;   /* 线程还卡在某个窗口调用里：进程马上退出，不再回收 */

    CloseHandle(s_thread);
    s_thread = nullptr;
    s_tid = 0;
    s_tool = nullptr;
    if (s_ready) { CloseHandle(s_ready); s_ready = nullptr; }
}

void PinToggle(HWND target)
{
    if (!s_tool) return;
    PostMessageW(s_tool, WM_PIN_TOGGLE, 0, reinterpret_cast<LPARAM>(target));
}

void PinUnpinAll(void)
{
    if (!s_tool) return;
    PostMessageW(s_tool, WM_PIN_UNPIN_ALL, 0, 0);
}

int PinCount(void)
{
    return static_cast<int>(InterlockedCompareExchange(&s_pinCount, 0, 0));
}

void PinCountChanged(size_t n)
{
    InterlockedExchange(&s_pinCount, static_cast<LONG>(n));
}
