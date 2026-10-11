# 参与 Single Editor 开发

Single Editor 仍处于 M3 开发阶段。请围绕可观察的问题提交局部变更，并把实际验证结果与尚待用户或设备验收的部分分开说明。构建入口和工具要求统一见 [BUILDING.md](BUILDING.md)。

## 开始一个变更

先查看相关源码、测试和本仓库的构建说明。说明问题出现的操作、期望行为和本次修改范围；涉及产品方向、既定验收标准或依赖许可的变化，先与维护者确认。

编辑前检查当前分支、`git status --short`、未暂存及已暂存差异。开发树可能包含别人的修改、必要的未跟踪文件或尚未交付的资源；不要自动清理、批量回退或覆盖它们。提交前只暂存自己明确负责的文件并检查暂存差异，不使用全仓库批量加入来收集未知资产。必要输入尚未纳入 Git 时，应先说明其来源和归属。

沿用现有组件和构建流程。修复功能时不顺便重构无关模块，也不附带个人或全局开发工具配置变更。仓库中的流程说明不授权修改使用者的全局 harness、hooks 或其他工程设置。

## 源码、资源与依赖边界

- 音频读取、播放、波形和基本编辑是核心；其他模块的资源加载和初始化应保持按需发生。修改打开路径时保留真实波形出现与可播放的计时边界，不能靠移动计时点隐藏退化。
- 使用 [现有 CMake 项目](SingleEditorJuce/CMakeLists.txt) 和 [项目 helpers](SingleEditorJuce/cmake/)。依赖改动应说明版本、来源、完整性和必要的许可信息，不手工修改共享 JUCE 树。
- 构建适配和生成文件放在构建目录，由已有生成步骤重现。不要把本机绝对路径、未说明的私有文件或人工复制步骤变成新构建依赖。
- 产品资源变更同时检查声明入口、真实透明通道或文件格式，以及相关 UI。保留已有用户认可的显示模式、主题和交互；必要的新素材应附来源说明。
- 不提交构建目录、历史候选、诊断原包、用户音频、恢复数据、凭据或个人设置。测试使用可再生成或已获授权的最小样本；第三方二进制不能仅因本机存在就加入源码仓库。

## 选择相关检查

根据改动影响选择现有测试和真实产品操作，先完成能直接验证问题的检查。失败时保留首次输出并解释原因；通过后，只有新变化或未解决的问题才需要扩大或重复检查。现有正式发布门禁仍按对应规格执行，局部检查不能代替最终验收。

当前 [CMakeLists.txt](SingleEditorJuce/CMakeLists.txt) 中的相关目标包括：

| 修改范围 | 可复用的检查入口 |
|---|---|
| 路由、基础逻辑与工作区布局 | `SingleEditorLogicTests` |
| 编辑、历史、保存和文档恢复 | `SingleEditorDocumentTests`；产品接缝另看 `SingleEditorNativeRecoveryTests` |
| 参数 EQ / True Peak Limiter | `SingleEditorParametricEqTests` / `SingleEditorTruePeakLimiterTests` |
| CLI | `SingleEditorCliTests` |
| 工作区与可访问性 | `SingleEditorWorkspaceModuleHostTests`、`SingleEditorCommandAccessibilityTests` 等对应目标 |
| 构建、重放或分发工具 | [tools/tests](tools/tests/) 中对应的 Node/PowerShell 检查 |

按构建指南完成配置后，可在同一个普通 PowerShell 中只构建并运行相关目标。例如基础逻辑变更：

```powershell
cmake --build $buildRoot --config Release --target SingleEditorLogicTests -- /m:2 /nr:false
if ($LASTEXITCODE -ne 0) { throw 'Logic test build failed' }
& (Join-Path $buildRoot 'Release/SingleEditorLogicTests.exe')
if ($LASTEXITCODE -ne 0) { throw 'Logic tests failed' }
```

这里的 `$buildRoot` 沿用构建指南配置的目录。部分测试目标是显式选择构建的目标，输出位置与参数也可能不同；执行前查看对应源码入口。当前 CMake 未注册统一的 CTest 套件，不要把空的 `ctest` 结果当作项目测试通过。涉及原生 UI、音频设备或外部媒体的测试，应明确所用环境和样本。

工具测试先检查脚本参数。源码重放工具的独立文件夹回归可直接运行：

```powershell
node tools/tests/Test-SourceReplayPreparation.mjs ../singleeditor-source-replay-check-01
if ($LASTEXITCODE -ne 0) { throw 'Source replay checks failed' }
```

示例输出目录必须尚不存在；新的测试运行使用新目录，避免覆盖之前结果。其他证据工具可能需要真实构建输入，并用特定退出码表示尚未完成；按其契约记录退出码和状态，不把 pending 写成通过。

变更说明应给出修复前后的行为、执行过的命令或操作、结果和未覆盖的条件。UI 变更附可审阅画面；性能变更说明候选、样本和环境。涉及普通音频打开路径时使用既定 Core Open 测量方法，分别记录波形与可播放结果。真实 ASIO、特定声道设备、物理 DPI/跨屏和屏幕阅读器等结果须来自对应环境。

## 报告问题

向项目维护者提供以下最小信息；公开 issue 入口启用后使用仓库实际入口：

1. GUI 或 CLI、所用版本/构建来源、Windows 版本；CLI 问题附脱敏后的命令和退出码。
2. 从启动或打开文件开始的最短操作步骤，期望结果、实际结果，以及能否稳定复现。
3. 音频问题附格式、时长、采样率、声道数和编码；设备问题再补驱动类型、设备型号及相关路由。UI 问题补主题、General/Simple 模式、缩放比例及显示器配置。
4. 最小截图或相关错误片段。能用自制短音频重现时优先使用它，并说明样本是否可共享。

分享前检查诊断包和日志中的用户名、绝对路径、文件名、设备标识及音频内容。不要公开原始工程、恢复目录、未获授权的媒体、凭据或完整内存转储；只提供复现所必需且可共享的内容。发现敏感问题时先使用维护者已明确提供的私下渠道，不把敏感细节先发到公开 issue。

## 交付说明

代码变更不会自动授权上传、发布、签名、安装或推广固定 portable。自有、可授权的项目代码采用 **AGPL-3.0-only**，完整文本见 [LICENSE](LICENSE)；第三方代码、字体与素材保留各自许可和版权声明。贡献指南沿用该项目许可证，CLA 或权利转让事项仍须单独明确。第三方/素材分发条件和最终对应源码交付继续在既有交付计划中处理。
