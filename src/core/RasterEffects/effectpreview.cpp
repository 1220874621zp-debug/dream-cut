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
# See 'README.md' for more information.
#
*/

#include "effectpreview.h"

#include "RasterEffects/rastereffectcollection.h"
#include "RasterEffects/rastereffectsinclude.h"
#include "RasterEffects/rastereffect.h"
#include "RasterEffects/rastereffectcaller.h"
#include "Animators/complexanimator.h"
#include "Animators/qrealanimator.h"
#include "Properties/comboboxproperty.h"
#include "glhelpers.h"
#include "cpurendertools.h"
#include "skia/skiaincludes.h"
#include "effectpreviewlogo.h"

#include <QtMath>
#include <QPainter>
#include <QPolygonF>
#include <cstring>

namespace {

// preview loop length: two seconds of scene time per cycle
constexpr qreal gPreviewLoopSec = 2.0;
constexpr qreal gPreviewFps = 24.;

enum class Sample {
    text,      // the embedded user-supplied image on a transparent rim
    green,     // chroma-green backdrop + the image as foreground
    liquid,    // fisheye-lens "liquid glass" look on the image
    lattice    // the image with a cyan lattice grid (warp readout)
};

// which demo content shows the effect best
Sample sampleFor(const RasterEffectType type) {
    switch (type) {
    case RasterEffectType::CHROMA_KEY:
        return Sample::green;
    case RasterEffectType::LIQUID_GLASS:
        return Sample::liquid;
    case RasterEffectType::LATTICE_WARP:
        return Sample::lattice;
    default:
        return Sample::text;
    }
}

// this skia tree's SkRect has no center(); not needed anymore since
// the glyph samples were replaced by the embedded wordmark

// draws the embedded user-supplied sample image aspect-fitted
// (contain) into the demo sample, centered
void drawWordmark(SkCanvas& c, SkPaint& p,
                  const int w, const int h, const qreal cyFactor) {
    static const QImage logo = QImage::fromData(
                gFrictionLogoPng, int(gFrictionLogoPngSize), "PNG");
    if (logo.isNull()) { return; }
    const qreal s = qMin(w * 0.84 / logo.width(),
                         h * 0.84 / logo.height());
    const int lw = qRound(logo.width() * s);
    const int lh = qRound(logo.height() * s);
    const QImage scaled = logo.scaled(lw, lh,
                                      Qt::IgnoreAspectRatio,
                                      Qt::SmoothTransformation);
    const auto info = SkImageInfo::Make(
                scaled.width(), scaled.height(),
                kN32_SkColorType, kPremul_SkAlphaType);
    const auto skLogo = SkImage::MakeFromRaster(
                SkPixmap(info, scaled.constBits(),
                         static_cast<size_t>(scaled.bytesPerLine())),
                nullptr, nullptr);
    if (!skLogo) { return; }
    const SkRect dst = SkRect::MakeXYWH(
                (w - lw) / 2.f, h * cyFactor - lh / 2.f, lw, lh);
    c.drawImageRect(skLogo,
                    SkRect::MakeWH(scaled.width(), scaled.height()),
                    dst, &p, SkCanvas::kStrict_SrcRectConstraint);
}

SkBitmap makeTextSample(const int w, const int h) {
    SkBitmap bmp;
    bmp.allocN32Pixels(w, h);
    bmp.eraseARGB(0, 0, 0, 0);
    SkCanvas c(bmp);
    SkPaint p;
    p.setAntiAlias(true);

    // transparent canvas + the image centered at 84%: the transparent
    // rim lets outside-the-silhouette output (shadows, glows, blur
    // spill, frayed edges) stay visible instead of being covered by
    // an opaque backdrop
    drawWordmark(c, p, w, h, 0.5);
    return bmp;
}

SkBitmap makeGreenSample(const int w, const int h) {
    SkBitmap bmp;
    bmp.allocN32Pixels(w, h);
    SkCanvas c(bmp);
    SkPaint p;
    p.setAntiAlias(true);

    p.setColor(SkColorSetRGB(0, 177, 64));
    c.drawRect(SkRect::MakeWH(w, h), p);

    // foreground: the friction wordmark survives the key
    drawWordmark(c, p, w, h, 0.5);
    return bmp;
}

// the embedded user-supplied sample image, aspect-fitted (contain)
// into a dark-gradient canvas, with a roaming fisheye lens and rim
// highlight painted over it: the "liquid glass" look (the real effect
// samples the canvas below the layer, which a single-layer preview
// cannot express, so the sample itself carries the look and the lens
// roams slowly to keep the tile alive)
SkBitmap makeLiquidSample(const int w, const int h, const qreal phase) {
    const SkBitmap base = makeTextSample(w, h);
    SkBitmap out;
    out.allocN32Pixels(w, h);
    const qreal cx = w * (0.5 + 0.07 * std::cos(2. * M_PI * phase));
    const qreal cy = h * (0.5 + 0.07 * std::sin(2. * M_PI * phase));
    const qreal R = w * 0.30;
    uint32_t* const dstRow =
            static_cast<uint32_t*>(out.getPixels());
    const uint32_t* const srcPixels =
            static_cast<const uint32_t*>(base.getPixels());
    for (int y = 0; y < h; y++) {
        uint32_t* const dr = dstRow + y * w;
        for (int x = 0; x < w; x++) {
            const qreal dx = x - cx;
            const qreal dy = y - cy;
            const qreal d = std::sqrt(dx * dx + dy * dy);
            if (d < R && R > 0.0001) {
                const qreal r = d / R;
                // magnifying lens: center enlarged, rim undistorted
                const qreal f = 1. - 0.38 * (1. - r * r);
                int sx = qRound(cx + dx * f);
                int sy = qRound(cy + dy * f);
                sx = qBound(0, sx, w - 1);
                sy = qBound(0, sy, h - 1);
                uint32_t px = srcPixels[sy * w + sx];
                // rim highlight ring near the lens edge
                if (r > 0.84) {
                    const qreal k = (r - 0.84) / 0.16;
                    const int a = qRound(150 * k * (1. - k) * 4.);
                    const int r8 = SkColorGetR(px) + a;
                    const int g8 = SkColorGetG(px) + a;
                    const int b8 = SkColorGetB(px) + a;
                    px = SkColorSetARGB(SkColorGetA(px),
                                        qMin(r8, 255), qMin(g8, 255),
                                        qMin(b8, 255));
                }
                // soft top sheen inside the upper half of the lens
                if (dy < 0. && r > 0.35 && r < 0.9) {
                    const int a = qRound(38 * (1. - std::abs(r - 0.62) / 0.28));
                    if (a > 0) {
                        const int r8 = SkColorGetR(px) + a;
                        const int g8 = SkColorGetG(px) + a;
                        const int b8 = SkColorGetB(px) + a;
                        px = SkColorSetARGB(SkColorGetA(px),
                                            qMin(r8, 255), qMin(g8, 255),
                                            qMin(b8, 255));
                    }
                }
                dr[x] = px;
            } else {
                dr[x] = srcPixels[y * w + x];
            }
        }
    }
    return out;
}

// cheap value-noise helpers shared by the procedural samples
// (roughen creep, fractal clouds)
inline qreal fxHash2(const qreal x, const qreal y) {
    const qreal v = std::sin(x * 127.1 + y * 311.7) * 43758.5453;
    return v - std::floor(v);
}

inline qreal fxValueNoise(const qreal x, const qreal y) {
    const qreal a = fxHash2(std::floor(x), std::floor(y));
    const qreal b = fxHash2(std::floor(x) + 1., std::floor(y));
    const qreal c = fxHash2(std::floor(x), std::floor(y) + 1.);
    const qreal d = fxHash2(std::floor(x) + 1., std::floor(y) + 1.);
    const qreal fx = x - std::floor(x);
    const qreal fy = y - std::floor(y);
    const qreal ux = fx * fx * (3. - 2. * fx);
    const qreal uy = fy * fy * (3. - 2. * fy);
    return a + (b - a) * ux + (c - a) * uy +
           (a - b - c + d) * ux * uy;
}

inline qreal fxFbm(const qreal x, const qreal y) {
    qreal sum = 0.;
    qreal amp = 0.5;
    qreal f = 1.;
    for (int o = 0; o < 5; o++) {
        sum += amp * fxValueNoise(x * f, y * f);
        amp *= 0.5;
        f *= 2.;
    }
    return sum;
}


// fractal-noise cloud: fbm value noise in grayscale, drifting with
// the phase (the CPU path of the real effect does not respond to
// parameter changes, so the sample carries the look and the motion)
SkBitmap makeFractalSample(const int w, const int h, const qreal phase) {
    SkBitmap out;
    out.allocN32Pixels(w, h);
    uint32_t* const dstPixels =
            static_cast<uint32_t*>(out.getPixels());
    const qreal ph = phase * 6.28318530718;
    const qreal driftX = std::cos(ph) * 8.;
    const qreal driftY = std::sin(ph) * 8.;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            const qreal n = fxFbm(x / 26. + driftX, y / 26. + driftY);
            const int g = qBound(0, qRound(20 + 215 * n), 255);
            dstPixels[y * w + x] = SkColorSetARGB(255, g, g, g);
        }
    }
    return out;
}

