#ifndef NLETIMELINECONTROLLER_H
#define NLETIMELINECONTROLLER_H

#include <QObject>
#include <QPointer>
#include <QHash>
#include <QImage>
#include <QSet>
#include <QString>
#include <QVector>
#include <QPair>

class Document;
class Canvas;
class eBoxOrSound;
class BoundingBox;
class NleTimelineModel;
class NleTimelineView;
class TimelineThumbProvider;
class QTimer;

// kdenlive-style controller between the timeline model/view pair and
// the rest of the application: routes media (decoded filmstrip tiles,
// real waveform peaks, midpoint WYSIWYG renders) into the view, keeps
// the playhead two-way in sync with the shown scene, mirrors the clip
// selection into the canvas box selection (property panels follow the
// NLE pick) and pushes guide display state (markers, in/out band)
// into the view
class NleTimelineController : public QObject
{
    Q_OBJECT
public:
    NleTimelineController(Document &document,
                          NleTimelineModel * const model,
                          NleTimelineView * const view,
                          QObject * const parent = nullptr);

private:
    // reflect the model's clip selection into the canvas box
    // selection (one-way; empty selections are ignored so refreshes
    // never clear a user's canvas selection)
    void pushSelectionToCanvas();
    // playhead: view drag -> document frame
    void syncPlayheadToDoc(const int frame);
    // markers + scene in/out band -> view display state
    void pushGuides();
    // media for the view: decoded filmstrip tiles for video-family
    // clips (provider LRU), WYSIWYG midpoint renders for other visual
    // blocks, real waveform peaks for audio clips; culled to the view
    // viewport with a sampling density that follows the zoom
    void requestMedia();
    void deliverThumbnail(const QString &key, const QImage &img);

    Document &mDocument;
    QPointer<NleTimelineModel> mModel;
    QPointer<NleTimelineView> mView;
    TimelineThumbProvider *mThumbProvider = nullptr;
    // delivery routing: provider key -> (clip id, abs frame/second)
    QHash<QString, QVector<QPair<int, int>>> mFilmRoutes;
    QHash<QString, QVector<QPair<int, int>>> mWaveRoutes;
    // midpoint thumbnail caches keyed by "<boxPtr>:<relFrame>" so
    // renames and refreshes reuse renders; only a changed mid frame
    // re-renders
    QHash<QString, QImage> mThumbDone;
    QSet<QString> mThumbPending;
    QTimer *mMediaTimer = nullptr;
};

#endif // NLETIMELINECONTROLLER_H
