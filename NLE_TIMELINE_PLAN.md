# NLE 时间轴改造方案（kdenlive 对齐）

基于 kdenlive 本地源码（`~/文档/ceshi/kdenlive`，src/timeline2）与本仓库现状的全量对比

## 一 kdenlive 时间轴模型（源码实证）

| 维度 | kdenlive 实现 | 源码位置 |
|------|--------------|----------|
| 三层架构 | Bin 素材库（DocClipBase）→ TimelineItemModel（轨道+剪辑+合成项）→ QML 视图 | timeline2/model |
| 剪辑 | 轻量引用：ClipModel 只持 binClipId + position/in/out/speed/pitchShift，内容在 Bin | clipmodel.hpp |
| 轨道 | 显式 TrackModel 集合，AudioOnly/VideoOnly 两种类型，固定顺序，增删为显式操作 | trackmodel.hpp:construct、timelinemodel.hpp:721 |
| 轨道状态 | lock/mute/hidden/shouldReceiveTimelineOp（insert/lift/overwrite/extract 只落 active 轨） | trackmodel.hpp:99-114 |
| 轨道类型硬约束 | requestClipMove 校验 clipState 与 trackType，错型拒绝（MoveErrorType） | timelinemodel.cpp:852-862 |
| 空隙 | MLT blank producer 显式存在，getNextBlank/getPreviousBlank 可查询 | timelinemodel.hpp:462 |
| 重叠 | requestClipMove 目标非空白直接拒绝，磁吸重排走显式推挤链 | 磁吸定案（方案A） |
| 转场 | CompositionModel 一等公民条目（相邻块之间的 mix），移动剪辑时 mixData 自动跟随维护 | trackmodel.hpp:requestClipMix |
| 剪辑能力 | 每剪辑 EffectStackModel（特效栈）、MarkerListModel（块上标记）、变速、timeRemap、showKeyframes | clipmodel.hpp |
| A/V | 单剪辑双流（同一 producer），分离音频=派生音频块+link 联动 | MLT 语义 |
| 编辑模式 | lift（留洞）/extract（波纹）/insert/overwrite，TimelineFunctions::extractZone(liftOnly) 与 insertZone(overwrite) | timelinefunctions.hpp:132 |
| 工具 | SelectTool/RazorTool/SpacerTool/RippleTool/RollTool/SlipTool/SlideTool/MulticamTool 共 8 种 | definitions.h:244 |
| 轨道头 | 隐藏V/静音A（Shift=全部同类轨）/锁/轨道特效开关/录制（音频轨）/高度拖拽/特效拖放 | TrackHead.qml |
| 撤销 | 全操作 Fun undo/redo 闭环，失败留痕 | timelinemodel.cpp |

标记分两级：时间轴 guides + 剪辑上 markers

## 二 dream-cut 现状（friction 图层模型）

| 维度 | 现状 | 位置 |
|------|------|------|
| 层=内容 | eBoxOrSound 持 DurationRectangle（入出窗口），scene 直接子层即面板块 | editortimelinesync.cpp:rebuild |
| 轨道=推导 | trackId 分组折叠成 lane；单块 lane=独立行；轨道集随内容浮动 | rebuild lane 推导 + applyTrackWriteback |
| 跨轨移动 | 结构重排：moveContainedInList 改 native 行序 + setTrackId，整链进撤销栈 | applyTrackWriteback:339 |
| 分割 | 整层深拷贝（BoxesClipboard paste）+双端 trim；视频的 eVideoSound 子对象随块复制；文件级解码缓存共享 | canvas.cpp:splitBoxesAtFrame |
| 音视频 | VideoBox 内嵌 eVideoSound（ca_addChild，SWT 隐藏），音画天然成对 | videobox.cpp:62 |
| 独立音频 | 无 SoundClipboard，音频层无法复制/分割（剪刀跳过） | clipboardcontainer.h |
| 组块 | ContainerBox 显示为单块，子层不展开；子场景=面板保持上一有块场景 | rebuild fallback |
| 轨道头 | 仅 M 静音（映射 visible）/L 锁/双击改名 | editortimelinewidget.cpp:drawTrackHeaders |
| 重叠 | 合并 lane 上允许重叠（无硬约束，靠用户手工消解） | overlapsOnTrack 注释 |
| 编辑模式 | 仅删除 + 波纹删除（Shift+Delete），无 lift/insert/overwrite 拖拽模式 | deleteSelectedClips |
| 工具 | 选择 V/剪刀 B/向前 A/向后 Shift+A（阶段5），无 Spacer | EditTool |
| 特效与关键帧 | 挂在图层上，关键帧只在经典视图；NLE 块上无关键帧条 | KeysView |
| 标记 | 仅场景级 ruler 标记 | Canvas::setMarker |

## 三 核心差异（按对剪辑语义的伤害排序）

