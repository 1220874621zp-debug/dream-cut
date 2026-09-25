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

#ifndef BOXESLISTANIMATIONDOCKWIDGET_H
#define BOXESLISTANIMATIONDOCKWIDGET_H

#include <QWidget>
#include <QVBoxLayout>
#include <QSpinBox>
#include <QPushButton>
#include <QLabel>
#include <QScrollArea>
#include <QApplication>
#include <QScrollBar>
#include <QSlider>
#include <QComboBox>
#include <QMenuBar>
#include <QLineEdit>
#include <QWidgetAction>
#include <QToolBar>
#include <QStackedWidget>
#include <QToolButton>
#include <QProgressBar>
#include <QTimer>
#include <QPointer>

#include "smartPointers/ememory.h"
#include "framerange.h"
#include "widgets/qdoubleslider.h"
#include "renderhandler.h"
#include "widgets/framespinbox.h"

class FrameScrollBar;
class MainWindow;
class AnimationDockWidget;
class RenderWidget;
class ActionButton;
class Canvas;
class Document;
class LayoutHandler;
class BrushContexedWrapper;
class NleTimelineModel;
class NleTimelineView;
class NleTimelineController;
class DirectPlayer;

enum class CanvasMode : short;

class TimelineDockWidget : public QWidget
{
    Q_OBJECT
public:
    explicit TimelineDockWidget(Document &document,
                                LayoutHandler* const layoutH,
                                MainWindow * const parent);

    // A sane fixed size hint keeps the dock at a reasonable initial
    // height; the content-driven hint would otherwise claim most of
    // the window. This does not limit manual resizing (only the
    // minimum size hint does).
    QSize sizeHint() const override { return QSize(600, 300); }

    bool processKeyPress(QKeyEvent *event);
    // keeps the checkable top-view toolbar button in sync with the
    // floating window open/closed state (called by MainWindow)
    void previewFinished();
    void previewBeingPlayed();
    void previewBeingRendered();
    void previewPaused();
    void stepPreview();

    bool setPreviewFromStart(PreviewState state);
    // keyframe navigation (timeline Up/Down keys); toolbar buttons
    // were retired with the NLE toolbar slim-down
    bool setNextKeyframe();
    bool setPrevKeyframe();

    void updateSettingsForCurrentCanvas(Canvas * const canvas);

    void stopPreview();

    void setIn();
    void setOut();
    void setMarker();
    void splitClip();

public:
    // instant play/pause for the Space shortcut: bypasses the
    // preview-cache render pass (no flash, one press per toggle)
    void spaceToggle();

    // NLE 面板模型访问（特效面板转场卡片应用入口经 MainWindow 转发）
    NleTimelineModel *nleModel() const { return mNleModel; }
    // NLE 视图播放头（转场卡片"应用"的目标交界判定帧）
    int nlePlayheadFrame() const;

private:
    void setLoop(const bool loop);
    // quick PNG export of the current canvas frame
    void snapshotCurrentFrame();
    void interruptPreview();
    void jumpToIntermediateFrame(bool forward);

    // AE-like property reveal shortcuts: expanded property rows of
    // the selected layers in the old classic timeline; the properties
    // panel is their home now (status-bar notice kept for the keys)
    void showTransformProperty(const int which); // 0 pivot 1 pos 2 scale 3 rot 4 opacity
    void showAnimatedProperties();               // U key behavior
    void setupPropertyShortcuts();

    // uniformly scale the selected layers so their width/height matches
    // the canvas (see Canvas::scaleSelectedBoxesToCanvas)
    void matchSelectedToCanvas(const bool byWidth);

    bool playPreview();
    void renderPreview();
    void pausePreview();
    void resumePreview();
    void setStepPreviewStop(const bool pause = false);
    void setStepPreviewStart();

    void updateButtonsVisibility(const CanvasMode mode);

    void updateFrameRange(const FrameRange &range);
    void handleCurrentFrameChanged(int frame);

    void showRenderStatus(bool show);

    void addSpacer();
    void addBlankAction();

    // the kdenlive-style editing timeline (model + view + controller)
    // is the dock's only content (classic keyframe stack retired)
    void setupNleActions();

    Document& mDocument;
    MainWindow* const mMainWindow;
    QWidget *mNlePage = nullptr;
    NleTimelineModel *mNleModel = nullptr;
    NleTimelineView *mNleView = nullptr;
    NleTimelineController *mNleController = nullptr;
    DirectPlayer *mDirectPlayer = nullptr;

    QToolBar *mToolBar;

    QVBoxLayout *mMainLayout;

    QAction *mPlayFromBeginningButton;
    QAction *mPlayButton;
    QAction *mStopButton;
    QAction *mLoopButton;
    QAction *mSnapshotButton = nullptr;
    QAction *mSafeFramesButton = nullptr;
    QAction *mClipCanvasButton = nullptr;
    QAction *mRulersButton = nullptr;
    QAction *mTransparencyGridButton = nullptr;
    QAction *mMatchCanvasWidthButton = nullptr;
    QAction *mMatchCanvasHeightButton = nullptr;

    QAction *mFrameRewindAct;
    QAction *mFrameFastForwardAct;
    QSlider *mZoomSlider = nullptr;

    // NLE mode toolbar group
    // PR/CapCut-style editing tools (checkable, exclusive): order
    // matches NleTimelineView::EditTool (5 tools incl. spacer)
    class QActionGroup *mToolGroup = nullptr;
    QAction *mToolActs[6] = { nullptr, nullptr, nullptr, nullptr, nullptr, nullptr };
    QAction *mNleToolSeps[3] = { nullptr, nullptr, nullptr };
    QToolButton *mNleAddBtn = nullptr;
    QAction *mNleSplitAtAct = nullptr;
    QAction *mNleFreezeAct = nullptr;
    QAction *mNleReverseAct = nullptr;
    QAction *mNleUndoAct = nullptr;
    QAction *mNleRedoAct = nullptr;
    QAction *mMagneticAct = nullptr;
    QAction *mFollowAct = nullptr;
    QAction *mNleZoomFitAct = nullptr;

    QAction *mRenderProgressAct;
    QProgressBar *mRenderProgress;

    QPair<bool,int> mPausedPreviewState;

    // the scene this dock's per-scene connections are attached to;
    // switching scenes must disconnect it, otherwise a stale scene's
    // frame/range changes still drive the dock (and revisiting a
    // scene would stack duplicate connections)
    QPointer<Canvas> mConnectedCanvas;
};

#endif // BOXESLISTANIMATIONDOCKWIDGET_H
