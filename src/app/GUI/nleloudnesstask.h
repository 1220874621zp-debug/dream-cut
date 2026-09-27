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

#ifndef NLELOUDNESSTASK_H
#define NLELOUDNESSTASK_H

#include "nletaskmanager.h"

class eSound;

// 达芬奇 Fairlight 响度标准化同语义：ffmpeg volumedetect 测源段平均
// 响度（RMS 近似 LUFS），算出把块音量拉到目标值所需的增益。音量量
// 纲 = friction 音量动画器 0..200（100 = 0dB = 1.0 线性）
class NleLoudnessTask : public NleTask {
public:
    // inSec/durSec >= 0 = 只分析该源段（-ss/-t）；< 0 = 整源
    NleLoudnessTask(const QString& src, const qreal targetDb = -14.,
                    const qreal inSec = -1., const qreal durSec = -1.);

    // 可独立直调（无头台架不进任务池）；错误信息 = run() 同语义
    QString analyze();
    qreal meanDb() const { return mMeanDb; }
    qreal maxDb() const { return mMaxDb; }
    qreal targetDb() const { return mTargetDb; }

    // volumedetect 日志行解析（提纯供台架直测）：无 mean 行 = 无音频流
    static bool parseVolumes(const QByteArray& log,
                             qreal* meanDb, qreal* maxDb);
protected:
    QString run() override;
private:
    QString mSrc;
    qreal mTargetDb;
    qreal mInSec;
    qreal mDurSec;
    qreal mMeanDb = 0.;
    qreal mMaxDb = -99.;
};

namespace NleLoudness {
// mean/max(源段) -> 归一到 targetDb 所需的音量动画器值（0..200）。
// 峰值保护优先：max 增益后必须 <= -1dB，防止归一爆音
qreal volumeForTarget(const qreal meanDb, const qreal maxDb,
                      const qreal targetDb);
// 把块音量动画器整体乘 factor（无键 = 基础值；有键 = 基础值与全部
// 键一起乘，保持包络形状）；返回 false = 无动画器
bool applyGainToSound(eSound* sound, const qreal factor);
} // namespace NleLoudness

#endif // NLELOUDNESSTASK_H
