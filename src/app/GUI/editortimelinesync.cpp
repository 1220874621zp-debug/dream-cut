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

    // selection bridge: panel clip pick drives the canvas selection
    if (mWidget) {
        connect(mWidget, &EditorTimelineWidget::selectionChanged,
                this, [this](const QString&) { pushSelectionToCanvas(); });
        connect(mWidget, &EditorTimelineWidget::deleteRequested,
                this, [this](const bool ripple) { deleteSelectedClips(ripple); });
        connect(mWidget, &EditorTimelineWidget::splitAtPlayheadRequested,
                this, &EditorTimelineSync::splitAtPlayhead);
        connect(mWidget, &EditorTimelineWidget::trackMuteToggleRequested,
                this, &EditorTimelineSync::toggleTrackMute);
        connect(mWidget, &EditorTimelineWidget::trackRenameRequested,
                this, [this](const int idx, const QString &name) {
            const int vc = mWidget ? mWidget->videoTrackCount() : 0;
            const QString key = idx < vc ? QStringLiteral("v%1").arg(idx + 1)
                                         : QStringLiteral("a%1").arg(idx - vc + 1);
            mTrackNames.insert(key, name);
        });
        connect(mWidget, &EditorTimelineWidget::markerAddRequested,
                this, [this](const int frame) {
            const auto scene = mPanelScene.data();
            if (scene) { scene->setMarker(frame); rebuild(); }
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
    struct Item { eBoxOrSound *layer; bool audio; };
    QList<Item> items;
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
            if (audio) { items.append({layer, true}); }
            else { items.append({layer, false}); }
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
    // lanes mirror the native track model: siblings sharing a trackId
    // collapse into one lane, everything else owns a lane; lane order
    // follows the contained order (contained[0] = native top row = top
    // lane), so panel lanes and native rows stay two views of one order
    QVector<int> lane(items.size(), 0);
    {
        QHash<int, int> vTidToLane, aTidToLane;
        int vNext = 0, aNext = 0;
        for (int i = 0; i < items.size(); ++i) {
            const int tid = items[i].layer->trackId();
            auto &tidToLane = items[i].audio ? aTidToLane : vTidToLane;
            int &next = items[i].audio ? aNext : vNext;
            if (tid >= 0) {
                const auto it = tidToLane.constFind(tid);
                if (it != tidToLane.constEnd()) { lane[i] = it.value(); continue; }
                tidToLane.insert(tid, next);
            }
            lane[i] = next;
            ++next;
        }
    }
    int videoCount = 0;
    int audioCount = 0;
    for (int i = 0; i < items.size(); ++i) {
        if (items[i].audio) { audioCount = qMax(audioCount, lane[i] + 1); }
        else { videoCount = qMax(videoCount, lane[i] + 1); }
    }

    mWidget->rebuildTracks(videoCount, audioCount);
    mWidget->clearAllClips();
    qDebug("[ETL] rebuild panel=%s items=%d video=%d audio=%d active=%s",
           scene ? scene->prp_getName().toUtf8().constData() : "-",
           items.size(), videoCount, audioCount,
           mDocument.fActiveScene ?
               mDocument.fActiveScene->prp_getName().toUtf8().constData() : "-");

    if (!scene) { return; }
    const qreal fps = scene->getFps();
    if (fps > 0.) { mWidget->setFps(fps); }
    {
        // lane name overrides + mute mirrors (all lane layers hidden)
        const int vc = mWidget->videoTrackCount();
        for (int t = 0; t < mWidget->trackCount(); ++t) {
            const QString key = t < vc ? QStringLiteral("v%1").arg(t + 1)
                                       : QStringLiteral("a%1").arg(t - vc + 1);
            const auto nameIt = mTrackNames.constFind(key);
            if (nameIt != mTrackNames.constEnd()) {
                mWidget->setTrackName(t, nameIt.value());
            }
        }
        // mute state: computed below per lane from the layer list
        QVector<bool> laneMuted(videoCount + audioCount, true);
        for (int i = 0; i < items.size(); ++i) {
            const int track = items[i].audio ? videoCount + lane[i] : lane[i];
            if (items[i].layer->isVisible()) { laneMuted[track] = false; }
        }
        for (int t = 0; t < laneMuted.size(); ++t) {
            mWidget->setTrackMuted(t, laneMuted[t]);
        }
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
        const auto dur = it.layer->getDurationRectangle();
        if (dur && fps > 0.) {
            // absolute frames: the native timeline draws and hit-tests
            // the duration bar in abs space (durationrectangle.cpp draw)
            const int minF = dur->getMinAbsFrame();
            const int maxF = dur->getMaxAbsFrame();
            start = minF / fps;
            len = (maxF - minF + 1) / fps;
        }
        const int track = it.audio ? videoCount + lane[i] : lane[i];
        const int id = mWidget->appendClip(it.layer->prp_getName(),
                                           start, len, it.audio, track);
        mClipToLayer.insert(id, it.layer);
    }
    requestMedia();
    updatePlayheadFromDoc();
}

void EditorTimelineSync::applyTrackWriteback()
{
    const auto scene = mPanelScene.data();
    if (!mWidget || !scene) { return; }
    const int videoTracks = mWidget->videoTrackCount();

    struct Entry { eBoxOrSound *layer; int lane; double start; };
    QList<Entry> video, audio;
    for (const auto &clip : mWidget->allClips()) {
        const auto layer = mClipToLayer.value(clip.id).data();
        if (!layer) { continue; }
        if (clip.audio) { audio.append({layer, clip.track - videoTracks, clip.start}); }
        else { video.append({layer, clip.track, clip.start}); }
    }

    mInWriteback = true;
    for (const bool isAudio : {false, true}) {
        auto &entries = isAudio ? audio : video;
        if (entries.isEmpty()) { continue; }
        // panel order: top lane first, then clip start (the panel's
        // top-to-bottom reading of the timeline)
        std::stable_sort(entries.begin(), entries.end(),
                         [](const Entry &a, const Entry &b) {
            if (a.lane != b.lane) { return a.lane < b.lane; }
            return a.start < b.start;
        });
        const auto parent = entries.first().layer->getParentGroup();
        if (!parent || parent->getParentScene() != scene) { continue; }

        // 1. persist the panel order as the native row order (contained
        // order). Skip when they already agree; when they differ, the
        // panel rows occupy the topmost slot the group currently holds
        // and the members stack in panel order below it
        bool needReorder = false;
        bool stale = false;
        int prevIdx = -1;
        int minIdx = INT_MAX;
        for (const auto &e : entries) {
            const int idx = parent->getContainedIndex(e.layer);
            if (idx < 0) { stale = true; break; }
            minIdx = qMin(minIdx, idx);
            if (idx <= prevIdx) { needReorder = true; }
            prevIdx = qMax(prevIdx, idx);
        }
        if (!stale && needReorder) {
            parent->moveContainedInList(entries.first().layer, minIdx);
            for (int i = 1; i < entries.size(); ++i) {
                parent->moveContainedBelow(entries[i].layer,
                                           entries[i - 1].layer);
            }
        }

        // 2. lanes -> tracks: a lane with several blocks joins (or keeps)
        //    one shared trackId; a single-block lane leaves any track
        int laneStart = 0;
        while (laneStart < entries.size()) {
            int laneEnd = laneStart;
            while (laneEnd < entries.size() &&
                   entries[laneEnd].lane == entries[laneStart].lane) { ++laneEnd; }
            if (laneEnd - laneStart == 1) {
                auto * const m = entries[laneStart].layer;
                if (m->isInTrack()) { m->setTrackId(-1); }
            } else {
                // keep an existing id when these members already share
                // one, otherwise open a fresh track
                QHash<int, int> votes;
                int best = -1, bestVotes = 0;
                for (int i = laneStart; i < laneEnd; ++i) {
                    const int tid = entries[i].layer->trackId();
                    if (tid < 0) { continue; }
                    const int v = ++votes[tid];
                    if (v > bestVotes) { bestVotes = v; best = tid; }
                }
                const int tid = best >= 0 ? best : scene->newTrackId(parent);
                for (int i = laneStart; i < laneEnd; ++i) {
                    auto * const m = entries[i].layer;
                    if (m->trackId() != tid) { m->setTrackId(tid); }
                }
            }
            laneStart = laneEnd;
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
                mWidget->compactLanes();      // drop lanes emptied by the drag
                applyTrackWriteback();        // lanes -> native rows/tracks
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

void EditorTimelineSync::splitAtPlayhead()
{
    if (!mWidget || mInWriteback || mDragging) { return; }
    const auto scene = mPanelScene.data();
    if (!scene) { return; }
    // the split operates at the scene's current frame: sync the panel
    // playhead first so the visual position is what gets split
    syncPlayheadToDoc();
    QList<BoundingBox*> boxes;
    const auto ids = mWidget->selectedClipIds();
    for (const int id : ids) {
        const auto layer = mClipToLayer.value(id).data();
        if (!layer) { continue; }
        const auto box = enve_cast<BoundingBox*>(layer);
        if (box) { boxes << box; }
    }
    if (boxes.isEmpty()) { return; }
    scene->clearBoxesSelection();
    for (auto *b : boxes) { scene->addBoxToSelection(b); }
    scene->splitAction();
    if (Document::sInstance) { Document::sInstance->actionFinished(); }
    rebuild();
}

void EditorTimelineSync::toggleTrackMute(const int trackIdx)
{
    if (!mWidget || mInWriteback || mDragging) { return; }
    const auto scene = mPanelScene.data();
    if (!scene) { return; }
    // lane layers: every clip currently parked on that panel track
    QList<eBoxOrSound*> laneLayers;
    const auto clips = mWidget->allClips();
    for (const auto &c : clips) {
        if (c.track != trackIdx) { continue; }
        const auto layer = mClipToLayer.value(c.id).data();
        if (layer) { laneLayers << layer; }
    }
    if (laneLayers.isEmpty()) { return; }
    const bool anyVisible = std::any_of(
                laneLayers.cbegin(), laneLayers.cend(),
                [](eBoxOrSound * const l) { return l->isVisible(); });
    for (auto *l : laneLayers) { l->setVisible(!anyVisible); }
    if (Document::sInstance) { Document::sInstance->actionFinished(); }
    rebuild();
}
