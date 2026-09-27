/*
#
# Friction - https://friction.graphics
#
# Copyright (c) Ole-André Rodlie and contributors
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.
#
# See 'README.md' for more information.
#
*/

// Fork of enve - Copyright (C) 2016-2020 Maurycy Liebner

#include "CacheHandlers/soundcachehandler.h"
#include "soundcomposition.h"
#include "esound.h"
#include "canvas.h"
#include "Boxes/boundingbox.h"
#include "CacheHandlers/soundcachecontainer.h"
#include "soundmerger.h"
#include "FileCacheHandlers/soundreaderformerger.h"

SoundComposition::SoundComposition(Canvas * const parent) :
    QIODevice(parent), mParent(parent) {
    connect(eSoundSettings::sInstance, &eSoundSettings::settingsChanged,
            this, [this]() {
        mSettings = eSoundSettings::sData();
        mSecondsCache.clear();
    });
}

void SoundComposition::start(const int startFrame) {
    mPos = qRound(startFrame*mSettings.fSampleRate/mParent->getFps());
    open(QIODevice::ReadOnly);
}

void SoundComposition::stop() {
    close();
    clearUseRange();
}

void SoundComposition::addSound(const qsptr<eSound>& sound) {
    auto& conn = mSounds.addObj(sound);
    conn << connect(sound.get(), &Property::prp_absFrameRangeChanged,
                    this, &SoundComposition::frameRangeChanged);
    frameRangeChanged(sound->prp_absInfluenceRange());
}

void SoundComposition::removeSound(const qsptr<eSound>& sound) {
    mSounds.removeObj(sound);
    frameRangeChanged(sound->prp_absInfluenceRange());
}

void SoundComposition::secondFinished(const int secondId,
                                      const stdsptr<Samples> &samples) {
    mProcessingSeconds.removeOne(secondId);
    if(!samples) return;
    const auto sCont = enve::make_shared<SoundCacheContainer>(
                samples, iValueRange{secondId, secondId}, &mSecondsCache);
    mSecondsCache.add(sCont);
}

void SoundComposition::setMinFrameUseRange(const int frame) {
    const qreal fps = mParent->getFps();
    const int sec = qFloor(frame/fps);
    mSecondsCache.setUseRange({sec, sec});
}

void SoundComposition::setMaxFrameUseRange(const int frame) {
    const qreal fps = mParent->getFps();
    const int sec = qFloor(frame/fps);
    mSecondsCache.setMaxUseRange(sec);
}

void SoundComposition::clearUseRange() {
    mSecondsCache.clearUseRange();
}

void SoundComposition::scheduleFrameRange(const FrameRange &range) {
    const qreal fps = mParent->getFps();
    const int minSec = qFloor((range.fMin + 1)/fps);
    const int maxSec = qFloor((range.fMax + 1)/fps);
    for(int i = minSec; i <= maxSec; i++) scheduleSecond(i);
}

SoundMerger *SoundComposition::scheduleFrame(const int frameId) {
    const qreal fps = mParent->getFps();
    return scheduleSecond(qFloor(frameId/fps));
}

void SoundComposition::invalidateRange(const FrameRange &range) {
    const qreal fps = mParent->getFps();
    if(fps <= 0.) return;
    secondRangeChanged({qFloor(range.fMin/fps), qCeil(range.fMax/fps)});
}

