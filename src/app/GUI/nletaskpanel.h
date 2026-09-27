/*
#
# Dream Cut - based on Friction
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

#ifndef NLETASKPANEL_H
#define NLETASKPANEL_H

#include <QWidget>

class QLabel;
class QProgressBar;
class QScrollArea;
class QToolButton;
class QVBoxLayout;
class NleTask;

// 任务面板（kdenlive 式）：后台任务列表——标题/进度条/取消/清除，
// 重建式渲染（任务量小，无虚拟化必要）
class NleTaskPanel : public QWidget {
    Q_OBJECT
public:
    explicit NleTaskPanel(QWidget* const parent = nullptr);

private slots:
    void rebuild();

private:
    QWidget* mHost = nullptr;
    QVBoxLayout* mListLayout = nullptr;
    QLabel* mEmptyLabel = nullptr;
};

#endif // NLETASKPANEL_H
