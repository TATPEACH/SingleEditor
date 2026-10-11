#include "ApplicationDiagnosticIdentity.h"
#include "DiagnosticTestActivity.h"
#include "DiagnosticBuildIdentity.h"
#if JUCE_WINDOWS
 #include <windows.h>
#endif
namespace singleeditor::diagnostics
{
// Hash only the running binary, on the explicitly requested worker. Closing
// the window cancels subsequent reads; no executable path enters the bundle.
class BinaryInput final : public juce::InputStream
{
public:
    BinaryInput(const juce::File& file, const std::atomic<bool>& stop) : input(file), cancelled(stop) {}
    juce::int64 getTotalLength() override { return input.getTotalLength(); }
    bool isExhausted() override { return cancelled.load() || input.isExhausted(); }
    juce::int64 getPosition() override { return input.getPosition(); }
    bool setPosition(juce::int64 p) override { return input.setPosition(p); }
    int read(void* data, int bytes) override { return cancelled.load() ? 0 : input.read(data, bytes); }
    juce::FileInputStream input;
    const std::atomic<bool>& cancelled;
};
BundleIdentity captureApplicationIdentity(const std::atomic<bool>& cancelled, const juce::String& distribution)
{
    SINGLE_EDITOR_DIAGNOSTIC_ACTIVITY(identity);
    BundleIdentity result;
    result.version = SINGLE_EDITOR_DIAGNOSTIC_VERSION;
    result.commit = SINGLE_EDITOR_DIAGNOSTIC_COMMIT;
#if JUCE_DEBUG
    result.configuration = "Debug";
#else
    result.configuration = "Release";
#endif
    result.distribution = distribution;
#if defined(_M_ARM64) || defined(__aarch64__)
    result.processArchitecture = "arm64";
#elif JUCE_64BIT
    result.processArchitecture = "x64";
#else
    result.processArchitecture = "x86";
#endif
#if JUCE_WINDOWS
    SYSTEM_INFO system{}; GetNativeSystemInfo(&system);
    result.osArchitecture = system.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64 ? "x64"
        : system.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64 ? "arm64"
        : system.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_INTEL ? "x86" : "unknown";
#else
    result.osArchitecture = "not-reported";
#endif
    result.osBuild = juce::SystemStats::getOperatingSystemName().replaceCharacter(' ', '-');
#if JUCE_WINDOWS
    const auto build = juce::WindowsRegistry::getValue("HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\CurrentBuildNumber",
        {}, juce::WindowsRegistry::WoW64_64bit);
    const auto revision = juce::WindowsRegistry::getValue("HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\UBR",
        {}, juce::WindowsRegistry::WoW64_64bit);
    result.osBuild += build.isNotEmpty() && build.containsOnly("0123456789") ? "-build-" + build : "-build-not-reported";
    if (revision.isNotEmpty() && revision.containsOnly("0123456789")) result.osBuild += "." + revision;
#endif
#if SINGLE_EDITOR_ENABLE_OPEN_TIMING
    result.timingInstrumentation = true;
#endif
    BinaryInput input(juce::File::getSpecialLocation(juce::File::currentExecutableFile), cancelled);
    const auto bytes = input.getTotalLength();
    if (input.input.openedOk() && bytes > 0 && bytes <= 512LL * 1024 * 1024)
    {
        const auto hash = juce::SHA256(input, bytes).toHexString();
        if (!cancelled.load() && input.getPosition() == bytes) result.binarySha256 = hash;
    }
    return result;
}
}
