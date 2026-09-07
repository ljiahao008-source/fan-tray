using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Threading;
// 项目全局引入了 System.Drawing，显式绑定到 WPF 类型避免歧义
using Brush = System.Windows.Media.Brush;
using Brushes = System.Windows.Media.Brushes;
using Button = System.Windows.Controls.Button;
using CheckBox = System.Windows.Controls.CheckBox;
using Color = System.Windows.Media.Color;
using Cursors = System.Windows.Input.Cursors;
using HorizontalAlignment = System.Windows.HorizontalAlignment;
using Point = System.Windows.Point;

namespace MonitoringApp;

/// <summary>
/// 运行中应用面板（管家样式，单击任务栏圆盘弹出）：
/// 深色圆角面板列出正在运行的进程（名称 + 内存占用条，按内存降序），
/// 头部显示整体内存使用，底部按钮结束勾选的应用。
/// 单例：重复 Toggle 即关闭；2.5 秒自动刷新列表。
/// </summary>
public sealed class RunningAppsPanel : Window
{
    private static RunningAppsPanel? _active;

    /// <summary>在任务栏小组件锚点旁打开面板；已打开则关闭（来回切换）。</summary>
    public static void Toggle((int X, int Y, int W, int H)? anchor)
    {
        if (_active is { } panel)
        {
            panel.Close();
            return;
        }

        _active = new RunningAppsPanel(anchor ?? (0, 0, 200, 48));
        _active.Show();
        _active.InstallHook();
        Log($"panel shown at ({_active.Left:0},{_active.Top:0}) {_active.ActualWidth:0}x{_active.ActualHeight:0}");
    }

    private static readonly string LogPath = @"C:\Users\<用户名>\Desktop\机械革命监控\widget_debug.log";

    private static void Log(string message)
    {
        try
        {
            File.AppendAllText(LogPath, $"{DateTime.Now:HH:mm:ss.fff} [panel] {message}\r\n");
        }
        catch
        {
            // 日志失败不影响运行
        }
    }

    private readonly (int X, int Y, int W, int H) _anchor;
    private readonly StackPanel _list = new();
    private readonly TextBlock _summary = new();
    private readonly DispatcherTimer _refresh;
    private readonly HashSet<int> _checked = new();   // 勾选待结束的应用 pid
    private IntPtr _hook;
    private HookProc? _hookProc;
    private (int X, int Y, int W, int H) _pxRect;     // 面板屏幕像素矩形（钩子判定用）
    private static readonly Dictionary<string, (Brush, string)> DotCache = new();

    private const int WH_MOUSE_LL = 14;
    private const int WM_LBUTTONDOWN = 0x0201;

    private delegate IntPtr HookProc(int nCode, IntPtr wParam, IntPtr lParam);

    [StructLayout(LayoutKind.Sequential)]
    private struct WindowsPoint
    {
        public int X;
        public int Y;
    }

    [DllImport("user32.dll")]
    private static extern IntPtr SetWindowsHookEx(int idHook, HookProc lpfn, IntPtr hMod, uint dwThreadId);

    [DllImport("user32.dll")]
    private static extern bool UnhookWindowsHookEx(IntPtr hhk);

    [DllImport("user32.dll")]
    private static extern IntPtr CallNextHookEx(IntPtr hhk, int nCode, IntPtr wParam, IntPtr lParam);

    [StructLayout(LayoutKind.Sequential)]
    private struct MSLLHOOKSTRUCT
    {
        public WindowsPoint pt;
        public uint mouseData;
        public uint flags;
        public uint time;
        public nuint dwExtraInfo;
    }

    /// <summary>低级鼠标钩子：面板打开期间点击面板/小组件以外任意位置即关闭（popup 行为）。</summary>
    private void InstallHook()
    {
        _hookProc = HookCallback;
        _hook = SetWindowsHookEx(WH_MOUSE_LL, _hookProc, IntPtr.Zero, 0);
    }

    private void UninstallHook()
    {
        if (_hook != IntPtr.Zero)
        {
            UnhookWindowsHookEx(_hook);
            _hook = IntPtr.Zero;
        }
        _hookProc = null;
    }

