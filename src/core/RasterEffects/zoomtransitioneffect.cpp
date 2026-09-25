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
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.
#
*/

#include "zoomtransitioneffect.h"

#include "rastereffectcaller.h"
#include "transitionanchor.h"
#include "Animators/qrealanimator.h"
#include "Properties/comboboxproperty.h"
#include "appsupport.h"

ZoomTransitionEffect::ZoomTransitionEffect() :
    RasterEffect(QObject::tr("缩放"),
                 AppSupport::getRasterEffectHardwareSupport("缩放",
                                                            HardwareSupport::cpuOnly),
                 false,
                 RasterEffectType::TRANSITION_ZOOM)
{
    mFadeIn = enve::make_shared<QrealAnimator>(12, 0, 600, 1, "淡入时长");
    ca_addChild(mFadeIn);

    mFadeOut = enve::make_shared<QrealAnimator>(12, 0, 600, 1, "淡出时长");
    ca_addChild(mFadeOut);

    mAmount = enve::make_shared<QrealAnimator>(1.6, 1.01, 8, 0.01, "缩放幅度");
    ca_addChild(mAmount);

    const auto modes = QStringList() <<
            QObject::tr("缩小进入") <<
            QObject::tr("放大进入");
    mMode = enve::make_shared<ComboBoxProperty>(QObject::tr("模式"), modes);
    ca_addChild(mMode);
}

class ZoomTransitionEffectCaller : public RasterEffectCaller {
public:
    ZoomTransitionEffectCaller(const HardwareSupport hwSupport,
                               const qreal scale,
                               const qreal alpha) :
        RasterEffectCaller(hwSupport), mScale(scale), mAlpha(alpha) {}

    void processCpu(CpuRenderTools& renderTools,
                    const CpuRenderData& data);
private:
    const qreal mScale;
    const qreal mAlpha;
};

stdsptr<RasterEffectCaller> ZoomTransitionEffect::getEffectCaller(
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

    const qreal amount = mAmount->getEffectiveValue(relFrame);
    // shrink-in: amount -> 1; grow-in: 1/amount -> 1
    const qreal k = 1. + (amount - 1.) * (1. - openness);
    const qreal scale = (mMode->getCurrentValue() == 0) ? k : 1. / k;

    return enve::make_shared<ZoomTransitionEffectCaller>(
                instanceHwSupport(), scale, openness);
}

void ZoomTransitionEffectCaller::processCpu(
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

    for (int yi = yMin; yi <= yMax; yi++) {
        auto dst = static_cast<uchar*>(
                    renderTools.fDstBtmp.getAddr(0, yi - yMin));
        // inverse map: destination pixel -> source pixel under scale
        const qreal sy = cy + ((yi + 0.5) - cy) / mScale - 0.5;
        const int sycl = qRound(sy);
        const bool yIn = sycl >= 0 && sycl < h;
        const auto srow = yIn ? static_cast<const uchar*>(
                       srcBtmp.getAddr(0, sycl)) : nullptr;
        for (int xi = xMin; xi <= xMax; xi++) {
            if (!srow) {
                dst += 4; // outside the source: transparent
                continue;
            }
            const qreal sx = cx + ((xi + 0.5) - cx) / mScale - 0.5;
            const int sxcl = qRound(sx);
            if (sxcl < 0 || sxcl >= w) {
                dst += 4;
                continue;
            }
            const uchar* p = srow + static_cast<size_t>(sxcl) * 4;
            for (int c = 0; c < 4; c++) {
                *dst++ = uchar(*p++ * a);
            }
        }
    }
}
