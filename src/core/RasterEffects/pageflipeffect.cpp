/*
#
# Friction - https://friction.graphics
#
# Copyright (c) Ole-André Rodlie and contributors.
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
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

#include "pageflipeffect.h"

#include "rastereffectcaller.h"
#include "transitionanchor.h"
#include "Animators/qrealanimator.h"
#include "appsupport.h"

#include <cmath>

PageFlipEffect::PageFlipEffect() :
    RasterEffect(QObject::tr("翻页"),
                 AppSupport::getRasterEffectHardwareSupport("翻页",
                                                            HardwareSupport::cpuOnly),
                 false,
                 RasterEffectType::TRANSITION_PAGE_FLIP)
{
    mFadeIn = enve::make_shared<QrealAnimator>(12, 0, 600, 1, "淡入时长");
    ca_addChild(mFadeIn);

    mFadeOut = enve::make_shared<QrealAnimator>(12, 0, 600, 1, "淡出时长");
    ca_addChild(mFadeOut);

    mBandWidth = enve::make_shared<QrealAnimator>(0.2, 0.05, 0.5, 0.01, "卷边宽度");
    ca_addChild(mBandWidth);
}

class PageFlipEffectCaller : public RasterEffectCaller {
public:
    PageFlipEffectCaller(const HardwareSupport hwSupport,
                         const qreal curlFront,
                         const qreal bandWidth) :
        RasterEffectCaller(hwSupport), mCurl(curlFront), mBand(bandWidth) {}

    void processCpu(CpuRenderTools& renderTools,
                    const CpuRenderData& data);
private:
    // diagonal position of the curl front (0 = top-left corner,
    // 1+band = fully unrolled past the far corner)
    const qreal mCurl;
    const qreal mBand;
};

stdsptr<RasterEffectCaller> PageFlipEffect::getEffectCaller(
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

    const qreal band = mBandWidth->getEffectiveValue(relFrame);
    // travel from 0 to 1 + 1.2*band so the band fully clears the
    // far corner at full openness (identity, no shading residue)
    const qreal curl = openness * (1. + 1.2 * band);

    return enve::make_shared<PageFlipEffectCaller>(
                instanceHwSupport(), curl, band);
}

void PageFlipEffectCaller::processCpu(CpuRenderTools& renderTools,
                                      const CpuRenderData& data)
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

    const qreal diag = (w - 1) + (h - 1);
    const qreal curlBack = mCurl - mBand;

    for (int yi = yMin; yi <= yMax; yi++) {
        auto dst = static_cast<uchar*>(
                    renderTools.fDstBtmp.getAddr(0, yi - yMin));
        auto src = static_cast<uchar*>(
                    renderTools.fSrcBtmp.getAddr(xMin, yi));
        for (int xi = xMin; xi <= xMax; xi++) {
            const qreal pos = (xi + yi) / diag;
            if (pos >= mCurl) {
                // not yet laid down: transparent, the outgoing clip
                // shows through
                dst += 4;
                src += 4;
                continue;
            }
            if (pos > curlBack) {
                // curl band: the page bends up here - darken towards
                // the front then a bright rim just behind it, like a
                // cylinder catching light
                const qreal s = (pos - curlBack) / mBand; // 0..1
                const qreal shade = 1. - 0.55 * std::sin(M_PI * s);
                for (int c = 0; c < 3; c++) {
                    *dst++ = uchar(*src++ * shade);
                }
                *dst++ = *src++;
            } else {
                for (int c = 0; c < 4; c++) { *dst++ = *src++; }
            }
        }
    }
}
