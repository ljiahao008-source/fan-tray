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
constexpr int kTipW = 216, kTipH = 132;

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

void Widget::Update(const SampleSet& s) {
    if (!_hwnd)
        return;
    if (!_embedded && !Embed())
        return;

    _metrics[(int)ItemKind::Power] = s.power;
    _metrics[(int)ItemKind::Fan] = s.fan;
    _metrics[(int)ItemKind::CpuUsage] = s.cpuUsage;
    _metrics[(int)ItemKind::CpuTemp] = s.cpuTemp;
    _metrics[(int)ItemKind::Mem] = s.mem;
    _metrics[(int)ItemKind::Net] = s.netDown;
    _netUp = s.netUp.valid ? s.netUp.current : 0.f;
    _netUpValid = s.netUp.valid;
    _hasData = true;

    // 60 拍滚动历史（供悬浮窗趋势图）；无效拍沿用上一值保持曲线连续
    for (int i = 0; i < kItemCount; i++) {
        const Metric& m = _metrics[i];
        if (m.valid)
            _hist[i].push_back(m.current);
        else if (!_hist[i].empty())
            _hist[i].push_back(_hist[i].back());
        if (_hist[i].size() > 60)
            _hist[i].erase(_hist[i].begin());
    }

    if (_tipHwnd && IsWindowVisible(_tipHwnd))
        InvalidateRect(_tipHwnd, nullptr, FALSE);   // 悬停时趋势图随采样实时刷新

    Render();
}

