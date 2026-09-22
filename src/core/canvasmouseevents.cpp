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

#include "canvas.h"
#include "Boxes/textbox.h"
#include "Boxes/rectangle.h"
#include "Boxes/circle.h"
#include "Boxes/smartvectorpath.h"
#include "Private/document.h"
#include "MovablePoints/pathpivot.h"
#include "eevent.h"
#include <QApplication>

void Canvas::mousePressEvent(const eMouseEvent &e) {
    if(mStylusDrawing) return;
    if(isPreviewingOrRendering()) return;
    if(e.fMouseGrabbing && e.fButton == Qt::LeftButton) return;
    if(e.fButton == Qt::LeftButton) {
        handleLeftButtonMousePress(e);
    }
}

void Canvas::mouseMoveEvent(const eMouseEvent &e)
{
    if (mStylusDrawing) { return; }
    if (isPreviewingOrRendering()) { return; }

    const bool leftPressed = e.fButtons & Qt::LeftButton;

    if (!leftPressed && !e.fMouseGrabbing) {
        const qreal invScaleUi = (qApp ? qApp->devicePixelRatio() : 1.0) * (1 / e.fScale);
        updateRotateHandleHover(e.fPos, invScaleUi);
        const auto lastHoveredBox = mHoveredBox;
        const auto lastHoveredPoint = mHoveredPoint_d;
        const auto lastNSegment = mHoveredNormalSegment;

        updateHovered(e);
        return;
    }

    if (leftPressed || e.fMouseGrabbing) {
        if (mMovesToSkip > 0) {
            mMovesToSkip--;
            return;
        }
        if (mStartTransform &&
            leftPressed &&
            !mGizmos.fState.rotatingFromHandle &&
            !mGizmos.fState.axisHandleActive &&
            !mGizmos.fState.scaleHandleActive &&
            !mGizmos.fState.shearHandleActive) {
            if ((mCurrentMode == CanvasMode::pointTransform &&
                !mPressedPoint && !mCurrentNormalSegment.isValid()) ||
               (mCurrentMode == CanvasMode::boxTransform &&
                !mPressedBox && !mPressedPoint)) {
                startSelectionAtPoint(e.fPos);
            }
        }
        if (mSelecting) {
            moveSecondSelectionPoint(e.fPos);
        } else if (mCurrentMode == CanvasMode::pointTransform) {
            handleMovePointMouseMove(e);
        } else if (mCurrentMode == CanvasMode::boxTransform) {
            if (mPressedPoint) {
                handleMovePointMouseMove(e);
            } else {
                handleMovePathMouseMove(e);
            }
        } else if (mCurrentMode == CanvasMode::pathCreate) {
            handleAddSmartPointMouseMove(e);
        } else if (mCurrentMode == CanvasMode::circleCreate) {
            if (!mCurrentMaskRectNodes.isEmpty()) {
                updateMaskCircleDrag(e);
            } else if (mCurrentCircle) {
                const QPointF anchor = mHasCreationPressPos ? mCreationPressPos : snapPosToGrid(e.fLastPressPos,
                                                                                                e.fModifiers,
                                                                                                false);
                const QPointF current = snapEventPos(e, false);
                const QPointF delta = current - anchor;
                if (e.shiftMod()) {
                    const qreal lenR = pointToLen(delta);
                    mCurrentCircle->moveRadiusesByAbs({lenR, lenR});
                } else {
                    mCurrentCircle->moveRadiusesByAbs(delta);
                }
            }
        } else if (mCurrentMode == CanvasMode::rectCreate) {
            if (!mCurrentMaskRectNodes.isEmpty()) {
                updateMaskRectDrag(e);
            } else if (mCurrentRectangle) {
                const QPointF anchor = mHasCreationPressPos ? mCreationPressPos : snapPosToGrid(e.fLastPressPos,
                                                                                                e.fModifiers,
                                                                                                false);
                const QPointF current = snapEventPos(e, false);
                const QPointF trans = current - anchor;
                if (e.shiftMod()) {
                    const qreal valF = qMax(trans.x(), trans.y());
                    mCurrentRectangle->moveSizePointByAbs({valF, valF});
                } else {
                    mCurrentRectangle->moveSizePointByAbs(trans);
                }
            }
        }
    }
    mStartTransform = false;

    if (!mSelecting && !e.fMouseGrabbing && leftPressed) {
        e.fGrabMouse();
    }
}

void Canvas::mouseReleaseEvent(const eMouseEvent &e)
{
    if (isPreviewingOrRendering()) { return; }
    if (e.fButton == Qt::RightButton) {
        switch(mCurrentMode) {
        case CanvasMode::circleCreate:
        case CanvasMode::rectCreate:
            // a right-click aborts an in-progress rect-mask drag
            // without dropping the half-drawn mask on the layer
            if (!mCurrentMaskRectNodes.isEmpty()) {
                if (mCurrentMaskRectPath) {
                    mCurrentMaskRectPath->removeFromParent_k();
                }
                mCurrentMaskRectNodes.clear();
                mCurrentMaskRectPath.clear();
            }
            clearSelectionAction();
            break;
        default:
            handleRightButtonMouseRelease(e);
        }
    }
    if (e.fButton != Qt::LeftButton) { return; }
    schedulePivotUpdate();
    handleLeftMouseRelease(e);

    mPressedBox = nullptr;
    mHoveredPoint_d = mPressedPoint;
    mPressedPoint = nullptr;
    if (e.fButton == Qt::LeftButton) {
        mHasCreationPressPos = false;
    }
}

#include "MovablePoints/smartnodepoint.h"
#include "MovablePoints/pathpointshandler.h"
#include "Private/document.h"
void Canvas::mouseDoubleClickEvent(const eMouseEvent &e) {
    if(e.fModifiers & Qt::ShiftModifier) return;
    mDoubleClick = true;

    if(mHoveredPoint_d) {
        if(mCurrentMode == CanvasMode::pointTransform &&
           mHoveredPoint_d->isSmartNodePoint()) {
            const auto adder = [this](MovablePoint* const pt) {
                addPointToSelection(pt);
            };
            const auto node = static_cast<SmartNodePoint*>(mHoveredPoint_d.data());
            node->getHandler()->addAllPointsToSelection(adder, mCurrentMode);
        }
    } else if(mHoveredBox) {
        if(enve_cast<ContainerBox*>(mHoveredBox) && !mHoveredBox->isLink()) {
            setCurrentBoxesGroup(static_cast<ContainerBox*>(mHoveredBox.data()));
            updateHovered(e);
        } else if((mCurrentMode == CanvasMode::boxTransform ||
                   mCurrentMode == CanvasMode::pointTransform) &&
                  enve_cast<TextBox*>(mHoveredBox)) {
            e.fReleaseMouse();
            emit openTextEditor();
        } else if(mCurrentMode == CanvasMode::boxTransform &&
                  enve_cast<SmartVectorPath*>(mHoveredBox)) {
            Document::sInstance->setCanvasMode(CanvasMode::pointTransform);
        }
    } else if(!mHoveredBox && !mHoveredPoint_d && !mHoveredNormalSegment.isValid()) {
        if(mCurrentContainer != this) {
            setCurrentBoxesGroup(mCurrentContainer->getParentGroup());
        }
    }
}

void Canvas::tabletEvent(const QTabletEvent * const e,
                         const QPointF &pos) {
    Q_UNUSED(e)
    Q_UNUSED(pos)
}
