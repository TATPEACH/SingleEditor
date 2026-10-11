#include "Audio/DeviceRoutePlan.h"
#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>

namespace
{
using Plan = singleeditor::DeviceRoutePlan;
using Role = singleeditor::SpeakerRole;
using Code = Plan::IssueCode;
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
bool has(const Plan::Review& review, Code code)
{ return std::any_of(review.issues().begin(), review.issues().end(), [code](const auto& issue) { return issue.code == code; }); }

struct Audio
{
    std::array<float, 4> left {0.25f, 0.5f, 0.75f, 1.0f}, right {0.5f, 0.25f, 0.75f, 0.25f}, lfe {0.125f, 0.25f, 0.5f, 0.75f};
    std::array<std::array<float, 4>, 3> output {};
    std::array<const float*, 3> inputs {left.data(), right.data(), lfe.data()};
    std::array<float*, 3> outputs {output[0].data(), output[1].data(), output[2].data()};
    void render(const Plan& plan) { require(plan.render(inputs.data(), 3, outputs.data(), 3, 4), "render exact canonical/physical shape"); }
};

void run()
{
    const Plan::Bus bus {"explicit-speaker-profile", 7, {{"speaker-left", Role::L}, {"speaker-right", Role::R}, {"sub", Role::LFE}}};
    const singleeditor::DeviceOutput::Snapshot device {"ASIO", "asio:exact-selected-driver", "Names are not identity", 48000, 128, 32,
        {1, 3, 6}, {"Port 0", "Port 1", "Port 2", "Port 3", "Port 4", "Port 5", "Port 6"}, 0, 64};
    auto untrusted = Plan::propose(bus, device);
    require(std::all_of(untrusted.outputsBySpeaker.begin(), untrusted.outputsBySpeaker.end(), [](const auto& row) { return row.empty(); }),
        "channel counts, indices and port labels do not invent speaker roles");
    auto unmapped = Plan::validate(untrusted, device, bus);
    require(unmapped.isValid() && has(unmapped, Code::requiredUnmapped) && !unmapped.approved() && unmapped.approved(true),
        "required unmapped speakers require explicit review confirmation, never automatic fallback");
    Audio silence; silence.render(*unmapped.approved(true));
    require(silence.output == std::array<std::array<float, 4>, 3>{}, "confirmed unmapped output is actually silent");

    Plan::TrustedRoles trusted {std::vector<Role>(7, Role::unknown)};
    (*trusted)[1] = Role::R; (*trusted)[3] = Role::L; (*trusted)[6] = Role::LFE;
    auto draft = Plan::propose(bus, device, trusted);
    require(draft.outputsBySpeaker == std::vector<std::vector<int>>{{3}, {1}, {6}},
        "trusted semantic proposal is role-first despite noncontiguous reversed physical indices");
    auto reviewed = Plan::validate(draft, device, bus, trusted);
    auto plan = reviewed.approved();
    require(plan && !reviewed.needsConfirmation() && plan->matches(device, bus, trusted), "one-to-one trusted binding validates without warnings");
    Audio routed; routed.render(*plan);
    require(routed.output[0] == routed.right && routed.output[1] == routed.left && routed.output[2] == routed.lfe,
        "actual unity matrix maps the canonical bus to selected physical outputs, keeping LFE independent");
    require(plan->outputSlotForPhysical(6) == 2 && plan->outputSlotForPhysical(2) == -1,
        "post-route Ping can resolve an exact physical output without guessing a compact channel index");
    auto sameGeometryNewRate = device; sameGeometryNewRate.sampleRate = 96000;
    require(plan->matches(sameGeometryNewRate, bus, trusted), "device clock policy is separate from the physical route binding");
    auto changedRoles = trusted; (*changedRoles)[3] = Role::C;
    require(!plan->matches(device, bus, changedRoles), "changed authoritative role metadata invalidates the binding");

    draft.outputsBySpeaker[0] = {1}; draft.outputsBySpeaker[1] = {3};
    auto crossed = Plan::validate(draft, device, bus, trusted);
    require(has(crossed, Code::crossedRole) && !crossed.approved() && crossed.approved(true),
        "a crossed explicit route requires confirmation and cannot come from automatic proposal");
    require(plan->connected(0, 1) && !plan->connected(0, 0), "editing a draft cannot change an already reviewed immutable plan");

    auto explicitDraft = Plan::propose(bus, device);
    explicitDraft.outputsBySpeaker = {{1, 3}, {}, {6}};
    auto copyReview = Plan::validate(explicitDraft, device, bus);
    require(!copyReview.approved() && has(copyReview, Code::requiredUnmapped), "copying L does not silently remap required R");
    auto copyPlan = copyReview.approved(true); require(copyPlan != nullptr, "confirm the exact reviewed omission");
    Audio copied; copied.render(*copyPlan);
    require(copied.output[0] == copied.left && copied.output[1] == copied.left && copied.output[2] == copied.lfe,
        "one-to-many produces full unity copies with no attenuation or directional content in LFE");
    explicitDraft.outputsBySpeaker = {{1}, {1}, {6}};
    auto mergeReview = Plan::validate(explicitDraft, device, bus);
    require(has(mergeReview, Code::convergentOutput) && has(mergeReview, Code::possibleClipping) && !mergeReview.approved(),
        "many-to-one explicitly reports convergence and possible clipping before activation");
    Audio merged; merged.render(*mergeReview.approved(true));
    for (std::size_t i = 0; i < merged.left.size(); ++i)
        require(merged.output[0][i] == merged.left[i] + merged.right[i] && merged.output[1][i] == 0 && merged.output[2][i] == merged.lfe[i],
            "confirmed merge adds unity signals, clears unconnected outputs and does not hide clipping with normalization");
    require(merged.output[0][2] == 1.5f, "route is not an undocumented limiter");

    explicitDraft.outputsBySpeaker = {{1, 1}, {3}, {6}};
    auto duplicate = Plan::validate(explicitDraft, device, bus);
    require(has(duplicate, Code::duplicateConnection) && !duplicate.approved(), "duplicate imports require review");
    Audio deduplicated; deduplicated.render(*duplicate.approved(true));
    require(deduplicated.output[0] == deduplicated.left, "binary connection never doubles gain for the same repeated cell");

    for (int change = 0; change < 4; ++change)
    {
        auto altered = device;
        if (change == 0) altered.backend = "Windows Audio";
        if (change == 1) altered.stableId = "asio:other-driver";
        if (change == 2) std::swap(altered.outputNames[1], altered.outputNames[3]);
        if (change == 3) altered.physicalOutputs = {0, 3, 6};
        require(!plan->matches(altered, bus, trusted), "backend, identity, topology or active-channel changes invalidate an active plan");
    }
    for (const auto revision : {6, 8})
    {
        auto altered = bus; altered.revision = revision;
        require(!plan->matches(device, altered, trusted)
            && !Plan::validate(draft, device, altered, trusted).approved(true),
            "geometry revision equality is required regardless of numeric direction");
    }
    auto reordered = bus; std::swap(reordered.speakers[0], reordered.speakers[1]);
    require(!plan->matches(device, reordered, trusted), "canonical stable-ID order is part of the route binding");
    explicitDraft.outputsBySpeaker = {{2}, {3}, {6}};
    require(!Plan::validate(explicitDraft, device, bus).approved(true), "an inactive physical channel cannot be authorized by warning confirmation");
    auto malformed = device; malformed.physicalOutputs = {1, 1, 6};
    require(!Plan::validate(explicitDraft, malformed, bus).approved(true), "ambiguous active slots are rejected");
    auto duplicateBus = bus; duplicateBus.speakers[1].stableId = duplicateBus.speakers[0].stableId;
    require(!Plan::validate(Plan::propose(duplicateBus, device), device, duplicateBus).approved(true), "canonical speaker IDs must be unique");
    auto ambiguousRoles = trusted; (*ambiguousRoles)[3] = Role::R;
    auto ambiguous = Plan::propose(bus, device, ambiguousRoles);
    require(ambiguous.outputsBySpeaker[0].empty() && ambiguous.outputsBySpeaker[1].empty(),
        "ambiguous role metadata does not automatically fan out or invent missing roles");
    for (auto& channel : routed.output) channel.fill(9.0f);
    require(!plan->render(routed.inputs.data(), 2, routed.outputs.data(), 3, 4)
        && routed.output == std::array<std::array<float, 4>, 3>{}, "wrong callback shape produces silence instead of a partial or guessed route");

    for (const int count : {8, 12, 16})
    {
        Plan::Bus multiBus {"explicit-multichannel-profile", 1, {}};
        auto multiDevice = device; multiDevice.outputNames.resize(512); multiDevice.physicalOutputs.clear();
        for (int index = 0; index < 512; ++index) multiDevice.outputNames[static_cast<std::size_t>(index)] = "Port " + std::to_string(index);
        for (int index = 0; index < count; ++index)
        {
            multiBus.speakers.push_back({"speaker-" + std::to_string(index), Role::unknown});
            multiDevice.physicalOutputs.push_back(index * 31 + 1);
        }
        auto multiDraft = Plan::propose(multiBus, multiDevice);
        for (int index = 0; index < count; ++index)
            multiDraft.outputsBySpeaker[static_cast<std::size_t>(index)] = {multiDevice.physicalOutputs[static_cast<std::size_t>((index + 1) % count)]};
        const auto multiPlan = Plan::validate(multiDraft, multiDevice, multiBus).approved();
        require(multiPlan != nullptr, "explicit stable speaker IDs support custom 8/12/16-channel buses without guessing roles");
        std::array<std::array<float, 4>, 16> input {}, output {};
        std::array<const float*, 16> inputPointers {}; std::array<float*, 16> outputPointers {};
        for (int index = 0; index < count; ++index)
        {
            inputPointers[static_cast<std::size_t>(index)] = input[static_cast<std::size_t>(index)].data();
            outputPointers[static_cast<std::size_t>(index)] = output[static_cast<std::size_t>(index)].data();
            for (int sample = 0; sample < 4; ++sample)
                input[static_cast<std::size_t>(index)][static_cast<std::size_t>(sample)] = static_cast<float>(index * 4 + sample);
        }
        require(multiPlan->render(inputPointers.data(), count, outputPointers.data(), count, 4), "render full multichannel block on noncontiguous high physical indices");
        for (int index = 0; index < count; ++index)
            require(output[static_cast<std::size_t>((index + 1) % count)] == input[static_cast<std::size_t>(index)],
                "all samples preserve their canonical channel and frame through the explicit physical permutation");
    }
}
}

int main()
{
    try { run(); std::cout << "DeviceRoutePlanTests passed: role-first proposals, immutable review, unity copy/sum, LFE and exact device/geometry binding\n"; return 0; }
    catch (const std::exception& e) { std::cerr << "DeviceRoutePlanTests FAILED: " << e.what() << '\n'; return 1; }
}
