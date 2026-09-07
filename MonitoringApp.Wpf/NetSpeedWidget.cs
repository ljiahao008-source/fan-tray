using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Documents;
using System.Windows.Input;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Shapes;
using System.Windows.Threading;
// 项目全局引入了 System.Drawing，显式绑定到 WPF 类型避免歧义
using Brushes = System.Windows.Media.Brushes;
using Brush = System.Windows.Media.Brush;
using Color = System.Windows.Media.Color;
using Drawing = System.Drawing;
using FontFamily = System.Windows.Media.FontFamily;
using HorizontalAlignment = System.Windows.HorizontalAlignment;
using Orientation = System.Windows.Controls.Orientation;
using Path = System.Windows.Shapes.Path;
using Point = System.Windows.Point;
using Size = System.Windows.Size;
using VerticalAlignment = System.Windows.VerticalAlignment;

namespace MonitoringApp;

/// <summary>
/// 任务栏监控小组件（管家样式 1:1 复刻）。把无边框透明 WPF 窗口 SetParent 为 Shell_TrayWnd 的子窗口，
/// 定位在 TrayNotifyWnd（托盘角落）左侧，整体布局与管家一致：
///   [圆形仪表（白圈 + 白色数字，温度状态配色）] [↑ 62.8 K/s / ↓ 326 K/s 两行彩色文本]
/// 嵌入方案与开源 TrafficMonitor 一致：explorer 重启（TaskbarCreated 广播）后自动重嵌；
/// 嵌入失败（安全软件拦截、开始菜单展开等）时 Available=false，由主窗口自动回退为方形网速图标。
/// </summary>
public sealed class NetSpeedWidget : Window
{
    private const int GWL_STYLE = -16;
    private const long WS_CHILD = 0x40000000L;
    private const long WS_VISIBLE = 0x10000000L;
    private const uint SWP_NOMOVE = 0x0002;
    private const uint SWP_NOSIZE = 0x0001;
    private const uint SWP_NOACTIVATE = 0x0010;
    private const uint SWP_SHOWWINDOW = 0x0040;
    private const uint SWP_HIDEWINDOW = 0x0080;

    private static readonly string LogPath = @"C:\Users\<用户名>\Desktop\机械革命监控\widget_debug.log";

    private static void Log(string message)
    {
        try
        {
            File.AppendAllText(LogPath, $"{DateTime.Now:HH:mm:ss.fff} {message}\r\n");
        }
        catch
        {
            // 日志失败不影响运行
        }
    }

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

    /// <summary>左键单击（主窗口做消抖后弹速率详情）。</summary>
    public event Action? SingleClick;

    /// <summary>双击（恢复主窗口）。</summary>
    public event Action? DoubleClick;

    /// <summary>单击圆盘区域（消抖后触发，弹运行中应用面板）。</summary>
    public event Action? RingSingleClick;

    /// <summary>是否已成功嵌入任务栏。</summary>
    public bool Available { get; private set; }

    /// <summary>小组件在屏幕上的像素矩形（用于弹出面板定位）；未嵌入时为 null。</summary>
    public (int X, int Y, int W, int H)? AnchorPx =>
        _embedded && _lastW > 0 ? (_lastX, _lastY, _lastW, _lastH) : null;

    private readonly AppSettings _settings;

    private readonly StackPanel _gaugePanel = new() { Orientation = Orientation.Horizontal, VerticalAlignment = VerticalAlignment.Center };
    private readonly StackPanel _pairPanel = new() { Orientation = Orientation.Horizontal, VerticalAlignment = VerticalAlignment.Center };
    private Grid _root = new();
    private readonly List<Ring> _rings = new();
    private readonly List<Pair> _pairs = new();
    private const double GaugeSize = 30;

    /// <summary>单个监控项圆环（双圆：外圈基础圆 + 内圈进度弧自底部对称上浮带渐变，管家同款）。</summary>
    private sealed class Ring : IWidgetPart
    {
        public Grid Host { get; } = new();
        public Ellipse Disc { get; } = new();
        public Ellipse Track { get; } = new();
        public Path Arc { get; } = new();
        public TextBlock Value { get; } = new();
        FrameworkElement IWidgetPart.Host => Host;
    }

