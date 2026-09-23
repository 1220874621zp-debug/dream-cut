#include "clipmonitorwidget.h"

#include <QAudioOutput>
#include <QDrag>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QMediaMetaData>
#include <QMediaPlayer>
#include <QMimeData>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPolygonF>
#include <QSvgRenderer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QVideoFrame>
#include <QVideoSink>
#include <QWheelEvent>
#include <functional>

namespace {

// SVG 图标（描边风格同 NLE 工具栏 glyph），%1 换色
const char* kSvgStart =
        "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 24 24\">"
        "<path fill=\"none\" stroke=\"%1\" stroke-width=\"2\""
        " stroke-linecap=\"round\" stroke-linejoin=\"round\""
        " d=\"M5 5 L11 12 L5 19 M13 5 L19 12 L13 19\"/></svg>";
const char* kSvgFrameBack =
        "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 24 24\">"
        "<path fill=\"none\" stroke=\"%1\" stroke-width=\"2\""
        " stroke-linecap=\"round\" stroke-linejoin=\"round\""
        " d=\"M15 6 L8.5 12 L15 18\"/></svg>";
const char* kSvgPlay =
        "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 24 24\">"
        "<path fill=\"%1\" d=\"M7.5 4.8 L19 12 L7.5 19.2 Z\"/></svg>";
const char* kSvgPause =
        "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 24 24\">"
        "<path fill=\"%1\" d=\"M6.5 5 H10.5 V19 H6.5 Z"
        " M13.5 5 H17.5 V19 H13.5 Z\"/></svg>";
const char* kSvgFrameFwd =
        "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 24 24\">"
        "<path fill=\"none\" stroke=\"%1\" stroke-width=\"2\""
        " stroke-linecap=\"round\" stroke-linejoin=\"round\""
        " d=\"M9 6 L15.5 12 L9 18\"/></svg>";
const char* kSvgEnd =
        "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 24 24\">"
        "<path fill=\"none\" stroke=\"%1\" stroke-width=\"2\""
        " stroke-linecap=\"round\" stroke-linejoin=\"round\""
        " d=\"M5 5 L11 12 L5 19 M13 5 L19 12 L13 19\"/></svg>";
const char* kSvgZoneStart =
        "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 24 24\">"
        "<path fill=\"none\" stroke=\"%1\" stroke-width=\"2\""
        " stroke-linecap=\"round\" stroke-linejoin=\"round\""
        " d=\"M5 5 L11 12 L5 19\"/>"
        "<path fill=\"%1\" d=\"M14.5 5.5 H17.5 V18.5 H14.5 Z\"/></svg>";
const char* kSvgMarkIn =
        "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 24 24\">"
        "<path fill=\"%1\" d=\"M5 5 H8 V19 H5 Z\"/>"
        "<path fill=\"none\" stroke=\"%1\" stroke-width=\"1.8\""
        " stroke-linecap=\"round\" d=\"M12.5 7 V17 M16.5 7 V17\"/></svg>";
const char* kSvgMarkOut =
        "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 24 24\">"
        "<path fill=\"none\" stroke=\"%1\" stroke-width=\"1.8\""
        " stroke-linecap=\"round\" d=\"M7.5 7 V17 M11.5 7 V17\"/>"
        "<path fill=\"%1\" d=\"M16 5 H19 V19 H16 Z\"/></svg>";
const char* kSvgZoneEnd =
        "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 24 24\">"
        "<path fill=\"none\" stroke=\"%1\" stroke-width=\"2\""
        " stroke-linecap=\"round\" stroke-linejoin=\"round\""
        " d=\"M19 5 L13 12 L19 19\"/>"
        "<path fill=\"%1\" d=\"M6.5 5.5 H9.5 V18.5 H6.5 Z\"/></svg>";
const char* kSvgZoneClear =
        "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 24 24\">"
        "<path fill=\"none\" stroke=\"%1\" stroke-width=\"1.8\""
        " d=\"M4.5 6.5 H19.5 V17.5 H4.5 Z\"/>"
        "<path fill=\"none\" stroke=\"%1\" stroke-width=\"1.8\""
        " stroke-linecap=\"round\" d=\"M6.5 19.5 L17.5 4.5\"/></svg>";
const char* kSvgOpen =
        "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 24 24\">"
        "<path fill=\"none\" stroke=\"%1\" stroke-width=\"1.8\""
        " stroke-linejoin=\"round\""
        " d=\"M3.5 7.5 V17 a1.5 1.5 0 0 0 1.5 1.5 H14 a1.5 1.5 0 0 0"
        " 1.5-1.5 V9.5 a1.5 1.5 0 0 0-1.5-1.5 H10 L8 5.5 H5 A1.5 1.5 0"
        " 0 0 3.5 7.5 Z\"/></svg>";

QPixmap glyphPixmap(const char *svg, const QColor &color,
                    const int base = 20)
{
    const qreal dpr = qApp ? qApp->devicePixelRatio() : 1.;
    QPixmap pm(QSize(base, base) * dpr);
    pm.fill(Qt::transparent);
    const QString str = QString::fromUtf8(svg).arg(color.name());
    QSvgRenderer renderer(str.toUtf8());
    if (renderer.isValid()) {
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        renderer.render(&p, QRectF(0, 0, base * dpr, base * dpr));
        p.end();
    }
    pm.setDevicePixelRatio(dpr);
    return pm;
}

} // namespace

