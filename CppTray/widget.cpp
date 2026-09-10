#include "widget.h"

#include <windowsx.h>
#include <cmath>
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
using Gdiplus::StringAlignment;
using Gdiplus::TextRenderingHint;
using Gdiplus::UnitPixel;

namespace {

constexpr wchar_t kWidgetClass[] = L"MechrevoTrayWidgetClass";
constexpr wchar_t kTipClass[] = L"MechrevoTrayTipClass";
constexpr wchar_t kThemeKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";

constexpr int kGapPx = 2;          // 与托盘角落间距
constexpr int kPairGap = 12;       // 功耗/风扇两块的间距（含内边距）
constexpr int kHoverPad = 5;       // 悬停热区余量
constexpr int kTipW = 214, kTipH = 88;

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

int MeasureText(Gdiplus::Graphics& g, const wchar_t* text, const Font& font) {
    RectF rect;
    g.MeasureString(text, -1, &font, PointF(0, 0), &rect);
    return (int)std::ceil(rect.Width) + 2;
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

    // 测量内容宽度（固定最宽值防抖）
    {
        HDC mdc = CreateCompatibleDC(screenDC);
        Graphics g(mdc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        int pw = MeasureText(g, L"888.8W", valFont);
        int fw = MeasureText(g, L"8888", valFont);
        int lp = MeasureText(g, L"功耗", lblFont);
        int lf = MeasureText(g, L"风扇", lblFont);
        if (lp > pw) pw = lp;
        if (lf > fw) fw = lf;
        DeleteDC(mdc);
        _powerBlockW = pw + kHoverPad * 2;
        _fanBlockW = fw + kHoverPad * 2;
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
        g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);
        g.Clear(Color(0, 0, 0, 0));

        auto DrawPair = [&](int x0, int bw, const wchar_t* valueText, const wchar_t* label,
                            const Metric& m, double elevated, double critical, bool hover) {
            if (hover) {
                SolidBrush hb(_hoverBack);
                g.FillRectangle(&hb, (float)x0, 1.f, (float)bw, (float)(height - 2));
            }

            StringFormat sf;
            sf.SetAlignment(Gdiplus::StringAlignmentCenter);
            RectF valRect((float)(x0 + kHoverPad), (float)(height / 2 - 12), (float)(bw - kHoverPad * 2), 18.f);
            Color vc = m.valid ? ValueColor(m.current, elevated, critical) : _labelColor;
            SolidBrush valBrush(vc);
            g.DrawString(valueText, -1, &valFont, valRect, &sf, &valBrush);

            RectF lblRect((float)(x0 + kHoverPad), (float)(height / 2 + 4), (float)(bw - kHoverPad * 2), 16.f);
            SolidBrush lblBrush(_labelColor);
            g.DrawString(label, -1, &lblFont, lblRect, &sf, &lblBrush);
        };

        wchar_t pbuf[32] = {};
        if (_power.valid)
            swprintf_s(pbuf, L"%.1fW", (double)_power.current);
        else
            wcscpy_s(pbuf, L"--");

        wchar_t fbuf[32] = {};
        if (_fan.valid)
            swprintf_s(fbuf, L"%.0f", (double)_fan.current);
        else
            wcscpy_s(fbuf, L"--");

        DrawPair(0, _powerBlockW, pbuf, L"功耗", _power, _thr.powerElevated, _thr.powerCritical,
                 _hovering && _hoverPower);
        DrawPair(_powerBlockW + kPairGap, _fanBlockW, fbuf, L"风扇", _fan, _thr.fanElevated, _thr.fanCritical,
                 _hovering && !_hoverPower);
    }

    BLENDFUNCTION blend{};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;

    POINT ptSrc{};
    POINT ptDst{};   // UpdateLayeredWindow 的 ptDst 是屏幕坐标。taskbar 屏幕原点 + 窗口相对位置即可，
    RECT tb2{};      // 不能用 GetWindowRect(_hwnd)（对嵌入子窗口返回异常值）。
    if (_taskbar && GetWindowRect(_taskbar, &tb2)) {
        ptDst.x = tb2.left + _relX;
        ptDst.y = tb2.top + _relY;
    }
    SIZE sz{ width, height };
    UpdateLayeredWindow(_hwnd, screenDC, &ptDst, &sz, memDC, &ptSrc, 0, &blend, ULW_ALPHA);

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

    // 相对 taskbar 的坐标（y 恒 0，贴任务栏顶部）；记录供 Render 转屏幕坐标
    _relX = x;
    _relY = 0;
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
                g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);
                g.Clear(Color(0, 0, 0, 0));

