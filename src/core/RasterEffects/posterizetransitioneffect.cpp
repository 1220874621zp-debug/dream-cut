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
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.
#
*/

#include "posterizetransitioneffect.h"

#include "rastereffectcaller.h"
#include "transitionanchor.h"
#include "Animators/qrealanimator.h"
#include "appsupport.h"

PosterizeTransitionEffect::PosterizeTransitionEffect() :
    RasterEffect(QObject::tr("海报"),
                 AppSupport::getRasterEffectHardwareSupport("海报",
                                                            HardwareSupport::cpuOnly),
                 false,
                 RasterEffectType::TRANSITION_POSTERIZE)
{
    mFadeIn = enve::make_shared<QrealAnimator>(12, 0, 600, 1, "淡入时长");
    ca_addChild(mFadeIn);

    mFadeOut = enve::make_shared<QrealAnimator>(12, 0, 600, 1, "淡出时长");
    ca_addChild(mFadeOut);

    mMinLevels = enve::make_shared<QrealAnimator>(2, 2, 16, 1, "最少色阶");
    ca_addChild(mMinLevels);
}

class PosterizeTransitionEffectCaller : public RasterEffectCaller {
public:
    PosterizeTransitionEffectCaller(const HardwareSupport hwSupport,
                                    const int levels,
                                    const qreal alpha) :
        RasterEffectCaller(hwSupport), mLevels(levels), mAlpha(alpha) {}

    void processCpu(CpuRenderTools& renderTools,
                    const CpuRenderData& data);
private:
    const int mLevels;
    const qreal mAlpha;
};

stdsptr<RasterEffectCaller> PosterizeTransitionEffect::getEffectCaller(
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

    const int minL = qRound(mMinLevels->getEffectiveValue(relFrame));
    // levels refine from minL up to 256 (full range = identity)
    const int levels = minL + qRound((256. - minL) * openness);

    return enve::make_shared<PosterizeTransitionEffectCaller>(
                instanceHwSupport(), levels, openness);
}

void PosterizeTransitionEffectCaller::processCpu(
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

    const float mix = float(qBound(0., mAlpha, 1.));
    const float keep = 1.f - mix;

    if (mLevels >= 250) {
        for (int yi = yMin; yi <= yMax; yi++) {
            auto dst = static_cast<uchar*>(
                        renderTools.fDstBtmp.getAddr(0, yi - yMin));
            auto src = static_cast<uchar*>(
                        renderTools.fSrcBtmp.getAddr(xMin, yi));
            for (int xi = xMin; xi <= xMax; xi++) {
                for (int c = 0; c < 4; c++) { *dst++ = *src++; }
            }
        }
        return;
    }

    const qreal step = 255. / (mLevels - 1);

    for (int yi = yMin; yi <= yMax; yi++) {
        auto dst = static_cast<uchar*>(
                    renderTools.fDstBtmp.getAddr(0, yi - yMin));
        auto src = static_cast<uchar*>(
                        renderTools.fSrcBtmp.getAddr(xMin, yi));
        for (int xi = xMin; xi <= xMax; xi++) {
            for (int c = 0; c < 4; c++) {
                // poster print develops into the true pixel: quantized
                // level steps blended with the original, shape alpha
                // kept intact (no transparent fade on the head)
                const qreal q = std::round(*src / step) * step;
                *dst++ = uchar(qBound(0., q, 255.) * keep +
                               qreal(*src) * mix);
                src++;
            }
        }
    }
}
