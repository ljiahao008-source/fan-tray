// mainwindow.cpp —— 全功能面板（Twinkle Tray 原版外观，GDI+ 全自绘）
// 覆盖：硬件监控 + 每台显示器（亮度/对比度/音量滑块、输入源、电源）
//     + 色温护眼 + 底部操作。滑块拖动松手写 DDC/CI，每秒刷新。
// 布局一致性：RebuildPanel 与 DrawPanel 用同一套"行式"坐标计算，
//           命中矩形与实际绘制位置严格对齐。

#include "mainwindow.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <vector>

#include <windowsx.h>
#include <gdiplus.h>

#pragma comment(lib, "gdiplus.lib")

#include "ddc.h"

using Gdiplus::Color;
using Gdiplus::Font;
using Gdiplus::FontFamily;
using Gdiplus::Graphics;
using Gdiplus::GraphicsPath;
using Gdiplus::Pen;
using Gdiplus::PointF;
using Gdiplus::RectF;
using Gdiplus::SolidBrush;
using Gdiplus::StringFormat;
using Gdiplus::StringAlignment;
using Gdiplus::TextRenderingHint;
using Gdiplus::UnitPixel;

namespace {

constexpr wchar_t kMainWinClass[] = L"MechrevoMainWindowClass";
constexpr int kPanelW = 380;
constexpr int kPad = 16;
constexpr int kMetricsH = 150;      // 监控区块高度
constexpr int kCardHead = 34;       // 卡片标题区高度
constexpr int kRowH = 28;           // 每行（滑块/按钮）高度
constexpr int kCtH = 58;            // 色温区块高度
constexpr int kFooterH = 50;        // 底部按钮区

// 色板（Twinkle Tray 深色风格）
const Color kBg(255, 0x14, 0x17, 0x1E);
const Color kSection(255, 0x1E, 0x23, 0x2E);
const Color kBorder(255, 0x2E, 0x35, 0x45);
const Color kText(255, 0xEC, 0xEF, 0xF6);
const Color kSub(255, 0x8A, 0x93, 0xA6);
const Color kAccent(255, 0x4A, 0xDE, 0x80);
const Color kAccentDim(255, 0x2E, 0x41, 0x38);

HWND g_hwnd = nullptr;
HINSTANCE g_hInst = nullptr;
HWND g_host = nullptr;
ULONG_PTR g_gdiToken = 0;
bool g_rebuilding = false;

SampleSet g_snap;
bool g_ctEnabled = false;
int g_ctKelvin = 6500;
int g_panelH = 320;

// ── 交互元素 ────────────────────────────────────────────────
enum BtnKind { kBtnAction = 0, kBtnInput = 1, kBtnPower = 2 };

struct Slider {
    RECT rc;
    int monitorIdx = -1;
    BYTE code = 0;
    int pct = 0;
};

struct PanelBtn {
    RECT rc;
    int kind = kBtnAction;
    UINT action = 0;
    int value = 0;
    wchar_t text[32] = {};
};

struct MonitorCard {
    std::wstring deviceName;
    wchar_t title[128] = {};
    std::vector<Slider> sliders;
    std::vector<PanelBtn> buttons;
    int rows = 0;          // 行数（滑块+按钮）
    int height = 0;        // 卡片总高
};

std::vector<MonitorCard> g_cards;
std::vector<PanelBtn> g_footer;
PanelBtn g_ctPlus, g_ctMinus, g_ctToggle;

bool g_dragging = false;
int g_dragCard = -1, g_dragSlider = -1;

// ── GDI+ 辅助 ───────────────────────────────────────────────
void RoundedRect(GraphicsPath& p, float x, float y, float w, float h, float r) {
    r = std::min(r, std::min(w, h) / 2);
    p.Reset();
    p.AddArc(x, y, r * 2, r * 2, 180, 90);
    p.AddArc(x + w - r * 2, y, r * 2, r * 2, 270, 90);
    p.AddArc(x + w - r * 2, y + h - r * 2, r * 2, r * 2, 0, 90);
    p.AddArc(x, y + h - r * 2, r * 2, r * 2, 90, 90);
    p.CloseFigure();
}

void DrawStr(Graphics& g, const wchar_t* s, const Font& f, float x, float y, const Color& c) {
    SolidBrush b(c);
    g.DrawString(s, -1, &f, PointF(x, y), &b);
}

void DrawButton(Graphics& g, const PanelBtn& b) {
    GraphicsPath p;
    RoundedRect(p, (float)b.rc.left, (float)b.rc.top,
                (float)(b.rc.right - b.rc.left), (float)(b.rc.bottom - b.rc.top), 6.f);
    SolidBrush bg(kAccentDim);
    g.FillPath(&bg, &p);
    Pen pen(kBorder, 1.f);
    g.DrawPath(&pen, &p);
    FontFamily ff(L"Microsoft YaHei UI");
    Font f(&ff, 12.f, Gdiplus::FontStyleRegular, UnitPixel);
    SolidBrush tb(kText);
    StringFormat sf;
    sf.SetAlignment(Gdiplus::StringAlignmentCenter);
    sf.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    RectF r((float)b.rc.left, (float)b.rc.top, (float)(b.rc.right - b.rc.left),
            (float)(b.rc.bottom - b.rc.top));
    g.DrawString(b.text, -1, &f, r, &sf, &tb);
}

void DrawSliderBar(Graphics& g, const Slider& s) {
    int yc = (s.rc.top + s.rc.bottom) / 2;
    int x0 = s.rc.left, x1 = s.rc.right;
    int trackH = 6;
    GraphicsPath track;
    RoundedRect(track, (float)x0, (float)(yc - trackH / 2), (float)(x1 - x0), (float)trackH, 3.f);
    SolidBrush tb(kBorder);
    g.FillPath(&tb, &track);
    float fx = (float)(x0 + (x1 - x0) * std::clamp(s.pct, 0, 100) / 100);
    if (fx > x0) {
        GraphicsPath fill;
        RoundedRect(fill, (float)x0, (float)(yc - trackH / 2), fx - x0, (float)trackH, 3.f);
        SolidBrush fb(kAccent);
        g.FillPath(&fb, &fill);
    }
    SolidBrush thumb(Color(255, 0xFF, 0xFF, 0xFF));
    g.FillEllipse(&thumb, fx - 7.f, (float)(yc - 7), 14.f, 14.f);
}

bool PtInRect(const RECT& r, int x, int y) {
    return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
}

bool HitSlider(int x, int y, int& cardIdx, int& sliderIdx) {
    for (size_t c = 0; c < g_cards.size(); c++)
        for (size_t s = 0; s < g_cards[c].sliders.size(); s++)
            if (PtInRect(g_cards[c].sliders[s].rc, x, y)) {
                cardIdx = (int)c;
                sliderIdx = (int)s;
                return true;
            }
    return false;
}

bool HitButton(int x, int y, PanelBtn** out) {
    for (auto& c : g_cards)
        for (auto& b : c.buttons)
            if (PtInRect(b.rc, x, y)) { *out = &b; return true; }
    for (auto& b : g_footer)
        if (PtInRect(b.rc, x, y)) { *out = &b; return true; }
    if (PtInRect(g_ctPlus.rc, x, y)) { *out = &g_ctPlus; return true; }
    if (PtInRect(g_ctMinus.rc, x, y)) { *out = &g_ctMinus; return true; }
    if (PtInRect(g_ctToggle.rc, x, y)) { *out = &g_ctToggle; return true; }
    return false;
}

// ── 布局（显示器集合变化时重建；与 Draw 区坐标一致）────────────
void RebuildPanel() {
    g_rebuilding = true;
    g_cards.clear();
    g_footer.clear();

    std::vector<ddc::Monitor> monitors = ddc::Ddc::Enumerate();

    int y = kPad + kCardHead;              // 监控区块顶部
    y += kMetricsH + 8;

    for (auto& mon : monitors) {
        MonitorCard card;
        card.deviceName = mon.deviceName;
        wcsncpy_s(card.title, mon.description.empty() ? L"显示器" : mon.description.c_str(), _TRUNCATE);

        int row = 0;
        auto rowY = [&](int r) { return y + kCardHead + 6 + r * kRowH; };

        auto addSlider = [&](BYTE code, DWORD cur, DWORD max) {
            Slider s;
            s.monitorIdx = (int)g_cards.size();
            s.code = code;
            s.pct = max > 0 ? (int)std::lround(cur * 100.0 / max) : 0;
            int cy = rowY(row);
            s.rc = { kPad + 96, cy - 6, kPanelW - kPad - 14, cy + 6 };
            card.sliders.push_back(s);
            row++;
        };
        if (mon.hasBrightness) addSlider(ddc::kVcpLuminance, mon.brightnessCur, mon.brightnessMax);
        if (mon.hasContrast)   addSlider(ddc::kVcpContrast, mon.contrastCur, mon.contrastMax);
        if (mon.hasVolume)     addSlider(ddc::kVcpAudioVolume, mon.volumeCur, mon.volumeMax);

        if (mon.hasInputs && !mon.inputs.empty()) {
            int cy = rowY(row);
            int bx = kPad + 96;
            for (DWORD v : mon.inputs) {
                PanelBtn b;
                b.kind = kBtnInput;
                b.value = (int)v;
                swprintf_s(b.text, L"%lu", v);
                b.rc = { bx, cy - 11, bx + 34, cy + 11 };
                card.buttons.push_back(b);
                bx += 40;
            }
            row++;
        }
        if (mon.hasPower) {
            int cy = rowY(row);
            PanelBtn b;
            b.kind = kBtnPower;
            wcscpy_s(b.text, L"电源");
            b.rc = { kPad + 96, cy - 12, kPad + 160, cy + 12 };
            card.buttons.push_back(b);
            row++;
        }

        card.rows = row;
        card.height = kCardHead + 6 + row * kRowH + 8;
        g_cards.push_back(std::move(card));
        y += card.height + 8;
    }

    if (monitors.empty()) {
        y += kCardHead + 26;
    }

    // 色温区块
    int cy = y + 30;
    g_ctPlus.rc = { kPad + 140, cy - 11, kPad + 196, cy + 11 };
    g_ctMinus.rc = { kPad + 202, cy - 11, kPad + 258, cy + 11 };
    g_ctToggle.rc = { kPad + 264, cy - 11, kPanelW - kPad - 14, cy + 11 };
    wcscpy_s(g_ctPlus.text, L"+500K");
    wcscpy_s(g_ctMinus.text, L"-500K");
    wcscpy_s(g_ctToggle.text, L"暂停/恢复");
    g_ctPlus.action = mainwin::ActionTempWarm;
    g_ctMinus.action = mainwin::ActionTempCool;
    g_ctToggle.action = mainwin::ActionTempToggle;
    y += kCtH + 8;

    // 底部操作
    int fy = y + 25;
    struct F { int id; const wchar_t* text; int x, w; };
    F fs[] = {
        { (int)mainwin::ActionSettings, L"设置", kPad, 66 },
        { (int)mainwin::ActionLockScreen, L"锁屏设置", kPad + 74, 86 },
        { (int)mainwin::ActionReset, L"重置统计", kPad + 168, 86 },
        { (int)mainwin::ActionExit, L"退出", kPad + 262, 44 },
    };
    for (auto& f : fs) {
        PanelBtn b;
        b.kind = kBtnAction;
        b.action = f.id;
        wcscpy_s(b.text, f.text);
        b.rc = { f.x, fy - 15, f.x + f.w, fy + 15 };
        g_footer.push_back(b);
    }
    y += kFooterH + 8;
    g_panelH = y;

    if (g_hwnd) {
        RECT rc{};
        GetClientRect(g_hwnd, &rc);
        SetWindowPos(g_hwnd, nullptr, 0, 0, kPanelW + 16, g_panelH + 39, SWP_NOMOVE | SWP_NOACTIVATE);
    }
    g_rebuilding = false;
}

// ── 绘制 ─────────────────────────────────────────────────────
void DrawPanel(Graphics& g) {
    FontFamily ffLbl(L"Microsoft YaHei UI");
    Font secFont(&ffLbl, 12.f, Gdiplus::FontStyleBold, UnitPixel);
    Font lblFont(&ffLbl, 12.f, Gdiplus::FontStyleRegular, UnitPixel);
    FontFamily ffVal(L"Segoe UI");
    Font valFont(&ffVal, 14.f, Gdiplus::FontStyleBold, UnitPixel);
    Font bigVal(&ffVal, 16.f, Gdiplus::FontStyleBold, UnitPixel);

    SolidBrush bg(kBg);
    g.FillRectangle(&bg, 0.f, 0.f, (float)kPanelW, (float)g_panelH);

    auto sev = [](double v, double e, double c) {
        return v <= e ? Color(255, 0x4A, 0xDE, 0x80)
                      : v <= c ? Color(255, 0xFB, 0x92, 0x3C)
                               : Color(255, 0xF8, 0x71, 0x71);
    };
    auto fmt = [](const Metric& m, const wchar_t* f, wchar_t* out, size_t n) {
        if (!m.valid) wcscpy_s(out, n, L"--");
        else swprintf_s(out, n, f, (double)m.current);
    };

    int y = kPad;

    // ── 硬件监控 ──
    {
        GraphicsPath card;
        RoundedRect(card, (float)kPad, (float)y, (float)(kPanelW - kPad * 2), (float)kMetricsH, 8.f);
        SolidBrush bgc(kSection);
        g.FillPath(&bgc, &card);
        Pen cpb(kBorder, 1.f);
        g.DrawPath(&cpb, &card);
        SolidBrush bar(kAccent);
        g.FillRectangle(&bar, (float)kPad + 14, (float)y + 9, 3.f, 12.f);
        DrawStr(g, L"硬件监控", secFont, (float)kPad + 24, (float)y + 6, kText);

        wchar_t b[2][24];
        int gy = y + 34;
        int gx0 = kPad + 26, gx1 = kPad + 176;
        auto cell = [&](int cx, int cy, const wchar_t* name, const wchar_t* val, const Color& c) {
            DrawStr(g, name, lblFont, (float)cx, (float)cy, kSub);
            DrawStr(g, val, valFont, (float)cx, (float)cy + 18, c);
        };
        fmt(g_snap.power, L"%.1f W", b[0], 24);
        cell(gx0, gy, L"功耗", b[0], sev(g_snap.power.current, 45, 72));
        fmt(g_snap.fan, L"%.0f RPM", b[1], 24);
        cell(gx1, gy, L"风扇", b[1], sev(g_snap.fan.current, 3200, 4800));
        gy += 34;
        fmt(g_snap.cpuUsage, L"%.0f%%", b[0], 24);
        cell(gx0, gy, L"CPU 占用", b[0], sev(g_snap.cpuUsage.current, 70, 90));
        fmt(g_snap.cpuTemp, L"%.0f°C", b[1], 24);
        cell(gx1, gy, L"CPU 温度", b[1], sev(g_snap.cpuTemp.current, 75, 90));
        gy += 34;
        fmt(g_snap.mem, L"%.0f%%", b[0], 24);
        cell(gx0, gy, L"内存", b[0], sev(g_snap.mem.current, 70, 90));
        wchar_t d[12] = {}, u[12] = {};
        fmt(g_snap.netDown, L"%.0f", d, 12);
        fmt(g_snap.netUp, L"%.0f", u, 12);
        swprintf_s(b[1], L"↓%s ↑%s KB/s", d, u);
        cell(gx1, gy, L"网速", b[1], Color(255, 0xFF, 0xFF, 0xFF));
        y += kMetricsH + 8;
    }

    // ── 显示器卡片 ──
    for (auto& card : g_cards) {
        GraphicsPath cp;
        RoundedRect(cp, (float)kPad, (float)y, (float)(kPanelW - kPad * 2), (float)card.height, 8.f);
        SolidBrush bgc(kSection);
        g.FillPath(&bgc, &cp);
        Pen cpb(kBorder, 1.f);
        g.DrawPath(&cpb, &cp);
        SolidBrush bar(kAccent);
        g.FillRectangle(&bar, (float)kPad + 14, (float)y + 9, 3.f, 12.f);
        DrawStr(g, card.title, bigVal, (float)kPad + 24, (float)y + 5, kText);

        int row = 0;
        auto rowY = [&](int r) { return y + kCardHead + 6 + r * kRowH; };
        for (auto& s : card.sliders) {
            int cy = rowY(row);
            const wchar_t* name = s.code == ddc::kVcpLuminance ? L"亮度" :
                                  s.code == ddc::kVcpContrast ? L"对比度" : L"音量";
            DrawStr(g, name, lblFont, (float)(kPad + 18), (float)(cy - 8), kSub);
            DrawSliderBar(g, s);
            wchar_t pbuf[16] = {};
            swprintf_s(pbuf, L"%d%%", s.pct);
            DrawStr(g, pbuf, valFont, (float)(kPanelW - kPad - 44), (float)(cy - 9), kText);
            row++;
        }
        for (auto& b : card.buttons) {
            int cy = rowY(row);
            DrawStr(g, b.kind == kBtnInput ? L"输入源" : L"电源", lblFont,
                    (float)(kPad + 18), (float)(cy - 8), kSub);
            DrawButton(g, b);
            row++;
        }
        y += card.height + 8;
    }

    // ── 色温 ──
    {
        GraphicsPath card;
        RoundedRect(card, (float)kPad, (float)y, (float)(kPanelW - kPad * 2), (float)kCtH, 8.f);
        SolidBrush bgc(kSection);
        g.FillPath(&bgc, &card);
        Pen cpb(kBorder, 1.f);
        g.DrawPath(&cpb, &card);
        SolidBrush bar(kAccent);
        g.FillRectangle(&bar, (float)kPad + 14, (float)y + 9, 3.f, 12.f);
        DrawStr(g, L"色温护眼", secFont, (float)kPad + 24, (float)y + 6, kText);

        wchar_t ct[64] = {};
        if (g_ctEnabled)
            swprintf_s(ct, L"已启用 · %dK", g_ctKelvin);
        else
            wcscpy_s(ct, L"未启用（设置中开启）");
        DrawStr(g, ct, lblFont, (float)kPad + 18, (float)(y + 32), kSub);
        DrawButton(g, g_ctPlus);
        DrawButton(g, g_ctMinus);
        DrawButton(g, g_ctToggle);
        y += kCtH + 8;
    }

    // ── 底部操作 ──
    for (auto& b : g_footer)
        DrawButton(g, b);
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
        DrawPanel(g);
    }
    HDC wdc = GetDC(hwnd);
    BitBlt(wdc, 0, 0, w, h, memDC, 0, 0, SRCCOPY);
    ReleaseDC(hwnd, wdc);
    SelectObject(memDC, old);
    DeleteObject(dib);
    DeleteDC(memDC);
    ReleaseDC(nullptr, screenDC);
}

