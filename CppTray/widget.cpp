#include "widget.h"

#include <windowsx.h>
#include <cmath>
#include <cwchar>
#include <string>
#include <vector>

#pragma comment(lib, "gdiplus.lib")

using Gdiplus::Bitmap;
using Gdiplus::Color;
using Gdiplus::Font;
using Gdiplus::FontFamily;
using Gdiplus::Graphics;
using Gdiplus::GraphicsPath;
using Gdiplus::Pen;
using Gdiplus::PointF;
using Gdiplus::RectF;
using Gdiplus::SmoothingMode;
using Gdiplus::SolidBrush;
using Gdiplus::StringFormat;
using Gdiplus::TextRenderingHint;
using Gdiplus::UnitPixel;

namespace {

constexpr wchar_t kWidgetClass[] = L"MechrevoTrayWidgetClass";
constexpr wchar_t kTipClass[] = L"MechrevoTrayTipClass";
constexpr wchar_t kThemeKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";

constexpr int kGapPx = 2;          // 与托盘角落间距
constexpr int kPairGap = 6;        // 功耗/风扇两块的间距
constexpr int kTextPad = 1;        // 文本四周留白（块宽 = 文本宽 + 2*pad）
constexpr int kTipW = 216, kTipH = 118;

Color MakeColor(BYTE a, BYTE r, BYTE g, BYTE b) { return Color(a, r, g, b); }

bool IsLightTheme() {
    HKEY key = nullptr;
    bool light = true;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kThemeKey, 0, KEY_READ, &key) == ERROR_SUCCESS) {
        DWORD v = 1;
        DWORD size = sizeof(v);
        DWORD type = 0;
        if (RegQueryValueExW(key, L"SystemUsesLightTheme", nullptr, &type,
                             (LPBYTE)&v, &size) == ERROR_SUCCESS)
            light = (v != 0);
        RegCloseKey(key);
    }
    return light;
}

// 创建 32bpp 顶向下 DIB 位图并选入 DC（供 GDI+ alpha 绘制 + UpdateLayeredWindow）
HBITMAP CreateDib(HDC dc, int w, int h, void** bits) {
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;   // 顶向下
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    return CreateDIBSection(dc, &bi, DIB_RGB_COLORS, bits, nullptr, 0);
}

// 用 MeasureCharacterRanges 实测"墨迹盒"（X=墨迹相对绘制点的起始偏移, W=实际宽度）。
// DrawString 渲染时墨迹占 [绘制点+X, 绘制点+X+W]：据此定块宽并手动居中绘制，
// 既没有 MeasureString 的虚胖，也不会像 RectF 居中那样把超宽部分裁掉。
struct InkBox { float x = 0.f, w = 0.f, y = 0.f, h = 0.f; };

InkBox MeasureInk(HDC mdc, const Font& font, const wchar_t* text) {
    InkBox box;
    Gdiplus::Graphics g(mdc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
    StringFormat sf;
    sf.SetFormatFlags(Gdiplus::StringFormatFlagsNoFitBlackBox);
    Gdiplus::CharacterRange range{ 0, (INT)wcslen(text) };
    sf.SetMeasurableCharacterRanges(1, &range);
    RectF layout(0, 0, 1000.f, 100.f);
    Gdiplus::Region regions[1];
    g.MeasureCharacterRanges(text, -1, &font, layout, &sf, 1, regions);
    RectF b;
    regions[0].GetBounds(&b, &g);
    box.x = b.X;
    box.w = b.Width;
    box.y = b.Y;
    box.h = b.Height;
    return box;
}

}  // namespace

// ==================== Widget ====================

bool Widget::Create(HINSTANCE hInst) {
    _hInst = hInst;

    Gdiplus::GdiplusStartupInput gsi;
    if (Gdiplus::GdiplusStartup((ULONG_PTR*)&_gdiToken, &gsi, nullptr) != Gdiplus::Ok)
        return false;

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = kWidgetClass;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);

    _hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW, kWidgetClass, L"",
                            WS_POPUP, 0, 0, 120, 46, nullptr, nullptr, hInst, this);
    if (!_hwnd)
        return false;

    RegisterTipClass(hInst);
    _taskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");
    ApplyTheme(IsLightTheme());
    return true;
}

