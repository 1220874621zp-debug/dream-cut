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

// Fork of enve - Copyright (C) 2016-2020 Maurycy Liebner

#include "timelinedockwidget.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QScrollBar>
#include <QShortcut>
#include <QPainter>
#include <QDir>
#include <QFileInfo>
#include <cmath>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTime>
#include <QStatusBar>
#include <QTimer>
#include <QSvgRenderer>
#include <QFile>
#include <QApplication>
#include <QActionGroup>

#include <functional>

#include "Private/document.h"
#include "GUI/global.h"
#include "GUI/BoxesList/boxscrollwidget.h"
#include "GUI/BoxesList/boxsinglewidget.h"
#include "GUI/keysview.h"
#include "Boxes/boundingbox.h"
#include "CacheHandlers/sceneframecontainer.h"
#include "skia/skiahelpers.h"
#include "Animators/transformanimator.h"
#include "Animators/complexanimator.h"
#include "Animators/animator.h"
#include "Animators/qrealanimator.h"
#include "Expressions/expression.h"
#include "Properties/property.h"
#include "GUI/propertynamedialog.h"
#include "swt_abstraction.h"

#include "mainwindow.h"
#include "canvaswindow.h"
#include "canvas.h"
#include "animationdockwidget.h"
#include "widgets/widgetstack.h"
#include "widgets/actionbutton.h"
#include "timelinewidget.h"
#include "widgets/framescrollbar.h"
#include "renderinstancesettings.h"
#include "layouthandler.h"
#include "memoryhandler.h"
#include "appsupport.h"
#include "actions.h"
#include "misc/keyfocustarget.h"
#include "GUI/Timeline/nletimelinemodel.h"
#include "GUI/Timeline/nletimelineview.h"
#include "GUI/Timeline/nletimelinecontroller.h"
#include "directplayer.h"
#include "Sound/audiohandler.h"

namespace {
// qrc SVG toolbar icon rendered AT the device pixel ratio: a plain
// 64x64 dpr=1 pixmap is upscaled 2x on 200% displays and looks blurry,
// so render 64*dpr physical px and tag the pixmap with the dpr.
// NOTE: QPainter on a QPixmap always works in PHYSICAL pixels
// (setDevicePixelRatio does not rescale the painter), so the render
// rect must be multiplied by dpr or the glyph lands in the top-left
// quarter only
QPixmap svgToolbarPixmap(const QString& qrcPath, const int inset = 4)
{
    const int base = 64;
    const qreal dpr = qApp ? qApp->devicePixelRatio() : 1.;
    QPixmap pm(QSize(base, base) * dpr);
    pm.fill(Qt::transparent);
    QSvgRenderer renderer(qrcPath);
    if (renderer.isValid()) {
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        const qreal o = inset * dpr;
        const qreal s = (base - 2 * inset) * dpr;
        renderer.render(&p, QRectF(o, o, s, s));
        p.end();
    }
    pm.setDevicePixelRatio(dpr);
    return pm;
}

// user-supplied magnet glyph; the fill is swapped per state
const char* kNleMagneticSvg =
        "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 1024 1024\">"
        "<path fill=\"%1\" d=\"M827.968 145.792a361.088 361.088 0 0 1 61.632 493.44"
        "l-11.328 14.72L648.32 934.4l-4.8 5.312a70.72 70.72 0 0 1-94.592 4.48"
        "l-134.016-109.888-6.4-6.4a44.992 44.992 0 0 1-4.864-49.216l5.056-7.488"
        " 246.08-300.288 4.416-5.952a72.32 72.32 0 0 0-14.464-95.744L638.848 364.8"
        "a72.32 72.32 0 0 0-90.752 8.96l-4.992 5.504-246.08 300.288a44.928 44.928"
        " 0 0 1-63.232 6.272L99.84 575.936a70.592 70.592 0 0 1-14.208-93.44"
        "l4.224-5.888 229.888-280.384 12.16-14.08a361.088 361.088 0 0 1 481.344"
        "-47.68l14.72 11.328zM218.56 420.544l-79.168 96.64a6.592 6.592 0 0 0"
        " 0.96 9.28l119.296 97.728 84.48-103.104-125.568-100.48z m610.176 192.832"
        "A297.088 297.088 0 0 0 369.28 236.8L259.2 371.072l125.632 100.48"
        " 108.8-132.864a136.32 136.32 0 0 1 210.816 172.8l-94.08 114.816"
        " 125.504 100.48 92.928-113.408z m-259.136 62.528"
        "l-99.2 121.024 119.232 97.792c2.816 2.24 6.912 1.92 9.216-0.896l96.32-117.504"
        "L569.6 675.84z\"/></svg>";

QPixmap nleMagneticPixmap(const QColor &color, const int base = 24)
{
    const qreal dpr = qApp ? qApp->devicePixelRatio() : 1.;
    QPixmap pm(QSize(base, base) * dpr);
    pm.fill(Qt::transparent);
    const QString svg = QString(kNleMagneticSvg).arg(color.name());
    QSvgRenderer renderer(svg.toUtf8());
    if (renderer.isValid()) {
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        renderer.render(&p, QRectF(0, 0, base * dpr, base * dpr));
        p.end();
    }
    pm.setDevicePixelRatio(dpr);
    return pm;
}

// ---- NLE editing-tool glyphs (inline SVG, %1 = glyph color) ----
// select tool: classic cursor arrow
const char* kNleToolSelectSvg =
        "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 24 24\">"
        "<path fill=\"%1\" d=\"M6 3 L18.2 13.4 L12.6 13.9 L15.6 20.1"
        " L12.9 21.3 L9.9 15.1 L6 18.4 Z\"/></svg>";
// razor tool: scissors (two rings + crossing blades)
const char* kNleToolRazorSvg =
        "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 24 24\">"
        "<circle cx=\"6\" cy=\"6.2\" r=\"2.3\" fill=\"none\" stroke=\"%1\" stroke-width=\"1.7\"/>"
        "<circle cx=\"6\" cy=\"17.8\" r=\"2.3\" fill=\"none\" stroke=\"%1\" stroke-width=\"1.7\"/>"
        "<path fill=\"none\" stroke=\"%1\" stroke-width=\"1.7\""
        " d=\"M7.9 7.7 L20 18.2 M7.9 16.3 L20 5.8\"/></svg>";
// track select backward: double chevron pointing left
const char* kNleToolBackSvg =
        "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 24 24\">"
        "<path fill=\"none\" stroke=\"%1\" stroke-width=\"2.2\""
        " stroke-linecap=\"round\" stroke-linejoin=\"round\""
        " d=\"M11.5 6 L5.5 12 L11.5 18 M18.5 6 L12.5 12 L18.5 18\"/></svg>";
// track select forward: mirrored
const char* kNleToolFwdSvg =
        "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 24 24\">"
        "<path fill=\"none\" stroke=\"%1\" stroke-width=\"2.2\""
        " stroke-linecap=\"round\" stroke-linejoin=\"round\""
        " d=\"M12.5 6 L18.5 12 L12.5 18 M5.5 6 L11.5 12 L5.5 18\"/></svg>";
// spacer tool: two blocks with a double arrow in the gap
const char* kNleToolSpacerSvg =
        "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 24 24\">"
        "<path fill=\"none\" stroke=\"%1\" stroke-width=\"1.9\""
        " d=\"M2.5 8 H8.5 V16 H2.5 Z M15.5 8 H21.5 V16 H15.5 Z\"/>"
        "<path fill=\"none\" stroke=\"%1\" stroke-width=\"1.9\""
        " stroke-linecap=\"round\" stroke-linejoin=\"round\""
        " d=\"M10.6 12 H13.4 M12.8 10.4 L10.6 12 L12.8 13.6"
        " M11.2 10.4 L13.4 12 L11.2 13.6\"/></svg>";
// hand tool: pan palm (simplified Material pan_tool, stroke style
// matching the other glyphs)
const char* kNleToolHandSvg =
        "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 24 24\">"
        "<path fill=\"none\" stroke=\"%1\" stroke-width=\"1.8\""
        " stroke-linejoin=\"round\""
        " d=\"M8.5 12.5 V5.5 a1.3 1.3 0 0 1 2.6 0 V11 M11.1 11 V3.8"
        " a1.3 1.3 0 0 1 2.6 0 V11 M13.7 11 V4.8 a1.3 1.3 0 0 1 2.6 0"
        " V12 M16.3 12 V6.5 a1.3 1.3 0 0 1 2.6 0 V14.5 c0 4.1-2.4 6.5"
        " -6.5 6.5 c-2.7 0-4.2-.9-5.6-2.6 l-3.3-4.2 a1.35 1.35 0 0 1"
        " 2-1.8 l2.5 2.1 Z\"/></svg>";
// freeze: snowflake-ish asterisk
const char* kNleFreezeSvg =
        "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 24 24\">"
        "<path fill=\"none\" stroke=\"%1\" stroke-width=\"1.9\""
        " stroke-linecap=\"round\" d=\"M12 3 V21 M4.2 7.5 L19.8 16.5"
        " M19.8 7.5 L4.2 16.5\"/></svg>";
// split at playhead: two half blocks cut by a dashed line
const char* kNleSplitSvg =
        "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 24 24\">"
        "<path fill=\"none\" stroke=\"%1\" stroke-width=\"1.9\""
        " stroke-dasharray=\"2.6 2\" d=\"M12 2.5 V21.5\"/>"
        "<path fill=\"none\" stroke=\"%1\" stroke-width=\"1.9\""
        " d=\"M2.5 8 H9 V16 H2.5 Z M15 8 H21.5 V16 H15 Z\"/></svg>";
// undo / redo: curved arrows
const char* kNleUndoSvg =
        "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 24 24\">"
        "<path fill=\"none\" stroke=\"%1\" stroke-width=\"2\""
        " stroke-linecap=\"round\" stroke-linejoin=\"round\""
        " d=\"M8.5 13.5 L3.5 8.5 L8.5 3.5 M3.5 8.5 H13"
        " A6.5 6.5 0 0 1 19.5 15 V20\"/></svg>";
const char* kNleRedoSvg =
        "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 24 24\">"
        "<path fill=\"none\" stroke=\"%1\" stroke-width=\"2\""
        " stroke-linecap=\"round\" stroke-linejoin=\"round\""
        " d=\"M15.5 13.5 L20.5 8.5 L15.5 3.5 M20.5 8.5 H11"
        " A6.5 6.5 0 0 0 4.5 15 V20\"/></svg>";

QPixmap nleGlyphPixmap(const char *svg, const QColor &color,
                       const int base = 24)
{
    const qreal dpr = qApp ? qApp->devicePixelRatio() : 1.;
    QPixmap pm(QSize(base, base) * dpr);
    pm.fill(Qt::transparent);
    const QString str = QString::fromUtf8(svg).arg(color.name());
    QSvgRenderer renderer(str.toUtf8());
    if (renderer.isValid()) {
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        renderer.render(&p, QRectF(0, 0, base * dpr, base * dpr));
        p.end();
    }
    pm.setDevicePixelRatio(dpr);
    return pm;
}
}

