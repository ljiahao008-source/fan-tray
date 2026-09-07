using System;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Linq;
using System.Runtime.InteropServices;
using System.Windows.Forms;
using LibreHardwareMonitor.Mechrevo;
using ScottPlot.WinForms;

namespace MonitoringApp;

public sealed class MainForm : Form
{
    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool DestroyIcon(IntPtr hIcon);

    private readonly MonitoringService _service = new();
    private readonly AppSettings _settings = AppSettings.Load();
    private readonly PowerHistory _powerHistory = PowerHistory.Load();
    private readonly TrendHistory _trend = new(600);
    private readonly System.Windows.Forms.Timer _timer;
    private readonly NotifyIcon _powerIcon = new();
    private readonly NotifyIcon _tempIcon = new();
    private readonly NotifyIcon _fanIcon = new();
    private readonly NotifyIcon _memIcon = new();
    private Icon? _powerLast, _tempLast, _fanLast, _memLast;
    private bool _isExiting;
    private DateTime _lastPowerSample = DateTime.MinValue;

    private readonly Label _titleLabel = new();
    private readonly MetricCard _powerCard = new();
    private readonly MetricCard _tempCard = new();
    private readonly MetricCard _fanCard = new();
    private readonly MetricCard _memCard = new();
    private readonly FormsPlot _plot = new();
    private readonly PowerCalendar _calendar = new();

    public MainForm()
    {
        Text = "机械革命硬件监控";
        ClientSize = new Size(640, 860);
        StartPosition = FormStartPosition.CenterScreen;
        FormBorderStyle = FormBorderStyle.FixedSingle;
        MaximizeBox = false;
        BackColor = Color.FromArgb(24, 27, 33);
        ForeColor = Color.White;

        BuildLayout();
        InitChart();
        InitTray();
        FormClosing += OnFormClosing;

        _timer = new System.Windows.Forms.Timer { Interval = _settings.RefreshIntervalMs };
        _timer.Tick += (_, _) => OnTick();
        _timer.Start();
        OnTick();
    }

    private void BuildLayout()
    {
        _titleLabel.Text = "硬件监控";
        _titleLabel.Font = new Font("Microsoft YaHei UI", 12F, FontStyle.Bold);
        _titleLabel.ForeColor = Color.FromArgb(230, 233, 240);
        _titleLabel.Location = new Point(20, 12);
        _titleLabel.AutoSize = true;

        _powerCard.Bounds = new Rectangle(20, 46, 292, 100);
        _tempCard.Bounds = new Rectangle(328, 46, 292, 100);
        _fanCard.Bounds = new Rectangle(20, 158, 292, 100);
        _memCard.Bounds = new Rectangle(328, 158, 292, 100);
        _plot.Bounds = new Rectangle(20, 270, 600, 230);
        _calendar.Bounds = new Rectangle(20, 512, 600, 332);

        Controls.AddRange(new Control[] { _titleLabel, _powerCard, _tempCard, _fanCard, _memCard, _plot, _calendar });
    }

    private void InitChart()
    {
        _plot.Plot.FigureBackground.Color = ScottPlot.Color.FromHex("#22262E");
        _plot.Plot.DataBackground.Color = ScottPlot.Color.FromHex("#22262E");
        _plot.Plot.Font.Set("Microsoft YaHei UI");
        _plot.Plot.Axes.Color(ScottPlot.Color.FromHex("#6B7280"));
        _plot.Plot.Axes.Left.TickLabelStyle.ForeColor = ScottPlot.Color.FromHex("#9CA3AF");
        _plot.Plot.Axes.Right.TickLabelStyle.ForeColor = ScottPlot.Color.FromHex("#9CA3AF");
        _plot.Plot.Axes.Bottom.TickLabelStyle.ForeColor = ScottPlot.Color.FromHex("#9CA3AF");
        _plot.Plot.Grid.MajorLineColor = ScottPlot.Color.FromHex("#2E333D");
    }

    private void InitTray()
    {
        ContextMenuStrip menu = BuildTrayMenu();
        InitTrayIcon(_powerIcon, menu);
        InitTrayIcon(_tempIcon, menu);
        InitTrayIcon(_fanIcon, menu);
        InitTrayIcon(_memIcon, menu);
    }

