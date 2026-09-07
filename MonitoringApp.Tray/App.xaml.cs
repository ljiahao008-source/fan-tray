using System;
using System.IO;
using System.Threading;
using System.Windows;

namespace MonitoringApp.Tray;

public partial class App : System.Windows.Application
{
    private static Mutex? _singleInstance;

    private void OnStartup(object sender, StartupEventArgs e)
    {
        // 单实例：已有精简版在跑时直接退出
        _singleInstance = new Mutex(true, "MechrevoMonitorTray_SingleInstance", out bool createdNew);
        if (!createdNew)
        {
            Shutdown();
            return;
        }

        DispatcherUnhandledException += (_, ev) => { Log(ev.Exception); ev.Handled = true; };
        AppDomain.CurrentDomain.UnhandledException += (_, ev) => Log(ev.ExceptionObject as Exception);

        // 无主窗口：启动即常驻托盘，显式退出前不结束进程
        var tray = new TrayController();
        tray.Start();

        // 资源优化：启动 60 秒后做一次带压缩的完整 GC。
        // 启动期（WPF 初始化/打开硬件）会产生一大坨临时对象，而平时每秒分配极少、
        // GC 触发频率低，这坨垃圾会长期滞留抬高内存；此后台采样已就绪时一次性收走，仅执行一次。
        _ = new System.Threading.Timer(_ => GC.Collect(2, GCCollectionMode.Forced, blocking: true, compacting: true),
            null, TimeSpan.FromSeconds(60), Timeout.InfiniteTimeSpan);
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
