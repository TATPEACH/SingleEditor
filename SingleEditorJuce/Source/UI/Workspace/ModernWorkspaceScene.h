#pragma once

namespace singleeditor
{
// One persisted full Mag layout. Simple is a transient core-only projection.
// Keep the legacy preset ID compatible; construct/restore only after CoreReady.
inline constexpr auto modernWorkspaceDefaultLayout = R"json({
  "schema":"single-editor.workspace-preset", "version":2, "presetId":"modern-glass-general-dsp",
  "core":{"instanceId":"core-waveform","placement":{"column":1,"row":0,"span":{"columns":4,"rows":2}}},
  "gridTracks":{"columns":[246,1028,363],"rows":[250,150,100,250]},
  "modules":[
    {"instanceId":"playlist","moduleId":"playlist","enabled":true,"placed":true,"order":0,"weight":1000,"viewState":{"version":1,"payload":{}},"placement":{"column":0,"row":0,"span":{"columns":1,"rows":1}}},
    {"instanceId":"metadata","moduleId":"metadata","enabled":true,"placed":true,"order":1,"weight":1000,"viewState":{"version":1,"payload":{}},"placement":{"column":0,"row":1,"span":{"columns":1,"rows":2}}},
    {"instanceId":"markers","moduleId":"markers","enabled":true,"placed":true,"order":5,"weight":1000,"viewState":{"version":1,"payload":{}},"placement":{"column":0,"row":3,"span":{"columns":1,"rows":1}}},
    {"instanceId":"parametric-eq","moduleId":"parametric-eq","enabled":true,"placed":true,"order":2,"weight":1000,"viewState":{"version":1,"payload":{}},"placement":{"column":1,"row":2,"span":{"columns":4,"rows":2}}},
    {"instanceId":"output-meter","moduleId":"output-meter","enabled":true,"placed":true,"order":3,"weight":1000,"viewState":{"version":1,"payload":{}},"placement":{"column":5,"row":0,"span":{"columns":1,"rows":2}}},
    {"instanceId":"true-peak-limiter","moduleId":"true-peak-limiter","enabled":true,"placed":true,"order":4,"weight":1000,"viewState":{"version":1,"payload":{}},"placement":{"column":5,"row":2,"span":{"columns":1,"rows":2}}}
  ], "stacks":[], "floating":[]
})json";
}
