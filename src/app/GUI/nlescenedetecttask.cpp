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

#include "nlescenedetecttask.h"

#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>

NleSceneDetectTask::NleSceneDetectTask(const QString& src,
                                       const qreal threshold)
    : NleTask(QStringLiteral("场景检测：%1").arg(
                  QFileInfo(src).fileName()))
    , mSrc(src), mThreshold(threshold)
{
    setDetail(src);
}

// showinfo 每个被选中的帧打一行 "n: ... pts_time:12.345 pts:...":
// 抽秒、升序去重、合并过密切点（镜头闪烁半帧抖动）
QVector<double> NleSceneDetectTask::parseShowinfoCuts(const QByteArray& log)
{
    static const QRegularExpression rx(
                QStringLiteral("pts_time:([0-9]+\\.?[0-9]*)"));
    QVector<double> cuts;
    auto it = rx.globalMatch(QString::fromUtf8(log));
    while (it.hasNext()) {
        const auto m = it.next();
        const double t = m.captured(1).toDouble();
        if (!cuts.isEmpty() && t - cuts.last() < 0.1) { continue; }
        cuts.append(t);
    }
    return cuts;
}

QString NleSceneDetectTask::detect()
{
    if (!QFileInfo::exists(mSrc)) {
        return QStringLiteral("源文件不存在");
    }
    // 源流帧率（"30000/1001" 分数形式）与总时长（进度分母）
    QProcess probe;
    probe.start(QStringLiteral("ffprobe"),
                {QStringLiteral("-v"), QStringLiteral("error"),
                 QStringLiteral("-select_streams"), QStringLiteral("v:0"),
                 QStringLiteral("-show_entries"),
                 QStringLiteral("stream=r_frame_rate"),
                 QStringLiteral("-of"), QStringLiteral("csv=p=0"),
                 mSrc});
    if (!probe.waitForStarted(5000)) {
        return QStringLiteral("ffprobe 无法启动");
    }
    if (!probe.waitForFinished(30000)) {
        return QStringLiteral("ffprobe 超时");
    }
    const QString rate = QString::fromUtf8(
                probe.readAllStandardOutput()).trimmed();
    const auto frac = rate.split('/');
    if (frac.size() == 2 && frac.at(1).toDouble() > 0) {
        mSrcFps = frac.at(0).toDouble() / frac.at(1).toDouble();
    } else if (frac.size() == 1) {
        mSrcFps = frac.at(0).toDouble();
    }

    probe.start(QStringLiteral("ffprobe"),
                {QStringLiteral("-v"), QStringLiteral("error"),
                 QStringLiteral("-show_entries"),
                 QStringLiteral("format=duration"),
                 QStringLiteral("-of"), QStringLiteral("csv=p=0"),
                 mSrc});
    if (!probe.waitForStarted(5000)) {
        return QStringLiteral("ffprobe 无法启动");
    }
    if (!probe.waitForFinished(30000)) {
        return QStringLiteral("ffprobe 超时");
    }
    const qreal dur = QString::fromUtf8(
                probe.readAllStandardOutput()).trimmed().toDouble();

    // scene 滤镜挑出镜头切变帧，showinfo 把切点时间打到 stderr；
    // stdout 接 -progress 管道做进度
    QProcess p;
    p.setProcessChannelMode(QProcess::SeparateChannels);
    p.start(QStringLiteral("ffmpeg"),
            {QStringLiteral("-hide_banner"), QStringLiteral("-nostats"),
             QStringLiteral("-i"), mSrc,
             QStringLiteral("-vf"),
             QStringLiteral("select='gt(scene,%1)',showinfo")
                    .arg(mThreshold, 0, 'g', 3),
             QStringLiteral("-an"), QStringLiteral("-sn"),
             QStringLiteral("-progress"), QStringLiteral("pipe:1"),
             QStringLiteral("-f"), QStringLiteral("null"),
             QStringLiteral("-")});
    if (!p.waitForStarted(5000)) {
        return QStringLiteral("ffmpeg 无法启动");
    }
    QByteArray showinfo;
    while (!p.waitForFinished(200)) {
        if (canceled()) {
            p.kill();
            p.waitForFinished(5000);
            return QStringLiteral("已取消");
        }
        showinfo += p.readAllStandardError();
        while (p.canReadLine()) {
            const QByteArray line = p.readLine();
            if (line.startsWith("out_time_us=") && dur > 0.) {
                const qint64 us = line.mid(12).trimmed().toLongLong();
                setProgress(qBound(0., qreal(us)/1e6/dur, 1.));
            }
        }
    }
    showinfo += p.readAllStandardError();
    if (p.exitStatus() != QProcess::NormalExit || p.exitCode() != 0) {
        return QString::fromUtf8(showinfo).right(300);
    }
    mCutSecs = parseShowinfoCuts(showinfo);
    setProgress(1.);
    setDetail(QStringLiteral("%1（%2 个切点）")
              .arg(QFileInfo(mSrc).fileName())
              .arg(mCutSecs.count()));
    return QString();
}

QString NleSceneDetectTask::run()
{
    return detect();
}
