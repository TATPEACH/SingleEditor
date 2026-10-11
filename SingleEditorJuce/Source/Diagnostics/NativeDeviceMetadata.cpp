#include "NativeDeviceMetadata.h"
#include "DiagnosticTestActivity.h"
#if JUCE_WINDOWS
 #include <windows.h>
 #include <initguid.h>
 #include <mmdeviceapi.h>
 #include <devicetopology.h>
 #include <functiondiscoverykeys_devpkey.h>
 #include <cfgmgr32.h>
 #include <devpkey.h>
 #include <wrl/client.h>
#endif

namespace singleeditor::diagnostics
{
namespace
{
constexpr Field metadataFields[] { Field::driverVendor, Field::driverProduct, Field::driverVersion,
    Field::hardwareManufacturer, Field::driverMetadataSource, Field::speakerMask, Field::speakerMaskSource };
void set(ProviderEvidence& result, Field field, const juce::var& value)
{
    for (auto& item : result.fields) if (item.field == field) { item.value = value; return; }
    result.fields.push_back({ field, value });
}
#if JUCE_WINDOWS
template<class T> using ComPtr = Microsoft::WRL::ComPtr<T>;
struct ComScope
{
    HRESULT status = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ~ComScope() { if (SUCCEEDED(status)) CoUninitialize(); }
};
struct Property
{
    PROPVARIANT value {};
    ~Property() { PropVariantClear(&value); }
};
struct TaskString
{
    LPWSTR value = nullptr;
    ~TaskString() { CoTaskMemFree(value); }
};
juce::String boundedString(const wchar_t* value, size_t limit = 2048)
{
    if (value == nullptr) return {};
    const auto count = wcsnlen_s(value, limit);
    return count > 0 && count < limit ? juce::String(value, count) : juce::String();
}
juce::String propertyString(IPropertyStore& store, REFPROPERTYKEY key)
{
    Property property;
    if (FAILED(store.GetValue(key, &property.value)) || property.value.vt != VT_LPWSTR) return {};
    return boundedString(property.value.pwszVal);
}
// Load only on an explicit metadata request. No new DLL import/startup work and
// no tree search: the exact adapter devnode comes from the endpoint connector.
struct DeviceProperties
{
    HMODULE module = LoadLibraryExW(L"cfgmgr32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    decltype(&CM_Locate_DevNodeW) locate = module ? reinterpret_cast<decltype(locate)>(GetProcAddress(module, "CM_Locate_DevNodeW")) : nullptr;
    decltype(&CM_Get_DevNode_PropertyW) get = module ? reinterpret_cast<decltype(get)>(GetProcAddress(module, "CM_Get_DevNode_PropertyW")) : nullptr;
    ~DeviceProperties() { if (module) FreeLibrary(module); }
    juce::String read(DEVINST node, const DEVPROPKEY& key) const
    {
        if (!get) return {};
        wchar_t buffer[2048] {};
        ULONG size = sizeof(buffer);
        DEVPROPTYPE type = 0;
        if (get(node, &key, &type, reinterpret_cast<PBYTE>(buffer), &size, 0) != CR_SUCCESS
            || type != DEVPROP_TYPE_STRING || size < sizeof(wchar_t) || size > sizeof(buffer)
            || size % sizeof(wchar_t) != 0 || buffer[size / sizeof(wchar_t) - 1] != L'\0') return {};
        return boundedString(buffer);
    }
};
#endif
}
ProviderEvidence readNativeDeviceMetadata(const juce::String& endpoint, const std::atomic<bool>& cancelled)
{
    SINGLE_EDITOR_DIAGNOSTIC_ACTIVITY(nativeMetadata);
    ProviderEvidence result;
    for (const auto field : metadataFields) result.fields.push_back({ field, {}, Unavailable::notReported });
    set(result, Field::deviceMetadataState, "not-supported");
    if (cancelled.load()) { set(result, Field::deviceMetadataState, "cancelled"); return result; }
#if JUCE_WINDOWS
    if (endpoint.isEmpty() || endpoint.length() > 2048) return result;
    set(result, Field::deviceMetadataState, "endpoint-unavailable");
    ComScope com;
    if (FAILED(com.status)) return result;
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
                               IID_PPV_ARGS(&enumerator))) || cancelled.load()) return result;
    ComPtr<IMMDevice> device;
    if (FAILED(enumerator->GetDevice(endpoint.toWideCharPointer(), &device)) || cancelled.load()) return result;
    set(result, Field::deviceMetadataState, "endpoint-resolved");
    ComPtr<IPropertyStore> properties;
    if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &properties)))
    {
        Property mask;
        if (SUCCEEDED(properties->GetValue(PKEY_AudioEndpoint_PhysicalSpeakers, &mask.value)) && mask.value.vt == VT_UI4)
        {
            set(result, Field::speakerMask, static_cast<juce::int64>(mask.value.ulVal));
            set(result, Field::speakerMaskSource, "windows-endpoint-physical-speakers-property");
        }
    }
    if (cancelled.load()) return result;
    ComPtr<IDeviceTopology> topology;
    // Query topology metadata only. Never activate IAudioClient or a control.
    if (FAILED(device->Activate(__uuidof(IDeviceTopology), CLSCTX_INPROC_SERVER, nullptr,
                                reinterpret_cast<void**>(topology.GetAddressOf())))) return result;
    UINT count = 0;
    if (FAILED(topology->GetConnectorCount(&count)) || count != 1 || cancelled.load()) return result;
    ComPtr<IConnector> connector;
    if (FAILED(topology->GetConnector(0, &connector))) return result;
    TaskString adapterId;
    if (FAILED(connector->GetDeviceIdConnectedTo(&adapterId.value)) || cancelled.load()) return result;
    const auto id = boundedString(adapterId.value);
    if (id.isEmpty()) return result;
    ComPtr<IMMDevice> adapter;
    ComPtr<IPropertyStore> adapterProperties;
    if (FAILED(enumerator->GetDevice(id.toWideCharPointer(), &adapter)) || cancelled.load()
        || FAILED(adapter->OpenPropertyStore(STGM_READ, &adapterProperties))) return result;
    const auto instance = propertyString(*adapterProperties.Get(), PKEY_Device_InstanceId);
    if (instance.isEmpty() || cancelled.load()) return result;
    DeviceProperties native;
    DEVINST node = 0;
    if (!native.locate || !native.get || native.locate(&node, const_cast<wchar_t*>(instance.toWideCharPointer()), CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS)
        return result;
    set(result, Field::deviceMetadataState, "adapter-resolved");
    set(result, Field::driverMetadataSource, "endpoint-connector-adapter-devnode-installed-INF");
    for (const auto& entry : { std::pair{ Field::driverVendor, &DEVPKEY_Device_DriverProvider },
                              std::pair{ Field::driverProduct, &DEVPKEY_Device_DeviceDesc },
                              std::pair{ Field::driverVersion, &DEVPKEY_Device_DriverVersion },
                              std::pair{ Field::hardwareManufacturer, &DEVPKEY_Device_Manufacturer } })
    {
        if (cancelled.load()) return result;
        const auto value = native.read(node, *entry.second);
        if (value.isNotEmpty()) set(result, entry.first, value);
    }
#else
    juce::ignoreUnused(endpoint);
#endif
    return result;
}
}
