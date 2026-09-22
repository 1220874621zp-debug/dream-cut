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

#include "directplayer.h"

#include <QTimer>

#include "Private/document.h"
#include "canvas.h"
#include "Sound/audiohandler.h"
#include "Sound/soundcomposition.h"

DirectPlayer::DirectPlayer(Document &document, AudioHandler &audio,
                           QObject * const parent)
    : QObject(parent)
    , mDocument(document)
    , mAudio(audio)
{
    mTimer.setSingleShot(false);
    mAudioTimer.setSingleShot(false);
    mAudioTimer.setInterval(30);
    connect(&mTimer, &QTimer::timeout, this, &DirectPlayer::tick);
    connect(&mAudioTimer, &QTimer::timeout, this, &DirectPlayer::pushAudio);
}

bool DirectPlayer::play()
{
    if (mPlaying) { return true; }
    const auto scene = mDocument.fActiveScene.data();
    if (!scene) { return false; }
    const qreal fps = scene->getFps();
    if (fps <= 0.) { return false; }

    const auto fIn = scene->getFrameIn();
    const auto fOut = scene->getFrameOut();
    const int minFrame = fIn.enabled ? fIn.frame : scene->getMinFrame();
    // content-aware end: the scene range may lag behind the clips
    // (default range + declined adjust-scene) - a playhead on the far
    // content would otherwise look "out of range" and restart from 0
    int contentEnd = scene->getMaxFrame();
    for (const auto &c : scene->getContained()) {
        if (!c) { continue; }
        const auto dur = c->getDurationRectangle();
        if (dur) { contentEnd = qMax(contentEnd, dur->getMaxAbsFrame() + 1); }
    }
    const int maxFrame = fOut.enabled ? fOut.frame : contentEnd;
    int startFrame = scene->anim_getCurrentAbsFrame();
    if (startFrame < minFrame || startFrame > maxFrame) {
        // out of the play range: restart from the in point (NLE habit)
        startFrame = minFrame;
    }
    if (minFrame >= maxFrame) {
        qWarning() << "[DIRECT-PLAY] refused: empty range"
                   << minFrame << maxFrame;
        return false;
    }

    // preview resolution clamp: a quarter of the pixels is a quarter of
    // the composition cost; restored verbatim on stop
    mSavedResolution = scene->getResolution();
    if (mSavedResolution > mPlayRes) {
        scene->setResolution(mPlayRes);
        if (Document::sInstance) { Document::sInstance->actionFinished(); }
    }

    // audio master clock: composition opened at the start frame and
    // pulled by the QAudioOutput (push pattern copied from RenderHandler)
    const auto comp = scene->getSoundComposition();
    if (comp) {
        mAudio.startAudio();
        comp->start(startFrame);
        scheduleSeconds(comp, startFrame, int(2 * fps));
    }

    mStartFrame = startFrame;
    mLastSetFrame = startFrame;
    mClock.start();
    mPlaying = true;
    mTimer.setInterval(qMax(5, int(1000. / fps)));
    mTimer.start();
    mAudioTimer.start();
    emit started();
    return true;
}

void DirectPlayer::stop()
{
    if (!mPlaying) { return; }
    mPlaying = false;
    mTimer.stop();
    mAudioTimer.stop();
    const auto scene = mDocument.fActiveScene.data();
    if (scene) {
        const auto comp = scene->getSoundComposition();
        if (comp) { comp->stop(); }
        mAudio.stopAudio();
        if (qAbs(mSavedResolution - scene->getResolution()) > 0.000001) {
            scene->setResolution(mSavedResolution);
            if (Document::sInstance) { Document::sInstance->actionFinished(); }
        }
    }
    emit finished();
}

// public API: SoundComposition::scheduleFrameRange schedules (and
// dedupes) every sound second covering the frame range
void DirectPlayer::scheduleSeconds(SoundComposition * const comp,
                                   const int fromFrame, const int nFrames)
{
    if (!comp || nFrames <= 0) { return; }
    comp->scheduleFrameRange({fromFrame, fromFrame + nFrames});
}

void DirectPlayer::tick()
{
    if (!mPlaying) { return; }
    const auto scene = mDocument.fActiveScene.data();
    if (!scene) { stop(); return; }
    const qreal fps = scene->getFps();
    if (fps <= 0.) { stop(); return; }

    const qreal elapsed = mClock.elapsed() / 1000.;
    const qreal targetF = mStartFrame + elapsed * fps;
    const auto fIn = scene->getFrameIn();
    const auto fOut = scene->getFrameOut();
    const int minFrame = fIn.enabled ? fIn.frame : scene->getMinFrame();
    const int maxFrame = fOut.enabled ? fOut.frame : scene->getMaxFrame();

    if (targetF > maxFrame) {
        if (mLoop) {
            // wrap to the in point: both clocks restart together so the
            // audio never drifts from the video target
            mStartFrame = minFrame;
            mClock.restart();
            const auto comp = scene->getSoundComposition();
            if (comp) {
                comp->stop();
                comp->start(minFrame);
                scheduleSeconds(comp, minFrame, int(2 * fps));
            }
            return;
        }
        stop();
        return;
    }

    const int target = qBound(minFrame, int(targetF), maxFrame);
    // frame drop by design: the target advances with the wall clock no
    // matter what; the canvas keeps the last finished frame on screen
    // until the one being scheduled lands, so slow compositions skip
    // frames instead of slowing down
    if (target != mLastSetFrame) {
        mLastSetFrame = target;
        scene->anim_setAbsFrame(target);
        if (Document::sInstance) { Document::sInstance->actionFinished(); }
        emit frameChanged(target);
        const auto comp = scene->getSoundComposition();
        if (comp) { scheduleSeconds(comp, target, int(2 * fps)); }
    }
}

void DirectPlayer::pushAudio()
{
    if (!mPlaying) { return; }
    const auto scene = mDocument.fActiveScene.data();
    if (!scene) { return; }
    const auto comp = scene->getSoundComposition();
    if (!comp) { return; }
    while (auto request = mAudio.dataRequest()) {
        const qint64 len = comp->read(request.fData, request.fSize);
        if (len <= 0) { break; }
        request.fSize = int(len);
        mAudio.provideData(request);
    }
}
