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

#include "glitchtransitioneffect.h"

#include "rastereffectcaller.h"
#include "transitionanchor.h"
#include "Animators/qrealanimator.h"
#include "appsupport.h"

GlitchTransitionEffect::GlitchTransitionEffect() :
    RasterEffect(QObject::tr("故障"),
                 AppSupport::getRasterEffectHardwareSupport("故障",
                                                            HardwareSupport::cpuOnly),
                 false,
                 RasterEffectType::TRANSITION_GLITCH)
{
    mFadeIn = enve::make_shared<QrealAnimator>(12, 0, 600, 1, "淡入时长");
    ca_addChild(mFadeIn);

    mFadeOut = enve::make_shared<QrealAnimator>(12, 0, 600, 1, "淡出时长");
    ca_addChild(mFadeOut);

    mStrength = enve::make_shared<QrealAnimator>(0.7, 0, 1, 0.01, "强度");
    ca_addChild(mStrength);
}

namespace {
inline float gh(const quint32 a, const quint32 b)
{
    quint32 h = a * 374761393u + b * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return float(h & 0x00ffffffu) / float(0x01000000);
}
}

class GlitchTransitionEffectCaller : public RasterEffectCaller {
public:
    GlitchTransitionEffectCaller(const HardwareSupport hwSupport,
                                 const qreal decay,
                                 const qreal strength,
                                 const quint32 seed) :
        RasterEffectCaller(hwSupport), mDecay(decay),
        mStrength(strength), mSeed(seed) {}

    void processCpu(CpuRenderTools& renderTools,
                    const CpuRenderData& data);
private:
    const qreal mDecay;
    const qreal mStrength;
    const quint32 mSeed;
};

stdsptr<RasterEffectCaller> GlitchTransitionEffect::getEffectCaller(
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

    // per-frame seed so the tearing jumps every frame like real
    // signal glitching (frame-local coordinate, stable in preview)
    const quint32 seed = quint32(qRound(local)) * 2654435761u;

    return enve::make_shared<GlitchTransitionEffectCaller>(
                instanceHwSupport(), (1. - openness),
                mStrength->getEffectiveValue(relFrame), seed);
}

void GlitchTransitionEffectCaller::processCpu(
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

    // all glitch amounts scale with decay: zero at full openness
    const float a = float(1. - mDecay);
    const qreal k = mDecay * mStrength;
    if (k < 0.001) {
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

    const int rowShiftPx = int(48. * k);
    const int chSplitPx = int(10. * k);
    const float flash = float(0.5 * k);

    for (int yi = yMin; yi <= yMax; yi++) {
        auto dst = static_cast<uchar*>(
                    renderTools.fDstBtmp.getAddr(0, yi - yMin));
        const auto srow = static_cast<const uchar*>(
                    srcBtmp.getAddr(0, yi));
        // row tear: whole scanline slides sideways
        const int shift = int((gh(quint32(yi), mSeed) - 0.5f)
                              * 2.f * rowShiftPx);
        // flashing block: some 8px-tall bands get brightened
        const bool flashRow = gh(quint32(yi >> 3), mSeed ^ 0x9e3779b9u)
                              < 0.12f;
        const float bright = flashRow ? 1.f + flash : 1.f;
        for (int xi = xMin; xi <= xMax; xi++) {
            const int xr = qBound(0, xi + shift - chSplitPx, w - 1);
            const int xg = qBound(0, xi + shift, w - 1);
            const int xb = qBound(0, xi + shift + chSplitPx, w - 1);
            // platform N32 little endian = BGRA byte order
            const uchar b = uchar(srow[static_cast<size_t>(xb) * 4 + 0] * bright);
            const uchar g = uchar(srow[static_cast<size_t>(xg) * 4 + 1] * bright);
            const uchar r = uchar(srow[static_cast<size_t>(xr) * 4 + 2] * bright);
            const uchar al = srow[static_cast<size_t>(xg) * 4 + 3];
            *dst++ = uchar(b * a);
            *dst++ = uchar(g * a);
            *dst++ = uchar(r * a);
            *dst++ = uchar(al * a);
        }
    }
}
