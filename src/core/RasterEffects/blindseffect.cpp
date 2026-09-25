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

#include "blindseffect.h"

#include "rastereffectcaller.h"
#include "transitionanchor.h"
#include "Animators/qrealanimator.h"
#include "Properties/comboboxproperty.h"
#include "appsupport.h"

BlindsEffect::BlindsEffect() :
    RasterEffect(QObject::tr("百叶窗"),
                 AppSupport::getRasterEffectHardwareSupport("百叶窗",
                                                            HardwareSupport::cpuOnly),
                 false,
                 RasterEffectType::TRANSITION_BLINDS)
{
    mFadeIn = enve::make_shared<QrealAnimator>(12, 0, 600, 1, "淡入时长");
    ca_addChild(mFadeIn);

    mFadeOut = enve::make_shared<QrealAnimator>(12, 0, 600, 1, "淡出时长");
    ca_addChild(mFadeOut);

    const auto dirs = QStringList() <<
            QObject::tr("水平条纹") <<
            QObject::tr("垂直条纹");
    mDirection = enve::make_shared<ComboBoxProperty>(QObject::tr("方向"), dirs);
    ca_addChild(mDirection);

    mBars = enve::make_shared<QrealAnimator>(8, 1, 64, 1, "条纹数量");
    ca_addChild(mBars);

    mSoftness = enve::make_shared<QrealAnimator>(0.08, 0, 1, 0.01, "柔边");
    ca_addChild(mSoftness);
}

class BlindsEffectCaller : public RasterEffectCaller {
public:
    BlindsEffectCaller(const HardwareSupport hwSupport,
                       const qreal openness,
                       const int direction,
                       const int bars,
                       const qreal softness) :
        RasterEffectCaller(hwSupport), mOpenness(openness),
        mDirection(direction), mBars(bars), mSoftness(softness) {}

    void processCpu(CpuRenderTools& renderTools,
                    const CpuRenderData& data);
private:
    const qreal mOpenness;
    const int mDirection;
    const int mBars;
    const qreal mSoftness;
};

stdsptr<RasterEffectCaller> BlindsEffect::getEffectCaller(
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

    return enve::make_shared<BlindsEffectCaller>(
                instanceHwSupport(), openness,
                mDirection->getCurrentValue(),
                qMax(1, qRound(mBars->getEffectiveValue(relFrame))),
                mSoftness->getEffectiveValue(relFrame));
}

void BlindsEffectCaller::processCpu(CpuRenderTools& renderTools,
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

    const qreal axisLen = (mDirection == 0) ? h : w;
    const qreal barLen = axisLen / mBars;
    // softness as a fraction of one bar, at least one pixel so the
    // soft band never collapses into a division by zero
    const qreal soft = qMax(1. / qMax(1., barLen), mSoftness);
    const qreal edge = mOpenness * (1. + soft);

    for (int yi = yMin; yi <= yMax; yi++) {
        auto dst = static_cast<uchar*>(
                    renderTools.fDstBtmp.getAddr(0, yi - yMin));
        auto src = static_cast<uchar*>(
                    renderTools.fSrcBtmp.getAddr(xMin, yi));
        for (int xi = xMin; xi <= xMax; xi++) {
            const qreal coord = (mDirection == 0) ? yi + 0.5 : xi + 0.5;
            // position inside the current bar, 0..1
            const qreal pos = (coord - std::floor(coord / barLen) * barLen)
                    / barLen;
            const qreal d = edge - pos;
            const float a = float(qBound(0., d / soft, 1.));
            for (int c = 0; c < 4; c++) {
                *dst++ = uchar(*src++ * a);
            }
        }
    }
}
