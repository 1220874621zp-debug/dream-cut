#ifndef NLETIMELINEVIEW_H
#define NLETIMELINEVIEW_H

#include <QWidget>
#include <QVector>
#include <QPixmap>
#include <QImage>
#include <QHash>
#include <QSet>
#include <QPair>

#include "nletimelinemodel.h"
#include "misc/keyfocustarget.h"

class QScrollBar;
class QrealAnimator;
class QrealKey;

// kdenlive-style timeline view: pure rendering + gestures over
// NleTimelineModel. The view holds NO document state - tracks, clips
// and selection are read from the model, and a running drag never
// mutates anything: candidate placements are computed against the
// model (which therefore stays the press-time snapshot) and painted
// as an overlay; the release turns them into one model commit.
// Everything works in absolute frames on the scene frame grid.
//
// The view is also a KeyFocusTarget (friction's keyboard routing):
// clicking it takes the KFT focus, so timeline keys (Delete, tools,
// split ...) never fall through to the canvas window - the kdenlive
// equivalent of tracksArea owning the keyboard while the user edits.
class NleTimelineView : public QWidget, public KeyFocusTarget
{
    Q_OBJECT
public:
    explicit NleTimelineView(NleTimelineModel * const model,
                             QWidget * const parent = nullptr);

    // 淡入淡出写口/读口（无头台架与后续右键菜单复用）：
    // 落键（len<=0 = 移除）corner 模式共线控制点 = 线性淡变
    void fadeWrite(const int clipId, const bool out, const int len);
    // 现有淡变长度（帧，-1 = 无）：角锚键值 0 + 邻近满值键
    int fadeDetectLen(const NleTimelineModel::Clip &c,
                      const bool out) const;
    // 淡变目标动画器（音频=音量 视觉=不透明度），台架/菜单复用
    QrealAnimator *fadeAnimatorFor(const NleTimelineModel::Clip &c,
                                   bool *isAudioOut) const;

    void setScrollBar(QScrollBar * const bar);
    int playheadFrame() const { return mPlayheadFrame; }

    // editing tools (PR/CapCut style): the tool changes what a plain
    // left click on a clip does; Select keeps the classic
    // move/trim/rubber-band interactions
    enum class EditTool { Select, Razor, TrackBackward, TrackForward,
                          Spacer, Hand };
    void setTool(const EditTool tool);
    EditTool tool() const { return mTool; }

    // toolbar zoom slider (0-100, right = zoom in), playhead-anchored
    void setZoomLevel(const int level);

    // kdenlive insert/overwrite toggle (toolbar): when on, drops push
    // the touched run right instead of requiring free space; Ctrl
    // during a drag flips it for that gesture only
    void setInsertMode(const bool on) { mInsertMode = on; }
    bool insertMode() const { return mInsertMode; }

    // ruler markers (abs frames + titles) and the scene in/out band
    // (frames; negative = disabled), fed by the controller
    void setMarkers(const QVector<QPair<int, QString>> &markers);
    void setRangeBand(const int inFrame, const int outFrame);

    // media delivered by the controller: real rendered frame per
    // video-family clip (midpoint thumbnail / decoded filmstrip tiles
    // keyed by abs frame) and real waveform peaks per abs second
    void setClipThumbnail(const int clipId, const QImage &image);
    void setClipThumbFrame(const int clipId, const int absFrame,
                           const QImage &image);
    void setClipWave(const int clipId, const int absSecond,
                     const QVector<qreal> &peaks);

    // visible range in frames (the controller culls media requests)
    int viewStartFrame() const;
    int viewEndFrame() const;
    double pxPerFrame() const { return mPxPerFrame; }

public slots:
    void zoomIn();
    void zoomOut();
    void zoomFit();
    void setPlayheadFrame(const int frame);
    // CapCut 分割 at the view playhead (toolbar button / S key)
    void splitAtPlayhead();
    // CapCut 定格: freeze the selection at the playhead
    void freezeAtPlayhead();
    // CapCut 倒放: reverse the selected clips (toggle)
    void reverseSelected();
    void requestDelete(const bool ripple = false);

signals:
    // zoom / scroll / resize changed the visible range
    void viewChanged();
    // the user dragged/clicked the playhead to this frame
    void playheadDragged(const int frame);
    // active editing tool changed (int = EditTool); the dock keeps
    // its checkable toolbar actions in sync
    void toolChanged(const int tool);
    // view-side refusals (status bar feedback)
    void logMessage(const QString &msg);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void leaveEvent(QEvent *event) override;
    // 片段监视器拖入：接收 path+出入点 mime，落块到目标轨
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

