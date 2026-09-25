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
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.
#
*/

#include "twirltransitioneffect.h"

#include "rastereffectcaller.h"
#include "transitionanchor.h"
#include "Animators/qrealanimator.h"
#include "appsupport.h"

#include <cmath>

TwirlTransitionEffect::TwirlTransitionEffect() :
    RasterEffect(QObject::tr("漩涡"),
                 AppSupport::getRasterEffectHardwareSupport("漩涡",
                                                            HardwareSupport::cpuOnly),
                 false,
                 RasterEffectType::TRANSITION_TWIRL)
{
    mFadeIn = enve::make_shared<QrealAnimator>(12, 0, 600, 1, "淡入时长");
    ca_addChild(mFadeIn);

    mFadeOut = enve::make_shared<QrealAnimator>(12, 0, 600, 1, "淡出时长");
    ca_addChild(mFadeOut);

    mMaxAngle = enve::make_shared<QrealAnimator>(540, 30, 2160, 30, "最大角度");
    ca_addChild(mMaxAngle);

    mRadius = enve::make_shared<QrealAnimator>(1, 0.2, 1.5, 0.05, "作用半径");
    ca_addChild(mRadius);
}

class TwirlTransitionEffectCaller : public RasterEffectCaller {
public:
    TwirlTransitionEffectCaller(const HardwareSupport hwSupport,
                                const qreal decay,
                                const qreal maxAngleRad,
                                const qreal radius) :
        RasterEffectCaller(hwSupport), mDecay(decay),
        mMaxAngle(maxAngleRad), mRadius(radius) {}

    void processCpu(CpuRenderTools& renderTools,
                    const CpuRenderData& data);
private:
    const qreal mDecay;
    const qreal mMaxAngle;
    const qreal mRadius;
};

stdsptr<RasterEffectCaller> TwirlTransitionEffect::getEffectCaller(
        const qreal relFrame, const qreal resolution,
        const qreal influence, BoxRenderData * const data) const
{
    Q_UNUSED(resolution)

    const qreal fadeIn = mFadeIn->getEffectiveValue(relFrame);
    const qreal fadeOut = mFadeOut->getEffectiveValue(relFrame);
    const qreal local = nleTransitionClipRelFrame(relFrame, data);
    const qreal total = nleTransitionTotalFrames(data);
    const qreal openness = nleTransitionOpenness(local, total,
                                                 fadeIn, fadeOut) * influence;

    return enve::make_shared<TwirlTransitionEffectCaller>(
                instanceHwSupport(), 1. - openness,
                mMaxAngle->getEffectiveValue(relFrame) * M_PI / 180.,
                mRadius->getEffectiveValue(relFrame));
}

void TwirlTransitionEffectCaller::processCpu(
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

    const qreal cx = w / 2.;
    const qreal cy = h / 2.;
    const qreal rMax = mRadius * 0.5 * std::hypot(w, h);
    // openness drives both the alpha fade and the swirl decay: fully
    // open = zero twist = plain copy
    const float alpha = float(1. - mDecay);

    for (int yi = yMin; yi <= yMax; yi++) {
        auto dst = static_cast<uchar*>(
                    renderTools.fDstBtmp.getAddr(0, yi - yMin));
        const qreal dy = (yi + 0.5) - cy;
        for (int xi = xMin; xi <= xMax; xi++) {
            const qreal dx = (xi + 0.5) - cx;
            const qreal r = std::hypot(dx, dy);
            if (r >= rMax || rMax < 1.) {
                // outside the swirl radius: plain faded copy
                const auto srow = static_cast<const uchar*>(
                            srcBtmp.getAddr(0, yi));
                const uchar* p = srow + static_cast<size_t>(xi) * 4;
                for (int c = 0; c < 4; c++) {
                    *dst++ = uchar(*p++ * alpha);
                }
                continue;
            }
            // twist fades from full at the centre to zero at rMax
            const qreal fall = 1. - r / rMax;
            const qreal ang = -mMaxAngle * mDecay * fall * fall;
            const qreal cosA = std::cos(ang);
            const qreal sinA = std::sin(ang);
            const qreal sx = cx + dx * cosA - dy * sinA - 0.5;
            const qreal sy = cy + dx * sinA + dy * cosA - 0.5;
            const int sxcl = qRound(sx);
            const int sycl = qRound(sy);
            if (sxcl < 0 || sxcl >= w || sycl < 0 || sycl >= h) {
                dst += 4;
                continue;
            }
            const auto srow = static_cast<const uchar*>(
                        srcBtmp.getAddr(0, sycl));
            const uchar* p = srow + static_cast<size_t>(sxcl) * 4;
            for (int c = 0; c < 4; c++) {
                *dst++ = uchar(*p++ * alpha);
            }
        }
    }
}