void Widget::RegisterTipClass(HINSTANCE hInst) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = TipWndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = kTipClass;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
}

bool Widget::Embed() {
    if (!_hwnd)
        return false;

    HWND taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
    HWND tray = taskbar ? FindWindowExW(taskbar, nullptr, L"TrayNotifyWnd", nullptr) : nullptr;
    if (!taskbar || !tray)
        return false;   // 开始菜单展开 / explorer 重启中：稍后重试

    LONG_PTR style = GetWindowLongPtrW(_hwnd, GWL_STYLE);
    SetWindowLongPtrW(_hwnd, GWL_STYLE, style | WS_CHILD | WS_VISIBLE);
    SetParent(_hwnd, taskbar);

    if (!_shown) {
        ShowWindow(_hwnd, SW_SHOW);
        _shown = true;
    }

    _taskbar = taskbar;
    _embedded = true;
    Position();
    return true;
}

void Widget::Dispose() {
    if (_tipHwnd) {
        DestroyWindow(_tipHwnd);
        _tipHwnd = nullptr;
    }
    if (_hwnd) {
        if (_embedded)
            SetParent(_hwnd, nullptr);
        DestroyWindow(_hwnd);
        _hwnd = nullptr;
    }
    if (_gdiToken) {
        Gdiplus::GdiplusShutdown((ULONG_PTR)_gdiToken);
        _gdiToken = 0;
    }
}

void Widget::Update(const Metric& power, const Metric& fan) {
    if (!_hwnd)
        return;
    if (!_embedded && !Embed())
        return;

    _power = power;
    _fan = fan;
    _hasData = true;

    // 60 拍滚动历史（供悬浮窗趋势图）；无效拍沿用上一值保持曲线连续
    if (power.valid)
        _powerHist.push_back(power.current);
    else if (!_powerHist.empty())
        _powerHist.push_back(_powerHist.back());
    if (_powerHist.size() > 60)
        _powerHist.erase(_powerHist.begin());
    if (fan.valid)
        _fanHist.push_back(fan.current);
    else if (!_fanHist.empty())
        _fanHist.push_back(_fanHist.back());
    if (_fanHist.size() > 60)
        _fanHist.erase(_fanHist.begin());

    if (_tipHwnd && IsWindowVisible(_tipHwnd))
        InvalidateRect(_tipHwnd, nullptr, FALSE);   // 悬停时趋势图随采样实时刷新

    Render();
}

void Widget::ApplyTheme(bool light) {
    _sevGreen = MakeColor(255, light ? 0x16 : 0x4A, light ? 0xA3 : 0xDE, light ? 0x4A : 0x80);
    _sevOrange = MakeColor(255, light ? 0xD9 : 0xFB, light ? 0x77 : 0x92, light ? 0x06 : 0x3C);
    _sevRed = MakeColor(255, light ? 0xDC : 0xF8, light ? 0x26 : 0x71, light ? 0x26 : 0x71);
    _labelColor = MakeColor(255, light ? 0x5E : 0xA8, light ? 0x66 : 0xB3, light ? 0x72 : 0xBD);
    _hoverBack = MakeColor(0x66, 0xFF, 0xFF, 0xFF);   // 白色 40% 悬停高亮
    if (_hasData)
        Render();
}

Color Widget::ValueColor(float v, double elevated, double critical) const {
    if (v <= elevated)
        return _sevGreen;
    return v <= critical ? _sevOrange : _sevRed;
}