    // ---- KeyFocusTarget (friction keyboard routing) ----
    // timeline keys are consumed here once the view holds the KFT
    // focus (any click takes it): Delete NEVER falls through to the
    // canvas window, which would otherwise delete the canvas
    // selection and steal the focus back
    bool KFT_keyPressEvent(QKeyEvent *e) override;
    void KFT_setFocusToWidget() override;
    void KFT_clearFocus() override {}

private:
    // ---- layout / mapping (frames <-> pixels) ----
    int rulerHeight() const { return 30; }
    int headerWidth() const { return 168; }
    int trackHeight(const int trackIdx) const; // live preview aware
    int trackY(const int trackIdx) const;      // top y of track content
    int trackAtY(const int y) const;           // -1 if none
    int xToFrame(const int x) const;
    int frameToX(const int frame) const;
    int contentFrames() const; // right edge of content (frames)
    QRectF clipRect(const NleTimelineModel::Clip &c) const;
    QRectF moveRect(const NleTimelineModel::Move &m) const;

    // ---- painting ----
    void drawRuler(QPainter &p);
    void drawTrackHeaders(QPainter &p);
    void drawTrackBodies(QPainter &p);
    void drawClip(QPainter &p, const NleTimelineModel::Clip &c,
                  bool ghost = false);
    void drawMove(QPainter &p, const NleTimelineModel::Move &m);
    // centered waveform strip (real peaks when the controller fed
    // them, deterministic pseudo wave otherwise)
    void drawWave(QPainter &p, const QRectF &body,
                  const NleTimelineModel::Clip &c);
    void drawPlayhead(QPainter &p);
    // CapCut main-track seam dots: red marks where neighbouring main
    // clips butt together (candidate-aware so they ride a drag)
    void drawSeamDots(QPainter &p);
    // 转场块：主轨交界重叠区上的圆角块（图标+名，选中高亮）
    void drawTransitions(QPainter &p);
    // 转场块 hit-test（返回 rightClipId，无则 -1）
    int transitionAt(const QPoint &pos) const;
    QRectF transitionRect(const NleTimelineModel::Transition &t) const;
    // CapCut audio volume envelope: white key line over the waveform
    void drawVolumeEnvelope(QPainter &p,
                            const NleTimelineModel::Clip &c,
                            const QRectF &body);
    // read-only envelope hit test (cursor + gesture arming)
    enum class VolHit { None, Line, Key };
    VolHit volumeHitTest(const QPoint &pos, int *clipIdOut,
                         QrealKey **keyOut) const;
    // consume a press on the envelope: drag a key, Alt-delete one, or
    // create+drag a new key at the cursor frame
    bool armVolumeGesture(const QPoint &pos, const bool alt);

    // ---- 块角淡入淡出手柄（CapCut 式）----
    // 音频块=音量关键帧对（0→100），视频/视觉块=不透明度关键帧对；
    // 拖角定长，点击角（已有淡变时）=清除（写口/读口在 public 区）
    QRectF fadeHandleRect(const NleTimelineModel::Move &m,
                          const bool out) const;
    void drawFadeOverlay(QPainter &p, const NleTimelineModel::Clip &c,
                         const NleTimelineModel::Move &m,
                         const QRectF &r);    QPixmap thumbnailTile(const int clipId, const int hueSeed, const int h);
    QString timecode(const int frame) const;
    // audio info tag (bitrate for independent sounds), cached per id
    QString audioTagFor(const NleTimelineModel::Clip &c);

