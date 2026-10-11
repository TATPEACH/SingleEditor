# Single Editor brand mark

Selected by the maintainer on 2026-10-11: study A, a continuous waveform.
The formal mark uses a white rounded stroke on coral `#F25355`, with transparent
rounded corners. It is independent project artwork authored as SVG, not a copy
of the MiniMax reference. The reference informed simplicity only.

`BrandMark.svg` is the editable source. `AppIcon.png` (1024 px), `AppIcon.ico`
and `../../icons/icon.ico` are generated exports; the ICO contains 16, 24, 32,
48, 64, 128 and 256 px frames. Generated `Source/UI/LookAndFeel/BrandMark.h`
draws the same path for the empty editor and immediate tray fallback,
without file reads or image decoding. Loading/error states keep their status glyphs.

Regenerate from the repository root with
`node tools/assets/GenerateBrandAssets.mjs <node_modules containing sharp>`.
Sharp 0.35.5 is asset-authoring tooling, not a product build dependency. Exports are
checked in. The artwork uses the project's AGPL-3.0-only license; this does not
assert a registered trademark. Tool versions used are recorded with validation.
