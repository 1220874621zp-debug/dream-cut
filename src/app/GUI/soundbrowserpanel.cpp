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

#include "soundbrowserpanel.h"

#include "widgets/flowlayout.h"
#include "appsupport.h"
#include "exceptions.h"
#include "filesourcescache.h"
#include "renderhandler.h"
#include "themesupport.h"

#include "CacheHandlers/soundcachehandler.h"
#include "Sound/soundpeaks.h"

#include <QAudioOutput>
#include <QApplication>
#include <QDesktopServices>
#include <QDir>
#include <QDrag>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMediaPlayer>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScrollBar>
#include <QScrollArea>
#include <QSlider>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include <cmath>

// ---------------------------------------------------------------
// SoundCardWidget
// ---------------------------------------------------------------

namespace {
constexpr int CARD_W = 190;
constexpr int CARD_H = 104;
constexpr int WAVE_H = 64;

// 音效库扩展白名单 = 工程声音扩展 + 常见流式格式
QStringList soundExtensions()
{
    static const QStringList exts = QStringList(FileExtensions::sound)
            << QStringLiteral("ogg") << QStringLiteral("opus")
            << QStringLiteral("aac") << QStringLiteral("wma");
    return exts;
}
} // namespace

SoundCardWidget::SoundCardWidget(const QString& path, const QString& title,
                                 QWidget* const parent)
    : QFrame(parent)
    , mPath(path)
    , mTitle(title)
{
    setFixedSize(CARD_W, CARD_H);
    setCursor(Qt::PointingHandCursor);
    setMouseTracking(true);

    // 收藏星标（特效面板 EffectFavButton 同款：自绘星，不依赖主题图标）
    mStarBtn = new QToolButton(this);
    mStarBtn->setCheckable(true);
    mStarBtn->setCursor(Qt::PointingHandCursor);
    mStarBtn->setToolTip(QString::fromUtf8("收藏"));
    mStarBtn->setFocusPolicy(Qt::NoFocus);
    mStarBtn->setFixedSize(22, 22);
    mStarBtn->move(width() - 24, 3);
    connect(mStarBtn, &QToolButton::toggled, this, [this](const bool on) {
        mFavorite = on;
        emit favoriteToggled(this, on);
        update();
    });
}

void SoundCardWidget::resizeEvent(QResizeEvent* e)
{
    QFrame::resizeEvent(e);
    mStarBtn->move(width() - 24, 3);
}

void SoundCardWidget::setDurationSec(const qreal sec)
{
    mDurationSec = qMax(0., sec);
    if (mDurationSec <= 0) { setDecodeError(); return; }
    update();
}

void SoundCardWidget::setExpectedCols(const int totalCols)
{
    mTotalCols = qMax(0, totalCols);
}

void SoundCardWidget::appendPeaks(const QVector<qreal>& cols)
{
    if (cols.isEmpty()) { return; }
    mCols += cols;
    update();
}

void SoundCardWidget::setDecodeError()
{
    mError = true;
    mDecodable = false;
    update();
}

void SoundCardWidget::detachHandler()
{
    // 停序列（再激活时已解秒从共享缓存秒回）；已绘包络保留
    mDecodable = false;
}

void SoundCardWidget::setPlaying(const bool on)
{
    if (mPlaying == on) { return; }
    mPlaying = on;
    if (!on) { mPlayFrac = -1; }
    update();
}

void SoundCardWidget::setPlayProgress(const qreal frac)
{
    mPlayFrac = qBound(0., frac, 1.);
    update();
}

void SoundCardWidget::setFavorite(const bool on)
{
    if (mStarBtn->isChecked() != on) { mStarBtn->setChecked(on); }
    mFavorite = on;
    update();
}

