// 片段监视器（kdenlive clip monitor 对齐）：预览素材、I/O 设 zone
// 出入点、拖入 NLE 时间轴落块。
// zone 语义照搬 kdenlive MonitorProxy：zoneIn=0 / zoneOut=-1（-1 =
// 未设 zone），拖出时未设 zone = 全片段
#ifndef CLIPMONITORWIDGET_H
#define CLIPMONITORWIDGET_H

#include <QWidget>
#include <QImage>

class QMediaPlayer;
class QAudioOutput;
class QVideoSink;
class QLabel;
class QToolButton;
class MonitorView;
class MonitorRuler;

class ClipMonitorWidget : public QWidget {
    Q_OBJECT
public:
    explicit ClipMonitorWidget(QWidget *parent = nullptr);

    // 装载素材（视频）：复位 zone，读取时长/帧率
    void loadFile(const QString &path);

    QString path() const { return mPath; }
    int zoneIn() const { return mZoneIn; }
    // 装载后恒为全片段或更窄（kdenlive：zone 永远可见，I/O 挪边）
    int zoneOut() const { return mZoneOut; }
    int frameCount() const { return mFrameCount; }
    bool hasZone() const { return mZoneOut >= mZoneIn && mFrameCount > 0; }
    // 直接设 zone（自动钳制交叉）；out<in = 清除
    void setZone(const int in, const int out);

signals:
    void logMessage(const QString &msg);
    void zoneChanged();
    void openRequested();

protected:
    void keyPressEvent(QKeyEvent *event) override;

private:
    friend class MonitorView;
    friend class MonitorRuler;

    int frameFromMs(const qint64 ms) const;
    qint64 msFromFrame(const int frame) const;
    QString timecode(const int frame) const;
    void setZoneIn(const int frame);
    void setZoneOut(const int frame);
    void resetZone();
    void togglePlay();
    void stepFrame(const int frames);
    void seekTo(const int frame);
    void seekToEnd();
    void updateControls();

    MonitorView *mView = nullptr;
    MonitorRuler *mRuler = nullptr;
    QMediaPlayer *mPlayer = nullptr;
    QAudioOutput *mAudio = nullptr;
    QVideoSink *mSink = nullptr;

    QString mPath;
    qreal mFps = 30.;
    qint64 mDurationMs = 0;
    int mFrameCount = 0;
    int mCurrentFrame = 0;
    int mZoneIn = 0;
    int mZoneOut = -1;

    QLabel *mTimeLabel = nullptr;
    QLabel *mFileLabel = nullptr;
    QToolButton *mPlayBtn = nullptr;
};

#endif // CLIPMONITORWIDGET_H