// ---------------------------------------------------------------- view

// 预览画面：QVideoSink 帧黑底 letterbox；叠层=左上文件名+右下时间码；
// 按住拖动 = 拖出片段（zone 或全片段）进时间轴
class MonitorView : public QWidget {
public:
    explicit MonitorView(ClipMonitorWidget *parent)
        : QWidget(parent), mMon(parent) {
        setMinimumHeight(140);
    }

    QImage mFrame;
    QString mEmptyText;
    QPoint mPressPos;
    bool mDragging = false;

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.fillRect(rect(), QColor(0x0c, 0x0c, 0x0e));
        if (mFrame.isNull()) {
            if (!mEmptyText.isEmpty()) {
                p.setPen(QColor(0x6e, 0x6e, 0x74));
                p.drawText(rect(), Qt::AlignCenter, mEmptyText);
            }
            return;
        }
        const qreal dpr = devicePixelRatioF();
        auto scaled = mFrame.scaled(
                    QSize(int(width() * dpr), int(height() * dpr)),
                    Qt::KeepAspectRatio, Qt::SmoothTransformation);
        scaled.setDevicePixelRatio(dpr);
        const int x = (width() - int(scaled.width() / dpr)) / 2;
        const int y = (height() - int(scaled.height() / dpr)) / 2;
        p.drawImage(QPoint(x, y), scaled);

        // 左上文件名叠层（半透明底）
        if (!mOverlayFile.isEmpty()) {
            QFont f = font();
            f.setPixelSize(10);
            p.setFont(f);
            const QFontMetrics fm(f);
            const QString label = fm.elidedText(
                        mOverlayFile, Qt::ElideMiddle, width() - 16);
            const QRect r(6, 6, fm.horizontalAdvance(label) + 10,
                          fm.height() + 4);
            p.fillRect(r, QColor(0, 0, 0, 130));
            p.setPen(QColor(0xd8, 0xd8, 0xdc));
            p.drawText(r.adjusted(5, 2, -5, -2), Qt::AlignVCenter, label);
        }
        // 右下时间码叠层（等宽字体半透明底）
        if (!mOverlayTc.isEmpty()) {
            QFont f = QFontDatabase::systemFont(
                        QFontDatabase::FixedFont);
            f.setPixelSize(11);
            p.setFont(f);
            const QFontMetrics fm(f);
            const QRect r(width() - fm.horizontalAdvance(mOverlayTc) - 16,
                          height() - fm.height() - 10,
                          fm.horizontalAdvance(mOverlayTc) + 10,
                          fm.height() + 4);
            p.fillRect(r, QColor(0, 0, 0, 130));
            p.setPen(QColor(0xf2, 0xf2, 0xf4));
            p.drawText(r.adjusted(5, 2, -5, -2), Qt::AlignVCenter,
                       mOverlayTc);
        }
    }

    void mousePressEvent(QMouseEvent *e) override {
        // 面板拿焦点：I/O/空格才能路由进监视器（否则被时间轴 dock
        // 的 processKeyPress 抢走设了场景入出点，监视器毫无反应）
        mMon->setFocus(Qt::MouseFocusReason);
        if (e->button() != Qt::LeftButton) { return; }
        mPressPos = e->pos();
        mDragging = false;
    }

    void mouseMoveEvent(QMouseEvent *e) override {
        if (!(e->buttons() & Qt::LeftButton) || mDragging) { return; }
        // 拖拽阈值（kdenlive 同款量级）：纯点击不发起拖出
        if ((e->pos() - mPressPos).manhattanLength() < 12) { return; }
        if (mMon->path().isEmpty()) { return; }
        mDragging = true;
        auto * const drag = new QDrag(this);
        auto * const mime = new QMimeData;
        // zone 未设 = 全片段；path 与出入点分键携带
        const int in = mMon->hasZone() ? mMon->zoneIn() : 0;
        const int out = mMon->hasZone() ? mMon->zoneOut()
                                        : mMon->frameCount() - 1;
        mime->setData(QStringLiteral("application/x-dreamcut-clip-path"),
                      mMon->path().toUtf8());
        mime->setData(QStringLiteral("application/x-dreamcut-clip-inout"),
                      QStringLiteral("%1,%2").arg(in).arg(out)
                              .toUtf8());
        drag->setMimeData(mime);
        if (!mFrame.isNull()) {
            drag->setPixmap(QPixmap::fromImage(
                                mFrame.scaled(96, 54,
                                              Qt::KeepAspectRatio)));
        }
        drag->exec(Qt::CopyAction);
        mDragging = false;
    }

