using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Documents;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Threading;
using Brushes = System.Windows.Media.Brushes;
using Brush = System.Windows.Media.Brush;
using Color = System.Windows.Media.Color;
using FontFamily = System.Windows.Media.FontFamily;
using HorizontalAlignment = System.Windows.HorizontalAlignment;
using Orientation = System.Windows.Controls.Orientation;
using VerticalAlignment = System.Windows.VerticalAlignment;

namespace MonitoringApp.Tray;

/// <summary>
/// 托盘渲染窗口：把无边框透明 WPF 窗口 SetParent 为 Shell_TrayWnd 的子窗口，
/// 定位在 TrayNotifyWnd（托盘角落）左侧，实时渲染两项监控数据（功耗 / 风扇转速）。
/// 嵌入方案与原版任务栏小组件、开源 TrafficMonitor 一致；explorer 重启后自动重嵌。
/// </summary>
public sealed class TrayWidget : Window
{
    private const int GWL_STYLE = -16;
    private const long WS_CHILD = 0x40000000L;
    private const long WS_VISIBLE = 0x10000000L;
    private const uint SWP_NOMOVE = 0x0002;
    private const uint SWP_NOSIZE = 0x0001;
    private const uint SWP_NOACTIVATE = 0x0010;
    private const uint SWP_SHOWWINDOW = 0x0040;

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern IntPtr FindWindow(string? cls, string? title);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern IntPtr FindWindowEx(IntPtr parent, IntPtr after, string? cls, string? title);

    [DllImport("user32.dll")]
    private static extern IntPtr SetParent(IntPtr child, IntPtr parent);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern int GetWindowLong(IntPtr hwnd, int index);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern int SetWindowLong(IntPtr hwnd, int index, int newStyle);

    [DllImport("user32.dll")]
    private static extern bool GetWindowRect(IntPtr hwnd, out RECT rect);

    [DllImport("user32.dll")]
    private static extern bool SetWindowPos(IntPtr hwnd, uint after, int x, int y, int cx, int cy, uint flags);

    [DllImport("user32.dll")]
    private static extern uint GetDpiForWindow(IntPtr hwnd);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern uint RegisterWindowMessage(string name);

    [DllImport("user32.dll")]
    private static extern bool IsWindow(IntPtr hwnd);

    [DllImport("user32.dll")]
    private static extern bool EnumChildWindows(IntPtr parent, EnumChildProc callback, IntPtr lParam);

    [DllImport("user32.dll")]
    private static extern bool EnumWindows(EnumChildProc callback, IntPtr lParam);

