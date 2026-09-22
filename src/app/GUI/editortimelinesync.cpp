#include "editortimelinesync.h"
#include "editortimelinewidget.h"

#include "Private/document.h"
#include "GUI/timelinethumbprovider.h"
#include "Boxes/animationbox.h"
#include "Sound/esoundobjectbase.h"
#include "canvas.h"
#include "Boxes/containerbox.h"
#include "Boxes/boundingbox.h"
#include "Boxes/boxrenderdata.h"
#include "Animators/eboxorsound.h"
#include "Sound/esound.h"
#include "Timeline/durationrectangle.h"
#include "smartPointers/ememory.h"

#include <QMouseEvent>
#include <QTimer>
#include <QSet>
#include <QDebug>
#include <climits>
#include <algorithm>

EditorTimelineSync::EditorTimelineSync(Document &document,
                                       EditorTimelineWidget * const widget,
                                       QObject * const parent)
    : QObject(parent)
    , mDocument(document)
    , mWidget(widget)
{
    mWidget->installEventFilter(this);

    // context-menu merges and the magnetic toggle inside the widget:
    // persist clip times (the magnetic toggle slides clips), push the
    // lanes into the native track model, refresh from it right away
    connect(mWidget, &EditorTimelineWidget::trackLayoutChanged,
            this, [this]() {
        applyWriteback();
        applyTrackWriteback();
        rebuild();
    });

    connect(&mDocument, qOverload<Canvas*>(&Document::sceneCreated),
            this, &EditorTimelineSync::rebuild);
    connect(&mDocument, qOverload<Canvas*>(&Document::sceneRemoved),
            this, &EditorTimelineSync::rebuild);
    connect(&mDocument, &Document::activeSceneSet,
            this, &EditorTimelineSync::rebuild);

    mThumbProvider = new TimelineThumbProvider(this);
    connect(mThumbProvider, &TimelineThumbProvider::frameThumbReady,
            this, [this](const QString &key, const int animFrame,
                         const QImage &img) {
        const auto &routes = mFilmRoutes.value(
                    QStringLiteral("%1:%2").arg(key).arg(animFrame));
        for (const auto &r : routes) {
            mWidget->setClipThumbFrame(r.first, r.second, img);
        }
    });
    connect(mThumbProvider, &TimelineThumbProvider::wavePeaksReady,
            this, [this](const QString &key, const int relSecond,
                         const QVector<qreal> &peaks) {
        const auto &routes = mWaveRoutes.value(
                    QStringLiteral("%1:%2").arg(key).arg(relSecond));
        for (const auto &r : routes) {
            mWidget->setClipWave(r.first, r.second, peaks);
        }
    });

    // viewport changes (zoom / scroll / resize) re-request media for
    // the newly visible range; debounced so a zoom gesture is one pass
    mMediaTimer = new QTimer(this);
    mMediaTimer->setSingleShot(true);
    mMediaTimer->setInterval(250);
    connect(mMediaTimer, &QTimer::timeout,
            this, &EditorTimelineSync::requestMedia);

    // selection bridge: panel clip pick drives the canvas selection
    if (mWidget) {
        connect(mWidget, &EditorTimelineWidget::selectionChanged,
                this, [this](const QString&) { pushSelectionToCanvas(); });
        connect(mWidget, &EditorTimelineWidget::deleteRequested,
                this, [this](const bool ripple) { deleteSelectedClips(ripple); });
        connect(mWidget, &EditorTimelineWidget::splitAtPlayheadRequested,
                this, &EditorTimelineSync::splitAtPlayhead);
        connect(mWidget, &EditorTimelineWidget::razorCutRequested,
                this, [this](const QList<int> &ids, const double sec) {
            razorCut(ids, sec);
        });
        connect(mWidget, &EditorTimelineWidget::trackMuteToggleRequested,
                this, [this](const int idx, const bool allType) {
            toggleTrackMute(idx, allType);
        });
        connect(mWidget, &EditorTimelineWidget::trackRenameRequested,
                this, [this](const int idx, const QString &name) {
            const auto scene = mPanelScene.data();
            if (!scene || idx < 0 || idx >= mLaneSpecIds.size()) { return; }
            scene->setTrackSpecName(mLaneSpecIds.at(idx), name);
        });
        // explicit track lifecycle: new video tracks land on top of
        // the video block, new audio tracks below the audio block
        connect(mWidget, &EditorTimelineWidget::trackAddRequested,
                this, [this](const bool audio) {
            const auto scene = mPanelScene.data();
            if (!scene) { return; }
            int sameType = 0;
            for (const auto &s : scene->getTrackSpecs()) {
                if (s.mAudio == audio) { ++sameType; }
            }
            const QString name = audio
                    ? QStringLiteral("A%1").arg(sameType + 1)
                    : QStringLiteral("V%1").arg(sameType + 1);
            scene->addTrackSpec(audio, name);
            if (Document::sInstance) { Document::sInstance->actionFinished(); }
            rebuild();
        });
        connect(mWidget, &EditorTimelineWidget::trackRemoveRequested,
                this, [this](const int trackIdx) {
            const auto scene = mPanelScene.data();
            if (!scene || trackIdx < 0 || trackIdx >= mLaneSpecIds.size()) { return; }
            // the last lane of a type must survive: fresh layers of
            // that type would have no legal home
            const bool audio = mLaneAudio.at(trackIdx);
            int sameType = 0;
            for (const auto &a : mLaneAudio) { if (a == audio) { ++sameType; } }
            if (sameType <= 1) {
                mWidget->log(QStringLiteral("最后一个%1轨不可删除")
                             .arg(audio ? QStringLiteral("音频") : QStringLiteral("视频")));
                return;
            }
            if (scene->removeTrackSpec(mLaneSpecIds.at(trackIdx))) {
                if (Document::sInstance) { Document::sInstance->actionFinished(); }
                rebuild();
            } else {
                mWidget->log(QStringLiteral("轨道非空或不存在，未删除"));
            }
        });
        connect(mWidget, &EditorTimelineWidget::trackHeightChanged,
                this, [this](const int trackIdx, const int height) {
            const auto scene = mPanelScene.data();
            if (!scene || trackIdx < 0 || trackIdx >= mLaneSpecIds.size()) { return; }
            scene->setTrackSpecHeight(mLaneSpecIds.at(trackIdx), height);
        });
        connect(mWidget, &EditorTimelineWidget::trackLockChanged,
                this, [this](const int trackIdx, const bool locked) {
            const auto scene = mPanelScene.data();
            if (!scene || trackIdx < 0 || trackIdx >= mLaneSpecIds.size()) { return; }
            scene->setTrackSpecLocked(mLaneSpecIds.at(trackIdx), locked);
        });
        connect(mWidget, &EditorTimelineWidget::markerAddRequested,
                this, [this](const int frame) {
            const auto scene = mPanelScene.data();
            if (scene) { scene->setMarker(frame); rebuild(); }
        });
        connect(mWidget, &EditorTimelineWidget::viewChanged,
                this, [this]() {
            if (!mDragging && mMediaTimer) { mMediaTimer->start(); }
        });
        connect(mWidget, &EditorTimelineWidget::markerRemoveRequested,
                this, [this](const int frame) {
            const auto scene = mPanelScene.data();
            if (scene && scene->removeMarker(frame)) { rebuild(); }
        });
    }

    rebuild();
}

