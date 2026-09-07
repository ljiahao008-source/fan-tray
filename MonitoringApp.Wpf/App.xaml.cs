using System;
using System.IO;
using System.Windows;

namespace MonitoringApp;

public partial class App : System.Windows.Application
{
    private void OnStartup(object sender, StartupEventArgs e)
    {
        DispatcherUnhandledException += (_, ev) => { Log(ev.Exception); ev.Handled = true; };
        AppDomain.CurrentDomain.UnhandledException += (_, ev) => Log(ev.ExceptionObject as Exception);

        MainWindow w = new();

        // 开机自启会带 --minimized 参数：先显示以完成初始化，随即隐藏到托盘（后台监控，不打扰）
        if (e.Args.Contains("--minimized"))
        {
            w.Show();
            w.Hide();
        }
        else
        {
            w.Show();
        }
    }

    private static void Log(Exception? ex)
    {
        if (ex == null)
            return;

        try
        {
            File.WriteAllText(Path.Combine(AppContext.BaseDirectory, "crash.log"), ex.ToString());
        }
        catch
        {
            // 忽略
        }
    }
}