void SoundCardWidget::paintEvent(QPaintEvent* e)
{
    Q_UNUSED(e)
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    const QColor bg = ThemeSupport::getThemeBaseDarkColor();
    QColor border = ThemeSupport::getThemeButtonBorderColor();
    if (mPlaying) { border = ThemeSupport::getThemeHighlightColor(); }

    const QRectF r = rect().adjusted(0.5, 0.5, -0.5, -0.5);
    QPainterPath framePath;
    framePath.addRoundedRect(r, 6, 6);
    p.fillPath(framePath, bg);
    p.setPen(QPen(border, 1));
    p.drawPath(framePath);

    // 波形包络区
    const QRectF waveRect(6, 6, width() - 12, WAVE_H);
    if (mError) {
        p.setPen(QPen(ThemeSupport::getThemeColorTextDisabled(), 1));
        p.drawText(rect(), Qt::AlignCenter,
                   QString::fromUtf8("无法解码"));
        return;
    }
    if (!mCols.isEmpty() && mTotalCols > 0) {
        // 已解列按解码进度占比铺左侧，右侧余量提示省略号
        // （每秒到达重绘一次，渐进填充）；波形基色=白（用户指定），
        // 试听已播部分叠主题强调色显示进度
        const qreal frac = qMin(1., qreal(mCols.size()) / mTotalCols);
        const qreal drawW = qMax(1., waveRect.width() * frac);
        const qreal midY = waveRect.center().y();
        const qreal halfH = waveRect.height() / 2 - 2;
        const QColor baseCol(255, 255, 255);
        const QColor playedCol = ThemeSupport::getThemeHighlightColor();
        const int n = mCols.size();
        int prevX = -1;
        for (int x = 0; x < int(drawW); x++) {
            const int idx = qBound(0, int(qreal(x) / drawW * n), n - 1);
            const qreal a = qBound(0., mCols.at(idx), 1.);
            const int xi = qRound(waveRect.left()) + x;
            if (xi == prevX) { continue; }
            prevX = xi;
            const bool played = mPlayFrac >= 0. &&
                    qreal(idx) / n <= mPlayFrac;
            p.setPen(QPen(played ? playedCol : baseCol, 1));
            p.drawLine(xi, qRound(midY - a * halfH),
                       xi, qRound(midY + a * halfH));
        }
        if (frac < 1.) {
            p.setPen(QPen(ThemeSupport::getThemeColorTextDisabled(), 1));
            p.drawText(waveRect, Qt::AlignRight | Qt::AlignVCenter,
                       QString::fromUtf8("…"));
        }
    } else if (mDecodable) {
        p.setPen(QPen(ThemeSupport::getThemeColorTextDisabled(), 1));
        p.drawText(waveRect, Qt::AlignCenter, QString::fromUtf8("…"));
    }

    // 名称（省略）+ 时长
    p.setPen(QPen(palette().color(QPalette::WindowText), 1));
    QFont f = font();
    f.setPointSizeF(9.);
    p.setFont(f);
    const QRectF nameRect(8, height() - 26, width() - 66, 18);
    p.drawText(nameRect, Qt::AlignVCenter | Qt::AlignLeft,
               fontMetrics().elidedText(mTitle, Qt::ElideRight,
                                        int(nameRect.width())));
    if (mDurationSec > 0) {
        p.setPen(QPen(ThemeSupport::getThemeColorTextDisabled(), 1));
        const QRectF durRect(width() - 56, height() - 26, 48, 18);
        p.drawText(durRect, Qt::AlignVCenter | Qt::AlignRight,
                   durationLabel(mDurationSec));
    }
}

void SoundCardWidget::enterEvent(QEnterEvent* e)
{
    QFrame::enterEvent(e);
    emit hoverEntered(this);
}

void SoundCardWidget::leaveEvent(QEvent* e)
{
    QFrame::leaveEvent(e);
    emit hoverLeft(this);
}

void SoundCardWidget::mousePressEvent(QMouseEvent* e)
{
    if (e->button() == Qt::LeftButton) {
        mPressPos = e->position().toPoint();
        mDragging = false;
    }
    QFrame::mousePressEvent(e);
}

void SoundCardWidget::mouseMoveEvent(QMouseEvent* e)
{
    if (!mDragging && (e->buttons() & Qt::LeftButton) &&
        (e->position().toPoint() - mPressPos).manhattanLength() >
            QApplication::startDragDistance()) {
        mDragging = true;
        // 拖拽前停掉试听（不残声）
        emit hoverLeft(this);
        startDrag();
        return;
    }
    QFrame::mouseMoveEvent(e);
}

void SoundCardWidget::mouseReleaseEvent(QMouseEvent* e)
{
    QFrame::mouseReleaseEvent(e);
    if (e->button() == Qt::LeftButton && !mDragging) {
        // 未成拖拽的按下-释放 = 播放/暂停切换
        emit clicked(this);
    }
    mDragging = false;
}

void SoundCardWidget::mouseDoubleClickEvent(QMouseEvent* e)
{
    QFrame::mouseDoubleClickEvent(e);
    if (e->button() == Qt::LeftButton) { emit doubleClicked(this); }
}

void SoundCardWidget::contextMenuEvent(QContextMenuEvent* e)
{
    QFrame::contextMenuEvent(e);
    emit contextRequested(this, e->globalPos());
}

