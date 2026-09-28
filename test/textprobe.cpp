/* 文字渲染对比探针（诊断用，不进源码清单）：
   同一组键帽文案，用四种路径画到同一张不透明白底 32bpp DIB 上，
   再 2x 最近邻放大存 PNG，肉眼对比笔画粗细与锐度。
     A  help.c 现状：GDI+ ClearType + 居中对齐（origin 落在亚像素相位）
     B  GDI+ ClearType + 整数 origin（只换定位，仍用 GDI+ 栅格化器）
     C  clip.c 方案：GDI DrawTextW + HFONT(DEFAULT_QUALITY)，整数矩形
     D  GDI+ 灰阶 AntiAliasGridFit + 居中（旧版路线）
   每行画 4 个不同宽度的"键帽"（复现帮助图里各键标签相位不同的事实）。 */
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include <stdio.h>
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

using namespace Gdiplus;

static const WCHAR* g_face = L"Microsoft YaHei UI";

static bool SavePng(Bitmap* bmp, const WCHAR* path) {
    static BYTE buf[65536];
    UINT n = 0, sz = 0;
    GetImageEncodersSize(&n, &sz);
    if (sz > sizeof(buf)) return false;
    ImageCodecInfo* ici = (ImageCodecInfo*)buf;
    GetImageEncoders(n, sz, ici);
    CLSID png = { 0 };
    for (UINT i = 0; i < n; i++)
        if (wcscmp(ici[i].MimeType, L"image/png") == 0) { png = ici[i].Clsid; break; }
    return png.Data1 != 0 && bmp->Save(path, &png, NULL) == Ok;
}

/* help.c TextWidth 的复刻：MeasureString 每侧补回 1/6 em */
static float TextWidth(Graphics* g, const WCHAR* s, Font* f, float em) {
    RectF box(0, 0, 0, 0);
    g->MeasureString(s, -1, f, PointF(0, 0), &box);
    return box.Width - em / 3.0f;
}

/* help.c DrawLine 的复刻（居中时左右各放宽 10%） */
static void DrawLineHelp(Graphics* g, const WCHAR* s, Font* f, COLORREF c,
                         float x, float y, float w, float h) {
    SolidBrush br(Color(0xFF, GetRValue(c), GetGValue(c), GetBValue(c)));
    StringFormat fmt;
    fmt.SetAlignment(StringAlignmentCenter);
    fmt.SetLineAlignment(StringAlignmentCenter);
    fmt.SetFormatFlags(StringFormatFlagsNoWrap);
    RectF rc(x - w * 0.10f, y, w * 1.20f, h);
    g->DrawString(s, -1, f, rc, &fmt, &br);
}

static void CapPath(GraphicsPath* p, float x, float y, float w, float h, float r) {
    float d = r * 2;
    p->AddArc(x, y, d, d, 180, 90);
    p->AddArc(x + w - d, y, d, d, 270, 90);
    p->AddArc(x + w - d, y + h - d, d, d, 0, 90);
    p->AddArc(x, y + h - d, d, d, 90, 90);
    p->CloseFigure();
}

