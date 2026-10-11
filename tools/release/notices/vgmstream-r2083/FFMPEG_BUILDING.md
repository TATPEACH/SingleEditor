# FFmpeg 对应源码、修改与构建说明

Single Editor 通过独立 vgmstream helper 使用 FFmpeg。本包的四个 x86 DLL 使用 **FFmpeg n5.1.2，LGPL-2.1-or-later**，并静态包含 **Opus 1.3.1**（保留其 BSD 许可及专利声明）。SE 自有代码的 AGPL-3.0-only 不替代这些库的许可。

## 对应的二进制和源码

四库原样来自 vgmstream 官方 r2083 Windows 包，SE 没有修改它们。所选 vgmstream helper 是另行按 `USE_G719=OFF` 重建的版本；它的改变不涉及本页四库。

| DLL | 版本 | SHA256 |
|---|---|---|
| avutil-vgmstream-57.dll | 57.28.100 | 89EF867A5BB25E203F61BB2D2ADD2D1B02C672A3706095C4522189F38489CF3C |
| swresample-vgmstream-4.dll | 4.7.100 | 4E3DB31079DDDF7588C743A2E414F4EEB954ACB55B0CD6D04D57F0E4148272E0 |
| avcodec-vgmstream-59.dll | 59.37.100 | DC6F469C2F1728D8F8809223C190AF693EE48E5E005B73711FC356C91B8C3ADC |
| avformat-vgmstream-59.dll | 59.27.100 | 909CF4B58656B0ED6913FF6E9964B76EADEAF65F1396852D90D4C8BC0CC18F05 |

同一次发布的 **SingleEditor-v0.1.0-dependency-sources.zip** 包含以下原始归档，以及本说明、来源清单和 SE 构建指南：

| 文件 | 固定来源 | 归档 SHA256 |
|---|---|---|
| ffmpeg-eacfcbae.tar.gz | FFmpeg n5.1.2，提交 eacfcbae690f914a4b1b4ad06999f138540cc3d8 | B5759EB412556DBA30D4330C4A6BB8AC55DA97DD6CA3F5AD6AF70C0B742A6518 |
| opus-1.3.1.tar.gz | Xiph Opus 1.3.1 原始发布 | 65B58E1E25B2A114157014736A3D9DFEAAD8D41BE1C8179866F144A2FB44FF9D |
| vgmstream-r2083.tar.gz | 提交 57df2e179d929532094f4e4dd42ce5395514622b | C0C3477A655FF56F31B44F24A5BE1FCC687A8ABAF36829A4853C789DC14F8D6E |

