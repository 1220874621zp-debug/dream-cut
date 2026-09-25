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

#include "dreamyzoomtransitioneffect.h"

#include "rastereffectcaller.h"
#include "transitionanchor.h"
#include "Animators/qrealanimator.h"
#include "appsupport.h"

#include <cmath>
#include <vector>

DreamyZoomTransitionEffect::DreamyZoomTransitionEffect() :
    RasterEffect(QObject::tr("梦幻变焦"),
                 AppSupport::getRasterEffectHardwareSupport("梦幻变焦",
                                                            HardwareSupport::cpuOnly),
                 false,
                 RasterEffectType::TRANSITION_DREAMY_ZOOM)
{
    mFadeIn = enve::make_shared<QrealAnimator>(12, 0, 600, 1, "淡入时长");
    ca_addChild(mFadeIn);

    mFadeOut = enve::make_shared<QrealAnimator>(12, 0, 600, 1, "淡出时长");
    ca_addChild(mFadeOut);

    mDepth = enve::make_shared<QrealAnimator>(0.18, 0, 1, 0.01, "变焦幅度");
    ca_addChild(mDepth);

    mMaxRadius = enve::make_shared<QrealAnimator>(16, 0, 60, 1, "最大模糊半径");
    ca_addChild(mMaxRadius);

    mGlow = enve::make_shared<QrealAnimator>(0.35, 0, 1, 0.01, "柔光强度");
    ca_addChild(mGlow);
}

class DreamyZoomTransitionEffectCaller : public RasterEffectCaller {
public:
    DreamyZoomTransitionEffectCaller(const HardwareSupport hwSupport,
                                     const qreal openness,
                                     const qreal depth,
                                     const int maxRadius,
                                     const qreal glow) :
        RasterEffectCaller(hwSupport), mOpenness(openness),
        mDepth(depth), mMaxRadius(maxRadius), mGlow(glow) {}

    void processCpu(CpuRenderTools& renderTools,
                    const CpuRenderData& data);
private:
    const qreal mOpenness;
    const qreal mDepth;
    const int mMaxRadius;
    const qreal mGlow;
};

stdsptr<RasterEffectCaller> DreamyZoomTransitionEffect::getEffectCaller(
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

    return enve::make_shared<DreamyZoomTransitionEffectCaller>(
                instanceHwSupport(), openness,
                mDepth->getEffectiveValue(relFrame),
                qMax(0, qRound(mMaxRadius->getEffectiveValue(relFrame))),
                mGlow->getEffectiveValue(relFrame));
}

void DreamyZoomTransitionEffectCaller::processCpu(
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

    const float a = float(mOpenness);
    const qreal punch = 1. - mOpenness;
    const qreal scale = 1. + mDepth * punch;
    const int r = int(mMaxRadius * punch);
    const float glow = float(mGlow * punch);
    const float screenK = glow * 0.8f;

    if (mOpenness >= 0.999 || (r < 1 && scale <= 1.0001 && screenK < 0.004f)) {
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

    // zoom-sampled stage: every pixel pulls from the scaled position
    // (out-of-range stays transparent so the blur smears real
    // transparency, not clamped edge streaks)
    const int tw = xMax - xMin + 1;
    // the vertical blur pass reaches r rows past the tile: sample
    // those extra rows too or the vertical pass sees clamped repeats
    // (tile seam, the blur-dissolve lesson)
    const int y0 = std::max(0, yMin - r);
    const int y1 = std::min(h - 1, yMax + r);
    const int extH = y1 - y0 + 1;
    std::vector<quint32> zoomed(static_cast<size_t>(tw) * extH);
    std::vector<quint32> tmp(static_cast<size_t>(tw) * extH);

    const qreal cx = w / 2.;
    const qreal cy = h / 2.;
    for (int row = 0; row < extH; row++) {
        const qreal dy = (y0 + row + 0.5) - cy;
        const int sy = qBound(0, int(cy + dy / scale), h - 1);
        const bool yIn = int(cy + dy / scale) >= 0 && int(cy + dy / scale) < h;
        for (int x = 0; x < tw; x++) {
            const qreal dx = (xMin + x + 0.5) - cx;
            const int sxi = int(cx + dx / scale);
            quint32 px = 0;
            if (yIn && sxi >= 0 && sxi < w) {
                px = *static_cast<const quint32*>(
                            srcBtmp.getAddr(sxi, sy));
            }
            zoomed[static_cast<size_t>(row) * tw + x] = px;
        }
    }

    if (r < 1) {
        // no blur: apply bloom + alpha directly from the zoom stage
        for (int yi = yMin; yi <= yMax; yi++) {
            auto dst = static_cast<uchar*>(
                        renderTools.fDstBtmp.getAddr(0, yi - yMin));
            const uchar* srow = reinterpret_cast<const uchar*>(
                        &zoomed[static_cast<size_t>(yi - y0) * tw]);
            for (int x = 0; x < tw; x++) {
                const uchar* p = srow + static_cast<size_t>(x) * 4;
                for (int c = 0; c < 4; c++) {
                    const float v = float(p[c]);
                    *dst++ = uchar((v + (255.f - v) * screenK) * a);
                }
            }
        }
        return;
    }

    // separable box blur on the zoomed stage (two passes)
    const int div = 2 * r + 1;
    for (int row = 0; row < extH; row++) {
        const uchar* srow = reinterpret_cast<const uchar*>(
                    &zoomed[static_cast<size_t>(row) * tw]);
        for (int x = 0; x < tw; x++) {
            const int sx = xMin + x;
            int acc[4] = { 0, 0, 0, 0 };
            for (int k = -r; k <= r; k++) {
                const int xx = qBound(0, sx + k - xMin, tw - 1);
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
                const float v = float(acc[c] / div);
                *dst++ = uchar((v + (255.f - v) * screenK) * a);
            }
        }
    }
}
