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

#include "timelinethumbprovider.h"

#include "FileCacheHandlers/animationcachehandler.h"
#include "FileCacheHandlers/imagecachehandler.h"
#include "Sound/esoundobjectbase.h"
#include "Sound/soundpeaks.h"

#include <QImage>

namespace {
// SkImage -> QImage copy (BGRA/RGBA premul), same conversion as the
// NleTimelineController midpoint thumbnails
QImage skImageToQImage(const sk_sp<SkImage> &src)
{
    QImage img;
    if (!src) { return img; }
    SkPixmap pm;
    if (!src->peekPixels(&pm)) { return img; }
    QImage::Format fmt = QImage::Format_Invalid;
    if (pm.colorType() == kBGRA_8888_SkColorType) {
        fmt = QImage::Format_ARGB32_Premultiplied;
    } else if (pm.colorType() == kRGBA_8888_SkColorType) {
        fmt = QImage::Format_RGBA8888_Premultiplied;
    }
    if (fmt == QImage::Format_Invalid) { return img; }
    return QImage(reinterpret_cast<const uchar*>(pm.addr()),
                  pm.width(), pm.height(),
                  int(pm.rowBytes()), fmt).copy();
}
}

TimelineThumbProvider::TimelineThumbProvider(QObject * const parent)
    : QObject(parent)
{}

void TimelineThumbProvider::trimCaches()
{
    if (mThumbCache.size() > 512) {
        // evict half: drop the first half of the hash iteration order
        const int drop = mThumbCache.size() / 2;
        auto it = mThumbCache.begin();
        for (int i = 0; i < drop && it != mThumbCache.end(); ++i) {
            it = mThumbCache.erase(it);
        }
    }
    if (mWaveCache.size() > 1024) {
        const int drop = mWaveCache.size() / 2;
        auto it = mWaveCache.begin();
        for (int i = 0; i < drop && it != mWaveCache.end(); ++i) {
            it = mWaveCache.erase(it);
        }
    }
}

void TimelineThumbProvider::requestFrameThumb(
        const QString &handlerKey, const int animFrame,
        AnimationFrameHandler * const handler, const int height)
{
    if (!handler || height <= 0) { return; }
    const QString key = QStringLiteral("%1:%2:%3").arg(handlerKey).arg(animFrame).arg(height);
    const auto cached = mThumbCache.constFind(key);
    if (cached != mThumbCache.constEnd()) {
        emit frameThumbReady(handlerKey, animFrame, cached.value());
        return;
    }
    if (mInFlight.contains(key) || mFailed.contains(key)) { return; }

    // synchronous cache hit (frame already decoded in memory)
    const auto cont = handler->getFrameAtFrame(animFrame);
    if (cont) {
        const QImage img = skImageToQImage(cont->getImage())
                .scaledToHeight(height, Qt::SmoothTransformation);
        if (!img.isNull()) {
            mThumbCache.insert(key, img);
            emit frameThumbReady(handlerKey, animFrame, img);
            return;
        }
    }

    const auto task = handler->scheduleFrameLoad(animFrame);
    if (!task) { mFailed.insert(key); return; }
    mInFlight.insert(key);
    const QPointer<TimelineThumbProvider> self = this;
    const QPointer<AnimationFrameHandler> handlerQ = handler;
    task->addDependent({[self, handlerQ, handlerKey, animFrame, height, key]() {
        QImage img;
        const auto h = handlerQ.data();
        if (self && h) {
            const auto c = h->getFrameAtFrame(animFrame);
            if (c) {
                img = skImageToQImage(c->getImage())
                        .scaledToHeight(height, Qt::SmoothTransformation);
            }
        }
        QMetaObject::invokeMethod(self, [self, handlerKey, animFrame,
                                         img, key]() {
            if (!self) { return; }
            self->mInFlight.remove(key);
            if (img.isNull()) { self->mFailed.insert(key); return; }
            self->mThumbCache.insert(key, img);
            self->trimCaches();
            emit self->frameThumbReady(handlerKey, animFrame, img);
        }, Qt::QueuedConnection);
    }, [](){}});
}

void TimelineThumbProvider::requestWavePeaks(
        const QString &soundKey, const int relSecond,
        eSoundObjectBase * const sound, const int bucketsPerSecond)
{
    if (!sound || relSecond < 0 || bucketsPerSecond <= 0) { return; }
    const QString key = QStringLiteral("%1:%2:%3").arg(soundKey).arg(relSecond).arg(bucketsPerSecond);
    const auto cached = mWaveCache.constFind(key);
    if (cached != mWaveCache.constEnd()) {
        emit wavePeaksReady(soundKey, relSecond, cached.value());
        return;
    }
    if (mInFlight.contains(key) || mFailed.contains(key)) { return; }

    // synchronous cache hit (second already decoded)
    auto samples = sound->getSamplesForSecond(relSecond);
    if (samples) {
        const auto peaks = SoundPeaks::peaksForSecond(samples, bucketsPerSecond);
        if (!peaks.isEmpty()) {
            mWaveCache.insert(key, peaks);
            emit wavePeaksReady(soundKey, relSecond, peaks);
            return;
        }
        mFailed.insert(key); // non-float / empty: never retry
        return;
    }

    const auto reader = sound->getSecondReader(relSecond);
    if (!reader) { mFailed.insert(key); return; }
    mInFlight.insert(key);
    const QPointer<TimelineThumbProvider> self = this;
    const QPointer<eSoundObjectBase> soundQ = sound;
    reader->addDependent({[self, soundQ, soundKey, relSecond,
                           bucketsPerSecond, key]() {
        QVector<qreal> peaks;
        const auto s = soundQ.data();
        if (self && s) {
            const auto samples = s->getSamplesForSecond(relSecond);
            peaks = SoundPeaks::peaksForSecond(samples, bucketsPerSecond);
        }
        QMetaObject::invokeMethod(self, [self, soundKey, relSecond,
                                         peaks, key]() {
            if (!self) { return; }
            self->mInFlight.remove(key);
            if (peaks.isEmpty()) { self->mFailed.insert(key); return; }
            self->mWaveCache.insert(key, peaks);
            self->trimCaches();
            emit self->wavePeaksReady(soundKey, relSecond, peaks);
        }, Qt::QueuedConnection);
    }, [](){}});
}