private:
    ClipMonitorWidget * const mMon;
public:
    QString mOverlayFile;
    QString mOverlayTc;
};

// ---------------------------------------------------------------- ruler

// kdenlive MonitorRuler 对齐：自适应刻度阶梯（1帧→30分）、主刻度半高
// 次刻度 1/4 高、zone=底部半高高亮+左右把手（Shift 拖动同步定位）、
// 时长气泡（拖 in 显示 入>长 / 拖 out 显示 长<出）、播放头=顶部下指
// 三角；点击/拖动=定位
class MonitorRuler : public QWidget {
public:
    explicit MonitorRuler(ClipMonitorWidget *parent)
        : QWidget(parent), mMon(parent) {
        setFixedHeight(26);
        setMouseTracking(true);
    }

    int mFrameCount = 0;
    int mCurrent = 0;
    int mZoneIn = 0;
    int mZoneOut = -1;
    // 缩放（kdenlive timeZoomFactor/timeZoomOffset 的窗口式表述）：
    // 可视窗口 = [mZoomFrom, mZoomFrom+mZoomFrames)，全宽=覆盖全长。
    // Ctrl+滚轮以鼠标为锚缩放，播放头越窗自动滚动追边
    int mZoomFrom = 0;
    int mZoomFrames = 0; // 0 = 跟随全长（未初始化）

    enum class Grab { None, Seek, InEdge, OutEdge };
    Grab mGrab = Grab::None;
    // 气泡：拖 zone 边时显示（入>长 / 长<出），悬停 zone 显示时长
    int mBubbleMode = 0; // 0=off 1=dur 2=in 3=out

    int zoomFrames() const {
        return mZoomFrames > 0 ? qMin(mZoomFrames, mFrameCount)
                               : mFrameCount;
    }
    bool zoomed() const { return zoomFrames() < mFrameCount; }
    void resetZoom() {
        mZoomFrom = 0;
        mZoomFrames = mFrameCount;
        update();
    }

private:
    ClipMonitorWidget * const mMon;

    qreal frameW() const {
        return zoomFrames() > 0 ? qreal(width()) / zoomFrames() : 0.;
    }
    int xOfFrame(const int f) const {
        return qRound((f - mZoomFrom + 0.5) * frameW());
    }
    int frameAtX(const int x) const {
        if (zoomFrames() <= 0) { return 0; }
        return qBound(0, mZoomFrom + int(x / frameW()), mFrameCount - 1);
    }
    int grabW() const { return 7; } // zone 把手抓取半宽

