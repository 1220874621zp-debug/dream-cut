/*
#
# Friction - https://friction.graphics
#
# Copyright (c) Ole-André Rodlie and contributors.
#
# This program is free software: you can redistribute it and/or
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

#include "flashfadeeffect.h"

#include "rastereffectcaller.h"
#include "transitionanchor.h"
#include "Animators/qrealanimator.h"
#include "Animators/coloranimator.h"
#include "appsupport.h"

FlashFadeEffect::FlashFadeEffect(const QColor& bgColor) :
    RasterEffect(QObject::tr("闪黑闪白"),
                 AppSupport::getRasterEffectHardwareSupport("闪黑闪白",
                                                            HardwareSupport::cpuOnly),
                 false,
                 RasterEffectType::TRANSITION_FLASH)
{
    mFadeIn = enve::make_shared<QrealAnimator>(12, 0, 600, 1, "淡入时长");
    ca_addChild(mFadeIn);

    mFadeOut = enve::make_shared<QrealAnimator>(12, 0, 600, 1, "淡出时长");
    ca_addChild(mFadeOut);

    mColor = enve::make_shared<ColorAnimator>("背景色");
    mColor->setColor(bgColor);
    ca_addChild(mColor);
}

class FlashFadeEffectCaller : public RasterEffectCaller {
public:
    FlashFadeEffectCaller(const HardwareSupport hwSupport,
                          const qreal openness,
                          const QColor& bgColor) :
        RasterEffectCaller(hwSupport), mOpenness(openness) {
        // premultiplied background weight (src weight + bg weight = 1),
        // channels laid out in the platform N32 memory order (little
        // endian = BGRA) so the per-pixel loop can add them linearly
        const qreal w = 1. - qBound(0., openness, 1.);
        const uchar r = uchar(bgColor.red() * w);
        const uchar g = uchar(bgColor.green() * w);
        const uchar b = uchar(bgColor.blue() * w);
        const uchar a = uchar(255 * w);
#if SK_R32_SHIFT == 0 && SK_B32_SHIFT == 16
        mBg[0] = r; mBg[1] = g; mBg[2] = b; mBg[3] = a;
#else
        mBg[0] = b; mBg[1] = g; mBg[2] = r; mBg[3] = a;
#endif
    }

    void processCpu(CpuRenderTools& renderTools,
                    const CpuRenderData& data);
private:
    const qreal mOpenness;
    uchar mBg[4];
};

stdsptr<RasterEffectCaller> FlashFadeEffect::getEffectCaller(
        const qreal relFrame, const qreal resolution,
        const qreal influence, BoxRenderData * const data) const
{
    Q_UNUSED(resolution)

    const qreal fadeIn = mFadeIn->getEffectiveValue(relFrame);
    const qreal fadeOut = mFadeOut->getEffectiveValue(relFrame);
    const qreal total = nleTransitionTotalFrames(data);
    const qreal openness = nleTransitionOpenness(relFrame, total,
                                                 fadeIn, fadeOut) * influence;
    const QColor bg = mColor->getColor(relFrame);

    return enve::make_shared<FlashFadeEffectCaller>(instanceHwSupport(),
                                                    openness, bg);
}

void FlashFadeEffectCaller::processCpu(CpuRenderTools& renderTools,
                                       const CpuRenderData& data)
{
    if (mOpenness >= 0.999) { return; } // fully open: passthrough

    const auto& srcBtmp = renderTools.fSrcBtmp;
    const auto& dstBtmp = renderTools.fDstBtmp;

    if (srcBtmp.empty() || srcBtmp.getPixels() == nullptr ||
        dstBtmp.empty() || dstBtmp.getPixels() == nullptr) { return; }

    const int xMin = std::max(0, data.fTexTile.left());
    const int xMax = std::min((int)data.fTexTile.right(),
                              (int)srcBtmp.width() - 1);
    const int yMin = std::max(0, data.fTexTile.top());
    const int yMax = std::min((int)data.fTexTile.bottom(),
                              (int)srcBtmp.height() - 1);

    const float a = float(qBound(0., mOpenness, 1.));
    for (int yi = yMin; yi <= yMax; yi++) {
        auto dst = static_cast<uchar*>(
                    renderTools.fDstBtmp.getAddr(0, yi - yMin));
        auto src = static_cast<uchar*>(
                    renderTools.fSrcBtmp.getAddr(xMin, yi));
        for (int xi = xMin; xi <= xMax; xi++) {
            for (int c = 0; c < 4; c++) {
                *dst++ = uchar(*src++ * a + mBg[c]);
            }
        }
    }
}