    [DllImport("user32.dll")]
    private static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint processId);

    [DllImport("user32.dll")]
    private static extern bool IsWindowVisible(IntPtr hwnd);

    private delegate bool EnumChildProc(IntPtr hwnd, IntPtr lParam);

    [StructLayout(LayoutKind.Sequential)]
    private struct RECT { public int Left, Top, Right, Bottom; }

    /// <summary>数值+中文标签的文本指标（数值在上、中文标签在下），并持有自己的悬停统计卡片。</summary>
    private sealed class Pair
    {
        // Border 自带 Padding：给悬停热区留余量用（Grid 没有 Padding 属性）
        public Border Host { get; } = new();
        public TextBlock Value { get; } = new();
        public TextBlock Label { get; } = new();

        // 悬停卡片（深色圆角 Popup）：标题固定，卡片构建时写入；数值每次 Update 刷新
        // 标题/数值/单位做成同一 TextBlock 的三个 Run —— 共享基线，字号不同也能对齐
        public Popup TipPopup { get; } = new();
        public StackPanel TipCard { get; } = new();
        // 标题单独一个 TextBlock（负底边距压到底线）；数值+单位同一 TextBlock（同为 Segoe，基线天然齐）
        public TextBlock TipTitle { get; } = new();
        public TextBlock TipValue { get; } = new();
        public Run TipValueRun { get; } = new("--");
        public Run TipUnitRun { get; } = new();
        public TextBlock TipMin { get; } = new();
        public TextBlock TipMax { get; } = new();
        public TextBlock TipAvg { get; } = new();

        /// <summary>最近一次的原始数值（用于状态色判断；null=无数据）。</summary>
        public float? LatestValue;
    }

    private readonly StackPanel _pairPanel = new() { Orientation = Orientation.Horizontal, VerticalAlignment = VerticalAlignment.Stretch };
    private readonly Pair _power = new();
    private readonly Pair _fan = new();
    private readonly Grid _root = new();
    private readonly DispatcherTimer _retry;
    private readonly DispatcherTimer _reposition;   // 低频回贴：任务栏布局变化（其他挂件退出等）后自动靠拢托盘角落
    private IntPtr _hwnd;
    private IntPtr _taskbar;
    private uint _taskbarCreatedMsg;
    private bool _embedded;
    private bool _shown;
    private bool _themeInit;
    private bool _lightTheme = true;
    private int _lastX, _lastW, _lastH;   // 防抖：与目标位置/尺寸比对，未变化则跳过 SetWindowPos
    private Brush _labelBrush = Brushes.Gray;
    private Brush _hoverBrush = Freeze(new SolidColorBrush(Color.FromArgb(0x66, 0xFF, 0xFF, 0xFF)));   // 悬停高亮（白色 40%）
    // 数值状态色：正常绿 / 偏高橙 / 超高红（深浅主题在 ApplyThemeCore 里重建）
    private Brush _sevGreen = Freeze(new SolidColorBrush(Color.FromRgb(0x4A, 0xDE, 0x80)));
    private Brush _sevOrange = Freeze(new SolidColorBrush(Color.FromRgb(0xFB, 0x92, 0x3C)));
    private Brush _sevRed = Freeze(new SolidColorBrush(Color.FromRgb(0xF8, 0x71, 0x71)));
    // 阈值由控制器按 CPU 配置设定（MonitorCore.GetThresholds 估算）
    private double _powerElevated = 45, _powerCritical = 72;
    private double _fanElevated = 3200, _fanCritical = 4800;

    /// <summary>设置状态色阈值（功耗 W / 风扇 RPM），控制器启动时按机器配置调用。</summary>
    public void SetThresholds(double powerElevated, double powerCritical, double fanElevated, double fanCritical)
    {
        _powerElevated = powerElevated;
        _powerCritical = powerCritical;
        _fanElevated = fanElevated;
        _fanCritical = fanCritical;
    }
    private List<(IntPtr Hwnd, RECT Rect)>? _foreignCache;   // 其他挂件扫描结果缓存（约 30 秒刷新）
    private int _foreignScanCounter;
    private RECT _lastTrayRectForScan;

    public TrayWidget()
    {
        WindowStyle = WindowStyle.None;
        ResizeMode = ResizeMode.NoResize;
        AllowsTransparency = true;
        Background = Brushes.Transparent;   // Transparent 笔刷可命中点击，null 会穿透
        ShowInTaskbar = false;
        ShowActivated = false;
        Width = 120;
        Height = 46;

        BuildLayout();

        // explorer 重启后 5 秒重试嵌入，直到成功
        _retry = new DispatcherTimer { Interval = TimeSpan.FromSeconds(5) };
        _retry.Tick += (_, _) => { if (!_embedded) Embed(); };

        // 每 5 秒校准一次位置：其他挂件退出/托盘布局变化后自动回贴。
        // Position() 内部有防抖（位置没变不 SetWindowPos），空闲时零开销、不打断悬停提示。
        _reposition = new DispatcherTimer { Interval = TimeSpan.FromSeconds(5) };
        _reposition.Tick += (_, _) => { if (_embedded) Position(); };

        // 不经 WPF Show() 创建句柄：后续完全用 Win32 控制嵌入与显隐
        new WindowInteropHelper(this).EnsureHandle();
        _hwnd = new WindowInteropHelper(this).Handle;
        _taskbarCreatedMsg = RegisterWindowMessage("TaskbarCreated");
        HwndSource.FromHwnd(_hwnd)?.AddHook(WndProc);
    }

    private void BuildLayout()
    {
        _root.HorizontalAlignment = HorizontalAlignment.Left;   // 内容靠左，ActualWidth 即真实内容宽度
        _root.Margin = new Thickness(2, 0, 0, 0);

        _pairPanel.Margin = new Thickness(0, 0, 0, 0);
        _root.Children.Add(_pairPanel);

        // 卡片固定深色底；数值颜色按状态动态判定（正常绿 / 偏高橙 / 超高红）
        _pairPanel.Children.Add(MakePair(_power, "功耗", "CPU 功耗", "W", rightMargin: 2).Host);
        // 文字间距 = 功耗右内边距5 + 边距2 + 风扇左内边距5 = 12px（6 太挤 / 24 太宽，取中）
        _pairPanel.Children.Add(MakePair(_fan, "风扇", "风扇转速", "RPM").Host);

        Content = _root;
        ApplyThemeCore(true);
        _pairPanel.Visibility = Visibility.Collapsed;
    }

    /// <summary>更新两项数据与各自悬停统计；未嵌入时会自动重试嵌入。指标为 null 表示读取失败。</summary>
    public void Update(Metric? power, Metric? fan)
    {
        if (!_embedded && !Embed())
            return;

        SetPair(_power, power, v => v?.ToString("0.0"), "W", _powerElevated, _powerCritical);
        SetPair(_fan, fan, v => v?.ToString("0"), "", _fanElevated, _fanCritical);

        // 首次有数据时恢复内容显示（初始 Collapsed），布局完成后重算宽度并定位
        if (_pairPanel.Visibility != Visibility.Visible)
        {
            _pairPanel.Visibility = Visibility.Visible;
            Dispatcher.BeginInvoke(DispatcherPriority.Loaded, () =>
            {
                double contentW = _root.ActualWidth > 10 ? Math.Ceiling(_root.ActualWidth) + 6 : Width;
                Width = contentW;
                Position();
            });
        }
    }

    /// <summary>刷新任务栏数值与悬停卡片（当前值按状态着色 + 最低/最高/平均三列）。
    /// mainSuffix 是任务栏数字上的单位（功耗带 W；风扇数字太长不带，卡片里有 RPM）。
    /// 资源优化：文本没变化就不赋值 —— WPF 依赖属性即使赋相同内容的新字符串实例
    /// 也会触发布局/渲染失效，每秒跳过一次无效重排，空闲时 CPU/GPU 占用显著下降。</summary>
    private void SetPair(Pair pair, Metric? m, Func<float?, string?> fmt, string mainSuffix, double elevated, double critical)
    {
        string current = m?.Current is { } v ? fmt(v) ?? "--" : "--";
        string taskbarText = current + mainSuffix;
        if (pair.Value.Text != taskbarText)
            pair.Value.Text = taskbarText;
        if (pair.TipValueRun.Text != current)
            pair.TipValueRun.Text = current;

        // 状态色：正常绿 / 偏高橙 / 超高红 / 无数据灰（同刷子实例引用赋值，WPF 自动短路不触发重排）
        pair.LatestValue = m?.Current;
        Brush valueBrush = ValueBrushFor(pair.LatestValue, elevated, critical);
        if (pair.Value.Foreground != valueBrush)
            pair.Value.Foreground = valueBrush;
        if (pair.TipValueRun.Foreground != valueBrush)
            pair.TipValueRun.Foreground = valueBrush;

        static string Stat(float? v, Func<float?, string?> fmt) => v is { } x ? fmt(x) ?? "--" : "--";

        string min = Stat(m?.Min, fmt);
        string max = Stat(m?.Max, fmt);
        string avg = Stat(m?.Average, fmt);
        if (pair.TipMin.Text != min)
            pair.TipMin.Text = min;
        if (pair.TipMax.Text != max)
            pair.TipMax.Text = max;
        if (pair.TipAvg.Text != avg)
            pair.TipAvg.Text = avg;
    }

    /// <summary>显示任务栏渲染组件（组件常驻，无隐藏入口）。</summary>
    public void ShowWidget()
    {
        _reposition.Start();
        if (!_embedded)
            Embed();
    }

    /// <summary>跟随系统主题切换配色（带防抖，每秒刷新调用无开销）。</summary>
    public void ApplyTheme(bool light)
    {
        if (_themeInit && _lightTheme == light)
            return;
        _lightTheme = light;
        _themeInit = true;
        ApplyThemeCore(light);
    }

    /// <summary>底层 Win32 窗口是否仍存活。explorer 崩溃时会连带销毁嵌入的 WS_CHILD 子窗口，
    /// 此时进程还活着但窗口已死，控制器据此重建挂件（自愈）。</summary>
    public bool IsWindowAlive => _hwnd != IntPtr.Zero && IsWindow(_hwnd);

    /// <summary>退出前解除任务栏父子关系，避免销毁顺序问题。</summary>
    public void Dispose()
    {
        _retry.Stop();
        _reposition.Stop();
        _power.TipPopup.IsOpen = false;   // 关闭悬停卡片
        _fan.TipPopup.IsOpen = false;
        try
        {
            if (_hwnd != IntPtr.Zero)
                SetParent(_hwnd, IntPtr.Zero);
        }
        catch
        {
            // 忽略：窗口可能已随任务栏销毁
        }
        try
        {
            Close();
        }
        catch
        {
            // 忽略：窗口可能已被系统连带着销毁（explorer 崩溃场景）
        }
    }

    private void ApplyThemeCore(bool light)
    {
        // 数值用状态色（正常绿 / 偏高橙 / 超高红，深色任务栏提亮一档）；标签灰色
        _sevGreen = Freeze(new SolidColorBrush(light ? Color.FromRgb(0x16, 0xA3, 0x4A) : Color.FromRgb(0x4A, 0xDE, 0x80)));
        _sevOrange = Freeze(new SolidColorBrush(light ? Color.FromRgb(0xD9, 0x77, 0x06) : Color.FromRgb(0xFB, 0x92, 0x3C)));
        _sevRed = Freeze(new SolidColorBrush(light ? Color.FromRgb(0xDC, 0x26, 0x26) : Color.FromRgb(0xF8, 0x71, 0x71)));
        _labelBrush = Freeze(new SolidColorBrush(light ? Color.FromRgb(0x5E, 0x66, 0x72) : Color.FromRgb(0xA8, 0xB3, 0xBD)));

        _power.Label.Foreground = _labelBrush;
        _fan.Label.Foreground = _labelBrush;

        RefreshValueColors();

        // 悬停高亮：统一白色（用户指定），40% 透明度
        _hoverBrush = Freeze(new SolidColorBrush(Color.FromArgb(0x66, 0xFF, 0xFF, 0xFF)));
    }

    /// <summary>按当前值重刷数值颜色（主题切换后调用；平时由 SetPair 按需更新）。</summary>
    private void RefreshValueColors()
    {
        UpdateValueBrush(_power, _powerElevated, _powerCritical);
        UpdateValueBrush(_fan, _fanElevated, _fanCritical);
    }

    private void UpdateValueBrush(Pair pair, double elevated, double critical)
    {
        Brush brush = ValueBrushFor(pair.LatestValue, elevated, critical);
        pair.Value.Foreground = brush;
        pair.TipValueRun.Foreground = brush;
    }

    /// <summary>严重度配色：正常绿 / 偏高橙 / 超高红；无数据灰色。</summary>
    private Brush ValueBrushFor(float? value, double elevated, double critical)
    {
        if (value is not { } v)
            return _labelBrush;
        if (v <= elevated)
            return _sevGreen;
        return v <= critical ? _sevOrange : _sevRed;
    }

    private static SolidColorBrush Freeze(SolidColorBrush b)
    {
        b.Freeze();
        return b;
    }

    private Pair MakePair(Pair pair, string label, string tipTitle, string tipUnit, double rightMargin = 6)
    {
        // 数值与标签作为一个紧凑块垂直居中：数字 12px 粗体彩色 + 标签 9px 灰，几乎贴在一起
        pair.Value.FontFamily = new FontFamily("Segoe UI");
        pair.Value.FontWeight = FontWeights.Bold;
        pair.Value.FontSize = 12;
        pair.Value.HorizontalAlignment = HorizontalAlignment.Center;
        pair.Value.Margin = new Thickness(0, 0, 0, 2);   // 数字与标签间留自然行距（参考常见任务栏挂件样式）
        pair.Value.Text = "--";

        pair.Label.Text = label;
        pair.Label.FontFamily = new FontFamily("Microsoft YaHei UI");
        pair.Label.FontSize = 9;
        pair.Label.HorizontalAlignment = HorizontalAlignment.Center;

        StackPanel stack = new() { VerticalAlignment = VerticalAlignment.Center };
        stack.Children.Add(pair.Value);
        stack.Children.Add(pair.Label);

        pair.Host.Child = stack;
        pair.Host.Margin = new Thickness(0, 0, rightMargin, 0);
        // 悬停热区加余量：文字左右各 5px、最小宽 36px 内都能触发悬浮卡片，不必精确压在文字上
        pair.Host.Padding = new Thickness(5, 0, 5, 0);
        pair.Host.MinWidth = 36;
        // 悬停高亮（TrafficMonitor 同款）：进格垫浅色圆角底，离格恢复透明（Transparent 保持可命中）
        pair.Host.CornerRadius = new CornerRadius(4);
        pair.Host.MouseEnter += (_, _) => { pair.Host.Background = _hoverBrush; OpenTip(pair); };
        pair.Host.MouseLeave += (_, _) => { pair.Host.Background = Brushes.Transparent; pair.TipPopup.IsOpen = false; };
        pair.Host.VerticalAlignment = VerticalAlignment.Stretch;   // 撑满任务栏高度 = 悬停热区
        pair.Host.Background = Brushes.Transparent;                // Transparent 可命中：不必贴着文字

        BuildTipCard(pair, tipTitle, tipUnit);
        return pair;
    }

    /// <summary>构建深色圆角统计卡片（ToolTip）：标题 + 彩色当前值 + 最低/最高/平均三列。
    /// 固定 Placement=Top：卡片始终出现在任务栏上方，绝不侵入任务栏。</summary>
    private static void BuildTipCard(Pair pair, string title, string unit)
    {
        static Brush Fg(byte a, byte r, byte g, byte bl)
        {
            SolidColorBrush brush = new(Color.FromArgb(a, r, g, bl));
            brush.Freeze();
            return brush;
        }

        Brush titleFg = Fg(0xC8, 0xFF, 0xFF, 0xFF);   // 卡片固定深色，不随任务栏主题变化
        Brush labelFg = Fg(0x96, 0xFF, 0xFF, 0xFF);
        Brush valueFg = Fg(0xF2, 0xFF, 0xFF, 0xFF);

        // 头部行结构：标题(TextBlock，负底边距压底) + [数值 Run + 单位 Run](同一 TextBlock 共享基线)。
        // 之前"三 Run 共基线"方案中文仍偏高：YaHei 12px 的字形底比 Segoe 20px 数字的字形底高约 3px
        //（Segoe 20px descent≈5.0px，YaHei 12px descent≈3.1px，基线相同时底边不齐）。
        // 现改为全部底对齐 + 标题负底边距 3px 补偿，底线严格齐平。
        pair.TipTitle.Text = title;
        pair.TipTitle.FontFamily = new FontFamily("Microsoft YaHei UI");
        pair.TipTitle.FontSize = 12;
        pair.TipTitle.Foreground = titleFg;
        pair.TipTitle.VerticalAlignment = VerticalAlignment.Bottom;
        pair.TipTitle.Margin = new Thickness(0, 0, 0, -3);   // 下压 3px：补齐与 20px 数字（Segoe descent）的底边差

        pair.TipValue.FontFamily = new FontFamily("Segoe UI");
        pair.TipValue.FontWeight = FontWeights.Bold;
        pair.TipValue.FontSize = 20;
        pair.TipValue.VerticalAlignment = VerticalAlignment.Bottom;
        // 颜色由 SetPair 按数值状态动态更新（正常绿 / 偏高橙 / 超高红），此处不设固定色

        pair.TipUnitRun.Text = " " + unit;
        pair.TipUnitRun.FontFamily = new FontFamily("Segoe UI");
        pair.TipUnitRun.FontSize = 11;
        pair.TipUnitRun.Foreground = labelFg;

        pair.TipValue.Inlines.Add(pair.TipValueRun);
        pair.TipValue.Inlines.Add(pair.TipUnitRun);

        StackPanel head = new() { Orientation = Orientation.Horizontal };
        head.Children.Add(pair.TipTitle);
        head.Children.Add(pair.TipValue);

        TextBlock MakeStatLabel(string text) => new()
        {
            Text = text,
            FontFamily = new FontFamily("Microsoft YaHei UI"),
            FontSize = 10,
            Foreground = labelFg,
        };

        pair.TipMin.FontFamily = new FontFamily("Segoe UI");
        pair.TipMin.FontWeight = FontWeights.SemiBold;
        pair.TipMin.FontSize = 13;
        pair.TipMin.Foreground = valueFg;
        pair.TipMax.FontFamily = new FontFamily("Segoe UI");
        pair.TipMax.FontWeight = FontWeights.SemiBold;
        pair.TipMax.FontSize = 13;
        pair.TipMax.Foreground = valueFg;
        pair.TipAvg.FontFamily = new FontFamily("Segoe UI");
        pair.TipAvg.FontWeight = FontWeights.SemiBold;
        pair.TipAvg.FontSize = 13;
        pair.TipAvg.Foreground = valueFg;

        StackPanel Col(string label, TextBlock value, double rightMargin)
        {
            StackPanel sp = new();
            sp.Children.Add(MakeStatLabel(label));
            sp.Children.Add(value);
            sp.Margin = new Thickness(0, 0, rightMargin, 0);
            return sp;
        }

        Grid stats = new();
        stats.Margin = new Thickness(0, 10, 0, 1);
        for (int i = 0; i < 3; i++)
            stats.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        StackPanel colMin = Col("最低", pair.TipMin, 12);   // 列间留 12px 间距，避免三列连成一串
        StackPanel colMax = Col("最高", pair.TipMax, 12);
        StackPanel colAvg = Col("平均", pair.TipAvg, 0);
        stats.Children.Add(colMin);
        stats.Children.Add(colMax);
        stats.Children.Add(colAvg);
        Grid.SetColumn(colMin, 0);
        Grid.SetColumn(colMax, 1);
        Grid.SetColumn(colAvg, 2);

        pair.TipCard.Children.Add(head);
        pair.TipCard.Children.Add(stats);

        // 深色圆角 Border + 投影 + 细描边（自绘 Popup，无系统 ToolTip 边框/动画/悬停延迟）
        Border cardBorder = new()
        {
            Background = new SolidColorBrush(Color.FromArgb(0xF2, 0x17, 0x24, 0x2F)),
            BorderBrush = new SolidColorBrush(Color.FromArgb(0x2E, 0xFF, 0xFF, 0xFF)),
            BorderThickness = new Thickness(1),
            CornerRadius = new CornerRadius(10),
            Padding = new Thickness(14, 10, 14, 11),
            Effect = new System.Windows.Media.Effects.DropShadowEffect
            {
                BlurRadius = 16,
                ShadowDepth = 3,
                Opacity = 0.38,
                Color = Colors.Black,
            },
            Child = pair.TipCard,
        };
        pair.TipPopup.Child = cardBorder;
        pair.TipPopup.AllowsTransparency = true;
        pair.TipPopup.StaysOpen = true;                 // 跟随悬停手动开关，不抢焦点
        pair.TipPopup.Placement = PlacementMode.RelativePoint;
        pair.TipPopup.PlacementTarget = pair.Host;
    }

    /// <summary>打开悬停卡片：相对指标格手动定位 —— 卡片底部 = 格顶 − 12px 净空，水平居中。
    /// 不用系统 ToolTip 排版（嵌入任务栏的子窗口里其垂直偏移不生效，卡片曾贴上任务栏）。</summary>
    private void OpenTip(Pair pair)
    {
        if (pair.TipPopup.Child is not UIElement child)
            return;

        child.Measure(new System.Windows.Size(double.PositiveInfinity, double.PositiveInfinity));
        System.Windows.Size d = child.DesiredSize;
        pair.TipPopup.HorizontalOffset = (pair.Host.ActualWidth - d.Width) / 2;
        pair.TipPopup.VerticalOffset = -(d.Height + 12);   // 12px 净空，含阴影不触任务栏
        pair.TipPopup.IsOpen = true;
    }

    /// <summary>嵌入任务栏：SetParent 成 Shell_TrayWnd 子窗口并定位到 TrayNotifyWnd 左侧。</summary>
    private bool Embed()
    {
        try
        {
            if (_hwnd == IntPtr.Zero)
                return false;

            IntPtr taskbar = FindWindow("Shell_TrayWnd", null);
            IntPtr tray = taskbar != IntPtr.Zero ? FindWindowEx(taskbar, IntPtr.Zero, "TrayNotifyWnd", null) : IntPtr.Zero;
            if (taskbar == IntPtr.Zero || tray == IntPtr.Zero)
            {
                _retry.Start();   // 开始菜单展开 / explorer 正在重启等情况：稍后重试
                return false;
            }

            // WPF 窗口默认 WS_POPUP，改为任务栏子窗口
            int style = GetWindowLong(_hwnd, GWL_STYLE);
            SetWindowLong(_hwnd, GWL_STYLE, (int)((uint)style | WS_CHILD | WS_VISIBLE));
            SetParent(_hwnd, taskbar);

            // 关键：EnsureHandle 只创建句柄、不触发渲染管线，必须 Show() 一次让 WPF 跑布局渲染，
            // 否则子窗口内容全透明
            if (!_shown)
            {
                Left = 0;
                Top = 0;
                Show();
                _shown = true;
            }

            _taskbar = taskbar;
            _embedded = true;
            _retry.Stop();
            Position();          // 立即定位渲染
            return true;
        }
        catch (Exception)
        {
            _retry.Start();
            return false;
        }
    }

    /// <summary>定位：任务栏内、TrayNotifyWnd 左侧留间距，高度撑满任务栏、内容垂直居中。
    /// 若托盘左侧已有其他第三方挂件，排到它左边避免重叠。</summary>
    private void Position()
    {
        if (!_embedded || _taskbar == IntPtr.Zero || !IsWindow(_taskbar))
            return;

        IntPtr tray = FindWindowEx(_taskbar, IntPtr.Zero, "TrayNotifyWnd", null);
        if (tray == IntPtr.Zero || !GetWindowRect(_taskbar, out RECT tb) || !GetWindowRect(tray, out RECT trayRect))
            return;

        double scale = GetDpiForWindow(_hwnd) / 96.0;

        // 窗口宽度收窄到真实内容宽度；文本列均为固定宽，数字变化不会引起宽度抖动
        double contentW = _root.ActualWidth > 10 ? Math.Ceiling(_root.ActualWidth) + 6 : Width;
        if (Math.Abs(Width - contentW) > 1)
            Width = contentW;
        int widthPx = (int)Math.Ceiling(Width * scale);
        int heightPx = tb.Bottom - tb.Top;
        int gapPx = (int)Math.Round(2 * scale);
        int x = trayRect.Left - tb.Left - gapPx - widthPx;

        GetWindowThreadProcessId(_taskbar, out uint taskbarPid);

        // 资源优化：全窗口枚举（EnumWindows/EnumChildWindows）降频到约 30 秒一次，结果缓存复用。
        // 其他挂件位置极少变化；托盘角落矩形变化（图标增减）时立即重扫。
        bool rescanForeign = _foreignCache is null ||
                             _lastTrayRectForScan.Equals(trayRect) == false ||
                             ++_foreignScanCounter >= 6;
        if (rescanForeign)
        {
            _foreignCache = ForeignWidgets(taskbarPid, trayRect.Left, tb.Top, tb.Bottom);
            _foreignScanCounter = 0;
            _lastTrayRectForScan = trayRect;
        }
        List<(IntPtr Hwnd, RECT Rect)> foreign = _foreignCache ?? new List<(IntPtr Hwnd, RECT Rect)>();

        // 自动避让：排到托盘角落左侧其他第三方挂件最左边缘的再左边，避免重叠
        if (foreign.Count > 0)
        {
            int foreignLeft = foreign[0].Rect.Left;
            foreach (var item in foreign)
                if (item.Rect.Left < foreignLeft)
                    foreignLeft = item.Rect.Left;
            int shifted = foreignLeft - tb.Left - gapPx - widthPx;
            if (shifted < x)
                x = shifted;
        }

        x = Math.Max(0, x);

        // 关键：位置和尺寸都没变就不再 SetWindowPos——避免打断 ToolTip 稳定显示
        if (x == _lastX && widthPx == _lastW && heightPx == _lastH)
            return;

        SetWindowPos(_hwnd, 0, x, 0, widthPx, heightPx, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        _lastX = x;

        _lastW = widthPx;
        _lastH = heightPx;
    }

    /// <summary>外部（DPI/任务栏变化等）请求重新定位。</summary>
    public void Reposition() => Position();

    /// <summary>任务栏上其他第三方挂件的（窗口句柄+屏幕矩形）列表（跳过自己与 explorer，过滤细小辅助窗口）。
    /// 用于自动避让：排到它们最左边缘的左边，避免重叠。</summary>
    private List<(IntPtr Hwnd, RECT Rect)> ForeignWidgets(uint taskbarPid, int boundaryRight, int tbTop, int tbBottom)
    {
        GetWindowThreadProcessId(_hwnd, out uint myPid);
        List<(IntPtr, RECT)> list = new();

        bool Consider(IntPtr hwnd, IntPtr lParam)
        {
            GetWindowThreadProcessId(hwnd, out uint pid);
            if (pid == myPid || pid == taskbarPid)
                return true;   // 跳过自己与系统（explorer）的窗口
            if (!IsWindowVisible(hwnd) || !GetWindowRect(hwnd, out RECT r))
                return true;
            if (r.Right - r.Left < 40 || r.Bottom - r.Top < 24)
                return true;   // 过滤细小的系统辅助窗口
            if (r.Right > boundaryRight + 4 || r.Right < boundaryRight - 800)
                return true;   // 只考虑紧邻托盘角落左侧的区域
            if (r.Bottom <= tbTop || r.Top >= tbBottom)
                return true;
            list.Add((hwnd, r));
            return true;
        }

        EnumChildWindows(_taskbar, Consider, IntPtr.Zero);
        EnumWindows(Consider, IntPtr.Zero);
        return list;
    }

    private IntPtr WndProc(IntPtr hwnd, int msg, IntPtr wParam, IntPtr lParam, ref bool handled)
    {
        // TaskbarCreated：explorer 重启广播，任务栏重建后重新嵌入
        if (_taskbarCreatedMsg != 0 && msg == (int)_taskbarCreatedMsg)
        {
            _embedded = false;
            Dispatcher.BeginInvoke(() => Embed());
        }
        return IntPtr.Zero;
    }
}
