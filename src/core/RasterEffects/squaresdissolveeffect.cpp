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

#include "squaresdissolveeffect.h"

#include "rastereffectcaller.h"
#include "transitionanchor.h"
#include "Animators/qrealanimator.h"
#include "appsupport.h"

#include <cmath>

SquaresDissolveEffect::SquaresDissolveEffect() :
    RasterEffect(QObject::tr("随机方块"),
                 AppSupport::getRasterEffectHardwareSupport("随机方块",
                                                            HardwareSupport::cpuOnly),
                 false,
                 RasterEffectType::TRANSITION_SQUARES)
{
    mFadeIn = enve::make_shared<QrealAnimator>(12, 0, 600, 1, "淡入时长");
    ca_addChild(mFadeIn);

    mFadeOut = enve::make_shared<QrealAnimator>(12, 0, 600, 1, "淡出时长");
    ca_addChild(mFadeOut);

    mCellSize = enve::make_shared<QrealAnimator>(52, 12, 240, 1, "方块大小");
    ca_addChild(mCellSize);

    mSoftness = enve::make_shared<QrealAnimator>(0.12, 0, 0.5, 0.01, "柔边");
    ca_addChild(mSoftness);
}

class SquaresDissolveEffectCaller : public RasterEffectCaller {
public:
    SquaresDissolveEffectCaller(const HardwareSupport hwSupport,
                                const qreal openness,
                                const qreal cellSize,
                                const qreal softness) :
        RasterEffectCaller(hwSupport), mOpenness(openness),
        mCellSize(cellSize), mSoftness(softness) {}

    void processCpu(CpuRenderTools& renderTools,
                    const CpuRenderData& data);
private:
    const qreal mOpenness;
    const qreal mCellSize;
    const qreal mSoftness;
};

stdsptr<RasterEffectCaller> SquaresDissolveEffect::getEffectCaller(
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

    return enve::make_shared<SquaresDissolveEffectCaller>(
                instanceHwSupport(), openness,
                mCellSize->getEffectiveValue(relFrame),
                mSoftness->getEffectiveValue(relFrame));
}

void SquaresDissolveEffectCaller::processCpu(
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

    const int cell = qMax(4, qRound(mCellSize));
    const qreal aa = qMax(0.02, mSoftness);
    // staggered starts spread over [0, 1-G]; the overshoot factor
    // guarantees every tile has overgrown its border at openness 1
    const qreal G = 0.45;
    const qreal over = 1. + aa + 0.03;

    for (int yi = yMin; yi <= yMax; yi++) {
        auto dst = static_cast<uchar*>(
                    renderTools.fDstBtmp.getAddr(0, yi - yMin));
        auto src = static_cast<uchar*>(
                    renderTools.fSrcBtmp.getAddr(xMin, yi));
        const int cy = yi / cell;
        const qreal cellCy = (cy + 0.5) * cell;
        const qreal halfH = cell / 2.;
        for (int xi = xMin; xi <= xMax; xi++) {
            const int cx = xi / cell;
            const qreal cellCx = (cx + 0.5) * cell;
            const qreal halfW = cell / 2.;
            const qreal m = std::max(std::fabs(xi + 0.5 - cellCx) / halfW,
                                     std::fabs(yi + 0.5 - cellCy) / halfH);
            const qreal tC = nleHash21(cx, cy) * (1. - G);
            const qreal s = qBound(0., (mOpenness - tC) / G, 1.) * over;
            const qreal alpha = qBound(0., (s - m) / aa, 1.);
            for (int c = 0; c < 4; c++) {
                *dst++ = uchar(*src++ * alpha);
            }
        }
    }
}