void SoundCardWidget::startDrag()
{
    auto mime = new QMimeData;
    // 与片段监视器同款 clip-path 键：时间轴拖放零改动接受；
    // 秒数键供音频块长度换算（视图按场景 fps 取整）；
    // url 兜底 = 拖到画布走常规导入
    mime->setData(QStringLiteral("application/x-dreamcut-clip-path"),
                  mPath.toUtf8());
    mime->setData(QStringLiteral("application/x-dreamcut-clip-sec"),
                  QString::number(mDurationSec, 'f', 3).toUtf8());
    mime->setUrls({QUrl::fromLocalFile(mPath)});
    QDrag drag(this);
    drag.setMimeData(mime);
    const QPixmap pm = grab(QRect(0, 0, width(), WAVE_H + 4));
    if (!pm.isNull()) {
        drag.setPixmap(pm.scaled(width() / 2, (WAVE_H + 4) / 2,
                                 Qt::KeepAspectRatio,
                                 Qt::SmoothTransformation));
        drag.setHotSpot(QPoint(pm.width() / 4, pm.height() / 4));
    }
    drag.exec(Qt::CopyAction);
}

QString SoundCardWidget::durationLabel(const qreal sec)
{
    if (sec < 60.) {
        return QString::number(sec, 'f', 1) + QString::fromUtf8("s");
    }
    return QString::fromUtf8("%1:%2")
            .arg(int(sec) / 60)
            .arg(int(sec) % 60, 2, 10, QChar('0'));
}

// ---------------------------------------------------------------
// SoundBrowserPanel
// ---------------------------------------------------------------

SoundBrowserPanel::SoundBrowserPanel(QWidget* const parent)
    : QWidget(parent)
{
    setupUi();
    setupPlayback();
    loadFavorites();

    mWatcher = new QFileSystemWatcher(this);
    mRescanTimer = new QTimer(this);
    mRescanTimer->setSingleShot(true);
    mRescanTimer->setInterval(600);
    connect(mRescanTimer, &QTimer::timeout, this,
            &SoundBrowserPanel::rescan);
    connect(mWatcher, &QFileSystemWatcher::directoryChanged,
            this, [this]() { mRescanTimer->start(); });

    const auto saved = AppSupport::getSettings(
                QStringLiteral("SoundBrowser"),
                QStringLiteral("rootDir")).toString();
    if (!saved.isEmpty() && QDir(saved).exists()) {
        setRootDir(saved);
    } else {
        mHintLabel->setVisible(true);
    }

    // 布局落定后再判可视集（首帧 + 视口尺寸变化都可能改几何）
    mGridScroll->viewport()->installEventFilter(this);
}

bool SoundBrowserPanel::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == mGridScroll->viewport() &&
        (event->type() == QEvent::Resize ||
         event->type() == QEvent::Paint)) {
        updateVisibleSet();
    }
    return QWidget::eventFilter(watched, event);
}

void SoundBrowserPanel::setupUi()
{
    const auto rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(4, 4, 4, 4);
    rootLayout->setSpacing(4);

    // 目录行
    const auto dirRow = new QWidget(this);
    const auto dirLayout = new QHBoxLayout(dirRow);
    dirLayout->setContentsMargins(2, 0, 2, 0);
    dirLayout->setSpacing(6);
    mDirButton = new QToolButton(dirRow);
    mDirButton->setText(QString::fromUtf8("选择音效库目录"));
    mDirButton->setToolTip(QString::fromUtf8(
                "选择音效库根目录（子文件夹即分类）"));
    mDirButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
    connect(mDirButton, &QToolButton::clicked, this, [this]() {
        const auto dir = QFileDialog::getExistingDirectory(
                    this, QString::fromUtf8("选择音效库目录"),
                    mRootDir.isEmpty() ? QDir::homePath() : mRootDir);
        if (dir.isEmpty()) { return; }
        setRootDir(dir);
    });
    dirLayout->addWidget(mDirButton, 1);

    mSearchEdit = new QLineEdit(dirRow);
    mSearchEdit->setPlaceholderText(QString::fromUtf8("搜索音效"));
    mSearchEdit->setClearButtonEnabled(true);
    mSearchEdit->setFixedHeight(26);
    mSearchEdit->setFixedWidth(150);
    connect(mSearchEdit, &QLineEdit::textChanged, this,
            [this](const QString& text) {
        mFilter = text.trimmed();
        applyFilter();
    });
    dirLayout->addWidget(mSearchEdit, 0);

    mVolSlider = new QSlider(Qt::Horizontal, dirRow);
    mVolSlider->setRange(0, 100);
    mVolSlider->setValue(80);
    mVolSlider->setFixedWidth(80);
    mVolSlider->setToolTip(QString::fromUtf8("试听音量"));
    dirLayout->addWidget(mVolSlider, 0);
    rootLayout->addWidget(dirRow, 0);

    // 分类 pill 行
    mCategoryHost = new QWidget(this);
    mCategoryLayout = new FlowLayout(mCategoryHost, 0, 4, 4);
    rootLayout->addWidget(mCategoryHost, 0);

    // 卡片网格
    mGridScroll = new QScrollArea(this);
    mGridScroll->setWidgetResizable(true);
    mGridScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    mGridScroll->setFrameShape(QFrame::NoFrame);
    mGridHost = new QWidget();
    mGridLayout = new FlowLayout(mGridHost, 6, 6, 6);
    mGridScroll->setWidget(mGridHost);
    rootLayout->addWidget(mGridScroll, 1);

    connect(mGridScroll->verticalScrollBar(), &QScrollBar::valueChanged,
            this, [this]() { updateVisibleSet(); });
    connect(mGridScroll->horizontalScrollBar(), &QScrollBar::valueChanged,
            this, [this]() { updateVisibleSet(); });

    mHintLabel = new QLabel(this);
    mHintLabel->setWordWrap(true);
    mHintLabel->setAlignment(Qt::AlignCenter);
    rootLayout->addWidget(mHintLabel, 0);
    mHintLabel->setVisible(false);
}