// lattice-warp look: the image warped by a breathing gaussian bulge
// with a cyan lattice grid drawn over it, the grid lines bending
// with the same deformation so the warp is directly readable. The
// real caller's resample swallows thin overlay lines, so the sample
// carries both the warp and the grid.
// lattice-warp sample with real control-point semantics: a 5x5 rule
// grid whose center control point is dragged around (smooth falloff
// to neighbors, like the effect's soft mode); image and cyan grid
// lines deform together so the lattice reads directly. The real
// caller's resample swallows thin overlay lines, so the sample
// carries the deformation.
SkBitmap makeLatticeGridSample(const int w, const int h, const qreal phase) {
    const SkBitmap base = makeTextSample(w, h);
    SkBitmap out;
    out.allocN32Pixels(w, h);
    out.eraseARGB(0, 0, 0, 0);
    const uint32_t* const srcPixels =
            static_cast<const uint32_t*>(base.getPixels());
    uint32_t* const dstPixels =
            static_cast<uint32_t*>(out.getPixels());

    constexpr int n = 5; // lattice resolution
    const qreal cellW = w / static_cast<qreal>(n - 1);
    const qreal cellH = h / static_cast<qreal>(n - 1);
    // the dragged control point: grid (2,2), oscillating
    const qreal dragX = cellW * 1.1 * std::sin(2. * M_PI * phase);
    const qreal dragY = cellH * 0.9 * std::cos(2. * M_PI * phase * 0.8);
    // smooth falloff from the dragged point (soft mode feel)
    const auto dispAt = [&](const qreal x, const qreal y) {
        const qreal gx = x / cellW;
        const qreal gy = y / cellH;
        const qreal d2 = (gx - 2.) * (gx - 2.) + (gy - 2.) * (gy - 2.);
        const qreal k = std::exp(-d2 / 2.2);
        return QPointF(dragX * k, dragY * k);
    };
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            const QPointF d = dispAt(x, y);
            const int sx = qBound(0, qRound(x - d.x()), w - 1);
            const int sy = qBound(0, qRound(y - d.y()), h - 1);
            dstPixels[y * w + x] = srcPixels[sy * w + sx];
        }
    }
    // the grid lines, pushed through the same displacement
    SkCanvas c(out);
    SkPaint p;
    p.setAntiAlias(true);
    p.setStyle(SkPaint::kStroke_Style);
    p.setColor(SkColorSetARGB(200, 90, 225, 255));
    p.setStrokeWidth(2.2f);
    const qreal step = 10.;
    for (int i = 0; i < n; i++) {
        SkPath vLine;
        bool vs = false;
        for (qreal y = 0.; y <= h + step / 2.; y += step) {
            const QPointF d = dispAt(i * cellW, y);
            if (!vs) { vLine.moveTo(i * cellW + d.x(), y + d.y()); vs = true; }
            else { vLine.lineTo(i * cellW + d.x(), y + d.y()); }
        }
        c.drawPath(vLine, p);
        SkPath hLine;
        bool hs = false;
        for (qreal x = 0.; x <= w + step / 2.; x += step) {
            const QPointF d = dispAt(x, i * cellH);
            if (!hs) { hLine.moveTo(x + d.x(), i * cellH + d.y()); hs = true; }
            else { hLine.lineTo(x + d.x(), i * cellH + d.y()); }
        }
        c.drawPath(hLine, p);
    }
    return out;
}