void Widget::Render() {
    HDC screenDC = GetDC(nullptr);

    FontFamily ffVal(L"Segoe UI");
    Font valFont(&ffVal, 12.f, Gdiplus::FontStyleBold, UnitPixel);
    FontFamily ffLbl(L"Microsoft YaHei UI");
    Font lblFont(&ffLbl, 9.f, Gdiplus::FontStyleRegular, UnitPixel);

    wchar_t pbuf[32] = {};
    if (_power.valid)
        swprintf_s(pbuf, L"%.1f", (double)_power.current);   // 托盘窗口不带单位，只显示数值
    else
        wcscpy_s(pbuf, L"--");

    wchar_t fbuf[32] = {};
    if (_fan.valid)
        swprintf_s(fbuf, L"%.0f", (double)_fan.current);
    else
        wcscpy_s(fbuf, L"--");

    // 块宽贴合实际墨迹（MeasureCharacterRanges）：居中且紧凑；位数进位只差几 px，窗口微调可接受
    InkBox powerVal, powerLbl, fanVal, fanLbl;
    {
        HDC mdc = CreateCompatibleDC(screenDC);
        powerVal = MeasureInk(mdc, valFont, pbuf);
        powerLbl = MeasureInk(mdc, lblFont, L"功耗");
        fanVal = MeasureInk(mdc, valFont, fbuf);
        fanLbl = MeasureInk(mdc, lblFont, L"风扇");
        DeleteDC(mdc);
        _powerBlockW = (int)std::ceil(std::max(powerVal.w, powerLbl.w)) + kTextPad * 2;
        _fanBlockW = (int)std::ceil(std::max(fanVal.w, fanLbl.w)) + kTextPad * 2;
        _contentW = _powerBlockW + kPairGap + _fanBlockW;
    }

    int width = _contentW;
    int height = 46;
    if (_taskbar) {
        RECT tb{};
        if (GetWindowRect(_taskbar, &tb) && tb.bottom > tb.top)
            height = tb.bottom - tb.top;
    }

    HDC memDC = CreateCompatibleDC(screenDC);
    void* bits = nullptr;
    HBITMAP dib = CreateDib(screenDC, width, height, &bits);
    HGDIOBJ oldBmp = SelectObject(memDC, dib);

    {
        Graphics g(memDC);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        // 透明表面不能 ClearType；用灰度抗锯齿+网格拟合（AntiAliasGridFit），小字号下比纯 AntiAlias 更锐利
        g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
        g.Clear(Color(0, 0, 0, 0));

        auto DrawPair = [&](int x0, int bw, const InkBox& valInk, const InkBox& lblInk,
                            const wchar_t* valueText, const wchar_t* label,
                            const Metric& m, double elevated, double critical, bool hover) {
            if (hover) {
                SolidBrush hb(_hoverBack);
                g.FillRectangle(&hb, (float)x0, 1.f, (float)bw, (float)(height - 2));
            }

            // 手动居中：墨迹盒宽 = 块宽-2*pad 时左右各留 1px；PointF 定位不裁剪
            Color vc = m.valid ? ValueColor(m.current, elevated, critical) : _labelColor;
            SolidBrush valBrush(vc);
            float vx = x0 + (bw - valInk.w) / 2.f - valInk.x;
            g.DrawString(valueText, -1, &valFont, PointF(vx, (float)(height / 2 - 12)), &valBrush);

            SolidBrush lblBrush(_labelColor);
            float lx = x0 + (bw - lblInk.w) / 2.f - lblInk.x;
            g.DrawString(label, -1, &lblFont, PointF(lx, (float)(height / 2 + 4)), &lblBrush);
        };

        DrawPair(0, _powerBlockW, powerVal, powerLbl, pbuf, L"功耗", _power, _thr.powerElevated, _thr.powerCritical,
                 _hovering && _hoverPower);
        DrawPair(_powerBlockW + kPairGap, _fanBlockW, fanVal, fanLbl, fbuf, L"风扇", _fan, _thr.fanElevated, _thr.fanCritical,
                 _hovering && !_hoverPower);
    }

    BLENDFUNCTION blend{};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;

    POINT ptSrc{};
    // UpdateLayeredWindow 的 ptDst 对子窗口是"相对父窗口坐标"，传入任何非零值都会移动窗口
    // （实测：传 taskbar 屏幕坐标会叠加父窗口位置导致 2 倍偏移）。位置完全由 SetWindowPos 决定，
    // 这里传 nullptr 表示"位置不变"。
    SIZE sz{ width, height };
    UpdateLayeredWindow(_hwnd, screenDC, nullptr, &sz, memDC, &ptSrc, 0, &blend, ULW_ALPHA);

    SelectObject(memDC, oldBmp);
    DeleteObject(dib);
    DeleteDC(memDC);
    ReleaseDC(nullptr, screenDC);

    Position();
}

