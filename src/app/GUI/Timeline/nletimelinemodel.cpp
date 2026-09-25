#include "nletimelinemodel.h"
#include "Sound/soundcomposition.h"

#include "Private/document.h"
#include "canvas.h"
#include "Boxes/containerbox.h"
#include "Boxes/boundingbox.h"
#include "Animators/eboxorsound.h"
#include "Sound/esound.h"
#include "Sound/eindependentsound.h"
#include "Boxes/videobox.h"
#include "Sound/evideosound.h"
#include "Timeline/durationrectangle.h"
#include "Timeline/fixedlenanimationrect.h"
#include "Boxes/animationbox.h"
#include "clipboardcontainer.h"
#include "RasterEffects/rastereffect.h"
#include "RasterEffects/rastereffectcollection.h"
#include "Animators/qrealanimator.h"
#include "smartPointers/ememory.h"

#include <QDebug>
#include <algorithm>
#include <climits>

namespace {
// CapCut-style clip timecode hh:mm:ss:ff (freeze-frame clip names,
// paste feedback)
QString frameToTimecode(const int frame, const qreal fps)
{
    const int fpc = fps > 0. ? qMax(1, qRound(fps)) : 25;
    const int f = qMax(0, frame);
    return QStringLiteral("%1:%2:%3:%4")
            .arg(f / (3600 * fpc), 2, 10, QLatin1Char('0'))
            .arg((f / (60 * fpc)) % 60, 2, 10, QLatin1Char('0'))
            .arg((f / fpc) % 60, 2, 10, QLatin1Char('0'))
            .arg(f % fpc, 2, 10, QLatin1Char('0'));
}

bool isTransitionType(const RasterEffectType t)
{
    switch (t) {
    case RasterEffectType::TRANSITION_DISSOLVE:
    case RasterEffectType::TRANSITION_FLASH:
    case RasterEffectType::TRANSITION_SLIDE:
    case RasterEffectType::TRANSITION_WIPE_CIRCLE:
    case RasterEffectType::TRANSITION_WIPE_LINEAR:
    case RasterEffectType::TRANSITION_BLINDS:
    case RasterEffectType::TRANSITION_NOISE:
    case RasterEffectType::TRANSITION_BLUR:
    case RasterEffectType::TRANSITION_ZOOM:
    case RasterEffectType::TRANSITION_MOSAIC:
    case RasterEffectType::TRANSITION_SPIN:
    case RasterEffectType::TRANSITION_MIRROR_FLIP:
    case RasterEffectType::TRANSITION_TWIRL:
    case RasterEffectType::TRANSITION_GLITCH:
    case RasterEffectType::TRANSITION_SHAKE:
    case RasterEffectType::TRANSITION_ZOOM_BLUR:
    case RasterEffectType::TRANSITION_DIR_BLUR:
    case RasterEffectType::TRANSITION_CHANNEL_SPLIT:
    case RasterEffectType::TRANSITION_HALFTONE:
    case RasterEffectType::TRANSITION_EDGE:
    case RasterEffectType::TRANSITION_INVERT:
    case RasterEffectType::TRANSITION_POSTERIZE:
    case RasterEffectType::TRANSITION_VIGNETTE:
    case RasterEffectType::TRANSITION_LIGHT_SWEEP:
    case RasterEffectType::TRANSITION_FILM_GRAIN:
    case RasterEffectType::TRANSITION_PAGE_FLIP:
        return true;
    default:
        return false;
    }
}

// 层上的转场特效（一个层最多一个：转场右块标识）
RasterEffect* transitionEffectOn(BoundingBox * const box)
{
    if (!box) { return nullptr; }
    const auto coll = box->rasterEffectsCollection();
    if (!coll) { return nullptr; }
    const int n = coll->ca_getNumberOfChildren();
    for (int i = 0; i < n; i++) {
        const auto re = enve_cast<RasterEffect*>(coll->ca_getChildAt(i));
        if (re && isTransitionType(re->getEffectType())) { return re; }
    }
    return nullptr;
}

// 转场特效的入窗口动画师：四个转场特效的第一个子属性恒为入窗口
// QrealAnimator（淡入时长/滑入时长），出窗口/颜色/方向随后
QrealAnimator* transitionInAnimator(RasterEffect * const re)
{
    if (!re) { return nullptr; }
    const int n = re->ca_getNumberOfChildren();
    for (int i = 0; i < n; i++) {
        const auto qa = enve_cast<QrealAnimator*>(re->ca_getChildAt(i));
        if (qa) { return qa; }
    }
    return nullptr;
}

int transitionInValue(RasterEffect * const re)
{
    const auto qa = transitionInAnimator(re);
    if (!qa) { return 0; }
    return qMax(1, qRound(qa->getEffectiveValue(0)));
}
}

NleTimelineModel::NleTimelineModel(Document &document,
                                   QObject * const parent)
    : QObject(parent)
    , mDocument(document)
{
    connect(&mDocument, qOverload<Canvas*>(&Document::sceneCreated),
            this, &NleTimelineModel::refreshFromDocument);
    connect(&mDocument, qOverload<Canvas*>(&Document::sceneRemoved),
            this, &NleTimelineModel::refreshFromDocument);
    connect(&mDocument, &Document::activeSceneSet,
            this, &NleTimelineModel::refreshFromDocument);
}

// ---------------------------------------------------------------- state

const NleTimelineModel::Clip *NleTimelineModel::clip(const int clipId) const
{
    for (const auto &c : mClips) {
        if (c.clipId == clipId) { return &c; }
    }
    return nullptr;
}

int NleTimelineModel::trackIndex(const int trackId) const
{
    for (int i = 0; i < mTracks.size(); ++i) {
        if (mTracks.at(i).id == trackId) { return i; }
    }
    return -1;
}

const NleTimelineModel::Track *NleTimelineModel::track(const int trackId) const
{
    const int idx = trackIndex(trackId);
    return idx < 0 ? nullptr : &mTracks.at(idx);
}

QList<int> NleTimelineModel::clipIdsOnTrack(const int trackId) const
{
    QList<QPair<int, int>> byStart; // start, id
    for (const auto &c : mClips) {
        if (c.trackId == trackId) { byStart.append({c.start, c.clipId}); }
    }
    std::sort(byStart.begin(), byStart.end());
    QList<int> ids;
    for (const auto &p : byStart) { ids << p.second; }
    return ids;
}

bool NleTimelineModel::trackLocked(const int trackId) const
{
    const auto t = track(trackId);
    return t ? t->locked : false;
}

int NleTimelineModel::mainTrackId() const
{
    // PERSISTENT designation (eTrackSpec.mMain): adding or removing
    // lanes never moves the main track. An unflagged table (legacy
    // project, first open) falls back to the bottom-most video lane
    // until the refresh designates it once
    const auto scene = mPanelScene.data();
    if (scene) {
        for (const auto &s : scene->getTrackSpecs()) {
            if (!s.mAudio && s.mMain) { return s.mId; }
        }
    }
    for (int i = mTracks.size() - 1; i >= 0; --i) {
        if (!mTracks.at(i).audio) { return mTracks.at(i).id; }
    }
    return -1;
}

int NleTimelineModel::minClipFrames() const
{
    return qMax(1, qRound(0.2 * mFps));
}

Canvas *NleTimelineModel::panelScene() const
{
    return mPanelScene.data();
}

// ------------------------------------------------- validation & planning

bool NleTimelineModel::rangesOverlap(const int aStart, const int aDur,
                                     const int bStart, const int bDur)
{
    return aStart < bStart + bDur && bStart < aStart + aDur;
}

bool NleTimelineModel::overlapOutside(const int trackId,
                                      const int start, const int dur,
                                      const QSet<int> &moving,
                                      const QSet<int> &exempt) const
{
    for (const auto &o : mClips) {
        if (moving.contains(o.clipId)) { continue; }
        if (exempt.contains(o.clipId)) { continue; }
        if (o.trackId != trackId) { continue; }
        if (rangesOverlap(start, dur, o.start, o.duration)) { return true; }
    }
    return false;
}

QSet<int> NleTimelineModel::collectTangleExemptions(
        const QSet<int> &moving) const
{
    QSet<int> tangled;
    for (const auto &o : mClips) {
        if (moving.contains(o.clipId)) { continue; }
        for (const auto &m : mClips) {
            if (!moving.contains(m.clipId)) { continue; }
            if (m.trackId != o.trackId) { continue; }
            if (rangesOverlap(m.start, m.duration, o.start, o.duration)) {
                tangled.insert(o.clipId);
                break;
            }
        }
    }
    return tangled;
}

void NleTimelineModel::trimBounds(const int clipId,
                                  const QSet<int> &moving,
                                  const QSet<int> &exempt,
                                  int *lo, int *hi) const
{
    const auto c = clip(clipId);
    if (!c) { return; }
    int l = 0;
    int h = INT_MAX;
    for (const auto &o : mClips) {
        if (o.clipId == clipId || moving.contains(o.clipId)) { continue; }
        if (exempt.contains(o.clipId)) { continue; }
        if (o.trackId != c->trackId) { continue; }
        const int oEnd = o.start + o.duration;
        if (oEnd <= c->start) { l = qMax(l, oEnd); }
        if (o.start >= c->start) { h = qMin(h, o.start); }
    }
    if (lo) { *lo = l; }
    if (hi) { *hi = h; }
}

QVector<NleTimelineModel::Move> NleTimelineModel::insertShiftPlan(
        const int targetTrackId, const int dropStart, const int dropDur,
        const QSet<int> &moving) const
{
    QVector<Move> moves;
    // the run = every same-track clip the drop touches (or that starts
    // right of it); it slides right as one rigid block keeping its
    // internal spacing
    int firstStart = -1;
    for (const auto &o : mClips) {
        if (moving.contains(o.clipId)) { continue; }
        if (o.trackId != targetTrackId) { continue; }
        if (o.start + o.duration <= dropStart) { continue; } // fully left
        firstStart = firstStart < 0 ? o.start : qMin(firstStart, o.start);
    }
    const int dropEnd = dropStart + dropDur;
    if (firstStart < 0 || firstStart >= dropEnd) { return moves; }
    const int shift = dropEnd - firstStart;
    for (const auto &o : mClips) {
        if (moving.contains(o.clipId)) { continue; }
        if (o.trackId != targetTrackId) { continue; }
        if (o.start >= firstStart) {
            moves.append({o.clipId, o.trackId, o.start + shift, o.duration});
        }
    }
    return moves;
}

QVector<NleTimelineModel::Move> NleTimelineModel::compactGapsPlan() const
{
    // MAIN TRACK ONLY (CapCut): per start order a clip that begins
    // after the previous one ends slides left onto that out point;
    // overlays keep their positions. Overlaps (merged layouts) are
    // kept as-is, so only genuine gaps close. 用户规则：主轨首块
    // 恒靠左对齐时间起点（帧 1/时间 00），拖走的头块随刷新被
    // 拉回，刻意的开头空隙在主轨不存在
    const int mainId = mainTrackId();
    QVector<Move> moves;
    for (const auto &t : mTracks) {
        if (t.id != mainId) { continue; }
        QList<QPair<int, int>> order; // start, clipId
        for (const auto &c : mClips) {
            if (c.trackId == t.id) { order.append({c.start, c.clipId}); }
        }
        // 单块轨也要走：块数 >= 2 的早退会让"只剩一块"的主轨
        // 永远不校验头块归帧 1——删除链头后的缺口拖到下一次
        // 恰好路过的刷新才闭合（用户感知=磁吸刷新迟钝/不刷新）
        if (order.isEmpty()) { continue; }
        std::sort(order.begin(), order.end());
        int cursor = 0; // CapCut rule: the chain head always sits at
                        // frame 1 (time 00) - the fallback compaction
                        // pulls a strayed head back too
        for (const auto &p : order) {
            const auto c = clip(p.second);
            if (!c) { continue; }
            // 转场右块允许沉入游标左侧的转场重叠窗口
            const int res = transitionWindowOf(*c);
            int newStart = c->start;
            const int lim = qMax(0, cursor - res);
            if (c->start > lim) { newStart = lim; }
            if (newStart != c->start) {
                moves.append({c->clipId, c->trackId, newStart, c->duration});
            }
            cursor = qMax(cursor, newStart + c->duration);
        }
    }
    return moves;
}

// ---------------------------------------------------------------- selection

bool NleTimelineModel::isSelected(const int clipId) const
{
    return mSelected.contains(clipId);
}

void NleTimelineModel::setSelection(const QSet<int> &ids)
{
    if (mSelected == ids) { return; }
    mSelected = ids;
    emit selectionChanged();
}

void NleTimelineModel::addToSelection(const QSet<int> &ids)
{
    if (ids.isEmpty()) { return; }
    mSelected.unite(ids);
    emit selectionChanged();
}

void NleTimelineModel::toggleInSelection(const int clipId)
{
    if (mSelected.contains(clipId)) { mSelected.remove(clipId); }
    else { mSelected.insert(clipId); }
    emit selectionChanged();
}

void NleTimelineModel::clearSelection()
{
    if (mSelected.isEmpty()) { return; }
    mSelected.clear();
    emit selectionChanged();
}

void NleTimelineModel::selectAll()
{
    QSet<int> ids;
    for (const auto &c : mClips) { ids.insert(c.clipId); }
    setSelection(ids);
}

// ---------------------------------------------------------------- gestures

void NleTimelineModel::setGestureActive(const bool active)
{
    if (mGestureActive == active) { return; }
    mGestureActive = active;
    if (!active && mRebuildQueued) {
        mRebuildQueued = false;
        refreshFromDocument();
    }
}

// ---------------------------------------------------------------- commit

