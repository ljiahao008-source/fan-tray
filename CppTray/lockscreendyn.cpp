// 锁屏设置业务层（二）：动态锁 / 配置导入导出（含轻量 JSON 解析）
// 移植自 C# 版 src/gui/{Dynlock,Config}.cs —— 行为逐项对齐

#include "lockscreen.h"

#include <aclapi.h>
#include <shellapi.h>
#include <algorithm>
#include <cstdio>
#include <cwctype>
#include <fstream>
#include <map>
#include <set>

namespace lockscreen {

namespace {

// ---------- 通用 ----------

std::wstring Trim(const std::wstring& s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == L' ' || s[b] == L'\t' || s[b] == L'\r' || s[b] == L'\n'))
        b++;
    while (e > b && (s[e - 1] == L' ' || s[e - 1] == L'\t' || s[e - 1] == L'\r' || s[e - 1] == L'\n'))
        e--;
    return s.substr(b, e - b);
}

std::wstring ToLowerW(const std::wstring& s) {
    std::wstring r;
    r.reserve(s.size());
    for (wchar_t c : s)
        r.push_back((wchar_t)towlower(c));
    return r;
}

std::wstring FromUtf8(const std::string& s) {
    if (s.empty())
        return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0)
        return L"";
    std::wstring out((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &out[0], n);
    return out;
}

std::string ToUtf8(const std::wstring& s) {
    if (s.empty())
        return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string out((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), &out[0], n, nullptr, nullptr);
    return out;
}

// ---------- 动态锁注册表常量 ----------

constexpr wchar_t kWinlogonKey[] = L"Software\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon";
constexpr wchar_t kBthDevicesKey[] = L"SYSTEM\\CurrentControlSet\\Services\\BTHPORT\\Parameters\\Devices";
constexpr wchar_t kBthKeysKey[] = L"SYSTEM\\CurrentControlSet\\Services\\BTHPORT\\Parameters\\Keys";
constexpr wchar_t kLogonUiKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Authentication\\LogonUI";

std::optional<std::wstring> ReadRegString(HKEY root, const std::wstring& path, const wchar_t* name) {
    HKEY k = nullptr;
    if (RegOpenKeyExW(root, path.c_str(), 0, KEY_READ, &k) != ERROR_SUCCESS)
        return std::nullopt;
    wchar_t buf[512] = {};
    DWORD size = sizeof(buf) - sizeof(wchar_t), type = 0;
    LSTATUS st = RegQueryValueExW(k, name, nullptr, &type, (LPBYTE)buf, &size);
    RegCloseKey(k);
    if (st != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ))
        return std::nullopt;
    return std::wstring(buf);
}

std::optional<DWORD> ReadRegDword(HKEY root, const std::wstring& path, const wchar_t* name) {
    HKEY k = nullptr;
    if (RegOpenKeyExW(root, path.c_str(), 0, KEY_READ, &k) != ERROR_SUCCESS)
        return std::nullopt;
    DWORD v = 0, size = sizeof(v), type = 0;
    LSTATUS st = RegQueryValueExW(k, name, nullptr, &type, (LPBYTE)&v, &size);
    RegCloseKey(k);
    if (st != ERROR_SUCCESS)
        return std::nullopt;
    if (type == REG_DWORD)
        return v;
    if (type == REG_BINARY && size == 4)
        return v;
    if (type == REG_SZ) {
        std::wstring s = reinterpret_cast<const wchar_t*>(&v);   // 不严谨，仅作兜底
        (void)s;
        return std::nullopt;
    }
    return std::nullopt;
}

// 读二进制值（用于蓝牙设备 Name / LastSeen / COD）
std::vector<BYTE> ReadRegBinary(HKEY root, const std::wstring& path, const wchar_t* name) {
    std::vector<BYTE> out;
    HKEY k = nullptr;
    if (RegOpenKeyExW(root, path.c_str(), 0, KEY_READ, &k) != ERROR_SUCCESS)
        return out;
    DWORD size = 0, type = 0;
    if (RegQueryValueExW(k, name, nullptr, &type, nullptr, &size) == ERROR_SUCCESS && size > 0) {
        out.resize(size);
        if (RegQueryValueExW(k, name, nullptr, &type, out.data(), &size) != ERROR_SUCCESS)
            out.clear();
        else
            out.resize(size);
    }
    RegCloseKey(k);
    return out;
}