    // kdenlive 刻度阶梯：按可视窗口的显示时长选帧距（<3s=1帧 …，
    // 缩放后窗口变窄刻度自动变密 = 精选出入点的核心收益）
    int tickFrames() const {
        if (zoomFrames() <= 0) { return 0; }
        const int fps = qMax(1, int(mMon->mFps));
        const int secs = zoomFrames() / fps;
        if (secs < 3) { return 1; }
        if (secs < 30) { return fps; }
        if (secs < 150) { return 5 * fps; }
        if (secs < 300) { return 10 * fps; }
        if (secs < 900) { return 30 * fps; }
        if (secs < 1800) { return 60 * fps; }
        if (secs < 9000) { return 300 * fps; }
        if (secs < 18000) { return 600 * fps; }
        return 1800 * fps;
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.fillRect(rect(), QColor(0x20, 0x20, 0x24));
        if (mFrameCount <= 0) { return; }
        const bool hasZone = mZoneOut >= mZoneIn;

        // zone：底部半高（kdenlive 高亮色暗调，opacity 视觉）。
        // 窗口化坐标：越窗部分被 QRect 自动裁剪
        if (hasZone) {
            const int zx = xOfFrame(mZoneIn) - qRound(frameW() / 2);
            const int zo = xOfFrame(mZoneOut) + qRound(frameW() / 2);
            const QRect zr(qMax(0, zx), height() / 2,
                           qMax(2, zo - zx), height() - height() / 2);
            p.fillRect(zr, QColor(0x4b, 0x69, 0x8c, 210));
            // 把手：白竖条（kdenlive trimIn/trimOut 视觉），悬停/拖动
            // 时高亮
            const bool inHot = mGrab == Grab::InEdge || mHoverEdge == 1;
            const bool outHot = mGrab == Grab::OutEdge || mHoverEdge == 2;
            p.fillRect(QRect(zr.x(), zr.y(), 2, zr.height()),
                       inHot ? QColor(0xff, 0xff, 0xff)
                             : QColor(0xd0, 0xd0, 0xd4, 160));
            p.fillRect(QRect(zr.right() - 1, zr.y(), 2, zr.height()),
                       outHot ? QColor(0xff, 0xff, 0xff)
                              : QColor(0xd0, 0xd0, 0xd4, 160));
        }

        // 刻度：主（每 5 个 tick）半高、次 1/4 高。刻度号对齐全局
        // 帧格（窗口平移不跳档），idx 从窗口起点起算保证档位稳定
        const int tf = tickFrames();
        if (tf > 0) {
            p.setPen(QColor(0x8a, 0x8a, 0x90));
            const int first = (mZoomFrom / tf) * tf;
            for (int f = first; f < mFrameCount; f += tf) {
                const int x = xOfFrame(f);
                if (x < 0 || x >= width()) { continue; }
                const int h = ((f / tf) % 5 == 0) ? height() / 2
                                                  : height() / 4;
                p.drawLine(x, height() - h, x, height());
            }
        }

        // 已播放区淡显（窗口内部分）
        if (mCurrent > 0) {
            const int cx = xOfFrame(mCurrent);
            p.fillRect(QRect(0, 0, cx, height()),
                       QColor(0x2c, 0x2c, 0x32, 110));
        }

        // 播放头：顶部下指三角 + 竖线（kdenlive TimelinePlayhead 位）
        const int cx = xOfFrame(mCurrent);
        p.setPen(QPen(QColor(0xe8, 0x4c, 0x4c), 1));
        p.drawLine(cx, 0, cx, height());
        p.setBrush(QColor(0xe8, 0x4c, 0x4c));
        p.setPen(Qt::NoPen);
        QPolygonF tri;
        tri << QPointF(cx - 4.5, 0) << QPointF(cx + 4.5, 0)
            << QPointF(cx, 8);
        p.drawPolygon(tri);

        // 气泡：zone 时长（拖 in=入>长 / 拖 out=长<出），等宽字体
        if (hasZone && mBubbleMode > 0) {
            const int dur = mZoneOut - mZoneIn + 1;
            QString label;
            if (mBubbleMode == 2) {
                label = QStringLiteral("%1 > %2")
                        .arg(mMon->timecode(mZoneIn))
                        .arg(mMon->timecode(dur));
            } else if (mBubbleMode == 3) {
                label = QStringLiteral("%1 < %2")
                        .arg(mMon->timecode(dur))
                        .arg(mMon->timecode(mZoneOut));
            } else {
                label = mMon->timecode(dur);
            }
            QFont f = QFontDatabase::systemFont(QFontDatabase::FixedFont);
            f.setPixelSize(10);
            const QFontMetrics fm(f);
            const int bw = fm.horizontalAdvance(label) + 10;
            int bx = (xOfFrame(mZoneIn) + xOfFrame(mZoneOut)) / 2 - bw / 2;
            bx = qBound(0, bx, qMax(0, width() - bw));
            const QRect br(bx, -15, bw, 15);
            p.setFont(f);
            p.fillRect(br, QColor(0x39, 0x39, 0x3e));
            p.setPen(QColor(0xf0, 0xf0, 0xf2));
            p.drawText(br, Qt::AlignCenter, label);
        }
    }

    void mousePressEvent(QMouseEvent *e) override {
        mMon->setFocus(Qt::MouseFocusReason);
        if (mFrameCount <= 0) { return; }
        if (e->button() != Qt::LeftButton) { return; }
        const bool hasZone = mZoneOut >= mZoneIn;
        if (hasZone) {
            const int ix = xOfFrame(mZoneIn);
            const int ox = xOfFrame(mZoneOut);
            if (qAbs(e->pos().x() - ix) <= grabW()) {
                mGrab = Grab::InEdge;
                mBubbleMode = 2;
                return;
            }
            if (qAbs(e->pos().x() - ox) <= grabW()) {
                mGrab = Grab::OutEdge;
                mBubbleMode = 3;
                return;
            }
        }
        mGrab = Grab::Seek;
        mMon->seekTo(frameAtX(e->pos().x()));
    }

