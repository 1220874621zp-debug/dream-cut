/*
#
# Friction - https://friction.graphics
#
# Copyright (c) Ole-André Rodlie and contributors.
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
*/

#include "noisedissolveeffect.h"

#include "rastereffectcaller.h"
#include "transitionanchor.h"
#include "Animators/qrealanimator.h"
#include "appsupport.h"

#include <cmath>

NoiseDissolveEffect::NoiseDissolveEffect() :
    RasterEffect(QObject::tr("噪波渐变"),
                 AppSupport::getRasterEffectHardwareSupport("噪波渐变",
                                                            HardwareSupport::cpuOnly),
                 false,
                 RasterEffectType::TRANSITION_NOISE)
{
    mFadeIn = enve::make_shared<QrealAnimator>(12, 0, 600, 1, "淡入时长");
    ca_addChild(mFadeIn);

    mFadeOut = enve::make_shared<QrealAnimator>(12, 0, 600, 1, "淡出时长");
    ca_addChild(mFadeOut);

    mGrain = enve::make_shared<QrealAnimator>(6, 1, 64, 1, "颗粒大小");
    ca_addChild(mGrain);

    mSoftness = enve::make_shared<QrealAnimator>(0.1, 0, 1, 0.01, "柔边");
    ca_addChild(mSoftness);
}

namespace {
// deterministic integer hash -> 0..1, no time term: the pattern is
// stable frame to frame so the dissolve reads as grains popping in,
// not as flickering static
inline float hash21(const int x, const int y)
{
    quint32 h = quint32(x) * 374761393u + quint32(y) * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return float(h & 0x00ffffffu) / float(0x01000000);
}
}

class NoiseDissolveEffectCaller : public RasterEffectCaller {
public:
    NoiseDissolveEffectCaller(const HardwareSupport hwSupport,
                              const qreal openness,
                              const int grain,
                              const qreal softness) :
        RasterEffectCaller(hwSupport), mOpenness(openness),
        mGrain(grain), mSoftness(softness) {}

    void processCpu(CpuRenderTools& renderTools,
                    const CpuRenderData& data);
private:
    const qreal mOpenness;
    const int mGrain;
    const qreal mSoftness;
};

stdsptr<RasterEffectCaller> NoiseDissolveEffect::getEffectCaller(
        const qreal relFrame, const qreal resolution,
        const qreal influence, BoxRenderData * const data) const
{
    Q_UNUSED(resolution)

    const qreal fadeIn = mFadeIn->getEffectiveValue(relFrame);
    const qreal fadeOut = mFadeOut->getEffectiveValue(relFrame);
    const qreal local = nleTransitionClipRelFrame(relFrame, data);
    const qreal total = nleTransitionTotalFrames(data);
    const qreal openness = nleTransitionOpenness(local, total, fadeIn, fadeOut,
                                                 transitionEaseMode()) * influence;

    return enve::make_shared<NoiseDissolveEffectCaller>(
                instanceHwSupport(), openness,
                qMax(1, qRound(mGrain->getEffectiveValue(relFrame))),
                mSoftness->getEffectiveValue(relFrame));
}

void NoiseDissolveEffectCaller::processCpu(
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

    // soft band as a threshold fraction; leading edge reaches past 1
    // so openness 1 shows every grain even at threshold ~1
    const qreal soft = qMax(0.001, mSoftness);
    const qreal edge = mOpenness * (1. + soft);

    for (int yi = yMin; yi <= yMax; yi++) {
        auto dst = static_cast<uchar*>(
                    renderTools.fDstBtmp.getAddr(0, yi - yMin));
        auto src = static_cast<uchar*>(
                    renderTools.fSrcBtmp.getAddr(xMin, yi));
        const int gy = yi / mGrain;
        for (int xi = xMin; xi <= xMax; xi++) {
            const float n = hash21(xi / mGrain, gy);
            const float a = float(qBound(0., (edge - n) / soft, 1.));
            for (int c = 0; c < 4; c++) {
                *dst++ = uchar(*src++ * a);
            }
        }
    }
}
