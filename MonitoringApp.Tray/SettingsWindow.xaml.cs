using System;
using System.Windows;

namespace MonitoringApp.Tray;

public partial class SettingsWindow : Window
{
    private readonly AppSettings _settings;

    private static readonly (string Label, int Ms)[] Intervals =
    {
        ("0.5 秒", 500),
        ("1 秒", 1000),
        ("2 秒", 2000),
        ("5 秒", 5000),
    };

    public SettingsWindow(AppSettings settings)
    {
        InitializeComponent();
        _settings = settings;

        foreach ((string Label, int Ms) item in Intervals)
            IntervalCombo.Items.Add(item.Label);
        int sel = Array.FindIndex(Intervals, x => x.Ms == _settings.RefreshIntervalMs);
        IntervalCombo.SelectedIndex = sel >= 0 ? sel : 1;

        AutoStart.IsChecked = _settings.AutoStart;
    }

    private void OnOk(object sender, RoutedEventArgs e)
    {
        // 下拉框意外处于无选中状态时回退 1 秒，避免索引越界
        int idx = IntervalCombo.SelectedIndex >= 0 ? IntervalCombo.SelectedIndex : 1;
        _settings.RefreshIntervalMs = Intervals[idx].Ms;
        _settings.AutoStart = AutoStart.IsChecked == true;

        // 写入失败要可见：旧版静默吞掉，用户以为保存成功实则没有
        string? error = null;
        if (!_settings.Save())
            error = "设置未能写入 config.json（程序所在目录可能没有写入权限），本次修改仅当前运行有效。";
        if (!_settings.ApplyAutoStart())
            error = (error is null ? "" : error + "\n") + "开机自启设置失败：创建计划任务需要管理员权限。";
        if (error != null)
            System.Windows.MessageBox.Show(this, error, "保存失败", MessageBoxButton.OK, MessageBoxImage.Warning);
        DialogResult = true;
        Close();
    }

    private void OnCancel(object sender, RoutedEventArgs e)
    {
        DialogResult = false;
        Close();
    }
}
