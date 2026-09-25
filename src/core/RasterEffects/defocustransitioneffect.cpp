/*
#
# Friction - https://friction.graphics
#
# Copyright (c) Ole-André Rodlie and contributors
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

#include "defocustransitioneffect.h"

#include "rastereffectcaller.h"
#include "transitionanchor.h"
#include "Animators/qrealanimator.h"
#include "appsupport.h"

#include <cmath>
#include <vector>

DefocusTransitionEffect::DefocusTransitionEffect() :
    RasterEffect(QObject::tr("失焦合焦"),
                 AppSupport::getRasterEffectHardwareSupport("失焦合焦",
                                                            HardwareSupport::cpuOnly),
                 false,
                 RasterEffectType::TRANSITION_DEFOCUS)
{
    mFadeIn = enve::make_shared<QrealAnimator>(12, 0, 600, 1, "淡入时长");
    ca_addChild(mFadeIn);

    mFadeOut = enve::make_shared<QrealAnimator>(12, 0, 600, 1, "淡出时长");
    ca_addChild(mFadeOut);

    mMaxRadius = enve::make_shared<QrealAnimator>(28, 0, 80, 1, "最大模糊半径");
    ca_addChild(mMaxRadius);
}

class DefocusTransitionEffectCaller : public RasterEffectCaller {
public:
    DefocusTransitionEffectCaller(const HardwareSupport hwSupport,
                                  const qreal openness,
                                  const int maxRadius) :
        RasterEffectCaller(hwSupport), mOpenness(openness),
        mMaxRadius(maxRadius) {}

    void processCpu(CpuRenderTools& renderTools,
                    const CpuRenderData& data);
private:
    const qreal mOpenness;
    const int mMaxRadius;
};

stdsptr<RasterEffectCaller> DefocusTransitionEffect::getEffectCaller(
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

    return enve::make_shared<DefocusTransitionEffectCaller>(
                instanceHwSupport(), openness,
                qMax(0, qRound(mMaxRadius->getEffectiveValue(relFrame))));
}

void DefocusTransitionEffectCaller::processCpu(
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

    // focus racks in with the openness; a quick alpha ramp (full at
    // 2/3 open) keeps the reveal from reading as a hard splice
    const float a = float(qMin(1., mOpenness * 1.5));
    const int r = int((1. - mOpenness) * mMaxRadius);

    if (r < 1) {
        for (int yi = yMin; yi <= yMax; yi++) {
            auto dst = static_cast<uchar*>(
                        renderTools.fDstBtmp.getAddr(0, yi - yMin));
            auto src = static_cast<uchar*>(
                        renderTools.fSrcBtmp.getAddr(xMin, yi));
            for (int xi = xMin; xi <= xMax; xi++) {
                for (int c = 0; c < 4; c++) {
                    *dst++ = uchar(*src++ * a);
                }
            }
        }
        return;
    }

    const int tw = xMax - xMin + 1;
    // vertical taps reach r rows past the tile: the horizontal pass
    // must cover those rows so the vertical pass sees real blurred
    // pixels instead of clamped tile-edge repeats (visible seam
    // between tiles otherwise)
    const int y0 = std::max(0, yMin - r);
    const int y1 = std::min(h - 1, yMax + r);
    const int extH = y1 - y0 + 1;
    std::vector<quint32> tmp(static_cast<size_t>(tw) * extH);

    // horizontal pass: full-image src rows -> tmp
    const int div = 2 * r + 1;
    for (int row = 0; row < extH; row++) {
        const uchar* srow = static_cast<const uchar*>(
                    srcBtmp.getAddr(0, y0 + row));
        for (int x = 0; x < tw; x++) {
            const int sx = xMin + x;
            int acc[4] = { 0, 0, 0, 0 };
            for (int k = -r; k <= r; k++) {
                const int xx = qBound(0, sx + k, w - 1);
                const uchar* p = srow + static_cast<size_t>(xx) * 4;
                for (int c = 0; c < 4; c++) { acc[c] += p[c]; }
            }
            quint32 px = 0;
            for (int c = 0; c < 4; c++) {
                px |= quint32(acc[c] / div) << (8 * c);
            }
            tmp[static_cast<size_t>(row) * tw + x] = px;
        }
    }

    // vertical pass: tmp -> dst with the transition alpha
    for (int yi = yMin; yi <= yMax; yi++) {
        auto dst = static_cast<uchar*>(
                    renderTools.fDstBtmp.getAddr(0, yi - yMin));
        for (int x = 0; x < tw; x++) {
            int acc[4] = { 0, 0, 0, 0 };
            for (int k = -r; k <= r; k++) {
                const int row = qBound(0, (yi + k) - y0, extH - 1);
                const quint32 px = tmp[static_cast<size_t>(row) * tw + x];
                for (int c = 0; c < 4; c++) {
                    acc[c] += int((px >> (8 * c)) & 0xff);
                }
            }
            for (int c = 0; c < 4; c++) {
                *dst++ = uchar(acc[c] / div * a);
            }
        }
    }
}