SkBitmap makeSample(const RasterEffectType type, const QSize& size) {
    const int w = size.width();
    const int h = size.height();
    switch (sampleFor(type)) {
    case Sample::green: return makeGreenSample(w, h);
    case Sample::liquid: return makeLiquidSample(w, h, 0.);
    case Sample::lattice: return makeLatticeGridSample(w, h, 0.);
    case Sample::text:
    default: return makeTextSample(w, h);
    }
}

// per-type explicit parameter sweeps for effects whose animation
// parameter the generic keyword scan misses (progress/time style
// names) or whose parameter lives nested inside a group (lattice
// control points). The value sweeps linearly from "from" to "to"
// over the loop and wraps.
struct NamedScan {
    const char* paramName;
    const char* altName; // second accepted name (translated variants)
    qreal from;
    qreal to;
};

NamedScan namedScanFor(const RasterEffectType type) {
    switch (type) {
    // clip transitions: no parameter sweep - from == to pins the
    // fade length so the relFrame sweep alone drives the in -> hold
    // -> out cycle in the tile
    case RasterEffectType::TRANSITION_DISSOLVE:
        return { "淡入时长", nullptr, 12., 12. };
    case RasterEffectType::TRANSITION_FLASH:
        return { "淡入时长", nullptr, 12., 12. };
    case RasterEffectType::TRANSITION_SLIDE:
        return { "滑入时长", nullptr, 24., 24. };
    case RasterEffectType::TRANSITION_WIPE_CIRCLE:
    case RasterEffectType::TRANSITION_WIPE_LINEAR:
    case RasterEffectType::TRANSITION_BLINDS:
    case RasterEffectType::TRANSITION_NOISE:
    case RasterEffectType::TRANSITION_BLUR:
    case RasterEffectType::TRANSITION_ZOOM:
    case RasterEffectType::TRANSITION_MOSAIC:
    case RasterEffectType::TRANSITION_SPIN:
    case RasterEffectType::TRANSITION_MIRROR_FLIP:
    case RasterEffectType::TRANSITION_TWIRL:
    case RasterEffectType::TRANSITION_GLITCH:
    case RasterEffectType::TRANSITION_SHAKE:
    case RasterEffectType::TRANSITION_ZOOM_BLUR:
    case RasterEffectType::TRANSITION_DIR_BLUR:
    case RasterEffectType::TRANSITION_CHANNEL_SPLIT:
    case RasterEffectType::TRANSITION_HALFTONE:
    case RasterEffectType::TRANSITION_EDGE:
    case RasterEffectType::TRANSITION_INVERT:
    case RasterEffectType::TRANSITION_POSTERIZE:
    case RasterEffectType::TRANSITION_VIGNETTE:
    case RasterEffectType::TRANSITION_LIGHT_SWEEP:
    case RasterEffectType::TRANSITION_FILM_GRAIN:
    case RasterEffectType::TRANSITION_PAGE_FLIP:
        // 24-frame ramps on the 96-frame preview loop: the motion
        // phase runs 1s in / 2s hold / 1s out so direction reads
        // clearly instead of flashing by in half a second
        return { "淡入时长", nullptr, 24., 24. };
    case RasterEffectType::WIPE:
        return { "time", nullptr, 0., 1. };
    case RasterEffectType::NOISE_FADE:
        return { "time", nullptr, 0., 1. };
    case RasterEffectType::COLORIZE:
        // pink <-> magenta swing around the 330 base
        return { "hue", nullptr, 300., 355. };
    case RasterEffectType::CHANNEL_BLUR:
        return { "blue radius", nullptr, 15., 55. };
    case RasterEffectType::BRIGHTNESS_CONTRAST:
        return { "contrast", nullptr, 0.2, 0.7 };
    case RasterEffectType::COLOR_GRADING:
        // all grading params default to 0 (= passthrough); a warm/
        // cool temperature swing on top of the cine base look
        return { "temperature", nullptr, -10., 40. };
    case RasterEffectType::LAYER_STYLES:
        // all styles are static; breathe the shadow distance so the
        // tile is alive (the param name is the Chinese tr source
        // string, living inside the shadow group)
        return { "distance", "距离", 12., 34. };
    case RasterEffectType::PAGE_CURL:
        // factory progress 0 = flat page; sweeping it curls the page
        // across the corner for a real turning look (the animator's
        // name is the Chinese tr source string)
        return { "progress", "卷曲进度", 0., 100. };
    default:
        return { nullptr, nullptr, 0., 0. };
    }
}

