# Single Editor Windows 构建

本页记录 Windows GUI/CLI 的构建路径。使用完整的项目源码 checkout，依赖和生成资源按下述步骤取得。产品与交付边界见 [README](README.md)，变更及检查入口见 [贡献指南](CONTRIBUTING.md)。

## 输入与工具

需要完整的 `SingleEditorJuce/CMakeLists.txt`、`Source/`、`Tests/`、`Resources/`、`cmake/`，以及仓库同级的 `icons/` 声明资源和 `tools/release/` 构建工具。不能只复制已经入 Git 的 `.cpp` 文件。构建所需的生成头、资源包和 JUCE overlays 由现有 CMake/Node 步骤生成，不需要复制旧 `build/` 或 `out/`。

当前已记录的工具组合是 Visual Studio 2026（C++ 桌面工具，x64）、MSVC 19.50.35728.0（安装目录版本14.50.35717）、Windows SDK10.0.26100、CMake4.3.0 和当前 Node25.7.0。历史产品 capture未固定当时的 Node版本。项目 CMake 最低声明3.24；以下示例采用已用过的 Visual Studio18生成器，不表示其它工具链已验证。PowerShell7用于完整构建证据脚本，普通 CMake命令可从普通 PowerShell运行。不要把 MSBuild 托管在 Context Mode内。

工具获取可从以下官方入口开始。Visual Studio Installer 中本机确认的组件为 `Microsoft.VisualStudio.Workload.NativeDesktop`、`Microsoft.VisualStudio.Component.VC.Tools.x86.x64`、`Microsoft.VisualStudio.Component.Windows11SDK.26100` 和 `Microsoft.VisualStudio.Component.VC.Redist.14.Latest`。组件系列 ID 不保证取得与旧候选完全相同的补丁字节。