    private void InitTrayIcon(NotifyIcon icon, ContextMenuStrip menu)
    {
        icon.ContextMenuStrip = menu;
        icon.Visible = false;
        icon.DoubleClick += (_, _) => ShowWindow();
    }

    private ContextMenuStrip BuildTrayMenu()
    {
        ContextMenuStrip menu = new();
        menu.Items.Add("显示窗口", null, (_, _) => ShowWindow());
        menu.Items.Add("设置", null, (_, _) => OpenSettings());
        menu.Items.Add("重置统计", null, (_, _) => { _service.Reset(); _trend.Clear(); RefreshAll(); });
        menu.Items.Add(new ToolStripSeparator());
        menu.Items.Add("退出", null, (_, _) => ExitApp());
        return menu;
    }

    private void OpenSettings()
    {
        using SettingsForm f = new(_settings);
        if (f.ShowDialog(this) == DialogResult.OK)
        {
            _timer.Interval = _settings.RefreshIntervalMs;
            RefreshAll();
        }
    }

    private void ShowWindow()
    {
        Show();
        WindowState = FormWindowState.Normal;
        Activate();
    }

    private void ExitApp()
    {
        _isExiting = true;
        _powerHistory.Save();
        _powerIcon.Visible = _tempIcon.Visible = _fanIcon.Visible = _memIcon.Visible = false;
        Close();
    }

    private void OnFormClosing(object? sender, FormClosingEventArgs e)
    {
        if (!_isExiting)
        {
            e.Cancel = true;
            Hide();
        }
    }

    private void OnTick()
    {
        RefreshAll();

        // 功耗按真实时间每 60 秒采样一次（不依赖 tick 计数）
        if ((DateTime.Now - _lastPowerSample).TotalSeconds >= 60)
        {
                if (_service.Read().CpuPower.Current is { } w)
                    _powerHistory.AddSample(w, null, null, null);
            _powerHistory.Save();
            _lastPowerSample = DateTime.Now;
        }
    }

    private void RefreshAll()
    {
        try
        {
            MonitoringSnapshot s = _service.Read();

            _trend.Add(s.CpuPower.Current ?? 0, s.CpuTemperature.Current ?? 0);

            _powerCard.SetData("CPU 功耗", Fmt(s.CpuPower.Current, "0.0"), "W", Stat(s.CpuPower, "0.0"), Color.FromArgb(255, 176, 74));
            _tempCard.SetData("CPU 温度", Fmt(s.CpuTemperature.Current, "0"), "°C", Stat(s.CpuTemperature, "0"), Color.FromArgb(255, 120, 90));
            _fanCard.SetData("风扇转速", Fmt(s.FanRpm.Current, "0"), "RPM", Stat(s.FanRpm, "0"), Color.FromArgb(94, 176, 122));
            _memCard.SetData("内存占用", Fmt(s.MemoryLoad.Current, "0"), "%", Stat(s.MemoryLoad, "0"), Color.FromArgb(120, 160, 220));

            UpdateChart();
            _calendar.SetData(_powerHistory);
            UpdateTray(s);
        }
        catch (Exception ex)
        {
            _powerCard.SetData("CPU 功耗", "--", "W", ex.Message, Color.Gray);
        }
    }

    private void UpdateChart()
    {
        _plot.Plot.Clear();
        InitChart();

        double[] powers = _trend.Points.Select(p => (double)p.Power).ToArray();
        double[] temps = _trend.Points.Select(p => (double)p.Temperature).ToArray();

        if (powers.Length >= 2)
        {
            var powerSig = _plot.Plot.Add.Signal(powers);
            powerSig.Color = ScottPlot.Color.FromHex("#FFB04A");
            powerSig.LineWidth = 2;
            powerSig.LegendText = "功耗";

            var tempSig = _plot.Plot.Add.Signal(temps);
            tempSig.Color = ScottPlot.Color.FromHex("#FF6E64");
            tempSig.LineWidth = 1.5f;
            tempSig.LegendText = "温度";

            _plot.Plot.Axes.AutoScale();
            _plot.Plot.ShowLegend();
        }

        _plot.Refresh();
    }

