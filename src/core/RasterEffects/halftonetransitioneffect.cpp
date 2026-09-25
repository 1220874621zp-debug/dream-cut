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

#include "halftonetransitioneffect.h"

#include "rastereffectcaller.h"
#include "transitionanchor.h"
#include "Animators/qrealanimator.h"
#include "appsupport.h"

#include <cmath>

HalftoneTransitionEffect::HalftoneTransitionEffect() :
    RasterEffect(QObject::tr("半调网点"),
                 AppSupport::getRasterEffectHardwareSupport("半调网点",
                                                            HardwareSupport::cpuOnly),
                 false,
                 RasterEffectType::TRANSITION_HALFTONE)
{
    mFadeIn = enve::make_shared<QrealAnimator>(12, 0, 600, 1, "淡入时长");
    ca_addChild(mFadeIn);

    mFadeOut = enve::make_shared<QrealAnimator>(12, 0, 600, 1, "淡出时长");
    ca_addChild(mFadeOut);

    mMaxCell = enve::make_shared<QrealAnimator>(8, 2, 32, 1, "网格大小");
    ca_addChild(mMaxCell);
}

class HalftoneTransitionEffectCaller : public RasterEffectCaller {
public:
    HalftoneTransitionEffectCaller(const HardwareSupport hwSupport,
                                   const int cell,
                                   const qreal mix,
                                   const qreal alpha) :
        RasterEffectCaller(hwSupport), mCell(cell), mMix(mix),
        mAlpha(alpha) {}

    void processCpu(CpuRenderTools& renderTools,
                    const CpuRenderData& data);
private:
    const int mCell;
    const qreal mMix;
    const qreal mAlpha;
};

stdsptr<RasterEffectCaller> HalftoneTransitionEffect::getEffectCaller(
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

    // a fixed-grid halftone print of the frame develops into the
    // true image as the clip opens up (a "developing print", not a
    // transparent fade: the head frame shows the opaque print, the
    // outgoing clip is covered immediately)
    return enve::make_shared<HalftoneTransitionEffectCaller>(
                instanceHwSupport(),
                qMax(2, qRound(mMaxCell->getEffectiveValue(relFrame))),
                openness, openness);
}

void HalftoneTransitionEffectCaller::processCpu(
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
    const float keep = 1.f - mix; // print weight

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

    const qreal half = mCell / 2.;

    for (int yi = yMin; yi <= yMax; yi++) {
        auto dst = static_cast<uchar*>(
                    renderTools.fDstBtmp.getAddr(0, yi - yMin));
        // grid cell centre sampling: one luma value drives the dot
        const int sy = qBound(0, static_cast<int>(
                    (std::floor(yi / qreal(mCell)) + 0.5) *
                    mCell), h - 1);
        const auto srow = static_cast<const uchar*>(
                    srcBtmp.getAddr(0, sy));
        for (int xi = xMin; xi <= xMax; xi++) {
            const int sx = qBound(0, static_cast<int>(
                        (std::floor(xi / qreal(mCell)) + 0.5) *
                        mCell), w - 1);
            const uchar* p = srow + static_cast<size_t>(sx) * 4;
            const int al = p[3];
            // unpremultiply the luma; empty pixels read as white
            qreal lum = 1.;
            if (al > 0) {
                const qreal inv = 1. / (al * 255.);
                lum = qBound(0., (0.30 * p[2] + 0.59 * p[1] +
                                  0.11 * p[0]) * inv, 1.);
            }
            // dot radius shrinks with brightness (dark = big dot)
            const qreal dx = std::fmod(xi + 0.5, mCell) - half;
            const qreal dy = std::fmod(yi + 0.5, mCell) - half;
            const bool inDot = std::hypot(dx, dy) < half * (1. - lum);
            // opaque white paper / black dot premultiplied at full
            // weight, developed into the true premultiplied pixel
            const float ht = inDot ? 0.f : 255.f;
            for (int c = 0; c < 3; c++) {
                *dst++ = uchar(ht * keep + float(p[c]) * mix);
            }
            // alpha develops from opaque paper to the true alpha
            *dst++ = uchar(255.f * keep + float(al) * mix);
        }
    }
}
