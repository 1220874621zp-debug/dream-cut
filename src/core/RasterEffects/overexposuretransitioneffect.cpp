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
*/

#include "overexposuretransitioneffect.h"

#include "rastereffectcaller.h"
#include "transitionanchor.h"
#include "Animators/qrealanimator.h"
#include "appsupport.h"

#include <cmath>

OverexposureTransitionEffect::OverexposureTransitionEffect() :
    RasterEffect(QObject::tr("过曝"),
                 AppSupport::getRasterEffectHardwareSupport("过曝",
                                                            HardwareSupport::cpuOnly),
                 false,
                 RasterEffectType::TRANSITION_OVEREXPOSE)
{
    mFadeIn = enve::make_shared<QrealAnimator>(12, 0, 600, 1, "淡入时长");
    ca_addChild(mFadeIn);

    mFadeOut = enve::make_shared<QrealAnimator>(12, 0, 600, 1, "淡出时长");
    ca_addChild(mFadeOut);

    mStrength = enve::make_shared<QrealAnimator>(0.9, 0, 1, 0.01, "曝光强度");
    ca_addChild(mStrength);

    mRate = enve::make_shared<QrealAnimator>(2, 0.5, 4, 0.1, "起亮速度");
    ca_addChild(mRate);
}

class OverexposureTransitionEffectCaller : public RasterEffectCaller {
public:
    OverexposureTransitionEffectCaller(const HardwareSupport hwSupport,
                                       const qreal openness,
                                       const qreal strength) :
        RasterEffectCaller(hwSupport), mOpenness(openness),
        mStrength(strength) {}

    void processCpu(CpuRenderTools& renderTools,
                    const CpuRenderData& data);
private:
    const qreal mOpenness;
    const qreal mStrength;
};

stdsptr<RasterEffectCaller> OverexposureTransitionEffect::getEffectCaller(
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

    const qreal rate = mRate->getEffectiveValue(relFrame);
    const qreal punch = std::pow(1. - openness, rate);

    return enve::make_shared<OverexposureTransitionEffectCaller>(
                instanceHwSupport(), openness,
                mStrength->getEffectiveValue(relFrame) * punch);
}

void OverexposureTransitionEffectCaller::processCpu(
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

    const float a = float(mOpenness);
    const float lift = float(mStrength);

    if (lift < 0.004f) {
        for (int yi = yMin; yi <= yMax; yi++) {
            auto dst = static_cast<uchar*>(
                        renderTools.fDstBtmp.getAddr(0, yi - yMin));
            auto src = static_cast<uchar*>(
                        renderTools.fSrcBtmp.getAddr(xMin, yi));
            for (int xi = xMin; xi <= xMax; xi++) {
                for (int c = 0; c < 4; c++) {
                    *dst++ = uchar(*src++ * a);
                }
            }
        }
        return;
    }

    // screen-style lift towards white with the alpha fade on top
    for (int yi = yMin; yi <= yMax; yi++) {
        auto dst = static_cast<uchar*>(
                    renderTools.fDstBtmp.getAddr(0, yi - yMin));
        auto src = static_cast<uchar*>(
                        renderTools.fSrcBtmp.getAddr(xMin, yi));
        for (int xi = xMin; xi <= xMax; xi++) {
            for (int c = 0; c < 4; c++) {
                const float v = float(*src++);
                *dst++ = uchar((v + (255.f - v) * lift) * a);
            }
        }
    }
}
