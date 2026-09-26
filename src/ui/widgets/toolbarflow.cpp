/*
#
# Friction - https://friction.graphics
#
# Copyright (c) Ole-André Rodlie and contributors
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, version 3.
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

#include "toolbarflow.h"

#include "widgets/flowlayout.h"

#include <QToolBar>
#include <QAction>
#include <QWidgetAction>
#include <QToolButton>
#include <QTimer>

using namespace Friction::Ui;

ToolBarFlow::ToolBarFlow(QWidget *parent)
    : QWidget{parent}
    , mPopulated(false)
{
}

void ToolBarFlow::attachToolBar(QToolBar *toolbar)
{
    if (!toolbar || mPopulated) { return; }
    mPopulated = true;
    // defer to the end of the current event queue: the toolbar may still
    // receive late action insertions (menu setup puts undo/redo first)
    QTimer::singleShot(0, this, [this, toolbar]() {
        populate(toolbar);
    });
}

void ToolBarFlow::populate(QToolBar *toolbar)
{
    auto flow = new FlowLayout(this, 2, 4, 4);

    // actions() returns a copy, safe to remove from the toolbar while
    // iterating
    const auto acts = toolbar->actions();
    for (QAction *action : acts) {
        if (action->isSeparator()) { continue; }

        QWidget *item = nullptr;
        if (const auto wa = qobject_cast<QWidgetAction*>(action)) {
            QWidget *widget = wa->defaultWidget();
            // spacer widgets carry no content, the flow provides the
            // arrangement
            if (!widget || widget->property("isFixedSpacer").toBool()) {
                continue;
            }
            // detaching the action releases the widget (default widgets
            // stay alive and owned by their creator); the empty toolbar
            // remains the hidden logical owner of every action
            toolbar->removeAction(action);
            item = widget;
            connect(action, &QAction::visibleChanged,
                    item, [action, item]() {
                item->setVisible(action->isVisible());
            });
            connect(action, &QAction::enabledChanged,
                    item, &QWidget::setEnabled);
        } else {
            // plain actions get a mirroring tool button; defaultAction
            // keeps icon/text/enabled/check in sync on its own
            auto button = new QToolButton(this);
            button->setAutoRaise(true);
            button->setFocusPolicy(Qt::NoFocus);
            button->setToolButtonStyle(Qt::ToolButtonIconOnly);
            if (const QWidget *src = toolbar->widgetForAction(action)) {
                // style parity with the toolbar-rendered twin (qss
                // objectName like ToolBoxButton) and its icon size
                button->setObjectName(src->objectName());
                if (const auto srcBtn = qobject_cast<const QToolButton*>(src)) {
                    button->setIconSize(srcBtn->iconSize());
                }
            }
            button->setDefaultAction(action);
            item = button;
            connect(action, &QAction::visibleChanged,
                    item, [action, item]() {
                item->setVisible(action->isVisible());
            });
        }

        item->setVisible(action->isVisible());
        item->setEnabled(action->isEnabled());
        flow->addWidget(item);
    }

    toolbar->hide();
}
