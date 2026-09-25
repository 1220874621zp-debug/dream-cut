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

#include "channelsplittransitioneffect.h"

#include "rastereffectcaller.h"
#include "transitionanchor.h"
#include "Animators/qrealanimator.h"
#include "appsupport.h"

ChannelSplitTransitionEffect::ChannelSplitTransitionEffect() :
    RasterEffect(QObject::tr("通道分离"),
                 AppSupport::getRasterEffectHardwareSupport("通道分离",
                                                            HardwareSupport::cpuOnly),
                 false,
                 RasterEffectType::TRANSITION_CHANNEL_SPLIT)
{
    mFadeIn = enve::make_shared<QrealAnimator>(12, 0, 600, 1, "淡入时长");
    ca_addChild(mFadeIn);

    mFadeOut = enve::make_shared<QrealAnimator>(12, 0, 600, 1, "淡出时长");
    ca_addChild(mFadeOut);

    mMaxSplit = enve::make_shared<QrealAnimator>(14, 0, 80, 1, "最大分离");
    ca_addChild(mMaxSplit);
}

class ChannelSplitTransitionEffectCaller : public RasterEffectCaller {
public:
    ChannelSplitTransitionEffectCaller(const HardwareSupport hwSupport,
                                       const int split,
                                       const qreal alpha) :
        RasterEffectCaller(hwSupport), mSplit(split), mAlpha(alpha) {}

    void processCpu(CpuRenderTools& renderTools,
                    const CpuRenderData& data);
private:
    const int mSplit;
    const qreal mAlpha;
};

stdsptr<RasterEffectCaller> ChannelSplitTransitionEffect::getEffectCaller(
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

    return enve::make_shared<ChannelSplitTransitionEffectCaller>(
                instanceHwSupport(),
                qRound((1. - openness) *
                       mMaxSplit->getEffectiveValue(relFrame)),
                openness);
}

void ChannelSplitTransitionEffectCaller::processCpu(
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

    const int xMin = std::max(0, data.fTexTile.left());
    const int xMax = std::min((int)data.fTexTile.right(), w - 1);
    const int yMin = std::max(0, data.fTexTile.top());
    const int yMax = std::min((int)data.fTexTile.bottom(),
                              (int)srcBtmp.height() - 1);

    const float a = float(mAlpha);

    if (mSplit < 1) {
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
        const auto srow = static_cast<const uchar*>(
                    srcBtmp.getAddr(0, yi));
        for (int xi = xMin; xi <= xMax; xi++) {
            const int xr = qBound(0, xi - mSplit, w - 1);
            const int xb = qBound(0, xi + mSplit, w - 1);
            // platform N32 little endian = BGRA byte order
            *dst++ = uchar(srow[static_cast<size_t>(xb) * 4 + 0] * a);
            *dst++ = uchar(srow[static_cast<size_t>(xi) * 4 + 1] * a);
            *dst++ = uchar(srow[static_cast<size_t>(xr) * 4 + 2] * a);
            *dst++ = uchar(srow[static_cast<size_t>(xi) * 4 + 3] * a);
        }
    }
}
