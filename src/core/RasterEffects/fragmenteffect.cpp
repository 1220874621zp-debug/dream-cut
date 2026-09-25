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

#include "fragmenteffect.h"

#include "rastereffectcaller.h"
#include "transitionanchor.h"
#include "Animators/qrealanimator.h"
#include "appsupport.h"

#include <cmath>

FragmentEffect::FragmentEffect() :
    RasterEffect(QObject::tr("碎片聚合"),
                 AppSupport::getRasterEffectHardwareSupport("碎片聚合",
                                                            HardwareSupport::cpuOnly),
                 false,
                 RasterEffectType::TRANSITION_FRAGMENT)
{
    mFadeIn = enve::make_shared<QrealAnimator>(12, 0, 600, 1, "淡入时长");
    ca_addChild(mFadeIn);

    mFadeOut = enve::make_shared<QrealAnimator>(12, 0, 600, 1, "淡出时长");
    ca_addChild(mFadeOut);

    mCellSize = enve::make_shared<QrealAnimator>(120, 40, 400, 1, "碎片大小");
    ca_addChild(mCellSize);

    mDistance = enve::make_shared<QrealAnimator>(0.45, 0, 1, 0.01, "飞散距离");
    ca_addChild(mDistance);

    mSpin = enve::make_shared<QrealAnimator>(22, 0, 60, 1, "旋转幅度");
    ca_addChild(mSpin);
}

class FragmentEffectCaller : public RasterEffectCaller {
public:
    FragmentEffectCaller(const HardwareSupport hwSupport,
                         const qreal openness,
                         const qreal cellSize,
                         const qreal distance,
                         const qreal spin) :
        RasterEffectCaller(hwSupport), mOpenness(openness),
        mCellSize(cellSize), mDistance(distance), mSpin(spin) {}

    void processCpu(CpuRenderTools& renderTools,
                    const CpuRenderData& data);
private:
    const qreal mOpenness;
    const qreal mCellSize;
    const qreal mDistance;
    const qreal mSpin;
};

stdsptr<RasterEffectCaller> FragmentEffect::getEffectCaller(
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

    return enve::make_shared<FragmentEffectCaller>(
                instanceHwSupport(), openness,
                mCellSize->getEffectiveValue(relFrame),
                mDistance->getEffectiveValue(relFrame),
                mSpin->getEffectiveValue(relFrame));
}

void FragmentEffectCaller::processCpu(
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

    if (mOpenness >= 0.999) {
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

    const int cell = qMax(24, qRound(mCellSize));
    const qreal cx0 = w / 2.;
    const qreal cy0 = h / 2.;
    const qreal spinRad = mSpin * M_PI / 180.;
    const qreal G = 0.6;

    for (int yi = yMin; yi <= yMax; yi++) {
        auto dst = static_cast<uchar*>(
                    renderTools.fDstBtmp.getAddr(0, yi - yMin));
        const int row = yi / cell;
        int lastCol = -1;
        qreal cosPhi = 1., sinPhi = 0., invS = 1.;
        qreal offX = 0., offY = 0.;
        for (int xi = xMin; xi <= xMax; xi++) {
            const int col = xi / cell;
            if (col != lastCol) {
                lastCol = col;
                const qreal tC = nleHash21(col * 13 + 1,
                                           row * 17 + 5) * 0.4;
                const qreal eRaw = qBound(0.,
                            (mOpenness - tC) / G, 1.);
                // ease-out settle; e(1) = 1 exactly (identity)
                const qreal e = 1. - (1. - eRaw) * (1. - eRaw);
                const qreal phi = (nleHash21(col + 91, row + 37) * 2. - 1.)
                        * spinRad * (1. - e);
                cosPhi = std::cos(phi);
                sinPhi = std::sin(phi);
                invS = 1. / (0.55 + 0.45 * e);
                // radial push away from the frame centre
                const qreal ccx = (col + 0.5) * cell - cx0;
                const qreal ccy = (row + 0.5) * cell - cy0;
                const qreal len = std::max(1., std::hypot(ccx, ccy));
                const qreal off = mDistance * w * (1. - e);
                offX = ccx / len * off;
                offY = ccy / len * off;
            }
            // inverse map: un-rotate, un-scale, un-translate around
            // the shard centre; at e = 1 all three are identities
            const qreal ccx = (col + 0.5) * cell;
            const qreal ccy = (row + 0.5) * cell;
            const qreal lx = xi + 0.5 - (ccx + offX);
            const qreal ly = yi + 0.5 - (ccy + offY);
            const int sx = int(ccx + (lx * cosPhi + ly * sinPhi) * invS);
            const int sy = int(ccy + (-lx * sinPhi + ly * cosPhi) * invS);
            if (sx < 0 || sx > w - 1 || sy < 0 || sy > h - 1) {
                dst += 4; // shard body not covering this pixel yet
                continue;
            }
            const uchar* p = static_cast<const uchar*>(
                        srcBtmp.getAddr(sx, sy));
            for (int c = 0; c < 4; c++) { *dst++ = *p++; }
        }
    }
}