void NleTimelineModel::setLayerRange(eBoxOrSound * const layer,
                                     const int start, const int duration)
{
    if (!layer) { return; }
    const auto dur = layer->getDurationRectangle();
    if (!dur) { return; }
    const int newMin = start;
    const int newMax = start + duration - 1;
    const int oldMin = dur->getMinAbsFrame();
    const int oldMax = dur->getMaxAbsFrame();
    // rel/abs shift is constant, so abs deltas are valid rel moves.
    // Order by direction (shiftLayer's rule): moving min first on a
    // right shift crosses min past max and the illegal-state clamp
    // pins it back to the OLD max - the clip then spans the wrong
    // range with an inflated duration (drag release looked like the
    // clip vanished into a wrong span)
    const auto moveMin = [&dur, newMin, oldMin]() {
        if (newMin == oldMin) { return; }
        dur->startMinFramePosTransform();
        dur->moveMinFrame(newMin - oldMin);
        dur->finishMinFramePosTransform();
    };
    const auto moveMax = [&dur, newMax, oldMax]() {
        if (newMax == oldMax) { return; }
        dur->startMaxFramePosTransform();
        dur->moveMaxFrame(newMax - oldMax);
        dur->finishMaxFramePosTransform();
    };
    if (newMin > oldMin || newMax > oldMax) { moveMax(); moveMin(); }
    else { moveMin(); moveMax(); }
    // 纯移动（时长不变）携动 FixedLen 的源映射锚。只挪 min/max 而
    // animMin 不动，块内 relFrame 会整段落在 anim 范围外：
    // getAnimationFrameForRelFrame 恒被钳到尾帧（画面冻结在最后一源
    // 帧），且 AnimationBox::prp_getIdenticalRelRange 走范围外分支返
    // 回整条剪辑——渲染数据跨帧复用（转场特效的位移烘焙在渲染数据
    // 里，随重渲时机乱跳=画面闪烁）。块首恒显源帧 in 的不变量 =
    // animMin == minRel - in：平移时两侧同加 δ 即保；修剪（时长变
    // 化）不动锚=CapCut 左修剪跳过源头的语义
    if (newMin != oldMin && newMax - newMin == oldMax - oldMin) {
        const auto flar = dur->ref<FixedLenAnimationRect>();
        if (flar) {
            flar->setFirstAnimationFrame(
                        flar->getMinAnimRelFrame() + (newMin - oldMin));
        }
    }
}

void NleTimelineModel::shiftLayer(eBoxOrSound * const layer,
                                  const int frameDelta)
{
    if (!layer || frameDelta == 0) { return; }
    const auto dur = layer->getDurationRectangle();
    if (!dur) { return; }
    // 整体平移动 relShift 而非 min/max（等价偏移：画面源帧映射
    // pixId = abs - relShift - animMinRel，两种实现逐帧相同），但
    // 声音链（eSound 的 getSampleShift 放置位置与 absSecondToRel-
    // Seconds 源映射）只认 relShift——旧 moveMin/moveMax 版本平移
    // 后画面走了声音不走，压实后音频与画面错开整段位移，播放被
    // 删区间听到的是平移块旧位置的声音（“删除的部分依然在播”
    // 的根因）。只动 relShift 也永无 min>max 非法态
    dur->startPosTransform();
    dur->changeFramePosBy(frameDelta);
    dur->finishPosTransform();
}

bool NleTimelineModel::commitMoves(const QVector<Move> &moves,
                                   const bool compactAfter)
{
    const auto scene = mPanelScene.data();
    if (!scene || mFps <= 0. || moves.isEmpty()) { return false; }

    // working copy with the moves applied: compaction (if requested)
    // plans on the post-move state so one commit carries both
    QVector<Move> all = moves;
    QVector<Clip> work = mClips;
    bool anyKnown = false;
    const auto applyToWork = [&work](const Move &m) {
        for (auto &c : work) {
            if (c.clipId != m.clipId) { continue; }
            c.trackId = m.trackId;
            c.start = qMax(0, m.start);
            c.duration = qMax(1, m.duration);
            return true;
        }
        return false;
    };
    for (const auto &m : all) { anyKnown = applyToWork(m) || anyKnown; }
    if (!anyKnown) { return false; }
    // 有时长变化的提交（右缘修剪收短等）允许范围收缩评估
    for (const auto &m : all) {
        const auto oc = clip(m.clipId);
        if (oc && oc->duration != m.duration) { mCheckRangeShrink = true; break; }
    }
    if (compactAfter && mMagnetic) {
        // compactGapsPlan over the working table - MAIN TRACK ONLY
        // (CapCut): overlays never get sucked in
        const int mainId = mainTrackId();
        for (const auto &t : mTracks) {
            if (t.id != mainId) { continue; }
            QList<QPair<int, const Clip*>> order;
            for (const auto &c : work) {
                if (c.trackId == t.id) { order.append({c.start, &c}); }
            }
            if (order.size() < 2) { continue; }
            std::sort(order.begin(), order.end(),
                      [](const auto &a, const auto &b) {
                          if (a.first != b.first) { return a.first < b.first; }
                          return a.second->clipId < b.second->clipId;
                      });
            int cursor = -1;
            for (const auto &p : order) {
                const Clip *c = p.second;
                // 转场右块允许沉入游标左侧的转场重叠窗口
                const int res = transitionWindowOf(*c);
                int newStart = c->start;
                const int lim = cursor >= 0 ? qMax(0, cursor - res)
                                            : c->start;
                if (cursor >= 0 && c->start > lim) { newStart = lim; }
                if (newStart != c->start) {
                    const Move m{c->clipId, c->trackId, newStart, c->duration};
                    all.append(m);
                    applyToWork(m);
                }
                cursor = qMax(cursor, newStart + c->duration);
            }
        }
    }

    // CapCut overlay following: clips on non-main video tracks ride
    // the main-track block their head sits on (anchor resolved in the
    // OLD layout, delta from this move set). Computed BEFORE any
    // write so the table is still pristine. The 轨道联动 toggle
    // switches the whole behaviour off (overlays stay put)
    {
        const int mainId = mFollowLinked ? mainTrackId() : -1;
        if (mainId >= 0) {
            QHash<int, int> deltas; // main clipId -> delta
            QSet<int> movingIds;
            for (const auto &m : all) {
                movingIds.insert(m.clipId);
                const auto oc = clip(m.clipId);
                if (oc && oc->trackId == mainId) {
                    deltas.insert(m.clipId, m.start - oc->start);
                }
            }
            if (!deltas.isEmpty()) {
                QVector<Move> rides;
                // 剪映联动含音效：音频块与其他覆盖块一样跟随其头部
                // 所锚的主轨块同位移
                for (const auto &o : mClips) {
                    if (o.trackId == mainId) { continue; }
                    if (movingIds.contains(o.clipId)) { continue; }
                    if (!o.layer) { continue; }
                    const Clip *anchor = nullptr;
                    for (const auto &mc : mClips) {
                        if (mc.trackId != mainId) { continue; }
                        if (mc.start <= o.start &&
                                o.start < mc.start + mc.duration) {
                            anchor = &mc;
                            break;
                        }
                    }
                    if (!anchor) { continue; }
                    const int delta = deltas.value(anchor->clipId, 0);
                    if (delta == 0) { continue; }
                    const int ns = qMax(0, o.start + delta);
                    if (ns != o.start) {
                        rides.append({o.clipId, o.trackId, ns, o.duration});
                    }
                }
                all += rides;
            }
        }
    }

    for (const auto &m : all) {
    }
    mInWriteback = true;
    for (const auto &m : all) {
        const auto orig = clip(m.clipId);
        if (!orig || !orig->layer) { continue; }
        auto * const layer = orig->layer.data();
        // track type is a hard constraint on every write
        const auto tr = track(m.trackId);
        if (!tr || tr->audio != orig->audio) { continue; }
        if (layer->trackId() != m.trackId) { layer->setTrackId(m.trackId); }
        setLayerRange(layer, m.start, m.duration);
    }
    // the row reorder must stay inside the writeback guard: its
    // movedObject signal would refresh mid-commit
    stabilizeRowOrder();
    // CapCut track lifecycle: a lane that lost its last clip goes
    // away. Membership comes from the RESOLVED table (parking
    // included): freshly imported layers carry trackId -1 and only
    // PARK on a lane - raw-id counting would call their lane empty
    // and delete it out from under them
    QHash<int, int> members;
    for (const auto &wc : work) { members[wc.trackId]++; }
    purgeEmptyTracks(members);
    mInWriteback = false;

    finishAction();
    return true;
}

void NleTimelineModel::stabilizeRowOrder()
{
    const auto scene = mPanelScene.data();
    if (!scene || mTracks.isEmpty()) { return; }

    // keep each track's members contiguous with the tracks in spec
    // order (the compositing-order invariant), but keep the CURRENT
    // relative order within a track - a same-lane time slide
    // reorders nothing. Reorder only when membership moved
    for (const bool isAudio : {false, true}) {
        QList<eBoxOrSound*> members; // in current contained order
        for (const auto &c : scene->getContained()) {
            const auto l = c.data();
            if (!l) { continue; }
            const bool audio = enve_cast<eSound*>(l) != nullptr;
            if (audio != isAudio) { continue; }
            members << l;
        }
        if (members.size() < 2) { continue; }
        QList<eBoxOrSound*> desired = members;
        std::stable_sort(desired.begin(), desired.end(),
                [this](eBoxOrSound * const a, eBoxOrSound * const b) {
            return trackIndex(a->trackId()) < trackIndex(b->trackId());
        });
        bool changed = false;
        for (int i = 0; i < members.size(); ++i) {
            if (members.at(i) != desired.at(i)) { changed = true; break; }
        }
        if (!changed) { continue; }
        const auto parent = members.first()->getParentGroup();
        if (!parent || parent->getParentScene() != scene) { continue; }
        int minIdx = INT_MAX;
        bool stale = false;
        for (const auto *m : members) {
            const int idx = parent->getContainedIndex(
                        const_cast<eBoxOrSound*>(m));
            if (idx < 0) { stale = true; break; }
            minIdx = qMin(minIdx, idx);
        }
        if (stale || minIdx == INT_MAX) { continue; }
        parent->moveContainedInList(desired.first(), minIdx);
        for (int i = 1; i < desired.size(); ++i) {
            parent->moveContainedBelow(desired.at(i), desired.at(i - 1));
        }
    }
}

void NleTimelineModel::purgeEmptyTracks(const QHash<int, int> &members)
{
    const auto scene = mPanelScene.data();
    if (!scene) { return; }
    // the designated main lane is NEVER purged - it is the fixed
    // primary track and also guarantees the video-type invariant
    const int mainId = mainTrackId();
    const auto specs = scene->getTrackSpecs();
    QString memberDump;
    for (const auto &s : specs) {
        memberDump += QStringLiteral("%1:%2 ").arg(s.mId).arg(
                    members.value(s.mId, 0));
    }
    qInfo("[TRK] purge scan main=%d members[%s]", mainId,
          qUtf8Printable(memberDump.trimmed()));
    for (int type = 0; type < 2; ++type) {
        const bool audio = type == 1;
        int withMembers = 0;
        for (const auto &s : specs) {
            if (s.mAudio == audio &&
                    members.value(s.mId, 0) > 0) { ++withMembers; }
        }
        // every lane of this type empty: keep them all (the
        // one-lane-per-type invariant guards fresh projects)
        if (withMembers == 0) { continue; }
        for (const auto &s : specs) {
            if (s.mAudio == audio && members.value(s.mId, 0) == 0 &&
                    s.mId != mainId) {
                qInfo("[TRK] purge lane id=%d name=%s", s.mId,
                      qUtf8Printable(s.mName));
                scene->removeTrackSpec(s.mId);
            }
        }
    }
}

void NleTimelineModel::finishAction()
{
    if (Document::sInstance) { Document::sInstance->actionFinished(); }
    refreshFromDocument();
}

// ---------------------------------------------------------------- doc ops

bool NleTimelineModel::splitBoxes(const QList<BoundingBox*> &boxes,
                                  const int frame)
{
    const auto scene = mPanelScene.data();
    if (!scene || boxes.isEmpty()) { return false; }
    // 分割=粘贴+两侧 durRect 修剪的多步写入：中途每一次写入都会经
    // minRelFrameChanged 同步触发 refreshFromDocument，而 refresh 尾
    // 部的磁吸兜底/孤儿回贴会在“写了一半”的表上做布局写入（原件或
    // 拷贝被兜底 shift 拉走、又被下一笔修剪拉回），终态随事件交错
    // 随机——连切几刀后块长漂移、边界处掉出 1 帧碎屑串的根因。写回
    // 守卫抑制全部中途刷新，finishAction 收尾一次性重建终态
    mInWriteback = true;
    scene->clearBoxesSelection();
    for (auto *b : boxes) { scene->addBoxToSelection(b); }
    scene->splitBoxesAtFrame(frame);
    mInWriteback = false;
    finishAction();
    return true;
}

bool NleTimelineModel::requestSplitAtFrame(const int frame)
{
    if (mInWriteback || mGestureActive) { return false; }
    const auto scene = mPanelScene.data();
    if (!scene || mFps <= 0.) { return false; }
    // the split operates at the scene's current frame: park the panel
    // scene on the requested frame first (active scenes route through
    // the document so the change propagates everywhere)
    if (scene == mDocument.fActiveScene.data()) {
        if (frame != mDocument.getActiveSceneFrame()) {
            mDocument.setActiveSceneFrame(frame);
        }
    } else if (frame != scene->anim_getCurrentAbsFrame()) {
        scene->anim_setAbsFrame(frame);
    }

    QList<BoundingBox*> boxes;
    QList<eIndependentSound*> sounds;
    int skippedEdges = 0;
    // 切点须严格落在块内部（两侧各留至少一帧）：与剪刀路径同款守卫。
    // 旧条件 start <= frame 允许在块头/块尾切——块尾切会把原件写成
    // [end+1..end] 的非法倒置矩形（钳回后成 1 帧重复块），块头切出
    // 1 帧碎片；这些退化半块不渲染缩略图（IMGDRAW 空图黑块）且把
    // 主轨拆得支离破碎
    const auto insideCut = [](const Clip * const c, const int f) {
        return c->start < f && f < c->start + c->duration - 1;
    };
    for (const int id : mSelected) {
        const auto c = clip(id);
        if (!c || !c->layer) { continue; }
        if (!insideCut(c, frame)) { ++skippedEdges; continue; }
        const auto snd = enve_cast<eIndependentSound*>(c->layer.data());
        if (snd) { sounds << snd; continue; }
        const auto box = enve_cast<BoundingBox*>(c->layer.data());
        if (box) { boxes << box; }
    }
    // CapCut-style fallback: no selection cuts EVERY unlocked-track
    // clip under the frame in one press (sounds included)
    if (boxes.isEmpty() && sounds.isEmpty()) {
        for (const auto &c : mClips) {
            if (!insideCut(&c, frame)) { continue; }
            if (trackLocked(c.trackId)) { continue; }
            if (!c.layer) { continue; }
            const auto snd = enve_cast<eIndependentSound*>(c.layer.data());
            if (snd) { sounds << snd; continue; }
            const auto box = enve_cast<BoundingBox*>(c.layer.data());
            if (box) { boxes << box; }
        }
    }
    if (skippedEdges > 0) {
        emit logMessage(QStringLiteral("%1 块在切点边缘，无需分割")
                                .arg(skippedEdges));
    }
    if (boxes.isEmpty() && sounds.isEmpty()) {
        qInfo("[NLE] split frame=%d refused edge/empty sel=%d",
              frame, int(mSelected.size()));
        return false;
    }
    // 转场窗口内禁分割：窗口帧同时是左块尾与右块头（CapCut 同款禁区）
    for (const auto &t : transitions()) {
        if (frame >= t.start && frame <= t.end) {
            emit logMessage(QStringLiteral("转场区域内不能分割，先删除或移开转场"));
            return false;
        }
    }
    // 分割转场右块：特效留左半（拷贝），右半（原件）剥掉
    QList<eBoxOrSound*> stripAfter;
    for (auto *box : boxes) {
        for (const auto &t : transitions()) {
            if (t.rightId == mLayerToClipId.value(box, -1)) {
                stripAfter << box;
            }
        }
    }
    bool did = false;
    if (!boxes.isEmpty()) { did = splitBoxes(boxes, frame) || did; }
    for (const auto l : stripAfter) { stripTransitionEffect(l); }
    if (!sounds.isEmpty()) {
        // 声音分割同款多步写入，同款守卫（理由同 splitBoxes）
        mInWriteback = true;
        scene->splitSoundsAtFrame(sounds, frame);
        mInWriteback = false;
        did = true;
    }
    qInfo("[NLE] split frame=%d boxes=%d sounds=%d edgeSkip=%d did=%d",
          frame, int(boxes.count()), int(sounds.count()),
          skippedEdges, int(did));
    return did;
}

