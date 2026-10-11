# Compiled third-party dependencies

These notices accompany the selected Windows GUI and CLI dependencies and retain each upstream author's terms. Single Editor's own, licensable project code uses AGPL-3.0-only; see the root [LICENSE](../../../LICENSE) and [README](../../../README.md) for its scope. Third-party code, adaptations, fonts and assets retain their respective licenses and copyright notices. The complete GNU GPL version 3 and GNU AGPL version 3 texts remain included for dependency disclosures.

| Dependency | Included notice | Use |
|---|---|---|
| JUCE 8.0.12 | [JUCE license](JUCE-8.0.12-LICENSE.md), [AGPL v3](GNU-AGPL-3.0.txt) | GUI and CLI framework; the open-source license route applies to this preparation. |
| FLAC, Ogg and Vorbis in JUCE | [FLAC](JUCE-8.0.12-FLAC.txt), [Ogg/Vorbis](JUCE-8.0.12-Ogg-Vorbis.txt), [libvorbis](JUCE-8.0.12-libvorbis-COPYING.txt) | Built-in audio format readers. |
| zlib in JUCE | [License](JUCE-8.0.12-zlib-LICENSE.txt), [copyright notice](JUCE-8.0.12-zlib-copyright.txt) | Built-in core compression. |
| HarfBuzz and SheenBidi in JUCE | [HarfBuzz](JUCE-8.0.12-HarfBuzz-COPYING.txt), [SheenBidi](JUCE-8.0.12-SheenBidi-LICENSE.txt) | GUI text shaping and bidirectional text. |
| libpng and IJG JPEG in JUCE | [libpng](JUCE-8.0.12-libpng-LICENSE.txt), [IJG](JUCE-8.0.12-IJG-LEGAL-ISSUES.txt) | GUI image formats. This software is based in part on the work of the Independent JPEG Group. |
| Steinberg ASIO SDK 2.3.4 (2025-10-15) | [SDK terms](ASIO-2.3.4-LICENSE.txt), [GPL v3](GNU-GPL-3.0.txt) | GUI external SDK integration. The GPL alternative is the selected dependency route; the SDK notice expressly limits its scope to files referencing it. |
| libsamplerate 0.2.2 | [COPYING](libsamplerate-0.2.2.txt) | GUI and CLI sample-rate conversion; supplied by the existing candidate output. |
| Phosphor Icons core 2.1.1 | [MIT license](Phosphor-Icons-2.1.1-LICENSE.txt) | The 21 embedded Core action SVGs match the regular icons at commit 2b75f3ad12b420c9504ef05df8d2564a28f8500e. Local root size and fill attributes differ; geometry is unchanged. This does not cover separate app/brand artwork. |

[Notice origins](ThirdPartyNoticeOrigins.json) gives fixed upstream archive URLs, hashes, source paths and exact text ranges. Original notice text is copied without edits. The IJG legal section and zlib copyright section are exact excerpts from their source README files; complete source README files remain available at the recorded upstream locations. JUCE's original index also describes unselected framework components; it is preserved verbatim and is not a statement that those components ship here.

Single Editor builds a local JUCE WAV-reader overlay for fast ordinary WAV opening and lazy ASWG metadata parsing. Its source and build recipe are part of the project source distribution; the shared JUCE installation is not modified by this build step. These copied notices are unmodified. Providing notices alone does not fulfill corresponding-source distribution obligations: the final source version, modifications and build instructions must accompany the release through the selected source delivery route.

Existing notices for the Microsoft runtime and Ambisonics resource remain separately packaged. The optional vgmstream runtime retains its own COPYING and codec licenses. FFmpeg n5.1.2 is used under LGPL-2.1-or-later; [its source/build delivery instructions](vgmstream-r2083/FFMPEG_BUILDING.md) identify the matching FFmpeg and Opus archives, actual configuration and applicable modifications. Supply the complete dependency-source and SE-source attachments alongside the binary on the same Release. This document records source delivery and notices; product acceptance and public upload approval remain separate.
