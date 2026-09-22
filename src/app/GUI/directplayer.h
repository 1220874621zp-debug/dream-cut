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

#ifndef DIRECTPLAYER_H
#define DIRECTPLAYER_H

#include <QElapsedTimer>
#include <QObject>
#include <QTimer>

#include "framerange.h"

class AudioHandler;
class Canvas;
class Document;
class SoundComposition;

// Kdenlive-style direct playback: composites every displayed frame on
// demand instead of pre-rendering a preview cache.
//
//  - the AUDIO CLOCK is the master: a wall-clock (started together with
//    the sound composition) drives the target frame; the canvas keeps
//    showing the last finished frame until the next one is ready, so a
//    slow composition degrades to frame DROPS, never to slowdown
//  - playback renders at a reduced preview resolution (default 1/2, the
//    user's scene resolution is restored on stop)
//  - sound seconds are scheduled ~2s ahead of the playhead so the
//    QIODevice pull never starves
class DirectPlayer : public QObject
{
    Q_OBJECT
public:
    DirectPlayer(Document &document, AudioHandler &audio,
                 QObject * const parent = nullptr);

    bool playing() const { return mPlaying; }

public slots:
    // start from the scene's current frame (in/out points honored);
    // returns false when playback cannot start (no scene / empty range)
    bool play();
    void stop();

    void setLoop(const bool loop) { mLoop = loop; }
    // preview resolution clamp used while playing (scene value restored)
    void setPlayResolution(const qreal res) { mPlayRes = qBound(0.1, res, 1.); }

signals:
    void started();
    void finished();
    // per-frame notification for timeline playheads (abs frame)
    void frameChanged(const int frame);

private:
    void tick();
    void pushAudio();
    void scheduleSeconds(SoundComposition * const comp,
                          const int fromFrame, const int nFrames);

    Document &mDocument;
    AudioHandler &mAudio;
    QTimer mTimer;      // frame driver (audio-clock driven target)
    QTimer mAudioTimer; // QAudioOutput push, same pattern as RenderHandler
    QElapsedTimer mClock;

    qreal mStartFrame = 0.;
    int mLastSetFrame = -1;
    qreal mSavedResolution = 1.;
    qreal mPlayRes = 0.5;
    bool mLoop = false;
    bool mPlaying = false;
};

#endif // DIRECTPLAYER_H
