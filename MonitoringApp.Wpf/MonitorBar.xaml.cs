using System;
using System.Windows;
using System.Windows.Input;
using System.Windows.Media;
using LibreHardwareMonitor.Mechrevo;

namespace MonitoringApp;

/// <summary>横向监控长条：置顶细条，六个指标（功耗/温度/转速/内存/下行/上行）横排完整显示数值带单位，
/// 不截断；可拖动、可隐藏。用于替代系统托盘（16px 方格）放不下的完整数值展示。</summary>
public partial class MonitorBar : Window
{
    // 浅色长条上的温度三态文字色（正常绿 / 警告琥珀 / 临界红）
    private static readonly SolidColorBrush BrushTempWarn = new(System.Windows.Media.Color.FromRgb(0xB4, 0x53, 0x09));
    private static readonly SolidColorBrush BrushTempCrit = new(System.Windows.Media.Color.FromRgb(0xDC, 0x26, 0x26));
    private static readonly SolidColorBrush BrushTempOk = new(System.Windows.Media.Color.FromRgb(0x16, 0xA3, 0x4A));

    /// <summary>拖动结束后（Left, Top），供外部记忆位置。</summary>
    public event Action<double, double>? MoveEnded;

    public MonitorBar()
    {
        InitializeComponent();
    }

    public void UpdateData(MonitoringSnapshot s, AppSettings st, NetworkSpeedMeter net)
    {
        PVal.Text = Fmt(s.CpuPower.Current, "0.0");
        PUnit.Text = "W";

        float? t = s.CpuTemperature.Current;
        TVal.Text = Fmt(t, "0");
        TVal.Foreground = t switch
        {
            { } v when v >= st.CriticalTemperature => BrushTempCrit,
            { } v when v >= st.WarnTemperature => BrushTempWarn,
            _ => BrushTempOk,
        };
        TUnit.Text = "℃";

        // 转速：完整四位 + 单位，长条内绝不截断
        FVal.Text = Fmt(s.FanRpm.Current, "0");
        FUnit.Text = "RPM";

        MVal.Text = Fmt(s.MemoryLoad.Current, "0");
        MUnit.Text = "%";

        (double dn, string dnU) = NetworkSpeedMeter.Format(net.DownKbps);
        DVal.Text = dnU == "MB/s" ? dn.ToString("0.0") : dn.ToString("0");
        DUnit.Text = dnU;

        (double up, string upU) = NetworkSpeedMeter.Format(net.UpKbps);
        UVal.Text = upU == "MB/s" ? up.ToString("0.0") : up.ToString("0");
        UUnit.Text = upU;
    }

    private static string Fmt(float? v, string f) => v?.ToString(f) ?? "--";

    private void OnBarDrag(object sender, MouseButtonEventArgs e)
    {
        // 避开"✕"按钮（原始命中的是按钮内部元素则不拖动）
        DependencyObject? hit = e.OriginalSource as DependencyObject;
        while (hit != null && !(hit is System.Windows.Controls.Button))
            hit = VisualTreeHelper.GetParent(hit);
        if (hit is System.Windows.Controls.Button)
            return;

        if (e.LeftButton == MouseButtonState.Pressed)
        {
            DragMove();
            MoveEnded?.Invoke(Left, Top);
        }
    }

    /// <summary>把长条约束在当前显示器工作区内：分辨率/多显示器变化导致记忆位置失效时，
    /// 自动收窄到屏幕宽度并移回屏内，保证完整可见、不压任务栏。</summary>
    public void EnsureOnScreen()
    {
        var wa = SystemParameters.WorkArea;
        double margin = 8;

        // 窄屏时收窄面板本身，保证内容完整不溢出屏幕
        double maxWidth = Math.Max(560, wa.Width - margin * 2);
        if (Width > maxWidth)
            Width = maxWidth;

        if (double.IsNaN(Left))
            Left = wa.Left + (wa.Width - Width) / 2;
        else
            Left = Math.Clamp(Left, wa.Left + margin, Math.Max(wa.Left + margin, wa.Right - Width - margin));

        if (double.IsNaN(Top))
            Top = wa.Top + 8;
        else
            Top = Math.Clamp(Top, wa.Top + margin, Math.Max(wa.Top + margin, wa.Bottom - Height - margin));
    }

    private void OnHide(object? sender, RoutedEventArgs e)
    {
        Hide();
    }
}
