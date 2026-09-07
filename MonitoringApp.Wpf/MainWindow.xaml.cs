using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Controls;
using Microsoft.Win32;
using System.Windows.Controls.Primitives;
using System.Windows.Media;
using System.Windows.Threading;
using LibreHardwareMonitor.Mechrevo;
using ScottPlot;
using Drawing = System.Drawing;
using Drawing2D = System.Drawing.Drawing2D;
using WF = System.Windows.Forms;
using Button = System.Windows.Controls.Button;

namespace MonitoringApp;

public partial class MainWindow : Window
{
    private const int SmCxSmIcon = 49;
    private const int NetIconIndex = 4; // 网络合显图标在托盘槽位中的下标

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern IntPtr FindWindow(string? lpClassName, string? lpWindowName);

    [DllImport("user32.dll")]
    private static extern uint GetDpiForWindow(IntPtr hwnd);

    [DllImport("user32.dll")]
    private static extern uint GetDpiForSystem();

    [DllImport("user32.dll")]
    private static extern int GetSystemMetricsForDpi(int nIndex, uint dpi);

    private readonly MonitoringService _service = new();
    private readonly AppSettings _settings = AppSettings.Load();
    private readonly PowerHistory _powerHistory = PowerHistory.Load();
    private readonly HistoryStore _history = new(TimeSpan.FromDays(7));
    private readonly DispatcherTimer _timer;
    private readonly WF.NotifyIcon[] _icons = new WF.NotifyIcon[5];
    private readonly Drawing.Icon?[] _lastIcons = new Drawing.Icon?[5];
    private readonly (string Text, Drawing.Color Color, int PixelSize, bool LightTheme)?[] _iconCache = new (string, Drawing.Color, int, bool)?[5];
    private int _trayIconSize;
    private bool _lightSystemTheme;
    private string? _netIconCacheKey; // 网络合显图标的绘制缓存键（↑值|↓值）
    private MetricKind _metric = MetricKind.Power;
    private TimeSpan _period = TimeSpan.FromMinutes(1);
    private bool _isExiting;
    private DateTime _lastPowerSample = DateTime.MinValue;
    private int _calYear = DateTime.Now.Year;
    private int _calMonth = DateTime.Now.Month;

    private readonly NetworkSpeedMeter _net = new();     // 实时网速采样
    private CpuUsageMeter? _cpu;                          // 整机 CPU 占用采样
    private System.Windows.Threading.DispatcherTimer? _netClickTimer; // 网络图标 单击/双击 消抖
    private NetSpeedWidget? _netWidget;                  // 任务栏网速文本小组件（管家样式）
    private WF.NotifyIcon? _appIcon;                     // 托盘唯一程序图标（菜单/恢复窗口入口）
    private readonly List<(WF.ToolStripMenuItem Item, Func<bool> Get)> _trayToggles = new();
    private readonly DateTime _startedAt = DateTime.Now;           // 本次会话起点
    private readonly RuntimeStore _runtime = RuntimeStore.Load();  // 运行时长档案（累计+按日）
    private DateTime _lastRuntimeAccount = DateTime.Now;           // 上次时长记账时间
    private MonitorBar? _bar;                            // 横向监控长条

    // 温度告警配色（Fluent 浅色：正常绿 / 警告琥珀 / 临界红）
    private static readonly SolidColorBrush BrushTempNormal = new(System.Windows.Media.Color.FromRgb(0x16, 0xA3, 0x4A));
    private static readonly SolidColorBrush BrushTempWarn = new(System.Windows.Media.Color.FromRgb(0xD9, 0x77, 0x06));
    private static readonly SolidColorBrush BrushTempCrit = new(System.Windows.Media.Color.FromRgb(0xDC, 0x26, 0x26));

    public MainWindow()
    {
        InitializeComponent();
        RestoreWindowState();
        InitChart();
        RefreshTrayEnvironment(force: true);
        SystemEvents.UserPreferenceChanged += OnUserPreferenceChanged;
        InitTray();
        InitNetWidget();
        BuildCalendar(_calYear, _calMonth);
        HighlightButtons();

        _timer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(_settings.RefreshIntervalMs) };
        _timer.Tick += (_, _) => OnTick();
        _timer.Start();

        try
        {
            _cpu = new CpuUsageMeter();
            _cpu.Next();   // 预热：首值恒为 0，丢弃
        }
        catch
        {
            _cpu = null;   // 性能计数器不可用时隐藏 CPU 指标
        }

        MonitoringSnapshot? s0 = Refresh();
        if (s0 != null)
        {
            UpdateChart(); // _autoScaleNeeded 默认 true，内部完成首次自动适配
            UpdateTray(s0);
        }