    /// <summary>数值+中文标签的文本指标（管家样式：6%/CPU、56℃/温度）。</summary>
    private sealed class Pair : IWidgetPart
    {
        public Grid Host { get; } = new();
        public TextBlock Value { get; } = new();
        public TextBlock Label { get; } = new();
        FrameworkElement IWidgetPart.Host => Host;
    }

    private interface IWidgetPart
    {
        FrameworkElement Host { get; }
    }

    /// <summary>任务栏圆环监控项。</summary>
    public sealed record GaugeItem(string Text, float Pct, Drawing.Color Accent, string Tooltip);

    /// <summary>任务栏文本指标项（数值在上、中文标签在下，数值按状态着色）。</summary>
    public sealed record TextPair(string Value, string Label, Drawing.Color Accent, string Tooltip);
    private readonly TextBlock _upText = new();
    private readonly TextBlock _downText = new();
    private readonly DispatcherTimer _retry;
    private IntPtr _hwnd;
    private IntPtr _taskbar;
    private uint _taskbarCreatedMsg;
    private bool _embedded;
    private bool _userHidden = true;   // 初始不显示，由 UpdateNetIcon 按设置驱动
    private bool _gaugeVisible;
    private bool _pairVisible;
    private bool _shown;
    private bool _themeInit;
    private bool _lightTheme = true;
    private int _lastX, _lastY, _lastW, _lastH;
    private DispatcherTimer? _ringClickTimer;
    private double _lastUp = -1, _lastDown = -1;
    private Brush _upArrowBrush = Brushes.DodgerBlue;
    private Brush _downArrowBrush = Brushes.Green;
    private Brush _valueBrush = Brushes.Black;
    private Brush _labelBrush = Brushes.Gray;
    private Brush _trackBrush = Brushes.Gray;

    public NetSpeedWidget(AppSettings settings)
    {
        _settings = settings;
        WindowStyle = WindowStyle.None;
        ResizeMode = ResizeMode.NoResize;
        AllowsTransparency = true;
        Background = Brushes.Transparent;   // Transparent 笔刷可命中点击，null 会穿透
        ShowInTaskbar = false;
        ShowActivated = false;
        Width = WidthWithItems;             // 内容驱动，嵌入时按 DPI 换算成像素
        Height = 46;

        BuildLayout();

        MouseLeftButtonDown += (_, e) =>
        {
            bool overRing = OverRing(e);
            if (e.ClickCount >= 2)
            {
                _ringClickTimer?.Stop();
                _ringClickTimer = null;
                DoubleClick?.Invoke();
                return;
            }

            if (overRing)
            {
                Log($"ring click @({e.GetPosition(this).X:0},{e.GetPosition(this).Y:0})");
                StartRingSingleClick();          // 圆盘单击消抖（等待可能到来的双击）
            }
            else
                SingleClick?.Invoke();
        };

        // explorer 重启后 5 秒重试嵌入，直到成功
        _retry = new DispatcherTimer { Interval = TimeSpan.FromSeconds(5) };
        _retry.Tick += (_, _) => { if (!_userHidden && !_embedded) Embed(); };

        // 不经 WPF Show() 创建句柄：后续完全用 Win32 控制嵌入与显隐
        new WindowInteropHelper(this).EnsureHandle();
        _hwnd = new WindowInteropHelper(this).Handle;
        _taskbarCreatedMsg = RegisterWindowMessage("TaskbarCreated");
        HwndSource.FromHwnd(_hwnd)?.AddHook(WndProc);
    }

    private double WidthWithItems => (_gaugeVisible ? _rings.Count * 36 : 0) + _pairs.Count * 44 + 108;

