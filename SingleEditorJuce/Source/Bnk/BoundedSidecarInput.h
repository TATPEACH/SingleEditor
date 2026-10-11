#pragma once
#include <juce_core/juce_core.h>
#include "Audio/SourceFileLease.h"
#include <chrono>
#include <deque>
#include <functional>
#include <string>
#include <vector>

namespace singleeditor
{
// Explicit worker scope. No scan, timer or file handle exists before optional
// work starts. All nested sidecar helpers share this scope's bounded resources.
class BoundedSidecarInput final
{
public:
    enum class Failure { none, cancelled, deadline, invalidData, hardLimit, ioFailure, memoryQuota };
    enum class Profile { conservative, expanded };
    struct Catalog final
    {
        struct Query final
        {
            juce::File root;
            juce::String wildcard;
            bool recursive = false;
            std::vector<juce::File> matches;
        };
        juce::File bankFile, projectRoot;
        Failure failure = Failure::none;
        std::vector<Query> queries;
        std::vector<std::shared_ptr<media::SourceFileLease>> inputs;
    };
    // Metadata-only discovery, before parser/fuse admission. Never reads XML,
    // TXT or WWU bodies. The resulting ordinary leases pin the actual inputs.
    static std::shared_ptr<const Catalog> freeze(const juce::File& bank, const juce::File& projectRoot = {},
        std::function<bool()> checkpoint = {});
    explicit BoundedSidecarInput(juce::StringArray* messages = nullptr, std::function<bool()> checkpoint = {},
                                 Profile profile = Profile::conservative, std::shared_ptr<const Catalog> catalog = {})
        : previous(active), diagnostics(messages), checkpoint(std::move(checkpoint)),
          wallTime(profile == Profile::expanded ? std::chrono::seconds(20) : std::chrono::seconds(5)), frozen(std::move(catalog))
    { active = this; if (frozen) failure = frozen->failure; }
    ~BoundedSidecarInput()
    {
        active = previous;
        if (wasLimited() && diagnostics != nullptr)
        { try { diagnostics->add(describe(failure)); } catch (...) {} }
    }
    BoundedSidecarInput(const BoundedSidecarInput&) = delete;
    BoundedSidecarInput& operator=(const BoundedSidecarInput&) = delete;
    bool wasLimited() const noexcept { return failure != Failure::none; }
    Failure failureReason() const noexcept { return failure; }
    static juce::String describe(Failure reason)
    {
        switch (reason)
        {
            case Failure::none: return {};
            case Failure::cancelled: return "Optional sidecar metadata was cancelled; proven core bank data was kept.";
            case Failure::deadline: return "Optional sidecar metadata exceeded its time budget; proven core bank data was kept.";
            case Failure::hardLimit: return "Optional sidecar metadata exceeded a fixed structural/read limit; proven core bank data was kept.";
            case Failure::invalidData: return "Optional sidecar metadata is malformed, ambiguous or leaves its allowed paths; proven core bank data was kept.";
            case Failure::memoryQuota: return "Optional sidecar metadata could not fit in memory; proven core bank data was kept.";
            case Failure::ioFailure: return "Optional sidecar input could not be read with its original identity; proven core bank data was kept.";
        }
        return {};
    }
    static bool continueWork() { return active != nullptr && active->check(); }
    // Generated hierarchy paths and normalized lookup keys can amplify a small
    // XML input. Charge conservative character capacity BEFORE construction,
    // across every file/pass of this optional transaction. This bounds derived
    // text work, not all allocator/DOM overhead or the process's resident memory.
    static constexpr size_t derivedTextLimit = 32u * 1024u * 1024u;
    static bool admitDerivedText(size_t characters, size_t representations = 1)
    {
        if (active == nullptr || !active->check()) return false;
        constexpr size_t maximumBytesPerCharacter = 4;
        if (characters >= derivedTextLimit / maximumBytesPerCharacter || representations == 0)
        { reject(Failure::hardLimit); return false; }
        const auto capacity = (characters + 1) * maximumBytesPerCharacter;
        if (representations > (derivedTextLimit - active->derivedTextBytes) / capacity)
        { reject(Failure::hardLimit); return false; }
        active->derivedTextBytes += capacity * representations;
        return true;
    }
    static std::shared_ptr<const Catalog> frozenInputs() { return active != nullptr ? active->frozen : nullptr; }
    static void reject(Failure reason)
    { if (active != nullptr && active->failure == Failure::none) active->failure = reason; }
    std::vector<media::SourceFileWitness::Ptr> inputWitnesses() const
    {
        std::vector<media::SourceFileWitness::Ptr> result; result.reserve(inputs.size());
        for (const auto& input : inputs) result.push_back(input->witness());
        return result;
    }
    bool finish()
    {
        if (!check()) return false;
        for (const auto& input : inputs)
        {
            if (!check()) return false;
            if (input->recheck().failed()) { failure = Failure::ioFailure; return false; }
        }
        return check();
    }

