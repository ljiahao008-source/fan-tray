using System;
using System.IO;
using System.Threading;
using System.Windows.Threading;
using LibreHardwareMonitor.Mechrevo;
using Microsoft.Win32;
using Drawing = System.Drawing;
using WF = System.Windows.Forms;

namespace MonitoringApp.Tray;

/// <summary>
/// 托盘主控制器：单个托盘图标（程序图标）+ 托盘渲染窗口（任务栏内实时渲染功耗 / 风扇转速）。
/// 资源优化：采样移到后台线程池定时器（System.Threading.Timer），LHM 读 MSR 与 WMI 读风扇的
/// 阻塞不再占用 UI 线程（WMI 偶发卡顿秒级时悬浮/高亮依旧丝滑）；带防重入保护，
/// 上一拍没读完就跳过本拍，不堆积不重入；结果经 Dispatcher 派发回 UI 线程。
/// </summary>
public sealed class TrayController
{
    private readonly MonitorCore _core = new();
    private readonly AppSettings _settings = AppSettings.Load();
    private readonly Dispatcher _uiDispatcher = System.Windows.Application.Current.Dispatcher;
    private readonly WF.NotifyIcon _icon;
    private TrayWidget _widget;
    private System.Threading.Timer? _sampleTimer;
    private int _sampling;                     // 0 空闲 / 1 采样中

    public TrayController()
    {
        SystemEvents.UserPreferenceChanged += OnUserPreferenceChanged;

        _widget = new TrayWidget();

        _icon = new WF.NotifyIcon
        {
            ContextMenuStrip = BuildTrayMenu(),
            Visible = true,
            Text = "机械革命监控（精简版）",
        };
        try
        {
            if (Environment.ProcessPath is { } exe)
                _icon.Icon = Drawing.Icon.ExtractAssociatedIcon(exe);
        }
        catch
        {
            // 图标提取失败时保留系统默认图标
        }
    }

    /// <summary>启动：主题适配 + 托盘渲染窗口常驻显示 + 后台采样（无任何弹窗提示）。</summary>
    public void Start()
    {
        // 按 CPU 配置设定状态色阈值（正常绿 / 偏高橙 / 超高红）
        (double pe, double pc, double fe, double fc) = _core.GetThresholds();
        _widget.SetThresholds(pe, pc, fe, fc);

        _widget.ApplyTheme(IsLightSystemTheme());
        _widget.ShowWidget();
        StartSampling();
    }

    private void StartSampling()
    {
        _sampleTimer?.Dispose();
        _sampleTimer = new System.Threading.Timer(
            SampleTick, null, TimeSpan.Zero, TimeSpan.FromMilliseconds(_settings.RefreshIntervalMs));
    }

    /// <summary>后台线程执行：读硬件 → 派发 UI。防重入避免 WMI 阻塞时任务堆积。</summary>
    private void SampleTick(object? state)
    {
        if (Interlocked.Exchange(ref _sampling, 1) == 1)
            return;

        try
        {
            MonitoringSnapshot s = _core.Read();
            BeginInvokeOnUi(() =>
            {
                EnsureWidget();
                _widget.Update(s.CpuPower, s.FanRpm);
                UpdateToolTip(s);
            });
        }
        catch
        {
            // 读取失败：渲染组件显示占位，不中断定时采样
            BeginInvokeOnUi(() =>
            {
                EnsureWidget();
                _widget.Update(null, null);
            });
        }
        finally
        {
            Interlocked.Exchange(ref _sampling, 0);
        }
    }

    private void BeginInvokeOnUi(Action action)
    {
        try
        {
            _uiDispatcher.BeginInvoke(action);
        }
        catch
        {
            // 应用正在退出，Dispatcher 已关闭
        }
    }

    /// <summary>自愈：explorer 崩溃会连带销毁嵌入任务栏的子窗口（进程还活着但窗口已死），
    /// 检测到就整体重建挂件（UI 线程调用）。</summary>
    private void EnsureWidget()
    {
        if (_widget.IsWindowAlive)
            return;

        try { _widget.Dispose(); } catch { /* 僵尸窗口清理失败可忽略 */ }
        _widget = new TrayWidget();
        _widget.ApplyTheme(IsLightSystemTheme());
        _widget.ShowWidget();
    }

