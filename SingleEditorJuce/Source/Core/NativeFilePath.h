#pragma once
#include <juce_core/juce_core.h>

namespace singleeditor::files
{
// I/O spelling only: keep logical paths in identities, policies and receipts.
// Explicit extended paths do not depend on machine registry/manifest opt-in.
inline juce::String nativePath(const juce::File& file)
{
    auto path = file.getFullPathName();
#if JUCE_WINDOWS
    // JUCE represents a drive root as C:. Extended syntax needs C:\, since
    // C: alone is a drive-relative name (or an invalid extended root).
    if (path.endsWithChar(':') && (path.length() == 2 || (path.length() == 6 && path.startsWith("\\\\?\\"))))
        path += "\\";
    if (path.isNotEmpty() && !path.startsWith("\\\\?\\") && !path.startsWith("\\\\.\\"))
    {
        path = path.replaceCharacter('/', '\\');
        return path.startsWith("\\\\") ? "\\\\?\\UNC\\" + path.substring(2) : "\\\\?\\" + path;
    }
#endif
    return path;
}

inline juce::File nativeFile(const juce::File& file) { return juce::File(nativePath(file)); }
}
