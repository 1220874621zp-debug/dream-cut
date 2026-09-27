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

#ifndef NLESCENEDETECTTASK_H
#define NLESCENEDETECTTASK_H

#include "nletaskmanager.h"

#include <QVector>

// 达芬奇场景检测（Media 页 Scene Cut Detection 同语义）：ffmpeg
// scene 滤镜扫描源视频的镜头切点，返回源时间秒列表；时间轴右键
// 视频块排队，完成后换算成时间轴帧逐刀分割（requestSceneDetectSplits）
class NleSceneDetectTask : public NleTask {
public:
    NleSceneDetectTask(const QString& src, const qreal threshold = 0.4);

    // 可独立直调（无头台架不进任务池）；错误信息 = run() 同语义
    QString detect();
    // 源时间秒切点（升序去重）；srcFps 为源流帧率（0 = 未知）
    const QVector<double>& cutSecs() const { return mCutSecs; }
    qreal srcFps() const { return mSrcFps; }

    // showinfo 日志行解析（切点秒，升序去重，间隔 <0.1s 合并）——
    // 提纯为自由入口供台架直测
    static QVector<double> parseShowinfoCuts(const QByteArray& log);
protected:
    QString run() override;
private:
    QString mSrc;
    qreal mThreshold;
    QVector<double> mCutSecs;
    qreal mSrcFps = 0.;
};

#endif // NLESCENEDETECTTASK_H
