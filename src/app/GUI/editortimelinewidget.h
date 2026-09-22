#ifndef EDITORTIMELINEWIDGET_H
#define EDITORTIMELINEWIDGET_H

#include <QWidget>
#include <QVector>
#include <QPixmap>
#include <QImage>
#include <QHash>
#include <QElapsedTimer>

class QScrollBar;

// Pure-UI editing timeline: ruler, tracks, draggable clips with
// thumbnails (video) / waveforms (audio), snapping, zoom, playhead.
// No media backend - thumbnails are procedurally generated previews.
class EditorTimelineWidget : public QWidget
{
    Q_OBJECT
public:
    explicit EditorTimelineWidget(QWidget *parent = nullptr);

    void setScrollBar(QScrollBar *bar);
    double playheadTime() const { return m_playhead; }

public:
    // semantic bridge: ids of ALL selected clips (multi-select)
    QList<int> selectedClipIds() const;
    // scene fps for timecode + frame-quantized snapping
    void setFps(const double fps) { m_fps = qMax(1.0, fps); }
    double fps() const { return m_fps; }
    // ruler markers (abs frames + titles), painted as amber guides
    void setMarkers(const QVector<QPair<int, QString>> &markers);
    // lane muted mirror (paint dimming); set from the sync rebuild
    void setTrackMuted(const int trackIdx, const bool muted);
    // lane name override applied after rebuildTracks
    void setTrackName(const int trackIdx, const QString &name);
    int trackCount() const { return m_tracks.size(); }
    // document ops go through the sync bridge (undoable on the doc side)
    void requestDelete(const bool ripple = false);
    // header badge rects (mute / lock), shared by paint + hit test
    QRect muteBadgeRect(const int trackIdx) const;
    QRect lockBadgeRect(const int trackIdx) const;

public slots:
    void addVideoClip();
    void addAudioClip();
    void removeSelectedClip();
    void zoomIn();
    void zoomOut();
    void zoomFit();

signals:
    // document-side operation requests (implemented by EditorTimelineSync)
    void deleteRequested(const bool ripple);
    void splitAtPlayheadRequested();
    void trackMuteToggleRequested(const int trackIdx);
    void trackRenameRequested(const int trackIdx, const QString &name);
    void markerAddRequested(const int frame);
    void markerRemoveRequested(const int frame);
    void logMessage(const QString &msg);
    void selectionChanged(const QString &info);
    void trackLayoutChanged();

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    enum class ClipType { Video, Audio };
    struct Clip {
        int id = 0;
        QString name;
        ClipType type = ClipType::Video;
        int track = 0;       // index into m_tracks
        double start = 0.0;  // seconds
        double length = 4.0; // seconds
        int hueSeed = 0;     // thumbnail variation seed
    };
    struct Track {
        QString name;
        int height = 64;
        ClipType type = ClipType::Video;
        bool locked = false; // UI-side: clips on the lane refuse drag/trim
        bool muted = false;  // mirror of the lane's layer visibility
    };

    // ---- layout / mapping ----
    int rulerHeight() const { return 30; }
    int headerWidth() const { return 132; }
    int trackY(int track) const;           // top y of track content
    int trackAtY(int y) const;             // -1 if none
    double xToTime(int x) const;           // content area x -> seconds
    int timeToX(double t) const;           // seconds -> content area x
    double contentDuration() const;        // right edge of content (seconds)
    QRectF clipRect(const Clip &c) const;

    // ---- painting ----
    void drawRuler(QPainter &p);
    void drawTrackHeaders(QPainter &p);
    void drawTrackBodies(QPainter &p);
    void drawClip(QPainter &p, int index, bool ghost = false);
    void drawPlayhead(QPainter &p);
    QPixmap thumbnailTile(const Clip &c, int h);
    QString timecode(double t) const;

    // ---- interaction ----
    enum class DragMode { None, MoveClip, TrimLeft, TrimRight, Playhead };
    int clipAt(const QPoint &pos, QRectF *rectOut = nullptr) const; // index or -1
    double snapTime(double t, int ignoreClipIdx, bool *snappedOut) const;
    void applyZoom(double factor, int anchorX);
    void clampView();
    void updateScrollBar();
    void emitLog(const QString &msg);
    bool overlapsOnTrack(int track, double start, double len, int ignoreIdx) const;

    QVector<Track> m_tracks;
    QVector<Clip>  m_clips;
    int m_nextId = 1;

    double m_pxPerSec = 60.0;
    double m_scrollSec = 0.0;   // left edge in seconds
    double m_playhead = 2.0;

    QSet<int> m_selectedIds;    // selected clip ids (multi-select)
    QSet<QString> m_keepSelNames; // selection survives rebuilds by name
    int m_hover = -1;

    DragMode m_drag = DragMode::None;
    int m_dragClip = -1;
    double m_grabOffsetSec = 0.0; // move: cursor time - clip start
    double m_origStart = 0.0;
    double m_origLength = 0.0;
    int m_origTrack = 0;
    int m_dragTempLane = -1;   // live-inserted lane while dragging outside
    double m_snapTarget = -1.0;   // for drawing snap guide, -1 = none
    QPoint m_pressPos;