    private IntPtr HookCallback(int nCode, IntPtr wParam, IntPtr lParam)
    {
        if (nCode >= 0 && wParam.ToInt64() == WM_LBUTTONDOWN)
        {
            var info = Marshal.PtrToStructure<MSLLHOOKSTRUCT>(lParam);
            bool insideSelf = IsInside(_pxRect, info.pt.X, info.pt.Y);   // 面板自身
            if (!insideSelf && !IsInside(_anchor, info.pt.X, info.pt.Y) && !IsInsideAnchor(info.pt.X, info.pt.Y))
                Dispatcher.BeginInvoke(Close);   // 点在面板、小组件之外 → 关闭（面板内点击放行，勾选才能生效）
        }
        return CallNextHookEx(_hook, nCode, wParam, lParam);
    }

    private static bool IsInside((int X, int Y, int W, int H) rect, int x, int y) =>
        x >= rect.X && x < rect.X + rect.W && y >= rect.Y && y < rect.Y + rect.H;

    /// <summary>小组件会随网速文本宽度微移，圈定其矩形并外扩几像素作为"自己人"区域。</summary>
    private bool IsInsideAnchor(int x, int y)
    {
        const int slack = 8;
        return x >= _anchor.X - slack && x < _anchor.X + _anchor.W + slack
            && y >= _anchor.Y - slack && y < _anchor.Y + _anchor.H + slack;
    }

    /// <summary>显示名缓存（进程名 → 文件描述的友好名称），避免每次刷新重复读版本信息。</summary>
    private static readonly Dictionary<string, string> NameCache = new();

    /// <summary>常见系统进程的中文映射（无文件描述或权限不足时兜底）。</summary>
    private static readonly Dictionary<string, string> SystemNames = new(StringComparer.OrdinalIgnoreCase)
    {
        ["dwm"] = "桌面窗口管理器",
        ["explorer"] = "文件资源管理器",
        ["system"] = "Windows 系统进程",
        ["idle"] = "系统空闲进程",
        ["registry"] = "注册表",
        ["svchost"] = "Windows 服务主进程",
        ["csrss"] = "Windows 系统进程",
        ["wininit"] = "Windows 系统进程",
        ["winlogon"] = "Windows 系统进程",
        ["services"] = "Windows 服务管理",
        ["lsass"] = "Windows 安全进程",
        ["smss"] = "Windows 会话管理",
        ["msedgewebview2"] = "Edge WebView 组件",
        ["msedge"] = "Microsoft Edge 浏览器",
        ["conhost"] = "控制台窗口主机",
        ["runtimebroker"] = "运行时代理",
        ["searchhost"] = "Windows 搜索",
        ["startmenuexperiencehost"] = "开始菜单",
        ["textinputhost"] = "文本输入",
        ["shellexperiencehost"] = "Windows 外壳体验",
        ["applicationframehost"] = "Windows 应用框架",
    };

