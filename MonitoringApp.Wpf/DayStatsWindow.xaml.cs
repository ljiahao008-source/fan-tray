using System.Windows;

namespace MonitoringApp;

/// <summary>某天的四指标监控记录弹窗。</summary>
public partial class DayStatsWindow : Window
{
    public DayStatsWindow(string date, DailyPower dp)
    {
        InitializeComponent();
        DateText.Text = date;

        (float? a, float? mx, float? mn) = DailyPower.Summary(dp.Samples);
        PowerText.Text = $"平均 {Fmt(a, "0.0")} · 最高 {Fmt(mx, "0.0")} · 最低 {Fmt(mn, "0.0")}";

        (a, mx, mn) = DailyPower.Summary(dp.Temperature);
        TempText.Text = $"平均 {Fmt(a, "0")} · 最高 {Fmt(mx, "0")} · 最低 {Fmt(mn, "0")}";

        (a, mx, mn) = DailyPower.Summary(dp.Fan);
        FanText.Text = $"平均 {Fmt(a, "0")} · 最高 {Fmt(mx, "0")} · 最低 {Fmt(mn, "0")}";

        (a, mx, mn) = DailyPower.Summary(dp.Memory);
        MemText.Text = $"平均 {Fmt(a, "0")} · 最高 {Fmt(mx, "0")} · 最低 {Fmt(mn, "0")}";

        SampleText.Text = dp.SampleCount > 0
            ? $"每 60 秒自动记录一次 · 本日共 {dp.SampleCount} 次采样"
            : "本日暂无功耗采样";
    }

    private static string Fmt(float? v, string format) => v?.ToString(format) ?? "--";

    private void OnClose(object sender, RoutedEventArgs e)
    {
        DialogResult = true;
        Close();
    }
}
