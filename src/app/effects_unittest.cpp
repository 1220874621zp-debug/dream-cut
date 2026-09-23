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

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QtConcurrent/QtConcurrentMap>
#include <QTranslator>
#include <QDebug>
#include <QFile>
#include <QDir>
#include <iostream>
#include <cassert>
#include <cstring>

#include "RasterEffects/rastereffectsinclude.h"
#include "RasterEffects/rastereffectcollection.h"
#include "RasterEffects/rastereffectmenucreator.h"
#include "RasterEffects/effectpreview.h"
#include "Properties/comboboxproperty.h"
#include "include/core/SkBitmap.h"

#include "themesupport.h"
#include "textanimpresets.h"
#include "layeranimpresets.h"
#include "GUI/mainwindow.h"
#include "GUI/canvaswindow.h"
#include "GUI/timelinedockwidget.h"
#include "GUI/RenderWidgets/renderwidget.h"
#include "GUI/RenderWidgets/renderinstancewidget.h"
#include "renderinstancesettings.h"
#include "renderhandler.h"

// frame-sequence hash for the parallel determinism test
static const auto gHashFrames = [](const QList<QImage>& frames) -> QByteArray {
    QCryptographicHash h(QCryptographicHash::Md5);
    for (const auto& f : frames) {
        const QImage c = f.convertToFormat(QImage::Format_ARGB32);
        h.addData(reinterpret_cast<const char*>(c.constBits()),
                  c.sizeInBytes());
    }
    return h.result();
};

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    // surface qWarning from core (psd parser diagnostics) on stderr:
    // the default Windows handler drops them when no real console
    qInstallMessageHandler([](QtMsgType type, const QMessageLogContext&,
                              const QString& msg) {
        fprintf(stderr, "[QT%d] %s\n", int(type), qPrintable(msg));
        fflush(stderr);
    });
    int passed = 0;
    int failed = 0;

    const auto runTest = [&](const char* name, auto func) {
        std::cout << "[RUNNING] " << name << " ... ";
        try {
            func();
            std::cout << "PASSED" << std::endl;
            passed++;
        } catch (const std::exception& e) {
            std::cout << "FAILED: " << e.what() << std::endl;
            failed++;
        } catch (...) {
            std::cout << "FAILED (unknown exception)" << std::endl;
            failed++;
        }
    };

    // Test 1: Factory instantiation for all RasterEffectTypes
    runTest("Test 1: createRasterEffectForNonCustomType", [&]() {
        const RasterEffectType types[] = {
            RasterEffectType::BLUR,
            RasterEffectType::SHADOW,
            RasterEffectType::WIPE,
            RasterEffectType::NOISE_FADE,
            RasterEffectType::COLORIZE,
            RasterEffectType::BRIGHTNESS_CONTRAST,
            RasterEffectType::VIGNETTE,
            RasterEffectType::CHROMATIC_ABERRATION,
            RasterEffectType::LETTERBOX,
            RasterEffectType::SCANLINES,
            RasterEffectType::GLOW,
            RasterEffectType::DIRECTIONAL_BLUR,
            RasterEffectType::RADIAL_BLUR,
            RasterEffectType::EDGE_DETECT,
            RasterEffectType::INVERT,
            RasterEffectType::TINT,
            RasterEffectType::PIXELATE,
            RasterEffectType::NOISE,
            RasterEffectType::MIRROR,
            RasterEffectType::GLITCH,
            RasterEffectType::POSTERIZE,
            RasterEffectType::TWIRL,
            RasterEffectType::CHANNEL_BLUR,
            RasterEffectType::HALFTONE,
            RasterEffectType::SHAKE,
            RasterEffectType::DROP_SHADOW,
            RasterEffectType::ZOOM_BLUR,
            RasterEffectType::COLOR_GRADING,
            RasterEffectType::STRIPE,
            RasterEffectType::LIGHT_SWEEP,
            RasterEffectType::FILM_GRAIN,
            RasterEffectType::BLACK_WHITE_FLASH,
            RasterEffectType::LIQUID_GLASS,
            RasterEffectType::PIXEL_ART,
            RasterEffectType::CHROMA_KEY,
            RasterEffectType::LAYER_STYLES,
            RasterEffectType::PAGE_CURL,
            RasterEffectType::TRANSITION_DISSOLVE,
            RasterEffectType::TRANSITION_FLASH,
            RasterEffectType::TRANSITION_SLIDE,
            RasterEffectType::TRANSITION_WIPE_CIRCLE
        };

        for (const auto t : types) {
            auto eff = createRasterEffectForNonCustomType(t);
            if (!eff) {
                throw std::runtime_error(std::string("Factory returned null for type ") + std::to_string(int(t)));
            }
            if (eff->getEffectType() != t) {
                throw std::runtime_error("Effect type mismatch");
            }
            if (eff->prp_getName().isEmpty()) {
                throw std::runtime_error("Effect name is empty");
            }
        }
    });

    // Test 2: Caller generation and CPU render execution
    runTest("Test 2: CPU Tile Processing", [&]() {
        const RasterEffectType types[] = {
            RasterEffectType::BRIGHTNESS_CONTRAST,
            RasterEffectType::COLORIZE,
            RasterEffectType::VIGNETTE,
            RasterEffectType::CHROMATIC_ABERRATION,
            RasterEffectType::LETTERBOX,
            RasterEffectType::SCANLINES,
            RasterEffectType::GLOW,
            RasterEffectType::DIRECTIONAL_BLUR,
            RasterEffectType::RADIAL_BLUR,
            RasterEffectType::EDGE_DETECT,
            RasterEffectType::INVERT,
            RasterEffectType::TINT,
            RasterEffectType::PIXELATE,
            RasterEffectType::NOISE,
            RasterEffectType::MIRROR,
            RasterEffectType::GLITCH,
            RasterEffectType::POSTERIZE,
            RasterEffectType::TWIRL,
            RasterEffectType::CHANNEL_BLUR,
            RasterEffectType::HALFTONE,
            RasterEffectType::SHAKE,
            RasterEffectType::DROP_SHADOW,
            RasterEffectType::ZOOM_BLUR,
            RasterEffectType::COLOR_GRADING,
            RasterEffectType::STRIPE,
            RasterEffectType::LIGHT_SWEEP,
            RasterEffectType::FILM_GRAIN,
            RasterEffectType::BLACK_WHITE_FLASH,
            RasterEffectType::LIQUID_GLASS,
            RasterEffectType::PIXEL_ART,
            RasterEffectType::PAGE_CURL
        };

        SkBitmap srcBtmp;
        srcBtmp.allocN32Pixels(64, 64);
        srcBtmp.eraseARGB(255, 128, 64, 200);

        SkBitmap dstBtmp;
        dstBtmp.allocN32Pixels(64, 64);
        dstBtmp.eraseARGB(0, 0, 0, 0);

        for (const auto t : types) {
            auto eff = createRasterEffectForNonCustomType(t);
            auto caller = eff->getEffectCaller(0.0, 1.0, 1.0, nullptr);
            if (!caller) {
                throw std::runtime_error("Caller is null for type " + std::to_string(int(t)));
            }

            CpuRenderTools tools{srcBtmp, dstBtmp};
            CpuRenderData data;
            data.fTexTile = SkIRect::MakeXYWH(0, 0, 64, 64);

            caller->processCpu(tools, data);
        }
    });

    // Test 2b: Layer Styles effect needs at least one style enabled
    // before a caller exists (default state is all-off = null caller).
    // Mirrors EffectSubTaskSpawner: full-size dst, per-tile subsets,
    // then pixel assertions (styles must land outside the silhouette)
    runTest("Test 2b: Layer Styles CPU render", [&]() {
        const auto eff = createRasterEffectForNonCustomType(
                RasterEffectType::LAYER_STYLES);
        if (!eff) { throw std::runtime_error("Factory returned null"); }
        const auto styles = enve_cast<LayerStylesEffect*>(eff.get());
        if (!styles) { throw std::runtime_error("Not a LayerStylesEffect"); }
        styles->shadowEnabled()->setCurrentBoolValue(true);
        styles->glowEnabled()->setCurrentBoolValue(true);
        styles->strokeEnabled()->setCurrentBoolValue(true);
        // angle 0 -> light from the right, shadow falls left
        styles->setShadow(true, 0.0, 10.0, 0.0, 5.0, 100.0, QColor(0, 0, 0));
        const auto caller = styles->getEffectCaller(0.0, 1.0, 1.0, nullptr);
        if (!caller) { throw std::runtime_error("Layer styles caller is null"); }

        SkBitmap srcBtmp;
        srcBtmp.allocN32Pixels(64, 64);
        srcBtmp.eraseARGB(0, 0, 0, 0);
        {
            SkCanvas c(srcBtmp);
            SkPaint p;
            p.setColor(SkColorSetARGB(255, 128, 64, 200));
            c.drawRect(SkRect::MakeXYWH(16, 16, 32, 32), p);
        }

        SkBitmap dstBtmp;
        dstBtmp.allocN32Pixels(srcBtmp.width(), srcBtmp.height());
        dstBtmp.eraseARGB(0, 0, 0, 0);

        // two vertical tiles, exactly like the spawner subsets them
        const SkIRect tiles[] = { SkIRect::MakeXYWH(0, 0, 32, 64),
                                  SkIRect::MakeXYWH(32, 0, 32, 64) };
        for (const auto& tile : tiles) {
            SkBitmap tileDst;
            if (!dstBtmp.extractSubset(&tileDst, tile)) {
                throw std::runtime_error("extractSubset failed");
            }
            CpuRenderTools tools{srcBtmp, tileDst};
            CpuRenderData data;
            data.fTexTile = tile;
            caller->processCpu(tools, data);
        }

        // silhouette core survives with the original color
        const auto core = static_cast<const uint32_t*>(dstBtmp.getAddr(32, 32));
        if (SkColorGetA(*core) < 250) {
            throw std::runtime_error("layer core lost alpha");
        }
        // stroke ring outside the right edge of the square (x=50)
        const auto rightRing = static_cast<const uint32_t*>(dstBtmp.getAddr(50, 32));
        if (SkColorGetA(*rightRing) < 100
                || SkColorGetR(*rightRing) < 200) {
            throw std::runtime_error("no red stroke ring on the right");
        }
        // shadow left of the square: square spans x[16,48], distance 10
        // -> shadow spans x[6,38]; sample inside it
        const auto shadowPx = static_cast<const uint32_t*>(dstBtmp.getAddr(7, 32));
        if (SkColorGetA(*shadowPx) < 30) {
            throw std::runtime_error("no shadow to the left");
        }

        // --- round 2: with spread/choke (the PSD-import parameter
        // shape that failed on the user's GPU render) ---
        dstBtmp.eraseARGB(0, 0, 0, 0);
        styles->setShadow(true, 90.0, 10.0, 56.0, 7.0, 40.0, QColor(0, 0, 0));
        styles->setGlow(true, 42.0, 54.0, 23.0, QColor(0, 255, 24));
        const auto caller2 = styles->getEffectCaller(0.0, 1.0, 1.0, nullptr);
        if (!caller2) { throw std::runtime_error("caller2 is null"); }
        for (const auto& tile : tiles) {
            SkBitmap tileDst;
            if (!dstBtmp.extractSubset(&tileDst, tile)) {
                throw std::runtime_error("extractSubset failed");
            }
            CpuRenderTools tools{srcBtmp, tileDst};
            CpuRenderData data;
            data.fTexTile = tile;
            caller2->processCpu(tools, data);
        }
        // angle 90 with the PS dial mapping -> shadow straight UP
        // (-10): spans y[6,38]; sample above the square (y=12);
        // shadow is black: tint must dominate, not the layer's purple
        const auto shadowPx2 = static_cast<const uint32_t*>(dstBtmp.getAddr(20, 12));
        if (SkColorGetA(*shadowPx2) < 20
                || SkColorGetB(*shadowPx2) > SkColorGetR(*shadowPx2) + 10) {
            throw std::runtime_error("no choked black shadow above");
        }
        // glow: the user's real PSD parameters (spread 42, size 54,
        // opacity 23%) must stay visible - spread is ignored for glow
        // and the rim is lifted x2, otherwise 0.23*0.5*choke leaves
        // a handful of alpha units invisible to the eye
        styles->setGlow(true, 42.0, 54.0, 23.0, QColor(0, 255, 24));
        const auto caller3 = styles->getEffectCaller(0.0, 1.0, 1.0, nullptr);
        if (!caller3) { throw std::runtime_error("caller3 is null"); }
        dstBtmp.eraseARGB(0, 0, 0, 0);
        for (const auto& tile : tiles) {
            SkBitmap tileDst;
            if (!dstBtmp.extractSubset(&tileDst, tile)) {
                throw std::runtime_error("extractSubset failed");
            }
            CpuRenderTools tools{srcBtmp, tileDst};
            CpuRenderData data;
            data.fTexTile = tile;
            caller3->processCpu(tools, data);
        }
        // (11,32) is 5px left of the square edge: ~48/255 green
        const auto glowPx = static_cast<const uint32_t*>(dstBtmp.getAddr(11, 32));
        if (SkColorGetA(*glowPx) < 30
                || SkColorGetG(*glowPx) < SkColorGetR(*glowPx)) {
            std::cout << " [glow debug:";
            for (int x = 8; x <= 20; x += 2) {
                const auto px = static_cast<const uint32_t*>(dstBtmp.getAddr(x, 32));
                std::cout << " x" << x << "=" << SkColorGetA(*px)
                          << "/g" << SkColorGetG(*px);
            }
            std::cout << "] ";
            throw std::runtime_error("no visible green glow around");
        }
    });

    // Test 2c: visual effect-preview frames for every core raster
    // effect (the card gallery renderer); optionally dumps PNGs to
    // argv[2] for eyeballing. Asserts the CPU offscreen path returns
    // a full frame sequence with non-blank content.
    runTest("Test 2c: EffectPreview frames", [&]() {
        const RasterEffectType types[] = {
            RasterEffectType::BLUR,
            RasterEffectType::SHADOW,
            RasterEffectType::WIPE,
            RasterEffectType::NOISE_FADE,
            RasterEffectType::COLORIZE,
            RasterEffectType::BRIGHTNESS_CONTRAST,
            RasterEffectType::CHROMA_KEY,
            RasterEffectType::LIQUID_GLASS,
            RasterEffectType::VIGNETTE,
            RasterEffectType::CHROMATIC_ABERRATION,
            RasterEffectType::LETTERBOX,
            RasterEffectType::SCANLINES,
            RasterEffectType::GLOW,
            RasterEffectType::DIRECTIONAL_BLUR,
            RasterEffectType::RADIAL_BLUR,
            RasterEffectType::EDGE_DETECT,
            RasterEffectType::INVERT,
            RasterEffectType::TINT,
            RasterEffectType::PIXELATE,
            RasterEffectType::NOISE,
            RasterEffectType::MIRROR,
            RasterEffectType::GLITCH,
            RasterEffectType::POSTERIZE,
            RasterEffectType::TWIRL,
            RasterEffectType::CHANNEL_BLUR,
            RasterEffectType::HALFTONE,
            RasterEffectType::SHAKE,
            RasterEffectType::DROP_SHADOW,
            RasterEffectType::ZOOM_BLUR,
            RasterEffectType::COLOR_GRADING,
            RasterEffectType::STRIPE,
            RasterEffectType::LIGHT_SWEEP,
            RasterEffectType::FILM_GRAIN,
            RasterEffectType::BLACK_WHITE_FLASH,
            RasterEffectType::PIXEL_ART,
            RasterEffectType::LAYER_STYLES,
            RasterEffectType::PAGE_CURL,
            RasterEffectType::LATTICE_WARP,
            RasterEffectType::TRANSITION_DISSOLVE,
            RasterEffectType::TRANSITION_FLASH,
            RasterEffectType::TRANSITION_SLIDE,
            RasterEffectType::TRANSITION_WIPE_CIRCLE
        };
        QString dumpDir;
        if (argc >= 3) {
            dumpDir = QString::fromLocal8Bit(argv[2]);
            QDir().mkpath(dumpDir);
        }
        int rendered = 0;
        int blank = 0;
        for (const auto t : types) {
            if (!EffectPreview::canPreview(t)) { continue; }
            const auto frames = EffectPreview::renderEffectFrames(
                        t, 16, QSize(160, 160));
            if (frames.count() != 16) {
                throw std::runtime_error("frame count mismatch for type "
                                         + std::to_string(int(t)));
            }
            // at least one frame must carry visible pixels
            bool anyOpaque = false;
            for (const auto& f : frames) {
                if (f.isNull()) { continue; }
                for (int y = 0; y < f.height() && !anyOpaque; y += 8) {
                    for (int x = 0; x < f.width(); x += 8) {
                        if (qAlpha(f.pixel(x, y)) > 8) {
                            anyOpaque = true;
                            break;
                        }
                    }
                }
                if (anyOpaque) { break; }
            }
            if (anyOpaque) { rendered++; }
            else {
                blank++;
                std::cout << " [blank: type " << int(t) << "] ";
            }
            if (!dumpDir.isEmpty() && !frames.isEmpty()) {
                const int idx = frames.count() / 2;
                frames.at(idx).save(dumpDir + "/" +
                                    QString::number(int(t)) + ".png");
                frames.at(0).save(dumpDir + "/f0_" +
                                  QString::number(int(t)) + ".png");
                frames.at(qMax(1, frames.count() / 4)).save(
                            dumpDir + "/q_" +
                            QString::number(int(t)) + ".png");
            }
        }
        std::cout << " (" << rendered << " rendered, "
                  << blank << " blank) ";
        if (rendered < 20) {
            throw std::runtime_error("too few effects produced visible frames");
        }
    });

    // Test 2d: concurrency determinism - the visual panel renders all
    // tiles in parallel on QtConcurrent threads; if any effect's CPU
    // path mutates shared/static state, parallel output differs from
    // serial. Hash both and compare per effect type.
    runTest("Test 2d: EffectPreview parallel determinism", [&]() {
        const RasterEffectType probeTypes[] = {
            RasterEffectType::BLUR,
            RasterEffectType::MIRROR,
            RasterEffectType::TWIRL,
            RasterEffectType::SHAKE,
            RasterEffectType::GLITCH,
            RasterEffectType::FILM_GRAIN,
            RasterEffectType::NOISE,
            RasterEffectType::GLOW,
        };
        const auto hashFrames = gHashFrames;
        // run the same batch in parallel several times; any mismatch
        // against the serial baseline or between rounds is a race
        QVector<QPair<RasterEffectType, int>> jobs;
        for (const auto t : probeTypes) {
            if (!EffectPreview::canPreview(t)) { continue; }
            jobs << qMakePair(t, 0);
            jobs << qMakePair(t, 1); // each effect twice concurrently
        }
        QVector<QByteArray> serial;
        for (const auto& j : jobs) {
            serial << hashFrames(EffectPreview::renderEffectFrames(
                        j.first, 6, QSize(64, 64)));
        }
        const auto runBatch = [jobs]() {
            return QtConcurrent::blockingMapped<QVector<QByteArray>>(
                        jobs, [](const QPair<RasterEffectType, int>& j) -> QByteArray {
                return gHashFrames(EffectPreview::renderEffectFrames(
                            j.first, 6, QSize(64, 64)));
            });
        };
        const int rounds = 6;
        int mismatches = 0;
        for (int r = 0; r < rounds; r++) {
            const auto par = runBatch();
            const int n = qMin(par.size(), serial.size());
            for (int i = 0; i < n; i++) {
                if (par.at(i) != serial.at(i)) {
                    mismatches++;
                    std::cout << " [RACE: type " << int(jobs.at(i).first)
                              << " job " << i << "] ";
                }
            }
        }
        std::cout << " (" << rounds << " rounds, "
                  << mismatches << " mismatches) ";
        if (mismatches > 0) {
            throw std::runtime_error("parallel rendering is not deterministic");
        }
    });

    // Test 3: Menu registry coverage
    runTest("Test 3: RasterEffectMenuCreator coverage", [&]() {
        int count = 0;
        RasterEffectMenuCreator::forEveryEffectCore(
            [&](const QString& name, const QString& cat,
                const RasterEffectMenuCreator::EffectCreator& creator) {
                Q_UNUSED(cat)
                auto eff = creator();
                if (!eff) {
                    throw std::runtime_error("Menu creator produced null effect: " + name.toStdString());
                }
                count++;
            });

        if (count < 20) {
            throw std::runtime_error("Expected at least 20 core effects in menu, found " + std::to_string(count));
        }
    });

    // Test 4: Chinese translation resource load test
    runTest("Test 4: Chinese (zh_CN) Translation Loading", [&]() {
        QTranslator translator;
        const bool loaded = translator.load(":/translations/dreamcut_zh_CN.qm");
        if (!loaded) {
            throw std::runtime_error("Failed to load :/translations/dreamcut_zh_CN.qm resource");
        }
    });

    // Test 7: Page Curl CPU math (identity at zero progress, curl at mid)
    runTest("Test 7: Page Curl CPU math", [&]() {
        const auto eff = createRasterEffectForNonCustomType(
                RasterEffectType::PAGE_CURL);
        if (!eff) { throw std::runtime_error("Factory returned null"); }
        // the effect defaults to wave mode; pin curl mode for these checks
        eff->ca_getChildAt<ComboBoxProperty>(0)->setCurrentValue(0);

        SkBitmap src;
        src.allocN32Pixels(128, 128);
        src.eraseARGB(255, 200, 100, 50);

        const auto render = [&](SkBitmap& dst) {
            dst.allocN32Pixels(128, 128);
            dst.eraseARGB(0, 0, 0, 0);
            const auto caller = eff->getEffectCaller(0.0, 1.0, 1.0, nullptr);
            if (!caller) { throw std::runtime_error("null caller"); }
            CpuRenderTools tools{src, dst};
            CpuRenderData data;
            data.fTexTile = SkIRect::MakeXYWH(0, 0, 128, 128);
            caller->processCpu(tools, data);
        };

        // identity: progress 0 must be an exact passthrough
        {
            SkBitmap dst;
            render(dst);
            for (int y = 0; y < 128; y += 5) {
                for (int x = 0; x < 128; x += 5) {
                    const SkColor c = dst.getColor(x, y);
                    if (SkColorGetA(c) != 255 ||
                        qAbs(int(SkColorGetR(c)) - 200) > 2 ||
                        qAbs(int(SkColorGetG(c)) - 100) > 2 ||
                        qAbs(int(SkColorGetB(c)) - 50) > 2) {
                        throw std::runtime_error("progress 0 is not identity");
                    }
                }
            }
        }

        // mid progress, default direction (right edge rolls leftward):
        // the consumed far-right side empties, the kept left side stays
        // opaque and shaded, and the back face shows near the tube
        const auto prog = eff->ca_getChildAt<QrealAnimator>(1);
        if (!prog) { throw std::runtime_error("no progress animator"); }
        prog->setCurrentBaseValue(50.0);
        {
            SkBitmap dst;
            render(dst);
            // far right: page has left
            if (SkColorGetA(dst.getColor(126, 64)) != 0) {
                throw std::runtime_error("consumed side is not transparent");
            }
            // far left: still opaque, shaded darker than the source
            const SkColor c = dst.getColor(6, 64);
            if (SkColorGetA(c) != 255) {
                throw std::runtime_error("kept side lost opacity");
            }
            if (SkColorGetR(c) >= 200 || SkColorGetR(c) < 60) {
                throw std::runtime_error("kept side not lit/shaded");
            }
            // near the tube the flipped back face (gray) must show:
            // some pixel there has blue >= red, unlike the orange front
            bool sawBack = false;
            for (int y = 20; y < 108 && !sawBack; y += 4) {
                for (int x = 30; x < 66; x += 2) {
                    const SkColor b = dst.getColor(x, y);
                    if (SkColorGetA(b) > 200 &&
                        SkColorGetB(b) >= SkColorGetR(b)) {
                        sawBack = true;
                        break;
                    }
                }
            }
            if (!sawBack) {
                throw std::runtime_error("back face never visible");
            }
        }
        prog->setCurrentBaseValue(0.0);

        // wave mode: the image must stay fully visible - amplitude 0 is
        // an exact passthrough, amplitude 8 keeps every pixel opaque and
        // shaded (no transparency anywhere)
        const auto mode = eff->ca_getChildAt<ComboBoxProperty>(0);
        const auto amp = eff->ca_getChildAt<QrealAnimator>(10);
        if (!mode || !amp) { throw std::runtime_error("no mode/amp animator"); }
        mode->setCurrentValue(1);
        amp->setCurrentBaseValue(0.0);
        {
            SkBitmap dst;
            render(dst);
            const SkColor c = dst.getColor(64, 64);
            if (SkColorGetA(c) != 255 ||
                qAbs(int(SkColorGetR(c)) - 200) > 2 ||
                qAbs(int(SkColorGetG(c)) - 100) > 2 ||
                qAbs(int(SkColorGetB(c)) - 50) > 2) {
                throw std::runtime_error("wave amp 0 is not identity");
            }
        }
        amp->setCurrentBaseValue(8.0);
        {
            SkBitmap dst;
            render(dst);
            for (int y = 0; y < 128; y += 5) {
                for (int x = 0; x < 128; x += 5) {
                    const SkColor c = dst.getColor(x, y);
                    if (SkColorGetA(c) != 255) {
                        throw std::runtime_error("wave mode lost opacity");
                    }
                }
            }
            const SkColor a = dst.getColor(10, 64);
            const SkColor b = dst.getColor(118, 64);
            if (a == b) {
                throw std::runtime_error("wave shading has no variation");
            }
        }
        // crossed wave keeps everything opaque too
        eff->ca_getChildAt<QrealAnimator>(16)->setCurrentBaseValue(50.0);
        {
            SkBitmap dst;
            render(dst);
            const SkColor c = dst.getColor(64, 10);
            if (SkColorGetA(c) != 255) {
                throw std::runtime_error("crossed wave lost opacity");
            }
        }
        eff->ca_getChildAt<QrealAnimator>(16)->setCurrentBaseValue(0.0);
        mode->setCurrentValue(0);
        amp->setCurrentBaseValue(0.0);

        // page turn (mode 2): mid progress shows the flipped back and
        // keeps a large opaque area; full progress lies flat mirrored
        mode->setCurrentValue(2);
        eff->ca_getChildAt<QrealAnimator>(1)->setCurrentBaseValue(50.0); // progress
        {
            SkBitmap dst;
            render(dst);
            int opaque = 0;
            int backish = 0;
            for (int y = 0; y < 128; y += 6) {
                for (int x = 0; x < 128; x += 6) {
                    const SkColor c = dst.getColor(x, y);
                    if (SkColorGetA(c) == 255) opaque++;
                    if (SkColorGetA(c) == 255 &&
                        qAbs(int(SkColorGetB(c)) - int(SkColorGetR(c))) < 12 &&
                        SkColorGetR(c) > 100) backish++;
                }
            }
            if (opaque < 100) {
                throw std::runtime_error("page turn lost the image");
            }
            if (backish < 8) {
                throw std::runtime_error("page turn back never visible");
            }
        }
        eff->ca_getChildAt<QrealAnimator>(1)->setCurrentBaseValue(100.0);
        {
            SkBitmap dst;
            render(dst);
            int opaque = 0;
            for (int y = 0; y < 128; y += 6) {
                for (int x = 0; x < 128; x += 6) {
                    if (SkColorGetA(dst.getColor(x, y)) == 255) opaque++;
                }
            }
            if (opaque < 300) {
                throw std::runtime_error("full turn does not lie flat");
            }
        }
        eff->ca_getChildAt<QrealAnimator>(1)->setCurrentBaseValue(0.0);

        // slant + perspective + spiral on curl mode: no crash, output
        // stays bounded and the far side still empties
        mode->setCurrentValue(0);
        eff->ca_getChildAt<QrealAnimator>(1)->setCurrentBaseValue(50.0);
        eff->ca_getChildAt<QrealAnimator>(13)->setCurrentBaseValue(60.0); // slant
        eff->ca_getChildAt<QrealAnimator>(14)->setCurrentBaseValue(40.0); // perspective
        eff->ca_getChildAt<QrealAnimator>(15)->setCurrentBaseValue(30.0); // spiral
        {
            SkBitmap dst;
            render(dst);
            int opaque = 0;
            for (int y = 0; y < 128; y += 6) {
                for (int x = 0; x < 128; x += 6) {
                    const SkColor c = dst.getColor(x, y);
                    if (SkColorGetA(c) == 255 &&
                        (SkColorGetR(c) > 250 || SkColorGetG(c) > 250)) {
                        throw std::runtime_error("slant/perspective/spline produced unclamped output");
                    }
                    if (SkColorGetA(c) == 255) opaque++;
                }
            }
            if (opaque < 80) {
                throw std::runtime_error("slant/perspective destroyed the image");
            }
        }
        eff->ca_getChildAt<QrealAnimator>(13)->setCurrentBaseValue(0.0);
        eff->ca_getChildAt<QrealAnimator>(14)->setCurrentBaseValue(0.0);
        eff->ca_getChildAt<QrealAnimator>(15)->setCurrentBaseValue(0.0);
        eff->ca_getChildAt<QrealAnimator>(1)->setCurrentBaseValue(0.0);

    });

    // Test 5: ThemeSupport presets, accents and style generation test
    runTest("Test 5: ThemeSupport presets and styling", [&]() {
        const auto &presets = ThemeSupport::themePresetList();
        if (presets.size() < 10) {
            throw std::runtime_error("Expected at least 10 theme presets, got " + std::to_string(presets.size()));
        }

        const auto &accents = ThemeSupport::accentPresetList();
        if (accents.size() < 10) {
            throw std::runtime_error("Expected at least 10 accent presets, got " + std::to_string(accents.size()));
        }

        // Test theme switching
        ThemeSupport::setThemeFromId(QStringLiteral("morandi_dark"));
        if (ThemeSupport::themeId() != QStringLiteral("morandi_dark")) {
            throw std::runtime_error("Theme ID mismatch for morandi_dark");
        }
        if (!ThemeSupport::getThemeBaseColor().isValid()) {
            throw std::runtime_error("Invalid base color for morandi_dark");
        }
        if (!ThemeSupport::getThemeHighlightColor().isValid()) {
            throw std::runtime_error("Invalid highlight color for morandi_dark");
        }

        // Test custom radius and scrollbar
        ThemeSupport::setBorderRadius(8);
        if (ThemeSupport::borderRadius() != 8) {
            throw std::runtime_error("Failed to set border radius to 8");
        }
        ThemeSupport::setScrollbarWidth(6);
        if (ThemeSupport::scrollbarWidth() != 6) {
            throw std::runtime_error("Failed to set scrollbar width to 6");
        }

        const QString style = ThemeSupport::getThemeStyle(20);
        if (style.isEmpty()) {
            throw std::runtime_error("Generated theme style string is empty");
        }

        // Restore friction theme
        ThemeSupport::setThemeFromId(QStringLiteral("friction"));
    });

    // Test 8: Kinetic Text & Layer Animation Presets
    runTest("Test 8: Kinetic Text & Layer Animation Presets", [&]() {
        const auto& textPresets = TextAnimPresets::all();
        if (textPresets.size() < 160) {
            throw std::runtime_error(QString("Text presets count too low: %1 (expected >= 160)").arg(textPresets.size()).toStdString());
        }

        const auto& layerPresets = LayerAnimPresets::all();
        if (layerPresets.size() < 60) {
            throw std::runtime_error(QString("Layer presets count too low: %1 (expected >= 60)").arg(layerPresets.size()).toStdString());
        }

        const int totalPresets = textPresets.size() + layerPresets.size();
        if (totalPresets < 220) {
            throw std::runtime_error(QString("Total presets count too low: %1 (expected >= 220)").arg(totalPresets).toStdString());
        }

        // Verify key text presets from each archetype exist and have valid fields
        const QStringList keyTextIds = {
            "sharp-snap-rise", "sharp-elastic-pop", "sharp-blade-cut",
            "sharp-double-bounce", "sharp-jelly-squash", "sharp-trampoline",
            "smooth-float-rise", "smooth-cinematic-fade", "smooth-aurora",
            "smooth-par-float", "smooth-bloom-slow",
            "prop-pos-x-left", "prop-scale-uniform", "prop-rot-full-360", "prop-shear-slash-x",
            "prop-scale-wide-8x",
            "3d-flip-y-cw", "3d-corkscrew", "3d-barrel-roll", "3d-door-swing-left",
            "tech-typewriter-std", "tech-number-roll", "tech-matrix-rain",
            "tech-binary-matrix", "tech-crt-scan",
            "loop-sine-wave", "loop-breathe-soft", "loop-heartbeat", "loop-rainbow-wave"
        };
        for (const auto& id : keyTextIds) {
            const auto p = TextAnimPresets::byId(id);
            if (!p) {
                throw std::runtime_error(QString("Missing key text preset: %1").arg(id).toStdString());
            }
            if (p->name.isEmpty() || p->duration <= 0.0 || p->tag.isEmpty()) {
                throw std::runtime_error(QString("Invalid data in text preset: %1").arg(id).toStdString());
            }
        }

        // Verify key text presets have diverse and distinct physical easings
        const auto snapPreset = TextAnimPresets::byId("sharp-snap-rise");
        if (!snapPreset || snapPreset->easing != TextEasing::sharpSnap) {
            throw std::runtime_error("sharp-snap-rise missing sharpSnap easing");
        }
        const auto bouncePreset = TextAnimPresets::byId("sharp-overshoot-down");
        if (!bouncePreset || bouncePreset->easing != TextEasing::bounce) {
            throw std::runtime_error("sharp-overshoot-down missing bounce easing");
        }
        const auto elasticPreset = TextAnimPresets::byId("sharp-elastic-pop");
        if (!elasticPreset || elasticPreset->easing != TextEasing::elastic) {
            throw std::runtime_error("sharp-elastic-pop missing elastic easing");
        }
        const auto anticipatePreset = TextAnimPresets::byId("3d-corkscrew");
        if (!anticipatePreset || anticipatePreset->easing != TextEasing::anticipate) {
            throw std::runtime_error("3d-corkscrew missing anticipate easing");
        }
        const auto steppedPreset = TextAnimPresets::byId("tech-typewriter-std");
        if (!steppedPreset || steppedPreset->easing != TextEasing::stepped) {
            throw std::runtime_error("tech-typewriter-std missing stepped easing");
        }

        // Verify TextEffect setups physics correctly
        const auto effect = enve::make_shared<TextEffect>();
        effect->setupFromPreset(*elasticPreset, 200.0, 48.0, 0, 30.0, 1.0);
        if (!effect->hasCustomPhysics()) {
            throw std::runtime_error("TextEffect failed to initialize custom physics");
        }
        if (effect->getEasing() != TextEasing::elastic) {
            throw std::runtime_error("TextEffect easing mismatch");
        }

        // Verify key layer presets exist and have valid generators
        const QStringList keyLayerIds = {
            "l-fade", "l-pop", "l-drop", "l-flip-x",
            "l-swing", "l-skew-slide", "l-elastic-scale", "l-orbit-3d",
            "l-door-open-l", "l-dive-3d", "l-jelly-wobble", "l-heavy-stamp-jitter",
            "l-sheet-slide-up", "l-glitch-shake", "l-heartbeat-layer"
        };
        for (const auto& id : keyLayerIds) {
            const auto p = LayerAnimPresets::byId(id);
            if (!p) {
                throw std::runtime_error(QString("Missing key layer preset: %1").arg(id).toStdString());
            }
            if (p->name.isEmpty() || p->duration <= 0.0 || (!p->gen && !p->outGen)) {
                throw std::runtime_error(QString("Invalid data or missing generator in layer preset: %1").arg(id).toStdString());
            }
        }
    });

    std::cout << "\n=========================================" << std::endl;
    std::cout << "Unit Test Summary: " << passed << " passed, " << failed << " failed." << std::endl;
    std::cout << "=========================================" << std::endl;

    return (failed == 0) ? 0 : 1;
}
