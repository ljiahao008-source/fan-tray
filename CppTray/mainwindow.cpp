// mainwindow.cpp —— 主程序交互窗口
// 上部：实时监控数值（深色卡片风格，随阈值变色）
// 下部：操作按钮（设置/锁屏设置/重置统计/退出），点击转发宿主处理
// 关闭窗口 = 隐藏回托盘；退出需走【退出】按钮或托盘菜单。

#include "mainwindow.h"

#include <cmath>
#include <cstdio>
#include <cwchar>
#include <vector>

#include <gdiplus.h>

#pragma comment(lib, "gdiplus.lib")

using Gdiplus::Color;
using Gdiplus::Font;
using Gdiplus::FontFamily;
using Gdiplus::Graphics;
using Gdiplus::Pen;
using Gdiplus::PointF;
using Gdiplus::SolidBrush;
using Gdiplus::TextRenderingHint;
using Gdiplus::UnitPixel;

namespace {

constexpr wchar_t kMainWinClass[] = L"MechrevoMainWindowClass";
constexpr int kClientW = 300, kClientH = 372;

HWND g_hwnd = nullptr;
HINSTANCE g_hInst = nullptr;
HWND g_host = nullptr;
SampleSet g_snap;
AppConfig g_cfg;
Thresholds g_thr;
ULONG_PTR g_gdiToken = 0;

Color SevColor(double v, double elevated, double critical) {
    if (v <= elevated)
        return Color(255, 0x4A, 0xDE, 0x80);   // 绿
    if (v <= critical)
        return Color(255, 0xFB, 0x92, 0x3C);   // 橙
    return Color(255, 0xF8, 0x71, 0x71);       // 红
}

struct Row {
    const wchar_t* label;
    wchar_t value[40];
    Color color;
};

void BuildRows(const SampleSet& s, std::vector<Row>& rows) {
    auto fmt = [](const Metric& m, const wchar_t* fmt, wchar_t* out, size_t n) {
        if (!m.valid)
            wcscpy_s(out, n, L"--");
        else
            swprintf_s(out, n, fmt, (double)m.current);
    };

    if (g_cfg.ShowPower) {
        Row r{ L"功耗", L"", SevColor(s.power.current, g_thr.powerElevated, g_thr.powerCritical) };
        fmt(s.power, L"%.1f W", r.value, 40);
        rows.push_back(r);
    }
    if (g_cfg.ShowFan) {
        Row r{ L"风扇转速", L"", SevColor(s.fan.current, g_thr.fanElevated, g_thr.fanCritical) };
        fmt(s.fan, L"%.0f RPM", r.value, 40);
        rows.push_back(r);
    }
    if (g_cfg.ShowCpuUsage) {
        Row r{ L"CPU 占用", L"", SevColor(s.cpuUsage.current, g_thr.usageElevated, g_thr.usageCritical) };
        fmt(s.cpuUsage, L"%.0f %%", r.value, 40);
        rows.push_back(r);
    }
    if (g_cfg.ShowCpuTemp) {
        Row r{ L"CPU 温度", L"", SevColor(s.cpuTemp.current, g_thr.tempElevated, g_thr.tempCritical) };
        fmt(s.cpuTemp, L"%.0f °C", r.value, 40);
        rows.push_back(r);
    }
    if (g_cfg.ShowMem) {
        Row r{ L"内存占用", L"", SevColor(s.mem.current, g_thr.memElevated, g_thr.memCritical) };
        fmt(s.mem, L"%.0f %%", r.value, 40);
        rows.push_back(r);
    }
    if (g_cfg.ShowNet) {
        Row r{ L"网速", L"", Color(255, 0xFF, 0xFF, 0xFF) };
        wchar_t d[16] = {}, u[16] = {};
        fmt(s.netDown, L"%.0f", d, 16);
        fmt(s.netUp, L"%.0f", u, 16);
        swprintf_s(r.value, L"↓ %s ↑ %s KB/s", d, u);
        rows.push_back(r);
    }
}

void DrawWindow(HWND hwnd) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    int w = rc.right, h = rc.bottom;

    HDC screenDC = GetDC(nullptr);
    HDC memDC = CreateCompatibleDC(screenDC);
    HBITMAP dib = CreateCompatibleBitmap(screenDC, w, h);
    HGDIOBJ old = SelectObject(memDC, dib);

    {
        Graphics g(memDC);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
        g.Clear(Color(255, 0x1A, 0x1E, 0x26));

        FontFamily ffTitle(L"Microsoft YaHei UI");
        Font titleFont(&ffTitle, 16.f, Gdiplus::FontStyleBold, UnitPixel);
        FontFamily ffVal(L"Segoe UI");
        Font valFont(&ffVal, 14.f, Gdiplus::FontStyleBold, UnitPixel);
        FontFamily ffLbl(L"Microsoft YaHei UI");
        Font lblFont(&ffLbl, 13.f, Gdiplus::FontStyleRegular, UnitPixel);

        SolidBrush titleBrush(Color(0xE6, 0xFF, 0xFF, 0xFF));
        SolidBrush lblBrush(Color(0xA8, 0xFF, 0xFF, 0xFF));

        // 标题
        g.DrawString(L"机械革命监控", -1, &titleFont, PointF(20, 14), &titleBrush);
        Pen divider(Color(0x26, 0xFF, 0xFF, 0xFF), 1.f);
        g.DrawLine(&divider, 20.f, 46.f, (float)(w - 20), 46.f);

        // 数据行
        std::vector<Row> rows;
        BuildRows(g_snap, rows);

        int y = 66;
        const int rowH = 40;
        for (auto& r : rows) {
            SolidBrush rb(r.color);
            g.DrawString(r.label, -1, &lblFont, PointF(20.f, (float)y + 4), &lblBrush);
            g.DrawString(r.value, -1, &valFont, PointF(140.f, (float)y), &rb);
            y += rowH;
        }
    }

