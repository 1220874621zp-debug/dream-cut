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

#include "directionalblurtransitioneffect.h"

#include "rastereffectcaller.h"
#include "transitionanchor.h"
#include "Animators/qrealanimator.h"
#include "Properties/comboboxproperty.h"
#include "appsupport.h"

#include <cmath>

DirectionalBlurTransitionEffect::DirectionalBlurTransitionEffect() :
    RasterEffect(QObject::tr("方向模糊"),
                 AppSupport::getRasterEffectHardwareSupport("方向模糊",
                                                            HardwareSupport::cpuOnly),
                 false,
                 RasterEffectType::TRANSITION_DIR_BLUR)
{
    mFadeIn = enve::make_shared<QrealAnimator>(12, 0, 600, 1, "淡入时长");
    ca_addChild(mFadeIn);

    mFadeOut = enve::make_shared<QrealAnimator>(12, 0, 600, 1, "淡出时长");
    ca_addChild(mFadeOut);

    mMaxDistance = enve::make_shared<QrealAnimator>(40, 0, 160, 1, "最大距离");
    ca_addChild(mMaxDistance);

    const auto dirs = QStringList() <<
            QObject::tr("水平") <<
            QObject::tr("垂直") <<
            QObject::tr("对角");
    mDirection = enve::make_shared<ComboBoxProperty>(QObject::tr("方向"), dirs);
    ca_addChild(mDirection);
}

class DirBlurTransitionEffectCaller : public RasterEffectCaller {
public:
    DirBlurTransitionEffectCaller(const HardwareSupport hwSupport,
                                  const qreal dx, const qreal dy,
                                  const qreal alpha) :
        RasterEffectCaller(hwSupport), mDx(dx), mDy(dy), mAlpha(alpha) {}

    void processCpu(CpuRenderTools& renderTools,
                    const CpuRenderData& data);
private:
    // half-length of the blur streak along the chosen axis
    const qreal mDx;
    const qreal mDy;
    const qreal mAlpha;
};

stdsptr<RasterEffectCaller> DirectionalBlurTransitionEffect::getEffectCaller(
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

    const qreal d = (1. - openness) *
            mMaxDistance->getEffectiveValue(relFrame);
    qreal dx = d, dy = 0.;
    switch (mDirection->getCurrentValue()) {
    case 1:  dx = 0.; dy = d; break;
    case 2:  dx = d * M_SQRT1_2; dy = d * M_SQRT1_2; break;
    default: break;
    }

    return enve::make_shared<DirBlurTransitionEffectCaller>(
                instanceHwSupport(), dx, dy, openness);
}

void DirBlurTransitionEffectCaller::processCpu(
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
    const int taps = 9;

    if (std::hypot(mDx, mDy) < 0.5) {
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
        for (int xi = xMin; xi <= xMax; xi++) {
            int acc[4] = { 0, 0, 0, 0 };
            const int used = taps;
            for (int t = 0; t < taps; t++) {
                const qreal f = 2. * t / (taps - 1) - 1.;
                const int sx = qBound(0, qRound(xi + mDx * f), w - 1);
                const int sy = qBound(0, qRound(yi + mDy * f), h - 1);
                const auto srow = static_cast<const uchar*>(
                            srcBtmp.getAddr(0, sy));
                const uchar* p = srow + static_cast<size_t>(sx) * 4;
                for (int c = 0; c < 4; c++) { acc[c] += p[c]; }
            }
            for (int c = 0; c < 4; c++) {
                *dst++ = uchar(acc[c] / used * a);
            }
        }
    }
}