TimelineDockWidget::TimelineDockWidget(Document& document,
                                       LayoutHandler * const layoutH,
                                       MainWindow * const parent)
    : QWidget(parent)
    , mDocument(document)
    , mMainWindow(parent)
    , mToolBar(nullptr)
    , mFrameRewindAct(nullptr)
    , mFrameFastForwardAct(nullptr)
    , mCurrentFrameSpinAct(nullptr)
    , mCurrentFrameSpin(nullptr)
    , mRenderProgressAct(nullptr)
    , mRenderProgress(nullptr)
    , mPausedPreviewState({false, 0})
{
    Q_UNUSED(layoutH) // classic timeline stack retired; handler unused
    connect(RenderHandler::sInstance, &RenderHandler::previewFinished,
            this, &TimelineDockWidget::previewFinished);
    connect(RenderHandler::sInstance, &RenderHandler::previewBeingPlayed,
            this, &TimelineDockWidget::previewBeingPlayed);
    connect(RenderHandler::sInstance, &RenderHandler::previewBeingRendered,
            this, &TimelineDockWidget::previewBeingRendered);
    connect(RenderHandler::sInstance, &RenderHandler::previewPaused,
            this, &TimelineDockWidget::previewPaused);

    connect(Document::sInstance, &Document::canvasModeSet,
            this, &TimelineDockWidget::updateButtonsVisibility);

    setFocusPolicy(Qt::NoFocus);

    mMainLayout = new QVBoxLayout(this);
    setLayout(mMainLayout);
    mMainLayout->setSpacing(0);
    mMainLayout->setContentsMargins(0, 0, 0, 0);

    // the timeline model exists from the start so the toolbar (zoom
    // slider, NLE cluster) can wire to it before the page is built
    mNleModel = new NleTimelineModel(mDocument, this);
    // magnetic (kdenlive-style) dragging is the default; the saved
    // state restores the flag WITHOUT compacting - a loaded project
    // must not be rewritten just because magnetic defaults on
    mNleModel->setMagnetic(AppSupport::getSettings(
                QStringLiteral("ui"),
                QStringLiteral("timelineMagnetic"), true).toBool(), false);
    connect(mNleModel, &NleTimelineModel::logMessage, this,
            [this](const QString &msg) {
        if (mMainWindow) { mMainWindow->statusBar()->showMessage(msg, 4000); }
    });

    mFrameRewindAct = new QAction(QIcon::fromTheme("rewind"),
                                  tr("Rewind"),
                                  this);
    mFrameRewindAct->setShortcut(QKeySequence(AppSupport::getSettings("shortcuts",
                                                                      "rewind",
                                                                      "Shift+Left").toString()));
    mFrameRewindAct->setData(tr("Go to First Frame"));
    connect(mFrameRewindAct, &QAction::triggered,
            this, [this]() {
        const auto scene = *mDocument.fActiveScene;
        if (!scene) { return; }
        const bool jumpFrame = (QApplication::keyboardModifiers() & (Qt::ShiftModifier | Qt::AltModifier)) == (Qt::ShiftModifier | Qt::AltModifier);
        if (jumpFrame) { // Go to previous scene quarter
            jumpToIntermediateFrame(false);
        } else { // Go to First Frame
            scene->anim_setAbsFrame(scene->getFrameRange().fMin);
            mDocument.actionFinished();
        }
    });

    mFrameFastForwardAct = new QAction(QIcon::fromTheme("fastforward"),
                                       tr("Fast Forward"),
                                       this);
    mFrameFastForwardAct->setShortcut(QKeySequence(AppSupport::getSettings("shortcuts",
                                                                           "fastForward",
                                                                           "Shift+Right").toString()));
    mFrameFastForwardAct->setData(tr("Go to Last Frame"));
    connect(mFrameFastForwardAct, &QAction::triggered,
            this, [this]() {
        const auto scene = *mDocument.fActiveScene;
        if (!scene) { return; }
        const bool jumpFrame = (QApplication::keyboardModifiers() & (Qt::ShiftModifier | Qt::AltModifier)) == (Qt::ShiftModifier | Qt::AltModifier);
        if (jumpFrame) { // Go to next scene quarter
            jumpToIntermediateFrame(true);
        } else { // Go to Last Frame
            scene->anim_setAbsFrame(scene->getFrameRange().fMax);
            mDocument.actionFinished();
        }
    });

    mPlayFromBeginningButton = new QAction(QIcon::fromTheme("preview"),
                                           tr("Play Preview From Start"),
                                           this);
    connect(mPlayFromBeginningButton, &QAction::triggered,
            this, [this]() {
        /*const auto scene = *mDocument.fActiveScene;
        if (!scene) { return; }
        scene->anim_setAbsFrame(scene->getFrameRange().fMin);
        renderPreview();*/
        const auto state = RenderHandler::sInstance->currentPreviewState();
        setPreviewFromStart(state);
    });

    mPlayButton = new QAction(QIcon::fromTheme("play"),
                              tr("Play Preview"),
                              this);

    mStopButton = new QAction(QIcon::fromTheme("stop"),
                              tr("Stop Preview"),
                              this);

    connect(mStopButton, &QAction::triggered,
            this, &TimelineDockWidget::interruptPreview);

    mLoopButton = new QAction(QIcon::fromTheme("preview_loop"),
                              tr("Loop Preview"),
                              this);
    mLoopButton->setCheckable(true);
    connect(mLoopButton, &QAction::triggered,
            this, &TimelineDockWidget::setLoop);

    // snapshot: quick PNG export of the current canvas frame
    {
        QPixmap pm = svgToolbarPixmap(
                    QStringLiteral(":/icons/camera_tool.svg"), 0);
        // white version (toolbar icon convention); painter on a
        // pixmap works in physical px so pm.rect() is correct here
        QPainter w(&pm);
        w.setCompositionMode(QPainter::CompositionMode_SourceIn);
        w.fillRect(pm.rect(), Qt::white);
        w.end();
        mSnapshotButton = new QAction(pm, tr("Snapshot PNG"), this);
        mSnapshotButton->setToolTip(tr(
                "Export the current frame as a 100% resolution PNG "
                "(snapshot path is configurable in Preferences, "
                "default: Desktop)"));
        connect(mSnapshotButton, &QAction::triggered,
                this, &TimelineDockWidget::snapshotCurrentFrame);
    }

    // AE-style view toggles: action/title safe guides + transparency
    // checkerboard background (view only, never rendered/exported)
    {
        QPixmap sf(64, 64);
        sf.fill(Qt::transparent);
        QPainter p(&sf);
        p.setRenderHint(QPainter::Antialiasing);
        QPen pen(QColor(255, 255, 255, 220));
        pen.setWidthF(4.);
        pen.setStyle(Qt::DashLine);
        p.setPen(pen);
        p.drawRect(QRectF(6, 14, 52, 36));
        pen.setColor(QColor(255, 220, 90, 220));
        pen.setWidthF(4.);
        p.setPen(pen);
        p.drawRect(QRectF(14, 21, 36, 22));
        p.end();
        mSafeFramesButton = new QAction(sf, tr("Safe Frames"), this);
        mSafeFramesButton->setCheckable(true);
        mSafeFramesButton->setToolTip(tr(
                "Show action/title safe frames (90%/80%)"));
        connect(mSafeFramesButton, &QAction::triggered,
                this, [this](const bool checked) {
            const auto scene = *mDocument.fActiveScene;
            if(scene) scene->setSafeFramesVisible(checked);
        });

        // mask everything outside the canvas - the same toggle as the
        // view menu "Clip to Scene" (shortcut C), surfaced as a button
        // next to the safe-frames toggle (user-supplied SVG icon)
        {
            mClipCanvasButton = new QAction(
                        svgToolbarPixmap(QStringLiteral(":/icons/clip_canvas.svg")),
                        tr("遮蔽画布外"), this);
        }
        mClipCanvasButton->setCheckable(true);
        mClipCanvasButton->setToolTip(tr(
                "遮蔽掉画布之外的内容（同视图菜单 Clip to Scene，快捷键 Ctrl+Shift+C）"));
        connect(mClipCanvasButton, &QAction::triggered,
                this, [this](const bool checked) {
            const auto scene = *mDocument.fActiveScene;
            if(scene) scene->setClipToCanvas(checked);
        });

        // canvas rulers toggle (viewport overlay strips), user SVG icon
        mRulersButton = new QAction(
                    svgToolbarPixmap(QStringLiteral(":/icons/canvas_rulers.svg"),
                                     6),
                    tr("画布标尺"), this);
        mRulersButton->setCheckable(true);
        mRulersButton->setChecked(AppSupport::getSettings(
                    QStringLiteral("view"), QStringLiteral("rulers"),
                    true).toBool());
        mRulersButton->setToolTip(tr("显示画布标尺（像素坐标，跟随缩放平移）"));
        connect(mRulersButton, &QAction::triggered,
                this, [this](const bool checked) {
            CanvasWindow::setRulersVisible(checked);
            const auto scene = *mDocument.fActiveScene;
            if (scene) { emit scene->requestUpdate(); }
        });

        QPixmap tg(64, 64);
        tg.fill(Qt::transparent);
        QPainter t(&tg);
        // clean 2x2 checkerboard (no rounded sub-patches - those left
        // antialiased seams that read as stray pixels), inset so the
        // icon matches the visual weight of the neighbouring icons
        const QRectF grid(14, 14, 36, 36);
        const qreal half = 18.;
        t.setPen(Qt::NoPen);
        t.setBrush(QColor(160, 160, 160));
        t.drawRect(grid);                       // gray base (TL + BR)
        t.setBrush(QColor(255, 255, 255));
        t.drawRect(QRectF(grid.left() + half, grid.top(),
                          half, half));         // white TR
        t.drawRect(QRectF(grid.left(), grid.top() + half,
                          half, half));         // white BL
        QPen border(QColor(255, 255, 255, 210));
        border.setWidthF(2.5);
        t.setPen(border);
        t.setBrush(Qt::NoBrush);
        t.drawRect(grid.adjusted(-1.25, -1.25, 1.25, 1.25));
        t.end();
        mTransparencyGridButton = new QAction(tg, tr("Transparency Grid"),
                                              this);
        mTransparencyGridButton->setCheckable(true);
        mTransparencyGridButton->setToolTip(tr(
                "Toggle the transparency grid background"));
        connect(mTransparencyGridButton, &QAction::triggered,
                this, [this](const bool checked) {
            const auto scene = *mDocument.fActiveScene;
            if(scene) scene->setTransparencyGrid(checked);
        });

    }

    // match canvas: uniformly scale every selected layer so its width
    // or height matches the canvas, then center it (AE fit-to-comp
    // alike); one click = one undo step for the whole batch
    {
        // canvas frame + white double arrow along the matched axis
        const auto makeMatchIcon = [](const bool horizontal) {
            QPixmap pm(64, 64);
            pm.fill(Qt::transparent);
            QPainter p(&pm);
            p.setRenderHint(QPainter::Antialiasing);
            QPen frame(QColor(255, 255, 255, 170));
            frame.setWidthF(3.);
            p.setBrush(Qt::NoBrush);
            p.setPen(frame);
            p.drawRoundedRect(QRectF(6, 6, 52, 52), 6, 6);
            QPen arrow(QColor(255, 255, 255, 240));
            arrow.setWidthF(4.5);
            arrow.setCapStyle(Qt::RoundCap);
            p.setPen(arrow);
            const QPointF head(7.5, 7.5);
            if (horizontal) {
                p.drawLine(QPointF(15, 32), QPointF(49, 32));
                p.drawLine(QPointF(15, 32), QPointF(22, 32) + QPointF(0, -head.y()));
                p.drawLine(QPointF(15, 32), QPointF(22, 32) + QPointF(0, head.y()));
                p.drawLine(QPointF(49, 32), QPointF(42, 32) + QPointF(0, -head.y()));
                p.drawLine(QPointF(49, 32), QPointF(42, 32) + QPointF(0, head.y()));
            } else {
                p.drawLine(QPointF(32, 15), QPointF(32, 49));
                p.drawLine(QPointF(32, 15), QPointF(32, 22) + QPointF(-head.x(), 0));
                p.drawLine(QPointF(32, 15), QPointF(32, 22) + QPointF(head.x(), 0));
                p.drawLine(QPointF(32, 49), QPointF(32, 42) + QPointF(-head.x(), 0));
                p.drawLine(QPointF(32, 49), QPointF(32, 42) + QPointF(head.x(), 0));
            }
            p.end();
            return pm;
        };
        mMatchCanvasWidthButton = new QAction(makeMatchIcon(true),
                                              tr("Match Canvas Width"),
                                              this);
        mMatchCanvasWidthButton->setToolTip(tr(
                "Uniformly scale each selected layer so its width "
                "matches the canvas width, then center it on the "
                "canvas (aspect ratio preserved)"));
        connect(mMatchCanvasWidthButton, &QAction::triggered,
                this, [this]() { matchSelectedToCanvas(true); });

        mMatchCanvasHeightButton = new QAction(makeMatchIcon(false),
                                               tr("Match Canvas Height"),
                                               this);
        mMatchCanvasHeightButton->setToolTip(tr(
                "Uniformly scale each selected layer so its height "
                "matches the canvas height, then center it on the "
                "canvas (aspect ratio preserved)"));
        connect(mMatchCanvasHeightButton, &QAction::triggered,
                this, [this]() { matchSelectedToCanvas(false); });
    }

    mCurrentFrameSpin = new FrameSpinBox(this);
    mCurrentFrameSpin->setKeyboardTracking(false);
    mCurrentFrameSpin->setAlignment(Qt::AlignHCenter);
    mCurrentFrameSpin->setObjectName(QString::fromUtf8("SpinBoxNoButtons"));
    mCurrentFrameSpin->setFocusPolicy(Qt::ClickFocus);
    mCurrentFrameSpin->setToolTip(tr("Current frame"));
    mCurrentFrameSpin->setRange(-INT_MAX, INT_MAX);
    connect(mCurrentFrameSpin,
            &QSpinBox::editingFinished,
            this, [this]() { gotoFrame(mCurrentFrameSpin->value()); });
    connect(mCurrentFrameSpin,
            &FrameSpinBox::wheelValueChanged,
            this, &TimelineDockWidget::gotoFrame);

    mSetInPointAct = new QAction(QIcon::fromTheme("range-in"),
                                  tr("Set Layer In Point (Alt+[)"),
                                  this);
    mSetInPointAct->setToolTip(tr("Set Layer In Point (Alt+[)"));
    mSetInPointAct->setData(mSetInPointAct->toolTip());
    connect(mSetInPointAct, &QAction::triggered, this, [this]() {
        const auto scene = *mDocument.fActiveScene;
        if (!scene) { return; }
        scene->setSelectedBoxesInPoint();
        mDocument.actionFinished();
    });

    mSetOutPointAct = new QAction(QIcon::fromTheme("range-out"),
                                   tr("Set Layer Out Point (Alt+])"),
                                   this);
    mSetOutPointAct->setToolTip(tr("Set Layer Out Point (Alt+])"));
    mSetOutPointAct->setData(mSetOutPointAct->toolTip());
    connect(mSetOutPointAct, &QAction::triggered, this, [this]() {
        const auto scene = *mDocument.fActiveScene;
        if (!scene) { return; }
        scene->setSelectedBoxesOutPoint();
        mDocument.actionFinished();
    });

    mToolBar = new QToolBar(this);
    mToolBar->setMovable(false);

    mRenderProgress = new QProgressBar(this);
    mRenderProgress->setSizePolicy(QSizePolicy::Expanding,
                                   QSizePolicy::Expanding);
    mRenderProgress->setFixedWidth(mCurrentFrameSpin->width());
    mRenderProgress->setFormat(tr("Cache %p%"));

    eSizesUI::widget.add(mToolBar, [this](const int size) {
        //mRenderProgress->setFixedHeight(eSizesUI::button);
        mToolBar->setIconSize(QSize(size, size));
    });

    // timeline zoom slider: logarithmic map over the viewed frame span
    // (right = zoom in), acting on the current scene's timeline
    mZoomSlider = new QSlider(Qt::Horizontal, this);
    mZoomSlider->setRange(0, 100);
    mZoomSlider->setValue(50);
    mZoomSlider->setFixedWidth(110);
    mZoomSlider->setMaximumHeight(18);
    mZoomSlider->setToolTip(tr("时间轴缩放（右=放大，等价 Ctrl+滚轮）"));
    connect(mZoomSlider, &QSlider::valueChanged, this, [this](const int v) {
        if (mNleView) { mNleView->setZoomLevel(v); }
    });
    mToolBar->addWidget(mZoomSlider);

    addSpacer();

    mToolBar->addAction(mFrameRewindAct);
    mToolBar->addAction(mFrameFastForwardAct);

    mToolBar->addSeparator();
    mToolBar->addAction(mSetInPointAct);
    mToolBar->addAction(mSetOutPointAct);
    mToolBar->addSeparator();

    mRenderProgressAct = mToolBar->addWidget(mRenderProgress);
    mCurrentFrameSpinAct = mToolBar->addWidget(mCurrentFrameSpin);

    mToolBar->addAction(mPlayFromBeginningButton);
    mToolBar->addAction(mPlayButton);
    mToolBar->addAction(mStopButton);
    mToolBar->addAction(mLoopButton);
    mToolBar->addAction(mSnapshotButton);
    mToolBar->addAction(mSafeFramesButton);
    mToolBar->addSeparator();
    mToolBar->addAction(mClipCanvasButton);
    mToolBar->addAction(mRulersButton);
    mToolBar->addAction(mTransparencyGridButton);
    mToolBar->addSeparator();
    mToolBar->addAction(mMatchCanvasWidthButton);
    mToolBar->addAction(mMatchCanvasHeightButton);

    // end layout

    mRenderProgressAct->setVisible(false);

    mMainWindow->cmdAddAction(mFrameRewindAct);
    mMainWindow->cmdAddAction(mFrameFastForwardAct);
    mMainWindow->cmdAddAction(mSetInPointAct);
    mMainWindow->cmdAddAction(mSetOutPointAct);
    mMainWindow->cmdAddAction(mPlayFromBeginningButton);
    mMainWindow->cmdAddAction(mPlayButton);
    mMainWindow->cmdAddAction(mStopButton);
    mMainWindow->cmdAddAction(mLoopButton);

    mMainLayout->addWidget(mToolBar);
    mMainLayout->addSpacing(2);

    mPlayFromBeginningButton->setEnabled(false);
    mPlayButton->setEnabled(false);
    mStopButton->setEnabled(false);

    connect(&mDocument, &Document::activeSceneSet,
            this, [this](Canvas* const scene) {
        mPlayFromBeginningButton->setEnabled(scene);
        mPlayButton->setEnabled(scene);
        mStopButton->setEnabled(scene);
    });

    // the kdenlive-style editing timeline IS the timeline now: the
    // classic per-scene keyframe stack is retired, the NLE view is
    // the dock's only content. The LayoutHandler timeline stack is
    // left unparented and never shown
    mNlePage = new QWidget(this);
    {
        auto nleLay = new QVBoxLayout(mNlePage);
        nleLay->setContentsMargins(0, 0, 0, 0);
        nleLay->setSpacing(0);
        mNleView = new NleTimelineView(mNleModel, mNlePage);
        auto hbar = new QScrollBar(Qt::Horizontal, mNlePage);
        hbar->setFixedHeight(12);
        mNleView->setScrollBar(hbar);
        nleLay->addWidget(mNleView, 1);
        nleLay->addWidget(hbar, 0);
        connect(mNleView, &NleTimelineView::logMessage, this,
                [this](const QString &msg) {
            if (mMainWindow) { mMainWindow->statusBar()->showMessage(msg, 4000); }
        });
    }
    mMainLayout->addWidget(mNlePage);

    mNleController = new NleTimelineController(mDocument, mNleModel,
                                               mNleView, this);
    mNleModel->refreshFromDocument();

    // NLE toolbar group must be created after the timeline view
    setupNleActions();

    previewFinished();

    connect(&mDocument, &Document::activeSceneSet,
            this, &TimelineDockWidget::updateSettingsForCurrentCanvas);

    // Kdenlive-style direct playback (composites on demand, audio
    // clock master, frame drops on slow compositions, half-res preview)
    mDirectPlayer = new DirectPlayer(mDocument, *AudioHandler::sInstance, this);
    connect(mDirectPlayer, &DirectPlayer::started,
            this, &TimelineDockWidget::previewBeingPlayed);
    connect(mDirectPlayer, &DirectPlayer::finished,
            this, &TimelineDockWidget::previewFinished);
    connect(mDirectPlayer, &DirectPlayer::frameChanged,
            this, [this](const int frame) {
        // NLE playhead follows playback without a panel rebuild
        if (mNleView) { mNleView->setPlayheadFrame(frame); }
    });

    setupPropertyShortcuts();

}