    void mouseMoveEvent(QMouseEvent *e) override {
        if (mFrameCount <= 0) { return; }
        const bool hasZone = mZoneOut >= mZoneIn;
        if (mGrab == Grab::None) {
            int hover = 0;
            if (hasZone) {
                const int ix = xOfFrame(mZoneIn);
                const int ox = xOfFrame(mZoneOut);
                if (qAbs(e->pos().x() - ix) <= grabW()) { hover = 1; }
                else if (qAbs(e->pos().x() - ox) <= grabW()) {
                    hover = 2;
                }
            }
            if (hover != mHoverEdge) {
                mHoverEdge = hover;
                update();
            }
            setCursor(hover ? Qt::SizeHorCursor : Qt::ArrowCursor);
            // 悬停 zone 内显示时长气泡
            const int oldBubble = mBubbleMode;
            mBubbleMode = 0;
            if (hasZone && hover == 0) {
                const int f = frameAtX(e->pos().x());
                if (f >= mZoneIn && f <= mZoneOut) { mBubbleMode = 1; }
            }
            if (mBubbleMode != oldBubble) { update(); }
            return;
        }
        const int f = frameAtX(e->pos().x());
        if (mGrab == Grab::Seek) {
            mMon->seekTo(f);
        } else if (mGrab == Grab::InEdge) {
            mMon->setZoneIn(f);
            // kdenlive：Shift 拖动 = 调 zone 同时定位
            if (e->modifiers() & Qt::ShiftModifier) { mMon->seekTo(f); }
        } else if (mGrab == Grab::OutEdge) {
            mMon->setZoneOut(f);
            if (e->modifiers() & Qt::ShiftModifier) { mMon->seekTo(f); }
        }
    }

    void mouseReleaseEvent(QMouseEvent *) override {
        mGrab = Grab::None;
        mBubbleMode = 0;
        update();
    }

    void wheelEvent(QWheelEvent *e) override {
        if (mFrameCount <= 0) { return; }
        if (!(e->modifiers() & Qt::ControlModifier)) { return; }
        // kdenlive zoomIn/OutRuler：1.2 步进；以鼠标位置为锚（锚帧
        // 在窗口内的比例缩放前后不变），窗口最小 8 帧、最大全长
        const int zf = zoomFrames();
        if (zf <= 0) { return; }
        const qreal factor = e->angleDelta().y() > 0 ? 1 / 1.25 : 1.25;
        int nz = qMax(8, qMin(mFrameCount, int(qRound(zf * factor))));
        if (nz == zf) { return; }
        const int anchor = frameAtX(int(e->position().x()));
        const qreal r = qBound(0., qreal(anchor - mZoomFrom) / zf, 1.);
        int from = qRound(anchor - r * nz);
        from = qBound(0, from, qMax(0, mFrameCount - nz));
        mZoomFrom = from;
        mZoomFrames = nz;
        update();
        e->accept();
    }

    void mouseDoubleClickEvent(QMouseEvent *) override {
        // 双击标尺 = 回到全宽（快捷复位入口）
        if (zoomed()) { resetZoom(); }
    }

    void leaveEvent(QEvent *) override {
        if (mGrab == Grab::None && (mHoverEdge || mBubbleMode)) {
            mHoverEdge = 0;
            mBubbleMode = 0;
            update();
        }
    }

    int mHoverEdge = 0; // 0 none 1 in 2 out

    // 播放头更新入口：越窗时自动滚动追边（kdenlive 的 seekOffset
    // 逻辑——播放头贴边留 8% 余量再滚，避免频繁跳动）
public:
    void setCurrent(const int f) {
        mCurrent = f;
        const int zf = zoomFrames();
        if (!zoomed() || zf <= 0 || width() <= 0) { return; }
        const int edge = qMax(1, width() / 12);
        const int px = (mCurrent - mZoomFrom) * width() / zf;
        if (px < edge && mZoomFrom > 0) {
            mZoomFrom = qMax(0, mCurrent - edge * zf / width());
        } else if (px > width() - edge) {
            mZoomFrom = qMin(qMax(0, mFrameCount - zf),
                             mCurrent - (width() - edge) * zf / width());
        }
    }
};

// ---------------------------------------------------------------- panel