        // 调试入口：--panel 直接打开运行中应用面板
        string[] cliArgs = Environment.GetCommandLineArgs();
        if (cliArgs.Contains("--panel"))
            Dispatcher.BeginInvoke(DispatcherPriority.Background, new Action(ToggleRunningApps));
    }

    private void InitChart()
    {
        Plot.Plot.FigureBackground.Color = ScottPlot.Color.FromHex("#FFFFFF");
        Plot.Plot.DataBackground.Color = ScottPlot.Color.FromHex("#FFFFFF");
        Plot.Plot.Font.Set("Microsoft YaHei UI");
        Plot.Plot.Axes.Color(ScottPlot.Color.FromHex("#C7CED8"));
        Plot.Plot.Axes.Left.TickLabelStyle.ForeColor = ScottPlot.Color.FromHex("#6B7686");
        Plot.Plot.Axes.Bottom.TickLabelStyle.ForeColor = ScottPlot.Color.FromHex("#6B7686");
        Plot.Plot.Axes.Left.Label.ForeColor = ScottPlot.Color.FromHex("#6B7686");
        Plot.Plot.Axes.Bottom.Label.ForeColor = ScottPlot.Color.FromHex("#6B7686");
        Plot.Plot.Grid.MajorLineColor = ScottPlot.Color.FromHex("#EDF0F4");
        Plot.Plot.Axes.DateTimeTicksBottom(); // 日期时间横轴，仅需配置一次
    }

    private void InitTray()
    {
        WF.ContextMenuStrip menu = BuildTrayMenu();
        WF.NotifyIcon icon = new() { ContextMenuStrip = menu, Visible = false };
        icon.DoubleClick += (_, _) => OnTrayDoubleClick();
        icon.MouseClick += (_, e) => OnNetIconClick(e);
        _icons[NetIconIndex] = icon;

        // 托盘只保留一个程序图标（监控数据全部展示在任务栏小组件里）
        _appIcon = new WF.NotifyIcon
        {
            ContextMenuStrip = menu,
            Visible = true,
            Text = "机械革命硬件监控",
        };
        try
        {
            if (Environment.ProcessPath is { } exe)
                _appIcon.Icon = Drawing.Icon.ExtractAssociatedIcon(exe);
        }
        catch
        {
            // 图标提取失败时保留系统默认图标
        }
        _appIcon.DoubleClick += (_, _) => ShowFromTray();
        _appIcon.MouseClick += (_, e) =>
        {
            if (e.Button == WF.MouseButtons.Left)
                ShowFromTray();
        };
    }

    /// <summary>创建任务栏网速小组件（管家样式）；嵌入失败时自动回退为方形网速图标。</summary>
    private void InitNetWidget()
    {
        _netWidget = new NetSpeedWidget(_settings);
        _netWidget.SingleClick += StartPendingNetClick;
        _netWidget.DoubleClick += OnTrayDoubleClick;
        _netWidget.RingSingleClick += ToggleRunningApps;   // 单击圆盘：运行中应用面板
    }

    /// <summary>单击圆盘：打开/关闭运行中应用面板（管家样式，按内存降序 + 占用条）。</summary>
    private void ToggleRunningApps()
    {
        RunningAppsPanel.Toggle(_netWidget?.AnchorPx);
    }

    private WF.ContextMenuStrip BuildTrayMenu()
    {
        WF.ContextMenuStrip menu = new();

        // —— 监控项开关（与任务栏小组件右键菜单共用同一份配置）——
        menu.Items.Add(TrayToggle("CPU", () => _settings.ShowCpuUsage, v => _settings.ShowCpuUsage = v));
        menu.Items.Add(TrayToggle("温度", () => _settings.ShowCpuTemperature, v => _settings.ShowCpuTemperature = v));
        menu.Items.Add(TrayToggle("网速", () => _settings.ShowNetworkSpeed, v => _settings.ShowNetworkSpeed = v));
        menu.Items.Add(TrayToggle("内存", () => _settings.ShowMemoryLoad, v => _settings.ShowMemoryLoad = v));
        menu.Items.Add(TrayToggle("功耗", () => _settings.ShowCpuPower, v => _settings.ShowCpuPower = v));
        menu.Items.Add(TrayToggle("风扇", () => _settings.ShowFanRpm, v => _settings.ShowFanRpm = v));
        menu.Items.Add(new WF.ToolStripSeparator());

        menu.Items.Add("显示主窗口", null, (_, _) => ShowFromTray());
        menu.Items.Add("显示/隐藏监控长条", null, (_, _) => ToggleBar());
        menu.Items.Add("设置", null, (_, _) => OpenSettings());
        menu.Items.Add("重置统计", null, (_, _) => { _service.Reset(); _history.Clear(); });
        menu.Items.Add(new WF.ToolStripSeparator());
        menu.Items.Add("退出", null, (_, _) => ExitApp());

        // 每次打开菜单时同步勾选状态（任务栏小组件右键菜单可能改过配置）
        menu.Opening += (_, _) =>
        {
            foreach (var (item, get) in _trayToggles)
                item.Checked = get();
        };
        return menu;
    }

    /// <summary>托盘菜单里的监控项开关（勾选即生效并持久化）。</summary>
    private WF.ToolStripMenuItem TrayToggle(string header, Func<bool> get, Action<bool> set)
    {
        WF.ToolStripMenuItem item = new(header) { CheckOnClick = true, Checked = get() };
        item.Click += (_, _) =>
        {
            set(item.Checked);
            _settings.Save();
            Refresh();
        };
        _trayToggles.Add((item, get));
        return item;
    }

    public void ShowFromTray()
    {
        Show();
        WindowState = WindowState.Normal;
        Activate();
    }

    /// <summary>双击任意托盘图标：取消未决的单击动作并恢复主窗口。</summary>
    private void OnTrayDoubleClick()
    {
        CancelPendingNetClick();
        ShowFromTray();
    }

    /// <summary>单击网络速率图标（左键）：延迟消抖后弹出完整速率详情，避免与双击冲突。</summary>
    private void OnNetIconClick(WF.MouseEventArgs e)
    {
        if (e.Button != WF.MouseButtons.Left)
            return;
        StartPendingNetClick();
    }

    /// <summary>延迟消抖的单击动作：弹完整速率详情（托盘图标与任务栏小组件共用）。</summary>
    private void StartPendingNetClick()
    {
        if (!_settings.ShowNetworkSpeed)
            return;

        CancelPendingNetClick();
        _netClickTimer = new System.Windows.Threading.DispatcherTimer
        {
            Interval = TimeSpan.FromMilliseconds(WF.SystemInformation.DoubleClickTime),
        };
        _netClickTimer.Tick += (_, _) =>
        {
            CancelPendingNetClick();
            ShowNetDetail();
        };
        _netClickTimer.Start();
    }

    private void CancelPendingNetClick()
    {
        _netClickTimer?.Stop();
        _netClickTimer = null;
    }

    /// <summary>弹出网络速率详情：当前上/下行实时速率与本次会话累计流量。</summary>
    private void ShowNetDetail()
    {
        string body =
            $"上行  {NetworkSpeedMeter.FormatText(_net.UpKbps)}    会话累计 ↑{_net.TotalUpMB:0.0} MB\n" +
            $"下行  {NetworkSpeedMeter.FormatText(_net.DownKbps)}    会话累计 ↓{_net.TotalDownMB:0.0} MB";
        ToastWindow.Show("实时网络速率", body, nearTray: true);
    }

    /// <summary>切换横向监控长条（显示/隐藏）。首次创建时按记忆位置或顶部居中放置。</summary>
    private void ToggleBar()
    {
        if (_bar == null)
        {
            _bar = new MonitorBar();
            _bar.MoveEnded += (l, t) =>
            {
                _settings.BarLeft = l;
                _settings.BarTop = t;
                _settings.Save();
            };
            if (!double.IsNaN(_settings.BarLeft) && !double.IsNaN(_settings.BarTop))
            {
                _bar.Left = _settings.BarLeft;
                _bar.Top = _settings.BarTop;
            }
            else
            {
                var wa = SystemParameters.WorkArea;
                _bar.Left = wa.Left + (wa.Width - _bar.Width) / 2;
                _bar.Top = wa.Top + 8;
            }
        }

        if (_bar.IsVisible)
        {
            _bar.Hide();
        }
        else
        {
            // 每次显示前校准到工作区内（防多屏拔插/分辨率变化后跑出屏幕），再刷新一次数据
            _bar.EnsureOnScreen();
            _bar.Show();
        }
    }

    private void ExitApp()
    {
        _isExiting = true;
        _powerHistory.Save();
        SaveWindowState();
        AccountRuntime();          // 退出前把最后一段运行时长入账
        _runtime.Save();
        _bar?.Close();
        _bar = null;
        _netWidget?.Dispose();
        _netWidget = null;
        _cpu?.Dispose();
        _cpu = null;
        _appIcon?.Dispose();
        _appIcon = null;
        SystemEvents.UserPreferenceChanged -= OnUserPreferenceChanged;
        foreach (WF.NotifyIcon icon in _icons)
        {
            icon.Visible = false;
            icon.Dispose();
        }
        foreach (Drawing.Icon? icon in _lastIcons)
            icon?.Dispose();
        _service.Dispose();
        Close();
    }

    private void RestoreWindowState()
    {
        bool posValid = !double.IsNaN(_settings.WindowLeft) && !double.IsNaN(_settings.WindowTop);

        // 校验窗口位置在可见屏幕范围内（防止多显示器拔出后窗口跑到屏幕外）
        if (posValid)
        {
            double vsLeft = SystemParameters.VirtualScreenLeft;
            double vsTop = SystemParameters.VirtualScreenTop;
            double vsRight = vsLeft + SystemParameters.VirtualScreenWidth;
            double vsBottom = vsTop + SystemParameters.VirtualScreenHeight;

            posValid = _settings.WindowLeft >= vsLeft && _settings.WindowLeft < vsRight - 80 &&
                       _settings.WindowTop >= vsTop && _settings.WindowTop < vsBottom - 40;
        }

        if (posValid)
        {
            Left = _settings.WindowLeft;
            Top = _settings.WindowTop;
        }

        Width = _settings.WindowWidth;
        Height = _settings.WindowHeight;
        if (_settings.WindowMaximized)
            WindowState = WindowState.Maximized;
    }

    private void SaveWindowState()
    {
        if (WindowState == WindowState.Normal)
        {
            _settings.WindowLeft = Left;
            _settings.WindowTop = Top;
            _settings.WindowWidth = Width;
            _settings.WindowHeight = Height;
            _settings.WindowMaximized = false;
        }
        else
        {
            _settings.WindowMaximized = true;
        }
        _settings.Save();
    }

    private void OnClosing(object? sender, System.ComponentModel.CancelEventArgs e)
    {
        if (!_isExiting)
        {
            e.Cancel = true;
            Hide();
            SaveWindowState(); // 隐藏到托盘时也保存窗口状态，避免隐藏后关机丢失

            // 首次隐藏时给一次气泡提示，避免用户误以为程序已退出（只提示一次）
            if (!_settings.NotifiedHideOnce)
            {
                _settings.NotifiedHideOnce = true;
                _settings.Save();
                _icons[0]?.ShowBalloonTip(3000, "已最小化到托盘",
                    "程序仍在后台监控。双击托盘图标可恢复窗口，右键图标可退出。",
                    WF.ToolTipIcon.Info);
            }
        }
    }

    private void OnSettings(object sender, RoutedEventArgs e) => OpenSettings();

    private void OpenSettings()
    {
        SettingsWindow w = new(_settings) { Owner = this };
        w.ShowDialog();
        _timer.Interval = TimeSpan.FromMilliseconds(_settings.RefreshIntervalMs);
    }

    /// <summary>运行时长文案：不足 1 小时显示分钟，避免"0.0h"的尴尬。</summary>
    private static string HoursText(double minutes) =>
        minutes < 60 ? $"{minutes:0} 分钟" : $"{minutes / 60.0:0.0} 小时";

    /// <summary>运行时长记账：每 60 秒把时间片写入 RuntimeStore 并落盘。</summary>
    private void AccountRuntime()
    {
        if ((DateTime.Now - _lastRuntimeAccount).TotalSeconds < 60)
            return;

        double minutes = (DateTime.Now - _lastRuntimeAccount).TotalMinutes;
        _lastRuntimeAccount = DateTime.Now;
        _runtime.Add(DateTime.Now, minutes);
        _runtime.Save();
    }

    private void OnTick()
    {
        AccountRuntime();
        MonitoringSnapshot? s = Refresh();

        if (s != null)
        {
            UpdateChart();
            UpdateTray(s);

            // 监控长条可见时同步刷新（共享同一次快照，避免二次采样）
            if (_bar?.IsVisible == true)
                _bar.UpdateData(s, _settings, _net);

            // 每 60 秒记录一次四指标采样（真实时间间隔，与刷新周期解耦；某项读取失败为 null 时该项跳过）
            if ((DateTime.Now - _lastPowerSample).TotalSeconds >= 60)
            {
                _powerHistory.AddSample(s.CpuPower.Current, s.CpuTemperature.Current, s.FanRpm.Current, s.MemoryLoad.Current);
                _powerHistory.Save();
                _lastPowerSample = DateTime.Now;
            }
        }
    }

    private MonitoringSnapshot? Refresh()
    {
        try
        {
            MonitoringSnapshot s = _service.Read();
            _net.Sample(); // 网速采样（1s 间隔差分）
            _history.Add(s.CpuPower.Current, s.CpuTemperature.Current, s.FanRpm.Current, s.MemoryLoad.Current,
                (float?)_net.DownKbps, (float?)_net.UpKbps);

            SetCard(PowerValue, PowerSub, s.CpuPower, "0.0");
            SetCard(TempValue, TempSub, s.CpuTemperature, "0");
            UpdateTempAlert(s.CpuTemperature.Current);
            SetCard(FanValue, FanSub, s.FanRpm, "0");
            SetCard(MemValue, MemSub, s.MemoryLoad, "0");
            UpdateNetDisplay();

            double sessionMinutes = (DateTime.Now - _startedAt).TotalMinutes;
            double unaccounted = (DateTime.Now - _lastRuntimeAccount).TotalMinutes;   // 尚未入账的尾数
            ClockText.Text = $"{DateTime.Now:yyyy-MM-dd HH:mm:ss}    已运行 {(DateTime.Now - _startedAt):hh\\:mm\\:ss}"
                + $"    今日 {HoursText(_runtime.GetMinutes(DateTime.Now) + unaccounted)}"
                + $" · 累计 {HoursText(_runtime.TotalMinutes + unaccounted)}";
            return s;
        }
        catch (Exception ex)
        {
            try
            {
                File.AppendAllText(@"C:\Users\<用户名>\Desktop\机械革命监控\widget_debug.log",
                    $"{DateTime.Now:HH:mm:ss.fff} refresh error: {ex}\r\n");
            }
            catch
            {
                // 日志失败不影响运行
            }
            PowerValue.Text = TempValue.Text = FanValue.Text = MemValue.Text = "--";
            ClockText.Text = "读取失败: " + ex.Message;
            return null;
        }
    }

    private static void SetCard(TextBlock value, TextBlock sub, Metric m, string fmt)
    {
        value.Text = m.Current?.ToString(fmt) ?? "--";
        sub.Text = $"最低 {F(m.Min, fmt)} · 最高 {F(m.Max, fmt)} · 平均 {F(m.Average, fmt)}";
    }

    private static string F(float? v, string fmt) => v?.ToString(fmt) ?? "--";

    /// <summary>更新标题栏的实时网速（↓ 下行 / ↑ 上行），tooltip 附会话累计流量。</summary>
    private void UpdateNetDisplay()
    {
        NetDownText.Text = "↓ " + NetworkSpeedMeter.FormatText(_net.DownKbps);
        NetUpText.Text = "↑ " + NetworkSpeedMeter.FormatText(_net.UpKbps);
        NetDownText.ToolTip = $"下行：{NetworkSpeedMeter.FormatText(_net.DownKbps)}\n会话累计下载：{_net.TotalDownMB:0.0} MB";
        NetUpText.ToolTip = $"上行：{NetworkSpeedMeter.FormatText(_net.UpKbps)}\n会话累计上传：{_net.TotalUpMB:0.0} MB";
    }

    /// <summary>温度超阈值时，卡片数值与标题给出颜色与文字告警。</summary>
    private void UpdateTempAlert(float? temp)
    {
        if (temp is not { } t || t < _settings.WarnTemperature)
        {
            TempValue.Foreground = BrushTempNormal;
            TempDot.Fill = BrushTempNormal;
            TempTitle.Text = "CPU 温度 (℃)";
        }
        else if (t < _settings.CriticalTemperature)
        {
            TempValue.Foreground = BrushTempWarn;
            TempDot.Fill = BrushTempWarn;
            TempTitle.Text = "CPU 温度 (℃) ⚠ 偏高";
        }
        else
        {
            TempValue.Foreground = BrushTempCrit;
            TempDot.Fill = BrushTempCrit;
            TempTitle.Text = "CPU 温度 (℃) ‼ 危险";
        }
    }

    // 是否需要在下一次刷新时自动适配坐标（启动 / 切换指标 / 切换周期时为 true；
    // 用户滚轮缩放或拖拽平移期间保持 false，每秒刷新不会把视图弹回）
    private bool _autoScaleNeeded = true;

    private void UpdateChart()
    {
        // 非自动适配时，先记住用户当前视图，刷新后原样恢复，保证缩放/平移不被每秒重绘打断
        ScottPlot.AxisLimits saved = default;
        bool preserveView = !_autoScaleNeeded;
        if (preserveView)
            saved = Plot.Plot.Axes.GetLimits();

        Plot.Plot.Clear();

        List<(DateTime Time, float Value)> series = _history.GetSeries(_metric, _period);
        Plot.Plot.Axes.Left.Label.Text = MetricLabel(_metric);
        if (series.Count >= 2)
        {
            double[] xs = series.Select(s => s.Time.ToOADate()).ToArray();
            double[] ys = series.Select(s => (double)s.Value).ToArray();

            ScottPlot.Plottables.Scatter sig = Plot.Plot.Add.Scatter(xs, ys);
            ScottPlot.Color line = MetricColor(_metric);
            sig.Color = line;
            sig.LineWidth = 2.2f;
            sig.MarkerSize = 0;
            // 曲线下方到 0 轴的淡色填充，层次更柔和
            double[] baseline = new double[xs.Length];
            ScottPlot.Plottables.FillY fill = Plot.Plot.Add.FillY(xs, ys, baseline);
            fill.FillColor = new ScottPlot.Color(line.R, line.G, line.B, 28);
            fill.LineWidth = 0;

            if (_autoScaleNeeded)
                Plot.Plot.Axes.AutoScale();
        }
        else if (_autoScaleNeeded)
        {
            // 无数据时给出真实时间窗与合理纵轴，避免出现 1899 年等异常坐标（仅自动适配时设置一次）
            DateTime end = DateTime.Now;
            DateTime start = end - _period;
            (double y0, double y1) = MetricDefaultRange(_metric);
            Plot.Plot.Axes.SetLimits(start.ToOADate(), end.ToOADate(), y0, y1);
        }

        if (preserveView)
            Plot.Plot.Axes.SetLimits(saved.Left, saved.Right, saved.Bottom, saved.Top);

        _autoScaleNeeded = false;
        Plot.Refresh();
    }

    /// <summary>无数据时各指标的默认纵轴范围，保证空图也美观可信。</summary>
    private static (double Min, double Max) MetricDefaultRange(MetricKind kind) => kind switch
    {
        MetricKind.Power => (0, 45),
        MetricKind.Temperature => (30, 90),
        MetricKind.Fan => (0, 6000),
        MetricKind.Memory => (0, 100),
        _ => (0, 1024),
    };

    private static ScottPlot.Color MetricColor(MetricKind kind) => kind switch
    {
        MetricKind.Power => ScottPlot.Color.FromHex("#EA580C"),
        MetricKind.Temperature => ScottPlot.Color.FromHex("#D97706"),
        MetricKind.Fan => ScottPlot.Color.FromHex("#0D9488"),
        MetricKind.Memory => ScottPlot.Color.FromHex("#DB2777"),
        MetricKind.NetworkDown => ScottPlot.Color.FromHex("#2563EB"),
        MetricKind.NetworkUp => ScottPlot.Color.FromHex("#7C3AED"),
        _ => ScottPlot.Color.FromHex("#EA580C"),
    };

    private static string MetricLabel(MetricKind kind) => kind switch
    {
        MetricKind.Power => "功耗 (W)",
        MetricKind.Temperature => "温度 (℃)",
        MetricKind.Fan => "风扇 (RPM)",
        MetricKind.Memory => "内存 (%)",
        MetricKind.NetworkDown => "下行 (KB/s)",
        MetricKind.NetworkUp => "上行 (KB/s)",
        _ => "功耗 (W)",
    };

    private void OnMetricClick(object sender, RoutedEventArgs e)
    {
        if (sender is Button b && b.Tag is string tag && Enum.TryParse(tag, out MetricKind kind))
        {
            _metric = kind;
            HighlightButtons();
            _autoScaleNeeded = true;
            UpdateChart();
        }
    }

    private void OnPeriodClick(object sender, RoutedEventArgs e)
    {
        if (sender is Button b && b.Tag is string tag && int.TryParse(tag, out int sec))
        {
            _period = TimeSpan.FromSeconds(sec);
            HighlightButtons();
            _autoScaleNeeded = true;
            UpdateChart();
        }
    }

    // 切换片选中/未选中配色（Fluent 蓝填充 vs 浅灰底）
    private static readonly SolidColorBrush ChipSelectedBg = new(System.Windows.Media.Color.FromRgb(0x25, 0x63, 0xEB));
    private static readonly SolidColorBrush ChipSelectedFg = new(System.Windows.Media.Color.FromRgb(0xFF, 0xFF, 0xFF));
    private static readonly SolidColorBrush ChipIdleBg = new(System.Windows.Media.Color.FromRgb(0xED, 0xF0, 0xF4));
    private static readonly SolidColorBrush ChipIdleFg = new(System.Windows.Media.Color.FromRgb(0x4B, 0x55, 0x63));

    private void HighlightButtons()
    {
        Button[] metricBtns = [BtnPower, BtnTemp, BtnFan, BtnMem, BtnNetDown, BtnNetUp];
        foreach (Button b in metricBtns)
            SetChip(b, (b.Tag as string) == _metric.ToString());

        Button[] periodBtns = [Btn1m, Btn5m, Btn15m, Btn1h, Btn4h, Btn1d];
        foreach (Button b in periodBtns)
            SetChip(b, (b.Tag as string) == ((int)_period.TotalSeconds).ToString());
    }

    private static void SetChip(Button b, bool selected)
    {
        b.Background = selected ? ChipSelectedBg : ChipIdleBg;
        b.Foreground = selected ? ChipSelectedFg : ChipIdleFg;
    }

    private void UpdateTray(MonitoringSnapshot s)
    {
        RefreshTrayEnvironment();
        UpdateTrayIcons(s);
    }

    // —— 托盘图标配色 v6（高对比实底徽章）：圆角实色块 + 纯白粗体数字，色相延续旧版身份色。
    // 浅色任务栏用 700 级深底、深色任务栏用 600 级亮底 + 提亮描边，对比度不依赖任务栏底色。
    // 配色与绘制统一在 TrayIconRenderer 中实现。

    /// <summary>各监控项独立托盘图标，逐项展示（按设置开关显示/隐藏）。图标按任务栏实际尺寸绘制，并随系统主题自动适配配色。</summary>
    private void UpdateTrayIcons(MonitoringSnapshot s)
    {
        bool light = _lightSystemTheme;
        float temp = s.CpuTemperature.Current ?? 0;
        Drawing.Color tempBadge = TrayIconRenderer.TempBadge(temp, _settings.WarnTemperature, _settings.CriticalTemperature, light);

        UpdateNetIcon();   // 先刷新任务栏小组件（同时确定 Available 状态）

        if (_netWidget is { Available: true })
        {
            // 管家式排布：内存圆环 + 网速文本 + 温度/功耗/风扇文本对（温度与内存位置已对换）
            var gauges = new List<NetSpeedWidget.GaugeItem>();
            var pairs = new List<NetSpeedWidget.TextPair>();

            if (_settings.ShowMemoryLoad)
            {
                float mem = s.MemoryLoad.Current ?? 0;
                gauges.Add(new(FInt(s.MemoryLoad.Current), Math.Clamp(mem / 100f, 0f, 1f),
                    TrayIconRenderer.TempBadge(mem, 60, 85, light), Tip("内存占用", s.MemoryLoad, "0", "%")));
            }
            // CPU 占用（紧跟网速，管家的排布位置）
            if (_settings.ShowCpuUsage && _cpu != null)
            {
                float cpuUsage = _cpu.Next();
                pairs.Add(new(FInt(cpuUsage) + "%", "CPU",
                    TrayIconRenderer.TempBadge(cpuUsage, 60, 85, light),
                    $"CPU 占用 {cpuUsage:0}%（上次刷新至今的平均值）"));
            }
            if (_settings.ShowCpuTemperature)
                pairs.Add(new(FInt(s.CpuTemperature.Current) + "℃", "温度", tempBadge,
                    Tip("CPU 温度", s.CpuTemperature, "0", "℃")));
            if (_settings.ShowCpuPower)
            {
                float watt = s.CpuPower.Current ?? 0;
                pairs.Add(new(FInt(s.CpuPower.Current) + "W", "功耗",
                    TrayIconRenderer.TempBadge(watt, 60, 90, light), Tip("CPU 功耗", s.CpuPower, "0.0", "W")));
            }
            if (_settings.ShowFanRpm)
            {
                float rpm = s.FanRpm.Current ?? 0;
                pairs.Add(new(FInt(s.FanRpm.Current), "风扇",
                    TrayIconRenderer.TempBadge(rpm, 4500, 5500, light), Tip("风扇转速", s.FanRpm, "0", "RPM")));
            }

            _netWidget.UpdatePairs(pairs);
            _netWidget.UpdateGauges(gauges);
            return;
        }

        // 兜底：任务栏窗口嵌入失败时，托盘不显示监控图标，仅悬停提示保留概览
        if (_appIcon != null)
            _appIcon.Text = SafeToolTip(
                $"温度 {FInt(s.CpuTemperature.Current)}℃ · 功耗 {FInt(s.CpuPower.Current)}W · 内存 {FInt(s.MemoryLoad.Current)}% · 风扇 {CompactRpm(s.FanRpm.Current)}");
    }

    /// <summary>网络速率显示：优先用管家式任务栏文本小组件（不受方形图标限制，最清晰），
    /// 嵌入失败（安全软件拦截/开始菜单展开等）时自动回退为方形网速图标。</summary>
    private void UpdateNetIcon()
    {
        WF.NotifyIcon icon = _icons[NetIconIndex];

        if (!_settings.ShowNetworkSpeed)
        {
            icon.Visible = false;
            _netWidget?.HideWidget();
            return;
        }

        string up = CompactNet(_net.UpKbps);
        string down = CompactNet(_net.DownKbps);
        string key = $"{up}|{down}";
        string tip =
            $"↑ 上行 {CompactText(_net.UpKbps)} · 会话 {_net.TotalUpMB:0.0}MB\n" +
            $"↓ 下行 {CompactText(_net.DownKbps)} · 会话 {_net.TotalDownMB:0.0}MB";

        if (_netWidget != null)
        {
            _netWidget.ShowWidget();
            _netWidget.ApplyTheme(_lightSystemTheme);
            _netWidget.Update(_net.UpKbps, _net.DownKbps, SafeToolTip(tip));
            if (_netWidget.Available)
            {
                icon.Visible = false;
                _netIconCacheKey = null;   // 兜底图标下次启用时强制重绘
                return;
            }
        }

        // 兜底：方形网速图标（双胶囊）
        if (_netIconCacheKey != key)
        {
            Drawing.Icon newIcon = TrayIconRenderer.CreateNetIcon(up, down,
                TrayIconRenderer.NetUpBadge(_lightSystemTheme), TrayIconRenderer.NetDownBadge(_lightSystemTheme),
                _trayIconSize, _lightSystemTheme);
            _lastIcons[NetIconIndex]?.Dispose();
            _lastIcons[NetIconIndex] = newIcon;
            icon.Icon = newIcon;
            _netIconCacheKey = key;
        }

        icon.Text = SafeToolTip(tip);
        icon.Visible = true;
    }

    /// <summary>网速紧凑短格式（≤4 字符，用于合显图标行内）：KB/s 取整加 K，MB/s 保留一位小数加 M。</summary>
    private static string CompactNet(double kbps)
    {
        (double num, string unit) = NetworkSpeedMeter.Format(kbps);
        if (unit == "MB/s")
            return num >= 10 ? num.ToString("0") + "M" : num.ToString("0.0") + "M";
        return num.ToString("0") + "K";
    }

    /// <summary>紧凑文本：单位不加空格（1.2MB/s），用于悬停提示压缩行宽。</summary>
    private static string CompactText(double kbps)
    {
        (double num, string unit) = NetworkSpeedMeter.Format(kbps);
        return (unit == "MB/s" ? num.ToString("0.0") : num.ToString("0")) + unit;
    }

    /// <summary>托盘悬浮提示：第一行当前值，第二行统计（平均值/最低值/最高值）。</summary>
    private static string Tip(string name, Metric m, string fmt, string unit) =>
        $"{name} {F(m.Current, fmt)} {unit}\n平均 {F(m.Average, fmt)} · 最低 {F(m.Min, fmt)} · 最高 {F(m.Max, fmt)}";

    private void UpdateIcon(int idx, bool visible, string text, Drawing.Color fg, string tip)
    {
        WF.NotifyIcon icon = _icons[idx];

        if (!visible)
        {
            icon.Visible = false;
            return;
        }

        // 先确保 Icon 已绘制完成（数字/颜色变化时才重绘），再设置 tooltip，最后显示。
        // 顺序很关键：必须在 Visible=true 之前设置 Icon，否则 NIM_ADD 会用空图标注册，
        // 导致 Windows 把多个图标识别混乱、在托盘里"连在一起"、无法单独拖动。
        if (_iconCache[idx] is not { } c || c.Text != text || c.Color != fg ||
            c.PixelSize != _trayIconSize || c.LightTheme != _lightSystemTheme)
        {
            Drawing.Icon newIcon = TrayIconRenderer.CreateMetricIcon(text, fg, _trayIconSize, _lightSystemTheme);
            _lastIcons[idx]?.Dispose();
            _lastIcons[idx] = newIcon;
            icon.Icon = newIcon;
            _iconCache[idx] = (text, fg, _trayIconSize, _lightSystemTheme);
        }

        icon.Text = SafeToolTip(tip);
        icon.Visible = true;
    }

    private static string FInt(float? v) => v?.ToString("0") ?? "--";

    /// <summary>风扇转速托盘短格式：≥1000 转显示 x.xK（如 2338→2.3K），让四位数也能大字显示；精确值在悬停提示里。</summary>
    private static string CompactRpm(float? v)
    {
        if (v is not { } r)
            return "--";
        if (r >= 1000f)
            return (r / 1000f).ToString("0.0") + "K";
        return ((int)Math.Round(r)).ToString();
    }

    private void OnUserPreferenceChanged(object sender, UserPreferenceChangedEventArgs e)
    {
        try
        {
            Dispatcher.BeginInvoke(() => RefreshTrayEnvironment(force: true));
        }
        catch (InvalidOperationException)
        {
            // 应用正在退出，Dispatcher 已关闭。
        }
    }

    private void RefreshTrayEnvironment(bool force = false)
    {
        uint dpi = 96;
        try
        {
            IntPtr taskbar = FindWindow("Shell_TrayWnd", null);
            dpi = taskbar != IntPtr.Zero ? GetDpiForWindow(taskbar) : GetDpiForSystem();
        }
        catch (Exception ex) when (ex is EntryPointNotFoundException or DllNotFoundException)
        {
            dpi = 96;
        }

        int size;
        try
        {
            size = GetSystemMetricsForDpi(SmCxSmIcon, dpi);
        }
        catch (Exception ex) when (ex is EntryPointNotFoundException or DllNotFoundException)
        {
            size = Math.Max(16, (int)Math.Round(16 * dpi / 96d));
        }

        if (size <= 0)
            size = Math.Max(16, (int)Math.Round(16 * dpi / 96d));

        bool lightTheme = IsLightSystemTheme();
        if (!force && size == _trayIconSize && lightTheme == _lightSystemTheme)
            return;

        _trayIconSize = Math.Clamp(size, 16, 64);
        _lightSystemTheme = lightTheme;
        Array.Clear(_iconCache, 0, _iconCache.Length);
        _netIconCacheKey = null;
        _netWidget?.ApplyTheme(lightTheme);
        _netWidget?.Reposition();   // DPI/显示变化时按新参数重新定位一次
    }

    private static bool IsLightSystemTheme()
    {
        try
        {
            using RegistryKey? key = Registry.CurrentUser.OpenSubKey(
                @"Software\Microsoft\Windows\CurrentVersion\Themes\Personalize");
            return key?.GetValue("SystemUsesLightTheme") is int value && value != 0;
        }
        catch
        {
            return true;
        }
    }

    private static string SafeToolTip(string text)
    {
        const int maxLength = 63;
        string normalized = text.Replace("\r\n", "\n").Replace('\r', '\n');
        return normalized.Length <= maxLength ? normalized : normalized[..(maxLength - 1)] + "…";
    }

    // 日历
    private void OnPrevMonth(object sender, RoutedEventArgs e)
    {
        _calMonth--;
        if (_calMonth < 1)
        {
            _calMonth = 12;
            _calYear--;
        }
        BuildCalendar(_calYear, _calMonth);
    }

    private void OnNextMonth(object sender, RoutedEventArgs e)
    {
        _calMonth++;
        if (_calMonth > 12)
        {
            _calMonth = 1;
            _calYear++;
        }
        BuildCalendar(_calYear, _calMonth);
    }

    private void BuildCalendar(int year, int month)
    {
        CalendarGrid.Children.Clear();
        string[] week = ["日", "一", "二", "三", "四", "五", "六"];
        DateTime today = DateTime.Now;
        bool isCurMonth = today.Year == year && today.Month == month;

        // 星期表头：今日所在列以青色标识
        for (int ci = 0; ci < week.Length; ci++)
        {
            bool isTodayCol = isCurMonth && ci == (int)today.DayOfWeek;
            CalendarGrid.Children.Add(new TextBlock
            {
                Text = week[ci],
                HorizontalAlignment = System.Windows.HorizontalAlignment.Center,
                VerticalAlignment = System.Windows.VerticalAlignment.Center,
                FontSize = 11,
                FontWeight = isTodayCol ? FontWeights.SemiBold : FontWeights.Normal,
                Foreground = new SolidColorBrush(isTodayCol ? ColorFromHex("#2563EB") : ColorFromHex("#8A94A3")),
                Margin = new Thickness(0, 0, 0, 2),
            });
        }

        int firstDay = (int)new DateTime(year, month, 1).DayOfWeek;
        int days = DateTime.DaysInMonth(year, month);

        for (int i = 0; i < firstDay; i++)
            CalendarGrid.Children.Add(new Border());

        for (int day = 1; day <= days; day++)
        {
            DateTime dt = new(year, month, day);
            bool isToday = isCurMonth && day == today.Day;
            bool isFuture = dt > today;
            DailyPower? dp = _powerHistory.GetDay(dt);
            bool hasData = dp is { SampleCount: > 0 };
            string dateStr = $"{year}-{month:00}-{day:00}";

            // 静态背景：有数据→功耗色带；过去无记录→空态浅底；未来→更浅
            System.Windows.Media.Color baseColor = isFuture
                ? ColorFromHex("#F1F4F8")
                : hasData ? PowerColor(dp!.Average) : ColorFromHex("#F4F6FA");

            // 热力格文字色：高功耗红底用白字，其余用深灰保证可读
            System.Windows.Media.Color dataFg = (hasData && dp!.Average >= 45f)
                ? ColorFromHex("#FFFFFF")
                : ColorFromHex("#334155");

            TextBlock tb = new()
            {
                Text = day.ToString(),
                HorizontalAlignment = System.Windows.HorizontalAlignment.Center,
                VerticalAlignment = System.Windows.VerticalAlignment.Center,
                FontSize = 12,
                FontWeight = isToday ? FontWeights.Bold : FontWeights.Normal,
                Foreground = new SolidColorBrush(
                    isFuture ? ColorFromHex("#B6BFCC")
                    : isToday ? ColorFromHex("#1D4ED8")
                    : hasData ? dataFg : ColorFromHex("#9AA4B2")),
            };

            Border b = new()
            {
                Background = new SolidColorBrush(baseColor),
                CornerRadius = new CornerRadius(6),
                Margin = new Thickness(2),
                Child = tb,
                Tag = dateStr,
            };

            // 今日：主题蓝描边
            if (isToday)
            {
                b.BorderBrush = new SolidColorBrush(ColorFromHex("#2563EB"));
                b.BorderThickness = new Thickness(1.5);
            }

            // 未来日期不可交互
            if (isFuture)
            {
                b.IsHitTestVisible = false;
                CalendarGrid.Children.Add(b);
                continue;
            }

            // 可点击日期：手型光标 + 悬停提亮 + 信息提示
            b.Cursor = System.Windows.Input.Cursors.Hand;
            b.ToolTip = hasData
                ? $"当日平均功耗 {dp!.Average:0.0} W · 点击查看四指标"
                : "当日暂无记录（程序运行期间每 60 秒采样）";
            b.MouseLeftButtonDown += (_, _) => ShowDayStats(dateStr);

            System.Windows.Media.Color hover = Lighten(baseColor, hasData ? 0.10f : 0.05f);
            b.MouseEnter += (_, _) =>
            {
                b.Background = new SolidColorBrush(hover);
                b.BorderBrush = new SolidColorBrush(ColorFromHex("#A62563EB")); // 半透蓝
                b.BorderThickness = new Thickness(1);
            };
            b.MouseLeave += (_, _) =>
            {
                b.Background = new SolidColorBrush(baseColor);
                b.BorderBrush = isToday ? new SolidColorBrush(ColorFromHex("#2563EB")) : System.Windows.Media.Brushes.Transparent;
                b.BorderThickness = isToday ? new Thickness(1.5) : new Thickness(0);
            };

            CalendarGrid.Children.Add(b);
        }

        CalendarTitle.Text = isCurMonth ? $"{year} 年 {month} 月（本月）" : $"{year} 年 {month} 月";
    }

    private void ShowDayStats(string date)
    {
        DailyPower? dp = _powerHistory.GetDay(DateTime.Parse(date));
        if (dp == null || dp.SampleCount == 0)
        {
            System.Windows.MessageBox.Show(
                $"{date}\n\n当日暂无记录。\n程序运行期间每 60 秒自动记录一次（CPU 功耗 / 温度 / 风扇 / 内存），请保持程序运行后再查看。",
                "监控记录", MessageBoxButton.OK, MessageBoxImage.Information);
            return;
        }

        DayStatsWindow w = new(date, dp) { Owner = this };
        w.ShowDialog();
    }

    // 日历功耗色带（冷→暖 = 低→高，浅色主题），与界面底部渐变图例一致
    private static readonly (byte R, byte G, byte B)[] PowerStops =
    {
        (0xE3, 0xF0, 0xFB), // 低功耗：浅蓝
        (0xC6, 0xE9, 0xD4), // 浅绿
        (0xFC, 0xEC, 0xB0), // 浅黄
        (0xFD, 0xC0, 0x8A), // 橙
        (0xF2, 0x6D, 0x6D), // 高功耗：红
    };

    private static System.Windows.Media.Color PowerColor(float? avg)
    {
        if (avg is not { } a || a <= 0)
            return ColorFromHex("#F4F6FA");

        float t = Math.Clamp(a / 60f, 0f, 1f);
        float pos = t * (PowerStops.Length - 1);
        int i = (int)Math.Floor(pos);
        if (i >= PowerStops.Length - 1)
            return ColorFromTuple(PowerStops[^1]);

        (byte R, byte G, byte B) c0 = PowerStops[i], c1 = PowerStops[i + 1];
        float k = pos - i;
        return System.Windows.Media.Color.FromRgb(
            (byte)(c0.R + (c1.R - c0.R) * k),
            (byte)(c0.G + (c1.G - c0.G) * k),
            (byte)(c0.B + (c1.B - c0.B) * k));
    }

    private static System.Windows.Media.Color ColorFromTuple((byte R, byte G, byte B) c) =>
        System.Windows.Media.Color.FromRgb(c.R, c.G, c.B);

    private static System.Windows.Media.Color ColorFromHex(string hex) =>
        (System.Windows.Media.Color)System.Windows.Media.ColorConverter.ConvertFromString(hex);

    private static System.Windows.Media.Color Lighten(System.Windows.Media.Color c, float amount) =>
        System.Windows.Media.Color.FromRgb(
            (byte)(c.R + (255 - c.R) * amount),
            (byte)(c.G + (255 - c.G) * amount),
            (byte)(c.B + (255 - c.B) * amount));
}