    private void UpdateTray(MonitoringSnapshot s)
    {
        float temp = s.CpuTemperature.Current ?? 0;
        Color tempColor = temp >= _settings.CriticalTemperature ? Color.FromArgb(220, 60, 60)
            : temp >= _settings.WarnTemperature ? Color.FromArgb(220, 150, 30)
            : Color.FromArgb(45, 150, 70);

        UpdateTrayIcon(_powerIcon, ref _powerLast, FmtInt(s.CpuPower.Current), Color.FromArgb(255, 176, 74), _settings.ShowCpuPower, $"CPU 功耗 {Fmt(s.CpuPower.Current, "0.0")} W（{Stat(s.CpuPower, "0")}）");
        UpdateTrayIcon(_tempIcon, ref _tempLast, FmtInt(s.CpuTemperature.Current), tempColor, _settings.ShowCpuTemperature, $"CPU 温度 {Fmt(s.CpuTemperature.Current, "0")}°C（{Stat(s.CpuTemperature, "0")}）");
        UpdateTrayIcon(_fanIcon, ref _fanLast, FmtFan(s.FanRpm.Current), Color.FromArgb(94, 176, 122), _settings.ShowFanRpm, $"风扇转速 {Fmt(s.FanRpm.Current, "0")} RPM（{Stat(s.FanRpm, "0")}）");
        UpdateTrayIcon(_memIcon, ref _memLast, FmtInt(s.MemoryLoad.Current), Color.FromArgb(120, 160, 220), _settings.ShowMemoryLoad, $"内存占用 {Fmt(s.MemoryLoad.Current, "0")}%（{Stat(s.MemoryLoad, "0")}）");
    }

    private void UpdateTrayIcon(NotifyIcon icon, ref Icon? last, string text, Color color, bool visible, string tip)
    {
        icon.Visible = visible;
        if (!visible)
            return;

        Icon newIcon = MakeIcon(text, color);
        last?.Dispose();
        last = newIcon;
        icon.Icon = newIcon;
        icon.Text = tip;
    }

    private static string Stat(Metric m, string format) => $"最低 {Fmt(m.Min, format)} · 最高 {Fmt(m.Max, format)} · 平均 {Fmt(m.Average, format)}";

    private static string Fmt(Metric m, string format) => m.Current?.ToString(format) ?? "--";

    private static string Fmt(float? v, string format) => v?.ToString(format) ?? "--";

    private static string FmtInt(float? v) => v?.ToString("0") ?? "--";

    private static string FmtFan(float? v) => v is null ? "--" : (v >= 1000 ? (v.Value / 1000).ToString("0.0") + "k" : v.Value.ToString("0"));

    private static Icon MakeIcon(string text, Color color)
    {
        using Bitmap bmp = new(64, 64);
        using Graphics g = Graphics.FromImage(bmp);
        g.SmoothingMode = SmoothingMode.AntiAlias;
        g.Clear(Color.Transparent);

        using (SolidBrush b = new(color))
            g.FillEllipse(b, 2, 2, 60, 60);

        using (Font f = new("Microsoft YaHei UI", 24, FontStyle.Bold))
        using (StringFormat sf = new() { Alignment = StringAlignment.Center, LineAlignment = StringAlignment.Center })
            g.DrawString(text, f, Brushes.White, new RectangleF(2, 2, 60, 60), sf);

        IntPtr h = bmp.GetHicon();
        try
        {
            return (Icon)Icon.FromHandle(h).Clone();
        }
        finally
        {
            DestroyIcon(h);
        }
    }

    protected override void Dispose(bool disposing)
    {
        if (disposing)
        {
            _powerHistory.Save();
            _timer.Dispose();
            _powerIcon.Dispose();
            _tempIcon.Dispose();
            _fanIcon.Dispose();
            _memIcon.Dispose();
            _powerLast?.Dispose();
            _tempLast?.Dispose();
            _fanLast?.Dispose();
            _memLast?.Dispose();
            _service.Dispose();
        }

        base.Dispose(disposing);
    }
}
