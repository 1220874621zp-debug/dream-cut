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

#include "crosszoomtransitioneffect.h"

#include "rastereffectcaller.h"
#include "transitionanchor.h"
#include "Animators/qrealanimator.h"
#include "appsupport.h"

#include <cmath>

CrossZoomTransitionEffect::CrossZoomTransitionEffect() :
    RasterEffect(QObject::tr("交叉变焦"),
                 AppSupport::getRasterEffectHardwareSupport("交叉变焦",
                                                            HardwareSupport::cpuOnly),
                 false,
                 RasterEffectType::TRANSITION_CROSS_ZOOM)
{
    mFadeIn = enve::make_shared<QrealAnimator>(12, 0, 600, 1, "淡入时长");
    ca_addChild(mFadeIn);

    mFadeOut = enve::make_shared<QrealAnimator>(12, 0, 600, 1, "淡出时长");
    ca_addChild(mFadeOut);

    mDepth = enve::make_shared<QrealAnimator>(0.4, 0.05, 1.5, 0.01, "变焦幅度");
    ca_addChild(mDepth);

    mStrength = enve::make_shared<QrealAnimator>(0.5, 0, 2, 0.01, "模糊强度");
    ca_addChild(mStrength);
}

class CrossZoomTransitionEffectCaller : public RasterEffectCaller {
public:
    CrossZoomTransitionEffectCaller(const HardwareSupport hwSupport,
                                    const qreal openness,
                                    const qreal depth,
                                    const qreal strength) :
        RasterEffectCaller(hwSupport), mOpenness(openness),
        mDepth(depth), mStrength(strength) {}

    void processCpu(CpuRenderTools& renderTools,
                    const CpuRenderData& data);
private:
    const qreal mOpenness;
    const qreal mDepth;
    const qreal mStrength;
};

stdsptr<RasterEffectCaller> CrossZoomTransitionEffect::getEffectCaller(
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

    return enve::make_shared<CrossZoomTransitionEffectCaller>(
                instanceHwSupport(), openness,
                mDepth->getEffectiveValue(relFrame),
                mStrength->getEffectiveValue(relFrame));
}

void CrossZoomTransitionEffectCaller::processCpu(
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

    const float a = float(mOpenness);
    const qreal punch = 1. - mOpenness;
    const qreal scale = 1. + mDepth * punch;
    // radial streak spread as a fraction of the distance to the
    // centre; collapses to 0 with the punch so openness 1 is identity
    const qreal spread = mStrength * 0.6 * punch;

    if (mOpenness >= 0.999 || spread < 0.002) {
        for (int yi = yMin; yi <= yMax; yi++) {
            auto dst = static_cast<uchar*>(
                        renderTools.fDstBtmp.getAddr(0, yi - yMin));
            auto src = static_cast<uchar*>(
                        renderTools.fSrcBtmp.getAddr(xMin, yi));
            for (int xi = xMin; xi <= xMax; xi++) {
                for (int c = 0; c < 4; c++) {
                    *dst++ = uchar(*src++ * a);
                }
            }
        }
        return;
    }

    const qreal cx = w / 2.;
    const qreal cy = h / 2.;
    const int taps = 9;

    for (int yi = yMin; yi <= yMax; yi++) {
        auto dst = static_cast<uchar*>(
                    renderTools.fDstBtmp.getAddr(0, yi - yMin));
        const qreal dy = (yi + 0.5) - cy;
        for (int xi = xMin; xi <= xMax; xi++) {
            const qreal dx = (xi + 0.5) - cx;
            float acc[4] = { 0.f, 0.f, 0.f, 0.f };
            for (int j = 0; j < taps; j++) {
                const qreal wj = (2. * j / (taps - 1)) - 1.;
                const qreal m = (1. + wj * spread) / scale;
                const int sx = qBound(0, int(cx + dx * m), w - 1);
                const int sy = qBound(0, int(cy + dy * m), h - 1);
                const uchar* p = static_cast<const uchar*>(
                            srcBtmp.getAddr(sx, sy));
                for (int c = 0; c < 4; c++) { acc[c] += p[c]; }
            }
            for (int c = 0; c < 4; c++) {
                *dst++ = uchar(acc[c] / taps * a);
            }
        }
    }
}