void Widget::Position() {
    if (!_embedded || !_taskbar || !IsWindow(_taskbar))
        return;

    // 悬停卡片打开时不动窗口，等收起后的下个校准周期再贴
    if (_tipHwnd && IsWindowVisible(_tipHwnd))
        return;

    HWND tray = FindWindowExW(_taskbar, nullptr, L"TrayNotifyWnd", nullptr);
    RECT tb{}, trayRect{};
    if (!tray || !GetWindowRect(_taskbar, &tb) || !GetWindowRect(tray, &trayRect))
        return;

    RECT trayRect2{};
    GetWindowRect(tray, &trayRect2);
    if (std::abs((long)(trayRect2.left - trayRect.left)) > 2)
        return;

    double scale = GetDpiForWindow(_hwnd) / 96.0;

    int width = (int)std::ceil(_contentW * scale);
    int height = tb.bottom - tb.top;
    int gapPx = (int)std::round(kGapPx * scale);
    int x = trayRect.left - tb.left - gapPx - width;

    RectW cur{ trayRect.left, tb.top, trayRect.right, tb.bottom };
    bool rescan = !_hasTrayRect ||
                  !(_lastTrayRect.left == cur.left && _lastTrayRect.top == cur.top &&
                    _lastTrayRect.right == cur.right && _lastTrayRect.bottom == cur.bottom) ||
                  ++_foreignScanCounter >= 6;
    if (rescan) {
        ScanForeignWidgets(trayRect.left, tb.top, tb.bottom);
        _foreignScanCounter = 0;
        _lastTrayRect = cur;
        _hasTrayRect = true;
    }
    if (!_foreign.empty()) {
        int foreignLeft = _foreign[0].left;
        for (const auto& r : _foreign)
            if (r.left < foreignLeft)
                foreignLeft = r.left;
        int shifted = foreignLeft - tb.left - gapPx - width;
        if (shifted < x)
            x = shifted;
    }

    x = std::max(0, x);

    // 合理性边界：右缘绝不进入托盘角落前 150px（瞬态脏值则放弃本拍）
    if (x + width > (tb.right - tb.left) - 150)
        return;

    if (x == _lastX && width == _lastW && height == _lastH)
        return;

    SetWindowPos(_hwnd, nullptr, x, 0, width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    _lastX = x;
    _lastW = width;
    _lastH = height;
}

BOOL CALLBACK Widget::EnumForeign(HWND hwnd, LPARAM lp) {
    Widget* self = (Widget*)lp;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId())
        return TRUE;
    DWORD taskbarPid = 0;
    if (self->_taskbar)
        GetWindowThreadProcessId(self->_taskbar, &taskbarPid);
    if (pid == taskbarPid)
        return TRUE;   // 跳过系统（explorer）
    if (!IsWindowVisible(hwnd))
        return TRUE;
    RECT r{};
    if (!GetWindowRect(hwnd, &r))
        return TRUE;
    if (r.right - r.left < 40 || r.bottom - r.top < 24)
        return TRUE;   // 过滤细小辅助窗口
    if (r.right > self->_scanBoundary + 4 || r.right < self->_scanBoundary - 800)
        return TRUE;
    if (r.bottom <= self->_scanTop || r.top >= self->_scanBottom)
        return TRUE;
    self->_foreign.push_back({ r.left, r.top, r.right, r.bottom });
    return TRUE;
}

void Widget::ScanForeignWidgets(int boundaryRight, int tbTop, int tbBottom) {
    _foreign.clear();
    _scanBoundary = boundaryRight;
    _scanTop = tbTop;
    _scanBottom = tbBottom;
    if (_taskbar)
        EnumChildWindows(_taskbar, EnumForeign, (LPARAM)this);
    EnumWindows(EnumForeign, (LPARAM)this);
}

// —— 悬停 ——