// find a numeric animator by name, descending into nested complex
// animators (lattice warp keeps its u/v/rot/scale inside per-point
// groups two levels down)
QrealAnimator* findAnimatorByName(Property* const prop,
                                  const QString& name,
                                  const QString& alt,
                                  const int depth = 0) {
    if (!prop || depth > 3) { return nullptr; }
    if (auto* const qa = enve_cast<QrealAnimator*>(prop)) {
        const QString n = qa->prp_getName().toLower();
        if (n.contains(name.toLower()) ||
            (!alt.isEmpty() && n.contains(alt.toLower()))) {
            return qa;
        }
        return nullptr;
    }
    if (auto* const ca = enve_cast<ComplexAnimator*>(prop)) {
        const int n = ca->ca_getNumberOfChildren();
        for (int i = 0; i < n; i++) {
            auto* const found = findAnimatorByName(ca->ca_getChildAt(i),
                                                   name, alt, depth + 1);
            if (found) { return found; }
        }
    }
    return nullptr;
}

// find a combo property by name, descending into nested groups
ComboBoxProperty* findComboByName(Property* const prop,
                                  const QString& name,
                                  const int depth = 0) {
    if (!prop || depth > 3) { return nullptr; }
    if (auto* const combo = enve_cast<ComboBoxProperty*>(prop)) {
        if (combo->prp_getName().contains(name)) { return combo; }
        return nullptr;
    }
    if (auto* const ca = enve_cast<ComplexAnimator*>(prop)) {
        const int n = ca->ca_getNumberOfChildren();
        for (int i = 0; i < n; i++) {
            auto* const found = findComboByName(ca->ca_getChildAt(i),
                                                name, depth + 1);
            if (found) { return found; }
        }
    }
    return nullptr;
}

