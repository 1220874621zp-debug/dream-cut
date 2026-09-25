/*
#
# Friction - https://friction.graphics
#
# Copyright (c) Ole-André Rodlie and contributors.
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.
#
*/

#include "edgetransitioneffect.h"

#include "rastereffectcaller.h"
#include "transitionanchor.h"
#include "Animators/qrealanimator.h"
#include "appsupport.h"

EdgeTransitionEffect::EdgeTransitionEffect() :
    RasterEffect(QObject::tr("线稿"),
                 AppSupport::getRasterEffectHardwareSupport("线稿",
                                                            HardwareSupport::cpuOnly),
                 false,
                 RasterEffectType::TRANSITION_EDGE)
{
    mFadeIn = enve::make_shared<QrealAnimator>(12, 0, 600, 1, "淡入时长");
    ca_addChild(mFadeIn);

    mFadeOut = enve::make_shared<QrealAnimator>(12, 0, 600, 1, "淡出时长");
    ca_addChild(mFadeOut);

    mLineGain = enve::make_shared<QrealAnimator>(2, 0.5, 8, 0.1, "线条浓度");
    ca_addChild(mLineGain);
}

class EdgeTransitionEffectCaller : public RasterEffectCaller {
public:
    EdgeTransitionEffectCaller(const HardwareSupport hwSupport,
                               const qreal mix, const qreal alpha,
                               const qreal gain) :
        RasterEffectCaller(hwSupport), mMix(mix), mAlpha(alpha),
        mGain(gain) {}

    void processCpu(CpuRenderTools& renderTools,
                    const CpuRenderData& data);
private:
    const qreal mMix;
    const qreal mAlpha;
    const qreal mGain;
};

stdsptr<RasterEffectCaller> EdgeTransitionEffect::getEffectCaller(
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

    // 0 = pure sketch, 1 = original image
    return enve::make_shared<EdgeTransitionEffectCaller>(
                instanceHwSupport(), openness, openness,
                mLineGain->getEffectiveValue(relFrame));
}

void EdgeTransitionEffectCaller::processCpu(
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

    const float mix = float(mMix);
    const float keep = 1.f - mix; // sketch weight
    Q_UNUSED(mAlpha)

    if (mix >= 0.999f) {
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

    // unpremultiplied luma at a clamped coordinate
    auto lumaAt = [&srcBtmp, w, h](const int x, const int y) {
        const int xc = qBound(0, x, w - 1);
        const int yc = qBound(0, y, h - 1);
        const auto row = static_cast<const uchar*>(
                    srcBtmp.getAddr(0, yc));
        const uchar* p = row + static_cast<size_t>(xc) * 4;
        const int al = p[3];
        if (al == 0) { return 1.f; }
        const float inv = 1.f / (al * 255.f);
        return float(0.30f * p[2] + 0.59f * p[1] + 0.11f * p[0]) * inv;
    };

    for (int yi = yMin; yi <= yMax; yi++) {
        auto dst = static_cast<uchar*>(
                    renderTools.fDstBtmp.getAddr(0, yi - yMin));
        const auto srow = static_cast<const uchar*>(
                    srcBtmp.getAddr(0, yi));
        for (int xi = xMin; xi <= xMax; xi++) {
            // Sobel magnitude on the luma plane
            const float gx = -lumaAt(xi-1,yi-1) - 2.f*lumaAt(xi-1,yi)
                             - lumaAt(xi-1,yi+1)
                             + lumaAt(xi+1,yi-1) + 2.f*lumaAt(xi+1,yi)
                             + lumaAt(xi+1,yi+1);
            const float gy = -lumaAt(xi-1,yi-1) - 2.f*lumaAt(xi,yi-1)
                             - lumaAt(xi+1,yi-1)
                             + lumaAt(xi-1,yi+1) + 2.f*lumaAt(xi,yi+1)
                             + lumaAt(xi+1,yi+1);
            const float mag = qBound(0.f,
                    std::hypot(gx, gy) * 2.f * float(mGain), 1.f);
            // opaque white paper with dark outlines premultiplied at
            // full weight, developing into the true pixel
            const float sketch = (1.f - mag) * 255.f;
            const uchar* p = srow + static_cast<size_t>(xi) * 4;
            for (int c = 0; c < 3; c++) {
                *dst++ = uchar(sketch * keep + float(p[c]) * mix);
            }
            // alpha develops from opaque paper to the true alpha
            *dst++ = uchar(255.f * keep + float(p[3]) * mix);
        }
    }
}