void SoundBrowserPanel::setupPlayback()
{
    mPlayer = new QMediaPlayer(this);
    mAudioOut = new QAudioOutput(this);
    mPlayer->setAudioOutput(mAudioOut);
    mAudioOut->setVolume(0.8f);
    if (mVolSlider) {
        connect(mVolSlider, &QSlider::valueChanged, this, [this](
                    const int v) { mAudioOut->setVolume(v / 100.f); });
    }

    mHoverTimer = new QTimer(this);
    mHoverTimer->setSingleShot(true);
    mHoverTimer->setInterval(350);
    connect(mHoverTimer, &QTimer::timeout, this,
            &SoundBrowserPanel::playHovered);

    // 试听进度联动：卡片波形已播部分叠强调色
    connect(mPlayer, &QMediaPlayer::positionChanged, this,
            [this](const qint64 pos) {
        if (!mPlayingCard) { return; }
        const qint64 dur = mPlayer->duration();
        if (dur > 0) {
            mPlayingCard->setPlayProgress(qreal(pos) / qreal(dur));
        }
    });
    connect(mPlayer, &QMediaPlayer::mediaStatusChanged, this,
            [this](const QMediaPlayer::MediaStatus status) {
        if (status == QMediaPlayer::EndOfMedia) { stopPreview(); }
    });

    // 与工程播放互斥：工程播放期间悬停试听不起播，
    // 播放开始时掐掉已在放的试听（防两路叠音）
    connect(RenderHandler::sInstance, &RenderHandler::previewBeingPlayed,
            this, [this]() {
        mPlaybackSuppressed = true;
        stopPreview();
    });
    connect(RenderHandler::sInstance, &RenderHandler::previewPaused,
            this, [this]() { mPlaybackSuppressed = false; });
    connect(RenderHandler::sInstance, &RenderHandler::previewFinished,
            this, [this]() { mPlaybackSuppressed = false; });
}

bool SoundBrowserPanel::isSoundFile(const QString& path)
{
    const QString ext = QFileInfo(path).suffix().toLower();
    return soundExtensions().contains(ext);
}

void SoundBrowserPanel::setRootDir(const QString& dir)
{
    mRootDir = QDir::cleanPath(dir);
    AppSupport::setSettings(QStringLiteral("SoundBrowser"),
                            QStringLiteral("rootDir"), mRootDir);
    const QFileInfo info(mRootDir);
    mDirButton->setText(info.fileName().isEmpty() ? mRootDir
                                                  : info.fileName());
    mDirButton->setToolTip(mRootDir);
    mCategory.clear();
    rescan();
}

int SoundBrowserPanel::decodedCardCount() const
{
    int n = 0;
    for (auto it = mCards.constBegin(); it != mCards.constEnd(); ++it) {
        if (it.value() && it.value()->hasEnvelope()) { n++; }
    }
    return n;
}

void SoundBrowserPanel::rebuildWatcher()
{
    const auto watched = mWatcher->directories();
    if (!watched.isEmpty()) { mWatcher->removePaths(watched); }
    if (mRootDir.isEmpty() || !QDir(mRootDir).exists()) { return; }
    mWatcher->addPath(mRootDir);
    const auto dirs = QDir(mRootDir).entryList(
                QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const auto& d : dirs) { mWatcher->addPath(mRootDir + '/' + d); }
}