std::wstring DeviceNameFromRegistry(const std::wstring& mac) {
    std::wstring path = std::wstring(kBthDevicesKey) + L"\\" + mac;
    std::vector<BYTE> v = ReadRegBinary(HKEY_LOCAL_MACHINE, path, L"Name");
    if (v.empty())
        return L"";
    size_t end = v.size();
    while (end > 0 && v[end - 1] == 0)
        end--;
    std::string s((const char*)v.data(), end);
    return FromUtf8(s);   // 设备名以 UTF-8 存储
}

bool DeviceKeyExists(const std::wstring& mac) {
    if (mac.empty())
        return false;
    std::wstring path = std::wstring(kBthDevicesKey) + L"\\" + mac;
    HKEY k = nullptr;
    LSTATUS st = RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, KEY_READ, &k);
    if (st == ERROR_SUCCESS)
        RegCloseKey(k);
    return st == ERROR_SUCCESS;
}

// COD Major Device Class == 2（手机）或名称含手机关键字
bool IsPhoneDevice(const std::wstring& mac) {
    std::wstring path = std::wstring(kBthDevicesKey) + L"\\" + mac;
    std::vector<BYTE> cod = ReadRegBinary(HKEY_LOCAL_MACHINE, path, L"COD");
    if (cod.size() >= 2) {
        int v = (int)cod[0] | ((int)cod[1] << 8);
        if (((v >> 8) & 0x1f) == 2)
            return true;
    }
    std::wstring n = ToLowerW(DeviceNameFromRegistry(mac));
    return n.find(L"phone") != std::wstring::npos || n.find(L"mobile") != std::wstring::npos ||
           n.find(L"cell") != std::wstring::npos;
}

// 7 天内有 LastSeen 的配对设备 MAC（LastSeen 为 FILETIME，1601 纪元）
std::vector<std::wstring> GetPairedDevices() {
    std::vector<std::wstring> result;
    HKEY root = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kBthDevicesKey, 0, KEY_READ, &root) != ERROR_SUCCESS)
        return result;

    FILETIME nowFt{};
    GetSystemTimeAsFileTime(&nowFt);
    ULARGE_INTEGER now{};
    now.LowPart = nowFt.dwLowDateTime;
    now.HighPart = nowFt.dwHighDateTime;
    const ULONGLONG weekTicks = 7ULL * 24 * 60 * 60 * 10000000ULL;   // 7 天（100ns 单位）

    wchar_t name[256] = {};
    DWORD idx = 0;
    while (true) {
        DWORD nameLen = 256;
        LSTATUS st = RegEnumKeyExW(root, idx++, name, &nameLen, nullptr, nullptr, nullptr, nullptr);
        if (st != ERROR_SUCCESS)
            break;
        std::wstring mac = name;
        std::wstring path = std::wstring(kBthDevicesKey) + L"\\" + mac;
        std::vector<BYTE> ls = ReadRegBinary(HKEY_LOCAL_MACHINE, path, L"LastSeen");
        ULONGLONG ft = 0;
        if (ls.size() >= 8) {
            ft = 0;
            for (int i = 7; i >= 0; i--)
                ft = (ft << 8) | ls[(size_t)i];
        } else {
            // 可能是 REG_QWORD
            HKEY dk = nullptr;
            if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, KEY_READ, &dk) == ERROR_SUCCESS) {
                ULONGLONG q = 0;
                DWORD sz = sizeof(q), type = 0;
                if (RegQueryValueExW(dk, L"LastSeen", nullptr, &type, (LPBYTE)&q, &sz) == ERROR_SUCCESS &&
                    type == REG_QWORD)
                    ft = q;
                RegCloseKey(dk);
            }
        }
        if (ft != 0 && now.QuadPart > ft && (now.QuadPart - ft) <= weekTicks) {
            std::wstring lower = ToLowerW(mac);
            result.push_back(lower);
        }
    }
    RegCloseKey(root);
    return result;
}

