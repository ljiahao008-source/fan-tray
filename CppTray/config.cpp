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

    if (cfg.RefreshIntervalMs < 250 || cfg.RefreshIntervalMs > 60000)
        cfg.RefreshIntervalMs = 1000;
    return cfg;
}

bool SaveConfig(const AppConfig& cfg) {
    wchar_t buf[256] = {};
    swprintf_s(buf, L"{\r\n  \"RefreshIntervalMs\": %d,\r\n  \"AutoStart\": %s\r\n}",
               cfg.RefreshIntervalMs, cfg.AutoStart ? L"true" : L"false");

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