void SoundBrowserPanel::rescan()
{
    rebuildWatcher();
    mCategories.clear();
    mAllEntries.clear();
    if (mRootDir.isEmpty() || !QDir(mRootDir).exists()) {
        rebuildCategoryPills();
        applyFilter();
        return;
    }

    QDir root(mRootDir);
    // 分类 = 一级子目录；根级散文件归"未分类"
    const auto subdirs = root.entryList(
                QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const auto& sub : subdirs) {
        const QDir subDir(mRootDir + '/' + sub);
        const auto files = subDir.entryList(QDir::Files, QDir::Name);
        for (const auto& f : files) {
            if (!isSoundFile(f)) { continue; }
            Entry e;
            e.path = subDir.absoluteFilePath(f);
            e.title = QFileInfo(f).completeBaseName();
            e.category = sub;
            mAllEntries.append(e);
        }
        mCategories.insert(sub);
    }
    const auto rootFiles = root.entryList(QDir::Files, QDir::Name);
    for (const auto& f : rootFiles) {
        if (!isSoundFile(f)) { continue; }
        Entry e;
        e.path = root.absoluteFilePath(f);
        e.title = QFileInfo(f).completeBaseName();
        e.category = QString::fromUtf8("未分类");
        mAllEntries.append(e);
        mCategories.insert(e.category);
    }

    rebuildCategoryPills();
    applyFilter();
    qInfo() << "[SOUNDBROWSER] rescan" << mRootDir
            << "categories" << mCategories.size()
            << "entries" << mAllEntries.size();
}

void SoundBrowserPanel::rebuildCategoryPills()
{
    while (mCategoryLayout->count() > 0) {
        const auto it = mCategoryLayout->takeAt(0);
        if (auto w = it->widget()) { w->deleteLater(); }
        delete it;
    }
    const auto addPill = [this](const QString& text, const QString& cat) {
        const auto btn = new QToolButton(mCategoryHost);
        btn->setText(text);
        btn->setCheckable(true);
        btn->setCursor(Qt::PointingHandCursor);
        btn->setChecked(mCategory == cat);
        connect(btn, &QToolButton::clicked, this, [this, cat]() {
            if (mCategory == cat) { return; }
            mCategory = cat;
            rebuildCategoryPills();
            applyFilter();
        });
        mCategoryLayout->addWidget(btn);
    };

    addPill(QString::fromUtf8("全部"), QString());
    addPill(QString::fromUtf8("★ 收藏"), QStringLiteral("\x01fav"));
    for (const auto& cat : mCategories.values()) {
        addPill(cat, cat);
    }
    mCategoryHost->setVisible(mCategoryLayout->count() > 2 ||
                              !mFavorites.isEmpty());
    mCategoryHost->updateGeometry();
}

void SoundBrowserPanel::applyFilter()
{
    const bool favOnly = mCategory == QStringLiteral("\x01fav");
    mShown.clear();
    for (const auto& e : mAllEntries) {
        if (!mFilter.isEmpty() &&
                !e.title.contains(mFilter, Qt::CaseInsensitive)) {
            continue;
        }
        if (favOnly) {
            if (!mFavorites.contains(e.path)) { continue; }
        } else if (!mCategory.isEmpty() && e.category != mCategory) {
            continue;
        }
        mShown.append(e);
    }
    rebuildGrid();
}

void SoundBrowserPanel::rebuildGrid()
{
    stopPreview();
    mCards.clear();
    mDecodeStarted.clear();
    while (mGridLayout->count() > 0) {
        const auto it = mGridLayout->takeAt(0);
        if (auto w = it->widget()) { w->deleteLater(); }
        delete it;
    }
    for (const auto& e : mShown) {
        const auto card = new SoundCardWidget(e.path, e.title, mGridHost);
        card->setFavorite(mFavorites.contains(e.path));
        connect(card, &SoundCardWidget::hoverEntered, this,
                [this](SoundCardWidget* c) {
            mHoverCard = c;
            mHoverTimer->start();
        });
        connect(card, &SoundCardWidget::hoverLeft, this,
                [this](SoundCardWidget* c) {
            if (mHoverCard == c) {
                mHoverCard.clear();
                mHoverTimer->stop();
            }
            stopPreview();
        });
        connect(card, &SoundCardWidget::favoriteToggled, this,
                &SoundBrowserPanel::toggleFavorite);
        connect(card, &SoundCardWidget::doubleClicked, this,
                [this](SoundCardWidget* c) {
            emit monitorRequested(c->path());
        });
        // 单击 = 播放/暂停切换；右键 = 音效管理菜单
        connect(card, &SoundCardWidget::clicked, this, [this](
                    SoundCardWidget* c) { togglePreview(c); });
        connect(card, &SoundCardWidget::contextRequested, this,
                [this](SoundCardWidget* c, const QPoint& gp) {
            showCardMenu(c, gp);
        });
        mCards.insert(e.path, card);
        mGridLayout->addWidget(card);
    }
    if (mShown.isEmpty()) {
        mHintLabel->setText(QString::fromUtf8(
                mRootDir.isEmpty()
                    ? "先选一个音效库目录：子文件夹会变成分类，悬停卡片试听，拖到时间轴音频轨。"
                    : "这个分类没有音效文件。"));
        mHintLabel->setVisible(true);
    } else {
        mHintLabel->setVisible(false);
    }
    QTimer::singleShot(0, this, [this]() { updateVisibleSet(); });
}