ClipMonitorWidget::ClipMonitorWidget(QWidget *parent) : QWidget(parent) {
    mPlayer = new QMediaPlayer(this);
    mAudio = new QAudioOutput(this);
    mPlayer->setAudioOutput(mAudio);
    mSink = new QVideoSink(this);
    mPlayer->setVideoSink(mSink);
    connect(mSink, &QVideoSink::videoFrameChanged, this,
            [this](const QVideoFrame &frame) {
        if (frame.isValid()) {
            mView->mFrame = frame.toImage();
            mView->update();
        }
    });
    connect(mPlayer, &QMediaPlayer::positionChanged, this,
            [this](const qint64 ms) {
        mCurrentFrame = frameFromMs(ms);
        mRuler->setCurrent(mCurrentFrame);
        mRuler->update();
        mView->mOverlayTc = timecode(mCurrentFrame);
        mView->update();
    });
    connect(mPlayer, &QMediaPlayer::playbackStateChanged, this,
            [this](const QMediaPlayer::PlaybackState state) {
        const bool playing = state == QMediaPlayer::PlayingState;
        mPlayBtn->setIcon(QIcon(glyphPixmap(
                playing ? kSvgPause : kSvgPlay,
                QColor(0xd8, 0xd8, 0xdc))));
        mPlayBtn->setToolTip(playing ? QStringLiteral("暂停（空格）")
                                      : QStringLiteral("播放（空格）"));
    });
    connect(mPlayer, &QMediaPlayer::mediaStatusChanged, this,
            [this](const QMediaPlayer::MediaStatus status) {
        if (status != QMediaPlayer::LoadedMedia) { return; }
        mDurationMs = mPlayer->duration();
        const auto rate = mPlayer->metaData().value(
                    QMediaMetaData::VideoFrameRate);
        mFps = rate.isValid() && rate.toReal() > 1. ? rate.toReal() : 30.;
        mFrameCount = qMax(1, int(qRound(mDurationMs * mFps / 1000.)));
        mRuler->mFrameCount = mFrameCount;
        // 重置只在新装载（loadFile 换了素材）：LoadedMedia 会重发
        // （播放到头再播、seek 回冲、后端重新缓冲），无条件重置会把
        // 用户设好的出入点悄悄抹回全片段（"设置了会自动改变"根因）
        mRuler->mZoomFrom = 0;
        mRuler->mZoomFrames = mFrameCount; // 新装载回全宽
        if (!mZoneResetPending) {
            // 尺长刷新后 zone 可能越界，夹回即可（值不动）
            mZoneIn = qBound(0, mZoneIn, mFrameCount - 1);
            mZoneOut = qBound(mZoneIn, qMax(mZoneIn, mZoneOut),
                              mFrameCount - 1);
            mRuler->mZoneIn = mZoneIn;
            mRuler->mZoneOut = mZoneOut;
            mRuler->update();
            return;
        }
        mZoneResetPending = false;
        // kdenlive 装载即 zone=全片段（monitor.cpp setZone(0, dur)）：
        // zone 永远可见，I/O 只是挪边，"清除"恢复全片段
        mZoneIn = 0;
        mZoneOut = mFrameCount - 1;
        mRuler->mZoneIn = mZoneIn;
        mRuler->mZoneOut = mZoneOut;
        mRuler->update();
        updateControls();
        emit zoneChanged();
    });
    connect(mPlayer, &QMediaPlayer::errorOccurred, this,
            [this](const QMediaPlayer::Error, const QString &msg) {
        emit logMessage(QStringLiteral("监视器加载失败：%1").arg(msg));
    });

    mView = new MonitorView(this);
    mView->mEmptyText = QStringLiteral(
                "打开素材或单击项目面板条目\n按住画面拖到时间轴放置");
    mRuler = new MonitorRuler(this);

    // ---- 控制栏（kdenlive 布局：左=走带，右=zone）----
    const auto mkBtn = [this](const char * const svg, const QString &tip)
            -> QToolButton* {
        auto * const b = new QToolButton(this);
        b->setIcon(QIcon(glyphPixmap(svg, QColor(0xd8, 0xd8, 0xdc))));
        b->setToolTip(tip);
        b->setAutoRaise(true);
        return b;
    };
    auto * const startBtn = mkBtn(kSvgStart, QStringLiteral("片首（Home）"));
    connect(startBtn, &QToolButton::clicked, this,
            [this]() { seekTo(0); });
    auto * const backBtn = mkBtn(kSvgFrameBack,
                                 QStringLiteral("上一帧（←）"));
    connect(backBtn, &QToolButton::clicked, this, [this]() { stepFrame(-1); });
    mPlayBtn = mkBtn(kSvgPlay, QStringLiteral("播放（空格）"));
    connect(mPlayBtn, &QToolButton::clicked, this,
            [this]() { togglePlay(); });
    auto * const fwdBtn = mkBtn(kSvgFrameFwd, QStringLiteral("下一帧（→）"));
    connect(fwdBtn, &QToolButton::clicked, this, [this]() { stepFrame(1); });
    auto * const endBtn = mkBtn(kSvgEnd, QStringLiteral("片尾（End）"));
    connect(endBtn, &QToolButton::clicked, this, [this]() { seekToEnd(); });

    auto * const zStartBtn = mkBtn(
                kSvgZoneStart, QStringLiteral("跳到入点（Shift+I）"));
    connect(zStartBtn, &QToolButton::clicked, this, [this]() {
        if (hasZone()) { seekTo(mZoneIn); }
    });
    auto * const markInBtn = mkBtn(kSvgMarkIn,
                                   QStringLiteral("设入点（I）"));
    connect(markInBtn, &QToolButton::clicked, this,
            [this]() { setZoneIn(mCurrentFrame); });
    auto * const markOutBtn = mkBtn(kSvgMarkOut,
                                    QStringLiteral("设出点（O）"));
    connect(markOutBtn, &QToolButton::clicked, this,
            [this]() { setZoneOut(mCurrentFrame); });
    auto * const zEndBtn = mkBtn(kSvgZoneEnd,
                                 QStringLiteral("跳到出点（Shift+O）"));
    connect(zEndBtn, &QToolButton::clicked, this, [this]() {
        if (hasZone()) { seekTo(mZoneOut); }
    });
    auto * const zClearBtn = mkBtn(
                kSvgZoneClear, QStringLiteral("重置为全片段"));
    connect(zClearBtn, &QToolButton::clicked, this, [this]() { resetZone(); });
    auto * const openBtn = mkBtn(kSvgOpen, QStringLiteral("打开素材文件"));
    connect(openBtn, &QToolButton::clicked, this,
            [this]() { emit openRequested(); });

    mTimeLabel = new QLabel(this);
    {
        QFont f = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        f.setPixelSize(11);
        mTimeLabel->setFont(f);
    }
    mTimeLabel->setAlignment(Qt::AlignVCenter | Qt::AlignRight);

    mFileLabel = new QLabel(tr("未加载素材"), this);
    mFileLabel->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);

    const auto controls = new QHBoxLayout();
    controls->setContentsMargins(4, 0, 4, 0);
    controls->setSpacing(1);
    controls->addWidget(startBtn);
    controls->addWidget(backBtn);
    controls->addWidget(mPlayBtn);
    controls->addWidget(fwdBtn);
    controls->addWidget(endBtn);
    controls->addSpacing(8);
    controls->addWidget(zStartBtn);
    controls->addWidget(markInBtn);
    controls->addWidget(markOutBtn);
    controls->addWidget(zEndBtn);
    controls->addWidget(zClearBtn);
    controls->addStretch(1);
    controls->addWidget(openBtn);
    controls->addSpacing(6);
    controls->addWidget(mTimeLabel);

    const auto layout = new QVBoxLayout(this);
    layout->setContentsMargins(2, 2, 2, 2);
    layout->setSpacing(2);
    layout->addWidget(mView, 1);
    layout->addWidget(mRuler);
    layout->addWidget(mFileLabel);
    layout->addLayout(controls);

    // StrongFocus：点击面板（view/ruler 的 setFocus）后键盘进面板
    setFocusPolicy(Qt::StrongFocus);
}

