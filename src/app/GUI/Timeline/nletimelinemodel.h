#ifndef NLETIMELINEMODEL_H
#define NLETIMELINEMODEL_H

#include <QObject>
#include <QVector>
#include <QSet>
#include <QHash>
#include <QPointer>
#include <QList>
#include <QPair>
#include <QString>

#include "smartPointers/ememory.h"

class Canvas;
class Document;
class eBoxOrSound;
class BoundingBox;
class eIndependentSound;
class BoxesClipboard;
struct eTrackSpec;

// kdenlive-style timeline model: the single source of truth for the
// NLE panel. Tracks are explicit persistent entities (the scene's
// track specs), clips are facade entries over the scene's child
// layers (a layer with a duration rectangle = one clip). Everything
// works in absolute frames on the scene frame grid.
//
// Edits go through request*() operations (document side, undoable)
// and commitMoves() (gesture release: the view validated the
// candidates live, the model trusts them and writes the layers).
// Views repaint purely from modelChanged/selectionChanged signals;
// a running gesture keeps the model untouched (it IS the press-time
// snapshot), so release math needs no snapshots of its own.
class NleTimelineModel : public QObject
{
    Q_OBJECT
public:
    explicit NleTimelineModel(Document &document,
                              QObject * const parent = nullptr);

    struct Track {
        int id = -1;            // persistent eTrackSpec id
        QString name;
        bool audio = false;
        bool locked = false;    // persistent: spec carries it
        bool solo = false;      // persistent: spec carries it
        bool muted = false;     // mirror: all of the track's layers hidden
        int height = 0;         // 0 = type default
    };
    struct Clip {
        int clipId = 0;         // model id, stable per layer
        int trackId = -1;
        int start = 0;          // absolute in frame
        int duration = 0;       // frames, >= 1
        QString name;
        bool audio = false;
        qreal speed = 1.;       // playback rate (1 = original)
        QPointer<eBoxOrSound> layer;
    };
    // one clip placement to write into the document (commitMoves) or
    // to hand to the view as a validated candidate (plans)
    struct Move {
        int clipId = 0;
        int trackId = -1;
        int start = 0;
        int duration = 0;
    };

    // ---- state ----
    const QVector<Track> &tracks() const { return mTracks; }
    const QVector<Clip> &clips() const { return mClips; }
    const Clip *clip(const int clipId) const;
    int trackIndex(const int trackId) const;
    const Track *track(const int trackId) const;
    QList<int> clipIdsOnTrack(const int trackId) const; // start order
    bool trackLocked(const int trackId) const;
    // CapCut main track: the bottom-most video lane (V1) - magnetic
    // rearranging/compaction applies to it alone, overlay tracks
    // above keep their layout and ride the main blocks they sit on
    int mainTrackId() const;
    qreal fps() const { return mFps; }
    // shortest legal clip in frames (frame-grid equivalent of the old
    // 0.2s minimum)
    int minClipFrames() const;
    // out-of-line: QPointer<T>::data() needs the complete type, the
    // view translation unit only forward-declares Canvas
    Canvas *panelScene() const;
    bool inWriteback() const { return mInWriteback; }

    // ---- live-drag validation & planning (view side) ----
    // kdenlive hard constraint: a placement is legal when nothing
    // outside the moving set collides on the target track. Clips
    // already tangled with the moving set at press time (legacy
    // overlap layouts) are exempt so knots can be dragged apart.
    static bool rangesOverlap(const int aStart, const int aDur,
                              const int bStart, const int bDur);
    bool overlapOutside(const int trackId, const int start, const int dur,
                        const QSet<int> &moving,
                        const QSet<int> &exempt) const;
    QSet<int> collectTangleExemptions(const QSet<int> &moving) const;
    // trim range [lo, hi] (in frames) for the neighbor-edge clamp:
    // lo = previous clip's out point, hi = next clip's start; clips
    // in the moving set never clamp, exempt (press-time tangled)
    // clips don't either - a legacy overlap layout must not pin the
    // edge BEHIND its own current position
    void trimBounds(const int clipId, const QSet<int> &moving,
                    const QSet<int> &exempt,
                    int *lo, int *hi) const;
    // Ctrl insert drop: the run the dropped clip touches on the lane
    // slides right as one rigid block (returns empty when nothing
    // needs to move)
    QVector<Move> insertShiftPlan(const int targetTrackId,
                                  const int dropStart, const int dropDur,
                                  const QSet<int> &moving) const;
    // magnetic (CapCut): close every genuine gap, keep overlaps;
    // returns only the clips that actually move. 用户规则：主轨首块
    // 恒靠左对齐时间起点，游标恒从帧 0 起压（拖走的头块也被拉回）
    QVector<Move> compactGapsPlan() const;
    // kdenlive-style magnetic drag rearrange (方案A): the dragged clip
    // lands at the drop frame 1:1; clips fully left of the drop
    // compact toward 0; every other same-track clip chains tightly
    // after the dragged clip's new out point - the whole track makes
    // room live, a horizontal drag across neighbours is a reorder
    QVector<Move> magneticRearrangePlan(const int trackId,
                                        const int draggedId,
                                        const int dropStart,
                                        const QSet<int> &movingIds) const;