SoundMerger *SoundComposition::scheduleSecond(const int secondId) {
    if(mSounds.isEmpty()) return nullptr;
    if(mProcessingSeconds.contains(secondId)) return nullptr;
    if(mSecondsCache.atFrame(secondId)) return nullptr;
    mProcessingSeconds.append(secondId);
    const int sampleRate = mSettings.fSampleRate;
    const SampleRange sampleRange = {secondId*sampleRate,
                                     (secondId + 1)*sampleRate - 1};
    const qreal fps = mParent->getFps();

    const auto task = enve::make_shared<SoundMerger>(secondId, sampleRange, this);
    // AE-style solo: when any sound is soloed, only soloed sounds play
    bool anySoundSolo = false;
    for(const auto &sound : mSounds) {
        if(sound->isSolo()) { anySoundSolo = true; break; }
    }
    for(const auto &sound : mSounds) {
        if(!sound->isVisible()) continue;
        // 内嵌音频跟随宿主盒可见性：NLE 的轨道 solo/静音/停用都是
        // 藏层（VideoBox），内嵌 eVideoSound 是独立对象、自身旗标
        // 恒 true——不查宿主的话画面藏了声音照响，solo 等于没效果
        if(const auto hostBox = sound->getFirstAncestor<BoundingBox>()) {
            if(!hostBox->isVisible()) continue;
        }
        if(anySoundSolo && !sound->isSolo()) continue;
        const auto enabledFrameRange = sound->prp_absInfluenceRange();
        const iValueRange enabledSecRange{qFloor(enabledFrameRange.fMin/fps),
                                          qFloor(enabledFrameRange.fMax/fps)};
        if(!enabledSecRange.inRange(secondId)) continue;
        // 混音器轨道推子（spec 平凡值，1 = 原声）
        const qreal trackVol =
                mParent->trackSpecVolume(sound->trackId());
        const auto secs = sound->absSecondToRelSeconds(secondId);
        for(int i = secs.fMin; i <= secs.fMax; i++) {
            const auto samples = sound->getSamplesForSecond(i);
            if(samples) {
                task->addSoundToMerge({sound->getSampleShift(),
                                       sound->absSampleRange(),
                                       sound->getVolumeSnap(),
                                       sound->getStretch(),
                                       enve::make_shared<Samples>(samples),
                                       trackVol});
            } else {
                const auto reader = sound->getSecondReader(i);
                if(!reader) continue;
                reader->addMerger(task.get());
                reader->addDependent(task.get());
                reader->addSingleSound(sound.get(),
                                       sound->getSampleShift(),
                                       sound->absSampleRange(),
                                       sound->getVolumeSnap(),
                                       sound->getStretch(),
                                       trackVol);
            }
        }
    }
    task->queTask();
    return task.get();
}

void SoundComposition::frameRangeChanged(const FrameRange &range) {
    const qreal fps = mParent->getFps();
    secondRangeChanged({qFloor(range.fMin/fps), qCeil(range.fMax/fps)});
}

// normalized [-1,1] value of one sample of a cached Samples block,
// idx relative to the block start (format-aware, float/int/planar)
static qreal sampleValueNorm(const Samples &s, const uint ch,
                             const int idx) {
    const auto nCh = s.fNChannels;
    if(idx < 0 || idx >= s.fSampleRange.span()) return 0.;
    switch(s.fFormat) {
    case AV_SAMPLE_FMT_FLT:
        return reinterpret_cast<const float*>(s.fData[0])[idx*nCh + ch];
    case AV_SAMPLE_FMT_FLTP:
        return reinterpret_cast<const float*>(s.fData[ch])[idx];
    case AV_SAMPLE_FMT_DBL:
        return static_cast<qreal>(
                    reinterpret_cast<const double*>(s.fData[0])[idx*nCh + ch]);
    case AV_SAMPLE_FMT_DBLP:
        return reinterpret_cast<const double*>(s.fData[ch])[idx];
    case AV_SAMPLE_FMT_S16:
        return reinterpret_cast<const qint16*>(s.fData[0])[idx*nCh + ch]
                / 32768.;
    case AV_SAMPLE_FMT_S16P:
        return reinterpret_cast<const qint16*>(s.fData[ch])[idx] / 32768.;
    case AV_SAMPLE_FMT_S32:
        return reinterpret_cast<const qint32*>(s.fData[0])[idx*nCh + ch]
                / 2147483648.;
    case AV_SAMPLE_FMT_S32P:
        return reinterpret_cast<const qint32*>(s.fData[ch])[idx]
                / 2147483648.;
    default:
        return 0.;
    }
}

