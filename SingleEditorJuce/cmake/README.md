# JUCE graphics startup overlay

## Official JUCE and the existing large-WAV path

The JUCE8.0.12 download is now pinned by archive SHA. `SingleEditorJuceWavMetadata.cmake` accepts both the official WAV source and the exact historical local FAST_WAV_OPEN source. From the official input it first reconstructs that historical implementation in the build directory and checks its complete normalized SHA, then applies the existing lazy ASWG metadata change. Both routes generate the same WAV source as the retained F92 build; shared JUCE remains untouched.

`SINGLE_EDITOR_LAZY_WAV_METADATA=OFF` only disables the ASWG change for these known inputs: official input still receives the existing fast path, and an already patched local input stays in place. The existing explicit upstream fallback for an unreviewed source remains separate from the default guarded route. Configure-only coverage lives in `tools/tests/Test-JuceWavMetadataOverlay.ps1`; full product/runtime evidence is not implied by those checks.

Configured JUCE and ASIO provider roots are recorded in `SINGLE_EDITOR_RESOLVED_JUCE_ROOT` and `SINGLE_EDITOR_RESOLVED_ASIO_SDK_ROOT`. Build capture uses those actual roots, including FetchContent providers; old explicit roots remain supported, while an empty unresolved root must fail rather than inventory the working directory.

## Exact active WASAPI diagnostic binding

`SingleEditorJuceAudioDiagnostics.cmake` selects a build-local wrapper that includes the unchanged audio-device amalgamation, then a project function which copies the concrete WASAPI owner's public output endpoint ID. The reviewed JUCE 8.0.12 amalgamation and WASAPI file are checked after LF normalization. Nothing in the shared dependency is edited; no private access or ABI change is used. `SINGLE_EDITOR_WASAPI_DIAGNOSTIC_BINDING=OFF` explicitly retains upstream source selection and reports native identity unsupported. Non-Windows builds use the inline unsupported path. The wrapper has no collector, static constructor or startup query; only an explicit diagnostic owner capture calls it. Validate configure behavior with `tools/tests/Test-JuceAudioDiagnosticsBinding.ps1`; real endpoint/adapter reads are separately tested in the native product workflow.

## Graphics

Normal Windows CMake builds enable `SINGLE_EDITOR_SKIP_EXTRA_OUTPUTLESS_ADAPTERS`. The overlay keeps JUCE's first successfully created default device and all adapters with outputs, while skipping device creation for additional adapters explicitly reporting no outputs. Unexpected enumeration results and default-device creation failures retain the upstream path. Existing display-factory re-enumeration is unchanged.

`SingleEditorJuceGraphics.cmake` generates two files in the build directory, then replaces only the graphics amalgamation's interface source. Original JUCE headers and all other translation units remain in place. This works with the configured shared JUCE path and the official8.0.12 archive fallback; it never patches either source tree. Hash guards check the reviewed upstream files with LF/CRLF normalization. Other content changes stop configuration for review instead of silently applying an old patch. Generated content is rewritten only when different.

To compare or recover upstream behavior, configure the same build with:

```text
-DSINGLE_EDITOR_SKIP_EXTRA_OUTPUTLESS_ADAPTERS=OFF
```

This selects the original translation unit; unused generated files are harmless. Non-Windows builds bypass the overlay. Validate integration with `tools/tests/Test-JuceGraphicsOverlay.ps1 -JuceRoot <JUCE source root>`; it exercises source preservation, LF/CRLF equivalence, unchanged configure, source selection, hash rejection and explicit upstream fallback. Application rendering/performance evidence lives in Round8 `qa/STARTUP_ADAPTER_INITIALIZATION_QA.md`. Do not infer hardware failure, remote-desktop or live-hotplug acceptance from the configure checks.