bool NleTimelineModel::requestRazorCut(const QSet<int> &clipIds,
                                       const int frame)
{
    if (mInWriteback || mGestureActive) { return false; }
    const auto scene = mPanelScene.data();
    if (!scene || mFps <= 0.) { return false; }

    // kdenlive parity: the razor cuts every clip kind - boxes go
    // through the box clipboard, independent sounds through the
    // clone-and-trim sound split
    QList<BoundingBox*> boxes;
    QList<eIndependentSound*> sounds;
    int skippedEdges = 0;
    for (const int id : clipIds) {
        const auto c = clip(id);
        if (!c || !c->layer) { continue; }
        // the cut must leave at least one frame on both sides
        if (!(c->start < frame && frame < c->start + c->duration - 1)) {
            ++skippedEdges;
            continue;
        }
        const auto snd = enve_cast<eIndependentSound*>(c->layer.data());
        if (snd) { sounds << snd; continue; }
        const auto box = enve_cast<BoundingBox*>(c->layer.data());
        if (box) { boxes << box; }
    }
    if (skippedEdges > 0) {
        emit logMessage(QStringLiteral("%1 块在切点边缘，无需分割").arg(skippedEdges));
    }
    if (boxes.isEmpty() && sounds.isEmpty()) { return false; }
    // 转场窗口内禁分割：窗口帧同时是左块尾与右块头（CapCut 同款
    // 禁区），切开会在转场重叠区里产生无主半块
    for (const auto &t : transitions()) {
        if (frame >= t.start && frame <= t.end) {
            emit logMessage(QStringLiteral("转场区域内不能分割，先删除或移开转场"));
            return false;
        }
    }
    // 分割转场右块：特效留在左半（拷贝）继续贴左块，右半（原件）
    // 剥掉——半块头窗口对着普通帧淡入是错误视觉
    QList<eBoxOrSound*> stripAfter;
    for (auto *box : boxes) {
        for (const auto &t : transitions()) {
            if (t.rightId == mLayerToClipId.value(box, -1)) {
                stripAfter << box;
            }
        }
    }
    bool did = false;
    if (!boxes.isEmpty()) { did = splitBoxes(boxes, frame) || did; }
    for (const auto l : stripAfter) { stripTransitionEffect(l); }
    if (!sounds.isEmpty()) {
        // 声音分割同款多步写入，同款守卫（理由同 splitBoxes）
        mInWriteback = true;
        scene->splitSoundsAtFrame(sounds, frame);
        mInWriteback = false;
        did = true;
    }
    return did;
}

bool NleTimelineModel::requestFreeze(const QSet<int> &clipIds,
                                     const int frame)
{
    if (mInWriteback || mGestureActive) { return false; }
    const auto scene = mPanelScene.data();
    if (!scene || mFps <= 0.) { return false; }

    // video-family clips only: freezing is frame remapping
    QList<AnimationBox*> animBoxes;
    for (const int id : clipIds) {
        const auto c = clip(id);
        if (!c || !c->layer) { continue; }
        if (!(c->start < frame && frame < c->start + c->duration - 1)) { continue; }
        const auto box = enve_cast<BoundingBox*>(c->layer.data());
        if (!box) { continue; }
        const auto animBox = dynamic_cast<AnimationBox*>(box);
        if (animBox) { animBoxes << animBox; }
    }
    if (animBoxes.isEmpty()) {
        emit logMessage(QStringLiteral("定格仅支持视频/序列类块"));
        return false;
    }

    // freezeFrameAction freezes the CURRENT scene frame: park the
    // playhead on the cut frame for the freeze, then restore it
    const int savedFrame = scene->anim_getCurrentAbsFrame();
    scene->anim_setAbsFrame(frame);
    scene->clearBoxesSelection();
    for (auto *b : animBoxes) { scene->addBoxToSelection(b); }
    // split: the ORIGINAL boxes become the right halves [frame..max]
    scene->splitBoxesAtFrame(frame);
    const qreal fps = mFps;
    for (auto *b : animBoxes) {
        b->freezeFrameAction();
        // CapCut 视觉：定格段块名 = “定格 时码”
        b->prp_setName(QStringLiteral("定格 ") + frameToTimecode(frame, fps));
    }
    scene->anim_setAbsFrame(savedFrame);
    finishAction();
    return true;
}

bool NleTimelineModel::requestSpeed(const int clipId, const qreal rate)
{
    if (mInWriteback || mGestureActive) { return false; }
    const auto scene = mPanelScene.data();
    if (!scene) { return false; }
    const auto c = clip(clipId);
    if (!c || !c->layer) { return false; }
    const auto box = enve_cast<BoundingBox*>(c->layer.data());
    const auto animBox = box ? dynamic_cast<AnimationBox*>(box) : nullptr;
    if (!animBox) {
        emit logMessage(QStringLiteral("变速仅支持视频/序列类块"));
        return false;
    }
    // stretch > 1 = slower, so rate 2x maps to stretch 0.5; VideoBox
    // overrides the setter and carries its embedded audio along
    animBox->setStretch(1.0 / rate);
    finishAction();
    return true;
}

bool NleTimelineModel::requestDelete(const QSet<int> &clipIds,
                                     const bool ripple)
{
    if (mInWriteback || mGestureActive) {
        qInfo("[NLE-PRV] delete REFUSED: writeback=%d gesture=%d "
              "ids=%d", int(mInWriteback), int(mGestureActive),
              clipIds.count());
        return false;
    }
    mCheckRangeShrink = true;
    const auto scene = mPanelScene.data();
    if (!scene || mFps <= 0.) {
        qInfo("[NLE-PRV] delete REFUSED: no scene");
        return false;
    }

    QList<eBoxOrSound*> victims;
    QSet<int> victimIds;
    for (const int id : clipIds) {
        const auto c = clip(id);
        if (!c || !c->layer) { continue; }
        victims << c->layer.data();
        victimIds << id;
    }
    if (victims.isEmpty()) { return false; }

    // 转场清理：删除转场左块时同步剥掉右块的转场特效——否则右块
    // 头窗口对着新左邻（或黑场）淡入，视觉错误（CapCut：删块转场
    // 随之消失）。特效剥掉后 transitionWindowOf 归 0，下方闭链自然
    // 把右块拉回贴邻，无需位移补偿
    {
        const auto trans = transitions();
        for (const auto &t : trans) {
            if (!victimIds.contains(t.leftId)) { continue; }
            if (victimIds.contains(t.rightId)) { continue; }
            const auto rc = clip(t.rightId);
            if (rc && rc->layer) { stripTransitionEffect(rc->layer.data()); }
        }
    }

    if (ripple) {
        // per track: every later clip slides left by the total length
        // of the removed clips that started before it (exact for
        // chains, in frames)
        QSet<int> affectedTracks;
        for (const int id : victimIds) {
            const auto c = clip(id);
            if (c) { affectedTracks.insert(c->trackId); }
        }
        const int mainIdR = mainTrackId();
        for (const int trackId : affectedTracks) {
            // magnetic main track skips the classic slide: the
            // from-zero closure below subsumes it (both would
            // double-slide the chain)
            if (mMagnetic && trackId == mainIdR) { continue; }
            for (const auto &c : mClips) {
                if (victimIds.contains(c.clipId)) { continue; }
                if (c.trackId != trackId || !c.layer) { continue; }
                int removedBefore = 0;
                for (const int vid : victimIds) {
                    const auto v = clip(vid);
                    if (v && v->trackId == trackId && v->start < c.start) {
                        removedBefore += v->duration;
                    }
                }
                if (removedBefore > 0) { shiftLayer(c.layer.data(), -removedBefore); }
            }
        }
        // 非磁吸波纹的覆盖块随动（联动）：头部锚在被删主轨块上的
        // 块=锚已毁不随（落进相邻幸存块后自然重锚）；只统计完整
        // 位于头部之前的受害块，位移量恒 ≤ 头部帧号，配合钳制
        // 永不越到帧 0 之前（旧 victim 累计版会把锚中覆盖块推出
        // 负帧位——min 负值后表内 qMax(0) 掩盖成错位假象）
        if (!mMagnetic && mFollowLinked) {
            const int mainId = mainTrackId();
            if (mainId >= 0) {
                for (const auto &o : mClips) {
                    if (o.trackId == mainId ||
                            victimIds.contains(o.clipId) || !o.layer) {
                        continue;
                    }
                    int removedBefore = 0;
                    bool anchorLost = false;
                    for (const int vid : victimIds) {
                        const auto v = clip(vid);
                        if (!v || v->trackId != mainId) { continue; }
                        if (v->start <= o.start &&
                                o.start < v->start + v->duration) {
                            anchorLost = true;
                            break;
                        }
                        if (v->start + v->duration <= o.start) {
                            removedBefore += v->duration;
                        }
                    }
                    if (anchorLost) { continue; }
                    const int ns = qMax(0, o.start - removedBefore);
                    if (ns != o.start) {
                        shiftLayer(o.layer.data(), ns - o.start);
                    }
                }
            }
        }
        // 磁吸下的覆盖随动由下方从零闭链的锚定 rides 一次完成
        // （victim 累计版与闭链双移已废）
    }

    // CapCut magnetic deletion: close the main chain over the removed
    // span FROM ZERO (磁吸删除后接缝归位；主轨首块恒靠左对齐时间
    // 起点，删除空出的开头同样闭合). Overlaid clips (audio included)
    // ride their anchors by the same deltas when 轨道联动 is on.
    // Planned against the survivors of the PRE-removal table,
    // applied inside the writeback below
    QVector<Move> closure;
    QList<QPair<int, int>> closureRides; // clipId, newStart
    if (mMagnetic) {
        const int mainId = mainTrackId();
        if (mainId >= 0) {
            QList<QPair<int, int>> order; // start, clipId
            for (const auto &c : mClips) {
                if (c.trackId != mainId ||
                        victimIds.contains(c.clipId)) { continue; }
                order.append({c.start, c.clipId});
            }
            if (!order.isEmpty()) {
                std::sort(order.begin(), order.end());
                int cursor = 0;
                for (const auto &p : order) {
                    const auto c = clip(p.second);
                    if (!c) { continue; }
                    // 转场右块允许沉入游标左侧的转场重叠窗口
                    const int res = transitionWindowOf(*c);
                    int newStart = c->start;
                    const int lim = qMax(0, cursor - res);
                    if (c->start > lim) { newStart = lim; }
                    if (newStart != c->start) {
                        closure.append({c->clipId, c->trackId,
                                        newStart, c->duration});
                    }
                    cursor = qMax(cursor, newStart + c->duration);
                }
                if (mFollowLinked && !closure.isEmpty()) {
                    QHash<int, int> deltas;
                    for (const auto &m : closure) {
                        const auto oc = clip(m.clipId);
                        if (oc) { deltas.insert(m.clipId,
                                                m.start - oc->start); }
                    }
                    for (const auto &o : mClips) {
                        if (o.trackId == mainId ||
                                victimIds.contains(o.clipId) ||
                                !o.layer) { continue; }
                        const Clip *anchor = nullptr;
                        for (const auto &p : order) {
                            const auto mc = clip(p.second);
                            if (mc && mc->start <= o.start &&
                                    o.start < mc->start + mc->duration) {
                                anchor = mc;
                                break;
                            }
                        }
                        if (!anchor) { continue; }
                        const int delta = deltas.value(anchor->clipId, 0);
                        if (delta != 0) {
                            closureRides.append({o.clipId,
                                                 qMax(0, o.start + delta)});
                        }
                    }
                }
            }
        }
    }

    // 声音合成缓存失效（删除+压实平移的全景一次性收口）：
    // SoundComposition 的已合并秒缓存只在 removeSound 时失效，而
    // 删除走层析构路径不触发它（victim 的 removeSound 从未被调，
    // PRV5 打点实证）——旧混音残留，再播放秒缓存全命中，被删块
    // 的声音照响；压实平移的幸存声音同样只失效新位置。清掉删除
    // 前时间线全景覆盖的所有新旧秒，Merger 按现存声音表重并
    {
        const auto comp = scene->getSoundComposition();
        if (comp && !mClips.isEmpty()) {
            int minF = INT_MAX;
            int maxF = 0;
            for (const auto &c : mClips) {
                minF = qMin(minF, c.start);
                maxF = qMax(maxF, c.start + c.duration);
            }
            if (minF <= maxF) { comp->invalidateRange({minF, maxF}); }
        }
    }

    mInWriteback = true;
    for (auto *layer : victims) { layer->removeFromParent_k(); }
    // CapCut track lifecycle: lanes emptied by the deletion go away
    // (membership = resolved clip table minus the victims, parking
    // layers count for the lane they are displayed on)
    QHash<int, int> members;
    for (const auto &c : mClips) {
        if (!victimIds.contains(c.clipId)) { members[c.trackId]++; }
    }
    purgeEmptyTracks(members);
    if (!closure.isEmpty() || !closureRides.isEmpty()) {
        // UI-state tidy like the fallback compactor (not part of the
        // undo entry - the removal itself stays undoable)
        const auto undoBlock = scene->blockUndoRedo();
        for (const auto &m : closure) {
            const auto oc = clip(m.clipId);
            if (oc && oc->layer) {
                shiftLayer(oc->layer.data(), m.start - oc->start);
            }
        }
        for (const auto &r : closureRides) {
            const auto oc = clip(r.first);
            if (oc && oc->layer) {
                shiftLayer(oc->layer.data(), r.second - oc->start);
            }
        }
    }
    mInWriteback = false;
    // 诊断探针（常驻门控）：删除即武装画布绘制分支探针，落日志供
    // 真机取证；判定删除在文档层是否真发生
    {
        QStringList victimNames;
        for (const auto *layer : victims) {
            victimNames << (layer ? layer->prp_getName() : QStringLiteral("null"));
        }
        qInfo("[NLE-PRV] delete victims=%d [%s] containedAfter=%d "
              "stateId=%u ripple=%d",
              victims.count(), qUtf8Printable(victimNames.join(
                  QStringLiteral(","))),
              scene->getContained().count(), scene->getBoxStateId(),
              int(ripple));
        QString specDump;
        for (const auto &s : scene->getTrackSpecs()) {
            specDump += QStringLiteral("%1(%2,%3) ").arg(s.mId).arg(
                        s.mName, s.mAudio ? QStringLiteral("A")
                                          : QStringLiteral("V"));
        }
        qInfo("[TRK] specs after delete: %s",
              qUtf8Printable(specDump.trimmed()));
        scene->nlePrvArmPaintProbe(50);
    }
    finishAction();
    return true;
}