// 蓝牙 Keys 键 ACL 中 ACE 数量（>= 5 视为健康；仅提权进程可读，失败返回 unknown）
void CheckKeysHealth(std::wstring& health, int& ace) {
    health = L"unknown";
    ace = -1;

    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    DWORD st = GetNamedSecurityInfoW((LPWSTR)kBthKeysKey, SE_REGISTRY_KEY, DACL_SECURITY_INFORMATION,
                                     nullptr, nullptr, &dacl, nullptr, &sd);
    if (st != ERROR_SUCCESS || dacl == nullptr) {
        if (sd)
            LocalFree(sd);
        return;
    }

    int count = 0;
    ACL_SIZE_INFORMATION info{};
    if (GetAclInformation(dacl, &info, sizeof(info), AclSizeInformation)) {
        for (DWORD i = 0; i < info.AceCount; i++) {
            void* pAce = nullptr;
            if (GetAce(dacl, i, &pAce))
                count++;
        }
    }
    ace = count;
    health = (count >= 5) ? L"healthy" : L"broken";
    LocalFree(sd);
}

void TryDisableLockdownOnLeave() {
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kLogonUiKey, 0, KEY_READ | KEY_SET_VALUE, &k) != ERROR_SUCCESS)
        return;
    DWORD v = 0, size = sizeof(v), type = 0;
    if (RegQueryValueExW(k, L"LockdownOnLeave", nullptr, &type, (LPBYTE)&v, &size) == ERROR_SUCCESS &&
        type == REG_DWORD && v != 0) {
        DWORD zero = 0;
        RegSetValueExW(k, L"LockdownOnLeave", 0, REG_DWORD, (const BYTE*)&zero, sizeof(zero));
    }
    RegCloseKey(k);
}

// ---------- 轻量 JSON（仅支持导入所需子集）----------

struct JsonValue {
    enum class Type { Null, Bool, Number, String, Object, Array } type = Type::Null;
    bool b = false;
    double num = 0;
    std::wstring str;
    std::vector<std::pair<std::wstring, JsonValue>> obj;
    std::vector<JsonValue> arr;

    const JsonValue* Find(const wchar_t* key) const {
        if (type != Type::Object)
            return nullptr;
        for (const auto& kv : obj)
            if (kv.first == key)
                return &kv.second;
        return nullptr;
    }
};

class JsonParser {
public:
    explicit JsonParser(const std::wstring& s) : _s(s) {}

    bool Parse(JsonValue& out) {
        SkipWs();
        if (!ParseValue(out))
            return false;
        SkipWs();
        return true;
    }

private:
    const std::wstring& _s;
    size_t _i = 0;

    void SkipWs() {
        while (_i < _s.size() && (_s[_i] == L' ' || _s[_i] == L'\t' || _s[_i] == L'\r' || _s[_i] == L'\n'))
            _i++;
    }

    bool ParseValue(JsonValue& v) {
        SkipWs();
        if (_i >= _s.size())
            return false;
        wchar_t c = _s[_i];
        if (c == L'{')
            return ParseObject(v);
        if (c == L'[')
            return ParseArray(v);
        if (c == L'"') {
            v.type = JsonValue::Type::String;
            return ParseString(v.str);
        }
        if (c == L't' || c == L'f') {
            if (_s.compare(_i, 4, L"true") == 0) {
                v.type = JsonValue::Type::Bool;
                v.b = true;
                _i += 4;
                return true;
            }
            if (_s.compare(_i, 5, L"false") == 0) {
                v.type = JsonValue::Type::Bool;
                v.b = false;
                _i += 5;
                return true;
            }
            return false;
        }
        if (c == L'n') {
            if (_s.compare(_i, 4, L"null") == 0) {
                v.type = JsonValue::Type::Null;
                _i += 4;
                return true;
            }
            return false;
        }
        // 数字
        {
            size_t start = _i;
            if (_i < _s.size() && (_s[_i] == L'-' || _s[_i] == L'+'))
                _i++;
            while (_i < _s.size() && (iswdigit(_s[_i]) || _s[_i] == L'.' || _s[_i] == L'e' || _s[_i] == L'E' ||
                                      _s[_i] == L'-' || _s[_i] == L'+'))
                _i++;
            if (_i == start)
                return false;
            v.type = JsonValue::Type::Number;
            v.num = wcstod(_s.substr(start, _i - start).c_str(), nullptr);
            return true;
        }
    }

