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

#include "cubeeffect.h"

#include "rastereffectcaller.h"
#include "transitionanchor.h"
#include "Animators/qrealanimator.h"
#include "appsupport.h"

#include <cmath>

CubeEffect::CubeEffect() :
    RasterEffect(QObject::tr("立方体"),
                 AppSupport::getRasterEffectHardwareSupport("立方体",
                                                            HardwareSupport::cpuOnly),
                 false,
                 RasterEffectType::TRANSITION_CUBE)
{
    mFadeIn = enve::make_shared<QrealAnimator>(12, 0, 600, 1, "淡入时长");
    ca_addChild(mFadeIn);

    mFadeOut = enve::make_shared<QrealAnimator>(12, 0, 600, 1, "淡出时长");
    ca_addChild(mFadeOut);

    mDist = enve::make_shared<QrealAnimator>(1.7, 1.25, 3, 0.05, "视距");
    ca_addChild(mDist);

    mShade = enve::make_shared<QrealAnimator>(0.45, 0, 0.8, 0.01, "阴影");
    ca_addChild(mShade);
}

class CubeEffectCaller : public RasterEffectCaller {
public:
    CubeEffectCaller(const HardwareSupport hwSupport,
                     const qreal openness,
                     const qreal dist,
                     const qreal shade) :
        RasterEffectCaller(hwSupport), mOpenness(openness),
        mDist(dist), mShade(shade) {}

    void processCpu(CpuRenderTools& renderTools,
                    const CpuRenderData& data);
private:
    const qreal mOpenness;
    const qreal mDist;
    const qreal mShade;
};

stdsptr<RasterEffectCaller> CubeEffect::getEffectCaller(
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

    return enve::make_shared<CubeEffectCaller>(
                instanceHwSupport(), openness,
                mDist->getEffectiveValue(relFrame),
                mShade->getEffectiveValue(relFrame));
}

void CubeEffectCaller::processCpu(
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

    // face angle: 90 degrees (standing edge-on along the bottom
    // hinge) at closed, toppling flat towards the camera as it opens;
    // the ^0.7 curve keeps the topple moving through the whole window
    // (a linear ramp completes it in the first third)
    const qreal theta = std::pow(1. - mOpenness, 0.7) * M_PI / 2.;
    const qreal cosT = std::cos(theta);
    const qreal sinT = std::sin(theta);
    const qreal Z = qBound(1.25, mDist, 3.);
    // shading only while tilted: exactly 1 when flat (identity)
    const qreal shade = 1. - mShade * (1. - cosT);

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

    for (int yi = yMin; yi <= yMax; yi++) {
        auto dst = static_cast<uchar*>(
                    renderTools.fDstBtmp.getAddr(0, yi - yMin));
        // distance above the bottom hinge, normalised
        const qreal sn = (h - (yi + 0.5)) / h;
        // a face point at distance d projects to height d*cosT*Z /
        // (Z - d*sinT) above the hinge (magnified as it swings
        // towards the lens); invert: d = sn*Z / (sn*sinT + Z*cosT)
        const qreal denom = sn * sinT + Z * cosT;
        const qreal d = denom > 1e-6 ? sn * Z / denom : 2.;
        if (d > 1.) {
            // this screen row is still above the tilted face's top
            // edge: the outgoing clip shows through
            dst += 4 * (xMax - xMin + 1);
            continue;
        }
        // the same perspective magnifies the row's x spread
        const qreal persp = Z / qMax(0.2, Z - d * sinT);
        const qreal vTop = 1. - d; // source row, from the top
        const int sy = qBound(0, int(vTop * h), h - 1);
        for (int xi = xMin; xi <= xMax; xi++) {
            const qreal xn = (xi + 0.5) / w - 0.5;
            const int sx = int((xn / persp + 0.5) * w);
            if (sx < 0 || sx > w - 1) {
                dst += 4;
                continue;
            }
            const uchar* p = static_cast<const uchar*>(
                        srcBtmp.getAddr(sx, sy));
            for (int c = 0; c < 3; c++) {
                *dst++ = uchar(*p++ * shade);
            }
            *dst++ = *p++;
        }
    }
}