bool NleTimelineModel::requestMoveClipToTrack(const int clipId,
                                              const int dstTrackId)
{
    const auto c = clip(clipId);
    if (!c || !c->layer) { return false; }
    const auto dst = track(dstTrackId);
    if (!dst) { return false; }
    if (dst->audio != c->audio) {
        emit logMessage(QStringLiteral("目标轨道类型不符"));
        return false;
    }
    if (dst->locked) {
        emit logMessage(QStringLiteral("目标轨道已锁定"));
        return false;
    }
    const QSet<int> moving{clipId};
    if (overlapOutside(dstTrackId, c->start, c->duration, moving, {})) {
        emit logMessage(QStringLiteral("目标位置与现有块重叠"));
        return false;
    }
    // snapshot before commitMoves: it refreshes and the row/track
    // pointers would dangle
    const QString clipName = c->name;
    const QString trackName = dst->name;
    if (!commitMoves({{clipId, dstTrackId, c->start, c->duration}})) {
        return false;
    }
    emit logMessage(QStringLiteral("移动块“%1”到 %2").arg(clipName, trackName));
    return true;
}

// 片段监视器拖入（CapCut 拖入媒体语义 = 让位不拒绝）：目标轨落点处
// 现有块整体右移，新块携带出入点落位。出入点映射：可见窗口
// [S..S+len-1]（len=out-in+1），源偏移 animMin=-in——块首显示源帧
// in（pixId = relFrame - animMinRel，见 AnimationBox），异步解码加载
// 的 updateAnimationRange 只改 anim 时长保留 animMin
bool NleTimelineModel::requestInsertMedia(const QString &path,
                                          const int inFrame,
                                          const int outFrame,
                                          const int trackId,
                                          const int startFrame)
{
    if (mInWriteback || mGestureActive) { return false; }
    const auto scene = mPanelScene.data();
    if (!scene || path.isEmpty() || outFrame < inFrame || inFrame < 0) {
        return false;
    }
    const auto tr = track(trackId);
    if (!tr) {
        emit logMessage(QStringLiteral("目标轨道不存在"));
        return false;
    }
    if (tr->audio) {
        emit logMessage(QStringLiteral("视频片段请拖到视频轨"));
        return false;
    }
    if (tr->locked) {
        emit logMessage(QStringLiteral("目标轨道已锁定"));
        return false;
    }
    const int S = qMax(0, startFrame);
    const int len = outFrame - inFrame + 1;

    // 让位 + 落块 = 一个事务：让位若先经 commitMoves，中途 refresh
    // 的磁吸兜底看到的是"没有新块"的让位中间态，归零压实会把让
    // 位块拉回帧 0，新块随后叠上（让位被吃）。全程 mInWriteback
    // 直写（refresh 短路），最后一次 finishAction 让磁吸兜底对
    // 插入终态压实 = CapCut 拖入即压实语义
    const auto pushes = insertShiftPlan(trackId, S, len, {});
    mInWriteback = true;
    {
        // 与导入同款：不进撤销栈（素材插入 + 异步解码混合态）
        const auto undoBlock = scene->blockUndoRedo();
        for (const auto &m : pushes) {
            const auto oc = clip(m.clipId);
            if (oc && oc->layer) {
                shiftLayer(oc->layer.data(), m.start - oc->start);
            }
        }
        // 联动随动：插在主轨的让位把锚定其上的覆盖块（音频含）
        // 一并带右——否则监视器拖入主轨后覆盖块与主轨内容静默
        // 错位（此处绕过 commitMoves，兜底压实只闭隙不修错位）
        if (mFollowLinked && trackId == mainTrackId() && !pushes.isEmpty()) {
            QHash<int, int> deltas;
            for (const auto &m : pushes) {
                const auto oc = clip(m.clipId);
                if (oc) { deltas.insert(m.clipId, m.start - oc->start); }
            }
            for (const auto &o : mClips) {
                if (o.trackId == trackId || !o.layer ||
                        deltas.contains(o.clipId)) { continue; }
                const Clip *anchor = nullptr;
                for (const auto &mc : mClips) {
                    if (mc.trackId != trackId) { continue; }
                    if (mc.start <= o.start &&
                            o.start < mc.start + mc.duration) {
                        anchor = &mc;
                        break;
                    }
                }
                if (!anchor) { continue; }
                const int delta = deltas.value(anchor->clipId, 0);
                if (delta == 0) { continue; }
                const int ns = qMax(0, o.start + delta);
                if (ns != o.start) { shiftLayer(o.layer.data(), ns - o.start); }
            }
        }
        const auto box = enve::make_shared<VideoBox>();
        box->setFilePath(path);
        scene->addContained(box);
        const auto dur = box->getDurationRectangle();
        if (dur) {
            const auto flar = dur->ref<FixedLenAnimationRect>();
            if (flar) { flar->setFirstAnimationFrame(-inFrame); }
            dur->setMinAbsFrame(S);
            dur->setMaxAbsFrame(S + len - 1);
            // 源映射锚跟最终落位走：块首恒显源帧 in 要求
            // animMin == minRel - in（锚留在 0 时 S>0 的插入整块
            // 落在 anim 范围外=源帧恒钳尾帧+渲染 identical 整条冻结）；
            // 异步解码若再把 minRel 漂 ±1，由 refresh 的锚治愈兜底
            if (flar) {
                flar->setFirstAnimationFrame(
                            dur->getMinRelFrame() - inFrame);
            }
        }
        box->setTrackId(trackId);
    }
    mInWriteback = false;
    finishAction();
    emit logMessage(QStringLiteral("已插入片段 入 %1 出 %2（%3 帧）")
                            .arg(inFrame).arg(outFrame).arg(len));
    return true;
}

// kdenlive-style detach: the embedded eVideoSound stays with the
// video (muted - visibility drives the sound composition) while a
// fresh eIndependentSound on the SAME file takes over the audio on
// an audio track; the shared file cache means no second decode
bool NleTimelineModel::requestDetachAudio(const int clipId)
{
    if (mInWriteback || mGestureActive) { return false; }
    const auto scene = mPanelScene.data();
    const auto c = clip(clipId);
    if (!scene || !c || !c->layer) { return false; }
    const auto vidBox = enve_cast<VideoBox*>(c->layer.data());
    const auto embedded = vidBox ? vidBox->sound() : nullptr;
    if (!embedded) {
        emit logMessage(QStringLiteral("该块没有内嵌音频"));
        return false;
    }
    const QString path = vidBox->getFilePath();
    if (path.isEmpty()) { return false; }
    // snapshot EVERYTHING up front: setting the new sound's durRect
    // fires min/max signals that refresh the clip table mid-function,
    // so the row pointer (c) dangles long before finishAction - the
    // range must come from locals, never from c after any mutation
    const QString clipName = c->name;
    const int clipStart = c->start;
    const int clipEnd = c->start + c->duration - 1;
    // park target: the first audio track (type completeness
    // guarantees one) - resolved BEFORE any mutation: addContained
    // and setTrackId both trigger refreshes that rebuild mTracks,
    // a range-for over it would iterate a dead vector
    int audioTrackId = -1;
    for (const auto &t : mTracks) {
        if (t.audio) { audioTrackId = t.id; break; }
    }

    scene->pushUndoRedoName(tr("Detach Audio"));

    const auto snd = enve::make_shared<eIndependentSound>();
    snd->setFilePath(path);
    scene->addContained(snd);
    snd->prp_setName(c->name + QStringLiteral(" 音频"));
    snd->setStretch(embedded->getStretch());
    const auto dur = snd->getDurationRectangle();
    if (dur) {
        // same visible window as the video clip (the file handler is
        // already cached from the import, so the length is final and
        // the range sticks)
        dur->setMinAbsFrame(clipStart);
        dur->setMaxAbsFrame(clipEnd);
    }
    if (audioTrackId >= 0) { snd->setTrackId(audioTrackId); }
    embedded->setVisible(false);

    finishAction();
    emit logMessage(QStringLiteral("已分离“%1”的音频到音频轨").arg(clipName));
    return true;
}

// ------------------------------------------------- decorations (CapCut)

int NleTimelineModel::colorMark(const int clipId) const
{
    return mColorMarks.value(clipId, -1);
}

void NleTimelineModel::requestColorMark(const QSet<int> &clipIds,
                                        const int color)
{
    for (const int id : clipIds) {
        if (!clip(id)) { continue; }
        if (color < 0) { mColorMarks.remove(id); }
        else { mColorMarks.insert(id, qBound(0, color, 6)); }
    }
    emit clipDecorationsChanged();
}

bool NleTimelineModel::requestSelectSameColor(const int clipId)
{
    const int mark = colorMark(clipId);
    if (mark < 0) {
        emit logMessage(QStringLiteral("该块没有颜色标记"));
        return false;
    }
    QSet<int> picked;
    for (const auto &c : mClips) {
        if (mColorMarks.value(c.clipId, -1) == mark) { picked.insert(c.clipId); }
    }
    setSelection(picked);
    emit logMessage(QStringLiteral("已选中 %1 个同色块").arg(picked.size()));
    return true;
}

bool NleTimelineModel::isDisabled(const int clipId) const
{
    return mDisabledIds.contains(clipId);
}

void NleTimelineModel::requestSetDisabled(const QSet<int> &clipIds,
                                          const bool disabled)
{
    const auto scene = mPanelScene.data();
    if (!scene) { return; }
    const auto undoBlock = scene->blockUndoRedo();
    int changed = 0;
    // writeback 守卫：setVisible 会发 visibilityChanged → refresh，
    // 中途重建表会让下面的行指针悬垂（铁律 1）
    mInWriteback = true;
    for (const int id : clipIds) {
        const auto c = clip(id);
        if (!c || !c->layer) { continue; }
        auto * const layer = c->layer.data();
        if (disabled) {
            if (mDisabledIds.contains(id)) { continue; }
            mDisabledIds.insert(id);
            // 压制中的层记录压制前可见性（solo 结束后启用应回到它，
            // 而不是压制期的 false）；否则记录当前可见性
            mDisabledSaved.insert(id,
                    mSoloSaved.contains(layer)
                        ? mSoloSaved.value(layer)
                        : layer->isVisible());
            layer->setVisible(false);
            ++changed;
        } else {
            if (!mDisabledIds.contains(id)) { continue; }
            mDisabledIds.remove(id);
            bool vis = mDisabledSaved.value(id, true);
            mDisabledSaved.remove(id);
            // solo 压制中的层保持隐藏（压制机制优先，refresh 会续压）
            if (mSoloSuppressed.contains(layer)) { vis = false; }
            layer->setVisible(vis);
            ++changed;
        }
    }
    mInWriteback = false;
    if (changed == 0) { return; }
    const QString what = disabled ? QStringLiteral("停用片段")
                                  : QStringLiteral("启用片段");
    emit logMessage(QStringLiteral("%1：%2 个块").arg(what).arg(changed));
    emit clipDecorationsChanged();
}

// --------------------------------------------- timeline clipboard (CapCut)

bool NleTimelineModel::hasClipClipboard() const
{
    return mBoxClipBoard || !mSoundClipBoard.isEmpty();
}

bool NleTimelineModel::requestCopy(const QSet<int> &clipIds)
{
    const auto scene = mPanelScene.data();
    if (!scene || clipIds.isEmpty()) { return false; }
    QList<BoundingBox*> boxes;
    QList<eIndependentSound*> sounds;
    for (const int id : clipIds) {
        const auto c = clip(id);
        if (!c || !c->layer) { continue; }
        const auto snd = enve_cast<eIndependentSound*>(c->layer.data());
        if (snd) { sounds << snd; continue; }
        const auto box = enve_cast<BoundingBox*>(c->layer.data());
        if (box) { boxes << box; }
    }
    if (boxes.isEmpty() && sounds.isEmpty()) { return false; }

    mBoxClipBoard.reset();
    mBoxClipSpecs.clear();
    mSoundClipBoard.clear();
    mSoundClipSpecs.clear();
    if (!boxes.isEmpty()) {
        mBoxClipBoard = enve::make_shared<BoxesClipboard>(boxes);
        for (const auto *box : boxes) {
            PasteSpec spec;
            spec.trackId = box->trackId();
            const auto dur = box->getDurationRectangle();
            spec.duration = dur ? dur->getAbsFrameRange().span() : 1;
            spec.name = box->prp_getName();
            mBoxClipSpecs << spec;
        }
    }
    // sounds cannot travel through BoxesClipboard: full-property
    // serialization clone (same channel as splitSoundsAtFrame), kept
    // OFF the scene until paste
    for (auto *snd : sounds) {
        const auto temp = enve::make_shared<Clipboard>(ClipboardType::misc);
        temp->write([snd](eWriteStream &dst) {
            const bool isBox = false;
            dst << isBox;
            snd->prp_writeProperty_impl(dst);
            dst.writeCheckpoint();
        });
        qsptr<eIndependentSound> clone;
        temp->read([&clone](eReadStream &src) {
            bool isBox;
            src >> isBox;
            clone = enve::make_shared<eIndependentSound>();
            clone->prp_readProperty_impl(src);
            src.readCheckpoint("Error reading sound clone");
        });
        if (!clone) { continue; }
        PasteSpec spec;
        spec.trackId = clone->trackId();
        const auto dur = clone->getDurationRectangle();
        spec.duration = dur ? dur->getAbsFrameRange().span() : 1;
        spec.name = clone->prp_getName();
        mSoundClipBoard << clone;
        mSoundClipSpecs << spec;
    }
    const int total = mBoxClipSpecs.size() + mSoundClipSpecs.size();
    emit logMessage(QStringLiteral("已复制 %1 个块").arg(total));
    return true;
}