void TimelineDockWidget::setupNleActions()
{
    if (!mToolBar || !mNleView || !mNleModel) { return; }

    // ---- editing tool cluster at the left of the toolbar: the tool
    // decides what a plain click on a clip does (PR toolbox style) ----
    const auto first = mToolBar->actions().isEmpty() ?
                nullptr : mToolBar->actions().constFirst();
    QAction *anchor = first; // fixed anchor: sequential inserts keep order
    const auto insertAct = [this, anchor](QAction * const a) {
        if (anchor) { mToolBar->insertAction(anchor, a); }
        else { mToolBar->addAction(a); }
    };
    const auto insertSep = [this, anchor]() {
        if (anchor) { return mToolBar->insertSeparator(anchor); }
        mToolBar->addSeparator();
        return static_cast<QAction*>(nullptr);
    };
    mNleToolSeps[0] = insertSep();

    mToolGroup = new QActionGroup(this);
    mToolGroup->setExclusive(true);
    using ET = NleTimelineView::EditTool;
    const auto addToolAct = [this, &insertAct](
            const QString &text, const char * const svg,
            const ET tool, const int slot) {
        auto * const a = new QAction(
                    QIcon(nleGlyphPixmap(svg, QColor(0xc8, 0xc8, 0xc8))),
                    text, this);
        a->setCheckable(true);
        a->setChecked(tool == ET::Select);
        a->setToolTip(text);
        a->setData(text);
        mToolGroup->addAction(a);
        insertAct(a);
        mMainWindow->cmdAddAction(a);
        connect(a, &QAction::toggled, this,
                [this, a, svg, tool](const bool on) {
            const QColor accent = ThemeSupport::getThemeHighlightColor();
            QColor onGlyph(0xff, 0xff, 0xff);
            if (accent.lightness() > 150) {
                onGlyph = ThemeSupport::getThemeHighlightDarkerColor().darker(160);
            }
            a->setIcon(QIcon(nleGlyphPixmap(
                        svg, on ? onGlyph : QColor(0xc8, 0xc8, 0xc8))));
            if (on && mNleView) { mNleView->setTool(tool); }
        });
        if (slot >= 0 && slot < 6) { mToolActs[slot] = a; }
        return a;
    };
    addToolAct(tr("选择工具 (V)"), kNleToolSelectSvg, ET::Select,
               static_cast<int>(ET::Select));
    addToolAct(tr("剪刀工具 (B)"), kNleToolRazorSvg, ET::Razor,
               static_cast<int>(ET::Razor));
    addToolAct(tr("向后选择工具 (Shift+A)"), kNleToolBackSvg, ET::TrackBackward,
               static_cast<int>(ET::TrackBackward));
    addToolAct(tr("向前选择工具 (A)"), kNleToolFwdSvg, ET::TrackForward,
               static_cast<int>(ET::TrackForward));
    addToolAct(tr("间隔工具 (D)"), kNleToolSpacerSvg, ET::Spacer,
               static_cast<int>(ET::Spacer));
    addToolAct(tr("手型工具 (H)"), kNleToolHandSvg, ET::Hand,
               static_cast<int>(ET::Hand));
    // keyboard tool switches stay in sync with the buttons (setTool
    // no-ops on the same tool, so the re-check cannot loop)
    connect(mNleView, &NleTimelineView::toolChanged, this,
            [this](const int tool) {
        if (tool < 0 || tool >= int(ET::Hand) + 1) { return; }
        auto * const a = mToolActs[tool];
        if (a && !a->isChecked()) { a->setChecked(true); }
    });
    mNleToolSeps[1] = insertSep();

    // split at the playhead (CapCut 分割): selected clips, or every
    // unlocked-track clip under the playhead when nothing is selected
    mNleSplitAtAct = new QAction(
                QIcon(nleGlyphPixmap(kNleSplitSvg, QColor(0xc8, 0xc8, 0xc8))),
                tr("分割"), this);
    mNleSplitAtAct->setToolTip(
                tr("在播放头分割选中块；未选中时分割播放头下所有块（C）"));
    mNleSplitAtAct->setData(mNleSplitAtAct->toolTip());
    connect(mNleSplitAtAct, &QAction::triggered, this, [this]() {
        if (mNleView) { mNleView->splitAtPlayhead(); }
    });
    insertAct(mNleSplitAtAct);
    mMainWindow->cmdAddAction(mNleSplitAtAct);

    // freeze the selection at the playhead (CapCut 定格)
    mNleFreezeAct = new QAction(
                QIcon(nleGlyphPixmap(kNleFreezeSvg, QColor(0xc8, 0xc8, 0xc8))),
                tr("定格"), this);
    mNleFreezeAct->setToolTip(
                tr("在播放头处定格选中块：切点起冻结为静止画面到块尾"));
    mNleFreezeAct->setData(mNleFreezeAct->toolTip());
    connect(mNleFreezeAct, &QAction::triggered, mNleView,
            &NleTimelineView::freezeAtPlayhead);
    insertAct(mNleFreezeAct);
    mMainWindow->cmdAddAction(mNleFreezeAct);

    // undo / redo right on the timeline toolbar (CapCut layout); the
    // model refreshes itself after the scene undo
    mNleUndoAct = new QAction(
                QIcon(nleGlyphPixmap(kNleUndoSvg, QColor(0xc8, 0xc8, 0xc8))),
                tr("撤销"), this);
    mNleUndoAct->setToolTip(tr("撤销上一步（Ctrl+Z）"));
    mNleUndoAct->setData(mNleUndoAct->toolTip());
    connect(mNleUndoAct, &QAction::triggered, this, [this]() {
        if (mNleModel) { mNleModel->undo(); }
    });
    insertAct(mNleUndoAct);
    mMainWindow->cmdAddAction(mNleUndoAct);

    mNleRedoAct = new QAction(
                QIcon(nleGlyphPixmap(kNleRedoSvg, QColor(0xc8, 0xc8, 0xc8))),
                tr("重做"), this);
    mNleRedoAct->setToolTip(tr("重做下一步（Ctrl+Shift+Z）"));
    mNleRedoAct->setData(mNleRedoAct->toolTip());
    connect(mNleRedoAct, &QAction::triggered, this, [this]() {
        if (mNleModel) { mNleModel->redo(); }
    });
    insertAct(mNleRedoAct);
    mMainWindow->cmdAddAction(mNleRedoAct);
    mNleToolSeps[2] = insertSep();

    // NLE editing group (joins the tool cluster at the left; CapCut
    // keeps every editing control in one contiguous toolbar block)
    mNleDeleteAct = new QAction(tr("删除块"), this);
    mNleDeleteAct->setToolTip(tr("删除选中的块（Delete）；Shift=波纹删除，后续块左移补洞"));
    mNleDeleteAct->setData(mNleDeleteAct->toolTip());
    connect(mNleDeleteAct, &QAction::triggered, this, [this]() {
        if (mNleView) { mNleView->requestDelete(false); }
    });
    insertAct(mNleDeleteAct);
    mMainWindow->cmdAddAction(mNleDeleteAct);

    mNleRippleAct = new QAction(tr("波纹删除"), this);
    mNleRippleAct->setToolTip(tr("删除选中的块并让同轨后续块左移补洞（Shift+Delete）"));
    mNleRippleAct->setData(mNleRippleAct->toolTip());
    connect(mNleRippleAct, &QAction::triggered, this, [this]() {
        if (mNleView) { mNleView->requestDelete(true); }
    });
    insertAct(mNleRippleAct);
    mMainWindow->cmdAddAction(mNleRippleAct);

    mMagneticAct = new QAction(
                QIcon(nleMagneticPixmap(QColor(0xc8, 0xc8, 0xc8))), QString(), this);
    mMagneticAct->setCheckable(true);
    // the button mirrors the model flag restored in the ctor (no
    // toggled side effects during the sync)
    mMagneticAct->blockSignals(true);
    mMagneticAct->setChecked(mNleModel->magnetic());
    mMagneticAct->blockSignals(false);
    // blockSignals skipped the toggled handler: sync the glyph too
    {
        const QColor accent = ThemeSupport::getThemeHighlightColor();
        QColor onGlyph(0xff, 0xff, 0xff);
        if (accent.lightness() > 150) {
            onGlyph = ThemeSupport::getThemeHighlightDarkerColor().darker(160);
        }
        mMagneticAct->setIcon(QIcon(nleMagneticPixmap(
                    mNleModel->magnetic() ? onGlyph
                                          : QColor(0xc8, 0xc8, 0xc8))));
    }
    mMagneticAct->setToolTip(tr("磁吸（kdenlive 式）：拖拽块时其余块实时让位重排，轨道保持无间隙"));
    connect(mMagneticAct, &QAction::toggled, this, [this](const bool on) {
        if (!mMagneticAct) { return; }
        const QColor accent = ThemeSupport::getThemeHighlightColor();
        QColor onGlyph(0xff, 0xff, 0xff);
        if (accent.lightness() > 150) {
            onGlyph = ThemeSupport::getThemeHighlightDarkerColor().darker(160);
        }
        mMagneticAct->setIcon(QIcon(nleMagneticPixmap(
                        on ? onGlyph : QColor(0xc8, 0xc8, 0xc8))));
        AppSupport::setSettings(QStringLiteral("ui"),
                                QStringLiteral("timelineMagnetic"), on);
        if (mNleModel) { mNleModel->setMagnetic(on); }
    });
    insertAct(mMagneticAct);
    mMainWindow->cmdAddAction(mMagneticAct);

    // kdenlive insert/overwrite toggle: when on, a drop pushes the
    // touched run right instead of requiring free space; Ctrl flips
    // it for a single gesture
    mNleInsertAct = new QAction(tr("插入模式"), this);
    mNleInsertAct->setCheckable(true);
    mNleInsertAct->setToolTip(
                tr("插入模式（kdenlive）：拖放时落点处的现有块整体右移让位；按住 Ctrl 可临时反转"));
    mNleInsertAct->setData(mNleInsertAct->toolTip());
    connect(mNleInsertAct, &QAction::toggled, this, [this](const bool on) {
        if (mNleView) { mNleView->setInsertMode(on); }
    });
    insertAct(mNleInsertAct);
    mMainWindow->cmdAddAction(mNleInsertAct);

    mNleZoomFitAct = new QAction(tr("适配"), this);
    mNleZoomFitAct->setToolTip(tr("时间轴缩放适配窗口宽度"));
    mNleZoomFitAct->setData(mNleZoomFitAct->toolTip());
    connect(mNleZoomFitAct, &QAction::triggered,
            mNleView, &NleTimelineView::zoomFit);
    insertAct(mNleZoomFitAct);
    mMainWindow->cmdAddAction(mNleZoomFitAct);
}