    private WF.ContextMenuStrip BuildTrayMenu()
    {
        WF.ContextMenuStrip menu = new();

        menu.Items.Add("设置", null, (_, _) => OpenSettings());
        menu.Items.Add("重置统计", null, (_, _) => _core.Reset());
        menu.Items.Add(new WF.ToolStripSeparator());

        WF.ToolStripMenuItem autoStart = new("开机自启") { CheckOnClick = true, Checked = _settings.AutoStart };
        autoStart.Click += (_, _) =>
        {
            bool enable = autoStart.Checked;
            _settings.AutoStart = enable;
            if (_settings.ApplyAutoStart())
            {
                _settings.Save();
            }
            else
            {
                // 失败回滚勾选与设置，并给出可见提示（旧版静默失败，用户以为自启已生效）
                autoStart.Checked = !enable;
                _settings.AutoStart = !enable;
                _settings.Save();
                WF.MessageBox.Show("开机自启设置失败：创建计划任务需要管理员权限。", "机械革命监控",
                    WF.MessageBoxButtons.OK, WF.MessageBoxIcon.Warning);
            }
        };
        menu.Items.Add(autoStart);

        menu.Items.Add(new WF.ToolStripSeparator());
        menu.Items.Add("退出", null, (_, _) => ExitApp());

        // 每次打开菜单时同步勾选状态
        menu.Opening += (_, _) =>
        {
            autoStart.Checked = _settings.AutoStart;
        };
        return menu;
    }

    // —— 设置 ——

    private void OpenSettings()
    {
        SettingsWindow w = new(_settings) { Topmost = true };
        w.ShowDialog();
        _sampleTimer?.Change(TimeSpan.Zero, TimeSpan.FromMilliseconds(_settings.RefreshIntervalMs));
    }

    // —— 托盘提示 ——

    /// <summary>悬停托盘图标显示实时值与统计（tooltip，非弹窗）。文本没变不写 WinForms，省 COM 调用。</summary>
    private void UpdateToolTip(MonitoringSnapshot s)
    {
        string text = SafeToolTip(
            $"功耗 {F(s.CpuPower.Current, "0.0")} W · 平均 {F(s.CpuPower.Average, "0.0")}\n" +
            $"风扇 {F(s.FanRpm.Current, "0")} RPM · 平均 {F(s.FanRpm.Average, "0")}");

        if (_icon.Text != text)
            _icon.Text = text;
    }

    // —— 退出 ——

    private void ExitApp()
    {
        _sampleTimer?.Dispose();
        _sampleTimer = null;
        // 每步独立兜底：任何一步抛异常都不能中断退出（曾有用户点"退出"弹异常框后进程残留）
        TryStep(() => _settings.Save());
        TryStep(() => SystemEvents.UserPreferenceChanged -= OnUserPreferenceChanged);
        TryStep(() => { _icon.Visible = false; });
        TryStep(() => { _icon.Dispose(); });
        TryStep(() => _widget.Dispose());
        TryStep(() => _core.Dispose());
        System.Windows.Application.Current.Shutdown();
    }

    private static void TryStep(Action step)
    {
        try { step(); }
        catch (Exception ex)
        {
            try
            {
                File.AppendAllText(Path.Combine(AppContext.BaseDirectory, "crash.log"),
                    $"[{DateTime.Now:yyyy-MM-dd HH:mm:ss}] ExitApp step failed: {ex}\n\n");
            }
            catch
            {
                // 日志也写不进去时只能放弃
            }
        }
    }

    // —— 环境（主题） ——

    private void OnUserPreferenceChanged(object sender, UserPreferenceChangedEventArgs e)
    {
        try
        {
            _uiDispatcher.BeginInvoke(() =>
            {
                _widget.ApplyTheme(IsLightSystemTheme());
                _widget.Reposition();
            });
        }
        catch (InvalidOperationException)
        {
            // 应用正在退出，Dispatcher 已关闭。
        }
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

    private static string F(float? v, string fmt) => v?.ToString(fmt) ?? "--";
}