    private void BuildLayout()
    {
        Grid root = new()
        {
            HorizontalAlignment = HorizontalAlignment.Left,   // 内容靠左，ActualWidth 即真实内容宽度
        };
        root.Margin = new Thickness(3, 0, 0, 0);
        root.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        root.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        root.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        root.Margin = new Thickness(3, 0, 0, 0);

        // —— 圆环仪表区 + 文本指标区（管家样式：值在上、中文标签在下）——
        _gaugePanel.Margin = new Thickness(0, 0, 6, 0);
        Grid.SetColumn(_gaugePanel, 0);
        root.Children.Add(_gaugePanel);

        _pairPanel.Margin = new Thickness(2, 0, 0, 0);
        Grid.SetColumn(_pairPanel, 2);
        root.Children.Add(_pairPanel);

        // —— 网速两行文本（固定宽度：数字变化不引起窗口宽度抖动）——
        Grid net = new() { Width = 84 };
        net.RowDefinitions.Add(new RowDefinition());
        net.RowDefinitions.Add(new RowDefinition());
        net.VerticalAlignment = VerticalAlignment.Center;
        foreach (var (tb, row) in new[] { (_upText, 0), (_downText, 1) })
        {
            tb.FontFamily = new FontFamily("Segoe UI");
            tb.FontWeight = FontWeights.Bold;
            tb.FontSize = 10.5;
            tb.VerticalAlignment = VerticalAlignment.Center;
            tb.HorizontalAlignment = HorizontalAlignment.Left;
            tb.Margin = new Thickness(0, 0, 2, 0);
            Grid.SetRow(tb, row);
            net.Children.Add(tb);
        }
        Grid.SetColumn(net, 1);
        root.Children.Add(net);

        Content = root;
        _root = root;
        ApplyThemeCore(true);
        _gaugePanel.Visibility = Visibility.Collapsed;
        _pairPanel.Visibility = Visibility.Collapsed;
    }

    /// <summary>更新圆环组（温度等）；空列表时隐藏圆环区。</summary>
    public void UpdateGauges(IReadOnlyList<GaugeItem> items)
    {
        SyncCount(_rings, items.Count, _gaugePanel.Children, MakeRing);
        ApplyVisibility(ref _gaugeVisible, items.Count > 0, _gaugePanel);

        for (int i = 0; i < items.Count; i++)
        {
            GaugeItem it = items[i];
            Ring ring = _rings[i];
            ring.Arc.Stroke = VerticalGradient(Color.FromRgb(it.Accent.R, it.Accent.G, it.Accent.B));
            ring.Arc.Data = SymmetricArcGeometry(it.Pct);
            ring.Value.Text = it.Text;
            ring.Host.ToolTip = it.Tooltip;
        }
    }

    /// <summary>更新文本指标组（管家样式：6%/CPU、56℃/温度）；空列表时隐藏。</summary>
    public void UpdatePairs(IReadOnlyList<TextPair> items)
    {
        SyncCount(_pairs, items.Count, _pairPanel.Children, MakePair);
        ApplyVisibility(ref _pairVisible, items.Count > 0, _pairPanel);

        for (int i = 0; i < items.Count; i++)
        {
            TextPair it = items[i];
            Pair pair = _pairs[i];
            pair.Value.Text = it.Value;
            pair.Label.Text = it.Label;
            pair.Value.Foreground = Freeze(new SolidColorBrush(Color.FromRgb(it.Accent.R, it.Accent.G, it.Accent.B)));
            pair.Host.ToolTip = it.Tooltip;
        }

        if (_embedded)
            Position();   // 宽度变了立即重定位
    }

    private void ApplyVisibility(ref bool current, bool show, UIElement element)
    {
        if (current == show)
            return;
        current = show;
        element.Visibility = show ? Visibility.Visible : Visibility.Collapsed;
        Width = WidthWithItems;
        if (_embedded)
            Position();
    }

    private static void SyncCount<T>(List<T> list, int count, UIElementCollection parent, Func<T> create)
        where T : class, IWidgetPart
    {
        while (list.Count < count)
        {
            T part = create();
            list.Add(part);
            parent.Add(part.Host);
        }
        for (int i = list.Count - 1; i >= count; i--)
        {
            parent.Remove(list[i].Host);
            list.RemoveAt(i);
        }
    }