void TimelineDockWidget::updateFrameRange(const FrameRange &range)
{
    mRenderProgress->setRange(range.fMin, range.fMax);
}

void TimelineDockWidget::handleCurrentFrameChanged(int frame)
{
    mCurrentFrameSpin->setValue(frame);
    if (mRenderProgress->isVisible()) { mRenderProgress->setValue(frame); }
}

void TimelineDockWidget::showRenderStatus(bool show)
{
    if (!show) { mRenderProgress->setValue(0); }
    mCurrentFrameSpinAct->setVisible(!show);
    mRenderProgressAct->setVisible(show);
}

void TimelineDockWidget::addSpacer()
{
    const auto spacer = new QWidget(this);
    spacer->setSizePolicy(QSizePolicy::Expanding,
                          QSizePolicy::Minimum);
    mToolBar->addWidget(spacer);
}

void TimelineDockWidget::addBlankAction()
{
    const auto act = mToolBar->addAction(QString());
    act->setEnabled(false);
}

void TimelineDockWidget::setLoop(const bool loop)
{
    RenderHandler::sInstance->setLoop(loop);
}

// quick PNG export of the current frame at FULL (100%) resolution:
// when the preview runs at a lower resolution (default 50%), the
// scene resolution is bumped to 1.0, the fresh frame is awaited and
// the previous resolution restored. Destination: the snapshot path
// from the preferences (default: Desktop)
void TimelineDockWidget::snapshotCurrentFrame()
{
    const auto scene = *mDocument.fActiveScene;
    if(!scene) return;
    const int frame = scene->getCurrentFrame();
    const auto status = [this](const QString& msg) {
        mMainWindow->statusBar()->showMessage(msg, 5000);
    };
    // destination: preferences setting, fallback Desktop
    QString dir = AppSupport::getSettings(QStringLiteral("snapshots"),
                                          QStringLiteral("dir")).toString();
    if(dir.isEmpty() || !QDir(dir).exists()) {
        dir = QStandardPaths::writableLocation(
                    QStandardPaths::DesktopLocation);
    }
    static const QRegularExpression badChars(
                QStringLiteral("[\\\\/:*?\"<>|]"));
    QString sceneName = scene->prp_getName();
    sceneName.replace(badChars, QStringLiteral("_"));
    const QString name = QStringLiteral("%1_f%2_%3.png")
            .arg(sceneName)
            .arg(frame)
            .arg(QTime::currentTime().toString(QStringLiteral("HHmmss")));
    const QString path = dir + QStringLiteral("/") + name;

    const qreal savedRes = scene->getResolution();
    const auto saveAndReport = [status, path](const sk_sp<SkImage>& img) {
        SkiaHelpers::saveImage(path, img,
                               SkEncodedImageFormat::kPNG, 100);
        if(QFile::exists(path)) {
            status(tr("Snapshot saved: %1").arg(path));
        } else {
            status(tr("Failed to save snapshot: %1").arg(path));
        }
    };

    const auto contRaw = scene->getSceneFramesHandler().atFrame(frame);
    const auto frameCont = dynamic_cast<SceneFrameContainer*>(contRaw);
    const sk_sp<SkImage> img = frameCont ? frameCont->getImage() : nullptr;
    if(img && (savedRes > 0.999 || frameCont->fResolution > 0.999)) {
        // already at full resolution
        saveAndReport(img);
        return;
    }
    if(savedRes > 0.999) {
        status(tr("No rendered frame available yet - wait for the "
                  "preview to render this frame"));
        return;
    }
    // bump the scene to 100% and wait for the fresh full-res frame;
    // actionFinished() actually SCHEDULES the re-render (the video
    // export does the same setResolution + actionFinished dance)
    scene->setResolution(1.);
    mDocument.actionFinished();
    QPointer<Canvas> sceneQ(scene);
    const bool restoreRes = true;
    const qreal resToRestore = savedRes;
    auto* const timer = new QTimer(this);
    auto tries = std::make_shared<int>(0);
    connect(timer, &QTimer::timeout, this,
            [this, timer, sceneQ, frame, path, restoreRes, resToRestore,
             tries, status, saveAndReport]() {
        if(!sceneQ) {
            timer->stop();
            timer->deleteLater();
            return;
        }
        const auto cont = sceneQ->getSceneFramesHandler().atFrame(frame);
        const auto fc = dynamic_cast<SceneFrameContainer*>(cont);
        const bool ready = fc && fc->getImage() &&
                           fc->fResolution > 0.999;
        if(!ready) {
            if(++(*tries) > 100) { // ~10s timeout
                timer->stop();
                timer->deleteLater();
                sceneQ->setResolution(resToRestore);
                mDocument.actionFinished();
                status(tr("Snapshot timed out - the frame did not "
                          "render in time"));
            }
            return;
        }
        timer->stop();
        timer->deleteLater();
        saveAndReport(fc->getImage());
        sceneQ->setResolution(resToRestore);
        mDocument.actionFinished();
    });
    timer->start(100);
    status(tr("Rendering snapshot at 100% resolution..."));
}

