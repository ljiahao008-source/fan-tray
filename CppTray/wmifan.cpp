#include "wmifan.h"

#include <comdef.h>
#include <wbemidl.h>
#include <cstdint>
#include <cstring>

#pragma comment(lib, "wbemuuid.lib")

namespace {
constexpr uint32_t kInvalid = 0x7FFFFFFFu;  // EC 返回的"无效/不支持"标记
}  // namespace

bool MechrevoFan::Init() {
    Close();

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (SUCCEEDED(hr))
        _comInited = true;
    else if (hr != RPC_E_CHANGED_MODE)
        return false;

    // 直接调用 WMI 必须初始化 COM 安全（.NET System.Management 内部自动处理，C++ 需手动）。
    // 每进程一次；重复调用会返回 RPC_E_TOO_LATE，忽略即可。
    static bool s_securityInited = false;
    if (!s_securityInited) {
        CoInitializeSecurity(nullptr, -1, nullptr, nullptr,
                             RPC_C_AUTHN_LEVEL_PKT_PRIVACY,
                             RPC_C_IMP_LEVEL_IMPERSONATE,
                             nullptr, EOAC_NONE, nullptr);
        s_securityInited = true;
    }

    IWbemLocator* loc = nullptr;
    hr = CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER,
                          IID_IWbemLocator, (void**)&loc);
    if (FAILED(hr)) {
        Close();
        return false;
    }
    _loc = loc;

    IWbemServices* svc = nullptr;
    hr = loc->ConnectServer(_bstr_t(L"root\\WMI"), nullptr, nullptr, nullptr,
                            0, nullptr, nullptr, &svc);
    if (FAILED(hr)) {
        Close();
        return false;
    }
    _wmi = svc;

    IEnumWbemClassObject* enumerator = nullptr;
    hr = svc->ExecQuery(_bstr_t(L"WQL"),
                        _bstr_t(L"SELECT * FROM PowerSwitchInterface"),
                        WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
                        nullptr, &enumerator);
    if (FAILED(hr) || enumerator == nullptr) {
        Close();
        return false;
    }

    IWbemClassObject* obj = nullptr;
    ULONG returned = 0;
    hr = enumerator->Next(WBEM_INFINITE, 1, &obj, &returned);
    enumerator->Release();
    if (FAILED(hr) || returned == 0 || obj == nullptr) {
        Close();
        return false;
    }

    // 读 key 属性 InstanceName 构造实例路径（动态 ACPI 类无 __PATH/__RELPATH，
    // ExecMethod 需相对路径 + 反斜杠转义，如 PowerSwitchInterface.InstanceName="ACPI\\PNP0C14\\..."）
    VARIANT nameVar;
    VariantInit(&nameVar);
    if (SUCCEEDED(obj->Get(L"InstanceName", 0, &nameVar, nullptr, nullptr)) &&
        nameVar.vt == VT_BSTR && nameVar.bstrVal != nullptr) {
        std::wstring in = nameVar.bstrVal;
        std::wstring escaped;
        escaped.reserve(in.size() + 8);
        for (wchar_t c : in) {
            if (c == L'\\')
                escaped += L"\\\\";
            else
                escaped += c;
        }
        std::wstring path = L"PowerSwitchInterface.InstanceName=\"" + escaped + L"\"";
        _path = SysAllocString(path.c_str());
    }
    VariantClear(&nameVar);

    // 类对象（GetMethod 必须在类对象上调用，实例上会返回 WBEM_E_INVALID_PARAMETER_ID）
    IWbemClassObject* cls = nullptr;
    hr = svc->GetObject(_bstr_t(L"PowerSwitchInterface"), 0, nullptr, &cls, nullptr);
    if (FAILED(hr) || cls == nullptr) {
        Close();
        return false;
    }
    _cls = cls;

    _obj = obj;
    return _path != nullptr;
}

float MechrevoFan::ReadFanRpm() {
    IWbemClassObject* cls = (IWbemClassObject*)_cls;
    IWbemServices* svc = (IWbemServices*)_wmi;
    if (!cls || !svc || !_path)
        return -1.f;

    try {
        // 方法签名输入参数类 → 实例化 → 设置 FanNumber=1（同 C# GetMethodParameters）
        IWbemClassObject* inSig = nullptr;
        if (FAILED(cls->GetMethod(L"GetFanControl", 0, &inSig, nullptr)))
            return -1.f;

        IWbemClassObject* inInst = nullptr;
        HRESULT hr = inSig->SpawnInstance(0, &inInst);
        inSig->Release();
        if (FAILED(hr) || inInst == nullptr)
            return -1.f;

        VARIANT vFan;
        VariantInit(&vFan);
        vFan.vt = VT_UI1;
        vFan.bVal = 1;
        inInst->Put(L"FanNumber", 0, &vFan, 0);
        VariantClear(&vFan);

        IWbemClassObject* outObj = nullptr;
        hr = svc->ExecMethod(_bstr_t(_path), _bstr_t(L"GetFanControl"), 0, nullptr,
                             inInst, &outObj, nullptr);
        inInst->Release();
        if (FAILED(hr) || outObj == nullptr)
            return -1.f;

        VARIANT duty;
        VariantInit(&duty);
        hr = outObj->Get(L"FanDuty", 0, &duty, nullptr, nullptr);
        outObj->Release();
        if (FAILED(hr))
            return -1.f;

        uint32_t fanDuty = 0;
        if (duty.vt == VT_I4)
            fanDuty = (uint32_t)duty.lVal;
        else if (duty.vt == VT_UI4)
            fanDuty = duty.ulVal;
        else if (duty.vt == VT_UI8)
            fanDuty = (uint32_t)(duty.ullVal & 0xFFFFFFFFu);
        VariantClear(&duty);

        auto Parse = [](uint32_t raw) -> float {
            return (raw > 0 && raw < kInvalid) ? (float)raw : -1.f;
        };
        float fan1 = Parse(fanDuty & 0xFFFFu);
        if (fan1 >= 0.f)
            return fan1;
        return Parse((fanDuty >> 16) & 0xFFFFu);
    } catch (...) {
        return -1.f;
    }
}

void MechrevoFan::Close() {
    if (_obj) {
        ((IWbemClassObject*)_obj)->Release();
        _obj = nullptr;
    }
    if (_cls) {
        ((IWbemClassObject*)_cls)->Release();
        _cls = nullptr;
    }
    if (_wmi) {
        ((IWbemServices*)_wmi)->Release();
        _wmi = nullptr;
    }
    if (_loc) {
        ((IWbemLocator*)_loc)->Release();
        _loc = nullptr;
    }
    if (_path) {
        SysFreeString(_path);
        _path = nullptr;
    }
    if (_comInited) {
        CoUninitialize();
        _comInited = false;
    }
}
