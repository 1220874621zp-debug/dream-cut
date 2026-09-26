/*
#
# Friction - https://friction.graphics
#
# Copyright (c) Ole-André Rodlie and contributors
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

#include "Boxes/internallinkcanvasbox.h"

#include "Private/document.h"
#include "ReadWrite/evformat.h"
#include "ReadWrite/ereadstream.h"
#include "ReadWrite/evtail.h"
#include "ReadWrite/filefooter.h"
#include "ReadWrite/ewritestream.h"
#include "swt_rulescollection.h"
#include "Timeline/fixedlenanimationrect.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

// ============================================================
// 外部工程流解析
//
// 流序与 MainWindow::loadEVFile + Document::readScenes 严格对
// 齐，但只取出场景对象（裸构造，不进 Document::fScenes，因
// 此不会建场景窗口），其余段落（layout/bookmarked/grid）按
// 字节消费跳过——core 不能依赖 ui 层，这里的 skip 函数复刻
// 各段读取端的字节协议；上游若改动对应字段，此处必须同步
// ============================================================
namespace {

// CanvasWindow::readState 的字节消费：int + int + QTransform
void skipCanvasWindowState(eReadStream& src) {
    int sceneReadId; src >> sceneReadId;
    int sceneDocumentId; src >> sceneDocumentId;
    QTransform viewTransform; src >> viewTransform;
}

// TimelineWidget::readState 的字节消费
void skipTimelineWindowState(eReadStream& src) {
    int sceneReadId; src >> sceneReadId;
    int sceneDocumentId; src >> sceneDocumentId;
    QString search; src >> search;
    int sliderPos; src >> sliderPos;
    int frame; src >> frame;
    int minViewedFrame; src >> minViewedFrame;
    int maxViewedFrame; src >> maxViewedFrame;
    if (src.evFileVersion() > 6) {
        // 三个枚举块按原读取端的 sizeof 消费（同编译器写读对称）
        SWT_BoxRule boxRule;
        SWT_Type type;
        SWT_Target target;
        src.read(&boxRule, sizeof(SWT_BoxRule));
        src.read(&type, sizeof(SWT_Type));
        src.read(&target, sizeof(SWT_Target));
    }
}

// dock 分割树（wrappernode.h 的 WrapperNode::write/sRead 协议）：
// 每节点 int 类型标签（0=base 1=widget 2=splitH 3=splitV）+
// readData。base 一个子节点，split 两个，widget 叶子按所属布
// 局列读窗口状态（scene 列叶子=CanvasWindow，timeline 列叶
// 子=TimelineWidget）
void skipWrapperTree(eReadStream& src, const bool timelineLeaf) {
    int typeVal; src >> typeVal;
    switch (typeVal) {
    case 0: // base
        skipWrapperTree(src, timelineLeaf);
        break;
    case 2: case 3: // splitH / splitV
        skipWrapperTree(src, timelineLeaf);
        skipWrapperTree(src, timelineLeaf);
        break;
    case 1: // widget leaf
        if (timelineLeaf) skipTimelineWindowState(src);
        else skipCanvasWindowState(src);
        break;
    default:
        RuntimeThrow("外部工程布局树含未知节点类型 " + std::to_string(typeVal));
    }
}

// LayoutData::read 的字节消费：QString + 场景布局树 + 时间轴布局树
void skipLayoutData(eReadStream& src) {
    QString name; src >> name;
    skipWrapperTree(src, false);
    skipWrapperTree(src, true);
}

// LayoutHandler::read 的字节消费：nLays 个布局 + nScenes 个场
// 景布局 + currentId（读取端后续 setCurrent 为纯 UI 操作零字节）
void skipLayoutState(eReadStream& src) {
    int nLays; src >> nLays;
    for (int i = 0; i < nLays; i++) skipLayoutData(src);
    int nScenes; src >> nScenes;
    for (int i = 0; i < nScenes; i++) skipLayoutData(src);
    int relCurrentId; src >> relCurrentId;
}

// Document::readBookmarked 的字节消费；SimpleBrushWrapper 的流
// 读 = 集合名 + 笔刷名两个 QString
void skipBookmarked(eReadStream& src) {
    int nCol; src >> nCol;
    for (int i = 0; i < nCol; i++) {
        QColor col; src >> col;
    }
    int nBrush; src >> nBrush;
    for (int i = 0; i < nBrush; i++) {
        QString brushCollection; src >> brushCollection;
        QString brushName; src >> brushName;
    }
}

// Grid::readDocument 的字节消费（20 字段，类型对齐 grid.h 的
// Settings；stepRotCtrl/stepRotShift 不进流）
void skipGridState(eReadStream& src) {
    double sizeX; src >> sizeX;
    double sizeY; src >> sizeY;
    double originX; src >> originX;
    double originY; src >> originY;
    int snapThresholdPx; src >> snapThresholdPx;
    bool show; src >> show;
    bool drawOnTop; src >> drawOnTop;
    bool snapEnabled; src >> snapEnabled;
    bool snapToCanvas; src >> snapToCanvas;
    bool snapToBoxes; src >> snapToBoxes;
    bool snapToNodes; src >> snapToNodes;
    bool snapToPivots; src >> snapToPivots;
    bool snapToGrid; src >> snapToGrid;
    bool snapAnchorPivot; src >> snapAnchorPivot;
    bool snapAnchorBounds; src >> snapAnchorBounds;
    bool snapAnchorNodes; src >> snapAnchorNodes;
    int majorEveryX; src >> majorEveryX;
    int majorEveryY; src >> majorEveryY;
    QColor color; src >> color;
    QColor colorMajor; src >> colorMajor;
}

bool parseExternalProject(const QString& path,
                          QList<qsptr<Canvas>>& out,
                          QString& err) {
    QFile file(path);
    if (!file.exists()) {
        err = QStringLiteral("文件不存在 %1").arg(path);
        return false;
    }
    if (!file.open(QIODevice::ReadOnly)) {
        err = QStringLiteral("无法打开文件 %1").arg(path);
        return false;
    }
    try {
        const int evVersion = FileFooter::sReadEvFileVersion(&file);
        if (evVersion <= 0) RuntimeThrow("不兼容或残缺的工程数据");
        if (evVersion > EvFormat::version) {
            RuntimeThrow(QStringLiteral("工程版本 %1 高于本程序支持的 %2，"
                                        "请升级后再链接")
                         .arg(evVersion).arg(int(EvFormat::version)));
        }
        // 远古格式（布局段结构不同）不做链接支持，正常打开另存一次即可
        if (evVersion < EvFormat::betterSWTAbsReadWrite) {
            RuntimeThrow("工程版本过老，请先在 Dream Cut 中打开并另存");
        }

        eReadStream src(evVersion, &file);
        src.setPath(path);

        const qint64 savedPos = file.pos();
        // 封面尾部块（EvTail）在 FileFooter 之后，future 表定位须
        // 以剥离尾部块后的流末尾为基准（与 loadEVFile 同款）
        const qint64 pos = EvTail::probe(&file) -
                FileFooter::sSize(evVersion) - qint64(sizeof(int));
        file.seek(pos);
        src.readFutureTable();
        file.seek(savedPos);

        src.readCheckpoint("File beginning pos mismatch");

        // 场景设置段：裸构造（不进 fScenes，不触发 UI），beforeContent
        // 版本先读设置，与 loadEVFile 的调用序一致
        int nScenes; src >> nScenes;
        const bool beforeContent =
                evVersion >= EvFormat::readSceneSettingsBeforeContent;
        for (int i = 0; i < nScenes; i++) {
            const auto scene = enve::make_shared<Canvas>(*Document::sInstance);
            if (beforeContent) scene->readSettings(src);
            out.append(scene);
        }
        skipLayoutState(src);
        src.readCheckpoint("Error reading Layout");

        if (src.evFileVersion() > 1) {
            skipBookmarked(src);
            src.readCheckpoint("Error reading bookmarks");
        }
        if (src.evFileVersion() >= EvFormat::grid) {
            skipGridState(src);
            src.readCheckpoint("Error reading grid");
        }

        int nScenes2; src.read(&nScenes2, sizeof(int));
        if (nScenes2 != out.count()) {
            RuntimeThrow(QStringLiteral("场景数不一致（%1 vs %2），流已错位")
                         .arg(nScenes2).arg(out.count()));
        }
        for (int i = 0; i < nScenes2; i++) {
            const auto& scene = out[i];
            const auto block = scene->blockUndoRedo();
            scene->readBoundingBox(src);
            src.readCheckpoint("Error reading scene");
        }
        // 流析构时处理 doneTasks（外部工程内部的跨场景引用按本流
        // 的 readId 表解析；刻意不泵 SimpleTask，避免提前处理主工
        // 程装载期间排程的上下文任务）
    } catch(const std::exception& e) {
        file.close();
        err = QString::fromUtf8(e.what());
        return false;
    } catch(...) {
        file.close();
        err = QStringLiteral("读取 %1 时发生未知错误").arg(path);
        return false;
    }
    file.close();
    return true;
}

} // namespace

// ============================================================
// ExternalProjectCache
// ============================================================

ExternalProjectCache::ExternalProjectCache() {
    connect(&mWatcher, &QFileSystemWatcher::fileChanged,
            this, &ExternalProjectCache::onFileChanged);
    // 外部编辑器多用原子保存（写临时文件后 rename 替换）：文件级
    // 监视在 rename 瞬间掉线且新 inode 的事件收不到，目录级监视兜底
    connect(&mWatcher, &QFileSystemWatcher::directoryChanged,
            this, &ExternalProjectCache::onFileChanged);
    mDebounce.setSingleShot(true);
    mDebounce.setInterval(600);
    connect(&mDebounce, &QTimer::timeout,
            this, &ExternalProjectCache::onDebounceTimeout);
}

ExternalProjectCache *ExternalProjectCache::instance() {
    // 首次调用发生在导入/装载（main 已运行），进程级单例
    static ExternalProjectCache inst;
    return &inst;
}

QSharedPointer<ExternalProjectCache::Loaded>
ExternalProjectCache::load(const QString &path,
                           const bool forceReload,
                           QString *err) {
    const QString key = QDir::cleanPath(
                QFileInfo(path).absoluteFilePath());
    if (!forceReload) {
        const auto it = mCache.constFind(key);
        if (it != mCache.constEnd()) return *it;
    }
    QList<qsptr<Canvas>> scenes;
    QString parseErr;
    if (!parseExternalProject(path, scenes, parseErr)) {
        if (err) *err = parseErr;
        return nullptr;
    }
    const auto loaded = QSharedPointer<Loaded>::create();
    loaded->path = key;
    loaded->scenes = std::move(scenes);
    mCache.insert(key, loaded);
    // 文件与父目录双双在册：rename 替换后靠目录事件兜底重挂
    if (QFile::exists(key) && !mWatcher.files().contains(key)) {
        mWatcher.addPath(key);
    }
    const QString dir = QFileInfo(key).absolutePath();
    if (!mWatcher.directories().contains(dir)) {
        mWatcher.addPath(dir);
    }
    return loaded;
}

Canvas *ExternalProjectCache::findScene(Loaded * const loaded,
                                        const int sceneDocId,
                                        const QString &sceneName) const {
    if (!loaded) return nullptr;
    if (sceneDocId >= 0) {
        for (const auto& scene : loaded->scenes) {
            if (scene && scene->getDocumentId() == sceneDocId) {
                return scene.get();
            }
        }
    }
    if (!sceneName.isEmpty()) {
        for (const auto& scene : loaded->scenes) {
            if (scene && scene->prp_getName() == sceneName) {
                return scene.get();
            }
        }
    }
    return loaded->scenes.isEmpty() ? nullptr : loaded->scenes.first().get();
}

QStringList ExternalProjectCache::sceneNames(Loaded * const loaded) {
    QStringList result;
    if (loaded) {
        for (const auto& scene : loaded->scenes) {
            result << (scene ? scene->prp_getName() : QString());
        }
    }
    return result;
}

void ExternalProjectCache::onFileChanged(const QString &path) {
    QFileInfo info(path);
    if (info.isDir()) {
        // 目录事件：目录里在册的链接工程全部进入待重载集
        bool touched = false;
        for (const auto& key : mCache.keys()) {
            if (QFileInfo(key).absolutePath() == path) {
                mPendingPaths.insert(key);
                touched = true;
            }
        }
        if (touched) { mDebounce.start(); }
        return;
    }
    const QString key = QDir::cleanPath(info.absoluteFilePath());
    mPendingPaths.insert(key);
    mDebounce.start(); // restart：编辑器一次保存可能连发多个信号
}

void ExternalProjectCache::onDebounceTimeout() {
    const auto paths = mPendingPaths;
    mPendingPaths.clear();
    for (const auto& path : paths) {
        if (QFile::exists(path) && !mWatcher.files().contains(path)) {
            mWatcher.addPath(path);
        }
        if (!QFile::exists(path)) { continue; } // 文件被移走：等待回归
        QString err;
        if (load(path, true, &err)) {
            qWarning() << "[ExtLink] 源工程已重载" << path;
            emit projectReloaded(path);
        } else {
            // 解析失败保留旧缓存（链接盒维持旧画面），错误进日志取证
            qWarning() << "[ExtLink] 源工程重载失败" << path << err;
        }
    }
}

// ============================================================
// InternalLinkCanvasBox
// ============================================================

InternalLinkCanvasBox::InternalLinkCanvasBox() :
    InternalLinkBox(nullptr, false) {
    // InternalLinkBox 构造把类型钉死为 internalLink，但读写两侧的
    // 盒类型标签就是 mType（readIdCreateBox 按 it 分发构造），子类
    // 必须改写为 internalLinkCanvas，否则读回构造的是基类、流错位
    mType = eBoxType::internalLinkCanvas;
    prp_setName(QStringLiteral("场景链接"));
    connect(ExternalProjectCache::instance(),
            &ExternalProjectCache::projectReloaded,
            this, [this](const QString& path) {
        if (path == mSourcePath) reloadFromSource();
    });
}

void InternalLinkCanvasBox::setSourceProject(const QString &path,
                                             const int sceneDocId,
                                             const QString &sceneName) {
    mSourcePath = path;
    mTargetSceneDocId = sceneDocId;
    mTargetSceneName = sceneName;
    QString err;
    const auto loaded = ExternalProjectCache::instance()->load(
                path, false, &err);
    if (!loaded) {
        qWarning() << "[ExtLink] 装载外部工程失败" << path << err;
        bindExternalScene(nullptr);
        return;
    }
    const auto scene = ExternalProjectCache::instance()->findScene(
                loaded.data(), sceneDocId, sceneName);
    bindExternalScene(scene);
    qWarning() << "[ExtLink] 链接建立" << path
               << "场景" << (scene ? scene->prp_getName() : QString())
               << "范围" << (scene ? scene->getFrameRange().fMax : -1);
}

void InternalLinkCanvasBox::reloadFromSource() {
    if (mSourcePath.isEmpty()) return;
    QString err;
    const auto loaded = ExternalProjectCache::instance()->load(
                mSourcePath, true, &err);
    if (!loaded) {
        // 缓存未被动过，保留旧画面
        qWarning() << "[ExtLink] 重载失败，保留旧链接" << mSourcePath << err;
        return;
    }
    const auto scene = ExternalProjectCache::instance()->findScene(
                loaded.data(), mTargetSceneDocId, mTargetSceneName);
    bindExternalScene(scene);
    qWarning() << "[ExtLink] 链接已刷新" << mSourcePath
               << "绑定" << (scene ? scene->prp_getName() : QStringLiteral("丢失"));
}

void InternalLinkCanvasBox::bindExternalScene(Canvas * const scene) {
    // 直接走活链接（绕开 BoxTargetProperty 的 undo 路径）：路径三
    // 元组才是持久化事实源，BoxTarget 仅作属性面板展示（保持空）
    assignLinkTarget(scene);
    if (scene) {
        rename(scene->prp_getName() + QStringLiteral(" 链接"));
        syncDurationToTarget();
    } else {
        rename(QStringLiteral("链接丢失"));
    }
    planUpdate(UpdateReason::userChange);
}

void InternalLinkCanvasBox::syncDurationToTarget() {
    const auto scene = enve_cast<Canvas*>(getLinkTarget());
    if (!scene) return;
    const int maxF = scene->getFrameRange().fMax;
    const auto dur = getDurationRectangle();
    if (!dur) {
        // NLE 生态所有层的块矩形都是 FixedLenAnimationRect：写侧的
        // 子类标签用 ref<>（qSharedPointerCast，静态转型恒成功）判
        // 定，普通 DurationRectangle 会被谎报成 FixedLen——写出
        // tag=1 + 12 字节内容，读回却按 22 字节读，流错位 10 字节，
        // 后随 name 读炸到 EOF、工程打开即崩。必须与生态同型
        const auto rect = enve::make_shared<FixedLenAnimationRect>(*this);
        rect->setMinAbsFrame(0);
        rect->setMaxAbsFrame(qMax(0, maxF));
        setDurationRectangle(rect);
    } else if (dur->getMaxAbsFrame() != maxF && maxF >= 0) {
        dur->setMaxAbsFrame(maxF);
    }
}

void InternalLinkCanvasBox::writeBoundingBox(eWriteStream &dst) const {
    BoundingBox::writeBoundingBox(dst);
    dst.writeFilePath(mSourcePath);
    dst << mTargetSceneDocId << mTargetSceneName;
}

void InternalLinkCanvasBox::readBoundingBox(eReadStream &src) {
    BoundingBox::readBoundingBox(src);
    mSourcePath = src.readFilePath();
    src >> mTargetSceneDocId >> mTargetSceneName;
    if (!mSourcePath.isEmpty()) {
        setSourceProject(mSourcePath, mTargetSceneDocId, mTargetSceneName);
    }
}

void InternalLinkCanvasBox::setupCanvasMenu(PropertyMenu * const menu) {
    if (menu->hasActionsForType<InternalLinkCanvasBox>()) return;
    menu->addedActionsForType<InternalLinkCanvasBox>();
    const PropertyMenu::PlainSelectedOp<InternalLinkCanvasBox> reloadOp =
            [](InternalLinkCanvasBox * const box) {
        box->reloadFromSource();
    };
    menu->addPlainAction(QIcon::fromTheme("loop"),
                         QObject::tr("重新加载链接"), reloadOp);
    InternalLinkBox::setupCanvasMenu(menu);
}
