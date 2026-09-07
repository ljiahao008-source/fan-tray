using System.Windows;

namespace MonitoringApp;

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

        ShowPower.IsChecked = _settings.ShowCpuPower;
        ShowTemp.IsChecked = _settings.ShowCpuTemperature;
        ShowFan.IsChecked = _settings.ShowFanRpm;
        ShowMem.IsChecked = _settings.ShowMemoryLoad;
        ShowNetSpeed.IsChecked = _settings.ShowNetworkSpeed;

        foreach ((string Label, int Ms) item in Intervals)
            IntervalCombo.Items.Add(item.Label);
        int sel = Array.FindIndex(Intervals, x => x.Ms == _settings.RefreshIntervalMs);
        IntervalCombo.SelectedIndex = sel >= 0 ? sel : 1;

        WarnSlider.Value = _settings.WarnTemperature;
        CriticalSlider.Value = _settings.CriticalTemperature;
        AutoStart.IsChecked = _settings.AutoStart;
    }

    private void OnWarnChanged(object sender, RoutedPropertyChangedEventArgs<double> e)
    {
        WarnLabel.Text = ((int)WarnSlider.Value).ToString();
    }

    private void OnCriticalChanged(object sender, RoutedPropertyChangedEventArgs<double> e)
    {
        CriticalLabel.Text = ((int)CriticalSlider.Value).ToString();
    }

    private void OnOk(object sender, RoutedEventArgs e)
    {
        bool hasTrayIcon = ShowPower.IsChecked == true || ShowTemp.IsChecked == true ||
                           ShowFan.IsChecked == true || ShowMem.IsChecked == true ||
                           ShowNetSpeed.IsChecked == true;
        if (!hasTrayIcon)
        {
            System.Windows.MessageBox.Show(this, "请至少保留一个托盘监控图标，否则主窗口隐藏后将无法从托盘恢复。",
                "需要保留托盘图标", MessageBoxButton.OK, MessageBoxImage.Information);
            return;
        }

        _settings.ShowCpuPower = ShowPower.IsChecked == true;
        _settings.ShowCpuTemperature = ShowTemp.IsChecked == true;
        _settings.ShowFanRpm = ShowFan.IsChecked == true;
        _settings.ShowMemoryLoad = ShowMem.IsChecked == true;
        _settings.ShowNetworkSpeed = ShowNetSpeed.IsChecked == true;
        _settings.RefreshIntervalMs = Intervals[IntervalCombo.SelectedIndex].Ms;
        _settings.WarnTemperature = (float)WarnSlider.Value;
        _settings.CriticalTemperature = (float)CriticalSlider.Value;
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
