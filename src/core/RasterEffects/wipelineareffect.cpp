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

#include "wipelineareffect.h"

#include "rastereffectcaller.h"
#include "transitionanchor.h"
#include "Animators/qrealanimator.h"
#include "Properties/comboboxproperty.h"
#include "appsupport.h"

WipeLinearEffect::WipeLinearEffect() :
    RasterEffect(QObject::tr("线性划像"),
                 AppSupport::getRasterEffectHardwareSupport("线性划像",
                                                            HardwareSupport::cpuOnly),
                 false,
                 RasterEffectType::TRANSITION_WIPE_LINEAR)
{
    mFadeIn = enve::make_shared<QrealAnimator>(12, 0, 600, 1, "淡入时长");
    ca_addChild(mFadeIn);

    mFadeOut = enve::make_shared<QrealAnimator>(12, 0, 600, 1, "淡出时长");
    ca_addChild(mFadeOut);

    const auto dirs = QStringList() <<
            QObject::tr("向右展开") <<
            QObject::tr("向左展开") <<
            QObject::tr("向下展开") <<
            QObject::tr("向上展开");
    mDirection = enve::make_shared<ComboBoxProperty>(QObject::tr("方向"), dirs);
    ca_addChild(mDirection);

    mSoftness = enve::make_shared<QrealAnimator>(0.08, 0, 1, 0.01, "柔边");
    ca_addChild(mSoftness);
}

class WipeLinearEffectCaller : public RasterEffectCaller {
public:
    WipeLinearEffectCaller(const HardwareSupport hwSupport,
                           const qreal openness,
                           const int direction,
                           const qreal softness) :
        RasterEffectCaller(hwSupport), mOpenness(openness),
        mDirection(direction), mSoftness(softness) {}

    void processCpu(CpuRenderTools& renderTools,
                    const CpuRenderData& data);
private:
    const qreal mOpenness;
    const int mDirection;
    const qreal mSoftness;
};

stdsptr<RasterEffectCaller> WipeLinearEffect::getEffectCaller(
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

    return enve::make_shared<WipeLinearEffectCaller>(
                instanceHwSupport(), openness,
                mDirection->getCurrentValue(),
                mSoftness->getEffectiveValue(relFrame));
}

void WipeLinearEffectCaller::processCpu(CpuRenderTools& renderTools,
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

    // sweep position along the wipe axis, in 0..1 of that axis
    auto axisPos = [w, h, this](const int xi, const int yi) {
        switch (mDirection) {
        case 0:  return (xi + 0.5) / w; // left edge first
        case 1:  return 1. - (xi + 0.5) / w; // right edge first
        case 2:  return (yi + 0.5) / h; // top edge first
        default: return 1. - (yi + 0.5) / h; // bottom edge first
        }
    };
    // softness as a fraction of the swept axis, at least one pixel
    // so the soft band never collapses into a division by zero
    const qreal axisLen = (mDirection < 2) ? w : h;
    const qreal soft = qMax(1. / qMax(1., axisLen), mSoftness);
    // leading edge of the sweep: openness 0 -> edge before the frame
    // (all hidden), openness 1 -> edge past the frame (all shown)
    const qreal edge = mOpenness * (1. + soft);

    for (int yi = yMin; yi <= yMax; yi++) {
        auto dst = static_cast<uchar*>(
                    renderTools.fDstBtmp.getAddr(0, yi - yMin));
        auto src = static_cast<uchar*>(
                    renderTools.fSrcBtmp.getAddr(xMin, yi));
        for (int xi = xMin; xi <= xMax; xi++) {
            const qreal d = edge - axisPos(xi, yi);
            const float a = float(qBound(0., d / soft, 1.));
            for (int c = 0; c < 4; c++) {
                *dst++ = uchar(*src++ * a);
            }
        }
    }
}
