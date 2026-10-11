# Single Editor

Single Editor 是基于 JUCE 的桌面音频编辑器，目前以 Windows 为主要开发平台。它将音频读取、播放、波形预览和基本编辑作为核心，将处理器和辅助面板组织为按需使用的模块（Mag）。

项目处于 **M3 开发中**。当前源码包含已接入的功能，正在完成工程收尾和用户、设备验收；正式下载尚未发布。

## 当前代码支持

- 普通音频打开、播放和波形显示，以及选区删除、增益/淡入淡出、保存、Undo/Redo。读取实现包含 WAV、AIFF、FLAC、Ogg Vorbis 和 MP3；实际格式能力仍受编码、文件内容和相应后端约束。
- Light/Night 外观和 General/Simple 显示模式；General/Simple 是工作区显示方式，参数 EQ 与 True Peak Limiter 是独立的 Mag。
- 元数据、标记、输出电平、空间能量及 Ambisonics 相关面板，按需接入工作区。
- 自动恢复存储、Recovery Center 恢复入口和本地诊断界面。
- 独立 CLI，包含单文件处理、批处理和诊断入口；BNK/WEM 等可选媒体路径需要相应运行时。

普通音频核心应先达到可显示波形、可播放的状态，可选模块保持惰性。新增模块不能把初始化成本移到普通音频打开路径上。

真实 ASIO/多声道设备、压缩媒体样本、物理 DPI/跨屏、屏幕阅读器及部分 Simple 交互仍有待验收项。最终品牌资源、安装包和 RC 也尚未完成统一验收；代码或自动检查通过不替代这些结果。

## 构建与运行

从 [Windows 构建指南](BUILDING.md) 开始。它记录工具链、固定依赖、完整输入要求、普通 PowerShell 下的 `/m:2` Release 构建命令，以及 GUI/CLI 产物路径。构建命令以该指南为准。

构建必须包含完整产品源码、资源、CMake helpers、声明使用的 sibling icons 和 release 工具。2026-10-11 已完成产品仓库的真实 clean checkout、GUI/CLI Release 构建及相关文档、保存、恢复、波形检查；这不是另一台全新电脑的验证。

本地试用包的 vgmstream CLI 按上游开关关闭 G.719，其余解码选项保留。BNK/WEM 仍可使用；BNSF 中 IS22（Siren 22/G.719）编码不再由该运行库解码。构建方法见 [可选运行库](BUILDING.md#optional-vgmstream-runtime)。

[build_portable_release.bat](build_portable_release.bat) 是使用显式候选、依赖和身份记录的分发组装入口，不是编译入口。仅编译两个 EXE 不会得到完整 portable；运行时、第三方声明和最终分发输入需要另行准备。

## 代码位置

| 路径 | 内容 |
|---|---|
| [SingleEditorJuce/Source](SingleEditorJuce/Source/) | 音频、文档与历史、恢复、Mag、原生 UI 和 CLI |
| [SingleEditorJuce/Tests](SingleEditorJuce/Tests/) | C++ 逻辑、文档、DSP、UI 与恢复测试 |
| [SingleEditorJuce/Resources](SingleEditorJuce/Resources/) | 产品资源与已有第三方资源声明 |
| [SingleEditorJuce/cmake](SingleEditorJuce/cmake/) | 项目构建辅助与构建目录内的依赖适配 |
| [icons](icons/) | 项目声明使用的图标源文件 |
| [tools/release](tools/release/) | 构建记录、源码重放、检查与分发工具 |
| [tools/tests](tools/tests/) | 工具和工程边界的针对性检查 |

## 参与与交付准备

变更和问题报告请参阅 [贡献指南](CONTRIBUTING.md)。Single Editor 自有、可授权的项目代码采用 **AGPL-3.0-only**，完整文本见 [LICENSE](LICENSE)。第三方代码、适配部分、字体与素材继续遵守各自许可和版权声明。公开仓库需包含实际构建与资源生成所需的完整输入，以及适用许可证和第三方声明；正式下载还需完成分发与验收。

已整理的编译依赖声明见 [第三方声明及来源](tools/release/notices/ThirdPartyNotices.md)。其中保留各依赖原文；最终对应源码交付继续按发布准备计划完成。第三方许可优先适用于对应文件，AGPL-3.0-only 是项目自有代码的固定版本选择。

21 个嵌入式工具栏图标的固定上游版本和本地文件映射见 [图标来源](icons/NOTICE.md)。应用品牌使用已选定的连续波形，源文件及生成方法见 [品牌资源](SingleEditorJuce/Resources/BRAND.md)。

开发与发布工具引用的格式/算法来源另见 [源码工具声明](tools/release/notices/source-tools/README.md)。这些说明随相应工具源码保留，不代表新增产品运行时依赖。
