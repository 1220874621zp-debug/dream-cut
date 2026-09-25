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

#include "hexdissolveeffect.h"

#include "rastereffectcaller.h"
#include "transitionanchor.h"
#include "Animators/qrealanimator.h"
#include "appsupport.h"

#include <cmath>

HexDissolveEffect::HexDissolveEffect() :
    RasterEffect(QObject::tr("蜂窝溶解"),
                 AppSupport::getRasterEffectHardwareSupport("蜂窝溶解",
                                                            HardwareSupport::cpuOnly),
                 false,
                 RasterEffectType::TRANSITION_HEX_DISSOLVE)
{
    mFadeIn = enve::make_shared<QrealAnimator>(12, 0, 600, 1, "淡入时长");
    ca_addChild(mFadeIn);

    mFadeOut = enve::make_shared<QrealAnimator>(12, 0, 600, 1, "淡出时长");
    ca_addChild(mFadeOut);

    mCellSize = enve::make_shared<QrealAnimator>(44, 12, 200, 1, "蜂窝大小");
    ca_addChild(mCellSize);

    mGrowth = enve::make_shared<QrealAnimator>(0.5, 0.1, 1, 0.01, "生长窗口");
    ca_addChild(mGrowth);
}

class HexDissolveEffectCaller : public RasterEffectCaller {
public:
    HexDissolveEffectCaller(const HardwareSupport hwSupport,
                            const qreal openness,
                            const qreal cellSize,
                            const qreal growth) :
        RasterEffectCaller(hwSupport), mOpenness(openness),
        mCellSize(cellSize), mGrowth(growth) {}

    void processCpu(CpuRenderTools& renderTools,
                    const CpuRenderData& data);
private:
    const qreal mOpenness;
    const qreal mCellSize;
    const qreal mGrowth;
};

stdsptr<RasterEffectCaller> HexDissolveEffect::getEffectCaller(
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

    return enve::make_shared<HexDissolveEffectCaller>(
                instanceHwSupport(), openness,
                mCellSize->getEffectiveValue(relFrame),
                mGrowth->getEffectiveValue(relFrame));
}

namespace {
// cube-round a fractional flat-top axial coordinate to the nearest
// hex cell (Red Blob Games reference algorithm)
inline void hexRound(qreal& q, qreal& r)
{
    qreal x = q;
    qreal z = r;
    qreal y = -x - z;
    qreal rx = std::round(x);
    qreal ry = std::round(y);
    qreal rz = std::round(z);
    const qreal dx = std::fabs(rx - x);
    const qreal dy = std::fabs(ry - y);
    const qreal dz = std::fabs(rz - z);
    if (dx > dy && dx > dz) { rx = -ry - rz; }
    else if (dy > dz) { ry = -rx - rz; }
    else { rz = -rx - ry; }
    q = rx;
    r = rz;
}
}

void HexDissolveEffectCaller::processCpu(
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

    // hex-space edge softness (1.0 = the cell border distance)
    const qreal aa = 0.08;
    // staggered starts spread over [0, 1-G]; the overshoot factor
    // guarantees every cell has fully overgrown its border at
    // openness 1 (identity)
    const qreal G = qBound(0.1, mGrowth, 1.);
    const qreal over = 1. + 2. * aa + 0.03;

    const qreal R = qMax(4., mCellSize);
    const qreal kQ = 2. / 3. / R;
    const qreal kR = -1. / 3. / R;
    const qreal kS = std::sqrt(3.) / 3. / R;

    for (int yi = yMin; yi <= yMax; yi++) {
        auto dst = static_cast<uchar*>(
                    renderTools.fDstBtmp.getAddr(0, yi - yMin));
        auto src = static_cast<uchar*>(
                    renderTools.fSrcBtmp.getAddr(xMin, yi));
        const qreal py = yi + 0.5;
        for (int xi = xMin; xi <= xMax; xi++) {
            const qreal px = xi + 0.5;
            qreal q = px * kQ;
            qreal r = py * kR + py * kS;
            hexRound(q, r);
            // hex-space distance from the cell centre (1 = border)
            const qreal dq = px * kQ - q;
            const qreal dr = (py * kR + py * kS) - r;
            const qreal ds = -dq - dr;
            const qreal d = std::max(std::max(std::fabs(dq),
                                              std::fabs(dr)),
                                     std::fabs(ds));
            const qreal tC = nleHash21(int(q) * 3 + 11, int(r) * 5 + 7);
            const qreal startC = tC * (1. - G);
            const qreal grow = qBound(0.,
                        (mOpenness - startC) / G, 1.) * over;
            const qreal alpha = qBound(0., (grow - d) / aa, 1.);
            for (int c = 0; c < 4; c++) {
                *dst++ = uchar(*src++ * alpha);
            }
        }
    }
}
