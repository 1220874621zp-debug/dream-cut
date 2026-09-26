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
// 性能设计（大库必答）：
// - 视口虚拟滚动：卡片固定尺寸，网格几何离线可算——只创建可视行
//   ±缓冲的卡片，滚远即毁（无 FlowLayout 全量建卡）
// - 开流异步化：ffmpeg 开源（SoundHandler 构造）在 worker 线程，
//   UI 线程零阻塞，并发上限 + 队列
// - 逐秒并行解码：预算内全部秒一次性入 HddCachable 池，槽位填色
//   （旧版逐秒串行链一秒一跳）
// - LRU 激活池限量解复用上下文（fd），DataHandler 与 SoundHandler
//   成对强持（注册表只持裸指针不保活）
class SoundBrowserPanel : public QWidget {
    Q_OBJECT
public:
    explicit SoundBrowserPanel(QWidget* const parent = nullptr);

    // 音频文件判定（扩展名白名单，时间轴拖放路由共用）
    static bool isSoundFile(const QString& path);

    // 设置音效库根目录（持久化 + 重扫）
    void setRootDir(const QString& dir);
    // 音效管理（右键菜单复用的薄封装；文件操作后自动重扫）
    void togglePreview(SoundCardWidget* card);
    bool isPreviewPlaying() const;
    void renameCard(SoundCardWidget* card, const QString& newName);
    void moveCardTo(SoundCardWidget* card, const QString& dir);
    void trashCard(SoundCardWidget* card);
    void showCardMenu(SoundCardWidget* card, const QPoint& globalPos);
    // 自测/调试只读访问
    int cardCount() const { return mShown.size(); }
    int decodedCardCount() const;
    int categoryCount() const { return mCategories.size(); }
    const QStringList& favorites() const { return mFavorites; }
    SoundCardWidget* cardFor(const QString& path) const;

    // DataHandler 注册表只持裸指针不保活，必须与 SoundHandler
    // 成对强持；成员序 dh 在前 = 析构时 SoundHandler 先亡
    struct HandlerPair {
        qsptr<SoundDataHandler> dh;   // SelfRef 系 = QSharedPointer
        stdsptr<SoundHandler> sh;     // StdSelfRef 系 = std::shared_ptr
    };

signals:
    void logMessage(const QString& msg);
    // 单击卡片：装载到片段监视器预览（项目面板同语义）
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

    // ---- 视口虚拟网格（固定尺寸离线算位，按需建/毁卡片）
    void updateVirtualGrid();
    void clearGridCards();
    SoundCardWidget* createCard(const int index);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

    // ---- 波形解码（异步开流 + LRU 池 + 逐秒并行）
    void requestDecode(SoundCardWidget* card);
    void pumpOpens();
    void finishOpen(const QString& path, const qsptr<SoundDataHandler>& dh,
                    const stdsptr<SoundHandler>& sh);
    void startDecodeWith(const QString& path, const HandlerPair& hp);
    void queueSecond(const HandlerPair& hp, const QString& path,
                     const int second);
    void deliverPeaks(const QString& path, const stdsptr<SoundHandler>& sh,
                      const int second);
    void insertActive(const QString& path, const HandlerPair& hp);

    // ---- 悬停试听
    void stopPreview();
    void playHovered();

    // ---- 收藏
    void loadFavorites();
    void saveFavorites();
    void toggleFavorite(SoundCardWidget* card, bool on);
    // 文件重命名/移动后同步收藏表路径
    void refileFavorite(const QString& oldPath, const QString& newPath);

    // ---- UI
    QToolButton* mDirButton = nullptr;
    QLineEdit* mSearchEdit = nullptr;
    QSlider* mVolSlider = nullptr;
    QSlider* mZoomSlider = nullptr;
    QWidget* mCategoryHost = nullptr;
    FlowLayout* mCategoryLayout = nullptr;
    QScrollArea* mGridScroll = nullptr;
    QWidget* mGridHost = nullptr;
    QLabel* mHintLabel = nullptr;

    // ---- 数据
    QString mRootDir;
    QString mCategory; // 空=全部
    QString mFilter;
    QStringList mFavorites;
    QList<Entry> mAllEntries;
    QList<Entry> mShown;
    QSet<QString> mCategories;
    QFileSystemWatcher* mWatcher = nullptr;
    QTimer* mRescanTimer = nullptr;

    // ---- 视口虚拟网格
    QHash<int, SoundCardWidget*> mIndexCards; // 条目索引 -> 卡片
    int mCols = 0;                            // 当前网格列数
    qreal mCardScale = 1.0;                   // 卡片缩放（缩放滑杆）

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
    static constexpr int kMaxOpens = 4; // 并发开源上限
    QHash<QString, HandlerPair> mActive;
    QStringList mActiveOrder;
    QSet<QString> mDecoding;               // 解码在途（按路径）
    QHash<QString, int> mPendingSeconds;   // 路径 -> 未回秒数
    QStringList mOpenQueue;                // 开流队列
    QSet<QString> mOpening;                // 开流在途/排队
    int mOpensInFlight = 0;
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
    // 缩放（面板缩放滑杆驱动）：w/h 为整卡尺寸，波形区高度随比例
    void setCardSize(const int w, const int h);
    // 包络槽位制：预分配 totalCols 列（-1=未填），各秒结果按位回填
    void setTotalCols(const int totalCols);
    void putPeaks(const int baseCol, const QVector<qreal>& cols);
    void setDecodeError();
    void setPlaying(const bool on);
    bool isPlaying() const { return mPlaying; }
    // 试听进度高亮：0..1 已播占比；<0 清除
    void setPlayProgress(const qreal frac);
    void setFavorite(const bool on);
    bool isFavorite() const { return mFavorite; }
    bool hasEnvelope() const { return mFilled > 0; }
    bool isDecodeError() const { return mError; }
    // 激活池淘汰：停掉序列，已绘包络保留（再激活时缓存秒秒回）
    void detachHandler();

signals:
    void hoverEntered(SoundCardWidget* card);
    void hoverLeft(SoundCardWidget* card);
    void favoriteToggled(SoundCardWidget* card, bool on);
    void doubleClicked(SoundCardWidget* card);
    // 单击（未成拖拽的按下-释放）= 装载到片段监视器
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
    int mFilled = 0;
    int mWaveH = 64; // 波形区高度（随卡片缩放）
    QVector<qreal> mCols; // 0..1 峰值列，-1 = 未填
    QPoint mPressPos;
    QToolButton* mStarBtn = nullptr;
};

#endif // SOUNDBROWSERPANEL_H