    bool ParseString(std::wstring& out) {
        out.clear();
        if (_i >= _s.size() || _s[_i] != L'"')
            return false;
        _i++;
        while (_i < _s.size()) {
            wchar_t c = _s[_i++];
            if (c == L'"')
                return true;
            if (c == L'\\' && _i < _s.size()) {
                wchar_t e = _s[_i++];
                switch (e) {
                    case L'n': out.push_back(L'\n'); break;
                    case L't': out.push_back(L'\t'); break;
                    case L'r': out.push_back(L'\r'); break;
                    case L'b': out.push_back(L'\b'); break;
                    case L'f': out.push_back(L'\f'); break;
                    case L'u': {
                        if (_i + 4 <= _s.size()) {
                            wchar_t hex[5] = {};
                            for (int k = 0; k < 4; k++)
                                hex[k] = _s[_i + k];
                            _i += 4;
                            out.push_back((wchar_t)wcstol(hex, nullptr, 16));
                        }
                        break;
                    }
                    default: out.push_back(e); break;
                }
            } else {
                out.push_back(c);
            }
        }
        return false;
    }

    bool ParseObject(JsonValue& v) {
        v.type = JsonValue::Type::Object;
        _i++;   // {
        SkipWs();
        if (_i < _s.size() && _s[_i] == L'}') {
            _i++;
            return true;
        }
        while (_i < _s.size()) {
            SkipWs();
            std::wstring key;
            if (!ParseString(key))
                return false;
            SkipWs();
            if (_i >= _s.size() || _s[_i] != L':')
                return false;
            _i++;
            JsonValue child;
            if (!ParseValue(child))
                return false;
            v.obj.emplace_back(key, std::move(child));
            SkipWs();
            if (_i < _s.size() && _s[_i] == L',') {
                _i++;
                continue;
            }
            if (_i < _s.size() && _s[_i] == L'}') {
                _i++;
                return true;
            }
            return false;
        }
        return false;
    }

    bool ParseArray(JsonValue& v) {
        v.type = JsonValue::Type::Array;
        _i++;   // [
        SkipWs();
        if (_i < _s.size() && _s[_i] == L']') {
            _i++;
            return true;
        }
        while (_i < _s.size()) {
            JsonValue child;
            if (!ParseValue(child))
                return false;
            v.arr.push_back(std::move(child));
            SkipWs();
            if (_i < _s.size() && _s[_i] == L',') {
                _i++;
                continue;
            }
            if (_i < _s.size() && _s[_i] == L']') {
                _i++;
                return true;
            }
            return false;
        }
        return false;
    }
};

std::optional<int> JsonInt(const JsonValue* v) {
    if (!v)
        return std::nullopt;
    if (v->type == JsonValue::Type::Number)
        return (int)(v->num >= 0 ? v->num + 0.5 : v->num - 0.5);
    if (v->type == JsonValue::Type::String) {
        std::wstring s = Trim(v->str);
        if (s.empty())
            return std::nullopt;
        return _wtoi(s.c_str());
    }
    return std::nullopt;
}

std::optional<bool> JsonBool(const JsonValue* v) {
    if (!v)
        return std::nullopt;
    if (v->type == JsonValue::Type::Bool)
        return v->b;
    if (v->type == JsonValue::Type::Number)
        return v->num != 0;
    if (v->type == JsonValue::Type::String) {
        std::wstring s = ToLowerW(Trim(v->str));
        if (s == L"true" || s == L"1" || s == L"on" || s == L"开" || s == L"开启")
            return true;
        if (s == L"false" || s == L"0" || s == L"off" || s == L"关" || s == L"关闭")
            return false;
    }
    return std::nullopt;
}

std::wstring EscapeJson(const std::wstring& s) {
    std::wstring out;
    for (wchar_t c : s) {
        switch (c) {
            case L'"': out += L"\\\""; break;
            case L'\\': out += L"\\\\"; break;
            case L'\n': out += L"\\n"; break;
            case L'\r': out += L"\\r"; break;
            case L'\t': out += L"\\t"; break;
            default:
                if (c < 0x20) {
                    wchar_t buf[8] = {};
                    swprintf_s(buf, L"\\u%04x", (unsigned)c);
                    out += buf;
                } else {
                    out.push_back(c);
                }
        }
    }
    return out;
}

