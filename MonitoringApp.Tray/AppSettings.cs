using System;
using System.IO;
using System.Text.Json;
using Microsoft.Win32;

namespace MonitoringApp.Tray;

/// <summary>应用设置（持久化到 exe 旁的 config.json，字段名与旧版兼容，旧配置可直接沿用）。</summary>
public sealed class AppSettings
{
    private static readonly string ConfigPath = Path.Combine(AppContext.BaseDirectory, "config.json");

    public bool ShowCpuPower { get; set; } = true;
    public bool ShowFanRpm { get; set; } = true;

    public int RefreshIntervalMs { get; set; } = 1000;
    public bool AutoStart { get; set; } = false;

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
        const string valueName = "MechrevoMonitorTray";

        string? exePath = Environment.ProcessPath;
        if (string.IsNullOrEmpty(exePath))
            return;

        try
        {
            using RegistryKey? key = Registry.CurrentUser.OpenSubKey(keyPath, writable: true);
            if (key == null)
                return;

            if (AutoStart)
                key.SetValue(valueName, "\"" + exePath + "\"");
            else
                key.DeleteValue(valueName, false);
        }
        catch
        {
            // 忽略注册表写入失败
        }
    }
}
