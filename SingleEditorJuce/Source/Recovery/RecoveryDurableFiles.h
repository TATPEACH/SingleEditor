#pragma once
#include "RecoveryStore.h"

namespace singleeditor::recovery::detail
{
// Internal metadata I/O seam sharing the Store's tested framing, checksums,
// directory pins and flush-before-atomic-publish primitive. No recursive cleanup.
class DurableDirectory final
{
public:
    ~DurableDirectory();
    // Read-only absence succeeds with a null output; other faults remain typed.
    static Status open(const juce::File&, bool create, std::unique_ptr<DurableDirectory>&);
    const juce::File& path() const noexcept;
    Status readJson(const juce::String& child, size_t maximum, juce::var&) const;
    Status publishJson(const juce::String& child, size_t maximum, const juce::var&, bool replace) const;
    Status listNames(size_t maximumEntries, std::vector<juce::String>&) const;
private:
    DurableDirectory();
    struct Impl;
    std::unique_ptr<Impl> impl;
};

// Worker-only metadata transaction for an explicit physical budget root. Each
// transaction owns root exclusion through reservation/publication, never for the
// lifetime of an active restore attempt. Reader-only DurableDirectory stays inert.
class DurableMutation final
{
public:
    ~DurableMutation();
    static Status open(const juce::File& root, const Limits&, std::unique_ptr<DurableMutation>&);
    Status reserve(size_t bytes, size_t entries);
    Status publishJson(const DurableDirectory&, const juce::String& child, size_t maximum,
                       const juce::var&, bool replace);
private:
    DurableMutation();
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}
