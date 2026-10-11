// Included after the build-local, hash-pinned JUCE 8.0.12 overlay. Only
// public constructors are used; no private-layout access or SDK COM rewrite.
#include "NativeAsioDevice.h"

#if JUCE_WINDOWS && JUCE_ASIO && JUCE_ASIO_USE_EXTERNAL_SDK
namespace singleeditor::asio_native
{
struct Key
{
    HKEY handle = nullptr;
    ~Key() { if (handle != nullptr) RegCloseKey(handle); }
};

static std::optional<juce::String> readString(HKEY key, const wchar_t* name)
{
    DWORD kind = 0, bytes = 0;
    if (RegQueryValueExW(key, name, nullptr, &kind, nullptr, &bytes) != ERROR_SUCCESS
        || (kind != REG_SZ && kind != REG_EXPAND_SZ) || bytes == 0
        || bytes > 65536 || bytes % sizeof(wchar_t) != 0)
        return {};
    std::vector<wchar_t> value(bytes / sizeof(wchar_t) + 1, 0);
    if (RegQueryValueExW(key, name, nullptr, &kind,
                        reinterpret_cast<BYTE*>(value.data()), &bytes) != ERROR_SUCCESS)
        return {};
    return juce::String(value.data());
}

static juce::String canonicalId(const CLSID& id)
{
    wchar_t text[40] {};
    return StringFromGUID2(id, text, 40) != 0 ? juce::String(text).toUpperCase() : juce::String{};
}
}
#endif

std::optional<singleeditor::AsioClockState> singleeditor::copyAsioClockState(const juce::AudioIODevice* device)
{
   #if JUCE_WINDOWS && JUCE_ASIO && JUCE_ASIO_USE_EXTERNAL_SDK
    if (const auto* asio = dynamic_cast<const juce::ASIOAudioIODevice*>(device))
        return asio->readClockStateForSingleEditor();
   #else
    juce::ignoreUnused(device);
   #endif
    return {};
}

bool singleeditor::armAsioDriverEvents(juce::AudioIODevice* device, std::uint64_t generation)
{
   #if JUCE_WINDOWS && JUCE_ASIO && JUCE_ASIO_USE_EXTERNAL_SDK
    if (auto* asio = dynamic_cast<juce::ASIOAudioIODevice*>(device)) return asio->armEventsForSingleEditor(generation);
   #else
    juce::ignoreUnused(device, generation);
   #endif
    return false;
}

std::optional<singleeditor::DeviceOutput::DriverIdentity> singleeditor::copyAsioDriverIdentity(const juce::AudioIODevice* device)
{
   #if JUCE_WINDOWS && JUCE_ASIO && JUCE_ASIO_USE_EXTERNAL_SDK
    if (const auto* asio = dynamic_cast<const juce::ASIOAudioIODevice*>(device))
        if (const auto identity = asio->copyDriverIdentityForSingleEditor()) return *identity;
   #else
    juce::ignoreUnused(device);
   #endif
    return {};
}

singleeditor::AsioDriverEvents::Snapshot singleeditor::readAsioDriverEvents(juce::AudioIODevice* device, std::uint64_t generation, bool consume, bool includeCounters)
{
   #if JUCE_WINDOWS && JUCE_ASIO && JUCE_ASIO_USE_EXTERNAL_SDK
    if (auto* asio = dynamic_cast<juce::ASIOAudioIODevice*>(device)) return asio->readEventsForSingleEditor(generation, consume, includeCounters);
   #else
    juce::ignoreUnused(device, generation, consume, includeCounters);
   #endif
    return {};
}

bool singleeditor::showManagedAsioControlPanel(juce::AudioIODevice* device, std::uint64_t generation)
{
   #if JUCE_WINDOWS && JUCE_ASIO && JUCE_ASIO_USE_EXTERNAL_SDK
    if (auto* asio = dynamic_cast<juce::ASIOAudioIODevice*>(device)) return asio->showManagedPanelForSingleEditor(generation);
   #else
    juce::ignoreUnused(device, generation);
   #endif
    return false;
}

bool singleeditor::acknowledgeAsioRevalidation(juce::AudioIODevice* device, std::uint64_t generation)
{
   #if JUCE_WINDOWS && JUCE_ASIO && JUCE_ASIO_USE_EXTERNAL_SDK
    if (auto* asio = dynamic_cast<juce::ASIOAudioIODevice*>(device)) return asio->acknowledgeRevalidationForSingleEditor(generation);
   #else
    juce::ignoreUnused(device, generation);
   #endif
    return false;
}

std::optional<singleeditor::AsioDriverActualState> singleeditor::readAsioDriverActualState(const juce::AudioIODevice* device)
{
   #if JUCE_WINDOWS && JUCE_ASIO && JUCE_ASIO_USE_EXTERNAL_SDK
    if (const auto* asio = dynamic_cast<const juce::ASIOAudioIODevice*>(device)) return asio->readActualStateForSingleEditor();
   #else
    juce::ignoreUnused(device);
   #endif
    return {};
}

bool singleeditor::requestAsioDriverReset(juce::AudioIODevice* device, std::uint64_t generation)
{
   #if JUCE_WINDOWS && JUCE_ASIO && JUCE_ASIO_USE_EXTERNAL_SDK
    if (auto* asio = dynamic_cast<juce::ASIOAudioIODevice*>(device)) return asio->requestResetForSingleEditor(generation);
   #else
    juce::ignoreUnused(device, generation);
   #endif
    return false;
}

