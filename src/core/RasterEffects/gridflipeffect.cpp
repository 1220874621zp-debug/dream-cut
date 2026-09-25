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

#include "gridflipeffect.h"

#include "rastereffectcaller.h"
#include "transitionanchor.h"
#include "Animators/qrealanimator.h"
#include "appsupport.h"

#include <cmath>

GridFlipEffect::GridFlipEffect() :
    RasterEffect(QObject::tr("网格翻转"),
                 AppSupport::getRasterEffectHardwareSupport("网格翻转",
                                                            HardwareSupport::cpuOnly),
                 false,
                 RasterEffectType::TRANSITION_GRID_FLIP)
{
    mFadeIn = enve::make_shared<QrealAnimator>(12, 0, 600, 1, "淡入时长");
    ca_addChild(mFadeIn);

    mFadeOut = enve::make_shared<QrealAnimator>(12, 0, 600, 1, "淡出时长");
    ca_addChild(mFadeOut);

    mCellSize = enve::make_shared<QrealAnimator>(90, 24, 400, 1, "格子大小");
    ca_addChild(mCellSize);

    mJitter = enve::make_shared<QrealAnimator>(0.5, 0, 0.5, 0.01, "随机延迟");
    ca_addChild(mJitter);
}

class GridFlipEffectCaller : public RasterEffectCaller {
public:
    GridFlipEffectCaller(const HardwareSupport hwSupport,
                         const qreal openness,
                         const qreal cellSize,
                         const qreal jitter) :
        RasterEffectCaller(hwSupport), mOpenness(openness),
        mCellSize(cellSize), mJitter(jitter) {}

    void processCpu(CpuRenderTools& renderTools,
                    const CpuRenderData& data);
private:
    const qreal mOpenness;
    const qreal mCellSize;
    const qreal mJitter;
};

stdsptr<RasterEffectCaller> GridFlipEffect::getEffectCaller(
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

    return enve::make_shared<GridFlipEffectCaller>(
                instanceHwSupport(), openness,
                mCellSize->getEffectiveValue(relFrame),
                mJitter->getEffectiveValue(relFrame));
}

void GridFlipEffectCaller::processCpu(
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

    const int cell = qMax(10, qRound(mCellSize));
    // per-tile rotation: pi (back side, invisible) at closed easing
    // to 0 (flat) - each tile at its own random start; the 1.35
    // travel margin guarantees ph clamps to 1 at openness 1
    const qreal jit = qBound(0., mJitter, 0.5);
    const qreal win = 0.65;

    for (int yi = yMin; yi <= yMax; yi++) {
        auto dst = static_cast<uchar*>(
                    renderTools.fDstBtmp.getAddr(0, yi - yMin));
        const int row = yi / cell;
        const qreal cellCy = (row + 0.5) * cell;
        const qreal halfH = cell / 2.;
        int lastCol = -1;
        qreal cosPhi = 0.;
        bool visible = false;
        qreal shade = 1.;
        for (int xi = xMin; xi <= xMax; xi++) {
            const int col = xi / cell;
            if (col != lastCol) {
                lastCol = col;
                const qreal tC = nleHash21(col * 7 + 3,
                                           row * 5 + 13) * jit;
                const qreal ph = qBound(0.,
                            (mOpenness * 1.35 - tC) / win, 1.);
                const qreal phi = (1. - ph) * M_PI;
                cosPhi = std::cos(phi);
                // back side and near-edge-on: invisible
                visible = cosPhi > 0.05;
                // shading only while tilted: exactly 1 when flat
                shade = 1. - 0.45 * (1. - std::fabs(cosPhi));
            }
            if (!visible) {
                dst += 4;
                continue;
            }
            // flip around the tile's own horizontal axis: foreshorten
            // the sample back onto the un-tilted tile
            const qreal vy = cellCy + (yi + 0.5 - cellCy) / cosPhi;
            if (std::fabs(vy - cellCy) > halfH) {
                dst += 4; // foreshortened sample leaves the tile
                continue;
            }
            const int sy = qBound(0, int(vy), h - 1);
            const uchar* p = static_cast<const uchar*>(
                        srcBtmp.getAddr(xi, sy));
            for (int c = 0; c < 3; c++) {
                *dst++ = uchar(*p++ * shade);
            }
            *dst++ = *p++;
        }
    }
}