void TimelineDockWidget::spaceToggle()
{
    const auto state = RenderHandler::sInstance->currentPreviewState();
    // diagnostic: distinguishes "Space never reached this slot" from
    // "reached but wrong branch" when users report dead Space keys
    qWarning() << "[SPACE] spaceToggle state=" << int(state)
               << "directPlay=" << (mDirectPlayer ? mDirectPlayer->playing() : false);
    // Space = play <-> full stop: any preview activity (rendering,
    // playing, paused) stops the preview completely; the next press
    // starts playback again
    if (state == PreviewState::rendering ||
        state == PreviewState::playing ||
        state == PreviewState::paused) {
        interruptPreview();
    } else if (mDirectPlayer && mDirectPlayer->playing()) {
        mDirectPlayer->stop();
    } else {
        // AE-style start: when the range ahead is not fully cached,
        // warm the cache first (visible progress, auto-plays when
        // done); with everything cached play straight from memory
        bool started = false;
        if (eSettings::instance().fPreviewCache) {
            const auto scene = *mDocument.fActiveScene;
            bool warm = false;
            if (scene) {
                const int cur = scene->anim_getCurrentAbsFrame();
                const int max = scene->getFrameRange().fMax;
                warm = scene->getSceneFramesHandler()
                            .firstEmptyFrameAtOrAfter(cur) > max &&
                       scene->sceneFramesCacheIsFresh();
            }
            if (warm) {
                started = RenderHandler::sInstance->playPreview();
                if (!started) {
                    // playPreview only refuses on an empty in/out range;
                    // warming the cache would fail the same way, so tell
                    // the user instead of silently rendering nothing
                    mMainWindow->statusBar()->showMessage(
                            tr("Cannot play: the preview range is empty - "
                               "check the In/Out points"), 5000);
                }
            } else {
                renderPreview();
                started = true;
            }
        } else {
            started = playPreview();
        }
        qWarning() << "[SPACE] start playPreview=" << started
                   << "activeScene="
                   << (*mDocument.fActiveScene ? "yes" : "null");
    }
}