    private Ring MakeRing()
    {
        var ring = new Ring();
        ring.Disc.Width = ring.Disc.Height = GaugeSize;
        ring.Disc.VerticalAlignment = VerticalAlignment.Center;

        // 外圈：基础圆（细、灰轨道色）
        ring.Track.Width = ring.Track.Height = GaugeSize;
        ring.Track.StrokeThickness = 1.5;
        ring.Track.VerticalAlignment = VerticalAlignment.Center;

        // 内圈：进度弧，自底部左右对称上浮、垂直渐变（底饱和顶浅）
        ring.Arc.Width = ring.Arc.Height = GaugeSize;
        ring.Arc.StrokeThickness = 2.5;
        ring.Arc.StrokeStartLineCap = PenLineCap.Round;
        ring.Arc.StrokeEndLineCap = PenLineCap.Round;
        ring.Arc.VerticalAlignment = VerticalAlignment.Center;

        ring.Value.HorizontalAlignment = HorizontalAlignment.Center;
        ring.Value.VerticalAlignment = VerticalAlignment.Center;
        ring.Value.FontFamily = new FontFamily("Microsoft YaHei UI");
        ring.Value.FontWeight = FontWeights.Bold;
        ring.Value.FontSize = 10.5;
        ring.Value.Foreground = _valueBrush;

        ring.Host.Children.Add(ring.Disc);
        ring.Host.Children.Add(ring.Track);
        ring.Host.Children.Add(ring.Arc);
        ring.Host.Children.Add(ring.Value);
        ring.Host.Margin = new Thickness(0, 0, 5, 0);
        ring.Host.VerticalAlignment = VerticalAlignment.Center;
        return ring;
    }

    /// <summary>内圈进度弧几何（双圆的内圆）：自底部（6 点钟）向左右两侧对称上浮，占比越大涨得越高，满值在顶部闭合。</summary>
    private static Geometry SymmetricArcGeometry(double pct)
    {
        double r = GaugeSize / 2.0 - 3.0;   // 内圈半径：留出外圈轨道
        double c = GaugeSize / 2.0;
        double p = Math.Clamp(pct, 0.01, 1.0);
        double theta = Math.PI * p;         // 每侧弧度：占比 50% 时恰好涨到顶部（9 点/3 点）
        double aL = Math.PI / 2 + theta;    // 左侧端点
        double aR = Math.PI / 2 - theta;    // 右侧端点
        var start = new Point(c, c + r);    // 起点：底部 6 点钟
        var geo = new StreamGeometry();
        using (StreamGeometryContext ctx = geo.Open())
        {
            ctx.BeginFigure(start, false, false);
            ctx.ArcTo(
                new Point(c + r * Math.Cos(aL), c + r * Math.Sin(aL)),
                new Size(r, r), 0, p > 0.5, SweepDirection.Clockwise, true, false);
            ctx.BeginFigure(start, false, false);
            ctx.ArcTo(
                new Point(c + r * Math.Cos(aR), c + r * Math.Sin(aR)),
                new Size(r, r), 0, p > 0.5, SweepDirection.Counterclockwise, true, false);
        }
        geo.Freeze();
        return geo;
    }

    /// <summary>进度弧画刷：垂直渐变——底部饱和状态色，顶端浅色（管家的渐变细节）。</summary>
    private static Brush VerticalGradient(Color baseColor)
    {
        var grad = new LinearGradientBrush
        {
            StartPoint = new Point(0, 1),   // 底
            EndPoint = new Point(0, 0),     // 顶
        };
        grad.GradientStops.Add(new GradientStop(baseColor, 1.0));
        grad.GradientStops.Add(new GradientStop(Shift(baseColor, 0.45), 0.0));
        grad.Freeze();
        return grad;
    }

    private Pair MakePair()
    {
        var pair = new Pair();
        pair.Value.FontFamily = new FontFamily("Segoe UI");
        pair.Value.FontWeight = FontWeights.Bold;
        pair.Value.FontSize = 11;
        pair.Value.Foreground = _valueBrush;
        pair.Value.HorizontalAlignment = HorizontalAlignment.Center;
        pair.Label.FontFamily = new FontFamily("Microsoft YaHei UI");
        pair.Label.FontSize = 8;
        pair.Label.Foreground = _labelBrush;
        pair.Label.HorizontalAlignment = HorizontalAlignment.Center;
        pair.Host.RowDefinitions.Add(new RowDefinition());
        pair.Host.RowDefinitions.Add(new RowDefinition());
        pair.Host.Children.Add(pair.Value);
        pair.Host.Children.Add(pair.Label);
        Grid.SetRow(pair.Value, 0);
        Grid.SetRow(pair.Label, 1);
        pair.Host.Width = 46;   // 固定宽度：数值变化不引起窗口宽度抖动
        pair.Host.Margin = new Thickness(7, 0, 0, 0);
        pair.Host.VerticalAlignment = VerticalAlignment.Center;
        return pair;
    }

