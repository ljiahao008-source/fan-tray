// 截屏诊断工具：shoot 截全屏；crop 裁剪；zoom 裁剪+放大；click 模拟左键单击；who 查点位窗口；find 列监控程序窗口
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;

[DllImport("user32.dll")]
static extern bool SetCursorPos(int x, int y);

[DllImport("user32.dll")]
static extern void mouse_event(uint flags, uint dx, uint dy, uint data, nuint extra);

[DllImport("user32.dll")]
static extern IntPtr WindowFromPoint(WindowsPoint p);

[DllImport("user32.dll")]
static extern IntPtr GetAncestor(IntPtr hwnd, uint flags);

[DllImport("user32.dll", CharSet = CharSet.Unicode)]
static extern int GetClassName(IntPtr hwnd, StringBuilder sb, int max);

[DllImport("user32.dll")]
static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);

[DllImport("user32.dll")]
static extern bool EnumWindows(EnumProc callback, IntPtr lParam);

[DllImport("user32.dll")]
static extern bool EnumChildWindows(IntPtr parent, EnumProc callback, IntPtr lParam);

[DllImport("user32.dll")]
static extern bool GetWindowRect(IntPtr hwnd, out WinRect rect);

[DllImport("user32.dll", CharSet = CharSet.Unicode)]
static extern IntPtr FindWindow(string cls, string? title);

const uint LEFTDOWN = 0x02, LEFTUP = 0x04;
const uint GA_ROOT = 2;
const string TargetProc = "MechrevoMonitor";

if (args.Length == 0)
{
    Console.WriteLine("usage: Shot shoot | crop <src> <x> <y> <w> <h> [out] | zoom <src> <x> <y> <w> <h> <scale> [out] | click <x> <y> [times] | who <x> <y> | find");
    return 1;
}

string dir = @"C:\Users\<用户名>\Desktop\机械革命监控\源码\Shot\shots";
Directory.CreateDirectory(dir);

switch (args[0])
{
    case "shoot":
    {
        string full = Path.Combine(dir, "full.png");
        using Bitmap shot = CaptureScreen();
        shot.Save(full, System.Drawing.Imaging.ImageFormat.Png);
        Console.WriteLine("saved " + full + " " + shot.Width + "x" + shot.Height);
        break;
    }
    case "crop":
    {
        using var src = new Bitmap(args[1]);
        int x = int.Parse(args[2]), y = int.Parse(args[3]), w = int.Parse(args[4]), h = int.Parse(args[5]);
        string outPath = args.Length > 6 ? args[6] : Path.Combine(dir, "crop.png");
        using Bitmap cloned = src.Clone(new Rectangle(x, y, w, h), src.PixelFormat);
        cloned.Save(outPath, System.Drawing.Imaging.ImageFormat.Png);
        Console.WriteLine("saved " + outPath);
        break;
    }
    case "click":
    {
        int x = int.Parse(args[1]), y = int.Parse(args[2]);
        int times = args.Length > 3 ? int.Parse(args[3]) : 1;
        SetCursorPos(x, y);
        Thread.Sleep(80);
        for (int i = 0; i < times; i++)
        {
            mouse_event(LEFTDOWN, 0, 0, 0, 0);
            Thread.Sleep(50);
            mouse_event(LEFTUP, 0, 0, 0, 0);
            if (i < times - 1)
                Thread.Sleep(120);
        }
        Console.WriteLine($"clicked {x},{y} x{times}");
        break;
    }
    case "who":
    {
        int x = int.Parse(args[1]), y = int.Parse(args[2]);
        IntPtr h = WindowFromPoint(new WindowsPoint { X = x, Y = y });
        var sb = new StringBuilder(256);
        GetClassName(h, sb, 256);
        GetWindowThreadProcessId(h, out uint pid);
        string? proc = null;
        try { proc = Process.GetProcessById((int)pid).ProcessName; } catch { }
        IntPtr root = GetAncestor(h, GA_ROOT);
        var sb2 = new StringBuilder(256);
        GetClassName(root, sb2, 256);
        Console.WriteLine($"at {x},{y}: hwnd=0x{h.ToInt64():X} class={sb} pid={pid}({proc}) rootClass={sb2}");
        break;
    }
    case "find":
        DumpMechrevoWindows();
        break;
    case "zoom":
    {
        using var src = new Bitmap(args[1]);
        int x = int.Parse(args[2]), y = int.Parse(args[3]), w = int.Parse(args[4]), h = int.Parse(args[5]);
        double scale = double.Parse(args[6]);
        string outPath = args.Length > 7 ? args[7] : Path.Combine(dir, "zoom.png");
        using Bitmap region = src.Clone(new Rectangle(x, y, w, h), src.PixelFormat);
        using Bitmap big = new((int)(w * scale), (int)(h * scale));
        using (Graphics g = Graphics.FromImage(big))
        {
            g.InterpolationMode = InterpolationMode.HighQualityBicubic;
            g.DrawImage(region, new Rectangle(0, 0, big.Width, big.Height));
        }
        big.Save(outPath, System.Drawing.Imaging.ImageFormat.Png);
        Console.WriteLine("saved " + outPath);
        break;
    }
    default:
        Console.WriteLine("unknown cmd");
        return 1;
}
return 0;

