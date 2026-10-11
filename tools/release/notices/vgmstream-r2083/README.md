# vgmstream r2083 runtime notices and sources

The selected SE runtime uses the fixed vgmstream source commit `57df2e179d929532094f4e4dd42ce5395514622b` (r2083). Its CLI was rebuilt with upstream `USE_G719=OFF`; other codec options remain enabled. mpg123 uses the official 1.31.0 x86 binary linked by that revision's build instructions, with its source archive retained. Other codec DLLs are unchanged members of the official r2083 Windows archive. [NoticeOrigins.json](NoticeOrigins.json) distinguishes the selected runtime from that historical archive and records their file hashes. See [the build recipe](../../../../BUILDING.md#optional-vgmstream-runtime).

SE uses the CLI only. G.719, the Winamp plugin and the XMPlay plugin are excluded. BNSF audio tagged IS22 (interleaved Siren 22/G.719) cannot be decoded by this runtime; G.722.1/Siren 14 remains enabled. Wwise BNK/WEM support is retained. Container or filename alone does not identify a codec.

| Dependency family | Notice files and known boundary |
|---|---|
| vgmstream CLI | Project COPYING accompanies the runtime. External libraries keep their own terms. |
| FFmpeg | [LGPL2.1](ffmpeg.COPYING.LGPLv2.1), [LGPL3](ffmpeg.COPYING.LGPLv3) and [GPL3](../GNU-GPL-3.0.txt). Module metadata reports n5.1.2 and LGPL2.1-or-later. Matching source, actual configuration and applicable modification/build steps are provided; see [FFmpeg source delivery](FFMPEG_BUILDING.md). |
| Opus inside FFmpeg | [COPYING](opus-1.3.1.COPYING), including patent notice links. Actual avcodec includes the libopus1.3.1 version string; matching original source and static-build instructions are included. |
| Combined Vorbis/Ogg | [Vorbis](libvorbis-1.3.7.COPYING) and [Ogg](libogg-1.3.5.COPYING). Both notices are preserved. |
| mpg123 | [Official 1.31.0 source COPYING](mpg123-1.31.0.COPYING). The selected official binary and source archives are recorded in the index; the unidentified historical DLL is not selected. |
| LibAtrac9 | [MIT notice](LibAtrac9.LICENSE); declared source commit6a9e00f6c7abd74d037fd210b6670d3cdb313049. |
| CELT0.6.1/0.11.0 | [First COPYING](celt-0.6.1.COPYING), [second COPYING](celt-0.11.0.COPYING), preserving their original three-condition BSD-style terms. |
| Speex | [COPYING](speex-1.2.1.COPYING); declared source1.2.1. |

Ten source archives cover eight families, including the fixed vgmstream tree, its referenced build/compatibility files and mpg123 1.31.0. The complete dependency-source Release attachment also includes the three core SE build dependencies. It retains original implementation, licenses and build files together with [FFmpeg source/build instructions](FFMPEG_BUILDING.md), the observed configuration and applicable modifications. Independent byte-identical reproduction of historical DLLs is not claimed or required for this delivery.

Co-distribute SingleEditor-v0.1.0-dependency-sources.zip and SingleEditor-v0.1.0-source.zip on the same Release as portable/setup; keep the license texts and this notice set in the binary package. The source download location is [the v0.1.0 Release](https://github.com/TATPEACH/SingleEditor/releases/tag/v0.1.0). The FFmpeg DLLs remain replaceable with compatible modified libraries. These notices do not alter upstream terms or encode overall product acceptance. G.719 is no longer bundled and is not a pending feature choice.
