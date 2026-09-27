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
    // JKL 梭动当前速率（正=正放 负=倒放；停止恢复 1）
    qreal rate() const { return mRate; }

public slots:
    // start from the scene's current frame (in/out points honored);
    // returns false when playback cannot start (no scene / empty range)
    bool play();
    void stop();
    // JKL 梭动：设速率换基继续（|rate|<0.01 = 停）；非播放态先起播。
    // 非整 1x 或倒放时音频静音（SoundComposition 只有 1x 正向流），
    // 回到 1x 正放从当前帧重启音频
    void shuttle(const qreal rate);

    void setLoop(const bool loop) { mLoop = loop; }
    // preview resolution clamp used while playing (scene value restored)
    void setPlayResolution(const qreal res) { mPlayRes = qBound(0.1, res, 1.); }

signals:
    void started();
    void finished();
    // per-frame notification for timeline playheads (abs frame)
    void frameChanged(const int frame);
    void rateChanged(const qreal rate);

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
    qreal mRate = 1.;   // JKL 梭动速率（play()/stop() 归 1）
    bool mLoop = false;
    bool mPlaying = false;
};

#endif // DIRECTPLAYER_H