std::optional<std::pair<int, int>> singleeditor::readAsioDriverLatencies(const juce::AudioIODevice* device)
{
   #if JUCE_WINDOWS && JUCE_ASIO && JUCE_ASIO_USE_EXTERNAL_SDK
    if (const auto* asio = dynamic_cast<const juce::ASIOAudioIODevice*>(device)) return asio->readLatenciesForSingleEditor();
   #else
    juce::ignoreUnused(device);
   #endif
    return {};
}

bool singleeditor::releaseClosedAsioDriver(juce::AudioIODevice* device)
{
   #if JUCE_WINDOWS && JUCE_ASIO && JUCE_ASIO_USE_EXTERNAL_SDK
    if (auto* asio = dynamic_cast<juce::ASIOAudioIODevice*>(device)) return asio->releaseDriverForSingleEditor();
   #else
    juce::ignoreUnused(device);
   #endif
    return false;
}
bool singleeditor::prepareClosedAsioDriver(juce::AudioIODevice* device)
{
   #if JUCE_WINDOWS && JUCE_ASIO && JUCE_ASIO_USE_EXTERNAL_SDK
    if (auto* asio = dynamic_cast<juce::ASIOAudioIODevice*>(device)) return asio->prepareDriverForSingleEditor();
   #else
    juce::ignoreUnused(device);
   #endif
    return false;
}
bool singleeditor::refreshAsioDriverCapabilities(juce::AudioIODevice* device)
{
   #if JUCE_WINDOWS && JUCE_ASIO && JUCE_ASIO_USE_EXTERNAL_SDK
    if (auto* asio = dynamic_cast<juce::ASIOAudioIODevice*>(device)) return asio->refreshCapabilitiesForSingleEditor();
   #else
    juce::ignoreUnused(device);
   #endif
    return false;
}

std::optional<std::vector<singleeditor::AsioDriverRegistration>> singleeditor::readAsioDriverRegistrations()
{
   #if JUCE_WINDOWS && JUCE_ASIO && JUCE_ASIO_USE_EXTERNAL_SDK
    using namespace asio_native;
    Key root;
    const auto opened = RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ASIO", 0, KEY_READ, &root.handle);
    if (opened == ERROR_FILE_NOT_FOUND) return std::vector<AsioDriverRegistration>{};
    if (opened != ERROR_SUCCESS) return {};
    std::vector<AsioDriverRegistration> result;
    for (DWORD index = 0;; ++index)
    {
        wchar_t keyName[256] {};
        DWORD keyLength = 256;
        const auto enumerated = RegEnumKeyExW(root.handle, index, keyName, &keyLength, nullptr, nullptr, nullptr, nullptr);
        if (enumerated == ERROR_NO_MORE_ITEMS) break;
        if (enumerated != ERROR_SUCCESS) return {};
        Key entry;
        if (RegOpenKeyExW(root.handle, keyName, 0, KEY_READ, &entry.handle) != ERROR_SUCCESS) continue;
        const auto classText = readString(entry.handle, L"clsid");
        CLSID id {};
        if (!classText || CLSIDFromString(classText->toWideCharPointer(), &id) != S_OK) continue;
        const auto classId = canonicalId(id);
        const auto classKey = "CLSID\\" + classId + "\\InprocServer32";
        Key server;
        if (RegOpenKeyExW(HKEY_CLASSES_ROOT, classKey.toWideCharPointer(), 0, KEY_READ, &server.handle) != ERROR_SUCCESS) continue;
        const auto serverPath = readString(server.handle, nullptr);
        if (!serverPath || serverPath->isEmpty()) continue;
        const auto description = readString(entry.handle, L"description");
        const auto name = description.value_or(juce::String(keyName));
        result.push_back({ name.toStdString(), classId.toStdString(), juce::String(keyName).toStdString(), serverPath->toStdString() });
    }
    return result;
   #else
    return {};
   #endif
}

std::unique_ptr<juce::AudioIODevice> singleeditor::createAsioDeviceByClassId(
    juce::AudioIODeviceType* type, const AsioDriverRegistration& registration)
{
   #if JUCE_WINDOWS && JUCE_ASIO && JUCE_ASIO_USE_EXTERNAL_SDK
    auto* asioType = dynamic_cast<juce::ASIOAudioIODeviceType*>(type);
    const auto text = juce::String(registration.classId);
    CLSID id {};
    if (!asioType || CLSIDFromString(text.toWideCharPointer(), &id) != S_OK
        || asio_native::canonicalId(id) != text) return {};
    const auto slot = juce::ASIOAudioIODeviceType::findFreeSlot();
    if (slot < 0) return {};
    // The normal JUCE createDevice(name) resolves duplicate names via indexOf.
    // Pass the exact selected CLSID to its existing public constructor instead.
    // This INITIALISES the driver and belongs only to a controlled switch window.
    return std::make_unique<juce::ASIOAudioIODevice>(asioType, juce::String(registration.displayName), id, slot);
   #else
    juce::ignoreUnused(type, registration);
    return {};
   #endif
}