// ---------------------------------------------------------------
// 波形解码（LRU 激活池 + 逐秒异步链）
// ---------------------------------------------------------------

SoundBrowserPanel::HandlerPair SoundBrowserPanel::acquireHandler(const QString& path)
{
    if (mActive.contains(path)) {
        mActiveOrder.removeAll(path);
        mActiveOrder.prepend(path);
        return mActive.value(path);
    }
    HandlerPair hp;
    try {
        // DataHandler 注册表只持裸指针不保活（裸指针仅用于查重），
        // 必须与 SoundHandler 成对强持在 LRU 条目里，否则出作用域
        // 即析构、SoundHandler::mDataHandler 悬垂（secondReaderFinished
        // 段错误实证）
        hp.dh = SoundDataHandler::sGetCreateDataHandler<SoundDataHandler>(path);
        if (hp.dh) { hp.sh = enve::make_shared<SoundHandler>(hp.dh.get()); }
    } catch (const std::exception& e) {
        gPrintExceptionCritical(e);
    } catch (...) {}
    if (hp.sh) {
        mActive.insert(path, hp);
        mActiveOrder.prepend(path);
        while (mActiveOrder.size() > kMaxActive) {
            const auto evict = mActiveOrder.takeLast();
            mActive.remove(evict);
            if (auto c = mCards.value(evict)) { c->detachHandler(); }
        }
    }
    return hp;
}

void SoundBrowserPanel::activateDecode(SoundCardWidget* card)
{
    const QString path = card->path();
    if (mDecodeStarted.contains(path) || mDecoding.contains(path)) {
        return;
    }
    mDecodeStarted.insert(path);
    const auto hp = acquireHandler(path);
    if (!hp.sh) {
        card->setDecodeError();
        return;
    }
    const qreal dur = hp.sh->durationSec();
    card->setDurationSec(dur);
    if (dur <= 0) {
        card->setDecodeError();
        return;
    }
    const int totalSecs = qMin(qCeil(dur), kDecodeBudgetSecs);
    card->setExpectedCols(totalSecs * kColsPerSec);
    mDecoding.insert(path);
    requestNextSecond(card, hp, path, 0, totalSecs);
}

void SoundBrowserPanel::requestNextSecond(SoundCardWidget* card,
                                          const HandlerPair& hp,
                                          const QString& path,
                                          int second,
                                          const int totalSecs)
{
    // dh 与 sh 一起进捕获链：在途解码回写 DataHandler 缓存，
    // 捕获保活防 LRU 淘汰后悬垂
    const auto dh = hp.dh;
    const auto sh = hp.sh;
    while (second < totalSecs) {
        // 已解码秒（缓存命中）直接出列
        if (const auto samples = sh->getSamplesForSecond(second)) {
            card->appendPeaks(
                    SoundPeaks::peaksForSecond(samples, kColsPerSec));
            second++;
            continue;
        }
        auto reader = sh->getSecondReader(second);
        if (!reader) { reader = sh->addSecondReader(second); }
        if (!reader) {
            mDecoding.remove(path);
            card->setDecodeError();
            return;
        }
        const QPointer<SoundCardWidget> cardQ = card;
        const QPointer<SoundBrowserPanel> selfQ = this;
        reader->addDependent(
            {[selfQ, cardQ, dh, sh, path, second, totalSecs]() {
                 // 任务线程 → 回 UI 线程续链
                 if (!selfQ) { return; }
                 QMetaObject::invokeMethod(
                         selfQ.data(),
                         [selfQ, cardQ, dh, sh, path, second, totalSecs]() {
                             if (!selfQ ||
                                 !selfQ->mDecoding.contains(path)) {
                                 return;
                             }
                             if (!cardQ) {
                                 selfQ->mDecoding.remove(path);
                                 return;
                             }
                             const auto samples =
                                     sh ? sh->getSamplesForSecond(second)
                                        : nullptr;
                             if (samples) {
                                 cardQ->appendPeaks(
                                         SoundPeaks::peaksForSecond(
                                                 samples, kColsPerSec));
                             } else {
                                 // 失败秒：跳过（包络留空隙）
                             }
                             selfQ->requestNextSecond(cardQ, {dh, sh},
                                                      path, second + 1,
                                                      totalSecs);
                         },
                         Qt::QueuedConnection);
             },
             [selfQ, path]() { // canceled
                 if (!selfQ) { return; }
                 QMetaObject::invokeMethod(
                         selfQ.data(),
                         [selfQ, path]() {
                             if (selfQ) { selfQ->mDecoding.remove(path); }
                         },
                         Qt::QueuedConnection);
             }});
        return; // 异步在途，等回调续链
    }
    mDecoding.remove(path);
    qInfo() << "[SOUNDBROWSER] decode done" << path;
}

