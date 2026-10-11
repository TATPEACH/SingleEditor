# 构建、测量与分发工具

普通源码编译从仓库根目录的 [BUILDING.md](../../BUILDING.md) 开始。

| 用途 | 入口 |
|---|---|
| 保留或核对有限源码副本 | `Compare-CleanBuilds.mjs prepare-source / verify-source` |
| 普通音频打开速度 | `../Invoke-CoreOpenBenchmark.ps1` |
| 针对实际启动问题采集 ETW | `Invoke-StartupTrace.ps1`、`etw/` |
| 组装已有候选和依赖 | `New-ReleasePayload.ps1` |
| 核对 payload、安装包内容及运行 | `Test-ReleasePayload.ps1`、`Test-InstallerPayload.ps1`、`Test-PortableFinalRuntime.ps1` |
| 从已核对 payload 制作安装包 | `Build-Installer.ps1` |

`Finalize-ProductLink.mjs`、`Finalize-LinkEvidence.mjs`、`BuildArtifactEvidence.mjs` 和 `dia/` 支持 CMake 显式选择的私有链接记录。普通构建默认关闭该选项，PDB 和链接记录不进入用户下载包。

相关工具检查在 [tools/tests](../tests/)；运行前查看脚本参数，使用新的输出目录。性能检查遵守固定语料、真实波形和播放就绪合同；用户媒体及测量结果不随源码分发。

模块、依赖、符号或运行耗时的诊断应服务具体问题。全 EXE 逐字节语义归属、未知项清零及归属百分比不属于默认开发或发布要求。历史研究脚本、生成表和原始证据留在维护者的内部档案中。

`notices/` 保留相应第三方声明。可选解码器运行时和对应源码需要单独准备，编译通过不代表这些分发材料已经齐备。