    HDC wdc = GetDC(hwnd);
    BitBlt(wdc, 0, 0, w, h, memDC, 0, 0, SRCCOPY);
    ReleaseDC(hwnd, wdc);

    SelectObject(memDC, old);
    DeleteObject(dib);
    DeleteDC(memDC);
    ReleaseDC(nullptr, screenDC);
}

// 创建底部操作按钮（点击 → kActionMsg 转发宿主）
void CreateButtons() {
    struct Btn { int id; const wchar_t* text; int x, w; };
    Btn btns[] = {
        { (int)mainwin::ActionSettings,    L"设置",       13, 56 },
        { (int)mainwin::ActionLockScreen,  L"锁屏设置",   79, 74 },
        { (int)mainwin::ActionReset,       L"重置统计",   161, 74 },
        { (int)mainwin::ActionExit,        L"退出",       243, 44 },
    };
    for (auto& b : btns) {
        CreateWindowExW(0, L"BUTTON", b.text,
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                        b.x, 330, b.w, 30, g_hwnd, (HMENU)(INT_PTR)b.id, g_hInst, nullptr);
    }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_COMMAND: {
            int id = LOWORD(wp);
            if (id >= (int)mainwin::ActionSettings && id <= (int)mainwin::ActionExit) {
                if (g_host)
                    PostMessageW(g_host, mainwin::kActionMsg, (WPARAM)id, 0);
                return 0;
            }
            break;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            DrawWindow(hwnd);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_CLOSE:
            ShowWindow(hwnd, SW_HIDE);   // 关闭 = 隐藏回托盘
            return 0;
        case WM_DESTROY:
            if (g_hwnd == hwnd)
                g_hwnd = nullptr;
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

namespace mainwin {

bool Create(HINSTANCE hInst) {
    g_hInst = hInst;

    Gdiplus::GdiplusStartupInput gsi;
    if (Gdiplus::GdiplusStartup((ULONG_PTR*)&g_gdiToken, &gsi, nullptr) != Gdiplus::Ok)
        return false;

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = kMainWinClass;
    RegisterClassW(&wc);

    g_hwnd = CreateWindowExW(0, kMainWinClass, L"机械革命监控",
                             WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                             CW_USEDEFAULT, CW_USEDEFAULT, kClientW + 16, kClientH + 39,
                             nullptr, nullptr, hInst, nullptr);
    if (!g_hwnd)
        return false;
    CreateButtons();
    return true;
}

void SetHost(HWND hostHiddenMain) {
    g_host = hostHiddenMain;
}

void Show() {
    if (!g_hwnd)
        return;
    if (IsWindowVisible(g_hwnd)) {
        SetForegroundWindow(g_hwnd);
        return;
    }
    // 居中于主屏工作区
    RECT wa{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    RECT wr{};
    GetWindowRect(g_hwnd, &wr);
    int x = wa.left + ((wa.right - wa.left) - (wr.right - wr.left)) / 2;
    int y = wa.top + ((wa.bottom - wa.top) - (wr.bottom - wr.top)) / 2;
    SetWindowPos(g_hwnd, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
    ShowWindow(g_hwnd, SW_SHOW);
    SetForegroundWindow(g_hwnd);
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

void Hide() {
    if (g_hwnd)
        ShowWindow(g_hwnd, SW_HIDE);
}

bool IsVisible() {
    return g_hwnd && IsWindowVisible(g_hwnd);
}

HWND Hwnd() { return g_hwnd; }

void Update(const SampleSet& s) {
    g_snap = s;
    if (g_hwnd && IsWindowVisible(g_hwnd))
        InvalidateRect(g_hwnd, nullptr, FALSE);
}

void ApplyConfig(const AppConfig& cfg) {
    g_cfg = cfg;
    if (g_hwnd && IsWindowVisible(g_hwnd))
        InvalidateRect(g_hwnd, nullptr, FALSE);
}

void SetThresholds(const Thresholds& thr) {
    g_thr = thr;
    if (g_hwnd && IsWindowVisible(g_hwnd))
        InvalidateRect(g_hwnd, nullptr, FALSE);
}

void DestroyWindowW() {
    if (g_hwnd) {
        DestroyWindow(g_hwnd);
        g_hwnd = nullptr;
    }
    if (g_gdiToken) {
        Gdiplus::GdiplusShutdown((ULONG_PTR)g_gdiToken);
        g_gdiToken = 0;
    }
}

}  // namespace mainwin