void EditorTimelineSync::connectPanelScene(Canvas * const scene)
{
    if (scene == mPanelScene.data()) { return; }
    for (const auto &conn : mSceneConns) { disconnect(conn); }
    mSceneConns.clear();
    mPanelScene = scene;
    if (!scene) { return; }

    mSceneConns << connect(scene, &Canvas::ca_childAdded,
                           this, [this](Property*) { rebuild(); });
    mSceneConns << connect(scene, &Canvas::ca_childRemoved,
                           this, [this](Property*) { rebuild(); });
    // native row reorder (drag in the layer panel / track writebacks /
    // undo) changes the lane order the panel derives from
    mSceneConns << connect(scene, &ContainerBox::movedObject,
                           this, [this](const int, const int, eBoxOrSound*) {
        rebuild();
    });
    mSceneConns << connect(scene, &Canvas::prp_currentFrameChanged,
                           this, [this](const UpdateReason) {
        updatePlayheadFromDoc();
    });
    mSceneConns << connect(scene, &Canvas::fpsChanged,
                           this, [this](const qreal) { rebuild(); });
}

void EditorTimelineSync::connectChildren(Canvas * const scene)
{
    for (const auto &conn : mChildConns) { disconnect(conn); }
    mChildConns.clear();
    if (!scene) { return; }
    for (const auto &child : scene->getContained()) {
        const auto layer = child.data();
        if (!layer) { continue; }
        mChildConns << connect(layer, &eBoxOrSound::prp_nameChanged,
                               this, [this](const QString&) { rebuild(); });
        // native track merge / split (layer-panel drags, context menus,
        // undo) must re-map the panel lanes
        mChildConns << connect(layer, &eBoxOrSound::trackIdChanged,
                               this, [this](const int) { rebuild(); });
        const auto dur = layer->getDurationRectangle();
        if (dur) {
            mChildConns << connect(dur, &DurationRectangle::minRelFrameChanged,
                                   this, [this](const int, const int) { rebuild(); });
            mChildConns << connect(dur, &DurationRectangle::maxRelFrameChanged,
                                   this, [this](const int, const int) { rebuild(); });
        }
    }
}

