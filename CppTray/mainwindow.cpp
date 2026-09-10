// mainwindow.cpp —— Twinkle Tray 式显示器控制面板
// 每台显示器一张卡片：亮度/对比度/音量滑块、输入源、电源；
// 滑块拖动松手即写 DDC/CI；数值每秒刷新（可见时）。

#include "mainwindow.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <vector>

#include <commctrl.h>
#include <gdiplus.h>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "comctl32.lib")

#include "ddc.h"

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
constexpr int kClientW = 320;

HWND g_hwnd = nullptr;
HINSTANCE g_hInst = nullptr;
HWND g_host = nullptr;
ULONG_PTR g_gdiToken = 0;
bool g_rebuilding = false;

// 控件 ID 段
constexpr int kIdTrackBase   = 0x10000;   // + (monitorIdx<<8) + feature
constexpr int kIdInputBase   = 0x20000;   // + (monitorIdx<<8) + value
constexpr int kIdPowerBase   = 0x30000;   // + monitorIdx

struct SliderCtl {
    HWND track = nullptr;
    HWND label = nullptr;
    BYTE code = 0;          // 0x10/0x12/0x62
    int monitorIdx = 0;
};

struct MonitorCard {
    std::wstring deviceName;
    std::vector<SliderCtl> sliders;
    std::vector<HWND> inputButtons;
    HWND powerButton = nullptr;
    bool dragging = false;
};

std::vector<MonitorCard> g_cards;

Color SevColor(double v, double elevated, double critical) {
    if (v <= elevated) return Color(255, 0x4A, 0xDE, 0x80);
    if (v <= critical) return Color(255, 0xFB, 0x92, 0x3C);
    return Color(255, 0xF8, 0x71, 0x71);
}

int TrackId(int mi, BYTE code) { return kIdTrackBase + (mi << 8) + code; }
int InputId(int mi, int value) { return kIdInputBase + (mi << 8) + value; }
int PowerId(int mi) { return kIdPowerBase + mi; }

// 控件创建（统一 UI 字体）
HFONT g_uiFont = nullptr;
HWND MakeCtrl(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id) {
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, g_hwnd,
                             (HMENU)(INT_PTR)id, g_hInst, nullptr);
    if (g_uiFont)
        SendMessageW(c, WM_SETFONT, (WPARAM)g_uiFont, TRUE);
    return c;
}