    // ---- CapCut 转场（两块之间的实体）----
    // 真重叠模型：右块 durRect 左移 N 帧与左块重叠，右块挂对应
    // 转场 RasterEffect（入窗口 = N、出窗口 = 0）淡入盖住左块尾巴；
    // 渲染复用特效管线的转场 caller，持久化随层特效天然保存
    struct Transition {
        int leftId = -1;
        int rightId = -1;
        int start = 0;   // = right.start（重叠区首帧）
        int end = 0;     // = left.end（重叠区末帧）
        int frames = 0;  // 转场窗口（特效入窗口值，<= 重叠宽）
        int type = 0;    // RasterEffectType
        QString typeName;
    };
    // 主轨上即时派生的转场表（不缓存：特效参数变化自动反映）
    QVector<Transition> transitions() const;
    // 播放头规则应用转场：播放头落在主轨块 L 上且 L 与右邻 R 贴邻
    // （或播放头正落在贴邻对的右块首帧）→ 应用到 L|R 交界；
    // 已有转场的交界 = 替换类型（布局不动）
    bool requestApplyTransition(const int junctionFrame,
                                const int transitionType);
    // 拖拽落点规则：吸附 dropFrame 附近（±1 秒）最近的贴邻交界
    bool requestApplyTransitionAtDrop(const int dropFrame,
                                      const int transitionType);
    // 删除转场：剥右块的转场特效并把右块右移回贴左块
    bool requestRemoveTransition(const int rightClipId);
    // 调整转场窗口时长：右块随新窗口位移保持贴邻关系
    bool requestTransitionDuration(const int rightClipId,
                                   const int frames);

    // ---- selection ----
    QSet<int> selection() const { return mSelected; }
    bool isSelected(const int clipId) const;
    void setSelection(const QSet<int> &ids);
    void addToSelection(const QSet<int> &ids);
    void toggleInSelection(const int clipId);
    void clearSelection();
    void selectAll();

    // ---- gesture lifecycle ----
    // while active, document-driven rebuilds queue up instead of
    // repopulating under a running drag; the release flushes them
    void setGestureActive(const bool active);
    bool gestureActive() const { return mGestureActive; }

    // ---- document-side operations (undoable) ----
    // write validated candidates into the layers: range transforms +
    // trackId writes only (never a structural row surgery), then the
    // row-order stabilization, one actionFinished; compactAfter also
    // closes the gaps when magnetic mode is on
    bool commitMoves(const QVector<Move> &moves,
                     const bool compactAfter = false);
    // CapCut 分割: selected clips at the frame, fallback = every
    // unlocked-track clip under it
    bool requestSplitAtFrame(const int frame);
    // razor: cut strictly inside each given clip, sounds refused
    bool requestRazorCut(const QSet<int> &clipIds, const int frame);
    // CapCut 定格: video-family clips only, freeze from the cut to
    // each clip's end on the cut frame
    bool requestFreeze(const QSet<int> &clipIds, const int frame);
    // playback rate (1 = original) -> layer stretch
    bool requestSpeed(const int clipId, const qreal rate);
    // CapCut 倒放: descending remapping keys reverse the source order,
    // toggling again restores forward playback
    bool requestReverse(const QSet<int> &clipIds);
    // ripple = false: plain remove; ripple = true: later clips on the
    // same track slide left to close the gap
    bool requestDelete(const QSet<int> &clipIds, const bool ripple);
    // context-menu lane hop to an explicit track (validated: type,
    // lock, overlap), one undo step
    bool requestMoveClipToTrack(const int clipId, const int dstTrackId);
    // 片段监视器拖入：path 带 [inFrame..outFrame] 源出入点，落在
    // trackId 的 startFrame（目标轨让位插入）
    bool requestInsertMedia(const QString &path, const int inFrame,
                            const int outFrame, const int trackId,
                            const int startFrame);
    // 音效库/纯音频拖入：落音频轨的 eIndependentSound 块（CapCut
    // 音效语义）；secHint = 源总时长秒（音效库 mime 携带），<=0 时
    // 临时激活 SoundHandler 现测时长；[inSec..outSec] = 源段出入点
    // 秒（监视器 zone 换算，outSec<0 = 全片段），块长按场景 fps 换
    // 算；目标轨让位插入
    bool requestInsertSound(const QString &path, const int trackId,
                            const int startFrame,
                            const qreal secHint = -1.0,
                            const qreal inSec = 0.0,
                            const qreal outSec = -1.0);
    // ---- 字幕（SRT ↔ "字幕"轨 TextBox 块）----
    // 字幕 = 名"字幕"的视频型轨上的文字层（导入自动建轨，组顶生长）
    int subtitleTrackId() const;
    // SRT 导入：逐条建 TextBox（底部居中），durRect 按场景 fps 换算，
    // 一次事务不进撤销栈；false = 解析失败/无场景
    bool requestSubtitleImport(const QString &path);
    // SRT 导出：收集字幕轨 TextBox 按起点排序写 SubRip
    bool requestSubtitleExport(const QString &path);
    // 项目面板链接场景拖入：在 trackId 的 startFrame 落外部工程场
    // 景的动态链接块（让位插入，requestInsertMedia 同款事务语义）
    bool requestInsertSceneLink(const QString &path, const int sceneDocId,
                                const QString &sceneName, const int trackId,
                                const int startFrame);
    // 菜单"导入链接工程"：链接场景追加到主轨末尾（多次调用自动
    // 接龙——每次都取当前主轨末帧作落点）
    bool requestAppendSceneLink(const QString &path, const int sceneDocId,
                                const QString &sceneName);
    // kdenlive "detach audio": pull the VideoBox's embedded sound out
    // as an independent audio clip (same file, same range, same
    // speed) parked on the first audio track; the embedded copy goes
    // silent (visibility = audibility in the sound composition)
    bool requestDetachAudio(const int clipId);

