# DreamCut

**剪映式桌面视频剪辑软件 · Linux 原生 · Qt6 / C++ / Skia**

DreamCut 是一款以剪映（CapCut）交互语义为目标重新设计剪辑时间轴的开源视频软件，基于 [Friction](https://github.com/friction2d/friction) 2.5D 深度定制分支二次开发。它把「轨道块 + 磁吸主轨 + 覆盖轨 + 即时预览」的移动端剪辑手感带到桌面端，同时完整保留了底座的 MG 动画/合成能力（矢量、位图、表达式、脚本、特效、Lottie 等）。

一句话定位：**用剪映的手感剪片，用 AE 级的底座做动效。** 界面全面中文化。项目持续开发中。

## ✨ 核心特性

### ✂️ 剪辑时间轴（NLE 三层架构）

- **磁吸时间轴**：主轨拖拽实时重排、被拖块 1:1 跟鼠标、左包向起点压实、块间恒无间隙（对齐 kdenlive 方案 A 并实测）
- **CapCut 轨道模型**：主轨 / 覆盖轨 / 音频轨三种角色；主轨金底徽标持久化，加删轨永不移动主轨
- **覆盖轨跟随**：覆盖块锚定所落的主轨块，主轨移动 / 磁吸重排 / 修剪 / 波纹删除时整体随动
- **轨道生命周期**：拖块到时间轴上下空白即新建轨道（幽灵轨预览）、零成员轨道自动清理（主轨豁免）
- **干净分割**：拷贝守 `[min..F]`、原件守 `[F+1..max]`，零共享帧；剪刀工具、块内切割、场景检测逐刀分割同一守卫
- 定格、变速、波纹删除、插入模式、禁用片段、拖拽阈值防误触
- 块颜色标记 + 选中同色块；块剪贴板跨工程复制粘贴（含声音序列化克隆）

### 🔄 转场系统（43 个转场）

- 块间真重叠模型（右块左移 N 帧挂转场特效），交界处转场条 UI，拖拽调时长、直接删除
- 内置转场 + [gl-transitions](https://github.com/gl-transitions/gl-transitions) 移植（渐变 / 擦除 / 缩放 / 故障 / 分形 / 显影……），统一缓动参数面板
- 转场卡片单击应用 / 替换 / mime 拖放，播放实时预览，CPU / GPU 双路径

### 🎬 监视器与播放

- **片段监视器**：出入点（I / O / Shift+I / Shift+O）、zone 黄区、时长气泡、选段直接拖出到时间轴
- **铺开波形视图**：整幅大包络绘制、滚轮锚点缩放、中键平移、选区带与条带双向同步，纯音频自动开启
- 时间轴标尺缩放（刻度按窗口自适应）、播放头追边、标尺滑动条
- **JKL 梭动播放**：J/L 变速（最高 8x、倒放）、K 让位暂停
- 直播放引擎：音频主时钟、内容感知终点，预览不掉链

### 📝 字幕

- SRT 导入 / 导出（容错解析：BOM / CRLF / 逗号或点毫秒 / 跨行正文）
- 字幕轨 = TextBox 块，导入自动建轨，可继续用底座 MG 能力做花字动效
- **语音转字幕**：whisper.cpp 外挂编排（ffmpeg 抽 16kHz 单声道 wav → whisper-cli 出 SRT → 落字幕轨），语言可选，缺件时给安装引导不静默

### 🔊 音频

- **分离音频**：同文件独立声音块（共享解码缓存零重复解码）、内嵌音轨可逆静音
- **混音器面板**：轨道音量推子（-60..+6 dB）、Mute / Solo、实时电平表（RMS + 峰值保持 + 过载红顶）
- **响度标准化**：一键 -14 LUFS（mean/max 检测 + 峰值保护钳制防爆音）
- **淡入淡出角手柄**：块角拖拽，音频落音量包络关键帧、视频 / 文字落不透明度关键帧，线性淡变精确
- **音效库面板**：波形卡片、悬停试听、收藏、最近使用、UCS 标准分类（专业音效库即插即浏览）、多色标签、键盘流翻听（↑↓←→ 翻到即听）、边播边叠听对比、直接拖入时间轴

### ⚡ 效率工具

- **后台任务管理器**：转码 / 分析任务排队、进度、协作取消、专用线程池
- **代理剪辑**：右键视频块生成 540p 代理，低分辨率上下文自动接管（播放用代理、导出恒原件），一键总开关
- **预渲染区域**：入出点区间后台渲染进缓存，渲完不抢播放，随时从缓存起播
- **素材库**：按名搜索过滤（场景恒显）、未使用素材一键清理（引用计数判定）
- **场景检测**：ffmpeg 场景切变阈值检测，自动逐刀分割成镜头
- **标记导航**：标尺标记 / 注释编辑、上一处 / 下一处跳转、全部标记时码列表点击跳转
- **单文件工程**：`.dreamcut` 封装（工程数据 + 封面缩略图封进一个文件），旧格式兼容读取，备份自动收编

### 🎨 继承自 Friction 2.5D 的 MG 底座

矢量 / 位图合成、2.5D 图层、骨骼系统、AE 式摄像机、表达式与脚本引擎、230+ 动效预设、蒙版 / 图层样式 / 剪贴蒙版、AE 式特效家族、Lottie / PSD / Krita / OCA 导入、AI 协作接口——剪辑之外仍是一个完整的 MG 动画工具。

## 🖥 系统要求

- 64 位 Linux（开发与日常验证环境：CachyOS / Arch；X11 / Wayland）
- Qt 6.x、FFmpeg、OpenGL
- CMake ≥ 3.16、Ninja、Clang 或 GCC（C++20）
- 可选：[whisper.cpp](https://github.com/ggml-org/whisper.cpp)（语音转字幕）

## 🔨 从源码构建

```bash
git clone --recursive https://github.com/1220874621zp-debug/dream-cut.git
cd dream-cut
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build dreamcut
cmake --install build --prefix ~/.local   # 安装到 ~/.local/bin/dreamcut
```

- Skia 以子模块形式内置，默认随主工程一起编译（`BUILD_SKIA=ON`）；有预编译 Skia 时可用 `-DSKIA_LIB_PATH=/path/to/skia` 外接
- 常用 CMake 选项：`FRICTION_OFFICIAL_RELEASE`（发布标识）、`SKIA_STATIC`（静态链接 Skia）
- 无头 / 自动化验证可配合 `xvfb-run`

## 🧱 源码结构

| 路径 | 说明 |
|------|------|
| `src/app/GUI/Timeline/` | NLE 时间轴三层：Model（轨道/剪辑唯一事实源）、View（渲染 + 手势）、Controller（媒体路由 / 播放头桥接） |
| `src/app/GUI/timelinedockwidget.*` | 时间轴 dock 装配层：工具栏、播放控制、键盘路由 |
| `src/core/canvas.cpp` | 场景侧轨道表、分割、轨道执法 |
| `src/app/GUI/directplayer.cpp` | 直播放引擎（音频主时钟） |
| `src/core/ReadWrite/evformat.h` | 工程文件版本化（EvFormat） |
| `NLE_TIMELINE_PLAN.md` | 时间轴对齐 kdenlive 的差异分析与分期方案 |

## 🧪 工程实践

所有时间轴行为均以**无头合成事件台架**实证：Xvfb 环境下用自测钩子把合成鼠标 / 键盘事件直发到视图，断言模型表落位与像素级结果，验证后清除测试码再出正式构建。各提交信息中保留完整 AUTOTEST 实证记录，行为语义（磁吸 / 分割 / 跟随 / 生命周期）以提交信息为准。

## 📄 许可证与致谢

GPL-3.0-only，见 [LICENSE.md](LICENSE.md)。

- [Friction](https://github.com/friction2d/friction) 及其贡献者 —— 本项目的上游底座
- [enve](https://github.com/MaurycyLiebner/enve) —— Friction 的前身
- [gl-transitions](https://github.com/gl-transitions/gl-transitions) —— 部分转场的参考实现
- [whisper.cpp](https://github.com/ggml-org/whisper.cpp) —— 语音转字幕（外挂程序，可选）
- 图标主题子模块：[friction-icon-theme](https://github.com/1220874621zp-debug/friction-icon-theme)（DreamCut 品牌化 `dreamcut` 分支）
