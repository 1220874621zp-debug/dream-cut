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

#include "nleproxytask.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>

NleProxyTask::NleProxyTask(const QString& src, const QString& dst)
    : NleTask(QStringLiteral("生成代理：%1").arg(
                  QFileInfo(src).fileName()))
    , mSrc(src), mDst(dst)
{
    setDetail(src);
}

QString NleProxyTask::run()
{
    if (!QFileInfo::exists(mSrc)) {
        return QStringLiteral("源文件不存在");
    }
    // 时长（进度分母）
    QProcess probe;
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
    QDir().mkpath(QFileInfo(mDst).absolutePath());

    QProcess p;
    p.start(QStringLiteral("ffmpeg"),
            {QStringLiteral("-y"),
             QStringLiteral("-i"), mSrc,
             QStringLiteral("-map"), QStringLiteral("0:v:0"),
             QStringLiteral("-vf"), QStringLiteral("scale=-2:540"),
             QStringLiteral("-c:v"), QStringLiteral("libx264"),
             QStringLiteral("-preset"), QStringLiteral("veryfast"),
             QStringLiteral("-crf"), QStringLiteral("28"),
             QStringLiteral("-an"),
             QStringLiteral("-progress"), QStringLiteral("pipe:1"),
             QStringLiteral("-nostats"), mDst});
    if (!p.waitForStarted(5000)) {
        return QStringLiteral("ffmpeg 无法启动");
    }
    while (!p.waitForFinished(200)) {
        if (canceled()) {
            p.kill();
            p.waitForFinished(5000);
            QFile::remove(mDst);
            return QStringLiteral("已取消");
        }
        while (p.canReadLine()) {
            const QByteArray line = p.readLine();
            if (line.startsWith("out_time_us=")) {
                const qint64 us = line.mid(12).trimmed().toLongLong();
                if (dur > 0.) {
                    setProgress(qBound(0., qreal(us)/1e6/dur, 1.));
                }
            }
        }
    }
    if (p.exitStatus() != QProcess::NormalExit || p.exitCode() != 0) {
        QFile::remove(mDst);
        return QString::fromUtf8(p.readAllStandardError()).right(300);
    }
    if (!QFileInfo::exists(mDst) ||
            QFileInfo(mDst).size() < 1024) {
        QFile::remove(mDst);
        return QStringLiteral("代理文件未生成");
    }
    setProgress(1.);
    setDetail(mDst);
    return QString();
}
