#include "nletimelinemodel.h"

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
#include "Boxes/animationbox.h"
#include "smartPointers/ememory.h"

#include <QDebug>
#include <algorithm>
#include <climits>

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
    // the video group renders top-down with V1 at its bottom, so the
    // LAST video entry in the panel order is the main track
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
    // kept as-is, so only genuine gaps close
    const int mainId = mainTrackId();
    QVector<Move> moves;
    for (const auto &t : mTracks) {
        if (t.id != mainId) { continue; }
        QList<QPair<int, int>> order; // start, clipId
        for (const auto &c : mClips) {
            if (c.trackId == t.id) { order.append({c.start, c.clipId}); }
        }
        if (order.size() < 2) { continue; }
        std::sort(order.begin(), order.end());
        int cursor = -1;
        for (const auto &p : order) {
            const auto c = clip(p.second);
            if (!c) { continue; }
            int newStart = c->start;
            if (cursor >= 0 && c->start > cursor) { newStart = cursor; }
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
    // rel/abs shift is constant, so abs deltas are valid rel moves
    if (newMin != oldMin) {
        dur->startMinFramePosTransform();
        dur->moveMinFrame(newMin - oldMin);
        dur->finishMinFramePosTransform();
    }
    if (newMax != oldMax) {
        dur->startMaxFramePosTransform();
        dur->moveMaxFrame(newMax - oldMax);
        dur->finishMaxFramePosTransform();
    }
}

void NleTimelineModel::shiftLayer(eBoxOrSound * const layer,
                                  const int frameDelta)
{
    if (!layer || frameDelta == 0) { return; }
    const auto dur = layer->getDurationRectangle();
    if (!dur) { return; }
    dur->startMinFramePosTransform();
    dur->moveMinFrame(frameDelta);
    dur->finishMinFramePosTransform();
    dur->startMaxFramePosTransform();
    dur->moveMaxFrame(frameDelta);
    dur->finishMaxFramePosTransform();
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
                int newStart = c->start;
                if (cursor >= 0 && c->start > cursor) { newStart = cursor; }
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
    // write so the table is still pristine
    {
        const int mainId = mainTrackId();
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
                for (const auto &o : mClips) {
                    if (o.audio || o.trackId == mainId) { continue; }
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
    const auto specs = scene->getTrackSpecs();
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
            if (s.mAudio == audio && members.value(s.mId, 0) == 0) {
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
    scene->clearBoxesSelection();
    for (auto *b : boxes) { scene->addBoxToSelection(b); }
    scene->splitBoxesAtFrame(frame);
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
    for (const int id : mSelected) {
        const auto c = clip(id);
        if (!c || !c->layer) { continue; }
        const auto snd = enve_cast<eIndependentSound*>(c->layer.data());
        if (snd) { sounds << snd; continue; }
        const auto box = enve_cast<BoundingBox*>(c->layer.data());
        if (box) { boxes << box; }
    }
    // CapCut-style fallback: no selection cuts EVERY unlocked-track
    // clip under the frame in one press (sounds included)
    if (boxes.isEmpty() && sounds.isEmpty()) {
        for (const auto &c : mClips) {
            if (!(c.start <= frame && frame < c.start + c.duration)) { continue; }
            if (trackLocked(c.trackId)) { continue; }
            if (!c.layer) { continue; }
            const auto snd = enve_cast<eIndependentSound*>(c.layer.data());
            if (snd) { sounds << snd; continue; }
            const auto box = enve_cast<BoundingBox*>(c.layer.data());
            if (box) { boxes << box; }
        }
    }
    if (boxes.isEmpty() && sounds.isEmpty()) { return false; }
    bool did = false;
    if (!boxes.isEmpty()) { did = splitBoxes(boxes, frame) || did; }
    if (!sounds.isEmpty()) {
        scene->splitSoundsAtFrame(sounds, frame);
        did = true;
    }
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
    bool did = false;
    if (!boxes.isEmpty()) { did = splitBoxes(boxes, frame) || did; }
    if (!sounds.isEmpty()) {
        scene->splitSoundsAtFrame(sounds, frame);
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
    for (auto *b : animBoxes) { b->freezeFrameAction(); }
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
    if (mInWriteback || mGestureActive) { return false; }
    const auto scene = mPanelScene.data();
    if (!scene || mFps <= 0.) { return false; }

    QList<eBoxOrSound*> victims;
    QSet<int> victimIds;
    for (const int id : clipIds) {
        const auto c = clip(id);
        if (!c || !c->layer) { continue; }
        victims << c->layer.data();
        victimIds << id;
    }
    if (victims.isEmpty()) { return false; }

    if (ripple) {
        // per track: every later clip slides left by the total length
        // of the removed clips that started before it (exact for
        // chains, in frames)
        QSet<int> affectedTracks;
        for (const int id : victimIds) {
            const auto c = clip(id);
            if (c) { affectedTracks.insert(c->trackId); }
        }
        for (const int trackId : affectedTracks) {
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
        // CapCut overlay following: clips on NON-main video tracks
        // slide by the MAIN-track victims removed before them (their
        // anchors are gone; everything after closes up)
        const int mainId = mainTrackId();
        if (mainId >= 0) {
            for (const auto &o : mClips) {
                if (o.audio || o.trackId == mainId) { continue; }
                if (victimIds.contains(o.clipId) || !o.layer) { continue; }
                int removedBefore = 0;
                for (const int vid : victimIds) {
                    const auto v = clip(vid);
                    if (v && !v->audio && v->trackId == mainId &&
                            v->start < o.start) {
                        removedBefore += v->duration;
                    }
                }
                if (removedBefore > 0) { shiftLayer(o.layer.data(), -removedBefore); }
            }
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
    mInWriteback = false;
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
    // kdenlive 方案A: dragged clip = drop frame 1:1; left package (end
    // <= drop) compacts toward 0; everything else chains tightly after
    // the dragged clip's new out point. Existing overlaps inside a
    // package keep their relative layout (compaction closes gaps
    // only, the right chain serializes). A clip straddling the drop
    // point belongs to the right package and moves as a whole.
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

    int cursor = 0;
    for (const Clip *c : left) {
        if (cursor != c->start) {
            moves.append({c->clipId, trackId, cursor, c->duration});
        }
        cursor = cursor + c->duration; // kdenlive compaction: cursor
                                       // advances by playtime, old
                                       // gaps never re-enter the pack
    }
    moves.append({draggedId, trackId, drop, dur});
    cursor = drop + dur;
    for (const Clip *c : right) {
        if (cursor != c->start) {
            moves.append({c->clipId, trackId, cursor, c->duration});
        }
        cursor = cursor + c->duration; // tight chain: no gaps
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
        if (s) { emit playheadFrameChanged(s->anim_getCurrentAbsFrame()); }
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
    qDebug("[NLE] track migration: %d tracks installed", specs.size());
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
    // a pre-P0 project) installs the derived legacy layout once
    auto specs = scene ? scene->getTrackSpecs() : QList<eTrackSpec>();
    if (scene && specs.isEmpty()) {
        // the setTrackId calls inside must not re-enter the refresh,
        // and must not leave undo entries (a load-time migration must
        // not be the user's first Ctrl+Z)
        mInWriteback = true;
        const auto undoBlock = scene->blockUndoRedo();
        specs = deriveTrackSpecs(scene, items);
        mInWriteback = false;
    }

    QHash<int, int> laneById;
    mTracks.clear();
    for (const auto &s : specs) {
        if (s.mAudio) { continue; }
        laneById.insert(s.mId, mTracks.size());
        mTracks.append({s.mId, s.mName, false, s.mLocked, false, s.mHeight});
    }
    const int videoCount = mTracks.size();
    for (const auto &s : specs) {
        if (!s.mAudio) { continue; }
        laneById.insert(s.mId, mTracks.size());
        mTracks.append({s.mId, s.mName, true, s.mLocked, false, s.mHeight});
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
            for (int t = 0; t < trackCount; ++t) {
                if (mTracks.at(t).audio == items[i].second) { laneIdx = t; break; }
            }
            if (laneIdx >= 0) { adoptions.append({items[i].first, laneIdx}); }
        }
        if (laneIdx < 0) { laneIdx = 0; }
        lane[i] = laneIdx;
        laneMembers[laneIdx].append(items[i].first);
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
    // mute mirror + default numbering for unnamed specs
    for (int t = 0; t < trackCount; ++t) {
        bool allHidden = !laneMembers[t].isEmpty();
        for (const auto *l : laneMembers[t]) {
            if (l->isVisible()) { allHidden = false; break; }
        }
        mTracks[t].muted = allHidden;
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

    emit modelChanged();
    emit guidesChanged();
    if (scene) { emit playheadFrameChanged(scene->anim_getCurrentAbsFrame()); }
}