// 混音器电平表：当前播放采样位置上，某音频轨所有可闻声源的
// 窗口 RMS（含块音量关键帧与轨道推子）。未播放或无数据 = 0。
// 过滤镜像 scheduleSecond（可见性/宿主盒/solo），与实际出声一致
qreal SoundComposition::trackLevelAt(const int trackId) {
    if(!isOpen() || trackId < 0) return 0.;
    if(mSettings.fSampleRate <= 0) return 0.;
    const qint64 pos = mPos;
    const int absSec = static_cast<int>(pos/mSettings.fSampleRate
                                        + (pos >= 0 ? 0 : -1));
    bool anySolo = false;
    for(const auto &sound : mSounds) {
        if(sound->isSolo()) { anySolo = true; break; }
    }
    const qreal trackVol = mParent->trackSpecVolume(trackId);
    const int halfWin = mSettings.fSampleRate/33; // ~30ms 窗口
    qreal sumSquares = 0.;
    for(const auto &sound : mSounds) {
        if(sound->trackId() != trackId) continue;
        if(!sound->isVisible()) continue;
        if(const auto hostBox = sound->getFirstAncestor<BoundingBox>()) {
            if(!hostBox->isVisible()) continue;
        }
        if(anySolo && !sound->isSolo()) continue;
        const SampleRange absRange = sound->absSampleRange();
        if(pos < absRange.fMin || pos > absRange.fMax) continue;
        const auto secs = sound->absSecondToRelSeconds(absSec);
        const qreal stretch = qMax(sound->getStretch(), 0.0001);
        // 音量快照按拉伸时间采样单位索引（与 merger 的 volIt 同轴）
        const qreal vol = sound->getVolumeSnap().getValue(
                    qreal(pos - sound->getSampleShift())) * trackVol;
        if(vol <= 0.) continue;
        for(int i = secs.fMin; i <= secs.fMax; i++) {
            const auto samples = sound->getSamplesForSecond(i);
            if(!samples) continue;
            // abs → 声源自身（未拉伸）相对采样
            const qreal relPosF =
                    (qreal(pos) - sound->getSampleShift())/stretch;
            const int relPos = qRound(relPosF);
            const SampleRange block = samples->fSampleRange;
            const int winMin = qMax(block.fMin, relPos - halfWin);
            const int winMax = qMin(block.fMax, relPos + halfWin);
            if(winMax < winMin) continue;
            qreal sSum = 0.;
            int n = 0;
            for(int j = winMin; j <= winMax; j++) {
                for(uint c = 0; c < samples->fNChannels; c++) {
                    const qreal v = sampleValueNorm(*samples, c,
                                                    j - block.fMin)*vol;
                    sSum += v*v;
                    n++;
                }
            }
            if(n > 0) { sumSquares += sSum/n; }
        }
    }
    return std::sqrt(sumSquares);
}

qint64 SoundComposition::readData(char *data, qint64 maxLen) {
    const int sampleRate = mSettings.fSampleRate;
    const int bytesPerSample = mSettings.bytesPerSample();
    const int nChannels = mSettings.channelCount();
    const int bytesPerSampleFrame = nChannels * bytesPerSample;

    qint64 total = 0;
    const SampleRange readSamples{static_cast<int>(mPos),
                                  static_cast<int>(mPos + maxLen/bytesPerSampleFrame)};
    while(maxLen > total) {
        const int secondId = static_cast<int>(mPos/sampleRate + (mPos >= 0 ? 0 : -1));
        const auto cont = mSecondsCache.atFrame<SoundCacheContainer>(secondId);
        if(!cont) break;
        const auto samples = cont->getSamples();
        const auto contSampleRange = samples->fSampleRange;
        const auto secondData = samples->fData;
        if(!secondData) break;
        const SampleRange samplesToRead = readSamples*contSampleRange;
        const SampleRange contRelRange = samplesToRead.shifted(-contSampleRange.fMin);
        const qint64 nSamples = contRelRange.span();
        const qint64 chunk = qMin(maxLen - total, nSamples*bytesPerSampleFrame);
        const auto src = secondData[0] + contRelRange.fMin*bytesPerSampleFrame;
        memcpy(data + total, src, static_cast<size_t>(chunk));
        mPos += nSamples;
        total += chunk;
    }

    return total;
}

qint64 SoundComposition::writeData(const char *data, qint64 len) {
    Q_UNUSED(data)
    Q_UNUSED(len)

    return 0;
}
