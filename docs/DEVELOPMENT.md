# 在另一台电脑接续开发

GitHub 的 `main` 是完整产品源码；`v0.1.0` 标签固定首次公开预览版。从新电脑开发不需要家里的 worktree、`handoff/`、构建缓存或旧候选包。产品行为先看 [README](../README.md) 与 [操作手册](../MANUAL.md)，构建细节看 [BUILDING.md](../BUILDING.md)，改动与检查约定看 [CONTRIBUTING.md](../CONTRIBUTING.md)。

## 1. 拉取源码和建立工作分支

```powershell
git clone https://github.com/TATPEACH/SingleEditor.git
Set-Location SingleEditor
git switch -c codex/my-change
```

`codex/my-change` 是示例任务分支名，按实际任务命名即可。已有 clone 时，先保存或提交本机改动，再在 `main` 执行 `git pull --ff-only`，然后开任务分支。不要在两台电脑之间复制 `.git`、构建目录或本机绝对路径配置。跨电脑接续同一个未合并任务时，提交并推送该任务分支，在另一台电脑获取该分支；仅保存在家里 worktree 的未提交改动不会随 clone 到公司。

日常修改在任务分支完成，验证后合入 `main`；版本标签保持不动。新的正式包使用新版本标签，避免让已下载的包与同名 Release 内容不同。

公开仓库的 clone 不需要登录；从公司电脑 push 时仍需在那台电脑配置 GitHub 登录或 SSH。不要把家里的访问令牌、凭据缓存或全局配置提交进仓库。

## 2. 安装工具并编译

本项目已验证的组合是 **Visual Studio 2026 + C++ 桌面开发工作负载、Windows SDK、CMake 4.3、Node.js 25**。精确记录和官方工具入口见构建指南。这里的命令使用 `Visual Studio 18 2026` 生成器；旧版 VS 不是已经验证过的替代组合。Node 用于生成资源，没有需要安装的 npm 依赖。

在仓库根的普通 PowerShell 执行：

```powershell
$buildRoot = [IO.Path]::GetFullPath('../SingleEditor-build')
cmake -S SingleEditorJuce -B $buildRoot -G 'Visual Studio 18 2026' -A x64 `
  -DSINGLE_EDITOR_ENABLE_OPEN_TIMING=OFF `
  -DSINGLE_EDITOR_ENABLE_LINK_EVIDENCE=OFF `
  -DSINGLE_EDITOR_ENABLE_PRIVATE_SYMBOLS=OFF
if ($LASTEXITCODE -ne 0) { throw 'Configure failed' }

cmake --build $buildRoot --config Release --target SingleEditor SingleEditorCLI -- /m:2 /nr:false
if ($LASTEXITCODE -ne 0) { throw 'Build failed' }

