#pragma once
// 任务栏嵌入渲染窗口：SetParent 嵌入 Shell_TrayWnd，GDI+ 真透明渲染
// 功能对照 TrayWidget.cs：实时数据 + 状态色 + 悬停统计卡片 + 自动避让 + 主题适配

#include <windows.h>
#include <gdiplus.h>
#include <vector>
#include "monitor.h"

class Widget {
public:
    Widget() = default;
    ~Widget() { Dispose(); }

    bool Create(HINSTANCE hInst);            // 注册窗口类 + 创建窗口
    bool Embed();                            // 嵌入任务栏（explorer 重启后重调）
    void Update(const Metric& power, const Metric& fan);
    void SetThresholds(const Thresholds& t) { _thr = t; }
    void ApplyTheme(bool light);
    bool IsAlive() const { return _hwnd != nullptr && IsWindow(_hwnd) != FALSE; }
    void Dispose();

private:
    void Render();                           // GDI+ 画帧 → UpdateLayeredWindow
    void Position();
    bool HitPower(POINT pt) const;           // 命中功耗热区
    bool HitFan(POINT pt) const;
    void ShowTip(POINT pt);                  // 显示/更新悬停卡片
    void HideTip();
    void ScanForeignWidgets(int boundaryRight, int tbTop, int tbBottom);
    Gdiplus::Color ValueColor(float v, double elevated, double critical) const;

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    static LRESULT CALLBACK TipWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    static void RegisterTipClass(HINSTANCE hInst);
    static BOOL CALLBACK EnumForeign(HWND hwnd, LPARAM lp);
    LRESULT OnMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    HWND _hwnd = nullptr;
    HWND _tipHwnd = nullptr;                 // 悬停卡片窗口
    HINSTANCE _hInst = nullptr;
    HWND _taskbar = nullptr;
    UINT _taskbarCreatedMsg = 0;
    bool _embedded = false;
    bool _shown = false;
    bool _hovering = false;                  // 是否正在悬停
    bool _hoverPower = false;                // 悬停的是功耗块还是风扇块

    // 数据
    Metric _power, _fan;
    Thresholds _thr;
    bool _hasData = false;
    int _lastX = 0, _lastW = 0, _lastH = 0;

    // 布局（px，96 DPI 基准，Position 时按 DPI 缩放）
    int _powerBlockW = 60;
    int _fanBlockW = 60;
    int _contentW = 130;
    int _relX = 0, _relY = 0;   // 窗口相对 taskbar 的坐标（Position 设置，Render 转屏幕给 UpdateLayeredWindow）

    // 主题
    Gdiplus::Color _sevGreen{}, _sevOrange{}, _sevRed{}, _labelColor{};
    Gdiplus::Color _hoverBack{};

    // 避让
    struct RectW { int left, top, right, bottom; };
    std::vector<RectW> _foreign;
    int _foreignScanCounter = 0;
    RectW _lastTrayRect{};
    bool _hasTrayRect = false;
    int _scanBoundary = 0, _scanTop = 0, _scanBottom = 0;   // EnumForeign 临时参数
    ULONG_PTR _gdiToken = 0;
};