std::wstring ExeDir() {
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring p = buf;
    size_t pos = p.find_last_of(L"\\/");
    return (pos == std::wstring::npos) ? p : p.substr(0, pos + 1);
}

std::wstring ResolvePath(const std::wstring& file) {
    if (file.empty())
        return ExeDir() + L"data\\锁屏设置.json";
    if (file.find(L':') != std::wstring::npos)
        return file;   // 绝对路径
    if (file.find(L'\\') != std::wstring::npos || file.find(L'/') != std::wstring::npos)
        return ExeDir() + file;
    return ExeDir() + L"data\\" + file;
}

}  // namespace

// ==================== 动态锁 ====================

DynlockState DynlockManager::Get() {
    DynlockState s;

    if (auto v = ReadRegDword(HKEY_CURRENT_USER, kWinlogonKey, L"EnableGoodbye"))
        s.enabled = (*v == 1);

    if (auto dp = ReadRegString(HKEY_CURRENT_USER, kWinlogonKey, L"DevicePairing"))
        s.selectedMac = Trim(*dp);

    std::vector<std::wstring> paired = GetPairedDevices();
    s.pairedCount = (int)paired.size();
    for (const auto& m : paired)
        if (IsPhoneDevice(m))
            s.phoneCount++;

    if (s.selectedMac.empty())
        s.deviceSelected = false;
    else if (DeviceKeyExists(s.selectedMac))
        s.deviceSelected = true;
    else {
        std::wstring lower = ToLowerW(s.selectedMac);
        s.deviceSelected = std::find(paired.begin(), paired.end(), lower) != paired.end();
    }

    CheckKeysHealth(s.keysHealth, s.keysAce);

    if (!s.selectedMac.empty()) {
        std::wstring n = DeviceNameFromRegistry(s.selectedMac);
        s.selectedName = n.empty() ? s.selectedMac : n;
    }

    if (auto v = ReadRegDword(HKEY_CURRENT_USER, kLogonUiKey, L"LockdownOnLeave"))
        s.lockdownOnLeave = (*v != 0);

    return s;
}

bool DynlockManager::Set(bool on, std::wstring& warning, std::wstring& error) {
    warning.clear();
    error.clear();

    HKEY k = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kWinlogonKey, 0, nullptr, 0, KEY_SET_VALUE | KEY_READ,
                        nullptr, &k, nullptr) != ERROR_SUCCESS) {
        error = L"无法打开注册表 Winlogon 键（权限不足？）";
        return false;
    }

    DWORD val = on ? 1 : 0;
    if (RegSetValueExW(k, L"EnableGoodbye", 0, REG_DWORD, (const BYTE*)&val, sizeof(val)) != ERROR_SUCCESS) {
        RegCloseKey(k);
        error = L"写入 EnableGoodbye 失败";
        return false;
    }

    if (on) {
        // 开启时若没有有效信任设备则自动补选（优先手机类），否则"开关开了但永不锁屏"用户无感
        std::wstring dp;
        if (auto cur = ReadRegString(HKEY_CURRENT_USER, kWinlogonKey, L"DevicePairing"))
            dp = Trim(*cur);

        if (dp.empty() || !DeviceKeyExists(dp)) {
            std::vector<std::wstring> paired = GetPairedDevices();
            std::wstring best;
            for (const auto& m : paired) {
                if (IsPhoneDevice(m)) {
                    best = m;
                    break;
                }
            }
            if (best.empty() && !paired.empty())
                best = paired[0];

            if (!best.empty()) {
                RegSetValueExW(k, L"DevicePairing", 0, REG_SZ, (const BYTE*)best.c_str(),
                               (DWORD)((best.size() + 1) * sizeof(wchar_t)));
                std::wstring name = DeviceNameFromRegistry(best);
                warning = L"已自动选择信任设备：" + (name.empty() ? best : name);
            } else if (dp.empty()) {
                warning = L"已开启，但未检测到已配对的蓝牙设备，请先在系统设置中配对手机";
            }
        }
    } else {
        // Win11 25H2「离开时锁定」是独立开关：关闭动态锁时一并关掉，否则仍会离开即锁屏
        TryDisableLockdownOnLeave();
    }

    RegCloseKey(k);
    return true;
}

