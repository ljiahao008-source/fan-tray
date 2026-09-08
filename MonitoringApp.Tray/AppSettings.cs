using System;
using System.Diagnostics;
using System.IO;
using System.Text.Json;
using Microsoft.Win32;

namespace MonitoringApp.Tray;

/// <summary>应用设置（持久化到 exe 旁的 config.json，字段名与旧版兼容，旧配置可直接沿用）。</summary>
public sealed class AppSettings
{
    private const string TaskName = "MechrevoMonitorTray";
    private const string LegacyRunValueName = TaskName;
    private const string LegacyRunKeyPath = @"Software\Microsoft\Windows\CurrentVersion\Run";

    private static readonly string ConfigPath = Path.Combine(AppContext.BaseDirectory, "config.json");

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

        // 刷新间隔校验：配置被改成 0 / 负数 / 超大值时回退默认，避免采样定时器参数非法
        if (s.RefreshIntervalMs is < 250 or > 60000)
            s.RefreshIntervalMs = 1000;

        return s;
    }

    /// <summary>持久化设置。返回 false 表示写入失败（目录无写入权限等），调用方应给出可见提示。</summary>
    public bool Save()
    {
        try
        {
            File.WriteAllText(ConfigPath, JsonSerializer.Serialize(this, new JsonSerializerOptions { WriteIndented = true }));
            return true;
        }
        catch
        {
            return false;
        }
    }

    /// <summary>按 <see cref="AutoStart"/> 创建或删除开机自启计划任务。
    /// 用计划任务（最高权限）而非 HKCU Run：本程序 requireAdministrator，
    /// HKCU Run 登录自启是非提权运行，读不到 MSR，功耗会一直显示 "--"；
    /// 计划任务 /RL HIGHEST 登录即以管理员运行，不弹 UAC。</summary>
    /// <returns>false 表示任务计划操作失败（需要管理员权限），调用方应给出可见提示。</returns>
    public bool ApplyAutoStart()
    {
        string? exePath = Environment.ProcessPath;
        if (string.IsNullOrEmpty(exePath))
            return false;

        bool ok = AutoStart ? CreateAutoStartTask(exePath) : RemoveAutoStartTask();
        RemoveLegacyRunEntry();   // 迁移：清掉旧版 HKCU Run 自启项（非提权启动读不到功耗）
        return ok;
    }

    private static bool CreateAutoStartTask(string exePath) => RunSchTasks("/Create", exePath);

    private static bool RemoveAutoStartTask()
    {
        if (RunSchTasks("/Delete"))
            return true;
        return !RunSchTasks("/Query");   // 任务本来就不存在视为已移除
    }

    private static bool RunSchTasks(string action, string? exePath = null)
    {
        try
        {
            ProcessStartInfo psi = new()
            {
                FileName = "schtasks.exe",
                UseShellExecute = false,
                CreateNoWindow = true,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
            };
            psi.ArgumentList.Add(action);
            psi.ArgumentList.Add("/TN");
            psi.ArgumentList.Add(TaskName);
            if (action == "/Create")
            {
                psi.ArgumentList.Add("/TR");
                psi.ArgumentList.Add("\"" + exePath + "\"");
                psi.ArgumentList.Add("/SC");
                psi.ArgumentList.Add("ONLOGON");
                psi.ArgumentList.Add("/RL");
                psi.ArgumentList.Add("HIGHEST");
                psi.ArgumentList.Add("/F");
            }
            else if (action == "/Delete")
            {
                psi.ArgumentList.Add("/F");
            }

            using Process? p = Process.Start(psi);
            if (p == null)
                return false;
            p.WaitForExit(10000);
            return p.ExitCode == 0;
        }
        catch
        {
            return false;
        }
    }

    private static void RemoveLegacyRunEntry()
    {
        try
        {
            using RegistryKey? key = Registry.CurrentUser.OpenSubKey(LegacyRunKeyPath, writable: true);
            key?.DeleteValue(LegacyRunValueName, false);
        }
        catch
        {
            // 清理旧版自启项失败可忽略：仅影响迁移，不影响本次设置
        }
    }
}
