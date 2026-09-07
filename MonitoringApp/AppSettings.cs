using System;
using System.IO;
using System.Text.Json;
using Microsoft.Win32;

namespace MonitoringApp;

/// <summary>应用设置（持久化到 exe 旁的 config.json）。</summary>
public sealed class AppSettings
{
    private static readonly string ConfigPath = Path.Combine(AppContext.BaseDirectory, "config.json");

    public bool ShowCpuPower { get; set; } = true;
    public bool ShowCpuTemperature { get; set; } = true;

    /// <summary>任务栏 CPU 占用文本指标。</summary>
    public bool ShowCpuUsage { get; set; } = true;

    public bool ShowFanRpm { get; set; } = true;
    public bool ShowMemoryLoad { get; set; } = true;

    /// <summary>托盘网络合显图标（↑下行 ↓上行同图标展示）。</summary>
    public bool ShowNetworkSpeed { get; set; } = true;

    public int RefreshIntervalMs { get; set; } = 1000;
    public float WarnTemperature { get; set; } = 70;
    public float CriticalTemperature { get; set; } = 85;
    public bool AutoStart { get; set; } = false;

    /// <summary>是否已提示过"关闭=最小化到托盘"（仅首次提示一次）。</summary>
    public bool NotifiedHideOnce { get; set; } = false;

    // 横向监控长条位置（NaN = 默认顶部居中）
    public double BarLeft { get; set; } = double.NaN;
    public double BarTop { get; set; } = double.NaN;

    // 窗口状态（重启时恢复位置和大小）
    public double WindowLeft { get; set; } = double.NaN;
    public double WindowTop { get; set; } = double.NaN;
    public double WindowWidth { get; set; } = 840;
    public double WindowHeight { get; set; } = 820;
    public bool WindowMaximized { get; set; } = false;

    public static AppSettings Load()
    {
        string? raw = null;
        try
        {
            if (File.Exists(ConfigPath))
                raw = File.ReadAllText(ConfigPath);
        }
        catch
        {
            // 忽略配置读取失败，使用默认值
        }

        AppSettings s = new();
        if (raw != null)
        {
            try
            {
                AppSettings? d = JsonSerializer.Deserialize<AppSettings>(raw);
                if (d != null)
                    s = d;
            }
            catch
            {
                // 配置损坏时回退默认值
            }

            // 迁移旧版"上下行两个独立托盘图标"设置 → 新版网络合显图标开关
            if (!raw.Contains("\"ShowNetworkSpeed\"", StringComparison.OrdinalIgnoreCase))
            {
                try
                {
                    using JsonDocument doc = JsonDocument.Parse(raw);
                    bool oldDown = doc.RootElement.TryGetProperty("ShowNetworkDown", out JsonElement e1) && e1.ValueKind == JsonValueKind.True;
                    bool oldUp = doc.RootElement.TryGetProperty("ShowNetworkUp", out JsonElement e2) && e2.ValueKind == JsonValueKind.True;
                    s.ShowNetworkSpeed = oldDown || oldUp;
                }
                catch
                {
                    // 解析失败保持默认
                }
            }
        }

        return s;
    }

    public void Save()
    {
        try
        {
            File.WriteAllText(ConfigPath, JsonSerializer.Serialize(this, new JsonSerializerOptions { WriteIndented = true }));
        }
        catch
        {
            // 忽略配置写入失败
        }
    }

    /// <summary>按 <see cref="AutoStart"/> 写入或删除开机自启注册表项。</summary>
    public void ApplyAutoStart()
    {
        const string keyPath = @"Software\Microsoft\Windows\CurrentVersion\Run";
        const string valueName = "MechrevoMonitor";

        string? exePath = Environment.ProcessPath;
        if (string.IsNullOrEmpty(exePath))
            return;

        try
        {
            using RegistryKey? key = Registry.CurrentUser.OpenSubKey(keyPath, writable: true);
            if (key == null)
                return;

            if (AutoStart)
                key.SetValue(valueName, "\"" + exePath + "\" --minimized");
            else
                key.DeleteValue(valueName, false);
        }
        catch
        {
            // 忽略注册表写入失败
        }
    }
}