// locate the "main" numeric parameter for the generic scan: prefer a
// keyword hit on the property name, fall back to the first numeric
// child (effects generally declare their primary parameter first)
QrealAnimator* findMainParam(RasterEffect* const eff) {
    QrealAnimator* firstNumeric = nullptr;
    static const char* keys[] = {
        "radius", "blur", "strength", "amount", "power", "intensity",
        "scale", "size", "distance", "threshold", "spread", "density",
        "angle", "brightness", "contrast", "saturation", "opacity",
        "tolerance", "softness", "count", "speed", "amplitude",
        "frequency", nullptr
    };
    const int nChildren = eff->ca_getNumberOfChildren();
    for (int i = 0; i < nChildren; i++) {
        Property* const prop = eff->ca_getChildAt(i);
        auto* const qa = enve_cast<QrealAnimator*>(prop);
        if (!qa) { continue; }
        if (!firstNumeric) { firstNumeric = qa; }
        const QString name = qa->prp_getName().toLower();
        for (int k = 0; keys[k]; k++) {
            if (name.contains(QString::fromLatin1(keys[k]))) { return qa; }
        }
    }
    return firstNumeric;
}

// one-time default-value tweaks for effects whose factory defaults do
// not show anything interesting on the demo samples
void setupDefaults(RasterEffect* const eff, const RasterEffectType type) {
    const auto setParam = [eff](const char* name, const qreal value) {
        auto* const qa = findAnimatorByName(eff, QString::fromLatin1(name),
                                            QString());
        if (qa) { qa->setCurrentBaseValue(value); }
    };
    const auto setCombo = [eff](const char* name, const int value) {
        auto* const combo = findComboByName(
                    eff, QString::fromUtf8(name));
        if (combo) { combo->setCurrentValue(value); }
    };
    switch (type) {
    case RasterEffectType::PAGE_CURL:
        // factory mode 1 is the wave (safe default); the preview
        // shows a real page turn - and a fat curl tube reads at
        // thumbnail size
        setCombo("模式", 2);
        setParam("radius", 20.);
        break;
    case RasterEffectType::CHANNEL_BLUR:
        // factory 0/0/0 = no blur at all; strong per-channel spread
        setParam("red radius", 8.);
        setParam("green radius", 20.);
        setParam("blue radius", 40.);
        break;
    case RasterEffectType::BRIGHTNESS_CONTRAST:
        // factory 0/0 = passthrough; punchy readout
        setParam("brightness", 0.22);
        setParam("contrast", 0.45);
        break;
    case RasterEffectType::COLOR_GRADING:
        // factory defaults are all zero = no grading at all; a bold
        // warm cine base so the tile reads as "graded"
        setParam("exposure", 0.6);
        setParam("contrast", 40.);
        setParam("temperature", 30.);
        setParam("saturation", 30.);
        break;
    case RasterEffectType::LAYER_STYLES: {
        // factory default is all-styles-off = null caller; enable a
        // bold set (same setters the PSD import uses)
        const auto styles = enve_cast<LayerStylesEffect*>(eff);
        if (!styles) { break; }
        styles->shadowEnabled()->setCurrentBoolValue(true);
        styles->glowEnabled()->setCurrentBoolValue(true);
        styles->strokeEnabled()->setCurrentBoolValue(true);
        styles->setShadow(true, 0.0, 20.0, 30.0, 32.0, 100.0,
                          QColor(0, 0, 0));
        styles->setGlow(true, 45.0, 80.0, 95.0, QColor(80, 200, 255));
        styles->setStroke(true, 0, 7, 100, QColor(255, 60, 60));
        break;
    }
    default:
        break;
    }
}

} // namespace

