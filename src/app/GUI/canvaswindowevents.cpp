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

#include "canvaswindow.h"
#include "canvas.h"
#include "GUI/global.h"
#include "Private/document.h"

#include <QResizeEvent>
#include <QEvent>

QPointF CanvasWindow::mapToCanvasCoord(const QPointF& windowCoord)
{
    qreal pixelRatio = devicePixelRatioF();
    return mViewTransform.inverted().scale(pixelRatio, pixelRatio).map(windowCoord);
}

void CanvasWindow::translateView(const QPointF &trans)
{
    if (!mCurrentCanvas) { return; }
    mViewTransform.translate(trans.x(), trans.y());
}

void CanvasWindow::zoomView(const qreal scaleBy,
                            const QPointF &absOrigin)
{
    if (!mCurrentCanvas) { return; }
    // 缩放钳制：防止滚轮连击把比例推到退化区间（mapToCanvasCoord
    // 求逆在极小/极大比例下精度崩坏，标尺刻度同样失真）
    const qreal target = mViewTransform.m11() * scaleBy;
    if (target < 1./64. || target > 64.) { return; }
    mUserAdjustedView = true;
    const QPointF transPoint = -mapToCanvasCoord(absOrigin);

    mViewTransform.translate(-transPoint.x(), -transPoint.y());
    mViewTransform.scale(scaleBy, scaleBy);
    mViewTransform.translate(transPoint.x(), transPoint.y());
}

// 监视器式视口（kdenlive 语义）：默认窗口尺寸变化即重新适配场景；
// 用户已滚轮缩放/中键平移进入自由观察态时改为中心锚定补偿，
// 正在观察的细节不因窗口微调被打回全幅
void CanvasWindow::resizeEvent(QResizeEvent *e)
{
    if (mUserAdjustedView && e->size().isValid() && mOldSize.isValid()) {
        const auto dSize = e->size() - mOldSize;
        const qreal dpr = devicePixelRatioF();
        // 目标：内容随窗口中心锚定，设备像素位移=dSize*dpr/2；
        // translateView 的参数先过已有缩放（实际位移=参数×m11），
        // 故除回 m11（QTransform::translate 语义，无头台架实证）
        const qreal scale = mViewTransform.m11();
        if (!qFuzzyIsNull(scale)) {
            translateView({dSize.width() * dpr * 0.5 / scale,
                           dSize.height() * dpr * 0.5 / scale});
        }
    } else {
        fitCanvasToSize();
    }
    if (e->size().isValid()) { mOldSize = e->size(); }
    GLWindow::resizeEvent(e);
}

void CanvasWindow::fitCanvasToSize(const bool &fitWidth)
{
    if (!mCurrentCanvas) { return; }
    mUserAdjustedView = false;
    mViewTransform.reset();
    qreal pixelRatio = devicePixelRatioF();
    const auto canvasSize = mCurrentCanvas->getCanvasSize();
    const qreal widWidth = width() * pixelRatio;
    const qreal widHeight = height() * pixelRatio;
    const qreal widthScale = (widWidth - eSizesUI::widget) / canvasSize.width();
    const qreal heightScale = (widHeight - eSizesUI::widget) / canvasSize.height();
    const qreal minScale = fitWidth ? widthScale : qMin(widthScale, heightScale);
    translateView({(widWidth - canvasSize.width() * minScale) * 0.5,
                   (widHeight - canvasSize.height() * minScale) * 0.5});
    mViewTransform.scale(minScale, minScale);
    update();
}

void CanvasWindow::zoomInView()
{
    if (!mCurrentCanvas) { return; }
    mUserAdjustedView = true;
    const auto canvasSize = mCurrentCanvas->getCanvasSize();
    mViewTransform.translate(canvasSize.width() * 0.5, canvasSize.height() * 0.5);
    mViewTransform.scale(1.1, 1.1);
    mViewTransform.translate(-canvasSize.width() * 0.5, -canvasSize.height() * 0.5);
    update();
}

void CanvasWindow::zoomOutView()
{
    if (!mCurrentCanvas) { return; }
    mUserAdjustedView = true;
    const auto canvasSize = mCurrentCanvas->getCanvasSize();
    mViewTransform.translate(canvasSize.width() * 0.5, canvasSize.height() * 0.5);
    mViewTransform.scale(0.9, 0.9);
    mViewTransform.translate(-canvasSize.width() * 0.5, -canvasSize.height() * 0.5);
    update();
}

bool CanvasWindow::event(QEvent *e)
{
    if (e->type() == QEvent::ShowToParent) { fitCanvasToSize(); }
    else if (e->type() == QEvent::Show) { KFT_setFocus(); }
#ifdef Q_OS_MAC
    if (e->type() == QEvent::NativeGesture) {
        auto g = dynamic_cast<QNativeGestureEvent*>(e);
        if (g->gestureType() == Qt::ZoomNativeGesture) {
            return handleNativeGestures(g);
        } else if (g->gestureType() == Qt::SmartZoomNativeGesture) {
            handleNativeGestures(g);
        }
    }
#endif
    return QWidget::event(e);
}

void CanvasWindow::hideEvent(QHideEvent *e)
{
    GLWindow::hideEvent(e);
    if (mCurrentCanvas) {
        mDocument.removeVisibleScene(mCurrentCanvas);
    }
}

void CanvasWindow::showEvent(QShowEvent *e)
{
    GLWindow::showEvent(e);
    if (mCurrentCanvas) {
        mDocument.addVisibleScene(mCurrentCanvas);
    }
}

void CanvasWindow::resetTransformation()
{
    if (!mCurrentCanvas) { return; }
    mUserAdjustedView = true;
    mViewTransform.reset();
    // 与 fitCanvasToSize 同口径：平移分量是设备像素，逻辑尺寸须乘 dpr
    // （上游原版漏乘 dpr，高 DPI 下 100% 视图偏向往左上）
    const qreal dpr = devicePixelRatioF();
    translateView({(width() * dpr - mCurrentCanvas->getCanvasWidth()) * 0.5,
                   (height() * dpr - mCurrentCanvas->getCanvasHeight()) * 0.5});
}
