#include "autostart.h"

#include <windows.h>
#include <string>
#include <vector>

namespace {

constexpr wchar_t kTaskName[] = L"MechrevoMonitorTray";
constexpr wchar_t kLegacyRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

std::wstring ExePath() {
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return buf;
}

// 运行 schtasks 并返回退出码；失败返回 -1。
int RunSchTasks(const std::vector<std::wstring>& args) {
    std::wstring cmd = L"schtasks.exe";
    for (const auto& a : args) {
        cmd += L" ";
        cmd += a;
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return -1;
    WaitForSingleObject(pi.hProcess, 10000);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)code;
}

void RemoveLegacyRunEntry() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kLegacyRunKey, 0, KEY_SET_VALUE, &key) == ERROR_SUCCESS) {
        RegDeleteValueW(key, kTaskName);
        RegCloseKey(key);
    }
}

}  // namespace

bool IsAutoStartTaskInstalled() {
    return RunSchTasks({L"/Query", L"/TN", kTaskName}) == 0;
}

bool ApplyAutoStart(bool enable) {
    RemoveLegacyRunEntry();

    if (enable) {
        std::wstring tr = L"\"" + ExePath() + L"\"";
        int code = RunSchTasks({L"/Create", L"/TN", kTaskName, L"/TR", tr,
                                L"/SC", L"ONLOGON", L"/RL", L"HIGHEST", L"/F"});
        return code == 0;
    }

    if (RunSchTasks({L"/Delete", L"/TN", kTaskName, L"/F"}) == 0)
        return true;
    return !IsAutoStartTaskInstalled();   // 任务本来就不存在视为已移除
}