    // ---- interaction ----
    enum class DragMode { None, MoveClip, TrimLeft, TrimRight, Playhead,
                          TrackHeight, SpacerMove, Pan, VolumePoint,
                          TransitionSize, FadeHandle };
    // candidate track id for the CapCut lane lifecycle: dragging a
    // clip below every track targets a NEW track of its type, which
    // materializes on release
    static constexpr int kGhostTrackId = -2;
    int ghostLaneTop() const; // y of the to-be-created lane
    int clipAt(const QPoint &pos, QRectF *rectOut = nullptr) const; // id or -1
    // candidate-or-model placement of a clip (snapping considers the
    // live candidate positions of the other moving clips)
    NleTimelineModel::Move effectiveMove(const int clipId) const;
    int snapFrame(const int frame, const QSet<int> &ignoreIds,
                  bool *snappedOut) const;
    void applyZoom(const double factor, const int anchorX);
    void clampView();
    void updateScrollBar();
    void applyToolCursor(const QPoint &pos);

    // ---- move/trim/spacer gesture helpers ----
    // drop legality: no collision with anything outside the moving
    // set (tangle exemptions apply); riders shift on their own tracks
    bool dropLegal(const int primaryId, const int targetTrackId,
                   const int newStart) const;
    // magnetic follow (CapCut): trimming the out point slides every
    // same-track clip that started at/after the old out point by the
    // same delta; returns their candidate moves for this newEnd
    QVector<NleTimelineModel::Move> magneticFollowMoves(
            const int clipId, const int oldEnd, const int newEnd) const;
    // plain-click actions of the non-select tools (also reused by the
    // clip context menu)
    void razorCutAt(const QPoint &pos, const int clipId,
                    const bool allTracks);
    // 代理剪辑：选中集视频块排队转码（右键菜单入口）
    void generateProxies();
    // 达芬奇场景检测：选中集视频块排队 ffmpeg scene 扫描，完成
    // 回调换算时间轴帧逐刀分割
    void sceneDetectSelected();
    // 达芬奇 Fairlight 响度标准化：选中集声音块（含内嵌音频的
    // 视频块）volumedetect 测响度，音量动画器拉到目标值
    void normalizeLoudnessSelected();
    void trackSelectAt(const QPoint &pos, const int clipId,
                       const bool backward);
    void pruneMediaCaches();

    // ---- track header interactions ----
    QRect addTrackRect(const bool audio) const;  // corner +V / +A buttons
    QRect muteBadgeRect(const int trackIdx) const;
    QRect lockBadgeRect(const int trackIdx) const;
    QRect soloBadgeRect(const int trackIdx) const;
    int nearestTrackOfType(const int y, const bool audio) const;
    void renameTrackDialog(const int trackIdx);
    // end an editing gesture: flush candidates into one model commit
    // (insert-mode push-aside + magnetic follow rides along), then
    // reset the gesture state
    void finishGestureCommit(const bool insertMode);

    NleTimelineModel * const mModel;

    // ---- view state ----
    double mPxPerFrame = 2.4;  // zoom (px per frame, fps-dependent)
    int mScrollFrame = 0;      // left edge frame
    int mPlayheadFrame = 50;
    QVector<QPair<int, QString>> mMarkers; // abs frame, title
    int mRangeIn = -1;         // scene in/out band (frames, <0 = off)
    int mRangeOut = -1;
    QHash<int, int> mHeightPreview; // track height gesture preview

    // ---- editing tools ----
    EditTool mTool = EditTool::Select;
    QCursor mRazorCursor;      // procedural blade cursor, built once