void Widget::ApplyConfig(const AppConfig& cfg) {
    _items[(int)ItemKind::Power].visible = cfg.ShowPower;
    _items[(int)ItemKind::Fan].visible = cfg.ShowFan;
    _items[(int)ItemKind::CpuUsage].visible = cfg.ShowCpuUsage;
    _items[(int)ItemKind::CpuTemp].visible = cfg.ShowCpuTemp;
    _items[(int)ItemKind::Mem].visible = cfg.ShowMem;
    _items[(int)ItemKind::Net].visible = cfg.ShowNet;

    // 防呆：全部取消勾选时兜底显示功耗，避免零宽窗口
    bool any = false;
    for (int i = 0; i < kItemCount; i++)
        any = any || _items[i].visible;
    if (!any)
        _items[(int)ItemKind::Power].visible = true;

    HideTip();   // 布局将变，先收卡片
    if (_hasData)
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

const wchar_t* Widget::ItemLabel(int idx) {
    switch ((ItemKind)idx) {
        case ItemKind::Power: return L"功耗";
        case ItemKind::Fan: return L"风扇";
        case ItemKind::CpuUsage: return L"占用";
        case ItemKind::CpuTemp: return L"温度";
        case ItemKind::Mem: return L"内存";
        case ItemKind::Net: return L"网速";
        default: return L"";
    }
}

const wchar_t* Widget::ItemTitle(int idx) {
    switch ((ItemKind)idx) {
        case ItemKind::Power: return L"CPU 功耗";
        case ItemKind::Fan: return L"风扇转速";
        case ItemKind::CpuUsage: return L"CPU 占用";
        case ItemKind::CpuTemp: return L"CPU 温度";
        case ItemKind::Mem: return L"内存占用";
        case ItemKind::Net: return L"网络速度";
        default: return L"";
    }
}

const wchar_t* Widget::ItemUnit(int idx) {
    switch ((ItemKind)idx) {
        case ItemKind::Power: return L"W";
        case ItemKind::Fan: return L"RPM";
        case ItemKind::CpuUsage: return L"%";
        case ItemKind::CpuTemp: return L"°C";
        case ItemKind::Mem: return L"%";
        case ItemKind::Net: return L"KB/s";
        default: return L"";
    }
}

// 速率格式化：<1000 KB/s 显示整数 KB/s，≥1000 显示 M（1 位小数）
static void FmtSpeed(float kb, wchar_t* buf, size_t cch) {
    if (kb >= 1024.f)
        swprintf_s(buf, cch, L"%.1fM", (double)(kb / 1024.f));
    else if (kb >= 10.f)
        swprintf_s(buf, cch, L"%.0f", (double)kb);
    else
        swprintf_s(buf, cch, L"%.1f", (double)kb);
}

void Widget::FormatValue(int idx, const Metric& m, float netUp, wchar_t* buf, size_t cch) {
    if (!m.valid) {
        wcscpy_s(buf, cch, L"--");
        return;
    }
    switch ((ItemKind)idx) {
        case ItemKind::Power:
            swprintf_s(buf, cch, L"%.1f", (double)m.current);   // 托盘窗口不带单位
            break;
        case ItemKind::Fan:
        case ItemKind::CpuUsage:
        case ItemKind::CpuTemp:
        case ItemKind::Mem:
            swprintf_s(buf, cch, L"%.0f", (double)m.current);
            break;
        case ItemKind::Net: {
            // 只取下行（↑上行由任务栏垂直行/卡片单独显示）
            wchar_t d[16] = {};
            FmtSpeed(m.current, d, 16);
            swprintf_s(buf, cch, L"↓%s", d);
            break;
        }
        default:
            wcscpy_s(buf, cch, L"--");
            break;
    }
}

Gdiplus::Color Widget::ItemColor(int idx) const {
    const Metric& m = _metrics[idx];
    if (!m.valid)
        return _labelColor;
    switch ((ItemKind)idx) {
        case ItemKind::Power: return ValueColor(m.current, _thr.powerElevated, _thr.powerCritical);
        case ItemKind::Fan: return ValueColor(m.current, _thr.fanElevated, _thr.fanCritical);
        case ItemKind::CpuUsage: return ValueColor(m.current, _thr.usageElevated, _thr.usageCritical);
        case ItemKind::CpuTemp: return ValueColor(m.current, _thr.tempElevated, _thr.tempCritical);
        case ItemKind::Mem: return ValueColor(m.current, _thr.memElevated, _thr.memCritical);
        case ItemKind::Net: return _sevGreen;   // 网速无阈值语义：恒定"正常色"
        default: return _labelColor;
    }
}

// 按"最坏值样本"一次性定块宽：数值位数进位/单位切换都不改变窗口宽度（防抽动）
void Widget::MeasureBlocks(HDC screenDC) {
    static const wchar_t* kMaxSample[kItemCount] = {
        L"888.8",          // 功耗
        L"8888",           // 风扇
        L"100",            // 占用
        L"100",            // 温度
        L"100",            // 内存
        L"↓888.8M",        // 网速（下行，垂直堆叠单值）
    };
    HDC mdc = CreateCompatibleDC(screenDC);
    FontFamily ffVal(L"Segoe UI");
    Font valFont(&ffVal, 12.f, Gdiplus::FontStyleBold, UnitPixel);
    FontFamily ffLbl(L"Microsoft YaHei UI");
    Font lblFont(&ffLbl, 9.f, Gdiplus::FontStyleRegular, UnitPixel);

    for (int i = 0; i < kItemCount; i++) {
        InkBox vb = MeasureInk(mdc, valFont, kMaxSample[i]);
        InkBox lb = MeasureInk(mdc, lblFont, ItemLabel(i));
        _items[i].blockW = (int)std::ceil(std::max(vb.w, lb.w)) + kTextPad * 2;
    }
    DeleteDC(mdc);
    _blocksMeasured = true;
}

void Widget::Render() {
    HDC screenDC = GetDC(nullptr);

    FontFamily ffVal(L"Segoe UI");
    Font valFont(&ffVal, 12.f, Gdiplus::FontStyleBold, UnitPixel);
    FontFamily ffLbl(L"Microsoft YaHei UI");
    Font lblFont(&ffLbl, 9.f, Gdiplus::FontStyleRegular, UnitPixel);

    if (!_blocksMeasured)
        MeasureBlocks(screenDC);

    // 只排列可见项：x 逐个累加（定宽块 + 项间距）
    int visIdx[kItemCount] = {};
    int visCount = 0;
    int cursorX = 0;
    for (int i = 0; i < kItemCount; i++) {
        if (!_items[i].visible)
            continue;
        if (visCount > 0)
            cursorX += kPairGap;
        _items[i].x = cursorX;
        cursorX += _items[i].blockW;
        visIdx[visCount++] = i;
    }
    _contentW = cursorX;

    if (visCount == 0) {
        ReleaseDC(nullptr, screenDC);
        return;
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

        HDC mdcInk = CreateCompatibleDC(screenDC);

        for (int k = 0; k < visCount; k++) {
            int i = visIdx[k];
            const Metric& m = _metrics[i];
            int x0 = _items[i].x;
            int bw = _items[i].blockW;

            if (_hovering && _hoverIdx == i) {
                SolidBrush hb(_hoverBack);
                g.FillRectangle(&hb, (float)x0, 1.f, (float)bw, (float)(height - 2));
            }

            // 网速块：垂直堆叠（下行在上 / 上行在下），无标签行
            if ((ItemKind)i == ItemKind::Net) {
                wchar_t d[24] = {}, u[24] = {};
                if (m.valid)
                    FmtSpeed(m.current, d, 24);
                else
                    wcscpy_s(d, L"--");
                if (_netUpValid)
                    FmtSpeed(_netUp, u, 24);
                else
                    wcscpy_s(u, L"--");

                InkBox db = MeasureInk(mdcInk, valFont, d);
                InkBox ub = MeasureInk(mdcInk, valFont, u);

                Color dcColor = m.valid ? _sevGreen : _labelColor;              // 下行：绿
                Color ucColor = _netUpValid ? MakeColor(255, 0x4A, 0x9E, 0xDE)
                                            : _labelColor;                      // 上行：青蓝
                SolidBrush dBrush(dcColor);
                SolidBrush uBrush(ucColor);

                float dx = x0 + (bw - db.w) / 2.f - db.x;
                g.DrawString(d, -1, &valFont, PointF(dx, (float)(height / 2 - 15)), &dBrush);
                float ux = x0 + (bw - ub.w) / 2.f - ub.x;
                g.DrawString(u, -1, &valFont, PointF(ux, (float)(height / 2 + 2)), &uBrush);
                continue;
            }

            wchar_t vbuf[40] = {};
            FormatValue(i, m, _netUp, vbuf, 40);
            const wchar_t* label = ItemLabel(i);

            InkBox vb = MeasureInk(mdcInk, valFont, vbuf);
            InkBox lb = MeasureInk(mdcInk, lblFont, label);

            // 手动居中：墨迹盒宽 = 块宽-2*pad 时左右各留 1px；PointF 定位不裁剪
            Color vc = ItemColor(i);
            SolidBrush valBrush(vc);
            float vx = x0 + (bw - vb.w) / 2.f - vb.x;
            g.DrawString(vbuf, -1, &valFont, PointF(vx, (float)(height / 2 - 12)), &valBrush);

            SolidBrush lblBrush(_labelColor);
            float lx = x0 + (bw - lb.w) / 2.f - lb.x;
            g.DrawString(label, -1, &lblFont, PointF(lx, (float)(height / 2 + 4)), &lblBrush);
        }

        DeleteDC(mdcInk);
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

int Widget::HitTest(POINT pt) const {
    if (!_hwnd)
        return -1;
    RECT r{};
    if (!GetWindowRect(_hwnd, &r))
        return -1;
    double scale = GetDpiForWindow(_hwnd) / 96.0;
    int rel = pt.x - r.left;
    for (int i = 0; i < kItemCount; i++) {
        if (!_items[i].visible)
            continue;
        int left = (int)(_items[i].x * scale);
        int right = (int)((_items[i].x + _items[i].blockW) * scale);
        if (rel >= left && rel < right)
            return i;
    }
    return -1;
}

void Widget::ShowTip(POINT pt) {
    int idx = HitTest(pt);
    if (idx < 0) {
        HideTip();
        return;
    }

    if (idx == _hoverIdx && _hovering && _tipHwnd && IsWindowVisible(_tipHwnd)) {
        // 同一块悬停，仅更新内容
        InvalidateRect(_tipHwnd, nullptr, FALSE);
        return;
    }

    if (!_tipHwnd) {
        _tipHwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
                                   kTipClass, L"", WS_POPUP, 0, 0, kTipW, kTipH,
                                   nullptr, nullptr, _hInst, this);
    }
    _hoverIdx = idx;
    _hovering = true;

    RECT r{};
    GetWindowRect(_hwnd, &r);
    RECT tb{};
    if (!GetWindowRect(_taskbar, &tb))
        return;

    double scale = GetDpiForWindow(_hwnd) / 96.0;
    int blockX = r.left + (int)(_items[idx].x * scale);
    int blockW = (int)(_items[idx].blockW * scale);
    int cardX = blockX + blockW / 2 - kTipW / 2;
    if (cardX < 0)
        cardX = 0;   // 屏幕左缘保护
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
            if (HitTest(pt) >= 0)
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

                int idx = self->_hoverIdx;
                if (idx < 0 || idx >= kItemCount)
                    idx = 0;
                const Metric& m = self->_metrics[idx];
                const wchar_t* title = ItemTitle(idx);
                bool has = m.valid;

                wchar_t cur[40] = {};
                FormatValue(idx, m, self->_netUp, cur, 40);

                FontFamily ffLbl(L"Microsoft YaHei UI");
                Font titleFont(&ffLbl, 13.f, Gdiplus::FontStyleRegular, UnitPixel);
                FontFamily ffVal(L"Segoe UI");
                Font valFont(&ffVal, 18.f, Gdiplus::FontStyleBold, UnitPixel);
                Font statFont(&ffVal, 14.f, Gdiplus::FontStyleBold, UnitPixel);
                Font smallFont(&ffLbl, 11.f, Gdiplus::FontStyleRegular, UnitPixel);

                SolidBrush titleBrush(Color(0xD6, 0xFF, 0xFF, 0xFF));
                Color vc = has ? self->ItemColor(idx) : Color(0x96, 0xFF, 0xFF, 0xFF);
                SolidBrush valBrush(vc);
                SolidBrush labelBrush(Color(0xA8, 0xFF, 0xFF, 0xFF));
                SolidBrush statBrush(Color(0xF2, 0xFF, 0xFF, 0xFF));

                // 顶部行：标题/数值/单位 底线对齐（同一条基线），圆点与标题垂直居中
                const wchar_t* unit = ItemUnit(idx);
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
                const std::vector<float>& hist = self->_hist[idx];
                float gx = 14.f, gy = 38.f, gw = (float)w - 28.f, gh = 40.f;

                // 网速项：标题行下方补一行「上行 xx KB/s」（下行走标题/趋势图/统计主线）
                bool isNet = ((ItemKind)idx == ItemKind::Net);
                if (isNet) {
                    gy = 52.f;
                    wchar_t upLine[48] = {};
                    if (self->_netUpValid) {
                        wchar_t ub2[16] = {};
                        FmtSpeed(self->_netUp, ub2, 16);
                        swprintf_s(upLine, L"↑%s KB/s 上行", ub2);
                    } else {
                        wcscpy_s(upLine, L"上行 -- KB/s");
                    }
                    FontFamily ffS(L"Segoe UI");
                    Font upFont(&ffS, 11.f, Gdiplus::FontStyleRegular, UnitPixel);
                    SolidBrush upBrush(Color(0xC0, 0x4A, 0x9E, 0xDE));   // 青蓝（与任务栏上行色一致）
                    g.DrawString(upLine, -1, &upFont, PointF(28.f, 31.f), &upBrush);
                }

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
