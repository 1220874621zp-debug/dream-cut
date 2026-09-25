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
// GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.
#
*/

#include "zoomblurtransitioneffect.h"

#include "rastereffectcaller.h"
#include "transitionanchor.h"
#include "Animators/qrealanimator.h"
#include "appsupport.h"

#include <cmath>

ZoomBlurTransitionEffect::ZoomBlurTransitionEffect() :
    RasterEffect(QObject::tr("变焦模糊"),
                 AppSupport::getRasterEffectHardwareSupport("变焦模糊",
                                                            HardwareSupport::cpuOnly),
                 false,
                 RasterEffectType::TRANSITION_ZOOM_BLUR)
{
    mFadeIn = enve::make_shared<QrealAnimator>(12, 0, 600, 1, "淡入时长");
    ca_addChild(mFadeIn);

    mFadeOut = enve::make_shared<QrealAnimator>(12, 0, 600, 1, "淡出时长");
    ca_addChild(mFadeOut);

    mStrength = enve::make_shared<QrealAnimator>(3, 0.5, 10, 0.5, "强度");
    ca_addChild(mStrength);
}

class ZoomBlurTransitionEffectCaller : public RasterEffectCaller {
public:
    ZoomBlurTransitionEffectCaller(const HardwareSupport hwSupport,
                                   const qreal decay,
                                   const qreal strength,
                                   const qreal alpha) :
        RasterEffectCaller(hwSupport), mDecay(decay),
        mStrength(strength), mAlpha(alpha) {}

    void processCpu(CpuRenderTools& renderTools,
                    const CpuRenderData& data);
private:
    const qreal mDecay;
    const qreal mStrength;
    const qreal mAlpha;
};

stdsptr<RasterEffectCaller> ZoomBlurTransitionEffect::getEffectCaller(
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

    return enve::make_shared<ZoomBlurTransitionEffectCaller>(
                instanceHwSupport(), 1. - openness,
                mStrength->getEffectiveValue(relFrame), openness);
}

void ZoomBlurTransitionEffectCaller::processCpu(
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

    const float a = float(mAlpha);
    const qreal cx = w / 2.;
    const qreal cy = h / 2.;
    const qreal rMax = 0.5 * std::hypot(w, h);
    // sample smear as a fraction of the pixel's own radius: centre
    // stays sharp, edges streak hardest
    const qreal k = 0.12 * mStrength * mDecay;
    const int taps = 9;

    if (k < 0.001) {
        for (int yi = yMin; yi <= yMax; yi++) {
            auto dst = static_cast<uchar*>(
                        renderTools.fDstBtmp.getAddr(0, yi - yMin));
            auto src = static_cast<uchar*>(
                        renderTools.fSrcBtmp.getAddr(xMin, yi));
            for (int xi = xMin; xi <= xMax; xi++) {
                for (int c = 0; c < 4; c++) { *dst++ = uchar(*src++ * a); }
            }
        }
        return;
    }

    for (int yi = yMin; yi <= yMax; yi++) {
        auto dst = static_cast<uchar*>(
                    renderTools.fDstBtmp.getAddr(0, yi - yMin));
        const qreal dy = (yi + 0.5) - cy;
        for (int xi = xMin; xi <= xMax; xi++) {
            const qreal dx = (xi + 0.5) - cx;
            int acc[4] = { 0, 0, 0, 0 };
            int used = 0;
            for (int t = 0; t < taps; t++) {
                const qreal f = 1. + k * (qreal(t) / (taps - 1));
                const int sx = qRound(cx + dx * f - 0.5);
                const int sy = qRound(cy + dy * f - 0.5);
                if (sx < 0 || sx >= w || sy < 0 || sy >= h) { continue; }
                const auto srow = static_cast<const uchar*>(
                            srcBtmp.getAddr(0, sy));
                const uchar* p = srow + static_cast<size_t>(sx) * 4;
                for (int c = 0; c < 4; c++) { acc[c] += p[c]; }
                used++;
            }
            if (used == 0) { dst += 4; continue; }
            for (int c = 0; c < 4; c++) {
                *dst++ = uchar(acc[c] / used * a);
            }
        }
    }
}
