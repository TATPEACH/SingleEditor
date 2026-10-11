# Single Editor 操作手册

适用于 v0.1 的 Windows x64 本地试用版，随首次公开版本一起定稿。界面中的按钮和命令保留英文名称，便于对照查找。

Single Editor（SE）把播放、波形查看和单文件编辑放在同一个窗口里：先打开并听到音频，需要时再进入选区编辑、处理器和多声道监听。

> v0.1 是早期版本。请先用文件副本熟悉编辑和保存；硬件设备、特殊编码和复杂 Wwise 事件的兼容性有各自边界，见对应章节。

## 目录

- [开始使用](#开始使用)
- [打开文件与播放列表](#打开文件与播放列表)
- [播放、定位与循环](#播放定位与循环)
- [波形、频谱与选区](#波形频谱与选区)
- [编辑与撤销](#编辑与撤销)
- [保存、另存和导出](#保存另存和导出)
- [布局、外观与 Mag](#布局外观与-mag)
- [EQ、Limiter 与电平表](#eqlimiter-与电平表)
- [文件信息、多声道与 Ambisonics](#文件信息多声道与-ambisonics)
- [音频设备与输出路由](#音频设备与输出路由)
- [WEM、BNK 与视频中的音频](#wembnk-与视频中的音频)
- [偏好设置](#偏好设置)
- [恢复与本地诊断](#恢复与本地诊断)
- [快捷键](#快捷键)
- [菜单与命令面板索引](#菜单与命令面板索引)
- [命令行工具](#命令行工具)
- [常见问题与当前限制](#常见问题与当前限制)
- [维护与行为来源](#维护与行为来源)

## 开始使用

### 运行程序

便携版请将 ZIP 完整解压到固定目录，再运行 `Single Editor.exe`。保持 EXE、运行库、图标和许可证等文件的相对位置；仅复制 EXE 可能导致部分功能不可用。

`SingleEditorCLI.exe` 是独立命令行工具，平常使用图形界面时不用打开它。

若使用安装版，请按安装向导操作。Windows 的默认打开程序仍由系统管理，可以在文件的“打开方式”或系统“默认应用”中选择 SE。便携版选定为默认程序之后，不要随意移动目录。

### 第一次完成一段编辑

1. 点击 **Open Audio**，选择 WAV、FLAC 或其他支持的音频。
2. 按 **Space** 播放，再按一次暂停。单击波形可跳到指定位置。
3. 在波形上按住左键横向拖动，创建选区。
4. 在选区浮层中调节增益，点击 **Apply** 提交；或在 **More → Edit** 中执行淡入、淡出。
5. 按 **Ctrl+Z** 可以撤销。
6. 首次练习建议用 **Ctrl+Shift+S / Save As** 保存为新 WAV，重新打开检查结果。

打开较大文件时，播放与波形可能分阶段就绪。状态栏会说明当前进度；编辑功能需等待可编辑文档准备完成。

## 打开文件与播放列表

### 打开方式

| 入口 | 操作 |
|---|---|
| 工具栏 | 点击 **Open Audio** |
| 快捷键 | **Ctrl+O** |
| 拖放 | 把文件从资源管理器拖到 SE 窗口 |
| 最近文件 | **More → File → Recent**，或标题栏 **File → Open Recent** |
| 系统默认程序 | 在 Windows 中把 SE 设为相应格式的默认打开程序 |

打开对话框支持一次选择多个文件。文件进入播放列表，当前项显示在波形区域。某个扩展名出现在文件列表中，并不保证其中每一种编码都能解码；错误会在当前操作状态中说明。

普通格式以 WAV、AIFF、FLAC、Ogg Vorbis、MP3 为主；特殊游戏格式、系统媒体格式和视频音频取决于对应解码能力。

### 使用播放列表

- 单击某一行或选中后按 **Enter**，打开该项。
- **Page Up / Page Down** 切换上一项、下一项。
- 开启 **Auto Play** 后，激活音频项会从头自动播放；关闭时只打开。
- 右键条目可选 **Open**、**Reveal in Explorer**、**Move Up**、**Move Down**、**Remove from Playlist**。
- **Remove** 和 **Remove from Playlist** 只移除列表条目，不删除磁盘文件。
- 恢复条目的资源管理器命令会显示为 **Reveal Original in Explorer**，指向原始来源。

切换文件、移除当前项或退出时，SE 会按当前文档状态处理未保存编辑。看到保存提示时，应明确选择保存、放弃或取消。不要把恢复记录当作主动保存的替代品。

**Preferences → Restore last playlist on launch** 控制是否在启动时恢复上次播放列表，默认关闭。**Keep audio items loaded while switching** 与 **Unload inactive audio to save memory** 控制非活动音频是否继续驻留内存。

## 播放、定位与循环

| 控件 / 命令 | 行为 |
|---|---|
| **Play / Space** | 从当前光标播放；到达文件末端后再次播放会回到开头 |
| **Pause / Space** | 暂停并保留位置 |
| **Stop** | 停止；返回位置受 RTS 控制 |
| **Start / Home / Go to Start** | 定位到文件开头 |
| **Previous Frame / Left** | 向前移动一个源采样帧 |
| **Next Frame / Right** | 向后移动一个源采样帧 |
| **RTS / Return to Start on Stop** | 停止后返回本次播放起点；关闭时停在当前位置 |
| **Loop / Loop Playback** | 到达文件末尾后返回本次播放起点继续播放 |
| **Auto Play** | 点击打开音频、事件或 BNK 媒体时自动从头播放 |

这里 **Frame 是音频采样帧**：48 kHz 音频一步约为 1/48000 秒。界面 `时:分:秒:帧` 时间码按 30 fps 显示，两者不要混淆。

普通 **Loop** 循环的是“播放起点到文件末尾”。拖出选区不会自动建立选区循环。增益、EQ、Limiter 的 **Preview** 有独立的处理预览范围，见编辑章节。

工具栏 **Monitor** 是监听音量，调节它只影响播放输出，不会更改保存的音频样本。

## 波形、频谱与选区

### 视图模式

- **Waveform**：查看振幅随时间的变化，并操作增益包络。
- **Spectral**：查看频谱随时间的变化。首次切换可能需要生成数据；它是查看模式，不是频谱绘画或频域修复工具。
- **Stacked Channels**：每个源声道独立显示。
- **Mixed Channels**：合并显示多个声道，便于观察整体轮廓；这不会把文件混成单声道。

可用波形面板上的模式按钮、选项菜单，或命令面板切换这些模式。

### 鼠标操作

| 操作 | 结果 |
|---|---|
| 在波形中单击 | 清除时间选区，并定位播放光标 |
| 左键横向拖动 | 建立时间选区；过短的拖动会按单击或无有效选区处理 |
| 滚轮上下滚动 | 以鼠标附近时间为中心水平缩放 |
| **Shift + 滚轮** | 放大或缩小显示振幅 |
| 水平滚轮 | 水平平移 |
| 鼠标中键拖动，或 **Alt + 左键拖动** | 平移当前可见时间范围 |
| 在 Waveform 中双击 | 插入包络点 |
| 在 Waveform 中右键空白位置 | 插入包络点 |
| 右键已有可移除包络点 | 删除该包络点 |
| 拖动已有包络点 | 调节时间位置和增益 |

底部概览条用于在整段音频中平移当前视窗，其缩放和重置按钮与主波形视图对应。

**Time − / R** 缩小时间轴，**Time + / T** 放大时间轴；**Amp − / Q** 缩小显示振幅，**Amp + / W** 放大显示振幅；**Reset View / E** 恢复视图。显示振幅缩放不会改变声音响度或保存内容。

### 时间选区与源声道选择

**Ctrl+A** 选中整段音频，并恢复所有源声道作为处理目标。

在分声道显示中，点击声道标签可选择该源声道和整个时间范围；按住 **Ctrl** 点击其他标签可以加入其他源声道。随后可再拖动波形调整时间范围。

增益、Parametric EQ 和 True-Peak Limiter 使用时间选区和源声道目标；没有限定源声道时按全部源声道处理。声道旁的 **M / S** 是监听静音和独奏，不等于编辑目标，也不会把未独奏声道从导出文件中删除。

## 编辑与撤销

编辑先发生在当前文档中，点击 **Save / Save As** 才写入相应磁盘文件。

### 选区增益

1. 建立时间选区，必要时先选择源声道。
2. 在浮层中拖动增益滑块，或点击 **−3 / +3** 每次调整 3 dB。GUI 的范围为 **−24 至 +24 dB**。
3. 调节时会更新处理预览和波形预览；点击 **Apply** 提交一次编辑。
4. **Cancel / Esc** 取消预览并收起浮层，保留时间选区。
5. 双击增益滑块可恢复 **0.0 dB**。

预览不等于已经提交，也不等于已经保存。应用后可撤销；增加增益过多可能导致峰值过高，应结合电平表试听。

### 淡入、淡出和删除

| 命令 | 操作与范围 |
|---|---|
| **Fade In** | 对所选时间段作线性淡入，从零逐渐到原电平 |
| **Fade Out** | 对所选时间段作线性淡出，从原电平逐渐到零 |
| **Delete Selection / Delete / Backspace** | 删除所选时间段并拼接前后内容，文件变短 |
| **Reset Envelope** | 重置增益包络，保留其他已提交编辑 |

这些命令可从 **More → Edit** 或命令面板找到。当前 **Fade In、Fade Out、Delete Selection 作用所有声道**；它们不采用增益/EQ/Limiter 的源声道筛选。删除覆盖整个文件的选区不会产生空音频文件。

### 增益包络

在 **Waveform** 模式插入、拖动和删除包络点，可改变不同时间位置的增益。包络会影响播放和保存；与单纯的 **Amp** 显示缩放不同。**Reset Envelope** 用于恢复包络。

### Undo / Redo

使用工具栏、标题栏 **Edit**、命令面板，或 **Ctrl+Z / Ctrl+Y**。撤销可以恢复已提交的编辑；在撤销后提交新编辑会形成新的编辑路径。保存不能代替磁盘备份，Undo 也不应被当作任意历史版本的永久存档。

当前 GUI 尚未提供复制、剪切、粘贴、多轨时间线和录音功能。

## 保存、另存和导出

### 三个命令的区别

| 命令 | 用途 | 当前文档之后指向哪里 |
|---|---|---|
| **Save / Ctrl+S** | 将编辑保存到当前来源；仅对可写来源可用 | 仍是当前文件 |
| **Save As / Ctrl+Shift+S** | 另存为指定路径或格式 | 成功后切换为新文件 |
| **Export Audio** | 输出当前编辑结果的一份副本 | 保持当前文档来源 |

**Export Audio** 可以在标题栏 **File** 或命令面板中找到。导出目标必须与当前来源不同；导出不是“保存当前文件”。当前导出的是文档音频结果，不是只输出拖选的片段，也不是设备监听路由的录制。

可写扩展名为 **WAV、AIFF/AIF、FLAC、Ogg Vorbis、MP3**。WEM、BNK 和视频容器没有原位回写功能；需要时把可编辑的解码音频另存为支持的格式。

### 编码选项

另存或导出到以下格式时，会出现对应选择，并记住选项供后续保存使用：

| 格式 | 可选项 |
|---|---|
| MP3 | 96、128、160、192、256、320 kbps |
| Ogg Vorbis | Quality 0–10 |
| FLAC | Compression Level 0–8 |

MP3 与 Ogg Vorbis 是有损编码。对它们执行原位保存会再次编码；SE 会在当前文档会话首次执行这类覆盖时提醒。希望避免重复有损编码时，可先另存 WAV 或 FLAC。

### 数据安全与元数据

- 保存期间以一份确定的编辑快照写入；若继续产生新编辑，状态栏可能提示“旧编辑已保存，较新修改仍需保存”。
- 若源文件在 SE 之外被修改，保存会提示 **Source File Changed**。**Reload** 放弃当前内存编辑并读取磁盘版本；**Save As** 保留编辑另存；**Cancel** 保持现状。
- 原格式不支持回写时，请直接使用 **Save As**，不要反复执行 Save。
- 转码和改变时间线可能转换或丢弃不兼容的元数据。SE 会按支持范围保留或转换可识别信息；不能承诺任意私有块、封面和未知标签逐字节不变。
- 删除时间段会影响标记、循环和时间相关元数据；检查保存状态提示，并重新打开重要结果。

**Monitor 音量、静音/独奏、声道解释和设备输出映射用于监听，不能替代写入音频的编辑操作。** 需要永久改变样本时，使用明确的编辑和处理器 **Apply**。

## 布局、外观与 Mag

### General 与 Simple

- **General**：完整工作区，保留当前面板布局。
- **Simple**：突出波形，暂时收起扩展工作区；播放、处理状态和 Mag 布局会保留。
- 在 Simple 中点击返回 General 的按钮，即可恢复完整工作区。

Simple 是同一文档的显示方式，不是另外打开一份文件，也不会因为切换视图自动提交预览或保存。

### Mag 是什么

Mag 是工作区中的一个功能面板，例如 Playlist、Metadata、Markers、Output Meter、Parametric EQ 或 True-Peak Limiter。当前版本使用内置面板；这不代表已经提供第三方动态插件或热插拔解码器接口。

在 Modern 工作区，从 **More → Mag** 集中打开和关闭 Playlist、File Information、Markers、Output Meter、Parametric EQ、True-Peak Limiter、Ambisonic Energy 和 Rendered Output Spatial Energy。勾选表示已开启；当前文件不支持的面板置灰。关闭后可从同一菜单重新打开，**Reset Mag Layout** 恢复布局。工具栏、经典菜单 **View → Modules** 和命令面板也保留对应入口。

拖动面板标题或标签可调整停靠位置；出现放置预览时释放。拖动分隔线可调整分区大小，**Esc** 可取消尚未提交的拖动。

- **Float …**：把面板移到独立浮动窗口。
- **Redock …**：把面板放回主工作区。
- 面板关闭按钮仅关闭该面板，可以重新打开。
- **Ctrl+Tab / Ctrl+Shift+Tab** 切换工作区的相邻标签。
- **Reset Module Layout** 恢复面板布局；不应把它当作清除音频编辑的命令。

### 外观

**More → Appearance** 提供：

- **Modern Glass – Light / Night**：明亮或深色主题。
- **Solid – Reduce transparency**：实体背景。
- **Frosted glass**：磨砂玻璃。
- **Liquid glass**：液态玻璃。

**Preferences → Reduce Motion** 可以改用直接状态切换。**Modern** 是当前发布界面；**Classic** 标为工程预览，布局和操作呈现可能不同。本文以 Modern 为主。

## EQ、Limiter 与电平表

### 共用工作方式

1. 打开相应 Mag。
2. 检查时间选区和源声道目标。没有时间限制时处理器按整段范围工作。
3. 调节参数，点击 **Preview** 试听；再次点击 **Stop Preview** 停止预览。
4. 点击 **Apply** 才把结果提交为文档编辑，然后按需要保存。
5. 使用 **Undo** 撤销已应用结果。

预览阶段不写入源文件。处理进行中，部分相冲突的编辑会暂时不可用。面板显示的不可用原因比仅看按钮是否灰色更有用。

### Parametric EQ

从 **More → Audio → Parametric EQ**、**View → Modules** 或命令面板打开。

- **Simple / Advanced** 切换简化与详细参数呈现。
- **S / M / L** 调节面板尺寸。
- 预设包括 **Flat、Vocal Clarity、Bass Control**。
- 八个频段可分别启用，滤波类型包括 High Pass、Low Shelf、Bell、Notch、High Shelf、Low Pass。
- 按类型调节 **Frequency、Gain、Q、Slope**；界面会限制或收起不适用的参数。
- 频率输入支持 `k` 表示千 Hz；例如 `2k` 表示 2000 Hz。
- 图形曲线表示当前参数的响应，实际结果应通过 Preview 和 Apply 后试听确认。

### True-Peak Limiter

从 **More → Audio → True-Peak Limiter**、**View → Modules** 或命令面板打开。

| 控件 | 含义与范围 |
|---|---|
| **On / Off** | 处理器开关 |
| **Threshold** | 阈值，−36 至 0 dB |
| **Ceiling** | 输出真峰值上限，−12 至 0 dBTP |
| **Release** | Auto 或 Manual；手动 20–2000 ms |
| **Last Applied** | 取回上次应用的参数 |
| **Preview / Apply** | 分别试听与提交 |

当前处理器固定 **5 ms lookahead**、按采样率选择真峰值检测方式、联动声道，并使用安全余量。TP/GR 数值来自实际预览或处理；静态图形不等于已经测得的输出峰值。

### Output Meter 与空间面板

- **Output Meter** 观察当前设备的各输出通道，可选 **TP、RMS、LUFS**，峰值保持可通过面板重置按钮清除。
- TP 表示真峰值，RMS 表示能量平均，当前 LUFS 显示全局瞬时响度，不是整首节目综合响度报告。
- 工具栏 RMS 是包括静音输出在内的所有设备通道功率平均；Output 面板的 RMS 读数显示最响通道，因此两处数值可能不同。
- **Ambisonic Energy** 用于 Ambisonic 声场观察。
- **Rendered Output Spatial Energy** 观察实际渲染输出的空间能量分布，与源音频布局和输出配置有关。

这些面板用于观察，打开它们不会自动把音频规范化或导出报告。

## 文件信息、多声道与 Ambisonics

**File information / Metadata** 显示当前文件的时长、采样率、位深、声道和可识别元数据。**Detailed metadata…** 展开详细信息；它是查看器，当前没有通用标签编辑器承诺。

**Markers** 面板显示当前播放位置、选区边界和可识别 WAV cue 标记。点击标记行或选中后按 **Enter** 可定位。当前面板以查看、跳转为主，不能据此认为已有任意标记新增、重命名和写回操作。

### Channel monitor

在 Metadata 中展开 **Channel monitor…**：

| 设置 | 作用 |
|---|---|
| **Source Channel Order** | 解释源文件各声道对应的扬声器角色 |
| **Auto device** | 根据当前设备自动选择监听方式 |
| **Multichannel** | 多声道监听 |
| **Stereo downmix** | 立体声下混监听 |
| **Include LFE in stereo downmix** | 是否在立体声下混中加入 LFE，默认关闭 |
| **Output mapping…** | 把语义扬声器角色映射到物理输出 |
| **Ping** | 通过当前路由播放所选扬声器角色的低电平测试信号 |

源声道解释、语义角色和声卡物理输出是三个不同概念。比如把源文件解释为 5.1，不会自动证明你的六根输出线已经接到相应扬声器。更改默认源声道顺序也不会重排设备端口。

以带标准 WAVE channel mask 的 7.1 文件为例，其原始声道顺序是“前方四路、后方一对、侧面一对”。SE 的 7.1 SMPTE 源预设则把侧面一对放在后方一对之前：

| 源文件声道 | WAVE mask 解释 | SE 的 7.1 SMPTE 解释 |
|---|---|---|
| CH1–4 | L、R、C、LFE | L、R、C、LFE |
| CH5 | Lsr，左后 | Lss，左侧环绕 |
| CH6 | Rsr，右后 | Rss，右侧环绕 |
| CH7 | Lss，左侧环绕 | Lsr，左后 |
| CH8 | Rss，右侧环绕 | Rsr，右后 |

**CH5 是文件里的第五条音轨，不天然等于“左侧”或“左后”。** 源预设为每条音轨指定角色，播放路由再把角色送到设备端口。切换源预设会同时改变标签和监听路由，不会交换文件中的样本。文件自带可信 WAVE mask 时，保留该解释；不要用源预设补偿接线或设备输出顺序问题。Windows/WAVE 默认输出的后方一对也在侧面一对之前。语义 **Ping Lsr** 应始终检查左后输出，不能因为源预设变化就变成左侧。

通道映射对话框中，选择相应连接后用 **Apply** 应用；**Default** 恢复默认，**Cancel** 放弃本次更改。测试前先降低监听音量，按实际接线确认。

### Ambisonics

对可识别的 Ambisonic 音频，Channel monitor 提供 **Speaker decode** 与 **Binaural / HRTF**。HRTF 首次使用需要准备数据，期间会显示 **Preparing HRTF…**。

方向控制可调整 **Yaw、Pitch、Roll**，**Reset** 恢复方向。这些是监听解码设置，不是把音频永久旋转并回写的命令。能否正确解释文件仍取决于其格式、阶数、声道约定和元数据。

## 音频设备与输出路由

按 **Ctrl+,** 打开 Preferences。普通设备设置包括驱动类型、输出设备、采样率和缓冲区大小；点击 **Refresh** 重新读取设备。

**Output Setup…** 打开详细的 **Audio output routing**。它支持 ASIO 与列出的 Windows Audio 后端。是否可用及支持的采样率、缓冲和输出数量由实际驱动决定。

### 配置一条明确的输出路由

1. 选择后端和输出设备，必要时点击 **Refresh devices**。
2. 点击 **Read capabilities**，读取该设备实际能力。
3. 选择标准扬声器布局、采样率和缓冲区。
4. 在矩阵中连接语义扬声器行与物理输出列。
5. 点击 **Validate**，检查警告；只有理解并接受列出的警告后，才勾选确认。
6. 可选择物理输出并点击 **Ping**，核对接线。
7. 点击 **Activate** 启用。仅修改草稿不会改变正在使用的路由。

**Choose Windows default** 选中当前 Windows 默认共享输出，之后仍需读取能力并确认路由。**ASIO Control Panel** 打开相应驱动面板；在驱动面板更改后应重新读取能力。

**Retry output** 用于重试保存的设备选择。设备失效或不支持所选配置时，查看状态说明；不要以为 SE 已经切到了另一个相同名称设备。

**Follow Source Sample Rate (expert)** 默认关闭，需先激活路由。开启后，源采样率变化会请求设备切换到对应采样率；不支持时回到先前输出，既有缓冲和路由继续按确认配置处理。

界面显示的聚合播放延迟来自驱动报告和已知处理延迟，不是扬声器到麦克风的实测往返延迟。README 中的启动就绪时间也不是按下 Play 到听到声音的时延。

### HDMI AVR 与 7.1.4

读取 12 声道文件、通过 12 路声卡输出，以及经 HDMI 向 AVR 发送 Dolby Atmos，是三种不同能力。当前 SE 使用普通 Windows Audio / ASIO 后端，尚未接入 Microsoft Spatial Sound 的 Atmos 输出，也不提供 Atmos 位流直通。Windows 中开启 Dolby Atmos for home theater，不会让 SE 的普通 PCM 后端自动获得独立顶部声道输出。

HDMI AVR 测试应先读取实际设备能力，使用当前可用的输出声道；不要强行将 8 路设备设为 12 路。7.1.4 PCM 文件可以用于检查文件读取和声道标签，完整的 12 路物理监听需要可用的 12 路设备后端。AVR 上混产生顶部声音，也不等于 SE 已独立发送四个顶部声道。微软区分普通 WASAPI 与空间音频对象输出，见 [Microsoft Spatial Sound](https://learn.microsoft.com/en-us/windows/win32/coreaudio/spatial-sound)。

## WEM、BNK 与视频中的音频

### WEM

WEM 是 Wwise 媒体容器，内部编码可能不同。直接打开可解码的 WEM 可预览播放；成功打开某一个 WEM 不代表全部 WEM 编码都受支持。需要保留编辑结果时，使用支持的音频格式另存，不能覆盖回 WEM。

### BNK

打开 BNK 后，可使用：

- **Events**：查看可识别事件，选择事件准备预览，播放操作请求事件预览。
- **Audio**：查看原始媒体，选择或播放可解码媒体。
- **Linked Only**：只显示与当前事件关联的原始媒体。
- **Switch / State**：选择已识别的上下文值，更新相应事件预览。
- **Attach Project**：指定可用的 Wwise 项目根目录，辅助源媒体和关联信息查找。

偏好中的 **Decode packed media** 优先使用包内媒体；**Prefer source media when available** 在可用时优先源素材。还可指定默认 Wwise 项目根目录。

v0.1 的 BNK 原始媒体读取和播放已经过本地验证；部分事件关联可能显示 **Unavailable / Partial**。这不等于 Audio 页中的媒体无法播放，也不等于 SE 完整复现 Wwise 的全部运行时事件行为。关闭 Linked Only 可检查全部可见媒体。

对于预算、解码器或依赖问题，失败对话框可能提供 **Retry once**、**Retry with Expanded Budget**、**Export diagnostics…**、**Keep current audio**。只有理解显示的额外资源需求后再扩大预算；反复失败时先导出诊断。失败或取消不会主动用半成品事件替换当前音频。

BNK 编辑与回写、完整游戏混音、所有 RTPC 和事件逻辑重建不属于当前操作承诺。

### G.719 的边界

当前分发解码器已关闭 **G.719**，因此使用该编码的 **BNSF IS22 / Siren 22** 不能解码。**Siren 14 / G.722.1** 是另一种编码，并未因此一并关闭。

这一限制不等于关闭 Wwise 的 BNK/WEM 功能；常见可支持编码的 BNK/WEM 读取与播放保留。当前没有用户可安装的 G.719 Mag 扩展包，也不要用未知来源的 DLL 覆盖分发运行库。

### 视频中的音频

SE 有从 MP4、M4V、MOV、AVI、WMV、MKV、WebM 提取音频的可选路径，需要本机有可用的 **FFmpeg 可执行程序 `ffmpeg.exe`**。当前便携包没有附带这个程序；vgmstream 使用的 FFmpeg DLL 不等于独立 FFmpeg 命令。SE 会检查应用目录、约定的媒体运行库目录及 PATH，因此该功能需自行提供可找到的 FFmpeg，不能按开箱即用功能理解。

当前提取**第一条音轨**，供音频预览和编辑；不显示视频画面、不提供音轨选择器、不把结果回写视频。使用 **Save As / Export Audio** 输出编辑后的音频。

准备过程中可以 **Cancel video audio**；失败时按提示保留当前音频、重试或导出诊断。

## 偏好设置

| 设置 | 用途 |
|---|---|
| **Window Close Behavior** | 关闭窗口退出程序，或隐藏到系统托盘；默认建议退出 |
| **Renderer** | Modern 发布界面，或 Classic 工程预览 |
| **Reduce Motion** | 标准过渡，或直接状态变化 |
| **Restore last playlist on launch** | 是否启动时恢复上次列表，默认关闭 |
| **Retain Playlist Audio** | 切换时保留音频，或卸载非活动音频节省内存 |
| **BNK / WEM playback mode** | 解码包内媒体，或可用时优先源媒体 |
| **Default Wwise project root** | 默认项目来源；Choose 选择、Clear 清除 |
| **Default Source Channel Order** | 4、6、8、12 声道文件的默认解释约定 |
| **Audio Device** | 驱动、设备、采样率、缓冲和详细 Output Setup |
| **Waveform Cache** | 显示缓存位置与占用；Open Folder 打开，Clear Cache 清理 |

清理波形缓存影响之后的波形生成时间，不会删除原始音频。恢复存储与波形缓存是不同内容，恢复清理由 Recovery Center 管理。

## 恢复与本地诊断

### Recovery Center

从 **More → Recovery Center…**、**Help → Recovery Center** 或命令面板打开。恢复用于异常中断后的工作找回，不等于每次编辑都已写入源文件。主窗口底部不再常驻恢复状态栏；有恢复记录、保护或清理任务时，在 More 中查看恢复状态，**Recovery Actions…** 提供重试、另存和诊断入口。

1. **Refresh** 读取恢复记录，**Next page** 查看后续条目。
2. 选择记录，点击 **Safe Inspect** 查看摘要和尝试历史；它不会开始播放或直接恢复音频。
3. **Check original** 检查原始来源；来源移动时用 **Locate original…** 指定位置。
4. 按需使用 **Deep Verify source** 检查原文件，或 **Deep Verify audio** 检查已保存恢复音频及撤销/重做历史。两者验证对象不同。
5. 选择恢复点，点击 **Restore Copy**。查看恢复任务状态，必要时 **Cancel restore / Retry restore**。
6. 恢复副本需要主动 **Save As** 到自己的音频路径。

**Later** 关闭中心并保留记录；已启动的后台恢复或清理任务可能继续。**Clear finished task** 清除任务显示，不等于删除原始音频。

**Storage → Refresh usage** 显示恢复存储使用情况；**Retention…** 调整非活动崩溃恢复记录的保留策略。**Review expiry…** 和 **Review Discard** 先审阅，**Discard…** 再确认丢弃恢复记录。活动记录受到保护。清理失败时按状态使用 **Retry cleanup / Resume cleanup**。

恢复保护暂停或容量不足时，从 More 查看恢复状态并处理；保存冲突按保存对话框提示处理。需要保留的编辑应尽快另存。检查或恢复失败可用 **Export diagnostics… / Task diagnostics…** 提供对应记录。

### Local Diagnostics

从 **Help → Local Diagnostics**、命令面板，或有关失败对话框进入：

1. 保留默认隐私选项，点击 **Refresh snapshot** 收集当前状态。
2. 阅读预览，按需选择 Audio device、Spatial、Media、Cache、Recovery 节。
3. 只有确实需要时才勾选 **Include file path / file name / device name**。
4. **Choose reproduction files…** 明确附加复现文件；它们可能包含完整音频或项目内容，先检查文件清单。
5. 点击 **Save reviewed bundle…**，把审核后的 ZIP 保存到本地。

诊断工具不会自动上传文件。默认状态采集不等于运行解码器或播放测试声音。**Probe audio… / Probe device…** 是单独的主动检查，需要停止当前播放和扬声器测试，并确认显示的作用；设备测试可能暂时释放当前输出，**Abort probe** 可取消。**Current state** 返回普通状态。

ZIP 保存后，再由你决定是否分享。工具中不可用的节或检查会说明原因，不能把“没有采集”理解为“已经通过”。

## 快捷键

以下为 Windows 当前实现。文字输入框、列表和弹窗会优先处理各自按键；快捷键不是系统全局热键。

| 按键 | 功能 |
|---|---|
| **Ctrl+O** | Open Audio |
| **Ctrl+S** | Save |
| **Ctrl+Shift+S** | Save As |
| **Ctrl+Z / Ctrl+Y** | Undo / Redo |
| **Ctrl+A** | 全文件、全部源声道选择 |
| **Delete / Backspace** | 删除时间选区 |
| **Space** | Play / Pause |
| **Home** | 到文件开头 |
| **Left / Right** | 前后移动一个源采样帧 |
| **Page Up / Page Down** | 上一 / 下一播放列表项 |
| **R / T** | 时间轴缩小 / 放大 |
| **Q / W** | 显示振幅缩小 / 放大 |
| **E** | 重置波形视图 |
| **Ctrl+,** | Preferences |
| **Ctrl+Shift+P** | Command Palette |
| **Alt+V** | 打开 View 菜单 |
| **F6 / Shift+F6** | 前后轮换主要界面区域焦点 |
| **Ctrl+Tab / Ctrl+Shift+Tab** | 工作区相邻标签 |
| **Ctrl+W** | 关闭窗口，按关闭行为设置处理 |
| **Esc** | 关闭命令面板、取消当前未提交编辑预览或面板拖动；具体取决于当前上下文 |

命令面板中输入命令名，使用 **Up / Down** 选择，**Enter** 执行，**Esc** 关闭。不可用命令会显示原因。

## 菜单与命令面板索引

**More** 汇集 File、Edit、View、Audio、Appearance，以及 **Command Palette…**。标题栏菜单和命令面板还提供下列入口；某些功能需要先打开文件或相关面板。

| 类别 | 命令 |
|---|---|
| 文件 | Open Audio、Open Recent / Recent、Save、Save As、Export Audio、Remove Current Playlist Item、Preferences |
| 编辑 | Undo、Redo、Delete Selection、Fade In、Fade Out、Reset Envelope |
| 播放 | Play or Pause、Play、Pause、Stop、Go to Start、Previous Frame、Next Frame、Return to Start on Stop、Loop Playback、Auto Play |
| 列表 | Previous Playlist Item、Next Playlist Item、Remove Current Playlist Item |
| 波形 | Waveform View、Spectral View、Stacked Channels、Mixed Channels、Zoom Time In / Out、Zoom Amplitude In / Out、Reset Waveform View |
| 工作区 | General Workspace Scene、Simple Waveform View、Toggle Playlist Sidebar、Toggle Metadata Sidebar、Toggle Markers、Reset Module Layout |
| 辅助面板 | Toggle Output Meter、Toggle Ambisonic Energy、Toggle Rendered Output Spatial Energy、Toggle Parametric EQ、Toggle True-Peak Limiter |
| 浮动 / 回停靠 | Float / Redock Audio Waveform、Output Meter、Ambisonic Energy、Spatial Energy、Parametric EQ、True-Peak Limiter、Playlist、Metadata、Markers |
| 窗口 | Minimize Window、Toggle Maximize、Close Window |
| 帮助 | Recovery Center、Local Diagnostics、About Single Editor |

用命令面板寻找未出现在当前紧凑工具栏中的功能即可；不必为了找到命令重置布局。

## 命令行工具

命令在 PowerShell 中运行。以下示例先进入包含 CLI 的程序目录，示例中的 `D:\Audio`、`D:\SE-work` 请换成自己的路径。输出目录应先存在，输入与输出文件使用完整路径最清楚。

### 查询能力

```powershell
.\SingleEditorCLI.exe operations --json
.\SingleEditorCLI.exe discover --jsonl
.\SingleEditorCLI.exe diagnostics --help
```

当前没有统一的 `SingleEditorCLI.exe --help`。`operations` 列出基础检查/编辑流水线能力，`discover` 列出 AudioSuite、计划协议和批处理能力。**`--json` 与 `--jsonl` 对应不同命令族，不能互换。**

### 基础检查与分析（JSON 命令族）

```powershell
.\SingleEditorCLI.exe inspect --input "D:\Audio\input.wav" --json
.\SingleEditorCLI.exe inspect --input "D:\Audio\input.wav" --sample-stats --json
.\SingleEditorCLI.exe validate --input "D:\Audio\input.wav" --json
.\SingleEditorCLI.exe analyze trim-silence --input "D:\Audio\input.wav" --head-only --threshold-dbfs -60 --plan "D:\SE-work\trim-plan.json" --json
.\SingleEditorCLI.exe analyze gain --input "D:\Audio\input.wav" --gain-db -3 --plan "D:\SE-work\gain-plan.json" --json
.\SingleEditorCLI.exe analyze fade --input "D:\Audio\input.wav" --fade-in-seconds 0.1 --fade-out-seconds 0.2 --plan "D:\SE-work\fade-plan.json" --json
.\SingleEditorCLI.exe analyze normalize-peak --input "D:\Audio\input.wav" --target-dbfs -1 --plan "D:\SE-work\normalize-plan.json" --json
```

`inspect` 读取信息；`--sample-stats` 还扫描样本统计。`validate` 是文件检查，不是人的听音验收。`analyze` 生成分析和计划，不修改音频。可以重复 `--input` 分析多个输入。

| 分析操作 | 参数 |
|---|---|
| `trim-silence` / `trim-silence.v1` | 开头静音分析；`--threshold-dbfs` −160 至 0，默认 −60；`--head-only` 明确只处理开头 |
| `gain` / `gain.v1` | `--gain-db` 必填，−120 至 +24 |
| `fade` / `fade.v1` | 至少一个 `--fade-in-seconds`、`--fade-out-seconds`，非负秒数 |
| `normalize-peak` / `normalize.peak.v1` | `--target-dbfs` −60 至 0，默认 −1 |
| `pipeline` / `edit-pipeline.v1` | `--pipeline <JSON文件>` 按顺序分析编辑流水线 |

流水线文件示例：

```json
{
  "schema": "single-editor.edit-pipeline.v1",
  "schemaVersion": 1,
  "operations": [
    { "id": "gain.v1", "gainDb": -3 },
    { "id": "fade.v1", "direction": "in", "durationSeconds": 0.1 },
    { "id": "normalize.peak.v1", "targetDbfs": -1 }
  ]
}
```

```powershell
.\SingleEditorCLI.exe analyze pipeline --input "D:\Audio\input.wav" --pipeline "D:\SE-work\pipeline.json" --plan "D:\SE-work\plan.json" --json
.\SingleEditorCLI.exe apply --plan "D:\SE-work\plan.json" --output-dir "D:\SE-work\output" --dry-run --json
.\SingleEditorCLI.exe apply --plan "D:\SE-work\plan.json" --output-dir "D:\SE-work\output" --on-conflict fail --manifest "D:\SE-work\result.json" --json
```

基础 `apply` 必须指定 `--plan`，并在 `--in-place` 与 `--output-dir` 中二选一。`--dry-run` 只做预检查，不写音频。可用写入选项：

| 参数 | 用途 |
|---|---|
| `--on-conflict fail\|overwrite\|skip\|rename` | 输出已存在时失败、覆盖、跳过或改名；默认 fail |
| `--backup` | 请求备份 |
| `--backup-dir <目录>` | 指定备份目录 |
| `--allow-lossy-reencode` | 明确允许有损重编码 |
| `--fail-fast` | 遇到失败停止后续项目 |
| `--manifest <文件>` | 保存执行清单 |

先检查 dry-run 和计划的结果，再执行原位或覆盖操作。计划与来源的绑定用于检测变化；旧计划不应套用到已改变的文件。

### 路由检查

```powershell
.\SingleEditorCLI.exe routing --input "D:\Audio\surround.wav" --output-channels 2 --monitor-mode stereo --json
```

| 参数 | 范围 / 含义 |
|---|---|
| `--output-channels` | 必填：1、2、4、6、8、12 |
| `--source-preset` | 源声道顺序预设 ID，默认自动解释 |
| `--include-lfe` | 立体声下混包含 LFE |
| `--output-map Role=OutputChannel` | 可重复，语义角色到物理输出映射 |
| `--mute-channel` / `--solo-channel` | 可重复，**从 1 开始**的源声道号，1–32 |
| `--monitor-mode` | auto、multichannel、stereo |
| `--ambisonic-monitor` | speaker 或 binaural |
| `--ambisonic-yaw` / `--ambisonic-roll` | −180 至 180 度 |
| `--ambisonic-pitch` | −90 至 90 度 |
| `--runtime-probe` | 运行相应渲染探测，不是物理扬声器接线验收 |
| `--probe-start-seconds` | 探测起点，非负秒数 |

### AudioSuite 单文件处理（JSONL 命令族）

此命令族当前只接受 **WAV → WAV**，保持采样率和声道数。操作 ID 为：

- `singleeditor.reference-gain`
- `singleeditor.parametric-eq-v1`
- `singleeditor.true-peak-limiter-v1`

```powershell
.\SingleEditorCLI.exe analyze singleeditor.reference-gain --input "D:\Audio\input.wav" --gain-db -3 --destination "D:\SE-work\gain.wav" --destination-mode create_new --plan "D:\SE-work\audiosuite-plan.json" --jsonl
.\SingleEditorCLI.exe apply --plan "D:\SE-work\audiosuite-plan.json" --jsonl
```

| analyze 参数 | 说明 |
|---|---|
| `--jsonl` | 必填 |
| `--input` | 恰好一个输入 |
| `--plan` | 必填计划文件 |
| `--destination-mode` | create_new（默认）、in_place、overwrite_existing |
| `--destination` | 新建或覆盖现有文件时必填；原位时可省略 |
| `--start-sample` 与 `--end-sample` | 必须成对；半开区间 `[start, end)`，不指定时为整段 |
| `--channel` | 可重复，**从 0 开始**的源声道索引，不指定时为全部；不允许重复值 |
| `--gain-db` / `--parameters` | Gain 二选一；EQ / Limiter 必须提供参数 JSON |

AudioSuite `apply` 使用计划的目的地，不接受基础命令族的 `--output-dir`。原位计划需加 **`--allow-in-place`**，覆盖现有文件计划需加 **`--allow-overwrite-existing`**；新建不需要这两个标志。

EQ 的参数 JSON 包含 `preset_id` 和恰好八个 `bands`，每个频段依次包含 `index`（0–7）、`enabled`、`type`、`frequency_hz`、`gain_db`、`q`、`slope`。类型取 `high-pass / low-shelf / bell / notch / high-shelf / low-pass`，斜率取 `db12 / db24`。`preset_id` 只是参数标识，不能只写预设名省略八段参数。

可以用 PowerShell 生成一个平直 EQ 参数文件，再修改需要的频段：

```powershell
$eqBands = @(0..7 | ForEach-Object {
    @{ index = $_; enabled = $false; type = 'bell'; frequency_hz = 1000; gain_db = 0; q = 0.707; slope = 'db12' }
})
@{ preset_id = 'flat'; bands = $eqBands } | ConvertTo-Json -Depth 5 | Set-Content -Encoding utf8 "D:\SE-work\eq.json"
.\SingleEditorCLI.exe analyze singleeditor.parametric-eq-v1 --input "D:\Audio\input.wav" --parameters "D:\SE-work\eq.json" --destination "D:\SE-work\eq.wav" --plan "D:\SE-work\eq-plan.json" --jsonl
```

Limiter 参数文件示例；当前契约要求 enabled 和 channels_linked 为 true、lookahead 固定 5 ms、oversampling 固定为下列值：

```json
{
  "enabled": true,
  "threshold_db": -3,
  "ceiling_dbtp": -1,
  "lookahead_ms": 5,
  "release_mode": "automatic",
  "release_ms": 180,
  "oversampling": "sample-rate-aware-fixed",
  "channels_linked": true
}
```

```powershell
.\SingleEditorCLI.exe analyze singleeditor.true-peak-limiter-v1 --input "D:\Audio\input.wav" --parameters "D:\SE-work\limiter.json" --destination "D:\SE-work\limited.wav" --plan "D:\SE-work\limiter-plan.json" --jsonl
```

### AudioSuite 批处理

先为每个文件生成单文件计划，再组成批计划：

```powershell
.\SingleEditorCLI.exe batch-plan --output "D:\SE-work\batch.json" --item first "D:\SE-work\eq-plan.json" --item second "D:\SE-work\limiter-plan.json" --jsonl
.\SingleEditorCLI.exe batch-apply --batch-plan "D:\SE-work\batch.json" --evidence "D:\SE-work\batch-result.json" --jsonl
```

- 一批最多 256 项，逐文件串行执行。
- `--evidence` 保存进度与结果；重试用同一批计划和对应结果文件加 **`--resume`**。
- `--cancel-file <路径>` 指定取消标记文件，取消在文件边界生效，不意味着中途回滚已经完成的文件。
- 原位/覆盖项仍需相应的 `--allow-in-place / --allow-overwrite-existing`。
- 已执行后不要手动修改计划或结果文件再继续；源和计划不一致会产生冲突。

### 诊断 CLI

```powershell
.\SingleEditorCLI.exe diagnostics validate --bundle "D:\SE-work\diagnostics.zip" --jsonl
.\SingleEditorCLI.exe diagnostics export --output "D:\SE-work\new-diagnostics.zip" --jsonl
.\SingleEditorCLI.exe diagnostics probe --codec "D:\Audio\input.wem" --active-probe --jsonl
```

`validate` 只验证诊断包，不解压。`export` 默认只观察该 CLI 进程，不读取正在运行的 GUI 状态。主动 probe 和导出都要求先阅读预览，再向 stdin 输入显示的精确确认 ID；不要用固定的 `yes` 跳过审核。

设备检查格式：

```text
diagnostics probe --device <准确后端名称> --active-probe
  [--output-device <准确设备名>] [--sample-rate <Hz>]
  [--buffer-size <samples>] [--output-channels <1..32>]
  [--ping-channel <1..输出数>] [--include-device-name] [--jsonl]
```

不指定 ping 时是静音回调检查；指定 ping 会发出 **250 ms、−40 dBFS、100 Hz** 的测试声，并要求单独确认。采样率和缓冲默认 0，表示后端策略；输出数默认 2。`diagnostics export` 也可使用同样设备选项，或使用 `--codec <路径> --active-probe`，不能同时请求两种探测。

导出可选 `--include-path`、`--include-file-name`。要包含完整复现文件，使用 **`--include-reproduction --attach <绝对路径>`**，`--attach` 可重复，需要额外内容确认。复现附件不会被当作可信指令或自动解析执行。

### 返回码

| 返回码 | 含义 |
|---|---|
| 0 | 成功 |
| 2 | 参数错误 |
| 3 | 验证失败或不支持 |
| 4 | 冲突或输入已经变化 |
| 5 | 执行 / 处理失败 |
| 6 | 取消；基础批处理也可能表示部分失败 |
| 7 | 内部错误 |

自动化时同时检查返回码与最后的结果记录。JSONL 中间事件不是最终成功；不能仅凭出现输出文件就判定所有项目完成。

## 常见问题与当前限制

**没有声音。** 先检查 Monitor、源声道 M/S、实际设备是否就绪，再看 Preferences 与 Output Setup 的输出路由；不要把波形显示正常视为物理音频已经送到扬声器。

**波形还在增加。** 大文件可能先获得可播放范围，再逐渐补充波形。未准备区域的定位或编辑可能受限；查看当前状态，等待完整可编辑文档。

**保存后仍提示未保存。** 可能保存的是较早快照，而之后又产生新编辑；再次保存最新状态。恢复副本应 Save As；只读来源不能原位保存。

**选了声道却其他声道也被淡入或删除。** 当前 Fade In、Fade Out 和 Delete Selection 是全声道时间编辑；只有增益/EQ/Limiter 使用源声道选择。

**BNK 没有可用事件。** 先到 Audio 页查看原始媒体，关闭 Linked Only；补充可用 Wwise 项目或关联资料。Unavailable/Partial 不能当作完整事件重建成功。

**G.719 文件打不开。** 当前分发不含该解码能力。BNSF IS22/Siren 22 受影响，Siren 14 与常见其他 BNK/WEM 编码不因此取消。

**视频打不开。** 检查 FFmpeg 是否可用，以及第一条音轨是否受支持。当前只处理音频，不是视频编辑器。

**“小于 200 ms”意味着什么。** 项目的本机启动测试针对指定文件、设置和系统状态下的核心就绪点。它不等于所有电脑的冷启动、所有格式完整加载、按下 Play 到扬声器出声均低于 200 ms。日常表现也会受设备驱动、文件和系统状态影响。

当前以 Windows x64 为目标，硬件 ASIO、多声道实际接线与听音需在自己的设备验证。CLI 与 GUI 的能力及参数范围并不完全相同。多轨 DAW、录音、通用标签编辑、视频音轨选择和第三方 Mag 插件加载尚未提供。

## 维护与行为来源

本手册根据当前应用源码和相应测试整理；发布变更应同步更新涉及的操作章节。以下为维护者核对入口，不是使用 SE 的前置步骤：

- [GUI 命令、快捷键、菜单、保存和 BNK 入口](SingleEditorJuce/Source/UI/MainComponent.cpp)
- [波形手势](SingleEditorJuce/Source/UI/Components/WaveformViewComponent.cpp)、[播放定位与循环](SingleEditorJuce/Source/Audio/AudioEngine.cpp)
- [保存行为与测试](SingleEditorJuce/Tests/DocumentSaveTests.cpp)、[元数据测试](SingleEditorJuce/Tests/WavMetadataTests.cpp)
- [基础 CLI 参数](SingleEditorJuce/Source/CLI/CliArguments.cpp)、[AudioSuite CLI](SingleEditorJuce/Source/CLI/CliSingleFileAudioSuite.cpp)、[批处理 CLI](SingleEditorJuce/Source/CLI/CliBatchAutomation.cpp)
- [AudioSuite 参数契约](SingleEditorJuce/Source/CLI/CliAudioSuiteOperationRegistry.cpp)、[诊断 CLI](SingleEditorJuce/Source/CLI/CliDiagnostics.cpp)
- [恢复中心](SingleEditorJuce/Source/UI/Components/RecoveryCenterComponent.cpp)、[本地诊断](SingleEditorJuce/Source/UI/Components/LocalDiagnosticsComponent.cpp)、[设备输出设置](SingleEditorJuce/Source/UI/Components/AsioSettingsComponent.cpp)