int main() {
    GdiplusStartupInput gsi;
    ULONG_PTR tok;
    if (GdiplusStartup(&tok, &gsi, NULL) != Ok) return 1;

    const int W = 780, H = 500;
    const int rowH = 112, boxH = 44, capR = 5.0f;
    /* 四个"键帽"宽度略有差异：复现帮助图里每个键标签的定位相位都不同 */
    const float capW[4] = { 148, 162, 176, 190 };
    const WCHAR* txt[4] = { L"帮助", L"静音", L"程序目录", L"音量-" };
    const WCHAR* rowName[4] = {
        L"A  help现状: GDI+ ClearType 居中(亚像素相位)",
        L"B  GDI+ ClearType 整数origin",
        L"C  clip方案: GDI DrawTextW 整数矩形",
        L"D  GDI+ 灰阶AntiAlias 居中",
    };

    BITMAPINFO bi; memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = W;  bi.bmiHeader.biHeight = -H;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void* bits = NULL;
    HDC sdc = GetDC(NULL);
    HDC mem = CreateCompatibleDC(sdc);
    HBITMAP dib = CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    ReleaseDC(NULL, sdc);
    HGDIOBJ oldBmp = SelectObject(mem, dib);
    RECT full = { 0, 0, W, H };
    FillRect(mem, &full, (HBRUSH)GetStockObject(WHITE_BRUSH));

    const COLORREF cFill  = RGB(220, 233, 247);   /* C_ACT_FILL */
    const COLORREF cLine  = RGB(157, 192, 232);   /* C_ACT_LINE */
    const COLORREF cText  = RGB(31, 78, 121);     /* C_ACT_FUNC */
    const int fsFunc = 24;                        /* help.c: int(88*0.28) */

    for (int row = 0; row < 4; row++) {
        float y0 = (float)(10 + row * rowH);
        {
            Graphics g(mem);
            g.SetSmoothingMode(SmoothingModeAntiAlias);
            /* 行标题用 GDI+ 画（它只是标签，不参与对比） */
            Font fLab(&FontFamily(g_face), 12.0f, FontStyleRegular, UnitPixel);
            SolidBrush brLab(Color(90, 90, 90));
            g.DrawString(rowName[row], -1, &fLab, PointF(14.0f, y0), &brLab);

            g.SetTextRenderingHint(
                (row == 3) ? TextRenderingHintAntiAliasGridFit
                           : TextRenderingHintClearTypeGridFit);
            Font fFunc(&FontFamily(g_face), (float)fsFunc, FontStyleRegular, UnitPixel);
            float by = y0 + 24;
            for (int i = 0; i < 4; i++) {
                float bx = 14.0f + (i == 0 ? 0.0f :
                           (capW[0]+8) * 0 + 0.0f);
                bx = 14.0f;
                for (int k = 0; k < i; k++) bx += capW[k] + 8.0f;
                GraphicsPath p;
                CapPath(&p, bx, by, capW[i], (float)boxH, capR);
                SolidBrush fill(Color(0xFF, GetRValue(cFill), GetGValue(cFill), GetBValue(cFill)));
                Pen stroke(Color(0xFF, GetRValue(cLine), GetGValue(cLine), GetBValue(cLine)), 1.0f);
                g.FillPath(&fill, &p);
                g.DrawPath(&stroke, &p);
                g.Flush(FlushIntentionSync);

                if (row == 2) {
                    /* C：GDI DrawTextW —— 整数矩形（clip.c DrawTipText 同款） */
                    LOGFONTW lf; memset(&lf, 0, sizeof(lf));
                    lf.lfHeight = -fsFunc;
                    lf.lfWeight = FW_NORMAL;
                    lf.lfQuality = DEFAULT_QUALITY;
                    lf.lfCharSet = DEFAULT_CHARSET;
                    wcscpy_s(lf.lfFaceName, LF_FACESIZE, g_face);
                    HFONT hf = CreateFontIndirectW(&lf);
                    RECT rc;
                    rc.left   = (int)(bx + 0.5f);
                    rc.top    = (int)(by + 0.5f);
                    rc.right  = (int)(bx + capW[i] + 0.5f);
                    rc.bottom = (int)(by + boxH + 0.5f);
                    HGDIOBJ of = SelectObject(mem, hf);
                    SetBkMode(mem, TRANSPARENT);
                    SetTextColor(mem, cText);
                    DrawTextW(mem, txt[i], -1, &rc,
                              DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX);
                    SelectObject(mem, of);
                    DeleteObject(hf);
                } else {
                    WCHAR one[64]; wcscpy_s(one, 64, txt[i]);
                    if (row == 1) {
                        /* B：GDI+ 但 origin 钳到整数 */
                        float tw = TextWidth(&g, one, &fFunc, (float)fsFunc);
                        float ox = bx + (capW[i] - tw) / 2.0f;
                        ox = (float)(int)(ox + 0.5f);
                        float oy = by + (boxH - (float)fsFunc) / 2.0f;
                        SolidBrush br(Color(0xFF, GetRValue(cText), GetGValue(cText), GetBValue(cText)));
                        StringFormat f2;
                        f2.SetFormatFlags(StringFormatFlagsNoWrap);
                        f2.SetAlignment(StringAlignmentNear);
                        g.DrawString(one, -1, &fFunc, PointF(ox, oy), &f2, &br);
                    } else {
                        /* A / D：现状路径，居中 -> 亚像素相位随键宽变化 */
                        DrawLineHelp(&g, one, &fFunc, cText, bx, by, capW[i], (float)boxH);
                    }
                    g.Flush(FlushIntentionSync);
                }
            }
        }
    }

    SelectObject(mem, oldBmp);

    /* 1x 存一份；再 2x 最近邻放大存一份（看亚像素细节用） */
    {
        Bitmap one(W, H, W * 4, PixelFormat32bppPARGB, (BYTE*)bits);
        SavePng(&one, L"obj\\textprobe_1x.png");
        Bitmap two(W * 2, H * 2, W * 8, PixelFormat32bppPARGB, NULL);
        Graphics g2(&two);
        g2.SetInterpolationMode(InterpolationModeNearestNeighbor);
        g2.SetPixelOffsetMode(PixelOffsetModeHalf);
        g2.DrawImage(&one, RectF(0, 0, (float)W * 2, (float)H * 2),
                     0, 0, (float)W, (float)H, UnitPixel);
        SavePng(&two, L"obj\\textprobe_2x.png");
    }

    DeleteObject(dib);
    DeleteDC(mem);
    GdiplusShutdown(tok);
    printf("saved obj/textprobe_1x.png obj/textprobe_2x.png\n");
    return 0;
}
