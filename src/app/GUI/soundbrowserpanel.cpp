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
#include <QDir>
#include <QDrag>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMediaPlayer>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScrollBar>
#include <QScrollArea>
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
        // （每秒到达重绘一次，渐进填充）
        const qreal frac = qMin(1., qreal(mCols.size()) / mTotalCols);
        const qreal drawW = qMax(1., waveRect.width() * frac);
        const qreal midY = waveRect.center().y();
        const qreal halfH = waveRect.height() / 2 - 2;
        p.setPen(QPen(ThemeSupport::getThemeHighlightColor(), 1));
        const int n = mCols.size();
        int prevX = -1;
        for (int x = 0; x < int(drawW); x++) {
            const int idx = qBound(0, int(qreal(x) / drawW * n), n - 1);
            const qreal a = qBound(0., mCols.at(idx), 1.);
            const int xi = qRound(waveRect.left()) + x;
            if (xi == prevX) { continue; }
            prevX = xi;
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
        // 按下即停试听（拖拽/选择时不残声）
        emit hoverLeft(this);
    }
    QFrame::mousePressEvent(e);
}

void SoundCardWidget::mouseMoveEvent(QMouseEvent* e)
{
    if ((e->buttons() & Qt::LeftButton) &&
        (e->position().toPoint() - mPressPos).manhattanLength() >
            QApplication::startDragDistance()) {
        startDrag();
        return;
    }
    QFrame::mouseMoveEvent(e);
}

void SoundCardWidget::mouseDoubleClickEvent(QMouseEvent* e)
{
    QFrame::mouseDoubleClickEvent(e);
    if (e->button() == Qt::LeftButton) { emit doubleClicked(this); }
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

    mHoverTimer = new QTimer(this);
    mHoverTimer->setSingleShot(true);
    mHoverTimer->setInterval(350);
    connect(mHoverTimer, &QTimer::timeout, this,
            &SoundBrowserPanel::playHovered);

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