bool Widget::HitPower(POINT pt) const {
    RECT r{};
    GetWindowRect(_hwnd, &r);
    int pw = (int)(_powerBlockW * (GetDpiForWindow(_hwnd) / 96.0));
    return pt.x >= r.left && pt.x < r.left + pw;
}

bool Widget::HitFan(POINT pt) const {
    RECT r{};
    GetWindowRect(_hwnd, &r);
    int pw = (int)(_powerBlockW * (GetDpiForWindow(_hwnd) / 96.0));
    int gw = (int)(kPairGap * (GetDpiForWindow(_hwnd) / 96.0));
    int left = r.left + pw + gw;
    int right = r.left + (int)(_contentW * (GetDpiForWindow(_hwnd) / 96.0));
    return pt.x >= left && pt.x < right;
}

void Widget::ShowTip(POINT pt) {
    bool power = HitPower(pt);
    if (power == _hoverPower && _hovering && _tipHwnd && IsWindowVisible(_tipHwnd)) {
        // 同一块悬停，仅更新内容
        InvalidateRect(_tipHwnd, nullptr, FALSE);
        return;
    }

    if (!_tipHwnd) {
        _tipHwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
                                   kTipClass, L"", WS_POPUP, 0, 0, kTipW, kTipH,
                                   nullptr, nullptr, _hInst, this);
    }
    _hoverPower = power;
    _hovering = true;

    RECT r{};
    GetWindowRect(_hwnd, &r);
    RECT tb{};
    if (!GetWindowRect(_taskbar, &tb))
        return;

    double scale = GetDpiForWindow(_hwnd) / 96.0;
    int blockW = (int)((power ? _powerBlockW : _fanBlockW) * scale);
    int blockX = power ? r.left : r.left + (int)((_powerBlockW + kPairGap) * scale);
    int cardX = blockX + blockW / 2 - kTipW / 2;
    int cardY = tb.top - kTipH - 12;
    if (cardY < 0)
        cardY = 0;
    SetWindowPos(_tipHwnd, HWND_TOPMOST, cardX, cardY, kTipW, kTipH,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(_tipHwnd, nullptr, FALSE);
    Render();
}

void Widget::HideTip() {
    _hovering = false;
    if (_tipHwnd && IsWindowVisible(_tipHwnd))
        ShowWindow(_tipHwnd, SW_HIDE);
    Render();
}

// —— 消息 ——

