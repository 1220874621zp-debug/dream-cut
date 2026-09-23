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

#include "slideeffect.h"

#include "rastereffectcaller.h"
#include "transitionanchor.h"
#include "Animators/qrealanimator.h"
#include "Properties/comboboxproperty.h"
#include "appsupport.h"

SlideEffect::SlideEffect() :
    RasterEffect(QObject::tr("滑动"),
                 AppSupport::getRasterEffectHardwareSupport("滑动",
                                                            HardwareSupport::cpuOnly),
                 false,
                 RasterEffectType::TRANSITION_SLIDE)
{
    mSlideIn = enve::make_shared<QrealAnimator>(12, 0, 600, 1, "滑入时长");
    ca_addChild(mSlideIn);

    mSlideOut = enve::make_shared<QrealAnimator>(12, 0, 600, 1, "滑出时长");
    ca_addChild(mSlideOut);

    const auto dirs = QStringList() <<
            QObject::tr("从右侧") <<
            QObject::tr("从左侧") <<
            QObject::tr("从下方") <<
            QObject::tr("从上方");
    mDirection = enve::make_shared<ComboBoxProperty>(QObject::tr("方向"), dirs);
    ca_addChild(mDirection);
}

class SlideEffectCaller : public RasterEffectCaller {
public:
    SlideEffectCaller(const HardwareSupport hwSupport,
                      const qreal dx, const qreal dy) :
        RasterEffectCaller(hwSupport), mDx(dx), mDy(dy) {}

    void processCpu(CpuRenderTools& renderTools,
                    const CpuRenderData& data);
private:
    // relative pixel offsets (-1..1 of width/height): the head window
    // slides in along the direction, the tail window slides on out
    const qreal mDx;
    const qreal mDy;
};

stdsptr<RasterEffectCaller> SlideEffect::getEffectCaller(
        const qreal relFrame, const qreal resolution,
        const qreal influence, BoxRenderData * const data) const
{
    Q_UNUSED(resolution)

    const qreal slideIn = mSlideIn->getEffectiveValue(relFrame);
    const qreal slideOut = mSlideOut->getEffectiveValue(relFrame);
    const qreal total = nleTransitionTotalFrames(data);

    // head window: openness 0 -> 1 (offset collapses to 0)
    const qreal headOpen = slideIn > 0.5 ?
                qBound(0., relFrame / slideIn, 1.) : 1.;
    // tail window: 1 -> 0 (offset grows out along -direction)
    const qreal tailOpen = slideOut > 0.5 ?
                qBound(0., (total - relFrame) / slideOut, 1.) : 1.;

    const qreal k = (1. - headOpen) * influence - (1. - tailOpen);

    qreal dx = 0., dy = 0.;
    switch (mDirection->getCurrentValue()) {
    case 0:  dx = k;  break; // from the right edge
    case 1:  dx = -k; break; // from the left edge
    case 2:  dy = k;  break; // from the bottom
    default: dy = -k; break; // from the top
    }

    return enve::make_shared<SlideEffectCaller>(instanceHwSupport(),
                                                dx, dy);
}

void SlideEffectCaller::processCpu(CpuRenderTools& renderTools,
                                   const CpuRenderData& data)
{
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

    if (qAbs(mDx) < 0.0001 && qAbs(mDy) < 0.0001) { return; }

    const int dx = qRound(mDx * w);
    const int dy = qRound(mDy * h);

    for (int yi = yMin; yi <= yMax; yi++) {
        auto dst = static_cast<uchar*>(
                    renderTools.fDstBtmp.getAddr(0, yi - yMin));
        const int sy = yi + dy;
        const bool rowIn = sy >= 0 && sy < h;
        auto src = rowIn ? static_cast<uchar*>(
                     renderTools.fSrcBtmp.getAddr(xMin, sy)) : nullptr;
        for (int xi = xMin; xi <= xMax; xi++) {
            const int sx = xi + dx;
            if (src && sx >= 0 && sx < w) {
                const uchar* s = src + 4 * (sx - xMin);
                for (int c = 0; c < 4; c++) { *dst++ = *s++; }
            } else {
                for (int c = 0; c < 4; c++) { *dst++ = 0; }
            }
        }
    }
}