bool DynlockManager::OpenSettings(const wchar_t* target) {
    std::wstring uri = L"ms-settings:signinoptions-dynamiclock";
    if (target) {
        std::wstring t = target;
        if (t == L"signin")
            uri = L"ms-settings:signinoptions";
        else if (t == L"bluetooth")
            uri = L"ms-settings:bluetooth";
    }
    HINSTANCE h = ShellExecuteW(nullptr, L"open", uri.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return (INT_PTR)h > 32;
}

// ==================== 配置导入导出 ====================

std::wstring ConfigIO::DefaultConfigPath() {
    return ExeDir() + L"data\\锁屏设置.json";
}

bool ConfigIO::Export(const std::wstring& file, std::wstring& outPath, std::wstring& err) {
    outPath = ResolvePath(file);
    err.clear();

    // 确保 data 目录存在
    size_t slash = outPath.find_last_of(L'\\');
    if (slash != std::wstring::npos)
        CreateDirectoryW(outPath.substr(0, slash).c_str(), nullptr);

    std::vector<std::pair<PowerKey, PowerState>> items;
    std::wstring qerr;
    if (!PowerManager::QueryAll(items, qerr) && !qerr.empty()) {
        err = qerr;
        return false;
    }

    SaverState sv = SaverManager::Get();
    DynlockState dl = DynlockManager::Get();
    std::wstring schemeName, schemeGuid;
    PowerManager::ActiveScheme(schemeName, schemeGuid);

    auto numOrNull = [](const std::optional<int>& v) -> std::wstring {
        return v ? std::to_wstring(*v) : L"null";
    };

    std::wstring j;
    j += L"{\r\n";
    j += L"  \"app\": \"lockscreen-delay\",\r\n";
    j += L"  \"type\": \"lockscreen-delay-config\",\r\n";
    j += L"  \"version\": \"3.2.0-cpp\",\r\n";

    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t ts[64] = {};
    swprintf_s(ts, L"%04d-%02d-%02dT%02d:%02d:%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
               st.wSecond);
    j += std::wstring(L"  \"exportedAt\": \"") + ts + L"\",\r\n";

    wchar_t comp[128] = {};
    DWORD compLen = 128;
    GetComputerNameW(comp, &compLen);
    j += std::wstring(L"  \"computer\": \"") + EscapeJson(comp) + L"\",\r\n";

    j += L"  \"scheme\": { \"name\": \"" + EscapeJson(schemeName) + L"\", \"guid\": \"" + schemeGuid + L"\" },\r\n";

    j += L"  \"items\": {\r\n";
    for (int i = 0; i < (int)PowerKey::Count; i++) {
        PowerKey k = (PowerKey)i;
        PowerState ps{};
        for (const auto& p : items)
            if (p.first == k)
                ps = p.second;
        const wchar_t* keyName = nullptr;
        switch (k) {
            case PowerKey::Lock: keyName = L"lock"; break;
            case PowerKey::Display: keyName = L"display"; break;
            case PowerKey::Sleep: keyName = L"sleep"; break;
            case PowerKey::Hibernate: keyName = L"hibernate"; break;
            default: break;
        }
        j += L"    \"" + std::wstring(keyName) + L"\": { \"ac\": " + numOrNull(ps.ac) + L", \"dc\": " +
             numOrNull(ps.dc) + L" }";
        j += (i + 1 < (int)PowerKey::Count) ? L",\r\n" : L"\r\n";
    }
    j += L"  },\r\n";

    wchar_t saverLine[256] = {};
    swprintf_s(saverLine, L"  \"saver\": { \"active\": %s, \"timeout\": %d, \"secure\": %s },\r\n",
               sv.active ? L"true" : L"false", sv.timeout, sv.secure ? L"true" : L"false");
    j += saverLine;

    j += std::wstring(L"  \"dynlock\": { \"enabled\": ") + (dl.enabled ? L"true" : L"false") + L" }\r\n";
    j += L"}\r\n";

    std::ofstream f(outPath.c_str(), std::ios::binary | std::ios::trunc);
    if (!f) {
        err = L"无法写入文件：" + outPath;
        return false;
    }
    std::string utf8 = ToUtf8(j);
    f.write(utf8.data(), (std::streamsize)utf8.size());
    f.close();
    return true;
}

bool ConfigIO::Import(const std::wstring& file, std::vector<std::wstring>& results,
                      std::vector<std::wstring>& errors) {
    results.clear();
    errors.clear();

    std::wstring path = ResolvePath(file);
    std::ifstream f(path.c_str(), std::ios::binary);
    if (!f) {
        errors.push_back(L"找不到配置文件：" + path);
        return false;
    }
    std::string raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    f.close();
    if (raw.empty()) {
        errors.push_back(L"配置文件为空：" + path);
        return false;
    }

    std::wstring text = FromUtf8(raw);
    JsonValue root;
    JsonParser parser(text);
    if (!parser.Parse(root) || root.type != JsonValue::Type::Object) {
        errors.push_back(L"配置文件不是有效的 JSON：" + path);
        return false;
    }
    const JsonValue* itemsNode = root.Find(L"items");
    if (!itemsNode) {
        errors.push_back(L"这不像是本工具导出的配置文件（缺少 items 字段）");
        return false;
    }

    int applied = 0;

    // 电源项：先批量写（不激活），最后统一 Activate 一次
    struct KeyMap {
        const wchar_t* json;
        PowerKey key;
    };
    const KeyMap maps[] = {
        { L"lock", PowerKey::Lock },
        { L"display", PowerKey::Display },
        { L"sleep", PowerKey::Sleep },
        { L"hibernate", PowerKey::Hibernate },
    };
    for (const auto& m : maps) {
        const JsonValue* node = itemsNode->Find(m.json);
        if (!node)
            continue;
        auto ac = JsonInt(node->Find(L"ac"));
        auto dc = JsonInt(node->Find(L"dc"));
        if (!ac || !dc)
            continue;   // 任一侧缺失则跳过该项，保持目标机原值
        applied++;

        std::vector<std::wstring> errs;
        PowerManager::SetItem(m.key, *ac, "ac", false, errs);
        for (const auto& e : errs)
            errors.push_back(std::wstring(PowerKeyName(m.key)) + L"：接通电源 " + e);
        errs.clear();
        PowerManager::SetItem(m.key, *dc, "dc", false, errs);
        for (const auto& e : errs)
            errors.push_back(std::wstring(PowerKeyName(m.key)) + L"：使用电池 " + e);

        results.push_back(std::wstring(L"  ") + PowerKeyName(m.key) + L"  接通电源 " +
                          FormatSeconds(*ac) + L"  使用电池 " + FormatSeconds(*dc));
    }
    if (applied > 0) {
        std::wstring err;
        if (!PowerManager::Activate(err))
            errors.push_back(L"使设置生效：" + err);
    }

    // 屏保
    if (const JsonValue* saver = root.Find(L"saver")) {
        auto on = JsonBool(saver->Find(L"active")).value_or(false);
        auto t = JsonInt(saver->Find(L"timeout"));
        auto sec = JsonBool(saver->Find(L"secure"));
        std::vector<std::wstring> errs;
        SaverManager::Set(on, t, sec, errs);
        for (const auto& e : errs)
            errors.push_back(L"屏幕保护程序：" + e);
        results.push_back(std::wstring(L"  屏幕保护程序  ") +
                          (on ? FormatSeconds(t.value_or(0)) : L"已关闭"));
    }

    // 动态锁（旧配置无此段则跳过，向后兼容）
    if (const JsonValue* dl = root.Find(L"dynlock")) {
        if (auto on = JsonBool(dl->Find(L"enabled"))) {
            std::wstring warning, err;
            if (!DynlockManager::Set(*on, warning, err)) {
                errors.push_back(L"动态锁：" + (err.empty() ? L"未知错误" : err));
            } else {
                std::wstring line = std::wstring(L"  动态锁  ") +
                                    (*on ? L"已开启" : L"已关闭（含 Win11「离开时锁定」独立开关）");
                if (*on && !warning.empty())
                    line += L"（" + warning + L"）";
                results.push_back(line);
            }
        }
    }

    if (applied == 0 && !root.Find(L"saver") && !root.Find(L"dynlock"))
        errors.push_back(L"配置里没有可用的设置项（items 为空）");

    return errors.empty();
}

}  // namespace lockscreen