static Bitmap CaptureScreen()
{
    var bounds = System.Windows.Forms.Screen.PrimaryScreen!.Bounds;
    Bitmap bmp = new(bounds.Width, bounds.Height);
    using Graphics g = Graphics.FromImage(bmp);
    g.CopyFromScreen(0, 0, 0, 0, bounds.Size);
    return bmp;
}

/// <summary>枚举监控程序的所有可见窗口并打印矩形（顶层 + 任务栏子窗口）。</summary>
static void DumpMechrevoWindows()
{
    uint targetPid = 0;
    foreach (Process p in Process.GetProcessesByName(TargetProc))
        targetPid = (uint)p.Id;

    if (targetPid == 0)
    {
        Console.WriteLine("process not running");
        return;
    }

    IntPtr taskbar = FindWindow("Shell_TrayWnd", null);
    Console.WriteLine($"-- top-level windows of pid {targetPid} --");
    EnumWindows((h, l) =>
    {
        GetWindowThreadProcessId(h, out uint pid);
        if (pid == targetPid && IsWindowVisible(h))
        {
            GetWindowRect(h, out WinRect r);
            var sb = new StringBuilder(256);
            GetClassName(h, sb, 256);
            Console.WriteLine($"top  0x{h.ToInt64():X} {sb} rect=({r.L},{r.T})-({r.R},{r.B}) size={r.R - r.L}x{r.B - r.T}");
        }
        return true;
    }, IntPtr.Zero);

    if (taskbar != IntPtr.Zero)
    {
        Console.WriteLine($"-- children of Shell_TrayWnd for pid {targetPid} --");
        EnumChildWindows(taskbar, (h, l) =>
        {
            GetWindowThreadProcessId(h, out uint pid);
            if (pid == targetPid && IsWindowVisible(h))
            {
                GetWindowRect(h, out WinRect r);
                var sb = new StringBuilder(256);
                GetClassName(h, sb, 256);
                Console.WriteLine($"child 0x{h.ToInt64():X} {sb} rect=({r.L},{r.T})-({r.R},{r.B}) size={r.R - r.L}x{r.B - r.T}");
            }
            return true;
        }, IntPtr.Zero);
    }
}

[DllImport("user32.dll")]
static extern bool IsWindowVisible(IntPtr hwnd);

[StructLayout(LayoutKind.Sequential)]
struct WinRect { public int L, T, R, B; }

[StructLayout(LayoutKind.Sequential)]
struct WindowsPoint { public int X, Y; }

delegate bool EnumProc(IntPtr hwnd, IntPtr lParam);
