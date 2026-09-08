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
        // 单实例：已有实例在跑时，先尝试结束它再接管（新版升级场景旧实例常驻托盘，
        // 直接静默退出会让用户觉得"双击没反应"）；仍失败才提示并退出
        _singleInstance = new Mutex(true, "MechrevoMonitorTray_SingleInstance", out bool createdNew);
        if (!createdNew)
        {
            TryKillOtherInstances();
            _singleInstance.Dispose();
            _singleInstance = new Mutex(true, "MechrevoMonitorTray_SingleInstance", out createdNew);
            if (!createdNew)
            {
                System.Windows.MessageBox.Show(
                    "机械革命监控已在运行（任务栏右下角托盘图标），请先从托盘菜单退出旧实例。",
                    "机械革命监控", System.Windows.MessageBoxButton.OK, System.Windows.MessageBoxImage.Information);
                Shutdown();
                return;
            }
        }

        DispatcherUnhandledException += (_, ev) => { Log(ev.Exception); ev.Handled = true; };
        AppDomain.CurrentDomain.UnhandledException += (_, ev) => Log(ev.ExceptionObject as Exception);
        // WinForms 控件（NotifyIcon/菜单）回调里的异常走 ThreadException，不经过 WPF Dispatcher，
        // 不挂这个 handler 会弹系统错误框且不留任何日志
        System.Windows.Forms.Application.SetUnhandledExceptionMode(System.Windows.Forms.UnhandledExceptionMode.CatchException);
        System.Windows.Forms.Application.ThreadException += (_, ev) => Log(ev.Exception);

        // 无主窗口：启动即常驻托盘，显式退出前不结束进程
        var tray = new TrayController();
        tray.Start();

        // 资源优化：启动 60 秒后做一次带压缩的完整 GC。
        // 启动期（WPF 初始化/打开硬件）会产生一大坨临时对象，而平时每秒分配极少、
        // GC 触发频率低，这坨垃圾会长期滞留抬高内存；此后台采样已就绪时一次性收走，仅执行一次。
        _ = new System.Threading.Timer(_ => GC.Collect(2, GCCollectionMode.Forced, blocking: true, compacting: true),
            null, TimeSpan.FromSeconds(60), Timeout.InfiniteTimeSpan);
    }

    /// <summary>结束其他同名进程（旧实例的 exe 可能已被改名，只能按进程名匹配）。</summary>
    private static void TryKillOtherInstances()
    {
        try
        {
            foreach (System.Diagnostics.Process p in System.Diagnostics.Process.GetProcessesByName("MechrevoMonitorTray"))
            {
                try
                {
                    if (p.Id != Environment.ProcessId)
                        p.Kill();
                }
                catch
                {
                    // 无权限结束（理论上新版提权后不会发生）时跳过
                }
                finally
                {
                    p.Dispose();
                }
            }

            // 等旧实例真正退出、释放互斥量
            System.Threading.Thread.Sleep(800);
        }
        catch
        {
            // 枚举失败就走提示退出分支
        }
    }

    private static void Log(Exception? ex)
    {
        if (ex == null)
            return;

        try
        {
            File.AppendAllText(Path.Combine(AppContext.BaseDirectory, "crash.log"),
                $"[{DateTime.Now:yyyy-MM-dd HH:mm:ss}] {ex}\n\n");
        }
        catch
        {
            // 忽略
        }
    }
}
