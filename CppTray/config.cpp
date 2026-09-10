#include "config.h"

#include <windows.h>
#include <string>
#include <vector>

namespace {

std::wstring ConfigPath() {
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring dir = buf;
    size_t pos = dir.find_last_of(L"\\/");
    if (pos != std::wstring::npos)
        dir.resize(pos + 1);
    return dir + L"config.json";
}

// 从 JSON 文本取 key 后的数值（忽略空白）。
bool JsonInt(const std::wstring& text, const wchar_t* key, int* out) {
    std::wstring k = L"\"" + std::wstring(key) + L"\"";
    size_t p = text.find(k);
    if (p == std::wstring::npos)
        return false;
    p = text.find(L":", p);
    if (p == std::wstring::npos)
        return false;
    p++;
    while (p < text.size() && (text[p] == L' ' || text[p] == L'\t' || text[p] == L'\r' || text[p] == L'\n'))
        p++;
    int sign = 1;
    if (p < text.size() && (text[p] == L'-' || text[p] == L'+')) {
        if (text[p] == L'-')
            sign = -1;
        p++;
    }
    int val = 0;
    bool any = false;
    while (p < text.size() && text[p] >= L'0' && text[p] <= L'9') {
        val = val * 10 + (text[p] - L'0');
        any = true;
        p++;
    }
    if (!any)
        return false;
    *out = val * sign;
    return true;
}

bool JsonBool(const std::wstring& text, const wchar_t* key, bool* out) {
    std::wstring k = L"\"" + std::wstring(key) + L"\"";
    size_t p = text.find(k);
    if (p == std::wstring::npos)
        return false;
    p = text.find(L":", p);
    if (p == std::wstring::npos)
        return false;
    p++;
    while (p < text.size() && (text[p] == L' ' || text[p] == L'\t' || text[p] == L'\r' || text[p] == L'\n'))
        p++;
    if (text.compare(p, 4, L"true") == 0) {
        *out = true;
        return true;
    }
    if (text.compare(p, 5, L"false") == 0) {
        *out = false;
        return true;
    }
    return false;
}

}  // namespace

