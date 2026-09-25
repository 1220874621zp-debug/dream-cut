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

#include "heartwipeeffect.h"

#include "rastereffectcaller.h"
#include "transitionanchor.h"
#include "Animators/qrealanimator.h"
#include "appsupport.h"

#include <cmath>
#include <vector>

HeartWipeEffect::HeartWipeEffect() :
    RasterEffect(QObject::tr("心形扩散"),
                 AppSupport::getRasterEffectHardwareSupport("心形扩散",
                                                            HardwareSupport::cpuOnly),
                 false,
                 RasterEffectType::TRANSITION_HEART)
{
    mFadeIn = enve::make_shared<QrealAnimator>(12, 0, 600, 1, "淡入时长");
    ca_addChild(mFadeIn);

    mFadeOut = enve::make_shared<QrealAnimator>(12, 0, 600, 1, "淡出时长");
    ca_addChild(mFadeOut);

    mSoftness = enve::make_shared<QrealAnimator>(0.04, 0.01, 0.3, 0.01, "柔边");
    ca_addChild(mSoftness);
}

class HeartWipeEffectCaller : public RasterEffectCaller {
public:
    HeartWipeEffectCaller(const HardwareSupport hwSupport,
                          const qreal openness,
                          const qreal softness) :
        RasterEffectCaller(hwSupport), mOpenness(openness),
        mSoftness(softness)
    {
        // bake the parametric heart x = 16 sin^3 t, y = 13 cos t -
        // 5 cos 2t - 2 cos 3t - cos 4t into a max-radius-per-angle
        // table (angle measured with y up)
        mLut.resize(LutN, 0.);
        for (int i = 0; i < 4096; i++) {
            const qreal t = 2. * M_PI * i / 4096.;
            const qreal x = 16. * std::pow(std::sin(t), 3) / 17.;
            const qreal y = (13. * std::cos(t) - 5. * std::cos(2. * t)
                             - 2. * std::cos(3. * t)
                             - std::cos(4. * t)) / 17.;
            const qreal ang = std::atan2(y, x);
            const int bin = qBound(0, int((ang + M_PI) / (2. * M_PI)
                                          * LutN), LutN - 1);
            const qreal r = std::hypot(x, y);
            if (r > mLut[bin]) { mLut[bin] = r; }
        }
        // fill any angle bins the parametric sweep missed (spokes
        // near the cusp) with their neighbours' max
        for (int i = 0; i < LutN; i++) {
            if (mLut[i] <= 0.) {
                const int l = (i + LutN - 1) % LutN;
                const int rr = (i + 1) % LutN;
                mLut[i] = qMax(mLut[l], mLut[rr]);
            }
        }
        mRMin = *std::min_element(mLut.begin(), mLut.end());
    }

    void processCpu(CpuRenderTools& renderTools,
                    const CpuRenderData& data);
private:
    static const int LutN = 512;
    const qreal mOpenness;
    const qreal mSoftness;
    std::vector<qreal> mLut;
    qreal mRMin = 0.5;
};

stdsptr<RasterEffectCaller> HeartWipeEffect::getEffectCaller(
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

    return enve::make_shared<HeartWipeEffectCaller>(
                instanceHwSupport(), openness,
                mSoftness->getEffectiveValue(relFrame));
}

void HeartWipeEffectCaller::processCpu(
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

    // normalized coords: the frame maps to [-A..A] on the short
    // normalisation A = 1.5 (covers the diagonal with margin)
    const qreal A = 1.5;
    // scale so even the shallowest heart direction (the top dip)
    // reaches past the farthest corner at openness 1 (identity)
    const qreal sMin = 0.1;
    const qreal sMax = (A * 1.415) / mRMin + 0.15;
    const qreal s = sMin + mOpenness * (sMax - sMin);
    const qreal aa = qMax(0.01, mSoftness);

    for (int yi = yMin; yi <= yMax; yi++) {
        auto dst = static_cast<uchar*>(
                    renderTools.fDstBtmp.getAddr(0, yi - yMin));
        auto src = static_cast<uchar*>(
                    renderTools.fSrcBtmp.getAddr(xMin, yi));
        // screen y is down, the heart table is y up
        const qreal ny = (1. - 2. * (yi + 0.5) / h) * A;
        for (int xi = xMin; xi <= xMax; xi++) {
            const qreal nx = (2. * (xi + 0.5) / w - 1.) * A;
            const qreal dist = std::hypot(nx, ny);
            const qreal ang = std::atan2(ny, nx);
            const int bin = qBound(0, int((ang + M_PI) / (2. * M_PI)
                                          * LutN), LutN - 1);
            const qreal alpha = qBound(0.,
                        (s * mLut[bin] - dist) / aa, 1.);
            for (int c = 0; c < 4; c++) {
                *dst++ = uchar(*src++ * alpha);
            }
        }
    }
}