// 面板重建：销毁旧控件，按当前显示器列表重建
void RebuildPanel() {
    g_rebuilding = true;
    // 销毁全部子控件（递归）
    HWND child = GetWindow(g_hwnd, GW_CHILD);
    while (child) {
        HWND next = GetWindow(child, GW_HWNDNEXT);
        DestroyWindow(child);
        child = next;
    }
    g_cards.clear();

    std::vector<ddc::Monitor> monitors = ddc::Ddc::Enumerate();

    int y = 10;
    HFONT titleFont = nullptr;
    {
        NONCLIENTMETRICSW ncm{};
        ncm.cbSize = sizeof(ncm);
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
        LOGFONTW lf = ncm.lfMessageFont;
        lf.lfHeight = -16;
        lf.lfWeight = FW_BOLD;
        titleFont = CreateFontIndirectW(&lf);
    }

    // 面板标题
    HWND hdr = MakeCtrl(L"STATIC", L"显示器亮度控制（DDC/CI）", SS_LEFT, 14, y, 260, 20, 0);
    SendMessageW(hdr, WM_SETFONT, (WPARAM)titleFont, TRUE);
    y += 28;

    for (size_t mi = 0; mi < monitors.size(); mi++) {
        auto& mon = monitors[mi];
        MonitorCard card;
        card.deviceName = mon.deviceName;

        int cy = y;
        HWND t = MakeCtrl(L"STATIC", mon.description.empty() ? L"显示器" : mon.description.c_str(),
                          SS_LEFT, 14, cy, 280, 18, 0);
        SendMessageW(t, WM_SETFONT, (WPARAM)titleFont, TRUE);
        cy += 26;

        auto addSlider = [&](const wchar_t* name, BYTE code, DWORD cur, DWORD max) {
            SliderCtl s;
            s.code = code;
            s.monitorIdx = (int)mi;
            MakeCtrl(L"STATIC", name, SS_LEFT, 20, cy + 3, 44, 16, 0);
            s.track = MakeCtrl(TRACKBAR_CLASSW, L"",
                               WS_TABSTOP | TBS_AUTOTICKS | TBS_ENABLESELRANGE,
                               70, cy, 190, 22, TrackId((int)mi, code));
            SendMessageW(s.track, TBM_SETRANGE, TRUE, MAKELPARAM(0, 1000));
            int pct = max > 0 ? (int)std::lround(cur * 100.0 / max) : 0;
            SendMessageW(s.track, TBM_SETPOS, TRUE, std::clamp(pct * 10, 0, 1000));
            wchar_t buf[16] = {};
            swprintf_s(buf, L"%d%%", pct);
            s.label = MakeCtrl(L"STATIC", buf, SS_RIGHT, 262, cy + 3, 44, 16, 0);
            card.sliders.push_back(s);
            cy += 28;
        };

        if (mon.hasBrightness)
            addSlider(L"亮度", ddc::kVcpLuminance, mon.brightnessCur, mon.brightnessMax);
        if (mon.hasContrast)
            addSlider(L"对比度", ddc::kVcpContrast, mon.contrastCur, mon.contrastMax);
        if (mon.hasVolume)
            addSlider(L"音量", ddc::kVcpAudioVolume, mon.volumeCur, mon.volumeMax);

        if (mon.hasInputs && !mon.inputs.empty()) {
            MakeCtrl(L"STATIC", L"输入源", SS_LEFT, 20, cy + 3, 44, 16, 0);
            int bx = 70;
            for (DWORD v : mon.inputs) {
                wchar_t buf[24] = {};
                swprintf_s(buf, L"%lu", v);
                HWND b = MakeCtrl(L"BUTTON", buf, WS_TABSTOP | BS_PUSHBUTTON,
                                  bx, cy, 34, 22, InputId((int)mi, (int)v));
                card.inputButtons.push_back(b);
                bx += 38;
                if (bx > 280) break;
            }
            cy += 30;
        }

        if (mon.hasPower) {
            card.powerButton = MakeCtrl(L"BUTTON", L"电源（关）", WS_TABSTOP | BS_PUSHBUTTON,
                                        70, cy, 90, 24, PowerId((int)mi));
            cy += 32;
        }

        // 卡片分隔线
        HWND sep = MakeCtrl(L"STATIC", L"", SS_LEFT, 10, cy, 300, 1, 0);
        (void)sep;
        y = cy + 14;
        g_cards.push_back(std::move(card));
    }

    if (monitors.empty()) {
        MakeCtrl(L"STATIC", L"未检测到支持 DDC/CI 的显示器。", SS_LEFT, 14, y, 280, 18, 0);
        y += 28;
    }

    y += 6;
    // 底部操作按钮
    struct Btn { int id; const wchar_t* text; int x, w; };
    Btn btns[] = {
        { (int)mainwin::ActionSettings,   L"设置",     13, 56 },
        { (int)mainwin::ActionLockScreen, L"锁屏设置", 79, 74 },
        { (int)mainwin::ActionReset,      L"重置统计", 161, 74 },
        { (int)mainwin::ActionExit,       L"退出",     243, 44 },
    };
    for (auto& b : btns)
        MakeCtrl(L"BUTTON", b.text, WS_TABSTOP | BS_PUSHBUTTON, b.x, y, b.w, 30, b.id);
    y += 40;

    if (titleFont)
        DeleteObject(titleFont);

    // 调整窗口高度适配内容
    RECT rc{};
    GetClientRect(g_hwnd, &rc);
    int curH = rc.bottom;
    if (y != curH && g_hwnd) {
        RECT wr{};
        GetWindowRect(g_hwnd, &wr);
        SetWindowPos(g_hwnd, nullptr, 0, 0, kClientW + 16, y + 39, SWP_NOMOVE | SWP_NOACTIVATE);
    }
    g_rebuilding = false;
}