    // ---- gesture state (candidates only - the model stays pristine
    // and doubles as the press-time snapshot) ----
    // a press first parks its intended gesture in mPending; only
    // moving past DRAG_THRESHOLD_PX promotes it to a live mDrag
    // (kdenlive MouseArea drag.threshold semantics) - a plain click
    // selects and nothing else, no micro-jitter rearrangement
    DragMode mDrag = DragMode::None;
    DragMode mPending = DragMode::None;
    bool mPendingRoll = false;  // Ctrl at press: trim as a kdenlive roll
    bool mRoll = false;         // active gesture trims as a roll
    void activatePendingDrag();
    int mDragClipId = -1;      // primary clip of the gesture
    int mGrabOffsetFrames = 0; // move: cursor frame - clip start
    QHash<int, NleTimelineModel::Move> mCandidates;
    QSet<int> mMovingIds;      // selection riding the gesture
    QSet<int> mTangled;        // press-time exemption set
    struct Snap { int clipId; int start; };
    QVector<Snap> mSpacerOrig; // spacer press snapshot (ids + starts)
    int mSpacerPressFrame = 0;
    bool mDropIllegal = false; // live overlap feedback during a move
    int mSnapTarget = -1;      // snap guide frame, -1 = none
    bool mGhostLane = false;   // drop targets a to-be-created track
    bool mGhostLaneAudio = false;
    bool mGhostLaneTop = false; // drag-into-the-ruler grows the group at its top
    QPoint mPressPos;
    int mDragTrackIdx = -1;    // TrackHeight gesture lane
    int mPressTrackHeight = 0;
    int mPanScroll = 0;        // Pan gesture press-time scroll frame
    // VolumePoint gesture: the animator + key under the press (the
    // animator owns the key, both stay alive for the gesture)
    QrealAnimator *mVolAnim = nullptr;
    QrealKey *mVolKey = nullptr;
    int mVolClipId = -1;
    // 块角淡入淡出拖拽态
    int mFadeClipId = -1;
    bool mFadeOut = false;      // false = 左上角淡入
    int mFadeLen = 0;           // 拖拽实时长度（帧）
    int mFadeStartLen = -1;     // 按下时已有淡变（-1 = 无）
    int mFadePressX = 0;

    int mHoverId = -1;
    QPoint mHoverPos;          // live cursor pos for the razor guide
    // 转场选中（= 右块 clipId）+ 窗口拖拽态：按下窗口帧数/起始 x、
    // 拖拽中的预览窗口（-1 = 无预览）、按住交界左/右半决定拖拽方向
    int mSelTransition = -1;
    int mTransDragFrames = 0;
    int mTransDragX = 0;
    int mTransPreviewN = -1;
    int mTransDragDir = 1;
    // 每帧绘制前刷新：转场右块 clipId -> 交界帧（左块出点）。
    // 绘制右块时可见左缘推到交界（重叠头部不画），两块视觉贴邻、
    // 转场条骑在交界上（CapCut 布局）
    QHash<int, int> mTransCutCache;
    bool mRubber = false;      // rubber band selection in progress
    QPoint mRubberStart;
    bool mInsertMode = false;  // kdenlive insert/overwrite default
    QString mLastFeedback;     // dedupe live drag TC status messages
    void feedback(const QString &msg);
    // double-dispatch dedup for KFT keys (same event object arrives
    // through two propagation chains, identical timestamp)
    ulong mLastKftTs = 0;
    int mLastKftKey = 0;

    QScrollBar *mScrollBar = nullptr;
    // media caches (clip ids are stable across rebuilds, so these
    // survive model changes)
    QHash<QString, QPixmap> mThumbCache;   // procedural placeholder tiles
    QHash<int, QImage> mRealThumbs;        // clipId -> rendered frame
    QHash<int, QPixmap> mRealScaled;       // clipId -> height-matched pm
    QHash<int, QMap<int, QImage>> mFilm;   // clipId -> abs frame -> tile
    QHash<int, QHash<int, QVector<qreal>>> mWaves; // clipId -> sec -> peaks
    QHash<int, QString> mAudioTags;        // clipId -> bitrate tag

    // theme
    QColor cBg       {0x1b,0x1b,0x1b};
    QColor cBgAlt    {0x22,0x22,0x22};
    QColor cRuler    {0x1d,0x1d,0x1d};
    QColor cHeader   {0x24,0x24,0x26};
    QColor cGridLine {0x2c,0x2c,0x2c};
    QColor cText     {0xc8,0xc8,0xc8};
    QColor cTextDim  {0x77,0x77,0x77};
    QColor cAccent   {0x08,0xa5,0x81}; // selection/highlight (theme)
    QColor cPlayhead {0xe8,0x4c,0x4c};
    QColor cVideoBar {0x0e,0x7d,0x6c}; // clip name bar (theme dark)
    QColor cAudioBody{0x1d,0x33,0x52};
    QColor cAudioWave{0x4f,0x8f,0xd6};
    void refreshThemeColors();
};

#endif // NLETIMELINEVIEW_H
