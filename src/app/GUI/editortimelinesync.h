#ifndef EDITORTIMELINESYNC_H
#define EDITORTIMELINESYNC_H

#include <QObject>
#include <QPointer>
#include <QList>
#include <QHash>
#include <QImage>
#include <QMetaObject>
#include <QSet>
#include <QString>

class Canvas;
class Document;
class EditorTimelineWidget;
class eBoxOrSound;

// Edit-timeline semantic bridge. The panel mirrors the native timeline:
// EVERY child layer of the ACTIVE scene shows here - sound layers become
// audio blocks, every visual layer (scene links = scenes, vectors,
// images, text, groups, ...) becomes a video block rendered with the
// same real-content thumbnail logic. When the active scene has no
// children at all (e.g. the user dove into an empty child scene), the
// panel keeps showing the last scene that had blocks instead of
// blanking. The playhead tracks the shown scene both ways; block
// drags/trims write back into the layers' duration rectangles on mouse
// release.
//
// Track semantics are aligned with the native "layer-as-track" model
// (eBoxOrSound::trackId): a panel lane with several blocks IS a native
// track (siblings sharing a trackId collapse into one row), a
// single-block lane IS an independent row (trackId -1). Reads derive
// lanes from trackId; releases write the panel order back into both the
// contained order (row order) and the trackIds (merge / split).
class EditorTimelineSync : public QObject
{
    Q_OBJECT
public:
    EditorTimelineSync(Document &document,
                       EditorTimelineWidget * const widget,
                       QObject * const parent = nullptr);

    bool eventFilter(QObject * const obj, QEvent * const ev) override;

    // selection bridge: reflect the panel's clip selection into the
    // canvas box selection so property panels follow the NLE pick
    // (one-way for now; empty selections are ignored so rebuilds never
    // clear a user's canvas selection)
    void pushSelectionToCanvas();

    // ---- document-side NLE operations (all undoable) ----
    // ripple=false: plain remove; ripple=true: later clips on the same
    // lane slide left to close the gap left by the removed ones
    void deleteSelectedClips(const bool ripple);
    // split every selected visual clip at the playhead frame
    void splitAtPlayhead();
    // toggle the lane's layer visibility (mute)
    void toggleTrackMute(const int trackIdx);
    // lane name overrides, survive rebuilds ("v1"/"a2" keyed)
    QHash<QString, QString> mTrackNames;
    // shift one layer's whole clip by frameDelta (min+max transforms)
    void shiftLayerFrames(eBoxOrSound * const layer, const int frameDelta);

public slots:
    void rebuild();
    void updatePlayheadFromDoc();

private:
    void connectPanelScene(Canvas * const scene);
    void connectChildren(Canvas * const scene);
    void syncPlayheadToDoc();
    void applyWriteback();
    // translate the panel lane layout into the native track model:
    // panel row order -> contained order, shared lanes -> shared trackId,
    // solo lanes -> independent rows
    void applyTrackWriteback();
    // media for the panel: decoded filmstrip tiles for video-family
    // clips (provider LRU), WYSIWYG midpoint renders for other visual
    // blocks, real waveform peaks for audio clips
    void requestMedia();
    void deliverThumbnail(const QString &key, const QImage &img);
    class TimelineThumbProvider *mThumbProvider = nullptr;
    // delivery routing: provider key -> (clip id, abs frame/second)
    QHash<QString, QVector<QPair<int, int>>> mFilmRoutes;
    QHash<QString, QVector<QPair<int, int>>> mWaveRoutes;

    Document &mDocument;
    QPointer<EditorTimelineWidget> mWidget;
    // scene whose children the panel shows: the active scene, or the
    // last one that had blocks when the active scene has none
    QPointer<Canvas> mPanelScene;
    QHash<int, QPointer<eBoxOrSound>> mClipToLayer;
    // thumbnail cache keyed by "<boxPtr>:<relFrame>" so renames and
    // rebuilds reuse renders; only a changed mid frame re-renders
    QHash<QString, QImage> mThumbDone;
    QSet<QString> mThumbPending;
    QList<QMetaObject::Connection> mSceneConns;
    QList<QMetaObject::Connection> mChildConns;
    bool mInWriteback = false;
    bool mDragging = false;
    bool mRebuildQueued = false;
};

#endif // EDITORTIMELINESYNC_H