bool TimelineDockWidget::processKeyPress(QKeyEvent *event)
{
    const int key = event->key();
    const auto mods = event->modifiers();
    const auto state = RenderHandler::sInstance->currentPreviewState();
    const bool jumpFrame = (mods & (Qt::ShiftModifier | Qt::AltModifier)) == (Qt::ShiftModifier | Qt::AltModifier);
    if (key == Qt::Key_Escape) { // stop playback
        if (state != PreviewState::stopped ||
            (mDirectPlayer && mDirectPlayer->playing())) { interruptPreview(); }
        else { return false; }

    } else if (key == Qt::Key_Space && (mods & Qt::ShiftModifier)) { // play from first frame
        /*const auto scene = *mDocument.fActiveScene;
        if (!scene) { return false; }
        if (state != PreviewState::stopped) { interruptPreview(); }
        scene->anim_setAbsFrame(scene->getFrameRange().fMin);
        renderPreview();*/
        if (!setPreviewFromStart(state)) { return false; }
    } else if (key == Qt::Key_Space) { // play <-> full stop
        // keep both Space paths (window shortcut and timeline keys)
        // on the exact same behavior
        spaceToggle();
    } else if (key == Qt::Key_K && mods == Qt::NoModifier) { // split clip
        splitClip();
    } else if (key == Qt::Key_M) { // set marker
        setMarker();
    } else if (key == Qt::Key_I || key == Qt::Key_O) { // set frame in/out
        switch(key) {
            case Qt::Key_I: setIn(); break;
            case Qt::Key_O: setOut(); break;
            default:;
        }
    } else if ((key == Qt::Key_BracketLeft || key == Qt::Key_BracketRight) &&
               mods == Qt::NoModifier) { // AE [/]: slide layer in/out point to the playhead
        const auto scene = *mDocument.fActiveScene;
        if (!scene) { return false; }
        if (key == Qt::Key_BracketLeft) { scene->moveSelectedBoxesInPointToCurrent(); }
        else { scene->moveSelectedBoxesOutPointToCurrent(); }
    } else if (key == Qt::Key_F2) { // rename the last selected layer
        const auto scene = *mDocument.fActiveScene;
        if (!scene) { return false; }
        const auto boxes = scene->getSelectedBoxesList();
        if (boxes.isEmpty()) { return false; }
        PropertyNameDialog::sRenameBox(boxes.last(), this);
    } else if (key == Qt::Key_Right && !(mods & Qt::ControlModifier)) {
        if (jumpFrame) { // jump to next scene quarter
            jumpToIntermediateFrame(true);
        } else { // next frame
            mDocument.incActiveSceneFrame();
        }
    } else if (key == Qt::Key_Left && !(mods & Qt::ControlModifier)) {
        if (jumpFrame) { // jump to previous scene quarter
            jumpToIntermediateFrame(false);
        } else { // previous frame
            mDocument.decActiveSceneFrame();
        }
    } else if (key == Qt::Key_Down && !(mods & Qt::ControlModifier)) { // previous keyframe
        /*const auto scene = *mDocument.fActiveScene;
        if (!scene) { return false; }
        int targetFrame;
        const int frame = mDocument.getActiveSceneFrame();
        if (scene->anim_prevRelFrameWithKey(frame, targetFrame)) {
            mDocument.setActiveSceneFrame(targetFrame);
        }*/
        if (!setPrevKeyframe()) { return false; }
    } else if (key == Qt::Key_Up && !(mods & Qt::ControlModifier)) { // next keyframe
        /*const auto scene = *mDocument.fActiveScene;
        if (!scene) { return false; }
        int targetFrame;
        const int frame = mDocument.getActiveSceneFrame();
        if (scene->anim_nextRelFrameWithKey(frame, targetFrame)) {
            mDocument.setActiveSceneFrame(targetFrame);
        }*/
        if (!setNextKeyframe()) { return false; }
    } else {
        return false;
    }
    return true;
}