直接来源：[官方 DLL 包](https://github.com/vgmstream/vgmstream/releases/download/r2083/vgmstream-win.zip)、[FFmpeg 源码](https://codeload.github.com/FFmpeg/FFmpeg/tar.gz/eacfcbae690f914a4b1b4ad06999f138540cc3d8)、[Opus 源码](https://archive.mozilla.org/pub/opus/opus-1.3.1.tar.gz)、[vgmstream 源码](https://codeload.github.com/vgmstream/vgmstream/tar.gz/57df2e179d929532094f4e4dd42ce5395514622b)。

FFmpeg 和 Opus 的源归档保留完整实现、头文件、原始许可和构建文件。vgmstream 归档保留 `doc/BUILD-LIB.md`、`ext_libs/extra/ffmpeg_options.txt`、`ext_libs/extra/ffmpeg_extra.sh` 和可选兼容补丁。源码下载与二进制在同一次 GitHub Release 的 Assets 中提供；本地试用目录也包含这些附件。公开发布时必须一同提供，不用只含许可证的 ZIP 代替源码。

## 实际配置及源码对应依据

[NoticeOrigins.json](NoticeOrigins.json) 的 `observed_ffmpeg` 记录来自当前四库的实际版本、许可和完整配置字符串。依赖源码附件的 `FFMPEG_CONFIGURATION.json` 另存了逐库观察值。

- 四库报告 LGPL 2.1 或更高版本，配置没有 `--enable-gpl` 或 `--enable-nonfree`。
- `avcodec` 包含 `libopus 1.3.1` 版本字符串，配置启用 `libopus` 并链接 `opus.lib`；它未依赖外部 Opus DLL。这与固定上游配方的静态 Opus 步骤对应。
- `avutil` 使用 Windows BCrypt 随机数 API，符合 n5.1.2 原始 `libavutil/random_seed.c`。本包不使用仅供 Windows XP 的 `ffmpeg-revert-bcrypt-random.patch`，该可选补丁仍随上游源码保留。
- 固定上游配置文件比当前 DLL 的实际配置多一个显式 `--disable-vulkan`。构建本页配置时应使用实际配置，而不要把上游配置文件冒充历史完整命令。

本页根据官方来源、实际元数据、指定版本和上游构建步骤确认源码对应关系。没有独立重建历史 DLL，也不声称知道当年编译器的精确补丁版本。分发不要求得到逐字节相同的二进制。

## Windows x86 构建步骤

需要 Visual Studio C++ x86 工具、Windows SDK、MSYS2 的 shell/make 工具及 Yasm。使用空格较少的工作路径；以下示例使用上游的 `C:\vgmstream-dlls\sources`。先把三个源码归档解压，分别命名为 `ffmpeg`、`opus-1.3.1` 和 `vgmstream`。

1. 在 x86 Native Tools 命令行中，按归档内 `doc/BUILD-LIB.md` 的 libopus 章节构建 `opus-1.3.1/win32/VS2015/opus.sln` 的 **Win32 / Release** 静态库。上游示例使用 v142 工具集；使用其他已安装工具集时按实际环境选择并记录。保留原始 Opus 源码及许可。
2. 将 Opus 的 `include` 目录追加到本次命令行的 `INCLUDE`，将生成 `opus.lib` 的 Win32/Release 目录追加到 `LIB`。
3. 在继承该工具环境的 MSYS2 shell 中进入 FFmpeg 源目录；确认 `cl` 和 `link` 指向 MSVC，而不是同名 Unix 工具。仅调整本次 shell 的 PATH，不需要改动系统工具。
4. 运行以下实际配置，然后执行 `make` 和 `make install`。配置使用与当前 helper 匹配的 x86 和四库主版本。

```sh
sh ./configure \
  --build-suffix=-vgmstream \
  --disable-pthreads --disable-w32threads --disable-os2threads \
  --disable-static --enable-shared --disable-programs --disable-doc \
  --disable-avdevice --disable-swresample --disable-swscale --disable-postproc --disable-avfilter \
  --disable-network --disable-everything --disable-autodetect --disable-iconv \
  --disable-mediafoundation --disable-schannel --disable-sdl2 --disable-zlib \
  --disable-swscale-alpha --disable-amf --disable-cuda --disable-cuvid \
  --disable-d3d11va --disable-dxva2 --disable-ffnvcodec --disable-nvenc --disable-nvdec --disable-vdpau \
  --enable-swresample --enable-libopus \
  --enable-demuxer='ac3,eac3,spdif,asf,xwma,mov,oma,xmv,ogg,flac,wav,aac,mp3,smacker,bink,binka,caf,mpc,mpc8,tak,ape' \
  --enable-parser='ac3,mpegaudio,xma,vorbis,opus' \
  --enable-decoder='ac3,eac3,wmapro,wmav1,wmav2,xma1,xma2,aac,atrac3,atrac3p,mp2float,mp3float,smackaud,binkaudio_dct,binkaudio_rdft,pcm_s16be,pcm_s16be_planar,pcm_s16le,pcm_s16le_planar,pcm_s8,pcm_s8_planar,flac,vorbis,libopus,mpc7,mpc8,alac,adpcm_ima_qt,adpcm_ima_dk3,adpcm_ima_dk4,tak,ape' \
  --target-os=win32 --toolchain=msvc --arch=x86 \
  --extra-ldflags=opus.lib --prefix=/c/vgmstream-dlls/out/ffmpeg-32
make
make install
```

### 上游可选的 Opus 依赖检测修改

如果 MSVC 环境的 pkg-config 不能发现 `opus.lib`，上游提供以下精确的 `configure` 修改；原始文件和这段配方均在所附源码中。仅在出现该依赖检测问题、且 INCLUDE/LIB 已正确设置时使用：

```sh
sed -i -e 's/require_pkg_config libopus/: #require_pkg_config libopus/g' configure
```

它只跳过 pkg-config 检测，不改变编解码实现。本包的元数据无法区分历史构建是否采用了这项检测修改，故同时提供原始文件和精确修改步骤，而不宣称当年一定应用或一定未应用。SE 没有另行修改四库的源码；不应用 XP 随机数回退补丁。

完整上游步骤：[固定 Windows 构建配方](https://github.com/vgmstream/vgmstream/blob/57df2e179d929532094f4e4dd42ce5395514622b/doc/BUILD-LIB.md#ffmpeg)、[配置文件](https://raw.githubusercontent.com/vgmstream/vgmstream/57df2e179d929532094f4e4dd42ce5395514622b/ext_libs/extra/ffmpeg_options.txt)、[可选补丁用途](https://raw.githubusercontent.com/vgmstream/vgmstream/57df2e179d929532094f4e4dd42ce5395514622b/ext_libs/extra/ffmpeg_extra.sh)。本说明整理这些步骤；本轮没有运行新的 FFmpeg 构建。

## 替换库与许可

四库保留为可替换 DLL，位于 `VGM/vgmstream-win/`。可以使用兼容的自行修改构建替换它们；须匹配 x86 架构、导出接口和相互依赖。主版本不同的 FFmpeg 不能只改名混用，需相应重建 helper。SE 不限制 LGPL 允许的修改，以及为调试修改后库而进行的反向工程。

原始许可随包保留：[LGPL 2.1](ffmpeg.COPYING.LGPLv2.1)、[LGPL 3](ffmpeg.COPYING.LGPLv3)、[Opus 许可](opus-1.3.1.COPYING)。源码归档也保留自身完整许可。分发范围与建议见 [FFmpeg 官方说明](https://ffmpeg.org/legal.html)。
