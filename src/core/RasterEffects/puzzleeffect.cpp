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

#include "puzzleeffect.h"

#include "rastereffectcaller.h"
#include "transitionanchor.h"
#include "Animators/qrealanimator.h"
#include "appsupport.h"

#include <cmath>

PuzzleEffect::PuzzleEffect() :
    RasterEffect(QObject::tr("拼图滑入"),
                 AppSupport::getRasterEffectHardwareSupport("拼图滑入",
                                                            HardwareSupport::cpuOnly),
                 false,
                 RasterEffectType::TRANSITION_PUZZLE)
{
    mFadeIn = enve::make_shared<QrealAnimator>(12, 0, 600, 1, "淡入时长");
    ca_addChild(mFadeIn);

    mFadeOut = enve::make_shared<QrealAnimator>(12, 0, 600, 1, "淡出时长");
    ca_addChild(mFadeOut);

    mCellSize = enve::make_shared<QrealAnimator>(110, 30, 400, 1, "拼块大小");
    ca_addChild(mCellSize);

    mSeam = enve::make_shared<QrealAnimator>(0.35, 0, 1, 0.01, "缝隙暗度");
    ca_addChild(mSeam);
}

class PuzzleEffectCaller : public RasterEffectCaller {
public:
    PuzzleEffectCaller(const HardwareSupport hwSupport,
                       const qreal openness,
                       const qreal cellSize,
                       const qreal seam) :
        RasterEffectCaller(hwSupport), mOpenness(openness),
        mCellSize(cellSize), mSeam(seam) {}

    void processCpu(CpuRenderTools& renderTools,
                    const CpuRenderData& data);
private:
    const qreal mOpenness;
    const qreal mCellSize;
    const qreal mSeam;
};

stdsptr<RasterEffectCaller> PuzzleEffect::getEffectCaller(
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

    return enve::make_shared<PuzzleEffectCaller>(
                instanceHwSupport(), openness,
                mCellSize->getEffectiveValue(relFrame),
                mSeam->getEffectiveValue(relFrame));
}

void PuzzleEffectCaller::processCpu(
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

    const int cell = qMax(16, qRound(mCellSize));
    // random starts spread over [0, G); ease-out settle per piece
    const qreal G = 0.55;

    for (int yi = yMin; yi <= yMax; yi++) {
        auto dst = static_cast<uchar*>(
                    renderTools.fDstBtmp.getAddr(0, yi - yMin));
        const int row = yi / cell;
        const int ly = yi - row * cell;
        int lastCol = -1;
        qreal travel = 0.;
        qreal seamK = 0.;
        for (int xi = xMin; xi <= xMax; xi++) {
            const int col = xi / cell;
            if (col != lastCol) {
                lastCol = col;
                const qreal tC = nleHash21(col * 3 + 11,
                                           row * 5 + 7) * G;
                const qreal eRaw = qBound(0.,
                            (mOpenness - tC) / (1. - G), 1.);
                // ease-out cubic: e(1) = 1 exactly (identity)
                const qreal e = 1. - (1. - eRaw) * (1. - eRaw) * (1. - eRaw);
                travel = (1. - e) * w;
                seamK = mSeam * (1. - e);
            }
            const int sx = int(std::round(xi - travel));
            if (sx < 0 || sx > w - 1) {
                dst += 4; // piece still off-frame: outgoing shows
                continue;
            }
            const int lx = xi - col * cell;
            const bool seam = lx < 2 || ly < 2;
            const uchar* p = static_cast<const uchar*>(
                        srcBtmp.getAddr(sx, yi));
            if (seam && seamK > 0.004) {
                const qreal k = 1. - seamK;
                for (int c = 0; c < 3; c++) {
                    *dst++ = uchar(*p++ * k);
                }
                *dst++ = *p++;
            } else {
                for (int c = 0; c < 4; c++) { *dst++ = *p++; }
            }
        }
    }
}