bool NleTimelineModel::requestPaste(const int frame)
{
    const auto scene = mPanelScene.data();
    if (!scene || !hasClipClipboard()) { return false; }
    scene->pushUndoRedoName(tr("粘贴"));
    int pasted = 0;
    int cursor = qMax(0, frame);

    // ---- boxes: pasteTo brings the clones in (selected) at their
    // source windows; ONE flush lets them enter the clip table with
    // stable ids, then relocation rides commitMoves - the same
    // validated channel as drag/magnetic moves. Hand-writing durRects
    // on freshly cloned boxes fights the FixedLen binding callback
    // (1-frame drift), so we never touch it directly here
    if (mBoxClipBoard) {
        scene->clearBoxesSelection();
        mBoxClipBoard->pasteTo(scene);
        const auto pastedBoxes = scene->getSelectedBoxesList();
        scene->clearBoxesSelection();
        finishAction(); // clones enter mClips (paste lands in history)
        for (const auto &box : pastedBoxes) {
            const auto d = box->getDurationRectangle();
            qInfo("[NLE] paste mid: box %s dur {%d..%d}",
                  box->prp_getName().toUtf8().constData(),
                  d ? d->getMinAbsFrame() : -99,
                  d ? d->getMaxAbsFrame() : -99);
        }

        // 重定位：相对平移（shiftLayer，波纹删除/分割同款通道），
        // 不重设长度——克隆块的 FixedLen durRect 与 fileHandler 绑定，
        // 手写绝对窗口会被回调拉回 1 帧（实测漂移案）
        mInWriteback = true;
        int specIdx = 0;
        for (const auto &box : pastedBoxes) {
            if (!box || specIdx >= mBoxClipSpecs.size()) { continue; }
            const auto &spec = mBoxClipSpecs.at(specIdx++);
            // 反查新块的 clipId（finishAction 后按层稳定）
            const NleTimelineModel::Clip *row = nullptr;
            for (const auto &c : mClips) {
                if (c.layer.data() == box) { row = &c; break; }
            }
            if (!row || !row->layer) { continue; }
            // 目标轨：源轨仍在用源轨，否则第一条视频轨
            int trackId = spec.trackId >= 0 &&
                    track(spec.trackId) ? spec.trackId : -1;
            if (trackId < 0) {
                for (const auto &t : mTracks) {
                    if (!t.audio) { trackId = t.id; break; }
                }
            }
            if (trackId < 0) { continue; }
            // 从 cursor 起第一个空位（长度用表里的真实窗口长度；
            // 新块自身占着的源窗口不算——它就是要被挪走的那个）
            const int len = row->duration;
            int start = cursor;
            bool moved = true;
            while (moved) {
                moved = false;
                for (const auto &c : mClips) {
                    if (c.clipId == row->clipId || c.trackId != trackId) {
                        continue;
                    }
                    if (rangesOverlap(start, len, c.start, c.duration)) {
                        start = c.start + c.duration;
                        moved = true;
                    }
                }
            }
            if (box->trackId() != trackId) { box->setTrackId(trackId); }
            qInfo("[NLE] paste shift: row{st %d dur %d} -> start %d",
                  row->start, row->duration, start);
            shiftLayer(row->layer.data(), start - row->start);
            {
                const auto d2 = box->getDurationRectangle();
                qInfo("[NLE] paste after shift: dur {%d..%d}",
                      d2 ? d2->getMinAbsFrame() : -99,
                      d2 ? d2->getMaxAbsFrame() : -99);
            }
            cursor = start + len;
            ++pasted;
        }
        mInWriteback = false;
    }

    // ---- sounds: add the kept clones and window them the same way
    for (int i = 0; i < mSoundClipBoard.size(); ++i) {
        const auto &clone = mSoundClipBoard.at(i);
        const auto &spec = mSoundClipSpecs.at(i);
        if (!clone) { continue; }
        int trackId = spec.trackId >= 0 &&
                track(spec.trackId) ? spec.trackId : -1;
        if (trackId < 0) {
            for (const auto &t : mTracks) {
                if (t.audio) { trackId = t.id; break; }
            }
        }
        const QString name = spec.name;
        scene->addContained(clone);
        clone->prp_setName(name);
        const auto dur = clone->getDurationRectangle();
        if (dur) {
            dur->setMinAbsFrame(cursor);
            dur->setMaxAbsFrame(cursor + spec.duration - 1);
        }
        if (trackId >= 0) { clone->setTrackId(trackId); }
        cursor += spec.duration;
        ++pasted;
    }

    finishAction();
    emit logMessage(QStringLiteral("已粘贴 %1 个块到 %2")
                    .arg(pasted).arg(frameToTimecode(qMax(0, frame), mFps)));
    return pasted > 0;
}

// ---------------------------------------------------------------- 转场

int NleTimelineModel::transitionWindowOf(const Clip &c) const
{
    const auto box = enve_cast<BoundingBox*>(c.layer.data());
    if (!box) { return 0; }
    return transitionInValue(transitionEffectOn(box));
}

bool NleTimelineModel::stripTransitionEffect(eBoxOrSound * const layer)
{
    const auto box = enve_cast<BoundingBox*>(layer);
    if (!box) { return false; }
    const auto re = transitionEffectOn(box);
    if (!re) { return false; }
    box->removeRasterEffect(re->ref<RasterEffect>());
    return true;
}

QVector<NleTimelineModel::Transition> NleTimelineModel::transitions() const
{
    QVector<Transition> out;
    const int mainId = mainTrackId();
    if (mainId < 0) { return out; }
    QList<QPair<int, const Clip*>> order;
    for (const auto &c : mClips) {
        if (c.trackId != mainId || c.audio) { continue; }
        order.append({c.start, &c});
    }
    std::sort(order.begin(), order.end(),
              [](const auto &a, const auto &b) {
                  if (a.first != b.first) { return a.first < b.first; }
                  return a.second->clipId < b.second->clipId;
              });
    for (int i = 0; i + 1 < order.size(); i++) {
        const auto L = order.at(i).second;
        const auto R = order.at(i + 1).second;
        if (R->start > L->start + L->duration - 1) { continue; } // 不重叠
        const auto box = enve_cast<BoundingBox*>(R->layer.data());
        if (!box) { continue; }
        const auto re = transitionEffectOn(box);
        if (!re) { continue; }
        Transition t;
        t.leftId = L->clipId;
        t.rightId = R->clipId;
        t.start = R->start;
        t.end = L->start + L->duration - 1;
        t.frames = qMin(transitionInValue(re), t.end - t.start + 1);
        t.type = int(re->getEffectType());
        t.typeName = re->prp_getName();
        out.append(t);
    }
    return out;
}

// 内部公共：对已解析的贴邻对应用/替换转场。替换 = 只换类型保窗口
bool NleTimelineModel::requestApplyTransitionOnPair(
        const Clip &L, const Clip &R, const int transitionType)
{
    const auto rBox = enve_cast<BoundingBox*>(R.layer.data());
    if (!rBox) { return false; }
    const auto existing = transitionEffectOn(rBox);
    if (existing) {
        // 替换类型：布局与窗口不动
        const auto fresh = createRasterEffectForNonCustomType(
                    RasterEffectType(transitionType));
        if (!fresh) { return false; }
        const int n = qMin(transitionInValue(existing),
                           L.start + L.duration - R.start);
        const int overlap = L.start + L.duration - R.start;
        mInWriteback = true;
        rBox->removeRasterEffect(existing->ref<RasterEffect>());
        rBox->addRasterEffect(fresh);
        if (const auto qa = transitionInAnimator(fresh.data())) {
            qa->setCurrentBaseValue(qMax(1, qMin(n, overlap)));
        }
        mInWriteback = false;
        finishAction();
        emit logMessage(QStringLiteral("转场已替换为「%1」")
                        .arg(fresh->prp_getName()));
        return true;
    }
    const auto lBox = enve_cast<BoundingBox*>(L.layer.data());
    if (transitionEffectOn(lBox)) {
        emit logMessage(QStringLiteral("左侧片段已参与转场，不能连用"));
        return false;
    }
    // 窗口 = 默认 12 帧夹进两侧块长与可回退空间
    int n = 12;
    n = qMin(n, L.duration - 1);
    n = qMin(n, R.duration - 1);
    n = qBound(2, n, 600);
    const int overlap = L.start + L.duration - R.start; // 恒 0（贴邻）
    Q_UNUSED(overlap)
    mInWriteback = true;
    // 右块左移 N 帧：与左块尾重叠 N 帧 = CapCut 转场窗口
    shiftLayer(rBox, -n);
    const auto fresh = createRasterEffectForNonCustomType(
                RasterEffectType(transitionType));
    if (fresh) {
        rBox->addRasterEffect(fresh);
        if (const auto qa = transitionInAnimator(fresh.data())) {
            qa->setCurrentBaseValue(qreal(n));
        }
        // 出窗口恒 0：转场只锚定右块头部
        if (const int m = fresh->ca_getNumberOfChildren()) {
            for (int i = 0; i < m; i++) {
                const auto qa = enve_cast<QrealAnimator*>(
                            fresh->ca_getChildAt(i));
                // 第二个 qreal 子恒为出窗口（构造序：入/出/其余）
                if (qa && transitionInAnimator(fresh.data()) != qa) {
                    const QString nm = qa->prp_getName();
                    if (nm.contains(QStringLiteral("淡出")) ||
                        nm.contains(QStringLiteral("滑出"))) {
                        qa->setCurrentBaseValue(0.);
                    }
                }
            }
        }
    }
    mInWriteback = false;
    finishAction();
    if (fresh) {
        emit logMessage(QStringLiteral("已添加转场「%1」（%2 帧）")
                        .arg(fresh->prp_getName()).arg(n));
    }
    return fresh != nullptr;
}

bool NleTimelineModel::requestApplyTransition(
        const int junctionFrame, const int transitionType)
{
    if (mInWriteback || mGestureActive) { return false; }
    const int mainId = mainTrackId();
    if (mainId < 0) { return false; }
    QList<QPair<int, const Clip*>> order;
    for (const auto &c : mClips) {
        if (c.trackId != mainId || c.audio) { continue; }
        order.append({c.start, &c});
    }
    std::sort(order.begin(), order.end(),
              [](const auto &a, const auto &b) { return a.first < b.first; });
    // 播放头规则：落在块 L 上（含左端）且右邻贴邻 → L|R；
    // 或正落在贴邻对右块 R 的首帧 → L|R
    for (int i = 0; i + 1 < order.size(); i++) {
        const auto L = order.at(i).second;
        const auto R = order.at(i + 1).second;
        // 贴邻对（新装）或转场重叠对（替换类型）都合法：
        // 只认贴邻会让"换类型"在转场已存在时永远找不到对
        const bool butt = R->start == L->start + L->duration;
        const bool transOverlap = transitionWindowOf(*R) > 0 &&
                R->start <= L->start + L->duration;
        if (!butt && !transOverlap) { continue; }
        const bool onL = junctionFrame >= L->start &&
                junctionFrame <= L->start + L->duration - 1;
        const bool onRHead = junctionFrame == R->start;
        if (onL || onRHead) {
            return requestApplyTransitionOnPair(*L, *R, transitionType);
        }
    }
    // 放宽（CapCut 点击即加）：播放头不在任何交界上时，吸附 ±2 秒
    // 内最近的贴邻交界——用户不必把播放头停得分毫不差
    return requestApplyTransitionAtDrop(junctionFrame, transitionType);
}

bool NleTimelineModel::requestApplyTransitionAtDrop(
        const int dropFrame, const int transitionType)
{
    if (mInWriteback || mGestureActive) { return false; }
    const int mainId = mainTrackId();
    if (mainId < 0) { return false; }
    QList<QPair<int, const Clip*>> order;
    for (const auto &c : mClips) {
        if (c.trackId != mainId || c.audio) { continue; }
        order.append({c.start, &c});
    }
    std::sort(order.begin(), order.end(),
              [](const auto &a, const auto &b) { return a.first < b.first; });
    const int snap = qMax(2, qRound(2 * mFps)); // ±2 秒吸附窗
    int bestDist = snap + 1;
    const Clip *bestL = nullptr;
    const Clip *bestR = nullptr;
    for (int i = 0; i + 1 < order.size(); i++) {
        const auto L = order.at(i).second;
        const auto R = order.at(i + 1).second;
        const bool butt = R->start == L->start + L->duration;
        const bool transOverlap = transitionWindowOf(*R) > 0 &&
                R->start <= L->start + L->duration;
        if (!butt && !transOverlap) { continue; }
        const int j = L->start + L->duration;
        const int d = qAbs(j - dropFrame);
        if (d < bestDist) { bestDist = d; bestL = L; bestR = R; }
    }
    if (!bestL) {
        emit logMessage(QStringLiteral(
                    "主轨上 ±2 秒内没有相邻片段交界，无法添加转场"));
        return false;
    }
    return requestApplyTransitionOnPair(*bestL, *bestR, transitionType);
}

bool NleTimelineModel::requestRemoveTransition(const int rightClipId)
{
    if (mInWriteback || mGestureActive) { return false; }
    const auto rc = clip(rightClipId);
    if (!rc || !rc->layer) { return false; }
    const auto box = enve_cast<BoundingBox*>(rc->layer.data());
    const auto re = transitionEffectOn(box);
    if (!re) { return false; }
    // 回贴量 = 布局重叠宽（剥特效 + 右块右移恢复贴邻）
    const int mainId = mainTrackId();
    int overlap = 0;
    for (const auto &c : mClips) {
        if (c.trackId != mainId || c.audio || c.clipId == rightClipId) {
            continue;
        }
        const int end = c.start + c.duration - 1;
        if (end >= rc->start && c.start < rc->start) {
            overlap = qMax(overlap, end + 1 - rc->start);
        }
    }
    mInWriteback = true;
    stripTransitionEffect(rc->layer.data());
    if (overlap > 0) { shiftLayer(rc->layer.data(), overlap); }
    mInWriteback = false;
    finishAction();
    emit logMessage(QStringLiteral("已删除转场"));
    return true;
}

