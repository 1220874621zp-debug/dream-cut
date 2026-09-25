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

#include "spineffect.h"

#include "rastereffectcaller.h"
#include "transitionanchor.h"
#include "Animators/qrealanimator.h"
#include "Properties/comboboxproperty.h"
#include "appsupport.h"

#include <cmath>

SpinEffect::SpinEffect() :
    RasterEffect(QObject::tr("旋转"),
                 AppSupport::getRasterEffectHardwareSupport("旋转",
                                                            HardwareSupport::cpuOnly),
                 false,
                 RasterEffectType::TRANSITION_SPIN)
{
    mFadeIn = enve::make_shared<QrealAnimator>(12, 0, 600, 1, "淡入时长");
    ca_addChild(mFadeIn);

    mFadeOut = enve::make_shared<QrealAnimator>(12, 0, 600, 1, "淡出时长");
    ca_addChild(mFadeOut);

    mLaps = enve::make_shared<QrealAnimator>(0.5, 0.125, 4, 0.125, "圈数");
    ca_addChild(mLaps);

    const auto dirs = QStringList() <<
            QObject::tr("顺时针") <<
            QObject::tr("逆时针");
    mDirection = enve::make_shared<ComboBoxProperty>(QObject::tr("方向"), dirs);
    ca_addChild(mDirection);
}

class SpinEffectCaller : public RasterEffectCaller {
public:
    SpinEffectCaller(const HardwareSupport hwSupport,
                     const qreal angle, const qreal alpha) :
        RasterEffectCaller(hwSupport), mAngle(angle), mAlpha(alpha) {}

    void processCpu(CpuRenderTools& renderTools,
                    const CpuRenderData& data);
private:
    const qreal mAngle;
    const qreal mAlpha;
};

stdsptr<RasterEffectCaller> SpinEffect::getEffectCaller(
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

    const qreal laps = mLaps->getEffectiveValue(relFrame);
    const qreal dir = (mDirection->getCurrentValue() == 0) ? 1. : -1.;
    const qreal angle = dir * laps * 2. * M_PI * (1. - openness);

    return enve::make_shared<SpinEffectCaller>(
                instanceHwSupport(), angle, openness);
}

void SpinEffectCaller::processCpu(CpuRenderTools& renderTools,
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

    const float a = float(mAlpha);
    const qreal cx = w / 2.;
    const qreal cy = h / 2.;
    // inverse rotation: undo the turn to find where the pixel came from
    const qreal cosA = std::cos(-mAngle);
    const qreal sinA = std::sin(-mAngle);

    for (int yi = yMin; yi <= yMax; yi++) {
        auto dst = static_cast<uchar*>(
                    renderTools.fDstBtmp.getAddr(0, yi - yMin));
        const qreal dy = (yi + 0.5) - cy;
        for (int xi = xMin; xi <= xMax; xi++) {
            const qreal dx = (xi + 0.5) - cx;
            const qreal sx = cx + dx * cosA - dy * sinA - 0.5;
            const qreal sy = cy + dx * sinA + dy * cosA - 0.5;
            const int sxcl = qRound(sx);
            const int sycl = qRound(sy);
            if (sxcl < 0 || sxcl >= w || sycl < 0 || sycl >= h) {
                dst += 4; // outside the source: transparent
                continue;
            }
            const auto srow = static_cast<const uchar*>(
                        srcBtmp.getAddr(0, sycl));
            const uchar* p = srow + static_cast<size_t>(sxcl) * 4;
            for (int c = 0; c < 4; c++) {
                *dst++ = uchar(*p++ * a);
            }
        }
    }
}