    // ---- clip decorations (CapCut 右键菜单语义，会话内存态) ----
    // 颜色标记 0..6（-1 = 无）：块名条右端圆点 + 选中同色片段
    int colorMark(const int clipId) const;
    // color < 0 清除标记；作用于整个选中集
    void requestColorMark(const QSet<int> &clipIds, const int color);
    // 选中与该块同色的全部块；无标记返回 false
    bool requestSelectSameColor(const int clipId);
    // 停用片段（Shift+E）：层隐藏 = 渲染跳过 + 声音静音，恢复走台账
    bool isDisabled(const int clipId) const;
    void requestSetDisabled(const QSet<int> &clipIds, const bool disabled);

    // ---- timeline clipboard（时间轴独立块剪贴板，不与画布互通）----
    bool hasClipClipboard() const;
    bool requestCopy(const QSet<int> &clipIds);
    // 粘贴到 frame：视频类块走 BoxesClipboard 完整克隆，声音类走
    // 序列化克隆；落位 = 同轨从 frame 起第一个无重叠槽位
    bool requestPaste(const int frame);

    // ---- tracks (explicit entities, spec ops on the scene) ----
    // returns the new track id (-1 on failure); the panel renders
    // video specs first, then audio specs, so an appended track lands
    // at the end of its type group; atTop grows the type group at
    // its top instead (drag-into-the-ruler drop zone)
    int requestTrackAdd(const bool audio, const bool atTop = false);
    bool requestTrackRemove(const int trackId);
    void requestTrackRename(const int trackId, const QString &name);
    void requestTrackSetLocked(const int trackId, const bool locked);
    void requestTrackToggleSolo(const int trackId);
    void requestTrackSetHeight(const int trackId, const int height);
    // Shift extends the toggle to every track of the same type
    void requestTrackToggleMute(const int trackId, const bool allSameType);

    // ---- guides ----
    void requestMarkerAdd(const int frame);
    void requestMarkerRemove(const int frame);
    // 标记注释编辑（canvas 撤销链 + guides 重推）
    bool requestMarkerRename(const int frame, const QString& title);
    // 场景检测分割：cutSecs = 源文件切点秒（ffmpeg scene 滤镜），
    // srcFps = 源流帧率（<=0 回退场景 fps）；逐刀走 requestRazorCut
    // 守卫（严格块内 + 转场禁区），返回实际完成的刀数
    int requestSceneDetectSplits(const int clipId,
                                 const QVector<double>& cutSecs,
                                 const qreal srcFps);

    // ---- magnetic mode ----
    bool magnetic() const { return mMagnetic; }
    // 轨道联动（CapCut 覆盖跟随）：覆盖轨的块跟随主轨块移动/删除
    // 位移（锚定其头部所落的主轨块）——工具栏可开关
    bool followLinked() const { return mFollowLinked; }
    void setFollowLinked(const bool on) { mFollowLinked = on; }
    // enabling compacts every track right away (undoable); compact =
    // false only restores the flag (session start: a loaded project
    // must not be rewritten just because magnetic defaults on)
    void setMagnetic(const bool on, const bool compact = true);

    // ---- undo ----
    void undo();
    void redo();

