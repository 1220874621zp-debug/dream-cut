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

#include "filmburntransitioneffect.h"

#include "rastereffectcaller.h"
#include "transitionanchor.h"
#include "Animators/qrealanimator.h"
#include "appsupport.h"

#include <cmath>

FilmBurnTransitionEffect::FilmBurnTransitionEffect() :
    RasterEffect(QObject::tr("胶片燃烧"),
                 AppSupport::getRasterEffectHardwareSupport("胶片燃烧",
                                                            HardwareSupport::cpuOnly),
                 false,
                 RasterEffectType::TRANSITION_FILM_BURN)
{
    mFadeIn = enve::make_shared<QrealAnimator>(12, 0, 600, 1, "淡入时长");
    ca_addChild(mFadeIn);

    mFadeOut = enve::make_shared<QrealAnimator>(12, 0, 600, 1, "淡出时长");
    ca_addChild(mFadeOut);

    mBurnWidth = enve::make_shared<QrealAnimator>(0.22, 0.05, 0.6, 0.01, "燃烧宽度");
    ca_addChild(mBurnWidth);

    mIntensity = enve::make_shared<QrealAnimator>(0.85, 0, 1, 0.01, "火焰亮度");
    ca_addChild(mIntensity);

    mGrain = enve::make_shared<QrealAnimator>(6, 1, 32, 1, "颗粒大小");
    ca_addChild(mGrain);
}

class FilmBurnTransitionEffectCaller : public RasterEffectCaller {
public:
    FilmBurnTransitionEffectCaller(const HardwareSupport hwSupport,
                                   const qreal openness,
                                   const qreal burnWidth,
                                   const qreal intensity,
                                   const int grain) :
        RasterEffectCaller(hwSupport), mOpenness(openness),
        mBurnWidth(burnWidth), mIntensity(intensity), mGrain(grain)
    {
        // fire gradient stops in the platform N32 memory order
        // (little endian = BGRA) so the loop can lerp linearly:
        // deep red at the tail, white-hot on the front
#if SK_R32_SHIFT == 0 && SK_B32_SHIFT == 16
        mFire[0] = 0; mFire[1] = 1; mFire[2] = 2;   // channel index map
        mColdR = 180; mColdG = 30; mColdB = 8;
        mHotR = 255; mHotG = 245; mHotB = 210;
#else
        mFire[0] = 2; mFire[1] = 1; mFire[2] = 0;
        mColdR = 180; mColdG = 30; mColdB = 8;
        mHotR = 255; mHotG = 245; mHotB = 210;
#endif
    }

    void processCpu(CpuRenderTools& renderTools,
                    const CpuRenderData& data);
private:
    const qreal mOpenness;
    const qreal mBurnWidth;
    const qreal mIntensity;
    const int mGrain;
    int mFire[3];
    int mColdR, mColdG, mColdB;
    int mHotR, mHotG, mHotB;
};

stdsptr<RasterEffectCaller> FilmBurnTransitionEffect::getEffectCaller(
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

    return enve::make_shared<FilmBurnTransitionEffectCaller>(
                instanceHwSupport(), openness,
                mBurnWidth->getEffectiveValue(relFrame),
                mIntensity->getEffectiveValue(relFrame),
                qMax(1, qRound(mGrain->getEffectiveValue(relFrame))));
}

void FilmBurnTransitionEffectCaller::processCpu(
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

    // reveal travels past 1 so the fire band fully clears the frame
    // at full openness (identity); soft leading edge like the noise
    // dissolve, bias so the burn rises from the bottom
    const qreal soft = 0.06;
    const qreal edge = mOpenness * (1. + soft + mBurnWidth * 1.05);

    for (int yi = yMin; yi <= yMax; yi++) {
        auto dst = static_cast<uchar*>(
                    renderTools.fDstBtmp.getAddr(0, yi - yMin));
        auto src = static_cast<uchar*>(
                    renderTools.fSrcBtmp.getAddr(xMin, yi));
        const int gy = yi / mGrain;
        const qreal bias = 0.14 * (1. - double(yi) / double(h - 1));
        for (int xi = xMin; xi <= xMax; xi++) {
            const qreal n = qBound(0.,
                        0.86 * (0.62 * nleHash21(xi / mGrain, gy)
                                + 0.38 * nleHash21(xi / mGrain / 2 + 91,
                                                   gy / 2 + 37)) + bias,
                        1.);
            const qreal d = edge - n;
            const qreal alpha = qBound(0., d / soft, 1.);
            // symmetric fire band around the front: e = 1 on it,
            // falling to 0 across the burn width either side
            const qreal e = qBound(0., 1. - std::abs(d) / mBurnWidth, 1.);
            const qreal k = e * mIntensity;
            if (k < 0.004) {
                for (int c = 0; c < 4; c++) {
                    *dst++ = uchar(*src++ * alpha);
                }
            } else {
                const qreal e2 = e * e;
                const qreal fireC[3] = {
                            mColdR + (mHotR - mColdR) * e,
                            mColdG + (mHotG - mColdG) * e2,
                            mColdB + (mHotB - mColdB) * e2 * e2 };
                const qreal outA = qMax(alpha, qMin(1., k * 1.15));
                for (int c = 0; c < 3; c++) {
                    *dst++ = uchar((*src++ * (1. - k)
                                    + fireC[mFire[c]] * k) * outA);
                }
                *dst++ = uchar(*src++ * outA);
            }
        }
    }
}
