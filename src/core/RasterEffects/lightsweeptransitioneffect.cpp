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

#include "lightsweeptransitioneffect.h"

#include "rastereffectcaller.h"
#include "transitionanchor.h"
#include "Animators/qrealanimator.h"
#include "appsupport.h"

#include <cmath>

LightSweepTransitionEffect::LightSweepTransitionEffect() :
    RasterEffect(QObject::tr("扫光"),
                 AppSupport::getRasterEffectHardwareSupport("扫光",
                                                            HardwareSupport::cpuOnly),
                 false,
                 RasterEffectType::TRANSITION_LIGHT_SWEEP)
{
    mFadeIn = enve::make_shared<QrealAnimator>(12, 0, 600, 1, "淡入时长");
    ca_addChild(mFadeIn);

    mFadeOut = enve::make_shared<QrealAnimator>(12, 0, 600, 1, "淡出时长");
    ca_addChild(mFadeOut);

    mBandWidth = enve::make_shared<QrealAnimator>(0.25, 0.05, 0.8, 0.01, "光带宽度");
    ca_addChild(mBandWidth);

    mIntensity = enve::make_shared<QrealAnimator>(130, 0, 255, 5, "光强");
    ca_addChild(mIntensity);
}

class LightSweepTransitionEffectCaller : public RasterEffectCaller {
public:
    LightSweepTransitionEffectCaller(const HardwareSupport hwSupport,
                                     const qreal bandCenter,
                                     const qreal bandWidth,
                                     const qreal intensity,
                                     const qreal alpha) :
        RasterEffectCaller(hwSupport), mCenter(bandCenter),
        mWidth(bandWidth), mIntensity(intensity), mAlpha(alpha) {}

    void processCpu(CpuRenderTools& renderTools,
                    const CpuRenderData& data);
private:
    const qreal mCenter;
    const qreal mWidth;
    const qreal mIntensity;
    const qreal mAlpha;
};

stdsptr<RasterEffectCaller> LightSweepTransitionEffect::getEffectCaller(
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

    const qreal band = mBandWidth->getEffectiveValue(relFrame);
    // the band starts fully off-frame left, crosses the diagonal and
    // ends fully off-frame right: at full openness the nearest pixel
    // sits 3+ half-widths from the centre, and the gaussian is
    // clamped hard to zero past 3 (identity, no residue brightening)
    const qreal center = openness * (1. + 4. * band) - band;

    return enve::make_shared<LightSweepTransitionEffectCaller>(
                instanceHwSupport(), center, band,
                mIntensity->getEffectiveValue(relFrame), openness);
}

void LightSweepTransitionEffectCaller::processCpu(
        CpuRenderTools& renderTools, const CpuRenderData& data)
{
    // fully open must still copy src into dst: the pipeline hands the
    // caller an uninitialized dst bitmap and replaces the rendered
    // image with it, so an early return here draws garbage/black
    const auto& srcBtmp = renderTools.fSrcBtmp;
    const auto& dstBtmp = renderTools.fDstBtmp;

    if (srcBtmp.empty() || srcBtmp.getPixels() == nullptr ||
        dstBtmp.empty() || dstBtmp.getPixels() == nullptr) { return; }

    const int w = srcBtmp.width();
    const int h = srcBtmp.height();

    const int xMin = std::max(0, data.fTexTile.left());
    const int xMax = std::min((int)data.fTexTile.right(), w - 1);
    const int yMin = std::max(0, data.fTexTile.top());
    const int yMax = std::min((int)data.fTexTile.bottom(), h - 1);

    const float a = float(mAlpha);
    const qreal diag = w + h;
    const qreal invBand = 1. / qMax(0.001, mWidth);

    for (int yi = yMin; yi <= yMax; yi++) {
        auto dst = static_cast<uchar*>(
                    renderTools.fDstBtmp.getAddr(0, yi - yMin));
        auto src = static_cast<uchar*>(
                    renderTools.fSrcBtmp.getAddr(xMin, yi));
        const qreal rowPos0 = (yi + 0.5) / diag;
        for (int xi = xMin; xi <= xMax; xi++) {
            const qreal pos = ((xi + 0.5) + (yi + 0.5)) / diag;
            const qreal d = (pos - mCenter) * invBand;
            // gaussian sheen clamped hard at zero: full openness puts
            // the band centre 3 half-widths past the far corner, so
            // the add term is exactly zero everywhere (identity)
            const float g = d > 3. || d < -3. ?
                        0.f : float(std::exp(-d * d));
            const float add = g * float(mIntensity);
            for (int c = 0; c < 3; c++) {
                const float v = *src * a + add * a;
                *dst++ = v > 255.f ? uchar(255) : uchar(v);
                src++;
            }
            *dst++ = uchar(*src++ * a);
        }
    }
}
