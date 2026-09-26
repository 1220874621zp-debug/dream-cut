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

#ifndef SOUNDBROWSERPANEL_H
#define SOUNDBROWSERPANEL_H

#include <QFrame>
#include <QWidget>
#include <QHash>
#include <QPointer>
#include <QSet>

#include "smartPointers/ememory.h"
#include "smartPointers/selfref.h"

class QLabel;
class QLineEdit;
class QScrollArea;
class QSlider;
class QToolButton;
class QMediaPlayer;
class QAudioOutput;
class QTimer;
class QFileSystemWatcher;
class FlowLayout;
class SoundHandler;
class SoundDataHandler;
class SoundCardWidget;

// 音效库面板（剪映式素材库）：本地音效目录浏览、波形卡片网格、
// 悬停自动试听、收藏、拖拽入时间轴音频轨。
// 波形提取绕开场景盒机制：按路径激活共享 SoundDataHandler +
// 独立 SoundHandler（每个打开一个解复用上下文），用小 LRU 池
// 限制同时激活数防止 fd 耗尽；逐秒 SoundReader 异步解码出
// 0..1 峰值列（SoundPeaks 同时间轴波形数学）。
class SoundBrowserPanel : public QWidget {
    Q_OBJECT
public:
    explicit SoundBrowserPanel(QWidget* const parent = nullptr);

    // 音频文件判定（扩展名白名单，时间轴拖放路由共用）
    static bool isSoundFile(const QString& path);

    // 设置音效库根目录（持久化 + 重扫）
    void setRootDir(const QString& dir);
    // 音效管理（右键菜单与单击复用的薄封装；文件操作后自动重扫）
    void togglePreview(SoundCardWidget* card);
    bool isPreviewPlaying() const;
    void renameCard(SoundCardWidget* card, const QString& newName);
    void moveCardTo(SoundCardWidget* card, const QString& dir);
    void trashCard(SoundCardWidget* card);
    void showCardMenu(SoundCardWidget* card, const QPoint& globalPos);
    // 自测/调试只读访问
    int cardCount() const { return mCards.size(); }
    int decodedCardCount() const;
    int categoryCount() const { return mCategories.size(); }
    const QStringList& favorites() const { return mFavorites; }
    SoundCardWidget* cardFor(const QString& path) const
    { return mCards.value(path); }

    // DataHandler 注册表只持裸指针不保活，必须与 SoundHandler
    // 成对强持；成员序 dh 在前 = 析构时 SoundHandler 先亡
    struct HandlerPair {
        qsptr<SoundDataHandler> dh;   // SelfRef 系 = QSharedPointer
        stdsptr<SoundHandler> sh;     // StdSelfRef 系 = std::shared_ptr
    };

signals:
    void logMessage(const QString& msg);
    // 双击卡片：装载到片段监视器预览（项目面板同语义）
    void monitorRequested(const QString& filePath);

private:
    struct Entry {
        QString path;
        QString title;
        QString category;
    };

    void setupUi();
    void setupPlayback();
    void rescan();
    void rebuildWatcher();
    void rebuildCategoryPills();
    void applyFilter();
    void rebuildGrid();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

    // ---- 波形解码（LRU 激活池 + 逐秒异步链）
    HandlerPair acquireHandler(const QString& path);
    void activateDecode(SoundCardWidget* card);
    void requestNextSecond(SoundCardWidget* card,
                           const HandlerPair& hp,
                           const QString& path,
                           int second,
                           const int totalSecs);
    void updateVisibleSet();

    // ---- 悬停试听
    void stopPreview();
    void playHovered();

    // ---- 收藏
    void loadFavorites();
    void saveFavorites();
    void toggleFavorite(SoundCardWidget* card, bool on);
    // 文件重命名/移动后同步收藏表路径
    void refileFavorite(const QString& oldPath, const QString& newPath);

    static QString durationLabel(const qreal sec);

