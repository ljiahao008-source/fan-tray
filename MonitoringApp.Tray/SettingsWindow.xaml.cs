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
        _settings.RefreshIntervalMs = Intervals[IntervalCombo.SelectedIndex].Ms;
        _settings.AutoStart = AutoStart.IsChecked == true;
        _settings.Save();
        _settings.ApplyAutoStart();
        DialogResult = true;
        Close();
    }

    private void OnCancel(object sender, RoutedEventArgs e)
    {
        DialogResult = false;
        Close();
    }
}
