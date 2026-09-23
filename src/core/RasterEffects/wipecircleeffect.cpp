/*
#
# Friction - https://friction.graphics
#
# Copyright (c) Ole-André Rodlie and contributors.
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License as
# published by the Free Software Foundation, either version 3 of
# the License, or (at your option) any later version.
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

#include "wipecircleeffect.h"

#include "rastereffectcaller.h"
#include "transitionanchor.h"
#include "Animators/qrealanimator.h"
#include "appsupport.h"

WipeCircleEffect::WipeCircleEffect() :
    RasterEffect(QObject::tr("圆形划像"),
                 AppSupport::getRasterEffectHardwareSupport("圆形划像",
                                                            HardwareSupport::cpuOnly),
                 false,
                 RasterEffectType::TRANSITION_WIPE_CIRCLE)
{
    mFadeIn = enve::make_shared<QrealAnimator>(12, 0, 600, 1, "淡入时长");
    ca_addChild(mFadeIn);

    mFadeOut = enve::make_shared<QrealAnimator>(12, 0, 600, 1, "淡出时长");
    ca_addChild(mFadeOut);

    mSoftness = enve::make_shared<QrealAnimator>(0.08, 0, 1, 0.01, "柔边");
    ca_addChild(mSoftness);
}

class WipeCircleEffectCaller : public RasterEffectCaller {
public:
    WipeCircleEffectCaller(const HardwareSupport hwSupport,
                           const qreal radius,
                           const qreal softness) :
        RasterEffectCaller(hwSupport), mRadius(radius), mSoft(softness) {}

    void processCpu(CpuRenderTools& renderTools,
                    const CpuRenderData& data);
private:
    // radius as a fraction of the half diagonal (1 = fully open),
    // softness as a fraction of the half diagonal edge band
    const qreal mRadius;
    const qreal mSoft;
};

stdsptr<RasterEffectCaller> WipeCircleEffect::getEffectCaller(
        const qreal relFrame, const qreal resolution,
        const qreal influence, BoxRenderData * const data) const
{
    Q_UNUSED(resolution)

    const qreal fadeIn = mFadeIn->getEffectiveValue(relFrame);
    const qreal fadeOut = mFadeOut->getEffectiveValue(relFrame);
    const qreal total = nleTransitionTotalFrames(data);
    const qreal openness = nleTransitionOpenness(relFrame, total,
                                                 fadeIn, fadeOut) * influence;
    const qreal softness = mSoftness->getEffectiveValue(relFrame);

    return enve::make_shared<WipeCircleEffectCaller>(
                instanceHwSupport(), openness, softness);
}

void WipeCircleEffectCaller::processCpu(CpuRenderTools& renderTools,
                                        const CpuRenderData& data)
{
    // fully open must still copy src into dst: the pipeline hands the
    // caller an uninitialized dst bitmap and replaces the rendered
    // image with it, so an early return here draws garbage/black
    const auto& srcBtmp = renderTools.fSrcBtmp;
    const auto& dstBtmp = renderTools.fDstBtmp;

    if (srcBtmp.empty() || srcBtmp.getPixels() == nullptr ||
        dstBtmp.empty() || dstBtmp.getPixels() == nullptr) { return; }

    const qreal w = srcBtmp.width();
    const qreal h = srcBtmp.height();

    const int xMin = std::max(0, data.fTexTile.left());
    const int xMax = std::min((int)data.fTexTile.right(),
                              (int)srcBtmp.width() - 1);
    const int yMin = std::max(0, data.fTexTile.top());
    const int yMax = std::min((int)data.fTexTile.bottom(),
                              (int)srcBtmp.height() - 1);

    const qreal halfDiag = 0.5 * std::hypot(w, h);
    // radius 1 must cover the farthest corner: scale so openness = 1
    // hides nothing even after rounding
    const qreal r = mRadius * halfDiag + 0.5;
    const qreal soft = qMax(1., mSoft * halfDiag);
    const qreal r0 = r - soft;

    for (int yi = yMin; yi <= yMax; yi++) {
        auto dst = static_cast<uchar*>(
                    renderTools.fDstBtmp.getAddr(0, yi - yMin));
        auto src = static_cast<uchar*>(
                    renderTools.fSrcBtmp.getAddr(xMin, yi));
        const qreal dy = (yi + 0.5) - h / 2.;
        for (int xi = xMin; xi <= xMax; xi++) {
            const qreal dx = (xi + 0.5) - w / 2.;
            const qreal dist = std::hypot(dx, dy);
            float alpha;
            if (dist <= r0) {
                alpha = 1.f;
            } else if (dist >= r) {
                alpha = 0.f;
            } else {
                alpha = float(1. - (dist - r0) / soft);
            }
            for (int c = 0; c < 4; c++) {
                *dst++ = uchar(*src++ * alpha);
            }
        }
    }
}
