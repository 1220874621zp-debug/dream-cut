/*
#
# Friction - https://friction.graphics
#
# Copyright (c) Ole-André Rodlie and contributors.
#
# This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.
#
*/

#include "filmgraintransitioneffect.h"

#include "rastereffectcaller.h"
#include "transitionanchor.h"
#include "Animators/qrealanimator.h"
#include "appsupport.h"

FilmGrainTransitionEffect::FilmGrainTransitionEffect() :
    RasterEffect(QObject::tr("胶片颗粒"),
                 AppSupport::getRasterEffectHardwareSupport("胶片颗粒",
                                                            HardwareSupport::cpuOnly),
                 false,
                 RasterEffectType::TRANSITION_FILM_GRAIN)
{
    mFadeIn = enve::make_shared<QrealAnimator>(12, 0, 600, 1, "淡入时长");
    ca_addChild(mFadeIn);

    mFadeOut = enve::make_shared<QrealAnimator>(12, 0, 600, 1, "淡出时长");
    ca_addChild(mFadeOut);

    mStrength = enve::make_shared<QrealAnimator>(80, 0, 255, 1, "颗粒强度");
    ca_addChild(mStrength);
}

namespace {
inline float grainHash(const quint32 a, const quint32 b)
{
    quint32 h = a * 374761393u + b * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return float(h & 0x00ffffffu) / float(0x01000000);
}
}

class FilmGrainTransitionEffectCaller : public RasterEffectCaller {
public:
    FilmGrainTransitionEffectCaller(const HardwareSupport hwSupport,
                                    const qreal decay,
                                    const qreal strength,
                                    const quint32 seed) :
        RasterEffectCaller(hwSupport), mDecay(decay),
        mStrength(strength), mSeed(seed) {}

    void processCpu(CpuRenderTools& renderTools,
                    const CpuRenderData& data);
private:
    const qreal mDecay;
    const qreal mStrength;
    const quint32 mSeed;
};

stdsptr<RasterEffectCaller> FilmGrainTransitionEffect::getEffectCaller(
        const qreal relFrame, const qreal resolution,
        const qreal influence, BoxRenderData * const data) const
{
    Q_UNUSED(resolution)

    const qreal fadeIn = mFadeIn->getEffectiveValue(relFrame);
    const qreal fadeOut = mFadeOut->getEffectiveValue(relFrame);
    const qreal local = nleTransitionClipRelFrame(relFrame, data);
    const qreal total = nleTransitionTotalFrames(data);
    const qreal openness = nleTransitionOpenness(local, total,
                                                 fadeIn, fadeOut) * influence;

    // per-frame seed: film grain boils frame to frame
    const quint32 seed = quint32(qRound(local)) * 2246822519u + 7u;

    return enve::make_shared<FilmGrainTransitionEffectCaller>(
                instanceHwSupport(), 1. - openness,
                mStrength->getEffectiveValue(relFrame), seed);
}

void FilmGrainTransitionEffectCaller::processCpu(
        CpuRenderTools& renderTools, const CpuRenderData& data)
{
    // fully open must still copy src into dst: the pipeline hands the
    // caller an uninitialized dst bitmap and replaces the rendered
    // image with it, so an early return here draws garbage/black
    const auto& srcBtmp = renderTools.fSrcBtmp;
    const auto& dstBtmp = renderTools.fDstBtmp;

    if (srcBtmp.empty() || srcBtmp.getPixels() == nullptr ||
        dstBtmp.empty() || dstBtmp.getPixels() == nullptr) { return; }

    const int xMin = std::max(0, data.fTexTile.left());
    const int xMax = std::min((int)data.fTexTile.right(),
                              (int)srcBtmp.width() - 1);
    const int yMin = std::max(0, data.fTexTile.top());
    const int yMax = std::min((int)data.fTexTile.bottom(),
                              (int)srcBtmp.height() - 1);

    const float a = float(1. - mDecay);
    const float k = float(mDecay * mStrength);

    if (k < 0.5f) {
        for (int yi = yMin; yi <= yMax; yi++) {
            auto dst = static_cast<uchar*>(
                        renderTools.fDstBtmp.getAddr(0, yi - yMin));
            auto src = static_cast<uchar*>(
                        renderTools.fSrcBtmp.getAddr(xMin, yi));
            for (int xi = xMin; xi <= xMax; xi++) {
                for (int c = 0; c < 4; c++) { *dst++ = uchar(*src++ * a); }
            }
        }
        return;
    }

    for (int yi = yMin; yi <= yMax; yi++) {
        auto dst = static_cast<uchar*>(
                    renderTools.fDstBtmp.getAddr(0, yi - yMin));
        auto src = static_cast<uchar*>(
                    renderTools.fSrcBtmp.getAddr(xMin, yi));
        for (int xi = xMin; xi <= xMax; xi++) {
            // 2x2 grain cells: chunkier than per-pixel noise
            const float n = grainHash(quint32(xi >> 1) ^ mSeed,
                                      quint32(yi >> 1));
            const float add = (n - 0.5f) * 2.f * k;
            for (int c = 0; c < 3; c++) {
                const float v = *src * a + add;
                *dst++ = v < 0.f ? uchar(0) : (v > 255.f ? uchar(255)
                                                         : uchar(v));
                src++;
            }
            *dst++ = uchar(*src++ * a);
        }
    }
}
