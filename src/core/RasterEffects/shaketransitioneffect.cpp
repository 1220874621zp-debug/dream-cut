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

#include "shaketransitioneffect.h"

#include "rastereffectcaller.h"
#include "transitionanchor.h"
#include "Animators/qrealanimator.h"
#include "appsupport.h"

ShakeTransitionEffect::ShakeTransitionEffect() :
    RasterEffect(QObject::tr("抖动"),
                 AppSupport::getRasterEffectHardwareSupport("抖动",
                                                            HardwareSupport::cpuOnly),
                 false,
                 RasterEffectType::TRANSITION_SHAKE)
{
    mFadeIn = enve::make_shared<QrealAnimator>(12, 0, 600, 1, "淡入时长");
    ca_addChild(mFadeIn);

    mFadeOut = enve::make_shared<QrealAnimator>(12, 0, 600, 1, "淡出时长");
    ca_addChild(mFadeOut);

    mMaxShift = enve::make_shared<QrealAnimator>(30, 0, 200, 1, "最大位移");
    ca_addChild(mMaxShift);
}

namespace {
inline float shHash(const quint32 s)
{
    quint32 h = s * 2654435761u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return float(h & 0x00ffffffu) / float(0x01000000);
}
}

class ShakeTransitionEffectCaller : public RasterEffectCaller {
public:
    ShakeTransitionEffectCaller(const HardwareSupport hwSupport,
                                const qreal dx, const qreal dy,
                                const qreal alpha) :
        RasterEffectCaller(hwSupport), mDx(dx), mDy(dy), mAlpha(alpha) {}

    void processCpu(CpuRenderTools& renderTools,
                    const CpuRenderData& data);
private:
    const qreal mDx;
    const qreal mDy;
    const qreal mAlpha;
};

stdsptr<RasterEffectCaller> ShakeTransitionEffect::getEffectCaller(
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

    // per-frame random jitter, magnitude decaying with openness
    const quint32 seed = quint32(qRound(local)) * 40503u + 1u;
    const qreal k = (1. - openness) * mMaxShift->getEffectiveValue(relFrame);
    const qreal dx = (shHash(seed) - 0.5) * 2. * k;
    const qreal dy = (shHash(seed ^ 0x85ebca6bu) - 0.5) * 2. * k;

    return enve::make_shared<ShakeTransitionEffectCaller>(
                instanceHwSupport(), dx, dy, openness);
}

void ShakeTransitionEffectCaller::processCpu(
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
    const int dx = qRound(mDx);
    const int dy = qRound(mDy);

    for (int yi = yMin; yi <= yMax; yi++) {
        auto dst = static_cast<uchar*>(
                    renderTools.fDstBtmp.getAddr(0, yi - yMin));
        const int sy = yi + dy;
        const bool rowIn = sy >= 0 && sy < h;
        const auto srow = rowIn ? static_cast<const uchar*>(
                    srcBtmp.getAddr(0, sy)) : nullptr;
        for (int xi = xMin; xi <= xMax; xi++) {
            const int sx = xi + dx;
            if (!srow || sx < 0 || sx >= w) {
                dst += 4;
                continue;
            }
            const uchar* p = srow + static_cast<size_t>(sx) * 4;
            for (int c = 0; c < 4; c++) {
                *dst++ = uchar(*p++ * a);
            }
        }
    }
}
