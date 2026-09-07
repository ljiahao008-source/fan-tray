using System;
using System.Windows;
using System.Windows.Media;
using System.Windows.Threading;

namespace MonitoringApp;

/// <summary>轻量非模态提示：出现在屏幕中间偏下、几秒后淡出，不遮挡主要界面。
/// 用于替代系统气泡弹窗（如操作结果提示）。</summary>
public partial class ToastWindow : Window
{
    private readonly DispatcherTimer _fade = new() { Interval = TimeSpan.FromMilliseconds(18) };
    private DateTime _next;
    private int _stage; // 0 淡入 → 1 停留 → 2 淡出
    private static readonly TimeSpan Hold = TimeSpan.FromSeconds(3.2);

    private ToastWindow(string title, string body)
    {
        InitializeComponent();
        TitleText.Text = title;
        BodyText.Text = body;
        _fade.Tick += OnFadeTick;
    }

    /// <summary>弹出一条提示，自动淡入停留后淡出。UI 线程调用即可。</summary>
    /// <param name="nearTray">true 时贴近托盘（屏幕右下角）显示，用于托盘交互反馈。</param>
    public static void Show(string title, string body, bool ok = true, bool nearTray = false)
    {
        ToastWindow w = new(title, body);
        w.MarkText.Text = ok ? "✓" : "✕";
        w.MarkText.Foreground = ok
            ? (System.Windows.Media.Brush)System.Windows.Application.Current.FindResource("GreenBrush")
            : (System.Windows.Media.Brush)System.Windows.Application.Current.FindResource("RedBrush");
        w.ShowToast(nearTray);
    }

    private void ShowToast(bool nearTray)
    {
        Show();
        // 位置：nearTray 贴近托盘（工作区右下角）；否则水平居中、垂直约 55%（中间偏下）
        UpdateLayout();
        double w = ActualWidth + 16, h = ActualHeight + 16;
        var wa = SystemParameters.WorkArea;
        if (nearTray)
        {
            Left = wa.Right - w - 14;
            Top = wa.Bottom - h - 10;
        }
        else
        {
            // 默认：水平居中、垂直位置下移（约 66% 高度处），避免遮挡主窗口中部上方的卡片与按钮
            Left = wa.Left + (wa.Width - w) / 2;
            Top = wa.Top + wa.Height * 0.66 - h / 2;
            if (Top + h > wa.Bottom - 12)
                Top = wa.Bottom - h - 12;
        }

        _stage = 0;
        _next = DateTime.UtcNow + TimeSpan.FromMilliseconds(240);
        _fade.Start();
    }

    private void OnFadeTick(object? sender, EventArgs e)
    {
        switch (_stage)
        {
            case 0: // 淡入
                Opacity += 0.09;
                if (Opacity >= 1)
                {
                    Opacity = 1;
                    _stage = 1;
                    _next = DateTime.UtcNow + Hold;
                }
                break;
            case 1: // 停留
                if (DateTime.UtcNow >= _next)
                    _stage = 2;
                break;
            case 2: // 淡出
                Opacity -= 0.07;
                if (Opacity <= 0)
                {
                    _fade.Stop();
                    Close();
                }
                break;
        }
    }
}