    // NLE upgrades
    double m_fps = 25.0;         // scene fps, drives timecode + frame snap
    QVector<QPair<int, QString>> m_markers; // ruler markers (abs frame, title)
    bool m_rubber = false;       // rubber band selection in progress
    QPoint m_rubberStart;
    // group move snapshot: the other selected clips ride the same delta
    struct GroupSnap { int idx; double origStart; };
    QVector<GroupSnap> m_groupOrig;
    void emitSelectionSummary();

    QScrollBar *m_scrollBar = nullptr;
    QHash<QString, QPixmap> m_thumbCache;
    // clipId -> real rendered frame (raw) and its height-matched pixmap;
    // placeholder tiles stay in m_thumbCache and are only used as fallback
    QHash<int, QImage> m_realThumbs;
    QHash<int, QPixmap> m_realScaled;
    // clipId -> (abs frame -> decoded frame image) filmstrip tiles
    QHash<int, QMap<int, QImage>> m_film;
    // clipId -> (abs second -> peak columns) real waveforms
    QHash<int, QHash<int, QVector<qreal>>> m_waves;

    // theme
    QColor cBg       {0x1b,0x1b,0x1b};
    QColor cBgAlt    {0x22,0x22,0x22};
    QColor cRuler    {0x1d,0x1d,0x1d};
    QColor cHeader   {0x24,0x24,0x26};
    QColor cGridLine {0x2c,0x2c,0x2c};
    QColor cText     {0xc8,0xc8,0xc8};
    QColor cTextDim  {0x77,0x77,0x77};
    QColor cAccent   {0x08,0xa5,0x81}; // pencil-dream green
    QColor cPlayhead {0xe8,0x4c,0x4c};
    QColor cVideoBar {0x0e,0x7d,0x6c}; // clip name bar (teal)
    QColor cAudioBody{0x1d,0x33,0x52}; // audio body (dark blue)
    QColor cAudioWave{0x4f,0x8f,0xd6};

public:
    // ---- friction semantic bridge (appended section; every line above
    // is the byte-level TimelineDemo port and stays untouched) ----
    struct ClipInfo {
        int id = 0;
        QString name;
        double start = 0.;
        double length = 0.;
        int track = 0;
        bool audio = false;
    };
    void clearAllClips();
    int appendClip(const QString &name, const double startSec,
                   const double lengthSec, const bool audio,
                   const int track);
    void rebuildTracks(const int videoCount, const int audioCount);
    void setPlayheadSec(const double t);
    QVector<ClipInfo> allClips() const;
    int videoTrackCount() const;
    void compactLanes();
    // real content thumbnail for a video clip (async offscreen render of
    // the linked scene, delivered by EditorTimelineSync); clips without
    // one keep the procedural placeholder tile
    void setClipThumbnail(const int clipId, const QImage &image);
    // filmstrip: one real decoded frame keyed by its ABS frame; painted
    // at its own time position (survives clip drags, shared frames
    // across split clips dedupe on the provider side)
    void setClipThumbFrame(const int clipId, const int absFrame,
                           const QImage &image);
    // real waveform peaks for one absolute second of an audio clip
    void setClipWave(const int clipId, const int absSecond,
                     const QVector<qreal> &peaks);
    // viewport in seconds (for provider request culling)
    double viewStartSec() const;
    double viewEndSec() const;
    double pxPerSec() const { return m_pxPerSec; }
    // CapCut-style magnetic mode. While on, tracks hold no gaps: turning
    // it on compacts every track right away, and every drag/trim release
    // re-compacts (later clips slide left onto the previous clip's out
    // point; overlaps between merged-track clips are kept). Trimming
    // additionally follows live. Emits trackLayoutChanged when the
    // enabling already moved clips so the bridge persists it.
    void setMagnetic(const bool on);
    bool magnetic() const { return mMagnetic; }
    int compactTrackGaps();

private:
    // ---- track merge (bridge extension): move every clip of srcTrack to
    // dstTrack (same type, overlaps allowed), then drop emptied lanes ----
    void mergeTrackInto(const int srcTrack, const int dstTrack);
    // free drag keeps the dragged clip on a throwaway lane while it hovers
    // outside every same-type lane: releasing there == separating to its
    // own lane (merge/separate without touching the context menu)
    int takeDragTempLane(const ClipType type, const int y);
    int dropDragTempLane();
    void renameLanes();

    // ---- magnetic follow (CapCut-style, bridge extension): trimming a
    // clip's out point shorter slides every same-track clip that started
    // inside the trimmed-away span left by the same delta, so a neighbour
    // attached to the old out point stays attached to the new one. Alt
    // while trimming suppresses it for a plain trim. Recomputed from the
    // press-time snapshot every move, so lengthening back restores all
    // positions (idempotent).
    struct TrimSnap { int idx; double start; };
    QVector<TrimSnap> m_trimSnap;
    void applyMagneticFollow(const bool active);
    bool mMagnetic = false;
    // re-pull the accent colors from the app theme (accent for
    // selection/highlight, its darker shade for clip name bars) so the
    // panel follows theme changes; cheap, called from paint
    void refreshThemeColors();
};

#endif // EDITORTIMELINEWIDGET_H