void TimelineDockWidget::previewFinished()
{
    mPausedPreviewState.first = false;
    if (const auto scene = *mDocument.fActiveScene) {
        scene->setGizmosSuppressed(false);
    }
    //setPlaying(false);
    mCurrentFrameSpinAct->setEnabled(true);
    showRenderStatus(false);
    mPlayFromBeginningButton->setDisabled(false);
    mStopButton->setDisabled(true);
    mPlayButton->setIcon(QIcon::fromTheme("play"));
    mPlayButton->setText(tr("Play Preview"));
    disconnect(mPlayButton, nullptr, this, nullptr);
    connect(mPlayButton, &QAction::triggered,
            this, &TimelineDockWidget::renderPreview);
}

void TimelineDockWidget::previewBeingPlayed()
{
    if (const auto scene = *mDocument.fActiveScene) {
        scene->setGizmosSuppressed(true);
    }
    mCurrentFrameSpinAct->setEnabled(false);
    showRenderStatus(false);
    mPlayFromBeginningButton->setDisabled(true);
    mStopButton->setDisabled(false);
    mPlayButton->setIcon(QIcon::fromTheme("pause"));
    mPlayButton->setText(tr("Pause Preview"));
    disconnect(mPlayButton, nullptr, this, nullptr);
    connect(mPlayButton, &QAction::triggered,
            this, &TimelineDockWidget::pausePreview);
}

void TimelineDockWidget::previewBeingRendered()
{
    mCurrentFrameSpinAct->setEnabled(false);
    showRenderStatus(true);
    mPlayFromBeginningButton->setDisabled(true);
    mStopButton->setDisabled(false);
    mPlayButton->setIcon(QIcon::fromTheme("play"));
    mPlayButton->setText(tr("Play Preview"));
    disconnect(mPlayButton, nullptr, this, nullptr);
    connect(mPlayButton, &QAction::triggered,
            this, &TimelineDockWidget::playPreview);
}

void TimelineDockWidget::previewPaused()
{
    mPausedPreviewState = {true, mDocument.getActiveSceneFrame()};

    if (const auto scene = *mDocument.fActiveScene) {
        scene->setGizmosSuppressed(false);
    }
    mCurrentFrameSpinAct->setEnabled(true);
    showRenderStatus(false);
    mPlayFromBeginningButton->setDisabled(true);
    mStopButton->setDisabled(false);
    mPlayButton->setIcon(QIcon::fromTheme("play"));
    mPlayButton->setText(tr("Resume Preview"));
    disconnect(mPlayButton, nullptr, this, nullptr);
    connect(mPlayButton, &QAction::triggered,
            this, &TimelineDockWidget::resumePreview);
}

bool TimelineDockWidget::setPreviewFromStart(PreviewState state)
{
    const auto scene = *mDocument.fActiveScene;
    if (!scene) { return false; }
    if (state != PreviewState::stopped) { interruptPreview(); }
    scene->anim_setAbsFrame(scene->getFrameRange().fMin);
    renderPreview();
    return true;
}

bool TimelineDockWidget::setNextKeyframe()
{
    const auto scene = *mDocument.fActiveScene;
    if (!scene) { return false; }
    int targetFrame;
    const int frame = mDocument.getActiveSceneFrame();
    if (scene->anim_nextRelFrameWithKey(frame, targetFrame)) {
        mDocument.setActiveSceneFrame(targetFrame);
    }
    return true;
}

bool TimelineDockWidget::setPrevKeyframe()
{
    const auto scene = *mDocument.fActiveScene;
    if (!scene) { return false; }
    int targetFrame;
    const int frame = mDocument.getActiveSceneFrame();
    if (scene->anim_prevRelFrameWithKey(frame, targetFrame)) {
        mDocument.setActiveSceneFrame(targetFrame);
    }
    return true;
}

void TimelineDockWidget::resumePreview()
{
    if (eSettings::instance().fPreviewCache) {
        if (mPausedPreviewState.first) {
            const int frame = mDocument.getActiveSceneFrame();
            if (mPausedPreviewState.second != frame) {
                qDebug() << "set new start frame for preview" << frame;
                RenderHandler::sInstance->setPreviewFrame(frame);
                mPausedPreviewState.first = false;
            }
        }
        RenderHandler::sInstance->resumePreview();
    } else { setStepPreviewStart(); }
}

void TimelineDockWidget::setStepPreviewStop(const bool pause)
{
    // direct playback has no paused state: Space is play <-> full stop
    if (mDirectPlayer) { mDirectPlayer->stop(); }
    if (pause) { previewPaused(); }
    else { previewFinished(); }
}

void TimelineDockWidget::setStepPreviewStart()
{
    if (eSettings::instance().fPreviewCache) { return; }
    if (!mDirectPlayer) { return; }
    mDirectPlayer->setLoop(mLoopButton && mLoopButton->isChecked());
    mDirectPlayer->setPlayResolution(0.5);
    if (!mDirectPlayer->play()) {
        previewFinished();
        mMainWindow->statusBar()->showMessage(
                    tr("Cannot play: the preview range is empty - "
                       "check the In/Out points"), 5000);
    }
}

void TimelineDockWidget::gotoFrame(int frame)
{
    const auto scene = *mDocument.fActiveScene;
    if (!scene) { return; }
    scene->anim_setAbsFrame(frame);
    mDocument.actionFinished();
}

void TimelineDockWidget::updateButtonsVisibility(const CanvasMode mode)
{
    Q_UNUSED(mode)
}

