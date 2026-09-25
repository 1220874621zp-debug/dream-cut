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

#include "polkadotseffect.h"

#include "rastereffectcaller.h"
#include "transitionanchor.h"
#include "Animators/qrealanimator.h"
#include "appsupport.h"

#include <cmath>

PolkaDotsEffect::PolkaDotsEffect() :
    RasterEffect(QObject::tr("波点帘幕"),
                 AppSupport::getRasterEffectHardwareSupport("波点帘幕",
                                                            HardwareSupport::cpuOnly),
                 false,
                 RasterEffectType::TRANSITION_POLKA_DOTS)
{
    mFadeIn = enve::make_shared<QrealAnimator>(12, 0, 600, 1, "淡入时长");
    ca_addChild(mFadeIn);

    mFadeOut = enve::make_shared<QrealAnimator>(12, 0, 600, 1, "淡出时长");
    ca_addChild(mFadeOut);

    mSpacing = enve::make_shared<QrealAnimator>(56, 16, 200, 1, "点距");
    ca_addChild(mSpacing);

    mCurtain = enve::make_shared<QrealAnimator>(0.5, 0, 0.9, 0.01, "帘幕延迟");
    ca_addChild(mCurtain);
}

class PolkaDotsEffectCaller : public RasterEffectCaller {
public:
    PolkaDotsEffectCaller(const HardwareSupport hwSupport,
                          const qreal openness,
                          const qreal spacing,
                          const qreal curtain) :
        RasterEffectCaller(hwSupport), mOpenness(openness),
        mSpacing(spacing), mCurtain(curtain) {}

    void processCpu(CpuRenderTools& renderTools,
                    const CpuRenderData& data);
private:
    const qreal mOpenness;
    const qreal mSpacing;
    const qreal mCurtain;
};

stdsptr<RasterEffectCaller> PolkaDotsEffect::getEffectCaller(
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

    return enve::make_shared<PolkaDotsEffectCaller>(
                instanceHwSupport(), openness,
                mSpacing->getEffectiveValue(relFrame),
                mCurtain->getEffectiveValue(relFrame));
}

void PolkaDotsEffectCaller::processCpu(
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

    const qreal C = qMax(8., mSpacing);
    const int cols = qMax(1, int(w / C) + 1);
    // columns bloom left to right: the last column starts at the
    // curtain fraction, everyone finishes together at openness 1
    const qreal growWin = 1. - qBound(0., mCurtain, 0.9);
    const qreal aa = 1.5;
    // final radius overgrows the cell corner (0.7071 C) so the dots
    // merge and cover the frame completely at full openness
    const qreal finalR = C * 0.9;

    for (int yi = yMin; yi <= yMax; yi++) {
        auto dst = static_cast<uchar*>(
                    renderTools.fDstBtmp.getAddr(0, yi - yMin));
        auto src = static_cast<uchar*>(
                    renderTools.fSrcBtmp.getAddr(xMin, yi));
        const qreal py = yi + 0.5;
        for (int xi = xMin; xi <= xMax; xi++) {
            const qreal px = xi + 0.5;
            const int col = qMin(cols - 1, int(px / C));
            const int row = int(py / C);
            const qreal tCol = double(col) / cols * (1. - growWin);
            const qreal ph = qBound(0.,
                        (mOpenness - tCol) / growWin, 1.);
            const qreal rr = ph * finalR;
            const qreal dx = px - (col + 0.5) * C;
            const qreal dy = py - (row + 0.5) * C;
            const qreal alpha = qBound(0.,
                        (rr - std::hypot(dx, dy)) / aa, 1.);
            for (int c = 0; c < 4; c++) {
                *dst++ = uchar(*src++ * alpha);
            }
        }
    }
}
