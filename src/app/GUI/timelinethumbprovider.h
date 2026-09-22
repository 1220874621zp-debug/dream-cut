/*
#
# Dream Cut - based on Friction
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
*/

#ifndef TIMELINETHUMBPROVIDER_H
#define TIMELINETHUMBPROVIDER_H

#include <QHash>
#include <QImage>
#include <QObject>
#include <QPointer>
#include <QSet>

#include "smartPointers/ememory.h"

class AnimationFrameHandler;
class eSoundObjectBase;
class eTask;

// Async media thumbnails + waveform peaks for the NLE timeline.
//
// Video/image-sequence filmstrip frames decode DIRECTLY through the
// global frame cache (AnimationFrameHandler::scheduleFrameLoad): no
// render-pipeline involvement, the HDD task thread rate-limits itself,
// and decoded frames stay in the shared cache for playback reuse.
// Non-video blocks keep the WYSIWYG midpoint render in NleTimelineController.
//
// Delivery pattern (FileThumbStore / effects panel style): request
// returns immediately (cache hit emits synchronously), worker results
// are marshalled back through a queued invocation, LRU caches with
// half-eviction cap memory, failed requests are never retried.
class TimelineThumbProvider : public QObject
{
    Q_OBJECT
public:
    explicit TimelineThumbProvider(QObject * const parent = nullptr);

    // one filmstrip tile: `handlerKey` identifies the media source
    // (handler pointer hex), `animFrame` the decoded source frame
    void requestFrameThumb(const QString &handlerKey,
                           const int animFrame,
                           AnimationFrameHandler * const handler,
                           const int height);
    // waveform peaks for one second of a sound (abs-peak columns)
    void requestWavePeaks(const QString &soundKey,
                          const int relSecond,
                          eSoundObjectBase * const sound,
                          const int bucketsPerSecond = 64);

signals:
    void frameThumbReady(const QString &handlerKey, const int animFrame,
                         const QImage &img);
    void wavePeaksReady(const QString &soundKey, const int relSecond,
                        const QVector<qreal> &peaks);

private:
    void trimCaches();

    QHash<QString, QImage> mThumbCache; // key "<handler>:<frame>:<h>"
    QHash<QString, QVector<qreal>> mWaveCache; // key "<sound>:<sec>:<n>"
    QSet<QString> mInFlight;
    QSet<QString> mFailed;
};

#endif // TIMELINETHUMBPROVIDER_H
