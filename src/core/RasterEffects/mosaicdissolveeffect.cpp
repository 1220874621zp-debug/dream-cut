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
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.
#
*/

#include "mosaicdissolveeffect.h"

#include "rastereffectcaller.h"
#include "transitionanchor.h"
#include "Animators/qrealanimator.h"
#include "appsupport.h"

#include <cmath>

MosaicDissolveEffect::MosaicDissolveEffect() :
    RasterEffect(QObject::tr("马赛克"),
                 AppSupport::getRasterEffectHardwareSupport("马赛克",
                                                            HardwareSupport::cpuOnly),
                 false,
                 RasterEffectType::TRANSITION_MOSAIC)
{
    mFadeIn = enve::make_shared<QrealAnimator>(12, 0, 600, 1, "淡入时长");
    ca_addChild(mFadeIn);

    mFadeOut = enve::make_shared<QrealAnimator>(12, 0, 600, 1, "淡出时长");
    ca_addChild(mFadeOut);

    mMaxBlock = enve::make_shared<QrealAnimator>(48, 2, 128, 1, "最大块尺寸");
    ca_addChild(mMaxBlock);
}

class MosaicDissolveEffectCaller : public RasterEffectCaller {
public:
    MosaicDissolveEffectCaller(const HardwareSupport hwSupport,
                               const qreal openness,
                               const int blockSize) :
        RasterEffectCaller(hwSupport), mAlpha(openness),
        mBlockSize(blockSize) {}

    void processCpu(CpuRenderTools& renderTools,
                    const CpuRenderData& data);
private:
    const qreal mAlpha;
    const int mBlockSize;
};

stdsptr<RasterEffectCaller> MosaicDissolveEffect::getEffectCaller(
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

    const qreal maxBlock = mMaxBlock->getEffectiveValue(relFrame);
    // block size shrinks from max to 1 (= original pixels) as the
    // clip opens up
    const int bs = qMax(1, qRound(1. + (maxBlock - 1.) * (1. - openness)));

    return enve::make_shared<MosaicDissolveEffectCaller>(
                instanceHwSupport(), openness, bs);
}

void MosaicDissolveEffectCaller::processCpu(
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

    if (mBlockSize <= 1) {
        // resolved: plain alpha copy
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

    for (int yi = yMin; yi <= yMax; yi++) {
        auto dst = static_cast<uchar*>(
                    renderTools.fDstBtmp.getAddr(0, yi - yMin));
        // sample the block centre pixel: origin-anchored grid
        const int sy = qBound(0, static_cast<int>(
                    (std::floor(yi / qreal(mBlockSize)) + 0.5) *
                    mBlockSize), h - 1);
        const auto srow = static_cast<const uchar*>(
                    srcBtmp.getAddr(0, sy));
        for (int xi = xMin; xi <= xMax; xi++) {
            const int sx = qBound(0, static_cast<int>(
                        (std::floor(xi / qreal(mBlockSize)) + 0.5) *
                        mBlockSize), w - 1);
            const uchar* p = srow + static_cast<size_t>(sx) * 4;
            for (int c = 0; c < 4; c++) {
                *dst++ = uchar(*p++ * a);
            }
        }
    }
}
