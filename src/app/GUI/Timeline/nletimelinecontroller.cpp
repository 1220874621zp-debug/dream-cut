#include "nletimelinecontroller.h"
#include "nletimelinemodel.h"
#include "nletimelineview.h"

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

#include <QTimer>

NleTimelineController::NleTimelineController(
        Document &document,
        NleTimelineModel * const model,
        NleTimelineView * const view,
        QObject * const parent)
    : QObject(parent)
    , mDocument(document)
    , mModel(model)
    , mView(view)
{
    mThumbProvider = new TimelineThumbProvider(this);
    connect(mThumbProvider, &TimelineThumbProvider::frameThumbReady,
            this, [this](const QString &key, const int animFrame,
                         const QImage &img) {
        const auto &routes = mFilmRoutes.value(
                    QStringLiteral("%1:%2").arg(key).arg(animFrame));
        for (const auto &r : routes) {
            mView->setClipThumbFrame(r.first, r.second, img);
        }
    });
    connect(mThumbProvider, &TimelineThumbProvider::wavePeaksReady,
            this, [this](const QString &key, const int relSecond,
                         const QVector<qreal> &peaks) {
        const auto &routes = mWaveRoutes.value(
                    QStringLiteral("%1:%2").arg(key).arg(relSecond));
        for (const auto &r : routes) {
            mView->setClipWave(r.first, r.second, peaks);
        }
    });

    // media follows model changes (immediately) and viewport changes
    // (debounced so a zoom gesture is one pass)
    mMediaTimer = new QTimer(this);
    mMediaTimer->setSingleShot(true);
    mMediaTimer->setInterval(250);
    connect(mMediaTimer, &QTimer::timeout,
            this, &NleTimelineController::requestMedia);
    connect(mModel, &NleTimelineModel::modelChanged,
            this, &NleTimelineController::requestMedia);
    connect(mModel, &NleTimelineModel::guidesChanged,
            this, &NleTimelineController::pushGuides);
    connect(mModel, &NleTimelineModel::playheadFrameChanged,
            mView, &NleTimelineView::setPlayheadFrame);
    connect(mModel, &NleTimelineModel::selectionChanged,
            this, &NleTimelineController::pushSelectionToCanvas);

    connect(mView, &NleTimelineView::viewChanged,
            this, [this]() {
        if (!mModel->gestureActive() && mMediaTimer) {
            mMediaTimer->start();
        }
    });
    connect(mView, &NleTimelineView::playheadDragged,
            this, &NleTimelineController::syncPlayheadToDoc);
}

void NleTimelineController::syncPlayheadToDoc(const int frame)
{
    const auto scene = mModel ? mModel->panelScene() : nullptr;
    if (!scene) { return; }
    if (scene == mDocument.fActiveScene.data()) {
        if (frame != mDocument.getActiveSceneFrame()) {
            mDocument.setActiveSceneFrame(frame);
        }
    } else if (frame != scene->anim_getCurrentAbsFrame()) {
        scene->anim_setAbsFrame(frame);
    }
}

void NleTimelineController::pushGuides()
{
    const auto scene = mModel ? mModel->panelScene() : nullptr;
    if (!mView || !scene) { return; }
    // ruler markers from the scene (abs frames + titles) and the
    // render in/out band (frames)
    QVector<QPair<int, QString>> marks;
    for (const auto &m : scene->getMarkers()) {
        if (m.enabled) { marks.append({m.frame, m.title}); }
    }
    mView->setMarkers(marks);
    if (scene->inOutEnabled()) {
        mView->setRangeBand(scene->inFrame(), scene->outFrame());
    } else {
        mView->setRangeBand(-1, -1);
    }
}

void NleTimelineController::pushSelectionToCanvas()
{
    if (!mModel || !mView) { return; }
    if (mModel->inWriteback() || mModel->gestureActive()) { return; }
    const auto ids = mModel->selection();
    if (ids.isEmpty()) { return; }
    const auto scene = mModel->panelScene();
    if (!scene) { return; }
    QList<BoundingBox*> boxes;
    for (const int id : ids) {
        const auto c = mModel->clip(id);
        if (!c || !c->layer) { continue; }
        const auto box = enve_cast<BoundingBox*>(c->layer.data());
        if (box) { boxes << box; }
    }
    if (boxes.isEmpty()) { return; }
    const auto &cur = scene->getSelectedBoxesList();
    if (cur.count() == boxes.count()) {
        bool same = true;
        for (const auto *b : boxes) {
            if (!cur.contains(const_cast<BoundingBox*>(b))) { same = false; break; }
        }
        if (same) { return; }
    }
    scene->clearBoxesSelection();
    for (auto *b : boxes) { scene->addBoxToSelection(b); }
}

