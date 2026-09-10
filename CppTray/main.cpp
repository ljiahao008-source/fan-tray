// 机械革命监控 C++ 版主入口（对照 App.xaml.cs + TrayController.cs）
// 单实例 + 隐藏主窗口 + 托盘 + 任务栏嵌入 + 后台采样线程

#include <windows.h>
#include <tlhelp32.h>
#include <string>

#include "tray.h"
#include "widget.h"
#include "monitor.h"
#include "config.h"
#include "autostart.h"
#include "settingsdlg.h"
#include "lockscreendlg.h"
#include "ddcbrightness.h"
#include "colortemp.h"
#include "mainwindow.h"
#include "ddc.h"

namespace {

constexpr wchar_t kMainClass[] = L"MechrevoMonitorTrayMainClass";
constexpr wchar_t kMutexName[] = L"MechrevoMonitorTray_SingleInstance";
constexpr UINT kSampleMsg = WM_APP + 11;
constexpr UINT_PTR kTrayMenuTimer = 1;   // 托盘单击→菜单的延迟判定（等双击）

struct App {
    HINSTANCE hInst = nullptr;
    HWND mainHwnd = nullptr;
    Widget* widget = nullptr;
    TrayIcon* tray = nullptr;
    MonitorCore* core = nullptr;
    AppConfig cfg;
    CRITICAL_SECTION snapLock;
    SampleSet snap;
    volatile LONG running = 1;
    volatile LONG intervalMs = 1000;   // 采样间隔（设置改动即时生效，免线程重启竞态）
    HANDLE sampleThread = nullptr;
    UINT taskbarCreatedMsg = 0;
    bool thresholdsApplied = false;
    Thresholds thr;
    HANDLE mutex = nullptr;
};

App g_app;

// 屏幕亮度快捷键（DDC/CI 模块，仅外接显示器）
ddcb::HotkeyManager g_brightnessKeys;
// 对比度 / 音量快捷键（完整 DDC/CI 模块）
ddcb::HotkeyManager g_contrastKeys;
ddcb::HotkeyManager g_volumeKeys;
// 色温护眼（LightBulb 引擎）
colortemp::ColorTemperatureManager g_colorTemp;

// 崩溃兜底：写 crash.log
LONG WINAPI CrashFilter(EXCEPTION_POINTERS* info) {
    (void)info;
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring p = path;
    size_t pos = p.find_last_of(L"\\/");
    if (pos != std::wstring::npos)
        p.resize(pos + 1);
    p += L"crash.log";

    HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        SetFilePointer(h, 0, nullptr, FILE_END);
        wchar_t line[128] = {};
        swprintf_s(line, L"[crash] %llu\n", (unsigned long long)GetTickCount64());
        DWORD w = 0;
        WriteFile(h, line, (DWORD)(wcslen(line) * sizeof(wchar_t)), &w, nullptr);
        CloseHandle(h);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// 枚举并结束其他同名进程（升级场景旧实例常驻）
void KillOtherInstances() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    DWORD myPid = GetCurrentProcessId();
    if (Process32FirstW(snap, &pe)) {
        do {
            if (pe.th32ProcessID == myPid)
                continue;
            if (_wcsicmp(pe.szExeFile, L"MechrevoMonitorTray.exe") == 0) {
                HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
                if (h) {
                    TerminateProcess(h, 0);
                    CloseHandle(h);
                }
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    Sleep(1500);   // 等旧实例释放驱动句柄（MSR 卸载竞态）
}

std::wstring ExeDir() {
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring p = buf;
    size_t pos = p.find_last_of(L"\\/");
    if (pos != std::wstring::npos)
        p.resize(pos + 1);
    return p;
}

void WriteLog(const wchar_t* msg) {
    HANDLE h = CreateFileW((ExeDir() + L"trace.log").c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return;
    SetFilePointer(h, 0, nullptr, FILE_END);
    wchar_t line[512] = {};
    swprintf_s(line, L"[%llu] %s\n", (unsigned long long)GetTickCount64(), msg);
    DWORD w = 0;
    WriteFile(h, line, (DWORD)(wcslen(line) * sizeof(wchar_t)), &w, nullptr);
    CloseHandle(h);
}

// 按配置安装/注销亮度快捷键（启动与设置保存后调用）
void ApplyBrightnessHotkeys(const AppConfig& cfg) {
    g_brightnessKeys.Uninstall();
    if (!cfg.BrightnessKeysEnabled)
        return;
    bool ok = g_brightnessKeys.Install(
        g_app.mainHwnd,
        [](int step) {
            int n = ddcb::BrightnessController::AdjustAll(step);
            wchar_t log[96] = {};
            swprintf_s(log, L"brightness adjust step=%d ok=%d", step, n);
            WriteLog(log);
        },
        MOD_CONTROL | MOD_ALT, VK_UP, VK_DOWN, cfg.BrightnessStepPercent);
    if (!ok)
        WriteLog(L"brightness hotkey register failed (occupied?)");
}

// 按配置安装/注销对比度/音量快捷键（完整 DDC/CI）
void ApplyExtDdcHotkeys(const AppConfig& cfg) {
    g_contrastKeys.Uninstall();
    g_volumeKeys.Uninstall();
    if (!cfg.ContrastVolumeKeysEnabled)
        return;
    bool ok = g_contrastKeys.Install(
        g_app.mainHwnd,
        [](int step) {
            int n = ddc::Ddc::AdjustAll(1, step);
            wchar_t log[96] = {};
            swprintf_s(log, L"contrast adjust step=%d ok=%d", step, n);
            WriteLog(log);
        },
        MOD_CONTROL | MOD_ALT | MOD_SHIFT, VK_UP, VK_DOWN, 10, 0xB021, 0xB022);
    ok = g_volumeKeys.Install(
        g_app.mainHwnd,
        [](int step) {
            int n = ddc::Ddc::AdjustAll(2, step);
            wchar_t log[96] = {};
            swprintf_s(log, L"volume adjust step=%d ok=%d", step, n);
            WriteLog(log);
        },
        MOD_CONTROL | MOD_ALT, VK_LEFT, VK_RIGHT, 5, 0xB023, 0xB024) && ok;
    if (!ok)
        WriteLog(L"contrast/volume hotkey register failed (occupied?)");
}

// AppConfig → 色温模块配置
colortemp::Settings ColorTempSettingsFromConfig(const AppConfig& cfg) {
    colortemp::Settings s;
    s.enabled = cfg.ColorTempEnabled;
    s.dayTemperature = cfg.ColorTempDayK;
    s.nightTemperature = cfg.ColorTempNightK;
    s.sunriseMinutes = cfg.ColorTempSunriseMinutes;
    s.sunsetMinutes = cfg.ColorTempSunsetMinutes;
    s.transitionMinutes = cfg.ColorTempTransitionMinutes;
    s.transitionOffset = 0.5;
    s.hotkeyStepK = cfg.ColorTempStepK;
    return s;
}

// 色温配置热更新（设置保存后调用）
void ApplyColorTemp(const AppConfig& cfg) {
    g_colorTemp.ApplySettings(ColorTempSettingsFromConfig(cfg));
}

// —— 应用动作（托盘菜单 / 主窗口按钮共用）——
void HandleAppAction(UINT id, HWND parent) {
    switch (id) {
        case kMenuSettings: {
            AppConfig newCfg = g_app.cfg;
            if (ShowSettingsDialog(parent, newCfg)) {
                g_app.cfg = newCfg;
                SaveConfig(newCfg);
                g_app.intervalMs = newCfg.RefreshIntervalMs;   // 采样线程下一拍生效
                if (g_app.widget)
                    g_app.widget->ApplyConfig(newCfg);          // 显示项即时生效
                mainwin::Rebuild();                             // 面板重建（显示器列表）
                ApplyBrightnessHotkeys(newCfg);                 // 亮度快捷键即时生效
                ApplyExtDdcHotkeys(newCfg);                     // 对比度/音量快捷键即时生效
                ApplyColorTemp(newCfg);                         // 色温设置即时生效
            }
            break;
        }
        case kMenuLockScreen:
            lockscreen::ShowLockScreenWindow(parent);
            break;
        case kMenuReset:
            if (g_app.core)
                g_app.core->ResetStats();
            break;
        case kMenuExit:
            PostMessage(g_app.mainHwnd, WM_CLOSE, 0, 0);
            break;
    }
}

DWORD WINAPI SampleThreadProc(LPVOID p);

// —— 采样线程（后台）：读硬件 → 快照 → 通知 UI ——
DWORD WINAPI SampleThreadProc(LPVOID p) {
    App* app = (App*)p;
    bool inited = false;
    while (InterlockedCompareExchange(&app->running, 1, 1)) {
        if (!inited) {
            if (app->core->Init()) {
                inited = true;
                WriteLog(L"sample: core init ok");
            } else {
                WriteLog(L"sample: core init failed, retry");
                Sleep(1000);
                continue;
            }
        }

        SampleSet s;
        app->core->Sample(s);

        EnterCriticalSection(&app->snapLock);
        app->snap = s;
        LeaveCriticalSection(&app->snapLock);

        PostMessage(app->mainHwnd, kSampleMsg, 0, 0);
        Sleep((DWORD)app->intervalMs);   // 动态读取，设置改动即时生效
    }
    return 0;
}

// —— 主窗口（隐藏）：托盘回调 + 采样结果 + TaskbarCreated ——
LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case kTrayCallbackMsg: {
            switch (LOWORD(lp)) {
                case WM_RBUTTONUP:
                    g_app.tray->ShowMenu(hwnd);
                    break;
                case WM_LBUTTONUP:
                    // 单击：延迟弹菜单（等待双击判定，避免双击时菜单闪一下）
                    SetTimer(hwnd, kTrayMenuTimer, GetDoubleClickTime(), nullptr);
                    break;
                case WM_LBUTTONDBLCLK:
                    KillTimer(hwnd, kTrayMenuTimer);
                    mainwin::Show();   // 双击打开主窗口
                    break;
            }
            return 0;
        }
        case WM_TIMER: {
            if (wp == kTrayMenuTimer) {
                KillTimer(hwnd, kTrayMenuTimer);
                g_app.tray->ShowMenu(hwnd);
            }
            return 0;
        }
        case kSampleMsg: {
            SampleSet s;
            EnterCriticalSection(&g_app.snapLock);
            s = g_app.snap;
            LeaveCriticalSection(&g_app.snapLock);

            if (g_app.widget && g_app.core) {
                if (!g_app.thresholdsApplied) {
                    g_app.thr = g_app.core->GetThresholds();
                    g_app.widget->SetThresholds(g_app.thr);
                    g_app.widget->ApplyConfig(g_app.cfg);
                    g_app.thresholdsApplied = true;
                }
                g_app.widget->Update(s);
            }
            mainwin::Refresh();   // 面板数值（仅可见时刷新）

            // 托盘 tooltip：全部指标当前值（-- 表示该项无数据）
            auto cur = [](const Metric& m, const wchar_t* fmt) {
                static wchar_t bufs[8][24];
                static int slot = 0;
                wchar_t* b = bufs[slot++ & 7];
                if (!m.valid)
                    wcscpy_s(b, 24, L"--");
                else
                    swprintf_s(b, 24, fmt, (double)m.current);
                return b;
            };

            wchar_t tip[256] = {};
            swprintf_s(tip, L"功耗 %s W · 风扇 %s RPM\nCPU %s%% · %s°C · 内存 %s%% · 网速 ↓%s↑%s KB/s",
                       cur(s.power, L"%.1f"), cur(s.fan, L"%.0f"),
                       cur(s.cpuUsage, L"%.0f"), cur(s.cpuTemp, L"%.0f"), cur(s.mem, L"%.0f"),
                       cur(s.netDown, L"%.0f"), cur(s.netUp, L"%.0f"));
            if (g_app.tray)
                g_app.tray->SetTooltip(tip);
            return 0;
        }
        case WM_CLOSE: {
            g_brightnessKeys.Uninstall();   // 注销亮度快捷键
            g_contrastKeys.Uninstall();
            g_volumeKeys.Uninstall();
            g_colorTemp.Stop();             // 恢复 gamma 并停色温线程
            mainwin::DestroyWindowW();      // 销毁面板窗口
            InterlockedExchange(&g_app.running, 0);
            // 采样线程可能在 WMI 阻塞：放宽等待；超时（线程未退出）则跳过 delete，进程退出由 OS 回收
            DWORD wait = WAIT_OBJECT_0;
            if (g_app.sampleThread) {
                wait = WaitForSingleObject(g_app.sampleThread, 15000);
                CloseHandle(g_app.sampleThread);
                g_app.sampleThread = nullptr;
            }
            if (g_app.tray)
                g_app.tray->Remove();
            if (g_app.widget)
                g_app.widget->Dispose();
            if (g_app.core && wait == WAIT_OBJECT_0)
                delete g_app.core;
            delete g_app.widget;
            delete g_app.tray;
            g_app.core = nullptr;
            g_app.widget = nullptr;
            g_app.tray = nullptr;
            DestroyWindow(hwnd);
            return 0;
        }
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        case WM_HOTKEY:
            // 亮度（Ctrl+Alt+↑/↓）→ 对比度（Ctrl+Alt+Shift+↑/↓）→ 音量（Ctrl+Alt+←/→）
            if (g_brightnessKeys.HandleMessage(msg, wp))
                return 0;
            if (g_contrastKeys.HandleMessage(msg, wp))
                return 0;
            if (g_volumeKeys.HandleMessage(msg, wp))
                return 0;
            // 色温（Ctrl+Alt+PgUp/PgDn/Home）
            if (g_colorTemp.HandleHotkey(msg, wp))
                return 0;
            break;
        case mainwin::kActionMsg: {
            // 主窗口按钮动作 → 复用应用动作处理
            UINT id = 0;
            switch (wp) {
                case mainwin::ActionSettings:    id = kMenuSettings; break;
                case mainwin::ActionLockScreen:  id = kMenuLockScreen; break;
                case mainwin::ActionReset:       id = kMenuReset; break;
                case mainwin::ActionExit:        id = kMenuExit; break;
            }
            if (id)
                HandleAppAction(id, mainwin::Hwnd());
            return 0;
        }
        default:
            if (msg == g_app.taskbarCreatedMsg) {
                // explorer 重启：任务栏重建后重新嵌入
                if (g_app.widget)
                    g_app.widget->Embed();
                return 0;
            }
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

// —— 托盘菜单回调（tray.cpp 通过 extern "C" 声明调用）——
extern "C" void TrayMenuCallback(UINT id, bool checked, HWND hwnd) {
    switch (id) {
        case kMenuOpenMain:
            mainwin::Show();
            break;
        case kMenuAutoStart: {
            bool enable = !checked;   // 菜单勾选状态取反 = 目标状态
            if (ApplyAutoStart(enable)) {
                g_app.cfg.AutoStart = enable;
                SaveConfig(g_app.cfg);
            } else {
                MessageBoxW(hwnd, L"开机自启设置失败：创建计划任务需要管理员权限。",
                            L"机械革命监控", MB_OK | MB_ICONWARNING);
            }
            break;
        }
        default:
            HandleAppAction(id, hwnd);
            break;
    }
}

int APIENTRY wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int) {
    SetUnhandledExceptionFilter(CrashFilter);
    WriteLog(L"startup-enter");

    // 单实例
    g_app.mutex = CreateMutexW(nullptr, TRUE, kMutexName);
    if (!g_app.mutex || GetLastError() == ERROR_ALREADY_EXISTS) {
        KillOtherInstances();
        g_app.mutex = CreateMutexW(nullptr, TRUE, kMutexName);
        if (!g_app.mutex || GetLastError() == ERROR_ALREADY_EXISTS) {
            MessageBoxW(nullptr, L"机械革命监控已在运行（任务栏右下角托盘图标）。",
                        L"机械革命监控", MB_OK | MB_ICONINFORMATION);
            return 0;
        }
    }
    WriteLog(L"mutex ok");

    g_app.hInst = hInst;
    InitializeCriticalSection(&g_app.snapLock);

    // 配置
    g_app.cfg = LoadConfig();
    if (g_app.cfg.AutoStart && !IsAutoStartTaskInstalled())
        ApplyAutoStart(true);

    // 注册主窗口类
    WNDCLASSW wc{};
    wc.lpfnWndProc = MainWndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = kMainClass;
    RegisterClassW(&wc);
    g_app.mainHwnd = CreateWindowExW(0, kMainClass, L"MechrevoMonitorTray",
                                     WS_OVERLAPPED, 0, 0, 0, 0,
                                     nullptr, nullptr, hInst, nullptr);
    g_app.taskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");

    // 组件
    g_app.core = new MonitorCore();
    g_app.widget = new Widget();
    if (!g_app.widget->Create(hInst)) {
        MessageBoxW(nullptr, L"窗口初始化失败。", L"机械革命监控", MB_OK | MB_ICONERROR);
        return 1;
    }
    g_app.tray = new TrayIcon();
    g_app.tray->Create(g_app.mainHwnd, hInst);

    mainwin::Create(hInst);                        // 显示器控制面板（隐藏，双击托盘/菜单打开）
    mainwin::SetHost(g_app.mainHwnd);              // 按钮动作转发到宿主

    ApplyBrightnessHotkeys(g_app.cfg);   // 亮度快捷键（DDC/CI）
    ApplyExtDdcHotkeys(g_app.cfg);       // 对比度/音量快捷键（DDC/CI）
    ApplyColorTemp(g_app.cfg);           // 色温配置就位
    g_colorTemp.Start(g_app.mainHwnd);   // 启动色温线程（每秒按调度应用 gamma）

    g_app.widget->ApplyConfig(g_app.cfg);   // 启动即按配置确定显示项（首拍数据到达前也保持正确宽度）

    // 启动采样线程
    g_app.intervalMs = g_app.cfg.RefreshIntervalMs;
    InterlockedExchange(&g_app.running, 1);
    g_app.sampleThread = CreateThread(nullptr, 0, &SampleThreadProc, &g_app, 0, nullptr);

    WriteLog(L"startup done");

    // 消息循环
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // 清理
    DeleteCriticalSection(&g_app.snapLock);
    if (g_app.mutex)
        ReleaseMutex(g_app.mutex);
    WriteLog(L"exit");
    return 0;
}