void EditorTimelineSync::rebuild()
{
    if (!mWidget) { return; }
    if (mInWriteback) { return; }
    if (mDragging) { mRebuildQueued = true; return; }

    // panel scene: follow the active scene, but when it has nothing to
    // edit (e.g. the user dove into a child scene), keep showing the
    // last scene that had blocks instead of blanking the panel
    const auto activeScene = mDocument.fActiveScene.data();
    QList<QPair<eBoxOrSound*, bool>> items;
    const auto collect = [&items](Canvas * const s) {
        items.clear();
        if (!s) { return; }
        // mirror the native timeline: EVERY child layer shows here.
        // Sounds become audio blocks; every visual layer (scene links,
        // vectors, images, text, groups, ...) becomes a video block
        // with the same thumbnail logic
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
    mClipToLayer.clear();

    // tracks are explicit persistent entities: video specs first (top
    // lane = first video spec), then audio specs. A virgin scene (or a
    // pre-P0 project) installs the derived legacy layout once
    auto specs = scene ? scene->getTrackSpecs() : QList<eTrackSpec>();
    if (scene && specs.isEmpty()) {
        // the setTrackId calls inside must not re-enter rebuild, and
        // must not leave undo entries (a load-time migration must not
        // be the user's first Ctrl+Z)
        mInWriteback = true;
        const auto undoBlock = scene->blockUndoRedo();
        specs = deriveTrackSpecs(scene, items);
        mInWriteback = false;
    }

    QVector<EditorTimelineWidget::TrackInfo> trackInfos;
    QHash<int, int> laneById;
    for (const auto &s : specs) {
        if (s.mAudio) { continue; }
        laneById.insert(s.mId, trackInfos.size());
        trackInfos.append({s.mId, s.mName, false, s.mLocked, false, s.mHeight});
    }
    const int videoCount = trackInfos.size();
    for (const auto &s : specs) {
        if (!s.mAudio) { continue; }
        laneById.insert(s.mId, trackInfos.size());
        trackInfos.append({s.mId, s.mName, true, s.mLocked, false, s.mHeight});
    }
    mLaneSpecIds.clear();
    mLaneAudio.clear();
    for (const auto &ti : trackInfos) {
        mLaneSpecIds.append(ti.id);
        mLaneAudio.append(ti.audio);
    }

    // lane membership: the layer's trackId; unknown ids (fresh layers)
    // park on the topmost track of their type until the next writeback
    // materializes the id - rebuild itself never mutates the doc
    QVector<int> lane(items.size(), 0);
    QVector<QList<eBoxOrSound*>> laneMembers(trackInfos.size());
    for (int i = 0; i < items.size(); ++i) {
        int laneIdx = laneById.value(items[i].first->trackId(), -1);
        if (laneIdx < 0) {
            for (int t = 0; t < trackInfos.size(); ++t) {
                if (trackInfos[t].audio == items[i].second) { laneIdx = t; break; }
            }
        }
        if (laneIdx < 0) { laneIdx = 0; }
        lane[i] = laneIdx;
        laneMembers[laneIdx].append(items[i].first);
    }
    // mute mirror + default numbering for unnamed specs
    for (int t = 0; t < trackInfos.size(); ++t) {
        bool allHidden = !laneMembers[t].isEmpty();
        for (const auto *l : laneMembers[t]) {
            if (l->isVisible()) { allHidden = false; break; }
        }
        trackInfos[t].muted = allHidden;
        if (trackInfos[t].name.isEmpty()) {
            trackInfos[t].name = trackInfos[t].audio
                    ? QStringLiteral("A%1").arg(trackInfos.size() - t)
                    : QStringLiteral("V%1").arg(videoCount - t);
        }
    }

    mWidget->setTracks(trackInfos);
    mWidget->clearAllClips();
    qDebug("[ETL] rebuild panel=%s items=%d video=%d audio=%d active=%s",
           scene ? scene->prp_getName().toUtf8().constData() : "-",
           items.size(), videoCount, trackInfos.size() - videoCount,
           mDocument.fActiveScene ?
               mDocument.fActiveScene->prp_getName().toUtf8().constData() : "-");

    if (!scene) { return; }
    const qreal fps = scene->getFps();
    if (fps > 0.) { mWidget->setFps(fps); }
    {
        // ruler markers from the scene (abs frames + titles)
        QVector<QPair<int, QString>> marks;
        for (const auto &m : scene->getMarkers()) {
            if (m.enabled) { marks.append({m.frame, m.title}); }
        }
        mWidget->setMarkers(marks);
    }
    const double fallbackLen = fps > 0. ?
                scene->getFrameRange().fMax / fps : 0.;
    for (int i = 0; i < items.size(); ++i) {
        const auto &it = items[i];
        double start = 0.;
        double len = fallbackLen;
        const auto dur = it.first->getDurationRectangle();
        if (dur && fps > 0.) {
            // absolute frames: the native timeline draws and hit-tests
            // the duration bar in abs space (durationrectangle.cpp draw)
            const int minF = dur->getMinAbsFrame();
            const int maxF = dur->getMaxAbsFrame();
            start = minF / fps;
            len = (maxF - minF + 1) / fps;
        }
        const int id = mWidget->appendClip(it.first->prp_getName(),
                                           start, len, it.second, lane[i]);
        mClipToLayer.insert(id, it.first);
    }
    requestMedia();
    updatePlayheadFromDoc();
}

QList<eTrackSpec> EditorTimelineSync::deriveTrackSpecs(
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
    // legal home (and the lane fallback would park it on a wrong-type
    // lane)
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
    qDebug("[ETL] track migration: %d tracks installed", specs.size());
    return specs;
}

void EditorTimelineSync::applyTrackWriteback()
{
    const auto scene = mPanelScene.data();
    if (!mWidget || !scene) { return; }
    if (mLaneSpecIds.isEmpty()) { return; }

    mInWriteback = true;

    // 1. lane -> trackId: pure membership. Moving a clip between lanes
    // is a trackId write, never a structural row surgery (the P0 core
    // invariant that turns timeline drags into real editing semantics)
    for (const auto &clip : mWidget->allClips()) {
        const auto layer = mClipToLayer.value(clip.id).data();
        if (!layer) { continue; }
        if (clip.track < 0 || clip.track >= mLaneSpecIds.size()) { continue; }
        const int specId = mLaneSpecIds.at(clip.track);
        if (layer->trackId() != specId) { layer->setTrackId(specId); }
    }

    // 2. row order: keep each track's members contiguous with the
    // tracks in spec order (the compositing-order invariant), but keep
    // the CURRENT relative order within a track - a same-lane time
    // slide reorders nothing. Reorder only when membership moved
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
            return mLaneSpecIds.indexOf(a->trackId())
                    < mLaneSpecIds.indexOf(b->trackId());
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

    mInWriteback = false;
    if (Document::sInstance) { Document::sInstance->actionFinished(); }
}

void EditorTimelineSync::updatePlayheadFromDoc()
{
    const auto scene = mPanelScene.data();
    if (!mWidget || !scene) { return; }
    const qreal fps = scene->getFps();
    if (fps <= 0.) { return; }
    mWidget->setPlayheadSec(scene->anim_getCurrentAbsFrame() / fps);
}

void EditorTimelineSync::syncPlayheadToDoc()
{
    const auto scene = mPanelScene.data();
    if (!mWidget || !scene) { return; }
    const qreal fps = scene->getFps();
    if (fps <= 0.) { return; }
    const int frame = qRound(mWidget->playheadTime() * fps);
    if (scene == mDocument.fActiveScene.data()) {
        if (frame != mDocument.getActiveSceneFrame()) {
            mDocument.setActiveSceneFrame(frame);
        }
    } else if (frame != scene->anim_getCurrentAbsFrame()) {
        scene->anim_setAbsFrame(frame);
    }
}

void EditorTimelineSync::applyWriteback()
{
    const auto scene = mPanelScene.data();
    if (!mWidget || !scene) { return; }
    const qreal fps = scene->getFps();
    if (fps <= 0.) { return; }

    mInWriteback = true;
    const auto clips = mWidget->allClips();
    for (const auto &clip : clips) {
        const auto layer = mClipToLayer.value(clip.id);
        if (!layer) { continue; }
        const auto dur = layer->getDurationRectangle();
        if (!dur) { continue; }
        const int newMin = qRound(clip.start * fps);
        const int newMax = qRound((clip.start + clip.length) * fps) - 1;
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
    mInWriteback = false;
}

void EditorTimelineSync::requestMedia()
{
    const auto scene = mPanelScene.data();
    if (!mWidget || !scene) { return; }
    const qreal fps = scene->getFps();
    if (fps <= 0.) { return; }
    if (!mThumbProvider) { return; }

    const double viewA = mWidget->viewStartSec();
    const double viewB = mWidget->viewEndSec();
    // filmstrip tile width in px at the current zoom (16:9 of a ~40px
    // strip body) drives the sampling interval: one decoded frame per
    // tile width, so the strip density follows the zoom level
    const int bodyH = 40;
    const double tileWpx = qMax(48., bodyH * 16. / 9.);
    const int intervalFrames = qMax(1, qRound(tileWpx / mWidget->pxPerSec() * fps));

    mFilmRoutes.clear();
    mWaveRoutes.clear();

    for (auto it = mClipToLayer.begin(); it != mClipToLayer.end(); ++it) {
        const int clipId = it.key();
        const auto layer = it.value().data();
        if (!layer) { continue; }
        const auto dur = layer->getDurationRectangle();
        if (!dur) { continue; }

        const auto soundObj = enve_cast<eSoundObjectBase*>(layer);
        if (soundObj) {
            // real waveform: peak columns for every visible second
            const int sec0 = qMax(0, int(viewA));
            const int sec1 = int(viewB) + 1;
            const int minSec = dur->getMinAbsFrame() / fps;
            const int maxSec = dur->getMaxAbsFrame() / fps + 1;
            const QString soundKey = QStringLiteral("%1")
                    .arg(reinterpret_cast<qulonglong>(soundObj), 0, 16);
            for (int absSec = sec0; absSec <= sec1; ++absSec) {
                if (absSec < minSec || absSec > maxSec) { continue; }
                const auto relRange = soundObj->absSecondToRelSeconds(absSec);
                if (!relRange.isValid()) { continue; }
                mWaveRoutes[
                        QStringLiteral("%1:%2").arg(soundKey).arg(relRange.fMin)
                        ].append({clipId, absSec});
                mThumbProvider->requestWavePeaks(soundKey, relRange.fMin,
                                                 soundObj);
            }
            continue;
        }

        const auto box = enve_cast<BoundingBox*>(layer);
        if (!box) { continue; }

        // video family (VideoBox / image sequences): decoded filmstrip
        const auto animBox = dynamic_cast<AnimationBox*>(box);
        if (animBox) {
            const auto handler = animBox->getAnimationFramesHandler();
            if (!handler) { continue; }
            const QString hKey = QStringLiteral("%1")
                    .arg(reinterpret_cast<qulonglong>(handler), 0, 16);
            const int f0 = qMax(dur->getMinAbsFrame(), qRound(viewA * fps));
            const int f1 = qMin(dur->getMaxAbsFrame(), qRound(viewB * fps));
            for (int f = f0; f <= f1; f += intervalFrames) {
                const qreal relFrame = box->prp_absFrameToRelFrameF(f);
                const int animFrame = animBox->getAnimationFrameForRelFrame(relFrame);
                mFilmRoutes[
                        QStringLiteral("%1:%2").arg(hKey).arg(animFrame)
                        ].append({clipId, f});
                mThumbProvider->requestFrameThumb(hKey, animFrame,
                                                  handler, bodyH);
            }
            continue; // no midpoint render for filmstrip clips
        }

        // other visual layers (images, vectors, text, groups): one real
        // WYSIWYG frame per block - the middle of its range (stable
        // across rebuilds; only a re-trim that moves the midpoint
        // re-renders)
        if (mThumbDone.size() > 128) { mThumbDone.clear(); }
        const int absMid = (dur->getMinAbsFrame() + dur->getMaxAbsFrame()) / 2;
        const qreal relFrame = box->prp_absFrameToRelFrameF(absMid);
        const QString key = QStringLiteral("%1:%2")
                .arg(reinterpret_cast<qulonglong>(box), 0, 16)
                .arg(qRound(relFrame));
        const auto cached = mThumbDone.constFind(key);
        if (cached != mThumbDone.constEnd()) {
            mWidget->setClipThumbnail(clipId, cached.value());
            continue;
        }
        if (mThumbPending.contains(key)) { continue; }
        // async offscreen render of the layer composition; result
        // marshalled back to the GUI thread, stale deliveries dropped
        // by clip id
        auto task = box->queExternalRender(relFrame, true);
        if (!task) { continue; }
        mThumbPending.insert(key);
        const std::weak_ptr<BoxRenderData> weak = task;
        const QPointer<EditorTimelineSync> self = this;
        task->addDependent({[self, weak, key]() {
            QImage img;
            const auto t = weak.lock();
            if (t && t->fRenderedImage) {
                SkPixmap pm;
                if (t->fRenderedImage->peekPixels(&pm)) {
                    QImage::Format fmt = QImage::Format_Invalid;
                    if (pm.colorType() == kBGRA_8888_SkColorType) {
                        fmt = QImage::Format_ARGB32_Premultiplied;
                    } else if (pm.colorType() == kRGBA_8888_SkColorType) {
                        fmt = QImage::Format_RGBA8888_Premultiplied;
                    }
                    if (fmt != QImage::Format_Invalid) {
                        img = QImage(reinterpret_cast<const uchar*>(pm.addr()),
                                     pm.width(), pm.height(),
                                     int(pm.rowBytes()), fmt).copy();
                    }
                }
            }
            QMetaObject::invokeMethod(self, [self, key, img]() {
                if (!self) { return; }
                self->mThumbPending.remove(key);
                if (img.isNull()) { return; }
                self->mThumbDone.insert(key, img);
                self->deliverThumbnail(key, img);
            }, Qt::QueuedConnection);
        }, [](){}});
    }
}

void EditorTimelineSync::deliverThumbnail(const QString &key, const QImage &img)
{
    const auto scene = mPanelScene.data();
    if (!mWidget || !scene) { return; }
    for (auto it = mClipToLayer.begin(); it != mClipToLayer.end(); ++it) {
        const auto box = enve_cast<BoundingBox*>(it.value().data());
        if (!box) { continue; }
        const auto dur = box->getDurationRectangle();
        if (!dur) { continue; }
        const int absMid = (dur->getMinAbsFrame() + dur->getMaxAbsFrame()) / 2;
        const qreal relFrame = box->prp_absFrameToRelFrameF(absMid);
        const QString curKey = QStringLiteral("%1:%2")
                .arg(reinterpret_cast<qulonglong>(box), 0, 16)
                .arg(qRound(relFrame));
        if (curKey == key) {
            mWidget->setClipThumbnail(it.key(), img);
        }
    }
}

bool EditorTimelineSync::eventFilter(QObject * const obj, QEvent * const ev)
{
    if (obj == mWidget.data()) {
        const auto type = ev->type();
        if (type == QEvent::MouseButtonPress) {
            const auto me = static_cast<QMouseEvent*>(ev);
            if (me->button() == Qt::LeftButton) { mDragging = true; }
        } else if (type == QEvent::Hide) {
            // dock toggled off: never leave the dragging lock behind, it
            // would freeze rebuilds until the next click-release
            if (mDragging) { qDebug("[ETL] hidden while dragging, unlock"); }
            mDragging = false;
        } else if (type == QEvent::MouseMove) {
            if (mDragging) { syncPlayheadToDoc(); }
        } else if (type == QEvent::MouseButtonRelease) {
            const auto me = static_cast<QMouseEvent*>(ev);
            if (me->button() == Qt::LeftButton && mDragging) {
                mDragging = false;
                syncPlayheadToDoc();
                // magnetic invariant: tracks hold no gaps after any edit
                if (mWidget->magnetic()) { mWidget->compactTrackGaps(); }
                applyWriteback();
                applyTrackWriteback();        // trackIds (+ row blocks)
                rebuild();
                mRebuildQueued = false;
            }
        }
    }
    return QObject::eventFilter(obj, ev);
}

void EditorTimelineSync::pushSelectionToCanvas()
{
    if (!mWidget || mInWriteback || mDragging) { return; }
    const auto ids = mWidget->selectedClipIds();
    if (ids.isEmpty()) { return; }
    const auto scene = mPanelScene.data();
    if (!scene) { return; }
    QList<BoundingBox*> boxes;
    for (const int id : ids) {
        const auto it = mClipToLayer.constFind(id);
        if (it == mClipToLayer.constEnd() || !it.value()) { continue; }
        const auto box = enve_cast<BoundingBox*>(it.value().data());
        if (box) { boxes << box; }
    }
    if (boxes.isEmpty()) { return; }
    const auto &cur = scene->getSelectedBoxesList();
    if (cur.count() == boxes.count()) {
        bool same = true;
        for (const auto *b : boxes) { if (!cur.contains(b)) { same = false; break; } }
        if (same) { return; }
    }
    scene->clearBoxesSelection();
    for (auto *b : boxes) { scene->addBoxToSelection(b); }
}

// ---------------------------------------------------------------- NLE ops

void EditorTimelineSync::shiftLayerFrames(eBoxOrSound * const layer,
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

void EditorTimelineSync::deleteSelectedClips(const bool ripple)
{
    if (!mWidget || mInWriteback || mDragging) { return; }
    const auto scene = mPanelScene.data();
    if (!scene) { return; }
    const qreal fps = scene->getFps();
    if (fps <= 0.) { return; }

    const auto ids = mWidget->selectedClipIds();
    if (ids.isEmpty()) { return; }

    // snapshot the pre-delete panel layout for the ripple math
    struct Snap { int track; double start; double length; };
    const auto clips = mWidget->allClips();
    QHash<int, Snap> snap;
    for (const auto &c : clips) { snap.insert(c.id, {c.track, c.start, c.length}); }

    QList<eBoxOrSound*> victims;
    QSet<int> victimIds;
    for (const int id : ids) {
        const auto layer = mClipToLayer.value(id).data();
        if (!layer) { continue; }
        victims << layer;
        victimIds << id;
    }
    if (victims.isEmpty()) { return; }

    if (ripple) {
        // per lane: every later clip slides left by the total length of
        // the removed clips that started before it (exact for chains)
        for (const int track : std::set<int>(
                 [&snap, &victimIds]() {
                     std::set<int> tracks;
                     for (auto it = snap.constBegin(); it != snap.constEnd(); ++it) {
                         if (victimIds.contains(it.key())) { tracks.insert(it.value().track); }
                     }
                     return tracks;
                 }())) {
            QList<QPair<double, eBoxOrSound*>> later;
            for (const auto &c : clips) {
                const bool removed = victimIds.contains(c.id);
                if (removed && snap.value(c.id).track == track) { continue; }
                const auto layer = mClipToLayer.value(c.id).data();
                if (!layer || snap.value(c.id).track != track) { continue; }
                double removedLenBefore = 0.;
                for (const int vid : victimIds) {
                    const auto vs = snap.value(vid);
                    if (vs.track == track && vs.start < c.start - 1e-9) {
                        removedLenBefore += vs.length;
                    }
                }
                if (removedLenBefore > 0.) { later.append({removedLenBefore, layer}); }
            }
            for (const auto &p : later) {
                shiftLayerFrames(p.second, -qRound(p.first * fps));
            }
        }
    }

    mInWriteback = true;
    for (auto *layer : victims) { layer->removeFromParent_k(); }
    mInWriteback = false;
    if (Document::sInstance) { Document::sInstance->actionFinished(); }
    rebuild();
}

// shared doc-side split: select the boxes, cut at the frame, finish
void EditorTimelineSync::splitBoxes(const QList<BoundingBox*> &boxes,
                                    const int frame)
{
    const auto scene = mPanelScene.data();
    if (!scene || boxes.isEmpty()) { return; }
    scene->clearBoxesSelection();
    for (auto *b : boxes) { scene->addBoxToSelection(b); }
    scene->splitBoxesAtFrame(frame);
    if (Document::sInstance) { Document::sInstance->actionFinished(); }
    rebuild();
}

void EditorTimelineSync::splitAtPlayhead()
{
    if (!mWidget || mInWriteback || mDragging) { return; }
    const auto scene = mPanelScene.data();
    if (!scene) { return; }
    // the split operates at the scene's current frame: sync the panel
    // playhead first so the visual position is what gets split
    syncPlayheadToDoc();
    const qreal fps = scene->getFps();
    if (fps <= 0.) { return; }
    const int frame = scene->anim_getCurrentAbsFrame();

    QList<BoundingBox*> boxes;
    const auto ids = mWidget->selectedClipIds();
    for (const int id : ids) {
        const auto layer = mClipToLayer.value(id).data();
        if (!layer) { continue; }
        const auto box = enve_cast<BoundingBox*>(layer);
        if (box) { boxes << box; }
    }
    // CapCut-style fallback: no selection cuts EVERY unlocked-lane clip
    // under the playhead in one press
    if (boxes.isEmpty()) {
        const double ph = mWidget->playheadTime();
        const auto clips = mWidget->allClips();
        for (const auto &c : clips) {
            if (!(c.start <= ph + 1e-9 && ph < c.start + c.length - 1e-9)) { continue; }
            if (mWidget->isTrackLocked(c.track)) { continue; }
            const auto layer = mClipToLayer.value(c.id).data();
            if (!layer) { continue; }
            const auto box = enve_cast<BoundingBox*>(layer);
            if (box) { boxes << box; }
        }
    }
    if (boxes.isEmpty()) { return; }
    splitBoxes(boxes, frame);
}

void EditorTimelineSync::razorCut(const QList<int> &clipIds, const double sec)
{
    if (!mWidget || mInWriteback || mDragging) { return; }
    const auto scene = mPanelScene.data();
    if (!scene) { return; }
    const qreal fps = scene->getFps();
    if (fps <= 0.) { return; }
    const int frame = qRound(sec * fps);

    QList<BoundingBox*> boxes;
    int skippedSounds = 0;
    int skippedEdges = 0;
    for (const int id : clipIds) {
        const auto layer = mClipToLayer.value(id).data();
        if (!layer) { continue; }
        const auto box = enve_cast<BoundingBox*>(layer);
        if (!box) { ++skippedSounds; continue; }
        const auto dur = box->getDurationRectangle();
        if (!dur) { continue; }
        // the cut must leave at least one frame on both sides
        if (!(dur->getMinAbsFrame() < frame && frame < dur->getMaxAbsFrame())) {
            ++skippedEdges;
            continue;
        }
        boxes << box;
    }
    if (skippedSounds > 0) {
        qDebug("[ETL] razor: skipped %d sound clip(s) (unsupported)", skippedSounds);
    }
    if (skippedEdges > 0) {
        qDebug("[ETL] razor: skipped %d clip(s) hit at an edge", skippedEdges);
    }
    if (boxes.isEmpty()) { return; }
    splitBoxes(boxes, frame);
}

void EditorTimelineSync::toggleTrackMute(const int trackIdx,
                                         const bool allSameType)
{
    if (!mWidget || mInWriteback || mDragging) { return; }
    const auto scene = mPanelScene.data();
    if (!scene) { return; }
    // target lanes: one, or every lane of the same type (kdenlive Shift)
    QList<int> lanes{trackIdx};
    if (allSameType && trackIdx >= 0 && trackIdx < mLaneAudio.size()) {
        const bool audio = mLaneAudio.at(trackIdx);
        lanes.clear();
        for (int t = 0; t < mLaneAudio.size(); ++t) {
            if (mLaneAudio.at(t) == audio) { lanes << t; }
        }
    }
    // lane layers: every clip currently parked on the target lanes
    QList<eBoxOrSound*> laneLayers;
    const auto clips = mWidget->allClips();
    for (const auto &c : clips) {
        if (!lanes.contains(c.track)) { continue; }
        const auto layer = mClipToLayer.value(c.id).data();
        if (layer) { laneLayers << layer; }
    }
    if (laneLayers.isEmpty()) { return; }
    // one press = one common state: any visible member -> hide all
    const bool anyVisible = std::any_of(
                laneLayers.cbegin(), laneLayers.cend(),
                [](eBoxOrSound * const l) { return l->isVisible(); });
    for (auto *l : laneLayers) { l->setVisible(!anyVisible); }
    if (Document::sInstance) { Document::sInstance->actionFinished(); }
    rebuild();
}