void NleTimelineController::requestMedia()
{
    const auto scene = mModel ? mModel->panelScene() : nullptr;
    if (!mView || !scene || !mThumbProvider) { return; }
    const qreal fps = scene->getFps();
    if (fps <= 0.) { return; }

    const int viewA = mView->viewStartFrame();
    const int viewB = mView->viewEndFrame();
    // filmstrip tile width in px at the current zoom (16:9 of a ~40px
    // strip body) drives the sampling interval: one decoded frame per
    // tile width, so the strip density follows the zoom level
    const int bodyH = 40;
    const double tileWpx = qMax(48., bodyH * 16. / 9.);
    const int intervalFrames = qMax(1, qRound(tileWpx / mView->pxPerFrame()));

    mFilmRoutes.clear();
    mWaveRoutes.clear();

    for (const auto &c : mModel->clips()) {
        const auto layer = c.layer.data();
        if (!layer) { continue; }
        const auto dur = layer->getDurationRectangle();
        if (!dur) { continue; }

        const auto soundObj = enve_cast<eSoundObjectBase*>(layer);
        if (soundObj) {
            // real waveform: peak columns for every visible second
            const int sec0 = qMax(0, int(viewA / fps));
            const int sec1 = int(viewB / fps) + 1;
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
                        ].append({c.clipId, absSec});
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
            const int f0 = qMax(dur->getMinAbsFrame(), viewA);
            const int f1 = qMin(dur->getMaxAbsFrame(), viewB);
            for (int f = f0; f <= f1; f += intervalFrames) {
                const qreal relFrame = box->prp_absFrameToRelFrameF(f);
                const int animFrame = animBox->getAnimationFrameForRelFrame(relFrame);
                mFilmRoutes[
                        QStringLiteral("%1:%2").arg(hKey).arg(animFrame)
                        ].append({c.clipId, f});
                mThumbProvider->requestFrameThumb(hKey, animFrame,
                                                  handler, bodyH);
            }
            continue; // no midpoint render for filmstrip clips
        }

        // other visual layers (images, vectors, text, groups): one
        // real WYSIWYG frame per block - the middle of its range
        // (stable across refreshes; only a re-trim that moves the
        // midpoint re-renders)
        if (mThumbDone.size() > 128) { mThumbDone.clear(); }
        const int absMid = (dur->getMinAbsFrame() + dur->getMaxAbsFrame()) / 2;
        const qreal relFrame = box->prp_absFrameToRelFrameF(absMid);
        const QString key = QStringLiteral("%1:%2")
                .arg(reinterpret_cast<qulonglong>(box), 0, 16)
                .arg(qRound(relFrame));
        const auto cached = mThumbDone.constFind(key);
        if (cached != mThumbDone.constEnd()) {
            mView->setClipThumbnail(c.clipId, cached.value());
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
        const QPointer<NleTimelineController> self = this;
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

void NleTimelineController::deliverThumbnail(const QString &key,
                                             const QImage &img)
{
    const auto scene = mModel ? mModel->panelScene() : nullptr;
    if (!mView || !scene) { return; }
    for (const auto &c : mModel->clips()) {
        const auto box = enve_cast<BoundingBox*>(c.layer.data());
        if (!box) { continue; }
        const auto dur = box->getDurationRectangle();
        if (!dur) { continue; }
        const int absMid = (dur->getMinAbsFrame() + dur->getMaxAbsFrame()) / 2;
        const qreal relFrame = box->prp_absFrameToRelFrameF(absMid);
        const QString curKey = QStringLiteral("%1:%2")
                .arg(reinterpret_cast<qulonglong>(box), 0, 16)
                .arg(qRound(relFrame));
        if (curKey == key) {
            mView->setClipThumbnail(c.clipId, img);
        }
    }
}