namespace {

// storyboard-style motion cue overlaid on transition preview cards:
// a dark outline pass under a bright core keeps the arrow readable
// over any content
void previewDrawArrow(QPainter &p, const QPointF &from,
                      const QPointF &to, const qreal thickness)
{
    const QPointF d = to - from;
    const qreal len = qMax(1., std::hypot(d.x(), d.y()));
    const QPointF u(d.x() / len, d.y() / len);
    const QPointF n(-u.y(), u.x());
    const qreal head = qMax(thickness * 2.6, 7.);
    const QPointF tip = to;
    QPolygonF tri;
    tri << tip
        << tip - u * head + n * head * 0.55
        << tip - u * head - n * head * 0.55;
    for (int pass = 0; pass < 2; pass++) {
        const QColor col(pass == 0 ? QColor(0, 0, 0, 170)
                                   : QColor(255, 255, 255, 215));
        QPen pen(col, pass == 0 ? thickness + 2.5 : thickness,
                 Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        p.setPen(pen);
        p.setBrush(pass == 0 ? col : col);
        p.drawLine(from, to - u * head * 0.7);
        p.setPen(pass == 0 ? QPen(col, 1.5) : Qt::NoPen);
        p.drawPolygon(tri);
    }
}

// directional transitions carry a persistent arrow cue (like camera
// move arrows on a storyboard): slide = one arrow along the travel,
// zoom = four corner arrows converging or expanding, spin = two
// opposing tangent arrows, mirror flip = two arrows meeting on the
// flip axis. Drawn identically on every frame, so the per-frame
// diff-based convergence tests cancel it out.
void overlayTransitionCues(QImage &img, RasterEffect * const eff)
{
    if (!eff) { return; }
    const auto type = eff->getEffectType();
    const char *comboName = nullptr;
    switch (type) {
    case RasterEffectType::TRANSITION_SLIDE:
    case RasterEffectType::TRANSITION_SPIN:
        comboName = "方向"; break;
    case RasterEffectType::TRANSITION_ZOOM:
        comboName = "模式"; break;          // 0 shrink-in 1 grow-in
    case RasterEffectType::TRANSITION_MIRROR_FLIP:
        comboName = "翻转轴"; break;        // 0 vertical 1 horizontal
    default:
        return; // non-directional transitions get no cue
    }
    int comboVal = 0;
    const auto combo = findComboByName(eff, QString::fromUtf8(comboName));
    if (combo) { comboVal = combo->getCurrentValue(); }

    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing);
    const qreal w = img.width();
    const qreal h = img.height();
    const qreal th = qMax(2.5, w / 30.);
    const QPointF c(w / 2., h / 2.);
    const qreal m = qMin(w, h);

    switch (type) {
    case RasterEffectType::TRANSITION_SLIDE: {
        // the clip travels INTO place: the cue follows the motion
        // (from-right = moving leftwards, etc., matching the labels)
        static const QPointF dirs[4] = {
            QPointF(-1, 0), QPointF(1, 0),
            QPointF(0, -1), QPointF(0, 1)
        };
        const QPointF d = dirs[comboVal & 3];
        previewDrawArrow(p, c - d * m * 0.30, c + d * m * 0.30, th);
        break;
    }
    case RasterEffectType::TRANSITION_ZOOM: {
        // four corner arrows along the diagonals: shrink-in points
        // at the centre, grow-in points away from it
        const bool inward = comboVal == 0;
        const qreal inset = m * 0.15;
        const QPointF corners[4] = {
            QPointF(inset, inset), QPointF(w - inset, inset),
            QPointF(w - inset, h - inset), QPointF(inset, h - inset)
        };
        for (const auto &corner : corners) {
            QPointF u = inward ? (c - corner) : (corner - c);
            const qreal l = qMax(1., std::hypot(u.x(), u.y()));
            u = QPointF(u.x() / l, u.y() / l);
            const QPointF a = corner + u * m * 0.06;
            const QPointF b = a + u * m * 0.15;
            previewDrawArrow(p, a, b, th * 0.62);
        }
        break;
    }
    case RasterEffectType::TRANSITION_SPIN: {
        // two tangent arrows on opposite sides of the centre:
        // clockwise = top arrow pointing right, bottom pointing left
        const bool cw = comboVal == 0;
        const qreal r = m * 0.26;
        const qreal arm = m * 0.16;
        const QPointF top(c.x(), c.y() - r);
        const QPointF bot(c.x(), c.y() + r);
        const QPointF rightDir(c.x() + arm, c.y() - r);
        const QPointF leftDir(c.x() - arm, c.y() - r);
        const QPointF rightDirB(c.x() + arm, c.y() + r);
        const QPointF leftDirB(c.x() - arm, c.y() + r);
        if (cw) {
            previewDrawArrow(p, top - QPointF(arm, 0), rightDir, th * 0.62);
            previewDrawArrow(p, bot + QPointF(arm, 0), leftDirB, th * 0.62);
        } else {
            previewDrawArrow(p, top + QPointF(arm, 0), leftDir, th * 0.62);
            previewDrawArrow(p, bot - QPointF(arm, 0), rightDirB, th * 0.62);
        }
        break;
    }
    case RasterEffectType::TRANSITION_MIRROR_FLIP: {
        // two arrows meeting head-on at the flip axis
        if (comboVal == 0) { // vertical axis: arrows press in from sides
            previewDrawArrow(p, QPointF(c.x() - m * 0.36, c.y()),
                             QPointF(c.x() - m * 0.10, c.y()), th * 0.62);
            previewDrawArrow(p, QPointF(c.x() + m * 0.36, c.y()),
                             QPointF(c.x() + m * 0.10, c.y()), th * 0.62);
        } else { // horizontal axis: arrows press in from top/bottom
            previewDrawArrow(p, QPointF(c.x(), c.y() - m * 0.36),
                             QPointF(c.x(), c.y() - m * 0.10), th * 0.62);
            previewDrawArrow(p, QPointF(c.x(), c.y() + m * 0.36),
                             QPointF(c.x(), c.y() + m * 0.10), th * 0.62);
        }
        break;
    }
    default:
        break;
    }
}

} // namespace

