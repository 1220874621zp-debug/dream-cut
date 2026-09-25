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

#include "starwipeeffect.h"

#include "rastereffectcaller.h"
#include "transitionanchor.h"
#include "Animators/qrealanimator.h"
#include "appsupport.h"

#include <cmath>

StarWipeEffect::StarWipeEffect() :
    RasterEffect(QObject::tr("星形扩散"),
                 AppSupport::getRasterEffectHardwareSupport("星形扩散",
                                                            HardwareSupport::cpuOnly),
                 false,
                 RasterEffectType::TRANSITION_STAR)
{
    mFadeIn = enve::make_shared<QrealAnimator>(12, 0, 600, 1, "淡入时长");
    ca_addChild(mFadeIn);

    mFadeOut = enve::make_shared<QrealAnimator>(12, 0, 600, 1, "淡出时长");
    ca_addChild(mFadeOut);

    mPoints = enve::make_shared<QrealAnimator>(5, 3, 12, 1, "角数");
    ca_addChild(mPoints);

    mInner = enve::make_shared<QrealAnimator>(0.45, 0.2, 0.8, 0.01, "内径比");
    ca_addChild(mInner);

    mSoftness = enve::make_shared<QrealAnimator>(0.04, 0.01, 0.3, 0.01, "柔边");
    ca_addChild(mSoftness);
}

class StarWipeEffectCaller : public RasterEffectCaller {
public:
    StarWipeEffectCaller(const HardwareSupport hwSupport,
                         const qreal openness,
                         const int points,
                         const qreal inner,
                         const qreal softness) :
        RasterEffectCaller(hwSupport), mOpenness(openness),
        mPoints(points), mInner(inner), mSoftness(softness) {}

    void processCpu(CpuRenderTools& renderTools,
                    const CpuRenderData& data);
private:
    const qreal mOpenness;
    const int mPoints;
    const qreal mInner;
    const qreal mSoftness;
};

stdsptr<RasterEffectCaller> StarWipeEffect::getEffectCaller(
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

    return enve::make_shared<StarWipeEffectCaller>(
                instanceHwSupport(), openness,
                qMax(3, qRound(mPoints->getEffectiveValue(relFrame))),
                mInner->getEffectiveValue(relFrame),
                mSoftness->getEffectiveValue(relFrame));
}

void StarWipeEffectCaller::processCpu(
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

    const qreal A = 1.5;
    // radius profile r(t) = inner + (1-inner)*|cos(n t/2)|^0.6:
    // spikes at 1, valleys at inner; the final scale pushes even the
    // valleys past the frame corners (identity at openness 1)
    const qreal corner = A * 1.415;
    const qreal sMin = 0.1;
    const qreal sMax = corner / mInner + 0.2;
    const qreal s = sMin + mOpenness * (sMax - sMin);
    const qreal aa = qMax(0.01, mSoftness);
    const qreal half = mPoints / 2.;

    for (int yi = yMin; yi <= yMax; yi++) {
        auto dst = static_cast<uchar*>(
                    renderTools.fDstBtmp.getAddr(0, yi - yMin));
        auto src = static_cast<uchar*>(
                    renderTools.fSrcBtmp.getAddr(xMin, yi));
        const qreal ny = (1. - 2. * (yi + 0.5) / h) * A;
        for (int xi = xMin; xi <= xMax; xi++) {
            const qreal nx = (2. * (xi + 0.5) / w - 1.) * A;
            const qreal dist = std::hypot(nx, ny);
            const qreal ang = std::atan2(ny, nx);
            const qreal prof = mInner + (1. - mInner)
                        * std::pow(std::fabs(std::cos(half * ang)), 0.6);
            const qreal alpha = qBound(0.,
                        (s * prof - dist) / aa, 1.);
            for (int c = 0; c < 4; c++) {
                *dst++ = uchar(*src++ * alpha);
            }
        }
    }
}