// 键盘路由（kdenlive 快捷键）：焦点在面板或从子控件冒泡都收。
// 焦点不在面板时 I/O 归时间轴 dock（friction 原生场景入出点）
void ClipMonitorWidget::keyPressEvent(QKeyEvent *event) {
    const int key = event->key();
    const bool shift = event->modifiers() & Qt::ShiftModifier;
    // kdenlive：I/O 设 zone，Shift+I/O 跳 zone 首尾
    if (key == Qt::Key_I && !shift) { setZoneIn(mCurrentFrame); return; }
    if (key == Qt::Key_O && !shift) { setZoneOut(mCurrentFrame); return; }
    if (key == Qt::Key_I && shift && hasZone()) { seekTo(mZoneIn); return; }
    if (key == Qt::Key_O && shift && hasZone()) { seekTo(mZoneOut); return; }
    if (key == Qt::Key_Space && !(event->modifiers() & Qt::ShiftModifier)) {
        togglePlay();
        return;
    }
    if (key == Qt::Key_Left) { stepFrame(-1); return; }
    if (key == Qt::Key_Right) { stepFrame(1); return; }
    if (key == Qt::Key_Home) { seekTo(0); return; }
    if (key == Qt::Key_End) { seekToEnd(); return; }
    QWidget::keyPressEvent(event);
}

void ClipMonitorWidget::loadFile(const QString &path) {
    if (path.isEmpty()) { return; }
    // 同素材重装（双击再导入/再点项目面板条目）：保住用户出入点，
    // kdenlive 的 zone 也是 per-clip 持久不随装载清
    if (path == mPath && mFrameCount > 0) { return; }
    mPath = path;
    mZoneResetPending = true;
    mZoneIn = 0;
    mZoneOut = -1; // 加载完成（LoadedMedia）后置全片段
    mFrameCount = 0;
    mCurrentFrame = 0;
    mView->mFrame = QImage();
    mView->mOverlayFile = path.section('/', -1);
    mView->mOverlayTc.clear();
    mRuler->mFrameCount = 0;
    mRuler->mZoneIn = 0;
    mRuler->mZoneOut = -1;
    mRuler->mZoomFrom = 0;
    mRuler->mZoomFrames = 0;
    mRuler->update();
    mFileLabel->setText(path);
    mPlayer->setSource(QUrl::fromLocalFile(path));
    mPlayer->pause();
    mPlayer->setPosition(0);
    updateControls();
    emit zoneChanged();
}

