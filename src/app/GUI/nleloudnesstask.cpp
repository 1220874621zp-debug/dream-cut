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

#include "nleloudnesstask.h"

#include "Sound/esound.h"
#include "Animators/qrealkey.h"

#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>

NleLoudnessTask::NleLoudnessTask(const QString& src, const qreal targetDb,
                                 const qreal inSec, const qreal durSec)
    : NleTask(QStringLiteral("响度分析：%1").arg(
                  QFileInfo(src).fileName()))
    , mSrc(src), mTargetDb(targetDb)
    , mInSec(inSec), mDurSec(durSec)
{
    setDetail(src);
}

bool NleLoudnessTask::parseVolumes(const QByteArray& log,
                                   qreal* meanDb, qreal* maxDb)
{
    static const QRegularExpression meanRx(
                QStringLiteral("mean_volume:\\s*(-?[0-9.]+) dB"));
    static const QRegularExpression maxRx(
                QStringLiteral("max_volume:\\s*(-?[0-9.]+) dB"));
    const auto meanM = meanRx.match(QString::fromUtf8(log));
    if (!meanM.hasMatch()) { return false; }
    const auto maxM = maxRx.match(QString::fromUtf8(log));
    if (meanDb) { *meanDb = meanM.captured(1).toDouble(); }
    if (maxDb) {
        *maxDb = maxM.hasMatch() ? maxM.captured(1).toDouble() : 0.;
    }
    return true;
}

QString NleLoudnessTask::analyze()
{
    if (!QFileInfo::exists(mSrc)) {
        return QStringLiteral("源文件不存在");
    }
    // volumedetect 统计（段参数 >= 0 时 -ss/-t 截取块内源段），
    // stderr 出 mean/max_volume
    QStringList args{
                QStringLiteral("-hide_banner"), QStringLiteral("-nostats")};
    if (mInSec >= 0.) {
        args << QStringLiteral("-ss") << QString::number(mInSec, 'f', 3);
        if (mDurSec > 0.) {
            args << QStringLiteral("-t")
                 << QString::number(mDurSec, 'f', 3);
        }
    }
    args << QStringLiteral("-i") << mSrc
         << QStringLiteral("-map") << QStringLiteral("0:a:0?")
         << QStringLiteral("-af") << QStringLiteral("volumedetect")
         << QStringLiteral("-vn") << QStringLiteral("-sn")
         << QStringLiteral("-f") << QStringLiteral("null")
         << QStringLiteral("-");
    QProcess p;
    p.start(QStringLiteral("ffmpeg"), args);
    if (!p.waitForStarted(5000)) {
        return QStringLiteral("ffmpeg 无法启动");
    }
    QByteArray log;
    while (!p.waitForFinished(200)) {
        if (canceled()) {
            p.kill();
            p.waitForFinished(5000);
            return QStringLiteral("已取消");
        }
        log += p.readAllStandardError();
    }
    log += p.readAllStandardError();
    if (p.exitStatus() != QProcess::NormalExit || p.exitCode() != 0) {
        return QString::fromUtf8(log).right(300);
    }
    if (!parseVolumes(log, &mMeanDb, &mMaxDb)) {
        return QStringLiteral("未检测到音频流");
    }
    setProgress(1.);
    setDetail(QStringLiteral("%1：%2 dB（目标 %3 dB）")
              .arg(QFileInfo(mSrc).fileName())
              .arg(QString::number(mMeanDb, 'f', 1))
              .arg(QString::number(mTargetDb, 'f', 0)));
    return QString();
}

QString NleLoudnessTask::run()
{
    return analyze();
}

namespace NleLoudness {
qreal volumeForTarget(const qreal meanDb, const qreal maxDb,
                      const qreal targetDb)
{
    // 增益 = 目标 - 现状；峰值保护钳在 max <= -1dB
    const qreal gainDb = qMin(targetDb - meanDb, -1. - maxDb);
    return qBound(0., 100. * std::pow(10., gainDb / 20.), 200.);
}

bool applyGainToSound(eSound* sound, const qreal factor)
{
    if (!sound) { return false; }
    // 增益可忽略：无需调整
    if (qFuzzyCompare(factor, 1.)) { return false; }
    auto* const anim = sound->volumeAnimator();
    if (!anim) { return false; }
    // 关键帧一起乘保持包络形状（淡入淡出相对量不变）
    for (const auto& k : anim->anim_getKeys()) {
        static_cast<QrealKey*>(k)->setValue(
                    static_cast<QrealKey*>(k)->getValue() * factor);
    }
    anim->setCurrentBaseValue(
                qBound(0., anim->getCurrentBaseValue() * factor, 200.));
    return true;
}
} // namespace NleLoudness
