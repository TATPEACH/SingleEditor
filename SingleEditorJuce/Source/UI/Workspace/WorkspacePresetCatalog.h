#pragma once

#include <optional>
#include <string>
#include <vector>

enum class WorkspaceContentCategory
{
    general,
    discreteMultichannel,
    ambisonics,
    videoAudio,
    bnk
};

enum class WorkspacePresetCatalogRestoreStatus
{
    applied,
    noState,
    corrupt,
    futureSchema
};

struct WorkspaceStoredPreset
{
    std::string id;
    std::string displayName;
    std::string workspaceState;
    std::string opaqueRecordJson = "{}";
};

class WorkspacePresetCatalog final
{
public:
    static WorkspaceContentCategory classify(bool isBnk,
                                             bool isVideoAudio,
                                             bool isAmbisonic,
                                             int channelCount) noexcept;
    static std::string categoryKey(WorkspaceContentCategory category);

    bool savePreset(std::string id, std::string displayName, std::string workspaceState);
    bool bindCategory(WorkspaceContentCategory category, const std::string& presetId);
    [[nodiscard]] std::optional<WorkspaceStoredPreset> preset(const std::string& id) const;
    [[nodiscard]] std::optional<WorkspaceStoredPreset> resolve(WorkspaceContentCategory category) const;
    [[nodiscard]] std::vector<WorkspaceStoredPreset> presets() const;

    [[nodiscard]] std::string serialise() const;
    WorkspacePresetCatalogRestoreStatus restore(const std::string& state);
    void reset();

private:
    struct Binding
    {
        std::string category;
        std::string presetId;
    };

    std::vector<WorkspaceStoredPreset> storedPresets;
    std::vector<Binding> bindings;
    std::string opaqueTopLevelJson = "{}";
};