    /// <summary>取进程的友好显示名：优先可执行文件的「文件描述」（即中文名），失败回退进程名/系统映射。</summary>
    private static string FriendlyName(Process p)
    {
        string key = p.ProcessName;
        if (NameCache.TryGetValue(key, out string? cached))
            return cached;

        string display = key;
        try
        {
            string? path = p.MainModule?.FileName;
            if (!string.IsNullOrEmpty(path))
            {
                FileVersionInfo info = FileVersionInfo.GetVersionInfo(path);
                if (!string.IsNullOrWhiteSpace(info.FileDescription))
                    display = info.FileDescription.Trim();
            }
        }
        catch
        {
            // 系统进程无权限读模块，保留进程名
        }

        if (ReferenceEquals(display, key) && SystemNames.TryGetValue(key, out string? mapped))
            display = mapped;

        NameCache[key] = display;
        return display;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct MEMORYSTATUSEX
    {
        public uint dwLength;
        public uint dwMemoryLoad;
        public ulong ullTotalPhys;
        public ulong ullAvailPhys;
        public ulong ullTotalPageFile;
        public ulong ullAvailPageFile;
        public ulong ullTotalVirtual;
        public ulong ullAvailVirtual;
        public ulong ullAvailExtendedVirtual;
    }

    [DllImport("kernel32.dll")]
    private static extern bool GlobalMemoryStatusEx(ref MEMORYSTATUSEX lpBuffer);

    private RunningAppsPanel((int X, int Y, int W, int H) anchor)
    {
        _anchor = anchor;
        WindowStyle = WindowStyle.None;
        ResizeMode = ResizeMode.NoResize;
        AllowsTransparency = true;
        Background = Brushes.Transparent;
        ShowInTaskbar = false;
        ShowActivated = false;
        Topmost = true;   // 面板必须浮在所有窗口之上（管家样式）
        Width = 320;
        Height = 452;

        BuildLayout();

        // explorer 重启/列表变化时自动刷新
        _refresh = new DispatcherTimer { Interval = TimeSpan.FromSeconds(2.5) };
        _refresh.Tick += (_, _) => Reload();
        _refresh.Start();

        new WindowInteropHelper(this).EnsureHandle();
        Position();
        Closed += (_, _) =>
        {
            _refresh.Stop();
            UninstallHook();
            _checked.Clear();
            if (ReferenceEquals(_active, this))
                _active = null;
        };
        Reload();
    }

    private void BuildLayout()
    {
        Border root = new()
        {
            Background = new SolidColorBrush(Color.FromRgb(0x1E, 0x22, 0x28)),
            BorderBrush = new SolidColorBrush(Color.FromRgb(0x38, 0x40, 0x4A)),
            BorderThickness = new Thickness(1),
            CornerRadius = new CornerRadius(10),
            Padding = new Thickness(12, 10, 12, 12),
        };

        Grid grid = new();
        grid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        grid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        grid.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });
        grid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });

        // 头部：标题 + 关闭
        Grid header = new();
        header.Children.Add(new TextBlock
        {
            Text = "运行中的应用",
            FontSize = 13,
            FontWeight = FontWeights.Bold,
            Foreground = new SolidColorBrush(Color.FromRgb(0xE8, 0xED, 0xF2)),
        });
        Button close = new()
        {
            Content = "✕",
            Width = 26,
            Height = 22,
            Background = Brushes.Transparent,
            BorderThickness = new Thickness(0),
            Foreground = new SolidColorBrush(Color.FromRgb(0x9A, 0xA4, 0xAE)),
            HorizontalAlignment = HorizontalAlignment.Right,
            Cursor = Cursors.Hand,
        };
        close.Click += (_, _) => Close();
        header.Children.Add(close);
        Grid.SetRow(header, 0);
        grid.Children.Add(header);

        // 内存总览
        _summary.FontSize = 11;
        _summary.Foreground = new SolidColorBrush(Color.FromRgb(0x9A, 0xA4, 0xAE));
        _summary.Margin = new Thickness(0, 4, 0, 6);
        Grid.SetRow(_summary, 1);
        grid.Children.Add(_summary);

        // 进程列表：隐藏滚动条（滚轮仍可滚动），避免竖线压住内存数值
        ScrollViewer scroll = new()
        {
            Content = _list,
            VerticalScrollBarVisibility = ScrollBarVisibility.Hidden,
            Margin = new Thickness(0, 0, 2, 0),
        };
        Grid.SetRow(scroll, 2);
        grid.Children.Add(scroll);

        // 结束应用
        Button boost = new()
        {
            Content = "结束勾选的应用",
            Height = 32,
            Margin = new Thickness(0, 10, 0, 0),
            Background = new SolidColorBrush(Color.FromRgb(0x16, 0xA3, 0x4A)),
            Foreground = Brushes.White,
            BorderThickness = new Thickness(0),
            FontSize = 12.5,
            FontWeight = FontWeights.Bold,
            Cursor = Cursors.Hand,
        };
        // 结束勾选的应用；完成后关闭面板
        boost.Click += async (_, _) =>
        {
            int[] kill = _checked.ToArray();
            _checked.Clear();
            if (kill.Length == 0)
                return;

            boost.IsEnabled = false;
            boost.Content = $"正在结束 {kill.Length} 个应用…";

            int ended = await Task.Run(() =>
            {
                foreach (int pid in kill)
                {
                    try
                    {
                        using Process p = Process.GetProcessById(pid);
                        _ = p.CloseMainWindow();   // 先礼貌关闭
                    }
                    catch
                    {
                        // 进程可能已退出
                    }
                }

                Thread.Sleep(900);

                int ended = 0;
                foreach (int pid in kill)
                {
                    try
                    {
                        using Process p = Process.GetProcessById(pid);
                        if (!p.HasExited)
                        {
                            p.Kill();
                            ended++;
                        }
                        else
                        {
                            ended++;
                        }
                    }
                    catch
                    {
                        // 已退出/无权限，跳过
                    }
                }

                return ended;
            });

            ToastWindow.Show("已完成", $"已结束 {ended} 个应用", nearTray: true);
            Close();
        };
        Grid.SetRow(boost, 3);
        grid.Children.Add(boost);

        root.Child = grid;
        Content = root;
    }

    /// <summary>按任务栏小组件锚点定位：右对齐、悬浮于任务栏上方。</summary>
    private void Position()
    {
        double scale = GetDpiScale();
        double leftPx = _anchor.X + _anchor.W - Width * scale;
        double topPx = _anchor.Y - Height * scale - 8;
        Left = Math.Max(4, leftPx / scale);
        Top = Math.Max(4, topPx / scale);
        _pxRect = ((int)Math.Round(Left * scale), (int)Math.Round(Top * scale),
            (int)Math.Round(Width * scale), (int)Math.Round(Height * scale));
    }

    [DllImport("user32.dll")]
    private static extern uint GetDpiForWindow(IntPtr hwnd);

    private double GetDpiScale()
    {
        IntPtr h = new WindowInteropHelper(this).Handle;
        return h != IntPtr.Zero ? GetDpiForWindow(h) / 96.0 : 1.0;
    }

    /// <summary>重建进程列表（内存降序）与内存总览。</summary>
    private void Reload()
    {
        MEMORYSTATUSEX st = new() { dwLength = (uint)Marshal.SizeOf<MEMORYSTATUSEX>() };
        double totalGB = 0, availGB = 0, loadPct = 0;
        try
        {
            if (GlobalMemoryStatusEx(ref st))
            {
                totalGB = st.ullTotalPhys / 1024.0 / 1024 / 1024;
                availGB = st.ullAvailPhys / 1024.0 / 1024 / 1024;
                loadPct = st.dwMemoryLoad;
            }
        }
        catch
        {
            // 忽略，仅无总览行
        }
        _summary.Text = totalGB > 0
            ? $"内存 {totalGB - availGB:0.0} / {totalGB:0.0} GB（占用 {loadPct:0}%）"
            : "内存占用";

        // 按应用聚合：同名进程（如 Chrome/WorkBuddy 的多进程）合并为一行，内存求和
        var groups = new Dictionary<string, (double MB, List<int> Pids)>();
        try
        {
            foreach (Process p in Process.GetProcesses())
            {
                try
                {
                    if (p.Id == Environment.ProcessId || p.SessionId == 0)
                    {
                        p.Dispose();
                        continue;
                    }
                    double mb = p.WorkingSet64 / 1024.0 / 1024;
                    if (mb < 15)
                    {
                        p.Dispose();
                        continue;   // 过滤碎进程
                    }
                    string name = FriendlyName(p);
                    if (!groups.TryGetValue(name, out var entry))
                        groups[name] = entry = (0, new List<int>());
                    entry.MB += mb;
                    entry.Pids.Add(p.Id);
                    groups[name] = entry;
                }
                catch
                {
                    // 权限不足等，跳过
                }
                finally
                {
                    p.Dispose();
                }
            }
        }
        catch
        {
            // 枚举失败保留旧列表
        }

        var rows = groups
            .Select(kv => (Name: kv.Key, MB: kv.Value.MB, Pids: kv.Value.Pids,
                Pct: totalGB > 0 ? kv.Value.MB / 1024.0 / totalGB : 0))
            .OrderByDescending(r => r.MB)
            .Take(30)
            .ToList();

        _checked.RemoveWhere(pid => rows.All(r => !r.Pids.Contains(pid)));   // 清理已消失进程的勾选

        _list.Children.Clear();
        double maxMb = rows.Count > 0 ? Math.Max(64, rows[0].MB) : 1;
        foreach (var r in rows)
        {
            _list.Children.Add(BuildRow(r.Pids, r.Name, r.MB, Math.Clamp(r.MB / maxMb, 0.03, 1.0), r.Pct));
        }
    }

    /// <summary>单行（一个应用的聚合）：勾选框 + 首字圆点 + 应用名 + 内存合计，下方占用条。</summary>
    private FrameworkElement BuildRow(List<int> pids, string name, double mb, double barPct, double sysPct)
    {
        if (!DotCache.TryGetValue(name, out var dot))
        {
            byte hue = (byte)(name.GetHashCode() & 0xFF);
            var bg = new SolidColorBrush(Color.FromRgb((byte)(80 + hue % 120), (byte)(90 + hue * 37 % 130), (byte)(110 + hue * 71 % 120)));
            bg.Freeze();
            dot = (bg, name.Length > 0 ? name[..1].ToUpperInvariant() : "?");
            DotCache[name] = dot;
        }

        Grid row = new();
        row.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        row.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        row.Margin = new Thickness(0, 3, 0, 3);

        Grid line = new();
        line.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        line.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        line.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        line.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });

        CheckBox check = new()
        {
            Width = 15,
            Height = 15,
            VerticalAlignment = VerticalAlignment.Center,
            Margin = new Thickness(0, 0, 6, 0),
            IsChecked = pids.Any(_checked.Contains),
        };
        check.Click += (_, _) =>
        {
            if (check.IsChecked == true)
            {
                foreach (int pid in pids)
                    _checked.Add(pid);
            }
            else
            {
                foreach (int pid in pids)
                    _checked.Remove(pid);
            }
        };
        Grid.SetColumn(check, 0);
        line.Children.Add(check);

        Border badge = new()
        {
            Width = 18,
            Height = 18,
            CornerRadius = new CornerRadius(9),
            Background = dot.Item1,
            VerticalAlignment = VerticalAlignment.Center,
            Margin = new Thickness(0, 0, 8, 0),
        };
        badge.Child = new TextBlock
        {
            Text = dot.Item2,
            FontSize = 10,
            FontWeight = FontWeights.Bold,
            Foreground = Brushes.White,
            HorizontalAlignment = HorizontalAlignment.Center,
            VerticalAlignment = VerticalAlignment.Center,
        };
        Grid.SetColumn(badge, 1);
        line.Children.Add(badge);

        TextBlock nameText = new()
        {
            Text = name,
            FontSize = 11.5,
            Foreground = new SolidColorBrush(Color.FromRgb(0xE6, 0xED, 0xF3)),
            VerticalAlignment = VerticalAlignment.Center,
            TextTrimming = TextTrimming.CharacterEllipsis,
        };
        Grid.SetColumn(nameText, 2);
        line.Children.Add(nameText);

        TextBlock memText = new()
        {
            Text = mb >= 1024 ? $"{mb / 1024:0.0} GB" : $"{mb:0} MB",
            FontSize = 11,
            FontWeight = FontWeights.Bold,
            Foreground = new SolidColorBrush(Color.FromRgb(0x58, 0xD6, 0x8D)),
            VerticalAlignment = VerticalAlignment.Center,
            Margin = new Thickness(8, 0, 6, 0),
        };
        Grid.SetColumn(memText, 3);
        line.Children.Add(memText);

        Grid.SetRow(line, 0);
        row.Children.Add(line);

        // 占用条：进程内存占物理内存比例
        Border barBg = new()
        {
            Height = 4,
            CornerRadius = new CornerRadius(2),
            Background = new SolidColorBrush(Color.FromRgb(0x2A, 0x30, 0x38)),
            Margin = new Thickness(40, 3, 0, 0),
        };
        var fill = new System.Windows.Shapes.Rectangle
        {
            Width = Math.Max(6, (Width - 60) * barPct),
            HorizontalAlignment = HorizontalAlignment.Left,
            RadiusX = 2,
            RadiusY = 2,
        };
        var grad = new LinearGradientBrush
        {
            StartPoint = new Point(0, 0),
            EndPoint = new Point(1, 0),
        };
        grad.GradientStops.Add(new GradientStop(Color.FromRgb(0x2E, 0xA8, 0x4F), 0));
        grad.GradientStops.Add(new GradientStop(Color.FromRgb(0x8F, 0xDC, 0xA0), 1));
        fill.Fill = grad;
        barBg.Child = fill;
        TextBlock sysTip = new() { Visibility = Visibility.Collapsed, Text = $"系统内存占用 {sysPct:0.0}%" };
        row.Children.Add(sysTip);
        Grid.SetRow(barBg, 1);
        row.Children.Add(barBg);

        row.ToolTip = pids.Count > 1
            ? $"{name}（{pids.Count} 个进程）\n内存合计约 {sysPct:0.0}%（{mb:0} MB）"
            : $"{name}\n物理内存占用约 {sysPct:0.0}%（{mb:0} MB）";
        return row;
    }
}