// 刷新数值：更新滑块位置与百分比标签
void RefreshValues() {
    if (g_rebuilding)
        return;
    std::vector<ddc::Monitor> monitors = ddc::Ddc::Enumerate();
    if (monitors.size() != g_cards.size())
        return;   // 显示器集合变化由 Rebuild 处理

    for (size_t mi = 0; mi < g_cards.size(); mi++) {
        auto& card = g_cards[mi];
        auto& mon = monitors[mi];
        if (card.deviceName != mon.deviceName)
            return;
        for (auto& s : card.sliders) {
            DWORD cur = 0, max = 0;
            if (!ddc::Ddc::GetVCP(card.deviceName, s.code, cur, max))
                continue;
            if (max == 0)
                max = 100;
            int pct = (int)std::lround(cur * 100.0 / max);
            if (!card.dragging)
                SendMessageW(s.track, TBM_SETPOS, TRUE, std::clamp(pct * 10, 0, 1000));
            wchar_t buf[16] = {};
            swprintf_s(buf, L"%d%%", pct);
            SetWindowTextW(s.label, buf);
        }
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
        g.Clear(Color(255, 0x1A, 0x1E, 0x26));
    }
    HDC wdc = GetDC(hwnd);
    BitBlt(wdc, 0, 0, w, h, memDC, 0, 0, SRCCOPY);
    ReleaseDC(hwnd, wdc);
    SelectObject(memDC, old);
    DeleteObject(dib);
    DeleteDC(memDC);
    ReleaseDC(nullptr, screenDC);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_COMMAND: {
            int id = LOWORD(wp);
            int mi = (id >> 8) & 0xFF;
            // 动作按钮
            if (id >= (int)mainwin::ActionSettings && id <= (int)mainwin::ActionExit) {
                if (g_host)
                    PostMessageW(g_host, mainwin::kActionMsg, (WPARAM)id, 0);
                return 0;
            }
            // 输入源按钮
            if ((id & 0xFF0000) == kIdInputBase && mi < (int)g_cards.size()) {
                int value = id & 0xFF;
                ddc::Ddc::SetVCP(g_cards[mi].deviceName, ddc::kVcpInputSource, (DWORD)value);
                return 0;
            }
            // 电源按钮
            if ((id & 0xFF0000) == kIdPowerBase && mi < (int)g_cards.size()) {
                ddc::Ddc::SetVCP(g_cards[mi].deviceName, ddc::kVcpPower, 0x01);   // 关闭
                return 0;
            }
            break;
        }
        case WM_HSCROLL: {
            HWND src = (HWND)lp;
            int id = GetDlgCtrlID(src);
            int mi = (id >> 8) & 0xFF;
            int code = id & 0xFF;
            if ((id & 0xFF0000) == kIdTrackBase && mi < (int)g_cards.size()) {
                auto& card = g_cards[mi];
                auto it = std::find_if(card.sliders.begin(), card.sliders.end(),
                                       [&](const SliderCtl& s) { return s.track == src; });
                if (it != card.sliders.end()) {
                    int pos = (int)SendMessageW(src, TBM_GETPOS, 0, 0);
                    int pct = std::clamp(pos / 10, 0, 100);
                    wchar_t buf[16] = {};
                    swprintf_s(buf, L"%d%%", pct);
                    SetWindowTextW(it->label, buf);
                    if (LOWORD(wp) == TB_THUMBTRACK)
                        card.dragging = true;
                    if (LOWORD(wp) == TB_ENDTRACK) {
                        card.dragging = false;
                        ddc::Ddc::SetVCP(card.deviceName, (BYTE)code, (DWORD)pct);
                    }
                }
            }
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            DrawWindow(hwnd);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;   // 自绘背景
        case WM_CLOSE:
            ShowWindow(hwnd, SW_HIDE);
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

    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_BAR_CLASSES };
    InitCommonControlsEx(&icc);

    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof(ncm);
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0))
        g_uiFont = CreateFontIndirectW(&ncm.lfMessageFont);

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = kMainWinClass;
    RegisterClassW(&wc);

    g_hwnd = CreateWindowExW(0, kMainWinClass, L"机械革命监控 - 显示器控制",
                             WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                             CW_USEDEFAULT, CW_USEDEFAULT, kClientW + 16, 320,
                             nullptr, nullptr, hInst, nullptr);
    if (!g_hwnd)
        return false;
    RebuildPanel();
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
        RebuildPanel();   // 打开时刷新显示器列表
        return;
    }
    RECT wa{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    RECT wr{};
    GetWindowRect(g_hwnd, &wr);
    int x = wa.left + ((wa.right - wa.left) - (wr.right - wr.left)) / 2;
    int y = wa.top + ((wa.bottom - wa.top) - (wr.bottom - wr.top)) / 2;
    SetWindowPos(g_hwnd, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
    ShowWindow(g_hwnd, SW_SHOW);
    SetForegroundWindow(g_hwnd);
    RebuildPanel();
}

void Hide() {
    if (g_hwnd)
        ShowWindow(g_hwnd, SW_HIDE);
}

bool IsVisible() {
    return g_hwnd && IsWindowVisible(g_hwnd);
}

HWND Hwnd() { return g_hwnd; }

void Refresh() {
    if (!g_hwnd || !IsWindowVisible(g_hwnd))
        return;
    RefreshValues();
}

void Rebuild() {
    if (g_hwnd)
        RebuildPanel();
}

void DestroyWindowW() {
    if (g_hwnd) {
        DestroyWindow(g_hwnd);
        g_hwnd = nullptr;
    }
    if (g_uiFont) {
        DeleteObject(g_uiFont);
        g_uiFont = nullptr;
    }
    if (g_gdiToken) {
        Gdiplus::GdiplusShutdown((ULONG_PTR)g_gdiToken);
        g_gdiToken = 0;
    }
}

}  // namespace mainwin
