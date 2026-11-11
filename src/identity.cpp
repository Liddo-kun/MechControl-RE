#include "profiles.h"
#include "config_text.h"
#include <windows.h>
#include <wbemidl.h>
#include <wrl/client.h>
#include <intrin.h>
#include <cstring>

namespace mech::config {
using Microsoft::WRL::ComPtr;
static std::string query(IWbemServices* services, const wchar_t* sql, const wchar_t* key) {
    BSTR language = SysAllocString(L"WQL"), text = SysAllocString(sql);
    ComPtr<IEnumWbemClassObject> rows;
    HRESULT hr =
        services->ExecQuery(language, text, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &rows);
    SysFreeString(language);
    SysFreeString(text);
    if (FAILED(hr)) return {};
    ComPtr<IWbemClassObject> row;
    ULONG count = 0;
    while (SUCCEEDED(rows->Next(5000, 1, &row, &count)) && count) {
        VARIANT value;
        VariantInit(&value);
        hr = row->Get(key, 0, &value, nullptr, nullptr);
        std::string result;
        if (SUCCEEDED(hr) && value.vt == VT_BSTR) result = utf8({value.bstrVal, SysStringLen(value.bstrVal)});
        VariantClear(&value);
        row.Reset();
        if (!blank(result)) return result;
    }
    return {};
}
Identity collect_identity(const std::function<uint8_t(int)>& read_ec) {
    Identity id;
    int cpu[4]{};
    __cpuid(cpu, 0);
    char vendor[13]{};
    memcpy(vendor, &cpu[1], 4);
    memcpy(vendor + 4, &cpu[3], 4);
    memcpy(vendor + 8, &cpu[2], 4);
    if (std::string_view(vendor) == "GenuineIntel")
        id.cpu_vendor = "intel";
    else if (std::string_view(vendor) == "AuthenticAMD")
        id.cpu_vendor = "amd";
    if (read_ec) try {
            id.project_id = read_ec(0x740);
        } catch (const std::exception&) {
        }
    HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(init) && init != RPC_E_CHANGED_MODE) return id;
    ComPtr<IWbemLocator> locator;
    ComPtr<IWbemServices> services;
    if (SUCCEEDED(CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&locator)))) {
        BSTR ns = SysAllocString(L"ROOT\\CIMV2");
        HRESULT hr = locator->ConnectServer(ns, nullptr, nullptr, nullptr, 0, nullptr, nullptr, &services);
        SysFreeString(ns);
        if (SUCCEEDED(hr) &&
            SUCCEEDED(CoSetProxyBlanket(services.Get(), RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr,
                                        RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE))) {
            id.baseboard = query(services.Get(), L"SELECT Product FROM Win32_BaseBoard", L"Product");
            id.cpu_name = query(services.Get(), L"SELECT Name FROM Win32_Processor", L"Name");
        }
    }
    services.Reset();
    locator.Reset();
    if (SUCCEEDED(init)) CoUninitialize();
    return id;
}
}
