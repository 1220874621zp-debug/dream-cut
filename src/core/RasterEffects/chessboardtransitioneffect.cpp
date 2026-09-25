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

#include "chessboardtransitioneffect.h"

#include "rastereffectcaller.h"
#include "transitionanchor.h"
#include "Animators/qrealanimator.h"
#include "appsupport.h"

#include <cmath>

ChessboardTransitionEffect::ChessboardTransitionEffect() :
    RasterEffect(QObject::tr("棋盘格"),
                 AppSupport::getRasterEffectHardwareSupport("棋盘格",
                                                            HardwareSupport::cpuOnly),
                 false,
                 RasterEffectType::TRANSITION_CHESSBOARD)
{
    mFadeIn = enve::make_shared<QrealAnimator>(12, 0, 600, 1, "淡入时长");
    ca_addChild(mFadeIn);

    mFadeOut = enve::make_shared<QrealAnimator>(12, 0, 600, 1, "淡出时长");
    ca_addChild(mFadeOut);

    mCellSize = enve::make_shared<QrealAnimator>(72, 16, 300, 1, "格子大小");
    ca_addChild(mCellSize);
}

class ChessboardTransitionEffectCaller : public RasterEffectCaller {
public:
    ChessboardTransitionEffectCaller(const HardwareSupport hwSupport,
                                     const qreal openness,
                                     const qreal cellSize) :
        RasterEffectCaller(hwSupport), mOpenness(openness),
        mCellSize(cellSize) {}

    void processCpu(CpuRenderTools& renderTools,
                    const CpuRenderData& data);
private:
    const qreal mOpenness;
    const qreal mCellSize;
};

stdsptr<RasterEffectCaller> ChessboardTransitionEffect::getEffectCaller(
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

    return enve::make_shared<ChessboardTransitionEffectCaller>(
                instanceHwSupport(), openness,
                mCellSize->getEffectiveValue(relFrame));
}

void ChessboardTransitionEffectCaller::processCpu(
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

    const int cell = qMax(6, qRound(mCellSize));
    const qreal aa = 0.1;
    // two waves: even cells start at 0, odd cells half a window in;
    // the overshoot guarantees full coverage at openness 1
    const qreal wave = 0.45;
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
            const qreal m = std::max(std::fabs(xi + 0.5 - cellCx) / (cell / 2.),
                                     std::fabs(yi + 0.5 - cellCy) / halfH);
            const qreal start = ((cx + cy) & 1) ? wave : 0.;
            const qreal s = qBound(0.,
                        (mOpenness * 1.02 - start) / (1. - wave), 1.) * over;
            const qreal alpha = qBound(0., (s - m) / aa, 1.);
            for (int c = 0; c < 4; c++) {
                *dst++ = uchar(*src++ * alpha);
            }
        }
    }
}
