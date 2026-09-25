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

#include "mirrorflipeffect.h"

#include "rastereffectcaller.h"
#include "transitionanchor.h"
#include "Animators/qrealanimator.h"
#include "Properties/comboboxproperty.h"
#include "appsupport.h"

MirrorFlipEffect::MirrorFlipEffect() :
    RasterEffect(QObject::tr("镜像翻转"),
                 AppSupport::getRasterEffectHardwareSupport("镜像翻转",
                                                            HardwareSupport::cpuOnly),
                 false,
                 RasterEffectType::TRANSITION_MIRROR_FLIP)
{
    mFadeIn = enve::make_shared<QrealAnimator>(12, 0, 600, 1, "淡入时长");
    ca_addChild(mFadeIn);

    mFadeOut = enve::make_shared<QrealAnimator>(12, 0, 600, 1, "淡出时长");
    ca_addChild(mFadeOut);

    const auto axes = QStringList() <<
            QObject::tr("垂直中轴") <<
            QObject::tr("水平中轴");
    mAxis = enve::make_shared<ComboBoxProperty>(QObject::tr("翻转轴"), axes);
    ca_addChild(mAxis);
}

class MirrorFlipEffectCaller : public RasterEffectCaller {
public:
    MirrorFlipEffectCaller(const HardwareSupport hwSupport,
                           const qreal scale, const int axis,
                           const qreal alpha) :
        RasterEffectCaller(hwSupport), mScale(scale), mAxis(axis),
        mAlpha(alpha) {}

    void processCpu(CpuRenderTools& renderTools,
                    const CpuRenderData& data);
private:
    // signed scale along the flip axis: -1 fully mirrored, 0 flat,
    // 1 normal orientation
    const qreal mScale;
    const int mAxis;
    const qreal mAlpha;
};

stdsptr<RasterEffectCaller> MirrorFlipEffect::getEffectCaller(
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

    // -1 (mirrored) -> +1 (normal); clamped away from zero so the
    // halfway flat state cannot divide by zero
    const qreal s = qBound(-1., 2. * openness - 1., 1.);
    const qreal scale = qFuzzyIsNull(s) ? (openness < 0.5 ? -0.02 : 0.02) : s;

    return enve::make_shared<MirrorFlipEffectCaller>(
                instanceHwSupport(), scale,
                mAxis->getCurrentValue(), openness);
}

void MirrorFlipEffectCaller::processCpu(
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
        const qreal dy = (yi + 0.5) - cy;
        // source row under the flip (vertical axis flips columns)
        const qreal sy = (mAxis == 0) ? (yi + 0.5) : cy + dy / mScale;
        const int sycl = qRound(sy - 0.5);
        const bool rowIn = sycl >= 0 && sycl < h;
        const auto srow = rowIn ? static_cast<const uchar*>(
                    srcBtmp.getAddr(0, sycl)) : nullptr;
        for (int xi = xMin; xi <= xMax; xi++) {
            if (!srow) {
                dst += 4;
                continue;
            }
            const qreal dx = (xi + 0.5) - cx;
            const qreal sx = (mAxis == 0) ? cx + dx / mScale : (xi + 0.5);
            const int sxcl = qRound(sx - 0.5);
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