$gui = Join-Path $buildRoot 'SingleEditor_artefacts/Release/Single Editor.exe'
$cli = Join-Path $buildRoot 'SingleEditorCLI_artefacts/Release/SingleEditorCLI.exe'
& $gui
```

在 Git clone 内配置时，源码身份和时间由 Git 提供，无需沿用旧候选的固定时间戳。首次配置会下载并校验 JUCE 8.0.12、ASIO SDK 2.3.4、libsamplerate 0.2.2，源码根、哈希和本地适配都在仓库内声明；生成头和资源包会在新的构建目录生成。网络需允许访问 GitHub 与 Steinberg 下载站。

公司网络若阻止这些下载，可以在允许的网络环境取得同次 Release 的 `dependency-sources.zip`，校验 SHA256 后解开其中的三份原始归档。在同一次 CMake 配置命令中分别传入 `-DSINGLE_EDITOR_JUCE_ROOT=<JUCE源码根>`、`-DSINGLE_EDITOR_ASIO_SDK_ROOT=<ASIO SDK源码根>` 和 `-DFETCHCONTENT_SOURCE_DIR_SINGLE_EDITOR_SAMPLERATE=<libsamplerate源码根>`；带空格的整个参数用双引号包住。SDK 根含 `common/` 和 `LICENSE.txt`，另外两个源码根含 `CMakeLists.txt`。不需要复制家里的 SDK 或 build cache。编译工具本身仍须按公司允许的方式安装。

开发构建直接从它的输出目录运行，保持生成的 `resources/`、`Ambisonics/` 和 `licenses/` 相对位置。先正常退出已打开的 SE 再测试新程序：SE 是单实例，旧进程会接收新进程的打开请求。开发时不必把候选目录设成 Windows 默认应用。

## 3. 普通音频与可选运行库

普通 WAV、AIFF、FLAC、Ogg Vorbis、MP3 的开发不需要下载便携包。BNK/WEM 开发还需要独立的 vgmstream helper 和相应 DLL，它们通过 Release 分发，未放入源码 Git 历史。

最直接的准备方式是下载并完整解压同次 Release 的 `windows-x64-portable.zip`，保持其中的 `VGM/vgmstream-win/` 完整，然后在当前开发终端指定 helper：

```powershell
$runtimeRoot = (Resolve-Path '../SingleEditor-runtime-v0.1.0').Path
$env:SINGLE_EDITOR_VGMSTREAM_PATH = Join-Path $runtimeRoot 'VGM/vgmstream-win/vgmstream-cli.exe'
& $gui
```

`SingleEditor-runtime-v0.1.0` 是示例解压目录。这个环境变量只对当前终端及其子进程生效。也可把完整 `VGM/` 复制到 GUI 或 CLI 各自的输出目录。不要用它替换新编译的 SE EXE，也不要使用未经说明的旧 vgmstream 包。

已发布 helper 通过上游开关关闭 G.719，BNSF IS22 / Siren 22 不可解码；现有 BNK/WEM 支持保留。要修改 helper，按构建指南的固定来源、G719-OFF 参数和许可交付要求重建。视频音频提取另需 `ffmpeg.exe`，与包中的 FFmpeg DLL 作用不同。

## 4. 跑相关测试

控制台检查使用代码生成的临时样本和仓库内已声明的资源，不需要把个人音频带到公司。**从仓库根目录执行以下命令**：Logic 测试还会按当前目录定位 `SingleEditorJuce/Resources/Ambisonics/` 中的 SADIE 测试资源，不能从 build 目录或仓库上一级直接照抄运行。

```powershell
cmake --build $buildRoot --config Release `
  --target SingleEditorLogicTests SingleEditorDocumentTests SingleEditorCliTests -- /m:2 /nr:false
if ($LASTEXITCODE -ne 0) { throw 'Test build failed' }

foreach ($name in @('SingleEditorLogicTests', 'SingleEditorDocumentTests', 'SingleEditorCliTests')) {
  & (Join-Path $buildRoot "Release/$name.exe")
  if ($LASTEXITCODE -ne 0) { throw "$name failed" }
}
```

按改动选择对应测试，不必每次重跑全部项目。原生界面、真实 ASIO / 多声道设备和听音验证另有环境要求，见贡献指南；当前没有统一注册的 CTest 套件。

## 5. 从哪里读代码

| 位置 | 主要职责 |
|---|---|
| `Source/App/` | 应用、窗口与单实例入口；应用持有文档、音频引擎和波形模型 |
| `Source/Core/` | 核心就绪与模块生命周期协调 |
| `Source/Document/`、`Source/Recovery/` | 编辑状态、保存、工作区会话和恢复 |
| `Source/Audio/`、`Source/Waveform/` | 播放、设备路由、监听处理和波形生成 |
| `Source/UI/` | 主界面、Mag 面板、布局及交互 |
| `Source/Bnk/`、`Source/Metadata/` | 游戏音频容器、事件关联及元数据 |
| `Source/CLI/`、`Source/Automation/`、`Source/AudioSuite/` | 命令行、处理协议与音频操作 |
| `Tests/`、`cmake/`、`tools/` | 测试、依赖适配、资源生成和可选开发 / 发布工具 |

以上位置都相对于 `SingleEditorJuce/`，只有 `tools/` 位于仓库根。程序行为以源码和测试为准，手册提供现有操作语义；不要把未同步的聊天讨论当成新电脑已有的设计事实。

## 当前开发边界

v0.1.0 已公开，下一项产品功能尚未指定。继续保持普通音频读取、播放、首帧有效波形和基本编辑的优先级；扩展面板、特殊格式与分析功能按需启用。保存安全、编辑结果和相关音频数据保持承诺仍需针对改动验证。

仓库保留了已有构建 / 体积 / 链接诊断工具作为可复用成果，它们不是每次开发的必经流程。全量逐字节归属不构成产品目标、默认发布要求或未完成缺陷。

正式性能脚本的合同已在 `Document/New_JuceEditor/milestone3/performance/core-open-v1/` 中。其原始压力音频、历史基线 EXE 和本机固定路径不是普通编译的依赖，也不会随源码分发；缺少这些输入时，不应把该专项空跑当作验证通过。日常功能验证优先使用可生成的最小样本。
