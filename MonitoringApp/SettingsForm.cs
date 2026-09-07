using System;
using System.Drawing;
using System.Windows.Forms;

namespace MonitoringApp;

public sealed class SettingsForm : Form
{
    private readonly AppSettings _settings;
    private readonly CheckBox _showPower;
    private readonly CheckBox _showTemp;
    private readonly CheckBox _showFan;
    private readonly CheckBox _showMem;
    private readonly ComboBox _intervalCombo;
    private readonly NumericUpDown _warnInput;
    private readonly NumericUpDown _criticalInput;
    private readonly CheckBox _autoStart;

    private static readonly (string Label, int Ms)[] Intervals =
    {
        ("0.5 秒", 500),
        ("1 秒", 1000),
        ("2 秒", 2000),
        ("5 秒", 5000),
    };

    public SettingsForm(AppSettings settings)
    {
        _settings = settings;
        Text = "托盘设置";
        ClientSize = new Size(360, 378);
        FormBorderStyle = FormBorderStyle.FixedDialog;
        StartPosition = FormStartPosition.CenterParent;
        MaximizeBox = false;
        MinimizeBox = false;
        BackColor = Color.FromArgb(40, 40, 45);
        ForeColor = Color.White;
        Font = new Font("Microsoft YaHei UI", 9F);

        Label showLabel = MakeLabel("托盘图标显示（可多选，挨个显示）", 20, 22);

        _showPower = MakeCheck("CPU 功耗", 20, 48, _settings.ShowCpuPower);
        _showTemp = MakeCheck("CPU 温度", 120, 48, _settings.ShowCpuTemperature);
        _showFan = MakeCheck("风扇转速", 220, 48, _settings.ShowFanRpm);
        _showMem = MakeCheck("内存占用", 20, 76, _settings.ShowMemoryLoad);

        Label intervalLabel = MakeLabel("数据刷新间隔", 20, 108);
        _intervalCombo = new ComboBox
        {
            Location = new Point(20, 130),
            Width = 320,
            DropDownStyle = ComboBoxStyle.DropDownList,
            BackColor = Color.FromArgb(60, 60, 66),
            ForeColor = Color.White,
        };
        _intervalCombo.Items.AddRange(["0.5 秒", "1 秒", "2 秒", "5 秒"]);
        int sel = Array.FindIndex(Intervals, x => x.Ms == _settings.RefreshIntervalMs);
        _intervalCombo.SelectedIndex = sel >= 0 ? sel : 1;

        Label warnLabel = MakeLabel("温度警告阈值（橙色，°C）", 20, 162);
        _warnInput = MakeNumeric(20, 184, _settings.WarnTemperature);

        Label criticalLabel = MakeLabel("温度临界阈值（红色，°C）", 20, 216);
        _criticalInput = MakeNumeric(20, 238, _settings.CriticalTemperature);

        _autoStart = new CheckBox
        {
            Text = "开机自启（登录 Windows 时自动运行）",
            AutoSize = true,
            Location = new Point(20, 270),
            ForeColor = Color.FromArgb(220, 220, 225),
            Checked = _settings.AutoStart,
        };

        Button ok = new()
        {
            Text = "确定",
            Location = new Point(150, 330),
            Size = new Size(90, 30),
            BackColor = Color.FromArgb(60, 130, 90),
            ForeColor = Color.White,
            FlatStyle = FlatStyle.Flat,
        };
        ok.Click += (_, _) =>
        {
            Save();
            DialogResult = DialogResult.OK;
            Close();
        };

        Button cancel = new()
        {
            Text = "取消",
            Location = new Point(250, 330),
            Size = new Size(90, 30),
            BackColor = Color.FromArgb(70, 70, 76),
            ForeColor = Color.White,
            FlatStyle = FlatStyle.Flat,
        };
        cancel.Click += (_, _) =>
        {
            DialogResult = DialogResult.Cancel;
            Close();
        };

        Controls.AddRange([showLabel, _showPower, _showTemp, _showFan, _showMem, intervalLabel, _intervalCombo, warnLabel, _warnInput, criticalLabel, _criticalInput, _autoStart, ok, cancel]);
    }

    private void Save()
    {
        _settings.ShowCpuPower = _showPower.Checked;
        _settings.ShowCpuTemperature = _showTemp.Checked;
        _settings.ShowFanRpm = _showFan.Checked;
        _settings.ShowMemoryLoad = _showMem.Checked;
        _settings.RefreshIntervalMs = Intervals[_intervalCombo.SelectedIndex].Ms;
        _settings.WarnTemperature = (float)_warnInput.Value;
        _settings.CriticalTemperature = (float)_criticalInput.Value;
        _settings.AutoStart = _autoStart.Checked;
        _settings.Save();
        _settings.ApplyAutoStart();
    }

    private static CheckBox MakeCheck(string text, int x, int y, bool value) => new()
    {
        Text = text,
        AutoSize = true,
        Location = new Point(x, y),
        ForeColor = Color.FromArgb(220, 220, 225),
        Checked = value,
    };

    private static Label MakeLabel(string text, int x, int y) => new()
    {
        Text = text,
        AutoSize = true,
        Location = new Point(x, y),
        ForeColor = Color.FromArgb(200, 200, 205),
    };

    private static NumericUpDown MakeNumeric(int x, int y, float value) => new()
    {
        Location = new Point(x, y),
        Width = 320,
        Minimum = 0,
        Maximum = 200,
        DecimalPlaces = 0,
        Value = (decimal)value,
        BackColor = Color.FromArgb(60, 60, 66),
        ForeColor = Color.White,
    };
}