LRESULT CALLBACK Widget::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    Widget* self = (Widget*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (msg == WM_NCCREATE) {
        self = (Widget*)((CREATESTRUCTW*)lp)->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
    }
    if (self)
        return self->OnMessage(hwnd, msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT Widget::OnMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_MOUSEMOVE: {
            TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
            POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            ClientToScreen(hwnd, &pt);
            if (HitPower(pt) || HitFan(pt))
                ShowTip(pt);
            else
                HideTip();
            return 0;
        }
        case WM_MOUSELEAVE:
            HideTip();
            return 0;
        case WM_MOVE:
            // 任务栏布局变化：立即收起卡片避免错位
            HideTip();
            return 0;
        case WM_DESTROY:
            _embedded = false;
            _taskbar = nullptr;
            return 0;
        default:
            if (msg == _taskbarCreatedMsg) {
                // explorer 重启广播：任务栏重建后重新嵌入
                _embedded = false;
                _taskbar = nullptr;
                Embed();
                return 0;
            }
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ==================== 悬停卡片 ====================

LRESULT CALLBACK Widget::TipWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    Widget* self = (Widget*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (msg == WM_NCCREATE) {
        self = (Widget*)((CREATESTRUCTW*)lp)->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
    }

    switch (msg) {
        case WM_PAINT: {
            if (!self)
                break;
            RECT rc{};
            GetClientRect(hwnd, &rc);
            int w = rc.right, h = rc.bottom;
            HDC screenDC = GetDC(nullptr);
            HDC memDC = CreateCompatibleDC(screenDC);
            void* bits = nullptr;
            HBITMAP dib = CreateDib(screenDC, w, h, &bits);
            HGDIOBJ oldBmp = SelectObject(memDC, dib);

            {
                Graphics g(memDC);
                g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);   // 透明表面：灰度抗锯齿+网格拟合
                g.Clear(Color(0, 0, 0, 0));

                // 深色圆角卡片：竖向渐变（上浅下深）+ 柔和细边框
                GraphicsPath path;
                int rad = 12;
                path.AddArc(0, 0, rad * 2, rad * 2, 180, 90);
                path.AddArc(w - rad * 2, 0, rad * 2, rad * 2, 270, 90);
                path.AddArc(w - rad * 2, h - rad * 2, rad * 2, rad * 2, 0, 90);
                path.AddArc(0, h - rad * 2, rad * 2, rad * 2, 90, 90);
                path.CloseFigure();
                Gdiplus::LinearGradientBrush bgBrush(PointF(0, 0), PointF(0, (float)h),
                                                     Color(0xF2, 0x23, 0x29, 0x35),
                                                     Color(0xF2, 0x15, 0x19, 0x20));
                g.FillPath(&bgBrush, &path);
                Pen borderPen(Color(0x26, 0xFF, 0xFF, 0xFF), 1.f);
                g.DrawPath(&borderPen, &path);

                const Metric& m = self->_hoverPower ? self->_power : self->_fan;
                const wchar_t* title = self->_hoverPower ? L"CPU 功耗" : L"风扇转速";
                bool has = m.valid;

                wchar_t cur[32] = {};
                if (has)
                    swprintf_s(cur, self->_hoverPower ? L"%.1f" : L"%.0f",
                               (double)m.current);
                else
                    wcscpy_s(cur, L"--");

                FontFamily ffLbl(L"Microsoft YaHei UI");
                Font titleFont(&ffLbl, 13.f, Gdiplus::FontStyleRegular, UnitPixel);
                FontFamily ffVal(L"Segoe UI");
                Font valFont(&ffVal, 18.f, Gdiplus::FontStyleBold, UnitPixel);
                Font statFont(&ffVal, 14.f, Gdiplus::FontStyleBold, UnitPixel);
                Font smallFont(&ffLbl, 11.f, Gdiplus::FontStyleRegular, UnitPixel);

                SolidBrush titleBrush(Color(0xD6, 0xFF, 0xFF, 0xFF));
                Color vc = has ? (self->_hoverPower
                                      ? self->ValueColor(m.current, self->_thr.powerElevated, self->_thr.powerCritical)
                                      : self->ValueColor(m.current, self->_thr.fanElevated, self->_thr.fanCritical))
                               : Color(0x96, 0xFF, 0xFF, 0xFF);
                SolidBrush valBrush(vc);
                SolidBrush labelBrush(Color(0xA8, 0xFF, 0xFF, 0xFF));
                SolidBrush statBrush(Color(0xF2, 0xFF, 0xFF, 0xFF));

                // 顶部行：标题/数值/单位 底线对齐（同一条基线），圆点与标题垂直居中
                const wchar_t* unit = self->_hoverPower ? L"W" : L"RPM";
                const float rowBottom = 26.f;
                HDC mdc2 = CreateCompatibleDC(screenDC);
                InkBox tb = MeasureInk(mdc2, titleFont, title);
                InkBox vb = MeasureInk(mdc2, valFont, cur);
                InkBox ub = MeasureInk(mdc2, smallFont, unit);
                DeleteDC(mdc2);
                float groupW = vb.w + 3.f + ub.w;
                float vx = (float)w - 14.f - groupW;
                g.DrawString(title, -1, &titleFont, PointF(28.f - tb.x, rowBottom - tb.y - tb.h), &titleBrush);
                g.DrawString(cur, -1, &valFont, PointF(vx - vb.x, rowBottom - vb.y - vb.h), &valBrush);
                g.DrawString(unit, -1, &smallFont, PointF(vx + vb.w + 3.f - ub.x, rowBottom - ub.y - ub.h), &labelBrush);
                SolidBrush dotBrush(vc);
                g.FillEllipse(&dotBrush, 14.f, rowBottom - tb.h / 2.f - 4.f, 8.f, 8.f);

                // 中部：60 拍趋势图（参照 GlintBar 悬停弹出大图 + 统计）
                const std::vector<float>& hist = self->_hoverPower ? self->_powerHist : self->_fanHist;
                float gx = 14.f, gy = 38.f, gw = (float)w - 28.f, gh = 40.f;
                if (!hist.empty()) {
                    float vmin = hist[0], vmax = hist[0];
                    for (float v : hist) {
                        if (v < vmin) vmin = v;
                        if (v > vmax) vmax = v;
                    }
                    if (vmax - vmin < 1.f)
                        vmax = vmin + 1.f;
                    std::vector<PointF> pts;
                    pts.reserve(hist.size());
                    int n = (int)hist.size();
                    for (int i = 0; i < n; i++) {
                        float x = gx + (n > 1 ? (float)i / (float)(n - 1) : 0.f) * gw;
                        float y = gy + gh - (hist[i] - vmin) / (vmax - vmin) * gh;
                        pts.push_back(PointF(x, y));
                    }
                    BYTE r = vc.GetR(), gg = vc.GetG(), b = vc.GetB();
                    GraphicsPath area;
                    area.AddLines(pts.data(), (INT)pts.size());
                    area.AddLine(pts.back().X, gy + gh, pts.front().X, gy + gh);
                    area.CloseFigure();
                    SolidBrush areaBrush(Color(0x2E, r, gg, b));
                    g.FillPath(&areaBrush, &area);
                    Pen linePen(vc, 1.5f);
                    linePen.SetLineJoin(Gdiplus::LineJoinRound);
                    g.DrawLines(&linePen, pts.data(), (INT)pts.size());
                    g.FillEllipse(&dotBrush, pts.back().X - 2.5f, pts.back().Y - 2.5f, 5.f, 5.f);
                } else {
                    Pen ghostPen(Color(0x3D, 0xFF, 0xFF, 0xFF), 1.f);
                    g.DrawLine(&ghostPen, gx, gy + gh, gx + gw, gy + gh);
                }

                wchar_t minB[24], maxB[24], avgB[24];
                if (has) {
                    swprintf_s(minB, L"%.0f", (double)m.min);
                    swprintf_s(maxB, L"%.0f", (double)m.max);
                    swprintf_s(avgB, L"%.0f", (double)m.avg);
                } else {
                    wcscpy_s(minB, L"--");
                    wcscpy_s(maxB, L"--");
                    wcscpy_s(avgB, L"--");
                }
                // 底部统计：三列按列中心居中（标签、数值各自对中，横向整齐）
                HDC mdc3 = CreateCompatibleDC(screenDC);
                auto DrawStat = [&](float cx, const wchar_t* lbl, const wchar_t* val) {
                    InkBox lb = MeasureInk(mdc3, smallFont, lbl);
                    InkBox vb = MeasureInk(mdc3, statFont, val);
                    g.DrawString(lbl, -1, &smallFont, PointF(cx - lb.w / 2.f - lb.x, 84), &labelBrush);
                    g.DrawString(val, -1, &statFont, PointF(cx - vb.w / 2.f - vb.x, 98), &statBrush);
                };
                DrawStat(46.f, L"最低", minB);
                DrawStat(110.f, L"最高", maxB);
                DrawStat(174.f, L"平均", avgB);
                DeleteDC(mdc3);
            }

            BLENDFUNCTION blend{};
            blend.BlendOp = AC_SRC_OVER;
            blend.SourceConstantAlpha = 255;
            blend.AlphaFormat = AC_SRC_ALPHA;
            POINT ptSrc{};
            SIZE sz{ w, h };
            // 位置由 ShowTip 的 SetWindowPos 决定，这里不移动（nullptr = 位置不变）
            UpdateLayeredWindow(hwnd, screenDC, nullptr, &sz, memDC, &ptSrc, 0, &blend, ULW_ALPHA);

            SelectObject(memDC, oldBmp);
            DeleteObject(dib);
            DeleteDC(memDC);
            ReleaseDC(nullptr, screenDC);
            ValidateRect(hwnd, nullptr);
            return 0;
        }
        case WM_LBUTTONDOWN:
        case WM_MOUSEWHEEL:
            return 0;   // 卡片不抢焦点
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