bool NleTimelineModel::requestTransitionDuration(
        const int rightClipId, const int frames)
{
    if (mInWriteback || mGestureActive) { return false; }
    const auto rc = clip(rightClipId);
    if (!rc || !rc->layer) { return false; }
    const auto box = enve_cast<BoundingBox*>(rc->layer.data());
    const auto re = transitionEffectOn(box);
    if (!re) { return false; }
    const int mainId = mainTrackId();
    const Clip *lc = nullptr;
    int overlap = 0;
    for (const auto &c : mClips) {
        if (c.trackId != mainId || c.audio || c.clipId == rightClipId) {
            continue;
        }
        const int end = c.start + c.duration - 1;
        if (end >= rc->start && c.start < rc->start) {
            const int o = end + 1 - rc->start;
            if (o > overlap) { overlap = o; lc = &c; }
        }
    }
    if (!lc || overlap <= 0) { return false; }
    // 新窗口夹进两侧块长；右块随窗口差位移（贴邻关系不变）
    const int n = qBound(2, frames, qMin(lc->duration - 1, rc->duration - 1));
    const int delta = overlap - n;
    mInWriteback = true;
    if (delta != 0) { shiftLayer(rc->layer.data(), delta); }
    if (const auto qa = transitionInAnimator(re)) {
        qa->setCurrentBaseValue(qreal(n));
    }
    mInWriteback = false;
    finishAction();
    return true;
}

// ---------------------------------------------------------------- tracks

int NleTimelineModel::requestTrackAdd(const bool audio, const bool atTop)
{
    const auto scene = mPanelScene.data();
    if (!scene) { return -1; }
    int sameType = 0;
    int insertIdx = -1;
    int scanned = 0;
    for (const auto &t : mTracks) {
        if (t.audio == audio) {
            ++sameType;
            if (insertIdx < 0) { insertIdx = scanned; }
        }
        ++scanned;
    }
    const QString name = audio
            ? QStringLiteral("A%1").arg(sameType + 1)
            : QStringLiteral("V%1").arg(sameType + 1);
    // atTop: insert at the type group head (the NLE track order is
    // video group then audio group, so this index is also the spec
    // list position of the group head)
    const int id = scene->addTrackSpec(audio, name,
                                       atTop ? insertIdx : -1);
    finishAction();
    return id;
}

bool NleTimelineModel::requestTrackRemove(const int trackId)
{
    const auto scene = mPanelScene.data();
    const auto t = track(trackId);
    if (!scene || !t) { return false; }
    // the last lane of a type must survive: fresh layers of that type
    // would have no legal home
    int sameType = 0;
    for (const auto &o : mTracks) {
        if (o.audio == t->audio) { ++sameType; }
    }
    if (sameType <= 1) {
        emit logMessage(QStringLiteral("最后一个%1轨不可删除")
                        .arg(t->audio ? QStringLiteral("音频")
                                      : QStringLiteral("视频")));
        return false;
    }
    if (scene->removeTrackSpec(trackId)) {
        finishAction();
        return true;
    }
    emit logMessage(QStringLiteral("轨道非空或不存在，未删除"));
    return false;
}

void NleTimelineModel::requestTrackRename(const int trackId,
                                          const QString &name)
{
    const auto scene = mPanelScene.data();
    if (!scene) { return; }
    scene->setTrackSpecName(trackId, name);
    refreshFromDocument();
}

void NleTimelineModel::requestTrackSetLocked(const int trackId,
                                             const bool locked)
{
    const auto scene = mPanelScene.data();
    if (!scene) { return; }
    scene->setTrackSpecLocked(trackId, locked);
    refreshFromDocument();
}

void NleTimelineModel::requestTrackSetHeight(const int trackId,
                                             const int height)
{
    const auto scene = mPanelScene.data();
    if (!scene) { return; }
    scene->setTrackSpecHeight(trackId, height);
    refreshFromDocument();
}

void NleTimelineModel::requestTrackToggleSolo(const int trackId)
{
    if (mInWriteback || mGestureActive) { return; }
    const auto scene = mPanelScene.data();
    const auto t = track(trackId);
    if (!scene || !t) { return; }
    // spec 旗标是平凡写（同锁定），压制/恢复在 refresh 的
    // solo 压制段统一执行
    scene->setTrackSpecSolo(trackId, !t->solo);
    refreshFromDocument();
}

void NleTimelineModel::requestTrackToggleMute(const int trackId,
                                              const bool allSameType)
{
    if (mInWriteback || mGestureActive) { return; }
    const auto scene = mPanelScene.data();
    const auto t = track(trackId);
    if (!scene || !t) { return; }
    // target lanes: one, or every lane of the same type (kdenlive Shift)
    QList<int> lanes{trackId};
    if (allSameType) {
        lanes.clear();
        for (const auto &o : mTracks) {
            if (o.audio == t->audio) { lanes << o.id; }
        }
    }
    // lane layers: every clip currently parked on the target lanes
    QList<eBoxOrSound*> laneLayers;
    for (const auto &c : mClips) {
        if (!lanes.contains(c.trackId)) { continue; }
        if (c.layer) { laneLayers << c.layer.data(); }
    }
    if (laneLayers.isEmpty()) { return; }
    // one press = one common state: any visible member -> hide all
    const bool anyVisible = std::any_of(
                laneLayers.cbegin(), laneLayers.cend(),
                [](eBoxOrSound * const l) { return l->isVisible(); });
    mInWriteback = true;
    for (auto *l : laneLayers) { l->setVisible(!anyVisible); }
    mInWriteback = false;
    finishAction();
}

// ---------------------------------------------------------------- guides

void NleTimelineModel::requestMarkerAdd(const int frame)
{
    const auto scene = mPanelScene.data();
    if (!scene) { return; }
    scene->setMarker(frame);
    refreshFromDocument();
}

void NleTimelineModel::requestMarkerRemove(const int frame)
{
    const auto scene = mPanelScene.data();
    if (!scene) { return; }
    if (scene->removeMarker(frame)) { refreshFromDocument(); }
}

// ---------------------------------------------------------------- magnetic

void NleTimelineModel::setMagnetic(const bool on, const bool compact)
{
    if (mMagnetic == on) { return; }
    mMagnetic = on;
    emit magneticChanged(on);
    if (!on || !compact) { return; }
    // turning it on enforces the no-gap invariant right away
    // (undoable through the commit)
    const auto moves = compactGapsPlan();
    if (!moves.isEmpty()) {
        emit logMessage(QStringLiteral("磁吸开启：%1 块左移贴紧").arg(moves.size()));
        commitMoves(moves, false);
    } else {
        emit logMessage(QStringLiteral("磁吸开启：轨道已无间隙"));
    }
}

QVector<NleTimelineModel::Move> NleTimelineModel::magneticRearrangePlan(
        const int trackId, const int draggedId, const int dropStart,
        const QSet<int> &movingIds) const
{
    // kdenlive 方案A: dragged clip = drop frame 1:1; left package
    // (end <= drop) compacts toward 0 - 用户规则：链头恒在帧 1 即
    // 时间 00，主轨首块始终靠左对齐；everything else chains tightly
    // after the dragged clip's new out point. A clip straddling the
    // drop point belongs to the right package and moves as a whole.
    QVector<Move> moves;
    const auto dragged = clip(draggedId);
    if (!dragged) { return moves; }
    const int dur = dragged->duration;
    const int drop = qMax(0, dropStart);

    QList<const Clip*> left, right;
    for (const auto &c : mClips) {
        if (c.clipId == draggedId || movingIds.contains(c.clipId)) { continue; }
        if (c.trackId != trackId) { continue; }
        if (c.start + c.duration <= drop) { left << &c; }
        else { right << &c; }
    }
    const auto byStart = [](const Clip * const a, const Clip * const b) {
        return a->start < b->start;
    };
    std::sort(left.begin(), left.end(), byStart);
    std::sort(right.begin(), right.end(), byStart);

    // kdenlive compaction toward 0: the head clip always lands at
    // frame 1 (time 00) - 用户规则主轨首块恒靠左对齐时间起点，左
    // 包从 0 起压收拢内部间隙
    int cursor = 0;
    for (const Clip *c : left) {
        // 转场右块允许沉入游标左侧的转场重叠窗口
        const int target = qMax(0, cursor - transitionWindowOf(*c));
        if (target != c->start) {
            moves.append({c->clipId, trackId, target, c->duration});
        }
        cursor = target + c->duration;
    }
    moves.append({draggedId, trackId, drop, dur});
    cursor = drop + dur;
    for (const Clip *c : right) {
        // 被拖块让开后的紧链：转场右块同样沉入重叠窗口
        const int target = qMax(0, cursor - transitionWindowOf(*c));
        if (target != c->start) {
            moves.append({c->clipId, trackId, target, c->duration});
        }
        cursor = target + c->duration; // tight chain: no gaps
    }
    return moves;
}

// ---------------------------------------------------------------- undo

void NleTimelineModel::undo()
{
    const auto scene = mDocument.fActiveScene.data();
    if (!scene) { return; }
    scene->undo();
    finishAction();
}

void NleTimelineModel::redo()
{
    const auto scene = mDocument.fActiveScene.data();
    if (!scene) { return; }
    scene->redo();
    finishAction();
}

// ---------------------------------------------------------------- doc sync

void NleTimelineModel::connectPanelScene(Canvas * const scene)
{
    if (scene == mPanelScene.data()) { return; }
    for (const auto &conn : mSceneConns) { disconnect(conn); }
    mSceneConns.clear();
    mPanelScene = scene;
    if (!scene) { return; }

    // box children are NOT ComplexAnimator properties: layer insertion
    // and removal fire insertedObject/removedObject on the container -
    // the ca_childAdded family never fires for scene layers, so
    // connecting only those left imports invisible until some
    // unrelated signal happened to rebuild
    mSceneConns << connect(scene, &ContainerBox::insertedObject,
                           this, [this](const int, eBoxOrSound*) { refreshFromDocument(); });
    mSceneConns << connect(scene, &ContainerBox::removedObject,
                           this, [this](const int, eBoxOrSound*) { refreshFromDocument(); });
    // native row reorder (drag in the layer panel / track writebacks /
    // undo) changes the lane order derived from the specs
    mSceneConns << connect(scene, &ContainerBox::movedObject,
                           this, [this](const int, const int, eBoxOrSound*) {
        refreshFromDocument();
    });
    mSceneConns << connect(scene, &Canvas::prp_currentFrameChanged,
                           this, [this](const UpdateReason) {
        if (mInWriteback) { return; }
        const auto s = mPanelScene.data();
        if (!s) { return; }
        // 输出渲染逐帧步进不推时间轴播放头：每帧一次整幅重绘
        // 白白与渲染线程抢 CPU，导出进度看队列条目即可
        if (s->isOutputRendering()) { return; }
        emit playheadFrameChanged(s->anim_getCurrentAbsFrame());
    });
    mSceneConns << connect(scene, &Canvas::fpsChanged,
                           this, [this](const qreal) { refreshFromDocument(); });
}

void NleTimelineModel::connectChildren(Canvas * const scene)
{
    for (const auto &conn : mChildConns) { disconnect(conn); }
    mChildConns.clear();
    if (!scene) { return; }
    for (const auto &child : scene->getContained()) {
        const auto layer = child.data();
        if (!layer) { continue; }
        mChildConns << connect(layer, &eBoxOrSound::prp_nameChanged,
                               this, [this](const QString&) { refreshFromDocument(); });
        // native track merge / split (layer-panel drags, context
        // menus, undo) must re-map the lanes
        mChildConns << connect(layer, &eBoxOrSound::trackIdChanged,
                               this, [this](const int) { refreshFromDocument(); });
        // 旁路可见性变化（经典图层面板眼睛、画布显示操作、脚本）
        // 同步进时间轴：块压暗/喇叭镜像只在 refresh 里重算，不接
        // 这条线就一直显示旧状态。模型自身的写回路径（solo 压制/
        // 静音/停用）都在 mInWriteback 守卫内发 setVisible，此处
        // 不会重入；工程读取直写 mVisible 不发信号，开档零风暴
        mChildConns << connect(layer, &eBoxOrSound::visibilityChanged,
                               this, [this](const bool) { refreshFromDocument(); });
        const auto dur = layer->getDurationRectangle();
        if (dur) {
            mChildConns << connect(dur, &DurationRectangle::minRelFrameChanged,
                                   this, [this](const int, const int) { refreshFromDocument(); });
            mChildConns << connect(dur, &DurationRectangle::maxRelFrameChanged,
                                   this, [this](const int, const int) { refreshFromDocument(); });
        }
    }
}

QList<eTrackSpec> NleTimelineModel::deriveTrackSpecs(
        Canvas * const scene,
        const QList<QPair<eBoxOrSound*, bool>> &items)
{
    // pre-P0 panel derivation, frozen into the persistent table:
    // siblings sharing a trackId share a lane (first-seen order),
    // every other layer owns one; ids come from the layers' own space
    struct Lane { int tid; QList<eBoxOrSound*> members; };
    QList<Lane> lanes[2]; // 0 = video, 1 = audio
    for (const auto &it : items) {
        const int audioIdx = it.second ? 1 : 0;
        const int tid = it.first->trackId();
        bool joined = false;
        if (tid >= 0) {
            for (auto &ln : lanes[audioIdx]) {
                if (ln.tid == tid) {
                    ln.members << it.first;
                    joined = true;
                    break;
                }
            }
        }
        if (!joined) { lanes[audioIdx].append({tid, {it.first}}); }
    }
    int nextId = scene->newTrackId(scene);
    QList<eTrackSpec> specs;
    for (int a = 0; a < 2; ++a) {
        const int total = lanes[a].size();
        for (int i = 0; i < total; ++i) {
            eTrackSpec spec;
            spec.mId = nextId++;
            spec.mAudio = a == 1;
            spec.mName = a == 0
                    ? QStringLiteral("V%1").arg(total - i)
                    : QStringLiteral("A%1").arg(total - i);
            specs.append(spec);
            for (auto *l : lanes[a][i].members) {
                if (l->trackId() != spec.mId) { l->setTrackId(spec.mId); }
            }
        }
    }
    // type completeness: the table must always hold at least one lane
    // of EACH type, otherwise a fresh sound/visual layer would have no
    // legal home
    const auto ensureType = [&](const bool audio) {
        for (const auto &s : specs) { if (s.mAudio == audio) { return; } }
        eTrackSpec spec;
        spec.mId = nextId++;
        spec.mAudio = audio;
        spec.mName = audio ? QStringLiteral("A1") : QStringLiteral("V1");
        specs.append(spec);
    };
    ensureType(false);
    ensureType(true);

    scene->initTrackSpecs(specs);
    qInfo("[TRK] derive migration: %d tracks installed", specs.size());
    return specs;
}


