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
// GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.
#
*/

#include "inverttransitioneffect.h"

#include "rastereffectcaller.h"
#include "transitionanchor.h"
#include "Animators/qrealanimator.h"
#include "appsupport.h"

InvertTransitionEffect::InvertTransitionEffect() :
    RasterEffect(QObject::tr("反色"),
                 AppSupport::getRasterEffectHardwareSupport("反色",
                                                            HardwareSupport::cpuOnly),
                 false,
                 RasterEffectType::TRANSITION_INVERT)
{
    mFadeIn = enve::make_shared<QrealAnimator>(12, 0, 600, 1, "淡入时长");
    ca_addChild(mFadeIn);

    mFadeOut = enve::make_shared<QrealAnimator>(12, 0, 600, 1, "淡出时长");
    ca_addChild(mFadeOut);
}

class InvertTransitionEffectCaller : public RasterEffectCaller {
public:
    InvertTransitionEffectCaller(const HardwareSupport hwSupport,
                                 const qreal mix, const qreal alpha) :
        RasterEffectCaller(hwSupport), mMix(mix), mAlpha(alpha) {}

    void processCpu(CpuRenderTools& renderTools,
                    const CpuRenderData& data);
private:
    const qreal mMix;
    const qreal mAlpha;
};

stdsptr<RasterEffectCaller> InvertTransitionEffect::getEffectCaller(
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

    // 0 = full negative, 1 = true colours
    return enve::make_shared<InvertTransitionEffectCaller>(
                instanceHwSupport(), openness, openness);
}

void InvertTransitionEffectCaller::processCpu(
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

    const float mix = float(mMix);
    const float keep = 1.f - mix; // negative weight
    // developing negative: the head frame shows the full negative
    // (shape alpha intact, no transparent fade - the clip covers the
    // outgoing one immediately and develops into true colours)
    const float kr = 255.f * keep;
    const float ki = 1.f - 2.f * keep;

    for (int yi = yMin; yi <= yMax; yi++) {
        auto dst = static_cast<uchar*>(
                    renderTools.fDstBtmp.getAddr(0, yi - yMin));
        auto src = static_cast<uchar*>(
                    renderTools.fSrcBtmp.getAddr(xMin, yi));
        for (int xi = xMin; xi <= xMax; xi++) {
            for (int c = 0; c < 3; c++) {
                // c' = keep*(255-c)*al/255 + mix*c ... in premul space
                // lerp(negative_premul, src_premul) = kr*al + ki*c
                *dst++ = uchar(kr * src[3] / 255.f + *src * ki);
                src++;
            }
            *dst++ = *src++;
        }
    }
}