- [Visual Studio 2026 发布历史](https://learn.microsoft.com/en-us/visualstudio/releases/2026/release-history)提供固定版本 Build Tools 入口；它与当前 Community 安装不是同一产品，尚未作项目替代验证。
- [CMake 4.3.0 Windows x64 ZIP](https://github.com/Kitware/CMake/releases/download/v4.3.0/cmake-4.3.0-windows-x86_64.zip)，官方 SHA256：`0F664F44EEB5B35B77F83E0984459C0DFA70539BD215C54AE1059604FD4F77EA`。
- [Node 25.7.0 Windows x64 ZIP](https://nodejs.org/dist/v25.7.0/node-v25.7.0-win-x64.zip)，官方 SHA256：`4A66EAC416C30474FE9C3F0EF7D4FFC85A8797CBC35F6B8566DFBCE02789A9C4`。
- [PowerShell 7.6.5 Windows x64 ZIP](https://github.com/PowerShell/PowerShell/releases/download/v7.6.5/PowerShell-7.6.5-win-x64.zip)，官方 SHA256：`32EB8F6CDCE08F86E987D625A2733E54AC3E289AE7E1621B14C0B5BCEC2434EA`；[同版本发布页](https://github.com/PowerShell/PowerShell/releases/tag/v7.6.5)也提供 MSI。该公开版本与当前宿主版本号相同，尚未将 ZIP 内文件与 Codex bundle 逐字节比较。
- [Windows SDK 下载表](https://learn.microsoft.com/en-us/windows/apps/windows-sdk/downloads)列有10.0.26100.7705及[对应安装器](https://go.microsoft.com/fwlink/?linkid=2349110)，不再仅依赖旧归档页中的26100.1742。目录版本10.0.26100.0与已服务产品版本7705须分别记录。
- [VC Redist 14.50.35719 x64 精确入口](https://aka.ms/vs/18/release/14.50.35719/VC_redist.x64.exe)来自[微软旧版本获取说明](https://learn.microsoft.com/en-us/cpp/windows/troubleshoot-vc-redistributable-installation-issues?view=msvc-170#download-old-versions-of-the-visual-c-redistributable-installer)。它是系统运行库安装器，尚未证明其中 DLL 与当前四个 app-local 文件完全相同。

当前 Windows SDK 实际安装服务版本为26100.7705，VC app-local运行库为14.50.35719.0；上述精确版本入口已确认，但尚未建立全部 MSVC/SDK/CRT 载荷与本机文件的对应关系。固定 VS Build Tools18.4.3 bootstrapper 已核对实际版本、SHA及微软签名；其内含固定目录入口返回的产品目录却与上级清单声明的大小/SHA不符，当前未将其包清单当作已验证输入。这一差异阻止采用该目录作为已验证的公开工具链获取配方，不阻止使用现有已捕获的 Community/MSVC 进行真实构建验证。原计划不要求逐包重现历史 Community 安装；公开 clean checkout 仍须有实际可用的工具获取路线和功能/资源对照，不能把现有安装、下载入口或正确版本字符串误写成另一台机器已完整复现。

项目依赖由现有 CMake管理：

| 依赖 | 固定来源 | 获取方式 |
|---|---|---|
| JUCE8.0.12 | 官方 tag归档，SHA256 `DEC1A8BAEE5AAEC4502B717A421B850E257AB7E9C2EFD03766185EC1957CF102` | 优先显式源码根，否则现有本地缓存，再下载官方归档 |
| ASIO SDK2.3.4 | 官方2025-10-15归档，SHA256 `D5EBF0C20DD2C5F43771FD0C1418F4B361BF52434EE670097CFA6B3A335E2ECA` | 显式 SDK根或 CMake下载，另检查三个头文件和许可证 |
| libsamplerate0.2.2 | 官方 tag归档，SHA256 `16E881487F184250DEB4FCB60432D7556AB12CB58CAEA71EF23960AEC6C0405A` | CMake下载并静态构建，构建输出附带上游 BSD声明 |

JUCE生成改动保留在当前构建目录：Windows graphics、WASAPI诊断、WAV及 FFT相关 overlays 和编译时间源。共享 JUCE不应被手工修改。依赖来源、许可证和可公开交付范围仍须以开源准备记录为准，构建成功本身不完成这些事项。

## 未提交开发树的源码重放副本

现有工具可以在一个全新、位于仓库之外的目录准备当前实际输入：

```powershell
node tools/release/Compare-CleanBuilds.mjs prepare-source `
  --source ./SingleEditorJuce --destination ../SingleEditor-source-replay-01
node tools/release/Compare-CleanBuilds.mjs verify-source `
  --manifest ../SingleEditor-source-replay-01/source-replay.json
```

它复制上述有限源码根、release工具和当前声明的21个核心SVG，包含必需的未跟踪文件；核对复制前后的源字节、复制件字节和完整成员集合。已有目标目录不会被覆盖。可选 `--build <已配置目录>` 只记录依赖根、工具路径和 source epoch的现有声明，不复制或认证外部依赖。这个副本没有 `.git`，不应称作 Git clean checkout，也不是完整公开仓库或已通过的 clean build。

## 普通 Release 构建

在完整源码根打开普通 PowerShell，选一个全新的构建目录。以下 `sourceEpoch` 是现有工程候选使用的明确时间值；新发布应记录其自己的源版本时间，不能为了获得相同哈希随意调整。

```powershell
$sourceRoot = (Resolve-Path 'SingleEditorJuce').Path
$buildRoot = [IO.Path]::GetFullPath('../SingleEditor-build-A')
$sourceEpoch = '1788108049'

cmake -S $sourceRoot -B $buildRoot -G 'Visual Studio 18 2026' -A x64 `
  -DSINGLE_EDITOR_ENABLE_OPEN_TIMING=OFF `
  "-DSINGLE_EDITOR_SOURCE_DATE_EPOCH=$sourceEpoch"
if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed' }

cmake --build $buildRoot --config Release --target SingleEditor SingleEditorCLI -- /m:2 /nr:false
if ($LASTEXITCODE -ne 0) { throw 'Release build failed' }
```

无 `.git` 的源码副本必须明确提供 `SINGLE_EDITOR_SOURCE_DATE_EPOCH`。离线构建可在配置时传入 `-DSINGLE_EDITOR_JUCE_ROOT=<JUCE源码根>` 和 `-DSINGLE_EDITOR_ASIO_SDK_ROOT=<SDK根>`，并使用 CMake现有 FetchContent源码覆盖提供固定版本的 libsamplerate。不要复制未说明来源的本机 SDK文件来绕过校验。

产物位于：

- `<buildRoot>/SingleEditor_artefacts/Release/Single Editor.exe`
- `<buildRoot>/SingleEditorCLI_artefacts/Release/SingleEditorCLI.exe`

可选媒体运行时及完整 portable不是仅编译这两个 EXE即可交付。`build_portable_release.bat` 是现有显式输入的分发组装入口，不是编译脚本；它要求已核验的候选、媒体运行时、图标、CRT、构建及身份记录。固定 portable的推广是单独受控步骤。

现有 portable 推广工具绑定维护者约定的固定目标，不是可直接用于任意电脑的通用发布命令。普通源码构建不依赖该目标。性能清单也保留原始语料和冻结基线的引用，源码集合不附带这些音频或历史二进制；执行性能/分发验收前必须提供其要求的真实输入，不能把缺少样本或空跑结果当作通过。

## 性能输入与复现边界

以下四份文件是测量合同及历史身份记录，普通 GUI/CLI 源码编译不读取它们。执行 [Core Open benchmark](tools/Invoke-CoreOpenBenchmark.ps1) 时需要另行提供真实输入；不附带用户音频或历史产品二进制，也不自动把开发 smoke 当作正式语料。

| 文件 | 保留含义与调用方式 |
|---|---|
| [contract.json](Document/New_JuceEditor/milestone3/performance/core-open-v1/contract.json) | CoreReady 定义、协议和样本合同不变。固定 portable 路径只是省略 `-ExePath` 时的维护者默认值；其他位置显式传入 `-ExePath`。 |
| [corpus.json](Document/New_JuceEditor/milestone3/performance/core-open-v1/corpus.json) | 正式 WAV 和开发 smoke 各有固定大小/SHA，均不随源码分发。`-AudioPath` 或 `-SharedRoot` 仅指定位置，脚本仍核对对应字节数和 SHA；不能随便换一段音频取得正式结果。 |
| [thresholds.json](Document/New_JuceEditor/milestone3/performance/core-open-v1/thresholds.json) | 相对退化、绝对时限和环境稳定性要求原样保留；无外部路径，也不因公开环境缺件而放宽。 |
| [M2_FINAL_BASELINE.json](Document/New_JuceEditor/milestone3/performance/core-open-v1/baselines/M2_FINAL_BASELINE.json) | `path_at_audit` 是历史位置；实际比较显式传 `-BaselineExePath`，并核对冻结 M2 的大小与 SHA。未取得该二进制时，这项正式比较仍缺输入；历史外部窗口数据不能当作新的 CoreReady handshake 结果。 |

## 构建证据与等价边界

验证干净源码时，从选定提交执行 `git clone` 到新目录，检查 `git status --porcelain` 为空，再按上面的命令配置到全新的构建目录，编译 GUI/CLI 并运行相关测试。依赖可以使用已核对的固定源码缓存；应说明是否重新下载，以及实际使用的编译器/SDK。不要复制旧生成头、对象文件或产品 EXE 来替代编译。

源码内容一致、Git来源身份一致、最终 EXE逐字节相同是三个不同结果。当前 CMake会记录真实 `HEAD[-dirty]`；无 Git时为 `not-reported`。现有 `/pathmap` 主要规范构建根，迁移源码根后的字节可复现性仍需实际构建比较。准备源码副本及配置通过，不等于实际编译、运行、性能或最终 RC通过。

## Optional vgmstream runtime

SE launches the vgmstream CLI only for supported optional-media requests. For the local v0.1 candidate, use upstream r2083 commit `57df2e179d929532094f4e4dd42ce5395514622b` and its official Windows codec DLLs, keeping their notices. Build into a new directory with Visual Studio 2026:

```powershell
$vgmSource = (Resolve-Path '<fixed-vgmstream-source>').Path
$vgmBuild = [IO.Path]::GetFullPath('<new-vgmstream-build>')
cmake -S $vgmSource -B $vgmBuild -G 'Visual Studio 18 2026' -A Win32 `
  -DUSE_G719=OFF -DUSE_G7221=ON -DUSE_MPEG=ON -DUSE_VORBIS=ON `
  -DUSE_FFMPEG=ON "-DFFMPEG_PATH=$vgmSource/ext_includes/ffmpeg" `
  -DUSE_ATRAC9=ON -DUSE_CELT=ON -DUSE_SPEEX=ON `
  -DBUILD_CLI=ON -DBUILD_FB2K=OFF -DBUILD_WINAMP=OFF -DBUILD_XMPLAY=OFF `
  -DCMAKE_POLICY_DEFAULT_CMP0091=NEW -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded
if ($LASTEXITCODE -ne 0) { throw 'vgmstream configure failed' }
cmake --build $vgmBuild --config Release --target vgmstream_cli -- /m:2 /nr:false
if ($LASTEXITCODE -ne 0) { throw 'vgmstream build failed' }
```

`FFMPEG_PATH` selects upstream's bundled Windows headers/import-library route; the codec DLLs still need to be supplied. Check the configure summary: FFmpeg and the other selected codecs must stay ON. Automatic FFmpeg discovery can otherwise turn it OFF. Static MSVC runtime avoids adding a second, x86 VC runtime to the x64 SE payload.

Prepare a separate runtime directory with the official archive members except `vgmstream-cli.exe`, `libg719_decode.dll`, `in_vgmstream.dll`, and `xmp-vgmstream.dll`; add the rebuilt `cli/Release/vgmstream-cli.exe`. Preserve the original archive separately. Confirm the rebuilt CLI has no G.719 import, then verify real WEM decoding and SE BNK playback against references. Deleting the DLL without rebuilding is insufficient.

Replace only `libmpg123-0.dll` with the member from the [official mpg123 1.31.0 x86 archive](https://www.mpg123.org/download/win32/mpg123-1.31.0-x86.zip), also linked by the fixed r2083 build instructions. Archive SHA256: `8B76A5F760EB98A3EC5C1EC1C619E4721E0E5C641286823DF382330C9B3C8B1C`; DLL SHA256: `F2963C9B8AEE2DF1C28CA323CAA72CE14F0316694636AC58F0F9D47C9810ACAB`. Retain the [matching source archive](https://www.mpg123.org/download/mpg123-1.31.0.tar.bz2), SHA256 `481BC0DD78904B28D7F8DF7456603A71BDCBC7140716C0C3EC8A1A6635F58C81`, and its COPYING. Do not add the archive's other executables or output plugins to SE. A generated stereo RIFF/MP3 fixture decoded through the rebuilt helper identically with the old and selected DLLs; real Opus WEM and embedded BNK reference outputs also remained identical.

This build removes BNSF IS22/Siren22/G.719 decoding. It preserves G.722.1/Siren14 and current Wwise BNK/WEM paths. Original third-party license texts and corresponding-source obligations still apply; the local-use candidate is not public-release approval.