namespace EffectPreview {

bool canPreview(const RasterEffectType type) {
    if (type == RasterEffectType::CUSTOM ||
        type == RasterEffectType::CUSTOM_SHADER) { return false; }
    return true;
}

QList<QImage> renderEffectFrames(const RasterEffectType type,
                                 const int nFrames,
                                 const QSize& imgSize)
{
    QList<QImage> result;
    if (!canPreview(type)) { return result; }
    if (nFrames < 1 || imgSize.width() < 2 || imgSize.height() < 2) {
        return result;
    }
    try {
        // purpose-built sample paths (their callers cannot produce
        // the look offscreen): liquid glass needs the composite below
        // the layer; fractal noise's CPU path ignores parameters.
        // Roughen edges and lattice warp now run their real CPU
        // callers. (motion blur / rain / fractal noise retired with
        // their effects)
        if (type == RasterEffectType::LIQUID_GLASS ||
            type == RasterEffectType::LATTICE_WARP) {
            for (int i = 0; i < nFrames; i++) {
                const qreal t = i / static_cast<qreal>(nFrames);
                const SkBitmap frame =
                        type == RasterEffectType::LIQUID_GLASS
                        ? makeLiquidSample(imgSize.width(),
                                           imgSize.height(), t)
                        : type == RasterEffectType::LATTICE_WARP
                        ? makeLatticeGridSample(imgSize.width(),
                                                imgSize.height(), t)
                        : makeFractalSample(imgSize.width(),
                                            imgSize.height(), t);
                QImage img(imgSize, QImage::Format_ARGB32_Premultiplied);
                if (img.sizeInBytes() > 0) {
                    memcpy(img.bits(), frame.getPixels(),
                           static_cast<size_t>(img.sizeInBytes()));
                }
                result << img;
            }
            return result;
        }

        const auto eff = createRasterEffectForNonCustomType(type);
        if (!eff) { return result; }
        setupDefaults(eff.get(), type);

        // explicit named sweep wins over the generic keyword scan
        // (progress/time-style parameters, nested lattice controls)
        const NamedScan named = namedScanFor(type);
        QrealAnimator* namedParam = nullptr;
        if (named.paramName) {
            // paramName is a UTF-8 byte string: Chinese parameter
            // names ("淡入时长") decode into mojibake under fromLatin1,
            // the lookup silently misses and the generic scan takes
            // over (the fade window then drifts across the loop
            // instead of staying pinned)
            namedParam = findAnimatorByName(
                        eff.get(), QString::fromUtf8(named.paramName),
                        named.altName ? QString::fromUtf8(named.altName)
                                      : QString());
        }

        // the scan anchors on the factory default so consecutive
        // frames cannot drift
        QrealAnimator* const scanParam = namedParam ? namedParam
                                                    : findMainParam(eff.get());
        const qreal baseVal = scanParam ? scanParam->getCurrentBaseValue() : 0.;
        const qreal targetVal = scanParam ? scanParam->clamped(baseVal * 2.5) : 0.;

        const SkBitmap src = makeSample(type, imgSize);
        // transitions play on a doubled loop (4s): with the fade
        // ramps pinned at 24 frames the motion phase lasts a full
        // second each way instead of flashing by in half one
        const int loopSceneFrames = qMax(2, qRound(gPreviewLoopSec * gPreviewFps)) *
                (isTransitionEffectType(type) ? 2 : 1);

        // position/axis-type parameters get a gentle oscillation
        // around the default instead of the wide 2.5x sweep: a wide
        // sweep walks e.g. the mirror axis off-canvas and half the
        // loop shows a half-flipped image that reads as broken
        const bool gentleScan = (type == RasterEffectType::MIRROR);

        for (int i = 0; i < nFrames; i++) {
            const qreal t = i / static_cast<qreal>(nFrames);
            const int relFrame = qRound(i * loopSceneFrames / static_cast<qreal>(nFrames));

            if (namedParam) {
                // linear one-way sweep (wipe progress, shatter spread)
                namedParam->setCurrentBaseValue(
                            named.from + (named.to - named.from) * t);
            } else if (scanParam && !qFuzzyIsNull(baseVal)) {
                if (gentleScan) {
                    const qreal v = scanParam->clamped(
                                baseVal + 0.15 * std::sin(2. * M_PI * t));
                    scanParam->setCurrentBaseValue(v);
                } else {
                    const qreal v = baseVal + (targetVal - baseVal)
                                        * (0.5 - 0.5 * std::cos(2. * M_PI * t));
                    scanParam->setCurrentBaseValue(v);
                }
            }

            const auto caller = eff->getEffectCaller(relFrame, 1., 1., nullptr);
            QImage img(imgSize, QImage::Format_ARGB32_Premultiplied);
            img.fill(Qt::transparent);
            if (caller && !caller->samplesBackdrop()) {
                // the skia raster surface writes straight into the
                // qimage buffer (matching memory layouts)
                SkBitmap dst;
                dst.installPixels(
                            SkImageInfo::Make(imgSize.width(),
                                              imgSize.height(),
                                              kN32_SkColorType,
                                              kPremul_SkAlphaType),
                            img.bits(),
                            static_cast<size_t>(img.bytesPerLine()));
                CpuRenderTools tools{src, dst};
                CpuRenderData data;
                data.fTexTile = SkIRect::MakeWH(imgSize.width(),
                                                imgSize.height());
                data.fWidth = static_cast<uint>(imgSize.width());
                data.fHeight = static_cast<uint>(imgSize.height());
                caller->processCpu(tools, data);
            }
            // storyboard motion cue on directional transitions
            overlayTransitionCues(img, eff.get());
            result << img;
        }
        // restore the factory default so a cached instance is not
        // left mid-scan (defensive; instances are per-call here)
        if (scanParam) { scanParam->setCurrentBaseValue(baseVal); }
    } catch (const std::exception& e) {
        qWarning() << "effect preview render failed:" << e.what();
        return {};
    } catch (...) {
        qWarning() << "effect preview render failed: unknown error";
        return {};
    }
    return result;
}

} // namespace EffectPreview
