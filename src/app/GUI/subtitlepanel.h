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

#ifndef SUBTITLEPANEL_H
#define SUBTITLEPANEL_H

#include <QWidget>

class QListWidget;
class MainWindow;

// 字幕面板（CapCut 式）：字幕轨（名"字幕"的视频型轨）上 TextBox 块
// 的条目视图——时间码+文本，单击跳播放头；SRT 导入（逐条建底部居
// 中文字块）与导出走时间轴模型事务。文本编辑在属性面板（选中块
// 即得文字属性）
class SubtitlePanel : public QWidget {
    Q_OBJECT
public:
    explicit SubtitlePanel(MainWindow* const mainWin);

    void refresh();
signals:
    void logMessage(const QString& msg);
private:
    MainWindow* const mMainWindow;
    QListWidget* mList;
};

#endif // SUBTITLEPANEL_H
