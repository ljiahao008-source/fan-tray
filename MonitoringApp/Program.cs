using System;
using System.IO;
using System.Windows.Forms;

namespace MonitoringApp;

internal static class Program
{
    [STAThread]
    private static void Main()
    {
        Application.ThreadException += (_, e) => Log(e.Exception);
        AppDomain.CurrentDomain.UnhandledException += (_, e) => Log(e.ExceptionObject as Exception);

        try
        {
            ApplicationConfiguration.Initialize();
            Application.Run(new MainForm());
        }
        catch (Exception ex)
        {
            Log(ex);
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