void NleTimelineModel::refreshFromDocument()
{
    if (mInWriteback) { return; }
    if (mGestureActive) { mRebuildQueued = true; return; }

    // panel scene: follow the active scene, but when it has nothing
    // to edit (e.g. the user dove into a child scene), keep showing
    // the last scene that had blocks instead of blanking the panel
    const auto activeScene = mDocument.fActiveScene.data();
    QList<QPair<eBoxOrSound*, bool>> items;
    const auto collect = [&items](Canvas * const s) {
        items.clear();
        if (!s) { return; }
        // mirror the native timeline: EVERY child layer shows here.
        // Sounds become audio clips; every visual layer (scene links,
        // vectors, images, text, groups, ...) becomes a video clip
        for (const auto &child : s->getContained()) {
            const auto layer = child.data();
            if (!layer) { continue; }
            const bool audio = enve_cast<eSound*>(layer) != nullptr;
            items.append({layer, audio});
        }
    };
    collect(activeScene);
    auto scene = activeScene;
    if (items.isEmpty()) {
        const auto fallback = mPanelScene.data();
        if (fallback && fallback != activeScene) {
            collect(fallback);
            if (!items.isEmpty()) { scene = fallback; }
        }
    }

    connectPanelScene(scene);
    connectChildren(scene);

    // selection survives rebuilds by STABLE CLIP ID: mLayerToClipId
    // keeps the layer->id mapping alive across refreshes, so the ids
    // in mSelected keep resolving. The old name-based survival
    // co-selected every same-named sibling (razor halves share a
    // name - the split restores the original), which made a plain
    // click look like select-all and silently added riders that
    // blocked every drag
    QSet<int> selectionIds = mSelected;
    mSelected.clear();

    // tracks are explicit persistent entities: video specs first (top
    // lane = first video spec), then audio specs. A virgin scene (or
    // a pre-P0 project) installs the derived legacy layout once.
    // NEVER while the project streams in: layers arrive before the
    // spec table, a mid-load derivation would rewrite every trackId
    // against a table the file is about to replace
    auto specs = scene ? scene->getTrackSpecs() : QList<eTrackSpec>();
    const bool loadingProject = scene && scene->isLoadingProject();
    if (scene && specs.isEmpty() && !loadingProject) {
        // the setTrackId calls inside must not re-enter the refresh,
        // and must not leave undo entries (a load-time migration must
        // not be the user's first Ctrl+Z)
        mInWriteback = true;
        const auto undoBlock = scene->blockUndoRedo();
        specs = deriveTrackSpecs(scene, items);
        mInWriteback = false;
    }
    // main-track designation: PERSISTENT per spec table. Legacy or
    // brand-new tables carry no flag - designate the bottom-most
    // video lane ONCE; later lane adds (even below it) never move it
    if (scene && !specs.isEmpty()) {
        bool anyVideo = false;
        bool anyMain = false;
        for (const auto &s : specs) {
            if (!s.mAudio) {
                anyVideo = true;
                if (s.mMain) { anyMain = true; }
            }
        }
        if (anyVideo && !anyMain) {
            for (int i = specs.size() - 1; i >= 0; --i) {
                auto &s = specs[i];
                if (s.mAudio) { continue; }
                s.mMain = true;
                scene->setTrackSpecMain(s.mId, true);
                break;
            }
        }
    }

    QHash<int, int> laneById;
    mTracks.clear();
    for (const auto &s : specs) {
        if (s.mAudio) { continue; }
        laneById.insert(s.mId, mTracks.size());
        mTracks.append({s.mId, s.mName, false, s.mLocked, s.mSolo,
                        false, s.mHeight});
    }
    const int videoCount = mTracks.size();
    for (const auto &s : specs) {
        if (!s.mAudio) { continue; }
        laneById.insert(s.mId, mTracks.size());
        mTracks.append({s.mId, s.mName, true, s.mLocked, s.mSolo,
                        true, s.mHeight});
    }

    // typed 新层一轨一层（CapCut 新建图层动线）：文字/图形/固态/
    // 调整/矢量容器这些新建层每个独占一条新轨，插在主轨正上方
    // （后建的贴近主轨，先建的被动上移；主轨恒最底不受影响）
    const auto isTypedLayer = [](eBoxOrSound * const layer) {
        const auto box = enve_cast<BoundingBox*>(layer);
        if (!box) { return false; }
        switch (box->getBoxType()) {
        case eBoxType::text:
        case eBoxType::adjustmentLayer:
        case eBoxType::solid:
        case eBoxType::vectorPath:
        case eBoxType::circle:
        case eBoxType::rectangle:
        case eBoxType::layer:
            return true;
        default:
            return false;
        }
    };
    // typed 待收编层：建轨后当场按 id 绑定（不记录 lane 索引——
    // 索引随逐条插入漂移，旧路径四层全落进最后一条轨=删除/撤销后
    // 轨道忽多忽少的二次塌方根因）
    {
        // 装载中不收编：层先于 spec 表读入（positional tail），
        // 此刻“trackId 无轨”全是表未到的假象，装载完成后的正式
        // refresh 按文件表匹配
        int insertAt = -1;
        const int mainId = mainTrackId();
        for (int t = 0; t < mTracks.size(); ++t) {
            if (mTracks.at(t).id == mainId) { insertAt = t; break; }
        }
        if (insertAt < 0) {
            insertAt = int(mTracks.size()) - int(std::count_if(
                        specs.begin(), specs.end(),
                        [](const eTrackSpec &sp) { return sp.mAudio; }));
        }
        bool anyTyped = false;
        if (!loadingProject) {
            for (int i2 = 0; i2 < items.size(); ++i2) {
                if (items[i2].second || !isTypedLayer(items[i2].first)) { continue; }
                if (laneById.contains(items[i2].first->trackId())) { continue; }
                anyTyped = true;
                break;
            }
        }
        if (anyTyped && scene) {
            // specs 里主轨的位置（addTrackSpec 用 specs 索引）
            int specMainIdx = -1;
            for (int k = 0; k < specs.size(); ++k) {
                if (specs.at(k).mId == mainId) { specMainIdx = k; break; }
            }
            if (specMainIdx < 0) {
                specMainIdx = int(specs.size()) - int(std::count_if(
                            specs.begin(), specs.end(),
                            [](const eTrackSpec &sp) { return sp.mAudio; }));
            }
            mInWriteback = true;
            for (int i2 = 0; i2 < items.size(); ++i2) {
                if (items[i2].second || !isTypedLayer(items[i2].first)) { continue; }
                if (laneById.contains(items[i2].first->trackId())) { continue; }
                // 文字层用固定的「文字」轨名（专属文字轨语
                // 感），其余用层名
                const auto tb = enve_cast<BoundingBox*>(items[i2].first);
                const bool isText = tb &&
                        tb->getBoxType() == eBoxType::text;
                const QString laneName = isText ? QObject::tr("文字")
                        : items[i2].first->prp_getName();
                int newId = -1;
                {
                    const auto undoBlock = scene->blockUndoRedo();
                    newId = scene->addTrackSpec(false, laneName, specMainIdx);
                    if (newId >= 0) { scene->setTrackSpecHeight(newId, 0); }
                }
                if (newId < 0) { continue; }
                qInfo("[TRK] adopt-spawn layer=%s lane=%s id=%d",
                      qUtf8Printable(items[i2].first->prp_getName()),
                      qUtf8Printable(laneName), newId);
                // 当场绑定：层立刻认领新轨，下方 membership 按
                // trackId 命中（索引漂移无关了）
                items[i2].first->setTrackId(newId);
                specs.insert(specMainIdx,
                             eTrackSpec{}); // 占位对齐（下面立刻重建）
                mTracks.insert(insertAt, {newId, laneName,
                                          false, false, false, false, 0});
                laneById.clear();
                for (int t = 0; t < mTracks.size(); ++t) {
                    laneById.insert(mTracks.at(t).id, t);
                }
            }
            mInWriteback = false;
        }
    }

    // lane membership: the layer's trackId; unknown ids (fresh
    // imports, scene-born layers) adopt their display lane RIGHT
    // HERE - a persistent -1 would re-park on whatever lane is
    // topmost on every later refresh and starve the purge counting
    // (one-time write, no undo entry, same pattern as the migration)
    const int trackCount = mTracks.size();
    QVector<int> lane(items.size(), 0);
    QVector<QList<eBoxOrSound*>> laneMembers(trackCount);
    QList<QPair<eBoxOrSound*, int>> adoptions; // layer, laneIdx
    for (int i = 0; i < items.size(); ++i) {
        int laneIdx = laneById.value(items[i].first->trackId(), -1);
        if (laneIdx < 0) {
            // CapCut: imported media lands on the MAIN track -
            // typed lanes sit ABOVE main, so "first matching
            // lane" would capture every later import onto a
            // 文字/图形矮轨; main first, other lanes as fallback
            if (!items[i].second) {
                const int mid = mainTrackId();
                for (int t = 0; t < trackCount; ++t) {
                    if (mTracks.at(t).id == mid) { laneIdx = t; break; }
                }
            }
            if (laneIdx < 0) {
                for (int t = 0; t < trackCount; ++t) {
                    if (mTracks.at(t).audio == items[i].second) {
                        laneIdx = t;
                        break;
                    }
                }
            }
            // 装载中不写回停靠：表未到，写回即污染（见 loadingProject）
            if (laneIdx >= 0 && !loadingProject) {
                adoptions.append({items[i].first, laneIdx});
            }
        }
        if (laneIdx < 0) { laneIdx = 0; }
        lane[i] = laneIdx;
        laneMembers[laneIdx].append(items[i].first);
    }

    // typed-only 轨道（文字/图形/固态/调整/矢量容器这些无媒体层）
    // 默认压到最小行高 36（CapCut 矮轨）；用户手调过的
    // （spec.mHeight > 0）不动
    {
        for (int t = 0; t < trackCount; ++t) {
            if (mTracks.at(t).audio || mTracks.at(t).height > 0) { continue; }
            const auto &members = laneMembers.at(t);
            if (members.isEmpty()) { continue; }
            bool allTyped = true;
            for (const auto l : members) {
                if (!isTypedLayer(l)) { allTyped = false; break; }
            }
            if (allTyped) { mTracks[t].height = 36; }
        }
    }
    if (!adoptions.isEmpty() && scene) {
        mInWriteback = true;
        {
            const auto undoBlock = scene->blockUndoRedo();
            for (const auto &a : adoptions) {
                a.first->setTrackId(mTracks.value(a.second).id);
            }
        }
        mInWriteback = false;
    }

    // 无窗口层补窗口（CapCut 新元素语义）：画布形状、调整层、固态
    // 层、矢量容器、文字这些菜单/工具/脚本新建的层没有 durRect——
    // 在时间轴上表现为整条场景兜底长块，且拖动/修剪落不了地
    // （setLayerRange 对 null 跳过）。一次性补上：锚定播放头，默认
    // 5 秒（与收编同款的一次性写入，不进撤销栈）
    if (scene) {
        const int head = scene->anim_getCurrentAbsFrame();
        const int len = qMax(1, qRound(5. * mFps));
        bool anyWindowless = false;
        for (int i = 0; i < items.size(); ++i) {
            if (items[i].second) { continue; } // sounds always own one
            if (items[i].first->getDurationRectangle()) { continue; }
            anyWindowless = true;
            break;
        }
        if (anyWindowless) {
            mInWriteback = true;
            const auto undoBlock = scene->blockUndoRedo();
            for (int i = 0; i < items.size(); ++i) {
                const auto layer = items[i].first;
                if (items[i].second ||
                        layer->getDurationRectangle()) { continue; }
                const auto dur = enve::make_shared<
                            FixedLenAnimationRect>(*layer);
                dur->setMinAbsFrame(head);
                dur->setMaxAbsFrame(head + len - 1);
                // QSharedPointer<FixedLen> converts to
                // QSharedPointer<DurationRectangle> implicitly
                layer->setDurationRectangle(dur);
            }
            mInWriteback = false;
        }
    }
    // ---- FixedLen 源映射锚治愈：视频块的健康不变量 = 可见窗 ⊆ 源
    // anim 窗（源长 ≥ 可见长 + 跳过的头）。历史写回（拖拽落位/监视
    // 插入/装载前的旧工程）只挪 min/max 不携动 animMin，块整段落在
    // anim 范围外——pixId 恒钳尾帧（画面冻在一帧）且 identical 范围
    // 整条剪辑（渲染数据跨帧复用，挂上去的转场特效位移随重渲时机
    // 乱跳 = 画面闪烁）。发现脱钩即重锚到 minRel（按 in=0 语义从源
    // 头播放；健康剪辑此判定恒假零开销）——一次性写入不进撤销栈，
    // 装载中跳过（表未到，同收编）
    if (scene && !loadingProject) {
        bool anyAnchorBroken = false;
        for (int i = 0; i < items.size(); ++i) {
            const auto layer = items[i].first;
            if (items[i].second) { continue; }
            const auto ab = enve_cast<AnimationBox*>(layer);
            if (!ab || !layer->getDurationRectangle()) { continue; }
            const auto flar = layer->getDurationRectangle()->
                    ref<FixedLenAnimationRect>();
            if (!flar) { continue; }
            const int aMin = flar->getMinAnimRelFrame();
            const int aMax = flar->getMaxAnimRelFrame();
            if (aMax - aMin < 1) { continue; }
            const auto vis = layer->getDurationRectangle()->getRelFrameRange();
            if (vis.fMin >= aMin && vis.fMax <= aMax) { continue; }
            anyAnchorBroken = true;
            break;
        }
        if (anyAnchorBroken) {
            mInWriteback = true;
            const auto undoBlock = scene->blockUndoRedo();
            for (int i = 0; i < items.size(); ++i) {
                const auto layer = items[i].first;
                if (items[i].second) { continue; }
                const auto ab = enve_cast<AnimationBox*>(layer);
                if (!ab || !layer->getDurationRectangle()) { continue; }
                const auto flar = layer->getDurationRectangle()->
                        ref<FixedLenAnimationRect>();
                if (!flar) { continue; }
                const int aMin = flar->getMinAnimRelFrame();
                const int aMax = flar->getMaxAnimRelFrame();
                if (aMax - aMin < 1) { continue; }
                const auto vis = layer->getDurationRectangle()->
                        getRelFrameRange();
                if (vis.fMin >= aMin && vis.fMax <= aMax) { continue; }
                flar->setFirstAnimationFrame(vis.fMin);
                // 锚是静默字段：内容变了必须显式失效（userChange 推
                // 状态版本号+作废渲染缓存，与编辑同款语义）
                ab->planUpdate(UpdateReason::userChange);
            }
            mInWriteback = false;
        }
    }
    // ---- 转场孤儿重叠回贴：剥特效不经模型的路径（属性面板"删除
    // 特效"、撤销半步回放等）会把转场右块留在左移位——主轨相邻对
    // 重叠而右块已无转场特效 = 布局残缺（磁吸压实只闭隙不拆叠，
    // 重叠永续；之后一切命中/拖拽/磁吸规划都在坏布局上进行）。
    // 发现即回贴右块恢复贴邻：按起始序扫描，无特效却沉入前块出点
    // 的右移差额；有特效（活转场）容忍重叠（窗口语义）。一次性写
    // 入不进撤销栈，装载中跳过（同锚治愈）
    if (scene && !loadingProject) {
        const int orphanMainId = mainTrackId();
        if (orphanMainId >= 0) {
            QList<const Clip*> order;
            for (const auto &c : mClips) {
                if (c.trackId != orphanMainId || c.audio) { continue; }
                order.append(&c);
            }
            std::sort(order.begin(), order.end(),
                      [](const Clip *a, const Clip *b) {
                          if (a->start != b->start) {
                              return a->start < b->start;
                          }
                          return a->clipId < b->clipId;
                      });
            QList<QPair<eBoxOrSound*, int>> orphanFixes;
            int prevEnd = -1; // 前块出点（含已回贴修正的运行值）
            for (const Clip *c : order) {
                int start = c->start;
                if (prevEnd > start) {
                    const auto box = enve_cast<BoundingBox*>(
                                c->layer.data());
                    if (!transitionEffectOn(box)) {
                        orphanFixes.append({c->layer.data(),
                                            prevEnd - start});
                        start = prevEnd;
                    }
                }
                prevEnd = qMax(prevEnd, start + c->duration);
            }
            if (!orphanFixes.isEmpty()) {
                mInWriteback = true;
                const auto undoBlock = scene->blockUndoRedo();
                for (const auto &f : orphanFixes) {
                    shiftLayer(f.first, f.second);
                }
                mInWriteback = false;
                // 模型表同步到终态（本分支无后续重建，同步防
                // modelChanged 画旧位——磁吸兜底同款收尾）
                for (const auto &f : orphanFixes) {
                    for (auto &row : mClips) {
                        if (row.layer.data() == f.first) {
                            row.start += f.second;
                            break;
                        }
                    }
                }
            }
        }
    }
    // ---- solo 压制：同类任一轨独奏时未独奏轨的成员层隐藏。
    // 压制即层可见性（声音可听性=可见性，与静音同一机制），恢复恒
    // visible=true（剪映语义：取消独奏该回来的都回来）。快照仅内存，
    // 工程文件只持久化 spec 上的 solo 旗标
    if (scene && trackCount > 0) {
        QSet<eBoxOrSound*> live;
        for (const auto &it : items) {
            if (it.first) { live.insert(it.first); }
        }
        // 剪枝已删图层：只比指针不解引用
        for (auto it = mSoloSaved.begin(); it != mSoloSaved.end();) {
            if (!live.contains(it.key())) { it = mSoloSaved.erase(it); }
            else { ++it; }
        }
        mSoloSuppressed.intersect(live);

        mInWriteback = true;
        const auto undoBlock = scene->blockUndoRedo();
        // CapCut 语义：独奏=全场景只留独奏轨（跨类型）——solo 任一
        // 轨后，其他所有轨（含另一类型）的成员层一律隐藏/静音；
        // 同类型各自的 anySolo 判定改为全局判定
        bool anySoloAny = false;
        for (const auto &o : mTracks) {
            if (o.solo) { anySoloAny = true; break; }
        }
        for (int t = 0; t < trackCount; ++t) {
            const bool anySolo = anySoloAny;
            for (eBoxOrSound* l : laneMembers[t]) {
                if (!l) { continue; }
                if (anySolo && !mTracks.at(t).solo) {
                    // 记录压制前的真实可见性（首次压制时），恢复时写
                    // 回它——用户静音/停用的层取消独奏后不能被点亮
                    if (!mSoloSaved.contains(l)) {
                        mSoloSaved.insert(l, l->isVisible());
                    }
                    if (l->isVisible()) { l->setVisible(false); }
                    mSoloSuppressed.insert(l);
                } else if (mSoloSaved.contains(l)) {
                    // 恢复到压制前的可见性（而非恒 true）；停用机制
                    // 优先：停用片段取消独奏后保持隐藏，由启用片段
                    // 的台账（mDisabledSaved）决定它回来时的可见性
                    const bool savedVis = mSoloSaved.take(l);
                    mSoloSuppressed.remove(l);
                    bool vis = savedVis;
                    const int cid = mLayerToClipId.value(l, -1);
                    if (cid >= 0 && mDisabledIds.contains(cid)) { vis = false; }
                    if (l->isVisible() != vis) { l->setVisible(vis); }
                }
            }
        }
        mInWriteback = false;
    }

    // mute mirror + default numbering for unnamed specs
    for (int t = 0; t < trackCount; ++t) {
        // 独奏压制的层不算用户隐藏，避免独奏期间整排轨误显静音；
        // 停用片段同样不算（停用=块级临时禁用，与轨道静音无关）
        bool anyMember = false;
        bool allHidden = true;
        for (eBoxOrSound* l : laneMembers[t]) {
            if (!l) { continue; }
            // solo 压制层计入 hidden：被独奏压制的轨在时间轴上同步
            // 压暗+喇叭红，视觉反馈即时（不再"看似没刷新"）
            const int cid = mLayerToClipId.value(l, -1);
            if (cid >= 0 && mDisabledIds.contains(cid)) { continue; }
            anyMember = true;
            if (l->isVisible()) { allHidden = false; break; }
        }
        mTracks[t].muted = anyMember && allHidden;
        if (mTracks[t].name.isEmpty()) {
            mTracks[t].name = mTracks[t].audio
                    ? QStringLiteral("A%1").arg(trackCount - t)
                    : QStringLiteral("V%1").arg(videoCount - t);
        }
    }

    // rebuild the clip table with stable ids: a layer keeps its clip
    // id across refreshes so the view's media caches survive (stale
    // dead-pointer keys are harmless bookkeeping)
    const qreal fps = scene ? scene->getFps() : 0.;
    if (fps > 0.) { mFps = fps; }
    const int fallbackLen = scene ? scene->getFrameRange().fMax : 0;
    mClips.clear();
    for (int i = 0; i < items.size(); ++i) {
        const auto &it = items[i];
        const auto dur = it.first->getDurationRectangle();
        int start = 0;
        int length = qMax(1, fallbackLen);
        if (dur) {
            // absolute frames: the native timeline draws and hit-tests
            // the duration bar in abs space
            const int minF = dur->getMinAbsFrame();
            const int maxF = dur->getMaxAbsFrame();
            start = minF;
            length = maxF - minF + 1;
        }
        Clip c;
        c.layer = it.first;
        c.clipId = mLayerToClipId.value(it.first, 0);
        if (c.clipId == 0) {
            c.clipId = mNextClipId++;
            mLayerToClipId.insert(it.first, c.clipId);
        }
        c.name = it.first->prp_getName();
        c.audio = it.second;
        // playback rate for the clip badge: stretch > 1 = slower, the
        // rate shown to the editor is its inverse
        qreal stretch = 1.;
        if (const auto sndObj = enve_cast<eSoundObjectBase*>(it.first)) {
            stretch = sndObj->getStretch();
        } else if (const auto animBox = enve_cast<AnimationBox*>(it.first)) {
            stretch = animBox->getStretch();
        }
        c.speed = stretch > 0. ? 1. / stretch : 1.;
        c.trackId = mTracks.value(lane[i]).id;
        c.start = qMax(0, start);
        c.duration = qMax(1, length);
        mClips.append(c);
        if (selectionIds.contains(c.clipId)) { mSelected.insert(c.clipId); }
    }
    // 剪枝：已删除图层的 id 映射清掉（指针键只比不解引用）
    {
        QSet<eBoxOrSound*> liveLayers;
        for (const auto &it : items) {
            if (it.first) { liveLayers.insert(it.first); }
        }
        for (auto it = mLayerToClipId.begin(); it != mLayerToClipId.end();) {
            if (!liveLayers.contains(it.key())) { it = mLayerToClipId.erase(it); }
            else { ++it; }
        }
        // 块装饰台账同剪（clipId 永不复用，层没了就是死键）
        QSet<int> liveIds;
        for (const auto &c : mClips) { liveIds.insert(c.clipId); }
        mDisabledIds.intersect(liveIds);
        for (auto it = mColorMarks.begin(); it != mColorMarks.end();) {
            if (!liveIds.contains(it.key())) { it = mColorMarks.erase(it); }
            else { ++it; }
        }
        for (auto it = mDisabledSaved.begin(); it != mDisabledSaved.end();) {
            if (!liveIds.contains(it.key())) { it = mDisabledSaved.erase(it); }
            else { ++it; }
        }
    }

    {
        QString ts, cs;
        for (const auto &t : mTracks) {
            ts += QString("[%1%2] ").arg(t.audio ? 'A' : 'V').arg(t.id); }
        for (const auto &c : mClips) {
            cs += QString("{c%1 tr%2} ").arg(c.clipId).arg(c.trackId); }
    }
    qDebug("[NLE] refresh scene=%s items=%d tracks=%d clips=%d",
           scene ? scene->prp_getName().toUtf8().constData() : "-",
           items.size(), trackCount, mClips.size());

    // NLE semantics: the scene timeline must cover the content. The
    // scene keeps its default range when the user declines the
    // adjust-scene dialog, and a playhead beyond scene max makes
    // DirectPlayer::play restart from the in point ("always plays
    // from the beginning"). Grow the range to the last clip's end.
    // 删短内容（删除/右缘修剪）后同样把范围收回内容末尾：留 1 秒
    // 余量防修剪抖动，且只在发生过删除/修剪的刷新里评估，用户在
    // 场景设置里手定的长范围不会被普通编辑悄悄改掉（UI 态写入）
    if (scene && !mClips.isEmpty()) {
        int contentEnd = 0;
        for (const auto &c : mClips) {
            contentEnd = qMax(contentEnd, c.start + c.duration);
        }
        const auto range = scene->getFrameRange();
        int newMax = range.fMax;
        if (contentEnd > range.fMax) {
            newMax = contentEnd;
        } else if (mCheckRangeShrink &&
                   range.fMax - contentEnd > qRound(mFps)) {
            newMax = qMax(range.fMin, contentEnd);
        }
        mCheckRangeShrink = false;
        if (newMax != range.fMax) {
            mInWriteback = true;
            scene->setFrameRange({range.fMin, newMax}, false);
            mInWriteback = false;
            if (scene->anim_getCurrentAbsFrame() > newMax) {
                scene->anim_setAbsFrame(newMax);
            }
        }
    }

    // ---- 主轨磁吸常驻兜底（CapCut 语义）：普通删除、变速缩短、
    // 旧工程等任何来源在主轨留下的缺口，随本次刷新立即闭合，不再
    // 等下一次拖拽提交。拖拽路径 commitMoves 已自带压实，这里通常
    // 空转。锚定在移动块头部的覆盖块随动；锚点消失（落在被删缺口
    // 里）则不随。写入走 writeback 守卫+blockUndoRedo（UI 态）
    if (mMagnetic && scene) {
        const int mainId = mainTrackId();
        if (mainId >= 0) {
            const auto moves = compactGapsPlan();
            if (!moves.isEmpty()) {
                QHash<int, int> deltas;
                for (const auto &m : moves) {
                    const auto oc = clip(m.clipId);
                    if (oc) { deltas.insert(m.clipId, m.start - oc->start); }
                }
                // 覆盖块随动锚点必须在旧布局上解析（任何表更新之前）
                struct Ride { int clipId; int newStart; };
                QList<Ride> rides;
                if (mFollowLinked) {
                    // 剪映联动含音效：音频块同样跟随锚定的主轨块
                    for (const auto &o : mClips) {
                        if (o.trackId == mainId || !o.layer) { continue; }
                        const Clip *anchor = nullptr;
                        for (const auto &mc : mClips) {
                            if (mc.audio || mc.trackId != mainId) { continue; }
                            if (mc.start <= o.start &&
                                    o.start < mc.start + mc.duration) {
                                anchor = &mc;
                                break;
                            }
                        }
                        if (!anchor) { continue; }
                        const int delta = deltas.value(anchor->clipId, 0);
                        if (delta != 0) {
                            rides.append({o.clipId,
                                          qMax(0, o.start + delta)});
                        }
                    }
                }
                mInWriteback = true;
                const auto undoBlock = scene->blockUndoRedo();
                for (const auto &m : moves) {
                    const auto oc = clip(m.clipId);
                    if (oc && oc->layer) { shiftLayer(oc->layer.data(), m.start - oc->start); }
                }
                for (const auto &r : rides) {
                    const auto oc = clip(r.clipId);
                    if (oc && oc->layer) {
                        shiftLayer(oc->layer.data(),
                                   r.newStart - oc->start);
                    }
                }
                mInWriteback = false;
                // 模型表同步到终态：本分支没有后续重建，不同步的话
                // 本次 modelChanged 画的还是移动前的旧位，要等下一
                // 次恰好路过的刷新才自愈（用户感知=磁吸刷新迟钝）
                const auto syncRow = [this](const int clipId,
                                            const int newStart) {
                    for (auto &row : mClips) {
                        if (row.clipId == clipId) {
                            row.start = qMax(0, newStart);
                            return;
                        }
                    }
                };
                for (const auto &m : moves) { syncRow(m.clipId, m.start); }
                for (const auto &r : rides) { syncRow(r.clipId, r.newStart); }
            }
        }
    }

    // 刷新内发生的文档可见性写入（solo 压制/恢复、停用/启用）需要
    // actionFinished 泵一次才入队渲染：planUpdate 只挂起更新，
    // requestTrackToggleSolo/requestSetDisabled 全程不调
    // actionFinished，画布就停在旧帧直到下一次无关交互（用户感知
    // =solo 后画面不刷新/迟迟才刷）。updateScenes 只做排队，无同步
    // 重入风险
    if (Document::sInstance) { Document::sInstance->actionFinished(); }

    emit modelChanged();
    emit guidesChanged();
    if (scene) { emit playheadFrameChanged(scene->anim_getCurrentAbsFrame()); }
}