void TimelineDockWidget::pausePreview()
{
    if (eSettings::instance().fPreviewCache) {
        RenderHandler::sInstance->pausePreview();
    } else { setStepPreviewStop(); }
}

bool TimelineDockWidget::playPreview()
{
    if (eSettings::instance().fPreviewCache) {
        return RenderHandler::sInstance->playPreview();
    }
    setStepPreviewStart();
    return true;
}

void TimelineDockWidget::renderPreview()
{
    if (eSettings::instance().fPreviewCache) {
        RenderHandler::sInstance->renderPreview();
    } else { setStepPreviewStart(); }
}

void TimelineDockWidget::interruptPreview()
{
    if (eSettings::instance().fPreviewCache) {
        RenderHandler::sInstance->interruptPreview();
    } else { setStepPreviewStop(); }
}

void TimelineDockWidget::updateSettingsForCurrentCanvas(Canvas* const canvas)
{
    if (!canvas) { return; }

    // drop the previous scene's connections first: a stale scene's
    // frame/range changes would keep driving the dock, and revisiting
    // a scene would stack duplicate connections
    if (mConnectedCanvas) {
        disconnect(mConnectedCanvas, nullptr, this, nullptr);
    }
    mConnectedCanvas = canvas;

    // keep the clip-to-canvas toggle in sync with the scene state (the
    // view-menu entry and the C shortcut can change it elsewhere)
    mClipCanvasButton->blockSignals(true);
    mClipCanvasButton->setChecked(canvas->clipToCanvas());
    mClipCanvasButton->blockSignals(false);

    const auto range = canvas->getFrameRange();
    updateFrameRange(range);
    handleCurrentFrameChanged(canvas->anim_getCurrentAbsFrame());

    mCurrentFrameSpin->setDisplayTimeCode(canvas->getDisplayTimecode());

    mCurrentFrameSpin->updateFps(canvas->getFps());

    connect(canvas, &Canvas::fpsChanged,
            this, [this](const qreal fps) {
        mCurrentFrameSpin->updateFps(fps);
    });
    connect(canvas, &Canvas::displayTimeCodeChanged,
            this, [this](const bool enabled) {
        mCurrentFrameSpin->setDisplayTimeCode(enabled);
    });

    connect(canvas,
            &Canvas::newFrameRange,
            this, [this](const FrameRange range) {
            updateFrameRange(range);
    });
    connect(canvas, &Canvas::currentFrameChanged,
            this, &TimelineDockWidget::handleCurrentFrameChanged);

    update(); // needed for loaded markers
}

void TimelineDockWidget::stopPreview()
{
    const auto state = RenderHandler::sInstance->currentPreviewState();
    switch (state) {
    case PreviewState::paused:
        interruptPreview();
        break;
    case PreviewState::playing:
    case PreviewState::rendering:
        interruptPreview();
        renderPreview();
        break;
    default:;
    }
}

void TimelineDockWidget::setIn()
{
    const auto scene = *mDocument.fActiveScene;
    if (!scene) { return; }
    const auto frame = scene->getCurrentFrame();
    if (scene->getFrameOut().enabled) {
        if (frame >= scene->getFrameOut().frame) {
            // a refused edit must be visible, not silent (I shortcut)
            mMainWindow->statusBar()->showMessage(
                    tr("In point must be before the Out point"), 4000);
            return;
        }
    }
    bool apply = frame == 0 ? true : (scene->getFrameIn().frame != frame);
    scene->setFrameIn(apply, frame);
}

void TimelineDockWidget::setOut()
{
    const auto scene = *mDocument.fActiveScene;
    if (!scene) { return; }
    const auto frame = scene->getCurrentFrame();
    if (scene->getFrameIn().enabled) {
        if (frame <= scene->getFrameIn().frame) {
            mMainWindow->statusBar()->showMessage(
                    tr("Out point must be after the In point"), 4000);
            return;
        }
    }
    bool apply = (scene->getFrameOut().frame != frame);
    scene->setFrameOut(apply, frame);
}

void TimelineDockWidget::setMarker()
{
    const auto scene = *mDocument.fActiveScene;
    if (!scene) { return; }
    const auto frame = scene->getCurrentFrame();
    scene->setMarker(frame);
}

void TimelineDockWidget::splitClip()
{
    // unified with the NLE timeline split (CapCut semantics: selected
    // clips at the current frame, fallback every unlocked-track clip
    // under it)
    if (mNleModel) {
        mNleModel->requestSplitAtFrame(mDocument.getActiveSceneFrame());
    }
}

void TimelineDockWidget::jumpToIntermediateFrame(bool forward) {
    const auto scene = *mDocument.fActiveScene;
    if (!scene) { return; }
    
    const auto range = scene->getFrameRange();
    const int currentFrame = scene->anim_getCurrentAbsFrame();
    const int totalFrames = range.fMax - range.fMin;
    const int quarterFrame = range.fMin + qRound(totalFrames * 0.25);
    const int middleFrame = range.fMin + qRound(totalFrames * 0.5);
    const int threeQuarterFrame = range.fMin + qRound(totalFrames * 0.75);
    
    if (forward) {
        if (currentFrame < quarterFrame) {
            scene->anim_setAbsFrame(quarterFrame);
        } else if (currentFrame < middleFrame) {
            scene->anim_setAbsFrame(middleFrame);
        } else if (currentFrame < threeQuarterFrame) {
            scene->anim_setAbsFrame(threeQuarterFrame);
        } else {
            scene->anim_setAbsFrame(range.fMax);
        }
    } else {
        if (currentFrame > threeQuarterFrame) {
            scene->anim_setAbsFrame(threeQuarterFrame);
        } else if (currentFrame > middleFrame) {
            scene->anim_setAbsFrame(middleFrame);
        } else if (currentFrame > quarterFrame) {
            scene->anim_setAbsFrame(quarterFrame);
        } else {
            scene->anim_setAbsFrame(range.fMin);
        }
    }
    mDocument.actionFinished();
}



void TimelineDockWidget::showTransformProperty(const int which)
{
    Q_UNUSED(which)
    // property-row reveal lived in the classic keyframe timeline, which
    // is retired; the properties panel stays the home for property rows
    if (mMainWindow) {
        mMainWindow->statusBar()->showMessage(
                    tr("属性快捷显示已随关键帧时间轴移除，请在属性面板操作"), 4000);
    }
}

void TimelineDockWidget::showAnimatedProperties()
{
    if (mMainWindow) {
        mMainWindow->statusBar()->showMessage(
                    tr("属性快捷显示已随关键帧时间轴移除，请在属性面板操作"), 4000);
    }
}

void TimelineDockWidget::matchSelectedToCanvas(const bool byWidth)
{
    const auto scene = *mDocument.fActiveScene;
    if (!scene) return;
    if (scene->getSelectedBoxesList().isEmpty()) return;
    scene->scaleSelectedBoxesToCanvas(byWidth);
    Document::sInstance->actionFinished();
    scene->updateAllBoxes(UpdateReason::userChange);
}

void TimelineDockWidget::setupPropertyShortcuts()
{
    const auto makeShortcut = [this](const QString &id,
                                     const std::function<void()> &fn) {
        const auto seq = AppSupport::getSettings("shortcuts",
                                                 id, "").toString();
        if (seq.isEmpty()) { return; }
        const auto keySeq = QKeySequence(seq);
        // user-configured property shortcuts take priority over
        // hardcoded action shortcuts (e.g. View->Timeline uses T);
        // with two identical shortcuts Qt treats them as ambiguous
        // and neither fires, so clear the conflicting one
        const auto clearConflicts = [this, keySeq]() {
            if (!mMainWindow) return;
            const auto acts = mMainWindow->findChildren<QAction*>();
            for (const auto a : acts) {
                if (a && a->shortcut() == keySeq) {
                    a->setShortcut(QKeySequence());
                }
            }
        };
        clearConflicts();
        // toolbox/menu actions may be created after this dock, run
        // again once everything is built
        QTimer::singleShot(0, this, clearConflicts);
        const auto sc = new QShortcut(keySeq, this);
        connect(sc, &QShortcut::activated, this, fn);
    };
    makeShortcut("showAnchor",   [this]() { showTransformProperty(0); });
    makeShortcut("showPosition", [this]() { showTransformProperty(1); });
    makeShortcut("showScale",    [this]() { showTransformProperty(2); });
    makeShortcut("showRotation", [this]() { showTransformProperty(3); });
    makeShortcut("showOpacity",  [this]() { showTransformProperty(4); });
    makeShortcut("showAnimated", [this]() { showAnimatedProperties(); });
}