                // 深色圆角卡片
                GraphicsPath path;
                int rad = 10;
                path.AddArc(0, 0, rad * 2, rad * 2, 180, 90);
                path.AddArc(w - rad * 2, 0, rad * 2, rad * 2, 270, 90);
                path.AddArc(w - rad * 2, h - rad * 2, rad * 2, rad * 2, 0, 90);
                path.AddArc(0, h - rad * 2, rad * 2, rad * 2, 90, 90);
                path.CloseFigure();
                SolidBrush cardBrush(Color(0xF2, 0x17, 0x24, 0x2F));
                g.FillPath(&cardBrush, &path);
                Pen borderPen(Color(0x2E, 0xFF, 0xFF, 0xFF), 1.f);
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
                Font titleFont(&ffLbl, 12.f, Gdiplus::FontStyleRegular, UnitPixel);
                FontFamily ffVal(L"Segoe UI");
                Font valFont(&ffVal, 20.f, Gdiplus::FontStyleBold, UnitPixel);
                Font statFont(&ffVal, 13.f, Gdiplus::FontStyleBold, UnitPixel);
                Font smallFont(&ffLbl, 10.f, Gdiplus::FontStyleRegular, UnitPixel);

                SolidBrush titleBrush(Color(0xC8, 0xFF, 0xFF, 0xFF));
                Color vc = has ? (self->_hoverPower
                                      ? self->ValueColor(m.current, self->_thr.powerElevated, self->_thr.powerCritical)
                                      : self->ValueColor(m.current, self->_thr.fanElevated, self->_thr.fanCritical))
                               : Color(0x96, 0xFF, 0xFF, 0xFF);
                SolidBrush valBrush(vc);
                SolidBrush labelBrush(Color(0x96, 0xFF, 0xFF, 0xFF));
                SolidBrush statBrush(Color(0xF2, 0xFF, 0xFF, 0xFF));

                g.DrawString(title, -1, &titleFont, PointF(14, 10), &titleBrush);
                g.DrawString(cur, -1, &valFont, PointF(84, 2), &valBrush);
                g.DrawString(self->_hoverPower ? L"W" : L"RPM", -1, &smallFont, PointF(168, 14), &labelBrush);

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
                g.DrawString(L"最低", -1, &smallFont, PointF(14, 46), &labelBrush);
                g.DrawString(minB, -1, &statFont, PointF(14, 60), &statBrush);
                g.DrawString(L"最高", -1, &smallFont, PointF(88, 46), &labelBrush);
                g.DrawString(maxB, -1, &statFont, PointF(88, 60), &statBrush);
                g.DrawString(L"平均", -1, &smallFont, PointF(162, 46), &labelBrush);
                g.DrawString(avgB, -1, &statFont, PointF(162, 60), &statBrush);
            }

            BLENDFUNCTION blend{};
            blend.BlendOp = AC_SRC_OVER;
            blend.SourceConstantAlpha = 255;
            blend.AlphaFormat = AC_SRC_ALPHA;
            POINT ptSrc{}, ptDst{};
            SIZE sz{ w, h };
            UpdateLayeredWindow(hwnd, screenDC, &ptDst, &sz, memDC, &ptSrc, 0, &blend, ULW_ALPHA);

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