void SoundBrowserPanel::updateVisibleSet()
{
    if (!mGridHost || mCards.isEmpty()) { return; }
    const auto vp = mGridScroll->viewport();
    const QRect vpRect(mGridScroll->horizontalScrollBar()->value(),
                       mGridScroll->verticalScrollBar()->value(),
                       vp->width(), vp->height());
    for (auto it = mCards.constBegin(); it != mCards.constEnd(); ++it) {
        auto card = it.value();
        if (!card || mDecodeStarted.contains(it.key())) { continue; }
        if (vpRect.intersects(card->geometry())) {
            activateDecode(card);
        }
    }
}

// ---------------------------------------------------------------
// 悬停试听
// ---------------------------------------------------------------

void SoundBrowserPanel::playHovered()
{
    if (!mHoverCard || mPlaybackSuppressed) { return; }
    if (mPlayingCard == mHoverCard) { return; }
    stopPreview();
    mPlayer->setSource(QUrl::fromLocalFile(mHoverCard->path()));
    mPlayer->play();
    mPlayingCard = mHoverCard;
    mPlayingCard->setPlaying(true);
}

void SoundBrowserPanel::stopPreview()
{
    mHoverTimer->stop();
    if (mPlayer &&
        mPlayer->playbackState() != QMediaPlayer::StoppedState) {
        mPlayer->stop();
    }
    if (mPlayingCard) {
        mPlayingCard->setPlaying(false);
        mPlayingCard.clear();
    }
}

// ---------------------------------------------------------------
// 收藏
// ---------------------------------------------------------------

void SoundBrowserPanel::loadFavorites()
{
    mFavorites = AppSupport::getSettings(
                             QStringLiteral("SoundBrowser"),
                             QStringLiteral("favorites"))
                         .toStringList();
}

void SoundBrowserPanel::saveFavorites()
{
    AppSupport::setSettings(QStringLiteral("SoundBrowser"),
                            QStringLiteral("favorites"), mFavorites);
}

void SoundBrowserPanel::toggleFavorite(SoundCardWidget* card, const bool on)
{
    if (!card) { return; }
    const QString path = card->path();
    if (on && !mFavorites.contains(path)) {
        mFavorites.append(path);
    } else if (!on) {
        mFavorites.removeAll(path);
    }
    saveFavorites();
    if (mCategory == QStringLiteral("\x01fav")) { applyFilter(); }
}

// ---------------------------------------------------------------
// 音效管理（第二阶段）：单击试听切换 + 右键文件管理
// ---------------------------------------------------------------

void SoundBrowserPanel::togglePreview(SoundCardWidget* card)
{
    if (!card) { return; }
    if (mPlayingCard == card) { stopPreview(); return; }
    stopPreview();
    if (mPlaybackSuppressed) {
        emit logMessage(QString::fromUtf8("工程播放中，暂停后可试听"));
        return;
    }
    mPlayer->setSource(QUrl::fromLocalFile(card->path()));
    mPlayer->play();
    mPlayingCard = card;
    mPlayingCard->setPlaying(true);
}

bool SoundBrowserPanel::isPreviewPlaying() const
{
    return mPlayer &&
           mPlayer->playbackState() == QMediaPlayer::PlayingState;
}

void SoundBrowserPanel::refileFavorite(const QString& oldPath,
                                       const QString& newPath)
{
    const int i = mFavorites.indexOf(oldPath);
    if (i < 0) { return; }
    mFavorites[i] = newPath;
    saveFavorites();
}

