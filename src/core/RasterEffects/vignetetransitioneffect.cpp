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
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.
#
*/

#include "vignetetransitioneffect.h"

#include "rastereffectcaller.h"
#include "transitionanchor.h"
#include "Animators/qrealanimator.h"
#include "appsupport.h"

#include <cmath>

VignetteTransitionEffect::VignetteTransitionEffect() :
    RasterEffect(QObject::tr("暗角"),
                 AppSupport::getRasterEffectHardwareSupport("暗角",
                                                            HardwareSupport::cpuOnly),
                 false,
                 RasterEffectType::TRANSITION_VIGNETTE)
{
    mFadeIn = enve::make_shared<QrealAnimator>(12, 0, 600, 1, "淡入时长");
    ca_addChild(mFadeIn);

    mFadeOut = enve::make_shared<QrealAnimator>(12, 0, 600, 1, "淡出时长");
    ca_addChild(mFadeOut);

    mSoftness = enve::make_shared<QrealAnimator>(0.45, 0.05, 1, 0.01, "柔边");
    ca_addChild(mSoftness);
}

class VignetteTransitionEffectCaller : public RasterEffectCaller {
public:
    VignetteTransitionEffectCaller(const HardwareSupport hwSupport,
                                   const qreal openness,
                                   const qreal softness) :
        RasterEffectCaller(hwSupport), mOpenness(openness),
        mSoftness(softness) {}

    void processCpu(CpuRenderTools& renderTools,
                    const CpuRenderData& data);
private:
    const qreal mOpenness;
    const qreal mSoftness;
};

stdsptr<RasterEffectCaller> VignetteTransitionEffect::getEffectCaller(
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

    return enve::make_shared<VignetteTransitionEffectCaller>(
                instanceHwSupport(), openness,
                mSoftness->getEffectiveValue(relFrame));
}

void VignetteTransitionEffectCaller::processCpu(
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

    const qreal soft = qMax(0.001, mSoftness);
    // elliptical distance normalised so the frame corners = 1 (raw
    // hypot reaches sqrt(2) there, which would never fully open)
    const qreal edge = mOpenness * (1. + soft);

    for (int yi = yMin; yi <= yMax; yi++) {
        auto dst = static_cast<uchar*>(
                    renderTools.fDstBtmp.getAddr(0, yi - yMin));
        auto src = static_cast<uchar*>(
                    renderTools.fSrcBtmp.getAddr(xMin, yi));
        const qreal ny = 2. * (yi + 0.5) / h - 1.;
        for (int xi = xMin; xi <= xMax; xi++) {
            const qreal nx = 2. * (xi + 0.5) / w - 1.;
            const qreal d = std::hypot(nx, ny) / M_SQRT2;
            const float vis = float(qBound(0., (edge - d) / soft, 1.));
            for (int c = 0; c < 4; c++) {
                *dst++ = uchar(*src++ * vis);
            }
        }
    }
}