    // ---- document sync ----
    // full rebuild from the active scene (with the last-scene-with-
    // blocks fallback); queues while a gesture runs
    void refreshFromDocument();

signals:
    // tracks and/or clips replaced: repaint everything
    void modelChanged();
    void selectionChanged();
    void magneticChanged(const bool on);
    // 颜色标记/停用态等块装饰变化（轻量重绘，不重建表）
    void clipDecorationsChanged();
    // markers / in-out band / fps display state changed
    void guidesChanged();
    // playhead moved on the document side (scene frame signal or a
    // rebuild switched scenes)
    void playheadFrameChanged(const int frame);
    // every refusal lands here (status bar feedback)
    void logMessage(const QString &msg);

private:
    void connectPanelScene(Canvas * const scene);
    void connectChildren(Canvas * const scene);
    // CapCut track lifecycle: every commit drops tracks that lost
    // their last clip (one lane of each type always survives);
    // membership is passed in RESOLVED form (the caller's clip
    // table), parking layers count for their displayed lane
    void purgeEmptyTracks(const QHash<int, int> &members);
    // one-time legacy migration: freeze the pre-P0 derived lane
    // layout into the scene's persistent track table
    QList<eTrackSpec> deriveTrackSpecs(
            Canvas * const scene,
            const QList<QPair<eBoxOrSound*, bool>> &items);
    // keep each track's members contiguous with the tracks in spec
    // order (compositing-order invariant) without touching the order
    // inside a track
    void stabilizeRowOrder();
    // select the boxes on the scene and run the frame-parameterized
    // split (one actionFinished, one refresh)
    bool splitBoxes(const QList<BoundingBox*> &boxes, const int frame);
    void setLayerRange(eBoxOrSound * const layer,
                       const int start, const int duration);
    void shiftLayer(eBoxOrSound * const layer, const int frameDelta);
    void finishAction(); // actionFinished + refresh + flush queue
    // 层挂转场特效时的窗口帧数（入窗口参数值）；无转场特效 = 0。
    // 磁吸压实游标的转场重叠预留量
    int transitionWindowOf(const Clip &c) const;
    // 剥掉层的转场特效（转场删除/删除左块/分割右半的公共尾部）
    bool stripTransitionEffect(eBoxOrSound * const layer);
    // 已解析贴邻对上的应用/替换（requestApply* 的公共尾部）
    bool requestApplyTransitionOnPair(const Clip &L, const Clip &R,
                                      const int transitionType);

    Document &mDocument;
    QPointer<Canvas> mPanelScene;
    QVector<Track> mTracks;
    QVector<Clip> mClips;
    // stable model ids: a layer keeps its clip id across rebuilds so
    // media caches (film strips / waveforms) survive AND the clip
    // selection survives by id (never by name - razor halves share
    // one name, name matching co-selected every sibling)
    QHash<eBoxOrSound*, int> mLayerToClipId;
    int mNextClipId = 1;
    QSet<int> mSelected;
    // 独奏压制台账：solo 生效期间被藏掉的层 -> 压制前的真实可见性
    // （恢复时写回保存值而非恒 true，否则用户静音/停用会被取消独奏
    // 无声抹掉）；仅内存态不进工程文件
    QHash<eBoxOrSound*, bool> mSoloSaved;
    QSet<eBoxOrSound*> mSoloSuppressed;
    // 停用片段台账（会话内存）：停用前的可见性，恢复时回写；
    // solo 压制中的层取消停用不点亮（压制机制优先）。
    // 键 = clipId（永不复用），层指针经活表反查
    QSet<int> mDisabledIds;
    QHash<int, bool> mDisabledSaved;
    // 颜色标记（会话内存）：clipId -> 0..6
    QHash<int, int> mColorMarks;
    // 时间轴块剪贴板：视频/图像类走 BoxesClipboard 完整序列化，
    // 声音类走 prp_writeProperty_impl 克隆体（BoxesClipboard 不收声音）
    stdsptr<BoxesClipboard> mBoxClipBoard;
    QList<qsptr<eIndependentSound>> mSoundClipBoard;
    struct PasteSpec {
        int trackId = -1;
        int duration = 0;
        QString name;
    };
    QVector<PasteSpec> mBoxClipSpecs;
    QVector<PasteSpec> mSoundClipSpecs;
    // 块删除/换场景后剪掉失效 id 的装饰台账（clipId 按层恒定，
    // 层没了 id 永不复用，不剪就是纯泄漏）
    void pruneClipState();
    qreal mFps = 25.;
    bool mMagnetic = false;
    bool mFollowLinked = true;
    bool mInWriteback = false;
    bool mGestureActive = false;
    bool mRebuildQueued = false;
    // 删除/修剪发生过：下一次 refresh 评估把场景范围收回内容末尾
    bool mCheckRangeShrink = false;
    QList<QMetaObject::Connection> mSceneConns;
    QList<QMetaObject::Connection> mChildConns;
};

#endif // NLETIMELINEMODEL_H
