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

#ifndef FRICTION_TOOLBAR_FLOW_H
#define FRICTION_TOOLBAR_FLOW_H

#include "ui_global.h"

#include <QWidget>

class QToolBar;

namespace Friction
{
    namespace Ui
    {
        /// Re-hosts the entries of a QToolBar in a wrapping FlowLayout
        /// (the toolbar itself becomes a hidden logical owner: actions
        /// and action groups keep managing state, only the presentation
        /// moves into this widget).
        class UI_EXPORT ToolBarFlow : public QWidget
        {
            Q_OBJECT
        public:
            explicit ToolBarFlow(QWidget *parent = nullptr);

            /// Populate the flow from the toolbar's current actions.
            /// Population is deferred to the end of the event queue so
            /// late insertions into the toolbar (menu setup inserting
            /// undo/redo) are included; only the first call wins.
            void attachToolBar(QToolBar *toolbar);

        private:
            void populate(QToolBar *toolbar);
            bool mPopulated;
        };
    }
}

#endif // FRICTION_TOOLBAR_FLOW_H