1. **轨道不是实体**：kdenlive 轨道是固定容器、块移动是纯数据操作；dream-cut 轨道是从 trackId 现推的，拖块跨轨=重排图层树=工程结构变更（撤销栈里是"图层重排"而不是"移动剪辑"）
2. **没有插入/覆盖模式**：只有覆盖式拖放（合并轨还允许重叠），无法"落点右侧整体右移腾位"；lift/extract 只有 extract
3. **块与源没有引用关系**：分割=整层深拷贝；拷贝共享文件解码缓存但层属性、特效、表达式全部双份，工程膨胀且两侧后续编辑互不感知（这在动画软件是特性，在剪辑软件是负债）
4. **独立音频不可编辑**：无复制通道；视频内嵌音频不可分离、不可单独静音段
5. **无转场条目**：相邻块之间无法声明过渡，只能靠手工叠加透明度关键帧
6. **轨道头信息不足**：无隐藏V/静音A分离、无轨道特效开关、无高度拖拽
7. **无 Spacer 工具**：整段后移/前移（B-roll 插入位腾挪）要框选后拖，易漏选
8. **块上无关键帧/标记**：音量/不透明度动画必须回经典视图
9. **无淡入淡出手柄**：块角拖拽生成曲线是剪映/KDE 共同的高频入口
10. **撤销语义混层**：NLE 操作（移动块）落到图层树撤销项，用户视角的"一步"与撤销栈的"一步"不一致

## 四 改造方案（四期）

### 方向决策

不引入 MLT 式独立剪辑模型，在 friction 图层树上立**剪辑门面**：轨道显式化、操作语义剪辑化，渲染/特效/表达式/撤销资产全部保留

### P0 轨道实体化（结构地基，已实施 877be56a9）

- 新增 TrackSpec 表（scene 侧）：`QVector<TrackSpec>{id,type,name,locked,muted,hidden,height}`，持久化进工程文件，旧工程首开自动推导一次后冻结
- rebuild 改为读 TrackSpec 渲染 lane，不再从 trackId 推导；轨道增删为显式操作（轨道头区 +V/+A 按钮，右键删除空轨）
- 块跨轨移动只写 trackId，**删除 applyTrackWriteback 里的 moveContainedInList 行序重排**；行序只在显式重排轨道时变更（渲染顺序=contained 顺序的既有不变量保持）
- 右键"合并到上一/下一轨"改为"移动块到上一/下一轨"（纯块操作）
- 轨道类型约束：音频层只进音频轨，视频/视觉层只进视频轨（对齐 MoveErrorType）
- 轨道头升级：隐藏V 与 静音A 分离（Shift=全部同类轨）、锁、双击改名（现有）、高度拖拽
- 收益：拖块不再是结构手术，撤销项变成真正的"移动剪辑"；重叠约束得以引入（同轨重叠=非法，给出红条提示）

风险：applyTrackWriteback 有磁吸压实/合并/分离多条调用链依赖行序写回，须逐链回归；旧工程迁移只做一次推导

### P1 剪辑操作语义补全

- 拖拽双模式：默认覆盖（现状），Ctrl 按住=插入模式（落点右侧同轨整体右移，复用 shiftLayerFrames），状态栏提示当前模式
- lift：块右键与 Z 键"删除（留洞）"，与现有 Shift+Delete 波纹删除构成 lift/extract 对
- Spacer 工具（间隔工具）：EditTool 增 Spacer，点击一侧整段平移，框选区域整体平移（建议键 R）
- 独立音频分割：补 eIndependentSound 的写读复制通道（或 SoundClipboard），剪刀支持音频块
- 定格（剪映高频）：播放头处以 queExternalRender 出帧 → 建 ImageBox 块插入
- 变速入口：块右键"变速"（VideoBox 已有 stretch 基础，FixedLenAnimationRect）

### P2 时间轴表达力

- 块上关键帧条：所选属性的关键帧标记绘制进块体（读图层 animator），点击跳帧，双击回经典视图
- 块标记（clip markers）与时间轴 guides 二级模型
- 分离音频：右键把 eVideoSound 提升为独立音频块 + link 元数据（联动移动/删除/分割）
- 淡入淡出手柄：块左上/右上角拖拽生成不透明度（视频）或音量（音频）线性/曲线关键帧
- 轨道特效开关（轨道头挂 RasterEffectCollection 的批量启停）

### P3 高阶剪辑

- 相邻块转场（mix）：两块交叠区声明过渡，先做交叉溶解，渲染走临时 surface 多 pass（液态玻璃管线先例）
- Ripple/Roll/Slip/Slide 高级修剪工具
- 轨道合成模式（qtblend 式变换轨道）、混音器与电平表

## 五 实施顺序与验收

- P0 → P1 → P2 → P3，P0 单独一个阶段提交（结构地基，后续全部依赖）
- 每期验收：编译 0 错误 + effects_test 全过 + wayland 冒烟 + GUI 回测清单
- P0 回测重点：拖块跨轨后经典视图行序不变、撤销恢复原轨、旧工程首开轨道推导正确、磁吸/波纹在新轨道模型下回归