int ClipMonitorWidget::frameFromMs(const qint64 ms) const {
    if (mFps <= 0.) { return 0; }
    return qBound(0, int(qRound(ms * mFps / 1000.)),
                  qMax(0, mFrameCount - 1));
}

qint64 ClipMonitorWidget::msFromFrame(const int frame) const {
    if (mFps <= 0.) { return 0; }
    return qint64(qRound(frame * 1000. / mFps));
}

// kdenlive 默认时间码格式 HH:MM:SS:FF
QString ClipMonitorWidget::timecode(const int frame) const {
    const int fps = qMax(1, int(mFps));
    const int total = qMax(0, frame);
    return QStringLiteral("%1:%2:%3:%4")
            .arg(total / (3600 * fps), 2, 10, QChar('0'))
            .arg((total / (60 * fps)) % 60, 2, 10, QChar('0'))
            .arg((total / fps) % 60, 2, 10, QChar('0'))
            .arg(total % fps, 2, 10, QChar('0'));
}

void ClipMonitorWidget::setZoneIn(const int frame) {
    if (mFrameCount <= 0) { return; }
    mZoneIn = qBound(0, frame, mFrameCount - 1);
    if (mZoneOut >= 0 && mZoneOut < mZoneIn) { mZoneOut = mZoneIn; }
    mRuler->mZoneIn = mZoneIn;
    mRuler->mZoneOut = mZoneOut;
    mRuler->update();
    updateControls();
    emit zoneChanged();
}

void ClipMonitorWidget::setZoneOut(const int frame) {
    if (mFrameCount <= 0) { return; }
    mZoneOut = qBound(0, frame, mFrameCount - 1);
    if (mZoneIn > mZoneOut) { mZoneIn = mZoneOut; }
    mRuler->mZoneIn = mZoneIn;
    mRuler->mZoneOut = mZoneOut;
    mRuler->update();
    updateControls();
    emit zoneChanged();
}

void ClipMonitorWidget::setZone(const int in, const int out) {
    if (mFrameCount <= 0) { return; }
    if (out < in) { resetZone(); return; }
    mZoneIn = qBound(0, in, mFrameCount - 1);
    mZoneOut = qBound(mZoneIn, out, mFrameCount - 1);
    mRuler->mZoneIn = mZoneIn;
    mRuler->mZoneOut = mZoneOut;
    mRuler->update();
    updateControls();
    emit zoneChanged();
}

// 清除 = 恢复全片段（kdenlive：zone 恒存在，清除即回全片段）
void ClipMonitorWidget::resetZone() {
    if (mFrameCount <= 0) { return; }
    mZoneIn = 0;
    mZoneOut = mFrameCount - 1;
    mRuler->mZoneIn = mZoneIn;
    mRuler->mZoneOut = mZoneOut;
    mRuler->update();
    updateControls();
    emit zoneChanged();
}

void ClipMonitorWidget::togglePlay() {
    if (mPlayer->playbackState() == QMediaPlayer::PlayingState) {
        mPlayer->pause();
    } else if (!mPath.isEmpty()) {
        mPlayer->play();
    }
}

void ClipMonitorWidget::seekTo(const int frame) {
    mPlayer->pause();
    mPlayer->setPosition(msFromFrame(
                             qBound(0, frame, qMax(0, mFrameCount - 1))));
}

void ClipMonitorWidget::seekToEnd() {
    seekTo(qMax(0, mFrameCount - 1));
}

void ClipMonitorWidget::stepFrame(const int frames) {
    seekTo(qBound(0, mCurrentFrame + frames, qMax(0, mFrameCount - 1)));
}

void ClipMonitorWidget::updateControls() {
    if (mFrameCount <= 0) {
        mTimeLabel->setText(QStringLiteral("--:--:--:--"));
    } else {
        mTimeLabel->setText(QStringLiteral("%1 / %2")
                                    .arg(timecode(mCurrentFrame),
                                         timecode(mFrameCount - 1)));
    }
    if (hasZone()) {
        mFileLabel->setText(QStringLiteral(
                    "入 %1 · 出 %2 · 区间 %3 帧")
                                .arg(timecode(mZoneIn),
                                     timecode(mZoneOut))
                                .arg(mZoneOut - mZoneIn + 1));
    } else if (!mPath.isEmpty()) {
        mFileLabel->setText(QStringLiteral("%1（未设出入点，拖出=全片段）")
                                    .arg(mPath));
    } else {
        mFileLabel->setText(tr("未加载素材"));
    }
}