    /// <summary>点击是否落在某个圆盘上。</summary>
    private bool OverRing(MouseButtonEventArgs e)
    {
        foreach (Ring ring in _rings)
        {
            Point p = e.GetPosition(ring.Host);
            if (p.X >= 0 && p.Y >= 0 && p.X <= ring.Host.ActualWidth && p.Y <= ring.Host.ActualHeight)
                return true;
        }
        return false;
    }

    /// <summary>圆盘单击消抖：等系统双击间隔，没有后续双击才判定为单击。</summary>
    private void StartRingSingleClick()
    {
        _ringClickTimer?.Stop();
        _ringClickTimer = new DispatcherTimer
        {
            Interval = TimeSpan.FromMilliseconds(System.Windows.Forms.SystemInformation.DoubleClickTime),
        };
        _ringClickTimer.Tick += (_, _) =>
        {
            _ringClickTimer?.Stop();
            _ringClickTimer = null;
            RingSingleClick?.Invoke();
        };
        _ringClickTimer.Start();
    }

    /// <summary>更新速率文本与悬停提示；未嵌入时会自动重试嵌入。</summary>
    public void Update(double upKbps, double downKbps, string tooltip)
    {
        _lastUp = upKbps;
        _lastDown = downKbps;
        ToolTip = tooltip;

        if (_userHidden)
            return;
        if (!_embedded && !Embed())
            return;
        Refresh();
    }

    /// <summary>按设置显示小组件。</summary>
    public void ShowWidget()
    {
        _userHidden = false;
        if (!_embedded)
            Embed();
    }