AppConfig LoadConfig() {
    AppConfig cfg;

    HANDLE h = CreateFileW(ConfigPath().c_str(), GENERIC_READ, FILE_SHARE_READ,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return cfg;

    DWORD size = GetFileSize(h, nullptr);
    std::vector<BYTE> raw(size, 0);
    DWORD read = 0;
    BOOL ok = ReadFile(h, raw.data(), size, &read, nullptr);
    CloseHandle(h);
    if (!ok || read == 0)
        return cfg;

    std::wstring s;
    if (read >= 2 && raw[0] == 0xFF && raw[1] == 0xFE) {
        // UTF-16LE（带 BOM）
        s.assign((const wchar_t*)(raw.data() + 2), (read - 2) / sizeof(wchar_t));
    } else {
        // UTF-8（C# JsonSerializer 默认输出，无 BOM；可能带 EF BB BF）
        size_t offset = (read >= 3 && raw[0] == 0xEF && raw[1] == 0xBB && raw[2] == 0xBF) ? 3 : 0;
        int len = MultiByteToWideChar(CP_UTF8, 0, (const char*)(raw.data() + offset),
                                      (int)(read - offset), nullptr, 0);
        if (len > 0) {
            s.resize(len);
            MultiByteToWideChar(CP_UTF8, 0, (const char*)(raw.data() + offset),
                                (int)(read - offset), &s[0], len);
        }
    }

    JsonInt(s, L"RefreshIntervalMs", &cfg.RefreshIntervalMs);
    JsonBool(s, L"AutoStart", &cfg.AutoStart);
    JsonBool(s, L"ShowPower", &cfg.ShowPower);
    JsonBool(s, L"ShowFan", &cfg.ShowFan);
    JsonBool(s, L"ShowCpuUsage", &cfg.ShowCpuUsage);
    JsonBool(s, L"ShowCpuTemp", &cfg.ShowCpuTemp);
    JsonBool(s, L"ShowMem", &cfg.ShowMem);
    JsonBool(s, L"ShowNet", &cfg.ShowNet);
    JsonBool(s, L"BrightnessKeysEnabled", &cfg.BrightnessKeysEnabled);
    JsonInt(s, L"BrightnessStepPercent", &cfg.BrightnessStepPercent);
    JsonBool(s, L"ColorTempEnabled", &cfg.ColorTempEnabled);
    JsonInt(s, L"ColorTempDayK", &cfg.ColorTempDayK);
    JsonInt(s, L"ColorTempNightK", &cfg.ColorTempNightK);
    JsonInt(s, L"ColorTempSunriseMinutes", &cfg.ColorTempSunriseMinutes);
    JsonInt(s, L"ColorTempSunsetMinutes", &cfg.ColorTempSunsetMinutes);
    JsonInt(s, L"ColorTempTransitionMinutes", &cfg.ColorTempTransitionMinutes);
    JsonInt(s, L"ColorTempStepK", &cfg.ColorTempStepK);

    if (cfg.RefreshIntervalMs < 250 || cfg.RefreshIntervalMs > 60000)
        cfg.RefreshIntervalMs = 1000;
    if (cfg.BrightnessStepPercent < 1 || cfg.BrightnessStepPercent > 50)
        cfg.BrightnessStepPercent = 10;
    if (cfg.ColorTempDayK < 1000 || cfg.ColorTempDayK > 10000)
        cfg.ColorTempDayK = 6500;
    if (cfg.ColorTempNightK < 1000 || cfg.ColorTempNightK > 10000)
        cfg.ColorTempNightK = 4500;
    if (cfg.ColorTempSunriseMinutes < 0 || cfg.ColorTempSunriseMinutes >= 1440)
        cfg.ColorTempSunriseMinutes = 360;
    if (cfg.ColorTempSunsetMinutes < 0 || cfg.ColorTempSunsetMinutes >= 1440)
        cfg.ColorTempSunsetMinutes = 1140;
    if (cfg.ColorTempTransitionMinutes < 1 || cfg.ColorTempTransitionMinutes > 300)
        cfg.ColorTempTransitionMinutes = 60;
    if (cfg.ColorTempStepK < 50 || cfg.ColorTempStepK > 2000)
        cfg.ColorTempStepK = 500;
    return cfg;
}

bool SaveConfig(const AppConfig& cfg) {
    wchar_t buf[1536] = {};
    swprintf_s(buf,
               L"{\r\n  \"RefreshIntervalMs\": %d,\r\n  \"AutoStart\": %s,\r\n"
               L"  \"ShowPower\": %s,\r\n  \"ShowFan\": %s,\r\n"
               L"  \"ShowCpuUsage\": %s,\r\n  \"ShowCpuTemp\": %s,\r\n"
               L"  \"ShowMem\": %s,\r\n  \"ShowNet\": %s,\r\n"
               L"  \"BrightnessKeysEnabled\": %s,\r\n  \"BrightnessStepPercent\": %d,\r\n"
               L"  \"ColorTempEnabled\": %s,\r\n  \"ColorTempDayK\": %d,\r\n"
               L"  \"ColorTempNightK\": %d,\r\n  \"ColorTempSunriseMinutes\": %d,\r\n"
               L"  \"ColorTempSunsetMinutes\": %d,\r\n  \"ColorTempTransitionMinutes\": %d,\r\n"
               L"  \"ColorTempStepK\": %d\r\n}",
               cfg.RefreshIntervalMs,
               cfg.AutoStart ? L"true" : L"false",
               cfg.ShowPower ? L"true" : L"false",
               cfg.ShowFan ? L"true" : L"false",
               cfg.ShowCpuUsage ? L"true" : L"false",
               cfg.ShowCpuTemp ? L"true" : L"false",
               cfg.ShowMem ? L"true" : L"false",
               cfg.ShowNet ? L"true" : L"false",
               cfg.BrightnessKeysEnabled ? L"true" : L"false",
               cfg.BrightnessStepPercent,
               cfg.ColorTempEnabled ? L"true" : L"false",
               cfg.ColorTempDayK,
               cfg.ColorTempNightK,
               cfg.ColorTempSunriseMinutes,
               cfg.ColorTempSunsetMinutes,
               cfg.ColorTempTransitionMinutes,
               cfg.ColorTempStepK);

    // 统一写 UTF-8（与 LoadConfig 的默认解码路径一致，避免保存后读回乱码）
    int utf8Len = WideCharToMultiByte(CP_UTF8, 0, buf, -1, nullptr, 0, nullptr, nullptr);
    if (utf8Len <= 1)
        return false;
    std::vector<char> utf8(utf8Len);
    WideCharToMultiByte(CP_UTF8, 0, buf, -1, utf8.data(), utf8Len, nullptr, nullptr);

    HANDLE h = CreateFileW(ConfigPath().c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    DWORD written = 0;
    BOOL ok = WriteFile(h, utf8.data(), utf8Len - 1, &written, nullptr);   // 去掉结尾 NUL
    CloseHandle(h);
    return ok != FALSE;
}