// ── 交互 ─────────────────────────────────────────────────────
void PostAction(UINT action) {
    if (g_host)
        PostMessageW(g_host, mainwin::kActionMsg, (WPARAM)action, 0);
}

void OnButton(PanelBtn* b) {
    if (!b)
        return;
    if (b->kind == kBtnAction) {
        PostAction(b->action);
    } else if (b->kind == kBtnInput) {
        for (auto& card : g_cards)
            for (auto& cb : card.buttons)
                if (&cb == b)
                    ddc::Ddc::SetVCP(card.deviceName, ddc::kVcpInputSource, (DWORD)b->value);
    } else if (b->kind == kBtnPower) {
        for (auto& card : g_cards)
            for (auto& cb : card.buttons)
                if (&cb == b)
                    ddc::Ddc::SetVCP(card.deviceName, ddc::kVcpPower, 0x01);
    }
}

void ApplySlider(int cardIdx, int sliderIdx) {
    if (cardIdx < 0 || cardIdx >= (int)g_cards.size())
        return;
    auto& card = g_cards[cardIdx];
    if (sliderIdx < 0 || sliderIdx >= (int)card.sliders.size())
        return;
    auto& s = card.sliders[sliderIdx];
    // 亮度/对比度走百分比换算（兼容 max≠100 的机型），音量按原始值写
    if (s.code == ddc::kVcpLuminance)
        ddc::Ddc::SetBrightnessPercent(card.deviceName, s.pct);
    else if (s.code == ddc::kVcpContrast)
        ddc::Ddc::SetContrastPercent(card.deviceName, s.pct);
    else
        ddc::Ddc::SetVCP(card.deviceName, s.code, (DWORD)s.pct);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_LBUTTONDOWN: {
            int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
            PanelBtn* btn = nullptr;
            if (HitSlider(x, y, g_dragCard, g_dragSlider)) {
                g_dragging = true;
                SetCapture(hwnd);
                auto& s = g_cards[g_dragCard].sliders[g_dragSlider];
                int sw = (int)(s.rc.right - s.rc.left);
                s.pct = std::clamp((int)((x - s.rc.left) * 100) / std::max(1, sw), 0, 100);
                InvalidateRect(hwnd, nullptr, FALSE);
            } else if (HitButton(x, y, &btn)) {
                OnButton(btn);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_MOUSEMOVE: {
            if (g_dragging && g_dragCard >= 0 && g_dragSlider >= 0) {
                int x = GET_X_LPARAM(lp);
                auto& s = g_cards[g_dragCard].sliders[g_dragSlider];
                int sw = (int)(s.rc.right - s.rc.left);
                int pct = std::clamp((int)((x - s.rc.left) * 100) / std::max(1, sw), 0, 100);
                if (pct != s.pct) {
                    s.pct = pct;
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
            }
            return 0;
        }
        case WM_LBUTTONUP: {
            if (g_dragging) {
                ApplySlider(g_dragCard, g_dragSlider);
                g_dragging = false;
                g_dragCard = g_dragSlider = -1;
                ReleaseCapture();
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
            return 1;
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

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = kMainWinClass;
    RegisterClassW(&wc);

    g_hwnd = CreateWindowExW(0, kMainWinClass, L"机械革命监控",
                             WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                             CW_USEDEFAULT, CW_USEDEFAULT, kPanelW + 16, 420,
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
        RebuildPanel();
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

void Refresh(const SampleSet& s) {
    if (!g_hwnd)
        return;
    g_snap = s;   // 始终保存最新数据；仅可见时重绘与读 DDC
    if (!IsWindowVisible(g_hwnd))
        return;
    // 显示器数值刷新（滑块未拖动时）
    if (!g_dragging) {
        std::vector<ddc::Monitor> ms = ddc::Ddc::Enumerate();
        for (size_t c = 0; c < g_cards.size() && c < ms.size(); c++) {
            for (auto& sl : g_cards[c].sliders) {
                DWORD cur = 0, max = 0;
                if (!ddc::Ddc::GetVCP(g_cards[c].deviceName, sl.code, cur, max))
                    continue;
                if (max == 0) max = 100;
                sl.pct = (int)std::lround(cur * 100.0 / max);
            }
        }
    }
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

// 色温状态注入（宿主 kSampleMsg 调用，查询 colortemp 后传入）
void SetColorTemp(bool enabled, int kelvin) {
    g_ctEnabled = enabled;
    g_ctKelvin = kelvin;
    if (g_hwnd && IsWindowVisible(g_hwnd))
        InvalidateRect(g_hwnd, nullptr, FALSE);
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
    if (g_gdiToken) {
        Gdiplus::GdiplusShutdown((ULONG_PTR)g_gdiToken);
        g_gdiToken = 0;
    }
}

}  // namespace mainwin