    /// <summary>按设置隐藏小组件（回退为方形图标）。</summary>
    public void HideWidget()
    {
        _userHidden = true;
        _retry.Stop();
        if (_hwnd != IntPtr.Zero)
            SetWindowPos(_hwnd, 0, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_HIDEWINDOW);
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

    /// <summary>退出前解除任务栏父子关系，避免销毁顺序问题。</summary>
    public void Dispose()
    {
        _retry.Stop();
        try
        {
            if (_hwnd != IntPtr.Zero)
                SetParent(_hwnd, IntPtr.Zero);
        }
        catch
        {
            // 忽略：窗口可能已随任务栏销毁
        }
        Close();
    }

    private void ApplyThemeCore(bool light)
    {
        // 箭头彩色（管家样式），数值主体：浅色任务栏黑字、深色任务栏白字；标签灰色
        _upArrowBrush = Freeze(new SolidColorBrush(light ? Color.FromRgb(0x1E, 0x8F, 0xE0) : Color.FromRgb(0x4C, 0xC2, 0xFF)));
        _downArrowBrush = Freeze(new SolidColorBrush(light ? Color.FromRgb(0x16, 0xA3, 0x4A) : Color.FromRgb(0x58, 0xD6, 0x8D)));
        _valueBrush = Freeze(new SolidColorBrush(light ? Color.FromRgb(0x1F, 0x23, 0x28) : Color.FromRgb(0xF2, 0xF5, 0xF7)));
        _labelBrush = Freeze(new SolidColorBrush(light ? Color.FromRgb(0x6B, 0x72, 0x80) : Color.FromRgb(0x9A, 0xA4, 0xAE)));
        _trackBrush = Freeze(new SolidColorBrush(light ? Color.FromRgb(0xD5, 0xDB, 0xE0) : Color.FromRgb(0x3A, 0x40, 0x46)));

        // 圆盘：白底（深色任务栏深底）；外圈轨道灰色，内圈进度弧按状态渐变（UpdateGauges）
        foreach (Ring ring in _rings)
        {
            ring.Disc.Fill = Freeze(new SolidColorBrush(light ? Color.FromRgb(0xFF, 0xFF, 0xFF) : Color.FromRgb(0x22, 0x26, 0x2C)));
            ring.Track.Stroke = _trackBrush;
            ring.Value.Foreground = _valueBrush;
        }
        foreach (Pair pair in _pairs)
        {
            pair.Value.Foreground = _valueBrush;
            pair.Label.Foreground = _labelBrush;
        }
        Refresh();   // 重刷箭头/数值 Runs 颜色（未嵌入时 Refresh 内部安全空转）
    }

    private static SolidColorBrush Freeze(SolidColorBrush b)
    {
        b.Freeze();
        return b;
    }

    /// <summary>嵌入任务栏：SetParent 成 Shell_TrayWnd 子窗口并定位到 TrayNotifyWnd 左侧。</summary>
    private bool Embed()
    {
        try
        {
            Available = false;
            if (_hwnd == IntPtr.Zero || _userHidden)
                return false;

            IntPtr taskbar = FindWindow("Shell_TrayWnd", null);
            IntPtr tray = taskbar != IntPtr.Zero ? FindWindowEx(taskbar, IntPtr.Zero, "TrayNotifyWnd", null) : IntPtr.Zero;
            if (taskbar == IntPtr.Zero || tray == IntPtr.Zero)
            {
                Log("embed retry: taskbar/tray not found");
                _retry.Start();   // 开始菜单展开 / explorer 正在重启等情况：稍后重试
                return false;
            }

            // WPF 窗口默认 WS_POPUP，改为任务栏子窗口
            int style = GetWindowLong(_hwnd, GWL_STYLE);
            SetWindowLong(_hwnd, GWL_STYLE, (int)((uint)style | WS_CHILD | WS_VISIBLE));
            SetParent(_hwnd, taskbar);

            // 关键：EnsureHandle 只创建句柄、不触发渲染管线，必须 Show() 一次让 WPF 跑布局渲染，
            // 否则子窗口内容全透明（此前「嵌入成功却看不见」的根因）
            if (!_shown)
            {
                Left = 0;
                Top = 0;
                Show();
                _shown = true;
            }

            _taskbar = taskbar;
            _embedded = true;
            Available = true;
            _retry.Stop();
            Refresh();          // 立即渲染最后一次数据并定位
            Log($"embed ok width={Width}");
            return true;
        }
        catch (Exception ex)
        {
            Log("embed fail: " + ex);
            _retry.Start();
            return false;
        }
    }

    private void Refresh()
    {
        SetSpeedLine(_upText, "↑ ", Format(_lastUp), _upArrowBrush);
        SetSpeedLine(_downText, "↓ ", Format(_lastDown), _downArrowBrush);
        // 这里刻意不调 Position()：窗口每秒一动，悬浮提示就无法稳定显示；
        // 只在嵌入、圆环/文本对数量变化、DPI 变化时才重新定位
    }

    /// <summary>速率行：方向箭头彩色 + 数值主体色（黑/白随主题），贴近管家样式。</summary>
    private void SetSpeedLine(TextBlock tb, string arrow, string value, Brush arrowBrush)
    {
        tb.Inlines.Clear();
        tb.Inlines.Add(new Run(arrow) { Foreground = arrowBrush });
        tb.Inlines.Add(new Run(value) { Foreground = _valueBrush });
    }

    /// <summary>定位：任务栏内、TrayNotifyWnd 左侧留 6px 间距，高度撑满任务栏、内容垂直居中。
    /// 若托盘左侧已有其他第三方挂件（如腾讯管家），自动排到它左边，避免重叠。</summary>
    private void Position()
    {
        if (!_embedded || _taskbar == IntPtr.Zero || !IsWindow(_taskbar))
            return;

        IntPtr tray = FindWindowEx(_taskbar, IntPtr.Zero, "TrayNotifyWnd", null);
        if (tray == IntPtr.Zero || !GetWindowRect(_taskbar, out RECT tb) || !GetWindowRect(tray, out RECT trayRect))
            return;

        double scale = GetDpiForWindow(_hwnd) / 96.0;

        // 窗口宽度收窄到真实内容宽度（左对齐布局下的 ActualWidth + 页边距）。
        // 文本列均为固定宽，数字变化不会引起宽度抖动；宽度变化超过 1px 才调整。
        double contentW = _root.ActualWidth > 10 ? Math.Ceiling(_root.ActualWidth) + 8 : Width;
        if (Math.Abs(Width - contentW) > 1)
            Width = contentW;
        int widthPx = (int)Math.Ceiling(Width * scale);
        int heightPx = tb.Bottom - tb.Top;
        int gapPx = (int)Math.Round(6 * scale);
        int x = trayRect.Left - tb.Left - gapPx - widthPx;

        // 避让：取托盘角落左侧第三方挂件的最左边缘，把自己排到它再左边
        GetWindowThreadProcessId(_taskbar, out uint taskbarPid);
        int foreignLeft = ForeignWidgetLeft(taskbarPid, trayRect.Left, tb.Top, tb.Bottom);
        if (foreignLeft != int.MaxValue)
        {
            int shifted = foreignLeft - tb.Left - gapPx - widthPx;
            if (shifted < x)
                x = shifted;
        }

        x = Math.Max(0, x);

        // 关键：位置和尺寸都没变就不再 SetWindowPos——每秒一条空白位置消息，
        // 会不断打断 ToolTip，导致悬浮提示无法稳定显示
        if (x == _lastX && widthPx == _lastW && heightPx == _lastH)
            return;

        SetWindowPos(_hwnd, 0, x, 0, widthPx, heightPx, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        _lastX = x;
        _lastY = tb.Top;
        _lastW = widthPx;
        _lastH = heightPx;
        Log($"position x={x} w={widthPx} foreignLeft={foreignLeft}");
    }

    /// <summary>外部（DPI/任务栏变化等）请求重新定位。</summary>
    public void Reposition() => Position();

    /// <summary>任务栏上其他第三方挂件（别的进程、占满任务栏高度、位于托盘角落左侧）的最左边缘屏幕坐标；无则 int.MaxValue。
    /// 同时扫描任务栏子窗口与顶层窗口（部分挂件不挂在任务栏树里，而是顶层悬浮在任务栏上方）。</summary>
    private int ForeignWidgetLeft(uint taskbarPid, int boundaryRight, int tbTop, int tbBottom)
    {
        GetWindowThreadProcessId(_hwnd, out uint myPid);
        int minLeft = int.MaxValue;

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
                return true;   // 与任务栏纵向范围无交集（顶层窗口扫描用）
            if (r.Left < minLeft)
                minLeft = r.Left;
            return true;
        }

        EnumChildWindows(_taskbar, Consider, IntPtr.Zero);
        EnumWindows(Consider, IntPtr.Zero);
        return minLeft;
    }

    /// <summary>速率短格式：始终带一位小数（79.5 K/s / 88.0 K/s / 2.4 M/s），对齐管家显示。</summary>
    private static string Format(double kbps)
    {
        (double num, string unit) = NetworkSpeedMeter.Format(kbps);
        string u = unit == "MB/s" ? "M/s" : "K/s";
        return (num >= 100 ? num.ToString("0") : num.ToString("0.0")) + " " + u;
    }

    /// <summary>把颜色向白(+t)/黑(-t)方向偏移。</summary>
    private static Color Shift(Color c, double t)
    {
        byte target = t >= 0 ? (byte)255 : (byte)0;
        double k = Math.Abs(t);
        return Color.FromRgb(
            (byte)(c.R + (target - c.R) * k),
            (byte)(c.G + (target - c.G) * k),
            (byte)(c.B + (target - c.B) * k));
    }

    private IntPtr WndProc(IntPtr hwnd, int msg, IntPtr wParam, IntPtr lParam, ref bool handled)
    {
        // TaskbarCreated：explorer 重启广播，任务栏重建后重新嵌入
        if (_taskbarCreatedMsg != 0 && msg == (int)_taskbarCreatedMsg)
        {
            _embedded = false;
            Available = false;
            Dispatcher.BeginInvoke(() => { if (!_userHidden) Embed(); });
        }
        return IntPtr.Zero;
    }
}
