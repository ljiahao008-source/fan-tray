#pragma once
// 任务栏嵌入渲染窗口：SetParent 嵌入 Shell_TrayWnd，GDI+ 真透明渲染
// 功能对照 TrayWidget.cs：实时数据 + 状态色 + 悬停统计卡片 + 自动避让 + 主题适配
// 显示项：功耗 / 风扇 / CPU 占用 / CPU 温度 / 内存 / 网速（可用配置勾选，单行排列）

#include <windows.h>
#include <gdiplus.h>
#include <vector>
#include <string>
#include "monitor.h"
#include "config.h"

// 任务栏显示项（枚举顺序 = 从左到右排列顺序）
enum class ItemKind { Power = 0, Fan, CpuUsage, CpuTemp, Mem, Net, Count };
constexpr int kItemCount = (int)ItemKind::Count;

class Widget {
public:
    Widget() = default;
    ~Widget() { Dispose(); }

    bool Create(HINSTANCE hInst);            // 注册窗口类 + 创建窗口
    bool Embed();                            // 嵌入任务栏（explorer 重启后重调）
    void Update(const SampleSet& s);         // 采样快照（每秒调用）
    void ApplyConfig(const AppConfig& cfg);  // 显示项开关（运行时可变）
    void SetThresholds(const Thresholds& t) { _thr = t; }
    void ApplyTheme(bool light);
    bool IsAlive() const { return _hwnd != nullptr && IsWindow(_hwnd) != FALSE; }
    void Dispose();

private:
    struct Item {
        bool visible = true;
        int blockW = 40;      // 定宽（按最坏值样本预测量，防位数进位导致窗口抽动）
        int x = 0;            // 内容区左缘（未乘 DPI）
    };

    void Render();                           // GDI+ 画帧 → UpdateLayeredWindow
    void Position();
    int HitTest(POINT pt) const;             // 命中项索引，-1 = 无
    void ShowTip(POINT pt);
    void HideTip();
    void ScanForeignWidgets(int boundaryRight, int tbTop, int tbBottom);
    Gdiplus::Color ValueColor(float v, double elevated, double critical) const;
    Gdiplus::Color ItemColor(int idx) const; // 按项取状态色
    static void FormatValue(int idx, const Metric& m, float netUp, wchar_t* buf, size_t cch);
    static const wchar_t* ItemLabel(int idx);
    static const wchar_t* ItemTitle(int idx);
    static const wchar_t* ItemUnit(int idx);
    void MeasureBlocks(HDC screenDC);        // 按最坏样本一次性定块宽

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
    bool _hovering = false;
    int _hoverIdx = -1;                      // 悬停的显示项

    // 数据
    Metric _metrics[kItemCount];             // 各显示项快照（Net 项用 netDown，另存 netUp）
    float _netUp = 0.f;
    bool _netUpValid = false;
    Thresholds _thr;
    bool _hasData = false;
    int _lastX = 0, _lastW = 0, _lastH = 0;
    std::vector<float> _hist[kItemCount];    // 60 拍滚动历史（悬浮窗趋势图）

    // 布局（px，96 DPI 基准，Position 时按 DPI 缩放）
    Item _items[kItemCount];
    bool _blocksMeasured = false;
    int _contentW = 130;

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
