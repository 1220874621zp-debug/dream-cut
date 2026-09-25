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

#include "bookflipeffect.h"

#include "rastereffectcaller.h"
#include "transitionanchor.h"
#include "Animators/qrealanimator.h"
#include "appsupport.h"

#include <cmath>

BookFlipEffect::BookFlipEffect() :
    RasterEffect(QObject::tr("翻书"),
                 AppSupport::getRasterEffectHardwareSupport("翻书",
                                                            HardwareSupport::cpuOnly),
                 false,
                 RasterEffectType::TRANSITION_BOOK_FLIP)
{
    mFadeIn = enve::make_shared<QrealAnimator>(12, 0, 600, 1, "淡入时长");
    ca_addChild(mFadeIn);

    mFadeOut = enve::make_shared<QrealAnimator>(12, 0, 600, 1, "淡出时长");
    ca_addChild(mFadeOut);

    mDist = enve::make_shared<QrealAnimator>(2.2, 1.4, 4, 0.05, "视距");
    ca_addChild(mDist);

    mShade = enve::make_shared<QrealAnimator>(0.4, 0, 0.8, 0.01, "阴影");
    ca_addChild(mShade);
}

class BookFlipEffectCaller : public RasterEffectCaller {
public:
    BookFlipEffectCaller(const HardwareSupport hwSupport,
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

stdsptr<RasterEffectCaller> BookFlipEffect::getEffectCaller(
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

    return enve::make_shared<BookFlipEffectCaller>(
                instanceHwSupport(), openness,
                mDist->getEffectiveValue(relFrame),
                mShade->getEffectiveValue(relFrame));
}

void BookFlipEffectCaller::processCpu(
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

    // page angle: 90 degrees (edge-on, nothing visible) at closed,
    // laying flat at full openness; the ^0.7 curve keeps the sweep
    // moving through the whole window (a linear ramp finishes the
    // reveal in the first third and then just settles)
    const qreal theta = std::pow(1. - mOpenness, 0.7) * M_PI / 2.;
    const qreal cosT = std::cos(theta);
    const qreal sinT = std::sin(theta);
    // shading only while tilted: 1 - depth*(1-cos) is exactly 1 when
    // the page lies flat (identity)
    const qreal shade = 1. - mShade * (1. - cosT);

    if (mOpenness >= 0.999) {
        for (int yi = yMin; yi <= yMax; yi++) {
            auto dst = static_cast<uchar*>(
                        renderTools.fDstBtmp.getAddr(0, yi - yMin));
            auto src = static_cast<uchar*>(
                        renderTools.fSrcBtmp.getAddr(xMin, yi));
            for (int xi = xMin; xi <= xMax; xi++) {
                for (int c = 0; c < 4; c++) {
                    *dst++ = *src++;
                }
            }
        }
        return;
    }

    // hinge on the left edge, camera at distance Z (in frame-width
    // units) straight in front of the screen plane: a page point at
    // distance d projects to screen_x = d*cosT*Z / (Z + d*sinT);
    // invert for the sample: d = Z*x / (Z*cosT - x*sinT), valid
    // while 0 <= d <= 1
    for (int yi = yMin; yi <= yMax; yi++) {
        auto dst = static_cast<uchar*>(
                    renderTools.fDstBtmp.getAddr(0, yi - yMin));
        for (int xi = xMin; xi <= xMax; xi++) {
            const qreal xn = (xi + 0.5) / w;
            const qreal denom = mDist * cosT - xn * sinT;
            if (denom <= 1e-6) {
                dst += 4; // past the page edge: outgoing clip shows
                continue;
            }
            const qreal d = mDist * xn / denom;
            if (d > 1.) {
                dst += 4;
                continue;
            }
            const int sx = qBound(0, int(d * w), w - 1);
            const uchar* p = static_cast<const uchar*>(
                        srcBtmp.getAddr(sx, yi));
            for (int c = 0; c < 3; c++) {
                *dst++ = uchar(*p++ * shade);
            }
            *dst++ = *p++;
        }
    }
}