    // ---- UI
    QToolButton* mDirButton = nullptr;
    QLineEdit* mSearchEdit = nullptr;
    QSlider* mVolSlider = nullptr;
    QWidget* mCategoryHost = nullptr;
    FlowLayout* mCategoryLayout = nullptr;
    QScrollArea* mGridScroll = nullptr;
    QWidget* mGridHost = nullptr;
    FlowLayout* mGridLayout = nullptr;
    QLabel* mHintLabel = nullptr;

    // ---- 数据
    QString mRootDir;
    QString mCategory; // 空=全部
    QString mFilter;
    QStringList mFavorites;
    QList<Entry> mAllEntries;
    QList<Entry> mShown;
    QHash<QString, SoundCardWidget*> mCards; // path -> card
    QSet<QString> mCategories;
    QSet<QString> mDecodeStarted;
    QFileSystemWatcher* mWatcher = nullptr;
    QTimer* mRescanTimer = nullptr;

    // ---- 悬停试听
    QMediaPlayer* mPlayer = nullptr;
    QAudioOutput* mAudioOut = nullptr;
    QTimer* mHoverTimer = nullptr;
    QPointer<SoundCardWidget> mHoverCard;
    QPointer<SoundCardWidget> mPlayingCard;
    bool mPlaybackSuppressed = false;

    // ---- 波形解码激活池（front = newest）
    static constexpr int kMaxActive = 24;
    static constexpr int kColsPerSec = 64;
    static constexpr int kDecodeBudgetSecs = 45;
    QHash<QString, HandlerPair> mActive;
    QStringList mActiveOrder;
    QSet<QString> mDecoding;
};

// 波形卡片：包络绘制 + 悬停/拖拽/收藏 + 名称时长标签
class SoundCardWidget : public QFrame {
    Q_OBJECT
public:
    SoundCardWidget(const QString& path, const QString& title,
                    QWidget* const parent = nullptr);

    QString path() const { return mPath; }
    QString title() const { return mTitle; }

    void setDurationSec(const qreal sec); // <=0 = 无法解码
    void setExpectedCols(const int totalCols);
    void appendPeaks(const QVector<qreal>& cols);
    void setDecodeError();
    void setPlaying(const bool on);
    bool isPlaying() const { return mPlaying; }
    // 试听进度高亮：0..1 已播占比；<0 清除
    void setPlayProgress(const qreal frac);
    void setFavorite(const bool on);
    bool isFavorite() const { return mFavorite; }
    bool hasEnvelope() const { return !mCols.isEmpty(); }
    bool isDecodeError() const { return mError; }
    // 激活池淘汰：停掉序列，已绘包络保留（再激活时缓存秒秒回）
    void detachHandler();

signals:
    void hoverEntered(SoundCardWidget* card);
    void hoverLeft(SoundCardWidget* card);
    void favoriteToggled(SoundCardWidget* card, bool on);
    void doubleClicked(SoundCardWidget* card);
    // 单击（未成拖拽的按下-释放）= 播放/暂停切换
    void clicked(SoundCardWidget* card);
    void contextRequested(SoundCardWidget* card, const QPoint& globalPos);

protected:
    void paintEvent(QPaintEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;
    void enterEvent(QEnterEvent* e) override;
    void leaveEvent(QEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;
    void contextMenuEvent(QContextMenuEvent* e) override;

private:
    void startDrag();
    static QString durationLabel(const qreal sec);

    QString mPath;
    QString mTitle;
    qreal mDurationSec = 0;
    bool mError = false;
    bool mPlaying = false;
    bool mDragging = false;
    bool mFavorite = false;
    bool mDecodable = true; // 尚未判定失败
    qreal mPlayFrac = -1;   // 试听进度（已播占比），<0 = 无
    int mTotalCols = 0;
    QVector<qreal> mCols; // 0..1 峰值列（累计）
    QPoint mPressPos;
    QToolButton* mStarBtn = nullptr;
};

#endif // SOUNDBROWSERPANEL_H
