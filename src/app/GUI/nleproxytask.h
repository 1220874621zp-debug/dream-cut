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

#ifndef NLEPROXYTASK_H
#define NLEPROXYTASK_H

#include "nletaskmanager.h"

// 代理转码任务（kdenlive proxytask 式）：ffmpeg 抽视频流转 540p
// x264（-an，声音仍走原件）；进度经 -progress 管道按时长归一；
// 取消 = worker 循环里协作杀进程（同线程 kill，无线程安全问题）
class NleProxyTask : public NleTask {
    Q_OBJECT
public:
    NleProxyTask(const QString& src, const QString& dst);
protected:
    QString run() override;
private:
    QString mSrc;
    QString mDst;
};

#endif // NLEPROXYTASK_H