    static juce::File withinRoot(const juce::File& root, const juce::String& path)
    {
        if (root == juce::File() || path.isEmpty() || (active && !active->check())) return {};
        const auto candidate = root.getChildFile(path.replaceCharacter('/', '\\'));
        if (candidate != root && !candidate.isAChildOf(root)) { reject(Failure::invalidData); return {}; }
        auto cursor = candidate;
        std::vector<juce::File> ancestors;
        for (int depth = 0; depth < 128; ++depth)
        {
            if (active && !active->check()) return {};
            ancestors.push_back(cursor);
            if (cursor == root)
            {
                for (auto ancestor = ancestors.rbegin(); ancestor != ancestors.rend(); ++ancestor)
                    if (ancestor->isSymbolicLink()) { reject(Failure::invalidData); return {}; }
                return candidate;
            }
            const auto parent = cursor.getParentDirectory();
            if (parent == cursor) break;
            cursor = parent;
        }
        reject(Failure::hardLimit); return {};
    }

    static juce::String text(const juce::File& file)
    {
        if (active == nullptr || !active->check()) return {};
        if (!file.existsAsFile()) return {};
        constexpr int64_t fileLimit = 4 * 1024 * 1024;
        if (++active->fileReads > 128) return rejectText(Failure::hardLimit);
        auto inputLease = active->pin(file);
        if (!inputLease) return {};
        const auto size = inputLease->witness()->sizeBytes;
        if (size < 0 || size > fileLimit || size > 32 * 1024 * 1024 - active->bytes)
            return rejectText(Failure::hardLimit);
        active->bytes += size;
        // The ordinary lease pins this path and its ancestors for the entire
        // scope. RetainedInputStream is reserved for an actual replaced Save
        // input and must not be manufactured for a normal sidecar read.
        auto input = file.createInputStream();
        if (!input || input->getTotalLength() != size) return rejectText(Failure::ioFailure);
        juce::MemoryBlock data(static_cast<size_t>(size));
        int64_t offset = 0;
        while (offset < size)
        {
            if (!active->check()) return {};
            const auto count = static_cast<int>(juce::jmin<int64_t>(65536, size - offset));
            if (input->read(static_cast<char*>(data.getData()) + offset, count) != count)
                return rejectText(Failure::ioFailure);
            offset += count;
        }
        if (!active->check()) return {};
        if (inputLease->recheck().failed()) return rejectText(Failure::ioFailure);
        auto result = juce::String::createStringFromData(data.getData(), static_cast<int>(data.getSize()));
        return active->check() ? result : juce::String{};
    }
    static std::unique_ptr<juce::XmlElement> xml(const juce::File& file)
    {
        const auto source = text(file);
        if (source.isEmpty()) return {};
        if (!boundedXml(source)) { rejectText(Failure::invalidData); return {}; }
        if (!continueWork()) return {};
        juce::XmlDocument document(source);
        auto result = document.getDocumentElement();
        if (result == nullptr) rejectText(Failure::invalidData);
        if (!continueWork()) return {};
        return result;
    }
    static std::vector<juce::File> files(const juce::File& root, const juce::String& wildcard, bool recursive = true)
    {
        std::vector<juce::File> result;
        if (active != nullptr && active->frozen)
        {
            if (!active->check()) return result;
            for (const auto& query : active->frozen->queries)
                if (query.root == root && query.wildcard == wildcard && query.recursive == recursive) return query.matches;
            // A parser cannot expand the admitted discovery scope on a retry.
            reject(Failure::ioFailure); return result;
        }
        if (active == nullptr || !active->check() || !root.isDirectory()) return result;
        if (root.isSymbolicLink()) { rejectText(Failure::invalidData); return result; }
        std::deque<std::pair<juce::File, int>> pending { { root, 0 } };
        while (!pending.empty() && active->check())
        {
            const auto [directory, depth] = pending.front(); pending.pop_front();
            if (++active->directories > 256) { rejectText(Failure::hardLimit); break; }
            for (const auto& entry : juce::RangedDirectoryIterator(directory, false, "*",
                juce::File::findFilesAndDirectories, juce::File::FollowSymlinks::no))
            {
                if (!active->check()) break;
                if (++active->entries > 8192) { rejectText(Failure::hardLimit); break; }
                const auto file = entry.getFile();
                // A nonrecursive probe admits only matching paths. Unrelated
                // OS compatibility junctions are neither inputs nor traversal.
                if (!recursive && !file.getFileName().matchesWildcard(wildcard, true)) continue;
                if (file.isSymbolicLink()) { rejectText(Failure::invalidData); break; }
                if (file.isDirectory() && recursive)
                {
                    if (depth >= 8 || pending.size() >= 256) { rejectText(Failure::hardLimit); break; }
                    pending.emplace_back(file, depth + 1);
                }
                else if (file.existsAsFile() && file.getFileName().matchesWildcard(wildcard, true))
                {
                    if (++active->matchedFiles > 128) { rejectText(Failure::hardLimit); break; }
                    result.push_back(file);
                }
            }
        }
        if (active->wasLimited()) result.clear();
        return result;
    }
private:
    std::shared_ptr<media::SourceFileLease> pin(const juce::File& file)
    {
        for (const auto& input : inputs) if (input->witness()->file == file) return input;
        if (inputs.size() >= 128) { reject(Failure::hardLimit); return {}; }
        if (frozen)
        {
            for (const auto& input : frozen->inputs)
                if (input->witness()->file == file)
                {
                    if (input->recheck().failed()) { reject(Failure::ioFailure); return {}; }
                    inputs.push_back(input); return input;
                }
            reject(Failure::ioFailure); return {};
        }
        std::unique_ptr<media::SourceFileLease> lease;
        if (media::SourceFileLease::open(file, lease).failed() || !lease)
        { reject(Failure::ioFailure); return {}; }
        inputs.emplace_back(std::move(lease));
        return inputs.back();
    }
    bool check()
    {
        if (wasLimited()) return false;
        if (checkpoint && !checkpoint()) { failure = Failure::cancelled; return false; }
        if (std::chrono::steady_clock::now() - started > wallTime) { failure = Failure::deadline; return false; }
        if (++workSteps > 1000000) { failure = Failure::hardLimit; return false; }
        return true;
    }
    static juce::String rejectText(Failure reason)
    { reject(reason); return {}; }
    static bool boundedXml(const juce::String& source)
    {
        // Reject DTD/entities before entering JUCE's DOM. These hard ceilings
        // are identical for both profiles; expansion changes only wall time.
        const auto s = source.toStdString();
        size_t position = 0, nodes = 0, attributes = 0;
        int depth = 0;
        while ((position = s.find('<', position)) != std::string::npos)
        {
            if (!continueWork()) return false;
            size_t end = std::string::npos;
            if (s.compare(position, 4, "<!--") == 0)
            { end = s.find("-->", position + 4); if (end == std::string::npos) return false; position = end + 3; continue; }
            if (s.compare(position, 9, "<![CDATA[") == 0)
            { end = s.find("]]>", position + 9); if (end == std::string::npos) return false; position = end + 3; continue; }
            if (s.compare(position, 2, "<?") == 0)
            { end = s.find("?>", position + 2); if (end == std::string::npos) return false; position = end + 2; continue; }
            if (s.compare(position, 2, "<!") == 0) return false;
            char quote = 0;
            size_t tagAttributes = 0;
            for (size_t i = position + 1; i < s.size() && i - position <= 65536; ++i)
            {
                if (((i - position) & 4095) == 0 && !continueWork()) return false;
                const auto c = s[i];
                if (quote != 0) { if (c == quote) quote = 0; continue; }
                if (c == '\'' || c == '"') { quote = c; continue; }
                if (c == '=' && (++tagAttributes > 128 || ++attributes > 65536)) return false;
                if (c == '>') { end = i; break; }
            }
            if (end == std::string::npos) return false;
            if (s[position + 1] == '/') { if (--depth < 0) return false; }
            else
            {
                if (++nodes > 32768) return false;
                if (s[end - 1] != '/' && ++depth > 64) return false;
            }
            position = end + 1;
        }
        return depth == 0;
    }
    inline static thread_local BoundedSidecarInput* active = nullptr;
    BoundedSidecarInput* previous;
    juce::StringArray* diagnostics;
    std::function<bool()> checkpoint;
    std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
    const std::chrono::seconds wallTime;
    const std::shared_ptr<const Catalog> frozen;
    std::vector<std::shared_ptr<media::SourceFileLease>> inputs;
    int fileReads = 0, directories = 0, entries = 0, matchedFiles = 0, workSteps = 0;
    int64_t bytes = 0;
    size_t derivedTextBytes = 0;
    Failure failure = Failure::none;
};
}
