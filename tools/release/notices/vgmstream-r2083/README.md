# vgmstream r2083 runtime notices and sources

This directory preserves the external dependency notices published with the fixed vgmstream r2083 source commit. The official Windows ZIP does not carry this notice directory. [Source and binary identities](NoticeOrigins.json) records the exact selected archive and its17members, nine dependency families, full notice hashes, source declarations and fixed build-document links.

| Dependency family | Notice files and known boundary |
|---|---|
| vgmstream CLI, Winamp and XMPlay plugins | [Project COPYING](../../VGM/vgmstream-win/COPYING) already accompanies the runtime. xmp-vgmstream.dll is the XMPlay plugin, not libxmp. External libraries keep their own terms. |
| FFmpeg | [LGPL2.1](ffmpeg.COPYING.LGPLv2.1) and [LGPL3](ffmpeg.COPYING.LGPLv3); the latter incorporates [GPL3](../GNU-GPL-3.0.txt). Actual module metadata reports n5.1.2 and LGPL2.1-or-later. Exact source/patch/build correspondence and corresponding-source delivery remain to complete. |
| Opus inside FFmpeg | [COPYING](opus-1.3.1.COPYING), including its patent notice links. The recipe specifies1.3.1 and a static library; the actual FFmpeg configuration enables libopus and links opus.lib. A separate Opus DLL is not required for this dependency to exist. |
| Combined Vorbis/Ogg | [Vorbis](libvorbis-1.3.7.COPYING) and [Ogg](libogg-1.3.5.COPYING); upstream describes Ogg, Vorbis and Vorbisfile in libvorbis.dll. |
| mpg123 | [Upstream retained COPYING](mpg123-1.31.1.COPYING). Its filename and the recipe say1.31.1, while BUILD lists1.32.0/1.31.1 and links an older binary. Actual DLL version remains unresolved; this notice filename is not used as proof of that version. |
| LibAtrac9 | [MIT notice](LibAtrac9.LICENSE); the source guide declares commit6a9e00f6c7abd74d037fd210b6670d3cdb313049. |
| CELT0.6.1/0.11.0 | [First COPYING](celt-0.6.1.COPYING), [second COPYING](celt-0.11.0.COPYING). Preserve these original three-condition BSD-style texts; the build guide's MIT shorthand does not replace their conditions. |
| Speex | [COPYING](speex-1.2.1.COPYING); declared source1.2.1. |
| G719 | The user selected retaining G719 support on2026-10-07; the OFF proposal is withdrawn and the current runtime remains. The fixed guide lists this implementation's license as unknown, so source and license work remain unfinished. Simply removing this DLL breaks helper/plugin loading. Feature retention is decided and is no longer a pending user choice. |

The source index now also records nine acquired archives for seven families: the complete fixed r2083 tree, FFmpeg n5.1.2, Opus, Vorbis/Ogg, both CELT versions, Speex and LibAtrac9. The r2083 tree includes the FFmpeg compatibility patch and CELT compatibility header referenced by its recipe. Their presence does not establish that the retained historical DLLs used those changes. mpg123's ambiguous actual version and G719's public disposition remain unresolved. Archive acquisition is complete for this declared subset; final source/patch/build correspondence and public source delivery are not.

These texts are copied verbatim; they do not modify upstream license choices or license Single Editor's own code. Version/commit entries labelled declared are upstream build-guide statements, not claims that local reproduction has already proved the exact corresponding source. The existing build route is used for remaining source delivery work; no independent replacement build system is introduced.

This is preparation for the retained internal runtime, not approval to publish it. Public release remains pending the stated source obligations, clarification of the retained G719 implementation's license and the product's other release requirements. G719 support is retained by explicit user decision. The current candidate and all runtime binaries are unchanged by these disclosure updates.