void SoundBrowserPanel::renameCard(SoundCardWidget* card,
                                   const QString& newName)
{
    if (!card) { return; }
    const QString clean = newName.trimmed();
    if (clean.isEmpty()) { return; }
    const QFileInfo fi(card->path());
    // 新名不带扩展名时保留原扩展名；带扩展名则按用户所写
    const QString fileName = clean.contains('.')
            ? clean : clean + '.' + fi.suffix();
    const QString newPath = fi.absolutePath() + '/' + fileName;
    if (newPath == card->path()) { return; }
    if (QFile::exists(newPath)) {
        emit logMessage(QString::fromUtf8("同名文件已存在：%1").arg(fileName));
        return;
    }
    if (!QFile::rename(card->path(), newPath)) {
        emit logMessage(QString::fromUtf8("重命名失败"));
        return;
    }
    refileFavorite(card->path(), newPath);
    emit logMessage(QString::fromUtf8("已重命名为 %1").arg(fileName));
    rescan();
}

void SoundBrowserPanel::moveCardTo(SoundCardWidget* card, const QString& dir)
{
    if (!card || dir.isEmpty()) { return; }
    const QFileInfo fi(card->path());
    const QString newPath = dir + '/' + fi.fileName();
    if (newPath == card->path()) { return; }
    if (QFile::exists(newPath)) {
        emit logMessage(QString::fromUtf8("目标分类已存在同名文件"));
        return;
    }
    if (!QFile::rename(card->path(), newPath)) {
        emit logMessage(QString::fromUtf8("移动失败"));
        return;
    }
    refileFavorite(card->path(), newPath);
    emit logMessage(QString::fromUtf8("已移动到分类「%1」")
                            .arg(QFileInfo(dir).fileName()));
    rescan();
}

void SoundBrowserPanel::trashCard(SoundCardWidget* card)
{
    if (!card) { return; }
    const QString path = card->path();
    if (!QFile::moveToTrash(path)) {
        emit logMessage(QString::fromUtf8("移入回收站失败"));
        return;
    }
    mFavorites.removeAll(path);
    saveFavorites();
    emit logMessage(QString::fromUtf8("已移入回收站：%1")
                            .arg(QFileInfo(path).fileName()));
    rescan();
}

void SoundBrowserPanel::showCardMenu(SoundCardWidget* card,
                                     const QPoint& globalPos)
{
    if (!card) { return; }
    const QString path = card->path();
    const QFileInfo fi(path);
    QMenu menu(this);
    menu.addAction(QString::fromUtf8(card->isPlaying()
                                             ? "停止试听" : "播放试听"),
                   this, [this, card]() { togglePreview(card); });
    menu.addAction(QString::fromUtf8("打开所在文件夹"), this, [fi]() {
        QDesktopServices::openUrl(QUrl::fromLocalFile(fi.absolutePath()));
    });
    menu.addAction(QString::fromUtf8("重命名…"), this,
                   [this, card, fi]() {
        bool ok = false;
        const QString name = QInputDialog::getText(
                    this, QString::fromUtf8("重命名音效"),
                    QString::fromUtf8("新名称"), QLineEdit::Normal,
                    fi.completeBaseName(), &ok);
        if (ok) { renameCard(card, name); }
    });
    auto* const mov = menu.addMenu(QString::fromUtf8("移动到分类"));
    const QString curDir = fi.absolutePath();
    for (const auto& cat : mCategories) {
        const QString dir = mRootDir + '/' + cat;
        if (dir == curDir) { continue; }
        mov->addAction(cat, this, [this, card, dir]() {
            moveCardTo(card, dir);
        });
    }
    mov->addSeparator();
    mov->addAction(QString::fromUtf8("新建分类…"), this, [this, card]() {
        bool ok = false;
        const QString name = QInputDialog::getText(
                    this, QString::fromUtf8("新建分类"),
                    QString::fromUtf8("分类名"), QLineEdit::Normal,
                    QString(), &ok);
        if (!ok) { return; }
        const QString clean = name.trimmed();
        if (clean.isEmpty()) { return; }
        const QString dir = mRootDir + '/' + clean;
        if (!QDir().mkpath(dir)) {
            emit logMessage(QString::fromUtf8("创建分类目录失败"));
            return;
        }
        moveCardTo(card, dir);
    });
    menu.addSeparator();
    menu.addAction(QString::fromUtf8("删除（移入回收站）"), this,
                   [this, card]() { trashCard(card); });
    menu.exec(globalPos);
}
