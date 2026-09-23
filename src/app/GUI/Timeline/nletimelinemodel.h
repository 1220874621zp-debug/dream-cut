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
    // returns only the clips that actually move
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
    // ripple = false: plain remove; ripple = true: later clips on the
    // same track slide left to close the gap
    bool requestDelete(const QSet<int> &clipIds, const bool ripple);
    // context-menu lane hop to an explicit track (validated: type,
    // lock, overlap), one undo step
    bool requestMoveClipToTrack(const int clipId, const int dstTrackId);
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
    // 独奏压制台账：solo 生效期间被藏掉的层（恢复即 visible=true，
    // 剪映语义：取消独奏该回来的都会回来）；仅内存态不进工程文件
    QSet<eBoxOrSound*> mSoloSaved;
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
