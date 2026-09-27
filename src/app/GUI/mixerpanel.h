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

#ifndef MIXERPANEL_H
#define MIXERPANEL_H

#include <QSlider>
#include <QWidget>

class QLabel;
class QSlider;
class QToolButton;
class QTimer;
class QHBoxLayout;
class MainWindow;

// 垂直电平条：RMS 填充 + 峰值保持线（-60..0dB 映射，>0 过载红顶）
class MixerMeter : public QWidget {
    Q_OBJECT
public:
    explicit MixerMeter(QWidget* const parent = nullptr);

    void setLevel(const qreal rms);
    // 峰值保持的缓慢回落（由面板轮询节拍驱动）
    void tick();
protected:
    void paintEvent(QPaintEvent* const e) override;
private:
    static qreal dbFrac(const qreal db)
    { return qBound(0., (db + 60.)/66., 1.); }

    qreal mRms = 0.;     // 线性 RMS（可 >1 = 过载）
    qreal mPeakDb = -99.;
};

// 单轨条：轨道名 + 电平表 + 推子（-60..+6dB）+ 静音/独奏
class MixerTrackStrip : public QWidget {
    Q_OBJECT
public:
    explicit MixerTrackStrip(QWidget* const parent = nullptr);

    void setInfo(const int trackId, const QString& name);
    // 外部同步推子（QSignalBlocker 防回环）
    void setVolumeDb(const qreal db);
    // 外部同步静音/独奏按钮态
    void setMs(const bool muted, const bool solo);

    int trackId() const { return mTrackId; }
    MixerMeter* meter() const { return mMeter; }
    QSlider* fader() const { return mFader; }
    QToolButton* muteButton() const { return mMuteBtn; }
    QToolButton* soloButton() const { return mSoloBtn; }
    qreal faderDb() const { return mFader->value() - 60.; }
signals:
    void volumeChanged(const int trackId, const qreal volume);
    void muteClicked(const int trackId);
    void soloClicked(const int trackId);
protected:
    void mouseDoubleClickEvent(QMouseEvent* const e) override;
private:
    int mTrackId = -1;
    QLabel* mNameLabel;
    MixerMeter* mMeter;
    QSlider* mFader;
    QLabel* mDbLabel;
    QToolButton* mMuteBtn;
    QToolButton* mSoloBtn;
};

// 混音器面板（kdenlive 式）：每条音频轨一条——推子写轨道 spec
// 音量（SoundMerger 乘进音量快照）、M/S 走时间轴模型现成语义、
// 电平表轮询 SoundComposition::trackLevelAt（播放位置窗口 RMS，
// 过滤与实际出声完全一致）。轨道增删/改名/状态变由轮询签名比对
// 自动重建条目
class MixerPanel : public QWidget {
    Q_OBJECT
public:
    explicit MixerPanel(MainWindow* const mainWin);

signals:
    void logMessage(const QString& msg);
private:
    void pollTick();
    void rebuildStrips();
    // 无头台架（env DREAMCUT_MIXER_AUTOTEST）
    void mixerAutotestStage1();
    bool mAutotestRetried = false;

    MainWindow* const mMainWindow;
    QTimer* mPollTimer;
    QWidget* mStripsHost;
    QHBoxLayout* mStripsLayout;
    QLabel* mEmptyLabel;
    QString mSignature;
};

#endif // MIXERPANEL_H
